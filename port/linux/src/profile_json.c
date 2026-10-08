/*
PROFILE_JSON.C

A recording's parts as files, on a writer thread of its own (a part takes
seconds to write, which a host must not freeze for): each part is a whole
Chrome trace (ui.perfetto.dev) with two keys of its own before
traceEvents, "halo" (the header and the network tables) and "cpu_summary",
one item per line, which tools/net_report.py reads without the events.
profile_json_write formats a frozen part and nothing else: no globals, no
clock, so tools/profile_check.c calls it.
*/

#ifdef HALO_PROFILE

#if !defined(_WIN32) && !defined(_FILE_OFFSET_BITS)
#define _FILE_OFFSET_BITS 64
#endif

#include "profile_part.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <sys/types.h>
#endif

#ifdef _WIN32
typedef __int64 profile_json_offset;
#define profile_json_tell(file) _ftelli64(file)
#define profile_json_seek(file, offset) _fseeki64((file), (offset), SEEK_SET)
#else
typedef off_t profile_json_offset;
#define profile_json_tell(file) ftello(file)
#define profile_json_seek(file, offset) fseeko((file), (off_t)(offset), SEEK_SET)
#endif

/* ---------- constants */

enum
{
	PROFILE_JSON_WORST_FRAMES = 10,
	PROFILE_JSON_LONGEST_CHILDREN = 3,
};

/* ---------- structures */

/* where the next item of a list goes: a comma and a line before all but
the first */
struct profile_json_list
{
	FILE *file;
	int items;
};

struct profile_json_name_totals
{
	unsigned long long count;
	unsigned long long total;
	unsigned long long maximum;
};

struct profile_json_join_part
{
	char path[520];
	struct profile_json_ranges ranges;
};

#ifdef PROFILE_CHECK
static int profile_json_test_fail_join_pending;
void profile_json_test_fail_join_after_start(void)
{
	profile_json_test_fail_join_pending = 1;
}
#endif

/* ---------- globals */

static const char *profile_json_directions[] = { "out", "in" };
static const char *profile_json_channels[] = { "datagram", "stream" };
static const char *profile_json_drops[NUMBER_OF_PROFILE_NET_DROPS] =
{
	"", "not_in_game", "bad_type", "bad_size", "wrong_direction", "stale", "fast_clock",
};
static const char *profile_json_failures[NUMBER_OF_PROFILE_NET_FAILURES] =
{
	"", "no_server", "no_machine", "no_connection", "too_large", "write_failed",
};
static const char *profile_json_stop_reasons[] = { "", "command", "seconds", "map_load", "exit" };

/* ---------- private code */

static void profile_json_item(
	struct profile_json_list *list)
{
	if (list->items++)
		fputs(",\n", list->file);
}

static void profile_json_escaped(
	FILE *file,
	const char *text)
{
	for (; text && *text; text++)
	{
		unsigned char character = (unsigned char)*text;

		if (character == '"' || character == '\\')
			fprintf(file, "\\%c", character);
		else if (character < 32 || character > 126)
			fprintf(file, "\\u%04x", character);
		else
			fputc(character, file);
	}
}

static void profile_json_string(
	FILE *file,
	const char *text)
{
	fputc('"', file);
	profile_json_escaped(file, text);
	fputc('"', file);
}

static void profile_json_address(
	FILE *file,
	unsigned long ipv4,
	unsigned short port)
{
	if (port)
		fprintf(file, "\"%lu.%lu.%lu.%lu:%u\"", ipv4 & 255, (ipv4 >> 8) & 255, (ipv4 >> 16) & 255, (ipv4 >> 24) & 255, port);
	else
		fprintf(file, "\"%lu.%lu.%lu.%lu\"", ipv4 & 255, (ipv4 >> 8) & 255, (ipv4 >> 16) & 255, (ipv4 >> 24) & 255);
}

static void profile_json_machine(
	FILE *file,
	long machine)
{
	if (machine == PROFILE_NET_HOST)
		fputs("\"host\"", file);
	else if (machine == PROFILE_NET_SERVER_DATAGRAMS)
		fputs("\"server_datagrams\"", file);
	else
		fprintf(file, "%ld", machine);
}

/* microseconds with three decimals, as the trace event format has them */
static void profile_json_time(
	FILE *file,
	unsigned long long nanoseconds)
{
	fprintf(file, "%llu.%03llu", nanoseconds / 1000ULL, nanoseconds % 1000ULL);
}

static int profile_json_compare_records(
	const void *a,
	const void *b)
{
	const struct profile_trace_record *first = a;
	const struct profile_trace_record *second = b;

	if (first->start != second->start)
		return first->start < second->start ? -1 : 1;
	return (int)first->depth - (int)second->depth;
}

static int profile_json_name_of(
	const struct profile_part *part,
	const char *text)
{
	int name;

	for (name = 0; name < part->name_count; name++)
	{
		if (strcmp(profile_trace_name_text(name), text) == 0)
			return name;
	}
	return -1;
}

static void profile_json_header(
	const struct profile_part *part,
	FILE *file)
{
	const struct profile_trace_session *session = &part->session;

	fputs("\"header\": {\"format\": 1, \"build\": ", file);
	profile_json_string(file, session->build);
	fputs(", \"platform\": ", file);
	profile_json_string(file, session->platform);
	fputs(", \"role\": ", file);
	profile_json_string(file, session->role);
	fprintf(file, ", \"own_machine\": %ld, \"map\": ", session->own_machine);
	profile_json_string(file, session->map);
	fputs(", \"map_name\": ", file);
	profile_json_string(file, session->map_name);
	fputs(", \"gametype\": ", file);
	profile_json_string(file, session->gametype);
	fprintf(file, ", \"players\": %ld, \"players_most\": %ld, \"start_utc\": ", session->players,
		session->players_most);
	profile_json_string(file, part->start_utc);
	fputs(", \"recording\": ", file);
	profile_json_string(file, part->name);
	fprintf(file, ", \"part\": %ld, \"last_part\": %s, \"first_interval\": %ld, \"intervals\": %ld"
		", \"first_frame\": %ld, \"frames\": %ld, \"first_tick\": %ld, \"ticks\": %ld",
		part->number, part->last ? "true" : "false", part->first_interval, part->intervals,
		part->first_frame, part->frames, part->first_tick, part->ticks);
	fprintf(file, ", \"start_s\": %.6f, \"duration_s\": %.6f, \"stop_reason\": ",
		(double)part->start_ns / 1e9, (double)(part->end_ns - part->start_ns) / 1e9);
	profile_json_string(file, profile_json_stop_reasons[part->stop_reason]);
	fprintf(file, ", \"clock_read_ns\": %.1f, \"memory_used\": %lu, \"memory_limit\": %lu, \"writer_wait_ms\": %.3f"
		", \"foreign_scopes\": %lu, \"deep_scopes\": %lu, \"unbalanced_scopes\": %lu, \"dropped_scopes\": %lu"
		", \"foreign_net_events\": %lu, \"entry_keys_overflowed\": %lu, \"dropped_rows\": %lu"
		", \"peers_dropped\": %lu, \"machines_dropped\": %lu, \"connections_overflowed\": %lu"
		", \"objects_overflowed\": %lu, \"batches_unbooked\": %lu, \"sites_overflowed\": %lu, \"fields_overflowed\": %lu"
		", \"layouts_overflowed\": %lu}",
		part->clock_read_ns, part->memory_used, part->memory_limit, part->writer_wait_ms,
		part->foreign_scopes, part->deep_scopes, part->unbalanced_scopes, part->dropped_scopes,
		part->counts.foreign_events, part->counts.entry_keys_overflowed, part->counts.dropped_rows,
		part->counts.peers_dropped, part->counts.machines_dropped, part->counts.connections_overflowed,
		part->counts.objects_overflowed, part->counts.batches_unbooked, part->counts.sites_overflowed, part->counts.fields_overflowed,
		part->counts.layouts_overflowed);
}

static void profile_json_table_begin(
	FILE *file,
	const char *name,
	const char *columns)
{
	fprintf(file, ",\n\"%s\": {\"columns\": [%s], \"rows\": [\n", name, columns);
}

static void profile_json_table_end(
	FILE *file,
	struct profile_json_list *list)
{
	fputs(list->items ? "\n]}" : "]}", file);
}

static void profile_json_metadata(
	const struct profile_part *part,
	FILE *file)
{
	struct profile_json_list list;
	long index;
	int member;

	profile_json_table_begin(file, "message_types", "\"id\", \"name\", \"handler\"");
	list.file = file;
	list.items = 0;
	profile_json_item(&list);
	fprintf(file, "[%d, \"batch_header\", \"\"]", PROFILE_NET_BATCH_HEADER);
	profile_json_item(&list);
	fputs("[255, \"short_message\", \"\"]", file);
	for (index = 0; index < part->net.types; index++)
	{
		const struct profile_net_message_name *name = profile_net_type_name_at(index);

		profile_json_item(&list);
		fprintf(file, "[%d, ", name->type);
		profile_json_string(file, name->name);
		fputs(", ", file);
		profile_json_string(file, name->handler);
		fputc(']', file);
	}
	profile_json_table_end(file, &list);

	profile_json_table_begin(file, "sites", "\"id\", \"function\", \"line\"");
	list.items = 0;
	for (index = 0; index < part->net.sites; index++)
	{
		const struct profile_net_site_entry *site = profile_net_site_entry(index);

		profile_json_item(&list);
		fprintf(file, "[%ld, ", index);
		profile_json_string(file, site->function);
		fprintf(file, ", %ld]", site->line);
	}
	profile_json_table_end(file, &list);

	profile_json_table_begin(file, "fields", "\"id\", \"name\", \"size\"");
	list.items = 0;
	for (index = 0; index < part->net.fields; index++)
	{
		profile_json_item(&list);
		fprintf(file, "[%ld, ", index);
		profile_json_string(file, profile_net_field_entry(index)->name);
		fputs(", 0]", file);
	}
	profile_json_table_end(file, &list);

	profile_json_table_begin(file, "layouts", "\"type\", \"member\", \"offset\", \"size\", \"key\"");
	list.items = 0;
	for (index = 0; index < part->net.layouts; index++)
	{
		const struct profile_net_layout_entry *layout = profile_net_layout_entry(index);

		unsigned int end = 0;

		for (member = 0; member < layout->count; member++)
		{
			profile_json_item(&list);
			fprintf(file, "[%d, ", layout->type);
			profile_json_string(file, layout->members[member].name);
			fprintf(file, ", %u, %u, %s]", layout->members[member].offset, layout->members[member].size,
				layout->members[member].key == _profile_net_key_datum ? "\"datum\"" :
				layout->members[member].key == _profile_net_key_player ? "\"player\"" : "\"\"");
			end = layout->members[member].offset + layout->members[member].size;
		}
		/* (the struct's padding after its last member goes on the wire too) */
		if (layout->entry_size > end)
		{
			profile_json_item(&list);
			fprintf(file, "[%d, \"tail_pad\", %u, %u, \"\"]", layout->type, end, layout->entry_size - end);
		}
	}
	profile_json_table_end(file, &list);

	profile_json_table_begin(file, "machines", "\"machine\", \"address\", \"tunnel_peer\"");
	list.items = 0;
	for (index = 0; index < part->net.machines; index++)
	{
		const struct profile_net_machine_entry *machine = profile_net_machine_entry(index);

		profile_json_item(&list);
		fputc('[', file);
		profile_json_machine(file, machine->machine);
		fputs(", ", file);
		profile_json_address(file, machine->ipv4, machine->port);
		fputs(", ", file);
		if (machine->peer_ipv4)
			profile_json_address(file, machine->peer_ipv4, 0);
		else
			fputs("\"\"", file);
		fputc(']', file);
	}
	profile_json_table_end(file, &list);

	profile_json_table_begin(file, "connections", "\"id\", \"address\", \"machine\"");
	list.items = 0;
	for (index = 0; index < part->net.connections; index++)
	{
		const struct profile_net_connection_entry *connection = profile_net_connection_entry(index);

		profile_json_item(&list);
		fprintf(file, "[%ld, ", index);
		profile_json_address(file, connection->ipv4, connection->port);
		fputs(", ", file);
		profile_json_machine(file, __atomic_load_n(&connection->machine, __ATOMIC_RELAXED));
		fputc(']', file);
	}
	profile_json_table_end(file, &list);

	profile_json_table_begin(file, "objects", "\"key\", \"object_type\", \"tag\"");
	list.items = 0;
	for (index = 0; index < part->net.objects; index++)
	{
		const struct profile_net_object_entry *object = profile_net_object_entry(index);

		profile_json_item(&list);
		fprintf(file, "[%ld, ", object->key);
		profile_json_string(file, object->object_type);
		fputs(", ", file);
		profile_json_string(file, object->tag);
		fputc(']', file);
	}
	profile_json_table_end(file, &list);
}

static const char *profile_json_handler(
	int type)
{
	const struct profile_net_message_name *name = profile_net_type_name(type);

	return name ? name->handler : "";
}

/* the rows of one table, in the columns that table has */
static void profile_json_rows(
	const struct profile_part *part,
	FILE *file,
	int table,
	const char *name,
	const char *columns)
{
	struct profile_json_list list;
	unsigned long index;

	profile_json_table_begin(file, name, columns);
	list.file = file;
	list.items = 0;
	for (index = 0; index < part->row_count; index++)
	{
		const struct profile_net_row *row = &part->rows[index];

		if (row->table != table)
			continue;
		profile_json_item(&list);
		fprintf(file, "[%u, ", row->interval);
		switch (table)
		{
		case _profile_net_table_intervals:
			fprintf(file, "%.3f, %.3f, %d, %u]", row->values[0] / 1000.0, row->values[1] / 1000.0, row->machine,
				row->values[2]);
			break;
		case _profile_net_table_messages:
			fprintf(file, "\"%s\", ", profile_json_directions[row->direction]);
			profile_json_machine(file, row->machine);
			fprintf(file, ", %u, %d, %u, %u, %u, %s]", row->type, row->site == PROFILE_NET_NO_SITE ? -1 : row->site,
				row->values[0], row->values[1], row->values[2], row->aux ? "true" : "false");
			break;
		case _profile_net_table_received:
			profile_json_machine(file, row->machine);
			fprintf(file, ", %u, ", row->type);
			profile_json_string(file, profile_json_handler(row->type));
			fprintf(file, ", %u, %u, %u, \"%s\"]", row->values[0], row->values[1], row->values[2],
				row->aux < NUMBER_OF_PROFILE_NET_DROPS ? profile_json_drops[row->aux] : "");
			break;
		case _profile_net_table_built:
			fprintf(file, "%u, %d, %u]", row->type, row->site == PROFILE_NET_NO_SITE ? -1 : row->site, row->values[0]);
			break;
		case _profile_net_table_entries:
			fprintf(file, "\"%s\", ", profile_json_directions[row->direction]);
			profile_json_machine(file, row->machine);
			fprintf(file, ", %u, ", row->type);
			if (row->key == PROFILE_NET_KEY_OTHER)
				fputs("\"other\"", file);
			else if (row->key == PROFILE_NET_KEY_NONE)
				fputs("\"\"", file);
			else
				fprintf(file, "%d", row->key);
			fprintf(file, ", %u, %u]", row->values[0], row->values[1]);
			break;
		case _profile_net_table_field_bytes:
			fprintf(file, "\"%s\", %u, %u, %u, %u]", profile_json_directions[row->direction], row->type, row->site,
				row->values[0], row->values[1]);
			break;
		case _profile_net_table_send_failures:
			profile_json_machine(file, row->machine);
			fprintf(file, ", \"%s\", %u, %u, %s]", row->aux < NUMBER_OF_PROFILE_NET_FAILURES ? profile_json_failures[row->aux] : "",
				row->values[0], row->values[1], row->type ? "true" : "false");
			break;
		case _profile_net_table_traffic:
			fprintf(file, "\"%s\", \"%s\", %d, %u, %u]", profile_json_directions[row->direction],
				profile_json_channels[row->aux & 1], row->key, row->values[0], row->values[1]);
			break;
		case _profile_net_table_queues:
			fprintf(file, "%d, %u]", row->key, row->values[0]);
			break;
		case _profile_net_table_pings:
			profile_json_machine(file, row->machine);
			fprintf(file, ", %u]", row->values[0]);
			break;
		case _profile_net_table_simulated_loss:
			fprintf(file, "%u]", row->values[0]);
			break;
		case _profile_net_table_datagrams:
			profile_json_machine(file, row->machine);
			fprintf(file, ", %u, %u]", row->values[0], row->values[1]);
			break;
		}
	}
	profile_json_table_end(file, &list);
}

enum
{
	/* the index of one interval's rows: at most half full of the rows an interval can have */
	PROFILE_JSON_TUNNEL_SLOTS = 65536,
};

typedef char profile_json_tunnel_slots_assert[PROFILE_JSON_TUNNEL_SLOTS >= 2 * PROFILE_NET_MAXIMUM_INTERVAL_ROWS ? 1 : -1];

/* a peer's packets and KCP rows of the interval being written: the last of
each in row order, as an index plus one */
struct profile_json_tunnel_slot
{
	int key;
	unsigned long packets;
	unsigned long kcp;
	int used;
};

/* (static: one writer thread runs at a time, and an index that is only ever
the size of an interval's rows costs a part no allocation that could fail) */
static struct profile_json_tunnel_slot profile_json_tunnel_slots[PROFILE_JSON_TUNNEL_SLOTS];
static unsigned int profile_json_tunnel_used[PROFILE_JSON_TUNNEL_SLOTS / 2];

static struct profile_json_tunnel_slot *profile_json_tunnel_find(
	int key,
	int create,
	unsigned long *used_count)
{
	unsigned long slot = (((unsigned long)(unsigned int)key * 2654435761UL) >> 7) & (PROFILE_JSON_TUNNEL_SLOTS - 1);

	while (profile_json_tunnel_slots[slot].used)
	{
		if (profile_json_tunnel_slots[slot].key == key)
			return &profile_json_tunnel_slots[slot];
		slot = (slot + 1) & (PROFILE_JSON_TUNNEL_SLOTS - 1);
	}
	if (!create || *used_count >= PROFILE_JSON_TUNNEL_SLOTS / 2)
		return NULL;
	profile_json_tunnel_slots[slot].used = 1;
	profile_json_tunnel_slots[slot].key = key;
	profile_json_tunnel_used[(*used_count)++] = (unsigned int)slot;
	return &profile_json_tunnel_slots[slot];
}

/* the tunnel's three rows of a peer and interval as one. A part's rows are in
interval order (profile_net.c gives each the interval it is made in), so each
run of an interval is indexed, written and forgotten in turn: a part has
millions of rows and a run at most an interval's */
static void profile_json_tunnel(
	const struct profile_part *part,
	FILE *file)
{
	struct profile_json_list list;
	unsigned long start, end, index, used_count;

	profile_json_table_begin(file, "tunnel", "\"interval\", \"peer\", \"bytes_out\", \"bytes_in\", \"packets_out\", "
		"\"packets_in\", \"lost_in\", \"kcp_payload\", \"kcp_output\", \"round_trip_ms\"");
	list.file = file;
	list.items = 0;
	for (start = 0; start < part->row_count; start = end)
	{
		for (end = start; end < part->row_count && part->rows[end].interval == part->rows[start].interval; end++)
			;
		used_count = 0;
		for (index = start; index < end; index++)
		{
			const struct profile_net_row *row = &part->rows[index];
			struct profile_json_tunnel_slot *slot;

			if (row->table != _profile_net_table_tunnel_packets && row->table != _profile_net_table_tunnel_kcp)
				continue;
			slot = profile_json_tunnel_find(row->key, 1, &used_count);
			if (!slot)
				continue;
			if (row->table == _profile_net_table_tunnel_packets)
				slot->packets = index + 1;
			else
				slot->kcp = index + 1;
		}
		for (index = start; index < end; index++)
		{
			const struct profile_net_row *row = &part->rows[index];
			const struct profile_json_tunnel_slot *found;
			unsigned int packets[3] = { 0, 0, 0 };
			unsigned int kcp[2] = { 0, 0 };

			if (row->table != _profile_net_table_tunnel_bytes)
				continue;
			/* (only rows after this one count for it) */
			found = used_count ? profile_json_tunnel_find(row->key, 0, &used_count) : NULL;
			if (found && found->packets > index + 1)
				memcpy(packets, part->rows[found->packets - 1].values, sizeof(packets));
			if (found && found->kcp > index + 1)
				memcpy(kcp, part->rows[found->kcp - 1].values, sizeof(kcp));
			profile_json_item(&list);
			fprintf(file, "[%u, ", row->interval);
			profile_json_address(file, (unsigned long)(unsigned int)row->key, 0);
			fprintf(file, ", %u, %u, %u, %u, %u, %u, %u, %u]", row->values[0], row->values[1], packets[0], packets[1],
				packets[2], kcp[0], kcp[1], row->values[2]);
		}
		for (index = 0; index < used_count; index++)
		{
			struct profile_json_tunnel_slot *slot = &profile_json_tunnel_slots[profile_json_tunnel_used[index]];

			slot->used = 0;
			slot->packets = slot->kcp = 0;
		}
	}
	profile_json_table_end(file, &list);
}

/* a worst frame's scopes: the open ones (one a depth, so no more than the
depth limit) and the three with the most time of their own so far */
struct profile_json_ranking
{
	unsigned long scope[MAXIMUM_PROFILE_TRACE_DEPTH];
	unsigned long long children[MAXIMUM_PROFILE_TRACE_DEPTH];
	int open;
	unsigned long longest[PROFILE_JSON_LONGEST_CHILDREN];
	unsigned int own[PROFILE_JSON_LONGEST_CHILDREN];
	int count;
};

/* the innermost open scope ends: its time less its children's is ranked, and
all of it is its parent's child time */
static void profile_json_close(
	struct profile_json_ranking *ranking,
	const struct profile_trace_record *game)
{
	int open = --ranking->open;
	unsigned long scope = ranking->scope[open];
	unsigned long long duration = game[scope].duration;
	unsigned int own = (unsigned int)(duration > ranking->children[open] ? duration - ranking->children[open] : 0);
	int slot;

	for (slot = ranking->count; slot > 0 && ranking->own[slot - 1] < own; slot--)
	{
		if (slot < PROFILE_JSON_LONGEST_CHILDREN)
		{
			ranking->longest[slot] = ranking->longest[slot - 1];
			ranking->own[slot] = ranking->own[slot - 1];
		}
	}
	if (slot < PROFILE_JSON_LONGEST_CHILDREN)
	{
		ranking->longest[slot] = scope;
		ranking->own[slot] = own;
		if (ranking->count < PROFILE_JSON_LONGEST_CHILDREN)
			ranking->count++;
	}
	if (open > 0)
		ranking->children[open - 1] += duration;
}

static void profile_json_cpu_summary(
	const struct profile_part *part,
	FILE *file)
{
	struct profile_json_name_totals totals[MAXIMUM_PROFILE_TRACE_NAMES];
	const struct profile_trace_record *game = part->records[_profile_track_game];
	unsigned long game_count = part->record_counts[_profile_track_game];
	unsigned long worst[PROFILE_JSON_WORST_FRAMES];
	long worst_frame_number[PROFILE_JSON_WORST_FRAMES];
	int worst_count = 0;
	struct profile_json_ranking ranking;
	int frame_name = profile_json_name_of(part, "frame");
	int tick_name = profile_json_name_of(part, "game_tick");
	struct profile_json_list list;
	unsigned long index;
	long frames = 0;
	int track, name;

	memset(totals, 0, sizeof(totals));
	for (track = 0; track < NUMBER_OF_PROFILE_TRACKS; track++)
	{
		for (index = 0; index < part->record_counts[track]; index++)
		{
			const struct profile_trace_record *record = &part->records[track][index];
			struct profile_json_name_totals *name_totals;

			if (record->name >= MAXIMUM_PROFILE_TRACE_NAMES)
				continue;
			name_totals = &totals[record->name];
			if (record->depth < MAXIMUM_PROFILE_TRACE_DEPTH || record->depth == PROFILE_TRACE_DEPTH_AGGREGATE_TIME)
			{
				name_totals->total += record->duration;
				if (record->duration > name_totals->maximum)
					name_totals->maximum = record->duration;
			}
			if (record->depth < MAXIMUM_PROFILE_TRACE_DEPTH)
				name_totals->count++;
			else if (record->depth == PROFILE_TRACE_DEPTH_AGGREGATE_COUNT)
				name_totals->count += record->duration;
		}
	}
	/* the worst frames: the game track is sorted, so a frame's children
	follow it */
	for (index = 0; index < game_count; index++)
	{
		int slot;

		if (game[index].depth >= MAXIMUM_PROFILE_TRACE_DEPTH || game[index].name != frame_name)
			continue;
		frames++;
		for (slot = worst_count; slot > 0 && game[worst[slot - 1]].duration < game[index].duration; slot--)
		{
			if (slot < PROFILE_JSON_WORST_FRAMES)
			{
				worst[slot] = worst[slot - 1];
				worst_frame_number[slot] = worst_frame_number[slot - 1];
			}
		}
		if (slot < PROFILE_JSON_WORST_FRAMES)
		{
			worst[slot] = index;
			worst_frame_number[slot] = part->first_frame + frames - 1;
			if (worst_count < PROFILE_JSON_WORST_FRAMES)
				worst_count++;
		}
	}

	fprintf(file, "{\"frames\": %ld, \"ticks\": %ld, \"columns\": [\"name\", \"count\", \"total_ms\", "
		"\"mean_ms\", \"max_ms\", \"per_frame\", \"per_tick_ms\"], \"rows\": [\n", frames, part->ticks);
	list.file = file;
	list.items = 0;
	for (name = 0; name < part->name_count && name < MAXIMUM_PROFILE_TRACE_NAMES; name++)
	{
		const char *text = profile_trace_name_text(name);
		struct profile_json_name_totals const *name_totals = &totals[name];
		int per_tick = strncmp(text, "game_tick", 9) == 0 || strncmp(text, "network_distributed_tick", 24) == 0;

		if (!name_totals->count)
			continue;
		profile_json_item(&list);
		fputc('[', file);
		profile_json_string(file, text);
		fprintf(file, ", %llu, %.3f, %.4f, %.3f, %.3f, ", name_totals->count, name_totals->total / 1e6,
			name_totals->total / 1e6 / (double)name_totals->count, name_totals->maximum / 1e6,
			frames ? (double)name_totals->count / (double)frames : 0.0);
		if (per_tick && part->ticks)
			fprintf(file, "%.4f]", name_totals->total / 1e6 / (double)part->ticks);
		else
			fputs("null]", file);
	}
	fputs(list.items ? "\n]" : "]", file);

	fputs(", \"worst_frames\": {\"columns\": [\"frame\", \"at_s\", \"frame_ms\", \"ticks\", \"longest\"], \"rows\": [\n", file);
	list.items = 0;
	for (index = 0; index < (unsigned long)worst_count; index++)
	{
		const struct profile_trace_record *frame = &game[worst[index]];
		unsigned long long end = frame->start + frame->duration;
		int ticks = 0;
		unsigned long child;
		int slot;

		/* (a tick sits under whatever the main loop wraps it in, so every
		depth of the frame counts; the scopes are ranked by the time they have
		of their own, or the parents, which hold all of it, would always come
		first. The open scopes are a stack, so nothing is allocated for it) */
		memset(&ranking, 0, sizeof(ranking));
		for (child = worst[index] + 1; child < game_count && game[child].start < end; child++)
		{
			if (game[child].depth >= MAXIMUM_PROFILE_TRACE_DEPTH || game[child].depth <= frame->depth)
				continue;
			if (game[child].name == tick_name)
				ticks++;
			while (ranking.open && game[ranking.scope[ranking.open - 1]].depth >= game[child].depth)
				profile_json_close(&ranking, game);
			ranking.scope[ranking.open] = child;
			ranking.children[ranking.open++] = 0;
		}
		while (ranking.open)
			profile_json_close(&ranking, game);
		profile_json_item(&list);
		fprintf(file, "[%ld, %.3f, %.3f, %d, [", worst_frame_number[index], frame->start / 1e9, frame->duration / 1e6,
			ticks);
		for (slot = 0; slot < ranking.count; slot++)
		{
			fputs(slot ? ", [" : "[", file);
			profile_json_string(file, profile_trace_name_text(game[ranking.longest[slot]].name));
			fprintf(file, ", %.3f]", ranking.own[slot] / 1e6);
		}
		fputs("]]", file);
	}
	fputs(list.items ? "\n]}}" : "]}}", file);
}

static void profile_json_event(
	const struct profile_part *part,
	FILE *file,
	struct profile_json_list *list,
	const struct profile_trace_record *record)
{
	const char *name = profile_trace_name_text(record->name);

	(void)part;
	profile_json_item(list);
	switch (record->depth)
	{
	case PROFILE_TRACE_DEPTH_AGGREGATE_TIME:
	case PROFILE_TRACE_DEPTH_AGGREGATE_COUNT:
		fputs("{\"ph\":\"C\",\"name\":\"", file);
		profile_json_escaped(file, name);
		fputs(record->depth == PROFILE_TRACE_DEPTH_AGGREGATE_TIME ? "_ms\",\"pid\":1,\"tid\":1,\"ts\":" :
			"_count\",\"pid\":1,\"tid\":1,\"ts\":", file);
		profile_json_time(file, record->start);
		if (record->depth == PROFILE_TRACE_DEPTH_AGGREGATE_TIME)
			fprintf(file, ",\"args\":{\"value\":%.4f}}", record->duration / 1e6);
		else
			fprintf(file, ",\"args\":{\"value\":%u}}", record->duration);
		break;
	case PROFILE_TRACE_DEPTH_NET_OUT:
	case PROFILE_TRACE_DEPTH_NET_IN:
		fprintf(file, "{\"ph\":\"C\",\"name\":\"%s\",\"pid\":1,\"tid\":1,\"ts\":",
			record->depth == PROFILE_TRACE_DEPTH_NET_OUT ? "net_out" : "net_in");
		profile_json_time(file, record->start);
		fprintf(file, ",\"args\":{\"bytes\":%u}}", record->duration);
		break;
	default:
		fputs("{\"ph\":\"X\",\"name\":", file);
		profile_json_string(file, name);
		fprintf(file, ",\"pid\":1,\"tid\":%d,\"ts\":", record->track + 1);
		profile_json_time(file, record->start);
		fputs(",\"dur\":", file);
		profile_json_time(file, record->duration);
		fputc('}', file);
		break;
	}
}

/* each interval's rates, as counters at its start: the rows of each run of an
interval (see profile_json_tunnel) summed, and written for its interval row */
static void profile_json_second_counters(
	const struct profile_part *part,
	FILE *file,
	struct profile_json_list *list)
{
	unsigned long start, end, index;

	for (start = 0; start < part->row_count; start = end)
	{
		double game[2] = { 0.0, 0.0 }, wire[2] = { 0.0, 0.0 };
		double ping = 0.0, lost = 0.0, received = 0.0;
		int pings = 0;

		for (end = start; end < part->row_count && part->rows[end].interval == part->rows[start].interval; end++)
		{
			const struct profile_net_row *row = &part->rows[end];

			if (row->table == _profile_net_table_traffic)
				game[row->direction & 1] += row->values[0];
			else if (row->table == _profile_net_table_tunnel_bytes)
			{
				wire[0] += row->values[0];
				wire[1] += row->values[1];
			}
			else if (row->table == _profile_net_table_tunnel_packets)
			{
				received += row->values[1];
				lost += row->values[2];
			}
			else if (row->table == _profile_net_table_pings)
			{
				ping += row->values[0];
				pings++;
			}
		}
		for (index = start; index < end; index++)
		{
			const struct profile_net_row *interval = &part->rows[index];
			double seconds = interval->values[1] / 1000.0;

			if (interval->table != _profile_net_table_intervals || seconds <= 0.0 ||
				(unsigned int)interval->key != interval->interval)
			{
				continue;
			}
			profile_json_item(list);
			fputs("{\"ph\":\"C\",\"name\":\"game_Bps\",\"pid\":1,\"tid\":1,\"ts\":", file);
			profile_json_time(file, (unsigned long long)interval->values[0] * 1000000ULL);
			fprintf(file, ",\"args\":{\"up\":%.0f,\"down\":%.0f}}", game[0] / seconds, game[1] / seconds);
			profile_json_item(list);
			fputs("{\"ph\":\"C\",\"name\":\"wire_Bps\",\"pid\":1,\"tid\":1,\"ts\":", file);
			profile_json_time(file, (unsigned long long)interval->values[0] * 1000000ULL);
			fprintf(file, ",\"args\":{\"up\":%.0f,\"down\":%.0f}}", wire[0] / seconds, wire[1] / seconds);
			profile_json_item(list);
			fputs("{\"ph\":\"C\",\"name\":\"ping_ms\",\"pid\":1,\"tid\":1,\"ts\":", file);
			profile_json_time(file, (unsigned long long)interval->values[0] * 1000000ULL);
			fprintf(file, ",\"args\":{\"value\":%.0f}}", pings ? ping / pings : 0.0);
			profile_json_item(list);
			fputs("{\"ph\":\"C\",\"name\":\"loss_pct\",\"pid\":1,\"tid\":1,\"ts\":", file);
			profile_json_time(file, (unsigned long long)interval->values[0] * 1000000ULL);
			fprintf(file, ",\"args\":{\"value\":%.2f}}", received + lost > 0.0 ? 100.0 * lost / (received + lost) : 0.0);
		}
	}
}

/* ---------- public code */

int profile_json_write(
	struct profile_part *part,
	FILE *file,
	struct profile_json_ranges *ranges)
{
	struct profile_json_list list;
	unsigned long next[NUMBER_OF_PROFILE_TRACKS] = { 0, 0 };
	int track;

	for (track = 0; track < NUMBER_OF_PROFILE_TRACKS; track++)
	{
		if (part->record_counts[track])
		{
			qsort(part->records[track], part->record_counts[track], sizeof(struct profile_trace_record),
				profile_json_compare_records);
		}
	}

	fputs("{\"halo\": ", file);
	if (ranges)
		ranges->halo_start = (long long)profile_json_tell(file);
	fputs("{\n", file);
	profile_json_header(part, file);
	profile_json_metadata(part, file);
	profile_json_rows(part, file, _profile_net_table_intervals, "intervals",
		"\"interval\", \"start_s\", \"length_s\", \"tick\", \"ticks\"");
	profile_json_rows(part, file, _profile_net_table_messages, "messages",
		"\"interval\", \"dir\", \"machine\", \"type\", \"site\", \"bytes\", \"messages\", \"entries\", \"reliable\"");
	profile_json_rows(part, file, _profile_net_table_received, "received",
		"\"interval\", \"machine\", \"type\", \"handler\", \"bytes\", \"messages\", \"entries\", \"dropped\"");
	profile_json_rows(part, file, _profile_net_table_built, "built", "\"interval\", \"type\", \"site\", \"messages\"");
	profile_json_rows(part, file, _profile_net_table_entries, "entries",
		"\"interval\", \"dir\", \"machine\", \"type\", \"key\", \"bytes\", \"entries\"");
	profile_json_rows(part, file, _profile_net_table_field_bytes, "field_bytes",
		"\"interval\", \"dir\", \"type\", \"field\", \"bytes\", \"count\"");
	profile_json_rows(part, file, _profile_net_table_send_failures, "send_failures",
		"\"interval\", \"machine\", \"reason\", \"sends\", \"bytes\", \"reliable\"");
	profile_json_rows(part, file, _profile_net_table_traffic, "traffic",
		"\"interval\", \"dir\", \"channel\", \"connection\", \"bytes\", \"packets\"");
	profile_json_rows(part, file, _profile_net_table_queues, "queues", "\"interval\", \"connection\", \"bytes\"");
	profile_json_tunnel(part, file);
	profile_json_rows(part, file, _profile_net_table_pings, "pings", "\"interval\", \"machine\", \"ping_ms\"");
	profile_json_rows(part, file, _profile_net_table_simulated_loss, "simulated_loss", "\"interval\", \"datagrams\"");
	profile_json_rows(part, file, _profile_net_table_datagrams, "datagrams",
		"\"interval\", \"machine\", \"bytes\", \"datagrams\"");
	fputs("}", file);
	if (ranges)
	{
		ranges->halo_end = (long long)profile_json_tell(file);
	}
	fputs(",\n\"cpu_summary\": ", file);
	if (ranges)
		ranges->cpu_start = (long long)profile_json_tell(file);
	profile_json_cpu_summary(part, file);
	if (ranges)
		ranges->cpu_end = (long long)profile_json_tell(file);
	fputs(",\n\"traceEvents\": [\n", file);
	if (ranges)
		ranges->events_start = (long long)profile_json_tell(file);

	list.file = file;
	list.items = 0;
	profile_json_item(&list);
	fprintf(file, "{\"ph\":\"M\",\"name\":\"process_name\",\"pid\":1,\"args\":{\"name\":\"halo %s %s\"}}",
		part->session.role, part->session.platform);
	profile_json_item(&list);
	fputs("{\"ph\":\"M\",\"name\":\"thread_name\",\"pid\":1,\"tid\":1,\"args\":{\"name\":\"game\"}}", file);
	profile_json_item(&list);
	fputs("{\"ph\":\"M\",\"name\":\"thread_name\",\"pid\":1,\"tid\":2,\"args\":{\"name\":\"p2p\"}}", file);
	if (part->number == 1)
	{
		profile_json_item(&list);
		fputs("{\"ph\":\"i\",\"name\":\"recording_start\",\"s\":\"g\",\"pid\":1,\"tid\":1,\"ts\":0.000}", file);
	}
	/* the two tracks merged, in order of start and depth: each scope after
	its parents */
	for (;;)
	{
		const struct profile_trace_record *game = next[0] < part->record_counts[0] ? &part->records[0][next[0]] : NULL;
		const struct profile_trace_record *p2p = next[1] < part->record_counts[1] ? &part->records[1][next[1]] : NULL;

		if (!game && !p2p)
			break;
		if (game && (!p2p || profile_json_compare_records(game, p2p) <= 0))
		{
			profile_json_event(part, file, &list, game);
			next[0]++;
		}
		else
		{
			profile_json_event(part, file, &list, p2p);
			next[1]++;
		}
	}
	profile_json_second_counters(part, file, &list);
	if (part->last)
	{
		profile_json_item(&list);
		fputs("{\"ph\":\"i\",\"name\":\"recording_stop\",\"s\":\"g\",\"pid\":1,\"tid\":1,\"ts\":", file);
		profile_json_time(file, part->end_ns);
		fprintf(file, ",\"args\":{\"reason\":\"%s\"}}", profile_json_stop_reasons[part->stop_reason]);
	}
	if (ranges)
		ranges->events_end = (long long)profile_json_tell(file);
	fputs("\n],\n\"displayTimeUnit\": \"ms\"}\n", file);
	return !ferror(file);
}

void profile_json_choose_name(
	const char *folder,
	const char *stamp,
	const char *role,
	char *name,
	int size)
{
	char path[512];
	int suffix;

	snprintf(name, (size_t)size, "profile_%s_%s", stamp, role);
	for (suffix = 2; suffix < 1000; suffix++)
	{
		FILE *file;
		int occupied = 0;

		snprintf(path, sizeof(path), "%s/%s.part1.json", folder, name);
		file = fopen(path, "rb");
		if (file)
		{
			fclose(file);
			occupied = 1;
		}
		snprintf(path, sizeof(path), "%s/%s.json", folder, name);
		file = fopen(path, "rb");
		if (file)
		{
			fclose(file);
			occupied = 1;
		}
		if (!occupied)
			break;
		snprintf(name, (size_t)size, "profile_%s_%s_%d", stamp, role, suffix);
	}
}

static void profile_json_component(const char *source, char *target, size_t size, int path_component)
{
	const char *start = source;
	const char *cursor;
	size_t used = 0;

	if (path_component)
	{
		for (cursor = source; *cursor; cursor++)
			if (*cursor == '/' || *cursor == '\\')
				start = cursor + 1;
	}
	for (cursor = start; *cursor && used + 1 < size; cursor++)
	{
		unsigned char character = (unsigned char)*cursor;
		if (character >= 'A' && character <= 'Z')
			character = (unsigned char)(character + ('a' - 'A'));
		target[used++] = (character >= 'a' && character <= 'z') || (character >= '0' && character <= '9') ||
			character == '_' || character == '-' ? (char)character : '-';
	}
	target[used] = 0;
	if (!used)
		snprintf(target, size, "nomap");
}

void profile_json_joined_name(const char *folder, const char *part_name,
	const struct profile_trace_session *session, char *name, int size)
{
	char map[64], role[16], gametype[16], stamp[32], path[512];
	char temporary[520];
	const char *begin = part_name + strlen("profile_");
	const char *end = strchr(begin, '_');
	int suffix;

	snprintf(stamp, sizeof(stamp), "%.*s", end ? (int)(end - begin) : 0, begin);
	profile_json_component(session->map_name[0] ? session->map_name : session->map, map, sizeof(map), 1);
	profile_json_component(session->role, role, sizeof(role), 0);
	profile_json_component(session->gametype, gametype, sizeof(gametype), 0);
	snprintf(name, (size_t)size, "profile_%s_%s_%s_%s", stamp, role, map, gametype);
	for (suffix = 2; suffix < 1000; suffix++)
	{
		FILE *file;
		int occupied = 0;
		snprintf(path, sizeof(path), "%s/%s.json", folder, name);
		file = fopen(path, "rb");
		if (file)
		{
			fclose(file);
			occupied = 1;
		}
		snprintf(temporary, sizeof(temporary), "%s.tmp", path);
		file = fopen(temporary, "rb");
		if (file)
		{
			fclose(file);
			occupied = 1;
		}
		if (!occupied)
			break;
		snprintf(name, (size_t)size, "profile_%s_%s_%s_%s_%d", stamp, role, map, gametype, suffix);
	}
}

int profile_json_write_part(
	struct profile_part *part,
	struct profile_json_ranges *ranges)
{
	char temporary[520];
	char path[512];
	FILE *file;
	int written;

	snprintf(path, sizeof(path), "%s/%s.part%ld.json", part->folder, part->name, part->number);
	snprintf(temporary, sizeof(temporary), "%s.tmp", path);
	file = fopen(temporary, "wb");
	if (!file)
	{
		profile_trace_log("cannot write %s", temporary);
		return 0;
	}
	written = profile_json_write(part, file, ranges);
	if (fclose(file) != 0)
		written = 0;
	/* (Windows' rename does not replace a file: the recording's name was
	chosen not to be there) */
	if (written && rename(temporary, path) != 0)
		written = 0;
	if (!written)
	{
		remove(temporary);
		profile_trace_log("cannot write %s", path);
		return 0;
	}
	profile_trace_log("wrote %s", path);
	return 1;
}

int profile_json_range_length(long long start, long long end, long long *length)
{
	if (!length || start < 0 || end < start)
		return 0;
	*length = end - start;
	return 1;
}

static int profile_json_copy_range(FILE *output, const char *path, long long start, long long end)
{
	unsigned char buffer[65536];
	FILE *input;
	long long remaining;
	int ok;

	if (!profile_json_range_length(start, end, &remaining))
		return 0;
	input = fopen(path, "rb");
	ok = input != NULL && profile_json_seek(input, (profile_json_offset)start) == 0;

	while (ok && remaining > 0)
	{
		size_t size = remaining < (long long)sizeof(buffer) ? (size_t)remaining : sizeof(buffer);
		if (fread(buffer, 1, size, input) != size || fwrite(buffer, 1, size, output) != size)
			ok = 0;
		remaining -= (long long)size;
	}
	if (input && fclose(input) != 0)
		ok = 0;
	return ok && remaining == 0;
}

static int profile_json_join(
	const struct profile_json_join_part *parts,
	size_t count,
	const char *folder,
	const char *name)
{
	char output[520], temporary[540];
	FILE *joined = NULL;
	size_t index;
	int ok = count > 0;
	int temporary_created = 0;

	snprintf(output, sizeof(output), "%s/%s.json", folder, name);
	snprintf(temporary, sizeof(temporary), "%s.tmp", output);
	if (ok)
	{
		FILE *exists = fopen(output, "rb");
		if (exists)
		{
			fclose(exists);
			ok = 0;
		}
	}
	if (ok)
	{
		joined = fopen(temporary, "wb");
		temporary_created = joined != NULL;
		ok = joined != NULL;
	}
	if (ok)
		ok = fputs("{\"halo_parts\": [", joined) >= 0;
#ifdef PROFILE_CHECK
	if (ok && profile_json_test_fail_join_pending)
	{
		profile_json_test_fail_join_pending = 0;
		ok = 0;
	}
#endif
	for (index = 0; ok && index < count; index++)
	{
		if (index && fputs(", ", joined) < 0)
			ok = 0;
		if (ok)
			ok = profile_json_copy_range(joined, parts[index].path, parts[index].ranges.halo_start,
				parts[index].ranges.halo_end);
	}
	if (ok)
		ok = fputs("],\n\"cpu_summary_parts\": [", joined) >= 0;
	for (index = 0; ok && index < count; index++)
	{
		if (index && fputs(", ", joined) < 0)
			ok = 0;
		if (ok)
			ok = profile_json_copy_range(joined, parts[index].path, parts[index].ranges.cpu_start,
				parts[index].ranges.cpu_end);
	}
	if (ok)
		ok = fputs("],\n\"traceEvents\": [\n", joined) >= 0;
	for (index = 0; ok && index < count; index++)
	{
		if (index && fputs(",\n", joined) < 0)
			ok = 0;
		if (ok)
			ok = profile_json_copy_range(joined, parts[index].path, parts[index].ranges.events_start,
				parts[index].ranges.events_end);
	}
	if (ok)
		ok = fputs("\n],\n\"displayTimeUnit\": \"ms\"}\n", joined) >= 0;
	if (joined && fclose(joined) != 0)
		ok = 0;
	if (ok && rename(temporary, output) != 0)
		ok = 0;
	if (!ok)
	{
		if (temporary_created)
			remove(temporary);
		profile_trace_log("cannot join %s", output);
		return 0;
	}
	for (index = 0; index < count; index++)
	{
		if (remove(parts[index].path) != 0)
			profile_trace_log("cannot delete joined part %s", parts[index].path);
	}
	profile_trace_log("joined %s.json, %lu parts", name, (unsigned long)count);
	return 1;
}

static void *profile_json_writer(
	void *unused)
{
	struct profile_json_join_part *parts = NULL;
	size_t count = 0, capacity = 0;
	int failed = 0;

	(void)unused;
	for (;;)
	{
		struct profile_part *part = profile_trace_writer_next();
		int last = part->last;
		struct profile_json_ranges ranges;
		int written = profile_json_write_part(part, &ranges);

		if (written && !failed)
		{
			struct profile_json_join_part *grown;

			if (count == capacity)
			{
				size_t next = capacity ? capacity * 2 : 4;
				grown = realloc(parts, next * sizeof(*parts));
				if (!grown)
					failed = 1;
				else
					parts = grown, capacity = next;
			}
			if (!failed)
			{
				snprintf(parts[count].path, sizeof(parts[count].path), "%s/%s.part%ld.json",
					part->folder, part->name, part->number);
				parts[count++].ranges = ranges;
			}
		}
		else
			failed = 1;
		if (last)
		{
			if (!failed && count == (size_t)part->number)
			{
				char joined_name[128];
				profile_json_joined_name(part->folder, part->name, &part->session, joined_name, sizeof(joined_name));
				profile_json_join(parts, count, part->folder, joined_name);
			}
			else
				profile_trace_log("cannot join %s/%s.json", part->folder, part->name);
			free(parts);
			parts = NULL;
			count = capacity = 0;
			failed = 0;
		}
		profile_trace_writer_done(part);
		if (last)
			break;
	}
	return NULL;
}

int profile_json_writer_start(
	void)
{
	pthread_attr_t attributes;
	pthread_t thread;
	int started;

	pthread_attr_init(&attributes);
	pthread_attr_setdetachstate(&attributes, PTHREAD_CREATE_DETACHED);
	started = pthread_create(&thread, &attributes, profile_json_writer, NULL) == 0;
	pthread_attr_destroy(&attributes);
	return started;
}

#endif

/*
PROFILE_NET.C

The profiling build's network accounting (profile_net.h): rows of counts
keyed by table, direction, machine, message type, site, reason and key,
one set per one-second interval of a recording, appended to the current
arena's row region (profile_trace.c). An interval's rows are found by a
scratch hash, emptied when the interval closes. The tables they refer to
(sites, fields, machines, connections, objects) only grow, in fixed
arrays. The writer reads only below the counts a part snapshots on the
game thread, and it gets the part through the writer lock, so those
entries are visible to it while the game thread appends above them
(sites too outside a recording: a call site interns its id once, and it
stays valid). Game thread only, but for the simulated loss counter.
*/

#ifdef HALO_PROFILE

#include "halo_port_limits.h"
#include "profile_part.h"

#include <string.h>

/* ---------- constants */

enum
{
	PROFILE_NET_SLOTS = 32768,
	PROFILE_NET_OBJECT_SLOTS = 16384,
	/* the netcode's batch indices: its machines, and the host's one past
	them (HOST_SENDER in network_distributed.c) */
	PROFILE_NET_SENDERS = HALO_PORT_MAXIMUM_NETWORK_MACHINES + 1,
	PROFILE_NET_PENDING = 64,
	/* rows kept for the intervals' own (profile_net_close_interval) */
	PROFILE_NET_ROW_RESERVE = 16,
	PROFILE_NET_SHORT_MESSAGE = 255,
	PROFILE_NET_FIELD_HEADER = 0,
};

/* (the rows an interval may hash, which profile_part.h gives the writer: three
quarters of the slots) */
typedef char profile_net_interval_rows_assert[PROFILE_NET_MAXIMUM_INTERVAL_ROWS == PROFILE_NET_SLOTS / 4 * 3 ? 1 : -1];

/* ---------- structures */

struct profile_net_pending
{
	int type;
	int site;
	unsigned long bytes;
	unsigned long messages;
	unsigned long entries;
};

struct profile_net_pending_field
{
	int field;
	unsigned long size;
};

struct profile_net_peer
{
	unsigned long virtual_address;
	struct profile_net_tunnel_peer previous;
	struct profile_net_loss loss;
	/* in the sample being taken (the others have gone) */
	int seen;
};

/* ---------- globals */

int profile_net_recording;
int profile_net_counting;
int profile_net_connections;

/* for the whole run */
static const struct profile_net_message_name *profile_net_names;
static int profile_net_name_count;
static struct profile_net_layout_entry profile_net_layouts[MAXIMUM_PROFILE_NET_TYPES];
static int profile_net_layout_count;
static signed char profile_net_layout_of_type[256];
static int profile_net_layouts_indexed;
static struct profile_net_site_entry profile_net_sites[MAXIMUM_PROFILE_NET_SITES];
static int profile_net_site_count;
static int profile_net_site_depth;
static int profile_net_current_site = PROFILE_NET_NO_SITE;
static struct profile_net_field_entry profile_net_fields[MAXIMUM_PROFILE_NET_FIELDS] = { { "header" } };
static int profile_net_field_count = 1;
static int (*profile_net_describe)(long key, char *object_type, int object_type_size, char *tag, int tag_size);
static unsigned long (*profile_net_endpoint)(unsigned long virtual_address);
static long (*profile_net_queued_bytes)(void const *connection);
static int profile_net_overlay;

/* a recording's */
static struct profile_net_machine_entry profile_net_machines[MAXIMUM_PROFILE_NET_MACHINES];
static int profile_net_machine_count;
static struct profile_net_connection_entry profile_net_connection_rows[MAXIMUM_PROFILE_NET_CONNECTIONS];
static int profile_net_connection_count;
/* the live connections' ids (closed ones leave, their rows stay) */
static void const *profile_net_live_connections[MAXIMUM_PROFILE_NET_CONNECTIONS];
static struct profile_net_object_entry profile_net_objects[MAXIMUM_PROFILE_NET_OBJECTS];
static int profile_net_object_count;
static int profile_net_object_slots[PROFILE_NET_OBJECT_SLOTS];

/* the current part and interval */
static struct profile_net_row *profile_net_rows;
static unsigned long profile_net_row_count;
static unsigned long profile_net_row_capacity;
static int profile_net_slots[PROFILE_NET_SLOTS];
static long profile_net_slots_used;
static long profile_net_interval_keys;
static long profile_net_interval;
static long profile_net_part_first_interval;
static unsigned long long profile_net_interval_start;
static long profile_net_interval_tick;
static long profile_net_tick;

static struct
{
	struct profile_net_pending items[PROFILE_NET_PENDING];
	int count;
} profile_net_batches[PROFILE_NET_SENDERS];
static struct profile_net_pending_field profile_net_pending_fields[PROFILE_NET_PENDING];
static int profile_net_pending_field_count;
static int profile_net_failure;
static unsigned long profile_net_tick_out;
static unsigned long profile_net_tick_in;
static unsigned long profile_net_foreign;
static unsigned long profile_net_overflowed;
static unsigned long profile_net_dropped_rows;
static unsigned long profile_net_peers_dropped;
static unsigned long profile_net_machines_dropped;
static unsigned long profile_net_connections_overflowed;
static unsigned long profile_net_objects_overflowed;
static unsigned long profile_net_batches_unbooked;
/* the run's tables: not reset with a recording */
static unsigned long profile_net_sites_overflowed;
static unsigned long profile_net_fields_overflowed;
static unsigned long profile_net_layouts_overflowed;
static unsigned int profile_net_simulated;

static struct profile_net_peer profile_net_peers[MAXIMUM_PROFILE_NET_PEERS];
static int profile_net_peer_count;
static struct profile_net_live profile_net_totals;
static long profile_net_ping_sum;
static long profile_net_ping_count;
static unsigned long long profile_net_second_lost;
static unsigned long long profile_net_second_received;

/* ---------- private code */

/* (atomic: a thread that is not the game's reaches the hooks' first test while
the game thread starts and stops a recording) */
#define PROFILE_NET_RECORDING() __atomic_load_n(&profile_net_recording, __ATOMIC_RELAXED)

/* the hooks' own test: the game thread only (others are counted) */
static int profile_net_game_thread(
	void)
{
	if (profile_trace_on_game_thread())
		return 1;
	if (PROFILE_NET_RECORDING())
		__atomic_fetch_add(&profile_net_foreign, 1, __ATOMIC_RELAXED);
	return 0;
}

static unsigned long profile_net_hash(
	int table,
	int direction,
	int type,
	int site,
	int aux,
	long machine,
	long key)
{
	unsigned long hash = 2166136261UL;

	hash = (hash ^ (unsigned long)table) * 16777619UL;
	hash = (hash ^ (unsigned long)direction) * 16777619UL;
	hash = (hash ^ (unsigned long)type) * 16777619UL;
	hash = (hash ^ (unsigned long)site) * 16777619UL;
	hash = (hash ^ (unsigned long)aux) * 16777619UL;
	hash = (hash ^ (unsigned long)machine) * 16777619UL;
	hash = (hash ^ (unsigned long)key) * 16777619UL;
	return hash ^ (hash >> 15);
}

/* the interval's row of the key: found, or added (create), or NULL */
static struct profile_net_row *profile_net_find(
	int table,
	int direction,
	int type,
	int site,
	int aux,
	long machine,
	long key,
	int create)
{
	unsigned long slot;
	struct profile_net_row *row;

	if (!profile_net_rows)
		return NULL;
	slot = profile_net_hash(table, direction, type, site, aux, machine, key) & (PROFILE_NET_SLOTS - 1);
	while (profile_net_slots[slot])
	{
		row = &profile_net_rows[profile_net_slots[slot] - 1];
		if (row->table == table && row->direction == direction && row->type == type && row->site == site &&
			row->aux == aux && row->machine == machine && row->key == key)
		{
			return row;
		}
		slot = (slot + 1) & (PROFILE_NET_SLOTS - 1);
	}
	if (!create)
		return NULL;
	if (profile_net_row_count + PROFILE_NET_ROW_RESERVE >= profile_net_row_capacity ||
		profile_net_slots_used >= PROFILE_NET_MAXIMUM_INTERVAL_ROWS)
	{
		profile_net_dropped_rows++;
		return NULL;
	}
	row = &profile_net_rows[profile_net_row_count++];
	memset(row, 0, sizeof(*row));
	row->interval = (unsigned int)profile_net_interval;
	row->table = (unsigned char)table;
	row->direction = (unsigned char)direction;
	row->type = (unsigned short)type;
	row->site = (unsigned short)site;
	row->aux = (unsigned short)aux;
	row->machine = (int)machine;
	row->key = (int)key;
	profile_net_slots[slot] = (int)profile_net_row_count;
	profile_net_slots_used++;
	return row;
}

static void profile_net_add(
	int table,
	int direction,
	int type,
	int site,
	int aux,
	long machine,
	long key,
	unsigned long value0,
	unsigned long value1,
	unsigned long value2)
{
	struct profile_net_row *row = profile_net_find(table, direction, type, site, aux, machine, key, 1);

	if (!row)
		return;
	row->values[0] += (unsigned int)value0;
	row->values[1] += (unsigned int)value1;
	row->values[2] += (unsigned int)value2;
}

static void profile_net_close_interval(
	unsigned long long now)
{
	struct profile_net_row *row;
	unsigned int lost = __atomic_exchange_n(&profile_net_simulated, 0, __ATOMIC_RELAXED);

	if (!profile_net_rows)
		return;
	if (lost)
		profile_net_add(_profile_net_table_simulated_loss, _profile_net_in, 0, PROFILE_NET_NO_SITE, 0, -1, -1, lost, 0, 0);
	/* (the interval's own row comes from the reserve: never dropped) */
	if (profile_net_row_count < profile_net_row_capacity)
	{
		row = &profile_net_rows[profile_net_row_count++];
		memset(row, 0, sizeof(*row));
		row->interval = (unsigned int)profile_net_interval;
		row->table = _profile_net_table_intervals;
		row->site = PROFILE_NET_NO_SITE;
		row->machine = (int)profile_net_interval_tick;
		row->key = (int)profile_net_interval;
		row->values[0] = (unsigned int)(profile_net_interval_start / 1000000ULL);
		row->values[1] = (unsigned int)((now - profile_net_interval_start) / 1000000ULL);
		row->values[2] = (unsigned int)(profile_net_tick - profile_net_interval_tick);
	}
	memset(profile_net_slots, 0, sizeof(profile_net_slots));
	profile_net_slots_used = 0;
	profile_net_interval_keys = 0;
	profile_net_interval++;
	profile_net_interval_start = now;
	profile_net_interval_tick = profile_net_tick;
}

static long profile_net_layout_index(
	int type)
{
	if (!profile_net_layouts_indexed)
	{
		memset(profile_net_layout_of_type, -1, sizeof(profile_net_layout_of_type));
		profile_net_layouts_indexed = 1;
	}
	return type >= 0 && type < 256 ? profile_net_layout_of_type[type] : -1;
}

/* the first time a recording sees an object key: its type and tag */
static void profile_net_note_object(
	long key)
{
	unsigned long slot = ((unsigned long)key * 2654435761UL) & (PROFILE_NET_OBJECT_SLOTS - 1);
	struct profile_net_object_entry *object;

	while (profile_net_object_slots[slot])
	{
		if (profile_net_objects[profile_net_object_slots[slot] - 1].key == key)
			return;
		slot = (slot + 1) & (PROFILE_NET_OBJECT_SLOTS - 1);
	}
	if (profile_net_object_count >= MAXIMUM_PROFILE_NET_OBJECTS)
	{
		profile_net_objects_overflowed++;
		return;
	}
	object = &profile_net_objects[profile_net_object_count];
	memset(object, 0, sizeof(*object));
	object->key = key;
	if (!profile_net_describe ||
		!profile_net_describe(key, object->object_type, sizeof(object->object_type), object->tag, sizeof(object->tag)))
	{
		strcpy(object->object_type, "unknown");
		object->tag[0] = 0;
	}
	object->object_type[sizeof(object->object_type) - 1] = 0;
	object->tag[sizeof(object->tag) - 1] = 0;
	profile_net_object_slots[slot] = ++profile_net_object_count;
}

static void profile_net_entry(
	long machine,
	int type,
	long key,
	unsigned long bytes,
	int kind)
{
	struct profile_net_row *row;

	/* datum handles carry their salt in bit 31, so real keys are negative; only -1 and -2 are not keys (a data array
	is far under 0xFFFE elements, so no handle is -2) */
	if (key != PROFILE_NET_KEY_NONE && key != PROFILE_NET_KEY_OTHER)
	{
		row = profile_net_find(_profile_net_table_entries, _profile_net_out, type, PROFILE_NET_NO_SITE, 0, machine, key, 0);
		if (!row)
		{
			if (profile_net_interval_keys >= MAXIMUM_PROFILE_NET_INTERVAL_KEYS)
			{
				profile_net_overflowed++;
				key = PROFILE_NET_KEY_OTHER;
			}
			else
			{
				profile_net_interval_keys++;
				if (kind == _profile_net_key_datum)
					profile_net_note_object(key);
			}
		}
	}
	profile_net_add(_profile_net_table_entries, _profile_net_out, type, PROFILE_NET_NO_SITE, 0, machine, key, bytes, 1, 0);
}

/* netcode addresses are host order; table addresses use network order */
static unsigned long profile_net_network_address(
	unsigned long host_address)
{
	return (unsigned long)__builtin_bswap32((unsigned int)host_address);
}

static int profile_net_connection_id(
	void const *connection,
	unsigned long ipv4,
	unsigned short port)
{
	int index;

	for (index = 0; index < profile_net_connection_count; index++)
	{
		if (profile_net_live_connections[index] == connection)
			return index;
	}
	if (profile_net_connection_count >= MAXIMUM_PROFILE_NET_CONNECTIONS)
	{
		profile_net_connections_overflowed++;
		return -1;
	}
	index = profile_net_connection_count;
	profile_net_connection_rows[index].ipv4 = profile_net_network_address(ipv4);
	profile_net_connection_rows[index].port = port;
	profile_net_connection_rows[index].machine = PROFILE_NET_NO_MACHINE;
	profile_net_live_connections[index] = connection;
	profile_net_connection_count++;
	profile_net_connections++;
	return index;
}

/* ---------- public code: start-up */

void profile_net_message_names(
	const struct profile_net_message_name *names,
	int count)
{
	profile_net_names = names;
	profile_net_name_count = count;
}

void profile_net_layout(
	int type,
	const struct profile_net_layout_member *members,
	int count,
	unsigned short entry_size)
{
	if (type < 0 || type >= 256)
		return;
	if (profile_net_layout_count >= MAXIMUM_PROFILE_NET_TYPES)
	{
		profile_net_layouts_overflowed++;
		return;
	}
	profile_net_layout_index(type);
	profile_net_layouts[profile_net_layout_count].type = type;
	profile_net_layouts[profile_net_layout_count].members = members;
	profile_net_layouts[profile_net_layout_count].count = count;
	profile_net_layouts[profile_net_layout_count].entry_size = entry_size;
	profile_net_layout_of_type[type] = (signed char)profile_net_layout_count;
	profile_net_layout_count++;
}

void profile_net_set_object_describe(
	int (*describe)(long key, char *object_type, int object_type_size, char *tag, int tag_size))
{
	profile_net_describe = describe;
}

void profile_net_set_peer_describe(
	unsigned long (*endpoint)(unsigned long virtual_address))
{
	profile_net_endpoint = endpoint;
}

void profile_net_set_queue_reader(
	long (*queued_bytes)(void const *connection))
{
	profile_net_queued_bytes = queued_bytes;
}

/* the tunnel's peers are sampled only while counting: after a gap the first
sample is a baseline again, not the gap's counts, and the table of peers
does not fill with the peers of earlier recordings */
static void profile_net_forget_peers(
	void)
{
	profile_net_peer_count = 0;
	profile_net_second_lost = 0;
	profile_net_second_received = 0;
}

void profile_net_set_overlay(
	int on)
{
	int was_counting = __atomic_load_n(&profile_net_counting, __ATOMIC_RELAXED);

	profile_net_overlay = on;
	__atomic_store_n(&profile_net_counting, profile_net_recording || on, __ATOMIC_RELAXED);
	if (!was_counting && __atomic_load_n(&profile_net_counting, __ATOMIC_RELAXED))
		profile_net_forget_peers();
}

/* ---------- public code: sites and fields */

int profile_net_site(
	const char *function,
	long line)
{
	int index;

	for (index = 0; index < profile_net_site_count; index++)
	{
		if (profile_net_sites[index].line == line && strcmp(profile_net_sites[index].function, function) == 0)
			return index;
	}
	if (profile_net_site_count >= MAXIMUM_PROFILE_NET_SITES)
	{
		profile_net_sites_overflowed++;
		return PROFILE_NET_NO_SITE;
	}
	profile_net_sites[profile_net_site_count].function = function;
	profile_net_sites[profile_net_site_count].line = line;
	return profile_net_site_count++;
}

void profile_net_site_push(
	int site)
{
	if (profile_net_site_depth++ == 0)
		profile_net_current_site = site;
}

void profile_net_site_pop(
	void)
{
	if (profile_net_site_depth > 0 && --profile_net_site_depth == 0)
		profile_net_current_site = PROFILE_NET_NO_SITE;
}

int profile_net_field(
	const char *expression)
{
	char name[sizeof(profile_net_fields[0].name)];
	const char *start = expression;
	const char *arrow;
	int length = 0;
	int index;

	/* "&state->position" is position, "relayed->control_flags[history]"
	control_flags */
	while ((arrow = strstr(start, "->")) != NULL)
		start = arrow + 2;
	if (strrchr(start, '.'))
		start = strrchr(start, '.') + 1;
	while (*start == '&' || *start == ' ' || *start == '(')
		start++;
	while (start[length] && start[length] != '[' && start[length] != ')' && start[length] != ' ' &&
		length < (int)sizeof(name) - 1)
	{
		name[length] = start[length];
		length++;
	}
	name[length] = 0;
	for (index = 0; index < profile_net_field_count; index++)
	{
		if (strcmp(profile_net_fields[index].name, name) == 0)
			return index;
	}
	if (profile_net_field_count >= MAXIMUM_PROFILE_NET_FIELDS)
	{
		profile_net_fields_overflowed++;
		return PROFILE_NET_FIELD_HEADER;
	}
	strcpy(profile_net_fields[profile_net_field_count].name, name);
	return profile_net_field_count++;
}

/* ---------- public code: the funnel */

void profile_net_built(
	int type)
{
	if (!PROFILE_NET_RECORDING() || !profile_net_game_thread())
		return;
	profile_net_add(_profile_net_table_built, _profile_net_out, type, profile_net_current_site, 0, -1, -1, 1, 0, 0);
}

void profile_net_batch_append(
	int sender,
	long machine,
	int type,
	unsigned long bytes,
	int entries)
{
	struct profile_net_pending *pending;
	int index;

	(void)machine;
	if (!PROFILE_NET_RECORDING() || !profile_net_game_thread() || sender < 0 || sender >= PROFILE_NET_SENDERS)
		return;
	for (index = 0; index < profile_net_batches[sender].count; index++)
	{
		pending = &profile_net_batches[sender].items[index];
		if (pending->type == type && pending->site == profile_net_current_site)
			break;
	}
	if (index == profile_net_batches[sender].count)
	{
		/* (past the pending list the last row takes the rest) */
		if (index >= PROFILE_NET_PENDING)
			index = PROFILE_NET_PENDING - 1;
		else
		{
			/* (a row of an earlier batch is still in the slot: start clean) */
			pending = &profile_net_batches[sender].items[index];
			memset(pending, 0, sizeof(*pending));
			pending->type = type;
			pending->site = profile_net_current_site;
			profile_net_batches[sender].count++;
		}
	}
	pending = &profile_net_batches[sender].items[index];
	pending->bytes += bytes;
	pending->messages++;
	pending->entries += (unsigned long)entries;
}

void profile_net_batch_flush(
	int sender,
	long machine,
	unsigned long size,
	int sent)
{
	unsigned long appended = PROFILE_NET_MESSAGE_HEADER_SIZE;
	int index;

	if (!PROFILE_NET_RECORDING() || !profile_net_game_thread() || sender < 0 || sender >= PROFILE_NET_SENDERS)
		return;
	for (index = 0; index < profile_net_batches[sender].count; index++)
		appended += profile_net_batches[sender].items[index].bytes;
	/* a batch with appends the recording did not see books nothing, its
	message rows would not add up to its datagram. Only a batch that began
	before the recording does that, and its flush is in the recording's first
	second (a new game discards the pending rows with the batch): a later one
	is a hook that lost an append, which the header counts for the report */
	if (appended != size)
	{
		if (profile_net_interval > 0)
			profile_net_batches_unbooked++;
		if (sent)
			profile_net_tick_out += size;
		profile_net_batches[sender].count = 0;
		profile_net_failure = _profile_net_failure_none;
		return;
	}
	/* (what was handed to the connection layer, sent or not: the report
	checks that the message rows add up to it) */
	profile_net_add(_profile_net_table_datagrams, _profile_net_out, 0, PROFILE_NET_NO_SITE, 0, machine, -1, size, 1, 0);
	if (sent)
	{
		for (index = 0; index < profile_net_batches[sender].count; index++)
		{
			struct profile_net_pending const *pending = &profile_net_batches[sender].items[index];

			profile_net_add(_profile_net_table_messages, _profile_net_out, pending->type, pending->site, 0, machine, -1,
				pending->bytes, pending->messages, pending->entries);
		}
		profile_net_add(_profile_net_table_messages, _profile_net_out, PROFILE_NET_BATCH_HEADER, PROFILE_NET_NO_SITE, 0,
			machine, -1, PROFILE_NET_MESSAGE_HEADER_SIZE, 1, 0);
		profile_net_tick_out += size;
	}
	else
	{
		profile_net_add(_profile_net_table_send_failures, _profile_net_out, 0, PROFILE_NET_NO_SITE,
			profile_net_failure ? profile_net_failure : _profile_net_failure_write_failed, machine, -1, 1, size, 0);
	}
	profile_net_batches[sender].count = 0;
	profile_net_failure = _profile_net_failure_none;
}

/* (the netcode emptied its batch without sending it) */
void profile_net_batch_discard(
	int sender)
{
	if (sender >= 0 && sender < PROFILE_NET_SENDERS)
		profile_net_batches[sender].count = 0;
}

void profile_net_send_failed(
	int failure)
{
	profile_net_failure = failure;
}

void profile_net_entries(
	long machine,
	int type,
	void const *entries,
	int count,
	unsigned long entry_size)
{
	long layout;
	const struct profile_net_layout_member *key_member = NULL;
	int index;

	if (!PROFILE_NET_RECORDING() || !profile_net_game_thread() || count <= 0)
		return;
	layout = profile_net_layout_index(type);
	if (layout >= 0)
	{
		for (index = 0; index < profile_net_layouts[layout].count; index++)
		{
			if (profile_net_layouts[layout].members[index].key)
			{
				key_member = &profile_net_layouts[layout].members[index];
				break;
			}
		}
	}
	if (!key_member)
	{
		profile_net_add(_profile_net_table_entries, _profile_net_out, type, PROFILE_NET_NO_SITE, 0, machine,
			PROFILE_NET_KEY_NONE, entry_size * (unsigned long)count, (unsigned long)count, 0);
		return;
	}
	for (index = 0; index < count; index++)
	{
		unsigned char const *entry = (unsigned char const *)entries + entry_size * (unsigned long)index;
		long key;

		if (key_member->key == _profile_net_key_datum)
		{
			int datum;

			memcpy(&datum, entry + key_member->offset, sizeof(datum));
			key = datum;
		}
		else
		{
			key = entry[key_member->offset];
		}
		profile_net_entry(machine, type, key, entry_size, key_member->key);
	}
}

void profile_net_field_put(
	int field,
	unsigned long size)
{
	if (profile_net_pending_field_count < PROFILE_NET_PENDING)
	{
		profile_net_pending_fields[profile_net_pending_field_count].field = field;
		profile_net_pending_fields[profile_net_pending_field_count].size = size;
		profile_net_pending_field_count++;
	}
}

void profile_net_packed_entry(
	long machine,
	int type,
	long key,
	unsigned long size,
	int added)
{
	unsigned long counted = 0;
	int index;

	if (!PROFILE_NET_RECORDING() || !profile_net_game_thread())
	{
		profile_net_pending_field_count = 0;
		return;
	}
	if (added)
	{
		profile_net_entry(machine, type, key, size, _profile_net_key_player);
		for (index = 0; index < profile_net_pending_field_count; index++)
		{
			profile_net_add(_profile_net_table_field_bytes, _profile_net_out, type, profile_net_pending_fields[index].field,
				0, -1, -1, profile_net_pending_fields[index].size, 1, 0);
			counted += profile_net_pending_fields[index].size;
		}
		if (size > counted)
		{
			profile_net_add(_profile_net_table_field_bytes, _profile_net_out, type, PROFILE_NET_FIELD_HEADER, 0, -1, -1,
				size - counted, 1, 0);
		}
	}
	profile_net_pending_field_count = 0;
}

void profile_net_reliable(
	long machine,
	void const *message,
	unsigned long size,
	int sent)
{
	unsigned char const *bytes = message;
	int type = size >= PROFILE_NET_MESSAGE_HEADER_SIZE ? bytes[PROFILE_NET_MESSAGE_TYPE_OFFSET] : PROFILE_NET_SHORT_MESSAGE;
	int count = size >= PROFILE_NET_MESSAGE_HEADER_SIZE ? bytes[PROFILE_NET_MESSAGE_TYPE_OFFSET + 1] : 0;

	if (!PROFILE_NET_RECORDING() || !profile_net_game_thread())
		return;
	if (sent)
	{
		long layout = profile_net_layout_index(type);

		profile_net_add(_profile_net_table_messages, _profile_net_out, type, profile_net_current_site, 1, machine, -1,
			size, 1, (unsigned long)count);
		profile_net_tick_out += size;
		if (layout >= 0 && size >= PROFILE_NET_MESSAGE_HEADER_SIZE)
		{
			unsigned long entry_size = profile_net_layouts[layout].entry_size;

			if (entry_size && count &&
				(size - PROFILE_NET_MESSAGE_HEADER_SIZE) % (unsigned long)count == 0 &&
				(size - PROFILE_NET_MESSAGE_HEADER_SIZE) / (unsigned long)count == entry_size)
				profile_net_entries(machine, type, bytes + PROFILE_NET_MESSAGE_HEADER_SIZE, count, entry_size);
		}
	}
	else
	{
		profile_net_add(_profile_net_table_send_failures, _profile_net_out, 1, PROFILE_NET_NO_SITE,
			_profile_net_failure_write_failed, machine, -1, 1, size, 0);
	}
}

void profile_net_machine_address(
	long machine,
	unsigned long ipv4,
	unsigned short port)
{
	int index;

	if (!PROFILE_NET_RECORDING() || !profile_net_game_thread())
		return;
	for (index = 0; index < profile_net_machine_count; index++)
	{
		if (profile_net_machines[index].machine == machine)
			return;
	}
	if (profile_net_machine_count >= MAXIMUM_PROFILE_NET_MACHINES)
	{
		profile_net_machines_dropped++;
		return;
	}
	profile_net_machines[profile_net_machine_count].machine = machine;
	profile_net_machines[profile_net_machine_count].ipv4 = profile_net_network_address(ipv4);
	profile_net_machines[profile_net_machine_count].port = port;
	profile_net_machines[profile_net_machine_count].peer_ipv4 = profile_net_endpoint ?
		profile_net_endpoint(profile_net_network_address(ipv4)) : 0;
	profile_net_machine_count++;
}

void profile_net_received(
	long machine,
	void const *message,
	unsigned long size,
	int kind,
	int drop)
{
	unsigned char const *bytes = message;
	int type = PROFILE_NET_SHORT_MESSAGE;
	int count = 0;

	if (!PROFILE_NET_RECORDING() || !profile_net_game_thread())
		return;
	if (size >= PROFILE_NET_MESSAGE_HEADER_SIZE)
	{
		type = bytes[PROFILE_NET_MESSAGE_TYPE_OFFSET];
		count = bytes[PROFILE_NET_MESSAGE_TYPE_OFFSET + 1];
	}
	if (kind != _profile_net_received_inner)
		profile_net_tick_in += size;
	/* a batch's datagram is its header here; the messages in it come one by
	one, inner */
	if (kind == _profile_net_received_batch)
	{
		profile_net_add(_profile_net_table_received, _profile_net_in, PROFILE_NET_BATCH_HEADER, PROFILE_NET_NO_SITE, 0,
			machine, -1, PROFILE_NET_MESSAGE_HEADER_SIZE, 1, 0);
		if (drop)
		{
			profile_net_add(_profile_net_table_received, _profile_net_in, PROFILE_NET_BATCH_HEADER, PROFILE_NET_NO_SITE,
				drop, machine, -1, size, 1, 0);
		}
		return;
	}
	profile_net_add(_profile_net_table_received, _profile_net_in, type, PROFILE_NET_NO_SITE, 0, machine, -1, size, 1,
		(unsigned long)count);
	if (drop)
	{
		profile_net_add(_profile_net_table_received, _profile_net_in, type, PROFILE_NET_NO_SITE, drop, machine, -1, size,
			1, (unsigned long)count);
	}
}

/* ---------- public code: the connection layer */

void profile_net_traffic(
	void const *connection,
	int direction,
	int channel,
	unsigned long bytes,
	unsigned long ipv4,
	unsigned short port)
{
	int id;

	if (!__atomic_load_n(&profile_net_counting, __ATOMIC_RELAXED) || !profile_net_game_thread() || (direction != _profile_net_out && direction != _profile_net_in))
		return;
	profile_net_totals.game_bytes[direction] += bytes;
	profile_net_totals.game_packets[direction]++;
	if (!profile_net_recording)
		return;
	id = profile_net_connection_id(connection, ipv4, port);
	profile_net_add(_profile_net_table_traffic, direction, 0, PROFILE_NET_NO_SITE, channel, -1, id, bytes, 1, 0);
}

void profile_net_connection_closed(
	void const *connection)
{
	int index;

	for (index = 0; index < profile_net_connection_count; index++)
	{
		if (profile_net_live_connections[index] == connection)
		{
			profile_net_live_connections[index] = NULL;
			profile_net_connections--;
		}
	}
}

void profile_net_connection_machine(
	void const *connection,
	long machine)
{
	int id;

	if (!PROFILE_NET_RECORDING() || !profile_net_game_thread())
		return;
	id = profile_net_connection_id(connection, 0, 0);
	if (id >= 0 && profile_net_connection_rows[id].machine != (int)machine)
		__atomic_store_n(&profile_net_connection_rows[id].machine, (int)machine, __ATOMIC_RELAXED);
}

void profile_net_simulated_loss(
	void)
{
	/* (the flag is read here by a thread that is not the game's, which sets
	it) */
	if (__atomic_load_n(&profile_net_recording, __ATOMIC_RELAXED))
		__atomic_fetch_add(&profile_net_simulated, 1, __ATOMIC_RELAXED);
}

/* ---------- public code: once a second */

unsigned long profile_net_loss_step(
	struct profile_net_loss *loss,
	unsigned long long highest,
	unsigned long long received)
{
	long long cumulative;
	long long lost;

	if (!loss->valid)
	{
		loss->valid = 1;
		loss->highest = highest;
		loss->received = received;
		loss->reported = 0;
		return 0;
	}
	cumulative = (long long)(highest - loss->highest) - (long long)(received - loss->received);
	if (cumulative <= loss->reported)
		return 0;
	lost = cumulative - loss->reported;
	loss->reported = cumulative;
	return (unsigned long)lost;
}

void profile_net_tunnel(
	const struct profile_net_tunnel_peer *peers,
	int count)
{
	long round_trip_sum = 0;
	long round_trips = 0;
	int index;
	int kept;

	if (!__atomic_load_n(&profile_net_counting, __ATOMIC_RELAXED) || !profile_net_game_thread())
		return;
	profile_net_second_lost = 0;
	profile_net_second_received = 0;
	/* (the second's pings come after this in the same pass: the overlay reads
	the last second's mean, whether it was on or not) */
	profile_net_ping_sum = 0;
	profile_net_ping_count = 0;
	for (index = 0; index < count; index++)
	{
		const struct profile_net_tunnel_peer *peer = &peers[index];
		struct profile_net_peer *state = NULL;
		struct profile_net_row *row;
		unsigned long long bytes_out, bytes_in, packets_out, packets_in, payload, output;
		unsigned long lost;
		int search;

		for (search = 0; search < profile_net_peer_count; search++)
		{
			if (profile_net_peers[search].virtual_address == peer->virtual_address)
			{
				state = &profile_net_peers[search];
				state->seen = 1;
				break;
			}
		}
		if (!state)
		{
			if (profile_net_peer_count >= MAXIMUM_PROFILE_NET_PEERS)
			{
				profile_net_peers_dropped++;
				continue;
			}
			state = &profile_net_peers[profile_net_peer_count++];
			memset(state, 0, sizeof(*state));
			state->virtual_address = peer->virtual_address;
			state->seen = 1;
			state->previous = *peer;
			profile_net_loss_step(&state->loss, peer->highest_in, peer->packets_in);
			continue;
		}
		/* (a peer made again since: its counts start over, in any of them) */
		if (peer->packets_in < state->previous.packets_in || peer->packets_out < state->previous.packets_out ||
			peer->bytes_in < state->previous.bytes_in || peer->bytes_out < state->previous.bytes_out ||
			peer->kcp_payload < state->previous.kcp_payload || peer->kcp_output < state->previous.kcp_output ||
			peer->highest_in < state->previous.highest_in)
		{
			state->previous = *peer;
			memset(&state->loss, 0, sizeof(state->loss));
			profile_net_loss_step(&state->loss, peer->highest_in, peer->packets_in);
			continue;
		}
		bytes_out = peer->bytes_out - state->previous.bytes_out;
		bytes_in = peer->bytes_in - state->previous.bytes_in;
		packets_out = peer->packets_out - state->previous.packets_out;
		packets_in = peer->packets_in - state->previous.packets_in;
		payload = peer->kcp_payload - state->previous.kcp_payload;
		output = peer->kcp_output - state->previous.kcp_output;
		lost = profile_net_loss_step(&state->loss, peer->highest_in, peer->packets_in);
		state->previous = *peer;
		profile_net_totals.wire_bytes[_profile_net_out] += bytes_out;
		profile_net_totals.wire_bytes[_profile_net_in] += bytes_in;
		profile_net_totals.wire_packets[_profile_net_out] += packets_out;
		profile_net_totals.wire_packets[_profile_net_in] += packets_in;
		round_trip_sum += (long)peer->round_trip;
		round_trips++;
		profile_net_second_lost += lost;
		profile_net_second_received += packets_in;
		if (!profile_net_recording)
			continue;
		profile_net_add(_profile_net_table_tunnel_bytes, _profile_net_out, 0, PROFILE_NET_NO_SITE, 0, -1,
			(long)(int)peer->virtual_address, (unsigned long)bytes_out, (unsigned long)bytes_in, 0);
		/* the latest sample, not a sum: two samples may land in one interval */
		row = profile_net_find(_profile_net_table_tunnel_bytes, _profile_net_out, 0, PROFILE_NET_NO_SITE, 0, -1,
			(long)(int)peer->virtual_address, 0);
		if (row)
			row->values[2] = peer->round_trip;
		profile_net_add(_profile_net_table_tunnel_packets, _profile_net_out, 0, PROFILE_NET_NO_SITE, 0, -1,
			(long)(int)peer->virtual_address, (unsigned long)packets_out, (unsigned long)packets_in, lost);
		profile_net_add(_profile_net_table_tunnel_kcp, _profile_net_out, 0, PROFILE_NET_NO_SITE, 0, -1,
			(long)(int)peer->virtual_address, (unsigned long)payload, (unsigned long)output, 0);
	}
	/* (a peer that is not in this sample is gone: the table holds the peers
	there are, a peer back is new, and the cap is on those together) */
	for (index = 0, kept = 0; index < profile_net_peer_count; index++)
	{
		if (profile_net_peers[index].seen)
		{
			profile_net_peers[kept] = profile_net_peers[index];
			profile_net_peers[kept++].seen = 0;
		}
	}
	profile_net_peer_count = kept;
	if (round_trips)
		profile_net_totals.wire_round_trip_ms = (round_trip_sum + round_trips / 2) / round_trips;
	profile_net_totals.loss_percent = profile_net_second_received + profile_net_second_lost ?
		100.0 * (double)profile_net_second_lost / (double)(profile_net_second_received + profile_net_second_lost) : 0.0;
}

void profile_net_ping(
	long machine,
	long milliseconds)
{
	struct profile_net_row *row;

	if (!__atomic_load_n(&profile_net_counting, __ATOMIC_RELAXED) || !profile_net_game_thread() || milliseconds < 0)
		return;
	profile_net_ping_sum += milliseconds;
	profile_net_ping_count++;
	if (!profile_net_recording)
		return;
	row = profile_net_find(_profile_net_table_pings, _profile_net_in, 0, PROFILE_NET_NO_SITE, 0, machine, -1, 1);
	if (row)
	{
		row->values[0] = (unsigned int)milliseconds;
		row->values[1]++;
	}
}

void profile_net_sample_queues(
	void)
{
	int index;

	if (!PROFILE_NET_RECORDING() || !profile_net_queued_bytes || !profile_net_game_thread())
		return;
	for (index = 0; index < profile_net_connection_count; index++)
	{
		struct profile_net_row *row;
		long bytes;

		if (!profile_net_live_connections[index])
			continue;
		bytes = profile_net_queued_bytes(profile_net_live_connections[index]);
		row = profile_net_find(_profile_net_table_queues, _profile_net_out, 0, PROFILE_NET_NO_SITE, 0, -1, index, 1);
		if (row && bytes > 0 && (unsigned long)bytes > row->values[0])
			row->values[0] = (unsigned int)bytes;
	}
}

void profile_net_live(
	struct profile_net_live *live)
{
	if (profile_net_ping_count)
		profile_net_totals.ping_ms = profile_net_ping_sum / profile_net_ping_count;
	profile_net_ping_sum = 0;
	profile_net_ping_count = 0;
	*live = profile_net_totals;
}

/* ---------- public code: the recording's side (profile_trace.c) */

void profile_net_recording_begin(
	void)
{
	int sender;

	if (!__atomic_load_n(&profile_net_counting, __ATOMIC_RELAXED))
		profile_net_forget_peers();
	profile_net_machine_count = 0;
	profile_net_connection_count = 0;
	profile_net_connections = 0;
	memset(profile_net_live_connections, 0, sizeof(profile_net_live_connections));
	profile_net_object_count = 0;
	memset(profile_net_object_slots, 0, sizeof(profile_net_object_slots));
	for (sender = 0; sender < PROFILE_NET_SENDERS; sender++)
		profile_net_batches[sender].count = 0;
	profile_net_pending_field_count = 0;
	profile_net_failure = _profile_net_failure_none;
	profile_net_tick_out = profile_net_tick_in = 0;
	__atomic_store_n(&profile_net_foreign, 0, __ATOMIC_RELAXED);
	profile_net_overflowed = profile_net_dropped_rows = 0;
	profile_net_peers_dropped = profile_net_machines_dropped = 0;
	profile_net_connections_overflowed = profile_net_objects_overflowed = 0;
	profile_net_batches_unbooked = 0;
	__atomic_store_n(&profile_net_simulated, 0, __ATOMIC_RELAXED);
	profile_net_interval = 0;
	profile_net_tick = 0;
	__atomic_store_n(&profile_net_recording, 1, __ATOMIC_RELAXED);
	__atomic_store_n(&profile_net_counting, 1, __ATOMIC_RELAXED);
}

void profile_net_recording_end(
	void)
{
	__atomic_store_n(&profile_net_recording, 0, __ATOMIC_RELAXED);
	__atomic_store_n(&profile_net_counting, profile_net_overlay, __ATOMIC_RELAXED);
	profile_net_rows = NULL;
}

void profile_net_part_begin(
	struct profile_net_row *rows,
	unsigned long capacity,
	unsigned long long now,
	long tick)
{
	profile_net_rows = rows;
	profile_net_row_count = 0;
	profile_net_row_capacity = capacity;
	memset(profile_net_slots, 0, sizeof(profile_net_slots));
	profile_net_slots_used = 0;
	profile_net_interval_keys = 0;
	profile_net_part_first_interval = profile_net_interval;
	profile_net_interval_start = now;
	profile_net_tick = tick;
	profile_net_interval_tick = tick;
}

void profile_net_part_end(
	unsigned long long now,
	unsigned long *row_count,
	long *first_interval,
	long *intervals,
	struct profile_net_snapshot *snapshot)
{
	profile_net_close_interval(now);
	*row_count = profile_net_row_count;
	*first_interval = profile_net_part_first_interval;
	*intervals = profile_net_interval - profile_net_part_first_interval;
	snapshot->types = profile_net_name_count;
	snapshot->sites = profile_net_site_count;
	snapshot->fields = profile_net_field_count;
	snapshot->layouts = profile_net_layout_count;
	snapshot->machines = profile_net_machine_count;
	snapshot->connections = profile_net_connection_count;
	snapshot->objects = profile_net_object_count;
	profile_net_rows = NULL;
	profile_net_row_count = 0;
	profile_net_row_capacity = 0;
}

int profile_net_part_full(
	void)
{
	return profile_net_rows &&
		profile_net_row_count + (profile_net_row_capacity >> 4) + PROFILE_NET_ROW_RESERVE >= profile_net_row_capacity;
}

void profile_net_frame(
	unsigned long long now,
	long tick)
{
	profile_net_tick = tick;
	if (profile_net_rows && now - profile_net_interval_start >= 1000000000ULL)
		profile_net_close_interval(now);
}

void profile_net_take_tick_bytes(
	unsigned long *out,
	unsigned long *in)
{
	*out = profile_net_tick_out;
	*in = profile_net_tick_in;
	profile_net_tick_out = 0;
	profile_net_tick_in = 0;
}

void profile_net_counters(
	struct profile_net_counts *counts)
{
	counts->foreign_events = __atomic_exchange_n(&profile_net_foreign, 0, __ATOMIC_RELAXED);
	counts->entry_keys_overflowed = profile_net_overflowed;
	counts->dropped_rows = profile_net_dropped_rows;
	counts->peers_dropped = profile_net_peers_dropped;
	counts->machines_dropped = profile_net_machines_dropped;
	counts->connections_overflowed = profile_net_connections_overflowed;
	counts->objects_overflowed = profile_net_objects_overflowed;
	counts->batches_unbooked = profile_net_batches_unbooked;
	counts->sites_overflowed = profile_net_sites_overflowed;
	counts->fields_overflowed = profile_net_fields_overflowed;
	counts->layouts_overflowed = profile_net_layouts_overflowed;
	profile_net_overflowed = profile_net_dropped_rows = 0;
	profile_net_peers_dropped = profile_net_machines_dropped = 0;
	profile_net_connections_overflowed = profile_net_objects_overflowed = 0;
	profile_net_batches_unbooked = 0;
}

long profile_net_type_name_count(
	void)
{
	return profile_net_name_count;
}

const struct profile_net_message_name *profile_net_type_name_at(
	long index)
{
	return index >= 0 && index < profile_net_name_count ? &profile_net_names[index] : NULL;
}

const struct profile_net_message_name *profile_net_type_name(
	int type)
{
	int index;

	for (index = 0; index < profile_net_name_count; index++)
	{
		if (profile_net_names[index].type == type)
			return &profile_net_names[index];
	}
	return NULL;
}

const struct profile_net_site_entry *profile_net_site_entry(
	long site)
{
	return site >= 0 && site < MAXIMUM_PROFILE_NET_SITES ? &profile_net_sites[site] : NULL;
}

const struct profile_net_field_entry *profile_net_field_entry(
	long field)
{
	return field >= 0 && field < MAXIMUM_PROFILE_NET_FIELDS ? &profile_net_fields[field] : NULL;
}

const struct profile_net_layout_entry *profile_net_layout_entry(
	long index)
{
	return index >= 0 && index < MAXIMUM_PROFILE_NET_TYPES ? &profile_net_layouts[index] : NULL;
}

const struct profile_net_machine_entry *profile_net_machine_entry(
	long index)
{
	return index >= 0 && index < MAXIMUM_PROFILE_NET_MACHINES ? &profile_net_machines[index] : NULL;
}

const struct profile_net_connection_entry *profile_net_connection_entry(
	long index)
{
	return index >= 0 && index < MAXIMUM_PROFILE_NET_CONNECTIONS ? &profile_net_connection_rows[index] : NULL;
}

const struct profile_net_object_entry *profile_net_object_entry(
	long index)
{
	return index >= 0 && index < MAXIMUM_PROFILE_NET_OBJECTS ? &profile_net_objects[index] : NULL;
}

#endif

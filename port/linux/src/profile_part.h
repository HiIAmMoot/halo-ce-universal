/*
PROFILE_PART.H

Define frozen recording parts shared by the recorder and writer.
*/

#ifndef __PROFILE_PART_H
#define __PROFILE_PART_H

#ifdef HALO_PROFILE

#include "profile_trace.h"
#include "profile_net.h"

#include <stdio.h>

/* ---------- constants */

/* records that are not scopes, by their depth (a scope's is under
MAXIMUM_PROFILE_TRACE_DEPTH): an aggregate name's time and count over a
frame (start: the frame's), and a tick's network bytes */
enum
{
	PROFILE_TRACE_DEPTH_AGGREGATE_TIME = 255,
	PROFILE_TRACE_DEPTH_AGGREGATE_COUNT = 254,
	PROFILE_TRACE_DEPTH_NET_OUT = 253,
	PROFILE_TRACE_DEPTH_NET_IN = 252,
};

enum profile_net_table
{
	_profile_net_table_messages = 0,
	_profile_net_table_received,
	_profile_net_table_built,
	_profile_net_table_entries,
	_profile_net_table_field_bytes,
	_profile_net_table_send_failures,
	_profile_net_table_datagrams,
	_profile_net_table_intervals,
	NUMBER_OF_PROFILE_NET_TABLES,
};

/* the rows one interval can have (profile_net.c: three quarters of its slots);
the writer's indexes are sized from it */
enum
{
	PROFILE_NET_MAXIMUM_INTERVAL_ROWS = 24576,
};

/* a row's site (or field) when it has none */
#define PROFILE_NET_NO_SITE 0xFFFF

/* ---------- structures */

/* a scope that ended: 16 bytes */
struct profile_trace_record
{
	/* nanoseconds since the recording started */
	unsigned long long start;
	/* nanoseconds, saturating (or the counter's value) */
	unsigned int duration;
	unsigned short name;
	unsigned char depth;
	unsigned char track;
};

/* one interval's count of a key: 32 bytes. The columns a table uses are
profile_json.c's; an interval row is its index (key), the tick at its start
(machine), its start and length in milliseconds and the ticks run
(values). */
struct profile_net_row
{
	unsigned int interval;
	unsigned char table;
	unsigned char direction;
	unsigned short type;
	unsigned short site;
	unsigned short aux;
	int machine;
	int key;
	unsigned int values[3];
};

/* the metadata a part's rows refer to, as far as it went at the cut (the
tables only grow during a recording, so a part reads below its counts
while the game thread appends above them) */
struct profile_net_snapshot
{
	long types;
	long sites;
	long fields;
	long layouts;
	long objects;
};

struct profile_net_site_entry
{
	const char *function;
	long line;
};

struct profile_net_field_entry
{
	char name[32];
};

struct profile_net_layout_entry
{
	int type;
	const struct profile_net_layout_member *members;
	int count;
	unsigned short entry_size;
};

struct profile_net_object_entry
{
	long key;
	char object_type[16];
	char tag[96];
};

/* what a part could not hold, read at its cut. All but the last three are
counted since the last cut; the sites, fields and layouts tables belong to the
run (a call site interns once, and may before a recording), so their counts
only grow and every part says them */
struct profile_net_counts
{
	unsigned long foreign_events;
	unsigned long entry_keys_overflowed;
	unsigned long dropped_rows;
	unsigned long objects_overflowed;
	unsigned long batches_unbooked;
	unsigned long sites_overflowed;
	unsigned long fields_overflowed;
	unsigned long layouts_overflowed;
};

struct profile_part
{
	char name[64];
	char folder[260];
	long number;
	int last;
	int stop_reason;
	int arena;
	struct profile_trace_record *records[NUMBER_OF_PROFILE_TRACKS];
	unsigned long record_counts[NUMBER_OF_PROFILE_TRACKS];
	struct profile_net_row *rows;
	unsigned long row_count;
	unsigned long long start_ns;
	unsigned long long end_ns;
	long first_frame;
	long frames;
	long first_tick;
	long ticks;
	long first_interval;
	long intervals;
	long name_count;
	struct profile_net_snapshot net;
	struct profile_trace_session session;
	char start_utc[32];
	double clock_read_ns;
	unsigned long memory_used;
	unsigned long memory_limit;
	double writer_wait_ms;
	unsigned long foreign_scopes;
	unsigned long deep_scopes;
	unsigned long unbalanced_scopes;
	unsigned long dropped_scopes;
	struct profile_net_counts counts;
};

typedef char profile_trace_record_size_assert[sizeof(struct profile_trace_record) == 16 ? 1 : -1];
typedef char profile_net_row_size_assert[sizeof(struct profile_net_row) == 32 ? 1 : -1];

/* ---------- prototypes/PROFILE_TRACE.C */

const char *profile_trace_name_text(int name);
int profile_trace_name_is_aggregate(int name);
/* the writer: the next part (waits for one), and a part written (its
arena back; after the last, both arenas freed and the recording over) */
struct profile_part *profile_trace_writer_next(void);
/* a line in the log (profile_trace_set_log) */
void profile_trace_log(const char *format, ...);
void profile_trace_writer_done(struct profile_part *part);

/* ---------- prototypes/PROFILE_NET.C (the recording's side) */

void profile_net_recording_begin(void);
void profile_net_recording_end(void);
void profile_net_part_begin(struct profile_net_row *rows, unsigned long capacity, unsigned long long now, long tick);
void profile_net_part_end(unsigned long long now, unsigned long *row_count, long *first_interval, long *intervals,
	struct profile_net_snapshot *snapshot);
int profile_net_part_full(void);
void profile_net_frame(unsigned long long now, long tick);
void profile_net_take_tick_bytes(unsigned long *out, unsigned long *in);
void profile_net_counters(struct profile_net_counts *counts);
const struct profile_net_message_name *profile_net_type_name(int type);
const struct profile_net_site_entry *profile_net_site_entry(long site);
const struct profile_net_field_entry *profile_net_field_entry(long field);
const struct profile_net_layout_entry *profile_net_layout_entry(long index);
const struct profile_net_object_entry *profile_net_object_entry(long index);
long profile_net_type_name_count(void);
const struct profile_net_message_name *profile_net_type_name_at(long index);

/* ---------- prototypes/PROFILE_JSON.C */

/* a part as a Chrome trace with the "halo" and "cpu_summary" keys; 0 when
the file could not be written */
int profile_json_write(struct profile_part *part, FILE *file);
/* Part names keep profile_<stamp>_<role>. */
void profile_json_choose_name(const char *folder, const char *stamp, const char *role, char *name, int size);
/* the writer thread of a recording: 0 when it cannot start */
int profile_json_writer_start(void);
/* a part's file: <folder>/<name>.part<n>.json, written as .tmp and renamed */
int profile_json_write_part(struct profile_part *part);

#endif

#endif

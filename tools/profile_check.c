/*
PROFILE_CHECK.C

Exercise the profiling recorder, writer and accounting with standalone fakes.
*/

#include "profile_part.h"
#include "profile_overlay_lines.h"
#include "profile_console_gametype.h"

#include <pthread.h>
#include <dirent.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

void profile_json_test_fail_join_after_start(void);

/* ---------- fakes */

static int failures;
static int session_sample_calls;
static unsigned long long fake_now = 1000000000ULL;
static pthread_mutex_t fake_track_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t fake_allocator_lock = PTHREAD_MUTEX_INITIALIZER;
static long allocations_outstanding;
static long allocations_made;
static int allocations_refused;
/* allocations that succeed before the rest are refused (-1: no such limit) */
static int allocations_allowed = -1;
static int fake_ready;
static const char *fake_role = "host";
static long fake_players = 2;
static long fake_own_machine = -1;
static const char *fake_map = "levels\\a30\\a30";
static const char *fake_map_name = "a30";
static const char *fake_folder = ".";
static char last_notice[128];
static char last_log[600];
static char last_join_log[600];
static char last_delete_log[600];
static int name_aggregate_child;

#ifdef PROFILE_CHECK_KEEP_PARTS
int profile_check_remove(const char *path)
{
	if (strstr(path, ".part") && strstr(path, ".json"))
		return -1;
	return unlink(path);
}
#endif

static void check(int good, const char *what)
{
	if (!good)
	{
		failures++;
		printf("FAIL: %s\n", what);
	}
}

static void gametype_checks(void)
{
	static const char *const expected[] = { "none", "ctf", "slayer", "oddball", "king", "race" };
	long engine;
	for (engine = 0; engine <= 5; engine++)
		check(strcmp(profile_console_engine_gametype(1, engine), expected[engine]) == 0,
			"engine index maps to its engine gametype");
	check(strcmp(profile_console_engine_gametype(0, 2), "campaign") == 0,
		"campaign does not reuse a stale slayer variant");
	check(strcmp(profile_console_engine_gametype(1, 6), "none") == 0 &&
		strcmp(profile_console_engine_gametype(1, 7), "none") == 0, "an engine past race is none");
	check(strcmp(profile_console_scenario_map("levels\\b30\\b30", "ui"), "levels\\b30\\b30") == 0,
		"the pending scenario takes precedence over the previous map");
}

static unsigned long long fake_clock(void)
{
	return __atomic_load_n(&fake_now, __ATOMIC_RELAXED);
}

static void advance(unsigned long long nanoseconds)
{
	__atomic_fetch_add(&fake_now, nanoseconds, __ATOMIC_RELAXED);
}

static void fake_lock(void)
{
	pthread_mutex_lock(&fake_track_lock);
}

static void fake_unlock(void)
{
	pthread_mutex_unlock(&fake_track_lock);
}

static void *fake_allocate(unsigned long size)
{
	void *block;

	pthread_mutex_lock(&fake_allocator_lock);
	block = allocations_refused || allocations_allowed == 0 ? NULL : malloc(size);
	if (allocations_allowed > 0)
		allocations_allowed--;
	if (block)
	{
		allocations_outstanding++;
		allocations_made++;
	}
	pthread_mutex_unlock(&fake_allocator_lock);
	return block;
}

static void fake_release(void *block)
{
	if (!block)
		return;
	pthread_mutex_lock(&fake_allocator_lock);
	allocations_outstanding--;
	pthread_mutex_unlock(&fake_allocator_lock);
	free(block);
}

static long outstanding(void)
{
	long count;

	pthread_mutex_lock(&fake_allocator_lock);
	count = allocations_outstanding;
	pthread_mutex_unlock(&fake_allocator_lock);
	return count;
}

static void quiet_log(const char *text)
{
	if (strstr(text, "cannot write"))
		snprintf(last_log, sizeof(last_log), "%s", text);
	if (strstr(text, "cannot join"))
		snprintf(last_join_log, sizeof(last_join_log), "%s", text);
	if (strstr(text, "cannot delete joined part"))
		snprintf(last_delete_log, sizeof(last_delete_log), "%s", text);
}

static void fake_notify(const char *text)
{
	snprintf(last_notice, sizeof(last_notice), "%s", text);
}

static int joined_file_exists(const char *folder, const char *recording)
{
	DIR *directory = opendir(folder);
	struct dirent *entry;
	char prefix[128];
	int found = 0;
	const char *role_end = strstr(recording, "_host");
	if (!role_end)
		role_end = strstr(recording, "_client");
	if (!role_end)
		role_end = strstr(recording, "_local");

	if (!directory)
		return 0;
	if (role_end)
		role_end += role_end[1] == 'h' ? 5 : role_end[1] == 'c' ? 7 : 6;
	snprintf(prefix, sizeof(prefix), "%.*s_", (int)(role_end ? role_end - recording : strlen(recording)), recording);
	while ((entry = readdir(directory)) != NULL)
	{
		if (strncmp(entry->d_name, prefix, strlen(prefix)) == 0 && strstr(entry->d_name, ".json") &&
			!strstr(entry->d_name, ".part"))
			found = 1;
	}
	closedir(directory);
	return found;
}

static int fake_ready_test(void)
{
	return fake_ready;
}

static void fake_session(struct profile_trace_session *session)
{
	memset(session, 0, sizeof(*session));
	snprintf(session->folder, sizeof(session->folder), "%s", fake_folder);
	strcpy(session->build, "check");
	strncpy(session->role, fake_role, sizeof(session->role) - 1);
	session->own_machine = fake_own_machine;
	strncpy(session->map, fake_map, sizeof(session->map) - 1);
	strncpy(session->map_name, fake_map_name, sizeof(session->map_name) - 1);
	strcpy(session->gametype, "campaign");
	session->players = fake_players;
	session->players_most = fake_players;
}

static void fake_session_sample(struct profile_trace_session *session)
{
	session_sample_calls++;
	fake_session(session);
}

static int describe_object(long key, char *object_type, int object_type_size, char *tag, int tag_size)
{
	if (key == 99)
		return 0;
	snprintf(object_type, (size_t)object_type_size, "biped");
	snprintf(tag, (size_t)tag_size, "characters\\elite\\elite %ld", key);
	return 1;
}

/* ---------- the fake writer: what each part held */

enum
{
	MAXIMUM_PARTS = 64,
};

static struct
{
	pthread_mutex_t lock;
	int parts;
	int finished;
	unsigned long counts[MAXIMUM_PROFILE_TRACE_NAMES];
	unsigned long counter_records;
	unsigned long long aggregate_time[MAXIMUM_PROFILE_TRACE_NAMES];
	unsigned long aggregate_count[MAXIMUM_PROFILE_TRACE_NAMES];
	long first_frame[MAXIMUM_PARTS];
	long frames[MAXIMUM_PARTS];
	long first_interval[MAXIMUM_PARTS];
	long intervals[MAXIMUM_PARTS];
	unsigned long long start_ns[MAXIMUM_PARTS];
	unsigned long long end_ns[MAXIMUM_PARTS];
	int last[MAXIMUM_PARTS];
	int stop_reason[MAXIMUM_PARTS];
	char map_name[MAXIMUM_PARTS][64];
	double writer_wait_ms;
	unsigned long unbalanced;
	unsigned long deep;
	unsigned long foreign;
	unsigned long dropped;
	unsigned long foreign_events;
	int records_in_span;
	int aggregate_child_depth;
	int delay_milliseconds;
} written = { .lock = PTHREAD_MUTEX_INITIALIZER };

static void *fake_writer(void *unused)
{
	(void)unused;
	for (;;)
	{
		struct profile_part *part = profile_trace_writer_next();
		int last = part->last;
		unsigned long index;
		int track;

		/* (a slow first part: the game thread's wait for it is on the fake
		clock too) */
		if (written.delay_milliseconds && part->number == 1)
		{
			usleep((useconds_t)written.delay_milliseconds * 1000);
			advance((unsigned long long)written.delay_milliseconds * 1000000ULL);
		}
		pthread_mutex_lock(&written.lock);
		if (written.parts < MAXIMUM_PARTS)
		{
			written.first_frame[written.parts] = part->first_frame;
			written.frames[written.parts] = part->frames;
			written.first_interval[written.parts] = part->first_interval;
			written.intervals[written.parts] = part->intervals;
			written.start_ns[written.parts] = part->start_ns;
			written.end_ns[written.parts] = part->end_ns;
			written.last[written.parts] = part->last;
			written.stop_reason[written.parts] = part->stop_reason;
			strcpy(written.map_name[written.parts], part->session.map_name);
		}
		written.parts++;
		written.writer_wait_ms = part->writer_wait_ms;
		written.unbalanced += part->unbalanced_scopes;
		written.deep += part->deep_scopes;
		written.foreign += part->foreign_scopes;
		written.dropped += part->dropped_scopes;
		written.foreign_events += part->counts.foreign_events;
		for (track = 0; track < NUMBER_OF_PROFILE_TRACKS; track++)
		{
			for (index = 0; index < part->record_counts[track]; index++)
			{
				const struct profile_trace_record *record = &part->records[track][index];

				if (record->depth < MAXIMUM_PROFILE_TRACE_DEPTH)
				{
					written.counts[record->name]++;
					if (record->name == name_aggregate_child)
						written.aggregate_child_depth = record->depth;
					/* (a scope that began before the part, in the p2p pass a
					cut came in, is in the part it ended in) */
					if (record->start + record->duration > part->end_ns)
						written.records_in_span = 0;
				}
				else
				{
					written.counter_records++;
					if (record->depth == PROFILE_TRACE_DEPTH_AGGREGATE_TIME)
						written.aggregate_time[record->name] += record->duration;
					else if (record->depth == PROFILE_TRACE_DEPTH_AGGREGATE_COUNT)
						written.aggregate_count[record->name] += record->duration;
				}
			}
		}
		pthread_mutex_unlock(&written.lock);
		if (last)
		{
			pthread_mutex_lock(&written.lock);
			written.finished++;
			pthread_mutex_unlock(&written.lock);
		}
		/* Publish the fake writer's last observation before done makes the
		recorder idle; finish() may read the observations as soon as it does. */
		profile_trace_writer_done(part);
		if (last)
			break;
	}
	return NULL;
}

static int fake_writer_start(void)
{
	pthread_t thread;

	return pthread_create(&thread, NULL, fake_writer, NULL) == 0 && pthread_detach(thread) == 0;
}

static void forget_written(void)
{
	pthread_mutex_lock(&written.lock);
	written.parts = 0;
	written.finished = 0;
	memset(written.counts, 0, sizeof(written.counts));
	written.counter_records = 0;
	written.unbalanced = written.deep = written.foreign = written.dropped = 0;
	written.foreign_events = 0;
	written.records_in_span = 1;
	pthread_mutex_unlock(&written.lock);
}

/* whether the writer has finished a recording, read through its lock */
static int writer_finished(void)
{
	int finished;

	pthread_mutex_lock(&written.lock);
	finished = written.finished;
	pthread_mutex_unlock(&written.lock);
	return finished;
}

/* frame boundaries until the recording is written and the state idle */
static void finish(void)
{
	struct profile_trace_status status;
	int frames;

	for (frames = 0; frames < 100000; frames++)
	{
		profile_trace_frame_boundary();
		profile_trace_status(&status);
		if (status.state == _profile_trace_idle || status.state == _profile_trace_armed)
		{
			/* (what the writer noted, seen through its lock) */
			pthread_mutex_lock(&written.lock);
			pthread_mutex_unlock(&written.lock);
			return;
		}
		usleep(100);
	}
	check(0, "the recording finishes");
}

/* ---------- checks */

static int name_frame, name_tick, name_a, name_b, name_c, name_texture, name_pass, name_inner_aggregate;

static void frame(int scopes)
{
	int index;

	profile_trace_frame_boundary();
	profile_trace_begin(name_frame);
	for (index = 0; index < scopes; index++)
	{
		profile_trace_begin(name_a);
		advance(1000);
		profile_trace_begin(name_b);
		advance(500);
		profile_trace_end(name_b);
		profile_trace_end(name_a);
	}
	advance(1000);
	profile_trace_end(name_frame);
}

static void scope_checks(void)
{
	struct profile_trace_status status;
	int index;

	forget_written();
	check(profile_trace_request_start(0.0, _profile_trace_when_now, 4) == _profile_trace_answer_armed,
		"profile_record arms");
	profile_trace_status(&status);
	check(status.state == _profile_trace_armed, "armed until the next frame boundary");
	check(strncmp(status.name, "profile_", 8) == 0 && strstr(status.name, "_host"), "named when asked for");
	check(!profile_trace_recording(), "not recording before the boundary");
	frame(3);
	check(profile_trace_recording(), "recording from the boundary");
	/* the first map load after launch (the main menu) does not stop it */
	profile_trace_map_loaded();
	profile_trace_frame_boundary();
	check(profile_trace_recording(), "the first map load does not stop a recording");
	check(profile_trace_request_start(0.0, _profile_trace_when_now, 4) == _profile_trace_answer_already_recording,
		"a second profile_record is refused");

	/* nesting deeper than the stack: not recorded, counted, and the rest
	still balances */
	profile_trace_frame_boundary();
	for (index = 0; index < MAXIMUM_PROFILE_TRACE_DEPTH + 3; index++)
		profile_trace_begin(name_c);
	for (index = 0; index < MAXIMUM_PROFILE_TRACE_DEPTH + 3; index++)
		profile_trace_end(name_c);
	/* an end that is not the top's, and one with nothing open */
	profile_trace_begin(name_a);
	profile_trace_end(name_b);
	profile_trace_end(name_a);
	profile_trace_end(name_b);
	/* aggregate scopes: summed for the frame, no records */
	profile_trace_begin(name_texture);
	profile_trace_begin(name_inner_aggregate);
	advance(500);
	profile_trace_begin(name_aggregate_child);
	advance(1000);
	profile_trace_end(name_aggregate_child);
	profile_trace_end(name_inner_aggregate);
	advance(500);
	profile_trace_end(name_texture);
	for (index = 0; index < 5; index++)
	{
		profile_trace_begin(name_texture);
		advance(2000);
		profile_trace_end(name_texture);
	}
	/* a scope left open is dropped at the boundary, and counted */
	profile_trace_begin(name_a);
	frame(1);
	check(profile_trace_request_stop() == _profile_trace_answer_stopping, "profile_stop stops");
	check(profile_trace_recording(), "a stop applies at the next boundary");
	profile_trace_frame_boundary();
	check(!profile_trace_recording(), "stopped at the boundary");
	check(profile_trace_request_start(0.0, _profile_trace_when_now, 4) == _profile_trace_answer_still_writing ||
		writer_finished(), "profile_record while writing is refused");
	finish();
	check(written.finished == 1 && written.parts == 1, "one part written");
	check(written.counts[name_frame] == 2, "two frames recorded");
	check(written.counts[name_a] == 5 && written.counts[name_b] == 4, "every scope recorded once");
	check(written.counts[name_c] == MAXIMUM_PROFILE_TRACE_DEPTH, "scopes within the stack recorded");
	check(written.deep == 3, "scopes past the stack counted");
	check(written.unbalanced == 3, "unbalanced ends and the open scope counted");
	check(written.counts[name_texture] == 0 && written.counter_records >= 2, "aggregate names give counters");
	check(written.aggregate_child_depth == 0, "ordinary scopes inside an aggregate keep visible depth");
	check(written.aggregate_count[name_texture] == 6 && written.aggregate_time[name_texture] == 12000,
		"outer aggregate totals include calls nested with another aggregate");
	check(written.aggregate_count[name_inner_aggregate] == 1 && written.aggregate_time[name_inner_aggregate] == 1500,
		"nested aggregate keeps its own cpu summary count and time");
	check(written.counts[name_aggregate_child] == 1, "ordinary nested scope contributes one cpu summary count");
	check(written.stop_reason[0] == _profile_trace_stop_command, "stopped by the command");
	check(outstanding() == 0, "both arenas freed after the recording");
}

static void state_checks(void)
{
	struct profile_trace_status status;

	/* game: armed until a game is in progress, stopped by a map load, then
	armed again */
	forget_written();
	fake_ready = 0;
	fake_map = "";
	fake_map_name = "";
	check(profile_trace_request_start(0.0, _profile_trace_when_game, 4) == _profile_trace_answer_armed, "armed for a game");
	frame(1);
	frame(1);
	profile_trace_status(&status);
	check(status.state == _profile_trace_armed, "waits for a game");
	fake_ready = 1;
	frame(1);
	check(profile_trace_recording(), "records once a game is in progress");
	frame(1);
	fake_map = "levels\\next\\next";
	fake_map_name = "next";
	profile_trace_map_loaded();
	frame(1);
	check(!profile_trace_recording(), "a map load stops it");
	finish();
	fake_map = "levels\\a30\\a30";
	fake_map_name = "a30";
	profile_trace_status(&status);
	check(status.state == _profile_trace_armed, "armed again for the next game");
	check(written.stop_reason[0] == _profile_trace_stop_map_load, "stopped by the map load");
	check(strcmp(written.map_name[0], "next") != 0, "the map that stops a recording is not sampled into it");
	frame(1);
	check(profile_trace_recording(), "the next game is recorded");
	check(profile_trace_request_stop() == _profile_trace_answer_stopping, "stopped by hand");
	frame(1);
	finish();
	profile_trace_status(&status);
	check(status.state == _profile_trace_idle, "a stop by hand does not arm again");
	check(outstanding() == 0, "nothing left allocated");

	/* the seconds of profile_record */
	forget_written();
	profile_trace_request_start(0.5, _profile_trace_when_now, 4);
	frame(1);
	advance(400000000ULL);
	frame(1);
	check(profile_trace_recording(), "still recording before its seconds");
	advance(200000000ULL);
	frame(1);
	check(!profile_trace_recording(), "stopped after its seconds");
	finish();
	check(written.stop_reason[0] == _profile_trace_stop_seconds, "stopped by its seconds");

	/* no memory: not recording, said so, nothing kept */
	allocations_refused = 1;
	profile_trace_request_start(0.0, _profile_trace_when_now, 4);
	frame(1);
	allocations_refused = 0;
	profile_trace_status(&status);
	check(status.state == _profile_trace_idle && strstr(last_notice, "no memory for 4 MB"), "no memory is reported");
	check(outstanding() == 0, "a failed allocation keeps nothing");
}

static volatile int p2p_running;
static unsigned long p2p_recorded;

static void *p2p_thread(void *unused)
{
	(void)unused;
	profile_trace_thread_register(_profile_track_p2p);
	while (__atomic_load_n(&p2p_running, __ATOMIC_RELAXED))
	{
		fake_lock();
		if (profile_trace_recording())
			p2p_recorded++;
		profile_trace_begin(name_pass);
		profile_trace_end(name_pass);
		fake_unlock();
		/* (a pass every 50 microseconds: the p2p region holds an eighth of
		the scopes, as the game's p2p thread has far fewer) */
		usleep(50);
	}
	return NULL;
}

/* what profile_stop answers while a recording is only armed: a game is waited for
only by "game" mode, which stop ends too */
static void stop_answer_checks(void)
{
	struct profile_trace_status status;

	check(profile_trace_request_start(0.0, _profile_trace_when_now, 4) == _profile_trace_answer_armed, "armed for the next frame");
	check(profile_trace_request_stop() == _profile_trace_answer_not_recording,
		"stopped before it began, a recording armed for the next frame was waiting for no game");
	profile_trace_status(&status);
	check(status.state == _profile_trace_idle, "stopped while armed, it does not start");
	check(profile_trace_request_start(0.0, _profile_trace_when_game, 4) == _profile_trace_answer_armed, "armed for a game");
	check(profile_trace_request_stop() == _profile_trace_answer_disarmed, "a recording armed for a game is not waited for any more");
	profile_trace_status(&status);
	check(status.state == _profile_trace_idle, "stopped while armed for a game, it does not start");
}

static void part_checks(void)
{
	pthread_t thread;
	unsigned long frames_recorded = 0;
	int frame_index, part;
	int continuous = 1;

	forget_written();
	p2p_recorded = 0;
	p2p_running = 1;
	pthread_create(&thread, NULL, p2p_thread, NULL);
	profile_trace_request_start(0.0, _profile_trace_when_now, 4);
	/* (4 MB: about 86 000 game records a part) */
	for (frame_index = 0; frame_index < 3000; frame_index++)
	{
		frame(50);
		if (profile_trace_recording())
			frames_recorded++;
		if (frame_index % 30 == 0)
			profile_trace_tick();
		advance(1000000);
	}
	profile_trace_request_stop();
	profile_trace_frame_boundary();
	__atomic_store_n(&p2p_running, 0, __ATOMIC_RELAXED);
	pthread_join(thread, NULL);
	finish();
	fake_role = "host";
	fake_players = 2;
	fake_own_machine = -1;
	check(written.parts >= 3, "a long recording is cut into parts");
	check(written.counts[name_frame] == frames_recorded, "every frame in exactly one part");
	check(written.counts[name_a] == frames_recorded * 50, "every scope in exactly one part");
	check(written.counts[name_pass] == p2p_recorded, "every recorded p2p pass in exactly one part");
	check(written.dropped == 0, "nothing dropped");
	for (part = 1; part < written.parts && part < MAXIMUM_PARTS; part++)
	{
		continuous &= written.first_frame[part] == written.first_frame[part - 1] + written.frames[part - 1];
		continuous &= written.first_interval[part] == written.first_interval[part - 1] + written.intervals[part - 1];
		continuous &= written.start_ns[part] == written.end_ns[part - 1];
		continuous &= !written.last[part - 1];
	}
	check(continuous, "frames, intervals and time continue across parts");
	check(written.last[written.parts - 1], "only the last part is the last");
	check(written.records_in_span, "each record within its part");
	check(outstanding() == 0, "both arenas freed after a long recording");

	/* a cut while the writer holds the other arena waits for it */
	forget_written();
	written.delay_milliseconds = 1000;
	p2p_recorded = 0;
	p2p_running = 1;
	pthread_create(&thread, NULL, p2p_thread, NULL);
	profile_trace_request_start(0.0, _profile_trace_when_now, 4);
	frames_recorded = 0;
	for (frame_index = 0; frame_index < 2000; frame_index++)
	{
		frame(50);
		if (profile_trace_recording())
			frames_recorded++;
	}
	profile_trace_request_stop();
	profile_trace_frame_boundary();
	__atomic_store_n(&p2p_running, 0, __ATOMIC_RELAXED);
	pthread_join(thread, NULL);
	finish();
	written.delay_milliseconds = 0;
	check(written.records_in_span, "each record within its part while the writer is slow");
	check(written.counts[name_pass] == p2p_recorded, "a slow writer loses no p2p pass");
	check(written.parts >= 3, "parts while the writer is slow");
	check(written.writer_wait_ms > 0.0, "the wait for the writer is counted");
	check(written.counts[name_a] == frames_recorded * 50, "a slow writer loses nothing");
	check(written.counts[profile_trace_name("profile_wait")] >= 1, "the wait is a scope");
}

static int straddle_begun;
static int straddle_released;

/* a p2p pass that lets go of its lock for slow work (p2p_resolve's DNS) */
static void *straddling_pass(void *unused)
{
	(void)unused;
	profile_trace_thread_register(_profile_track_p2p);
	fake_lock();
	profile_trace_begin(name_pass);
	fake_unlock();
	__atomic_store_n(&straddle_begun, 1, __ATOMIC_RELEASE);
	while (!__atomic_load_n(&straddle_released, __ATOMIC_ACQUIRE))
		usleep(100);
	fake_lock();
	profile_trace_end(name_pass);
	fake_unlock();
	return NULL;
}

static void straddle(int cut)
{
	pthread_t thread;
	struct profile_trace_status status;

	forget_written();
	straddle_begun = straddle_released = 0;
	profile_trace_request_start(0.0, _profile_trace_when_now, 4);
	frame(1);
	pthread_create(&thread, NULL, straddling_pass, NULL);
	while (!__atomic_load_n(&straddle_begun, __ATOMIC_ACQUIRE))
		usleep(100);
	if (cut)
	{
		/* frames until a cut comes, while the pass is out of its lock */
		do
		{
			frame(50);
			profile_trace_status(&status);
		}
		while (status.part < 2);
	}
	else
	{
		profile_trace_request_stop();
		profile_trace_frame_boundary();
	}
	__atomic_store_n(&straddle_released, 1, __ATOMIC_RELEASE);
	pthread_join(thread, NULL);
	if (cut)
	{
		profile_trace_request_stop();
		profile_trace_frame_boundary();
	}
	finish();
}

/* a pass begun in one recording and ended in the next (a stop, a re-arm and
a start between): its start is before the new origin, so it is dropped, and
counted in the new recording. concurrent: it ends while the start is made */
static void straddle_restart(int concurrent)
{
	pthread_t thread;

	forget_written();
	straddle_begun = straddle_released = 0;
	profile_trace_request_start(0.0, _profile_trace_when_now, 4);
	frame(1);
	pthread_create(&thread, NULL, straddling_pass, NULL);
	while (!__atomic_load_n(&straddle_begun, __ATOMIC_ACQUIRE))
		usleep(100);
	profile_trace_request_stop();
	profile_trace_frame_boundary();
	finish();
	forget_written();
	advance(1000000);
	profile_trace_request_start(0.0, _profile_trace_when_now, 4);
	if (concurrent)
	{
		__atomic_store_n(&straddle_released, 1, __ATOMIC_RELEASE);
		frame(1);
	}
	else
	{
		frame(1);
		__atomic_store_n(&straddle_released, 1, __ATOMIC_RELEASE);
	}
	pthread_join(thread, NULL);
	frame(1);
	profile_trace_request_stop();
	profile_trace_frame_boundary();
	finish();
}

static void straddle_checks(void)
{
	straddle(1);
	check(written.counts[name_pass] == 1, "a pass across a cut is in the part it ended in, once");
	straddle(0);
	check(written.counts[name_pass] == 0, "a pass that outlives its recording is not written");
	check(outstanding() == 0, "nothing left after a pass across a stop");
	straddle_restart(0);
	check(written.counts[name_pass] == 0, "a pass across a restart is dropped, not given a wrapped start");
	check(written.dropped == 1, "and counted in the new recording");
	straddle_restart(1);
	check(written.counts[name_pass] == 0, "a pass ending as the next recording starts is dropped");
	check(outstanding() == 0, "nothing left after a pass across a restart");
}

static void reliable_entry_checks(void)
{
	static const struct profile_net_layout_member layout[] =
	{
		{ "object_index", 0, 4, _profile_net_key_datum },
		{ "flags", 4, 4, _profile_net_key_none },
	};
	struct profile_net_row rows[64];
	struct profile_net_snapshot snapshot;
	unsigned char message[25] = { 0 };
	int key = 99, entries = 0, failures = 0;
	unsigned long count, index;
	long first, intervals;

	profile_net_layout(6, layout, 2, 8);
	message[2] = 6;
	message[3] = 2;
	memcpy(message + 8, &key, 4);
	memcpy(message + 16, &key, 4);
	profile_net_recording_begin();
	profile_net_part_begin(rows, 64, 0, 0);
	profile_net_reliable(1, message, sizeof(message) - 1, 1);
	/* a truncated payload is still a sent message, but is not a pair of entries */
	profile_net_reliable(1, message, 15, 1);
	/* a complete pair followed by a partial entry is not a valid fixed table */
	profile_net_reliable(1, message, 25, 1);
	profile_net_reliable(1, message, sizeof(message), 0);
	profile_net_part_end(1000000000ULL, &count, &first, &intervals, &snapshot);
	for (index = 0; index < count; index++)
	{
		if (rows[index].table == _profile_net_table_entries && rows[index].type == 6)
		{
			check(rows[index].key == 99 && rows[index].values[0] == 16,
				"reliable fixed entries keep their full object key and payload bytes");
			entries += rows[index].values[1];
		}
		if (rows[index].table == _profile_net_table_send_failures)
		{
			check(rows[index].type == 1, "a reliable failure is distinguished from a datagram failure");
			failures += rows[index].values[0];
		}
	}
	check(entries == 2, "only a successful bounded reliable payload adds layout entries");
	check(failures == 1, "a reliable write failure is counted once");
	profile_net_recording_end();
}

static void net_checks(void)
{
	static const struct profile_net_layout_member layout[] =
	{
		{ "object_index", 0, 4, _profile_net_key_datum },
		{ "flags", 4, 4, _profile_net_key_none },
	};
	static const struct profile_net_message_name names[] =
	{
		{ 6, "object_states", "network_objects_handle_states" },
	};
	struct profile_net_row rows[2048];
	struct profile_net_snapshot snapshot;
	unsigned char entries[8 * 4];
	unsigned long row_count, index, totals[4] = { 0, 0, 0, 0 };
	long first_interval, intervals;
	int site, field_a, field_b, key;
	struct profile_net_loss loss;

	profile_net_message_names(names, 1);
	profile_net_layout(6, layout, 2, 8);
	profile_net_set_object_describe(describe_object);
	profile_net_recording_begin();
	profile_net_part_begin(rows, 2048, 0, 0);

	/* batches: messages counted at the flush, as sent or failed */
	site = profile_net_site("distributed_host_send_states", 1112);
	check(site == profile_net_site("distributed_host_send_states", 1112), "a site is interned");
	profile_net_site_push(site);
	profile_net_site_push(profile_net_site("send_to_clients", 570));
	profile_net_built(6);
	profile_net_batch_append(0, 0, 6, 50, 2);
	profile_net_batch_append(0, 0, 6, 30, 1);
	profile_net_site_pop();
	profile_net_site_pop();
	profile_net_batch_flush(0, 0, 88, 1);
	profile_net_batch_append(1, 1, 6, 50, 2);
	profile_net_send_failed(_profile_net_failure_no_connection);
	profile_net_batch_flush(1, 1, 58, 0);
	/* an object key described once, an unknown one as unknown */
	key = 99;
	memcpy(entries, &key, 4);
	profile_net_entries(0, 6, entries, 1, 8);
	profile_net_entries(0, 6, entries, 1, 8);
	profile_net_part_end(1000000000ULL, &row_count, &first_interval, &intervals, &snapshot);
	for (index = 0; index < row_count; index++)
	{
		if (rows[index].table == _profile_net_table_messages)
		{
			totals[0] += rows[index].values[0];
			if (rows[index].type == 6)
				check(rows[index].site == site, "the outermost site is the message's");
		}
		if (rows[index].table == _profile_net_table_send_failures)
		{
			check(rows[index].aux == _profile_net_failure_no_connection && rows[index].values[1] == 58,
				"a failed send counts its reason and bytes");
		}
		if (rows[index].table == _profile_net_table_built)
			totals[1] += rows[index].values[0];
		if (rows[index].table == _profile_net_table_entries)
			totals[2] += rows[index].values[1];
	}
	check(totals[0] == 88, "the bytes by type and the batch header equal the datagram");
	check(totals[1] == 1, "a message is built once, before its fan-out");
	check(totals[2] == 2, "entries counted by key");
	check(snapshot.objects == 1 && strcmp(profile_net_object_entry(0)->object_type, "unknown") == 0,
		"an object no longer there is unknown, described once");
	check(snapshot.sites >= 2 && first_interval == 0 && intervals == 1, "the snapshot counts the tables");

	/* a bigger row region for the key test */
	{
		struct profile_net_row *many = calloc(40000, sizeof(*many));
		unsigned long long bytes = 0, counted = 0;
		int next_interval_rows = 0;
		int other_rows = 0;

		profile_net_part_begin(many, 40000, 1000000000ULL, 0);
		for (key = 0; key < MAXIMUM_PROFILE_NET_INTERVAL_KEYS + 10; key++)
		{
			int object = key;

			memcpy(entries, &object, 4);
			profile_net_entries(0, 6, entries, 1, 8);
			bytes += 8;
		}
		profile_net_frame(2100000000ULL, 30);
		/* a new interval: the scratch index starts over */
		memcpy(entries, &key, 4);
		profile_net_entries(0, 6, entries, 1, 8);
		profile_net_part_end(2200000000ULL, &row_count, &first_interval, &intervals, &snapshot);
		for (index = 0; index < row_count; index++)
		{
			if (many[index].table != _profile_net_table_entries)
				continue;
			if (many[index].interval == 1)
			{
				counted += many[index].values[0];
				if (many[index].key == PROFILE_NET_KEY_OTHER)
				{
					check(many[index].values[1] == 10, "keys past the limit go to other");
					other_rows++;
				}
			}
			if (many[index].interval == 2)
			{
				check(many[index].key == key, "the next interval keys again");
				next_interval_rows++;
			}
		}
		check(next_interval_rows == 1, "the next interval has rows of its own");
		check(other_rows == 1, "one other row for the keys past the limit");
		check(counted == bytes, "entry totals stay exact past the key limit");
		check(intervals == 2 && first_interval == 1, "an interval closes after a second");
		free(many);
	}

	/* packed entries: fields committed on add, discarded on drop, the rest
	"header" */
	profile_net_part_begin(rows, 2048, 3000000000ULL, 0);
	field_a = profile_net_field("&state->position");
	field_b = profile_net_field("relayed->control_flags[history]");
	check(strcmp(profile_net_field_entry(field_a)->name, "position") == 0, "a field is named by its member");
	check(strcmp(profile_net_field_entry(field_b)->name, "control_flags") == 0, "an array field without its index");
	profile_net_field_put(field_a, 12);
	profile_net_field_put(field_b, 2);
	profile_net_packed_entry(0, 2, 3, 17, 1);
	profile_net_field_put(field_a, 12);
	profile_net_packed_entry(0, 2, 4, 15, 0);
	profile_net_field_put(field_b, 2);
	profile_net_packed_entry(0, 2, 5, 2, 1);
	profile_net_part_end(4000000000ULL, &row_count, &first_interval, &intervals, &snapshot);
	memset(totals, 0, sizeof(totals));
	for (index = 0; index < row_count; index++)
	{
		if (rows[index].table != _profile_net_table_field_bytes)
			continue;
		if (rows[index].site == (unsigned short)field_a)
			totals[0] += rows[index].values[0];
		else if (rows[index].site == (unsigned short)field_b)
			totals[1] += rows[index].values[0];
		else if (rows[index].site == 0)
			totals[2] += rows[index].values[0];
	}
	check(totals[0] == 12 && totals[1] == 4 && totals[2] == 3, "packed fields: committed, discarded, header");
	profile_net_recording_end();

	/* tunnel loss from the packet counter */
	memset(&loss, 0, sizeof(loss));
	check(profile_net_loss_step(&loss, 10, 10) == 0, "the first sample is the base");
	check(profile_net_loss_step(&loss, 20, 20) == 0, "in order: no loss");
	check(profile_net_loss_step(&loss, 30, 27) == 3, "a gap is loss");
	check(profile_net_loss_step(&loss, 40, 37) == 0, "reordered within the second: no loss");
	check(profile_net_loss_step(&loss, 50, 45) == 2, "late packets count lost first");
	check(profile_net_loss_step(&loss, 60, 57) == 0, "and make up for it when they come");
	check(profile_net_loss_step(&loss, 70, 66) == 0, "later loss is set against them");
	check(profile_net_loss_step(&loss, 170, 67) == 98, "a gap wider than the window");
}

/* an entry as the netcode's files lay theirs out: its members in a table
made of the real macros, and a gap of padding the compiler leaves after the
last member */
struct sample_entry
{
	int object_index;
	unsigned char player_index;
	unsigned char pad;
	short count;
	float position[3];
	unsigned char last;
};

#define SAMPLE_ENTRY_LAYOUT(M) \
	M(struct sample_entry, object_index, _profile_net_key_datum) \
	M(struct sample_entry, player_index, _profile_net_key_none) \
	M(struct sample_entry, pad, _profile_net_key_none) \
	M(struct sample_entry, count, _profile_net_key_none) \
	M(struct sample_entry, position, _profile_net_key_none) \
	M(struct sample_entry, last, _profile_net_key_none)
#define SAMPLE_PLAYER_LAYOUT(M) \
	M(struct sample_entry, object_index, _profile_net_key_none) \
	M(struct sample_entry, player_index, _profile_net_key_player)
/* (a table that left members out does not add up to where the last ends) */
#define SAMPLE_SHORT_LAYOUT(M) \
	M(struct sample_entry, object_index, _profile_net_key_datum) \
	M(struct sample_entry, count, _profile_net_key_none)

static const struct profile_net_layout_member sample_entry_layout[] = { SAMPLE_ENTRY_LAYOUT(PROFILE_NET_MEMBER) };
static const struct profile_net_layout_member sample_player_layout[] = { SAMPLE_PLAYER_LAYOUT(PROFILE_NET_MEMBER) };
typedef char sample_entry_layout_assert[(0 SAMPLE_ENTRY_LAYOUT(PROFILE_NET_MEMBER_SIZE)) ==
	PROFILE_NET_END(struct sample_entry, last) ? 1 : -1];
typedef char sample_short_layout_is_caught[(0 SAMPLE_SHORT_LAYOUT(PROFILE_NET_MEMBER_SIZE)) !=
	PROFILE_NET_END(struct sample_entry, last) ? 1 : -1];

/* (nor does one that left a member out after the last it lists: the
writer would report its bytes as padding) */
struct sample_trailing
{
	int first;
	short last;
	int added_later;
};
#define SAMPLE_TRAILING_LAYOUT(M) \
	M(struct sample_trailing, first, _profile_net_key_none) \
	M(struct sample_trailing, last, _profile_net_key_none)
typedef char sample_trailing_member_is_caught[PROFILE_NET_LAYOUT_OK(0 SAMPLE_TRAILING_LAYOUT(PROFILE_NET_MEMBER_SIZE),
	struct sample_trailing, last) ? -1 : 1];
typedef char sample_entry_tail_is_padding[PROFILE_NET_LAYOUT_OK(0 SAMPLE_ENTRY_LAYOUT(PROFILE_NET_MEMBER_SIZE),
	struct sample_entry, last) ? 1 : -1];

enum
{
	SAMPLE_ENTRY_TYPE = 7,
	SAMPLE_PLAYER_TYPE = 8,
};

/* the layouts the netcode's files register: members as the compiler laid
them out (the tail of padding past the last one is what the writer reports
as tail_pad), the key's kind, and what a keyed entry counts under */
static void layout_checks(void)
{
	static const char *const names[] = { "object_index", "player_index", "pad", "count", "position", "last" };
	static const int keys[] = { _profile_net_key_datum, 0, 0, 0, 0, 0 };
	struct profile_net_row rows[256];
	struct profile_net_snapshot snapshot;
	struct sample_entry entries[3];
	const struct profile_net_layout_entry *layout = NULL;
	unsigned long row_count, index, bytes[3] = { 0, 0, 0 };
	long first_interval, intervals;
	int member, described = 0, none_rows = 0, keyed_players = 0;

	profile_net_layout(SAMPLE_ENTRY_TYPE, sample_entry_layout, (int)(sizeof(sample_entry_layout) / sizeof(sample_entry_layout[0])),
		sizeof(struct sample_entry));
	profile_net_layout(SAMPLE_PLAYER_TYPE, sample_player_layout, 2, sizeof(struct sample_entry));
	for (index = 0; index < 16 && !layout; index++)
	{
		if (profile_net_layout_entry((long)index)->type == SAMPLE_ENTRY_TYPE)
			layout = profile_net_layout_entry((long)index);
	}
	check(layout && layout->count == 6 && layout->entry_size == sizeof(struct sample_entry),
		"a layout keeps its members and its entry's size");
	if (layout && layout->count == 6)
	{
		for (member = 0; member < layout->count; member++)
		{
			check(strcmp(layout->members[member].name, names[member]) == 0, "a member is named as in the struct");
			check(layout->members[member].key == keys[member], "a member's key kind");
		}
		check(layout->members[0].offset == 0 && layout->members[0].size == 4, "the key member's offset and size");
		check(layout->members[2].offset == 5 && layout->members[2].size == 1, "a byte's offset and size");
		check(layout->members[3].offset == 6 && layout->members[3].size == 2, "a short's offset");
		check(layout->members[4].offset == 8 && layout->members[4].size == 12, "an array member's size is all of it");
		check(layout->members[5].offset == 20 && layout->members[5].size == 1, "the last member's offset");
		check(layout->entry_size == 24 && layout->members[5].offset + layout->members[5].size == 21,
			"the entry ends 3 bytes past its last member");
	}

	memset(entries, 0, sizeof(entries));
	entries[0].object_index = (int)0xE1740005UL;
	entries[1].object_index = (int)0xE1740005UL;
	/* (a datum handle's salt is bit 31, so a key is negative: only -1 and -2 are not keys) */
	entries[2].object_index = -1;
	profile_net_recording_begin();
	profile_net_part_begin(rows, 256, 0, 0);
	profile_net_entries(0, SAMPLE_ENTRY_TYPE, entries, 3, sizeof(struct sample_entry));
	entries[0].player_index = 3;
	profile_net_entries(0, SAMPLE_PLAYER_TYPE, entries, 1, sizeof(struct sample_entry));
	profile_net_part_end(1000000000ULL, &row_count, &first_interval, &intervals, &snapshot);
	for (index = 0; index < row_count; index++)
	{
		if (rows[index].table != _profile_net_table_entries)
			continue;
		if (rows[index].type == SAMPLE_ENTRY_TYPE && rows[index].key == (int)0xE1740005UL)
		{
			bytes[0] += rows[index].values[0];
			check(rows[index].values[1] == 2, "a handle's entries are counted under it");
		}
		else if (rows[index].type == SAMPLE_ENTRY_TYPE && rows[index].key == PROFILE_NET_KEY_NONE)
		{
			bytes[1] += rows[index].values[0];
			none_rows++;
		}
		else if (rows[index].type == SAMPLE_PLAYER_TYPE && rows[index].key == 3)
		{
			bytes[2] += rows[index].values[0];
		}
	}
	check(bytes[0] == 48 && bytes[1] == 24 && none_rows == 1 && bytes[2] == 24,
		"a keyed entry counts its size under its key; -1 is no key, a player's index is");
	for (index = 0; index < (unsigned long)snapshot.objects; index++)
	{
		const struct profile_net_object_entry *object = profile_net_object_entry((long)index);

		if (object->key == (int)0xE1740005UL)
			described++;
		if (object->key == PROFILE_NET_KEY_NONE || object->key == 3)
			keyed_players++;
	}
	check(described == 1 && keyed_players == 0, "a handle is described once; -1 and a player's index are no objects");
	profile_net_recording_end();
}

/* the netcode's measured put (network_distributed.c's macro, extracted by
test_profile.py): the bytes it writes are the plain function's, each
argument is evaluated once, and a recording counts the field under its
member's name once its entry is added */
static unsigned char *distributed_put(unsigned char *cursor, void const *data, short size)
{
	memcpy(cursor, data, (size_t)size);
	return cursor + size;
}

#include DISTRIBUTED_PUT_MACRO

static int put_calls;

static int put_argument(int value)
{
	put_calls++;
	return value;
}

static void put_checks(void)
{
	struct profile_net_row rows[256];
	struct profile_net_snapshot snapshot;
	struct { int position[3]; short flags; unsigned char control[2]; } state = { { 1, 2, 3 }, 4, { 5, 6 } };
	unsigned char measured[64], plain[64], *cursor, *other;
	unsigned long row_count, index, position = 0, control = 0, header = 0;
	long first_interval, intervals;
	int round;

	/* not recording: the same bytes, each argument evaluated once, nothing counted */
	memset(measured, 0xEE, sizeof(measured));
	memset(plain, 0xEE, sizeof(plain));
	put_calls = 0;
	cursor = measured;
	for (round = 0; round < 2; round++)
	{
		cursor = distributed_put(cursor + put_argument(0), &state.position, (short)put_argument(sizeof(state.position)));
		cursor = distributed_put(cursor, &state.flags, sizeof(state.flags));
		cursor = distributed_put(cursor, &state.control[1], sizeof(state.control[1]));
	}
	other = plain;
	for (round = 0; round < 2; round++)
	{
		memcpy(other, &state.position, sizeof(state.position));
		other += sizeof(state.position);
		memcpy(other, &state.flags, sizeof(state.flags));
		other += sizeof(state.flags);
		memcpy(other, &state.control[1], 1);
		other += 1;
	}
	check(put_calls == 4, "a put evaluates its cursor and its size once");
	check(cursor - measured == other - plain && memcmp(measured, plain, sizeof(measured)) == 0,
		"the measured put writes the bytes the plain one does");

	/* recording: an entry's puts are counted when it is added, dropped when it is not; what they leave is the header */
	profile_net_recording_begin();
	profile_net_part_begin(rows, 256, 0, 0);
	put_calls = 0;
	cursor = distributed_put(measured, &state.position, (short)put_argument(sizeof(state.position)));
	cursor = distributed_put(cursor, &state.control[0], sizeof(state.control[0]));
	check(put_calls == 1, "a recording evaluates the size once too");
	check(memcmp(measured, &state.position, sizeof(state.position)) == 0 && measured[12] == 5 && cursor == measured + 13,
		"and writes the same bytes");
	profile_net_packed_entry(0, 2, 1, 15, 1);
	cursor = distributed_put(measured, &state.control[1], sizeof(state.control[1]));
	profile_net_packed_entry(0, 2, 1, 1, 0);
	cursor = distributed_put(measured, &state.position, sizeof(state.position));
	profile_net_packed_entry(0, 2, 2, 14, 1);
	profile_net_part_end(1000000000ULL, &row_count, &first_interval, &intervals, &snapshot);
	for (index = 0; index < row_count; index++)
	{
		const struct profile_net_field_entry *field;

		if (rows[index].table != _profile_net_table_field_bytes)
			continue;
		field = profile_net_field_entry(rows[index].site);
		if (strcmp(field->name, "position") == 0)
			position += rows[index].values[0];
		else if (strcmp(field->name, "control") == 0)
			control += rows[index].values[0];
		else if (strcmp(field->name, "header") == 0)
			header += rows[index].values[0];
	}
	check(position == 24 && control == 1 && header == 4, "the fields of the added entries are counted by member, the rest as header");
	profile_net_recording_end();
}

/* the sum of one value of a table's rows */
static unsigned long net_table_sum(struct profile_net_row const *rows, unsigned long count, int table, int value)
{
	unsigned long index, total = 0;

	for (index = 0; index < count; index++)
	{
		if (rows[index].table == table)
			total += rows[index].values[value];
	}
	return total;
}

struct site_reader
{
	long count;
	long bad;
};

static void *site_reader_thread(void *argument)
{
	struct site_reader *reader = argument;
	long index;

	for (index = 0; index < reader->count; index++)
	{
		struct profile_net_site_entry const *entry = profile_net_site_entry((int)index);

		if (!entry || !entry->function || !entry->function[0])
			reader->bad++;
	}
	return NULL;
}

static void net_sender_checks(void)
{
	struct profile_net_row rows[256];
	struct profile_net_snapshot snapshot;
	struct site_reader reader;
	pthread_t thread;
	unsigned long row_count, index, failed = 0;
	long first_interval, intervals;
	int sender, last = 0;
	/* (a site keeps its function name's pointer) */
	static char names[MAXIMUM_PROFILE_NET_SITES + 8][24];

	profile_net_recording_begin();
	profile_net_part_begin(rows, 256, 0, 0);
	/* every batch index the netcode has: a client's messages to its host go
	out as sender 128 (HOST_SENDER) */
	for (sender = 127; sender <= 128; sender++)
	{
		profile_net_batch_append(sender, sender == 128 ? PROFILE_NET_HOST : sender, 6, 50, 2);
		profile_net_batch_flush(sender, sender == 128 ? PROFILE_NET_HOST : sender, 58, 1);
	}
	/* a flush that failed books its reason and not its messages, and the next
	flush starts without the reason */
	profile_net_batch_append(5, 5, 6, 50, 2);
	profile_net_send_failed(_profile_net_failure_no_connection);
	profile_net_batch_flush(5, 5, 58, 0);
	profile_net_batch_append(5, 5, 6, 20, 1);
	profile_net_batch_flush(5, 5, 28, 0);
	profile_net_batch_append(5, 5, 6, 20, 1);
	profile_net_batch_flush(5, 5, 28, 1);
	profile_net_part_end(1000000000ULL, &row_count, &first_interval, &intervals, &snapshot);
	check(net_table_sum(rows, row_count, _profile_net_table_messages, 0) == 2 * 58 + 28,
		"messages of the last sender indices are counted, those of a failed flush are not");
	check(net_table_sum(rows, row_count, _profile_net_table_datagrams, 0) == 2 * 58 + 58 + 28 + 28, "datagrams of every flush");
	for (index = 0; index < row_count; index++)
	{
		if (rows[index].table == _profile_net_table_send_failures)
		{
			failed++;
			check(rows[index].aux == (rows[index].values[1] == 58 ? _profile_net_failure_no_connection :
				_profile_net_failure_write_failed), "a failure's reason is reset after its flush");
		}
	}
	check(failed == 2, "two failed flushes, two rows");
	profile_net_recording_end();

	/* sites are interned outside a recording and stay valid across them. The
	table is fixed and an entry is written before the count passes it, so the
	writer's reads below a part's count are safe with a concurrent append; a
	full table answers NO_SITE, which is not negative, so a call site's static
	id does not look it up again */
	profile_net_recording_begin();
	profile_net_part_begin(rows, 256, 0, 0);
	profile_net_part_end(1000000000ULL, &row_count, &first_interval, &intervals, &snapshot);
	reader.count = snapshot.sites;
	reader.bad = 0;
	check(reader.count >= 2, "sites are known at the part's cut");
	pthread_create(&thread, NULL, site_reader_thread, &reader);
	for (index = 0; index < MAXIMUM_PROFILE_NET_SITES + 8; index++)
	{
		snprintf(names[index], sizeof(names[index]), "late%lu", index);
		last = profile_net_site(names[index], 100000 + (long)index);
	}
	pthread_join(thread, NULL);
	profile_net_recording_end();
	check(reader.bad == 0, "a part reads its sites while the game thread adds");
	check(last == PROFILE_NET_NO_SITE, "a full sites table answers no site");
}

/* the report's invariant (the datagrams handed on are the message rows and
the header) must not break where the netcode's batches and the recording
disagree: a batch begun before the recording, and one the netcode threw away */
static void net_batch_seam_checks(void)
{
	struct profile_net_row rows[64];
	struct profile_net_snapshot snapshot;
	unsigned long row_count;
	long first_interval, intervals;

	profile_net_recording_begin();
	profile_net_part_begin(rows, 64, 0, 0);
	/* the batch held 50 bytes past its header when the recording began: only
	the 20 after are seen, and the datagram is 8 + 50 + 20 */
	profile_net_batch_append(3, 3, 6, 20, 1);
	profile_net_batch_flush(3, 3, 78, 1);
	profile_net_batch_append(3, 3, 6, 20, 1);
	profile_net_batch_flush(3, 3, 28, 1);
	profile_net_part_end(1000000000ULL, &row_count, &first_interval, &intervals, &snapshot);
	check(net_table_sum(rows, row_count, _profile_net_table_datagrams, 0) == 28 &&
		net_table_sum(rows, row_count, _profile_net_table_messages, 0) == 28,
		"a batch that began before the recording is not booked, the next one is");

	/* new_game empties the batches without a flush: the rows pending for them go too */
	profile_net_part_begin(rows, 64, 1000000000ULL, 0);
	profile_net_batch_append(4, 4, 6, 50, 1);
	profile_net_batch_discard(4);
	profile_net_batch_append(4, 4, 6, 20, 1);
	profile_net_batch_flush(4, 4, 28, 1);
	profile_net_part_end(2000000000ULL, &row_count, &first_interval, &intervals, &snapshot);
	check(net_table_sum(rows, row_count, _profile_net_table_datagrams, 0) == 28 &&
		net_table_sum(rows, row_count, _profile_net_table_messages, 0) == 28,
		"the rows of a batch the netcode threw away are not added to the next one's");
	profile_net_recording_end();

	/* a flush that does not add up is expected in the recording's first second
	only (its start, mid-batch): later it is counted, for the report to say */
	{
		struct profile_net_counts counts;

		profile_net_recording_begin();
		profile_net_part_begin(rows, 64, 0, 0);
		profile_net_batch_append(3, 3, 6, 20, 1);
		profile_net_batch_flush(3, 3, 99, 1);
		profile_net_frame(1100000000ULL, 1);
		profile_net_batch_append(3, 3, 6, 20, 1);
		profile_net_batch_flush(3, 3, 99, 1);
		profile_net_batch_append(3, 3, 6, 20, 1);
		profile_net_batch_flush(3, 3, 28, 1);
		profile_net_part_end(1200000000ULL, &row_count, &first_interval, &intervals, &snapshot);
		profile_net_counters(&counts);
		check(counts.batches_unbooked == 1, "a flush that does not add up is counted after the first second, not in it");
		profile_net_recording_end();
	}
}

static void lifetime_checks(void)
{
	int cycle;

	/* many recordings, cut and not, stopped every way, with nothing left */
	for (cycle = 0; cycle < 40; cycle++)
	{
		int frames = cycle % 4 == 0 ? 2500 : 3;
		int index;

		forget_written();
		profile_trace_request_start(0.0, _profile_trace_when_now, 4);
		for (index = 0; index < frames; index++)
			frame(50);
		if (cycle % 3 == 0)
		{
			profile_trace_map_loaded();
			profile_trace_frame_boundary();
		}
		else if (cycle % 3 == 1)
		{
			profile_trace_request_stop();
			profile_trace_frame_boundary();
		}
		else
		{
			profile_trace_shutdown();
		}
		finish();
		if (outstanding() != 0)
		{
			check(0, "a recording leaves nothing allocated");
			break;
		}
	}
	check(allocations_made > 80, "the cycles allocated");
	/* exit while armed, while writing, and twice */
	profile_trace_request_start(0.0, _profile_trace_when_game, 4);
	fake_ready = 0;
	profile_trace_shutdown();
	profile_trace_shutdown();
	check(outstanding() == 0, "shutdown while armed keeps nothing");
	fake_ready = 1;
}

static unsigned long profile_test_peer_endpoint(unsigned long virtual_address)
{
	return virtual_address == 0x01004064UL ? 0x01004064UL : 0;
}

/* a real recording through the real writer, into the folder */
static void file_checks(const char *folder)
{
	int frame_index;
	int quoted;
	unsigned char entries[8 * 4];
	static char connection_identity;
	struct profile_net_tunnel_peer peer;
	struct profile_trace_status status;
	long long range_length;
	session_sample_calls = 0;
	check(profile_json_range_length(2147483648LL, 6442450944LL, &range_length) &&
		range_length == 4294967296LL, "64-bit part ranges exceed 2 GiB");
	check(!profile_json_range_length(6442450944LL, 2147483648LL, &range_length), "reversed 64-bit ranges fail");
	{
		struct profile_trace_session session;
		char joined_name[128];

		fake_session(&session);
		strcpy(session.map_name, "levels\\test\\bloodgulch");
		strcpy(session.gametype, "ctf");
		profile_json_joined_name(folder, "profile_20261008-065527_client", &session, joined_name, sizeof(joined_name));
		check(strcmp(joined_name, "profile_20261008-065527_host_bloodgulch_ctf") == 0,
			"joined names use the sanitized scenario and engine type");
		strcpy(session.map_name, "levels\\a-b!?");
		strcpy(session.gametype, "slayer");
		profile_json_joined_name(folder, "profile_20261008-065527_host", &session, joined_name, sizeof(joined_name));
		check(strcmp(joined_name, "profile_20261008-065527_host_a-b--_slayer") == 0,
			"joined names replace unsupported map characters");
		session.map_name[0] = 0;
		profile_json_joined_name(folder, "profile_20261008-065527_host", &session, joined_name, sizeof(joined_name));
		check(strcmp(joined_name, "profile_20261008-065527_host_a30_slayer") == 0,
			"a legacy map path supplies the joined-name slug");
		session.map[0] = 0;
		profile_json_joined_name(folder, "profile_20261008-065527_host", &session, joined_name, sizeof(joined_name));
		check(strcmp(joined_name, "profile_20261008-065527_host_nomap_slayer") == 0,
			"an empty map is named nomap");
	}

	fake_folder = folder;
	profile_trace_set_writer(NULL);
	profile_trace_set_session_sampler(fake_session_sample);
	profile_net_set_peer_describe(profile_test_peer_endpoint);
	quoted = profile_trace_name("quote\"back\\slash");
	fake_map = "";
	fake_map_name = "";
	profile_trace_request_start(0.0, _profile_trace_when_now, 4);
	fake_role = "client";
	fake_players = 5;
	fake_own_machine = 1;
	fake_map = "levels\\a30\\a30";
	fake_map_name = "a30";
	memset(&peer, 0, sizeof(peer));
	for (frame_index = 0; frame_index < 2400; frame_index++)
	{
		if (frame_index == 2)
		{
			fake_map = "levels\\b30\\b30";
			fake_map_name = "b30";
		}
		frame(40);
		profile_trace_begin(name_texture);
		profile_trace_begin(name_inner_aggregate);
		advance(100);
		profile_trace_end(name_inner_aggregate);
		profile_trace_begin(name_aggregate_child);
		advance(100);
		profile_trace_end(name_aggregate_child);
		profile_trace_end(name_texture);
		if (frame_index == 0)
		{
			profile_net_machine_address(202, 0x64400001UL, 5152);
			profile_net_machine_address(201, 0x7F0000C9UL, 5151);
			profile_net_traffic(&connection_identity, _profile_net_out, _profile_net_datagram, 100,
				0x7F0000C9UL, 5151);
			profile_net_connection_machine(&connection_identity, 201);
			peer.virtual_address = 0x01004064UL;
			profile_net_tunnel(&peer, 1);
			peer.bytes_out = 1000;
			peer.bytes_in = 500;
			peer.packets_out = 1;
			peer.packets_in = 1;
			peer.highest_in = 1;
			profile_net_tunnel(&peer, 1);
		}
		/* (salted datum handles are negative: the part's entries and objects tables carry them as they are) */
		{
			int handle = (int)(0xE1740000UL + (unsigned long)(frame_index % 5));

			memcpy(entries, &handle, 4);
			profile_net_entries(0, 6, entries, 1, 8);
		}
		profile_net_field_put(profile_net_field("&state->position"), 12);
		profile_net_packed_entry(0, 2, 1, 20, 1);
		profile_trace_begin(quoted);
		advance(10);
		profile_trace_end(quoted);
		if (frame_index % 2 == 0)
		{
			profile_trace_begin(name_tick);
			profile_trace_tick();
			advance(100);
			profile_trace_end(name_tick);
		}
		advance(16000000);
	}
	profile_trace_shutdown();
	check(session_sample_calls > 0 && session_sample_calls < 2400,
		"session sampling is periodic and not per frame");
	fake_role = "host";
	fake_players = 2;
	fake_own_machine = -1;
	fake_map = "levels\\a30\\a30";
	fake_map_name = "a30";
	check(outstanding() == 0, "the real writer frees both arenas");
	profile_trace_status(&status);
	{
		char name[64];
		char joined_name[128];
		char path[512];
		FILE *file;
		struct profile_trace_session session;
		char joined_path[512];
		char temporary_path[520];

		profile_json_choose_name(folder, "20261005-142233", "host", name, sizeof(name));
		check(strcmp(name, "profile_20261005-142233_host") == 0, "a free name is taken as it is");
		snprintf(path, sizeof(path), "%s/%s.part1.json", folder, name);
		file = fopen(path, "wb");
		if (file)
			fclose(file);
		profile_json_choose_name(folder, "20261005-142233", "host", name, sizeof(name));
		check(strcmp(name, "profile_20261005-142233_host_2") == 0, "a name taken gets _2");
		remove(path);
		fake_session(&session);
		profile_json_joined_name(folder, "profile_20261005-142233_host", &session, joined_name, sizeof(joined_name));
		snprintf(joined_path, sizeof(joined_path), "%s/profile_20261005-142233_host_a30_campaign.json", folder);
		file = fopen(joined_path, "wb");
		if (file)
			fclose(file);
		profile_json_joined_name(folder, "profile_20261005-142233_host", &session, joined_name, sizeof(joined_name));
		check(strcmp(joined_name, "profile_20261005-142233_host_a30_campaign_2") == 0,
			"a joined trace name is not reused");
		remove(joined_path);
		profile_json_joined_name(folder, "profile_20261005-142233_host", &session, joined_name, sizeof(joined_name));
		check(strcmp(joined_name, "profile_20261005-142233_host_a30_campaign") == 0,
			"a free suffixed name is chosen for the joined trace");
		snprintf(joined_path, sizeof(joined_path), "%s/%s.json", folder, joined_name);
		file = fopen(joined_path, "wb");
		if (file)
			fclose(file);
		profile_json_joined_name(folder, "profile_20261005-142233_host", &session, joined_name, sizeof(joined_name));
		check(strcmp(joined_name, "profile_20261005-142233_host_a30_campaign_2") == 0,
			"a joined-name collision gets _2");
		remove(joined_path);
		snprintf(temporary_path, sizeof(temporary_path), "%s.tmp", joined_path);
		check(joined_file_exists(folder, status.name), "the C writer joins its recording");
		file = fopen(temporary_path, "rb");
		check(file == NULL, "a successful join leaves no temporary file");
		if (file)
			fclose(file);
		snprintf(path, sizeof(path), "%s/%s.part1.json", folder, status.name);
		file = fopen(path, "rb");
#ifndef PROFILE_CHECK_KEEP_PARTS
		check(file == NULL, "a successful join deletes its parts");
#else
		check(file != NULL, "the parity check retains source parts");
		check(strstr(last_delete_log, "cannot delete joined part") != NULL,
			"a part deletion failure after joining is logged");
#endif
		if (file)
			fclose(file);
	}
	{
		char temporary_path[520];
		char joined_name[128];
		struct profile_trace_session session;

		profile_trace_request_start(0.0, _profile_trace_when_now, 4);
		profile_trace_status(&status);
		fake_session(&session);
		profile_json_joined_name(folder, status.name, &session, joined_name, sizeof(joined_name));
		snprintf(temporary_path, sizeof(temporary_path), "%s/%s.json.tmp", folder, joined_name);
		check(mkdir(temporary_path, 0700) == 0, "a competing join temporary is prepared");
		frame(1);
		profile_trace_shutdown();
		check(access(temporary_path, F_OK) == 0, "a competing temporary file is preserved");
		rmdir(temporary_path);
	}
	{
		char part_path[512];
		FILE *file;

		profile_trace_request_start(0.0, _profile_trace_when_now, 4);
		frame(1);
		profile_trace_shutdown();
		profile_trace_status(&status);
		snprintf(part_path, sizeof(part_path), "%s/%s.part1.json", folder, status.name);
		check(joined_file_exists(folder, status.name), "a one-part recording is joined");
		file = fopen(part_path, "rb");
#ifndef PROFILE_CHECK_KEEP_PARTS
		check(file == NULL, "a one-part join removes its part");
#else
		check(file != NULL, "the parity build keeps its one-part source");
#endif
		if (file)
		{
			fclose(file);
#ifndef PROFILE_CHECK_KEEP_PARTS
			remove(part_path);
#endif
		}
	}
	{
		char temporary_path[520];
		char part_path[512];
		char joined_path[512];
		char joined_name[128];
		struct profile_trace_session session;
		FILE *file;
		int frame_index;

		profile_trace_request_start(0.0, _profile_trace_when_now, 4);
		profile_trace_status(&status);
		snprintf(temporary_path, sizeof(temporary_path), "%s/%s.part2.json.tmp", folder, status.name);
		check(mkdir(temporary_path, 0700) == 0, "the middle-part failure path is prepared");
		for (frame_index = 0; frame_index < 2400; frame_index++)
		{
			frame(40);
			profile_trace_begin(name_texture);
			advance(200);
			profile_trace_end(name_texture);
		}
		profile_trace_shutdown();
		check(strstr(last_log, "cannot write") != NULL, "a failed middle part write is logged");
		check(strstr(last_join_log, "cannot join") != NULL, "a failed middle part skips joining");
		rmdir(temporary_path);
		fake_session(&session);
		profile_json_joined_name(folder, status.name, &session, joined_name, sizeof(joined_name));
		snprintf(joined_path, sizeof(joined_path), "%s/%s.json", folder, joined_name);
		file = fopen(joined_path, "rb");
		check(file == NULL, "a failed middle part creates no joined file");
		if (file)
			fclose(file), remove(joined_path);
		snprintf(part_path, sizeof(part_path), "%s/%s.part1.json", folder, status.name);
		file = fopen(part_path, "rb");
		check(file != NULL, "a failed middle part keeps the already written first part");
		if (file)
		{
			fclose(file);
#ifndef PROFILE_CHECK_KEEP_PARTS
			remove(part_path);
#endif
		}
	}
	{
		char part_path[512];
		char temporary_path[520];
		FILE *file;
		profile_trace_request_start(0.0, _profile_trace_when_now, 4);
		frame(1);
		profile_trace_status(&status);
		snprintf(part_path, sizeof(part_path), "%s/%s.part1.json", folder, status.name);
		snprintf(temporary_path, sizeof(temporary_path), "%s/%s_a30_campaign.json.tmp", folder, status.name);
		last_join_log[0] = 0;
		profile_json_test_fail_join_after_start();
		profile_trace_shutdown();
		check(strstr(last_join_log, "cannot join") != NULL, "a mid-copy join failure is logged");
		file = fopen(temporary_path, "rb");
		check(file == NULL, "a mid-copy join failure removes its temporary");
		if (file)
			fclose(file), remove(temporary_path);
		file = fopen(part_path, "rb");
		check(file != NULL, "a mid-copy join failure keeps its parts");
		if (file)
		{
			fclose(file);
#ifndef PROFILE_CHECK_KEEP_PARTS
			remove(part_path);
#endif
		}
	}
}

/* a folder that cannot be written: each part is logged and given up, its
.tmp gone, the arenas freed, and the recording still finishes */
static void unwritable_checks(const char *folder)
{
	char missing[512];
	int frame_index;

	snprintf(missing, sizeof(missing), "%s/missing/deeper", folder);
	fake_folder = missing;
	last_log[0] = 0;
	profile_trace_request_start(0.0, _profile_trace_when_now, 4);
	for (frame_index = 0; frame_index < 2400; frame_index++)
		frame(40);
	profile_trace_request_stop();
	profile_trace_frame_boundary();
	finish();
	check(strstr(last_log, "cannot write") && strstr(last_log, "missing/deeper"), "a part that cannot be written is logged");
	check(outstanding() == 0, "a part that cannot be written frees its arena");
	fake_folder = folder;
}

/* what the received table holds for one type, reason and machine: bytes and
messages */
static void received_row(struct profile_net_row const *rows, unsigned long count, int type, int reason, int machine,
	unsigned long *bytes, unsigned long *messages)
{
	unsigned long index;

	*bytes = *messages = 0;
	for (index = 0; index < count; index++)
	{
		if (rows[index].table == _profile_net_table_received && rows[index].type == type &&
			rows[index].aux == reason && rows[index].machine == machine)
		{
			*bytes += rows[index].values[0];
			*messages += rows[index].values[1];
		}
	}
}

/* the receive path's rows: a message by its type, each drop reason beside
its message's row, a batch's header and its inner messages, and machines at
the top of the netcode's range and the host's own (a client's NONE) */
static void net_received_checks(void)
{
	struct profile_net_row rows[256];
	struct profile_net_snapshot snapshot;
	unsigned char message[64] = { 0 };
	unsigned long row_count, bytes, messages;
	long first_interval, intervals;
	int reason;

	profile_net_recording_begin();
	profile_net_part_begin(rows, 256, 0, 0);
	message[2] = 6;
	message[3] = 3;
	profile_net_received(127, message, 40, _profile_net_received_message, _profile_net_drop_handled);
	profile_net_received(127, message, 40, _profile_net_received_message, _profile_net_drop_handled);
	for (reason = _profile_net_drop_not_in_game; reason < NUMBER_OF_PROFILE_NET_DROPS; reason++)
		profile_net_received(PROFILE_NET_HOST, message, 10 * reason, _profile_net_received_message, reason);
	/* a datagram shorter than a message header: a type of its own */
	profile_net_received(PROFILE_NET_HOST, message, 3, _profile_net_received_message, _profile_net_drop_bad_size);
	/* a batch's datagram is its header; the messages in it are inner, and a
	length that did not fit drops the batch's rest */
	message[2] = PROFILE_NET_BATCH_HEADER;
	profile_net_received(5, message, 100, _profile_net_received_batch, _profile_net_drop_handled);
	profile_net_received(5, message, 100, _profile_net_received_batch, _profile_net_drop_bad_size);
	message[2] = 6;
	profile_net_received(5, message, 30, _profile_net_received_inner, _profile_net_drop_handled);
	profile_net_received(5, message, 20, _profile_net_received_inner, _profile_net_drop_stale);
	profile_net_part_end(1000000000ULL, &row_count, &first_interval, &intervals, &snapshot);

	received_row(rows, row_count, 6, _profile_net_drop_handled, 127, &bytes, &messages);
	check(bytes == 80 && messages == 2, "received messages of the top machine are counted by type");
	received_row(rows, row_count, 6, _profile_net_drop_handled, PROFILE_NET_HOST, &bytes, &messages);
	check(messages == NUMBER_OF_PROFILE_NET_DROPS - 1, "each dropped message keeps its row beside its reason row");
	for (reason = _profile_net_drop_not_in_game; reason < NUMBER_OF_PROFILE_NET_DROPS; reason++)
	{
		received_row(rows, row_count, 6, reason, PROFILE_NET_HOST, &bytes, &messages);
		check(bytes == (unsigned long)(10 * reason) && messages == 1, "a drop reason has a row of its message's bytes");
	}
	received_row(rows, row_count, 255, _profile_net_drop_bad_size, PROFILE_NET_HOST, &bytes, &messages);
	check(bytes == 3 && messages == 1, "a datagram shorter than a header is its own type, dropped as bad_size");
	received_row(rows, row_count, PROFILE_NET_BATCH_HEADER, _profile_net_drop_handled, 5, &bytes, &messages);
	check(bytes == 2 * PROFILE_NET_MESSAGE_HEADER_SIZE && messages == 2, "a batch counts its header, not its datagram");
	received_row(rows, row_count, PROFILE_NET_BATCH_HEADER, _profile_net_drop_bad_size, 5, &bytes, &messages);
	check(bytes == 100 && messages == 1, "a batch that did not fit is dropped as bad_size on its header row");
	received_row(rows, row_count, 6, _profile_net_drop_handled, 5, &bytes, &messages);
	check(bytes == 50 && messages == 2, "a batch's inner messages are counted by their own type");
	received_row(rows, row_count, 6, _profile_net_drop_stale, 5, &bytes, &messages);
	check(bytes == 20 && messages == 1, "an inner message keeps its drop reason");
	profile_net_recording_end();
}

/* the first row of a table with this direction, aux, machine and key; the sum of a value over the matching rows */
static struct profile_net_row const *net_row(struct profile_net_row const *rows, unsigned long count, int table,
	int direction, int aux, int machine, int key)
{
	unsigned long index;

	for (index = 0; index < count; index++)
	{
		if (rows[index].table == table && rows[index].direction == direction && rows[index].aux == aux &&
			rows[index].machine == machine && rows[index].key == key)
		{
			return &rows[index];
		}
	}
	return NULL;
}

static unsigned long net_rows_of(struct profile_net_row const *rows, unsigned long count, int table)
{
	unsigned long index, found = 0;

	for (index = 0; index < count; index++)
		found += rows[index].table == table;
	return found;
}

/* a connection is known to profile_net.c by its address alone */
static char connection_identity[4];
static long queue_bytes;
static int queue_reads[4];

static long fake_queue_reader(void const *connection)
{
	int index;

	for (index = 0; index < 4; index++)
	{
		if (connection == &connection_identity[index])
			queue_reads[index]++;
	}
	return queue_bytes;
}

/* the connection layer: traffic by connection, direction and channel, the
machine a connection is learnt to be, the reliable backlog read once a
second, and a closed connection never read again (its pointer is kept in
profile_net.c and the connection freed behind it) */
static void net_connection_checks(void)
{
	struct profile_net_row rows[256];
	struct profile_net_row const *row;
	struct profile_net_snapshot snapshot;
	struct profile_net_live before, after;
	unsigned long row_count;
	long first_interval, intervals;

	profile_net_set_queue_reader(fake_queue_reader);
	profile_net_live(&before);
	profile_net_recording_begin();
	profile_net_part_begin(rows, 256, 0, 0);
	profile_net_traffic(&connection_identity[0], _profile_net_out, _profile_net_datagram, 100, 0x7F000001, 2302);
	profile_net_traffic(&connection_identity[0], _profile_net_out, _profile_net_datagram, 50, 0x7F000001, 2302);
	profile_net_traffic(&connection_identity[0], _profile_net_in, _profile_net_stream, 30, 0x7F000001, 2302);
	/* the top of the address and port ranges */
	profile_net_traffic(&connection_identity[1], _profile_net_out, _profile_net_stream, 7, 0xFFFFFFFFUL, 65535);
	profile_net_connection_machine(&connection_identity[0], PROFILE_NET_SERVER_DATAGRAMS);
	profile_net_connection_machine(&connection_identity[1], 127);
	check(profile_net_connections == 2, "two connections are live");
	check(profile_net_connection_entry(0)->ipv4 == 0x0100007F && profile_net_connection_entry(0)->port == 2302 &&
		profile_net_connection_entry(0)->machine == PROFILE_NET_SERVER_DATAGRAMS,
		"a connection keeps its address and the host's datagram connection its own machine");
	check(profile_net_connection_entry(1)->ipv4 == 0xFFFFFFFFUL && profile_net_connection_entry(1)->port == 65535 &&
		profile_net_connection_entry(1)->machine == 127, "the top address, port and machine of the ranges");

	/* a connection that closed: not read again, and its memory's next owner is a connection of its own */
	profile_net_traffic(&connection_identity[2], _profile_net_out, _profile_net_stream, 9, 0x00000002, 1);
	check(profile_net_connections == 3, "a third connection is live");
	profile_net_connection_closed(&connection_identity[2]);
	check(profile_net_connections == 2, "a closed connection is not live");
	profile_net_connection_closed(&connection_identity[2]);
	check(profile_net_connections == 2, "a connection closes once");
	queue_bytes = 900;
	profile_net_sample_queues();
	queue_bytes = 300;
	profile_net_sample_queues();
	check(queue_reads[0] == 2 && queue_reads[1] == 2 && queue_reads[2] == 0, "the live connections are read each second, a closed one never");
	profile_net_traffic(&connection_identity[2], _profile_net_in, _profile_net_datagram, 11, 0x00000003, 2);
	check(profile_net_connection_entry(3) && profile_net_connection_entry(3)->ipv4 == 0x03000000,
		"memory reused by a new connection is a new row");
	profile_net_part_end(1000000000ULL, &row_count, &first_interval, &intervals, &snapshot);

	check(snapshot.connections == 4, "the connections at the cut: closed ones keep their rows");
	row = net_row(rows, row_count, _profile_net_table_traffic, _profile_net_out, _profile_net_datagram, -1, 0);
	check(row && row->values[0] == 150 && row->values[1] == 2, "datagram bytes out and their count by connection");
	row = net_row(rows, row_count, _profile_net_table_traffic, _profile_net_in, _profile_net_stream, -1, 0);
	check(row && row->values[0] == 30 && row->values[1] == 1, "stream bytes in by connection");
	row = net_row(rows, row_count, _profile_net_table_traffic, _profile_net_out, _profile_net_stream, -1, 1);
	check(row && row->values[0] == 7, "the connection at the top of the ranges has its row");
	row = net_row(rows, row_count, _profile_net_table_traffic, _profile_net_in, _profile_net_datagram, -1, 3);
	check(row && row->values[0] == 11, "a reused address counts under its new connection");
	row = net_row(rows, row_count, _profile_net_table_queues, _profile_net_out, 0, -1, 0);
	check(row && row->values[0] == 900, "the queue row keeps the interval's largest backlog");
	row = net_row(rows, row_count, _profile_net_table_queues, _profile_net_out, 0, -1, 2);
	check(!row, "a closed connection has no queue row");
	check(net_rows_of(rows, row_count, _profile_net_table_queues) == 2, "a queue row for each live connection");
	profile_net_recording_end();
	/* the overlay's totals count without a recording */
	profile_net_set_overlay(1);
	profile_net_traffic(&connection_identity[0], _profile_net_out, _profile_net_datagram, 5, 0, 0);
	profile_net_set_overlay(0);
	profile_net_live(&after);
	check(after.game_bytes[_profile_net_out] - before.game_bytes[_profile_net_out] == 100 + 50 + 7 + 9 + 5 &&
		after.game_packets[_profile_net_in] - before.game_packets[_profile_net_in] == 2,
		"the live totals are the connection layer's bytes and datagrams");
}

/* the tunnel's peers: a first sample is the baseline, the next ones are the
differences. A peer address with its top bit set is a negative key, and a
peer past the table's last is left out */
static void net_tunnel_checks(void)
{
	struct profile_net_tunnel_peer peers[MAXIMUM_PROFILE_NET_PEERS + 1];
	struct profile_net_row *rows = calloc(1024, sizeof(*rows));
	struct profile_net_row const *row;
	struct profile_net_snapshot snapshot;
	struct profile_net_live live;
	struct profile_net_counts counts;
	unsigned long row_count;
	long first_interval, intervals;
	int index, peer_count = MAXIMUM_PROFILE_NET_PEERS + 1;

	/* (the p2p layer's own limit, which p2p.c asserts the table holds) */
	check(MAXIMUM_PROFILE_NET_PEERS == HALO_PORT_MAXIMUM_NETWORK_MACHINES - 1 &&
		MAXIMUM_PROFILE_NET_MACHINES == HALO_PORT_MAXIMUM_NETWORK_MACHINES, "the tables hold the netcode's machines");
	memset(peers, 0, sizeof(peers));
	for (index = 0; index < peer_count; index++)
	{
		peers[index].virtual_address = 0xFEFF7F64UL - (unsigned long)index;
		peers[index].bytes_out = 1000;
		peers[index].bytes_in = 2000;
		peers[index].packets_out = 100;
		peers[index].packets_in = 200;
		peers[index].highest_in = 200;
		peers[index].kcp_payload = 300;
		peers[index].kcp_output = 400;
		peers[index].round_trip = 20;
	}
	profile_net_recording_begin();
	profile_net_part_begin(rows, 1024, 0, 0);
	profile_net_tunnel(peers, peer_count);
	for (index = 0; index < peer_count; index++)
	{
		peers[index].bytes_out += 100 * (unsigned long)(index + 1);
		peers[index].bytes_in += 10;
		peers[index].packets_out += 4;
		peers[index].packets_in += 10;
		peers[index].highest_in += 12;
		peers[index].kcp_payload += 50;
		peers[index].kcp_output += 90;
		peers[index].round_trip = 35;
	}
	profile_net_tunnel(peers, peer_count);
	profile_net_live(&live);
	profile_net_part_end(1000000000ULL, &row_count, &first_interval, &intervals, &snapshot);
	check(net_rows_of(rows, row_count, _profile_net_table_tunnel_bytes) == MAXIMUM_PROFILE_NET_PEERS,
		"a row for each peer of the table, none for the first sample, none past the last");
	row = net_row(rows, row_count, _profile_net_table_tunnel_bytes, _profile_net_out, 0, -1, (int)peers[MAXIMUM_PROFILE_NET_PEERS - 1].virtual_address);
	check(row && row->key != PROFILE_NET_KEY_NONE && row->key != PROFILE_NET_KEY_OTHER && row->key < 0 &&
		row->values[0] == 100 * MAXIMUM_PROFILE_NET_PEERS && row->values[1] == 10 && row->values[2] == 35,
		"the last peer's bytes out and in, and its round trip, under its negative address key");
	row = net_row(rows, row_count, _profile_net_table_tunnel_bytes, _profile_net_out, 0, -1, (int)peers[MAXIMUM_PROFILE_NET_PEERS].virtual_address);
	check(!row, "a peer past the table has no row");
	profile_net_counters(&counts);
	check(counts.peers_dropped == 2, "a peer sample past the table is counted, in each sample");
	row = net_row(rows, row_count, _profile_net_table_tunnel_packets, _profile_net_out, 0, -1, (int)peers[0].virtual_address);
	check(row && row->values[0] == 4 && row->values[1] == 10 && row->values[2] == 2,
		"packets out and in, and the two the advance of the highest number says were lost");
	row = net_row(rows, row_count, _profile_net_table_tunnel_kcp, _profile_net_out, 0, -1, (int)peers[0].virtual_address);
	check(row && row->values[0] == 50 && row->values[1] == 90, "KCP's payload and output");
	check(live.wire_bytes[_profile_net_in] == 10 * MAXIMUM_PROFILE_NET_PEERS && live.wire_packets[_profile_net_out] == 4 * MAXIMUM_PROFILE_NET_PEERS &&
		live.wire_round_trip_ms == 35, "the live wire totals are the peers' differences");
	check(live.loss_percent > 16.6 && live.loss_percent < 16.7, "the loss of the second is the lost over the lost and the received");
	profile_net_recording_end();

	/* a new recording's first sample is a baseline again: nothing of the gap
	since the last sample, in the rows or in the live totals */
	profile_net_live(&live);
	{
		struct profile_net_live gap;

		profile_net_recording_begin();
		memset(rows, 0, 1024 * sizeof(*rows));
		profile_net_part_begin(rows, 1024, 0, 0);
		for (index = 0; index < 2; index++)
		{
			peers[index].bytes_out += 5000;
			peers[index].packets_out += 50;
			peers[index].packets_in += 50;
			peers[index].highest_in += 50;
		}
		profile_net_tunnel(peers, 2);
		profile_net_live(&gap);
		check(gap.wire_bytes[_profile_net_out] == live.wire_bytes[_profile_net_out] &&
			gap.wire_packets[_profile_net_in] == live.wire_packets[_profile_net_in],
			"the gap between recordings is not in the live totals");
		profile_net_part_end(1000000000ULL, &row_count, &first_interval, &intervals, &snapshot);
		check(net_rows_of(rows, row_count, _profile_net_table_tunnel_bytes) == 0 &&
			net_rows_of(rows, row_count, _profile_net_table_tunnel_packets) == 0,
			"the first sample of a recording has no rows");
		profile_net_recording_end();
	}

	/* the table of peers is a recording's: peers of earlier recordings do not fill it */
	{
		unsigned long address = 0x0A000001UL;
		int recording;

		for (recording = 0; recording < 3; recording++)
		{
			memset(peers, 0, sizeof(peers));
			for (index = 0; index < 20; index++)
			{
				peers[index].virtual_address = address++;
				peers[index].bytes_out = 100;
				peers[index].packets_out = 10;
				peers[index].packets_in = 10;
				peers[index].highest_in = 10;
			}
			profile_net_recording_begin();
			memset(rows, 0, 1024 * sizeof(*rows));
			profile_net_part_begin(rows, 1024, 0, 0);
			profile_net_tunnel(peers, 20);
			for (index = 0; index < 20; index++)
			{
				peers[index].bytes_out += 7;
				peers[index].packets_out += 1;
			}
			profile_net_tunnel(peers, 20);
			profile_net_part_end(1000000000ULL, &row_count, &first_interval, &intervals, &snapshot);
			check(net_rows_of(rows, row_count, _profile_net_table_tunnel_bytes) == 20,
				"the peers of a later recording are tracked after 40 others came before");
			profile_net_recording_end();
		}
	}

	/* a peer made again since has its counts start over, in any of them: bytes
	down with packets up is not a difference to add */
	memset(peers, 0, sizeof(peers));
	peers[0].virtual_address = 0x0B000001UL;
	peers[0].bytes_out = 5000;
	peers[0].packets_out = 100;
	peers[0].packets_in = 100;
	peers[0].highest_in = 100;
	profile_net_recording_begin();
	memset(rows, 0, 1024 * sizeof(*rows));
	profile_net_part_begin(rows, 1024, 0, 0);
	profile_net_tunnel(peers, 1);
	profile_net_live(&live);
	peers[0].bytes_out = 100;
	peers[0].packets_out = 150;
	profile_net_tunnel(peers, 1);
	profile_net_part_end(1000000000ULL, &row_count, &first_interval, &intervals, &snapshot);
	check(net_rows_of(rows, row_count, _profile_net_table_tunnel_bytes) == 0,
		"a peer whose bytes went down has no row for the sample that restarts it");
	{
		struct profile_net_live after;

		profile_net_live(&after);
		check(after.wire_bytes[_profile_net_out] == live.wire_bytes[_profile_net_out],
			"a peer made again adds no wrapped difference to the live totals");
	}
	profile_net_recording_end();

	/* the peers of one long recording: far more than the table's size over
	time, a few at once, all tracked (a peer that is gone leaves the table) */
	{
		struct profile_net_tunnel_peer window[3];
		int sample, slot, tracked = 0;
		unsigned long address;

		profile_net_recording_begin();
		memset(rows, 0, 1024 * sizeof(*rows));
		profile_net_part_begin(rows, 1024, 0, 0);
		for (sample = 0; sample < 40; sample++)
		{
			memset(window, 0, sizeof(window));
			for (slot = 0; slot < 3; slot++)
			{
				window[slot].virtual_address = 0x0C000000UL + (unsigned long)(sample + slot);
				window[slot].bytes_out = 1000 + 10 * (unsigned long)sample;
				window[slot].packets_out = 100 + (unsigned long)sample;
				window[slot].packets_in = 100 + (unsigned long)sample;
				window[slot].highest_in = 100 + (unsigned long)sample;
			}
			profile_net_tunnel(window, 3);
		}
		profile_net_part_end(1000000000ULL, &row_count, &first_interval, &intervals, &snapshot);
		for (address = 0x0C000000UL; address < 0x0C000000UL + 42; address++)
			tracked += net_row(rows, row_count, _profile_net_table_tunnel_bytes, _profile_net_out, 0, -1, (int)address) != NULL;
		check(tracked == 40, "every peer that was there for two samples is tracked, 42 over time in a table of 32");
		profile_net_recording_end();
	}

	/* a peer that disappears and comes back is new: a baseline, no row */
	{
		struct profile_net_tunnel_peer peer, other;

		memset(&peer, 0, sizeof(peer));
		other = peer;
		peer.virtual_address = 0x0D000001UL;
		other.virtual_address = 0x0D000002UL;
		profile_net_recording_begin();
		memset(rows, 0, 1024 * sizeof(*rows));
		profile_net_part_begin(rows, 1024, 0, 0);
		peer.bytes_out = 100;
		peer.packets_out = 10;
		profile_net_tunnel(&peer, 1);
		profile_net_tunnel(&other, 1);
		peer.bytes_out = 900;
		peer.packets_out = 90;
		profile_net_tunnel(&peer, 1);
		profile_net_part_end(1000000000ULL, &row_count, &first_interval, &intervals, &snapshot);
		check(net_rows_of(rows, row_count, _profile_net_table_tunnel_bytes) == 0,
			"a peer that was gone and is back has a baseline, not the difference over its absence");
		profile_net_recording_end();
	}
	free(rows);
}

/* the pings: the last of the second and how many, by machine, the host's
under its own value; a negative one (not known) is not counted */
static void net_ping_checks(void)
{
	struct profile_net_row rows[64];
	struct profile_net_row const *row;
	struct profile_net_snapshot snapshot;
	struct profile_net_live live;
	unsigned long row_count;
	long first_interval, intervals;

	profile_net_live(&live);
	profile_net_recording_begin();
	profile_net_part_begin(rows, 64, 0, 0);
	profile_net_ping(127, 40);
	profile_net_ping(127, 60);
	profile_net_ping(PROFILE_NET_HOST, 25);
	profile_net_ping(5, -1);
	profile_net_part_end(1000000000ULL, &row_count, &first_interval, &intervals, &snapshot);
	row = net_row(rows, row_count, _profile_net_table_pings, _profile_net_in, 0, 127, -1);
	check(row && row->values[0] == 60 && row->values[1] == 2, "the top machine's last ping and how many were taken");
	row = net_row(rows, row_count, _profile_net_table_pings, _profile_net_in, 0, PROFILE_NET_HOST, -1);
	check(row && row->values[0] == 25 && row->values[1] == 1, "the host's ping has its own machine");
	check(!net_row(rows, row_count, _profile_net_table_pings, _profile_net_in, 0, 5, -1), "an unknown ping is not a row");
	profile_net_live(&live);
	check(live.ping_ms == (40 + 60 + 25) / 3, "the overlay's ping is the mean of the second's");
	profile_net_recording_end();
}

/* the overlay's round trip is the mean of the peers sampled, and its ping the
last second's, whether or not the overlay was reading in between */
static void net_overlay_number_checks(void)
{
	struct profile_net_tunnel_peer peers[3];
	struct profile_net_live live;
	int index;

	memset(peers, 0, sizeof(peers));
	for (index = 0; index < 3; index++)
	{
		peers[index].virtual_address = 0x64400001UL + (unsigned long)index;
		peers[index].round_trip = 10UL + 20UL * (unsigned long)index * (unsigned long)index;
	}
	profile_net_live(&live);
	profile_net_recording_begin();
	profile_net_tunnel(peers, 3);
	for (index = 0; index < 3; index++)
	{
		peers[index].packets_in += 5;
		peers[index].bytes_in += 50;
	}
	profile_net_tunnel(peers, 3);
	profile_net_live(&live);
	check(live.wire_round_trip_ms == (10 + 30 + 90) / 3, "the wire round trip is the mean of the peers, not the last one's");

	profile_net_ping(1, 100);
	profile_net_tunnel(peers, 0);
	profile_net_ping(1, 200);
	profile_net_tunnel(peers, 0);
	profile_net_ping(1, 60);
	profile_net_live(&live);
	check(live.ping_ms == 60, "the pings of earlier seconds are not in the overlay's mean, read or not");
	profile_net_recording_end();
}

/* the thread drops until the game thread says stop, and says when it began,
so that the two overlap */
struct loss_thread
{
	long count;
	int started;
	int stop;
};

static void *loss_thread_main(void *argument)
{
	struct loss_thread *thread = argument;

	__atomic_store_n(&thread->started, 1, __ATOMIC_SEQ_CST);
	while (!__atomic_load_n(&thread->stop, __ATOMIC_SEQ_CST))
	{
		profile_net_simulated_loss();
		thread->count++;
	}
	return NULL;
}

/* debug.network_loss's datagrams are dropped on the thread that reads the
socket, which may not be the game's: counted atomically, in the interval
the game thread closes next. The recording flag it tests is flipped by the
game thread meanwhile */
static void net_simulated_loss_checks(void)
{
	struct profile_net_row *rows = calloc(8192, sizeof(*rows));
	struct profile_net_snapshot snapshot;
	struct loss_thread loss = { 0, 0, 0 };
	struct profile_net_live live;
	unsigned long row_count;
	long first_interval, intervals;
	unsigned long long now = 0;
	pthread_t thread;
	unsigned long counted;
	int round;

	/* the flag's own flips (nothing is asserted of what is counted) */
	profile_net_recording_begin();
	profile_net_part_begin(rows, 8192, now, 0);
	pthread_create(&thread, NULL, loss_thread_main, &loss);
	while (!__atomic_load_n(&loss.started, __ATOMIC_SEQ_CST))
		;
	for (round = 0; round < 2000; round++)
	{
		profile_net_recording_end();
		profile_net_recording_begin();
		profile_net_part_begin(rows, 8192, now, 0);
	}
	__atomic_store_n(&loss.stop, 1, __ATOMIC_SEQ_CST);
	pthread_join(thread, NULL);
	profile_net_part_end(now, &row_count, &first_interval, &intervals, &snapshot);
	profile_net_recording_end();

	/* in a recording that stays on, the game thread sampling and closing intervals meanwhile: nothing is lost */
	memset(rows, 0, 8192 * sizeof(*rows));
	profile_net_recording_begin();
	profile_net_part_begin(rows, 8192, now, 0);
	loss.count = 0;
	loss.started = 0;
	loss.stop = 0;
	pthread_create(&thread, NULL, loss_thread_main, &loss);
	while (!__atomic_load_n(&loss.started, __ATOMIC_SEQ_CST))
		;
	for (round = 0; round < 40; round++)
	{
		now += 1100000000ULL;
		profile_net_ping(1, round);
		profile_net_sample_queues();
		profile_net_live(&live);
		profile_net_frame(now, round);
	}
	__atomic_store_n(&loss.stop, 1, __ATOMIC_SEQ_CST);
	pthread_join(thread, NULL);
	profile_net_part_end(now + 1100000000ULL, &row_count, &first_interval, &intervals, &snapshot);
	counted = net_table_sum(rows, row_count, _profile_net_table_simulated_loss, 0);
	check(counted == (unsigned long)loss.count, "every datagram a second thread drops is counted once, across the intervals closed meanwhile");
	profile_net_recording_end();
	profile_net_simulated_loss();
	profile_net_recording_begin();
	profile_net_part_begin(rows, 8192, now, 0);
	profile_net_part_end(now, &row_count, &first_interval, &intervals, &snapshot);
	check(net_table_sum(rows, row_count, _profile_net_table_simulated_loss, 0) == 0, "a drop outside a recording is not counted into the next");
	profile_net_recording_end();
	free(rows);
}

static void *traffic_thread_main(void *argument)
{
	struct loss_thread *thread = argument;

	__atomic_store_n(&thread->started, 1, __ATOMIC_SEQ_CST);
	while (!__atomic_load_n(&thread->stop, __ATOMIC_SEQ_CST))
	{
		/* (not the game's thread: counted at most, and only while recording) */
		profile_net_traffic(&connection_identity[0], _profile_net_out, _profile_net_datagram, 1, 0, 0);
		thread->count++;
	}
	return NULL;
}

/* the connection layer's hooks may be reached from another thread: they
read the counting flag the game thread turns on and off */
static void net_counting_checks(void)
{
	struct loss_thread traffic = { 0, 0, 0 };
	pthread_t thread;
	int round;

	pthread_create(&thread, NULL, traffic_thread_main, &traffic);
	while (!__atomic_load_n(&traffic.started, __ATOMIC_SEQ_CST))
		;
	for (round = 0; round < 5000; round++)
	{
		profile_net_set_overlay(1);
		profile_net_set_overlay(0);
	}
	__atomic_store_n(&traffic.stop, 1, __ATOMIC_SEQ_CST);
	pthread_join(thread, NULL);
	check(traffic.count > 0, "the other thread ran while the flag was toggled");
}

/* profile_overlay's argument: nothing toggles, on and off set, anything else is wrong
(a prefix such as "onx" is not "on") */
static void overlay_switch_checks(void)
{
	check(profile_overlay_switch("") == _profile_overlay_switch_toggle && profile_overlay_switch(")") == _profile_overlay_switch_toggle &&
		profile_overlay_switch("  )") == _profile_overlay_switch_toggle, "no word toggles the overlay, in or out of parentheses");
	check(profile_overlay_switch("on") == _profile_overlay_switch_on && profile_overlay_switch("off") == _profile_overlay_switch_off,
		"on and off set it");
	check(profile_overlay_switch("ON") == _profile_overlay_switch_on && profile_overlay_switch("Off )") == _profile_overlay_switch_off &&
		profile_overlay_switch("on)") == _profile_overlay_switch_on && profile_overlay_switch("on \t") == _profile_overlay_switch_on,
		"in either case, before a closing parenthesis, with spaces after");
	check(profile_overlay_switch("onx") == _profile_overlay_switch_invalid && profile_overlay_switch("offline") == _profile_overlay_switch_invalid &&
		profile_overlay_switch("o") == _profile_overlay_switch_invalid && profile_overlay_switch("on off") == _profile_overlay_switch_invalid &&
		profile_overlay_switch("1") == _profile_overlay_switch_invalid, "any other word is not an answer");
	/* the last member of the loss state: nothing is left of it that nothing uses */
	check(offsetof(struct profile_net_loss, reported) + sizeof(long long) == sizeof(struct profile_net_loss), "the loss state has no spare member");
}

static void overlay_checks(void)
{
	static const double first_ticks[] = { 3.0 };
	static const double second_ticks[] = { 2.0, 5.0 };
	struct profile_overlay_lines overlay;
	struct profile_trace_status status;
	struct profile_net_live live, quiet;

	profile_overlay_lines_reset(&overlay);
	check(strcmp(overlay.lines[0], "profile idle") == 0, "the overlay starts idle");
	memset(&status, 0, sizeof(status));
	memset(&live, 0, sizeof(live));
	profile_overlay_lines_second(&overlay, &status, "local", 0, &live, 1.0);
	check(strcmp(overlay.lines[1], "local") == 0, "a local game says so");
	check(strcmp(overlay.lines[2], "frame 0.0 ms avg 0.0 max   tick --") == 0, "no tick: --");
	check(strcmp(overlay.lines[3], "tick slow: --") == 0, "no tick: no slow sections");
	check(strcmp(overlay.lines[5], "up 0.0 KB/s  down 0.0 KB/s") == 0, "no tunnel: no wire");
	check(strcmp(overlay.lines[6], "ping 0 ms  loss 0.0%  packets 0 up 0 down") == 0, "no tunnel: no loss, no wire ping");

	/* a busy second */
	profile_overlay_lines_frame(&overlay, 8.0, 1, first_ticks, 1.0);
	profile_overlay_lines_frame(&overlay, 20.0, 2, second_ticks, 2.0);
	profile_overlay_lines_section(&overlay, "game_tick.objects", 4.2);
	profile_overlay_lines_section(&overlay, "game_tick.ai", 2.7);
	profile_overlay_lines_section(&overlay, "network_distributed_tick", 1.8);
	profile_overlay_lines_section(&overlay, "render", 9.0);
	status.state = _profile_trace_recording;
	status.seconds = 724.5;
	status.part = 3;
	status.memory_percent = 41;
	live.game_bytes[0] = 41200;
	live.game_bytes[1] = 6100;
	live.wire_bytes[0] = 52000;
	live.wire_bytes[1] = 9000;
	live.game_packets[0] = 92;
	live.game_packets[1] = 31;
	live.wire_packets[0] = 95;
	live.wire_packets[1] = 40;
	live.ping_ms = 95;
	live.wire_round_trip_ms = 61;
	live.loss_percent = 0.4;
	quiet = live;
	profile_overlay_lines_second(&overlay, &status, "host", 3, &live, 1.0);
	check(strcmp(overlay.lines[0], "REC 12:04 part 3  mem 41%") == 0, "recording: time, part and memory");
	check(strcmp(overlay.lines[1], "host  3 clients") == 0, "a host: its clients");
	check(strcmp(overlay.lines[2], "frame 14.0 ms avg 20.0 max   tick 4.3 ms avg 6.0 max") == 0,
		"frame and tick, mean and most");
	check(strcmp(overlay.lines[3], "tick slow: objects 1.4  ai 0.9  network_distributed 0.6") == 0,
		"the three slowest sections per tick");
	check(overlay.lines[4][0] == 0, "short slow sections stay on one line");
	check(strcmp(overlay.lines[5], "up 41.2 KB/s (wire 52.0)  down 6.1 KB/s (wire 9.0)") == 0, "up and down, game and wire");
	check(strcmp(overlay.lines[6], "ping 95 ms (wire 61)  loss 0.4%  packets 92 up 31 down") == 0,
		"ping, loss and packets");
	for (int line = 0; line < PROFILE_OVERLAY_LINE_COUNT; line++)
		check(strlen(overlay.lines[line]) <= PROFILE_OVERLAY_LINE_LIMIT,
			"every overlay line stays within the character cap");

	/* a second with no tunnel packets: the wire parts go, the loss with them */
	profile_overlay_lines_frame(&overlay, 10.0, 1, first_ticks, 0.0);
	profile_overlay_lines_second(&overlay, &status, "host", 3, &quiet, 1.0);
	check(strcmp(overlay.lines[5], "up 0.0 KB/s  down 0.0 KB/s") == 0, "no tunnel packets this second: no wire parts");
	check(strcmp(overlay.lines[6], "ping 95 ms  loss 0.0%  packets 0 up 0 down") == 0,
		"no tunnel packets this second: loss 0.0");
	check(strcmp(overlay.lines[3], "tick slow: --") == 0, "a tick without sections: no slow sections");
	check(strcmp(overlay.lines[2], "frame 10.0 ms avg 10.0 max   tick 3.0 ms avg 3.0 max") == 0, "a tick without the network's time");

	/* ranked by time per tick, whatever order the sections came in; a fourth is left out */
	profile_overlay_lines_frame(&overlay, 5.0, 2, second_ticks, 0.0);
	profile_overlay_lines_section(&overlay, "game_tick.a", 1.0);
	profile_overlay_lines_section(&overlay, "game_tick.b", 6.0);
	profile_overlay_lines_section(&overlay, "game_tick.c", 2.0);
	profile_overlay_lines_section(&overlay, "game_tick.d", 4.0);
	profile_overlay_lines_section(&overlay, "game_tick.b", 2.0);
	profile_overlay_lines_second(&overlay, &status, "host", 3, &quiet, 1.0);
	check(strcmp(overlay.lines[3], "tick slow: b 4.0  d 2.0  c 1.0") == 0,
		"slow sections: ranked, summed over frames, three of them");

	/* long names wrap between sections, leaving both lines tracker-safe */
	profile_overlay_lines_frame(&overlay, 5.0, 1, first_ticks, 0.0);
	profile_overlay_lines_section(&overlay, "game_tick.very_long_section_alpha", 9.0);
	profile_overlay_lines_section(&overlay, "game_tick.very_long_section_bravo", 6.0);
	profile_overlay_lines_section(&overlay, "game_tick.very_long_section_charlie", 3.0);
	profile_overlay_lines_second(&overlay, &status, "host", 3, &quiet, 1.0);
	check(strlen(overlay.lines[3]) <= PROFILE_OVERLAY_SLOW_LINE_LIMIT &&
		strlen(overlay.lines[4]) <= PROFILE_OVERLAY_SLOW_LINE_LIMIT && overlay.lines[4][0],
		"three long slow names wrap without exceeding the character cap");
	check(strstr(overlay.lines[3], "very_long_section_alpha") &&
		strstr(overlay.lines[4], "very_long_section_bravo") &&
		!strstr(overlay.lines[4], "very_long_section_charlie"),
		"overflow moves the second entry and stops before a third that cannot fit");
	profile_overlay_lines_frame(&overlay, 5.0, 1, first_ticks, 0.0);
	profile_overlay_lines_section(&overlay, "game_tick.short", 1.0);
	profile_overlay_lines_second(&overlay, &status, "host", 3, &quiet, 1.0);
	check(!overlay.lines[4][0], "a new second clears the previous continuation");

	/* exact cap stays intact; the next character starts a new line */
	profile_overlay_lines_reset(&overlay);
	profile_overlay_lines_frame(&overlay, 5.0, 1, first_ticks, 0.0);
	profile_overlay_lines_section(&overlay, "game_tick.abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNO", 1.0);
	profile_overlay_lines_section(&overlay, "game_tick.b", 0.9);
	profile_overlay_lines_section(&overlay, "game_tick.c", 0.8);
	profile_overlay_lines_second(&overlay, &status, "host", 3, &quiet, 1.0);
	check(strlen(overlay.lines[3]) == PROFILE_OVERLAY_LINE_LIMIT && strcmp(overlay.lines[4], "b 0.9  c 0.8") == 0,
		"an exact-cap line stays intact and continuation entries keep their separator");
	profile_overlay_lines_frame(&overlay, 5.0, 1, first_ticks, 0.0);
	profile_overlay_lines_section(&overlay, "game_tick.abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOP", 1.0);
	profile_overlay_lines_second(&overlay, &status, "host", 3, &quiet, 1.0);
	check(strlen(overlay.lines[3]) <= PROFILE_OVERLAY_LINE_LIMIT && overlay.lines[4][0],
		"a slow line one character over the cap wraps");
	{
		char text[PROFILE_OVERLAY_LINE_COUNT * (PROFILE_OVERLAY_LINE_SIZE + 2) + 1];
		int breaks = 0;
		overlay.lines[PROFILE_OVERLAY_CONTINUATION_LINE][0] = 0;
		profile_overlay_lines_text(&overlay, text);
		for (char *at = text; (at = strstr(at, "|n")) != NULL; at += 2)
			breaks++;
		check(breaks == PROFILE_OVERLAY_LINE_COUNT - 1,
			"overlay text excludes the empty continuation line");
	}

	status.state = _profile_trace_finishing;
	strcpy(status.name, "profile_20261005-142233_host");
	profile_overlay_lines_second(&overlay, &status, "client", 0, &live, 1.0);
	check(strcmp(overlay.lines[0], "profile writing profile_20261005-142233_host") == 0, "writing: the name");
	check(strcmp(overlay.lines[1], "client") == 0, "a client says so");
	check(strcmp(overlay.lines[2], "frame 0.0 ms avg 0.0 max   tick --") == 0, "a new second starts empty");
	status.state = _profile_trace_armed;
	profile_overlay_lines_second(&overlay, &status, "local", 0, &live, 1.0);
	check(strcmp(overlay.lines[0], "profile armed") == 0, "armed: waiting for a game");
}

static int refusing_writer_start(void)
{
	return 0;
}

/* a start that fails after it has allocated: nothing is kept, it says so, and
a recording of "game" mode is not armed again to fail at every game */
static void release_path_checks(void)
{
	static const char *const what[] = { "the second arena refused", "the writer thread cannot start" };
	int fail, game;

	for (fail = 0; fail < 2; fail++)
	{
		for (game = 0; game < 2; game++)
		{
			struct profile_trace_status status;
			char text[96];

			snprintf(text, sizeof(text), "%s, %s mode", what[fail], game ? "game" : "now");
			forget_written();
			fake_ready = 1;
			last_notice[0] = 0;
			if (fail == 0)
				allocations_allowed = 1;
			else
				profile_trace_set_writer(refusing_writer_start);
			profile_trace_request_start(0.0, game ? _profile_trace_when_game : _profile_trace_when_now, 4);
			frame(1);
			allocations_allowed = -1;
			profile_trace_set_writer(fake_writer_start);
			profile_trace_status(&status);
			check(outstanding() == 0, text);
			check(status.state == _profile_trace_idle, text);
			check(strstr(last_notice, fail == 0 ? "no memory for 4 MB" : "the writer thread cannot start") != NULL, text);
			check(!profile_net_recording && !profile_trace_recording(), text);
			frame(1);
			profile_trace_status(&status);
			check(status.state == _profile_trace_idle && outstanding() == 0, text);
		}
	}
}

/* a worst frame's ticks and longest scopes come from any depth of the frame:
game_tick sits under game_time_update, and the time a scope has of its own is
what ranks it (its parents would always come first otherwise) */
static void worst_frame_checks(void)
{
	struct profile_trace_record game[16];
	struct profile_part *part = calloc(1, sizeof(*part));
	int frame_id = profile_trace_name("frame");
	int update = profile_trace_name("game_time_update");
	int tick = profile_trace_name("game_tick");
	int objects = profile_trace_name("game_tick.objects");
	int ai = profile_trace_name("game_tick.ai");
	int render = profile_trace_name("render");
	int bsp = profile_trace_name("render.bsp");
	int texture = profile_trace_name("texture");
	char text[65536];
	unsigned long count = 0, length;
	FILE *file = tmpfile();
	const char *row;

	/* (milliseconds, as nanoseconds below) */
#define WORST_RECORD(start_ms, duration_ms, id, level) \
	do { game[count].start = (start_ms) * 1000000ULL; game[count].duration = (duration_ms) * 1000000U; \
		game[count].name = (unsigned short)(id); game[count].depth = (level); game[count].track = 0; count++; } while (0)
	WORST_RECORD(0, 100, frame_id, 0);
	WORST_RECORD(0, 60, update, 1);
	WORST_RECORD(0, 30, tick, 2);
	WORST_RECORD(0, 20, objects, 3);
	WORST_RECORD(30, 25, tick, 2);
	WORST_RECORD(30, 22, ai, 3);
	WORST_RECORD(60, 35, render, 1);
	WORST_RECORD(60, 10, bsp, 2);
	/* (an aggregate's time is a counter record, not a scope) */
	WORST_RECORD(5, 99, texture, PROFILE_TRACE_DEPTH_AGGREGATE_TIME);
	WORST_RECORD(100, 5, frame_id, 0);
#undef WORST_RECORD
	part->records[_profile_track_game] = game;
	part->record_counts[_profile_track_game] = count;
	part->name_count = profile_trace_name("worst_frame_last") + 1;
	part->ticks = 2;
	check(file != NULL, "a scratch file for the worst frames");
	if (!file)
	{
		free(part);
		return;
	}
	check(profile_json_write(part, file, NULL), "a part with nested scopes is written");
	rewind(file);
	length = fread(text, 1, sizeof(text) - 1, file);
	text[length] = 0;
	fclose(file);
	row = strstr(text, "\"worst_frames\"");
	check(row && strstr(row, "[0, 0.000, 100.000, 2, [[\"render\", 25.000], [\"game_tick.ai\", 22.000], "
		"[\"game_tick.objects\", 20.000]]]"),
		"the worst frame runs its two ticks under game_time_update and names the scopes with the most time of their own");
	check(row && strstr(row, "[1, 0.100, 5.000, 0, []]"), "a frame with no scopes in it has no ticks and no longest");
	free(part);
}

/* the writer's passes over a part's rows: the tunnel table and the second
counters must stay what the plain scans (kept here, as they were written) make
of the same rows, and a part of a million rows is written in seconds, not
minutes */
static void reference_address(FILE *file, unsigned long ipv4)
{
	fprintf(file, "\"%lu.%lu.%lu.%lu\"", ipv4 & 255, (ipv4 >> 8) & 255, (ipv4 >> 16) & 255, (ipv4 >> 24) & 255);
}

static void reference_tunnel(const struct profile_part *part, FILE *file)
{
	unsigned long index, other, items = 0;

	fputs(",\n\"tunnel\": {\"columns\": [\"interval\", \"peer\", \"bytes_out\", \"bytes_in\", \"packets_out\", "
		"\"packets_in\", \"lost_in\", \"kcp_payload\", \"kcp_output\", \"round_trip_ms\"], \"rows\": [\n", file);
	for (index = 0; index < part->row_count; index++)
	{
		const struct profile_net_row *row = &part->rows[index];
		unsigned int packets[3] = { 0, 0, 0 };
		unsigned int kcp[2] = { 0, 0 };

		if (row->table != _profile_net_table_tunnel_bytes)
			continue;
		for (other = index + 1; other < part->row_count; other++)
		{
			const struct profile_net_row *match = &part->rows[other];

			if (match->interval != row->interval || match->key != row->key)
				continue;
			if (match->table == _profile_net_table_tunnel_packets)
				memcpy(packets, match->values, sizeof(packets));
			else if (match->table == _profile_net_table_tunnel_kcp)
				memcpy(kcp, match->values, sizeof(kcp));
		}
		if (items++)
			fputs(",\n", file);
		fprintf(file, "[%u, ", row->interval);
		reference_address(file, (unsigned long)(unsigned int)row->key);
		fprintf(file, ", %u, %u, %u, %u, %u, %u, %u, %u]", row->values[0], row->values[1], packets[0], packets[1],
			packets[2], kcp[0], kcp[1], row->values[2]);
	}
	fputs(items ? "\n]}" : "]}", file);
}

static void reference_counters(const struct profile_part *part, FILE *file)
{
	unsigned long index, other, items = 0;

	for (index = 0; index < part->row_count; index++)
	{
		const struct profile_net_row *interval = &part->rows[index];
		double seconds = interval->values[1] / 1000.0;
		double game[2] = { 0.0, 0.0 }, wire[2] = { 0.0, 0.0 };
		double ping = 0.0, lost = 0.0, received = 0.0;
		unsigned long long start = (unsigned long long)interval->values[0] * 1000000ULL;
		int pings = 0;

		if (interval->table != _profile_net_table_intervals || seconds <= 0.0)
			continue;
		for (other = 0; other < part->row_count; other++)
		{
			const struct profile_net_row *row = &part->rows[other];

			if (row->interval != (unsigned int)interval->key)
				continue;
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
		fprintf(file, "%s{\"ph\":\"C\",\"name\":\"game_Bps\",\"pid\":1,\"tid\":1,\"ts\":%llu.%03llu,\"args\":{\"up\":%.0f,\"down\":%.0f}}",
			items++ ? ",\n" : "", start / 1000ULL, start % 1000ULL, game[0] / seconds, game[1] / seconds);
		fprintf(file, ",\n{\"ph\":\"C\",\"name\":\"wire_Bps\",\"pid\":1,\"tid\":1,\"ts\":%llu.%03llu,\"args\":{\"up\":%.0f,\"down\":%.0f}}",
			start / 1000ULL, start % 1000ULL, wire[0] / seconds, wire[1] / seconds);
		fprintf(file, ",\n{\"ph\":\"C\",\"name\":\"ping_ms\",\"pid\":1,\"tid\":1,\"ts\":%llu.%03llu,\"args\":{\"value\":%.0f}}",
			start / 1000ULL, start % 1000ULL, pings ? ping / pings : 0.0);
		fprintf(file, ",\n{\"ph\":\"C\",\"name\":\"loss_pct\",\"pid\":1,\"tid\":1,\"ts\":%llu.%03llu,\"args\":{\"value\":%.2f}}",
			start / 1000ULL, start % 1000ULL, received + lost > 0.0 ? 100.0 * lost / (received + lost) : 0.0);
	}
}

static struct profile_net_row *writer_row(struct profile_net_row *rows, unsigned long *count, unsigned int interval,
	int table, int key, unsigned int first, unsigned int second, unsigned int third)
{
	struct profile_net_row *row = &rows[(*count)++];

	memset(row, 0, sizeof(*row));
	row->interval = interval;
	row->table = (unsigned char)table;
	row->direction = (unsigned char)(key & 1);
	row->site = PROFILE_NET_NO_SITE;
	row->key = key;
	row->values[0] = first;
	row->values[1] = second;
	row->values[2] = third;
	return row;
}

/* intervals of peers' tunnel rows among the other tables' rows, as a cut leaves them */
static unsigned long writer_rows(struct profile_net_row *rows, int intervals, int peers, int filler)
{
	unsigned long count = 0;
	int interval, peer, extra;

	for (interval = 0; interval < intervals; interval++)
	{
		/* (rows of a table the writer does not print: they cost the scans only) */
		for (extra = 0; extra < filler; extra++)
			writer_row(rows, &count, (unsigned int)interval, 200, extra, 1, 1, 1);
		for (peer = 0; peer < peers; peer++)
		{
			int key = (peer & 1) ? (int)(0xFEFF0000UL + (unsigned long)peer) : 0x64400000 + peer;
			unsigned int value = (unsigned int)(interval * 1000 + peer);

			writer_row(rows, &count, (unsigned int)interval, _profile_net_table_traffic, key, value + 1, 3, 0);
			writer_row(rows, &count, (unsigned int)interval, _profile_net_table_tunnel_bytes, key, value + 2, value + 3, 20 + peer);
			/* (the kcp row first in some intervals) */
			if (interval % 3 == 1)
			{
				writer_row(rows, &count, (unsigned int)interval, _profile_net_table_tunnel_kcp, key, value + 6, value + 7, 0);
				writer_row(rows, &count, (unsigned int)interval, _profile_net_table_tunnel_packets, key, value + 4, value + 5, peer);
			}
			else
			{
				writer_row(rows, &count, (unsigned int)interval, _profile_net_table_tunnel_packets, key, value + 4, value + 5, peer);
				writer_row(rows, &count, (unsigned int)interval, _profile_net_table_tunnel_kcp, key, value + 6, value + 7, 0);
			}
			if (peer % 5 == 2)
				writer_row(rows, &count, (unsigned int)interval, _profile_net_table_pings, key, 40 + peer, 1, 0);
			/* (a later sample of the interval's peer: the last row counts) */
			if (interval == 5 && peer == 3)
				writer_row(rows, &count, (unsigned int)interval, _profile_net_table_tunnel_packets, key, 9, 8, 7);
			/* (a packets row before its bytes row does not count for it) */
			if (interval == 6 && peer == 4)
			{
				writer_row(rows, &count, (unsigned int)interval, _profile_net_table_tunnel_packets, key + 1, 99, 98, 97);
				writer_row(rows, &count, (unsigned int)interval, _profile_net_table_tunnel_bytes, key + 1, 5, 6, 7);
			}
		}
		/* (the interval's own row last, as the close leaves it; interval 4 has no length) */
		writer_row(rows, &count, (unsigned int)interval, _profile_net_table_intervals, interval, (unsigned int)interval * 1000,
			interval == 4 ? 0 : 1000, (unsigned int)interval * 30)->machine = interval * 30;
	}
	return count;
}

static char *writer_text(struct profile_part *part, unsigned long *length)
{
	FILE *file = tmpfile();
	char *text = NULL;

	if (!file)
		return NULL;
	check(profile_json_write(part, file, NULL), "a part of many tunnel rows is written");
	*length = (unsigned long)ftell(file);
	rewind(file);
	text = malloc(*length + 1);
	if (text && fread(text, 1, *length, file) == *length)
		text[*length] = 0;
	else
	{
		free(text);
		text = NULL;
	}
	fclose(file);
	return text;
}

/* what a reference function wrote, whole */
static char *writer_reference(const struct profile_part *part, int counters, unsigned long *length)
{
	FILE *file = tmpfile();
	char *text = NULL;

	if (!file)
		return NULL;
	if (counters)
		reference_counters(part, file);
	else
		reference_tunnel(part, file);
	*length = (unsigned long)ftell(file);
	rewind(file);
	text = malloc(*length + 1);
	if (text && fread(text, 1, *length, file) == *length)
		text[*length] = 0;
	else
	{
		free(text);
		text = NULL;
	}
	fclose(file);
	return text;
}

/* the writer's text for a part against the plain scans' */
static void writer_same_as_scans(struct profile_part *part, const char *what)
{
	unsigned long length = 0, expected_length = 0;
	char *text = writer_text(part, &length);
	char *wanted = writer_reference(part, 0, &expected_length);
	char *found = text ? strstr(text, ",\n\"tunnel\": {") : NULL;
	char *end = found ? strstr(found, ",\n\"pings\": {") : NULL;

	check(text && wanted && found && end && (unsigned long)(end - found) == expected_length &&
		memcmp(found, wanted, expected_length) == 0, what);
	free(wanted);
	wanted = writer_reference(part, 1, &expected_length);
	found = text ? strstr(text, "{\"ph\":\"C\",\"name\":\"game_Bps\"") : NULL;
	end = found ? strstr(found, "\n],\n\"displayTimeUnit\"") : NULL;
	check(text && wanted && found && end && expected_length > 100 && (unsigned long)(end - found) == expected_length &&
		memcmp(found, wanted, expected_length) == 0, what);
	free(wanted);
	free(text);
}

/* the seconds a part takes to write */
static double writer_seconds(struct profile_part *part)
{
	struct timespec before, after;
	unsigned long length = 0;
	char *text;

	clock_gettime(CLOCK_MONOTONIC, &before);
	text = writer_text(part, &length);
	clock_gettime(CLOCK_MONOTONIC, &after);
	free(text);
	return (double)(after.tv_sec - before.tv_sec) + (double)(after.tv_nsec - before.tv_nsec) / 1e9;
}

static void writer_pass_checks(void)
{
	struct profile_part *part = calloc(1, sizeof(*part));
	struct profile_net_row *rows = calloc(1500000, sizeof(*rows));
	double seconds;

	if (!part || !rows)
	{
		check(0, "memory for the writer's passes");
		free(part);
		free(rows);
		return;
	}
	part->rows = rows;
	part->first_interval = 0;

	part->intervals = 30;
	part->row_count = writer_rows(rows, 30, 40, 50);
	writer_same_as_scans(part, "30 intervals of 40 peers are what the plain scans make of the rows");

	/* an interval of as many rows as the recording's index lets one have */
	memset(rows, 0, 1500000 * sizeof(*rows));
	part->intervals = 1;
	part->row_count = writer_rows(rows, 1, PROFILE_NET_MAXIMUM_INTERVAL_ROWS / 2, 0);
	check(part->row_count > PROFILE_NET_MAXIMUM_INTERVAL_ROWS, "an interval of more rows than the recording lets an interval have");
	writer_same_as_scans(part, "an interval of the most tunnel rows is what the plain scans make of it");

	/* thousands of intervals, a few rows each: a pass per interval over every row is quadratic in them */
	memset(rows, 0, 1500000 * sizeof(*rows));
	part->intervals = 40000;
	part->row_count = writer_rows(rows, 40000, 2, 0);
	seconds = writer_seconds(part);
	check(seconds < 2.0, "40000 intervals are written in two seconds");
	if (seconds >= 2.0)
		printf("writing %lu rows of 40000 intervals took %.1f s\n", part->row_count, seconds);

	/* most of a million rows of 127 peers, which the plain scans take minutes over */
	memset(rows, 0, 1500000 * sizeof(*rows));
	part->intervals = 100;
	part->row_count = writer_rows(rows, 100, 127, 8000);
	check(part->row_count > 800000 && part->row_count < 1500000, "most of a million rows");
	seconds = writer_seconds(part);
	check(seconds < 2.0, "most of a million rows of 127 peers are written in two seconds");
	if (seconds >= 2.0)
		printf("writing %lu rows took %.1f s\n", part->row_count, seconds);
	free(rows);
	free(part);
}

/* the tables that cannot grow past their sizes count what no longer fits (the
header carries it to the report), and keep every machine the netcode has */
static void net_table_overflow_checks(void)
{
	static char identities[MAXIMUM_PROFILE_NET_CONNECTIONS + 5];
	static const struct profile_net_layout_member filler[] = { { "filler", 0, 4, _profile_net_key_none } };
	struct profile_net_row *rows = calloc(20000, sizeof(*rows));
	struct profile_net_snapshot snapshot;
	struct profile_net_counts counts;
	unsigned long row_count;
	long first_interval, intervals;
	unsigned char entries[8];
	char name[24];
	int index;

	profile_net_recording_begin();
	profile_net_part_begin(rows, 20000, 0, 0);
	for (index = 0; index < HALO_PORT_MAXIMUM_NETWORK_MACHINES + 3; index++)
		profile_net_machine_address(index, 0x0100007F, 2302);
	for (index = 0; index < MAXIMUM_PROFILE_NET_CONNECTIONS + 5; index++)
		profile_net_traffic(&identities[index], _profile_net_out, _profile_net_datagram, 1, 0, 0);
	for (index = 0; index < MAXIMUM_PROFILE_NET_OBJECTS + 10; index++)
	{
		int handle = (int)(0x80000000UL + (unsigned long)index);

		memset(entries, 0, sizeof(entries));
		memcpy(entries, &handle, 4);
		profile_net_entries(0, 6, entries, 1, 8);
	}
	for (index = 0; index < MAXIMUM_PROFILE_NET_FIELDS + 5; index++)
	{
		snprintf(name, sizeof(name), "overflow_%d", index);
		profile_net_field(name);
	}
	for (index = 0; index < MAXIMUM_PROFILE_NET_TYPES + 5; index++)
		profile_net_layout(200 + index % 50, filler, 1, 4);
	profile_net_part_end(1000000000ULL, &row_count, &first_interval, &intervals, &snapshot);
	profile_net_counters(&counts);
	check(snapshot.machines == HALO_PORT_MAXIMUM_NETWORK_MACHINES &&
		profile_net_machine_entry(HALO_PORT_MAXIMUM_NETWORK_MACHINES - 1)->machine == HALO_PORT_MAXIMUM_NETWORK_MACHINES - 1,
		"every machine of the netcode has its address");
	check(counts.machines_dropped == 3, "machines past the table are counted");
	check(snapshot.connections == MAXIMUM_PROFILE_NET_CONNECTIONS && counts.connections_overflowed == 5,
		"connection events past the table are counted");
	check(snapshot.objects == MAXIMUM_PROFILE_NET_OBJECTS && counts.objects_overflowed == 10,
		"object keys past the table are counted");
	check(counts.fields_overflowed >= 5, "field names past the table are counted");
	check(counts.layouts_overflowed >= 5, "layouts past the table are counted");
	check(counts.sites_overflowed >= 8, "sending sites past the table are counted (net_sender_checks filled it)");
	/* (the run's tables: the count stays until the process ends, in every part) */
	profile_net_counters(&counts);
	check(counts.machines_dropped == 0 && counts.sites_overflowed >= 8 && counts.fields_overflowed >= 5,
		"a recording's counts start over at each part, the run's tables' do not");
	profile_net_recording_end();
	free(rows);
}

/* the reset race: a p2p pass begun in one recording ends, from inside
the next recording's allocator seam, before that start resets the tracks'
counts. Only the track lock orders its bump before the reset, which
ThreadSanitizer checks (the reset moved out of the lock is a race) */
static int race_armed;
static int race_ended;

static void *race_pass(void *unused)
{
	(void)unused;
	profile_trace_thread_register(_profile_track_p2p);
	fake_lock();
	profile_trace_begin(name_pass);
	fake_unlock();
	__atomic_store_n(&straddle_begun, 1, __ATOMIC_RELEASE);
	/* (relaxed: no happens-before from the game thread's release) */
	while (!__atomic_load_n(&straddle_released, __ATOMIC_RELAXED))
		usleep(100);
	fake_lock();
	profile_trace_end(name_pass);
	fake_unlock();
	__atomic_store_n(&race_ended, 1, __ATOMIC_RELAXED);
	return NULL;
}

static void *race_allocate(unsigned long size)
{
	if (__atomic_load_n(&race_armed, __ATOMIC_RELAXED))
	{
		__atomic_store_n(&race_armed, 0, __ATOMIC_RELAXED);
		__atomic_store_n(&straddle_released, 1, __ATOMIC_RELAXED);
		while (!__atomic_load_n(&race_ended, __ATOMIC_RELAXED))
			usleep(100);
	}
	return fake_allocate(size);
}

static void reset_race_checks(void)
{
	pthread_t thread;

	profile_trace_set_allocator(race_allocate, fake_release);
	forget_written();
	straddle_begun = straddle_released = 0;
	profile_trace_request_start(0.0, _profile_trace_when_now, 4);
	frame(1);
	pthread_create(&thread, NULL, race_pass, NULL);
	while (!__atomic_load_n(&straddle_begun, __ATOMIC_ACQUIRE))
		usleep(100);
	profile_trace_request_stop();
	profile_trace_frame_boundary();
	finish();

	forget_written();
	advance(1000000);
	__atomic_store_n(&race_armed, 1, __ATOMIC_RELAXED);
	profile_trace_request_start(0.0, _profile_trace_when_now, 4);
	frame(1);
	pthread_join(thread, NULL);
	frame(1);
	profile_trace_request_stop();
	profile_trace_frame_boundary();
	finish();
	check(written.counts[name_pass] == 0, "the pass of the last recording is not written");
	check(written.dropped == 0, "a pass ended before the start is not counted in the new recording");
	check(outstanding() == 0, "the race recordings leave nothing allocated");
	profile_trace_set_allocator(fake_allocate, fake_release);
}

/* a thread that is not the game's reaches a funnel hook while recordings
start and stop: its events are counted, and ThreadSanitizer sees no race on
the flag or the counter */
static int foreign_stop;
static long foreign_calls;

static void *foreign_thread(void *unused)
{
	(void)unused;
	while (!__atomic_load_n(&foreign_stop, __ATOMIC_RELAXED))
	{
		profile_net_built(6);
		__atomic_fetch_add(&foreign_calls, 1, __ATOMIC_RELAXED);
		usleep(10);
	}
	return NULL;
}

static void foreign_thread_checks(void)
{
	pthread_t thread;
	unsigned long counted = 0;
	int round, index;

	foreign_stop = 0;
	pthread_create(&thread, NULL, foreign_thread, NULL);
	for (round = 0; round < 20; round++)
	{
		forget_written();
		profile_trace_request_start(0.0, _profile_trace_when_now, 4);
		for (index = 0; index < 50; index++)
		{
			frame(2);
			usleep(200);
		}
		profile_trace_request_stop();
		profile_trace_frame_boundary();
		finish();
		counted += written.foreign_events;
	}
	__atomic_store_n(&foreign_stop, 1, __ATOMIC_RELAXED);
	pthread_join(thread, NULL);
	/* (a thousand frames of 200 us with a call every 10 us: the thread cannot miss every recording) */
	check(counted > 0, "an event of the foreign thread is counted while recording");
	check(foreign_calls > 0 && outstanding() == 0, "the foreign thread ran, and nothing is left allocated");
}

int main(int argc, char **argv)
{
	profile_trace_set_clock(fake_clock);
	profile_trace_set_track_lock(fake_lock, fake_unlock);
	profile_trace_set_allocator(fake_allocate, fake_release);
	profile_trace_set_log(quiet_log);
	profile_trace_set_notify(fake_notify);
	profile_trace_set_writer(fake_writer_start);
	profile_trace_set_ready(fake_ready_test);
	profile_trace_set_session(fake_session);
	profile_trace_set_session_sampler(fake_session_sample);
	gametype_checks();
	profile_trace_thread_register(_profile_track_game);
	name_frame = profile_trace_name("frame");
	name_tick = profile_trace_name("game_tick");
	name_a = profile_trace_name("game_tick.objects");
	name_b = profile_trace_name("objects_update");
	name_c = profile_trace_name("deep");
	name_pass = profile_trace_name("p2p.pass");
	name_aggregate_child = profile_trace_name("aggregate_child");
	profile_trace_name_aggregate("texture");
	profile_trace_name_aggregate("render_model");
	name_texture = profile_trace_name("texture");
	name_inner_aggregate = profile_trace_name("render_model");
	fake_ready = 1;
	written.records_in_span = 1;

	scope_checks();
	state_checks();
	stop_answer_checks();
	part_checks();
	net_checks();
	layout_checks();
	put_checks();
	net_sender_checks();
	net_batch_seam_checks();
	net_received_checks();
	reliable_entry_checks();
	net_connection_checks();
	net_tunnel_checks();
	net_ping_checks();
	net_overlay_number_checks();
	net_simulated_loss_checks();
	net_counting_checks();
	straddle_checks();
	lifetime_checks();
	overlay_checks();
	overlay_switch_checks();
	net_table_overflow_checks();
	worst_frame_checks();
	writer_pass_checks();
	release_path_checks();
	reset_race_checks();
	foreign_thread_checks();
	if (argc > 1)
	{
		file_checks(argv[1]);
		unwritable_checks(argv[1]);
	}
	printf("%s (%d failures)\n", failures ? "FAIL" : "PASS", failures);
	return failures != 0;
}

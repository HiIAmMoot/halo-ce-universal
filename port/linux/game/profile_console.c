/*
PROFILE_CONSOLE.C

The profiling build's console commands and launch settings (configure.py
--profile; the recording is port/linux/src/profile_trace.c's):
profile_record [seconds] and profile_stop, read
as text before the script compiler, as the host's ban and kick are, so they
are no script functions (compiled map scripts index those) and a client
may give them; debug.profile_record, profile_record_when and
profile_memory; and the game's side of the recording's seams: its log,
its console, when a game is in progress, and what the header says of the
machine and its map.
*/

#ifdef HALO_PROFILE

#include "cseries.h"
#include "game/game.h"
#include "game/game_engine.h"
#include "main/console.h"
#include "networking/network_game_globals.h"
#include "objects/objects.h"
#include "objects/object_types.h"
#include "tag_files/tag_files.h"
#include "network_distributed.h"
#include "profile_console.h"
#include "profile_console_gametype.h"
#include "profile_overlay.h"
#include "profile_overlay_lines.h"
#include "profile_trace.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* the platform layer's (platform.h, port_config.h), and main.c's */
void platform_log(const char *format, ...);
const char *platform_data_root(void);
int posix_make_directory(const char *path);
void *posix_directory_open(const char *path);
void posix_directory_close(void *directory);
void p2p_profile_lock(void);
void p2p_profile_unlock(void);
unsigned long p2p_peer_endpoint_address(unsigned long virtual_address);
int p2p_profile_statistics(struct profile_net_tunnel_peer *peers, int maximum);
long network_connection_profile_queued_bytes(void const *connection);
int config_boolean(const char *name);
long config_integer(const char *name);
const char *config_string(const char *name);
boolean main_menu_is_loaded(void);

/* ---------- globals */

static boolean profile_console_installed;

static void profile_console_map_component(const char *map, char *name, int size)
{
	const char *component = map;
	const char *cursor;
	int used = 0;

	for (cursor = map; *cursor; cursor++)
		if (*cursor == '/' || *cursor == '\\')
			component = cursor + 1;
	for (cursor = component; *cursor && used + 1 < size; cursor++)
	{
		unsigned char character = (unsigned char)*cursor;
		if (character >= 'A' && character <= 'Z')
			character = (unsigned char)(character + ('a' - 'A'));
		name[used++] = (character >= 'a' && character <= 'z') || (character >= '0' && character <= '9') ||
			character == '_' || character == '-' ? (char)character : '-';
	}
	name[used] = 0;
}

static const char *profile_console_gametype(void)
{
	long engine;

	if (!game_in_progress() || main_menu_is_loaded() || game_connection() == _game_connection_film_playback)
		return "none";
	if (!game_engine_running())
		return profile_console_engine_gametype(FALSE, 0);
	engine = game_engine_get_variant()->game_engine_index;
	return profile_console_engine_gametype(TRUE, engine);
}

/* the map the header names: the one loading takes its place only after the
frame boundary that stops a recording on it (profile_console_frame) */
static char profile_console_map[64];
static char profile_console_next_map[64];

/* when the tunnel, the pings and the queues are next read (the clock's
nanoseconds) */
static unsigned long long profile_console_next_second;

/* ---------- private code */

static void profile_console_log(
	const char *text)
{
	platform_log("profile: %s", text);
}

static void profile_console_notify(
	const char *text)
{
	console_printf(FALSE, "%s", text);
}

static int profile_console_ready(
	void)
{
	return game_in_progress() && !main_menu_is_loaded();
}

static void profile_console_session(
	struct profile_trace_session *session)
{
	short player_index;
	const char *scenario = profile_console_scenario_map(profile_console_next_map, profile_console_map);
	void *folder;

	csmemset(session, 0, sizeof(*session));
	snprintf(session->folder, sizeof(session->folder), "%s/profiles", platform_data_root());
	posix_make_directory(session->folder);
	/* (a folder that can't be made must not lose the recording: the parts go
	to the data root itself then) */
	folder = posix_directory_open(session->folder);
	if (folder)
		posix_directory_close(folder);
	else
		strncpy(session->folder, platform_data_root(), sizeof(session->folder) - 1);
#ifdef HALO_RELEASE
	strcpy(session->build, "release profile");
#else
	strcpy(session->build, "debug profile");
#endif
	session->own_machine = -1;
	switch (game_connection())
	{
	case _game_connection_network_server:
		strcpy(session->role, "host");
		break;
	case _game_connection_network_client:
		strcpy(session->role, "client");
		session->own_machine = network_game_client_get_local_machine_index();
		break;
	default:
		strcpy(session->role, "local");
		break;
	}
	strncpy(session->map, scenario, sizeof(session->map) - 1);
	profile_console_map_component(scenario, session->map_name, sizeof(session->map_name));
	strncpy(session->gametype, profile_console_gametype(), sizeof(session->gametype) - 1);
	for (player_index = 0; player_index < MAXIMUM_TRACKED_PLAYERS; player_index++)
	{
		if (distributed_player(player_index))
			session->players++;
	}
	session->players_most = session->players;
}

/* (once a second on the game thread: unlike the start-time description it
makes and opens no folder, which would land in the measured frame) */
static void profile_console_sample_session(
	struct profile_trace_session *session)
{
	short player_index;
	const char *scenario = profile_console_scenario_map(profile_console_next_map, profile_console_map);

	if (!session->map_name[0] && scenario[0])
	{
		strncpy(session->map, scenario, sizeof(session->map) - 1);
		profile_console_map_component(scenario, session->map_name, sizeof(session->map_name));
		strncpy(session->gametype, profile_console_gametype(), sizeof(session->gametype) - 1);
	}
	for (player_index = 0; player_index < MAXIMUM_TRACKED_PLAYERS; player_index++)
		if (distributed_player(player_index))
			session->players++;
}

/* an object's type and its definition's tag name, the first time a
recording sees its datum index (networked objects have the same index on
every machine); FALSE when no object has it any more, or the key is not a
handle: a real one has a salt (above bit 15), and a key without one (a
cutscene flag's index, in a coop event) would have datum_try_and_get skip its
salt check and name whatever object is in that slot. Not between maps, when
the objects' array is gone or not valid */
static int profile_console_describe(
	long key,
	char *object_type,
	int object_type_size,
	char *tag,
	int tag_size)
{
	struct object_datum *object;
	char const *name;

	if ((key >> 16) == 0 || !object_header_data || !object_header_data->valid)
		return FALSE;
	object = object_try_and_get(key);
	if (!object)
		return FALSE;
	strncpy(object_type, object_type_get_name(object->object.type), object_type_size - 1);
	object_type[object_type_size - 1] = 0;
	name = tag_get_name(object->definition_index);
	strncpy(tag, name ? name : "", tag_size - 1);
	tag[tag_size - 1] = 0;
	return TRUE;
}

/* the recording's seams. Not left to the first frame: init.txt's
profile_record is handled before it, and the recording's name (folder and
role) is chosen from the session when it is requested */
static void profile_console_install(
	void)
{
	if (profile_console_installed)
		return;
	profile_console_installed = TRUE;
	profile_trace_set_log(profile_console_log);
	profile_trace_set_notify(profile_console_notify);
	profile_trace_set_ready(profile_console_ready);
	profile_trace_set_session(profile_console_session);
	profile_trace_set_session_sampler(profile_console_sample_session);
	profile_trace_set_track_lock(p2p_profile_lock, p2p_profile_unlock);
	network_distributed_profile_register();
	profile_net_set_object_describe(profile_console_describe);
	profile_net_set_peer_describe(p2p_peer_endpoint_address);
	profile_net_set_queue_reader(network_connection_profile_queued_bytes);
	/* (the window's close, debug.exit_after and XLaunchNewImage end the
	process with exit(), which main_exit never sees) */
	atexit(profile_trace_shutdown);
}

static void profile_console_tunnel(
	void)
{
	/* (static: 127 peers are about 7 KB, too much for the game thread's stack on the Android guest) */
	static struct profile_net_tunnel_peer peers[MAXIMUM_PROFILE_NET_PEERS];

	profile_net_tunnel(peers, p2p_profile_statistics(peers, MAXIMUM_PROFILE_NET_PEERS));
}

/* the pings the scoreboard shows: a client's own round trip, the host's of
each client's machine (once: a machine's players share its ping) */
static void profile_console_pings(
	void)
{
	short player_index;
	byte taken[HALO_PORT_MAXIMUM_NETWORK_MACHINES] = { 0 };

	if (game_connection() == _game_connection_network_client)
	{
		real round_trip = distributed_own_round_trip_ticks();

		if (round_trip > 0.0f)
			profile_net_ping(PROFILE_NET_HOST, (long)(round_trip * 1000.0f / TICKS_PER_SECOND + 0.5f));
		return;
	}
	if (game_connection() != _game_connection_network_server)
		return;
	for (player_index = 0; player_index < MAXIMUM_TRACKED_PLAYERS; player_index++)
	{
		long machine = distributed_player_machine(player_index);
		long ping = distributed_player_ping(player_index);

		if (machine < 0 || machine >= HALO_PORT_MAXIMUM_NETWORK_MACHINES || ping == NONE || taken[machine])
			continue;
		taken[machine] = 1;
		profile_net_ping(machine, ping);
	}
}

/* what follows the command word an expression starts with (past spaces and
an opening parenthesis, as the console and init.txt give it), else NULL */
static char const *profile_console_word(
	char const *expression,
	char const *word)
{
	long length = (long)strlen(word);
	long index;

	while (*expression == ' ' || *expression == '\t' || *expression == '(')
		expression++;
	for (index = 0; index < length; index++)
	{
		char character = expression[index] >= 'A' && expression[index] <= 'Z' ? expression[index] - 'A' + 'a' : expression[index];

		if (character != word[index])
			return NULL;
	}
	expression += length;
	if (*expression && *expression != ' ' && *expression != '\t' && *expression != ')')
		return NULL;
	while (*expression == ' ' || *expression == '\t')
		expression++;
	return expression;
}

/* ---------- public code */

void profile_console_launch(
	void)
{
	char const *when = config_string("debug.profile_record_when");

	strcpy(profile_console_map, profile_console_next_map);
	profile_console_install();
	profile_trace_thread_register(_profile_track_game);
	platform_log("profile: network version %d", HALO_PORT_NETWORK_VERSION);
	profile_overlay_toggle(config_boolean("debug.profile_overlay"));
	if (strcmp(when, "start") != 0 && strcmp(when, "game") != 0)
		platform_log("profile: debug.profile_record_when is \"%s\": \"start\" taken", when);
	if (config_boolean("debug.profile_record"))
	{
		profile_trace_request_start(0.0, strcmp(when, "game") == 0 ? _profile_trace_when_game : _profile_trace_when_now,
			config_integer("debug.profile_memory"));
	}
}

void profile_console_frame(
	void)
{
	unsigned long long now = profile_trace_clock();

	strcpy(profile_console_map, profile_console_next_map);
	if (!profile_net_counting || now < profile_console_next_second)
		return;
	profile_console_next_second = now + 1000000000ULL;
	profile_console_tunnel();
	profile_console_pings();
	profile_net_sample_queues();
}

void profile_console_map_loaded(
	char const *map_name)
{
	strncpy(profile_console_next_map, map_name ? map_name : "", sizeof(profile_console_next_map) - 1);
	profile_trace_map_loaded();
}

boolean profile_console_command(
	char const *expression)
{
	struct profile_trace_status status;
	char const *rest;

	profile_console_install();
	if ((rest = profile_console_word(expression, "profile_record")) != NULL)
	{
		char *end;
		double seconds = strtod(rest, &end);

		if (end == rest)
			seconds = 0.0;
		if ((*rest && end == rest) || seconds < 0.0 ||
			(*end && *end != ' ' && *end != '\t' && *end != ')'))
		{
			console_printf(FALSE, "profile: profile_record takes a number of seconds, 0 or more");
			return TRUE;
		}
		switch (profile_trace_request_start(seconds, _profile_trace_when_now, config_integer("debug.profile_memory")))
		{
		case _profile_trace_answer_armed:
			profile_trace_status(&status);
			if (seconds > 0.0)
				console_printf(FALSE, "profile: recording %s, %g s", status.name, seconds);
			else
				console_printf(FALSE, "profile: recording %s, until profile_stop", status.name);
			break;
		case _profile_trace_answer_still_writing:
			profile_trace_status(&status);
			console_printf(FALSE, "profile: still writing %s", status.name);
			break;
		default:
			console_printf(FALSE, "profile: already recording");
			break;
		}
		return TRUE;
	}
	if (profile_console_word(expression, "profile_stop"))
	{
		profile_trace_status(&status);
		switch (profile_trace_request_stop())
		{
		case _profile_trace_answer_stopping:
			console_printf(FALSE, "profile: writing %s, %ld parts", status.name, status.part);
			break;
		case _profile_trace_answer_disarmed:
			console_printf(FALSE, "profile: not waiting for a game any more");
			break;
		default:
			console_printf(FALSE, "profile: not recording");
			break;
		}
		return TRUE;
	}
	if ((rest = profile_console_word(expression, "profile_overlay")) != NULL)
	{
		boolean on = !profile_overlay_visible();

		switch (profile_overlay_switch(rest))
		{
		case _profile_overlay_switch_on:
			on = TRUE;
			break;
		case _profile_overlay_switch_off:
			on = FALSE;
			break;
		case _profile_overlay_switch_invalid:
			console_printf(FALSE, "profile: profile_overlay takes on or off");
			return TRUE;
		}
		profile_overlay_toggle(on);
		console_printf(FALSE, "profile: overlay %s", on ? "on" : "off");
		return TRUE;
	}
	return FALSE;
}

#endif

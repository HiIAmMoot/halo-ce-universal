/*
NETWORK_TEST.C

Automated system link sessions for testing the netcode without the menus
(debug.network_test in config.toml, HALO_NETWORK_TEST):

- "host:<map>[:<variant>[,<variant>...]]" hosts a game on that
  multiplayer map (bloodgulch, ...) with one of the built-in game variants
  (slayer by default; game_engine_get_variant_by_name), as the pregame
  screen's fast setup does, and starts it debug.network_test_start seconds
  later; with more variants, once a game is over (debug.network_test_score
  makes it short) the next, as the host's button on the scores does. A map
  with a path is that level (custom_maps\a30, a Custom Edition map's), and
  the variant "coop" makes the game network co-op on it, at normal
  difficulty, as the Map screen does for a campaign level;
- "local:<map>[:<variant>...]" the same with a local (split screen) game of
  one player, as Multiplayer > Split Screen of the Xbox's menus sets one up
  (no other machine joins it: the bots' games, bots.count);
- "join" searches for games and joins the first it finds, as picking it in
  the system link list does;
- "browse" joins the game with the most players that the server browser
  lists after five seconds (internet play's public games: a host's with
  debug.network_test_public), co-op ones last, as picking it there does,
  and then as "join". It logs what it lists.
- "log" hosts and joins nothing: the game that runs (map_name, in init.txt,
  plays one without the menus) is logged, as below.
- "probe" (halo --probe [<invite>]: a list site's look at a server) searches
  as "join" does, joins nothing, and prints the first game it finds (with
  an invite, only its host's, over its tunnel; without, one on the LAN) on
  the standard output as one line of JSON, then exits (1, with ok false, if
  none is found in PROBE_SECONDS).

Once the game runs, every second each machine logs where every player's
unit is, so the machines' views of the game can be compared, and the host
how far its own run of each client's player came from the client's
(network_distributed_divergence): tools/net_compare.py compares two builds
so (port/linux/NETCODE.md, "Comparing two builds", has the line's format).

Scripted play for the netcode's parts the bots' wandering does not reach:
debug.network_test_kill (the host kills the last player every so often),
debug.network_test_shoot (every so often each machine's player hits the
next with their weapon's projectile: a client's through its report to the
host) and debug.network_test_vehicle (the host seats the last player as a
vehicle's driver that many seconds in, takes them out 15 seconds on, and 5
seconds later stands them at a vehicle's driver's entrance, where a joining
machine's player holds the action button, as getting in does)
and debug.network_test_pickup (the last player stands on a weapon lying
about that many seconds in, and a joining machine's player holds the action
button a second later, to pick it up).

debug.network_test_cues: console lines the host (or a game of one machine)
runs at their ticks, counted from the first tick every player of
debug.network_test_cue_players has a unit (tools/stagekit's scenarios).

Called from the main loop every frame (main.c).
*/

#include "cseries.h"
#include "game_modes.h"
#include "main/main.h"
#include "bink/bink_playback.h"
#include "interface/player_ui.h"
#include "saved games/player_profile.h"
#include "interface/ui_widget.h"
#include "networking/network_game_globals.h"
#include "networking/network_client_manager.h"
#include "networking/network_server_manager.h"
#include "game/game.h"
#include "game/game_engine.h"
#include "game/players.h"
#include "objects/objects.h"
#include "units/units.h"
#include "units/unit_definitions.h"
#include "items/weapons.h"
#include "items/weapon_definitions.h"
#include "items/projectile_definitions.h"
#include "items/items.h"
#include "objects/damage.h"
#include "scenario/scenario.h"
#include "tag_files/tag_files.h"
#include "camera/observer.h"
#include "hs/hs.h"
#include "main/console.h"
#include "cseries/errors.h"
#include "scenario/scenario_definitions.h"
#include "units/vehicle_datum.h"
#include "units/bipeds.h"
#include "custom_edition_cache.h"
#include "profile_console_gametype.h"

#include "../src/p2p.h" /* port: port/linux/src/p2p.c, p2p_lobby.c */
#include "network_chat.h"
#include "network_voice.h"
#include "halo_keyboard.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <time.h>
#include <stdlib.h>
#include <string.h>

/* the platform layer's (port/linux/src/port_config.c) */
const char *config_string(char const *name);
int config_boolean(char const *name);
double config_real(char const *name);
long config_integer(char const *name);
int config_write(char const *name, char const *value);
void platform_log(char const *format, ...);
/* the data root and its folders, and the game's name for a path (xbox_files.c, posix.h) */
const char *platform_data_root(void);
void platform_translate_path(const char *xbox_path, char *host_path, unsigned long host_path_size);
void *posix_directory_open(const char *path);
int posix_directory_next(void *directory, char *name, unsigned long name_size);
void posix_directory_close(void *directory);
int posix_make_directory(const char *path);
/* game_time.c's */
real game_time_get_tick_fraction(void);
/* damage.c's */
void damage_kill_object_for_player(long object_index, long player_index);
/* network_distributed.c's */
void network_distributed_statistics(long *sent, long *received, long *corrections);
void network_distributed_divergence(long *samples, long *apart, real *mean, real *largest);
void network_distributed_item_statistics(long *creates, long *deletes, long *failures, long *removed);
void network_damage_statistics(long *sent_reports, long *dealt_reports, long *rejected_reports, long *replayed_events);
/* xinput_sdl.c's */
void test_input_hold_action(int hold);
void test_input_press(unsigned long buttons);
/* menu_functions.c's: whether the client's advertised game at the index is
an internet play peer's (over the tunnel), not the LAN's */
boolean menu_advertised_game_from_peer(long index);

enum
{
	/* how long a probe looks for a game (halo --probe), and the client's
	advertised games (network_client_manager.c's) */
	PROBE_SECONDS = 30,
	PROBE_MAXIMUM_GAMES = 9,
};

enum
{
	_network_test_off,
	_network_test_host,
	_network_test_join,
	_network_test_browse,
	_network_test_log,
	_network_test_probe,
};

static struct
{
	boolean checked;
	short mode;
	char map_name[64];
	char variant_name[64];
	/* "local:": a local (split screen) game, not a system link one */
	boolean local;
	/* ... the variant of this game, of variant_name's list; and the seconds
	the last game's scores have been shown */
	short variant_index;
	real postgame_seconds;
	boolean game_over;
	real start_delay;
	real menu_seconds;
	boolean set_up;
	/* (the hosted game's whole invite logged, for the test that joins it:
	the game's own lines give its first digits only, since players post
	their logs. This is debug.network_test's host alone, a test's machine,
	whose log nobody posts) */
	boolean invite_logged;
	real setup_seconds;
	boolean started;
	boolean joined;
	boolean map_set;
	boolean player_added;
	real joined_seconds;
	boolean team_set;
	real kill_interval;
	real shoot_interval;
	real vehicle_time;
	real pickup_time;
	char pickup_weapon[64];
	/* debug.network_test_chat's, and the lines said this game */
	real chat_time;
	short chat_lines_said;
	long score_to_win;
	long logged_time;
	boolean public_game;
	boolean browsing;
	boolean browse_listed;
	float browse_seconds;
	float probe_seconds;
	/* debug.network_test_cues: each line and its tick from the cues' start,
	the first tick every player of cue_players has a unit (NONE until then) */
	struct
	{
		long tick;
		char line[256];
	} cues[32];
	short cue_count;
	short next_cue;
	long cue_players;
	long cues_start;
} network_test;

/* the variant at the index of the list (copied to name), FALSE past its end */
static boolean network_test_variant(
	short index,
	char *name,
	size_t size)
{
	char const *variant = network_test.variant_name;

	for (; index > 0 && variant; index--)
	{
		variant = strchr(variant, ',');
		if (variant)
			variant++;
	}
	if (!variant || !*variant)
		return FALSE;
	if (name)
	{
		size_t length = strcspn(variant, ",");

		snprintf(name, size, "%.*s", (int)length, variant);
	}
	return TRUE;
}

static void network_test_read_settings(
	void)
{
	char const *setting = config_string("debug.network_test");

	network_test.checked = TRUE;
	if ((!strncmp(setting, "host:", 5) && setting[5]) || (!strncmp(setting, "local:", 6) && setting[6]))
	{
		char *colon;

		network_test.mode = _network_test_host;
		network_test.local = setting[0] == 'l';
		snprintf(network_test.map_name, sizeof(network_test.map_name), "%s", strchr(setting, ':') + 1);
		snprintf(network_test.variant_name, sizeof(network_test.variant_name), "slayer");
		colon = strchr(network_test.map_name, ':');
		if (colon)
		{
			*colon = 0;
			snprintf(network_test.variant_name, sizeof(network_test.variant_name), "%s", colon + 1);
		}
	}
	else if (!strcmp(setting, "join"))
	{
		network_test.mode = _network_test_join;
	}
	else if (!strcmp(setting, "browse"))
	{
		network_test.mode = _network_test_browse;
	}
	else if (!strcmp(setting, "log"))
	{
		network_test.mode = _network_test_log;
	}
	else if (!strcmp(setting, "probe"))
	{
		network_test.mode = _network_test_probe;
	}
	network_test.public_game = config_boolean("debug.network_test_public") != 0;
	network_test.start_delay = (real)config_real("debug.network_test_start");
	network_test.kill_interval = (real)config_real("debug.network_test_kill");
	network_test.shoot_interval = (real)config_real("debug.network_test_shoot");
	network_test.vehicle_time = (real)config_real("debug.network_test_vehicle");
	network_test.pickup_time = (real)config_real("debug.network_test_pickup");
	network_test.chat_time = (real)config_real("debug.network_test_chat");
	snprintf(network_test.pickup_weapon, sizeof(network_test.pickup_weapon), "%s",
		config_string("debug.network_test_pickup_weapon"));
	network_test.score_to_win = (long)config_integer("debug.network_test_score");
	network_test.cue_players = MAX(1, config_integer("debug.network_test_cue_players"));
	network_test.cues_start = NONE;
#ifndef HALO_WEB
	/* (not on the web, where a link's ?set= would have it run what it says) */
	{
		char const *cue = config_string("debug.network_test_cues");

		while (*cue)
		{
			char const *end = strchr(cue, '|');
			int length = end ? (int)(end - cue) : (int)strlen(cue);
			char *line;
			double second = strtod(cue, &line);
			short index;

			if (!(second >= 0.0 && second < 86400.0) || line == cue || line >= cue + length ||
				cue + length - line >= (int)sizeof(network_test.cues[0].line))
			{
				platform_log("network test: cue \"%.*s\" is not \"<second> <line>\" of %d characters at most: left out",
					length, cue, (int)sizeof(network_test.cues[0].line) - 1);
			}
			else if (network_test.cue_count >= (short)NUMBEROF(network_test.cues))
			{
				platform_log("network test: cues past %d left out", (int)NUMBEROF(network_test.cues));
			}
			else
			{
				/* (in the order they run) */
				long tick = (long)(second * TICKS_PER_SECOND + 0.5);

				for (index = network_test.cue_count; index > 0 && network_test.cues[index - 1].tick > tick; index--)
					network_test.cues[index] = network_test.cues[index - 1];
				network_test.cues[index].tick = tick;
				snprintf(network_test.cues[index].line, sizeof(network_test.cues[index].line), "%.*s",
					(int)(cue + length - line), line);
				network_test.cue_count++;
			}
			cue += length + (end != NULL);
		}
	}
#endif
	if (network_test.mode != _network_test_off)
		platform_log("network test: %s", setting);
}

/* appends to a line, cut short when it is full */
static void network_test_append(
	char *line,
	int size,
	int *length,
	char const *format,
	...)
{
	va_list arguments;
	int written;

	if (*length >= size - 1)
		return;
	va_start(arguments, format);
	written = vsnprintf(line + *length, (size_t)(size - *length), format, arguments);
	va_end(arguments);
	if (written > 0)
		*length = MIN(*length + written, size - 1);
}

/* every player's unit, as this machine sees it */
static void network_test_log_players(
	void)
{
	struct data_iterator iterator;
	struct player_datum *player;
	char line[4096];
	int length = 0;

	line[0] = 0;
	/* (each player's name, once a game: for tests that name a player, such
	as the host's ban command) */
	{
		static wchar_t named[HALO_PORT_MAXIMUM_NETWORK_PLAYERS][12];

		if (game_time_get() < 2 * TICKS_PER_SECOND)
			csmemset(named, 0, sizeof(named));
		data_iterator_new(&iterator, player_data);
		while ((player = (struct player_datum *)data_iterator_next(&iterator)) != NULL)
		{
			long absolute_index = DATUM_INDEX_TO_ABSOLUTE_INDEX(iterator.datum_index);
			char name[13];
			short index;

			if (absolute_index >= HALO_PORT_MAXIMUM_NETWORK_PLAYERS ||
				!csmemcmp(named[absolute_index], player->name, sizeof(named[absolute_index])))
			{
				continue;
			}
			csmemcpy(named[absolute_index], player->name, sizeof(named[absolute_index]));
			for (index = 0; index < 12 && player->name[index]; index++)
				name[index] = player->name[index] >= 32 && player->name[index] < 127 ? (char)player->name[index] : '?';
			name[index] = 0;
			platform_log("network test: player %ld is named %s", absolute_index, name);
		}
	}
	data_iterator_new(&iterator, player_data);
	while ((player = (struct player_datum *)data_iterator_next(&iterator)) != NULL)
	{
		if (player->unit_index != NONE)
		{
			struct object_datum *object = object_get(player->unit_index);
			struct unit_datum *unit = unit_get(player->unit_index);
			short slot;

			/* (riding: where its vehicle is) */
			struct object_datum *placed = object->object.parent_object_index != NONE ?
				object_get(object->object.parent_object_index) : object;

			network_test_append(line, (int)sizeof(line), &length, " player %ld: (%.3f %.3f %.3f) h%.2f/%.2f%s%s g%d/%d w",
				(long)DATUM_INDEX_TO_ABSOLUTE_INDEX(iterator.datum_index), placed->object.position.x,
				placed->object.position.y, placed->object.position.z, object->object.body_vitality,
				object->object.shield_vitality, placed != object ? " riding" : "",
				TEST_FLAG(unit->unit.flags, _unit_active_camouflaged_bit) ? " camo" : "",
				unit->unit.grenade_counts[0], unit->unit.grenade_counts[1]);
			/* where it aims (yaw and pitch, degrees), its animation state and
			how hard it is moving */
			network_test_append(line, (int)sizeof(line), &length, " a%.0f/%.0f f%.0f l%.0f/%.0f as%d/%d st%d thr%.2f",
				atan2(unit->unit.aiming_vector.j, unit->unit.aiming_vector.i) * 57.29578,
				asin(PIN(unit->unit.aiming_vector.k, -1.0f, 1.0f)) * 57.29578,
				atan2(object->object.forward.j, object->object.forward.i) * 57.29578,
				atan2(unit->unit.looking_vector.j, unit->unit.looking_vector.i) * 57.29578,
				asin(PIN(unit->unit.looking_vector.k, -1.0f, 1.0f)) * 57.29578,
				(int)unit->unit.animation.aiming_screen_index, (int)unit->unit.animation.looking_screen_index,
				(int)unit->unit.animation.state,
				sqrt(unit->unit.throttle.i * unit->unit.throttle.i + unit->unit.throttle.j * unit->unit.throttle.j));
			for (slot = 0; slot < MAXIMUM_WEAPONS_PER_UNIT; slot++)
			{
				long weapon_index = unit->unit.weapon_object_indices[slot];

				if (weapon_index != NONE)
				{
					struct weapon_datum *weapon = weapon_get(weapon_index);

					network_test_append(line, (int)sizeof(line), &length, " %lx:%d",
						(unsigned long)weapon->definition_index & 0xFFFF, weapon->weapon.magazines[0].rounds_total +
						weapon->weapon.magazines[0].rounds_loaded);
				}
			}
		}
		else
		{
			network_test_append(line, (int)sizeof(line), &length, " player %ld: dead",
				(long)DATUM_INDEX_TO_ABSOLUTE_INDEX(iterator.datum_index));
		}
		/* the game type's score and the kills and deaths */
		network_test_append(line, (int)sizeof(line), &length, " s%ld k%d d%d f%d t%ld m%d",
			game_engine && game_engine->get_player_score ?
				game_engine->get_player_score(iterator.datum_index, _get_score_individual) : -1L,
			player->statistics.kills[0], player->statistics.deaths, player->statistics.friendly_fire_kills, (long)player->team_index,
			(int)player->network_player_data.machine_index);
	}
	{
		long sent, received, corrections;
		struct object_iterator objects;
		long ground_items = 0;
		long creates, deletes, failures, removed;
		long sent_reports, dealt_reports, rejected_reports, replayed_events;
		/* this machine's player and where its camera is (a player that never
		spawns leaves it where it began) */
		long local_player_index = local_player_get_player_index(0);
		struct observer_result const *camera = observer_get_camera(0);
		long divergence_samples, divergence_apart;
		real divergence_mean, divergence_largest;

		object_iterator_new(&objects, _object_mask_weapon | _object_mask_equipment, 0);
		while (object_iterator_next(&objects))
		{
			struct item_datum *item = item_get(objects.index);

			if (item->object.parent_object_index == NONE &&
				TEST_FLAG(item->object.flags, _object_connected_to_map_bit) &&
				!TEST_FLAG(item->item.flags, _item_attached_to_unit_bit))
			{
				ground_items++;
			}
		}
		network_distributed_statistics(&sent, &received, &corrections);
		network_distributed_item_statistics(&creates, &deletes, &failures, &removed);
		network_damage_statistics(&sent_reports, &dealt_reports, &rejected_reports, &replayed_events);
		network_distributed_divergence(&divergence_samples, &divergence_apart, &divergence_mean, &divergence_largest);

		platform_log("network test: tick %ld%s | items %ld (+%ld -%ld !%ld x%ld) | %s to %ld | sent %ld received %ld corrected %ld"
			" | hits %ld dealt %ld rejected %ld replayed %ld | local %ld camera (%.1f %.1f %.1f) respawn %ld"
			" | drift %ld %ld %.6f %.6f",
			game_time_get(), line, ground_items, creates, deletes, failures, removed,
			game_engine_can_score() ? "playing" : "game over",
			game_engine_running() ? (long)game_engine_get_variant()->universal_variant.score_to_win : 0L,
			sent, received, corrections,
			sent_reports, dealt_reports, rejected_reports, replayed_events,
			local_player_index == NONE ? -1L : (long)DATUM_INDEX_TO_ABSOLUTE_INDEX(local_player_index),
			camera ? camera->position.x : 0.0f, camera ? camera->position.y : 0.0f, camera ? camera->position.z : 0.0f,
			local_player_index == NONE ? 0L : (long)player_get(local_player_index)->respawn_timer,
			divergence_samples, divergence_apart, divergence_mean, divergence_largest);
	}
}

/* each of this machine's players hits the next player with their weapon's
projectile, as its impact would, when it is within the projectile's reach
(as a real shot is: the host checks it) */
static void network_test_shoot(
	void)
{
	struct data_iterator iterator;
	struct player_datum *player;

	data_iterator_new(&iterator, player_data);
	while ((player = (struct player_datum *)data_iterator_next(&iterator)) != NULL)
	{
		struct data_iterator targets;
		struct player_datum *target = NULL;
		struct player_datum *candidate;
		struct unit_datum *unit;
		long weapon_index;
		struct weapon_definition *weapon;
		struct weapon_trigger_definition *trigger;
		long damage_index = NONE;
		real reach = 0.0f;
		boolean melee;
		struct damage_data damage;
		struct object_datum *target_object;
		real_vector3d direction;

		if (player->local_player_index == NONE || player->unit_index == NONE)
			continue;
		data_iterator_new(&targets, player_data);
		while ((candidate = (struct player_datum *)data_iterator_next(&targets)) != NULL)
		{
			if (candidate != player && candidate->unit_index != NONE)
			{
				target = candidate;
				break;
			}
		}
		unit = unit_get(player->unit_index);
		if (!target || unit->unit.current_weapon_index == NONE ||
			TEST_FLAG(object_get(target->unit_index)->object.damage_flags, _object_dead_bit))
		{
			continue;
		}
		weapon_index = unit->unit.weapon_object_indices[unit->unit.current_weapon_index];
		if (weapon_index == NONE)
			continue;
		weapon = weapon_definition_get(object_get(weapon_index)->definition_index);
		if (weapon->weapon.triggers.count > 0)
		{
			trigger = TAG_BLOCK_GET_ELEMENT(&weapon->weapon.triggers, 0, struct weapon_trigger_definition);
			if (trigger->projectile.index != NONE)
			{
				struct projectile_definition *projectile = projectile_definition_get(trigger->projectile.index);

				damage_index = projectile->projectile.impact_damage.index;
				/* (how far it flies: its range, or at its speed for as long as
				its timer runs; none for no bound, network_damage.c) */
				if (projectile->projectile.maximum_range > 0.0f)
					reach = projectile->projectile.maximum_range;
				else if (projectile->projectile.detonation_timer_starts == 0)
				{
					reach = projectile->projectile.timer_upper_bound * TICKS_PER_SECOND *
						MAX(projectile->projectile.initial_velocity, projectile->projectile.final_velocity);
				}
			}
		}
		/* (else its melee: a blow's epicenter is its striker's, as
		unit_cause_player_melee_damage has it) */
		melee = damage_index == NONE;
		if (melee)
			damage_index = weapon->weapon.melee_attack_damage.index;
		if (damage_index == NONE)
			continue;
		target_object = object_get(target->unit_index);
		/* (a blow reaches only a target at hand, as the host checks) */
		if (melee)
		{
			real dx = target_object->object.position.x - unit->object.position.x;
			real dy = target_object->object.position.y - unit->object.position.y;
			real dz = target_object->object.position.z - unit->object.position.z;

			if (dx * dx + dy * dy + dz * dz > 1.5f * 1.5f)
				continue;
		}
		/* (a shot reaches no further than its projectile flies, with a margin
		for how far the target is from where the shot started) */
		else if (reach > 0.0f)
		{
			real dx = target_object->object.position.x - unit->object.position.x;
			real dy = target_object->object.position.y - unit->object.position.y;
			real dz = target_object->object.position.z - unit->object.position.z;

			if (dx * dx + dy * dy + dz * dz > 0.9f * reach * 0.9f * reach)
				continue;
		}
		damage_data_new(&damage, damage_index);
		damage.owner_player_index = iterator.datum_index;
		damage.owner_object_index = player->unit_index;
		damage.owner_team_index = unit->object.owner_team_index;
		damage.origin = melee ? unit->object.bounding_sphere_center : target_object->object.position;
		damage.epicenter = melee ? unit->object.bounding_sphere_center : target_object->object.position;
		direction.i = target_object->object.position.x - unit->object.position.x;
		direction.j = target_object->object.position.y - unit->object.position.y;
		direction.k = target_object->object.position.z - unit->object.position.z;
		normalize3d(&direction);
		damage.direction = direction;
		damage.scale = 1.0f;
		scenario_location_from_point(&damage.location, &damage.epicenter);
		object_cause_damage(&damage, target->unit_index, NONE, NONE, NONE, NULL);
		platform_log("network test: player %ld shoots player %ld",
			(long)DATUM_INDEX_TO_ABSOLUTE_INDEX(iterator.datum_index), (long)(target - (struct player_datum *)player_data->data));
	}
}

/* the host seats the last player as the nearest vehicle's driver, or out */
/* the host brings the players far from the first (on foot) near it, so
that they are within their weapons' reach (network_test_shoot shoots only
so near, as a player does); the host moving a client's player, as a
teleporter does */
static void network_test_gather(
	boolean leave_last)
{
	struct data_iterator iterator;
	struct player_datum *player;
	struct player_datum *first = NULL;
	struct player_datum *last = NULL;
	short count = 0;

	data_iterator_new(&iterator, player_data);
	while ((player = (struct player_datum *)data_iterator_next(&iterator)) != NULL)
	{
		if (!first)
			first = player;
		last = player;
	}
	if (!first || first->unit_index == NONE)
		return;
	data_iterator_new(&iterator, player_data);
	while ((player = (struct player_datum *)data_iterator_next(&iterator)) != NULL)
	{
		struct object_datum const *center = object_get(first->unit_index);
		struct object_datum *unit;
		real_point3d position;
		real dx, dy, dz;

		if (player == first || player->unit_index == NONE || (leave_last && player == last))
			continue;
		count++;
		unit = object_get(player->unit_index);
		if (unit->object.parent_object_index != NONE || TEST_FLAG(unit->object.damage_flags, _object_dead_bit))
			continue;
		dx = unit->object.position.x - center->object.position.x;
		dy = unit->object.position.y - center->object.position.y;
		dz = unit->object.position.z - center->object.position.z;
		if (dx * dx + dy * dy + dz * dz <= 20.0f * 20.0f)
			continue;
		/* (beside it, where the map is open: at its feet and its head) */
		{
			static real const offsets[][2] = { { 1.5f, 0.0f }, { -1.5f, 0.0f }, { 0.0f, 1.5f }, { 0.0f, -1.5f },
				{ 1.5f, 1.5f }, { -1.5f, -1.5f }, { 1.5f, -1.5f }, { -1.5f, 1.5f } };
			short try_index;

			for (try_index = 0; try_index < (short)NUMBEROF(offsets); try_index++)
			{
				short index = (short)((count + try_index) % (short)NUMBEROF(offsets));
				struct location feet, head;
				real_point3d above;

				position = center->object.position;
				position.x += offsets[index][0];
				position.y += offsets[index][1];
				position.z += 0.2f;
				above = position;
				above.z += 0.7f;
				scenario_location_from_point(&feet, &position);
				scenario_location_from_point(&head, &above);
				if (feet.cluster_index != NONE && head.cluster_index != NONE)
					break;
			}
			if (try_index >= (short)NUMBEROF(offsets))
				continue;
		}
		object_set_position(player->unit_index, &position, NULL, NULL);
		platform_log("network test: the host brings player %ld near the first",
			(long)(player - (struct player_datum *)player_data->data));
	}
}

static void network_test_vehicle(
	boolean enter)
{
	struct data_iterator iterator;
	struct player_datum *player;
	struct player_datum *last = NULL;

	data_iterator_new(&iterator, player_data);
	while ((player = (struct player_datum *)data_iterator_next(&iterator)) != NULL)
		last = player;
	if (!last || last->unit_index == NONE)
		return;
	if (!enter)
	{
		if (object_get(last->unit_index)->object.parent_object_index != NONE)
		{
			unit_exit_seat_end(last->unit_index);
			platform_log("network test: the last player leaves the vehicle");
		}
		return;
	}
	{
		struct object_iterator vehicles;
		long nearest_index = NONE;
		real nearest_distance = 0.0f;
		real_point3d const *position = &object_get(last->unit_index)->object.position;
		short seat_index;

		object_iterator_new(&vehicles, _object_mask_vehicle, 0);
		while (object_iterator_next(&vehicles))
		{
			real_point3d const *vehicle_position = &object_get(vehicles.index)->object.position;
			real distance = distance_squared3d(position, vehicle_position);

			if (nearest_index == NONE || distance < nearest_distance)
			{
				nearest_index = vehicles.index;
				nearest_distance = distance;
			}
		}
		if (nearest_index == NONE)
		{
			platform_log("network test: no vehicle");
			return;
		}
		for (seat_index = 0; seat_index < unit_definition_get(object_get(nearest_index)->definition_index)->unit.seats.count; seat_index++)
		{
			if (unit_seat_is_driver(nearest_index, seat_index) &&
				unit_enter_seat(last->unit_index, nearest_index, seat_index))
			{
				platform_log("network test: the last player drives vehicle %lx", nearest_index);
				return;
			}
		}
		platform_log("network test: the last player cannot drive vehicle %lx", nearest_index);
	}
}

/* debug.network_test_name: the name the machine's first player takes (its
profile's, which it joins with), in ASCII */
static void network_test_set_name(
	void)
{
	char const *name = config_string("debug.network_test_name");
	struct player_profile profile;
	short index;

	if (!name || !name[0])
		return;
	player_ui_get_active_player_profile(0, &profile);
	for (index = 0; name[index] && index < MAXIMUM_PLAYER_PROFILE_NAME_LENGTH - 1; index++)
		profile.player_name[index] = (wchar_t)(unsigned char)name[index];
	profile.player_name[index] = 0;
	player_ui_set_active_player_profile(0, player_ui_get_active_player_profile_index(0), &profile);
	platform_log("network test: this machine's player is named %s", name);
}

/* debug.network_test_chat: the host seats the last two players in one
vehicle, the nearest to the first of them with seats for both (any, a map's
own), for the vehicle chat (two joining machines' players, of one team: they
start near each other) */
static void network_test_seat_together(
	void)
{
	struct data_iterator iterator;
	struct player_datum *player;
	struct object_iterator vehicles;
	long units[2];
	long best_index = NONE;
	real best_distance = 0.0f;
	short count = 0;
	short index;

	data_iterator_new(&iterator, player_data);
	while ((player = (struct player_datum *)data_iterator_next(&iterator)) != NULL)
	{
		if (player->unit_index == NONE)
			continue;
		units[0] = count ? units[1] : NONE;
		units[1] = player->unit_index;
		count++;
	}
	if (count < 2)
		return;
	object_iterator_new(&vehicles, _object_mask_vehicle, 0);
	while (object_iterator_next(&vehicles))
	{
		real distance;

		if (unit_definition_get(object_get(vehicles.index)->definition_index)->unit.seats.count < 2)
			continue;
		distance = distance_squared3d(&object_get(units[0])->object.position, &object_get(vehicles.index)->object.position);
		if (best_index == NONE || distance < best_distance)
		{
			best_index = vehicles.index;
			best_distance = distance;
		}
	}
	if (best_index == NONE)
	{
		platform_log("network test: no vehicle with two seats");
		return;
	}
	for (index = 0; index < (short)NUMBEROF(units); index++)
	{
		short seat_index;

		for (seat_index = 0;
			seat_index < unit_definition_get(object_get(best_index)->definition_index)->unit.seats.count; seat_index++)
		{
			if (!unit_seat_filled(best_index, seat_index) && unit_enter_seat(units[index], best_index, seat_index))
				break;
		}
	}
	platform_log("network test: the last two players ride vehicle %lx: %s, %s", best_index,
		object_get(units[0])->object.parent_object_index == best_index ? "in" : "out",
		object_get(units[1])->object.parent_object_index == best_index ? "in" : "out");
}

/* debug.network_test_chat: a line typed on the text chat's on-screen
keyboard with the controller's buttons, ticks since it began: each held
for two frames and let go for two, whatever the frame rate (a press of two
ticks falls between two frames of a machine drawing slowly) (the keyboard
opens on the first key, 'q': "gg ez", then D-pad Down to the team, then
Start) */
static void network_test_keyboard_line(
	long ticks)
{
	enum { STEP_FRAMES = 4 };
	static long frames;
	static long last_ticks = -1;
	static unsigned long const steps[] =
	{
		1UL << HALO_CHAT_PAD_UP,
		1UL << HALO_CHAT_PAD_STICK_DOWN,
		1UL << HALO_CHAT_PAD_STICK_RIGHT, 1UL << HALO_CHAT_PAD_STICK_RIGHT, 1UL << HALO_CHAT_PAD_STICK_RIGHT,
		1UL << HALO_CHAT_PAD_STICK_RIGHT,
		1UL << HALO_CHAT_PAD_A, 1UL << HALO_CHAT_PAD_A,
		1UL << HALO_CHAT_PAD_STICK_DOWN, 1UL << HALO_CHAT_PAD_STICK_DOWN,
		1UL << HALO_CHAT_PAD_A,
		1UL << HALO_CHAT_PAD_STICK_UP, 1UL << HALO_CHAT_PAD_STICK_UP, 1UL << HALO_CHAT_PAD_STICK_UP,
		1UL << HALO_CHAT_PAD_STICK_LEFT, 1UL << HALO_CHAT_PAD_STICK_LEFT,
		1UL << HALO_CHAT_PAD_A,
		1UL << HALO_CHAT_PAD_STICK_DOWN, 1UL << HALO_CHAT_PAD_STICK_DOWN,
		1UL << HALO_CHAT_PAD_STICK_LEFT, 1UL << HALO_CHAT_PAD_STICK_LEFT,
		1UL << HALO_CHAT_PAD_A,
		1UL << HALO_CHAT_PAD_DOWN,
		1UL << HALO_CHAT_PAD_START,
	};
	long step;

	/* (frames since it began: again for the next game's line) */
	if (ticks < last_ticks)
		frames = 0;
	last_ticks = ticks;
	step = frames / STEP_FRAMES;
	frames++;
	if (step >= (long)NUMBEROF(steps))
	{
		/* (after the line, let go: a held key would move nothing more) */
		if (step == (long)NUMBEROF(steps))
			test_input_press(0);
		return;
	}
	test_input_press((frames - 1) % STEP_FRAMES < STEP_FRAMES / 2 ? steps[step] : 0);
}

/* debug.network_test_chat: the last player (absolute index; NONE), whom the
host mutes; whether it is this machine's; and the first player's machine
(the network's index: the host's), which the last player's mutes */
static short network_test_last_player_index(
	void)
{
	struct data_iterator iterator;
	short last = NONE;

	data_iterator_new(&iterator, player_data);
	while (data_iterator_next(&iterator) != NULL)
		last = (short)DATUM_INDEX_TO_ABSOLUTE_INDEX(iterator.datum_index);
	return last;
}

static boolean network_test_last_player_local(
	void)
{
	struct data_iterator iterator;
	struct player_datum *player;
	struct player_datum *last = NULL;

	data_iterator_new(&iterator, player_data);
	while ((player = (struct player_datum *)data_iterator_next(&iterator)) != NULL)
		last = player;
	return last && last->local_player_index != NONE;
}

static long network_test_first_player_machine(
	void)
{
	struct data_iterator iterator;
	struct player_datum *player;

	data_iterator_new(&iterator, player_data);
	player = (struct player_datum *)data_iterator_next(&iterator);
	return player ? player->network_player_data.machine_index : NONE;
}

/* debug.network_test_chat: the last player's name, in ASCII (the host
mutes it) */
static void network_test_last_player_name(
	char *name,
	short size)
{
	struct data_iterator iterator;
	struct player_datum *player;
	struct player_datum *last = NULL;
	short index;

	data_iterator_new(&iterator, player_data);
	while ((player = (struct player_datum *)data_iterator_next(&iterator)) != NULL)
		last = player;
	for (index = 0; last && index < size - 1 && index < (short)NUMBEROF(last->name) && last->name[index]; index++)
		name[index] = player_name_character_ascii(last->name[index]);
	name[index] = 0;
}

/* both machines stand the last player at the driver's entrance of the
nearest vehicle no one rides (a joining machine's player then holds the
action button, as a player getting in does) */
static void network_test_vehicle_approach(
	void)
{
	struct data_iterator iterator;
	struct player_datum *player;
	struct player_datum *last = NULL;
	struct object_iterator vehicles;
	long nearest_index = NONE;
	real nearest_distance = 0.0f;

	data_iterator_new(&iterator, player_data);
	while ((player = (struct player_datum *)data_iterator_next(&iterator)) != NULL)
		last = player;
	if (!last || last->unit_index == NONE || object_get(last->unit_index)->object.parent_object_index != NONE)
		return;
	object_iterator_new(&vehicles, _object_mask_vehicle, 0);
	while (object_iterator_next(&vehicles))
	{
		struct unit_datum *vehicle = unit_get(vehicles.index);
		/* (the same vehicle on every machine: the lowest index) */
		real distance = (real)DATUM_INDEX_TO_ABSOLUTE_INDEX(vehicles.index);

		if (vehicle->unit.driver_object_index != NONE || TEST_FLAG(vehicle->object.damage_flags, _object_dead_bit))
			continue;
		if (nearest_index == NONE || distance < nearest_distance)
		{
			nearest_index = vehicles.index;
			nearest_distance = distance;
		}
	}
	if (nearest_index != NONE)
	{
		short seat_index;

		for (seat_index = 0; seat_index < unit_definition_get(object_get(nearest_index)->definition_index)->unit.seats.count;
			seat_index++)
		{
			real_point3d entrance;
			real_point3d seat;

			if (unit_seat_is_driver(nearest_index, seat_index) &&
				unit_get_seat_entrance_point(last->unit_index, nearest_index, seat_index, &entrance, &seat, NULL))
			{
				object_set_position(last->unit_index, &entrance, NULL, NULL);
				platform_log("network test: the last player stands at vehicle %lx's driver's entrance (%.2f %.2f %.2f)",
					nearest_index, entrance.x, entrance.y, entrance.z);
				return;
			}
		}
	}
	platform_log("network test: no vehicle to get into");
}

/* the host gives the last player a second weapon, lying about */
static void network_test_second_weapon(
	void)
{
	struct data_iterator iterator;
	struct player_datum *player;
	struct player_datum *last = NULL;
	struct object_iterator weapons;

	data_iterator_new(&iterator, player_data);
	while ((player = (struct player_datum *)data_iterator_next(&iterator)) != NULL)
		last = player;
	if (!last || last->unit_index == NONE)
		return;
	object_iterator_new(&weapons, _object_mask_weapon, 0);
	while (object_iterator_next(&weapons))
	{
		struct object_datum *weapon = object_get(weapons.index);
		struct unit_datum *unit = unit_get(last->unit_index);
		long current_index = unit->unit.weapon_object_indices[0];

		if (weapon->object.parent_object_index == NONE && TEST_FLAG(weapon->object.flags, _object_connected_to_map_bit) &&
			(current_index == NONE || object_get(current_index)->definition_index != weapon->definition_index) &&
			unit_add_weapon_to_inventory(last->unit_index, weapons.index, TRUE))
		{
			platform_log("network test: the last player takes a second weapon (%lx)", weapon->definition_index);
			/* (and camouflage, as a powerup gives) */
			player_handle_powerup(DATUM_INDEX_NEW(last - (struct player_datum *)player_data->data, last->identifier),
				_player_powerup_active_camouflage, 10 * TICKS_PER_SECOND);
			return;
		}
	}
}

/* debug.network_test_cues: from the first tick every player waited for has a
unit (logged, cues or not: where a test's seconds start), each line at its
tick (the host's, or a game of one machine: a client runs none) */
static void network_test_run_cues(
	void)
{
	if (network_test.mode != _network_test_host && network_test.mode != _network_test_log)
		return;
	if (network_test.cues_start == NONE)
	{
		struct data_iterator iterator;
		struct player_datum *player;
		long players = 0;

		data_iterator_new(&iterator, player_data);
		while ((player = (struct player_datum *)data_iterator_next(&iterator)) != NULL)
			players += player->unit_index != NONE;
		if (players < network_test.cue_players)
			return;
		network_test.cues_start = game_time_get();
		platform_log("network test: cues from tick %ld", network_test.cues_start);
	}
	while (network_test.next_cue < network_test.cue_count &&
		game_time_get() >= network_test.cues_start + network_test.cues[network_test.next_cue].tick)
	{
		platform_log("network test: cue at tick %ld: %s", game_time_get(), network_test.cues[network_test.next_cue].line);
		hs_compile_and_evaluate(network_test.cues[network_test.next_cue].line);
		network_test.next_cue++;
	}
}

/* both machines stand the last player on the first weapon lying about that
it does not carry (the same object on both: a client's own player is where
it has it, within a tolerance) */
static void network_test_pickup(
	void)
{
	struct data_iterator iterator;
	struct player_datum *player;
	struct player_datum *last = NULL;
	struct object_iterator weapons;
	long nearest_index = NONE;
	real nearest_distance = 0.0f;
	struct unit_datum *unit;

	data_iterator_new(&iterator, player_data);
	while ((player = (struct player_datum *)data_iterator_next(&iterator)) != NULL)
		last = player;
	if (!last || last->unit_index == NONE)
		return;
	unit = unit_get(last->unit_index);
	object_iterator_new(&weapons, _object_mask_weapon, 0);
	while (object_iterator_next(&weapons))
	{
		struct object_datum *weapon = object_get(weapons.index);
		short slot;
		boolean carried = FALSE;
		real distance;

		if (weapon->object.parent_object_index != NONE ||
			!TEST_FLAG(weapon->object.flags, _object_connected_to_map_bit))
		{
			continue;
		}
		for (slot = 0; slot < MAXIMUM_WEAPONS_PER_UNIT; slot++)
		{
			long carried_index = unit->unit.weapon_object_indices[slot];

			carried |= carried_index != NONE && object_get(carried_index)->definition_index == weapon->definition_index;
		}
		if (network_test.pickup_weapon[0] && !strstr(tag_get_name(weapon->definition_index), network_test.pickup_weapon))
			continue;
		distance = (real)DATUM_INDEX_TO_ABSOLUTE_INDEX(weapons.index);
		if (!carried && (nearest_index == NONE || distance < nearest_distance))
		{
			nearest_index = weapons.index;
			nearest_distance = distance;
		}
	}
	if (nearest_index != NONE)
	{
		real_point3d position = object_get(nearest_index)->object.position;

		position.z += 0.1f;
		object_set_position(last->unit_index, &position, NULL, NULL);
		platform_log("network test: the last player stands on weapon %lx (%lx)", nearest_index,
			object_get(nearest_index)->definition_index);
	}
}

/* (a probe: halo --probe) the first game found, printed as JSON on the
standard output, and the end; when given_up, that none was. With an
invite (p2p.c joins the command line's), the invite's host's game alone:
the tunnel's peers are its host only, and a game on the LAN is another */
static void network_test_probe_answer(
	boolean given_up)
{
	static char const *const engines[] = { "none", "ctf", "slayer", "oddball", "king", "race" };
	struct network_advertised_game_summary game;
	long index;
	int stage;
	boolean invited = p2p_join_progress(&stage, NULL, 0) != P2P_JOIN_NONE;

	for (index = 0; index < PROBE_MAXIMUM_GAMES; index++)
	{
		char name[2 * NUMBEROF(game.name) + 1];
		char map[2 * sizeof(game.map_name) + 1];
		char const *last;
		long character, length = 0;

		if (!network_game_client_port_advertised_game_summary(index, &game) ||
			(invited && !menu_advertised_game_from_peer(index)))
		{
			continue;
		}
		/* (printable ASCII, quotes and backslashes escaped) */
		for (character = 0; character < (long)NUMBEROF(game.name) && game.name[character]; character++)
		{
			char ascii = game.name[character] >= 0x20 && game.name[character] < 0x7F ? (char)game.name[character] : '?';

			if (ascii == '"' || ascii == '\\')
				name[length++] = '\\';
			name[length++] = ascii;
		}
		name[length] = 0;
		/* (the map's name, the last part of its path) */
		last = strrchr(game.map_name, '\\');
		last = last ? last + 1 : game.map_name;
		for (length = 0, character = 0; last[character]; character++)
		{
			char ascii = (unsigned char)last[character] >= 0x20 && (unsigned char)last[character] < 0x7F ?
				last[character] : '?';

			if (ascii == '"' || ascii == '\\')
				map[length++] = '\\';
			map[length++] = ascii;
		}
		map[length] = 0;
		printf("{\"ok\":true,\"name\":\"%s\",\"map\":\"%s\",\"gametype\":\"%s\",\"players\":%d,"
			"\"max_players\":%d,\"teams\":%s,\"in_progress\":%s,\"open\":%s,\"dedicated\":%s,"
			"\"network_version\":%u,\"kind\":%u}\n", name, map,
			game.engine_type >= 0 && game.engine_type < (short)NUMBEROF(engines) ? engines[game.engine_type] : "none",
			game.player_count, game.maximum_player_count, game.has_teams ? "true" : "false",
			game.in_progress ? "true" : "false", game.open ? "true" : "false", game.dedicated ? "true" : "false",
			(unsigned)game.version, (unsigned)game.kind);
		fflush(stdout);
		exit(0);
	}
	if (given_up)
	{
		printf("{\"ok\":false,\"error\":\"no game answered in %d seconds%s\"}\n", PROBE_SECONDS,
			invited ? " from the invite's host" : "");
		fflush(stdout);
		exit(1);
	}
}

/* ---------- input tape */

/* the console's tape_record, tape_stop and tape_play <file> (input_tape_command) and debug.input_tape: "record:<file>" writes each local player's input of every tick (from tick N with
"record@N:<file>"), with the pose the player started it from; "play:<file>" runs the game fast to that tick (a local or
hosted game only), puts the player back in the pose and gives the input back at the same ticks. A replay matches the
recording only as far as the game is the same: map, difficulty, one machine, no other input before the start */
enum
{
	INPUT_TAPE_MAXIMUM = 200000,
	INPUT_TAPE_PATH_LENGTH = 512,
	/* (a local game runs 7 ticks in a frame at most: slowing down this far ahead of the start cannot overshoot it) */
	INPUT_TAPE_SLOW_AHEAD = 8,
	INPUT_TAPE_HEADER_LENGTH = 1024,
	/* (the clock of a game not yet started: any game's ticks are before it) */
	INPUT_TAPE_NEVER = 0x7fffffff
};

struct input_tape_entry
{
	long key;
	unsigned char bytes[sizeof(struct player_action)];
};

/* the vehicle a player was in, and where it was and how it moved: found again by its index (a game of the same map
and ticks gives it the same), else by its kind and nearest place */
struct input_tape_vehicle
{
	long present;
	long object_index;
	long definition_index;
	long seat_index;
	struct _vehicle_datum state;
	real_point3d position;
	real_vector3d forward;
	real_vector3d up;
	real_vector3d velocity;
	real_vector3d angular_velocity;
};

/* a weapon the player carried: its kind and how much it had left (by slot, as the unit carries them) */
struct input_tape_weapon
{
	long definition_index;
	short rounds_total[2];
	short rounds_loaded[2];
	real heat;
	real age;
};

/* the motion a player's body had besides its velocity: whether it was in the air or slipping, landing, crouched */
struct input_tape_momentum
{
	unsigned long flags;
	char landing_recovery_counter;
	char landing_recovery_time;
	char state;
	char airborne_ticks;
	char slipping_ticks;
	char stop_ticks;
	char jump_recovery_timer;
	short landing;
	real crouch;
	real bank;
	real_plane3d ground_plane;
	real_vector3d angular_velocity;
};

/* what the player carried: the weapons, which was in hand, and the grenades */
struct input_tape_loadout
{
	struct input_tape_weapon weapons[MAXIMUM_WEAPONS_PER_UNIT];
	short current_weapon;
	char grenade_counts[NUMBER_OF_UNIT_GRENADE_TYPES];
	char current_grenade;
};

struct input_tape_pose
{
	real_point3d position;
	real_vector3d forward;
	real_vector3d up;
	real_vector3d velocity;
	real_vector3d aiming;
	real_vector3d looking;
	struct input_tape_vehicle vehicle;
	struct input_tape_loadout loadout;
	struct input_tape_momentum momentum;
	unsigned long random_seed;
};

static struct
{
	boolean armed;
	boolean recording;
	boolean playing;
	boolean ended;
	boolean skipping;
	boolean waiting;
	boolean record_auto;
	char segment_map[2*INPUT_TAPE_PATH_LENGTH];
	char path[INPUT_TAPE_PATH_LENGTH];
	char last_path[INPUT_TAPE_PATH_LENGTH];
	char header[INPUT_TAPE_HEADER_LENGTH];
	FILE *file;
	long record_from;
	long start_tick;
	long last_tick;
	real saved_speed;
	long last_key[MAXIMUM_LOCAL_PLAYERS];
	boolean started[MAXIMUM_LOCAL_PLAYERS];
	boolean pose_valid[MAXIMUM_LOCAL_PLAYERS];
	boolean pose_applied[MAXIMUM_LOCAL_PLAYERS];
	long pose_tick[MAXIMUM_LOCAL_PLAYERS];
	struct input_tape_pose pose[MAXIMUM_LOCAL_PLAYERS];
	long count;
} input_tape;

static struct input_tape_entry input_tape_entries[INPUT_TAPE_MAXIMUM];

/* a checkpoint every INPUT_TAPE_CHECK_EVERY ticks: the game's random seed and a hash of where the vehicles and bodies are,
to tell on playback where it stops being the recorded game, and whether the random numbers or the world went first */
enum
{
	INPUT_TAPE_CHECK_MAXIMUM = 40000,
	INPUT_TAPE_CHECK_EVERY = 15
};

struct input_tape_check
{
	long tick;
	unsigned int seed;
	unsigned int world;
};

static struct input_tape_check input_tape_checks[INPUT_TAPE_CHECK_MAXIMUM];
static long input_tape_check_count;
static long input_tape_check_last = NONE;
static boolean input_tape_desync_reported;

/* a tape's order: the tick, then the local player */
static long input_tape_key(
	long tick,
	short local_player_index)
{
	return tick*MAXIMUM_LOCAL_PLAYERS + local_player_index;
}

static void input_tape_hex_write(
	FILE *file,
	void const *data,
	size_t size)
{
	unsigned char const *bytes = (unsigned char const *)data;
	size_t index;

	for (index = 0; index < size; index++)
		fprintf(file, "%02x", (unsigned)bytes[index]);
	fprintf(file, "\n");
}

static int input_tape_hex(
	char digit)
{
	if (digit >= '0' && digit <= '9')
		return digit - '0';
	if (digit >= 'a' && digit <= 'f')
		return digit - 'a' + 10;
	return NONE;
}

/* FALSE for hex that is not exactly size bytes */
static boolean input_tape_hex_read(
	char const *text,
	void *data,
	size_t size)
{
	unsigned char *bytes = (unsigned char *)data;
	size_t index;

	for (index = 0; index < size; index++)
	{
		int high = input_tape_hex(text[2*index]);
		int low = high == NONE ? NONE : input_tape_hex(text[2*index + 1]);

		if (low == NONE)
			return FALSE;
		bytes[index] = (unsigned char)(high*16 + low);
	}
	return text[2*size] == '\n' || text[2*size] == '\r' || text[2*size] == '\0';
}

static void input_tape_line_write(
	FILE *file,
	long key,
	struct player_action const *action)
{
	fprintf(file, "%ld ", key);
	input_tape_hex_write(file, action, sizeof(struct player_action));
}

/* FALSE for a line that is not one of input_tape_line_write's */
static boolean input_tape_line_read(
	char const *line,
	long *key,
	unsigned char *bytes)
{
	int offset = 0;

	if (sscanf(line, "%ld %n", key, &offset) != 1 || *key < 0)
		return FALSE;
	return input_tape_hex_read(line + offset, bytes, sizeof(struct player_action));
}

static void input_tape_pose_write(
	FILE *file,
	short local_player_index,
	long tick,
	struct input_tape_pose const *pose)
{
	fprintf(file, "pose %d %ld ", (int)local_player_index, tick);
	input_tape_hex_write(file, pose, sizeof(struct input_tape_pose));
}

static boolean input_tape_pose_read(
	char const *line,
	short *local_player_index,
	long *tick,
	struct input_tape_pose *pose)
{
	int player;
	int offset = 0;

	if (sscanf(line, "pose %d %ld %n", &player, tick, &offset) != 2 || player < 0 || player >= MAXIMUM_LOCAL_PLAYERS ||
		*tick < 0)
	{
		return FALSE;
	}
	*local_player_index = (short)player;
	return input_tape_hex_read(line + offset, pose, sizeof(struct input_tape_pose));
}

static unsigned int input_tape_world_hash(
	void)
{
	unsigned int hash = 2166136261u;
	struct object_iterator iterator;

	object_iterator_new(&iterator, _object_mask_vehicle | _object_mask_biped, 0);
	while (object_iterator_next(&iterator))
	{
		struct object_datum *object = object_get(iterator.index);
		unsigned int words[6];
		short index;

		csmemcpy(&words[0], &object->object.position, 3*sizeof(unsigned int));
		csmemcpy(&words[3], &object->object.forward, 3*sizeof(unsigned int));
		for (index = 0; index < 6; index++)
			hash = (hash ^ words[index])*16777619u;
	}
	return hash;
}

static void input_tape_load(
	void)
{
	FILE *file = fopen(input_tape.path, "r");
	char line[INPUT_TAPE_HEADER_LENGTH + 16];

	input_tape.playing = FALSE;
	if (!file)
	{
		error(_error_silent, "input_tape: cannot open '%s'", input_tape.path);
		return;
	}
	while (fgets(line, sizeof(line), file))
	{
		struct input_tape_entry *entry = &input_tape_entries[input_tape.count];
		short player;
		long tick;
		boolean good;

		if (!strncmp(line, "tape ", 5))
		{
			snprintf(input_tape.header, sizeof(input_tape.header), "%s", line + 5);
			input_tape.header[strcspn(input_tape.header, "\r\n")] = '\0';
			continue;
		}
		if (!strncmp(line, "check ", 6))
		{
			struct input_tape_check *check = &input_tape_checks[input_tape_check_count];

			if (input_tape_check_count < INPUT_TAPE_CHECK_MAXIMUM &&
				sscanf(line, "check %ld %x %x", &check->tick, &check->seed, &check->world) == 3)
			{
				input_tape_check_count++;
				continue;
			}
			good = FALSE;
		}
		else if (!strncmp(line, "pose ", 5))
		{
			struct input_tape_pose pose;

			good = input_tape_pose_read(line, &player, &tick, &pose);
			if (good)
			{
				input_tape.pose[player] = pose;
				input_tape.pose_tick[player] = tick;
				input_tape.pose_valid[player] = TRUE;
			}
		}
		else
		{
			good = input_tape.count < INPUT_TAPE_MAXIMUM && input_tape_line_read(line, &entry->key, entry->bytes) &&
				(input_tape.count == 0 || entry->key > entry[-1].key);
			if (good)
				input_tape.count++;
		}
		if (!good)
		{
			error(_error_silent, "input_tape: '%s' is malformed or too long after %ld inputs", input_tape.path,
				input_tape.count);
			input_tape.count = 0;
			break;
		}
	}
	fclose(file);
	input_tape.playing = input_tape.count > 0;
	if (input_tape.playing)
	{
		short player;

		input_tape.start_tick = input_tape_entries[0].key/MAXIMUM_LOCAL_PLAYERS;
		for (player = 0; player < MAXIMUM_LOCAL_PLAYERS; player++)
		{
			/* (the poses are of a tick of the tape's first) */
			if (input_tape.pose_valid[player] && input_tape.pose_tick[player] < input_tape.start_tick)
				input_tape.start_tick = input_tape.pose_tick[player];
		}
		error(_error_silent, "input_tape: playing %ld inputs from tick %ld of '%s'", input_tape.count,
			input_tape.start_tick, input_tape.path);
	}
}

static void input_tape_arm(
	void)
{
	char const *setting = config_string("debug.input_tape");
	char const *path = NULL;
	long from = 0;
	int offset = 0;

	input_tape.armed = TRUE;
	if (strlen(setting) >= INPUT_TAPE_PATH_LENGTH + 16)
		setting = "";
	if (!strncmp(setting, "record:", 7))
		path = setting + 7;
	else if (sscanf(setting, "record@%ld:%n", &from, &offset) == 1 && from >= 0 && offset > 0)
		path = setting + offset;
	if (path)
	{
		input_tape.recording = TRUE;
		input_tape.record_from = from;
		snprintf(input_tape.path, sizeof(input_tape.path), "%s", path);
	}
	else if (!strncmp(setting, "play:", 5))
	{
		snprintf(input_tape.path, sizeof(input_tape.path), "%s", setting + 5);
		input_tape_load();
	}
	else if (setting[0])
		error(_error_silent, "input_tape: setting must be record:<file>, record@<tick>:<file> or play:<file>");
}

/* the map's file name part and the game's type, as the recording's name and header give them */
static void input_tape_component(
	char const *text,
	char *name,
	size_t size)
{
	char const *component = text;
	char const *cursor;
	size_t used = 0;

	for (cursor = text; *cursor; cursor++)
	{
		if (*cursor == '/' || *cursor == '\\')
			component = cursor + 1;
	}
	for (cursor = component; *cursor && used + 1 < size; cursor++)
	{
		char character = *cursor >= 'A' && *cursor <= 'Z' ? (char)(*cursor - 'A' + 'a') : *cursor;

		name[used++] = (character >= 'a' && character <= 'z') || (character >= '0' && character <= '9') ||
			character == '_' || character == '-' ? character : '-';
	}
	name[used] = '\0';
}

/* the name the running game's map was loaded by: a multiplayer map's is not the solo map name's (which keeps the last
campaign level's) */
static char const *input_tape_scenario(
	void)
{
	return global_scenario_get()->type == _scenario_type_multiplayer ? main_get_multiplayer_map_name() :
		main_get_map_name();
}

/* how many local players the game has */
static short input_tape_local_players(
	void)
{
	short player;
	short count = 0;

	for (player = 0; player < MAXIMUM_LOCAL_PLAYERS; player++)
	{
		if (local_player_get_player_index(player) != NONE)
			count++;
	}
	return count;
}

/* whether the map is a multiplayer one: a game engine can be set up on a campaign map too (a game variant outlives its
game), and a variant given to a campaign map starts it as a multiplayer game that has no spawn points */
static boolean input_tape_multiplayer(
	void)
{
	return game_engine_running() && global_scenario_get()->type == _scenario_type_multiplayer;
}

static void input_tape_description(
	char *map,
	size_t map_size,
	char *gametype,
	size_t gametype_size)
{
	char const *scenario = input_tape_scenario();

	input_tape_component(scenario ? scenario : "", map, map_size);
	snprintf(gametype, gametype_size, "%s", !input_tape_multiplayer() ? "campaign" :
		profile_console_engine_gametype(TRUE, game_engine_get_variant()->game_engine_index));
}

/* a Custom Edition map's version (the first 32 bits of its file's hash: maps of one name differ by it, and the game's
own maps have none, 0) */
static unsigned long input_tape_map_version(
	char const *scenario)
{
	return custom_edition_level_name(scenario) ? custom_edition_map_version(scenario) : 0;
}

/* the line after "tape ": what a person reads first, then what tape_play starts the game with (the variant of a
multiplayer game, as the bytes the game keeps), the scenario last (it is the rest of the line) */
static void input_tape_header_make(
	long from_tick)
{
	char map[64];
	char gametype[32];
	char date[32];
	char variant[2*sizeof(struct game_variant) + 1] = "";
	char const *scenario = input_tape_scenario();
	time_t now = time(NULL);
	struct tm *local = localtime(&now);

	input_tape_description(map, sizeof(map), gametype, sizeof(gametype));
	if (!local || !strftime(date, sizeof(date), "%Y-%m-%d %H:%M:%S", local))
		snprintf(date, sizeof(date), "unknown");
	if (input_tape_multiplayer())
	{
		unsigned char const *bytes = (unsigned char const *)game_engine_get_variant();
		size_t index;

		for (index = 0; index < sizeof(struct game_variant); index++)
			snprintf(variant + 2*index, 3, "%02x", (unsigned)bytes[index]);
	}
	snprintf(input_tape.header, sizeof(input_tape.header),
		"date=\"%s\" map=%s gametype=%s from_tick=%ld map_version=%08lx difficulty=%d players=%d variant=%s scenario=%s",
		date, map, gametype, from_tick, input_tape_map_version(scenario ? scenario : ""), (int)main_get_difficulty(),
		(int)input_tape_local_players(), variant, scenario ? scenario : "");
}

/* a field of the header: the text after " name=" up to a space, or to the end of the line for the scenario */
static boolean input_tape_field(
	char const *header,
	char const *name,
	boolean to_end,
	char *value,
	size_t size)
{
	char key[24];
	char const *found;
	size_t length;

	snprintf(key, sizeof(key), " %s=", name);
	found = strstr(header, key);
	if (!found)
		return FALSE;
	found += strlen(key);
	length = to_end ? strlen(found) : strcspn(found, " ");
	if (length >= size)
		return FALSE;
	csmemcpy(value, found, length);
	value[length] = '\0';
	return TRUE;
}

/* a tape's path as the disk has it, for what the player is told */
static char const *input_tape_host_path(
	char const *path)
{
	static char host[1024];

	platform_translate_path(path, host, sizeof(host));
	return host;
}

static void input_tape_stop(
	char const *why)
{
	if (input_tape.file)
		fclose(input_tape.file);
	input_tape.file = NULL;
	input_tape.recording = FALSE;
	error(_error_silent, "input_tape: recording of '%s' stopped (%s)", input_tape.path, why);
}

/* a new game's ticks count from the start again, so the tape starts again with it */
static void input_tape_restart(
	void)
{
	short player;

	input_tape.ended = FALSE;
	input_tape.skipping = FALSE;
	input_tape_check_last = NONE;
	input_tape_desync_reported = FALSE;
	for (player = 0; player < MAXIMUM_LOCAL_PLAYERS; player++)
	{
		input_tape.last_key[player] = NONE;
		input_tape.started[player] = FALSE;
		input_tape.pose_applied[player] = FALSE;
		if (input_tape.recording)
			input_tape.pose_valid[player] = FALSE;
	}
	if (input_tape.recording)
	{
		if (input_tape.file)
			fclose(input_tape.file);
		input_tape.file = fopen(input_tape.path, "w");
		if (!input_tape.file)
		{
			error(_error_silent, "input_tape: cannot write '%s'", input_tape.path);
			console_printf(FALSE, "tape: cannot write %s, recording stopped", input_tape_host_path(input_tape.path));
			input_tape.recording = FALSE;
			input_tape.record_auto = FALSE;
		}
		else
		{
			input_tape_header_make(input_tape.record_from);
			fprintf(input_tape.file, "tape %s\n", input_tape.header);
		}
	}
}

/* the local player's body, FALSE while it has none (or is carried by something that is no vehicle) */
static boolean input_tape_body(
	short local_player_index,
	long *unit_index)
{
	long player_index = local_player_get_player_index(local_player_index);
	struct player_datum *player = player_index != NONE ? player_try_and_get(player_index) : NULL;

	if (!player || player->unit_index == NONE ||
		!object_try_and_get_and_verify_type(player->unit_index, _object_mask_biped))
	{
		return FALSE;
	}
	if (object_get(player->unit_index)->object.parent_object_index != NONE &&
		!object_try_and_get_and_verify_type(object_get(player->unit_index)->object.parent_object_index,
			_object_mask_vehicle))
	{
		return FALSE;
	}
	*unit_index = player->unit_index;
	return TRUE;
}

static void input_tape_pose_take(
	long unit_index,
	struct input_tape_pose *pose)
{
	struct unit_datum *unit = unit_get(unit_index);
	struct biped_datum *biped = (struct biped_datum *)unit;
	short slot;

	csmemset(pose, 0, sizeof(*pose));
	pose->random_seed = get_random_seed();
	pose->momentum.flags = biped->biped.flags;
	pose->momentum.landing_recovery_counter = biped->biped.landing_recovery_counter;
	pose->momentum.landing_recovery_time = biped->biped.landing_recovery_time;
	pose->momentum.state = biped->biped.state;
	pose->momentum.airborne_ticks = biped->biped.airborne_ticks;
	pose->momentum.slipping_ticks = biped->biped.slipping_ticks;
	pose->momentum.stop_ticks = biped->biped.stop_ticks;
	pose->momentum.jump_recovery_timer = biped->biped.jump_recovery_timer;
	pose->momentum.landing = biped->biped.landing;
	pose->momentum.crouch = biped->biped.crouch;
	pose->momentum.bank = biped->biped.bank;
	pose->momentum.ground_plane = biped->biped.ground_plane;
	pose->momentum.angular_velocity = unit->object.angular_velocity;
	pose->position = unit->object.position;
	pose->forward = unit->object.forward;
	pose->up = unit->object.up;
	pose->velocity = unit->object.translational_velocity;
	pose->aiming = unit->unit.aiming_vector;
	pose->looking = unit->unit.looking_vector;
	csmemset(&pose->loadout, 0, sizeof(pose->loadout));
	pose->loadout.current_weapon = unit->unit.current_weapon_index;
	pose->loadout.current_grenade = unit->unit.current_grenade_index;
	for (slot = 0; slot < NUMBER_OF_UNIT_GRENADE_TYPES; slot++)
		pose->loadout.grenade_counts[slot] = unit->unit.grenade_counts[slot];
	for (slot = 0; slot < MAXIMUM_WEAPONS_PER_UNIT; slot++)
	{
		struct weapon_datum *weapon = unit->unit.weapon_object_indices[slot] == NONE ? NULL :
			weapon_try_and_get(unit->unit.weapon_object_indices[slot]);
		short magazine;

		pose->loadout.weapons[slot].definition_index = weapon ? weapon->definition_index : NONE;
		if (!weapon)
			continue;
		for (magazine = 0; magazine < 2; magazine++)
		{
			pose->loadout.weapons[slot].rounds_total[magazine] = weapon->weapon.magazines[magazine].rounds_total;
			pose->loadout.weapons[slot].rounds_loaded[magazine] = weapon->weapon.magazines[magazine].rounds_loaded;
		}
		pose->loadout.weapons[slot].heat = weapon->weapon.heat;
		pose->loadout.weapons[slot].age = weapon->weapon.age;
	}
	csmemset(&pose->vehicle, 0, sizeof(pose->vehicle));
	if (unit->object.parent_object_index != NONE)
	{
		struct object_datum *vehicle = object_get(unit->object.parent_object_index);

		pose->vehicle.present = TRUE;
		pose->vehicle.object_index = unit->object.parent_object_index;
		pose->vehicle.state = vehicle_datum_get(unit->object.parent_object_index)->vehicle;
		pose->vehicle.definition_index = vehicle->definition_index;
		pose->vehicle.seat_index = unit->unit.parent_seat_index;
		pose->vehicle.position = vehicle->object.position;
		pose->vehicle.forward = vehicle->object.forward;
		pose->vehicle.up = vehicle->object.up;
		pose->vehicle.velocity = vehicle->object.translational_velocity;
		pose->vehicle.angular_velocity = vehicle->object.angular_velocity;
	}
}

/* the player's weapons and grenades as they were: new weapons of the recorded kinds with the recorded ammunition, the one
in hand readied again (a weapon that was not the first is drawn, as a player changing to it would) */
static void input_tape_loadout_put(
	long unit_index,
	struct input_tape_loadout const *loadout)
{
	struct unit_datum *unit = unit_get(unit_index);
	boolean first = TRUE;
	boolean same = TRUE;
	short slot;
	short added = 0;
	short in_hand = NONE;

	/* (the weapons a player already carries are the recorded ones in a game of the same start: new ones would be drawn, and
	fire a few ticks after the recorded ones did) */
	for (slot = 0; slot < MAXIMUM_WEAPONS_PER_UNIT; slot++)
	{
		struct weapon_datum *carried = unit->unit.weapon_object_indices[slot] == NONE ? NULL :
			weapon_try_and_get(unit->unit.weapon_object_indices[slot]);

		if ((carried ? carried->definition_index : NONE) != loadout->weapons[slot].definition_index)
			same = FALSE;
	}
	if (same)
	{
		for (slot = 0; slot < MAXIMUM_WEAPONS_PER_UNIT; slot++)
		{
			struct weapon_datum *carried = loadout->weapons[slot].definition_index == NONE ? NULL :
				weapon_get(unit->unit.weapon_object_indices[slot]);
			short magazine;

			if (!carried)
				continue;
			for (magazine = 0; magazine < 2; magazine++)
			{
				carried->weapon.magazines[magazine].rounds_total = loadout->weapons[slot].rounds_total[magazine];
				carried->weapon.magazines[magazine].rounds_loaded = loadout->weapons[slot].rounds_loaded[magazine];
			}
			carried->weapon.heat = loadout->weapons[slot].heat;
			carried->weapon.age = loadout->weapons[slot].age;
		}
		if (unit->unit.current_weapon_index != loadout->current_weapon)
			unit->unit.desired_weapon_index = loadout->current_weapon;
		for (slot = 0; slot < NUMBER_OF_UNIT_GRENADE_TYPES; slot++)
			unit->unit.grenade_counts[slot] = loadout->grenade_counts[slot];
		unit->unit.current_grenade_index = unit->unit.desired_grenade_index = loadout->current_grenade;
		return;
	}
	unit_delete_all_weapons(unit_index);
	for (slot = 0; slot < MAXIMUM_WEAPONS_PER_UNIT; slot++)
	{
		struct input_tape_weapon const *recorded = &loadout->weapons[slot];
		struct object_placement_data placement_data;
		struct weapon_datum *weapon;
		long weapon_index;
		short magazine;

		if (recorded->definition_index == NONE)
			continue;
		object_placement_data_new(&placement_data, recorded->definition_index, NONE);
		weapon_index = object_new(&placement_data);
		if (weapon_index == NONE)
			continue;
		weapon = weapon_get(weapon_index);
		for (magazine = 0; magazine < 2; magazine++)
		{
			weapon->weapon.magazines[magazine].rounds_total = recorded->rounds_total[magazine];
			weapon->weapon.magazines[magazine].rounds_loaded = recorded->rounds_loaded[magazine];
		}
		weapon->weapon.heat = recorded->heat;
		weapon->weapon.age = recorded->age;
		if (!unit_add_weapon_to_inventory(unit_index, weapon_index, first ? 2 : 0))
		{
			object_delete(weapon_index);
			error(_error_silent, "input_tape: the player cannot carry %s", tag_get_name(recorded->definition_index));
			continue;
		}
		first = FALSE;
		if (slot == loadout->current_weapon)
			in_hand = added;
		added++;
	}
	if (in_hand != NONE)
		unit->unit.desired_weapon_index = in_hand;
	for (slot = 0; slot < NUMBER_OF_UNIT_GRENADE_TYPES; slot++)
		unit->unit.grenade_counts[slot] = loadout->grenade_counts[slot];
	unit->unit.current_grenade_index = unit->unit.desired_grenade_index = loadout->current_grenade;
}

/* the recorded vehicle: the object of its index if that is a vehicle of its kind, else the one of the kind nearest to
where it was; NONE if there is none */
static long input_tape_vehicle_find(
	struct input_tape_vehicle const *recorded)
{
	struct object_iterator vehicles;
	long nearest = NONE;
	real nearest_distance = 0.f;

	if (object_try_and_get_and_verify_type(recorded->object_index, _object_mask_vehicle) &&
		object_get(recorded->object_index)->definition_index == recorded->definition_index)
	{
		return recorded->object_index;
	}
	error(_error_silent, "input_tape: vehicle %lx is not the recorded kind, the nearest of the kind is taken",
		recorded->object_index);
	object_iterator_new(&vehicles, _object_mask_vehicle, 0);
	while (object_iterator_next(&vehicles))
	{
		real distance;

		if (object_get(vehicles.index)->definition_index != recorded->definition_index)
			continue;
		distance = distance_squared3d(&object_get(vehicles.index)->object.position, &recorded->position);
		if (nearest == NONE || distance < nearest_distance)
		{
			nearest = vehicles.index;
			nearest_distance = distance;
		}
	}
	return nearest;
}

/* the player's body, or the vehicle it was in put back and the player seated in it as it was; FALSE when the recorded
vehicle is not in this game */
static boolean input_tape_pose_put(
	long unit_index,
	struct input_tape_pose const *pose)
{
	struct unit_datum *unit = unit_get(unit_index);

	input_tape_loadout_put(unit_index, &pose->loadout);
	if (pose->vehicle.present)
	{
		long vehicle_index = input_tape_vehicle_find(&pose->vehicle);
		struct object_datum *vehicle;

		if (vehicle_index == NONE)
			return FALSE;
		vehicle = object_get(vehicle_index);
		if (unit->object.parent_object_index != NONE && unit->object.parent_object_index != vehicle_index)
			unit_exit_seat_end(unit_index);
		object_set_position(vehicle_index, &pose->vehicle.position, &pose->vehicle.forward, &pose->vehicle.up);
		vehicle->object.translational_velocity = pose->vehicle.velocity;
		vehicle->object.angular_velocity = pose->vehicle.angular_velocity;
		vehicle_datum_get(vehicle_index)->vehicle = pose->vehicle.state;
		if (unit->object.parent_object_index != vehicle_index &&
			!unit_enter_seat(unit_index, vehicle_index, (short)pose->vehicle.seat_index))
		{
			error(_error_silent, "input_tape: the player cannot take seat %ld of vehicle %lx", pose->vehicle.seat_index,
				vehicle_index);
			return FALSE;
		}
		unit->unit.aiming_vector = unit->unit.desired_aiming_vector = pose->aiming;
		unit->unit.looking_vector = unit->unit.desired_looking_vector = pose->looking;
		csmemset(&unit->unit.aiming_velocity, 0, sizeof(unit->unit.aiming_velocity));
		csmemset(&unit->unit.looking_velocity, 0, sizeof(unit->unit.looking_velocity));
		return TRUE;
	}
	/* (moving a body that is where it was only disturbs it: its ground, its animation, what it stands on) */
	if (unit->object.parent_object_index == NONE &&
		!memcmp(&unit->object.position, &pose->position, sizeof(pose->position)) &&
		!memcmp(&unit->object.forward, &pose->forward, sizeof(pose->forward)) &&
		!memcmp(&unit->object.up, &pose->up, sizeof(pose->up)) &&
		!memcmp(&unit->object.translational_velocity, &pose->velocity, sizeof(pose->velocity)))
	{
		return TRUE;
	}
	if (unit->object.parent_object_index != NONE)
		unit_exit_seat_end(unit_index);
	object_set_position(unit_index, &pose->position, &pose->forward, &pose->up);
	unit->object.translational_velocity = pose->velocity;
	{
		struct biped_datum *biped = (struct biped_datum *)unit;

		unit->object.angular_velocity = pose->momentum.angular_velocity;
		biped->biped.flags = pose->momentum.flags;
		biped->biped.landing_recovery_counter = pose->momentum.landing_recovery_counter;
		biped->biped.landing_recovery_time = pose->momentum.landing_recovery_time;
		biped->biped.state = pose->momentum.state;
		biped->biped.airborne_ticks = pose->momentum.airborne_ticks;
		biped->biped.slipping_ticks = pose->momentum.slipping_ticks;
		biped->biped.stop_ticks = pose->momentum.stop_ticks;
		biped->biped.jump_recovery_timer = pose->momentum.jump_recovery_timer;
		biped->biped.landing = pose->momentum.landing;
		biped->biped.crouch = pose->momentum.crouch;
		biped->biped.bank = pose->momentum.bank;
		biped->biped.ground_plane = pose->momentum.ground_plane;
	}
	unit->unit.aiming_vector = unit->unit.desired_aiming_vector = pose->aiming;
	unit->unit.looking_vector = unit->unit.desired_looking_vector = pose->looking;
	unit->unit.desired_facing_vector = pose->forward;
	csmemset(&unit->unit.aiming_velocity, 0, sizeof(unit->unit.aiming_velocity));
	csmemset(&unit->unit.looking_velocity, 0, sizeof(unit->unit.looking_velocity));
	return TRUE;
}

/* the game runs fast up to the tape's start (a local or hosted game: a client's clock is the host's), then at the speed
it had */
static void input_tape_pace(
	long tick)
{
	if (input_tape.start_tick - tick > INPUT_TAPE_SLOW_AHEAD)
	{
		if (!input_tape.skipping)
		{
			input_tape.skipping = TRUE;
			input_tape.saved_speed = game_time_get_speed();
		}
		game_time_set_speed(100.f);
	}
	else if (input_tape.skipping)
	{
		input_tape.skipping = FALSE;
		game_time_set_speed(input_tape.saved_speed);
	}
}

/* the tick each press of the trigger starts at (debug.txt), in a recording and in its playback: the two lists of ticks are
what to compare when a replay differs */
static void input_tape_note_trigger(
	long tick,
	short local_player_index,
	struct player_action const *action)
{
	static real last[MAXIMUM_LOCAL_PLAYERS];
	static long last_tick[MAXIMUM_LOCAL_PLAYERS];

	if (last_tick[local_player_index] == tick)
		return;
	last_tick[local_player_index] = tick;
	if (action->primary_trigger > 0.f && last[local_player_index] <= 0.f)
	{
		error(_error_silent, "input_tape: %s player %d trigger at tick %ld, seed %08x, world %08x",
			input_tape.recording ? "recording" : "playing", (int)local_player_index, tick, (unsigned int)get_random_seed(),
			input_tape_world_hash());
	}
	last[local_player_index] = action->primary_trigger;
}

/* what follows the command word an expression starts with (past spaces and an opening parenthesis, as the console and
init.txt give it), else NULL */
static char const *input_tape_word(
	char const *expression,
	char const *word)
{
	size_t length = strlen(word);
	size_t index;

	while (*expression == ' ' || *expression == '\t' || *expression == '(')
		expression++;
	for (index = 0; index < length; index++)
	{
		char character = expression[index] >= 'A' && expression[index] <= 'Z' ? (char)(expression[index] - 'A' + 'a') :
			expression[index];

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

/* the tapes folder of the data root, made, as the game names it (d:\tapes: a host path would be taken for a game's, and
d: for the data root) */
static void input_tape_folder(
	char *folder,
	size_t size)
{
	char host[INPUT_TAPE_PATH_LENGTH];

	snprintf(host, sizeof(host), "%s/tapes", platform_data_root());
	posix_make_directory(host);
	snprintf(folder, size, "d:\\tapes");
}

/* the map a recording is of: both names the game keeps (a solo, local or split-screen game's, and a multiplayer one's) */
static void input_tape_map_key(
	char *key,
	size_t size)
{
	snprintf(key, size, "%s|%s", main_get_map_name(), main_get_multiplayer_map_name());
}

/* the numbered tapes of the folder (<number>_<date and time>_<map>_<game type>.tape): the highest number, 0 for none; the
name of the one with number `wanted`, or of the highest for NONE, in `found` (empty when there is none) */
static long input_tape_numbered(
	long wanted,
	char *found,
	size_t size)
{
	char host[INPUT_TAPE_PATH_LENGTH];
	char name[256];
	char highest_name[256] = "";
	long highest = 0;
	void *directory;

	snprintf(host, sizeof(host), "%s/tapes", platform_data_root());
	found[0] = '\0';
	directory = posix_directory_open(host);
	if (!directory)
		return 0;
	while (posix_directory_next(directory, name, sizeof(name)))
	{
		size_t length = strlen(name);
		char *end;
		long number;

		if (name[0] < '0' || name[0] > '9' || length < 6 || strcmp(name + length - 5, ".tape"))
			continue;
		number = strtol(name, &end, 10);
		if (*end != '_')
			continue;
		if (number > highest)
		{
			highest = number;
			snprintf(highest_name, sizeof(highest_name), "%s", name);
		}
		if (number == wanted)
			snprintf(found, size, "%s", name);
	}
	posix_directory_close(directory);
	if (wanted == NONE)
		snprintf(found, size, "%s", highest_name);
	return highest;
}

/* a recording of the game that runs now, from this tick on, into
tapes/<date and time>_<map>_<game type>.tape of the data root */
static void input_tape_segment_begin(
	void)
{
	char folder[INPUT_TAPE_PATH_LENGTH];
	char map[64];
	char gametype[32];
	char stamp[32];
	time_t now = time(NULL);
	struct tm *local = localtime(&now);

	input_tape_folder(folder, sizeof(folder));
	input_tape_description(map, sizeof(map), gametype, sizeof(gametype));
	if (!local || !strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", local))
		snprintf(stamp, sizeof(stamp), "tape");
	input_tape.playing = FALSE;
	input_tape.waiting = FALSE;
	input_tape.recording = TRUE;
	input_tape.record_from = game_time_get();
	input_tape.last_tick = NONE;
	input_tape_map_key(input_tape.segment_map, sizeof(input_tape.segment_map));
	{
		char ignored[8];

		snprintf(input_tape.path, sizeof(input_tape.path), "%s\\%04ld_%s_%s_%s.tape", folder,
			input_tape_numbered(0, ignored, sizeof(ignored)) + 1, stamp, map, gametype);
	}
	snprintf(input_tape.last_path, sizeof(input_tape.last_path), "%s", input_tape.path);
	console_printf(FALSE, "tape: recording %s from tick %ld", input_tape_host_path(input_tape.path), input_tape.record_from);
}

/* tape_record: a recording of this game (the next, from the main menu) and of every game after it, each in a file of its own, until tape_stop */
static void input_tape_command_record(
	void)
{
	if (input_tape.record_auto)
	{
		console_printf(FALSE, "tape: already recording, until tape_stop");
		return;
	}
	input_tape.record_auto = TRUE;
	/* (from the main menu it waits for the next game: network_test_tape_frame begins it) */
	if (game_in_progress() && !main_menu_is_active())
		input_tape_segment_begin();
	console_printf(FALSE, "tape: a file for each map and game, until tape_stop");
}

/* the tape's input of a tick, or NULL */
static struct input_tape_entry const *input_tape_find(
	long key)
{
	long low = 0;
	long high = input_tape.count - 1;

	while (low <= high)
	{
		long middle = (low + high)/2;

		if (input_tape_entries[middle].key == key)
			return &input_tape_entries[middle];
		if (input_tape_entries[middle].key < key)
			low = middle + 1;
		else
			high = middle - 1;
	}
	return NULL;
}

/* the camera between two ticks of a playback: the game draws several frames to a tick, and the controller's angles, which
the camera follows, are set a tick at a time (input_tape_action), so a frame takes the angles part of the way from the
tick the game has run to the next, which the tape already has */
static void input_tape_view_frame(
	void)
{
	long tick = game_time_get();
	real fraction = game_time_get_tick_fraction();
	short player;

	for (player = 0; player < MAXIMUM_LOCAL_PLAYERS; player++)
	{
		struct input_tape_entry const *from = input_tape_find(input_tape_key(tick - 1, player));
		struct input_tape_entry const *to = input_tape_find(input_tape_key(tick, player));
		struct player_action before;
		struct player_action after;
		real_euler_angles2d angles;
		real_vector3d facing;
		real turn;

		if (!from || !to)
			continue;
		csmemcpy(&before, from->bytes, sizeof(before));
		csmemcpy(&after, to->bytes, sizeof(after));
		turn = after.desired_facing.yaw - before.desired_facing.yaw;
		while (turn > _pi)
			turn -= 2.f*_pi;
		while (turn < -_pi)
			turn += 2.f*_pi;
		angles.yaw = before.desired_facing.yaw + turn*fraction;
		angles.pitch = before.desired_facing.pitch + (after.desired_facing.pitch - before.desired_facing.pitch)*fraction;
		vector3d_from_euler_angles2d(&facing, &angles);
		player_control_set_facing(player, &facing);
	}
}

/* each frame (main.c): the recording tape_record asks for is of the games, never of the main menu: its file ends when
the main menu loads or the map changes, and the next game's is another */
void network_test_tape_frame(
	boolean main_menu_loaded)
{
	char key[2*INPUT_TAPE_PATH_LENGTH];

	if (input_tape.playing && !input_tape.waiting && !input_tape.skipping && !input_tape.ended && game_in_progress() &&
		!main_menu_loaded && !game_time_get_paused())
	{
		input_tape_view_frame();
	}
	if (!input_tape.record_auto)
		return;
	if (!game_in_progress() || main_menu_loaded)
	{
		if (input_tape.recording)
			input_tape_stop("the main menu");
		return;
	}
	input_tape_map_key(key, sizeof(key));
	if (input_tape.recording && strcmp(key, input_tape.segment_map))
		input_tape_stop("the map changed");
	if (!input_tape.recording)
		input_tape_segment_begin();
}

/* a game on the tape's map (its hash, not its name: maps of one name differ) as the tape was recorded, from a local game or
the main menu; FALSE, and why told, when this machine cannot */
static boolean input_tape_launch(void)
{
	char scenario[128];
	char text[2*sizeof(struct game_variant) + 1];
	char path[256];
	char level[160];
	unsigned long version = 0;
	long difficulty = _game_difficulty_level_normal;
	long players = 1;
	struct game_variant variant;
	boolean has_variant = FALSE;
	short player;
	char const *name;

	if (!input_tape_field(input_tape.header, "scenario", TRUE, scenario, sizeof(scenario)) || !scenario[0])
	{
		console_printf(FALSE, "tape: the tape names no map, start its game yourself");
		return FALSE;
	}
	if (game_connection() == _game_connection_network_client || game_connection() == _game_connection_network_server)
	{
		console_printf(FALSE, "tape: leave the network game first");
		return FALSE;
	}
	if (input_tape_field(input_tape.header, "map_version", FALSE, text, sizeof(text)))
		version = strtoul(text, NULL, 16);
	if (input_tape_field(input_tape.header, "difficulty", FALSE, text, sizeof(text)))
		difficulty = strtol(text, NULL, 10);
	if (input_tape_field(input_tape.header, "players", FALSE, text, sizeof(text)))
		players = strtol(text, NULL, 10);
	if (input_tape_field(input_tape.header, "variant", FALSE, text, sizeof(text)) &&
		input_tape_hex_read(text, &variant, sizeof(struct game_variant)))
	{
		has_variant = TRUE;
	}
	snprintf(level, sizeof(level), "%s", scenario);
	if (custom_edition_level_name(scenario) && version != 0)
	{
		/* (another file of the name, whatever its version: the level is named by that file's own name) */
		if (!custom_edition_map_find_version(scenario, version, path))
		{
			console_printf(FALSE, "tape: no map of %s with the tape's hash %08lx", scenario, version);
			return FALSE;
		}
		name = strrchr(path, '/') > strrchr(path, '\\') ? strrchr(path, '/') : strrchr(path, '\\');
		name = name ? name + 1 : path;
		snprintf(level, sizeof(level), "custom_maps\\%s", name);
		if (strlen(level) > 4 && !strcmp(level + strlen(level) - 4, ".map"))
			level[strlen(level) - 4] = '\0';
	}
	else if (custom_edition_level_name(scenario) && !custom_edition_map_file_present(scenario))
	{
		console_printf(FALSE, "tape: no map %s", scenario);
		return FALSE;
	}
	if (main_menu_is_active())
		ui_widgets_close_all();
	game_connection_set(_game_connection_local);
	main_set_difficulty((short)difficulty);
	player_spawn_count = (short)(players >= 1 && players <= MAXIMUM_LOCAL_PLAYERS ? players : 1);
	for (player = 0; player < player_spawn_count; player++)
		player_ui_set_single_player_local_player_controller(player, player);
	if (has_variant)
		game_set_game_variant(&variant);
	main_set_map_name(level);
	if (main_menu_is_active())
		main_menu_switch_to_single_player();
	return TRUE;
}

/* tape_play <file>: the file of the tapes folder (or a whole path), "last" for the last one recorded; it plays from the
start of the next game */
static void input_tape_command_play(
	char const *name)
{
	char folder[INPUT_TAPE_PATH_LENGTH];
	char file[INPUT_TAPE_PATH_LENGTH];
	size_t length;
	short player;

	snprintf(file, sizeof(file), "%s", name);
	length = strlen(file);
	while (length > 0 && (file[length - 1] == ' ' || file[length - 1] == ')'))
		file[--length] = '\0';
	/* (a number or last: the tape of that number, or the highest) */
	if (!strcmp(file, "last") || !file[0] || strspn(file, "0123456789") == strlen(file))
	{
		char numbered[256];
		boolean last = !strcmp(file, "last") || !file[0];

		input_tape_numbered(last ? NONE : strtol(file, NULL, 10), numbered, sizeof(numbered));
		if (!numbered[0])
		{
			console_printf(FALSE, last ? "tape: no numbered tapes yet" : "tape: no tape number %s", file);
			return;
		}
		snprintf(file, sizeof(file), "%s", numbered);
	}
	input_tape.record_auto = FALSE;
	if (input_tape.recording)
		input_tape_stop("tape_play");
	if (input_tape.skipping)
		game_time_set_speed(input_tape.saved_speed);
	input_tape.skipping = FALSE;
	input_tape_folder(folder, sizeof(folder));
	if (strpbrk(file, ":/\\"))
		snprintf(input_tape.path, sizeof(input_tape.path), "%s", file);
	else
		snprintf(input_tape.path, sizeof(input_tape.path), "%s\\%s%s", folder, file, strchr(file, '.') ? "" : ".tape");
	input_tape.count = 0;
	input_tape_check_count = 0;
	input_tape.header[0] = '\0';
	for (player = 0; player < MAXIMUM_LOCAL_PLAYERS; player++)
		input_tape.pose_valid[player] = FALSE;
	input_tape_load();
	if (!input_tape.playing)
	{
		console_printf(FALSE, "tape: cannot play %s", input_tape.path);
		return;
	}
	input_tape.waiting = TRUE;
	input_tape.last_tick = game_in_progress() && !main_menu_is_active() ? game_time_get() : INPUT_TAPE_NEVER;
	{
		/* (the header's start, for a person: not its variant bytes) */
		char shown[160];
		char const *cut = strstr(input_tape.header, " variant=");

		snprintf(shown, sizeof(shown), "%.*s", cut ? (int)(cut - input_tape.header) : 150, input_tape.header);
		console_printf(FALSE, "tape: %s", shown);
	}
	if (input_tape_launch())
		console_printf(FALSE, "tape: starting its game, playing from tick %ld", input_tape.start_tick);
	else
		console_printf(FALSE, "tape: plays from tick %ld of the next game you start, on that map", input_tape.start_tick);
}

/* tape_list: the last ten numbered tapes */
static void input_tape_command_list(
	void)
{
	char name[256];
	long highest = input_tape_numbered(0, name, sizeof(name));
	long number;
	long shown = 0;

	for (number = highest; number >= 1 && shown < 10; number--)
	{
		input_tape_numbered(number, name, sizeof(name));
		if (name[0])
		{
			console_printf(FALSE, "tape: %s", name);
			shown++;
		}
	}
	if (!shown)
		console_printf(FALSE, "tape: no numbered tapes yet");
}

/* tape_record, tape_stop and tape_play <file> (hs.c, before the script compiler): TRUE when the expression was one of
them */
boolean network_test_tape_command(
	char const *expression)
{
	char const *rest;

	if (!input_tape.armed)
	{
		input_tape_arm();
		input_tape.last_tick = NONE;
	}
	if (input_tape_word(expression, "tape_record"))
	{
		input_tape_command_record();
		return TRUE;
	}
	if (input_tape_word(expression, "tape_stop"))
	{
		if (input_tape.recording || input_tape.record_auto)
		{
			input_tape.record_auto = FALSE;
			if (input_tape.recording)
				input_tape_stop("tape_stop");
			console_printf(FALSE, "tape: stopped, the last file is %s", input_tape_host_path(input_tape.last_path));
		}
		else if (input_tape.playing)
		{
			if (input_tape.skipping)
				game_time_set_speed(input_tape.saved_speed);
			input_tape.skipping = FALSE;
			input_tape.playing = FALSE;
			input_tape.waiting = FALSE;
			console_printf(FALSE, "tape: playback stopped");
		}
		else
			console_printf(FALSE, "tape: not recording or playing");
		return TRUE;
	}
	if (input_tape_word(expression, "tape_list"))
	{
		input_tape_command_list();
		return TRUE;
	}
	if ((rest = input_tape_word(expression, "tape_play")) != NULL)
	{
		input_tape_command_play(rest);
		return TRUE;
	}
	return FALSE;
}

/* whether the tape gave the action (playing); recording only copies the input of a tick's first ask */
static boolean input_tape_action(
	short local_player_index,
	struct player_action *action)
{
	long tick;
	long key;
	long low = 0;
	long high;
	long unit_index;

	if (local_player_index < 0 || local_player_index >= MAXIMUM_LOCAL_PLAYERS)
		return FALSE;
	if (!input_tape.armed)
	{
		input_tape_arm();
		input_tape.last_tick = NONE;
	}
	if (!input_tape.recording && !input_tape.playing)
		return FALSE;
	/* (the pause menu stops the game's time, so no tick asks; this keeps a paused game's input out of a tape and a tape's out of the menu) */
	if (game_time_get_paused())
		return FALSE;
	tick = game_time_get();
	if (input_tape.last_tick == NONE)
		input_tape_restart();
	else if (tick < input_tape.last_tick)
	{
		/* (a recording is of one game: the restart that follows a new game's start would write the file again) */
		if (input_tape.recording)
		{
			input_tape_stop("a new game started");
			if (input_tape.record_auto)
			{
				input_tape_segment_begin();
				input_tape_restart();
			}
		}
		else
		{
			input_tape_restart();
			input_tape.waiting = FALSE;
		}
	}
	input_tape.last_tick = tick;
	if (input_tape.waiting)
		return FALSE;
	key = input_tape_key(tick, local_player_index);
	if (input_tape.recording)
	{
		if (!input_tape.file || tick < input_tape.record_from)
			return FALSE;
		if (!input_tape.started[local_player_index] && input_tape_body(local_player_index, &unit_index))
		{
			input_tape_pose_take(unit_index, &input_tape.pose[local_player_index]);
			input_tape_pose_write(input_tape.file, local_player_index, tick, &input_tape.pose[local_player_index]);
			input_tape.started[local_player_index] = TRUE;
		}
		if (input_tape.started[local_player_index] && local_player_index == 0 && tick % INPUT_TAPE_CHECK_EVERY == 0 &&
			tick != input_tape_check_last)
		{
			input_tape_check_last = tick;
			fprintf(input_tape.file, "check %ld %08x %08x\n", tick, (unsigned int)get_random_seed(),
				input_tape_world_hash());
		}
		if (input_tape.started[local_player_index] && key != input_tape.last_key[local_player_index])
		{
			input_tape_line_write(input_tape.file, key, action);
			fflush(input_tape.file);
			input_tape.last_key[local_player_index] = key;
			input_tape_note_trigger(tick, local_player_index, action);
		}
		return FALSE;
	}
	if (!input_tape.playing)
		return FALSE;
	if (tick < input_tape.start_tick || input_tape.skipping)
		input_tape_pace(tick);
	if (input_tape.pose_valid[local_player_index] && !input_tape.pose_applied[local_player_index] &&
		tick >= input_tape.pose_tick[local_player_index] && input_tape_body(local_player_index, &unit_index))
	{
		boolean put = input_tape_pose_put(unit_index, &input_tape.pose[local_player_index]);

		input_tape.pose_applied[local_player_index] = TRUE;
		*get_global_random_seed_address() = input_tape.pose[local_player_index].random_seed;
		error(_error_silent, "input_tape: player %d %s at tick %ld (recorded at %ld%s)", (int)local_player_index,
			put ? "put back" : "could not be put back", tick, input_tape.pose_tick[local_player_index],
			input_tape.pose[local_player_index].vehicle.present ? ", in a vehicle" : "");
	}
	if (local_player_index == 0 && input_tape_check_count > 0 && tick % INPUT_TAPE_CHECK_EVERY == 0 &&
		tick != input_tape_check_last)
	{
		long first = 0;
		long last = input_tape_check_count - 1;

		input_tape_check_last = tick;
		while (first <= last)
		{
			long middle = (first + last)/2;
			struct input_tape_check const *check = &input_tape_checks[middle];

			if (check->tick == tick)
			{
				boolean seed_same = check->seed == (unsigned int)get_random_seed();
				boolean world_same = check->world == input_tape_world_hash();

				if ((!seed_same || !world_same) && !input_tape_desync_reported)
				{
					input_tape_desync_reported = TRUE;
					error(_error_silent, "input_tape: out of step at tick %ld: random seed %s, world %s", tick,
						seed_same ? "the same" : "different", world_same ? "the same" : "different");
					console_printf(FALSE, "tape: out of step at tick %ld (random seed %s, world %s)", tick,
						seed_same ? "same" : "different", world_same ? "same" : "different");
				}
				break;
			}
			if (check->tick < tick)
				first = middle + 1;
			else
				last = middle - 1;
		}
	}
	high = input_tape.count - 1;
	while (low <= high)
	{
		long middle = (low + high)/2;

		if (input_tape_entries[middle].key == key)
		{
			real_vector3d facing;

			csmemcpy(action, input_tape_entries[middle].bytes, sizeof(struct player_action));
			/* (the camera and the next frame's input follow the controller's own angles, not this tick's action) */
			vector3d_from_euler_angles2d(&facing, &action->desired_facing);
			player_control_set_facing(local_player_index, &facing);
			input_tape_note_trigger(tick, local_player_index, action);
			return TRUE;
		}
		if (input_tape_entries[middle].key < key)
			low = middle + 1;
		else
			high = middle - 1;
	}
	if (key > input_tape_entries[input_tape.count - 1].key && !input_tape.ended)
	{
		input_tape.ended = TRUE;
		error(_error_silent, "input_tape: playback finished at tick %ld", tick);
		console_printf(FALSE, "tape: playback finished");
	}
	return FALSE;
}

/* a local player's input this tick, the tape's (player_queues_new.c) */
void network_test_tape_action(
	short local_player_index,
	struct player_action *action)
{
	input_tape_action(local_player_index, action);
}

void network_test_update(
	boolean main_menu_loaded,
	real seconds)
{
	if (!network_test.checked)
		network_test_read_settings();
	if (network_test.mode == _network_test_off)
		return;

	/* debug.network_test_chat: each client's player types "gg ez" on the
	text chat's on-screen keyboard 32 seconds on, as a controller does, and
	sends it to the team (each frame, not each second, as the rest) */
	if (network_test.chat_time > 0.0f && network_test.mode != _network_test_host && game_in_progress() &&
		!main_menu_loaded &&
		game_time_get() >= (long)(network_test.chat_time * TICKS_PER_SECOND) + 32 * TICKS_PER_SECOND)
	{
		network_test_keyboard_line(game_time_get() - (long)(network_test.chat_time * TICKS_PER_SECOND) -
			32 * TICKS_PER_SECOND);
	}
	/* the game running: report (from the start of each game, or a revert to
	a checkpoint: the time starts over, and the cues with it) */
	if (game_in_progress() && game_time_get() < network_test.logged_time)
	{
		network_test.logged_time = 0;
		network_test.cues_start = NONE;
		network_test.next_cue = 0;
	}
	if (game_in_progress() && !main_menu_loaded)
		network_test_run_cues();
	if (game_in_progress() && !main_menu_loaded && game_time_get() - network_test.logged_time >= TICKS_PER_SECOND)
	{
		network_test.logged_time = game_time_get();
		network_test_log_players();
		if (network_test.shoot_interval > 0.0f &&
			game_time_get() % MAX(1, (long)(network_test.shoot_interval * TICKS_PER_SECOND)) < TICKS_PER_SECOND)
		{
			network_test_shoot();
		}
		/* (the host brings the players near a second before they shoot;
		not the last while the vehicle test has it) */
		if (network_test.mode == _network_test_host && network_test.shoot_interval > 0.0f &&
			(game_time_get() + TICKS_PER_SECOND) % MAX(1, (long)(network_test.shoot_interval * TICKS_PER_SECOND)) <
				TICKS_PER_SECOND)
		{
			long vehicle_time = (long)(network_test.vehicle_time * TICKS_PER_SECOND);

			network_test_gather(network_test.vehicle_time > 0.0f && game_time_get() >= vehicle_time - 2 * TICKS_PER_SECOND);
		}
		if (network_test.pickup_time > 0.0f)
		{
			long pickup_time = (long)(network_test.pickup_time * TICKS_PER_SECOND);

			/* (two weapons first: picking up a third swaps) */
			if (network_test.mode == _network_test_host && game_time_get() >= pickup_time - 3 * TICKS_PER_SECOND &&
				game_time_get() - (pickup_time - 3 * TICKS_PER_SECOND) < TICKS_PER_SECOND)
			{
				network_test_second_weapon();
			}

			if (game_time_get() >= pickup_time && game_time_get() - pickup_time < TICKS_PER_SECOND)
			{
				network_test_pickup();
			}
			/* (standing there for four seconds, the button held from a second
			on) */
			if (network_test.mode == _network_test_join)
			{
				boolean hold = game_time_get() >= pickup_time &&
					game_time_get() < pickup_time + 4 * TICKS_PER_SECOND;

				test_input_hold_action(hold);
			}
		}
		/* debug.network_test_chat: each machine's player says a line to
		everyone that many seconds in, one to the team two seconds on, and
		one to the vehicle two more on, the host having seated the last two
		players in one vehicle (the others in none) three seconds before;
		then the host mutes the last player for a minute (a client's /mute
		of the host refused), each says a line to everyone (the last
		player's going no further than the host), the host unmutes it, and
		each says another; then each says a line with a link (refused, as
		network.text_chat "on" refuses links), the host turns the text chat
		off (network.text_chat "off"), and each says a line (refused); then
		the host mutes the last player for everyone, as its scoreboard does,
		and each says a line, and the last player's machine mutes the host's
		for itself, as a client's scoreboard does, and each says another */
		if (network_test.chat_time > 0.0f)
		{
			long chat_time = (long)(network_test.chat_time * TICKS_PER_SECOND);
			char const *machine = network_test.mode == _network_test_host ? "the host" : "a client";
			char text[64];

			if (game_time_get() < chat_time - 3 * TICKS_PER_SECOND)
				network_test.chat_lines_said = 0;
			if (network_test.mode == _network_test_host && network_test.chat_lines_said == 0 &&
				game_time_get() >= chat_time - 3 * TICKS_PER_SECOND)
			{
				network_test_seat_together();
				network_test.chat_lines_said = -1;
			}
			if (network_test.chat_lines_said <= 0 && game_time_get() >= chat_time)
			{
				/* (long enough to wrap in the chat's column) */
				snprintf(text, sizeof(text), "hello everyone from %s, long enough to wrap the column", machine);
				network_chat_say(_chat_channel_everyone, text);
				network_test.chat_lines_said = 1;
			}
			if (network_test.chat_lines_said == 1 && game_time_get() >= chat_time + 2 * TICKS_PER_SECOND)
			{
				snprintf(text, sizeof(text), "hello team from %s", machine);
				network_chat_say(_chat_channel_team, text);
				network_test.chat_lines_said++;
			}
			if (network_test.chat_lines_said == 2 && game_time_get() >= chat_time + 4 * TICKS_PER_SECOND)
			{
				snprintf(text, sizeof(text), "hello vehicle from %s", machine);
				network_chat_say(_chat_channel_vehicle, text);
				network_test.chat_lines_said++;
			}
			if (network_test.chat_lines_said == 3 && game_time_get() >= chat_time + 6 * TICKS_PER_SECOND)
			{
				char name[16];

				network_test_last_player_name(name, sizeof(name));
				if (network_test.mode == _network_test_host)
					snprintf(text, sizeof(text), "/mute %s 1", name);
				else
					snprintf(text, sizeof(text), "/mute host");
				network_chat_say(_chat_channel_everyone, text);
				network_test.chat_lines_said++;
			}
			if (network_test.chat_lines_said == 4 && game_time_get() >= chat_time + 8 * TICKS_PER_SECOND)
			{
				snprintf(text, sizeof(text), "while muted from %s", machine);
				network_chat_say(_chat_channel_everyone, text);
				network_test.chat_lines_said++;
			}
			if (network_test.chat_lines_said == 5 && game_time_get() >= chat_time + 10 * TICKS_PER_SECOND)
			{
				char name[16];

				network_test_last_player_name(name, sizeof(name));
				if (network_test.mode == _network_test_host)
				{
					snprintf(text, sizeof(text), "/unmute %s", name);
					network_chat_say(_chat_channel_everyone, text);
				}
				network_test.chat_lines_said++;
			}
			if (network_test.chat_lines_said == 6 && game_time_get() >= chat_time + 12 * TICKS_PER_SECOND)
			{
				snprintf(text, sizeof(text), "after unmute from %s", machine);
				network_chat_say(_chat_channel_everyone, text);
				network_test.chat_lines_said++;
			}
			if (network_test.chat_lines_said == 7 && game_time_get() >= chat_time + 14 * TICKS_PER_SECOND)
			{
				/* (a client's sent past its own check, for the host to refuse) */
				snprintf(text, sizeof(text), "join us at discord(dot)gg/halo from %s", machine);
				if (network_test.mode == _network_test_host)
					network_chat_say(_chat_channel_everyone, text);
				else
					network_chat_say_unchecked(_chat_channel_everyone, text);
				network_test.chat_lines_said++;
			}
			if (network_test.chat_lines_said == 8 && game_time_get() >= chat_time + 16 * TICKS_PER_SECOND)
			{
				if (network_test.mode == _network_test_host && !config_write("network.text_chat", "off"))
					platform_log("network test: could not turn the text chat off");
				network_test.chat_lines_said++;
			}
			if (network_test.chat_lines_said == 9 && game_time_get() >= chat_time + 18 * TICKS_PER_SECOND)
			{
				snprintf(text, sizeof(text), "with the chat off from %s", machine);
				if (network_test.mode == _network_test_host)
					network_chat_say(_chat_channel_everyone, text);
				else
					network_chat_say_unchecked(_chat_channel_everyone, text);
				network_test.chat_lines_said++;
			}
			/* (and on again: config.toml is the executable's folder's, which
			every test of it shares) */
			if (network_test.chat_lines_said == 10 && game_time_get() >= chat_time + 20 * TICKS_PER_SECOND)
			{
				if (network_test.mode == _network_test_host && !config_write("network.text_chat", "on"))
					platform_log("network test: could not turn the text chat on");
				network_test.chat_lines_said++;
			}
			/* (the scoreboard's mutes: the host's of the last player for
			everyone, their lines and voice, then the last player's machine's
			own mute of the host's machine, here alone) */
			if (network_test.chat_lines_said == 11 && game_time_get() >= chat_time + 22 * TICKS_PER_SECOND)
			{
				if (network_test.mode == _network_test_host)
					network_chat_host_mute(network_test_last_player_index(), TRUE);
				network_test.chat_lines_said++;
			}
			if (network_test.chat_lines_said == 12 && game_time_get() >= chat_time + 24 * TICKS_PER_SECOND)
			{
				snprintf(text, sizeof(text), "after the scoreboard mute from %s", machine);
				network_chat_say(_chat_channel_everyone, text);
				network_test.chat_lines_said++;
			}
			if (network_test.chat_lines_said == 13 && game_time_get() >= chat_time + 28 * TICKS_PER_SECOND)
			{
				if (network_test.mode == _network_test_host)
					network_chat_host_mute(network_test_last_player_index(), FALSE);
				else if (network_test_last_player_local())
					network_voice_mute_machine(network_test_first_player_machine(), TRUE);
				network_test.chat_lines_said++;
			}
			if (network_test.chat_lines_said == 14 && game_time_get() >= chat_time + 30 * TICKS_PER_SECOND)
			{
				snprintf(text, sizeof(text), "after the local mute from %s", machine);
				network_chat_say(_chat_channel_everyone, text);
				network_test.chat_lines_said++;
			}
		}
		if (network_test.mode == _network_test_host && network_test.vehicle_time > 0.0f)
		{
			long enter_time = (long)(network_test.vehicle_time * TICKS_PER_SECOND);

			if (game_time_get() >= enter_time && game_time_get() - enter_time < TICKS_PER_SECOND)
				network_test_vehicle(TRUE);
			if (game_time_get() >= enter_time + 15 * TICKS_PER_SECOND &&
				game_time_get() - enter_time - 15 * TICKS_PER_SECOND < TICKS_PER_SECOND)
			{
				network_test_vehicle(FALSE);
			}
		}
		/* ... and 20 seconds on, the last player getting in as a player does:
		at the entrance, the button held for three seconds */
		if (network_test.vehicle_time > 0.0f)
		{
			long approach_time = (long)(network_test.vehicle_time * TICKS_PER_SECOND) + 20 * TICKS_PER_SECOND;

			if (game_time_get() >= approach_time && game_time_get() - approach_time < TICKS_PER_SECOND)
				network_test_vehicle_approach();
			if (network_test.mode == _network_test_join && network_test.pickup_time <= 0.0f)
			{
				test_input_hold_action(game_time_get() >= approach_time + TICKS_PER_SECOND &&
					game_time_get() < approach_time + 4 * TICKS_PER_SECOND);
			}
		}
		/* debug.network_test_kill: the host kills the last player every so
		often, to test deaths and respawns reaching the clients */
		if (network_test.mode == _network_test_host && network_test.kill_interval > 0.0f &&
			game_time_get() % MAX(1, (long)(network_test.kill_interval * TICKS_PER_SECOND)) < TICKS_PER_SECOND)
		{
			struct data_iterator iterator;
			struct player_datum *player;
			struct player_datum *last = NULL;

			struct player_datum *first = NULL;
			long first_index = NONE;

			data_iterator_new(&iterator, player_data);
			while ((player = (struct player_datum *)data_iterator_next(&iterator)) != NULL)
			{
				if (!first)
				{
					first = player;
					first_index = iterator.datum_index;
				}
				last = player;
			}
			if (last && last != first && last->unit_index != NONE && first->unit_index != NONE)
			{
				/* killed by the first player: a kill that scores */
				platform_log("network test: the first player kills the last");
				damage_kill_object_for_player(last->unit_index, first_index);
				/* and picks up a weapon lying about, and two grenades of each kind */
				{
					struct object_iterator objects;
					struct unit_datum *unit = unit_get(first->unit_index);
					long held_index = unit->unit.weapon_object_indices[0];
					long held_definition_index = held_index != NONE ? weapon_get(held_index)->definition_index : NONE;

					object_iterator_new(&objects, _object_mask_weapon, 0);
					while (object_iterator_next(&objects))
					{
						struct weapon_datum *weapon = weapon_get(objects.index);

						if (weapon->object.parent_object_index == NONE &&
							weapon->definition_index != held_definition_index)
						{
							if (unit_add_weapon_to_inventory(first->unit_index, objects.index, TRUE))
								platform_log("network test: the first player picks up a weapon");
							break;
						}
					}
					unit->unit.grenade_counts[0] = 2;
					unit->unit.grenade_counts[1] = 2;
				}
			}
		}
	}

	/* the next game of the list: once the scores have been shown a while,
	the host's button (the bots may press it first) */
	if (network_test.mode == _network_test_host && network_test.started && game_engine_running() &&
		!main_menu_loaded && !game_engine_can_score())
	{
		network_test.game_over = TRUE;
		if (game_engine_showing_postgame() && global_network_game_server_get())
		{
			network_test.postgame_seconds += seconds;
			if (network_test.postgame_seconds >= 3.0f && network_test_variant(network_test.variant_index + 1, NULL, 0))
			{
				network_test.postgame_seconds = 0.0f;
				network_game_server_reset_to_pregame(global_network_game_server_get());
			}
		}
	}
	/* (a joining machine's player: the other team again in the next game's
	lobby, whose variant may have teams where the last had none) */
	if (network_test.mode == _network_test_join && network_test.team_set && game_engine_running() &&
		!main_menu_loaded && !game_engine_can_score())
	{
		network_test.game_over = TRUE;
	}
	if (network_test.mode == _network_test_join && network_test.game_over && main_menu_loaded)
	{
		network_test.game_over = FALSE;
		network_test.team_set = FALSE;
		network_test.joined_seconds = 0.0f;
	}
	/* ... back in the lobby, set up as the first was */
	if (network_test.mode == _network_test_host && network_test.game_over && main_menu_loaded)
	{
		network_test.game_over = FALSE;
		network_test.postgame_seconds = 0.0f;
		if (network_test_variant(network_test.variant_index + 1, NULL, 0))
		{
			network_test.variant_index++;
			network_test.started = FALSE;
			network_test.map_set = FALSE;
			network_test.setup_seconds = 0.0f;
			network_test.menu_seconds = 0.0f;
			/* (as picking the next game's map does: the scores' map choice
			holds the countdown) */
			if (global_network_game_server_get())
				network_game_server_pause_countdown(global_network_game_server_get(), FALSE);
			/* a join that arrived as the last game ended would refuse every
			later one, the server holding it behind the queued player */
			network_game_server_port_clear_queued_players(global_network_game_server_get());
			platform_log("network test: the next game");
		}
	}

	if (!main_menu_loaded)
		return;
	/* the intro movie skipped first, as a button press does: the movie
	holds the texture cache's spare memory, which starting the map
	takes too (texture_cache_steal_memory's assertion) */
	if (bink_playback_in_progress())
	{
		bink_playback_stop();
		network_test.menu_seconds = 0.0f;
		platform_log("network test: skipped the movie");
		return;
	}
	network_test.menu_seconds += seconds;
	/* (the main menu settling first) */
	if (network_test.menu_seconds < 2.0f)
		return;

	switch (network_test.mode)
	{
	case _network_test_browse:
	{
		struct p2p_listing listed[8], games[8];
		int listed_count, count = 0, pass, index;

		if (!network_test.browsing)
		{
			network_test.browsing = TRUE;
			p2p_lobby_browse(TRUE);
			platform_log("network test: browsing the public games");
		}
		/* (listings arriving meanwhile from every broker) */
		network_test.browse_seconds += seconds;
		listed_count = p2p_lobby_games(listed, NUMBEROF(listed));
		if (!listed_count || network_test.browse_seconds < 5.0f)
			break;
		/* those that can be joined (no password, not failed): the list has
		the most players first; co-op, the game with no game engine, last */
		for (pass = 0; pass < 2; pass++)
		{
			for (index = 0; index < listed_count; index++)
			{
				if (listed[index].invite[0] && !listed[index].failed && !listed[index].engine_type == pass)
					games[count++] = listed[index];
			}
		}
		if (!network_test.browse_listed)
		{
			network_test.browse_listed = TRUE;
			for (index = 0; index < listed_count; index++)
			{
				platform_log("network test: listed %s's game (%s, %s) %d/%d players%s%s", listed[index].name,
					listed[index].map, listed[index].gametype, listed[index].player_count,
					listed[index].maximum_player_count, listed[index].engine_type ? "" : ", co-op",
					listed[index].invite[0] ? "" : ", locked");
			}
		}
		if (count > 0 && p2p_join_listing(&games[0]))
		{
			p2p_lobby_browse(FALSE);
			platform_log("network test: joining %s's game (%s) from the server browser", games[0].name, games[0].map);
			network_test.mode = _network_test_join;
		}
		break;
	}
	case _network_test_host:
		if (network_test.set_up && !network_test.local && !network_test.invite_logged)
		{
			char invite[256];

			if (p2p_invite_link(invite, sizeof(invite)))
			{
				network_test.invite_logged = TRUE;
				platform_log("network test: invite %s", invite);
			}
		}
		if (!network_test.set_up)
		{
			network_test.set_up = TRUE;
			main_set_multiplayer_map_name(network_test.map_name);
			if (network_test.local)
			{
				/* as Multiplayer > Split Screen does (split_screen_game_initialize) */
				ui_widgets_close_all();
				dispose_global_network_game_server();
				dispose_global_network_game_client();
				network_game_accept_remote_connections(FALSE);
				game_engine_playlist_initialize();
				if (create_global_network_game_server())
				{
					game_engine_playlist_begin();
					game_connection_set(_game_connection_network_server);
				}
				if (!global_network_game_client_get())
					create_global_network_game_client();
				ui_widget_load_by_name_or_tag(
					"ui\\shell\\main_menu\\multiplayer_type_select\\split_screen\\pregame\\splitscreen_pregame_wrapper_normal",
					NONE, NULL, NONE, NONE, NONE, NONE);
				platform_log("network test: a local game on %s", network_test.map_name);
			}
			else
			{
				player_ui_fast_setup_network_server();
				platform_log("network test: hosting %s", network_test.map_name);
				/* (in the server browser, as Create Game > Internet's PUBLIC) */
				if (network_test.public_game)
					p2p_set_hosting_public(TRUE);
			}
		}
		else if (!network_test.started)
		{
			network_test.setup_seconds += seconds;
			/* the map (fast setup clears it), and a player for controller 1, as
			pressing A in the lobby adds one */
			if (!network_test.map_set && network_test.setup_seconds >= 1.0f && global_network_game_server_get())
			{
				char path[128];
				char variant_name[64];
				struct game_variant variant;

				/* (a level of its own path, else a multiplayer map's name) */
				if (strchr(network_test.map_name, '\\'))
					snprintf(path, sizeof(path), "%s", network_test.map_name);
				else
					snprintf(path, sizeof(path), "levels\\test\\%s\\%s", network_test.map_name, network_test.map_name);
				network_test_variant(network_test.variant_index, variant_name, sizeof(variant_name));
				if (!strcmp(variant_name, "coop"))
				{
					/* (the Map screen's co-op: menu_functions.c) */
					extern boolean ui_widget_port_cooperative_level_choose(char const *map_name, short difficulty);

					if (!ui_widget_port_cooperative_level_choose(path, _game_difficulty_level_normal))
						platform_log("network test: %s is not a campaign level, for co-op", path);
					platform_log("network test: game %d, co-op", network_test.variant_index + 1);
				}
				else
				{
					struct game_variant_options options;
					/* (one of OpenCE's gametypes by its name: game_modes.c) */
					boolean opence = game_modes_gametype_by_name(variant_name, &variant, &options);

					network_game_server_change_map_name(global_network_game_server_get(), path);
					/* the variant, as picking the game settings does */
					if (!opence)
						variant = *game_engine_get_variant_by_name(&variant, variant_name);
					platform_log("network test: game %d, %s", network_test.variant_index + 1, variant_name);
					/* debug.network_test_score: a short game, to test the next */
					if (network_test.score_to_win > 0)
						variant.universal_variant.score_to_win = network_test.score_to_win;
					player_ui_set_game_variant(&variant);
					if (opence)
						player_ui_set_game_variant_options(&options);
					network_game_server_change_game_variant(global_network_game_server_get(), &variant);
				}
				network_test.map_set = TRUE;
			}
			if (!network_test.player_added && network_test.setup_seconds >= 2.0f && global_network_game_client_get())
			{
				network_test_set_name();
				network_test.player_added = network_game_client_add_player(global_network_game_client_get(), 0);
			}
			if (network_test.setup_seconds >= network_test.start_delay)
			{
				network_test.started = TRUE;
				network_game_client_request_immediate_start();
				platform_log("network test: starting the game");
			}
		}
		break;
	case _network_test_probe:
		if (!network_test.set_up)
		{
			network_test.set_up = TRUE;
			dispose_global_network_game_client();
			dispose_global_network_game_server();
			if (create_global_network_game_client())
				game_connection_set(_game_connection_network_client);
		}
		else
		{
			network_test.probe_seconds += seconds;
			network_test_probe_answer(network_test.probe_seconds >= PROBE_SECONDS);
		}
		break;
	case _network_test_join:
		if (!network_test.set_up)
		{
			network_test.set_up = TRUE;
			dispose_global_network_game_client();
			dispose_global_network_game_server();
			network_test_set_name();
			if (create_global_network_game_client())
			{
				game_connection_set(_game_connection_network_client);
				/* (the player joined to multiplayer first, as a player picking
				their profile: the pregame screen then asks for them every
				frame until they are in the settings) */
				player_ui_local_player_joined_multiplayer_game(0);
				platform_log("network test: searching for games");
			}
		}
		else if (!network_test.joined && network_game_client_join_first_available_game())
		{
			network_test.joined = TRUE;
			ui_widgets_close_all();
			ui_widget_load_by_name_or_tag(
				pc_menus_screen("ui\\shell\\main_menu\\multiplayer_type_select\\connected\\pregame\\connected_pregame_screen"),
				NONE, NULL, NONE, NONE, NONE, NONE);
			platform_log("network test: joining");
		}
		else if (network_test.joined && !network_test.player_added)
		{
			network_test.joined_seconds += seconds;
			if (network_test.joined_seconds >= 3.0f && global_network_game_client_get())
				network_test.player_added = TRUE;
		}
		/* (the other team from the host's player: a team game needs both) */
		else if (network_test.player_added && !network_test.team_set)
		{
			network_test.joined_seconds += seconds;
			if (network_test.joined_seconds >= 5.0f)
				network_test.team_set = network_game_client_set_team(NONE);
		}
		break;
	}
}

/* the game's time in seconds while a game runs, -1 otherwise: the scripted
player's clock (xinput_sdl.c, debug.test_input) */
double network_test_game_seconds(
	void)
{
	return game_time_initialized() && game_in_progress() ? (double)game_time_get() / TICKS_PER_SECOND : -1.0;
}

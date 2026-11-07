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
- "local:<map>:slayer" with debug.network_test_ride "longsword" starts that
  riding test with two local players and no network connection (pilot 0,
  rider 1), not the split screen game of "local:" above;
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
#include "hs/hs.h"
#include "objects/objects.h"
#include "units/units.h"
#include "units/unit_definitions.h"
#include "units/vehicle_datum.h"
#include "items/weapons.h"
#include "items/weapon_definitions.h"
#include "items/projectile_definitions.h"
#include "items/items.h"
#include "objects/damage.h"
#include "scenario/scenario.h"
#include "tag_files/tag_files.h"
#include "camera/observer.h"
#include "hs/hs.h"
#include "cseries/errors.h"
#include "cutscene/cinematics.h"
#include "units/bipeds.h"
#include "units/biped_definitions.h"
#include "ai/actor_definitions.h"
#include "ai/actors.h"
#include "scenario/scenario_definitions.h"
#include "tag_files/tag_groups.h"
#include "physics/collisions.h"
#include "physics/collision_models.h"
#include "physics/collision_features.h"
#include "physics/collision_bsp.h"
#include "physics/physics.h"
#include "vehicle_riding.h"
#include "network_test.h"
#include "network_distributed.h"

#include "../src/p2p.h" /* port: port/linux/src/p2p.c, p2p_lobby.c */
#include "network_chat.h"
#include "network_voice.h"
#include "halo_keyboard.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* the platform layer's (port/linux/src/port_config.c) */
const char *config_string(char const *name);
int config_boolean(char const *name);
double config_real(char const *name);
long config_integer(char const *name);
int config_write(char const *name, char const *value);
void platform_log(char const *format, ...);
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
	_network_test_local,
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

static void ride_settings(void);

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
	/* (local: with the Longsword riding test is that test's own, two local players and no network; any other
	local: is a split screen game the host's way) */
	boolean ride_local = !strncmp(setting, "local:", 6) && !strcmp(config_string("debug.network_test_ride"), "longsword");

	network_test.checked = TRUE;
	if ((!strncmp(setting, "host:", 5) && setting[5]) || (!strncmp(setting, "local:", 6) && setting[6]))
	{
		char *colon;

		network_test.mode = ride_local ? _network_test_local : _network_test_host;
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
	ride_settings();
}

/* ---------- riding tests */

/* debug.network_test_ride (NETCODE.md, Testing): the host gives each test
vehicle its velocity every tick (its physics then moves it, as a driver's
throttle does), every machine's player rides it, each machine plays its own
player's input from the script, and every machine logs what it has of each
rider and vehicle every tick, as "ride" records in debug.txt
(tools/network_ride_test.py reads them) */

/* the vehicles' motion, world units and radians a tick */
#define RIDE_STRAIGHT_SPEED 0.05f
#define RIDE_CIRCLE_SPEED 0.04f
#define RIDE_CIRCLE_TURN 0.01f
#define RIDE_RISE_SPEED 0.03f
#define RIDE_CONVOY_SPEED 0.04f
#define RIDE_BRUSH_SPEED 0.03f
/* the riders' and the driver's input */
#define RIDE_WALK_THROTTLE 0.15f
#define RIDE_SIDE_THROTTLE 0.3f
#define RIDE_DRIVE_THROTTLE 0.5f
#define RIDE_DRIVE_TURN 0.3f
#define RIDE_RECOVERY_WAIT 12
#define RIDE_RECOVERY_LINEAR .005f
#define RIDE_RECOVERY_ANGULAR .005f
#define RIDE_RECOVERY_TIMEOUT 180
#define RIDE_RECOVERY_CONTACT 6
#define RIDE_RECOVERY_SETTLE 6
#define RIDE_RECOVERY_MAX 10
/* a level run: the ground not rising or falling more than RIDE_LEVEL_RISE in
each RIDE_LEVEL_STEP, over a scenario's whole drive and RIDE_LEVEL_MARGIN
more, and at least RIDE_LEVEL_RUN */
#define RIDE_LEVEL_RUN 70.0f
#define RIDE_LEVEL_MARGIN 10.0f
#define RIDE_LEVEL_STEP 5.0f
#define RIDE_LEVEL_RISE 1.0f
/* a spot's ground also level two steps behind its origin, and for the convoy
along the second vehicle's lane: both where ride_convoy_place starts it, this
far to the left, and where it ends up (their decks' edges RIDE_CONVOY_GAP apart:
the vehicles' origins about 2.2 wu apart) */
#define RIDE_LEVEL_BEHIND 10.0f
#define RIDE_CONVOY_LANE 4.0f
#define RIDE_CONVOY_LANE_NEAR 1.0f
/* a lane's ground within this much of the first vehicle's, for each wu it lies
to the left (a lane level along itself can still be a hillside's contour: the
second vehicle would stand on a slope across its width and tumble); 0.35 is
about 19 degrees, the most a spot may have: a 46 degree hillside (1.06) rolled
both Warthogs, and the flattest spot Blood Gulch has (0.33 and 0.23 for the
two lanes, about 18 degrees) still lets them slide together, so among the spots
passing it the flattest is taken */
#define RIDE_CONVOY_CROSS_RISE 0.35f
/* the host holds the convoy's second vehicle on its first's left, at the offset it was placed at, and its heading the first's, so a
slope across them cannot slide it away: its sideways speed (wu a tick) is this for each wu of error, at most the cap, and its turn
(radians a tick) this for each radian, at most the turn cap */
#define RIDE_CONVOY_HOLD_GAIN 0.2f
#define RIDE_CONVOY_HOLD_CAP 0.02f
#define RIDE_CONVOY_HOLD_TURN_GAIN 0.5f
#define RIDE_CONVOY_HOLD_TURN_CAP 0.005f
/* spots whose cross rises differ by less than this are equals (the earlier one found is kept) */
#define RIDE_CONVOY_CROSS_TIE 0.01f
/* the convoy's heading turned this far to the left (counterclockwise) from the
first level one: no Blood Gulch heading is level for the convoy's whole drive,
and this one is */
#define RIDE_CONVOY_TURN (10.f*_pi/180.f)
#define RIDE_COOP_HALF_WIDTH 0.6f
#define RIDE_COOP_CROSS_RISE 0.05f
/* a rider's side of the Warthog's centre line on the deck: the last measured
column inside the deck's edge (the bed has no room for a 0.4 pill off its
centre) */
#define RIDE_BOARD_SIDE 0.2f
/* where a spot has no level run, the ground searched for one in rings this far apart, out to this far */
#define RIDE_SEARCH_STEP 10.0f
#define RIDE_SEARCH_RADIUS 120.0f
/* the convoy's: its drive and lanes need so much level ground that none lies within RIDE_SEARCH_RADIUS of the
Warthog's spawn on Blood Gulch, so the search covers the valley floor (a ring past RIDE_SEARCH_RADIUS has a
multiple of sixteen points, so they stay about as close together) */
#define RIDE_CONVOY_SEARCH_RADIUS 300.0f
/* the hull scan's step, placing tolerance and clearance for the suspension
settling on the cross slope */
#define RIDE_CONVOY_GAP 0.025f
#define RIDE_CONVOY_MAXIMUM_GAP 0.05f
#define RIDE_CONVOY_HULL_GAP 0.15f
/* legacy tilt helpers; the live Longsword programme uses only native pilot inputs */
#define RIDE_TILT_CAP (_pi/180.f)
#define RIDE_TILT_LIFT 20.0f
#define RIDE_TILT_HOLD_GAIN 0.2f
#define RIDE_PILOT_CLIMB 300
#define RIDE_PILOT_TURN 1200
#define RIDE_PILOT_LEVEL 1500
#define RIDE_PILOT_HOVER 2400
#define RIDE_PILOT_PITCH 2700
#define RIDE_PILOT_PITCH_LEVEL 3000
#define RIDE_PILOT_BANK 3300
#define RIDE_PILOT_BANK_LEVEL 3600
#define RIDE_PILOT_COMBINED 3900
#define RIDE_PILOT_RECOVER 4200
#define RIDE_PILOT_DESCEND 4500
#define RIDE_PILOT_DOOR 8100
#define RIDE_PILOT_END 8460
#define RIDE_TILT_HOLD_CAP 0.05f
/* the survey's grid over the vehicle, body-local (wu) */
#define RIDE_SURVEY_STEP 0.25f
/* half the measured .0665 wu first airborne step, above support chatter */
#define RIDE_OBSERVED_LAUNCH 0.03325f
/* how far a rider's walk may carry it along the deck from where it began (wu) before the walk turns back (a moving deck
makes the forward leg longer than the backward one, so the shuffle would otherwise creep off the hood) */
#define RIDE_WALK_RANGE 0.1f

enum
{
	RIDE_PLAYERS = HALO_PORT_MAXIMUM_NETWORK_PLAYERS,
	/* the ticks of a test from its board tick: standing, walking (turning
	every RIDE_WALK_PERIOD: a short shuffle, a leg moving a rider 0.1 wu at
	most (0.011 wu a tick at RIDE_WALK_THROTTLE, measured), so it stays on
	its deck part), jumping (RIDE_JUMPS jumps RIDE_JUMP_PERIOD
	apart), crossing, stepping off, watching, the end */
	RIDE_BOARD_TICK = 300,
	RIDE_WALK = 600,
	RIDE_WALK_PERIOD = 8,
	RIDE_JUMP = 840,
	RIDE_JUMP_PERIOD = 90,
	/* the ticks before a scheduled press the rider stands still, so every press is a standing jump */
	RIDE_JUMP_SETTLE = 12,
	RIDE_JUMPS = 10,
	RIDE_CROSS = 1740,
	RIDE_CONVOY_HOP_TICKS = 8,
	RIDE_STEP_OFF = 1920,
	RIDE_BRUSH_STEP_TICKS = 30,
	RIDE_BRUSH_APPROACH_TICKS = 11,
	RIDE_OBSERVE = 2040,
	RIDE_END = 2400,
	/* ten recoveries may each spend the timeout budget and the acquisition hold */
	RIDE_DRIVEN_EXTRA = RIDE_RECOVERY_MAX*(RIDE_RECOVERY_TIMEOUT + RIDE_RECOVERY_SETTLE),
	/* straight's brake, at the fifth jump, and its length; rise's each part */
	RIDE_BRAKE = 1200,
	RIDE_BRAKE_TICKS = 60,
	RIDE_RISE_TICKS = 300,
	/* the vehicles readied this long before the board tick; how long co-op
	waits for its level's opening to end */
	RIDE_PREPARE_TICKS = 120,
	/* let the host's initial placement reach a remote observer before scoring */
	RIDE_DRIVEN_PREBOARD = RIDE_PREPARE_TICKS/2,
	/* a rider off the deck is put back this long into each jump's period, once
	its air time (about 55 ticks) has ended; it has the rest to settle */
	RIDE_REBOARD_AFTER = 40,
	RIDE_READY_TIMEOUT = 3600,
	/* the Longsword's: standing level before its programme, each step's hold, the tick of a hold its rider jumps at,
	and the last step's descent back to where it was found before the door walk (20 wu at 0.05 a tick, and a
	settling), so the rider walks out at the ground, never out of a ship 20 wu up */
	RIDE_TILT_SETTLE = 300,
	RIDE_TILT_HOLD = 150,
	RIDE_TILT_JUMP_AT = 60,
	RIDE_TILT_DESCEND = 450,
};

enum
{
	_ride_straight,
	_ride_circle,
	_ride_rise,
	_ride_convoy,
	_ride_driven,
	_ride_gun,
	_ride_brush,
	_ride_coop,
	_ride_coop_double,
	_ride_longsword,
	NUMBER_OF_RIDE_SCENARIOS
};

enum
{
	_ride_phase_prepare,
	_ride_phase_stand,
	_ride_phase_walk,
	_ride_phase_jump,
	_ride_phase_cross,
	_ride_phase_step_off,
	_ride_phase_observe,
	NUMBER_OF_RIDE_PHASES
};

static char const *const ride_names[NUMBER_OF_RIDE_SCENARIOS] =
{
	"straight", "circle", "rise", "convoy", "driven", "gun", "brush", "coop", "coop-double", "longsword"
};
static char const *const ride_phases[NUMBER_OF_RIDE_PHASES] =
{
	"prepare", "stand", "walk", "jump", "cross", "step-off", "observe"
};

/* the Longsword's programme (the spec's acceptance order): each step's setpoint in degrees, pitch nose up and roll the
left wing down, reached at RIDE_TILT_CAP a tick and held RIDE_TILT_HOLD ticks; jump: its rider jumps in the hold;
pair: pitch and roll each nine tenths of the angle protection starts at (each within it, their total tilt past it);
the last step, level, sets the vehicle down (RIDE_TILT_DESCEND) and then is the door walk */
struct ride_tilt_step
{
	short pitch;
	short roll;
	boolean jump;
	boolean pair;
};

static struct ride_tilt_step const ride_tilt_steps[] =
{
	{ 20, 0, FALSE, FALSE }, { 0, 0, FALSE, FALSE }, { 40, 0, TRUE, FALSE }, { 0, 0, FALSE, FALSE },
	{ 60, 0, TRUE, FALSE }, { 0, 0, FALSE, FALSE }, { 90, 0, FALSE, FALSE }, { 0, 0, FALSE, FALSE },
	{ 0, 20, FALSE, FALSE }, { 0, 0, FALSE, FALSE }, { 0, 40, TRUE, FALSE }, { 0, 0, FALSE, FALSE },
	{ 0, 60, FALSE, FALSE }, { 0, 0, FALSE, FALSE }, { 0, 90, FALSE, FALSE }, { 0, 0, FALSE, FALSE },
	{ 30, 30, FALSE, FALSE }, { 0, 0, FALSE, FALSE }, { 45, 45, TRUE, FALSE }, { 0, 0, FALSE, FALSE },
	{ 60, 30, FALSE, FALSE }, { 0, 0, FALSE, FALSE }, { 0, 0, TRUE, TRUE }, { 0, 0, FALSE, FALSE },
	{ 180, 0, TRUE, FALSE }, { 0, 0, FALSE, FALSE }, { 0, 0, FALSE, FALSE }
};
#define RIDE_TILT_STEPS ((short)(sizeof(ride_tilt_steps)/sizeof(ride_tilt_steps[0])))

/* a rider, by its player's absolute index */
struct ride_player
{
	long unit;
	/* the player-to-unit delivery decision, not a later script's input flag */
	long control_tick;
	boolean control_delivered;
	boolean control_inhibited;
	long assigned;
	/* its first tick riding, or crossing (the settling's start) */
	long board_tick;
	/* where along the deck (place x, on the node) the walk phase began, to turn the shuffle back from */
	real walk_origin;
	short walk_node;
	boolean walk_origin_valid;
	long applied_host_tick;
	long last_press_tick;
	/* the last jump (by its slot in the jump phase) it pressed */
	short last_jump;
	long previous_vehicle;
	short previous_ticks;
	short phase;
	short presses;
	short jumps;
	boolean local;
	long launch_tick;
	long episode;
	boolean episode_open;
	boolean episode_off;
	boolean physical_previous;
	short episode_recoveries;
	short support_losses;
	short landings;
	short crossings;
	short landed_off;
	short tick_corrections;
	short ordinal;
	/* the jump cycle it was last put back in, and the first tick of its landing off the deck */
	long reboard_cycle;
	long off_tick;
	/* (put back: its next jump counts although it has not settled) */
	boolean reboarded;
	boolean stepped_off;
	boolean recovering;
	boolean recovery_placed;
	long recovery_start;
	long recovery_land;
	long recovery_ready;
	short recovery_contacts;
	short recovery_steady;
	short recoveries;
	boolean life_logged;
	real_point3d life_position;
	/* the selected physical contact of this tick, never an incoming claim */
	long contact_tick;
	long contact_vehicle;
	short contact_node;
	long contact_surface;
	boolean contact_valid;
	real_point3d contact_point;
	real_vector3d contact_normal;
};

static struct
{
	boolean enabled;
	boolean failed;
	boolean prepared;
	boolean ended;
	boolean client_driver;
	boolean control_ready;
	boolean scripts_suspended;
	short scenario;
	char session[64];
	long seq;
	long board_tick;
	long before_tick;
	long after_tick;
	long vehicles[2];
	/* (the convoy's second vehicle's place along its first's left vector, as placed) */
	real convoy_offset;
	/* (the heading both vehicles are held to after the hull is placed) */
	real convoy_yaw;
	/* (the lead vehicle's origin when the convoy corridor is placed) */
	real_point3d convoy_origin;
	/* (the Longsword's: its rider's spot and door walk from debug.network_test_ride_spot, none for the survey; the pair
	step's angle, from the rider's surface limit; where the host holds it, where it was found (set down there for the
	door walk) and its heading) */
	boolean have_spot;
	boolean corner;
	boolean door_started;
	long door_tick;
	real spot_x;
	real spot_y;
	real exit_forward;
	real exit_left;
	real pair;
	real_point3d tilt_position;
	real_point3d tilt_ground;
	real tilt_yaw;
	long driver;
	/* (each vehicle where the last sample had it, and the netcode's writes
	of it since: network_test_ride_vehicle_written) */
	boolean vehicle_seen[2];
	real_point3d vehicle_last[2];
	real vehicle_written[2];
	short vehicle_writes[2];
	short vehicle_carries[2];
	struct ride_player players[RIDE_PLAYERS];
} ride;

static boolean ride_host(
	void)
{
	return (network_test.mode == _network_test_local && game_connection() == _game_connection_local) ||
		game_connection() == _game_connection_network_server;
}

static short ride_vehicle_count(
	void)
{
	return ride.scenario == _ride_convoy ? 2 : 1;
}

/* the riders a scenario boards: one beside a player driving (driven) or piloting (the Longsword), else two */
static short ride_rider_count(
	void)
{
	return ride.scenario == _ride_driven || ride.scenario == _ride_longsword ? 1 : 2;
}

/* a record in debug.txt (errors.c: its line is 1024 bytes, this one at
most 900 of it) */
static void ride_log(
	char const *kind,
	char const *format,
	...)
{
	char body[900];
	va_list arguments;
	int length;

	if (!ride.enabled)
		return;
	va_start(arguments, format);
	length = vsnprintf(body, sizeof(body), format, arguments);
	va_end(arguments);
	if (length < 0 || length >= (int)sizeof(body))
	{
		ride.failed = TRUE;
		kind = "error";
		snprintf(body, sizeof(body), "tick=-1 reason=record-too-long");
	}
	error(_error_log, "ride schema=1 kind=%s session=%s scenario=%s side=%s seq=%ld %s", kind, ride.session,
		ride_names[ride.scenario], ride_host() ? "host" : "client", ++ride.seq, body);
}

static void ride_error(
	char const *reason)
{
	long tick = game_time_initialized() ? game_time_get() : NONE;

	ride_log("error", "tick=%ld reason=%s", tick, reason);
	ride.failed = TRUE;
	/* (the runner decides at both logs' end, not at its timeout) */
	if (!ride.ended)
	{
		ride.ended = TRUE;
		ride_log("end", "tick=%ld players=%d", tick, (int)ride_rider_count());
	}
}

/* a test's state at a game's start */
static void ride_reset(
	void)
{
	short index;

	ride.failed = ride.prepared = ride.ended = ride.control_ready = FALSE;
	ride.board_tick = RIDE_BOARD_TICK;
	ride.before_tick = ride.after_tick = NONE;
	ride.vehicles[0] = ride.vehicles[1] = ride.driver = NONE;
	for (index = 0; index < 2; index++)
	{
		ride.vehicle_seen[index] = FALSE;
		ride.vehicle_written[index] = 0.f;
		ride.vehicle_writes[index] = 0;
		ride.vehicle_carries[index] = 0;
	}
	for (index = 0; index < RIDE_PLAYERS; index++)
	{
		csmemset(&ride.players[index], 0, sizeof(ride.players[index]));
		ride.players[index].unit = ride.players[index].assigned = NONE;
		ride.players[index].board_tick = ride.players[index].applied_host_tick = NONE;
		ride.players[index].last_press_tick = ride.players[index].previous_vehicle = NONE;
		ride.players[index].reboard_cycle = ride.players[index].off_tick = NONE;
		ride.players[index].last_jump = NONE;
	}
}

/* debug.network_test_ride_spot, "x,y,forward,left": the Longsword rider's body-local spot on the deck and its door walk's
throttle; FALSE for none (the survey) or one malformed */
static boolean ride_spot_parse(
	char const *text,
	real *x,
	real *y,
	real *exit_forward,
	real *exit_left)
{
	float values[4];
	char extra;
	short index;

	if (!text[0] || sscanf(text, "%f,%f,%f,%f%c", &values[0], &values[1], &values[2], &values[3], &extra) != 4)
		return FALSE;
	for (index = 0; index < 4; index++)
	{
		/* (not a number, or off any vehicle) */
		if (!(fabs(values[index]) < 100.f))
			return FALSE;
	}
	*x = values[0];
	*y = values[1];
	*exit_forward = values[2];
	*exit_left = values[3];
	return TRUE;
}

static void ride_settings(
	void)
{
	char const *name = config_string("debug.network_test_ride");
	char const *driver = config_string("debug.network_test_ride_driver");
	char const *session = config_string("debug.network_test_ride_session");
	char const *spot = config_string("debug.network_test_ride_spot");
	short index;

	csmemset(&ride, 0, sizeof(ride));
	ride_reset();
	if (!name[0] || network_test.mode == _network_test_off)
		return;
	ride.enabled = TRUE;
	snprintf(ride.session, sizeof(ride.session), "%s", session);
	for (index = 0; index < NUMBER_OF_RIDE_SCENARIOS && strcmp(name, ride_names[index]); index++)
	{
	}
	ride.scenario = index < NUMBER_OF_RIDE_SCENARIOS ? index : _ride_straight;
	if (index == NUMBER_OF_RIDE_SCENARIOS)
		ride_error("invalid-scenario");
	if (!session[0] || strlen(session) >= sizeof(ride.session) || strpbrk(session, " \t\r\n="))
	{
		snprintf(ride.session, sizeof(ride.session), "invalid");
		ride_error("invalid-session");
	}
	if (strcmp(driver, "host") && strcmp(driver, "client"))
		ride_error("invalid-driver");
	ride.have_spot = ride_spot_parse(spot, &ride.spot_x, &ride.spot_y, &ride.exit_forward, &ride.exit_left);
	ride.corner = config_boolean("debug.network_test_ride_corner") != 0;
	if (spot[0] && !ride.have_spot)
		ride_error("invalid-spot");
	ride.client_driver = !strcmp(driver, "client");
	/* (the other scripts move and seat the same players) */
	if (network_test.kill_interval > 0.0f || network_test.shoot_interval > 0.0f ||
		network_test.vehicle_time > 0.0f || network_test.pickup_time > 0.0f)
	{
		ride_error("conflicting-scripts");
	}
}

/* the local engine creates both players when the deferred map load finishes */
static void network_test_start_local(
	void)
{
	if (!ride.enabled || ride.scenario != _ride_longsword)
	{
		ride_error("local-only-longsword");
		return;
	}
	ui_widgets_close_all();
	game_connection_set(_game_connection_local);
	player_spawn_count = 2;
	player_ui_set_single_player_local_player_controller(0, 0);
	player_ui_set_single_player_local_player_controller(1, 1);
	game_set_game_variant_from_name("slayer");
	main_set_map_name(network_test.map_name);
	main_menu_switch_to_single_player();
}

/* driven reserves its bounded recovery time in jumping, shifting every later phase */
static long ride_phase_tick(
	short scenario,
	long tick)
{
	return tick + (scenario == _ride_driven && tick >= RIDE_CROSS ? RIDE_DRIVEN_EXTRA : 0);
}

/* a test's phase at elapsed ticks from its board tick */
static short ride_phase(
	short scenario,
	long elapsed)
{
	if (elapsed < 0)
		return _ride_phase_prepare;
	/* (the Longsword runs its own programme: no walk, jumps or crossing by phase) */
	if (scenario == _ride_longsword)
		return elapsed < RIDE_TILT_SETTLE ? _ride_phase_stand : _ride_phase_observe;
	if (elapsed < RIDE_WALK)
		return _ride_phase_stand;
	if (elapsed < RIDE_JUMP)
		return _ride_phase_walk;
	if (elapsed < ride_phase_tick(scenario, RIDE_CROSS))
		return _ride_phase_jump;
	if (elapsed < ride_phase_tick(scenario, RIDE_STEP_OFF))
		return _ride_phase_cross;
	if (elapsed < (scenario == _ride_brush ? RIDE_STEP_OFF + RIDE_BRUSH_STEP_TICKS : ride_phase_tick(scenario, RIDE_OBSERVE)))
		return _ride_phase_step_off;
	return _ride_phase_observe;
}

/* the velocity and the angular velocity the host gives a scenario's
vehicle at elapsed ticks from the board tick, its forward along the
ground; none for driven (its driver's throttle moves it) */
static void ride_motion(
	short scenario,
	long elapsed,
	real_vector3d const *forward,
	real_vector3d *velocity,
	real_vector3d *angular)
{
	real speed = 0.f;

	velocity->i = velocity->j = velocity->k = 0.f;
	angular->i = angular->j = angular->k = 0.f;
	if (elapsed < 0)
		return;
	switch (scenario)
	{
	case _ride_straight:
		/* (braking through the fifth jump) */
		if (elapsed < RIDE_BRAKE)
			speed = RIDE_STRAIGHT_SPEED;
		else if (elapsed < RIDE_BRAKE + RIDE_BRAKE_TICKS)
			speed = RIDE_STRAIGHT_SPEED*(real)(RIDE_BRAKE + RIDE_BRAKE_TICKS - elapsed)/(real)RIDE_BRAKE_TICKS;
		break;
	case _ride_circle:
	case _ride_gun:
	case _ride_coop:
	case _ride_coop_double:
		speed = RIDE_CIRCLE_SPEED;
		angular->k = RIDE_CIRCLE_TURN;
		break;
	case _ride_rise:
		/* (lifted under its riders standing, held, and let down) */
		if (elapsed >= 0 && elapsed < RIDE_RISE_TICKS)
			velocity->k = RIDE_RISE_SPEED;
		else if (elapsed >= 2*RIDE_RISE_TICKS && elapsed < 3*RIDE_RISE_TICKS)
			velocity->k = -RIDE_RISE_SPEED;
		break;
	case _ride_convoy:
		/* (held in place until its board tick) */
		speed = RIDE_CONVOY_SPEED;
		break;
	case _ride_brush:
		/* (meet both flanks within the step-off window, then pass forward;
		only about 0.2 wu beyond first contact, not through the riders) */
		if (elapsed >= RIDE_STEP_OFF + RIDE_BRUSH_STEP_TICKS)
		{
			if (elapsed < RIDE_STEP_OFF + RIDE_BRUSH_STEP_TICKS + RIDE_BRUSH_APPROACH_TICKS)
			{
				velocity->i = forward->j*RIDE_BRUSH_SPEED;
				velocity->j = -forward->i*RIDE_BRUSH_SPEED;
			}
			else
				speed = RIDE_BRUSH_SPEED;
		}
		break;
	default:
		break;
	}
	velocity->i += forward->i*speed;
	velocity->j += forward->j*speed;
}

/* the slot of the jump an elapsed tick lies in (a jump is scheduled at its
slot's first tick; the phase's end is the caller's to bound); NONE before the
first */
static short ride_jump_slot(
	long elapsed)
{
	if (elapsed < RIDE_JUMP)
		return NONE;
	return (short)((elapsed - RIDE_JUMP)/RIDE_JUMP_PERIOD);
}

/* fixed board-relative input phases on every machine; measured preparation, tilt and landing are judged, not transitions */
static short ride_pilot_at(
	long elapsed,
	long *hold)
{
	static long const starts[] = { 0, RIDE_PILOT_CLIMB, RIDE_PILOT_TURN, RIDE_PILOT_LEVEL, RIDE_PILOT_HOVER, RIDE_PILOT_PITCH,
		RIDE_PILOT_PITCH_LEVEL, RIDE_PILOT_BANK, RIDE_PILOT_BANK_LEVEL, RIDE_PILOT_COMBINED,
		RIDE_PILOT_RECOVER, RIDE_PILOT_DESCEND, RIDE_PILOT_DOOR };
	short step = (short)(sizeof(starts)/sizeof(starts[0]) - 1);

	while (step > 0 && elapsed < starts[step])
		step--;
	*hold = elapsed - starts[step];
	return step;
}

/* human-plane pitch follows facing; banking comes from turning across forward velocity, not a roll axis */
static void ride_pilot_action(
	short step,
	real yaw,
	long grounded,
	struct player_action *action)
{
	action->control_flags = 0;
	action->primary_trigger = 0.f;
	action->throttle.i = 0.f;
	action->throttle.j = 0.f;
	action->desired_weapon_index = NONE;
	action->desired_grenade_index = NONE;
	action->desired_zoom_level = NONE;
	action->desired_facing.yaw = yaw;
	action->desired_facing.pitch = 0.f;
	if (step == 1 || step == 2)
	{
		SET_FLAG(action->control_flags, _unit_control_jump_bit, TRUE);
	}
	if (step == 5 || step == 7 || step == 9)
	{
		action->throttle.i = 1.f;
		if (step == 5)
			action->desired_facing.pitch = 75.f*_pi/180.f;
		if (step == 7 || step == 9)
			action->desired_facing.yaw = yaw - 1.2f;
		if (step == 9)
			action->desired_facing.pitch = 40.f*_pi/180.f;
	}
	/* (a short ground ray can miss at flight height; contact, not its fallback clearance, ends descent) */
	if (step == 11 && grounded == 0)
		SET_FLAG(action->control_flags, _unit_control_crouch_modifier_bit, TRUE);
	if (step == 10)
		SET_FLAG(action->control_flags, _unit_control_jump_bit, TRUE);
}

static boolean ride_tilt_landed(
	long grounded)
{
	/* (one contact tick is not enough to start walking out of the ship) */
	return grounded >= 30;
}

/* a preparation step's setpoint (radians) and the ticks it turns to it from the last step's */
static long ride_tilt_ramp(
	short step,
	real pair,
	real from_pitch,
	real from_roll,
	real *to_pitch,
	real *to_roll)
{
	real pitch_turn;
	real roll_turn;

	*to_pitch = ride_tilt_steps[step].pair ? pair : (real)ride_tilt_steps[step].pitch*_pi/180.f;
	*to_roll = ride_tilt_steps[step].pair ? pair : (real)ride_tilt_steps[step].roll*_pi/180.f;
	pitch_turn = (real)fabs(*to_pitch - from_pitch);
	roll_turn = (real)fabs(*to_roll - from_roll);
	return (long)ceil((pitch_turn > roll_turn ? pitch_turn : roll_turn)/RIDE_TILT_CAP - .0001f);
}

/* the Longsword's setpoint at elapsed ticks from its board tick (radians), the step it is in (NONE in the settle) and the
tick of that step's hold (NONE while it turns); the same on every machine, so each rider's script knows the programme
without the host's word */
static short ride_tilt_at(
	long elapsed,
	real pair,
	real *pitch,
	real *roll,
	long *hold)
{
	real from_pitch = 0.f;
	real from_roll = 0.f;
	long start = RIDE_TILT_SETTLE;
	short step;

	*pitch = *roll = 0.f;
	*hold = elapsed;
	if (elapsed < RIDE_TILT_SETTLE)
		return NONE;
	for (step = 0; step < RIDE_TILT_STEPS; step++)
	{
		real to_pitch;
		real to_roll;
		long ramp = ride_tilt_ramp(step, pair, from_pitch, from_roll, &to_pitch, &to_roll);

		if (elapsed < start + ramp)
		{
			real part = (real)(elapsed - start)/(real)ramp;

			*pitch = from_pitch + (to_pitch - from_pitch)*part;
			*roll = from_roll + (to_roll - from_roll)*part;
			*hold = NONE;
			return step;
		}
		*pitch = to_pitch;
		*roll = to_roll;
		if (elapsed < start + ramp + RIDE_TILT_HOLD || step == RIDE_TILT_STEPS - 1)
		{
			*hold = elapsed - start - ramp;
			return step;
		}
		start += ramp + RIDE_TILT_HOLD;
		from_pitch = to_pitch;
		from_roll = to_roll;
	}
	return RIDE_TILT_STEPS - 1;
}

/* the ticks from the board tick to the programme's end: the last step's hold */
static long ride_tilt_end(
	real pair)
{
	real from_pitch = 0.f;
	real from_roll = 0.f;
	long end = RIDE_TILT_SETTLE;
	short step;

	for (step = 0; step < RIDE_TILT_STEPS; step++)
	{
		real to_pitch;
		real to_roll;

		end += ride_tilt_ramp(step, pair, from_pitch, from_roll, &to_pitch, &to_roll) + RIDE_TILT_HOLD;
		from_pitch = to_pitch;
		from_roll = to_roll;
	}
	/* (the last step sets the vehicle down before its door walk's hold) */
	end += RIDE_TILT_DESCEND;
	return end;
}

/* the pair step's angle: nine tenths of the angle off upright at which protection starts (the rider's surface limit,
or 45 degrees where that is steeper), so each of the two is within it and their total tilt is past it */
static real ride_tilt_pair(
	real minimum_normal_k)
{
	real limit = minimum_normal_k > .70710678f ? minimum_normal_k : .70710678f;

	if (limit > 1.f)
		limit = 1.f;
	return .9f*(real)acos(limit);
}

/* the orientation of a heading pitched nose up and then rolled the left wing down (up leaning to the body's left,
+y): its up's k is the total tilt's cosine, cos pitch cos roll */
static void ride_tilt_frame(
	real yaw,
	real pitch,
	real roll,
	real_vector3d *forward,
	real_vector3d *up)
{
	real cosine = (real)cos(yaw);
	real sine = (real)sin(yaw);
	real i;

	forward->i = (real)cos(pitch);
	forward->j = 0.f;
	forward->k = (real)sin(pitch);
	up->i = -(real)sin(pitch)*(real)cos(roll);
	up->j = (real)sin(roll);
	up->k = (real)cos(pitch)*(real)cos(roll);
	{
		i = forward->i;
		forward->i = i*cosine - forward->j*sine;
		forward->j = i*sine + forward->j*cosine;
		i = up->i;
		up->i = i*cosine - up->j*sine;
		up->j = i*sine + up->j*cosine;
	}
}

/* the Longsword rider's input at elapsed ticks from the board tick: facing the vehicle's yaw, still, its jump pressed
once in a jump step's hold (the step is its slot, as a jump phase's slot is), the door walk's throttle once the last
step has set the vehicle down */
static void ride_tilt_action(
	long elapsed,
	real pair,
	real yaw,
	real exit_forward,
	real exit_left,
	short last_jump,
	boolean pressed_now,
	struct player_action *action)
{
	real pitch;
	real roll;
	long hold;
	short step = ride_tilt_at(elapsed, pair, &pitch, &roll, &hold);

	action->control_flags = 0;
	action->primary_trigger = 0.f;
	action->throttle.i = 0.f;
	action->throttle.j = 0.f;
	action->desired_weapon_index = NONE;
	action->desired_grenade_index = NONE;
	action->desired_zoom_level = NONE;
	action->desired_facing.yaw = yaw;
	action->desired_facing.pitch = 0.f;
	if (step == NONE || hold == NONE)
		return;
	if (step == RIDE_TILT_STEPS - 1 && hold >= RIDE_TILT_DESCEND)
	{
		action->throttle.i = exit_forward;
		action->throttle.j = exit_left;
		return;
	}
	if (step == RIDE_TILT_STEPS - 1)
		return;
	if (ride_tilt_steps[step].jump && hold >= RIDE_TILT_JUMP_AT &&
		(step > last_jump || (pressed_now && step == last_jump)))
	{
		SET_FLAG(action->control_flags, _unit_control_jump_bit, TRUE);
	}
}

/* the slot a jump pressed at elapsed counts in: the jump phase's, the Longsword's step */
static short ride_press_slot(
	short scenario,
	long elapsed,
	real pair)
{
	real pitch;
	real roll;
	long hold;

	if (scenario == _ride_longsword)
		return ride_tilt_at(elapsed, pair, &pitch, &roll, &hold);
	return ride_jump_slot(elapsed);
}

/* whether a vehicle's tag is a scenario's: the Longsword by its tag's name (Extinction's is
vehicles\\fighterbomber\\fighterbomber_mp), else the Warthog */
static boolean ride_vehicle_named(
	short scenario,
	char const *name)
{
	if (scenario == _ride_longsword)
		return strstr(name, "fighterbomber") != NULL;
	return !strcmp(name, "vehicles\\warthog\\warthog");
}

/* the left-lane convoy rider leaves away from the other hull */
static real ride_step_off_side(
	short scenario,
	long deck_vehicle)
{
	return scenario == _ride_convoy && deck_vehicle == ride.vehicles[1] ? 1.f : -1.f;
}

/* a player's input at elapsed ticks from the board tick: facing the
vehicle's yaw, a driver its throttle and a facing off it (so it turns), a
rider the script's walk, jumps, crossing and step */
static void ride_script_action(
	short scenario,
	long elapsed,
	boolean driver,
	real yaw,
	short last_jump,
	boolean pressed_now,
	boolean grounded,
	real walk_offset,
	long deck_vehicle,
	struct player_action *action)
{
	action->control_flags = 0;
	action->primary_trigger = 0.f;
	action->throttle.i = 0.f;
	action->throttle.j = 0.f;
	action->desired_weapon_index = NONE;
	action->desired_grenade_index = NONE;
	action->desired_zoom_level = NONE;
	action->desired_facing.yaw = yaw;
	action->desired_facing.pitch = 0.f;
	if (driver)
	{
		/* (the Longsword's pilot sits still: the host's script turns the vehicle) */
		if (scenario == _ride_longsword)
			return;
		action->throttle.i = RIDE_DRIVE_THROTTLE;
		action->desired_facing.yaw = yaw + RIDE_DRIVE_TURN;
		return;
	}
	switch (ride_phase(scenario, elapsed))
	{
	case _ride_phase_walk:
		action->throttle.i = ((elapsed - RIDE_WALK)/RIDE_WALK_PERIOD) & 1 ? -RIDE_WALK_THROTTLE : RIDE_WALK_THROTTLE;
		/* (under the gun, start away from the mount, then return to the spot) */
		if (scenario == _ride_gun || scenario == _ride_coop || scenario == _ride_coop_double || scenario == _ride_convoy)
			action->throttle.i = -action->throttle.i;
		/* (turned back in deck coordinates, after the inversion above: the facing is the vehicle's heading, so a positive
		throttle is +x on the deck in every scenario. On a moving deck a forward leg carries further than a backward one,
		and the rider would creep off the hood's front edge) */
		if (walk_offset >= RIDE_WALK_RANGE)
			action->throttle.i = -RIDE_WALK_THROTTLE;
		else if (walk_offset <= -RIDE_WALK_RANGE)
			action->throttle.i = RIDE_WALK_THROTTLE;
		break;
	case _ride_phase_jump:
		/* (the first request at or after a jump's scheduled tick presses it, once: input is sampled per frame and a late frame runs
		its ticks on one sample, so the exact tick may never be asked; a slot's jump missed whole stays missed) */
		if (ride_jump_slot(elapsed) > last_jump || (pressed_now && ride_jump_slot(elapsed) == last_jump))
			SET_FLAG(action->control_flags, _unit_control_jump_bit, TRUE);
		/* (a jump lands about .05 wu ahead on a moving deck, so between jumps a rider on the deck walks back toward where the
		walk began: never in the air, on a press tick (the branch above), nor in the settle ahead of the next) */
		else if (grounded && (elapsed - RIDE_JUMP) % RIDE_JUMP_PERIOD < RIDE_JUMP_PERIOD - RIDE_JUMP_SETTLE)
		{
			if (walk_offset >= RIDE_WALK_RANGE)
				action->throttle.i = -RIDE_WALK_THROTTLE;
			else if (walk_offset <= -RIDE_WALK_RANGE)
				action->throttle.i = RIDE_WALK_THROTTLE;
		}
		break;
	case _ride_phase_cross:
		if (scenario == _ride_convoy)
		{
			action->throttle.j = RIDE_SIDE_THROTTLE;
			/* (the measured standable gap needs a hop after a short run-up;
			the jump phase still has its own ten presses) */
			if (elapsed >= RIDE_CROSS + RIDE_CONVOY_HOP_TICKS &&
				(last_jump < RIDE_JUMPS || (pressed_now && last_jump == RIDE_JUMPS)))
			{
				SET_FLAG(action->control_flags, _unit_control_jump_bit, TRUE);
			}
		}
		break;
	case _ride_phase_step_off:
		if (scenario != _ride_brush || elapsed < RIDE_STEP_OFF + RIDE_BRUSH_STEP_TICKS)
		{
			action->throttle.j = ride_step_off_side(scenario, deck_vehicle) * RIDE_SIDE_THROTTLE;
		}
		break;
	default:
		break;
	}
}

/* a vehicle's heading: its forward along the ground, and its yaw */
static real ride_heading(
	long vehicle_index,
	real_vector3d *forward)
{
	struct object_datum *object = object_get(vehicle_index);

	forward->i = object->object.forward.i;
	forward->j = object->object.forward.j;
	forward->k = 0.f;
	if (normalize3d(forward) < .001f)
	{
		forward->i = 1.f;
		forward->j = 0.f;
	}
	return (real)atan2(forward->j, forward->i);
}

/* the ground's height under a point (the map's structure only) */
static boolean ride_ground_height(
	real_point3d const *point,
	real *height)
{
	struct collision_result hit;
	real_point3d start = *point;
	real_vector3d down;

	start.z += 50.f;
	down.i = down.j = 0.f;
	down.k = -100.f;
	if (!collision_test_vector(FLAG(_collision_test_structure_bit) | FLAG(_collision_test_front_facing_surfaces_bit),
		&start, &down, NONE, &hit))
	{
		return FALSE;
	}
	*height = hit.point.z;
	return TRUE;
}

/* whether the ground runs level from point along angle for length */
static boolean ride_level_along(
	real_point3d const *point,
	real angle,
	real length)
{
	real previous = 0.f;
	real ground = 0.f;
	real distance;
	boolean level = ride_ground_height(point, &previous);

	for (distance = RIDE_LEVEL_STEP; level && distance <= length; distance += RIDE_LEVEL_STEP)
	{
		real_point3d at = *point;

		at.x += (real)cos(angle)*distance;
		at.y += (real)sin(angle)*distance;
		level = ride_ground_height(&at, &ground) && fabs(ground - previous) <= RIDE_LEVEL_RISE;
		previous = ground;
	}
	return level;
}

/* how much the ground along the line offset to the left of point, for length
along angle, differs from the ground along point's own, for each wu of the
offset, at the most (a huge rise for ground missing) */
static real ride_level_across(
	real_point3d const *point,
	real angle,
	real length,
	real offset)
{
	real distance;
	real rise = 0.f;

	for (distance = 0.f; distance <= length; distance += RIDE_LEVEL_STEP)
	{
		real_point3d centre = *point;
		real_point3d beside;
		real ground;
		real beside_ground;

		centre.x += (real)cos(angle)*distance;
		centre.y += (real)sin(angle)*distance;
		beside = centre;
		beside.x += (real)cos(angle + _pi/2.f)*offset;
		beside.y += (real)sin(angle + _pi/2.f)*offset;
		if (!ride_ground_height(&centre, &ground) || !ride_ground_height(&beside, &beside_ground))
			return 1.0e6f;
		if (fabs(beside_ground - ground)/offset > rise)
			rise = (real)fabs(beside_ground - ground)/offset;
	}
	return rise;
}

/* whether the ground runs level for length along angle from the line offset
to the left of point */
static boolean ride_level_beside(
	real_point3d const *point,
	real angle,
	real length,
	real offset)
{
	real_point3d beside = *point;

	beside.x += (real)cos(angle + _pi/2.f)*offset;
	beside.y += (real)sin(angle + _pi/2.f)*offset;
	return ride_level_along(&beside, angle, length);
}

/* co-op's two flank runs, with their ground close to the centre's height;
parallel lanes alone can be flat along a steep cross slope */
static boolean ride_level_flanks(
	real_point3d const *point,
	real angle,
	real length)
{
	real distance;
	real half = RIDE_COOP_HALF_WIDTH;

	if (!ride_level_beside(point, angle, length, half) || !ride_level_beside(point, angle, length, -half))
		return FALSE;
	for (distance = 0.f; distance <= length; distance += RIDE_LEVEL_STEP)
	{
		real_point3d centre = *point;
		real_point3d left;
		real_point3d right;
		real ground;
		real left_ground;
		real right_ground;

		centre.x += (real)cos(angle)*distance;
		centre.y += (real)sin(angle)*distance;
		left = right = centre;
		left.x += (real)cos(angle + _pi/2.f)*half;
		left.y += (real)sin(angle + _pi/2.f)*half;
		right.x -= (real)cos(angle + _pi/2.f)*half;
		right.y -= (real)sin(angle + _pi/2.f)*half;
		if (!ride_ground_height(&centre, &ground) || !ride_ground_height(&left, &left_ground) ||
			!ride_ground_height(&right, &right_ground) || fabs(left_ground - ground) > RIDE_COOP_CROSS_RISE ||
			fabs(right_ground - ground) > RIDE_COOP_CROSS_RISE)
		{
			return FALSE;
		}
	}
	return TRUE;
}

/* the first of sixteen yaws from point along which the ground runs level
for length, and (if not 0) for behind behind the point and along the lanes
lane and RIDE_CONVOY_LANE_NEAR to its left, their ground across level too;
co-op's flanks level across; with a turn (not 0), each yaw is followed by the
same one turned left by that much (the same checks); FALSE for none */
static boolean ride_level_heading(
	real_point3d const *point,
	real length,
	real behind,
	real lane,
	real turn,
	real *yaw,
	real *rise)
{
	real best = 0.f;
	boolean found = FALSE;
	short heading;

	for (heading = 0; heading < (turn != 0.f ? 32 : 16); heading++)
	{
		real angle = (real)(turn != 0.f ? heading/2 : heading)*_pi/8.f + (turn != 0.f && (heading & 1) ? turn : 0.f);
		real cross;

		if (!ride_level_along(point, angle, length))
			continue;
		if (behind > 0.f && !ride_level_along(point, angle + _pi, behind))
			continue;
		if (lane > 0.f && !(ride_level_beside(point, angle, length, RIDE_CONVOY_LANE_NEAR) &&
			ride_level_beside(point, angle, length, lane)))
			continue;
		if ((ride.scenario == _ride_coop || ride.scenario == _ride_coop_double) &&
			(!ride_level_flanks(point, angle, length) || (behind > 0.f && !ride_level_flanks(point, angle + _pi, behind))))
		{
			continue;
		}
		if (lane <= 0.f)
		{
			*yaw = angle;
			*rise = 0.f;
			return TRUE;
		}
		cross = ride_level_across(point, angle, length, RIDE_CONVOY_LANE_NEAR);
		if (ride_level_across(point, angle, length, lane) > cross)
			cross = ride_level_across(point, angle, length, lane);
		if (cross > RIDE_CONVOY_CROSS_RISE)
			continue;
		if (!found || cross < best - RIDE_CONVOY_CROSS_TIE)
		{
			best = cross;
			*yaw = angle;
			found = TRUE;
		}
	}
	*rise = best;
	return found;
}

/* the points of the search's ring at radius */
static short ride_level_ring_points(
	real radius)
{
	return 16*(short)ceil(radius/RIDE_SEARCH_RADIUS);
}

/* a spot with a level run (ahead, behind and along the lanes): point itself, else the nearest ground (rings of
sixteen points, then more further out, each ring's first due east, then counterclockwise) that has
one behind and along lane too, moved there with the vehicle's rest height
over the ground; with lanes, the spot whose lanes lie the least across the
slope of all within reach (the earlier found of equals: nearer ring, earlier point, earlier heading),
else the first found; FALSE for none within reach (the Blood Gulch Warthog's own
spot has none) */
static boolean ride_level_spot(
	real_point3d *position,
	real rest,
	real lane,
	real run,
	real reach,
	real turn,
	real *yaw)
{
	real_point3d spot;
	real spot_yaw = 0.f;
	real best = 0.f;
	real candidate;
	real radius;
	real rise;
	boolean found = FALSE;
	boolean done = FALSE;
	short point;

	if (ride_level_heading(position, run, RIDE_LEVEL_BEHIND, lane, turn, yaw, &rise))
	{
		spot = *position;
		spot_yaw = *yaw;
		best = rise;
		found = TRUE;
		done = lane <= 0.f || rise <= 0.f;
	}
	for (radius = RIDE_SEARCH_STEP; !done && radius <= reach; radius += RIDE_SEARCH_STEP)
	{
		short points = ride_level_ring_points(radius);

		for (point = 0; !done && point < points; point++)
		{
			real angle = (real)point*_pi/(real)(points/2);
			real_point3d at = *position;
			real ground;

			at.x += (real)cos(angle)*radius;
			at.y += (real)sin(angle)*radius;
			if (!ride_ground_height(&at, &ground))
				continue;
			at.z = ground + rest;
			if (ride_level_heading(&at, run, RIDE_LEVEL_BEHIND, lane, turn, &candidate, &rise))
			{
				if (!found || rise < best - RIDE_CONVOY_CROSS_TIE)
				{
					best = rise;
					spot = at;
					spot_yaw = candidate;
					found = TRUE;
				}
				done = lane <= 0.f || best <= 0.f;
			}
		}
	}
	if (found)
	{
		*position = spot;
		*yaw = spot_yaw;
	}
	return found;
}

/* the deck under a point of a vehicle: the lowest surface of its model on
the vertical line through point that a biped stands on with room above it
(a gun over the bed is no deck; the bed under it is); FALSE for none */
static boolean ride_deck_surface(
	long vehicle_index,
	real_point3d const *point,
	real room,
	real minimum_normal_k,
	real_point3d *surface)
{
	struct object_datum *object = object_get(vehicle_index);
	struct collision_model_instance instance;
	real_point3d start = *point;
	real bottom = object->object.bounding_sphere_center.z - object->object.bounding_sphere_radius - 1.f;
	real above = 1.0e6f;
	boolean found = FALSE;
	short hits;

	if (!collision_model_instance_new(&instance, vehicle_index))
		return FALSE;
	start.z = object->object.bounding_sphere_center.z + object->object.bounding_sphere_radius + 1.f;
	for (hits = 0; hits < 8 && start.z > bottom; hits++)
	{
		struct collision_model_test_vector_result hit;
		real_vector3d down;
		real_vector3d normal;
		real z;

		down.i = down.j = 0.f;
		down.k = bottom - start.z;
		if (!collision_model_test_vector(&instance, FLAG(_collision_test_front_facing_surfaces_bit), &start, &down, &hit))
			break;
		z = start.z + down.k*hit.bsp_result.t;
		if (hit.node_index >= 0 && hit.node_index < instance.node_count && hit.bsp_result.plane)
		{
			matrix4x3_transform_normal(&instance.matrices[hit.node_index], &hit.bsp_result.plane->n, &normal);
			if (normal.k >= minimum_normal_k && above - z >= room)
			{
				*surface = start;
				surface->z = z;
				found = TRUE;
			}
		}
		above = z;
		start.z = z - .01f;
	}
	return found;
}

/* how far the whole hull reaches to a side (direction 1 its left, -1 its
right), wheels included: a bed's narrower edge is not collision clearance */
static boolean ride_deck_edge(
	long vehicle_index,
	real direction,
	real_point3d *edge)
{
	struct vehicle_riding_vehicle vehicle;
	boolean found = FALSE;
	short step;
	short row;
	short widest = -1;

	if (!biped_riding_vehicle(vehicle_index, NONE, &vehicle))
		return FALSE;
	for (row = -12; row <= 12; row++)
	{
		for (step = 0; step <= 160; step++)
		{
			real_point3d local;
			real_point3d point;
			real_point3d surface;

			local.x = .1f*(real)row;
			local.y = direction*RIDE_CONVOY_GAP*(real)step;
			local.z = 0.f;
			vehicle_riding_point_to_world(&vehicle.position, &vehicle.forward, &vehicle.up, &local, &point);
			if (!ride_deck_surface(vehicle_index, &point, 0.f, -1.f, &surface))
				continue;
			if (step > widest)
			{
				*edge = surface;
				widest = step;
				found = TRUE;
			}
		}
	}
	return found;
}

/* a convoy vehicle's speed along one line (sideways across the corridor, or
fore and aft behind the lead) for how far it is from its place on it (that
less where it is), held to the most the hold may ask */
static real ride_convoy_lateral(
	real error)
{
	real speed = RIDE_CONVOY_HOLD_GAIN*error;

	return speed > RIDE_CONVOY_HOLD_CAP ? RIDE_CONVOY_HOLD_CAP : speed < -RIDE_CONVOY_HOLD_CAP ? -RIDE_CONVOY_HOLD_CAP : speed;
}

/* a convoy vehicle's turn rate (counterclockwise) for how far its yaw is from
the convoy heading saved when the hull was placed (the short way round; both
vehicles are held to it, not the second to the first) */
static real ride_convoy_turn(
	real error)
{
	real rate;

	while (error > _pi)
		error -= 2.f*_pi;
	while (error < -_pi)
		error += 2.f*_pi;
	rate = RIDE_CONVOY_HOLD_TURN_GAIN*error;
	return rate > RIDE_CONVOY_HOLD_TURN_CAP ? RIDE_CONVOY_HOLD_TURN_CAP : rate < -RIDE_CONVOY_HOLD_TURN_CAP ? -RIDE_CONVOY_HOLD_TURN_CAP : rate;
}

/* the height of the convoy's second vehicle: as far over its own ground as
the first is over its (ground a little higher at its side would bury it) */
static real ride_convoy_height(
	real first_z,
	real first_ground,
	real second_ground)
{
	return second_ground + (first_z - first_ground);
}

/* the convoy's second vehicle on the first's left, their full hulls
RIDE_CONVOY_HULL_GAP apart, measured again at its own ground's height */
static boolean ride_convoy_place(
	void)
{
	struct object_datum *first = object_get(ride.vehicles[0]);
	real_point3d left_edge;
	real_point3d right_edge;
	real_point3d position;
	real_vector3d left;
	real_vector3d across;
	real first_ground;
	real ground;
	real gap;

	if (!ride_ground_height(&first->object.position, &first_ground))
		return FALSE;
	cross_product3d(&first->object.up, &first->object.forward, &left);
	normalize3d(&left);
	position = first->object.position;
	position.x += left.i*4.f;
	position.y += left.j*4.f;
	if (!ride_ground_height(&position, &ground))
		return FALSE;
	position.z = ride_convoy_height(first->object.position.z, first_ground, ground);
	object_set_position(ride.vehicles[1], &position, &first->object.forward, &first->object.up);
	if (!ride_deck_edge(ride.vehicles[0], 1.f, &left_edge) || !ride_deck_edge(ride.vehicles[1], -1.f, &right_edge))
		return FALSE;
	vector_from_points3d(&left_edge, &right_edge, &across);
	gap = dot_product3d(&across, &left) - RIDE_CONVOY_HULL_GAP;
	position.x -= left.i*gap;
	position.y -= left.j*gap;
	if (!ride_ground_height(&position, &ground))
		return FALSE;
	position.z = ride_convoy_height(first->object.position.z, first_ground, ground);
	object_set_position(ride.vehicles[1], &position, &first->object.forward, &first->object.up);
	if (!ride_deck_edge(ride.vehicles[1], -1.f, &right_edge))
		return FALSE;
	vector_from_points3d(&left_edge, &right_edge, &across);
	gap = dot_product3d(&across, &left);
	ride_log("geometry", "tick=%ld gap=%.4f", game_time_get(), gap);
	ride.convoy_offset = (position.x - first->object.position.x)*left.i + (position.y - first->object.position.y)*left.j;
	ride.convoy_origin = first->object.position;
	ride.convoy_yaw = (real)atan2(first->object.forward.j, first->object.forward.i);
	return gap >= 0.f && gap <= RIDE_CONVOY_HULL_GAP + RIDE_CONVOY_MAXIMUM_GAP;
}

/* the first vehicle of a palette a scenario takes (ride_vehicle_named) */
static long ride_palette_vehicle(
	struct tag_block *palette)
{
	long index;

	for (index = 0; index < palette->count; index++)
	{
		struct scenario_object_palette_entry *entry =
			TAG_BLOCK_GET_ELEMENT(palette, index, struct scenario_object_palette_entry);

		if (entry->reference.index != NONE && ride_vehicle_named(ride.scenario, tag_get_name(entry->reference.index)))
			return entry->reference.index;
	}
	return NONE;
}

static long ride_new(
	long definition_index,
	real_point3d const *position)
{
	struct object_placement_data data;

	if (definition_index == NONE)
		return NONE;
	object_placement_data_new(&data, definition_index, NONE);
	data.position = *position;
	return object_new(&data);
}

static boolean ride_seat(
	long unit_index,
	long vehicle_index)
{
	short seat;
	short count = unit_definition_get(object_get(vehicle_index)->definition_index)->unit.seats.count;

	for (seat = 0; seat < count; seat++)
	{
		if (unit_seat_is_driver(vehicle_index, seat) && unit_enter_seat(unit_index, vehicle_index, seat))
			return TRUE;
	}
	return FALSE;
}

/* the test's vehicles: the live Warthogs of the lowest indices, the same
on every machine */
static boolean ride_find_vehicles(
	void)
{
	struct object_iterator iterator;
	short count = 0;

	ride.vehicles[0] = ride.vehicles[1] = NONE;
	object_iterator_new(&iterator, _object_mask_vehicle, 0);
	while (count < ride_vehicle_count() && object_iterator_next(&iterator))
	{
		struct object_datum *object = object_get(iterator.index);

		if (!TEST_FLAG(object->object.damage_flags, _object_dead_bit) &&
			ride_vehicle_named(ride.scenario, tag_get_name(object->definition_index)))
		{
			ride.vehicles[count++] = iterator.index;
		}
	}
	return count == ride_vehicle_count();
}

/* brush steps onto level ground; co-op needs ground level across the hull */
static boolean ride_level_scenario(
	short scenario)
{
	return scenario == _ride_straight || scenario == _ride_convoy ||
		scenario == _ride_coop || scenario == _ride_coop_double || scenario == _ride_brush;
}

/* how far ahead a scenario's spot must be level: its whole drive (from its
own speed and ticks) and RIDE_LEVEL_MARGIN more, never under RIDE_LEVEL_RUN
(the spots chosen for the shorter drives stay where they were) */
static real ride_level_run(
	short scenario)
{
	real drive = 0.f;

	switch (scenario)
	{
	case _ride_straight:
		drive = RIDE_STRAIGHT_SPEED*(real)RIDE_BRAKE + RIDE_STRAIGHT_SPEED*(real)RIDE_BRAKE_TICKS/2.f;
		break;
	case _ride_convoy:
		drive = RIDE_CONVOY_SPEED*(real)RIDE_END;
		break;
	case _ride_brush:
		drive = RIDE_BRUSH_SPEED*(real)(RIDE_END - RIDE_STEP_OFF - RIDE_BRUSH_STEP_TICKS);
		break;
	case _ride_coop:
	case _ride_coop_double:
		/* (a circle: no further than its diameter from where it starts) */
		drive = 2.f*RIDE_CIRCLE_SPEED/RIDE_CIRCLE_TURN;
		break;
	default:
		break;
	}
	return drive + RIDE_LEVEL_MARGIN > RIDE_LEVEL_RUN ? drive + RIDE_LEVEL_MARGIN : RIDE_LEVEL_RUN;
}

/* how far out from the start a scenario's spot is searched for */
static real ride_level_reach(
	short scenario)
{
	return scenario == _ride_convoy ? RIDE_CONVOY_SEARCH_RADIUS : RIDE_SEARCH_RADIUS;
}

/* how far a scenario's heading is turned left from the first level one */
static real ride_level_turn(
	short scenario)
{
	return scenario == _ride_convoy ? RIDE_CONVOY_TURN : 0.f;
}

/* (the host) the vehicles made ready: made near its player if the level
has too few, kept awake, straight's, brush's, co-op's and the convoy's put where the
ground runs level and turned along it, the convoy's second beside the first */
static boolean ride_select_vehicles(
	void)
{
	short index;

	if (!ride_find_vehicles())
	{
		long player_index = local_player_get_player_index(0);
		long definition = ride_palette_vehicle(&global_scenario_get()->vehicle_palette);
		real_point3d near;

		if (player_index == NONE || player_get(player_index)->unit_index == NONE || definition == NONE)
			return FALSE;
		near = object_get(player_get(player_index)->unit_index)->object.position;
		for (index = 0; index < ride_vehicle_count(); index++)
		{
			real_point3d at = near;
			real ground;

			at.x += 6.f + 8.f*(real)index;
			if (!ride_ground_height(&at, &ground))
				return FALSE;
			at.z = ground + 1.f;
			if (ride_new(definition, &at) == NONE)
				return FALSE;
		}
		if (!ride_find_vehicles())
			return FALSE;
	}
	for (index = 0; index < ride_vehicle_count(); index++)
	{
		object_activate(ride.vehicles[index]);
		object_set_automatic_deactivation(ride.vehicles[index], FALSE);
	}
	if (ride_level_scenario(ride.scenario))
	{
		struct object_datum *object = object_get(ride.vehicles[0]);
		real_point3d position = object->object.position;
		real_vector3d forward;
		real_vector3d up;
		real yaw;
		real ground;

		if (!ride_ground_height(&position, &ground))
			return FALSE;
		if (!ride_level_spot(&position, position.z - ground, ride.scenario == _ride_convoy ? RIDE_CONVOY_LANE : 0.f,
			ride_level_run(ride.scenario), ride_level_reach(ride.scenario), ride_level_turn(ride.scenario), &yaw))
			return FALSE;
		forward.i = (real)cos(yaw);
		forward.j = (real)sin(yaw);
		forward.k = 0.f;
		up.i = up.j = 0.f;
		up.k = 1.f;
		/* (a little up so no wheel catches the slope) */
		position.z += .2f;
		object_set_position(ride.vehicles[0], &position, &forward, &up);
	}
	if (ride.scenario == _ride_longsword)
	{
		struct object_datum *object = object_get(ride.vehicles[0]);
		real_point3d position = object->object.position;
		real_vector3d forward;
		real_vector3d up;

		/* board at the map's resting position on both machines; only the pilot takes off */
		ride.tilt_yaw = ride_heading(ride.vehicles[0], &forward);
		up.i = up.j = 0.f;
		up.k = 1.f;
		ride.tilt_ground = position;
		ride.tilt_position = position;
	}
	if (ride.scenario == _ride_convoy && !ride_convoy_place())
		return FALSE;
	/* (a vehicle still flagged at rest goes out only in the netcode's resting round-robin, so a client would keep it at its spawn) */
	for (index = 0; index < ride_vehicle_count(); index++)
		SET_FLAG(object_get(ride.vehicles[index])->object.flags, _object_at_rest_bit, FALSE);
	return TRUE;
}

/* (the host) co-op's palette marine or driven's host or client player in
the first driver's seat; the script moves co-op's AI-driven vehicle */
static boolean ride_assign_driver(
	boolean seat_driver)
{
	if (ride.scenario == _ride_coop || ride.scenario == _ride_coop_double)
	{
		struct tag_block const *palette = &global_scenario_get()->biped_palette;
		struct tag_block const *actor_palette = &global_scenario_get()->ai_actor_palette;
		long definition = NONE;
		long variant = NONE;
		long actor;
		long index;
		real_point3d position = object_get(ride.vehicles[0])->object.position;
		/* (ai_script.c's actor default states are file-local) */
		enum { actor_default_state_none = 0, actor_default_state_alert = 2 };

		if (!ride_host())
			return FALSE;
		for (index = 0; index < palette->count; index++)
		{
			struct scenario_object_palette_entry const *entry = TAG_BLOCK_GET_ELEMENT(palette, index, struct scenario_object_palette_entry);
			char const *leaf;

			if (entry->reference.index == NONE)
				continue;
			leaf = strrchr(tag_get_name(entry->reference.index), '\\');
			if (leaf && !strncmp(leaf + 1, "marine", 6) && (leaf[7] == '\0' || leaf[7] == '_'))
			{
				definition = entry->reference.index;
				break;
			}
		}
		if (definition == NONE)
			return FALSE;
		for (index = 0; index < actor_palette->count; index++)
		{
			struct tag_reference const *entry = TAG_BLOCK_GET_ELEMENT(actor_palette, index, struct tag_reference);

			if (entry->index != NONE && actor_variant_definition_get(entry->index)->unit_reference.index == definition)
			{
				variant = entry->index;
				break;
			}
		}
		if (variant == NONE)
			return FALSE;
		position.z += 2.f;
		ride.driver = ride_new(definition, &position);
		if (ride.driver == NONE)
			return FALSE;
		/* (only actor states carry this non-player's seat to the client) */
		actor = actor_create_for_unit(FALSE, ride.driver, variant, NONE, NONE, FALSE, NONE, FALSE,
			actor_default_state_alert, actor_default_state_none, NONE, 0);
		if (actor == NONE)
			return FALSE;
		/* (the test's velocity script moves the vehicle, not the AI) */
		actor_braindead(actor, TRUE);
		return ride_seat(ride.driver, ride.vehicles[0]);
	}
	if (ride.scenario == _ride_driven || ride.scenario == _ride_longsword)
	{
		struct data_iterator iterator;
		struct player_datum *player;

		data_iterator_new(&iterator, player_data);
		while ((player = (struct player_datum *)data_iterator_next(&iterator)) != NULL)
		{
			boolean client = player->local_player_index == NONE;

			if (ride.scenario == _ride_longsword && !ride_host())
				client = !client;
			if (network_test.mode == _network_test_local)
				client = player->local_player_index != 0;
			if (client == ride.client_driver && player->unit_index != NONE)
			{
				ride.driver = player->unit_index;
				return !seat_driver || ride_seat(ride.driver, ride.vehicles[0]);
			}
		}
		return FALSE;
	}
	return TRUE;
}

/* a rider's body-local place on the measured deck, candidate index of three:
the first rider in the bed (gun's under the gun, on the left), the second on
the hood (brush's both on the right, where they step off); the two never
within a pill's width of each other */
static void ride_board_spot(
	short scenario,
	short ordinal,
	short index,
	real *x,
	real *y)
{
	static real const bed[3] = { -.75f, -.8f, -.7f };
	static real const under_gun[3] = { -.8f, -.805f, -.81f };
	static real const hood[3] = { .6f, .65f, .7f };

	/* (the full pill touches the mount at x -0.75, y 0.2 even though a
	ray finds the bed there; x -0.8 clears it, and its shuffle goes back) */
	/* (co-op's settled pitch overlaps the full pill at x -0.8, y 0;
	its campaign collision probes clear the left spot and backwards leg) */
	*x = ordinal ? hood[index] : scenario == _ride_convoy ? under_gun[index] :
		(scenario == _ride_gun || scenario == _ride_coop || scenario == _ride_coop_double) ?
		under_gun[index] : bed[index];
	*y = scenario == _ride_convoy ? RIDE_BOARD_SIDE : scenario == _ride_brush ? -RIDE_BOARD_SIDE :
		(scenario == _ride_gun || scenario == _ride_coop || scenario == _ride_coop_double) && !ordinal ? RIDE_BOARD_SIDE : 0.f;
}

/* a centre ray can find deck while the pill overlaps a rim beside it;
the setup must fit the whole pill before physics tries to board it */
static boolean ride_board_clear(
	long vehicle_index,
	real_point3d const *surface,
	real height,
	real width)
{
	struct collision_model_instance instance;
	struct collision_feature_list features;
	struct collision_plane collision;
	real_point3d base = *surface;
	real_point3d center;

	if (!collision_model_instance_new(&instance, vehicle_index))
		return FALSE;
	base.z += width + .02f;
	center = base;
	center.z += height*.5f;
	collision_features_new(&features);
	collision_model_get_features_in_sphere(&instance, &center, height*.5f + width, height, width, &features);
	return !collision_features_test_point(&features, &base, &collision);
}

/* a bounded search in each rider's part of the deck; side routes never
cross the centre line, and bed and hood searches stay a pill apart */
static void ride_board_candidate(
	short scenario,
	short ordinal,
	short index,
	real *x,
	real *y)
{
	if (scenario == _ride_longsword)
	{
		*x = ride.spot_x + (index < 3 ? .025f*(index == 1 ? -1 : index == 2) : -.1f + .025f*((index - 3)%9));
		*y = ride.spot_y + (index < 3 ? 0.f : -.1f + .025f*((index - 3)/9));
		return;
	}
	ride_board_spot(scenario, ordinal, index < 3 ? index : 0, x, y);
	if (index < 3)
		return;
	*x = ordinal && scenario != _ride_driven ? .15f + .1f*((index - 3)%9) : -.95f + .05f*((index - 3)%9);
	if (scenario == _ride_driven || *y == 0.f)
		*y = -.2f + .05f*((index - 3)/9);
	else
		*y = (*y > 0.f ? 1.f : -1.f)*(.05f + .05f*((index - 3)/9));
}

/* a rider put on the deck at one of its places (body-local, by its
ordinal), its pill's bottom just over the deck */
static boolean ride_board(
	long unit_index,
	long vehicle_index,
	short ordinal)
{
	struct object_datum *unit = object_get(unit_index);
	struct vehicle_riding_vehicle vehicle;
	real_point3d base;
	real height;
	real width;
	real minimum_normal_k;
	short index;

	if (unit->object.type != _object_type_biped || !biped_riding_vehicle(vehicle_index, NONE, &vehicle))
		return FALSE;
	minimum_normal_k = biped_definition_get(unit->definition_index)->biped.runtime_minimum_normal_k;
	biped_get_physics_pill(unit_index, &base, &height, &width);
	for (index = 0; index < 84; index++)
	{
		real_point3d local;
		real_point3d point;
		real_point3d surface;
		real_point3d origin;
		boolean ray;
		boolean clear;

		ride_board_candidate(ride.scenario, ordinal, index, &local.x, &local.y);
		local.z = 0.f;
		vehicle_riding_point_to_world(&vehicle.position, &vehicle.forward, &vehicle.up, &local, &point);
		ray = ride_deck_surface(vehicle_index, &point, height + 2.f*width, minimum_normal_k, &surface);
		clear = ray && ride_board_clear(vehicle_index, &surface, height, width);
		if (!ray || !clear)
		{
			ride_log("board_refused", "tick=%ld unit=%ld vehicle=%ld ordinal=%d candidate=%d place=%.5f,%.5f,%.5f "
				"ray=%d clearance=%d origin=%.6f,%.6f,%.6f forward=%.6f,%.6f,%.6f up=%.6f,%.6f,%.6f",
				game_time_get(), unit_index, vehicle_index, (int)ordinal, (int)index, local.x, local.y, local.z,
				(int)ray, ray ? (int)clear : -1, vehicle.position.x, vehicle.position.y, vehicle.position.z,
				vehicle.forward.i, vehicle.forward.j, vehicle.forward.k, vehicle.up.i, vehicle.up.j, vehicle.up.k);
			continue;
		}
		if (unit->object.parent_object_index != NONE)
			unit_exit_seat_end(unit_index);
		/* (the origin as far above the pill's bottom as it is now) */
		origin = unit->object.position;
		origin.x = surface.x;
		origin.y = surface.y;
		origin.z = surface.z + .02f + (unit->object.position.z - (base.z - width));
		object_set_position(unit_index, &origin, NULL, NULL);
		/* (a setup teleport onto a moving convoy starts in its motion, not
		at rest against a hull that the original physics treats as a hit) */
		if (ride.scenario == _ride_convoy)
			unit->object.translational_velocity = object_get(vehicle_index)->object.translational_velocity;
		if (ride.scenario == _ride_driven)
		{
			/* (a teleport must replace the off-deck fall with the motion
			of this point, including the Warthog's remaining angular motion) */
			biped_get_physics_pill(unit_index, &base, &height, &width);
			vehicle_riding_deck_point_velocity(&vehicle, &base, game_players_are_double_speed(),
				&unit->object.translational_velocity);
		}
		return TRUE;
	}
	return FALSE;
}

/* only the test owns these observations; the normal delivery path is unchanged */
void network_test_ride_control_delivered(
	long player_index,
	boolean delivered)
{
	long absolute = DATUM_INDEX_TO_ABSOLUTE_INDEX(player_index);
	struct ride_player *rider;

	if (!ride.enabled || !ride.prepared)
		return;
	if (absolute < 0 || absolute >= RIDE_PLAYERS)
		return;
	rider = &ride.players[absolute];
	if (rider->unit == NONE || !rider->local)
		return;
	rider->control_tick = game_time_get();
	rider->control_delivered = delivered;
}

/* the campaign's out-of-scene scripts must not take the test's controls back */
static void ride_suspend_scripts(
	void)
{
	if (!ride.enabled || !ride.prepared || ride.scripts_suspended || !ride_host() ||
		(ride.scenario != _ride_coop && ride.scenario != _ride_coop_double))
	{
		return;
	}
	hs_runtime_sleep_scripts();
	ride.scripts_suspended = TRUE;
	ride_log("scripts_suspended", "tick=%ld", game_time_get());
}

/* scoring continues on its fixed clock, even if something else inhibits input */
static void ride_monitor_controls(
	long tick)
{
	short index;

	if (tick < ride.board_tick || (ride.scenario != _ride_coop && ride.scenario != _ride_coop_double))
		return;
	for (index = 0; index < RIDE_PLAYERS; index++)
	{
		struct ride_player *rider = &ride.players[index];

		if (rider->unit == NONE || !rider->local || rider->control_tick != tick)
			continue;
		if (!rider->control_delivered && !rider->control_inhibited)
		{
			ride_log("control_inhibited", "tick=%ld phase=%s player=%d", tick,
				ride_phases[ride_phase(ride.scenario, tick - ride.board_tick)], (int)index);
		}
		rider->control_inhibited = !rider->control_delivered;
	}
}

/* co-op: the level's opening over, every local player able to play */
static boolean ride_local_ready(
	void)
{
	short local;
	boolean found = FALSE;

	if (cinematic_in_progress() || !player_input_enabled())
		return FALSE;
	for (local = 0; local < MAXIMUM_LOCAL_PLAYERS; local++)
	{
		long index = local_player_get_player_index(local);
		struct player_datum *player = index != NONE ? player_try_and_get(index) : NULL;
		struct unit_datum *unit;

		if (index == NONE)
			continue;
		if (!player || player->unit_index == NONE)
			return FALSE;
		unit = unit_get(player->unit_index);
		if (TEST_FLAG(unit->object.damage_flags, _object_dead_bit) || unit->object.parent_object_index != NONE ||
			!TEST_FLAG(unit->unit.flags, _unit_controllable_bit) || TEST_FLAG(unit->unit.flags, _unit_suspended_bit))
		{
			return FALSE;
		}
		found = TRUE;
	}
	return found;
}

/* the host's view of every player, remote ones too: a client's player gets
its unit on the host only after the client joins, later than the host's own */
static boolean ride_players_ready(
	void)
{
	struct data_iterator iterator;
	struct player_datum *player;
	short count = 0;

	data_iterator_new(&iterator, player_data);
	while ((player = (struct player_datum *)data_iterator_next(&iterator)) != NULL)
	{
		struct unit_datum *unit;

		if (player->unit_index == NONE)
			return FALSE;
		unit = unit_get(player->unit_index);
		if (TEST_FLAG(unit->object.damage_flags, _object_dead_bit) || unit->object.parent_object_index != NONE ||
			!TEST_FLAG(unit->unit.flags, _unit_controllable_bit) || TEST_FLAG(unit->unit.flags, _unit_suspended_bit))
		{
			return FALSE;
		}
		count++;
	}
	/* (both co-op scenarios carry two riders) */
	return count >= 2;
}

/* the runner gives both processes this clock file, unique to their case.
only the host chooses its tick, after the cinematic; a client's own input
can be ready at tick zero, before it has received any presentation. */
static boolean ride_clock_tick(
	boolean host,
	boolean local_ready,
	long tick,
	FILE *clock,
	char const *session,
	long *board_tick)
{
	char received_session[64];
	long received_tick;
	int terminator;

	if (!local_ready || !clock)
		return FALSE;
	if (host)
	{
		received_tick = tick + RIDE_PREPARE_TICKS;
		if (fprintf(clock, "%s %ld\n", session, received_tick) < 0 || fflush(clock) != 0)
			return FALSE;
	}
	else
	{
		if (fscanf(clock, "%63s %ld", received_session, &received_tick) != 2 ||
			strcmp(received_session, session) || received_tick < tick ||
			received_tick > RIDE_READY_TIMEOUT + RIDE_PREPARE_TICKS)
		{
			return FALSE;
		}
		/* (the host may still be writing the tick's final digits) */
		terminator = fgetc(clock);
		if (terminator == '\r')
			terminator = fgetc(clock);
		if (terminator != '\n')
			return FALSE;
	}
	*board_tick = received_tick;
	return TRUE;
}

/* the old enclosure test (bipeds.c's biped_riding_enclosure) at a pill's base: a ray world up from its top, the
rider's jump high, against the vehicle alone, both faces, and whether the top is inside the vehicle */
static void ride_old_ray(
	long vehicle_index,
	struct biped_definition const *definition,
	real_point3d const *base,
	real height,
	boolean *hit,
	real *distance,
	boolean *inside)
{
	struct collision_model_instance instance;
	struct collision_model_test_vector_result result;
	real_point3d top = *base;
	real_vector3d upward;
	real jump_height = vehicle_riding_reach(definition->biped.jump_velocity, global_gravity, 0.f);

	*hit = *inside = FALSE;
	*distance = 0.f;
	top.z += height;
	upward.i = upward.j = 0.f;
	upward.k = jump_height;
	if (!collision_model_instance_new(&instance, vehicle_index))
		return;
	*inside = collision_model_test_point(&instance, &top);
	*hit = jump_height > 0.f && collision_model_test_vector(&instance,
		FLAG(_collision_test_front_facing_surfaces_bit) | FLAG(_collision_test_back_facing_surfaces_bit), &top, &upward,
		&result);
	if (*hit)
		*distance = result.bsp_result.t*jump_height;
}

/* the room over a pill's base up to the vehicle's next surface (world up, either face; 10 wu for none) */
static real ride_room_above(
	long vehicle_index,
	real_point3d const *base)
{
	struct collision_model_instance instance;
	struct collision_model_test_vector_result result;
	real_vector3d upward;

	upward.i = upward.j = 0.f;
	upward.k = 10.f;
	if (collision_model_instance_new(&instance, vehicle_index) && collision_model_test_vector(&instance,
		FLAG(_collision_test_front_facing_surfaces_bit) | FLAG(_collision_test_back_facing_surfaces_bit), base, &upward,
		&result))
	{
		return result.bsp_result.t*10.f;
	}
	return 10.f;
}

/* (debug.network_test_ride_spot empty) the Longsword's deck surveyed, no rider boarded: the vehicle's tag and the
rider's limits, then every place of a body-local grid over it where a pill stands with room (ride_deck_surface): its
height in the body's frame, the room over it and what the old enclosure ray finds there; then the end */
static void ride_survey(
	long tick)
{
	struct vehicle_riding_vehicle vehicle;
	struct data_iterator iterator;
	struct player_datum *player;
	struct biped_definition *definition = NULL;
	real_point3d base;
	real height = 0.f;
	real width = 0.f;
	real radius;
	real x;
	real y;

	if (!ride_find_vehicles() || !biped_riding_vehicle(ride.vehicles[0], NONE, &vehicle))
	{
		ride_error("survey-vehicle");
		return;
	}
	data_iterator_new(&iterator, player_data);
	while ((player = (struct player_datum *)data_iterator_next(&iterator)) != NULL)
	{
		struct object_datum *unit = player->unit_index != NONE ? object_get(player->unit_index) : NULL;

		if (unit && unit->object.type == _object_type_biped && unit->object.parent_object_index == NONE)
		{
			definition = biped_definition_get(unit->definition_index);
			biped_get_physics_pill(player->unit_index, &base, &height, &width);
			break;
		}
	}
	if (!definition)
	{
		ride_error("survey-rider");
		return;
	}
	radius = object_get(ride.vehicles[0])->object.bounding_sphere_radius;
	ride_log("survey_vehicle", "tick=%ld vehicle=%ld tag=%s radius=%.4f m=%.5f slope=%.4f height=%.4f width=%.4f "
		"jump=%.4f", tick, ride.vehicles[0], tag_get_name(object_get(ride.vehicles[0])->definition_index), radius,
		definition->biped.runtime_minimum_normal_k, definition->biped.maximum_slope_angle, height, width,
		vehicle_riding_reach(definition->biped.jump_velocity, global_gravity, 0.f));
	for (x = -radius; x <= radius; x += RIDE_SURVEY_STEP)
	{
		for (y = -radius; y <= radius; y += RIDE_SURVEY_STEP)
		{
			real_point3d local;
			real_point3d point;
			real_point3d surface;
			real_point3d place;
			boolean hit;
			boolean inside;
			real distance;

			local.x = x;
			local.y = y;
			local.z = 0.f;
			vehicle_riding_point_to_world(&vehicle.origin, &vehicle.body_forward, &vehicle.body_up, &local, &point);
			if (!ride_deck_surface(ride.vehicles[0], &point, height + 2.f*width, definition->biped.runtime_minimum_normal_k,
				&surface))
			{
				continue;
			}
			/* (the pill's base, its lower sphere's centre, a width over the deck) */
			surface.z += width;
			vehicle_riding_point_to_local(&vehicle.origin, &vehicle.body_forward, &vehicle.body_up, &surface, &place);
			ride_old_ray(ride.vehicles[0], definition, &surface, height, &hit, &distance, &inside);
			ride_log("survey", "tick=%ld x=%.3f y=%.3f z=%.4f room=%.4f old_hit=%d old_distance=%.4f old_inside=%d", tick,
				place.x, place.y, place.z, ride_room_above(ride.vehicles[0], &surface), hit ? 1 : 0, distance,
				inside ? 1 : 0);
		}
	}
	ride.ended = TRUE;
	ride_log("end", "tick=%ld players=%d", tick, (int)ride_rider_count());
}

/* every machine at the board tick: the vehicles found, the riders (every
player but the driver, in player order) put on the deck, the host every
one and a client its own */
static boolean ride_prepare(
	void)
{
	struct data_iterator iterator;
	struct player_datum *player;
	short ordinal = 0;

	if (!ride_find_vehicles())
		return FALSE;
	/* identify the pilot without seating them: the rider boards the unpowered ship first */
	if (ride.scenario == _ride_longsword && !ride_assign_driver(FALSE))
		return FALSE;
	data_iterator_new(&iterator, player_data);
	while ((player = (struct player_datum *)data_iterator_next(&iterator)) != NULL)
	{
		short absolute = (short)DATUM_INDEX_TO_ABSOLUTE_INDEX(iterator.datum_index);

		if (absolute >= RIDE_PLAYERS || player->unit_index == NONE)
			return FALSE;
		if (object_get(player->unit_index)->object.parent_object_index == ride.vehicles[0] ||
			(ride.scenario == _ride_longsword && player->unit_index == ride.driver))
			continue;
		ride.players[absolute].unit = player->unit_index;
		ride.players[absolute].local = player->local_player_index != NONE;
		ride.players[absolute].assigned = ride.vehicles[0];
		ride.players[absolute].ordinal = ordinal;
		if ((ride_host() || player->local_player_index != NONE) &&
			!ride_board(player->unit_index, ride.vehicles[0], ordinal))
		{
			return FALSE;
		}
		/* (with the host's riding switch off the test still runs, and the shield would only delay the host's word) */
		if (vehicle_riding_enabled() && !ride_host() && player->local_player_index != NONE)
			network_distributed_own_reboard(player->local_player_index, player->unit_index, ride.vehicles[0]);
		ordinal++;
	}
	if (ride.scenario != _ride_longsword)
		ride.driver = unit_get(ride.vehicles[0])->unit.driver_object_index;
	/* (every machine the same pair step: the riders' tags are the same) */
	if (ride.scenario == _ride_longsword)
	{
		short player;
		real_vector3d forward;

		/* align the grounded pilot with the parking lane through facing input, on both machines */
		ride.tilt_yaw = 0.f;

		for (player = 0; player < RIDE_PLAYERS; player++)
		{
			if (ride.players[player].unit != NONE)
			{
				ride.pair = ride_tilt_pair(biped_definition_get(
					object_get(ride.players[player].unit)->definition_index)->biped.runtime_minimum_normal_k);
				break;
			}
		}
	}
	if (ordinal != ride_rider_count())
		return FALSE;
	if ((ride.scenario == _ride_coop || ride.scenario == _ride_coop_double) &&
		((ride_host() && ride.driver == NONE) ||
		(ride.driver != NONE && unit_get(ride.driver)->unit.player_index != NONE)))
	{
		return FALSE;
	}
	if ((ride.scenario == _ride_driven || ride.scenario == _ride_longsword) && (ride.driver == NONE || unit_get(ride.driver)->unit.player_index == NONE))
		return FALSE;
	ride_log("prepare", "tick=%ld players=%d vehicles=%ld,%ld driver=%ld driver_player=%ld board_tick=%ld driver_missing=%d",
		game_time_get(), (int)ordinal, ride.vehicles[0], ride.vehicles[1], ride.driver,
		ride.driver == NONE ? NONE : unit_get(ride.driver)->unit.player_index, ride.board_tick,
		(int)((ride.scenario == _ride_coop || ride.scenario == _ride_coop_double) && ride.driver == NONE));
	return TRUE;
}

/* the height of a pill's bottom (its base, the lower sphere's centre, less
its radius) over the surface a ray from start_z down ray_k found at t:
FALSE for no hit (a deck lost is no deck at the feet) */
static boolean ride_height_from_hit(
	boolean hit,
	real t,
	real start_z,
	real ray_k,
	real base_z,
	real radius,
	real *height)
{
	if (!hit || !(t >= 0.f && t <= 1.f))
		return FALSE;
	*height = (base_z - radius) - (start_z + ray_k*t);
	return TRUE;
}

/* the vertical height above the selected capsule contact's surface plane */
static boolean ride_contact_height(
	boolean physical,
	long support,
	long vehicle,
	real_point3d const *contact,
	real_vector3d const *normal,
	real_point3d const *base,
	real radius,
	real *height)
{
	real surface_z;

	if (!physical || support != vehicle || vehicle == NONE || !(normal->k > 0.f))
		return FALSE;
	surface_z = contact->z - (normal->i*(base->x - contact->x) + normal->j*(base->y - contact->y))/normal->k;
	*height = base->z - radius - surface_z;
	return TRUE;
}

static struct ride_player *ride_contact_player(
	long unit_index)
{
	short index;

	if (!ride.enabled || ride.failed || ride.ended || !ride.prepared)
		return NULL;
	for (index = 0; index < RIDE_PLAYERS; index++)
		if (ride.players[index].unit == unit_index)
			return &ride.players[index];
	return NULL;
}

/* only the successful jump call opens an episode; support chatter cannot */
static void ride_outcome_launch(
	struct ride_player *rider,
	long tick)
{
	if (rider->launch_tick == tick)
		return;
	rider->launch_tick = tick;
	rider->episode++;
	rider->episode_open = TRUE;
	rider->episode_off = FALSE;
	rider->jumps++;
}

/* other machines do not consume this owner's input: observe its physical
departure, but never infer a launch from a received ticks change alone */
static boolean ride_outcome_observe(
	struct ride_player *rider,
	long tick,
	boolean supported,
	real upward)
{
	if (rider->local || rider->episode_open || !rider->physical_previous || supported || upward < RIDE_OBSERVED_LAUNCH)
		return FALSE;
	ride_outcome_launch(rider, tick);
	return TRUE;
}

/* terminal results belong to one launch; re-placement is never a landing */
static short ride_outcome_sample(
	struct ride_player *rider,
	long tick,
	boolean grounded,
	boolean supported,
	boolean recovery)
{
	if (rider->physical_previous && !supported && !rider->episode_open)
		rider->support_losses++;
	rider->physical_previous = supported;
	if (!rider->episode_open || tick <= rider->launch_tick)
		return 0;
	if (recovery)
	{
		rider->episode_recoveries++;
		rider->episode_open = FALSE;
		return 3;
	}
	if (!grounded)
		return 0;
	if (supported)
	{
		rider->landings++;
		rider->episode_open = FALSE;
		return 1;
	}
	if (rider->episode_off)
		return 0;
	rider->episode_off = TRUE;
	rider->landed_off++;
	if (ride.scenario != _ride_driven)
	{
		rider->landings++;
		rider->episode_open = FALSE;
	}
	return 2;
}

void network_test_ride_launch(
	long unit_index)
{
	struct ride_player *rider = ride_contact_player(unit_index);
	long tick = game_time_get();

	if (rider && rider->local && ride_phase(ride.scenario, tick - ride.board_tick) == _ride_phase_jump)
	{
		ride_outcome_launch(rider, tick);
		ride_log("launch", "tick=%ld player=%d unit=%ld episode=%ld physical=%d", tick,
			(int)(rider - ride.players), unit_index, rider->episode,
			rider->contact_valid && rider->contact_tick == tick - 1 ? 1 : 0);
	}
}

void network_test_ride_contact_begin(
	long unit_index)
{
	struct ride_player *rider = ride_contact_player(unit_index);

	if (rider)
	{
		rider->contact_tick = game_time_get();
		rider->contact_valid = FALSE;
		rider->contact_vehicle = NONE;
	}
}

void network_test_ride_contact(
	long unit_index,
	long vehicle,
	short node,
	long surface,
	real_point3d const *point,
	real_vector3d const *normal,
	real radius)
{
	struct ride_player *rider = ride_contact_player(unit_index);

	if (rider)
	{
		rider->contact_tick = game_time_get();
		rider->contact_vehicle = vehicle;
		rider->contact_node = node;
		rider->contact_surface = surface;
		rider->contact_normal = *normal;
		rider->contact_point.x = point->x - normal->i*radius;
		rider->contact_point.y = point->y - normal->j*radius;
		rider->contact_point.z = point->z - normal->k*radius;
		rider->contact_valid = TRUE;
	}
}

void network_test_ride_contact_end(
	long unit_index,
	long support,
	boolean grounded)
{
	struct ride_player *rider = ride_contact_player(unit_index);

	if (rider)
		rider->contact_valid = rider->contact_valid && rider->contact_tick == game_time_get() &&
			grounded && rider->contact_vehicle == support;
}

/* a rider's height over a vehicle's deck: a ray straight down from its
pill's top (under a gun over the bed), against that vehicle alone, to below
it */
static boolean ride_height(
	long vehicle_index,
	real_point3d const *base,
	real pill_height,
	real radius,
	real *height)
{
	struct object_datum *object = vehicle_index != NONE ?
		(struct object_datum *)object_try_and_get_and_verify_type(vehicle_index, _object_mask_vehicle) : NULL;
	struct collision_model_instance instance;
	struct collision_model_test_vector_result hit;
	real_point3d start;
	real_vector3d down;
	boolean found;

	if (!object || !collision_model_instance_new(&instance, vehicle_index))
		return FALSE;
	start = *base;
	start.z += pill_height + radius + .01f;
	down.i = down.j = 0.f;
	down.k = object->object.bounding_sphere_center.z - object->object.bounding_sphere_radius - 1.f - start.z;
	if (!(down.k < 0.f))
		return FALSE;
	found = collision_model_test_vector(&instance, FLAG(_collision_test_front_facing_surfaces_bit), &start, &down, &hit);
	return ride_height_from_hit(found, found ? hit.bsp_result.t : 0.f, start.z, down.k, base->z, radius, height);
}

/* a rider as this machine has it: the deck part it would send, its pill,
the frame its place is in, and (a sample) its height and health; a sample
counts its jumps, landings and crossings */
static void ride_rider_log(
	char const *kind,
	short player_index,
	long unit_index,
	long tick,
	boolean local)
{
	struct ride_player *rider = &ride.players[player_index];
	struct biped_datum *biped = (struct biped_datum *)object_try_and_get_and_verify_type(unit_index, _object_mask_biped);
	struct vehicle_riding_deck deck;
	struct vehicle_riding_vehicle frame;
	real_point3d base;
	real_point3d place;
	real_vector3d left;
	real pill_height;
	real radius;
	real height = 0.f;
	char height_text[32];
	boolean present;
	boolean height_valid;
	boolean riding;
	long vehicle_index;
	short phase = ride_phase(ride.scenario, tick - ride.board_tick);

	if (!biped)
		return;
	biped_get_physics_pill(unit_index, &base, &pill_height, &radius);
	present = biped_riding_deck(unit_index, &deck);
	if (!present)
	{
		csmemset(&deck, 0, sizeof(deck));
		deck.vehicle_index = NONE;
		deck.node_index = NONE;
	}
	vehicle_index = present ? deck.vehicle_index : rider->assigned;
	if (!biped_riding_vehicle(vehicle_index, present ? deck.node_index : NONE, &frame))
	{
		csmemset(&frame, 0, sizeof(frame));
		frame.forward.i = 1.f;
		frame.up.k = 1.f;
	}
	if (present)
		place = deck.place;
	else
		vehicle_riding_point_to_local(&frame.position, &frame.forward, &frame.up, &base, &place);
	cross_product3d(&frame.up, &frame.forward, &left);
	if (!strcmp(kind, "own"))
	{
		ride_log("own", "tick=%ld player=%d unit=%ld phase=%s deck_present=%d vehicle=%ld node=%d ticks=%d flags=%u "
			"velocity=%.4f,%.4f,%.4f peak=%.4f world=%.4f,%.4f,%.4f base=%.4f,%.4f,%.4f place=%.4f,%.4f,%.4f "
			"origin=%.4f,%.4f,%.4f forward=%.4f,%.4f,%.4f left=%.4f,%.4f,%.4f up=%.4f,%.4f,%.4f board_tick=%ld",
			tick, (int)player_index, unit_index, ride_phases[phase], present ? 1 : 0, deck.vehicle_index,
			(int)deck.node_index, (int)deck.ticks, (unsigned)deck.flags, deck.velocity.i, deck.velocity.j,
			deck.velocity.k, deck.peak, biped->object.position.x, biped->object.position.y, biped->object.position.z,
			base.x, base.y, base.z, place.x, place.y, place.z, frame.position.x, frame.position.y, frame.position.z,
			frame.forward.i, frame.forward.j, frame.forward.k, left.i, left.j, left.k, frame.up.i, frame.up.j,
			frame.up.k, rider->board_tick);
		return;
	}
	height_valid = ride_contact_height(rider->contact_valid && rider->contact_tick == tick &&
		!TEST_FLAG(biped->biped.flags, _biped_airborne_bit), rider->contact_vehicle, vehicle_index,
		&rider->contact_point, &rider->contact_normal, &base, radius, &height);
	ride_log("contact", "tick=%ld player=%d vehicle=%ld node=%d surface=%ld physical=%d point=%.6f,%.6f,%.6f "
		"normal=%.6f,%.6f,%.6f", tick, (int)player_index, rider->contact_vehicle, (int)rider->contact_node,
		rider->contact_surface, height_valid ? 1 : 0, rider->contact_point.x, rider->contact_point.y,
		rider->contact_point.z, rider->contact_normal.i, rider->contact_normal.j, rider->contact_normal.k);
	if (height_valid)
		snprintf(height_text, sizeof(height_text), "%.4f", height);
	else
		snprintf(height_text, sizeof(height_text), "none");
	/* (a physical vehicle switch starts settling again in every phase;
	only a switch during the cross phase counts as a crossing) */
	riding = present && deck.ticks == RIDING_TICKS;
	/* (incoming state may say supported before physics loses it this tick;
	acquisition counts the same completed samples the judge reads) */
	if (ride.scenario == _ride_driven && rider->recovering && rider->recovery_placed)
	{
		rider->recovery_contacts = riding && deck.vehicle_index == rider->assigned ? rider->recovery_contacts + 1 : 0;
		if (rider->recovery_contacts >= RIDE_RECOVERY_CONTACT)
		{
			rider->recovering = FALSE;
			rider->recovery_ready = tick + RIDE_RECOVERY_SETTLE;
			rider->recovery_land = 0;
			ride_log("recovery", "tick=%ld player=%d unit=%ld reason=acquired start_tick=%ld recoveries=%d", tick,
				(int)player_index, unit_index, rider->recovery_start, (int)rider->recoveries);
		}
	}
	{
		real_vector3d deck_velocity;

		vehicle_riding_deck_point_velocity(&frame, &base, game_players_are_double_speed(), &deck_velocity);
		if (phase == _ride_phase_jump && ride_outcome_observe(rider, tick, height_valid,
			biped->object.translational_velocity.k - deck_velocity.k))
			ride_log("launch", "tick=%ld player=%d unit=%ld episode=%ld physical=1 observed=1 upward=%.6f", tick,
				(int)player_index, unit_index, rider->episode, biped->object.translational_velocity.k - deck_velocity.k);
	}
	{
		short terminal = ride_outcome_sample(rider, tick, !TEST_FLAG(biped->biped.flags, _biped_airborne_bit) &&
			!(ride.scenario == _ride_driven && rider->recovering && rider->recovery_placed),
			height_valid && !(ride.scenario == _ride_driven && rider->recovery_placed),
			ride.scenario == _ride_driven && rider->recovery_placed && !rider->recovering);

		if (terminal)
			ride_log("episode_end", "tick=%ld player=%d unit=%ld episode=%ld terminal=%d", tick,
				(int)player_index, unit_index, rider->episode, (int)terminal);
	}
	if (height_valid && rider->previous_ticks < RIDING_TICKS)
	{
		if (rider->board_tick == NONE)
			rider->board_tick = tick;
	}
	/* (support after a recovery placement is acquisition, not a jump's landing) */
	if (ride.scenario == _ride_driven && !rider->recovering)
		rider->recovery_placed = FALSE;
	if (riding && rider->previous_vehicle != NONE && rider->previous_vehicle != deck.vehicle_index)
	{
		if (phase == _ride_phase_cross)
			rider->crossings++;
		rider->board_tick = tick;
	}
	if (riding)
		rider->previous_vehicle = deck.vehicle_index;
	if (riding)
		rider->reboarded = FALSE;
	if (phase >= _ride_phase_step_off && !riding)
		rider->stepped_off = TRUE;
	rider->previous_ticks = present ? deck.ticks : 0;
	if (phase != rider->phase)
	{
		rider->phase = phase;
		ride_log("phase", "tick=%ld player=%d phase=%s", tick, (int)player_index, ride_phases[phase]);
	}
	ride_log("sample", "simulation_tick=%ld network_tick=%ld player=%d unit=%ld local=%d phase=%s deck_present=%d "
		"vehicle=%ld node=%d ticks=%d flags=%u velocity=%.4f,%.4f,%.4f peak=%.4f world=%.4f,%.4f,%.4f "
		"base=%.4f,%.4f,%.4f place=%.4f,%.4f,%.4f height_valid=%d height=%s body=%.4f shield=%.4f board_tick=%ld "
		"last_applied_host_tick=%ld origin=%.4f,%.4f,%.4f forward=%.4f,%.4f,%.4f left=%.4f,%.4f,%.4f up=%.4f,%.4f,%.4f",
		tick, tick + 1, (int)player_index, unit_index, local ? 1 : 0, ride_phases[phase], present ? 1 : 0,
		deck.vehicle_index, (int)deck.node_index, (int)deck.ticks, (unsigned)deck.flags, deck.velocity.i,
		deck.velocity.j, deck.velocity.k, deck.peak, biped->object.position.x, biped->object.position.y,
		biped->object.position.z, base.x, base.y, base.z, place.x, place.y, place.z, height_valid ? 1 : 0, height_text,
		biped->object.body_vitality, biped->object.shield_vitality, rider->board_tick, rider->applied_host_tick,
		frame.position.x, frame.position.y, frame.position.z, frame.forward.i, frame.forward.j, frame.forward.k,
		left.i, left.j, left.k, frame.up.i, frame.up.j, frame.up.k);
}

/* measured orientation and native pilot controls, not requested Euler angles; counted updates distinguish physics from pose writes */
static void ride_tilt_log(
	long tick)
{
	struct object_datum *object = object_get(ride.vehicles[0]);
	struct unit_datum *unit = unit_get(ride.vehicles[0]);
	real_vector3d left;
	real up_k = object->object.up.k/magnitude3d(&object->object.up);
	long hold;
	short step = ride_pilot_at(tick - ride.board_tick, &hold);

	cross_product3d(&object->object.up, &object->object.forward, &left);
	if (tick == ride.board_tick + RIDE_PILOT_CLIMB - 1)
	{
		struct collision_result hit;
		real_point3d start;
		real_vector3d ray;
		short level;
		boolean blocked;
		real heading;

		for (level = 0; level <= 21; level++)
		{
			start = object->object.position;
			start.z += level <= 8 ? 5.f*level : 0.f;
			if (level >= 13)
				start.z += 5.f*(level - 13);
			heading = ride.tilt_yaw + _pi/2.f;
			if (level >= 10 && level <= 12)
				heading = ride.tilt_yaw + (level - 10)*_pi/2.f;
			if (level >= 13)
				heading = ride.tilt_yaw - _pi/2.f;
			ray.i = level == 9 ? 0.f : cosine(heading)*200.f;
			ray.j = level == 9 ? 0.f : sine(heading)*200.f;
			ray.k = level == 9 ? 200.f : 0.f;
			blocked = collision_test_vector(_collision_test_for_vehicles_flags,
				&start, &ray, ride.vehicles[0], &hit);
			ride_log("route", "tick=%ld height=%.3f world=%.4f,%.4f,%.4f old_hit=%d old_distance=%.3f ground_k=%.4f heading=%.3f",
				tick, start.z - object->object.position.z, start.x, start.y, start.z, (int)blocked,
				blocked ? hit.t*200.f : 200.f, blocked ? hit.plane.n.k : 0.f, level == 9 ? 90.f : heading*180.f/_pi);
		}
	}
	ride_log("tilt", "tick=%ld step=%d hold=%ld door=%d descending=%d pitch=%.3f roll=%.3f tilt=%.3f "
		"throttle=%.4f,%.4f facing=%.4f,%.4f controls=%u grounded=%d speed=%.4f updates=%ld "
		"heading=%.3f parked_heading=%.3f flight_heading=%.3f", tick, (int)step,
		hold, step == 12 ? 1 : 0, step == 11 ? 1 : 0,
		(real)atan2(object->object.forward.k, sqrt(object->object.forward.i*object->object.forward.i +
			object->object.forward.j*object->object.forward.j))*180.f/_pi,
		(real)atan2(-left.k, object->object.up.k)*180.f/_pi,
		(real)acos(up_k > 1.f ? 1.f : up_k < -1.f ? -1.f : up_k)*180.f/_pi,
		unit->unit.throttle.i, unit->unit.throttle.j,
		(real)atan2(unit->unit.desired_facing_vector.j, unit->unit.desired_facing_vector.i),
		(real)atan2(unit->unit.desired_facing_vector.k, sqrt(unit->unit.desired_facing_vector.i*unit->unit.desired_facing_vector.i +
			unit->unit.desired_facing_vector.j*unit->unit.desired_facing_vector.j)), (unsigned int)unit->unit.control_flags,
		(int)vehicle_datum_get(ride.vehicles[0])->vehicle.on_ground_ticks,
		magnitude3d(&object->object.translational_velocity),
		object_update_count(ride.vehicles[0]),
		(real)atan2(object->object.forward.j, object->object.forward.i)*180.f/_pi,
		ride.tilt_yaw*180.f/_pi, (ride.tilt_yaw + _pi/2.f)*180.f/_pi);
}

/* a Longsword rider each tick: the vehicle's pitch, roll and total tilt (degrees) and its up's k, the rider's surface
limits and where protection starts, its riding ticks, airborne and ground, the old enclosure ray at its pill, its
base in the vehicle body's frame (the runner's independent cabin box) and its health */
static void ride_cabin_log(
	short player_index,
	long unit_index,
	long tick)
{
	struct biped_datum *biped = (struct biped_datum *)object_try_and_get_and_verify_type(unit_index, _object_mask_biped);
	struct biped_definition *definition;
	struct vehicle_riding_vehicle vehicle;
	struct vehicle_riding_deck deck;
	real_point3d base;
	real_point3d place;
	real_vector3d left;
	real height;
	real width;
	real up_k;
	real limit;
	real distance;
	boolean hit;
	boolean inside;
	struct vehicle_riding_cabin const *cabin = vehicle_riding_player_cabin(player_index);
	struct vehicle_riding_cabin none;
	boolean present;

	if (!biped || !biped_riding_vehicle(ride.vehicles[0], NONE, &vehicle))
		return;
	definition = biped_definition_get(biped->definition_index);
	biped_get_physics_pill(unit_index, &base, &height, &width);
	cross_product3d(&vehicle.body_up, &vehicle.body_forward, &left);
	up_k = vehicle.body_up.k/magnitude3d(&vehicle.body_up);
	limit = definition->biped.runtime_minimum_normal_k > .70710678f ? definition->biped.runtime_minimum_normal_k : .70710678f;
	vehicle_riding_point_to_local(&vehicle.origin, &vehicle.body_forward, &vehicle.body_up, &base, &place);
	ride_old_ray(ride.vehicles[0], definition, &base, height, &hit, &distance, &inside);
	if (!cabin)
	{
		csmemset(&none, 0, sizeof(none));
		cabin = &none;
	}
	present = biped_riding_deck(unit_index, &deck);
	ride_log("cabin", "tick=%ld player=%d unit=%ld pitch=%.3f roll=%.3f tilt=%.3f uk=%.5f m=%.5f limit=%.5f slope=%.4f "
		"ticks=%d airborne=%d ground_k=%.4f old_hit=%d old_distance=%.4f old_inside=%d place=%.4f,%.4f,%.4f body=%.4f "
		"shield=%.4f corrections=%d"
		" axis=%d raw=%d inside=%d ceiling=%d ceiling_distance=%.4f floor=%d floor_distance=%.4f standing=%d misses=%d"
		" jump=%d take_off=%d exception=%d carry=%d settle=%d rejected=%d anchor=%d verdict=%d refusal=%d",
		tick, (int)player_index, unit_index,
		(real)atan2(vehicle.body_forward.k, sqrt(vehicle.body_forward.i*vehicle.body_forward.i +
			vehicle.body_forward.j*vehicle.body_forward.j))*180.f/_pi,
		(real)atan2(-left.k, vehicle.body_up.k)*180.f/_pi,
		(real)acos(up_k > 1.f ? 1.f : up_k < -1.f ? -1.f : up_k)*180.f/_pi, up_k,
		definition->biped.runtime_minimum_normal_k, limit, definition->biped.maximum_slope_angle,
		present ? (int)deck.ticks : 0, TEST_FLAG(biped->biped.flags, _biped_airborne_bit) ? 1 : 0,
		biped->biped.ground_plane.n.k, hit ? 1 : 0, distance, inside ? 1 : 0, place.x, place.y, place.z,
		biped->object.body_vitality, biped->object.shield_vitality, (int)ride.players[player_index].tick_corrections,
		cabin->body_axis ? 1 : 0, cabin->rays.hit ? 1 : 0, cabin->rays.inside ? 1 : 0, cabin->rays.ceiling ? 1 : 0,
		cabin->rays.ceiling_distance, cabin->rays.floor ? 1 : 0, cabin->rays.floor_distance, (int)cabin->standing,
		(int)cabin->misses, (int)cabin->jump, cabin->take_off ? 1 : 0, cabin->exception ? 1 : 0,
		cabin->contact_carry ? 1 : 0, (int)cabin->settle, (int)cabin->rejected, cabin->have_anchor ? 1 : 0,
		present ? (int)((deck.flags >> VEHICLE_RIDING_DECK_VERDICT_SHIFT) & VEHICLE_RIDING_DECK_VERDICT_MASK) : 0, (int)cabin->refusal);
	ride.players[player_index].tick_corrections = 0;
}

/* engine-owned attribution, when available; proximity is context, not a
damage source. A deleted unit has only the last observed position and reason. */
static void ride_life_log(
	short player_index,
	long current_unit,
	long tick)
{
	struct ride_player *rider = &ride.players[player_index];
	struct unit_datum *unit = unit_try_and_get(rider->unit);
	struct object_iterator iterator;
	long nearest = NONE;
	real distance = -1.f;
	real speed = 0.f;
	long attacker_object = NONE;
	long attacker_player = NONE;
	long attacker_tick = NONE;
	short index;
	boolean changed = rider->unit != current_unit;
	boolean dead = unit && TEST_FLAG(unit->object.damage_flags, _object_dead_bit);

	if (unit)
		rider->life_position = unit->object.position;
	if (!changed && (!dead || rider->life_logged))
		return;
	if (unit)
	{
		for (index = 0; index < MAXIMUM_ATTACKERS_PER_UNIT; index++)
		{
			struct unit_attacker const *attacker = &unit->unit.attackers[index];

			if (attacker->object_index != NONE && attacker->damage_inflicted > 0.f &&
				(attacker_tick == NONE || attacker->game_time_stamp >= (unsigned long)attacker_tick))
			{
				attacker_object = attacker->object_index;
				attacker_player = attacker->player_index;
				attacker_tick = attacker->game_time_stamp;
			}
		}
	}
	object_iterator_new(&iterator, _object_mask_vehicle, 0);
	while (object_iterator_next(&iterator))
	{
		struct object_datum *vehicle = object_get(iterator.index);
		real candidate = distance3d(&rider->life_position, &vehicle->object.position);

		if (nearest == NONE || candidate < distance)
		{
			nearest = iterator.index;
			distance = candidate;
			speed = magnitude3d(&vehicle->object.translational_velocity);
		}
	}
	ride_log("life", "tick=%ld player=%d unit=%ld current_unit=%ld reason=%s recorded=%d damage_category=%d damage_flags=%u "
		"death_tick=%ld attacker_object=%ld attacker_player=%ld attacker_tick=%ld nearest_vehicle=%ld vehicle_speed=%.4f vehicle_distance=%.4f "
		"world=%.4f,%.4f,%.4f", tick, (int)player_index, rider->unit, current_unit, changed ? "unit-changed" : "dead", unit ? 1 : 0,
		unit ? (int)unit->unit.last_damage_category : NONE, unit ? (unsigned)unit->object.damage_flags : 0,
		unit ? unit->unit.time_of_death : NONE, attacker_object, attacker_player, attacker_tick, nearest, speed, distance,
		rider->life_position.x, rider->life_position.y, rider->life_position.z);
	rider->life_logged = TRUE;
}

/* every rider and test vehicle, after the objects update */
static void ride_samples(
	long tick)
{
	struct data_iterator iterator;
	struct player_datum *player;
	short index;

	data_iterator_new(&iterator, player_data);
	while ((player = (struct player_datum *)data_iterator_next(&iterator)) != NULL)
	{
		short absolute = (short)DATUM_INDEX_TO_ABSOLUTE_INDEX(iterator.datum_index);

		if (absolute >= RIDE_PLAYERS || ride.players[absolute].unit == NONE)
			continue;
		ride_life_log(absolute, player->unit_index, tick);
		if (ride.players[absolute].unit != player->unit_index)
		{
			ride_error("rider-life-changed");
			return;
		}
		ride_rider_log("sample", absolute, player->unit_index, tick, player->local_player_index != NONE);
		if (ride.scenario == _ride_longsword)
			ride_cabin_log(absolute, player->unit_index, tick);
	}
	for (index = 0; index < ride_vehicle_count(); index++)
	{
		struct unit_datum *unit =
			(struct unit_datum *)object_try_and_get_and_verify_type(ride.vehicles[index], _object_mask_vehicle);
		real moved;
		long driver;

		if (!unit)
		{
			ride_error("vehicle-deleted");
			return;
		}
		moved = ride.vehicle_seen[index] ? distance3d(&ride.vehicle_last[index], &unit->object.position) : 0.f;
		ride.vehicle_last[index] = unit->object.position;
		ride.vehicle_seen[index] = TRUE;
		driver = unit->unit.driver_object_index;
		ride_log("vehicle", "tick=%ld vehicle=%ld world=%.4f,%.4f,%.4f velocity=%.4f,%.4f,%.4f angular=%.4f,%.4f,%.4f "
			"moved=%.5f written=%.5f writes=%d carries=%d driver=%ld driver_player=%ld", tick, ride.vehicles[index],
			unit->object.position.x, unit->object.position.y, unit->object.position.z,
			unit->object.translational_velocity.i, unit->object.translational_velocity.j,
			unit->object.translational_velocity.k, unit->object.angular_velocity.i, unit->object.angular_velocity.j,
			unit->object.angular_velocity.k, moved, ride.vehicle_written[index], (int)ride.vehicle_writes[index],
			(int)ride.vehicle_carries[index], driver,
			driver == NONE ? NONE : unit_get(driver)->unit.player_index);
		ride.vehicle_written[index] = 0.f;
		ride.vehicle_writes[index] = 0;
		ride.vehicle_carries[index] = 0;
	}
	if (ride.scenario == _ride_longsword)
		ride_tilt_log(tick);
}

/* whether a rider is to be put back on the deck now: in the jumps, past the
air time of this period's jump, on the ground off the deck, not yet put back
in this period */
static boolean ride_reboard_due(
	long elapsed,
	boolean airborne,
	boolean deck,
	long done_cycle)
{
	long cycle;

	if (ride_phase(_ride_straight, elapsed) != _ride_phase_jump || airborne || deck)
		return FALSE;
	if ((elapsed - RIDE_JUMP) % RIDE_JUMP_PERIOD < RIDE_REBOARD_AFTER)
		return FALSE;
	cycle = (elapsed - RIDE_JUMP)/RIDE_JUMP_PERIOD;
	return cycle != done_cycle;
}

/* a rider that landed off the deck put back on its spot before its next jump,
the landing and the delay logged; it counts as the landing. In the convoy the
host also reboards remote riders, so a client-only teleport cannot drift past
the host's prediction bounds and be corrected back onto the ground. */
static void ride_reboard(
	long tick)
{
	long elapsed = tick - ride.board_tick;
	struct data_iterator iterator;
	struct player_datum *player;

	if (ride.scenario == _ride_driven || ride.scenario == _ride_longsword || ride_phase(ride.scenario, elapsed) != _ride_phase_jump)
		return;
	data_iterator_new(&iterator, player_data);
	while ((player = (struct player_datum *)data_iterator_next(&iterator)) != NULL)
	{
		short absolute = (short)DATUM_INDEX_TO_ABSOLUTE_INDEX(iterator.datum_index);
		struct ride_player *rider;
		struct biped_datum *biped;
		struct vehicle_riding_deck deck;
		boolean airborne;
		boolean present;

		if (absolute >= RIDE_PLAYERS || (player->local_player_index == NONE && !(ride.scenario == _ride_convoy && ride_host())) ||
			ride.players[absolute].unit != player->unit_index)
			continue;
		rider = &ride.players[absolute];
		biped = (struct biped_datum *)object_try_and_get_and_verify_type(player->unit_index, _object_mask_biped);
		if (!biped)
			continue;
		airborne = TEST_FLAG(biped->biped.flags, _biped_airborne_bit) ? TRUE : FALSE;
		present = biped_riding_deck(player->unit_index, &deck);
		if (present)
			rider->off_tick = NONE;
		else if (!airborne && rider->off_tick == NONE)
			rider->off_tick = tick;
		if (!ride_reboard_due(elapsed, airborne, present, rider->reboard_cycle))
			continue;
		if (!ride_board(player->unit_index, ride.vehicles[0], rider->ordinal))
		{
			ride_error("reboard");
			return;
		}
		if (player->local_player_index != NONE)
			network_distributed_own_reboard(player->local_player_index, player->unit_index, ride.vehicles[0]);
		rider->reboard_cycle = (elapsed - RIDE_JUMP)/RIDE_JUMP_PERIOD;
		rider->reboarded = TRUE;
		ride_log("reboard", "tick=%ld player=%d unit=%ld landing_tick=%ld ticks=%ld", tick, (int)absolute,
			player->unit_index, rider->off_tick, tick - rider->off_tick);
		rider->off_tick = NONE;
	}
}

/* a missed driven jump is natural in a hard turn; settle the pilot before
putting the rider back, then wait for physical support on this vehicle */
static void ride_driven_recover(
	long tick)
{
	struct data_iterator iterator;
	struct player_datum *player;
	long elapsed = tick - ride.board_tick;

	if (ride.scenario != _ride_driven)
		return;
	data_iterator_new(&iterator, player_data);
	while ((player = (struct player_datum *)data_iterator_next(&iterator)) != NULL)
	{
		short absolute = (short)DATUM_INDEX_TO_ABSOLUTE_INDEX(iterator.datum_index);
		struct ride_player *rider;
		struct biped_datum *biped;
		struct vehicle_riding_deck deck;
		boolean supported;
		boolean grounded;

		if (absolute >= RIDE_PLAYERS || ride.players[absolute].unit != player->unit_index || player->unit_index == NONE)
			continue;
		rider = &ride.players[absolute];
		biped = (struct biped_datum *)object_try_and_get_and_verify_type(player->unit_index, _object_mask_biped);
		if (!biped)
			continue;
		supported = biped_riding_deck(player->unit_index, &deck) && deck.ticks == RIDING_TICKS &&
			deck.vehicle_index == rider->assigned;
		grounded = !TEST_FLAG(biped->biped.flags, _biped_airborne_bit);
		if (!rider->recovering)
		{
			if (elapsed < RIDE_JUMP || elapsed >= ride_phase_tick(ride.scenario, RIDE_STEP_OFF) || rider->jumps == 0 || supported || !grounded || tick < rider->recovery_ready)
			{
				rider->recovery_land = 0;
				continue;
			}
			if (!rider->recovery_land)
				rider->recovery_land = tick;
			if (tick - rider->recovery_land < 2)
				continue;
			if (++rider->recoveries > RIDE_RECOVERY_MAX)
			{
				ride_error("recovery-limit");
				return;
			}
			rider->recovering = TRUE;
			rider->recovery_placed = FALSE;
			rider->recovery_contacts = 0;
			rider->recovery_steady = 0;
			rider->recovery_start = tick;
			ride_log("recovery", "tick=%ld player=%d unit=%ld reason=start start_tick=%ld recoveries=%d", tick,
				(int)absolute, player->unit_index, tick, (int)rider->recoveries);
		}
		if (tick - rider->recovery_start >= RIDE_RECOVERY_TIMEOUT)
		{
			ride_log("recovery", "tick=%ld player=%d unit=%ld reason=failed start_tick=%ld recoveries=%d", tick,
				(int)absolute, player->unit_index, rider->recovery_start, (int)rider->recoveries);
			ride_error("recovery-timeout");
			return;
		}
		if (!rider->recovery_placed)
		{
			struct vehicle_riding_vehicle frame;

			/* (the pilot brakes; wait for six measured stopped ticks,
			including translation, within the same bounded recovery) */
			if (biped_riding_vehicle(rider->assigned, NONE, &frame) &&
				dot_product3d(&frame.translational_velocity, &frame.translational_velocity) < RIDE_RECOVERY_LINEAR*RIDE_RECOVERY_LINEAR &&
				dot_product3d(&frame.angular_velocity, &frame.angular_velocity) < RIDE_RECOVERY_ANGULAR*RIDE_RECOVERY_ANGULAR)
				rider->recovery_steady++;
			else
				rider->recovery_steady = 0;
		}
		if (!rider->recovery_placed && tick - rider->recovery_start >= RIDE_RECOVERY_WAIT &&
			rider->recovery_steady >= RIDE_RECOVERY_CONTACT)
		{
			/* (as the convoy's reset: the host moves its remote copy too;
			a client changes only its owner, observing the remote take) */
			if ((ride_host() || player->local_player_index != NONE) && !ride_board(player->unit_index, rider->assigned, rider->ordinal))
			{
				ride_error("recovery-placement");
				return;
			}
			rider->recovery_placed = TRUE;
			ride_log("recovery", "tick=%ld player=%d unit=%ld reason=place start_tick=%ld recoveries=%d", tick,
				(int)absolute, player->unit_index, rider->recovery_start, (int)rider->recoveries);
		}
	}
}

static boolean ride_driven_holding(
	long tick)
{
	short index;
	for (index = 0; index < RIDE_PLAYERS; index++)
		if (ride.players[index].unit != NONE && (ride.players[index].recovering || tick < ride.players[index].recovery_ready))
			return TRUE;
	return FALSE;
}

/* (the host) the Longsword's velocities this tick: turned toward its setpoint (ride_tilt_frame at its heading) by the
whole rotation between them, at most twice RIDE_TILT_CAP, and held on its lifted spot; its own update makes the move,
counted, as a pose write would not be (vehicle_riding_snapshot_step refuses a move without an update) */
static void ride_tilt_motion(
	long vehicle_index,
	long elapsed,
	real_vector3d *velocity,
	real_vector3d *angular)
{
	static real_point3d const origin = { 0.f, 0.f, 0.f };
	struct object_datum *object = object_get(vehicle_index);
	real_matrix4x3 now;
	real_matrix4x3 target;
	real_vector3d forward;
	real_vector3d up;
	real_vector3d drift;
	real pitch;
	real roll;
	real length;
	long hold;

	short step = ride_tilt_at(elapsed, ride.pair, &pitch, &roll, &hold);

	ride_tilt_frame(ride.tilt_yaw, pitch, roll, &forward, &up);
	matrix4x3_from_point_and_vectors(&now, &origin, &object->object.forward, &object->object.up);
	matrix4x3_from_point_and_vectors(&target, &origin, &forward, &up);
	vector_from_matrices4x3(&now, &target, angular);
	length = magnitude3d(angular);
	if (length > 2.f*RIDE_TILT_CAP)
		scale_vector3d(angular, 2.f*RIDE_TILT_CAP/length, angular);
	/* (the last step brings it down to where it was found, at the hold's speed cap, for the door walk) */
	vector_from_points3d(&object->object.position, step == RIDE_TILT_STEPS - 1 ? &ride.tilt_ground : &ride.tilt_position,
		&drift);
	scale_vector3d(&drift, RIDE_TILT_HOLD_GAIN, velocity);
	length = magnitude3d(velocity);
	if (length > RIDE_TILT_HOLD_CAP)
		scale_vector3d(velocity, RIDE_TILT_HOLD_CAP/length, velocity);
}

/* preparation must have physically settled before motion or the first jump;
the fixed scored schedule never absorbs a missing acquisition */
static boolean ride_setup_supported(
	long tick)
{
	struct data_iterator iterator;
	struct player_datum *player;

	data_iterator_new(&iterator, player_data);
	while ((player = (struct player_datum *)data_iterator_next(&iterator)) != NULL)
	{
		short absolute = (short)DATUM_INDEX_TO_ABSOLUTE_INDEX(iterator.datum_index);
		struct ride_player *rider;

		if (absolute >= RIDE_PLAYERS || (!ride_host() && player->local_player_index == NONE))
			continue;
		rider = &ride.players[absolute];
		if (rider->unit == NONE || rider->unit != player->unit_index)
			continue;
		if (!rider->contact_valid || rider->contact_tick != tick - 1 || rider->contact_vehicle != rider->assigned)
			return FALSE;
	}
	return TRUE;
}

/* the velocity and the angular velocity the host gives a ride vehicle for
a step that is elapsed ticks from the board tick: the scenario's motion, and
the convoy's holds of its lane, spacing and heading from the vehicle's pose */
static void ride_drive_motion(
	short index,
	long elapsed,
	struct object_datum const *object,
	real_vector3d *velocity,
	real_vector3d *angular)
{
	real_vector3d forward;
	real own_yaw;
	real sideways;
	real along;

	own_yaw = ride_heading(ride.vehicles[index], &forward);
	if (ride.scenario == _ride_convoy)
	{
		forward.i = (real)cos(ride.convoy_yaw);
		forward.j = (real)sin(ride.convoy_yaw);
		forward.k = 0.f;
	}
	if (ride.scenario == _ride_longsword)
		ride_tilt_motion(ride.vehicles[index], elapsed, velocity, angular);
	else
		ride_motion(ride.scenario, elapsed, &forward, velocity, angular);
	if (ride.scenario == _ride_convoy)
	{
		struct object_datum *lead = object_get(ride.vehicles[0]);
		real_vector3d left;

		/* (hold both vehicles in the saved lateral corridor; also correct
		their fore/aft spacing without following a wheel's slope heading) */
		left.i = -forward.j;
		left.j = forward.i;
		left.k = 0.f;
		sideways = ride_convoy_lateral((index ? ride.convoy_offset : 0.f) -
			((object->object.position.x - ride.convoy_origin.x)*left.i +
			(object->object.position.y - ride.convoy_origin.y)*left.j));
		velocity->i += left.i*sideways;
		velocity->j += left.j*sideways;
		if (index == 1)
		{
			/* (keep the receiving rear beside the crossing lane) */
			along = ride_convoy_lateral(-((object->object.position.x - lead->object.position.x)*forward.i +
				(object->object.position.y - lead->object.position.y)*forward.j));
			velocity->i += forward.i*along;
			velocity->j += forward.j*along;
		}
		angular->k += ride_convoy_turn(ride.convoy_yaw - own_yaw);
	}
}

void network_test_ride_before_objects(
	void)
{
	long tick;
	short index;

	/* (the main menu's scene ticks in a lobby too, before the game the test is for: game_time_menu_scene) */
	if (!ride.enabled || ride.failed || ride.ended || !game_in_progress() || game_time_menu_scene())
		return;
	tick = game_time_get();
	/* (a game started over: its clock too) */
	if (ride.before_tick != NONE && tick < ride.before_tick)
		ride_reset();
	if (tick == ride.before_tick)
		return;
	ride.before_tick = tick;
	if ((ride.scenario == _ride_coop || ride.scenario == _ride_coop_double) && !ride.control_ready)
	{
		char const *path = config_string("debug.network_test_ride_clock");
		FILE *clock;

		if (tick >= RIDE_READY_TIMEOUT)
		{
			ride_error("cinematic-preparation-timeout");
			return;
		}
		/* (tick zero precedes the level scripts that start its cinematic) */
		if (tick <= 0 || !ride_local_ready() || (ride_host() && !ride_players_ready()))
			return;
		if (!path[0])
		{
			ride_error("missing-board-clock");
			return;
		}
		clock = fopen(path, ride_host() ? "w" : "r");
		ride.control_ready = ride_clock_tick(ride_host(), ride_local_ready(), tick, clock, ride.session, &ride.board_tick);
		if (clock)
			fclose(clock);
		if (!ride.control_ready)
		{
			if (ride_host())
				ride_error("board-clock-write");
			return;
		}
		ride_log("ready", "tick=%ld board_tick=%ld", tick, ride.board_tick);
	}
	if (tick == ride.board_tick - RIDE_PREPARE_TICKS && ride_host() &&
		(!ride_select_vehicles() || (ride.scenario != _ride_longsword && !ride_assign_driver(TRUE))))
	{
		ride_error("vehicle-preparation");
		return;
	}
	if (tick == ride.board_tick - (ride.scenario != _ride_longsword ? RIDE_DRIVEN_PREBOARD : 0) && !ride.prepared)
	{
		if (ride.scenario == _ride_longsword && !ride.have_spot)
		{
			ride_survey(tick);
			return;
		}
		ride.prepared = ride_prepare();
		if (ride.prepared && ride.scenario == _ride_longsword && ride_host())
			ride.prepared = ride_assign_driver(TRUE);
		if (!ride.prepared)
		{
			ride_error("rider-preparation");
			return;
		}
	}
	if (ride.prepared && ride.scenario != _ride_longsword && tick == ride.board_tick && !ride_setup_supported(tick))
	{
		ride_error("setup-unsupported");
		return;
	}
	if (ride.prepared && (ride.scenario == _ride_brush || ride.scenario == _ride_rise) &&
		tick == ride.board_tick + RIDE_JUMP && !ride_setup_supported(tick))
		ride_log("support_failure", "tick=%ld phase=jump", tick);
	/* (the convoy's vehicles are held from their placing, not left to slide down the slope until the board tick) */
	if (!ride.prepared && !((ride.scenario == _ride_convoy || ride.scenario == _ride_longsword) && ride_host() &&
		ride.vehicles[0] != NONE && tick > ride.board_tick - RIDE_PREPARE_TICKS))
	{
		return;
	}
	if (ride.prepared)
	{
		ride_suspend_scripts();
		ride_monitor_controls(tick);
		ride_reboard(tick);
		ride_driven_recover(tick);
		/* (both machines the same, as a pickup would make it: game.c) */
		if (ride.scenario == _ride_coop_double)
			game_set_players_are_double_speed(TRUE);
	}
	if (ride.scenario == _ride_longsword)
		return;
	if (!ride_host() || ride.scenario == _ride_driven)
		return;
	for (index = 0; index < ride_vehicle_count(); index++)
	{
		struct object_datum *object =
			(struct object_datum *)object_try_and_get_and_verify_type(ride.vehicles[index], _object_mask_vehicle);
		real_vector3d velocity;
		real_vector3d angular;

		if (!object)
		{
			ride_error("vehicle-deleted");
			return;
		}
		ride_drive_motion(index, tick - ride.board_tick, object, &velocity, &angular);
		object->object.translational_velocity = velocity;
		object->object.angular_velocity = angular;
		SET_FLAG(object->object.flags, _object_at_rest_bit, FALSE);
		object_activate(ride.vehicles[index]);
	}
}

/* what the host's vehicles hold once the objects have updated: the velocity
the next tick's before hook writes, so the state sent between the ticks
carries what the next step starts from */
static void ride_drive_after(
	long tick)
{
	short index;

	if (!ride_host() || ride.scenario == _ride_driven || ride.scenario == _ride_longsword)
		return;
	for (index = 0; index < ride_vehicle_count(); index++)
	{
		struct object_datum *object =
			(struct object_datum *)object_try_and_get_and_verify_type(ride.vehicles[index], _object_mask_vehicle);
		real_vector3d velocity;
		real_vector3d angular;

		if (!object || TEST_FLAG(object->object.flags, _object_at_rest_bit))
			continue;
		/* (the state sent between the ticks carries what the next step starts from, as a real game's does; the before write repeats it) */
		ride_drive_motion(index, tick + 1 - ride.board_tick, object, &velocity, &angular);
		object->object.translational_velocity = velocity;
		object->object.angular_velocity = angular;
	}
}

void network_test_ride_after_objects(
	void)
{
	long tick;

	if (!ride.enabled || ride.failed || ride.ended || !ride.prepared || !game_in_progress() || game_time_menu_scene())
		return;
	tick = game_time_get();
	if (tick < ride.board_tick || tick == ride.after_tick)
		return;
	ride.after_tick = tick;
	ride_samples(tick);
	if (!ride.failed && (ride.scenario == _ride_longsword ? tick >= ride.board_tick + RIDE_PILOT_END :
		tick >= ride.board_tick + ride_phase_tick(ride.scenario, RIDE_END)))
	{
		short index;

		for (index = 0; index < RIDE_PLAYERS; index++)
		{
			struct ride_player const *rider = &ride.players[index];

			if (rider->unit != NONE)
			{
				ride_log("outcome", "tick=%ld player=%d jump_presses=%d jumps=%d landings=%d crossings=%d stepped_off=%d "
					"landed_off=%d episode_recoveries=%d support_losses=%d", tick, (int)index, (int)rider->presses, (int)rider->jumps, (int)rider->landings,
					(int)rider->crossings, rider->stepped_off ? 1 : 0, (int)rider->landed_off,
					(int)rider->episode_recoveries, (int)rider->support_losses);
			}
		}
		ride.ended = TRUE;
		ride_log("end", "tick=%ld players=%d", tick, (int)ride_rider_count());
	}
	if (!ride.failed && !ride.ended)
		ride_drive_after(tick);
}

void network_test_ride_action(
	short local_player_index,
	struct player_action *action)
{
	long tick;
	long player_index;
	struct player_datum *player;
	short absolute;
	short last_jump = NONE;
	boolean pressed_now = FALSE;
	boolean driver;
	real walk_offset = 0.f;
	boolean grounded = FALSE;
	long deck_vehicle = NONE;
	real_vector3d forward;

	if (!ride.enabled || ride.failed || ride.ended ||
		local_player_index < 0 || local_player_index >= MAXIMUM_LOCAL_PLAYERS)
	{
		return;
	}
	/* neutralize any existing pilot's generic test facing until rider preparation finishes */
	if (!ride.prepared && (ride.scenario != _ride_longsword || !ride_find_vehicles()))
		return;
	tick = game_time_get();
	player_index = local_player_get_player_index(local_player_index);
	player = player_index != NONE ? player_try_and_get(player_index) : NULL;
	if (!player || player->unit_index == NONE ||
		!object_try_and_get_and_verify_type(ride.vehicles[0], _object_mask_vehicle))
	{
		return;
	}
	absolute = (short)DATUM_INDEX_TO_ABSOLUTE_INDEX(player_index);
	driver = object_get(player->unit_index)->object.parent_object_index == ride.vehicles[0];
	if (!ride.prepared)
	{
		if (driver)
			ride_pilot_action(0, 0.f, 0, action);
		return;
	}
	if (absolute < RIDE_PLAYERS)
	{
		last_jump = ride.players[absolute].last_jump;
		pressed_now = ride.players[absolute].last_press_tick == tick;
	}
	if (ride.scenario == _ride_longsword)
	{
		long hold;
		short step = ride_pilot_at(tick - ride.board_tick, &hold);

		ride_pilot_action(driver ? step : 0, driver ? ride.tilt_yaw + (step >= 2 ? _pi/2.f : 0.f) :
			ride_heading(ride.vehicles[0], &forward),
			vehicle_datum_get(ride.vehicles[0])->vehicle.on_ground_ticks, action);
		if (driver && step == 10)
		{
			action->desired_facing.yaw = ride.tilt_yaw + _pi/2.f + _pi;
			if (hold >= 150)
			{
				SET_FLAG(action->control_flags, _unit_control_jump_bit, FALSE);
				action->throttle.i = 1.f;
			}
		}
		/* start the hover lift, then climb slowly without accelerating forward into the spawn */
		if (driver && step == 1 && hold >= 20 && hold % 15 >= 7)
			SET_FLAG(action->control_flags, _unit_control_jump_bit, FALSE);
		/* stop rising below the hangar roof; use an odd-period hover, not alternate-tick pulses */
		if (driver && ((step == 1 && hold >= 550) || step == 2))
			SET_FLAG(action->control_flags, _unit_control_jump_bit, hold % 3 == 0);
		if (driver && step == 3 && hold >= 180)
		{
			action->throttle.i = hold < 420 ? (hold - 179)/480.f : .5f;
			action->desired_facing.pitch = 6.f*_pi/180.f;
		}
		if (!driver && (step == 0 || step == 3 || step == 6 || step == 8) && hold >= 120 &&
			(step > last_jump || (pressed_now && step == last_jump)))
		{
			SET_FLAG(action->control_flags, _unit_control_jump_bit, TRUE);
		}
		/* (landing starts this episode's walk; a later contact blip must not stop it) */
		if (step != 12)
			ride.door_started = FALSE;
		else if (!ride.door_started)
		{
			ride.door_started = ride_tilt_landed(vehicle_datum_get(ride.vehicles[0])->vehicle.on_ground_ticks);
			if (ride.door_started)
			{
				ride.door_tick = tick;
				unit_open(ride.vehicles[0]);
			}
		}
		if (!driver && step == 12 && ride.door_started)
		{
			/* Measured in Extinction: wait for opening, forward to the passage,
			right crouched, settle down it, then forward along the open ramp.
			The remaining hold lets the deck's contact grace expire. */
			long walk = tick - ride.door_tick;
			action->throttle.i = (walk >= 60 && walk < 92) || (walk >= 132 && walk < 282) ? 1.f : 0.f;
			action->throttle.j = walk >= 92 && walk < 112 ? -1.f : 0.f;
			SET_FLAG(action->control_flags, _unit_control_crouch_modifier_bit, walk >= 92);
		}
		/* (the opt-in play-test reproduction: hold nose up and walk into the rear corner, then away; only the local
		test, where both riders are ours) */
		if (ride.corner && network_test.mode == _network_test_local && step == 5)
		{
			if (driver)
				action->desired_facing.pitch = 89.9f*_pi/180.f;
			else if (hold >= 120)
			{
				action->throttle.i = hold < 240 ? -1.f : 1.f;
				action->throttle.j = hold < 240 ? -.7f : 0.f;
			}
		}
	}
	else
	{
		/* (only the machine that owns the rider reads its deck; the walk's first tick fixes the origin, and a node change
		(another frame) starts it again in the walk and ends the re-centring between jumps) */
		short phase = ride_phase(ride.scenario, tick - ride.board_tick);

		if (!driver && absolute < RIDE_PLAYERS && ride.players[absolute].local &&
			(phase == _ride_phase_walk || phase == _ride_phase_jump))
		{
			struct ride_player *rider = &ride.players[absolute];
			struct vehicle_riding_deck deck;
			struct biped_datum *biped = (struct biped_datum *)object_try_and_get_and_verify_type(player->unit_index, _object_mask_biped);

			if (biped && biped_riding_deck(player->unit_index, &deck))
			{
				if (phase == _ride_phase_walk && (!rider->walk_origin_valid || rider->walk_node != deck.node_index))
				{
					rider->walk_origin = deck.place.x;
					rider->walk_node = deck.node_index;
					rider->walk_origin_valid = TRUE;
				}
				if (rider->walk_origin_valid && rider->walk_node == deck.node_index)
				{
					walk_offset = deck.place.x - rider->walk_origin;
					grounded = !TEST_FLAG(biped->biped.flags, _biped_airborne_bit) && deck.ticks == RIDING_TICKS;
				}
			}
		}
		if (!driver && phase == _ride_phase_step_off && ride.scenario == _ride_convoy && absolute < RIDE_PLAYERS)
		{
			struct vehicle_riding_deck deck;

			/* (keep the departure's side after the deck window expires) */
			deck_vehicle = ride.players[absolute].previous_vehicle;
			if (biped_riding_deck(player->unit_index, &deck))
				deck_vehicle = deck.vehicle_index;
		}
		ride_script_action(ride.scenario, tick - ride.board_tick, driver, ride_heading(ride.vehicles[0], &forward),
			last_jump, pressed_now, grounded, walk_offset, deck_vehicle, action);
	}
	/* (only a physical switch during this cross phase counts; a jump-phase
	round trip cannot stop the later approach on the original deck) */
	if (ride.scenario == _ride_convoy && absolute < RIDE_PLAYERS &&
		ride_phase(ride.scenario, tick - ride.board_tick) == _ride_phase_cross && ride.players[absolute].crossings > 0)
	{
		action->throttle.j = 0.f;
		action->control_flags = 0;
	}
	if (ride.scenario == _ride_driven && (tick < ride.board_tick || ride_driven_holding(tick)))
	{
		/* (the Warthog's jump input is its brake; stop through physical
		acquisition and the standing hold before resuming the drive) */
		if (driver)
		{
			action->throttle.i = action->throttle.j = 0.f;
			SET_FLAG(action->control_flags, _unit_control_jump_bit, TRUE);
			action->desired_facing.yaw = ride_heading(ride.vehicles[0], &forward);
		}
		else
			SET_FLAG(action->control_flags, _unit_control_jump_bit, FALSE);
	}
	else if (ride.scenario == _ride_driven && !driver && absolute < RIDE_PLAYERS &&
		ride_phase(ride.scenario, tick - ride.board_tick) == _ride_phase_jump)
	{
		struct ride_player *rider = &ride.players[absolute];

		/* (the press count is the queue: a recovery cannot consume a
		suppressed slot; release one each normal period, in this phase only) */
		SET_FLAG(action->control_flags, _unit_control_jump_bit, pressed_now ||
			(rider->presses < RIDE_JUMPS && ride_jump_slot(tick - ride.board_tick) >= rider->presses &&
			(rider->last_press_tick == NONE || tick - rider->last_press_tick >= RIDE_JUMP_PERIOD)));
	}
	if (driver || absolute >= RIDE_PLAYERS)
		return;
	/* (a press counted once a tick, however often the tick's input is asked) */
	if (TEST_FLAG(action->control_flags, _unit_control_jump_bit) && ride.players[absolute].last_press_tick != tick)
	{
		long hold;

		ride.players[absolute].last_press_tick = tick;
		ride.players[absolute].last_jump = ride.scenario == _ride_longsword ? ride_pilot_at(tick - ride.board_tick, &hold) :
			ride_phase(ride.scenario, tick - ride.board_tick) == _ride_phase_cross ? RIDE_JUMPS :
			ride_press_slot(ride.scenario, tick - ride.board_tick, ride.pair);
		ride.players[absolute].presses++;
	}
	ride_log("input", "tick=%ld player=%d unit=%ld phase=%s jump=%d throttle=%.4f,%.4f", tick, (int)absolute,
		player->unit_index, ride_phases[ride_phase(ride.scenario, tick - ride.board_tick)],
		TEST_FLAG(action->control_flags, _unit_control_jump_bit) ? 1 : 0, action->throttle.i, action->throttle.j);
}

void network_test_ride_state(
	short player_index,
	long unit_index,
	long host_tick,
	boolean predicted,
	word predicted_low16,
	real_point3d const *world,
	boolean deck_present,
	struct vehicle_riding_deck const *deck)
{
	struct vehicle_riding_deck absent;

	/* (the host logs from the test's start, not once prepared, and past its own
	end: the runner checks each state a client applied against one the host
	logged, and a client may apply a state the host collected before its own
	riders were prepared, or one past the host's end) */
	if (ride_host() ? !ride.enabled : (!ride.enabled || !ride.prepared || ride.ended))
		return;
	if (!deck_present)
	{
		csmemset(&absent, 0, sizeof(absent));
		absent.vehicle_index = NONE;
		absent.node_index = NONE;
		deck = &absent;
	}
	ride_log("state", "tick=%ld host_tick=%ld player=%d unit=%ld predicted_valid=%d predicted_low16=%u deck_present=%d "
		"vehicle=%ld node=%d ticks=%d flags=%u velocity=%.4f,%.4f,%.4f peak=%.4f world=%.4f,%.4f,%.4f "
		"place=%.4f,%.4f,%.4f", game_time_get(), host_tick, (int)player_index, unit_index, predicted ? 1 : 0,
		(unsigned)predicted_low16, deck_present ? 1 : 0, deck->vehicle_index, (int)deck->node_index, (int)deck->ticks,
		(unsigned)deck->flags, deck->velocity.i, deck->velocity.j, deck->velocity.k, deck->peak, world->x, world->y,
		world->z, deck->place.x, deck->place.y, deck->place.z);
}

void network_test_ride_applied(
	short player_index,
	long unit_index,
	long host_tick)
{
	if (!ride.enabled || !ride.prepared || ride.ended || player_index < 0 || player_index >= RIDE_PLAYERS)
		return;
	ride.players[player_index].applied_host_tick = host_tick;
	ride_log("apply", "tick=%ld player=%d unit=%ld applied_host_tick=%ld", game_time_get(), (int)player_index,
		unit_index, host_tick);
}

void network_test_ride_own(
	short player_index,
	long tick,
	long unit_index)
{
	if (!ride.enabled || !ride.prepared || ride.ended || player_index < 0 || player_index >= RIDE_PLAYERS ||
		ride.players[player_index].unit != unit_index)
	{
		return;
	}
	ride_rider_log("own", player_index, unit_index, tick, TRUE);
}

boolean network_test_ride_running(
	void)
{
	return ride.enabled;
}

void network_test_ride_prediction(
	short player_index,
	long unit_index,
	long host_tick,
	word predicted_low16,
	boolean eligible,
	boolean accepted)
{
	if (!ride.enabled || !ride.prepared || ride.ended)
		return;
	ride_log("prediction", "host_tick=%ld player=%d unit=%ld predicted_low16=%u eligible=%d accepted=%d", host_tick,
		(int)player_index, unit_index, (unsigned)predicted_low16, eligible ? 1 : 0, accepted ? 1 : 0);
}

void network_test_ride_correction(
	short player_index,
	long unit_index,
	real_point3d const *before,
	real_point3d const *after)
{
	if (!ride.enabled || !ride.prepared || ride.ended || game_time_get() < ride.board_tick)
		return;
	if (player_index >= 0 && player_index < RIDE_PLAYERS)
		ride.players[player_index].tick_corrections++;
	ride_log("correction", "tick=%ld player=%d unit=%ld before=%.4f,%.4f,%.4f after=%.4f,%.4f,%.4f", game_time_get(),
		(int)player_index, unit_index, before->x, before->y, before->z, after->x, after->y, after->z);
}

void network_test_ride_history(
	short player_index,
	long tick,
	long unit_index,
	real_point3d const *before,
	real_point3d const *after,
	struct vehicle_riding_deck const *deck,
	long window,
	long air_episode)
{
	if (!ride.enabled || !ride.prepared || ride.ended)
		return;
	ride_log("history", "tick=%ld player=%d unit=%ld old_world=%.4f,%.4f,%.4f new_world=%.4f,%.4f,%.4f "
		"deck_present=%d vehicle=%ld node=%d ticks=%d place=%.4f,%.4f,%.4f velocity=%.4f,%.4f,%.4f window=%ld air_episode=%ld", tick,
		(int)player_index, unit_index, before->x, before->y, before->z, after->x, after->y, after->z,
		deck ? 1 : 0, deck ? deck->vehicle_index : NONE, deck ? (int)deck->node_index : NONE, deck ? (int)deck->ticks : 0,
		deck ? deck->place.x : 0.0f, deck ? deck->place.y : 0.0f, deck ? deck->place.z : 0.0f,
		deck ? deck->velocity.i : 0.0f, deck ? deck->velocity.j : 0.0f, deck ? deck->velocity.k : 0.0f, window, air_episode);
}

void network_test_ride_grace(
	short player_index,
	short ticks)
{
	if (!ride.enabled || !ride.prepared || ride.ended)
		return;
	ride_log("grace", "tick=%ld player=%d grace_ticks=%d", game_time_get(), (int)player_index, (int)ticks);
}

void network_test_ride_vehicle_written(
	long vehicle_index,
	real_point3d const *before,
	real_point3d const *after,
	short kind)
{
	short index;

	if (!ride.enabled || !ride.prepared || ride.ended)
		return;
	for (index = 0; index < ride_vehicle_count(); index++)
	{
		if (ride.vehicles[index] == vehicle_index)
		{
			ride.vehicle_written[index] += distance3d(before, after);
			ride.vehicle_writes[index]++;
			if (kind == _network_vehicle_write_carry)
				ride.vehicle_carries[index]++;
		}
	}
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
	case _network_test_local:
		if (!network_test.started)
		{
			network_test.started = TRUE;
			network_test_start_local();
			platform_log("network test: local split screen %s", network_test.map_name);
		}
		break;
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
				/* (as Server Setup's START GAME does: menu_functions.c's server_start; a split screen game never rides) */
				if (!network_test.local)
					network_game_server_port_set_vehicle_riding(config_boolean("game.vehicle_riding") != 0);
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

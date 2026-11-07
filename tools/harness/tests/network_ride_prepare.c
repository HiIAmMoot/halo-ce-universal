/*
NETWORK_RIDE_PREPARE.C (test)

Co-op preparation's palette driver, client seat gap and host-issued clock,
using the real functions and caller expressions from network_test.c.
*/
#include "harness.h"
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#define TAG_BLOCK_GET_ELEMENT(block, index, type) (&((type *)(block)->address)[index])
struct tag_block { long count; void *address; };
struct tag_reference { long index; };
struct scenario_object_palette_entry { struct tag_reference reference; };
struct scenario { struct tag_block biped_palette; struct tag_block ai_actor_palette; };
struct actor_variant_definition { struct tag_reference unit_reference; };
static struct actor_variant_definition variants[3];
static struct actor_variant_definition *actor_variant_definition_get(long index) { return &variants[index]; }
#define _object_dead_bit 0
#define _unit_controllable_bit 0
#define _unit_suspended_bit 1
struct object_datum { struct { real_point3d position; long parent_object_index; } object; long definition_index; };
struct biped_definition { struct { real runtime_minimum_normal_k; } biped; };
struct unit_datum { struct { real_point3d position; long parent_object_index; unsigned long damage_flags; } object; struct { long driver_object_index; long player_index; unsigned long flags; long actor_index; } unit; };
struct data_iterator { long datum_index; };
struct player_datum { short local_player_index; long unit_index; };
static struct object_datum objects[104];
static struct unit_datum units[104];
static struct player_datum players[2];
#include "ride_prepare_types.inc"
static struct {
	boolean enabled, failed, prepared, ended, control_ready, have_spot, scripts_suspended;
	long before_tick;
	short scenario;
	long vehicles[2];
	long driver;
	long board_tick;
	char session[64];
	boolean client_driver;
	real pair;
	real tilt_yaw;
	struct ride_player players[128];
} ride;
static boolean seated;
static long seated_unit = NONE;
static boolean host = TRUE;
enum { _network_test_off, _network_test_host, _network_test_join, _network_test_browse, _network_test_local };
static struct { short mode; } network_test;
static boolean local_ready = TRUE;
static boolean ride_local_ready(void) { return local_ready; }
static long player_count = 2;
static struct scenario scenario;
static char const *names[3];
static long created = NONE;
static long actor_variant = NONE;
static boolean actor_created;
static boolean braindead;
static boolean braindead_when_seated;
static boolean actor_failure;
static boolean object_failure;
static boolean encounterless;
static short actor_initial_state;
static short actor_default_state;
static long actor_create_for_unit(boolean swarm, long unit, long variant, long encounter, short squad,
	boolean add_to_squads, long disallow, boolean initially_braindead, short initial_state,
	short default_state, short command_list, short sequence)
{
	actor_created = TRUE;
	actor_variant = variant;
	encounterless = !swarm && unit == 103 && encounter == NONE && squad == NONE &&
		!add_to_squads && disallow == NONE && !initially_braindead && command_list == NONE && sequence == 0;
	actor_initial_state = initial_state;
	actor_default_state = default_state;
	if (actor_failure)
		return NONE;
	units[unit].unit.actor_index = 88;
	return 88;
}
static void actor_braindead(long actor, boolean value) { braindead = actor == 88 && value; }
static char logged[512];
static struct scenario *global_scenario_get(void) { return &scenario; }
static char const *tag_get_name(long index) { return names[index]; }
static struct object_datum *object_get(long index) { return &objects[index]; }
static struct unit_datum healthy_unit = { { { 0, 0, 0 }, NONE, 0 }, { NONE, NONE, 1 } };
/* (an absent unit reads as a healthy one, so a check that skips NONE shows) */
static struct unit_datum *unit_get(long index) { return index == NONE ? &healthy_unit : &units[index]; }
static long ride_new(long definition, real_point3d const *position)
{
	created = definition;
	units[103].unit.actor_index = NONE;
	braindead = FALSE;
	return object_failure ? NONE : 103;
}
static boolean ride_seat(long unit, long vehicle_index)
{
	seated = unit >= 101 && unit <= 103 && vehicle_index == 100;
	seated_unit = unit;
	braindead_when_seated = units[103].unit.actor_index == 88 && braindead;
	return seated;
}
static void *player_data;
static void data_iterator_new(struct data_iterator *iterator, void *data) { iterator->datum_index = NONE; }
static void *data_iterator_next(struct data_iterator *iterator)
{
	iterator->datum_index++;
	return iterator->datum_index < player_count ? &players[iterator->datum_index] : NULL;
}
static boolean ride_find_vehicles(void) { return TRUE; }
/* the Longsword has one player riding; co-op has both and a palette marine pilot */
static struct biped_definition biped_definition_stub;
static struct biped_definition *biped_definition_get(long index) { return &biped_definition_stub; }
static real ride_tilt_pair(real minimum_normal_k) { return minimum_normal_k; }
static short ride_rider_count(void) { return ride.driver == 101 || ride.driver == 102 ? 1 : 2; }
static real heading = 0.f;
static real ride_heading(long vehicle_index, real_vector3d *forward) { return heading; }
static boolean ride_host(void) { return host; }
static boolean ride_board(long unit, long vehicle, short ordinal) { return TRUE; }
static long shield_calls, shield_player, shield_unit, shield_vehicle;
static boolean riding_on = TRUE;
static boolean vehicle_riding_enabled(void) { return riding_on; }
static void network_distributed_own_reboard(short local_player_index, long unit_index, long vehicle_index)
{
	shield_calls++; shield_player = local_player_index; shield_unit = unit_index; shield_vehicle = vehicle_index;
}
static long now = 1122;
static long game_time_get(void) { return now; }
static long inhibited_records, inhibited_tick, inhibited_player;
static char inhibited_body[512];
static void ride_log(char const *kind, char const *format, ...)
{
	va_list arguments;
	va_start(arguments, format);
	vsnprintf(logged, sizeof(logged), format, arguments);
	va_end(arguments);
	if (!strcmp(kind, "control_inhibited"))
	{
		inhibited_records++;
		inhibited_tick = now;
		strcpy(inhibited_body, logged);
	}
}
static boolean input_enabled = TRUE;
static void player_input_enable(boolean enabled) { input_enabled = enabled; }
static boolean player_input_enabled(void) { return input_enabled; }
/* (the real HS scheduler below decides whether this script body may run) */
enum { _hs_thread_type_script, _hs_thread_type_global_initialize, _hs_thread_type_console_command };
enum { _hs_thread_sleeping_bit = 1 };
#define SET_FLAG(flags, bit, value) ((value) ? ((flags) |= FLAG(bit)) : ((flags) &= ~FLAG(bit)))
struct hs_thread_datum { short type; long sleep_until, previous_sleep_until; unsigned long flags; };
static struct hs_thread_datum threads[256];
static short thread_count = 2;
static struct { boolean initialized; } hs_runtime_globals = { TRUE };
static void *hs_thread_data;
#define hs_thread_get(index) (&threads[index])
static long data_next_index(void *data, long index) { return index + 1 < thread_count ? index + 1 : NONE; }
static long script_calls;
static void hs_thread_main(long index) { script_calls++; player_input_enable(FALSE); }
static void object_list_gc(void) {}
static void hs_node_gc(void) {}
static boolean game_in_progress(void) { return TRUE; }
static boolean game_time_menu_scene(void) { return FALSE; }
static void ride_reset(void) {}
static char const *config_string(char const *key) { return ""; }
static void ride_error(char const *reason) { ride.failed = TRUE; }
static boolean ride_select_vehicles(void) { return TRUE; }
static void ride_survey(long tick) {}
static boolean ride_setup_supported(long tick) { return TRUE; }
static void ride_reboard(long tick) {}
static void ride_driven_recover(long tick) {}
static void game_set_players_are_double_speed(boolean enabled) {}
static char const *const ride_phases[] = { "prepare", "stand", "walk", "jump", "cross", "step-off", "observe" };
#include "ride_prepare_under_test.inc"

int main(int argc, char **argv)
{
	char const *case_name = argc > 1 ? argv[1] : "";
	FILE *clock;
	long board = NONE;
	short index;
	struct scenario_object_palette_entry entries[2];
	struct tag_reference actor_entries[4];

	ride.scenario = _ride_coop;
	ride.vehicles[0] = 100;
	ride.vehicles[1] = NONE;
	ride.board_tick = 1122;
	strcpy(ride.session, "session");
	units[100].unit.driver_object_index = NONE;
	units[103].unit.player_index = NONE;
	entries[0].reference.index = 0;
	entries[1].reference.index = 1;
	scenario.biped_palette.count = 2;
	scenario.biped_palette.address = entries;
	actor_entries[0].index = NONE;
	actor_entries[1].index = 0;
	actor_entries[2].index = 1;
	scenario.ai_actor_palette.count = 3;
	scenario.ai_actor_palette.address = actor_entries;
	variants[0].unit_reference.index = 0;
	variants[1].unit_reference.index = 1;
	variants[2].unit_reference.index = 1; /* loaded but not in the actor palette */
	names[0] = "characters\\pilot\\pilot";
	names[1] = "characters\\marine_armored\\marine_armored";
	names[2] = "characters\\marine\\marine"; /* loaded, but not in the palette */
	for (index = 0; index < 2; index++)
	{
		players[index].unit_index = 101 + index;
		players[index].local_player_index = index ? NONE : 0;
		objects[101 + index].object.parent_object_index = NONE;
		units[101 + index].object.parent_object_index = NONE;
		units[101 + index].unit.flags = FLAG(_unit_controllable_bit);
		units[101 + index].unit.player_index = index;
	}
	CASE("ride-coop-scripts-scoped")
	{
		short scenario_index;
		short enabled;
		short prepared;
		for (scenario_index = 0; scenario_index < NUMBER_OF_RIDE_SCENARIOS; scenario_index++)
		for (enabled = 0; enabled < 2; enabled++)
		for (prepared = 0; prepared < 2; prepared++)
		{
			boolean suspend = enabled && prepared && (scenario_index == _ride_coop || scenario_index == _ride_coop_double);
			ride.scenario = scenario_index;
			ride.enabled = enabled;
			ride.prepared = prepared;
			ride.scripts_suspended = FALSE;
			threads[0].sleep_until = 900;
			threads[0].previous_sleep_until = 0;
			threads[0].flags = 0;
			threads[0].type = _hs_thread_type_script;
			ride_suspend_scripts();
			CHECK((threads[0].sleep_until == NONE - 1) == suspend,
				"script suspension scope wrong: scenario=%d enabled=%d prepared=%d", scenario_index, enabled, prepared);
			CHECK(!suspend || (threads[0].previous_sleep_until == 900 && TEST_FLAG(threads[0].flags, _hs_thread_sleeping_bit)),
				"engine sleep did not preserve the old wake time");
		}
		return 0;
	}
	CASE("ride-coop-script-disable-suspended")
	{
		ride.enabled = ride.prepared = ride.control_ready = TRUE;
		ride.before_tick = NONE;
		ride.players[0].unit = 101;
		ride.players[0].local = TRUE;
		threads[0].type = _hs_thread_type_script;
		threads[0].sleep_until = now + 1;
		/* (a completed startup and a console thread must retain their states) */
		threads[1].type = _hs_thread_type_console_command;
		threads[1].sleep_until = now + 20;
		network_test_ride_before_objects();
		CHECK(threads[0].sleep_until == NONE - 1, "successful preparation did not suspend the level script");
		CHECK(threads[1].sleep_until == now + 20, "suspension changed a console thread");
		now++;
		hs_runtime_update();
		CHECK(script_calls == 0 && player_input_enabled(), "script ran after suspension and zeroed owner input");
		/* (largest engine thread table, with no quadratic dedup or skipped tail) */
		thread_count = 256;
		for (index = 0; index < 256; index++)
		{
			threads[index].type = _hs_thread_type_script;
			threads[index].sleep_until = now;
		}
		hs_runtime_sleep_scripts();
		hs_runtime_update();
		CHECK(script_calls == 0 && player_input_enabled(), "tail of 256-thread table still ran");
		threads[255].sleep_until = NONE;
		hs_runtime_sleep_scripts();
		CHECK(threads[255].sleep_until == NONE, "finished script revived by suspension");
		return 0;
	}
	CASE("ride-control-hook-off")
	{
		unsigned char snapshot[sizeof(ride)];
		for (index = 0; index < 2; index++)
		{
			ride.enabled = index != 0;
			ride.prepared = index == 0;
			ride.players[0].unit = 101;
			ride.players[0].local = TRUE;
			memcpy(snapshot, &ride, sizeof(ride));
			network_test_ride_control_delivered(0, FALSE);
			CHECK(!memcmp(snapshot, &ride, sizeof(ride)), "off/unprepared hook wrote test state");
		}
		return 0;
	}
	if (!strcmp(case_name, "ride-coop-control-inhibited-scored") ||
		!strcmp(case_name, "ride-coop-inhibited-then-enabled-before-hook") ||
		!strcmp(case_name, "ride-coop-delivered-then-disabled-before-hook"))
	{
		long board_snapshot;
		short press_snapshot;
		boolean delivered_first = !strcmp(case_name, "ride-coop-delivered-then-disabled-before-hook");
		ride.enabled = ride.prepared = ride.control_ready = TRUE;
		ride.before_tick = NONE;
		ride.players[0].unit = 101;
		ride.players[0].local = TRUE;
		ride.players[0].presses = 4;
		board_snapshot = ride.board_tick;
		press_snapshot = ride.players[0].presses;
		now = ride.board_tick + RIDE_JUMP;
		for (index = 0; index < 6; index++, now++)
		{
			network_test_ride_control_delivered(0, delivered_first);
			input_enabled = !delivered_first;
			network_test_ride_before_objects();
			CHECK(inhibited_records == (delivered_first ? 0 : 1), "wrong inhibited episode count %ld", inhibited_records);
		}
		CHECK(delivered_first || (inhibited_tick == board_snapshot + RIDE_JUMP &&
			strstr(inhibited_body, "phase=jump player=0")), "record lost its first tick, phase or player: %s", inhibited_body);
		network_test_ride_control_delivered(0, TRUE);
		network_test_ride_before_objects();
		now++;
		/* (a stale inhibited observation is not this tick's evidence) */
		network_test_ride_control_delivered(0, FALSE);
		now++;
		network_test_ride_before_objects();
		CHECK(inhibited_records == (delivered_first ? 0 : 1), "stale observation judged");
		now++;
		network_test_ride_control_delivered(0, FALSE);
		network_test_ride_before_objects();
		CHECK(inhibited_records == (delivered_first ? 1 : 2), "new inhibition not logged after delivery");
		CHECK(ride.board_tick == board_snapshot && ride.players[0].presses == press_snapshot,
			"monitor paused the programme or rewrote consumed presses");
		return 0;
	}
	CASE("ride-coop-driver-actor")
	{
		for (index = 0; index < 2; index++)
		{
			ride.scenario = index ? _ride_coop_double : _ride_coop;
			CHECK(ride_assign_driver(TRUE), "co-op driver preparation failed");
			CHECK(units[103].unit.actor_index == 88 && actor_variant == 1 && encounterless,
				"co-op driver has no encounterless actor from its matching palette variant");
			CHECK(actor_initial_state == 2 && actor_default_state == 0, "actor not created like ai_attach_free");
			CHECK(seated && braindead_when_seated, "co-op driver was not made braindead before seating");
		}
		return 0;
	}
	CASE("ride-coop-driver-client")
	{
		host = FALSE;
		CHECK(!ride_assign_driver(TRUE) && created == NONE && !actor_created && !seated,
			"client created or seated a co-op AI driver");
		return 0;
	}
	CASE("ride-coop-driver-no-variant")
	{
		variants[1].unit_reference.index = 0;
		CHECK(!ride_assign_driver(TRUE) && created == NONE && !actor_created && !seated,
			"unmatched marine accepted or actor variant selected outside scenario palette");
		return 0;
	}
	CASE("ride-coop-driver-failure")
	{
		actor_failure = TRUE;
		CHECK(!ride_assign_driver(TRUE) && actor_created && !seated && !braindead,
			"failed actor creation accepted, seated, or made braindead");
		actor_created = FALSE;
		object_failure = TRUE;
		CHECK(!ride_assign_driver(TRUE) && !actor_created && !seated,
			"failed marine creation reached actor creation or seating");
		return 0;
	}
	CASE("ride-palette-driver")
	{
		CHECK(ride_assign_driver(TRUE) && seated && created == 1, "a30's palette marine not seated");
		ride.scenario = _ride_coop_double;
		names[1] = "characters\\marine_variant\\marine_variant";
		CHECK(ride_assign_driver(TRUE) && seated && created == 1, "palette variant not seated");
		names[1] = "characters\\pilot\\pilot";
		/* (the old fallback control must not be hidden by the new variant lookup) */
		actor_entries[3].index = 2;
		variants[2].unit_reference.index = 2;
		scenario.ai_actor_palette.count = 4;
		created = NONE;
		seated = FALSE;
		CHECK(!ride_assign_driver(TRUE) && created == NONE && !seated, "absent marine replaced by a loaded tag");
		return 0;
	}
	CASE("ride-tilt-yaw")
	{
		/* both machines align the parked ship with the same lane through pilot input */
		ride.scenario = _ride_longsword;
		host = FALSE;
		heading = 1.2f;
		ride.tilt_yaw = -1.f;
		units[100].unit.driver_object_index = 103;
		units[103].unit.player_index = 0;
		CHECK(ride_prepare(), "a client's Longsword preparation failed");
		CHECK(ride.tilt_yaw == 0.f, "the parking heading %g, not the lane's", (double)ride.tilt_yaw);
		return 0;
	}
	CASE("ride-driver-choice")
	{
		short pilot;
		short rider;

		ride.scenario = _ride_longsword;
		for (index = 0; index < 4; index++)
		{
			host = (index & 2) == 0;
			ride.client_driver = (index & 1) != 0;
			pilot = host ? ride.client_driver : !ride.client_driver;
			rider = 1 - pilot;
			seated = FALSE;
			ride.players[0].unit = ride.players[1].unit = NONE;
			CHECK(ride_prepare() && ride.driver == 101 + pilot, "wrong pilot for host=%d client_driver=%d", host, ride.client_driver);
			CHECK(!seated, "identifying the pilot seated them before rider placement");
			CHECK(ride.players[pilot].unit == NONE && ride.players[rider].unit == 101 + rider,
				"pilot teleported with the rider");
			CHECK(ride_assign_driver(TRUE) && seated && seated_unit == 101 + pilot, "wrong player seated");
		}
		return 0;
	}
	CASE("ride-local-driver")
	{
		ride.scenario = _ride_longsword;
		network_test.mode = _network_test_local;
		ride.players[0].unit = ride.players[1].unit = NONE;
		/* reverse iteration order: local index, not datum order, chooses the pilot */
		players[0].local_player_index = 1;
		players[1].local_player_index = 0;
		CHECK(ride_prepare() && ride.driver == 102, "local player 0 not selected as pilot");
		CHECK(!seated, "pilot seated before rider placed");
		CHECK(ride.players[0].unit == 101 && ride.players[1].unit == NONE, "local rider not sole boarded player");
		CHECK(ride_assign_driver(TRUE) && seated_unit == 102, "wrong local pilot seated");
		return 0;
	}
	CASE("ride-prepare-arms-own-shield")
	{
		/* (a client's own player, put on the deck by its own preparation, is the one the host's older states would undo) */
		host = FALSE;
		units[100].unit.driver_object_index = 103;
		shield_calls = 0;
		CHECK(ride_prepare(), "a client's preparation failed");
		CHECK(shield_calls == 1 && shield_player == 0 && shield_unit == 101 && shield_vehicle == 100,
			"the owner's preparation armed %ld shields (player %ld unit %ld vehicle %ld)", shield_calls, shield_player, shield_unit,
			shield_vehicle);
		host = TRUE;
		shield_calls = 0;
		CHECK(ride_prepare(), "the host's preparation failed");
		CHECK(shield_calls == 0, "the host armed a shield");
		return 0;
	}
	CASE("ride-prepare-no-shield-riding-off")
	{
		/* (the host's switch off: the debug programme runs as the original, no delay on the host's word) */
		host = FALSE;
		riding_on = FALSE;
		units[100].unit.driver_object_index = 103;
		shield_calls = 0;
		CHECK(ride_prepare(), "a client's preparation failed");
		CHECK(shield_calls == 0, "the preparation armed %ld shields with the riding switch off", shield_calls);
		return 0;
	}
	CASE("ride-coop-prepare")
	{
		for (index = 0; index < 2; index++)
		{
			ride.scenario = index ? _ride_coop_double : _ride_coop;
			host = TRUE;
			CHECK(!ride_prepare(), "host accepts empty co-op vehicle");
			units[100].unit.driver_object_index = 103;
			CHECK(ride_prepare() && ride.driver == 103, "host rejects AI driver");
			units[103].unit.player_index = 0;
			CHECK(!ride_prepare(), "host accepts player driver");
			units[103].unit.player_index = NONE;
			host = FALSE;
			CHECK(ride_prepare(), "client rejects replicated AI driver");
			units[100].unit.driver_object_index = NONE;
			CHECK(ride_prepare(), "client seat gap aborts preparation");
			CHECK(strstr(logged, "driver=-1") && strstr(logged, "driver_missing=1"), "client gap not logged: %s", logged);
		}
		return 0;
	}
	CASE("ride-host-clock")
	{
		clock = tmpfile();
		CHECK(clock != NULL, "no temporary clock");
		CHECK(!ride_clock_tick(FALSE, TRUE, 1, NULL, "session", &board), "client boards independently at tick 121");
		CHECK(!ride_clock_tick(TRUE, FALSE, 1001, clock, "session", &board), "host boards during cinematic");
		CHECK(ride_clock_tick(TRUE, TRUE, 1002, clock, "session", &board) && board == 1122, "host clock wrong");
		rewind(clock);
		board = NONE;
		CHECK(ride_clock_tick(FALSE, TRUE, 1004, clock, "session", &board) && board == 1122, "client did not take host board tick");
		rewind(clock);
		CHECK(!ride_clock_tick(FALSE, TRUE, 1004, clock, "other", &board), "another session's clock accepted");
		rewind(clock);
		CHECK(!ride_clock_tick(FALSE, TRUE, 1123, clock, "session", &board), "past board tick accepted");
		fclose(clock);
		clock = tmpfile();
		CHECK(clock != NULL, "no temporary clock");
		fputs("session 3721\n", clock);
		rewind(clock);
		CHECK(!ride_clock_tick(FALSE, TRUE, 1004, clock, "session", &board), "unbounded board tick accepted");
		fclose(clock);
		clock = tmpfile();
		CHECK(clock != NULL, "no temporary clock");
		fputs("session", clock);
		rewind(clock);
		CHECK(!ride_clock_tick(FALSE, TRUE, 1004, clock, "session", &board), "partial clock accepted");
		fclose(clock);
		return 0;
	}
	CASE("ride-clock-torn")
	{
		clock = tmpfile();
		CHECK(clock != NULL, "no temporary clock");
		fputs("session 112", clock);
		rewind(clock);
		CHECK(!ride_clock_tick(FALSE, TRUE, 100, clock, "session", &board) && board == NONE, "partial digits accepted as board tick");
		fclose(clock);
		clock = tmpfile();
		CHECK(clock != NULL, "no temporary clock");
		fputs("session 1122\r\n", clock);
		rewind(clock);
		CHECK(ride_clock_tick(FALSE, TRUE, 100, clock, "session", &board) && board == 1122, "complete CRLF clock refused");
		fclose(clock);
		return 0;
	}
	CASE("ride-caller-readiness")
	{
		clock = tmpfile();
		CHECK(clock != NULL, "no temporary clock");
		local_ready = FALSE;
		/* exercise the real call expression with the earlier return removed */
		CHECK(!ride_ungated_caller_tick(1002, clock), "caller passes TRUE while local input is unavailable");
		fclose(clock);
		return 0;
	}
	CASE("ride-players-ready")
	{
		CHECK(ride_players_ready(), "two healthy players not ready");
		players[1].unit_index = NONE;
		CHECK(!ride_players_ready(), "a remote player without a unit ready");
		players[1].unit_index = 102;
		units[102].object.damage_flags = FLAG(_object_dead_bit);
		CHECK(!ride_players_ready(), "a dead unit ready");
		units[102].object.damage_flags = 0;
		units[102].object.parent_object_index = 100;
		CHECK(!ride_players_ready(), "a parented unit ready");
		units[102].object.parent_object_index = NONE;
		units[102].unit.flags = 0;
		CHECK(!ride_players_ready(), "an uncontrollable unit ready");
		units[102].unit.flags = FLAG(_unit_controllable_bit) | FLAG(_unit_suspended_bit);
		CHECK(!ride_players_ready(), "a suspended unit ready");
		units[102].unit.flags = FLAG(_unit_controllable_bit);
		CHECK(ride_players_ready(), "healthy players not ready again");
		player_count = 1;
		CHECK(!ride_players_ready(), "a single player ready for co-op");
		return 0;
	}
	CASE("ride-caller-remote")
	{
		clock = tmpfile();
		CHECK(clock != NULL, "no temporary clock");
		players[1].unit_index = NONE;
		CHECK(!ride_caller_tick(1002, clock), "host publishes before the client's player has a unit");
		players[1].unit_index = 102;
		CHECK(ride_caller_tick(1002, clock), "host with every unit did not publish");
		fclose(clock);
		return 0;
	}
	CASE("ride-caller-startup")
	{
		clock = tmpfile();
		CHECK(clock != NULL, "no temporary clock");
		CHECK(!ride_caller_tick(0, clock), "host publishes before tick-zero cinematic scripts run");
		CHECK(ride_caller_tick(1002, clock) && ride.board_tick == 1122, "ready host did not publish clock");
		fclose(clock);
		return 0;
	}
	return 2;
}

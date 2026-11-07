/* The production pre-object preparation path, stopped before vehicle motion. */
#include "harness.h"
#include <stdio.h>
enum { _ride_longsword = 1, _ride_coop, _ride_coop_double, _ride_driven, _ride_brush, _ride_rise };
#define RIDE_JUMP 840
#define NONE (-1)
#define RIDE_READY_TIMEOUT 900
#define RIDE_PREPARE_TICKS 120
#define RIDE_DRIVEN_PREBOARD (RIDE_PREPARE_TICKS/2)
static struct {
	boolean enabled, failed, ended, control_ready, prepared;
	short scenario;
	long before_tick, board_tick;
	boolean have_spot;
} ride;
static long tick_now;
static short events, rider_event, pilot_event;
#define game_in_progress() TRUE
#define game_time_menu_scene() FALSE
#define game_time_get() tick_now
#define ride_reset() ((void)0)
#define ride_host() TRUE
#define ride_local_ready() TRUE
#define ride_setup_supported(tick) TRUE
#define ride_players_ready() TRUE
#define config_string(key) ""
#define ride_clock_tick(...) FALSE
#define ride_log(...) ((void)0)
#define ride_error(reason) (ride.failed = TRUE)
#define ride_select_vehicles() TRUE
#define ride_survey(tick) ((void)0)
static boolean ride_assign_driver(boolean seat) { if (seat) pilot_event = ++events; return TRUE; }
static boolean ride_prepare(void) { rider_event = ++events; return TRUE; }
#include "under_test.inc"
int main(int argc, char **argv)
{
	(void)argc;
	ride.enabled = ride.have_spot = TRUE;
	ride.scenario = !strcmp(argv[1], "stock") ? 0 : _ride_longsword;
	ride.before_tick = NONE;
	ride.board_tick = 300;
	tick_now = 180;
	network_test_ride_before_objects();
	if (ride.scenario != _ride_longsword)
	{
		CHECK(pilot_event == 1 && rider_event == 0, "stock preboard order changed");
		tick_now = 240;
		network_test_ride_before_objects();
		CHECK(pilot_event == 1 && rider_event == 2, "stock board order changed");
		return 0;
	}
	CHECK(pilot_event == 0, "pilot seated before rider placement");
	tick_now = 300;
	network_test_ride_before_objects();
	CHECK(rider_event == 1 && pilot_event == 2, "boarding order: rider=%d pilot=%d", rider_event, pilot_event);
	CHECK(ride.prepared && !ride.failed, "preparation did not finish");
	return 0;
}

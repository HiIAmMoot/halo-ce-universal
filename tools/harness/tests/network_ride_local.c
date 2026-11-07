/* the real local startup and setting parser, against engine boundary stubs */
#include "harness.h"
#include <stdio.h>
#include <string.h>
#define MAX(a,b) ((a) > (b) ? (a) : (b))
#define NUMBEROF(a) (sizeof(a) / sizeof((a)[0]))
#define TICKS_PER_SECOND 30
#define _game_connection_local 0
#define _game_connection_network_client 1
#define _game_connection_network_server 2
#include "local_types.inc"
static struct { boolean enabled; short scenario; } ride;
static short connection = _game_connection_network_client;
static short player_spawn_count = 1;
static short controllers[2] = { -1, -1 };
static char selected_map[128], selected_variant[64];
static boolean deferred, closed, failed;
static short loads;
static char const *setting = "", *ride_setting = "longsword";
static char const *config_string(char const *name)
{
	return !strcmp(name, "debug.network_test") ? setting : !strcmp(name, "debug.network_test_ride") ? ride_setting : "";
}
static int config_boolean(char const *name) { (void)name; return 0; }
static double config_real(char const *name) { (void)name; return 0; }
static long config_integer(char const *name) { (void)name; return 0; }
static void platform_log(char const *format, ...) { (void)format; }
static void ride_settings(void) { }
static short game_connection(void) { return connection; }
static void game_connection_set(short value) { connection = value; }
static void ui_widgets_close_all(void) { closed = TRUE; }
static void player_ui_set_single_player_local_player_controller(short local, short controller) { controllers[local] = controller; }
static void game_set_game_variant_from_name(char const *name) { strcpy(selected_variant, name); }
static void main_set_map_name(char const *name) { strcpy(selected_map, name); }
static void main_menu_switch_to_single_player(void) { deferred = TRUE; loads++; }
static void ride_error(char const *reason) { (void)reason; failed = TRUE; }
#include "local_under_test.inc"

int main(int argc, char **argv)
{
	char const *case_name = argc > 1 ? argv[1] : "";
	ride.enabled = TRUE;
	ride.scenario = _ride_longsword;
	network_test.mode = _network_test_local;
	strcpy(network_test.map_name, "custom_maps\\extinction");
	CASE("local-start")
	{
		local_frame();
		local_frame();
		CHECK(connection == _game_connection_local, "network connection started");
		CHECK(player_spawn_count == 2, "second local player missing");
		CHECK(controllers[0] == 0 && controllers[1] == 1, "controllers shared");
		CHECK(!strcmp(selected_map, "custom_maps\\extinction"), "wrong map");
		CHECK(!strcmp(selected_variant, "slayer"), "multiplayer engine missing");
		CHECK(deferred && closed && !failed, "map load not requested cleanly");
		CHECK(loads == 1, "local frame reloaded the map");
		return 0;
	}
	CASE("local-parse")
	{
		setting = "local:custom_maps\\extinction:slayer";
		network_test_read_settings();
		CHECK(network_test.checked && network_test.mode == _network_test_local, "local selector not parsed");
		CHECK(!strcmp(network_test.map_name, "custom_maps\\extinction"), "map prefix or variant left in path");
		CHECK(!strcmp(network_test.variant_name, "slayer"), "variant lost");
		/* (local: without the Longsword test is the split screen game of the host's way) */
		ride_setting = "";
		network_test.local = FALSE;
		network_test_read_settings();
		CHECK(network_test.mode == _network_test_host && network_test.local, "plain local: not a split screen game");
		ride_setting = "longsword";
		setting = "host:bloodgulch:ctf";
		network_test_read_settings();
		CHECK(network_test.mode == _network_test_host && !network_test.local && !strcmp(network_test.map_name, "bloodgulch") &&
			!strcmp(network_test.variant_name, "ctf"), "networked parsing changed");
		return 0;
	}
	CASE("local-side")
	{
		connection = _game_connection_local;
		CHECK(ride_host(), "local logs labelled client");
		network_test.mode = _network_test_host;
		CHECK(!ride_host(), "ordinary local play treated as test host");
		connection = _game_connection_network_server;
		CHECK(ride_host(), "networked host changed");
		connection = _game_connection_network_client;
		CHECK(!ride_host(), "networked client changed");
		return 0;
	}
	CASE("local-only-longsword")
	{
		ride.scenario = _ride_straight;
		network_test_start_local();
		CHECK(failed && !deferred, "local stock scenario allowed");
		failed = FALSE;
		ride.scenario = _ride_longsword;
		ride.enabled = FALSE;
		network_test_start_local();
		CHECK(failed && !deferred, "local mode without riding script allowed");
		return 0;
	}
	fprintf(stderr, "unknown case: %s\n", case_name);
	return 2;
}

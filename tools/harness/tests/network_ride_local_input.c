/* local action dequeue's real caller, with synthetic queued actions */
#include "harness.h"
#include <string.h>
typedef struct { real i, j; } real_vector2d;
typedef struct { real yaw, pitch; } real_euler_angles2d;
#define MAXIMUM_LOCAL_PLAYERS 2
#define MAXIMUM_NUMBER_OF_PLAYERS 2
#define MAXIMUM_WEAPONS_PER_UNIT 4
#define NUMBER_OF_UNIT_GRENADE_TYPES 2
#define LATCHED_CONTROL_FLAGS 1
#define _game_connection_local 0
#define _game_connection_network_client 1
#define _game_connection_network_server 2
#include "local_input_types.inc"
struct update { struct { short action_count; struct player_action actions[2]; } update; };
static struct update update;
static struct update_client_queue_datum queues[2];
static struct { void *data; short count; } data;
static struct { boolean initialized; long next_update_number_to_dequeue, latest_update_number_received;
	void *unused; typeof(data) *queues; } update_client_globals;
static short connection;
static long local_players[2] = { 1, 0 };
static short hooks;
static short game_connection(void) { return connection; }
static int halo_server_mode(void) { return 0; }
static long local_player_get_player_index(short local) { return local_players[local]; }
static struct update *update_client_get_update(long tick) { (void)tick; return &update; }
static boolean update_client_dequeue_distributed(struct player_action *actions) { (void)actions; return FALSE; }
static void network_test_ride_action(short local, struct player_action *action)
{
	hooks++;
	action->throttle.i = local + 1;
	action->control_flags = 1;
}
#include "local_input_under_test.inc"
int main(int argc, char **argv)
{
	struct player_action actions[2];
	short index;
	(void)argc;
	(void)argv;
	data.data = queues;
	data.count = 2;
	update_client_globals.initialized = TRUE;
	update_client_globals.queues = &data;
	update_client_globals.latest_update_number_received = 100;
	update.update.action_count = 2;
	for (index = 0; index < 2; index++)
	{
		update.update.actions[index].desired_weapon_index = NONE;
		update.update.actions[index].desired_grenade_index = NONE;
		update.update.actions[index].desired_zoom_level = NONE;
	}
	CHECK(update_client_dequeue(actions) && hooks == 2, "local dequeue never called script for both players");
	CHECK(actions[0].throttle.i == 2.f && actions[1].throttle.i == 1.f, "script not routed by local player index");
	CHECK(actions[0].control_flags == 1 && actions[1].control_flags == 1, "script input lost before dequeue");
	CHECK(update_client_dequeue(actions) && actions[0].control_flags == 0, "script bypassed native button latching");
	connection = _game_connection_network_server;
	hooks = 0;
	CHECK(update_client_dequeue(actions) && hooks == 0, "networked input path changed");
	connection = _game_connection_local;
	local_players[1] = NONE;
	hooks = 0;
	CHECK(update_client_dequeue(actions) && hooks == 1, "script called for nonexistent local player");
	return 0;
}

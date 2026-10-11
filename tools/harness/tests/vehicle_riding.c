/*
VEHICLE_RIDING.C (test)

vehicle_riding.c compiled whole against fake settings, connection and look
impulses, for a player standing on a moving vehicle.
*/

#include "harness.h"
#include <math.h>
#include "enums.inc"

typedef struct { real yaw, pitch; } real_euler_angles2d;
#define TICKS_PER_SECOND 30
#define PI 3.14159265f

static int setting_riding = 1, setting_look_carry = 1;
static double setting_damage_speed, setting_teleport_step = 4.0, setting_moved_alone = 0.25;
int config_boolean(const char *name)
{
	if (!strcmp(name, "game.vehicle_riding")) return setting_riding;
	if (!strcmp(name, "debug.riding_look_carry")) return setting_look_carry;
	return 0;
}
double config_real(const char *name)
{
	if (!strcmp(name, "debug.riding_damage_speed")) return setting_damage_speed;
	if (!strcmp(name, "debug.riding_teleport_step")) return setting_teleport_step;
	if (!strcmp(name, "debug.riding_moved_alone")) return setting_moved_alone;
	return 0.0;
}
static short connection = _game_connection_local;
static boolean in_editor;
short game_connection(void) { return connection; }
boolean game_in_editor(void) { return in_editor; }
static real impulses[MAXIMUM_LOCAL_PLAYERS];
void player_control_permanent_impulse(short local_player_index, real_euler_angles2d const *delta)
{
	impulses[local_player_index] += delta->yaw;
}
#include "under_test.inc"

#define NEAR(a, b) CHECK(fabs((double)(a) - (double)(b)) < .0001, "%s: %g, not %g", #a, (double)(a), (double)(b))
#define VEHICLE 0x10000001
static real_vector3d const up = { 0.f, 0.f, 1.f };
static real_point3d point(real x, real y, real z)
{
	real_point3d result;
	result.x = x; result.y = y; result.z = z;
	return result;
}
static real_vector3d vector(real i, real j, real k)
{
	real_vector3d result;
	result.i = i; result.j = j; result.k = k;
	return result;
}
static struct vehicle_riding_vehicle vehicle_at(long object_index, real x, real y, real z, real yaw, long updates)
{
	struct vehicle_riding_vehicle vehicle;
	memset(&vehicle, 0, sizeof(vehicle));
	vehicle.object_index = object_index; vehicle.node_index = NONE;
	vehicle.position = point(x, y, z); vehicle.origin = vehicle.position;
	vehicle.forward = vector((real)cos(yaw), (real)sin(yaw), 0.f);
	vehicle.up = up; vehicle.updates = updates;
	vehicle.bounding_sphere_center = vehicle.position; vehicle.bounding_sphere_radius = 3.f;
	return vehicle;
}
static struct vehicle_riding_state riding_on(struct vehicle_riding_vehicle const *vehicle,
	real_point3d base, real_vector3d velocity)
{
	struct vehicle_riding_state state;
	vehicle_riding_clear(&state);
	state.ticks = RIDING_TICKS; state.object_index = vehicle->object_index;
	state.vehicle_position = vehicle->position; state.vehicle_forward = vehicle->forward;
	state.vehicle_up = vehicle->up; state.vehicle_updates = vehicle->updates;
	state.velocity = velocity; state.node_index = vehicle->node_index;
	vehicle_riding_point_to_local(&vehicle->position, &vehicle->forward, &vehicle->up, &base, &state.local_point);
	return state;
}
static real_vector3d pitched(real_vector3d v, real angle)
{
	return vector(v.i*(real)cos(angle) + v.k*(real)sin(angle), v.j,
		-v.i*(real)sin(angle) + v.k*(real)cos(angle));
}

int main(int argc, char **argv)
{
	char const *case_name = argc > 1 ? argv[1] : "";
	struct vehicle_riding_step step;
	vehicle_riding_latch();
	CASE("carry")
	{
		struct vehicle_riding_vehicle before = vehicle_at(VEHICLE, 0.f, 0.f, 0.f, 0.f, 10);
		struct vehicle_riding_vehicle after = vehicle_at(VEHICLE, 5.f, 0.f, 0.f, PI/2.f, 11);
		struct vehicle_riding_state state = riding_on(&before, point(2.f, 0.f, 0.f), vector(0.f, 0.f, 0.f));
		real_point3d base = point(2.f, 0.f, 0.f);
		CHECK(vehicle_riding_snapshot_step(&state, &after, &base, 1.4f, .2f, FALSE, &step), "dropped");
		NEAR(base.x + step.displacement.i, 5.f); NEAR(base.y + step.displacement.j, 2.f);
		NEAR(base.z + step.displacement.k, 0.f); NEAR(step.dyaw, PI/2.f); return 0;
	}
	CASE("dyaw-vertical")
	{
		real_vector3d forward = vector(.0995037f, 0.f, .9950372f), upward = vector(-.9950372f, 0.f, .0995037f);
		real_vector3d yawed_forward = forward, yawed_up = upward;
		real_vector3d tilted_forward = pitched(forward, .1f), tilted_up = pitched(upward, .1f);
		vehicle_riding_turn_vector(&yawed_forward, .1f); vehicle_riding_turn_vector(&yawed_up, .1f);
		NEAR(vehicle_riding_yaw(&forward, &upward, &yawed_forward, &yawed_up), .1f);
		NEAR(vehicle_riding_yaw(&forward, &upward, &tilted_forward, &tilted_up), 0.f); return 0;
	}
	CASE("node-carry")
	{
		struct vehicle_riding_vehicle before = vehicle_at(VEHICLE, 3.f, 0.f, 1.f, 0.f, 10);
		struct vehicle_riding_vehicle after = vehicle_at(VEHICLE, 3.f, 0.f, 1.f, .2f, 11);
		struct vehicle_riding_state state;
		real_point3d base = point(5.f, 0.f, 1.f);
		before.node_index = after.node_index = 2; before.origin = after.origin = point(0.f, 0.f, 0.f);
		state = riding_on(&before, base, vector(0.f, 0.f, 0.f));
		CHECK(vehicle_riding_snapshot_step(&state, &after, &base, 1.4f, .2f, FALSE, &step), "dropped");
		NEAR(base.x + step.displacement.i, 3.f + 2.f*(real)cos(.2f));
		NEAR(base.y + step.displacement.j, 2.f*(real)sin(.2f)); NEAR(step.dyaw, .2f); return 0;
	}
	CASE("stuck")
	{
		struct vehicle_riding_vehicle before = vehicle_at(VEHICLE, 0.f, 0.f, 0.f, 0.f, 10), after;
		struct vehicle_riding_state state = riding_on(&before, point(1.f, 0.f, 0.f), vector(0.f, 0.f, 0.f));
		real_point3d base = point(1.f, 0.f, 0.f);
		short tick;
		after = before; after.translational_velocity = vector(.3f, 0.f, 0.f);
		for (tick = 0; tick < 30; tick++)
		{
			after.updates++;
			CHECK(vehicle_riding_snapshot_step(&state, &after, &base, 1.4f, .2f, FALSE, &step), "dropped");
			NEAR(step.displacement.i, 0.f);
			NEAR(step.velocity_now.i, 0.f);
			state.vehicle_updates = after.updates;
		}
		return 0;
	}
	CASE("contact-point")
	{
		struct vehicle_riding_vehicle before = vehicle_at(VEHICLE, 0.f, 0.f, 0.f, 0.f, 10);
		struct vehicle_riding_vehicle after = vehicle_at(VEHICLE, 0.f, 0.f, 0.f, .1f, 11);
		struct vehicle_riding_state state = riding_on(&before, point(2.f, 0.f, 0.f), vector(0.f, 0.f, 0.f));
		real_point3d base = point(2.2f, 0.f, 0.f);
		CHECK(vehicle_riding_snapshot_step(&state, &after, &base, 1.4f, .2f, FALSE, &step), "dropped");
		NEAR(step.displacement.i, 2.f*(real)cos(.1f) - 2.f); NEAR(step.displacement.j, 2.f*(real)sin(.1f)); return 0;
	}
	CASE("guards")
	{
		struct vehicle_riding_vehicle before = vehicle_at(VEHICLE, 0.f, 0.f, 0.f, 0.f, 10);
		struct vehicle_riding_vehicle after;
		struct vehicle_riding_state state;
		real_point3d base;
		state = riding_on(&before, point(1.f, 0.f, 0.f), vector(0.f, 0.f, 0.f));
		after = vehicle_at(VEHICLE, 0.f, 0.f, 0.f, 0.f, 11);
		base = point(4.f, 0.f, 0.f);
		CHECK(!vehicle_riding_snapshot_step(&state, &after, &base, 1.4f, .2f, FALSE, &step), "moved alone kept");
		CHECK(state.object_index == NONE && state.ticks == 0, "not dropped");
		state = riding_on(&before, point(1.f, 0.f, 0.f), vector(0.f, 0.f, 0.f));
		base = point(1.1f, 0.f, 0.f);
		CHECK(vehicle_riding_snapshot_step(&state, &after, &base, 1.4f, .2f, FALSE, &step), "0.1 dropped");
		base = point(1.f, 0.f, 0.f);
		state = riding_on(&before, base, vector(0.f, 0.f, 0.f));
		after = vehicle_at(VEHICLE, 5.f, 0.f, 0.f, 0.f, 11);
		CHECK(!vehicle_riding_snapshot_step(&state, &after, &base, 1.4f, .2f, FALSE, &step), "5 in one kept");
		state = riding_on(&before, base, vector(0.f, 0.f, 0.f));
		after = vehicle_at(VEHICLE, 1.f, 0.f, 0.f, 0.f, 11);
		CHECK(vehicle_riding_snapshot_step(&state, &after, &base, 1.4f, .2f, FALSE, &step), "1 in one dropped");
		state = riding_on(&before, base, vector(0.f, 0.f, 0.f));
		after = vehicle_at(VEHICLE, 6.f, 0.f, 0.f, 0.f, 12);
		CHECK(vehicle_riding_snapshot_step(&state, &after, &base, 1.4f, .2f, FALSE, &step), "6 in two dropped");
		state = riding_on(&before, base, vector(0.f, 0.f, 0.f));
		after = vehicle_at(VEHICLE, .01f, 0.f, 0.f, 0.f, 10);
		CHECK(!vehicle_riding_snapshot_step(&state, &after, &base, 1.4f, .2f, FALSE, &step), "unupdated move kept");
		state = riding_on(&before, base, vector(0.f, 0.f, -.3f));
		state.ticks = 30; state.falling = 1;
		CHECK(!vehicle_riding_snapshot_step(&state, NULL, &base, 1.4f, .2f, FALSE, &step), "gone kept");
		CHECK(state.object_index == NONE && state.falling == 1, "falling frame lost");
		NEAR(state.velocity.k, -.3f);
		state = riding_on(&before, base, vector(0.f, 0.f, 0.f));
		state.ticks = 30;
		after = vehicle_at(VEHICLE, 0.f, 0.f, 0.f, 0.f, 11);
		base = point(5.f, 0.f, 0.f);
		CHECK(!vehicle_riding_snapshot_step(&state, &after, &base, 1.4f, .2f, FALSE, &step), "far kept");
		return 0;
	}
	CASE("gate")
	{
		CHECK(vehicle_riding_enabled(), "off"); CHECK(vehicle_riding_applies(TRUE, FALSE), "living player does not ride");
		CHECK(!vehicle_riding_applies(FALSE, FALSE), "AI rides"); CHECK(!vehicle_riding_applies(TRUE, TRUE), "dead rides");
		connection = _game_connection_network_client; CHECK(!vehicle_riding_enabled(), "on in network game");
		connection = _game_connection_local; in_editor = TRUE; CHECK(!vehicle_riding_enabled(), "on in editor");
		in_editor = FALSE; setting_riding = 0; vehicle_riding_latch(); CHECK(!vehicle_riding_enabled(), "on with setting off"); return 0;
	}
	CASE("deferred")
	{
		struct vehicle_riding_vehicle vehicle = vehicle_at(VEHICLE, 0.f, 0.f, 0.f, 0.f, 10);
		struct vehicle_riding_state state = riding_on(&vehicle, point(0.f, 0.f, 0.f), vector(0.f, 0.f, 0.f));
		CHECK(vehicle_riding_deferred(&state, TRUE), "a rider not deferred");
		state.ticks = 1;
		CHECK(vehicle_riding_deferred(&state, TRUE), "a biped in the window not deferred");
		CHECK(!vehicle_riding_deferred(&state, FALSE), "deferred where riding does not apply");
		state.ticks = 0;
		CHECK(!vehicle_riding_deferred(&state, TRUE), "deferred outside any window");
		return 0;
	}
	CASE("look-carry")
	{
		NEAR(vehicle_riding_look(0, TRUE, .2f), .2f); NEAR(impulses[0], .2f);
		NEAR(vehicle_riding_kept_dyaw(0), .2f); NEAR(vehicle_riding_look(0, FALSE, .2f), 0.f);
		NEAR(vehicle_riding_kept_dyaw(0), 0.f); setting_look_carry = 0; vehicle_riding_latch();
		NEAR(vehicle_riding_look(0, TRUE, .2f), 0.f); NEAR(impulses[0], .2f); return 0;
	}
	CASE("two-local-players")
	{
		vehicle_riding_look(0, TRUE, .2f); vehicle_riding_look(1, TRUE, -.1f);
		NEAR(impulses[0], .2f); NEAR(impulses[1], -.1f);
		NEAR(vehicle_riding_kept_dyaw(0), .2f); NEAR(vehicle_riding_kept_dyaw(1), -.1f);
		vehicle_riding_look(1, FALSE, 0.f); NEAR(vehicle_riding_kept_dyaw(0), .2f);
		NEAR(vehicle_riding_kept_dyaw(1), 0.f); return 0;
	}
	CASE("latch")
	{
		vehicle_riding_look(1, TRUE, .2f); setting_riding = 0; CHECK(!vehicle_riding_latch(), "on");
		NEAR(vehicle_riding_kept_dyaw(1), 0.f); setting_riding = 1; CHECK(vehicle_riding_latch(), "off");
		return 0;
	}
	fprintf(stderr, "unknown case: %s\n", case_name); return 2;
}

/*
VEHICLE_RIDING.C (test)

vehicle_riding.c compiled whole: the carry, the frames, the guards and the
collision decision of a player standing on a moving vehicle, against fake
settings, connection and look impulses. test_vehicle_riding.py takes the
code (under_test.inc) and the enums (enums.inc) from the sources.
*/

#include "harness.h"
#include <math.h>
#include "enums.inc"

typedef struct { real yaw, pitch; } real_euler_angles2d;
#define TICKS_PER_SECOND 30
#define PI 3.14159265f

/* the settings (port_config.c) */
static int setting_riding = 1, setting_look_carry = 1;
static double setting_damage_speed, setting_teleport_step = 4.0, setting_moved_alone = 0.25;

int config_boolean(const char *name)
{
	if (!strcmp(name, "game.vehicle_riding"))
		return setting_riding;
	if (!strcmp(name, "debug.riding_look_carry"))
		return setting_look_carry;
	return 0;
}

double config_real(const char *name)
{
	if (!strcmp(name, "debug.riding_damage_speed"))
		return setting_damage_speed;
	if (!strcmp(name, "debug.riding_teleport_step"))
		return setting_teleport_step;
	if (!strcmp(name, "debug.riding_moved_alone"))
		return setting_moved_alone;
	return 0.0;
}

static short connection = _game_connection_local;
static boolean in_editor;

short game_connection(void)
{
	return connection;
}

boolean game_in_editor(void)
{
	return in_editor;
}

/* the look impulses each local player was given */
static real impulses[MAXIMUM_LOCAL_PLAYERS];

void player_control_permanent_impulse(short local_player_index, real_euler_angles2d const *delta)
{
	impulses[local_player_index] += delta->yaw;
}

#include "under_test.inc"

#define NEAR(a, b) CHECK(fabs((double)(a) - (double)(b)) < .0001, "%s: %g, not %g", #a, (double)(a), (double)(b))
#define VEHICLE 0x10000001
#define OTHER_VEHICLE 0x10010002

static real_vector3d const zero = { 0.f, 0.f, 0.f };
static real_vector3d const up = { 0.f, 0.f, 1.f };

static real_point3d point(real x, real y, real z)
{
	real_point3d result;

	result.x = x;
	result.y = y;
	result.z = z;
	return result;
}

static real_vector3d vector(real i, real j, real k)
{
	real_vector3d result;

	result.i = i;
	result.j = j;
	result.k = k;
	return result;
}

/* a vehicle at (x, y, z), turned by yaw about up, after this many updates */
static struct vehicle_riding_vehicle vehicle_at(long object_index, real x, real y, real z, real yaw, long updates)
{
	struct vehicle_riding_vehicle vehicle;

	memset(&vehicle, 0, sizeof(vehicle));
	vehicle.object_index = object_index;
	vehicle.node_index = NONE;
	vehicle.position = point(x, y, z);
	vehicle.origin = vehicle.position;
	vehicle.forward = vector((real)cos(yaw), (real)sin(yaw), 0.f);
	vehicle.up = up;
	vehicle.updates = updates;
	vehicle.bounding_sphere_center = vehicle.position;
	vehicle.bounding_sphere_radius = 3.f;
	return vehicle;
}

/* a biped that ended its last tick standing at base on the vehicle */
static struct vehicle_riding_state riding_on(struct vehicle_riding_vehicle const *vehicle, real_point3d base,
	real_vector3d velocity)
{
	struct vehicle_riding_state state;

	vehicle_riding_clear(&state);
	state.ticks = RIDING_TICKS;
	state.object_index = vehicle->object_index;
	state.vehicle_position = vehicle->position;
	state.vehicle_forward = vehicle->forward;
	state.vehicle_up = vehicle->up;
	state.vehicle_updates = vehicle->updates;
	state.velocity = velocity;
	state.node_index = vehicle->node_index;
	vehicle_riding_point_to_local(&vehicle->position, &vehicle->forward, &vehicle->up, &base, &state.local_point);
	return state;
}

/* a vector turned about the vehicle's left axis (j) by angle: a pitch */
static real_vector3d pitched(real_vector3d v, real angle)
{
	return vector(v.i*(real)cos(angle) + v.k*(real)sin(angle), v.j, -v.i*(real)sin(angle) + v.k*(real)cos(angle));
}

/* the world velocity after a tick in which a biped riding at velocity relative
to its deck (no friction, no gravity) was carried and recorded on support */
static real_vector3d riding_tick(struct vehicle_riding_state *state, struct vehicle_riding_vehicle const *vehicle,
	real_point3d *base, real_vector3d world_velocity, struct vehicle_riding_vehicle const *support, boolean supported,
	real_vector3d support_velocity)
{
	struct vehicle_riding_step tick_step;
	real_vector3d velocity, frame;
	boolean valid = state->ticks > 0 &&
		vehicle_riding_snapshot_step(state, vehicle, base, 1.4f, .2f, FALSE, &tick_step);

	if (valid && vehicle_riding_is_riding(state))
	{
		base->x += tick_step.displacement.i;
		base->y += tick_step.displacement.j;
		base->z += tick_step.displacement.k;
	}
	vehicle_riding_seed(state, &world_velocity, &velocity, &frame);
	base->x += velocity.i;
	base->y += velocity.j;
	base->z += velocity.k;
	vehicle_riding_record(state, valid ? &tick_step : NULL, support, supported, &support_velocity, &up, base, FALSE,
		&velocity);
	return velocity;
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
		struct vehicle_riding_state state = riding_on(&before, point(2.f, 0.f, 0.f), zero);
		real_point3d base = point(2.f, 0.f, 0.f);

		CHECK(vehicle_riding_snapshot_step(&state, &after, &base, 1.4f, .2f, FALSE, &step), "dropped");
		NEAR(base.x + step.displacement.i, 5.f);
		NEAR(base.y + step.displacement.j, 2.f);
		NEAR(base.z + step.displacement.k, 0.f);
		NEAR(step.dyaw, PI/2.f);
		return 0;
	}
	CASE("dyaw-vertical")
	{
		/* forward nearly straight up: a yaw of 0.1 is still 0.1, a pitch alone none */
		real_vector3d forward = vector(.0995037f, 0.f, .9950372f);
		real_vector3d upward = vector(-.9950372f, 0.f, .0995037f);
		real_vector3d yawed_forward = forward, yawed_up = upward;
		real_vector3d tilted_forward = pitched(forward, .1f), tilted_up = pitched(upward, .1f);

		vehicle_riding_turn_vector(&yawed_forward, .1f);
		vehicle_riding_turn_vector(&yawed_up, .1f);
		NEAR(vehicle_riding_yaw(&forward, &upward, &yawed_forward, &yawed_up), .1f);
		NEAR(vehicle_riding_yaw(&forward, &upward, &tilted_forward, &tilted_up), 0.f);
		return 0;
	}
	CASE("node-carry")
	{
		/* the vehicle still, its node (a turret) at (3, 0, 1) turning 0.2 about up */
		struct vehicle_riding_vehicle before = vehicle_at(VEHICLE, 3.f, 0.f, 1.f, 0.f, 10);
		struct vehicle_riding_vehicle after = vehicle_at(VEHICLE, 3.f, 0.f, 1.f, .2f, 11);
		struct vehicle_riding_state state;
		real_point3d base = point(5.f, 0.f, 1.f);

		before.node_index = after.node_index = 2;
		before.origin = after.origin = point(0.f, 0.f, 0.f);
		state = riding_on(&before, base, zero);
		CHECK(vehicle_riding_snapshot_step(&state, &after, &base, 1.4f, .2f, FALSE, &step), "dropped");
		NEAR(base.x + step.displacement.i, 3.f + 2.f*(real)cos(.2f));
		NEAR(base.y + step.displacement.j, 2.f*(real)sin(.2f));
		NEAR(step.dyaw, .2f);
		return 0;
	}
	CASE("stuck")
	{
		/* a vehicle that reports a velocity but did not move, update after update */
		struct vehicle_riding_vehicle before = vehicle_at(VEHICLE, 0.f, 0.f, 0.f, 0.f, 10);
		struct vehicle_riding_vehicle after = before;
		struct vehicle_riding_state state = riding_on(&before, point(1.f, 0.f, 0.f), zero);
		real_point3d base = point(1.f, 0.f, 0.f);
		short tick;

		after.translational_velocity = vector(.3f, 0.f, 0.f);
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
		/* the base 0.2 from where the biped ended its tick: the carry is the contact point's */
		struct vehicle_riding_vehicle before = vehicle_at(VEHICLE, 0.f, 0.f, 0.f, 0.f, 10);
		struct vehicle_riding_vehicle after = vehicle_at(VEHICLE, 0.f, 0.f, 0.f, .1f, 11);
		struct vehicle_riding_state state = riding_on(&before, point(2.f, 0.f, 0.f), zero);
		real_point3d base = point(2.2f, 0.f, 0.f);

		CHECK(vehicle_riding_snapshot_step(&state, &after, &base, 1.4f, .2f, FALSE, &step), "dropped");
		NEAR(step.displacement.i, 2.f*(real)cos(.1f) - 2.f);
		NEAR(step.displacement.j, 2.f*(real)sin(.1f));
		return 0;
	}
	CASE("guards")
	{
		struct vehicle_riding_vehicle before = vehicle_at(VEHICLE, 0.f, 0.f, 0.f, 0.f, 10);
		struct vehicle_riding_vehicle after;
		struct vehicle_riding_state state;
		real_point3d base;

		/* moved alone 3 from where it ended its tick: dropped; 0.1: kept */
		state = riding_on(&before, point(1.f, 0.f, 0.f), zero);
		after = vehicle_at(VEHICLE, 0.f, 0.f, 0.f, 0.f, 11);
		base = point(4.f, 0.f, 0.f);
		CHECK(!vehicle_riding_snapshot_step(&state, &after, &base, 1.4f, .2f, FALSE, &step), "moved alone kept");
		CHECK(state.object_index == NONE && state.ticks == 0, "not dropped");
		state = riding_on(&before, point(1.f, 0.f, 0.f), zero);
		base = point(1.1f, 0.f, 0.f);
		CHECK(vehicle_riding_snapshot_step(&state, &after, &base, 1.4f, .2f, FALSE, &step), "0.1 dropped");
		/* a deck step of 5 in one update: dropped; 1: kept; 6 over two updates: kept */
		base = point(1.f, 0.f, 0.f);
		state = riding_on(&before, base, zero);
		after = vehicle_at(VEHICLE, 5.f, 0.f, 0.f, 0.f, 11);
		CHECK(!vehicle_riding_snapshot_step(&state, &after, &base, 1.4f, .2f, FALSE, &step), "5 in one kept");
		state = riding_on(&before, base, zero);
		after = vehicle_at(VEHICLE, 1.f, 0.f, 0.f, 0.f, 11);
		CHECK(vehicle_riding_snapshot_step(&state, &after, &base, 1.4f, .2f, FALSE, &step), "1 in one dropped");
		state = riding_on(&before, base, zero);
		after = vehicle_at(VEHICLE, 6.f, 0.f, 0.f, 0.f, 12);
		CHECK(vehicle_riding_snapshot_step(&state, &after, &base, 1.4f, .2f, FALSE, &step), "6 in two dropped");
		/* moved without an update: dropped */
		state = riding_on(&before, base, zero);
		after = vehicle_at(VEHICLE, .01f, 0.f, 0.f, 0.f, 10);
		CHECK(!vehicle_riding_snapshot_step(&state, &after, &base, 1.4f, .2f, FALSE, &step), "unupdated move kept");
		/* gone while the biped falls from it: dropped, the falling frame kept */
		state = riding_on(&before, base, vector(0.f, 0.f, -.3f));
		state.ticks = 30;
		state.falling = 1;
		CHECK(!vehicle_riding_snapshot_step(&state, NULL, &base, 1.4f, .2f, FALSE, &step), "gone kept");
		CHECK(state.object_index == NONE && state.falling == 1, "falling frame lost");
		NEAR(state.velocity.k, -.3f);
		/* in the window, not riding, outside the widened sphere: dropped */
		state = riding_on(&before, base, zero);
		state.ticks = 30;
		after = vehicle_at(VEHICLE, 0.f, 0.f, 0.f, 0.f, 11);
		base = point(5.f, 0.f, 0.f);
		CHECK(!vehicle_riding_snapshot_step(&state, &after, &base, 1.4f, .2f, FALSE, &step), "far kept");
		return 0;
	}
	CASE("gate")
	{
		CHECK(vehicle_riding_enabled(), "off");
		CHECK(vehicle_riding_applies(TRUE, FALSE), "a living player does not ride");
		CHECK(!vehicle_riding_applies(FALSE, FALSE), "an AI rides");
		CHECK(!vehicle_riding_applies(TRUE, TRUE), "a dead player rides");
		/* the connection read on every call, not at the latch */
		connection = _game_connection_network_client;
		CHECK(!vehicle_riding_enabled(), "on in a network game");
		connection = _game_connection_local;
		in_editor = TRUE;
		CHECK(!vehicle_riding_enabled(), "on in the editor");
		in_editor = FALSE;
		setting_riding = 0;
		vehicle_riding_latch();
		CHECK(!vehicle_riding_enabled(), "on with the setting off");
		return 0;
	}
	CASE("deferred")
	{
		struct vehicle_riding_vehicle vehicle = vehicle_at(VEHICLE, 0.f, 0.f, 0.f, 0.f, 10);
		struct vehicle_riding_state state = riding_on(&vehicle, point(0.f, 0.f, 0.f), zero);

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
		NEAR(vehicle_riding_look(0, TRUE, .2f), .2f);
		NEAR(impulses[0], .2f);
		NEAR(vehicle_riding_kept_dyaw(0), .2f);
		/* the next tick off the deck */
		NEAR(vehicle_riding_look(0, FALSE, .2f), 0.f);
		NEAR(vehicle_riding_kept_dyaw(0), 0.f);
		NEAR(impulses[0], .2f);
		/* the setting off: no turn (the position is carried all the same, by the step) */
		setting_look_carry = 0;
		vehicle_riding_latch();
		NEAR(vehicle_riding_look(0, TRUE, .2f), 0.f);
		NEAR(impulses[0], .2f);
		return 0;
	}
	CASE("two-local-players")
	{
		vehicle_riding_look(0, TRUE, .2f);
		vehicle_riding_look(1, TRUE, -.1f);
		NEAR(impulses[0], .2f);
		NEAR(impulses[1], -.1f);
		NEAR(vehicle_riding_kept_dyaw(0), .2f);
		NEAR(vehicle_riding_kept_dyaw(1), -.1f);
		vehicle_riding_look(1, FALSE, 0.f);
		NEAR(vehicle_riding_kept_dyaw(0), .2f);
		NEAR(vehicle_riding_kept_dyaw(1), 0.f);
		return 0;
	}
	CASE("latch")
	{
		vehicle_riding_look(1, TRUE, .2f);
		setting_riding = 0;
		CHECK(!vehicle_riding_latch(), "on");
		NEAR(vehicle_riding_kept_dyaw(1), 0.f);
		setting_riding = 1;
		CHECK(vehicle_riding_latch(), "off");
		return 0;
	}
	CASE("momentum")
	{
		struct vehicle_riding_vehicle vehicle = vehicle_at(VEHICLE, 0.f, 0.f, 0.f, 0.f, 10);
		struct vehicle_riding_state state = riding_on(&vehicle, point(0.f, 0.f, 0.f), vector(.1f, 0.f, 0.f));
		real_point3d base = point(0.f, 0.f, 0.f);
		real_vector3d world;

		/* the tick it steps off: its own velocity plus the deck's */
		vehicle = vehicle_at(VEHICLE, .1f, 0.f, 0.f, 0.f, 11);
		world = riding_tick(&state, &vehicle, &base, vector(.15f, 0.f, 0.f), NULL, FALSE, zero);
		NEAR(world.i, .15f);
		CHECK(state.falling == 1 && state.ticks == RIDING_TICKS - 1, "not left");
		/* the next tick: no carry, the world velocity alone */
		vehicle = vehicle_at(VEHICLE, .2f, 0.f, 0.f, 0.f, 12);
		{
			real start = base.x;

			riding_tick(&state, &vehicle, &base, world, NULL, FALSE, zero);
			NEAR(base.x - start, .15f);
		}
		return 0;
	}
	CASE("no-double-count")
	{
		struct vehicle_riding_vehicle vehicle = vehicle_at(VEHICLE, 0.f, 0.f, 0.f, 0.f, 10);
		struct vehicle_riding_state state = riding_on(&vehicle, point(0.f, 0.f, 0.f), vector(.1f, 0.f, 0.f));
		real_point3d base = point(0.f, 0.f, 0.f);
		real_vector3d world = vector(.1f, 0.f, 0.f);
		short tick;

		for (tick = 1; tick <= 10; tick++)
		{
			vehicle = vehicle_at(VEHICLE, .1f*tick, 0.f, 0.f, 0.f, 10 + tick);
			world = riding_tick(&state, &vehicle, &base, world, &vehicle, TRUE, zero);
		}
		NEAR(base.x, 1.f);
		NEAR(world.i, .1f);
		return 0;
	}
	CASE("spans")
	{
		/* landed before the vehicle's update in datum order: the next carry spans two */
		struct vehicle_riding_vehicle vehicle = vehicle_at(VEHICLE, 0.f, 0.f, 0.f, 0.f, 10);
		struct vehicle_riding_state state;
		real_point3d base = point(0.f, 0.f, 0.f);
		real_vector3d velocity = vector(.1f, 0.f, 0.f);
		real_vector3d deck = vector(.1f, 0.f, 0.f);

		vehicle_riding_clear(&state);
		vehicle.translational_velocity = deck;
		CHECK(vehicle_riding_record(&state, NULL, &vehicle, TRUE, &deck, &up, &base, FALSE, &velocity), "no elevator clear");
		vehicle = vehicle_at(VEHICLE, .2f, 0.f, 0.f, 0.f, 12);
		CHECK(vehicle_riding_snapshot_step(&state, &vehicle, &base, 1.4f, .2f, FALSE, &step), "dropped");
		NEAR(step.velocity_now.i, .1f);
		/* leaving then: the vehicle's speed, not twice it */
		velocity = zero;
		vehicle_riding_record(&state, &step, NULL, FALSE, &zero, &up, &base, FALSE, &velocity);
		NEAR(velocity.i, .1f);
		return 0;
	}
	CASE("double-speed")
	{
		/* a vehicle without a player, 0.2 an update, an update every other tick */
		struct vehicle_riding_vehicle vehicle = vehicle_at(VEHICLE, 0.f, 0.f, 0.f, 0.f, 10);
		struct vehicle_riding_state state;
		real_point3d base = point(0.f, 0.f, 0.f);
		real_vector3d velocity;
		real_vector3d deck;

		/* first landing: the deck's motion per player tick */
		vehicle.translational_velocity = vector(.2f, 0.f, 0.f);
		vehicle_riding_deck_point_velocity(&vehicle, &base, TRUE, &deck);
		NEAR(deck.i, .1f);
		vehicle_riding_clear(&state);
		velocity = deck;
		vehicle_riding_record(&state, NULL, &vehicle, TRUE, &deck, &up, &base, FALSE, &velocity);
		NEAR(state.velocity.i, .1f);
		/* an even tick (the vehicle updated): 0.1 a tick */
		vehicle = vehicle_at(VEHICLE, .2f, 0.f, 0.f, 0.f, 11);
		base.x = .2f;
		CHECK(vehicle_riding_snapshot_step(&state, &vehicle, &base, 1.4f, .2f, TRUE, &step), "dropped");
		NEAR(step.velocity_now.i, .1f);
		/* leaving on it */
		velocity = zero;
		vehicle_riding_record(&state, &step, NULL, FALSE, &zero, &up, &base, FALSE, &velocity);
		NEAR(velocity.i, .1f);
		/* an odd tick (no update): the frames hold, landing back gives 0.1 too */
		CHECK(vehicle_riding_snapshot_step(&state, &vehicle, &base, 1.4f, .2f, TRUE, &step), "dropped");
		NEAR(step.tracked_deck_velocity.i, .1f);
		velocity = zero;
		vehicle_riding_record(&state, &step, &vehicle, TRUE, &step.tracked_deck_velocity, &up, &base, FALSE, &velocity);
		NEAR(state.velocity.i, .1f);
		/* an odd tick, riding: no carry, 0.1 held */
		CHECK(vehicle_riding_snapshot_step(&state, &vehicle, &base, 1.4f, .2f, TRUE, &step), "dropped");
		NEAR(step.displacement.i, 0.f);
		NEAR(step.velocity_now.i, .1f);
		return 0;
	}
	CASE("landing-frame")
	{
		/* riding a deck descending 0.3 a tick, moving 0.05 across it */
		real_vector3d frame = vector(0.f, 0.f, -.3f);
		real_vector3d relative = vector(.05f, 0.f, 0.f);
		real_vector3d other = vector(0.f, 0.f, -.1f);
		real_vector3d support;
		struct vehicle_riding_vehicle vehicle = vehicle_at(VEHICLE, 0.f, 0.f, 0.f, 0.f, 10);
		struct vehicle_riding_vehicle second = vehicle_at(OTHER_VEHICLE, 2.f, 0.f, 0.f, 0.f, 10);
		struct vehicle_riding_state state;
		real_point3d base = point(0.f, 0.f, 0.f);
		real_vector3d velocity;

		/* onto the ground: the world landing velocity */
		vehicle_riding_support_velocity(NONE, VEHICLE, &frame, NONE, &zero, &zero, &support);
		NEAR(vehicle_riding_landing_velocity(&relative, &frame, &support, &up), .3f);
		/* onto a second vehicle descending 0.1: relative to it */
		vehicle_riding_support_velocity(OTHER_VEHICLE, VEHICLE, &frame, NONE, &zero, &other, &support);
		NEAR(vehicle_riding_landing_velocity(&relative, &frame, &support, &up), .2f);
		/* back on the ridden deck: as before */
		vehicle_riding_support_velocity(VEHICLE, VEHICLE, &frame, NONE, &zero, &other, &support);
		NEAR(vehicle_riding_landing_velocity(&relative, &frame, &support, &up), 0.f);
		/* recorded on the ground: no part into it left, so no second landing */
		state = riding_on(&vehicle, base, frame);
		vehicle = vehicle_at(VEHICLE, 0.f, 0.f, -.3f, 0.f, 11);
		CHECK(vehicle_riding_snapshot_step(&state, &vehicle, &base, 1.4f, .2f, FALSE, &step), "dropped");
		velocity = relative;
		vehicle_riding_record(&state, &step, NULL, TRUE, &zero, &up, &base, FALSE, &velocity);
		NEAR(velocity.k, 0.f);
		NEAR(velocity.i, .05f);
		/* recorded on the second vehicle: only its own descent left */
		state = riding_on(&vehicle, base, frame);
		vehicle = vehicle_at(VEHICLE, 0.f, 0.f, -.6f, 0.f, 12);
		CHECK(vehicle_riding_snapshot_step(&state, &vehicle, &base, 1.4f, .2f, FALSE, &step), "dropped");
		velocity = relative;
		vehicle_riding_record(&state, &step, &second, TRUE, &other, &up, &base, FALSE, &velocity);
		NEAR(velocity.k, -.1f);
		CHECK(state.object_index == OTHER_VEHICLE, "not riding the second vehicle");
		NEAR(state.velocity.k, -.1f);
		/* landing back on the tracked deck (in its window, not riding): its tracked velocity */
		{
			real_vector3d tracked = vector(0.f, 0.f, -.2f);

			vehicle_riding_support_velocity(VEHICLE, NONE, &zero, VEHICLE, &tracked, &other, &support);
			NEAR(support.k, -.2f);
		}
		return 0;
	}
	CASE("seat-exit-onto-deck")
	{
		/* a seat exit leaves the biped with the vehicle's velocity: landing on the
		same deck, it stands still on it */
		struct vehicle_riding_vehicle vehicle = vehicle_at(VEHICLE, 0.f, 0.f, 0.f, 0.f, 10);
		struct vehicle_riding_state state;
		real_point3d base = point(0.f, 0.f, 1.f);
		real_vector3d deck = vector(.3f, 0.f, 0.f);
		real_vector3d velocity = deck;
		real_vector3d relative;
		real_vector3d frame;

		vehicle.translational_velocity = deck;
		vehicle_riding_clear(&state);
		vehicle_riding_record(&state, NULL, &vehicle, TRUE, &deck, &up, &base, FALSE, &velocity);
		vehicle_riding_seed(&state, &velocity, &relative, &frame);
		NEAR(relative.i, 0.f);
		return 0;
	}
	CASE("refresh")
	{
		/* a deck accelerating upward: stepping off keeps this tick's deck velocity */
		struct vehicle_riding_vehicle vehicle = vehicle_at(VEHICLE, 0.f, 0.f, 0.f, 0.f, 10);
		struct vehicle_riding_state state = riding_on(&vehicle, point(0.f, 0.f, 0.f), vector(0.f, 0.f, .1f));
		real_point3d base = point(0.f, 0.f, 0.f);
		real_vector3d velocity = zero;

		vehicle = vehicle_at(VEHICLE, 0.f, 0.f, .2f, 0.f, 11);
		CHECK(vehicle_riding_snapshot_step(&state, &vehicle, &base, 1.4f, .2f, FALSE, &step), "dropped");
		vehicle_riding_record(&state, &step, NULL, FALSE, &zero, &up, &base, FALSE, &velocity);
		NEAR(state.velocity.k, .2f);
		return 0;
	}
	CASE("elevator")
	{
		struct vehicle_riding_vehicle vehicle = vehicle_at(VEHICLE, 0.f, 0.f, 0.f, 0.f, 10);
		struct vehicle_riding_state state;
		real_point3d base = point(0.f, 0.f, 0.f);
		real_vector3d velocity = zero;

		vehicle_riding_clear(&state);
		CHECK(vehicle_riding_record(&state, NULL, &vehicle, TRUE, &zero, &up, &base, FALSE, &velocity),
			"riding does not clear the elevator");
		vehicle_riding_clear(&state);
		CHECK(!vehicle_riding_record(&state, NULL, NULL, TRUE, &zero, &up, &base, FALSE, &velocity),
			"the ground clears the elevator");
		return 0;
	}
	CASE("local-frame")
	{
		/* a point into a yawed and pitched vehicle's frame and back */
		real_vector3d forward = pitched(vector((real)cos(.3f), (real)sin(.3f), 0.f), .2f);
		real_vector3d upward = pitched(up, .2f);
		real_point3d origin = point(1.f, 2.f, 3.f);
		real_point3d world = point(2.5f, -1.f, 4.f);
		real_point3d local, back;

		vehicle_riding_point_to_local(&origin, &forward, &upward, &world, &local);
		vehicle_riding_point_to_world(&origin, &forward, &upward, &local, &back);
		CHECK(fabs(local.y) > .1, "the case needs a point to the side");
		NEAR(back.x, 2.5f);
		NEAR(back.y, -1.f);
		NEAR(back.z, 4.f);
		/* 30 ticks in the air off a moving deck: its place on the deck stays */
		{
			struct vehicle_riding_vehicle vehicle = vehicle_at(VEHICLE, 0.f, 0.f, 0.f, 0.f, 10);
			struct vehicle_riding_state state = riding_on(&vehicle, point(1.f, 1.f, 0.f), vector(.1f, 0.f, 0.f));
			real_point3d base = point(1.f, 1.f, 0.f);
			real_point3d kept = state.local_point;
			real_vector3d velocity = zero;
			short tick;

			for (tick = 1; tick <= 30; tick++)
			{
				vehicle = vehicle_at(VEHICLE, .1f*tick, 0.f, 0.f, 0.f, 10 + tick);
				base.z += .05f;
				CHECK(vehicle_riding_snapshot_step(&state, &vehicle, &base, 1.4f, .2f, FALSE, &step), "dropped");
				vehicle_riding_record(&state, &step, NULL, FALSE, &zero, &up, &base, FALSE, &velocity);
			}
			NEAR(state.local_point.x, kept.x);
			NEAR(state.local_point.y, kept.y);
			NEAR(state.local_point.z, kept.z);
		}
		return 0;
	}
	CASE("node-change")
	{
		/* from the bed (node 1) onto the turret (node 2) of the same vehicle */
		struct vehicle_riding_vehicle bed = vehicle_at(VEHICLE, 0.f, 0.f, 0.f, 0.f, 10);
		struct vehicle_riding_vehicle turret = vehicle_at(VEHICLE, 1.f, 0.f, 1.f, .5f, 10);
		struct vehicle_riding_state state;
		real_point3d base = point(1.5f, .5f, 1.f);
		real_vector3d velocity = zero;

		bed.node_index = 1;
		turret.node_index = 2;
		turret.origin = bed.origin;
		vehicle_riding_clear(&state);
		vehicle_riding_record(&state, NULL, &bed, TRUE, &zero, &up, &base, FALSE, &velocity);
		CHECK(state.node_index == 1, "node %d, not the bed's", state.node_index);
		bed.updates = turret.updates = 11;
		CHECK(vehicle_riding_snapshot_step(&state, &bed, &base, 1.4f, .2f, FALSE, &step), "dropped");
		vehicle_riding_record(&state, &step, &turret, TRUE, &zero, &up, &base, FALSE, &velocity);
		CHECK(state.node_index == 2, "node %d, not the turret's", state.node_index);
		/* both still: no carry, no jump from the change of frame */
		turret.updates = 12;
		CHECK(vehicle_riding_snapshot_step(&state, &turret, &base, 1.4f, .2f, FALSE, &step), "dropped");
		NEAR(step.displacement.i, 0.f);
		NEAR(step.displacement.j, 0.f);
		NEAR(step.displacement.k, 0.f);
		return 0;
	}
	CASE("suspended")
	{
		/* a suspended rider (a cutscene holds it): the carried place stands and its velocity is the deck's */
		struct vehicle_riding_vehicle vehicle = vehicle_at(VEHICLE, 0.f, 0.f, 0.f, 0.f, 10);
		struct vehicle_riding_state state = riding_on(&vehicle, point(0.f, 0.f, 0.f), vector(.1f, 0.f, 0.f));
		real_point3d base = point(0.f, 0.f, 0.f);
		real_vector3d velocity = vector(.5f, 0.f, 0.f);

		vehicle = vehicle_at(VEHICLE, .1f, 0.f, 0.f, 0.f, 11);
		CHECK(vehicle_riding_snapshot_step(&state, &vehicle, &base, 1.4f, .2f, FALSE, &step), "dropped");
		vehicle_riding_record(&state, &step, &vehicle, TRUE, &zero, &up, &base, TRUE, &velocity);
		NEAR(velocity.i, .1f);
		return 0;
	}
	CASE("falling")
	{
		/* a deck descending 2 a tick, faster than any falling limit */
		struct vehicle_riding_vehicle vehicle = vehicle_at(VEHICLE, 0.f, 0.f, 0.f, 0.f, 10);
		struct vehicle_riding_state state = riding_on(&vehicle, point(0.f, 0.f, 0.f), vector(0.f, 0.f, -2.f));
		real_point3d base = point(0.f, 0.f, 0.f);
		real_vector3d velocity;
		short tick;

		NEAR(vehicle_riding_falling_velocity(&state, -2.05f), -.05f);
		/* the tick it steps off: relative */
		vehicle = vehicle_at(VEHICLE, 0.f, 0.f, 0.f, 0.f, 11);
		CHECK(vehicle_riding_snapshot_step(&state, &vehicle, &base, 1.4f, .2f, FALSE, &step), "dropped");
		step.velocity_now = vector(0.f, 0.f, -2.f);
		velocity = zero;
		vehicle_riding_record(&state, &step, NULL, FALSE, &zero, &up, &base, FALSE, &velocity);
		NEAR(vehicle_riding_falling_velocity(&state, -2.05f), -.05f);
		/* 90 ticks later, the window over, still in the air: relative */
		for (tick = 0; tick < 90; tick++)
		{
			boolean valid;

			vehicle.updates++;
			valid = state.ticks > 0 && vehicle_riding_snapshot_step(&state, &vehicle, &base, 1.4f, .2f, FALSE, &step);
			vehicle_riding_record(&state, valid ? &step : NULL, NULL, FALSE, &zero, &up, &base, FALSE, &velocity);
		}
		CHECK(state.ticks == 0 && state.falling == 1, "the falling frame did not outlast the window");
		NEAR(vehicle_riding_falling_velocity(&state, -2.05f), -.05f);
		/* landed on the ground beside a rising deck, in its window: the world's */
		state = riding_on(&vehicle, base, vector(0.f, 0.f, 2.f));
		state.ticks = 30;
		state.falling = 1;
		vehicle.updates++;
		CHECK(vehicle_riding_snapshot_step(&state, &vehicle, &base, 1.4f, .2f, FALSE, &step), "dropped");
		vehicle_riding_record(&state, &step, NULL, TRUE, &zero, &up, &base, FALSE, &velocity);
		NEAR(vehicle_riding_falling_velocity(&state, -1.f), -1.f);
		/* landed beside a descending deck, in its window: the world's too */
		state = riding_on(&vehicle, base, vector(0.f, 0.f, -2.f));
		state.ticks = 30;
		state.falling = 1;
		vehicle.updates++;
		CHECK(vehicle_riding_snapshot_step(&state, &vehicle, &base, 1.4f, .2f, FALSE, &step), "dropped");
		vehicle_riding_record(&state, &step, NULL, TRUE, &zero, &up, &base, FALSE, &velocity);
		NEAR(vehicle_riding_falling_velocity(&state, -1.f), -1.f);
		/* off a deck rising 0.1 a tick: never stricter than the world */
		state = riding_on(&vehicle, base, vector(0.f, 0.f, .1f));
		state.ticks = 30;
		state.falling = 1;
		NEAR(vehicle_riding_falling_velocity(&state, -1.f), -1.f);
		return 0;
	}
	CASE("jump-deck-frame")
	{
		/* the jump as biped_jump takes it, above the frame (jump velocity 0.2) */
		real decks[2] = { .3f, -.3f };
		short index;

		for (index = 0; index < 2; index++)
		{
			struct vehicle_riding_vehicle vehicle = vehicle_at(VEHICLE, 0.f, 0.f, 0.f, 0.f, 10);
			struct vehicle_riding_state state = riding_on(&vehicle, point(0.f, 0.f, 0.f), vector(0.f, 0.f, decks[index]));
			real_vector3d velocity = vector(0.f, 0.f, decks[index]);
			real_vector3d jump_frame;
			real upward;

			vehicle_riding_jump_frame(&state, &jump_frame);
			velocity.k -= jump_frame.k;
			upward = velocity.k;
			if (upward < .2f)
				velocity.k += .2f - upward;
			velocity.k += jump_frame.k;
			NEAR(velocity.k - decks[index], .2f);
		}
		/* stepped onto a rising deck this tick (recorded), jumping in the same tick */
		{
			struct vehicle_riding_vehicle vehicle = vehicle_at(VEHICLE, 0.f, 0.f, 0.f, 0.f, 10);
			struct vehicle_riding_state state;
			real_point3d base = point(0.f, 0.f, 0.f);
			real_vector3d deck = vector(0.f, 0.f, .3f);
			real_vector3d velocity = zero;
			real_vector3d jump_frame;

			vehicle_riding_clear(&state);
			vehicle_riding_record(&state, NULL, &vehicle, TRUE, &deck, &up, &base, FALSE, &velocity);
			vehicle_riding_jump_frame(&state, &jump_frame);
			NEAR(jump_frame.k, .3f);
		}
		return 0;
	}
	CASE("collision-decision")
	{
		struct vehicle_riding_vehicle vehicle = vehicle_at(VEHICLE, 0.f, 0.f, 0.f, 0.f, 10);
		struct vehicle_riding_state state = riding_on(&vehicle, point(0.f, 0.f, 0.f), zero);
		real_vector3d deck;
		real_point3d base;

		CHECK(vehicle_riding_collision_window(&state, TRUE, VEHICLE) == _vehicle_riding_collision_ignore, "rider hit");
		state.ticks = 30;
		CHECK(vehicle_riding_collision_window(&state, TRUE, VEHICLE) == _vehicle_riding_collision_fix, "window pushed");
		/* closing below and above the damage speed */
		deck = vector(.1f, 0.f, 0.f);
		{
			real_vector3d slow = vector(.1f, 0.f, 0.f);
			real_vector3d fast = vector(.5f, 0.f, 0.f);

			CHECK(!vehicle_riding_window_damage(&slow, &deck, .25f), "hurt at the deck's speed");
			CHECK(vehicle_riding_window_damage(&fast, &deck, .25f), "not hurt closing at 0.4");
		}
		/* a large deck turning 0.05 a tick, the biped 4 from its origin moving with it */
		vehicle.angular_velocity = vector(0.f, 0.f, .05f);
		base = point(4.f, 0.f, 0.f);
		vehicle_riding_deck_point_velocity(&vehicle, &base, FALSE, &deck);
		{
			real_vector3d with_it = vector(0.f, .2f, 0.f);

			CHECK(!vehicle_riding_window_damage(&with_it, &deck, .1f), "hurt by the deck's turn");
		}
		/* double speed: a vehicle without a player 0.2 an update, the biped with it at 0.1 a tick */
		vehicle = vehicle_at(VEHICLE, 0.f, 0.f, 0.f, 0.f, 10);
		vehicle.translational_velocity = vector(.2f, 0.f, 0.f);
		vehicle_riding_deck_point_velocity(&vehicle, &base, TRUE, &deck);
		{
			real_vector3d with_it = vector(.1f, 0.f, 0.f);

			CHECK(!vehicle_riding_window_damage(&with_it, &deck, .05f), "hurt in double speed");
		}
		/* the verdict: a failed fix hurts a pedestrian (the original), not a biped in the window */
		CHECK(vehicle_riding_collision_damage(_vehicle_riding_collision_original, TRUE), "pedestrian spared");
		CHECK(!vehicle_riding_collision_damage(_vehicle_riding_collision_original, FALSE), "pedestrian hurt");
		CHECK(!vehicle_riding_collision_damage(_vehicle_riding_collision_fix, TRUE), "hurt on a failed fix in the window");
		CHECK(vehicle_riding_collision_damage(_vehicle_riding_collision_fix_and_damage, FALSE), "closing hit spared");
		return 0;
	}
	CASE("claim")
	{
		/* the best hit is the other Warthog's deck (0), the ridden one's deck is hit too (1) */
		long objects[3] = { OTHER_VEHICLE, VEHICLE, NONE };
		real normal_ks[3] = { 1.f, .95f, 1.f };

		CHECK(vehicle_riding_claim(VEHICLE, 3, objects, normal_ks, .7f, 0) == 1, "the ridden deck lost its rider");
		/* the ridden vehicle hit only on a wall: the best takes over */
		normal_ks[1] = .2f;
		CHECK(vehicle_riding_claim(VEHICLE, 3, objects, normal_ks, .7f, 0) == 0, "a wall kept the rider");
		/* not riding: the best */
		normal_ks[1] = .95f;
		CHECK(vehicle_riding_claim(NONE, 3, objects, normal_ks, .7f, 0) == 0, "claimed while not riding");
		/* the best already the ridden one */
		CHECK(vehicle_riding_claim(VEHICLE, 3, objects, normal_ks, .7f, 1) == 1, "the best ridden deck lost");
		return 0;
	}
	CASE("collision-others")
	{
		struct vehicle_riding_vehicle vehicle = vehicle_at(VEHICLE, 0.f, 0.f, 0.f, 0.f, 10);
		struct vehicle_riding_state state;

		/* never rode */
		vehicle_riding_clear(&state);
		CHECK(vehicle_riding_collision_window(&state, TRUE, VEHICLE) == _vehicle_riding_collision_original, "pedestrian");
		/* in another vehicle's window */
		state = riding_on(&vehicle, point(0.f, 0.f, 0.f), zero);
		state.ticks = 30;
		CHECK(vehicle_riding_collision_window(&state, TRUE, OTHER_VEHICLE) == _vehicle_riding_collision_original,
			"another vehicle's window");
		/* riding where riding does not apply (an AI, a network game) */
		CHECK(vehicle_riding_collision_window(&state, FALSE, VEHICLE) == _vehicle_riding_collision_original, "AI");
		/* riding one Warthog, a second one alongside: never as a pedestrian */
		state = riding_on(&vehicle, point(0.f, 0.f, 0.f), zero);
		CHECK(vehicle_riding_collision_window(&state, TRUE, OTHER_VEHICLE) == _vehicle_riding_collision_fix,
			"a second vehicle treats its rider as a pedestrian");
		return 0;
	}
	CASE("damage-speed-default")
	{
		real run = (real)sqrt(3.f*3.f + 2.75f*2.75f)/30.f;

		setting_damage_speed = 0.0;
		vehicle_riding_latch();
		NEAR(vehicle_riding_damage_speed(TRUE, 3.f, 2.5f, 2.75f, FALSE, 1.5f, 1.f, .1f),
			2.f*(real)sqrt(run*run + .01f));
		NEAR(vehicle_riding_damage_speed(TRUE, 3.f, 2.5f, 2.75f, TRUE, 1.5f, 1.f, .1f),
			2.f*(real)sqrt(1.5f*run*1.5f*run + .01f));
		NEAR(vehicle_riding_damage_speed(TRUE, 3.f, 2.5f, 2.75f, TRUE, 1.5f, 1.5f, .1f),
			2.f*(real)sqrt(2.25f*run*2.25f*run + .01f));
		NEAR(vehicle_riding_damage_speed(FALSE, 3.f, 2.5f, 2.75f, FALSE, 1.f, 1.f, .1f), .2f);
		NEAR(vehicle_riding_damage_speed(TRUE, 3.f, 2.5f, (real)NAN, FALSE, 1.f, 1.f, .1f), 0.f);
		setting_damage_speed = 0.4;
		vehicle_riding_latch();
		NEAR(vehicle_riding_damage_speed(TRUE, 3.f, 2.5f, 2.75f, FALSE, 1.f, 1.f, .1f), .4f);
		return 0;
	}
	fprintf(stderr, "unknown case: %s\n", case_name);
	return 2;
}

/*
VEHICLE_RIDING.C

A player standing on a moving vehicle is carried by it (game.vehicle_riding,
port/linux/README.md): its position and view move with the vehicle, a jump
lands back on it, stepping off keeps its motion, and the vehicle does not
hurt it. These are the decisions of each tick, over a plain copy of a
biped's riding fields, so the asset-free harness compiles this file whole
(tools/harness/tests/vehicle_riding.c); bipeds.c gathers what the game
knows and applies what comes back, objects.c updates riders after the
vehicles, and physics.c asks the collision decision. Local games only:
network games, film playback and the editor play the original game.
*/

#include "cseries.h"
#include "math/real_math.h"
#include "vehicle_riding.h"
#include "editor/editor_stubs.h"
#include "game/game.h"
#include "game/players.h"

#include <math.h>

/* ---------- prototypes */

/* (port_config.c, read as units.c reads them) */
int config_boolean(const char *name);
double config_real(const char *name);

/* ---------- globals */

/* the settings as of the last latch */
static boolean riding_setting;
static boolean riding_look_carry = TRUE;
static real riding_damage_speed;
static real riding_teleport_step = 4.f;
static real riding_moved_alone = 0.25f;

/* each local player's view turn by its vehicle this tick, for the direct
camera (render_interpolation.c) */
static real riding_kept_dyaws[MAXIMUM_LOCAL_PLAYERS];

/* ---------- private code */

static real riding_dot(
	real_vector3d const *a,
	real_vector3d const *b)
{
	return a->i*b->i + a->j*b->j + a->k*b->k;
}

static real riding_magnitude(
	real_vector3d const *vector)
{
	return (real)sqrt(riding_dot(vector, vector));
}

static void riding_cross(
	real_vector3d const *a,
	real_vector3d const *b,
	real_vector3d *result)
{
	real_vector3d cross;

	cross.i = a->j*b->k - a->k*b->j;
	cross.j = a->k*b->i - a->i*b->k;
	cross.k = a->i*b->j - a->j*b->i;
	*result = cross;
}

static void riding_between(
	real_point3d const *from,
	real_point3d const *to,
	real_vector3d *result)
{
	result->i = to->x - from->x;
	result->j = to->y - from->y;
	result->k = to->z - from->z;
}

static real riding_distance(
	real_point3d const *a,
	real_point3d const *b)
{
	real_vector3d between;

	riding_between(a, b, &between);
	return riding_magnitude(&between);
}

static void riding_scale(
	real_vector3d *vector,
	real scale)
{
	vector->i *= scale;
	vector->j *= scale;
	vector->k *= scale;
}

static real riding_component(
	real_vector3d const *vector,
	short axis)
{
	return axis == 0 ? vector->i : axis == 1 ? vector->j : vector->k;
}

/* (a vehicle without a player updates every other tick in double speed, so
its velocities and moves are for two player ticks) */
static real riding_period(
	struct vehicle_riding_vehicle const *vehicle,
	boolean double_speed)
{
	return double_speed && !vehicle->has_player ? 2.f : 1.f;
}

/* ---------- public code */

boolean vehicle_riding_latch(
	void)
{
	short local_player_index;

	riding_setting = config_boolean("game.vehicle_riding") != 0;
	riding_look_carry = config_boolean("debug.riding_look_carry") != 0;
	riding_damage_speed = (real)config_real("debug.riding_damage_speed");
	riding_teleport_step = (real)config_real("debug.riding_teleport_step");
	riding_moved_alone = (real)config_real("debug.riding_moved_alone");
	for (local_player_index = 0; local_player_index < MAXIMUM_LOCAL_PLAYERS; local_player_index++)
	{
		riding_kept_dyaws[local_player_index] = 0.f;
	}

	return riding_setting;
}

boolean vehicle_riding_enabled(
	void)
{
	/* (the connection is asked on every call: a latch does not know which
	game comes next) */
	return riding_setting && game_connection() == _game_connection_local && !game_in_editor();
}

boolean vehicle_riding_applies(
	boolean has_player,
	boolean dead)
{
	return has_player && !dead && vehicle_riding_enabled();
}

boolean vehicle_riding_deferred(
	struct vehicle_riding_state const *state,
	boolean applies)
{
	/* (every biped in a window, not only those riding: one that left the
	deck last tick and lands back this tick must still update after the
	vehicle, or its carry spans none or two of the vehicle's updates) */
	return applies && state->ticks > 0;
}

void vehicle_riding_clear(
	struct vehicle_riding_state *state)
{
	struct vehicle_riding_state cleared = { 0 };

	*state = cleared;
	state->object_index = NONE;
	state->node_index = NONE;
}

void vehicle_riding_drop(
	struct vehicle_riding_state *state)
{
	char falling = state->falling;
	real_vector3d velocity = state->velocity;

	vehicle_riding_clear(state);
	/* (a biped still in the air keeps the frame it fell from: its fall is
	judged against the deck until it lands) */
	if (falling)
	{
		state->falling = falling;
		state->velocity = velocity;
	}
}

boolean vehicle_riding_is_riding(
	struct vehicle_riding_state const *state)
{
	return state->ticks == RIDING_TICKS;
}

void vehicle_riding_point_to_local(
	real_point3d const *origin,
	real_vector3d const *forward,
	real_vector3d const *up,
	real_point3d const *point,
	real_point3d *local)
{
	real_vector3d left;
	real_vector3d offset;

	riding_cross(up, forward, &left);
	riding_between(origin, point, &offset);
	local->x = riding_dot(&offset, forward);
	local->y = riding_dot(&offset, &left);
	local->z = riding_dot(&offset, up);
}

void vehicle_riding_point_to_world(
	real_point3d const *origin,
	real_vector3d const *forward,
	real_vector3d const *up,
	real_point3d const *local,
	real_point3d *point)
{
	real_vector3d left;
	real_point3d result;

	riding_cross(up, forward, &left);
	result.x = origin->x + forward->i*local->x + left.i*local->y + up->i*local->z;
	result.y = origin->y + forward->j*local->x + left.j*local->y + up->j*local->z;
	result.z = origin->z + forward->k*local->x + left.k*local->y + up->k*local->z;
	*point = result;
}

real vehicle_riding_yaw(
	real_vector3d const *from_forward,
	real_vector3d const *from_up,
	real_vector3d const *to_forward,
	real_vector3d const *to_up)
{
	real_vector3d from_left;
	real_vector3d to_left;
	real_vector3d const *from_basis[3];
	real_vector3d const *to_basis[3];
	real rotation[3][3];
	real_vector3d axis;
	real sine2;
	real cosine;
	real angle;
	short row;
	short column;
	short basis;

	riding_cross(from_up, from_forward, &from_left);
	riding_cross(to_up, to_forward, &to_left);
	from_basis[0] = from_forward;
	from_basis[1] = &from_left;
	from_basis[2] = from_up;
	to_basis[0] = to_forward;
	to_basis[1] = &to_left;
	to_basis[2] = to_up;
	/* (the rotation that takes the old basis onto the new one) */
	for (row = 0; row < 3; row++)
	{
		for (column = 0; column < 3; column++)
		{
			rotation[row][column] = 0.f;
			for (basis = 0; basis < 3; basis++)
			{
				rotation[row][column] +=
					riding_component(to_basis[basis], row)*riding_component(from_basis[basis], column);
			}
		}
	}

	axis.i = rotation[2][1] - rotation[1][2];
	axis.j = rotation[0][2] - rotation[2][0];
	axis.k = rotation[1][0] - rotation[0][1];
	sine2 = riding_magnitude(&axis);
	if (sine2 < 0.000001f)
		return 0.f;
	cosine = (rotation[0][0] + rotation[1][1] + rotation[2][2] - 1.f)*0.5f;
	angle = (real)atan2(sine2*0.5f, cosine);
	/* (the rotation's axis times its angle, projected on global up: the
	vehicle's yaw alone, whatever its pitch) */
	return angle*axis.k/sine2;
}

void vehicle_riding_turn_vector(
	real_vector3d *vector,
	real angle)
{
	real cosine = (real)cos(angle);
	real sine = (real)sin(angle);
	real i = vector->i;

	vector->i = i*cosine - vector->j*sine;
	vector->j = i*sine + vector->j*cosine;
}

void vehicle_riding_deck_point_velocity(
	struct vehicle_riding_vehicle const *vehicle,
	real_point3d const *point,
	boolean double_speed,
	real_vector3d *velocity)
{
	real_vector3d offset;
	real_vector3d turning;

	/* (about the vehicle's own origin: its velocities are the body's, not a node's) */
	riding_between(&vehicle->origin, point, &offset);
	riding_cross(&vehicle->angular_velocity, &offset, &turning);
	velocity->i = vehicle->translational_velocity.i + turning.i;
	velocity->j = vehicle->translational_velocity.j + turning.j;
	velocity->k = vehicle->translational_velocity.k + turning.k;
	riding_scale(velocity, 1.f/riding_period(vehicle, double_speed));
}

boolean vehicle_riding_snapshot_step(
	struct vehicle_riding_state *state,
	struct vehicle_riding_vehicle const *vehicle,
	real_point3d const *base,
	real pill_height,
	real pill_width,
	boolean double_speed,
	struct vehicle_riding_step *step)
{
	boolean riding = vehicle_riding_is_riding(state);
	real_point3d last;
	real_point3d carried;
	real_point3d local_base;
	real_point3d moved_base;
	real distance;
	real per_tick;

	if (!vehicle || vehicle->object_index != state->object_index)
	{
		vehicle_riding_drop(state);
		return FALSE;
	}
	/* (where it ended its last tick, from its place on the deck as the deck was then) */
	vehicle_riding_point_to_world(&state->vehicle_position, &state->vehicle_forward, &state->vehicle_up, &state->local_point, &last);
	/* (moved by something else since its last tick: a script, a cutscene, a teleport) */
	if (riding && riding_distance(base, &last) > riding_moved_alone)
	{
		vehicle_riding_drop(state);
		return FALSE;
	}
	/* (as an elevator only moves bipeds in its own sphere, device_machines.c) */
	if (!riding &&
		riding_distance(base, &vehicle->bounding_sphere_center) > vehicle->bounding_sphere_radius + pill_height + pill_width)
	{
		vehicle_riding_drop(state);
		return FALSE;
	}

	step->vehicle = *vehicle;
	step->updates = vehicle->updates - state->vehicle_updates;
	/* (the same place on the deck as the deck is now: the carry is the
	deck's motion there, about the ridden node, whatever the base did) */
	vehicle_riding_point_to_world(&vehicle->position, &vehicle->forward, &vehicle->up, &state->local_point, &carried);
	vehicle_riding_point_to_local(&state->vehicle_position, &state->vehicle_forward, &state->vehicle_up, base,
		&local_base);
	vehicle_riding_point_to_world(&vehicle->position, &vehicle->forward, &vehicle->up, &local_base, &moved_base);
	riding_between(&last, &carried, &step->displacement);
	distance = riding_magnitude(&step->displacement);
	/* (a teleport, or a move without an update: stage 1 does not carry it) */
	if (step->updates < 0 ||
		(step->updates == 0 && distance > 0.000001f) ||
		(step->updates > 0 && distance > riding_teleport_step*step->updates))
	{
		vehicle_riding_drop(state);
		return FALSE;
	}

	if (step->updates == 0)
	{
		/* (double speed's skipped tick: the vehicle did not update, and both
		frame velocities hold) */
		step->displacement.i = step->displacement.j = step->displacement.k = 0.f;
		step->dyaw = 0.f;
		step->velocity_now = state->velocity;
		step->tracked_deck_velocity = state->velocity;
		return TRUE;
	}

	per_tick = 1.f/(step->updates*riding_period(vehicle, double_speed));
	step->velocity_now = step->displacement;
	riding_scale(&step->velocity_now, per_tick);
	riding_between(base, &moved_base, &step->tracked_deck_velocity);
	riding_scale(&step->tracked_deck_velocity, per_tick);
	step->dyaw = vehicle_riding_yaw(&state->vehicle_forward, &state->vehicle_up, &vehicle->forward, &vehicle->up);

	return TRUE;
}

real vehicle_riding_look(
	short local_player_index,
	boolean carried,
	real dyaw)
{
	real_euler_angles2d impulse;

	if (local_player_index < 0 || local_player_index >= MAXIMUM_LOCAL_PLAYERS)
		return 0.f;
	if (!carried || !riding_look_carry)
		dyaw = 0.f;
	if (dyaw != 0.f)
	{
		impulse.yaw = dyaw;
		impulse.pitch = 0.f;
		player_control_permanent_impulse(local_player_index, &impulse);
	}
	riding_kept_dyaws[local_player_index] = dyaw;

	return dyaw;
}

real vehicle_riding_kept_dyaw(
	short local_player_index)
{
	if (local_player_index < 0 || local_player_index >= MAXIMUM_LOCAL_PLAYERS)
		return 0.f;
	return riding_kept_dyaws[local_player_index];
}

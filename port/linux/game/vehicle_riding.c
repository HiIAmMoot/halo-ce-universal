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

static void riding_sum(
	real_vector3d const *a,
	real_vector3d const *b,
	real_vector3d *result)
{
	result->i = a->i + b->i;
	result->j = a->j + b->j;
	result->k = a->k + b->k;
}

static void riding_subtract(
	real_vector3d const *a,
	real_vector3d const *b,
	real_vector3d *result)
{
	result->i = a->i - b->i;
	result->j = a->j - b->j;
	result->k = a->k - b->k;
}

static void riding_add(
	real_vector3d *vector,
	real_vector3d const *added)
{
	riding_sum(vector, added, vector);
}

/* the part of a velocity into a support's plane, measured against the
support's own motion, taken off: the landing on it is counted once */
static void riding_clip(
	real_vector3d *velocity,
	real_vector3d const *support_velocity,
	real_vector3d const *normal)
{
	real_vector3d relative;
	real into;

	riding_subtract(velocity, support_velocity, &relative);
	into = riding_dot(&relative, normal);
	if (into < 0.f)
	{
		velocity->i -= normal->i*into;
		velocity->j -= normal->j*into;
		velocity->k -= normal->k*into;
	}
}

/* the vehicle's pose and update count as they are now */
static void riding_snapshot(
	struct vehicle_riding_state *state,
	struct vehicle_riding_vehicle const *vehicle)
{
	state->vehicle_position = vehicle->position;
	state->vehicle_forward = vehicle->forward;
	state->vehicle_up = vehicle->up;
	state->vehicle_updates = vehicle->updates;
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

void vehicle_riding_seed(
	struct vehicle_riding_state const *state,
	real_vector3d const *world_velocity,
	real_vector3d *velocity,
	real_vector3d *frame_velocity)
{
	frame_velocity->i = frame_velocity->j = frame_velocity->k = 0.f;
	if (vehicle_riding_is_riding(state))
		*frame_velocity = state->velocity;
	riding_subtract(world_velocity, frame_velocity, velocity);
}

void vehicle_riding_support_velocity(
	long support_object_index,
	long riding_object_index,
	real_vector3d const *riding_frame_velocity,
	long tracked_object_index,
	real_vector3d const *tracked_deck_velocity,
	real_vector3d const *other_velocity,
	real_vector3d *support_velocity)
{
	if (support_object_index == NONE)
		support_velocity->i = support_velocity->j = support_velocity->k = 0.f;
	else if (support_object_index == riding_object_index)
		*support_velocity = *riding_frame_velocity;
	else if (support_object_index == tracked_object_index)
		*support_velocity = *tracked_deck_velocity;
	else
		*support_velocity = *other_velocity;
}

real vehicle_riding_landing_velocity(
	real_vector3d const *velocity,
	real_vector3d const *frame_velocity,
	real_vector3d const *support_velocity,
	real_vector3d const *normal)
{
	real_vector3d relative;

	/* (the physics' velocity is in the ridden deck's frame: back to the
	world's, then into the frame of what it lands on) */
	riding_sum(velocity, frame_velocity, &relative);
	riding_subtract(&relative, support_velocity, &relative);
	return -riding_dot(&relative, normal);
}

boolean vehicle_riding_record(
	struct vehicle_riding_state *state,
	struct vehicle_riding_step const *step,
	struct vehicle_riding_vehicle const *support,
	boolean supported,
	real_vector3d const *support_velocity,
	real_vector3d const *support_normal,
	real_point3d const *new_base,
	boolean suspended,
	real_vector3d *new_velocity)
{
	boolean started_riding = vehicle_riding_is_riding(state);

	if (started_riding)
	{
		/* (the deck's motion, taken off at the seed, added back: the world velocity) */
		if (suspended)
			*new_velocity = step->velocity_now;
		else
			riding_add(new_velocity, &step->velocity_now);
		state->velocity = step->velocity_now;
		if (supported && (!support || support->object_index != state->object_index))
			riding_clip(new_velocity, support_velocity, support_normal);
	}

	if (support)
	{
		if (step && support->object_index == state->object_index)
			state->velocity = started_riding ? step->velocity_now : step->tracked_deck_velocity;
		else
			state->velocity = *support_velocity;
		state->ticks = RIDING_TICKS;
		state->falling = 0;
		state->object_index = support->object_index;
		state->node_index = support->node_index;
		riding_snapshot(state, support);
		vehicle_riding_point_to_local(&support->position, &support->forward, &support->up, new_base,
			&state->local_point);
		return TRUE;
	}

	if (supported)
		state->falling = 0;
	else if (started_riding)
		state->falling = 1;
	if (step)
	{
		/* (its place on the deck is kept and the snapshot follows the deck:
		a landing back then has the deck's motion of that tick only) */
		riding_snapshot(state, &step->vehicle);
		if (--state->ticks <= 0)
			vehicle_riding_drop(state);
	}

	return FALSE;
}

real vehicle_riding_falling_velocity(
	struct vehicle_riding_state const *state,
	real world_velocity_k)
{
	real relative;

	if (vehicle_riding_is_riding(state) || state->falling)
	{
		relative = world_velocity_k - state->velocity.k;
		/* (the more lenient of the two: a descending deck is judged
		relative to it, a rising one never makes a fall deadlier than in
		the original game) */
		return relative > world_velocity_k ? relative : world_velocity_k;
	}
	return world_velocity_k;
}

void vehicle_riding_jump_frame(
	struct vehicle_riding_state const *state,
	real_vector3d *jump_frame)
{
	jump_frame->i = jump_frame->j = jump_frame->k = 0.f;
	if (vehicle_riding_is_riding(state))
	{
		*jump_frame = state->velocity;
	}
}

short vehicle_riding_collision_window(
	struct vehicle_riding_state const *state,
	boolean applies,
	long vehicle_index)
{
	if (!applies || state->ticks <= 0)
		return _vehicle_riding_collision_original;
	if (state->object_index != vehicle_index)
	{
		/* (a vehicle beside the one it rides, in a convoy: judged on the
		closing speed, never as a pedestrian) */
		return vehicle_riding_is_riding(state) ? _vehicle_riding_collision_fix : _vehicle_riding_collision_original;
	}
	if (vehicle_riding_is_riding(state))
		return _vehicle_riding_collision_ignore;
	return _vehicle_riding_collision_fix;
}

boolean vehicle_riding_window_damage(
	real_vector3d const *biped_velocity,
	real_vector3d const *deck_velocity,
	real damage_speed)
{
	real_vector3d closing;

	riding_subtract(biped_velocity, deck_velocity, &closing);
	return riding_magnitude(&closing) > damage_speed;
}

boolean vehicle_riding_collision_damage(
	short decision,
	boolean original_damage)
{
	if (decision == _vehicle_riding_collision_original)
		return original_damage;
	/* (in the window the closing speed alone decides, whether the fix
	succeeded or not) */
	return decision == _vehicle_riding_collision_fix_and_damage;
}

real vehicle_riding_damage_speed(
	boolean have_information,
	real run_forward,
	real run_backward,
	real run_sideways,
	boolean double_speed,
	real double_speed_multiplier,
	real player_speed_multiplier,
	real jump_velocity)
{
	real run = 0.f;
	real speed;

	if (riding_damage_speed != 0.f)
		return riding_damage_speed;
	/* (as bipeds.c moves a player: forward or back and sideways at once) */
	if (have_information)
	{
		real forward = run_forward > run_backward ? run_forward : run_backward;

		run = (real)sqrt(forward*forward + run_sideways*run_sideways)/TICKS_PER_SECOND;
		if (double_speed && double_speed_multiplier > 1.f)
			run *= double_speed_multiplier;
	}
	if (player_speed_multiplier > 1.f)
		run *= player_speed_multiplier;
	speed = 2.f*(real)sqrt(run*run + jump_velocity*jump_velocity);
	/* (so written that a tag's speed not a number is none) */
	if (!(speed >= 0.f))
		speed = 0.f;

	return speed;
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

short vehicle_riding_claim(
	long riding_object_index,
	short count,
	long const *objects,
	real const *normal_ks,
	real minimum_normal_k,
	short best)
{
	short index;

	if (riding_object_index == NONE || best < 0 || objects[best] == riding_object_index)
		return best;
	/* (two decks side by side: the one it rides keeps it, so the carry does
	not swap between them from tick to tick) */
	for (index = 0; index < count; index++)
	{
		if (objects[index] == riding_object_index && normal_ks[index] >= minimum_normal_k)
			return index;
	}
	return best;
}

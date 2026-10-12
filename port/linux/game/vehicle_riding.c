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

/* (a vehicle without a player updates every other tick in double speed, so
its velocities and moves are for two player ticks) */
static real riding_period(
	struct vehicle_riding_vehicle const *vehicle,
	boolean double_speed)
{
	return double_speed && !vehicle->has_player ? 2.f : 1.f;
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

	subtract_vectors3d(velocity, support_velocity, &relative);
	into = dot_product3d(&relative, normal);
	if (into < 0.f)
	{
		scale_vector3d(normal, into, &relative);
		subtract_vectors3d(velocity, &relative, velocity);
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
	real_matrix4x3 frame;

	matrix4x3_from_point_and_vectors(&frame, origin, forward, up);
	matrix4x3_inverse_transform_point(&frame, point, local);
}

void vehicle_riding_point_to_world(
	real_point3d const *origin,
	real_vector3d const *forward,
	real_vector3d const *up,
	real_point3d const *local,
	real_point3d *point)
{
	real_matrix4x3 frame;

	matrix4x3_from_point_and_vectors(&frame, origin, forward, up);
	matrix4x3_transform_point(&frame, local, point);
}

real vehicle_riding_yaw(
	real_vector3d const *from_forward,
	real_vector3d const *from_up,
	real_vector3d const *to_forward,
	real_vector3d const *to_up)
{
	static real_point3d const origin = { 0.f, 0.f, 0.f };
	real_matrix4x3 from;
	real_matrix4x3 to;
	real_vector3d rotation;

	matrix4x3_from_point_and_vectors(&from, &origin, from_forward, from_up);
	matrix4x3_from_point_and_vectors(&to, &origin, to_forward, to_up);
	vector_from_matrices4x3(&from, &to, &rotation);
	/* (the rotation's axis times its angle, its k: the vehicle's yaw
	alone, whatever its pitch) */
	return rotation.k;
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
	vector_from_points3d(&vehicle->origin, point, &offset);
	cross_product3d(&vehicle->angular_velocity, &offset, &turning);
	add_vectors3d(&vehicle->translational_velocity, &turning, velocity);
	scale_vector3d(velocity, 1.f/riding_period(vehicle, double_speed), velocity);
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
	if (riding && distance3d(base, &last) > riding_moved_alone)
	{
		vehicle_riding_drop(state);
		return FALSE;
	}
	/* (as an elevator only moves bipeds in its own sphere, device_machines.c) */
	if (!riding &&
		distance3d(base, &vehicle->bounding_sphere_center) > vehicle->bounding_sphere_radius + pill_height + pill_width)
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
	vector_from_points3d(&last, &carried, &step->displacement);
	distance = magnitude3d(&step->displacement);
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
	scale_vector3d(&step->displacement, per_tick, &step->velocity_now);
	vector_from_points3d(base, &moved_base, &step->tracked_deck_velocity);
	scale_vector3d(&step->tracked_deck_velocity, per_tick, &step->tracked_deck_velocity);
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
	subtract_vectors3d(world_velocity, frame_velocity, velocity);
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
	add_vectors3d(velocity, frame_velocity, &relative);
	subtract_vectors3d(&relative, support_velocity, &relative);
	return -dot_product3d(&relative, normal);
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
			add_vectors3d(new_velocity, &step->velocity_now, new_velocity);
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

	subtract_vectors3d(biped_velocity, deck_velocity, &closing);
	return magnitude3d(&closing) > damage_speed;
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

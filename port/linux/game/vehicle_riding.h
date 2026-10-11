/*
VEHICLE_RIDING.H

Players carried by the vehicle they stand on (vehicle_riding.c).
*/

#ifndef VEHICLE_RIDING_H
#define VEHICLE_RIDING_H

enum
{
	/* the ticks a biped stays tied to a vehicle it stopped standing on (a
	jump's airtime, a bump that lifts it off for a tick) */
	RIDING_TICKS = 60,
};

/* how a vehicle's collision treats a biped (vehicle_riding_collision_window) */
enum vehicle_riding_collision
{
	_vehicle_riding_collision_original,
	_vehicle_riding_collision_ignore,
	_vehicle_riding_collision_fix,
	_vehicle_riding_collision_fix_and_damage,
};

/* a biped's riding fields (bipeds.h), copied in and out by bipeds.c */
struct vehicle_riding_state
{
	char ticks;
	char falling;
	long object_index;
	long vehicle_updates;
	real_point3d vehicle_position;
	real_vector3d vehicle_forward;
	real_vector3d vehicle_up;
	real_vector3d velocity;
	/* (where it last stood, in the frame of the vehicle's node it stood on:
	along the node's forward, its left and its up from its position) */
	real_point3d local_point;
	short node_index;
};

/* a vehicle as it is now: the pose of the node a biped stands on (the
vehicle's own with no node), and the vehicle's own motion */
struct vehicle_riding_vehicle
{
	long object_index;
	short node_index;
	real_point3d position;
	real_vector3d forward;
	real_vector3d up;
	real_point3d origin;
	long updates;
	real_vector3d translational_velocity;
	real_vector3d angular_velocity;
	real_point3d bounding_sphere_center;
	real bounding_sphere_radius;
	boolean has_player;
};

/* a tick's carry, from the snapshot to the vehicle now */
struct vehicle_riding_step
{
	struct vehicle_riding_vehicle vehicle;
	long updates;
	real_vector3d displacement;
	real dyaw;
	real_vector3d velocity_now;
	real_vector3d tracked_deck_velocity;
};

/* the settings, taken at a map's start, a game's start and every load;
whether riding is on */
boolean vehicle_riding_latch(void);
/* the latched setting, in a local game outside the editor */
boolean vehicle_riding_enabled(void);
/* whether a biped rides at all: a living player's, riding on */
boolean vehicle_riding_applies(boolean has_player, boolean dead);
/* whether a biped updates after the main loop (objects_update_passes) */
boolean vehicle_riding_deferred(struct vehicle_riding_state const *state, boolean applies);

void vehicle_riding_clear(struct vehicle_riding_state *state);
/* ends riding; a biped still in the air keeps the frame it fell from */
void vehicle_riding_drop(struct vehicle_riding_state *state);
boolean vehicle_riding_is_riding(struct vehicle_riding_state const *state);

/* a world point in a pose's own frame (along its forward, its left and its
up from its origin), and back */
void vehicle_riding_point_to_local(real_point3d const *origin, real_vector3d const *forward, real_vector3d const *up,
	real_point3d const *point, real_point3d *local);
void vehicle_riding_point_to_world(real_point3d const *origin, real_vector3d const *forward, real_vector3d const *up,
	real_point3d const *local, real_point3d *point);
/* the turn from one basis to another about global up, in radians */
real vehicle_riding_yaw(real_vector3d const *from_forward, real_vector3d const *from_up,
	real_vector3d const *to_forward, real_vector3d const *to_up);
/* a vector turned about global up */
void vehicle_riding_turn_vector(real_vector3d *vector, real angle);
/* the vehicle body's motion at a point, per player tick */
void vehicle_riding_deck_point_velocity(struct vehicle_riding_vehicle const *vehicle, real_point3d const *point,
	boolean double_speed, real_vector3d *velocity);

/* the deck's motion since the biped's last tick (NULL vehicle: gone);
FALSE when riding was dropped */
boolean vehicle_riding_snapshot_step(struct vehicle_riding_state *state, struct vehicle_riding_vehicle const *vehicle,
	real_point3d const *base, real pill_height, real pill_width, boolean double_speed,
	struct vehicle_riding_step *step);

/* a local player's view turned by its vehicle this tick (the impulse given);
0 when it is not carried */
real vehicle_riding_look(short local_player_index, boolean carried, real dyaw);
/* that turn, for the direct camera */
real vehicle_riding_kept_dyaw(short local_player_index);

#endif

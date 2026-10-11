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

enum vehicle_riding_collision
{
	_vehicle_riding_collision_original,
	_vehicle_riding_collision_ignore,
	_vehicle_riding_collision_fix,
	_vehicle_riding_collision_fix_and_damage,
};

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
	real_point3d local_point;
	short node_index;
};

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

struct vehicle_riding_step
{
	struct vehicle_riding_vehicle vehicle;
	long updates;
	real_vector3d displacement;
	real dyaw;
	real_vector3d velocity_now;
	real_vector3d tracked_deck_velocity;
};

boolean vehicle_riding_latch(void);
boolean vehicle_riding_enabled(void);
boolean vehicle_riding_applies(boolean has_player, boolean dead);
boolean vehicle_riding_deferred(struct vehicle_riding_state const *state, boolean applies);
void vehicle_riding_clear(struct vehicle_riding_state *state);
void vehicle_riding_drop(struct vehicle_riding_state *state);
boolean vehicle_riding_is_riding(struct vehicle_riding_state const *state);
void vehicle_riding_point_to_local(real_point3d const *origin, real_vector3d const *forward, real_vector3d const *up,
	real_point3d const *point, real_point3d *local);
void vehicle_riding_point_to_world(real_point3d const *origin, real_vector3d const *forward, real_vector3d const *up,
	real_point3d const *local, real_point3d *point);
real vehicle_riding_yaw(real_vector3d const *from_forward, real_vector3d const *from_up,
	real_vector3d const *to_forward, real_vector3d const *to_up);
void vehicle_riding_turn_vector(real_vector3d *vector, real angle);
void vehicle_riding_deck_point_velocity(struct vehicle_riding_vehicle const *vehicle, real_point3d const *point,
	boolean double_speed, real_vector3d *velocity);
boolean vehicle_riding_snapshot_step(struct vehicle_riding_state *state, struct vehicle_riding_vehicle const *vehicle,
	real_point3d const *base, real pill_height, real pill_width, boolean double_speed,
	struct vehicle_riding_step *step);
real vehicle_riding_look(short local_player_index, boolean carried, real dyaw);
real vehicle_riding_kept_dyaw(short local_player_index);

#endif

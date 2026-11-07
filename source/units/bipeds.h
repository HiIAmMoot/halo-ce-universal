/*
BIPEDS.H

header included in hcex build.
*/

#ifndef __BIPEDS_H
#define __BIPEDS_H
#pragma once

/* ---------- headers */

#include "units.h"

/* ---------- constants */

enum
{
	_biped_airborne_bit = 0,
	_biped_slipping_bit,
	_biped_absolute_movement_bit,
	_biped_no_collision_bit,
	_biped_movement_passes_through_bipeds_bit,
	_biped_limp_body_physics_active_bit,
};

/* ---------- macros */

#define biped_get(index)			((struct biped_datum *)object_get_and_verify_type((index), _object_mask_biped))
#define biped_try_and_get(index)	((struct biped_datum *)object_try_and_get_and_verify_type((index), _object_mask_biped))

/* ---------- structures */

struct animation;
struct object_placement_data;
struct real_orientation;
struct scenario_object_datum;
struct unit_animation_update_data;

struct _biped_datum
{
	unsigned long flags;
	char landing_recovery_counter;
	char landing_recovery_time;
	char state;
	char elevator_ticks;
	long elevator_object_index;
	long support_surface_index;
	long pathfinding_surface_index;
	real_point3d pathfinding_point;
	long last_pathfinding_attempt_time;
	long last_pathfinding_surface_index;
	long impact_target_object_index;
	long last_falling_communication_time;
	long bump_object_index;
	char bump_ticks;
	char airborne_ticks;
	char slipping_ticks;
	char stop_ticks;
	char jump_recovery_timer;
	char player_melee_ticks;
	char player_melee_attack_tick;
	short landing;
	real crouch;
	real bank;
	real_plane3d ground_plane;
	byte limp_body_current_relaxation_iterations;
	byte limp_body_max_relaxation_iterations;
	/* port: the vehicle the biped stands on, and where it and the biped
	were at the end of the biped's last tick, for its carry
	(vehicle_riding.c; the two bytes sit in the padding after the limp body
	counts) */
	char riding_ticks;
	char riding_falling;
	long riding_object_index;
	long riding_vehicle_updates;
	real_point3d riding_vehicle_position;
	real_vector3d riding_vehicle_forward;
	real_vector3d riding_vehicle_up;
	real_vector3d riding_velocity;
	/* (where it last stood, in the frame of the vehicle's node it stood on) */
	real_point3d riding_local_point;
	short riding_node_index;
	short riding_pad;
	/* (the vehicle's own pose then, to tell a node that moves with the body
	from one that moves on its own) */
	real_point3d riding_body_position;
	real_vector3d riding_body_forward;
	real_vector3d riding_body_up;
};

struct biped_datum
{
	long definition_index;
	struct _object_datum object;
	struct _unit_datum unit;
	struct _biped_datum biped;
};

/* port: a biped's game datum, riding fields included (object_types.c's
biped row; game_state.c refuses saved games of another size) */
#define BIPED_DATUM_SIZE 0x4EC
typedef char biped_datum_size_assert[sizeof(struct biped_datum) == BIPED_DATUM_SIZE ? 1 : -1];

/* ---------- prototypes/BIPEDS.C */

/* port: riding on a moving vehicle (vehicle_riding.c) taken from the
settings; off, no biped keeps riding state */
void bipeds_riding_latch(void);

/* port: a biped that updates after the main loop, and a unit's velocity
relative to the deck it rides (vehicle_riding.c) */
boolean biped_riding_deferred(long biped_index);
void biped_riding_relative_velocity(long unit_index, real_vector3d *velocity);

/* port: the deck part of a player's unit for the wire, FALSE outside any
window; and how many nodes a vehicle a deck part names has, NONE for none
(vehicle_riding.c's deck functions, network_distributed.c) */
struct vehicle_riding_deck;
boolean biped_riding_deck(long biped_index, struct vehicle_riding_deck *deck);
/* port: the unit's own window for deck-frame network commits */
struct vehicle_riding_state;
boolean biped_riding_get_state(long biped_index, struct vehicle_riding_state *state);
void biped_riding_drop(long biped_index);
boolean biped_riding_rebase(long biped_index);
boolean biped_riding_board(long biped_index, long vehicle_index, short node_index);
boolean biped_riding_deck_position(long biped_index, struct vehicle_riding_deck const *deck, real_point3d *position);
/* port: whether a place in a vehicle node's frame is a standing spot on that vehicle's deck now */
boolean biped_riding_deck_spot(long biped_index, long vehicle_index, short node_index, real_point3d const *place);
/* port: take a remote window with this machine's deck frame */
boolean biped_riding_take_deck(long biped_index, struct vehicle_riding_deck const *deck);
boolean biped_riding_air_correct(long biped_index, real_vector3d const *difference, real peak, short verdict);
boolean biped_riding_air_take(long biped_index, struct vehicle_riding_deck const *deck);
/* port: the host's legacy verdict does not wait for a positional correction */
boolean biped_riding_air_verdict_host(long biped_index, struct vehicle_riding_deck const *deck, boolean current_episode);
/* port: a host's deck part's cabin verdict for the unit's player (deck NULL: none) */
void biped_riding_cabin_host(long biped_index, struct vehicle_riding_deck const *deck, boolean current_window);
boolean biped_riding_frame_velocity(long biped_index, struct vehicle_riding_deck const *deck,
	real_point3d const *base, real_vector3d *velocity);
short biped_riding_vehicle_node_count(long vehicle_index);
/* port: a vehicle as the riding code sees it, posed as its node (NONE: as
itself); FALSE: none, or not a vehicle (the riding tests: network_test.c) */
struct vehicle_riding_vehicle;
boolean biped_riding_vehicle(long vehicle_index, short node_index, struct vehicle_riding_vehicle *vehicle);

/* port: how a vehicle's collision treats a biped standing on it, just off
it or in the air over it: an enum vehicle_riding_collision
(vehicle_riding.h); a touch it does not hit is recorded for the biped's
own tick (base: the pill's, of that height; contact_normal: the contact
plane's, NULL when the base is inside the model) */
short biped_riding_collision(long biped_index, long vehicle_index, real_vector3d const *contact_normal,
	real_point3d const *base, real height);

/* port: a vehicle's state correction carries the biped it newly touches
(biped_riding_vehicle_writing measures before the write,
biped_riding_vehicle_written carries after it) */
void biped_riding_vehicle_writing(long vehicle_index);
void biped_riding_vehicle_written(long vehicle_index, real_point3d const *before_position,
	real_vector3d const *before_forward, real_vector3d const *before_up);

char animation_left_foot_frame_index(
	struct animation const *animation);
char animation_right_foot_frame_index(
	struct animation const *animation);

void bipeds_initialize(
	void);
void bipeds_initialize_for_new_map(
	void);
void bipeds_dispose_from_old_map(
	void);
void bipeds_dispose(
	void);
void biped_place(
	long biped_index,
	struct scenario_object_datum *scenario_object);
void biped_delete(
	long biped_index);
void biped_reset(
	long biped_index);
boolean biped_flying_through_air(
	long biped_index);
void biped_adjust_placement(
	long object_index,
	struct object_placement_data *data);
void biped_export_function_values(
	long biped_index);
void biped_disconnect_from_structure_bsp(
	long biped_index);
void biped_stop_melee_attack(
	long biped_index);

void biped_get_sight_position(
	long biped_index,
	short estimate_mode,
	real_point3d const *estimated_body_position,
	real_vector3d *desired_facing,
	real_vector3d const *desired_gun_offset,
	real_point3d *sight_position);
void biped_get_physics_pill(
	long biped_index,
	real_point3d *base,
	real *height,
	real *width);
void biped_get_autoaim_pill(
	long biped_index,
	real_point3d *base,
	real_vector3d *height,
	real *width);
boolean biped_fix_position(
	long biped_index,
	long line_of_sight_object_index,
	real_point3d const *new_position,
	real_point3d *final_position,
	real maximum_radius_fudge_factor,
	boolean fix_below_new_position,
	boolean dont_teleport,
	boolean use_radius_as_multiplier);
/* port: the next biped_fix_position calls skip other players, so a co-op
script can stand someone on a spot a teammate already occupies. Cleared by
the caller before it returns. */
void biped_fix_ignore_players(
	boolean ignore);
void biped_accelerate(
	long biped_index,
	real_vector3d *acceleration);

void biped_stop_limp_body_physics(
	long biped_index);
void biped_start_limp_body_physics(
	long biped_index);

void biped_build_flying_axes(
	real_vector3d const *forward_vector,
	real_vector3d *left_vector,
	real_vector3d *up_vector);
void biped_exit_seat_end(
	long biped_index,
	long seat_object_index);
void biped_render_debug(
	long biped_index);
long biped_approximate_surface_index(
	long biped_index,
	real_point3d *point);
long biped_find_pathfinding_surface_index(
	long biped_index,
	real_point3d *pathfinding_point);
boolean biped_new(
	long biped_index);
boolean biped_update(
	long biped_index);
void biped_preprocess_node_orientations(
	long biped_index,
	struct real_orientation *node_orientations);

/* ---------- globals */

/* ---------- public code */

#endif // __BIPEDS_H

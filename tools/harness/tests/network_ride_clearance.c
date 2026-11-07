/* Asset-free world: every centre ray sees deck, but a rim vertex's expanded
   cylinder intersects the first candidate. The engine's point test is real. */
#include "harness.h"
#include <math.h>
#include <float.h>
typedef struct { real x, y; } real_point2d;
typedef struct { real_vector3d n; real d; } real_plane3d;
#define REAL_MIN (-FLT_MAX)
#include "ride_clearance_types.inc"
enum { _object_type_biped };
struct object_datum {
	long definition_index;
	struct { short type; long parent_object_index; real_point3d position; real_vector3d translational_velocity; } object;
};
struct vehicle_riding_vehicle { real_point3d position, origin; real_vector3d forward, up, translational_velocity, angular_velocity; boolean has_player; };
struct collision_model_instance { long object_index; };
static struct { struct { real runtime_minimum_normal_k; } biped; } definition;
static struct { short scenario; real spot_x, spot_y; } ride;
static struct object_datum unit;
static short placements;
static short queries;
static boolean first_three_blocked;
static void unit_exit_seat_end(long index) { unit.object.parent_object_index = NONE; }
static boolean all_blocked, radius_trap, centre_blocked;
static real requested_width;
static real_vector3d deck_linear, deck_angular;
#define game_players_are_double_speed() FALSE
static struct object_datum *object_get(long index) { return &unit; }
#define biped_definition_get(index) (&definition)
#define ride_log(...) ((void)0)
static boolean biped_riding_vehicle(long index, long node, struct vehicle_riding_vehicle *v) {
	memset(v, 0, sizeof(*v)); v->translational_velocity = deck_linear; v->angular_velocity = deck_angular; return TRUE;
}
static void biped_get_physics_pill(long index, real_point3d *base, real *height, real *width) {
	*base = unit.object.position; base->z += .2f; *height = .3f; *width = .2f;
}
static void ride_board_spot(short scenario, short ordinal, short index, real *x, real *y) {
	*x = 0.f; *y = .05f*index;
	if (first_three_blocked)
		*y = scenario == _ride_convoy ? .2f : scenario == _ride_brush ? -.2f : 0.f;
}
static void vehicle_riding_point_to_world(real_point3d const *p, real_vector3d const *f,
	real_vector3d const *u, real_point3d const *local, real_point3d *world) { *world = *local; }
static boolean ride_deck_surface(long index, real_point3d const *point, real room,
	real minimum_normal, real_point3d *surface) { *surface = *point; surface->z = 0.f; return TRUE; }
static void object_set_position(long index, real_point3d const *p, void *f, void *u) {
	unit.object.position = *p; placements++;
}
static boolean collision_model_instance_new(struct collision_model_instance *instance, long index) {
	instance->object_index = index; return TRUE;
}
static void collision_features_new(struct collision_feature_list *features) { memset(features, 0, sizeof(*features)); }
static void collision_model_get_features_in_sphere(struct collision_model_instance *instance,
	real_point3d const *center, real radius, real height, real width, struct collision_feature_list *features) {
	struct collision_cylinder *c;
	queries++;
	if (first_three_blocked && queries > 3)
		return;
	requested_width = width;
	features->count[_collision_feature_cylinder] = 1;
	c = &features->cylinders[0]; memset(c, 0, sizeof(*c));
	c->object_index = instance->object_index;
	c->base.x = all_blocked || first_three_blocked ? center->x + .18f : radius_trap ? .195f : .18f;
	c->base.y = all_blocked || first_three_blocked || centre_blocked ? center->y : radius_trap ? 0.f : -.08f;
	c->height.k = height + .4f;
	c->width = width;
}
static void vector_from_points3d(real_point3d const *a, real_point3d const *b, real_vector3d *v) {
	v->i = b->x-a->x; v->j = b->y-a->y; v->k = b->z-a->z;
}
static real dot_product3d(real_vector3d const *a, real_vector3d const *b) { return a->i*b->i+a->j*b->j+a->k*b->k; }
static real magnitude_squared3d(real_vector3d const *v) { return dot_product3d(v,v); }
static void scale_vector3d(real_vector3d const *v, real s, real_vector3d *out) { out->i=v->i*s; out->j=v->j*s; out->k=v->k*s; }
static void subtract_vectors3d(real_vector3d const *a, real_vector3d const *b, real_vector3d *v) { v->i=a->i-b->i; v->j=a->j-b->j; v->k=a->k-b->k; }
static real normalize3d(real_vector3d *v) { real n=sqrtf(magnitude_squared3d(v)); if(n) scale_vector3d(v,1.f/n,v); return n; }
static void set_real_vector3d(real_vector3d *v, real i, real j, real k) { v->i=i; v->j=j; v->k=k; }
static void cross_product3d(real_vector3d const *a, real_vector3d const *b, real_vector3d *v) { v->i=a->j*b->k-a->k*b->j; v->j=a->k*b->i-a->i*b->k; v->k=a->i*b->j-a->j*b->i; }
static void add_vectors3d(real_vector3d const *a, real_vector3d const *b, real_vector3d *v) { v->i=a->i+b->i; v->j=a->j+b->j; v->k=a->k+b->k; }
static boolean collision_sphere_test_point(struct collision_sphere const *s, real_point3d const *p, real *d, real_plane3d *n) { return FALSE; }
static boolean collision_prism_test_point(struct collision_prism const *s, real_point3d const *p, real *d, real_plane3d *n) { return FALSE; }
#include "ride_clearance_under_test.inc"
int main(int argc, char **argv) {
	char const *case_name = argc > 1 ? argv[1] : "";
	ride.scenario = _ride_driven; definition.biped.runtime_minimum_normal_k = .7f;
	unit.object.parent_object_index = NONE;
	CASE("board-all-scenario-fallback") {
		short scenario, ordinal;
		first_three_blocked = TRUE;
		for (scenario = _ride_straight; scenario < NUMBER_OF_RIDE_SCENARIOS; scenario++) {
			for (ordinal = 0; ordinal < 2; ordinal++) {
				ride.scenario = scenario; queries = placements = 0;
				CHECK(ride_board(1, 2, ordinal), "three refused spots exhausted scenario %d ordinal %d", scenario, ordinal);
				CHECK(queries == 4 && placements == 1, "fallback bypassed clearance or moved twice");
				if (scenario != _ride_longsword && scenario != _ride_driven) {
					CHECK(fabsf(unit.object.position.x - (ordinal ? .15f : -.95f)) < .00001f, "wrong ordinal region");
					CHECK(scenario != _ride_convoy || unit.object.position.y > 0.f, "convoy changed side");
					CHECK(scenario != _ride_brush || unit.object.position.y < 0.f, "brush changed side");
				}
			}
		}
		return 0;
	}
	CASE("board-all-scenario-exhaustion") {
		short scenario;
		all_blocked = TRUE;
		for (scenario = _ride_straight; scenario < NUMBER_OF_RIDE_SCENARIOS; scenario++) {
			ride.scenario = scenario; queries = placements = 0;
			CHECK(!ride_board(1, 2, 0), "exhausted search accepted overlap");
			CHECK(queries == 84 && placements == 0, "search did not test its exact maximum");
		}
		return 0;
	}
	CASE("board-rejects-overlap") {
		CHECK(ride_board(1, 2, 0), "clear second candidate refused");
		CHECK(placements == 1 && unit.object.position.y == .05f, "centre ray accepted a pill overlapping the rim");
		return 0;
	}
	CASE("board-no-clear-spot") {
		all_blocked = TRUE;
		CHECK(!ride_board(1, 2, 0), "all overlapping candidates accepted");
		CHECK(placements == 0, "failed setup moved the player"); return 0;
	}
	CASE("board-circle-clearance") {
		ride.scenario = _ride_circle;
		CHECK(ride_board(1, 2, 0), "clear circle candidate refused");
		CHECK(placements == 1 && unit.object.position.y == .05f, "non-driven boarding skipped whole-pill clearance");
		return 0;
	}
	CASE("board-full-radius") {
		radius_trap = TRUE;
		CHECK(ride_board(1, 2, 0), "clear pill refused");
		CHECK(unit.object.position.y == .05f, "full-radius overlap accepted");
		CHECK(requested_width == .2f, "setup used a shrunken physics collision radius"); return 0;
	}
	CASE("board-deck-velocity") {
		deck_linear.i=.1f; deck_linear.j=.2f; deck_linear.k=.3f; deck_angular.k=.5f;
		unit.object.translational_velocity.i = unit.object.translational_velocity.j = unit.object.translational_velocity.k = -.7f;
		CHECK(ride_board(1, 2, 0), "no clear candidate");
		CHECK(fabsf(unit.object.translational_velocity.i-.075f)<.00001f &&
			fabsf(unit.object.translational_velocity.j-.2f)<.00001f &&
			fabsf(unit.object.translational_velocity.k-.3f)<.00001f, "fall not replaced by linear plus angular cross offset");
		return 0;
	}
	CASE("board-recovery-search") {
		centre_blocked = TRUE;
		CHECK(ride_board(1, 2, 0), "moving pose has no clear centre-line candidate");
		CHECK(unit.object.position.x != 0.f, "overlapping centre-line pill accepted");
		return 0;
	}
	return 2;
}

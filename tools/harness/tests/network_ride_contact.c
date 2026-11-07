/* a contact plane can support a capsule whose centre ray misses the edge */
#include "harness.h"
#include <math.h>
struct object_datum { struct { real_point3d bounding_sphere_center; real bounding_sphere_radius; } object; } object;
struct collision_model_instance { int unused; };
struct collision_model_test_vector_result { struct { real t; } bsp_result; };
enum { _object_mask_vehicle, _collision_test_front_facing_surfaces_bit };
static boolean ray_hits;
#define object_try_and_get_and_verify_type(index, mask) (&object)
#define collision_model_instance_new(instance, index) TRUE
static boolean collision_model_test_vector(struct collision_model_instance *i, long flags, real_point3d const *p,
	real_vector3d const *v, struct collision_model_test_vector_result *hit) { hit->bsp_result.t = .5f; return ray_hits; }
#include "under_test.inc"
int main(int argc, char **argv) {
	real_point3d base = { .9611f, -.0821f, .4555f };
	real_point3d contact = { .90f, -.0821f, .2555f };
	real_vector3d normal = { 0.f, 0.f, 1.f };
	real height = -99.f;
	char const *case_name = argv[1];
	CASE("edge") {
		CHECK(ride_contact_height(TRUE, 2, 2, &contact, &normal, &base, .2f, &height), "physical edge support rejected with centre-ray miss");
		CHECK(fabsf(height) < .00001f, "height not computed from physical contact"); return 0;
	}
	CASE("unsupported") {
		ray_hits = TRUE;
		CHECK(!ride_contact_height(FALSE, NONE, 2, &contact, &normal, &base, .2f, &height), "ray accepted without physical support"); return 0;
	}
	CASE("ticks-only") {
		/* a received ticks-60 state changes none of these physical inputs */
		CHECK(!ride_contact_height(FALSE, 2, 2, &contact, &normal, &base, .2f, &height), "network claim accepted without physical support"); return 0;
	}
	CASE("other-vehicle") {
		CHECK(!ride_contact_height(TRUE, 3, 2, &contact, &normal, &base, .2f, &height), "other vehicle accepted"); return 0;
	}
	CASE("slope") {
		normal.i = .6f; normal.k = .8f; contact.x = .8611f; contact.z = .3f;
		CHECK(ride_contact_height(TRUE, 2, 2, &contact, &normal, &base, .2f, &height), "valid sloped support rejected");
		CHECK(fabsf(height - .0305f) < .00001f, "independent plane height wrong"); return 0;
	}
	return 2;
}

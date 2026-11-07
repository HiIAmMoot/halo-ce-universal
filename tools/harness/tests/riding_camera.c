/*
RIDING_CAMERA.C (test)

The desktop direct camera's turn against the drawn vehicle, and the dead
player whose last kept turn no longer changes. The real camera and vector
turn are taken from their port files by test_riding_camera.py.
*/

#include "harness.h"
#include <math.h>

#define MAXIMUM_LOCAL_PLAYERS 2
#define _director_perspective_first_person 0
#define _object_dead_bit 0
struct observer_result { real_vector3d forward, up; };
struct object_datum
{
	struct { long parent_object_index; unsigned long damage_flags; } object;
};
static struct object_datum unit;
static struct observer_result direct_cameras[MAXIMUM_LOCAL_PLAYERS];
static real fraction, kept[2];
static real_vector3d facing;
static boolean direct_enabled = TRUE;
#define config_changes() 0UL
#define config_boolean(name) direct_enabled
#define director_get_perspective(index) _director_perspective_first_person
#define director_inhibited_facing(index) FALSE
#define cinematic_in_progress() FALSE
#define player_control_get_unit_index(index) 7
#define object_get(index) (&unit)
#define player_control_get_facing_direction(index, output) (*(output) = facing)
#define render_interpolation_fraction() fraction
#define vehicle_riding_kept_dyaw(index) kept[index]
static void observer_up_from_forward(real_vector3d const *forward, real_vector3d *up)
{
	up->i = -forward->k * forward->i;
	up->j = -forward->k * forward->j;
	up->k = forward->i * forward->i + forward->j * forward->j;
}

#include "under_test.inc"

int main(int argc, char **argv)
{
	char const *case_name = argc > 1 ? argv[1] : "";
	struct observer_result observer;
	struct observer_result const *drawn;
	short index;

	memset(&observer, 0, sizeof(observer));
	unit.object.parent_object_index = NONE;
	facing.i = 0.f;
	facing.j = 0.8f;
	facing.k = 0.6f;
	kept[0] = 1.5707963267948966f;
	kept[1] = -1.5707963267948966f;
	CASE("smooth")
	{
		for (index = 0; index <= 4; index++)
		{
			real angle = index * 1.5707963267948966f / 4.f;

			fraction = index / 4.f;
			drawn = render_interpolation_direct_camera(0, &observer);
			CHECK(fabs(drawn->forward.i - 0.8f*cos(angle)) < 0.000001f,
				"wrong drawn x at fraction %g: %g", fraction, drawn->forward.i);
			CHECK(fabs(drawn->forward.j - 0.8f*sin(angle)) < 0.000001f,
				"wrong drawn y at fraction %g: %g", fraction, drawn->forward.j);
			CHECK(drawn->forward.k == 0.6f, "vehicle changed pitch");
		}
		return 0;
	}
	CASE("dead")
	{
		unit.object.damage_flags = FLAG(_object_dead_bit);
		fraction = 0.f;
		drawn = render_interpolation_direct_camera(0, &observer);
		CHECK(drawn->forward.i == facing.i && drawn->forward.j == facing.j &&
			drawn->forward.k == facing.k, "dead player's frozen turn was undone");
		return 0;
	}
	CASE("players")
	{
		fraction = 0.f;
		drawn = render_interpolation_direct_camera(1, &observer);
		CHECK(fabs(drawn->forward.i + 0.8f) < 0.000001f &&
			fabs(drawn->forward.j) < 0.000001f, "used another player's kept turn");
		return 0;
	}
	CASE("parented")
	{
		unit.object.parent_object_index = 9;
		CHECK(render_interpolation_direct_camera(0, &observer) == &observer,
			"changed a seated player's camera");
		return 0;
	}
	CASE("no-turn")
	{
		kept[0] = 0.f;
		drawn = render_interpolation_direct_camera(0, &observer);
		CHECK(drawn->forward.i == facing.i && drawn->forward.j == facing.j &&
			drawn->forward.k == facing.k, "changed facing without a kept turn");
		return 0;
	}
	fprintf(stderr, "unknown case: %s\n", case_name);
	return 2;
}

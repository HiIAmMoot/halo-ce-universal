/*
NETWORK_RIDE_FLANKS.C (test)

Co-op preparation's half-width lanes and cross slope, using the real level
search functions from port/linux/game/network_test.c.
*/
#include "harness.h"
#include <math.h>
typedef struct { real i, j; } real_vector2d;
typedef struct { real yaw, pitch; } real_euler_angles2d;
#define SET_FLAG(v, b, on) ((v) = (on) ? ((v) | FLAG(b)) : ((v) & ~FLAG(b)))
#define _pi 3.14159265358979f
#include "ride_types.inc"
static struct { short scenario; long vehicles[2]; } ride;
static boolean cross_slope;
static short rough_flank;
static boolean ride_ground_height(real_point3d const *point, real *height)
{
	/* centre steps 0.99: allowed; one flank steps 1.02: not allowed,
	but within 0.03 of the centre, so the along-lane test must catch it */
	*height = cross_slope ? .55f*point->y : point->x >= 5.f && point->x <= 10.f ?
		(point->y*rough_flank > .5f && point->y*rough_flank < .7f ? 1.02f : .99f) : 0.f;
	return TRUE;
}
#include "ride_under_test.inc"

int main(int argc, char **argv)
{
	char const *case_name = argc > 1 ? argv[1] : "";
	real_point3d position = { 0.f, 0.f, 0.f };
	real yaw = -1.f;
	real rise;
	short scenario;

	CASE("ride-coop-flanks")
	{
		for (scenario = _ride_coop; scenario <= _ride_coop_double; scenario++)
		{
			ride.scenario = scenario;
			cross_slope = FALSE;
			rough_flank = 1;
			CHECK(ride_level_heading(&position, 70.f, 10.f, 0.f, 0.f, &yaw, &rise) && yaw != 0.f, "left flank run unchecked");
			rough_flank = -1;
			CHECK(ride_level_heading(&position, 70.f, 10.f, 0.f, 0.f, &yaw, &rise) && yaw != 0.f, "right flank run unchecked");
			rough_flank = 0;
			cross_slope = TRUE;
			CHECK(!ride_level_heading(&position, 70.f, 10.f, 0.f, 0.f, &yaw, &rise), "28-degree cross slope accepted");
		}
		return 0;
	}
	return 2;
}

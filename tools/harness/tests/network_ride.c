/*
NETWORK_RIDE.C (test)

The riding tests' script (port/linux/game/network_test.c): their phases,
how the host moves each scenario's vehicles a tick, and each rider's and
driver's input a tick, taken from the sources by test_network_ride.py.
*/

#include "harness.h"
#include <math.h>
#include <stdarg.h>
typedef struct { real i, j; } real_vector2d;
typedef struct { real yaw, pitch; } real_euler_angles2d;
#define SET_FLAG(v, b, on) ((v) = (on) ? ((v) | FLAG(b)) : ((v) & ~FLAG(b)))
#define _pi 3.14159265358979f
#define csmemset memset
#define RIDING_TICKS 60
#include "ride_types.inc"

/* a fake ground: stairs rising 3 wu at 5, 10, 15 and 22 wu from the origin (so no level run starts inside, and every 70 wu
run from the ring at 20 ends over the step at 22: the ring at 30 is the nearest that has one);
rough_all keeps the stairs rising every 5 wu everywhere, rough_east only where x is over 1, rough_lane only on a
patch at (27, 3) (the start of the convoy lane 4 wu left of the ring at 30's first spot), rough_near one at (29.3, .7)
(the start of the lane 1 wu left of it, where the second vehicle ends up); rough_cells rises in squares 76 wu across
(3 wu across each one's x edge, 5 across its y; no stairs), so no straight line is level for more than the square's diagonal, 107 wu */
static boolean rough_all;
static boolean rough_cells;
static boolean rough_wedge;
static boolean rough_far;
static boolean far_north;
static real sidehill;
static boolean rough_two;
static boolean rough_east;
static boolean rough_lane;
static boolean rough_near;
static struct {
	short scenario;
	boolean enabled, failed, ended, prepared, control_ready, corner, have_spot, door_started;
	char session[64];
	long before_tick;
	long after_tick;
	long board_tick;
	long door_tick;
	long vehicles[2];
	real convoy_offset;
	real convoy_yaw;
	real_point3d convoy_origin;
	short pilot_step;
	real pair, exit_forward, exit_left, tilt_yaw, spot_x, spot_y;
	struct ride_player players[RIDE_PLAYERS];
} ride;

struct data_iterator { long datum_index; };
struct player_datum { long unit_index; short local_player_index; };
struct biped_datum {
	struct { unsigned long flags; } biped;
	struct { real_point3d position; real_vector3d translational_velocity; real body_vitality, shield_vitality; } object;
};
struct vehicle_riding_deck { short ticks, node_index, flags; long vehicle_index; real_point3d place; real_vector3d velocity; real peak; };
static struct vehicle_riding_deck reported_deck;
#define _biped_airborne_bit 0
#define _object_mask_biped 1
#define _object_mask_vehicle 2
#define _object_at_rest_bit 0
#define _object_type_biped 1
#define MAXIMUM_LOCAL_PLAYERS 4
struct object_datum {
	long definition_index;
	struct {
		short type;
		long parent_object_index;
		real_point3d position;
		real_vector3d forward, up, translational_velocity, angular_velocity;
		unsigned long flags;
	} object;
};
static struct object_datum objects[4];
static struct { struct { unsigned char on_ground_ticks; } vehicle; } test_vehicle;
#define vehicle_datum_get(index) (&test_vehicle)
static long now = 1800;
static long ramp_opens;
static long ramp_tick;
void unit_open(long index)
{
	(void)index;
	ramp_opens++;
	ramp_tick = now;
}
#define test_tick now
#define test_objects objects
#define test_player players[0]
#define test_second_player players[1]
#define _network_test_local 1
static struct { short mode; } network_test;
static void *player_data;
static struct player_datum players[2];
static long local_player_get_player_index(short index) { return index; }
static struct player_datum *player_try_and_get(long index) { return &players[index]; }
static struct biped_datum bipeds[2];
static boolean decks[2];
static boolean host;
static boolean board_failed;
static boolean reboard_error;
static boolean ride_host(void) { return host; }
static void data_iterator_new(struct data_iterator *iterator, void *data) { iterator->datum_index = NONE; }
static void *data_iterator_next(struct data_iterator *iterator)
{
	iterator->datum_index++;
	return iterator->datum_index < 2 ? &players[iterator->datum_index] : NULL;
}
static void *object_try_and_get_and_verify_type(long unit, long mask) { return mask == _object_mask_biped ? (void *)&bipeds[unit] : (void *)&objects[unit]; }
static struct object_datum *object_get(long index) { return &objects[index]; }
static struct { struct { real runtime_minimum_normal_k; } biped; } definition = { { .70710678f } };
#define biped_definition_get(index) (&definition)
static void biped_get_physics_pill(long index, real_point3d *base, real *height, real *width)
{
	*base = objects[index].object.position;
	base->z += .2f;
	*height = .3f;
	*width = .2f;
}
static void unit_exit_seat_end(long index) { objects[index].object.parent_object_index = NONE; }
static void object_set_position(long index, real_point3d const *position, real_vector3d const *forward, real_vector3d const *up)
{
	objects[index].object.position = *position;
	if (forward) objects[index].object.forward = *forward;
	if (up) objects[index].object.up = *up;
}
static boolean biped_riding_deck(long unit, struct vehicle_riding_deck *deck) { *deck = reported_deck; return decks[unit]; }
static long shield_calls;
static boolean vehicle_riding_enabled(void) { return TRUE; }
static void network_distributed_own_reboard(short local_player_index, long unit_index, long vehicle_index) { shield_calls++; }
static boolean ride_board(long unit, long vehicle, short ordinal)
{
	if (board_failed)
		return FALSE;
	decks[unit] = TRUE;
	return TRUE;
}
/* Capsule fit is exercised with real point geometry in network_ride_clearance. */
static boolean ride_board_clear(long vehicle, real_point3d const *surface, real height, real width) { return TRUE; }
static void ride_error(char const *reason) { reboard_error = TRUE; }
static char last_sample[4096];
static short support_failures;
static void ride_log(char const *kind, char const *format, ...)
{
	va_list args;
	if (!strcmp(kind, "support_failure")) support_failures++;
	if (strcmp(kind, "sample")) return;
	va_start(args, format);
	vsnprintf(last_sample, sizeof(last_sample), format, args);
	va_end(args);
}
static char const *ride_phases[NUMBER_OF_RIDE_PHASES] = { "prepare", "stand", "walk", "jump", "cross", "step-off", "observe" };
static boolean game_in_progress(void) { return TRUE; }
static boolean game_time_menu_scene(void) { return FALSE; }
static long game_time_get(void) { return now; }
static void ride_reset(void) { }
static char const *config_string(char const *key) { return ""; }
static boolean ride_local_ready(void) { return TRUE; }
static boolean ride_players_ready(void) { return TRUE; }
static boolean ride_clock_tick(boolean h, boolean ready, long tick, FILE *clock, char const *session, long *board) { return TRUE; }
static boolean ride_select_vehicles(void) { return TRUE; }
static boolean ride_assign_driver(boolean preboard) { return TRUE; }
static void ride_survey(long tick) { }
static void ride_tilt_motion(long vehicle_index, long elapsed, real_vector3d *velocity, real_vector3d *angular)
{
	velocity->i = velocity->j = velocity->k = angular->i = angular->j = angular->k = 0.f;
}
static boolean ride_find_vehicles(void) { ride.vehicles[0] = 1; return TRUE; }
static short preparations;
static short sample_calls;
static boolean ride_prepare(void) { preparations++; return TRUE; }
static real_vector3d sample_velocity[2], sample_angular[2];
static boolean samples_fail;
static void ride_samples(long tick)
{
	short index;

	sample_calls++;
	for (index = 0; index < 2; index++)
	{
		sample_velocity[index] = objects[index].object.translational_velocity;
		sample_angular[index] = objects[index].object.angular_velocity;
	}
	if (samples_fail)
		ride.failed = TRUE;
}
static short ride_rider_count(void) { return 1; }
static void game_set_players_are_double_speed(boolean on) { }
static short ride_vehicle_count(void) { return ride.scenario == _ride_convoy ? 2 : 1; }
static short activations;
static void object_activate(long index) { activations++; }
static real ride_heading(long vehicle, real_vector3d *forward)
{
	if (ride.scenario == _ride_longsword)
	{
		forward->i = 1.f;
		forward->j = forward->k = 0.f;
		return .5f;
	}
	*forward = objects[vehicle].object.forward;
	return (real)atan2(forward->j, forward->i);
}
static void cross_product3d(real_vector3d const *a, real_vector3d const *b, real_vector3d *c)
{
	c->i = a->j*b->k - a->k*b->j;
	c->j = a->k*b->i - a->i*b->k;
	c->k = a->i*b->j - a->j*b->i;
}
static real normalize3d(real_vector3d *v)
{
	real length = (real)sqrt(v->i*v->i + v->j*v->j + v->k*v->k);
	v->i /= length;
	v->j /= length;
	v->k /= length;
	return length;
}
static void vector_from_points3d(real_point3d const *a, real_point3d const *b, real_vector3d *v)
{
	v->i = b->x - a->x;
	v->j = b->y - a->y;
	v->k = b->z - a->z;
}
static real dot_product3d(real_vector3d const *a, real_vector3d const *b) { return a->i*b->i + a->j*b->j + a->k*b->k; }
static boolean ride_height(long index, real_point3d const *base, real pill, real radius, real *height) { *height = 0.f; return TRUE; }
static void vehicle_riding_point_to_local(real_point3d const *origin, real_vector3d const *forward,
	real_vector3d const *up, real_point3d const *point, real_point3d *local) { *local = *point; }
static real_vector3d recovery_angular, recovery_linear;
struct vehicle_riding_vehicle { real_point3d position; real_vector3d forward, up, angular_velocity, translational_velocity; };
#define game_players_are_double_speed() FALSE
/* The full angular velocity calculation is exercised in network_ride_clearance. */
static void vehicle_riding_deck_point_velocity(struct vehicle_riding_vehicle const *v, real_point3d const *point, boolean twice, real_vector3d *out) { memset(out, 0, sizeof(*out)); }
static boolean biped_riding_vehicle(long index, short node, struct vehicle_riding_vehicle *v)
{
	v->position = objects[index].object.position;
	v->forward = objects[index].object.forward;
	v->up = objects[index].object.up;
	v->angular_velocity = recovery_angular;
	v->translational_velocity = recovery_linear;
	return TRUE;
}
static void vehicle_riding_point_to_world(real_point3d const *origin, real_vector3d const *forward,
	real_vector3d const *up, real_point3d const *local, real_point3d *point)
{
	real_vector3d left;

	cross_product3d(up, forward, &left);
	point->x = origin->x + forward->i*local->x + left.i*local->y + up->i*local->z;
	point->y = origin->y + forward->j*local->x + left.j*local->y + up->j*local->z;
	point->z = origin->z + forward->k*local->x + left.k*local->y + up->k*local->z;
}
/* A narrow bed and a wider, separate front wheel. A ray through the centre
misses the wheel; it must still limit how close another hull can stand. */
static boolean ride_deck_surface(long index, real_point3d const *point, real room, real normal, real_point3d *surface)
{
	real x = point->x - objects[index].object.position.x;
	real y = point->y - objects[index].object.position.y;
	boolean wheel = fabs(x - .5f) < .001f;

	if (fabs(x) > .901f || fabs(y) > (wheel ? .751f : .551f) || (wheel && fabs(y) < .3f))
		return FALSE;
	*surface = *point;
	surface->z = objects[index].object.position.z + .3f;
	return TRUE;
}

static boolean ride_ground_height(
	real_point3d const *point,
	real *height)
{
	real radius;

	if (rough_far && point->x > 205.f && (!far_north || point->y > 40.f))
	{
		*height = 0.f;
		return TRUE;
	}
	if (rough_two)
	{
		/* a hillside across the x axis, 0.3 a wu left of x 40 and 0.1 right of it (the axis itself level) */
		*height = (point->x < 40.f ? .3f : .1f)*point->y;
		return TRUE;
	}
	if (sidehill != 0.f)
	{
		/* a slope across the x axis: level along it, rising sidehill wu for each wu to its left */
		*height = sidehill*point->y;
		return TRUE;
	}
	if (rough_wedge)
	{
		/* a step across the axis from (100, 0) 90 wu east, and one right of the heading 10 degrees right of it (the one
		10 degrees left is clear) */
		*height = (point->x > 190.f && fabs((double)point->y) < 3.) || (point->x > 180.f && point->y > -25.f && point->y < -10.f) ? 3.f : 0.f;
		return TRUE;
	}
	if (rough_cells)
	{
		*height = 3.f*(real)floor((double)(point->x/76.f)) + 5.f*(real)floor((double)(point->y/76.f));
		return TRUE;
	}
	radius = (real)sqrt((double)(point->x*point->x + point->y*point->y));
	if (rough_all || (rough_east && point->x > 1.f) || (rough_near && point->y > .6f && point->y < .8f && point->x > 29.2f && point->x < 29.4f)
		|| (rough_lane && point->y > 2.f && point->y < 4.f && point->x > 26.f && point->x < 28.5f))
		*height = 3.f*(real)floor((double)((radius + .001f)/5.f));
	else
		*height = 3.f*((radius >= 5.f) + (radius >= 10.f) + (radius >= 15.f) + (radius >= 22.f));
	return TRUE;
}

/* (the preparation fixture covers the script suspension and the delivery monitor) */
static void ride_suspend_scripts(void) {}
static void ride_monitor_controls(long tick) {}
#include "ride_under_test.inc"

/* a rider as network_test_ride_action keeps it: the last jump pressed and its tick, and the presses counted */
struct rider
{
	short last_jump;
	long press_tick;
	short presses;
	long first_press;
};

/* the input asked at elapsed ticks (the tick's presses counted once, as the game's caller does), and whether it jumps */
static boolean rider_asks(
	struct rider *rider,
	long elapsed)
{
	struct player_action action;
	boolean jump;

	ride_script_action(_ride_straight, elapsed, FALSE, 0.f, rider->last_jump, rider->press_tick == elapsed, FALSE, 0.f, NONE, &action);
	jump = TEST_FLAG(action.control_flags, _unit_control_jump_bit);
	if (jump && rider->press_tick != elapsed)
	{
		rider->press_tick = elapsed;
		rider->last_jump = ride_jump_slot(elapsed);
		if (!rider->presses++)
			rider->first_press = elapsed;
	}
	return jump;
}

static void rider_start(
	struct rider *rider)
{
	rider->last_jump = NONE;
	rider->press_tick = NONE;
	rider->presses = 0;
	rider->first_press = NONE;
}

/* whether the measured deck has every cell a point touches (x from -1.0 and y from -0.6, in tenths) */
static boolean deck_has(
	char const *const *deck,
	real x,
	real y)
{
	short row_low = (short)floor((double)(x*10.f + 10.f) + .001);
	short row_high = (short)ceil((double)(x*10.f + 10.f) - .001);
	short column_low = (short)floor((double)(y*10.f + 6.f) + .001);
	short column_high = (short)ceil((double)(y*10.f + 6.f) - .001);
	short row;
	short column;

	for (row = row_low; row <= row_high; row++)
	{
		for (column = column_low; column <= column_high; column++)
		{
			if (row < 0 || row >= 21 || column < 0 || column >= 13 || deck[row][column] != 'X')
				return FALSE;
		}
	}
	return TRUE;
}

/* a host drive of a ride scenario at elapsed ticks from the board tick, with the board tick at 100 and the vehicles held still on the ground facing .3 radians (the second beside the first in a convoy) */
static void drive_setup(
	short scenario,
	long elapsed)
{
	host = TRUE;
	memset(&ride, 0, sizeof(ride));
	memset(objects, 0, sizeof(objects));
	ride.enabled = ride.prepared = TRUE;
	ride.scenario = scenario;
	ride.board_tick = 100;
	ride.before_tick = ride.after_tick = NONE;
	ride.vehicles[0] = 0;
	ride.vehicles[1] = scenario == _ride_convoy ? 1 : NONE;
	ride.convoy_yaw = .3f;
	ride.convoy_offset = 1.1f;
	objects[0].object.forward.i = (real)cos(.3);
	objects[0].object.forward.j = (real)sin(.3);
	objects[0].object.up.k = 1.f;
	objects[1].object = objects[0].object;
	objects[1].object.position.x = -.4f;
	objects[1].object.position.y = 1.1f;
	now = 100 + elapsed;
	sample_calls = 0;
	samples_fail = FALSE;
}

/* the engine's step, modelled: a vehicle that is not at rest loses 5% of its velocity and 7% of its turn, and moves by what is left */
static void drive_step(void)
{
	short index;

	for (index = 0; index < 2; index++)
	{
		struct object_datum *object = &objects[index];

		if (TEST_FLAG(object->object.flags, _object_at_rest_bit))
			continue;
		object->object.translational_velocity.i *= .95f;
		object->object.translational_velocity.j *= .95f;
		object->object.translational_velocity.k *= .95f;
		object->object.angular_velocity.k *= .93f;
		object->object.position.x += object->object.translational_velocity.i;
		object->object.position.y += object->object.translational_velocity.j;
		object->object.position.z += object->object.translational_velocity.k;
	}
}

#define NEAR(a, b) CHECK(fabs((double)(a) - (double)(b)) < .00001, "%s: %g, not %g", #a, (double)(a), (double)(b))

int main(int argc, char **argv)
{
	char const *case_name = argc > 1 ? argv[1] : "";
	real_vector3d forward = { 1.f, 0.f, 0.f };
	real_vector3d velocity;
	real_vector3d angular;

	CASE("ride-contact-wiring")
	{
		ride.players[0].assigned = 2;
		ride.players[0].contact_vehicle = 2; ride.players[0].contact_tick = 90;
		ride.players[0].contact_valid = TRUE; ride.players[0].contact_normal.k = 1.f;
		decks[0] = TRUE; reported_deck.vehicle_index = 2; reported_deck.ticks = 60;
		ride_rider_log("sample", 0, 0, 90, TRUE);
		CHECK(strstr(last_sample, "height_valid=1 "), "current physical contact rejected");
		ride_rider_log("sample", 0, 0, 91, TRUE);
		CHECK(strstr(last_sample, "height_valid=0 "), "stale contact accepted by call-site wiring");
		ride.players[0].contact_tick = 92; ride.players[0].contact_valid = FALSE;
		ride_rider_log("sample", 0, 0, 92, TRUE);
		CHECK(strstr(last_sample, "height_valid=0 "), "ticks 60 fabricated physical contact");
		ride.players[0].contact_valid = TRUE; ride.players[0].contact_tick = 93;
		bipeds[0].biped.flags = FLAG(_biped_airborne_bit);
		ride_rider_log("sample", 0, 0, 93, TRUE);
		CHECK(strstr(last_sample, "height_valid=0 "), "airborne contact accepted by call-site wiring");
		bipeds[0].biped.flags = 0; ride.players[0].contact_vehicle = 3; ride.players[0].contact_tick = 94;
		ride_rider_log("sample", 0, 0, 94, TRUE);
		CHECK(strstr(last_sample, "height_valid=0 "), "another object's contact accepted"); return 0;
	}
	CASE("ride-contact-transaction")
	{
		real_point3d point = { 0.f, 0.f, .2f };
		real_vector3d normal = { 0.f, 0.f, 1.f };
		ride.enabled = ride.prepared = TRUE; ride.players[0].unit = 0;
		now = 90;
		network_test_ride_contact_begin(0);
		network_test_ride_contact(0, 2, 0, 4, &point, &normal, .2f);
		network_test_ride_contact_end(0, 3, TRUE);
		CHECK(!ride.players[0].contact_valid, "end accepted another final support object");
		network_test_ride_contact(0, 2, 0, 4, &point, &normal, .2f);
		network_test_ride_contact_end(0, 2, FALSE);
		CHECK(!ride.players[0].contact_valid, "end accepted an airborne output");
		network_test_ride_contact(0, 2, 0, 4, &point, &normal, .2f);
		now = 91; network_test_ride_contact_end(0, 2, TRUE);
		CHECK(!ride.players[0].contact_valid, "end accepted stale contact");
		network_test_ride_contact(0, 2, 0, 4, &point, &normal, .2f);
		network_test_ride_contact_begin(0);
		CHECK(!ride.players[0].contact_valid, "begin retained prior contact"); return 0;
	}
	CASE("ride-setup-contact-age")
	{
		host = TRUE; players[0].unit_index = 0; players[1].unit_index = NONE;
		ride.players[0].unit = 0; ride.players[0].assigned = 2;
		ride.players[0].contact_valid = TRUE; ride.players[0].contact_vehicle = 2;
		ride.players[0].contact_tick = 89;
		CHECK(ride_setup_supported(90), "immediately preceding physics was rejected");
		ride.players[0].contact_tick = 88;
		CHECK(!ride_setup_supported(90), "setup accepted stale physical contact"); return 0;
	}
	CASE("ride-observation-disabled")
	{
		byte before[sizeof(ride)];
		real_point3d point = { 0.f, 0.f, .2f };
		real_vector3d normal = { 0.f, 0.f, 1.f };
		short mode;
		for (mode = 0; mode < 2; mode++) {
			memset(&ride, 0, sizeof(ride));
			ride.enabled = mode != 0; ride.prepared = mode == 0;
			ride.players[0].unit = 0; ride.players[0].local = TRUE;
			ride.board_tick = 300; now = 1140;
			memcpy(before, &ride, sizeof(ride));
			network_test_ride_contact_begin(0);
			network_test_ride_contact(0, 2, 0, 4, &point, &normal, .2f);
			network_test_ride_contact_end(0, 2, TRUE);
			network_test_ride_launch(0);
			CHECK(!memcmp(before, &ride, sizeof(ride)), "observation hooks changed state while disabled or unprepared");
		}
		return 0;
	}
	CASE("ride-stock-preacquisition")
	{
		short scenario;
		for (scenario = _ride_straight; scenario <= _ride_coop_double; scenario++) {
			if (scenario == _ride_driven) continue;
			memset(&ride, 0, sizeof(ride)); preparations = sample_calls = 0;
			ride.enabled = ride.control_ready = TRUE; ride.scenario = scenario;
			ride.board_tick = 300; ride.before_tick = NONE; ride.vehicles[0] = 2;
			objects[2].object.forward.i = objects[2].object.up.k = 1.f;
			host = TRUE; now = 240;
			network_test_ride_before_objects();
			CHECK(ride.prepared && preparations == 1, "scenario %d did not prepare before scored start", scenario);
			CHECK(objects[2].object.translational_velocity.i == 0.f && objects[2].object.translational_velocity.j == 0.f &&
				objects[2].object.translational_velocity.k == 0.f, "vehicle moved during acquisition");
			network_test_ride_after_objects();
			CHECK(!sample_calls, "acquisition entered scored coverage");
		}
		return 0;
	}
	CASE("ride-setup-corrections")
	{
		real_point3d p = { 0.f, 0.f, 0.f };
		ride.enabled = ride.prepared = TRUE; ride.board_tick = 300;
		now = 240;
		network_test_ride_correction(0, 0, &p, &p);
		CHECK(ride.players[0].tick_corrections == 0, "pre-score correction has no scored sample");
		now = 300;
		network_test_ride_correction(0, 0, &p, &p);
		CHECK(ride.players[0].tick_corrections == 1, "scored correction disappeared"); return 0;
	}
	CASE("ride-first-jump-supported")
	{
		ride.enabled = ride.prepared = TRUE; ride.scenario = _ride_brush; ride.board_tick = 300;
		ride.before_tick = NONE; ride.vehicles[0] = 2; ride.players[0].unit = 0; ride.players[0].assigned = 2;
		players[0].unit_index = 0; players[1].unit_index = NONE;
		ride.players[0].contact_valid = FALSE; ride.players[0].contact_tick = 1139;
		now = 1140; network_test_ride_before_objects();
		CHECK(!reboard_error && support_failures == 1, "unsupported first launch did not record a scored failure");
		network_test_ride_after_objects();
		CHECK(sample_calls == 1, "unsupported first launch stopped later scoring"); return 0;
	}
	CASE("ride-board-support-required")
	{
		ride.enabled = ride.prepared = TRUE; ride.scenario = _ride_rise; ride.board_tick = 300;
		ride.before_tick = NONE; ride.vehicles[0] = 2; ride.players[0].unit = 0; ride.players[0].assigned = 2;
		players[0].unit_index = 0; players[1].unit_index = NONE;
		now = 300; network_test_ride_before_objects();
		CHECK(reboard_error && support_failures == 0, "unsupported initial setup was not a setup error"); return 0;
	}
	CASE("ride-setup-shuffle")
	{
		struct player_action action;
		long elapsed;
		for (elapsed = RIDE_WALK; elapsed < RIDE_JUMP; elapsed++) {
			real throttle = ((elapsed - RIDE_WALK)/RIDE_WALK_PERIOD) & 1 ? -RIDE_WALK_THROTTLE : RIDE_WALK_THROTTLE;
			ride_script_action(_ride_brush, elapsed, FALSE, 0.f, NONE, FALSE, FALSE, 0.f, NONE, &action);
			CHECK(action.throttle.i == throttle, "brush changed its original 240-tick shuffle");
			ride_script_action(_ride_rise, elapsed, FALSE, 0.f, NONE, FALSE, FALSE, 0.f, NONE, &action);
			CHECK(action.throttle.i == throttle, "rise changed its original 240-tick shuffle");
			/* (the original route while the rider stays inside the range) */
			ride_script_action(_ride_brush, elapsed, FALSE, 0.f, NONE, FALSE, FALSE, RIDE_WALK_RANGE*.5f, NONE, &action);
			CHECK(action.throttle.i == throttle, "brush left its route inside the range");
			ride_script_action(_ride_rise, elapsed, FALSE, 0.f, NONE, FALSE, FALSE, -RIDE_WALK_RANGE*.5f, NONE, &action);
			CHECK(action.throttle.i == throttle, "rise left its route inside the range");
		}
		return 0;
	}
	CASE("ride-walk-anchored-shuffle")
	{
		static short const scenarios[7] = { _ride_straight, _ride_circle, _ride_brush, _ride_gun, _ride_coop, _ride_coop_double, _ride_convoy };
		static real const offsets[7] = { 0.f, RIDE_WALK_RANGE*.99f, -RIDE_WALK_RANGE*.99f, RIDE_WALK_RANGE, -RIDE_WALK_RANGE, RIDE_WALK_RANGE*3.f, -RIDE_WALK_RANGE*3.f };
		struct player_action action;
		short which;
		short at;
		long elapsed;

		for (which = 0; which < 7; which++)
		{
			short scenario = scenarios[which];
			boolean inverted = scenario == _ride_gun || scenario == _ride_coop || scenario == _ride_coop_double || scenario == _ride_convoy;

			for (at = 0; at < 7; at++)
			{
				for (elapsed = RIDE_WALK; elapsed < RIDE_JUMP; elapsed++)
				{
					real nominal = ((elapsed - RIDE_WALK)/RIDE_WALK_PERIOD) & 1 ? -RIDE_WALK_THROTTLE : RIDE_WALK_THROTTLE;
					real expected = inverted ? -nominal : nominal;

					/* (in deck coordinates, after the scenario's inversion: positive throttle is +x for every scenario) */
					if (offsets[at] >= RIDE_WALK_RANGE)
						expected = -RIDE_WALK_THROTTLE;
					else if (offsets[at] <= -RIDE_WALK_RANGE)
						expected = RIDE_WALK_THROTTLE;
					ride_script_action(scenario, elapsed, FALSE, 0.f, NONE, FALSE, FALSE, offsets[at], NONE, &action);
					CHECK(action.throttle.i == expected, "scenario %d offset %g tick %ld: throttle %g, wanted %g", scenario,
						(double)offsets[at], elapsed, (double)action.throttle.i, (double)expected);
					CHECK(fabs(action.throttle.i) == RIDE_WALK_THROTTLE && action.throttle.j == 0.f, "scenario %d amplitude", scenario);
				}
			}
		}
		return 0;
	}
	CASE("ride-jump-recentre")
	{
		static short const scenarios[7] = { _ride_straight, _ride_circle, _ride_brush, _ride_gun, _ride_coop, _ride_coop_double, _ride_convoy };
		static real const offsets[4] = { RIDE_WALK_RANGE*3.f, -RIDE_WALK_RANGE*3.f, RIDE_WALK_RANGE*.5f, 0.f };
		static real const wanted[4] = { -RIDE_WALK_THROTTLE, RIDE_WALK_THROTTLE, 0.f, 0.f };
		struct player_action action;
		short which;
		short at;
		short air;
		long elapsed;

		for (which = 0; which < 7; which++)
		{
			for (at = 0; at < 4; at++)
			{
				for (air = 0; air < 2; air++)
				{
					short last_jump = NONE;
					short presses = 0;
					long press_tick = NONE;

					for (elapsed = RIDE_JUMP; elapsed < RIDE_CROSS; elapsed++)
					{
						long into = (elapsed - RIDE_JUMP) % RIDE_JUMP_PERIOD;
						boolean pressed;
						real expected = air ? 0.f : wanted[at];

						ride_script_action(scenarios[which], elapsed, FALSE, 0.f, last_jump, press_tick == elapsed, !air, offsets[at], NONE, &action);
						pressed = TEST_FLAG(action.control_flags, _unit_control_jump_bit);
						/* (the presses stay on the schedule, once each, and the press tick never walks) */
						CHECK(pressed == (into == 0), "scenario %d tick %ld: press %d", scenarios[which], elapsed, (int)pressed);
						if (pressed)
						{
							CHECK(action.throttle.i == 0.f && action.throttle.j == 0.f, "walking on a press tick %ld", elapsed);
							press_tick = elapsed;
							last_jump = ride_jump_slot(elapsed);
							presses++;
							/* (asked again on the same tick) */
							ride_script_action(scenarios[which], elapsed, FALSE, 0.f, last_jump, TRUE, !air, offsets[at], NONE, &action);
							CHECK(action.throttle.i == 0.f, "walking on a repeated press tick %ld", elapsed);
							continue;
						}
						if (into >= RIDE_JUMP_PERIOD - RIDE_JUMP_SETTLE)
							expected = 0.f;
						CHECK(action.throttle.i == expected && action.throttle.j == 0.f,
							"scenario %d offset %g airborne %d tick %ld (%ld into the period): throttle %g, wanted %g", scenarios[which],
							(double)offsets[at], (int)air, elapsed, into, (double)action.throttle.i, (double)expected);
					}
					CHECK(presses == RIDE_JUMPS, "scenario %d: %d presses", scenarios[which], (int)presses);
				}
			}
		}
		CHECK(RIDE_JUMP_SETTLE >= 10, "a settle of %d ticks", RIDE_JUMP_SETTLE);
		return 0;
	}
	CASE("ride-convoy-step-off-outboard")
	{
		struct player_action action;
		short scenarios[] = { _ride_straight, _ride_circle, _ride_rise, _ride_gun, _ride_brush, _ride_coop, _ride_coop_double, _ride_convoy };
		short which;
		short lane;

		ride.enabled = ride.prepared = TRUE;
		ride.board_tick = 300;
		ride.vehicles[0] = 2;
		ride.vehicles[1] = 3;
		objects[2].object.forward.i = 1.f;
		test_second_player.unit_index = 1;
		objects[1].object.parent_object_index = NONE;
		ride.players[1].unit = 1;
		ride.players[1].local = TRUE;
		ride.players[1].assigned = 2;
		decks[1] = TRUE;
		reported_deck.ticks = RIDING_TICKS;
		for (which = 0; which < (short)(sizeof(scenarios)/sizeof(scenarios[0])); which++)
		{
			ride.scenario = scenarios[which];
			test_tick = 300 + ride_phase_tick(ride.scenario, RIDE_STEP_OFF);
			for (lane = 0; lane < 2; lane++)
			{
				real side = ride.scenario == _ride_convoy && lane == 1 ? 1.f : -1.f;

				reported_deck.vehicle_index = ride.vehicles[lane];
				network_test_ride_action(1, &action);
				NEAR(action.throttle.j, side*RIDE_SIDE_THROTTLE);
				CHECK(action.throttle.i == 0.f && action.control_flags == 0, "only lateral step-off");
				NEAR(ride_step_off_side(ride.scenario, ride.vehicles[lane]), side);
				/* (the last deck remains the departure frame after its contact window ends) */
				ride.players[1].previous_vehicle = ride.vehicles[lane];
				decks[1] = FALSE;
				network_test_ride_action(1, &action);
				NEAR(action.throttle.j, side*RIDE_SIDE_THROTTLE);
				decks[1] = TRUE;
			}
		}
		return 0;
	}
	CASE("ride-jump-recentre-caller")
	{
		struct player_action action;
		long tick = 300 + RIDE_JUMP + 5;

		ride.enabled = ride.prepared = TRUE;
		ride.scenario = _ride_circle;
		ride.board_tick = 300;
		ride.vehicles[0] = 2;
		objects[2].object.forward.i = 1.f;
		test_second_player.unit_index = 1;
		objects[1].object.parent_object_index = NONE;
		ride.players[1].unit = 1;
		ride.players[1].local = TRUE;
		ride.players[1].last_jump = 0;
		ride.players[1].last_press_tick = 300 + RIDE_JUMP;
		decks[1] = TRUE;
		reported_deck.ticks = RIDING_TICKS;
		reported_deck.vehicle_index = 2;
		reported_deck.place.x = .7f;
		/* (the walk fixes the origin) */
		test_tick = 300 + RIDE_WALK;
		network_test_ride_action(1, &action);
		/* (grounded on the deck, past the range, between jumps: back toward the origin) */
		test_tick = tick;
		reported_deck.place.x = .7f + 2.f*RIDE_WALK_RANGE;
		network_test_ride_action(1, &action);
		NEAR(action.throttle.i, -RIDE_WALK_THROTTLE);
		reported_deck.place.x = .7f - 2.f*RIDE_WALK_RANGE;
		network_test_ride_action(1, &action);
		NEAR(action.throttle.i, RIDE_WALK_THROTTLE);
		reported_deck.place.x = .7f + 2.f*RIDE_WALK_RANGE;
		/* (in the air, or without the deck's full support: nothing) */
		bipeds[1].biped.flags = 1;
		network_test_ride_action(1, &action);
		NEAR(action.throttle.i, 0.f);
		bipeds[1].biped.flags = 0;
		reported_deck.ticks = RIDING_TICKS - 1;
		network_test_ride_action(1, &action);
		NEAR(action.throttle.i, 0.f);
		reported_deck.ticks = RIDING_TICKS;
		/* (another node is another frame) */
		reported_deck.node_index = 1;
		network_test_ride_action(1, &action);
		NEAR(action.throttle.i, 0.f);
		reported_deck.node_index = 0;
		/* (a copy that does not own the rider, or with no deck) */
		ride.players[1].local = FALSE;
		network_test_ride_action(1, &action);
		NEAR(action.throttle.i, 0.f);
		ride.players[1].local = TRUE;
		decks[1] = FALSE;
		network_test_ride_action(1, &action);
		NEAR(action.throttle.i, 0.f);
		decks[1] = TRUE;
		network_test_ride_action(1, &action);
		NEAR(action.throttle.i, -RIDE_WALK_THROTTLE);
		return 0;
	}
	CASE("ride-walk-offset-owner-only")
	{
		struct player_action action;
		long walk = 300 + RIDE_WALK;

		ride.enabled = ride.prepared = TRUE;
		ride.scenario = _ride_circle;
		ride.board_tick = 300;
		ride.vehicles[0] = 2;
		objects[2].object.forward.i = 1.f;
		test_second_player.unit_index = 1;
		objects[1].object.parent_object_index = NONE;
		ride.players[1].unit = 1;
		ride.players[1].local = TRUE;
		ride.players[1].last_jump = NONE;
		ride.players[1].last_press_tick = NONE;
		decks[1] = TRUE;
		reported_deck.ticks = RIDING_TICKS;
		reported_deck.vehicle_index = 2;
		reported_deck.place.x = .7f;
		/* (the owner: the offset is its place since the walk began) */
		test_tick = walk;
		network_test_ride_action(1, &action);
		NEAR(action.throttle.i, RIDE_WALK_THROTTLE);
		reported_deck.place.x = .7f + RIDE_WALK_RANGE;
		network_test_ride_action(1, &action);
		NEAR(action.throttle.i, -RIDE_WALK_THROTTLE);
		reported_deck.place.x = .7f - RIDE_WALK_RANGE;
		network_test_ride_action(1, &action);
		NEAR(action.throttle.i, RIDE_WALK_THROTTLE);
		reported_deck.place.x = .7f + RIDE_WALK_RANGE*.5f;
		test_tick = walk + RIDE_WALK_PERIOD;
		network_test_ride_action(1, &action);
		NEAR(action.throttle.i, -RIDE_WALK_THROTTLE);
		/* (another node is another frame: the walk's origin is taken again there) */
		reported_deck.node_index = 1;
		reported_deck.place.x = .7f + 5.f*RIDE_WALK_RANGE;
		test_tick = walk;
		network_test_ride_action(1, &action);
		NEAR(action.throttle.i, RIDE_WALK_THROTTLE);
		/* (a copy that does not own the rider passes nothing, however far the deck says it is) */
		ride.players[1].local = FALSE;
		reported_deck.place.x = .7f + 10.f*RIDE_WALK_RANGE;
		test_tick = walk;
		network_test_ride_action(1, &action);
		NEAR(action.throttle.i, RIDE_WALK_THROTTLE);
		reported_deck.place.x = .7f - 10.f*RIDE_WALK_RANGE;
		network_test_ride_action(1, &action);
		NEAR(action.throttle.i, RIDE_WALK_THROTTLE);
		/* (the owner with no deck passes nothing either) */
		ride.players[1].local = TRUE;
		decks[1] = FALSE;
		reported_deck.place.x = .7f + 10.f*RIDE_WALK_RANGE;
		network_test_ride_action(1, &action);
		NEAR(action.throttle.i, RIDE_WALK_THROTTLE);
		return 0;
	}
	CASE("ride-phases")
	{
		CHECK(ride_phase(_ride_straight, -1) == _ride_phase_prepare && ride_phase(_ride_straight, 0) == _ride_phase_stand, "the start");
		CHECK(ride_phase(_ride_straight, 599) == _ride_phase_stand && ride_phase(_ride_straight, 600) == _ride_phase_walk, "twenty seconds standing");
		CHECK(ride_phase(_ride_straight, 839) == _ride_phase_walk && ride_phase(_ride_straight, 840) == _ride_phase_jump, "the walk");
		CHECK(ride_phase(_ride_straight, 1739) == _ride_phase_jump && ride_phase(_ride_straight, 1740) == _ride_phase_cross, "the jumps");
		CHECK(ride_phase(_ride_straight, 1919) == _ride_phase_cross && ride_phase(_ride_straight, 1920) == _ride_phase_step_off, "the crossing");
		CHECK(ride_phase(_ride_straight, 2039) == _ride_phase_step_off && ride_phase(_ride_straight, 2040) == _ride_phase_observe, "the step off");
		/* the Longsword: stand through its settle, then its own programme (no walk, jump phase or reboard) */
		CHECK(ride_phase(_ride_longsword, RIDE_TILT_SETTLE - 1) == _ride_phase_stand &&
			ride_phase(_ride_longsword, RIDE_JUMP + 5) == _ride_phase_observe, "the Longsword's phases");
		return 0;
	}
	CASE("ride-driven-deck-landings")
	{
		long tick;
		ride.scenario = _ride_driven;
		ride.board_tick = 300;
		ride.players[0].board_tick = 301;
		ride.players[0].assigned = 2;
		ride.players[0].previous_ticks = 0;
		ride.players[0].recovering = ride.players[0].recovery_placed = TRUE;
		ride_outcome_launch(&ride.players[0], 1499);
		decks[0] = TRUE;
		reported_deck.vehicle_index = 2;
		reported_deck.ticks = 60;
		for (tick = 1500; tick < 1506; tick++)
		{
			ride_rider_log("sample", 0, 0, tick, TRUE);
			CHECK(ride.players[0].landings == 0, "recovery acquisition counted as a deck landing");
		}
		CHECK(!ride.players[0].recovering, "recovery did not acquire");
		/* (the next airborne turn really lands, after the recovery marker is spent) */
		reported_deck.ticks = 59;
		ride_outcome_launch(&ride.players[0], 1600);
		ride_rider_log("sample", 0, 0, 1600, TRUE);
		reported_deck.ticks = 60;
		ride.players[0].contact_valid = TRUE; ride.players[0].contact_tick = 1601;
		ride.players[0].contact_vehicle = 2; ride.players[0].contact_normal.k = 1.f;
		ride_rider_log("sample", 0, 0, 1601, TRUE);
		CHECK(ride.players[0].landings == 1, "natural deck landing after recovery was lost");
		ride.scenario = _ride_circle;
		ride.players[0].recovery_placed = TRUE;
		ride.players[0].previous_ticks = 59;
		ride_outcome_launch(&ride.players[0], 1602);
		ride.players[0].contact_tick = 1603;
		ride_rider_log("sample", 0, 0, 1603, TRUE);
		CHECK(ride.players[0].landings == 2, "non-driven landing rule changed");
		return 0;
	}
	CASE("ride-driven-post-physics")
	{
		long tick;
		ride.scenario = _ride_driven;
		ride.board_tick = 300;
		players[0].unit_index = 0;
		players[1].unit_index = 1;
		ride.players[0].unit = 0;
		ride.players[1].unit = NONE;
		ride.players[0].assigned = 2;
		ride.players[0].recovering = ride.players[0].recovery_placed = TRUE;
		ride.players[0].recovery_start = 1924;
		decks[0] = TRUE;
		reported_deck.vehicle_index = 2;
		for (tick = 1998; tick <= 2004; tick++)
		{
			/* (a remote state says 60 before the physics step loses contact for one tick) */
			reported_deck.ticks = 60;
			ride_driven_recover(tick);
			reported_deck.ticks = tick == 1998 ? 59 : 60;
			ride_rider_log("sample", 0, 0, tick, FALSE);
			if (tick == 1998)
				CHECK(ride.players[0].recovery_contacts == 0, "pre-physics support counted despite post-physics tick59");
			if (tick == 2003)
				CHECK(ride.players[0].recovering, "five sampled contacts logged as six");
		}
		CHECK(!ride.players[0].recovering && ride.players[0].recovery_ready == 2010,
			"six completed supporting samples did not acquire with the existing hold");
		return 0;
	}
	CASE("ride-driven-prepare-before-score")
	{
		struct player_action action;
		ride.enabled = ride.control_ready = TRUE;
		ride.scenario = _ride_driven;
		ride.board_tick = 300;
		ride.vehicles[0] = 2;
		ride.players[0].unit = ride.players[1].unit = NONE;
		players[1].unit_index = 1;
		objects[1].object.parent_object_index = 2;
		objects[2].object.forward.i = objects[2].object.up.k = 1.f;
		host = TRUE;
		now = 239;
		network_test_ride_before_objects();
		CHECK(!preparations, "preparation earlier than its fixed setup lead");
		now = 240;
		network_test_ride_before_objects();
		CHECK(ride.prepared && preparations == 1, "host placement left until the first scored tick");
		network_test_ride_after_objects();
		CHECK(!sample_calls, "setup became part of scored coverage");
		network_test_ride_action(1, &action);
		CHECK(action.throttle.i == 0.f && action.throttle.j == 0.f && TEST_FLAG(action.control_flags, _unit_control_jump_bit),
			"pilot drives before the unchanged board tick");
		now = 300;
		network_test_ride_before_objects();
		network_test_ride_after_objects();
		CHECK(preparations == 1 && sample_calls == 1, "setup repeated or board sample skipped");
		network_test_ride_action(1, &action);
		CHECK(action.throttle.i == RIDE_DRIVE_THROTTLE, "pilot never started at board tick");
		ride.scenario = _ride_circle;
		ride.prepared = FALSE;
		ride.before_tick = 0;
		preparations = 0;
		now = 240;
		network_test_ride_before_objects();
		CHECK(preparations == 1, "non-driven preparation did not acquire before scoring");
		now = 300;
		network_test_ride_before_objects();
		CHECK(preparations == 1, "non-driven board preparation missing");
		return 0;
	}
	CASE("ride-driven-recovery-budget")
	{
		struct player_action action;
		long tick;
		long previous = NONE;
		short count = 0;
		short scenario;
		CHECK(ride_phase_tick(_ride_driven, RIDE_END) == 4260 && ride_phase_tick(_ride_circle, RIDE_END) == 2400,
			"end did not shift with the driven phases only");
		CHECK(ride_phase(_ride_driven, 3599) == _ride_phase_jump && ride_phase(_ride_driven, 3600) == _ride_phase_cross,
			"driven jump phase lacks ten bounded recovery holds");
		CHECK(ride_phase(_ride_driven, 3779) == _ride_phase_cross && ride_phase(_ride_driven, 3780) == _ride_phase_step_off,
			"driven crossing did not shift intact");
		CHECK(ride_phase(_ride_driven, 3899) == _ride_phase_step_off && ride_phase(_ride_driven, 3900) == _ride_phase_observe,
			"driven observe did not shift intact");
		for (scenario = 0; scenario < NUMBER_OF_RIDE_SCENARIOS; scenario++)
		{
			if (scenario == _ride_driven || scenario == _ride_longsword)
				continue;
			CHECK(ride_phase(scenario, 1739) == _ride_phase_jump && ride_phase(scenario, 1740) == _ride_phase_cross &&
				ride_phase(scenario, 1920) == _ride_phase_step_off &&
				ride_phase(scenario, scenario == _ride_brush ? 1950 : 2040) == _ride_phase_observe,
				"non-driven schedule changed");
		}
		ride.enabled = ride.prepared = TRUE;
		ride.scenario = _ride_driven;
		ride.board_tick = 300;
		ride.vehicles[0] = 2;
		objects[2].object.forward.i = objects[2].object.up.k = 1.f;
		players[0].unit_index = 0;
		players[0].local_player_index = 0;
		ride.players[0].unit = 0;
		ride.players[1].unit = NONE;
		ride.players[0].last_jump = ride.players[0].last_press_tick = NONE;
		/* (ten full recovery budgets, each delaying a due press by 180 plus its six-tick hold) */
		ride.players[0].recovery_ready = 1326;
		for (tick = 1140; tick < 3905; tick++)
		{
			now = tick;
			network_test_ride_action(0, &action);
			if (!TEST_FLAG(action.control_flags, _unit_control_jump_bit))
				continue;
			CHECK(previous == NONE || tick - previous >= 90, "recovery backlog shortened the jump period");
			CHECK(tick < 3900 && count < 10, "extra press or press after the extended phase");
			previous = tick;
			count++;
			ride.players[0].recovery_ready = tick + 90 + 186;
		}
		CHECK(count == 10 && previous == 3810, "ten recoveries left fewer than ten normal-period jumps");
		return 0;
	}
	CASE("ride-driven-deferred-presses")
	{
		struct player_action action;
		long tick;
		long previous = NONE;
		short count = 0;
		ride.enabled = ride.prepared = TRUE;
		ride.scenario = _ride_driven;
		ride.board_tick = 300;
		ride.vehicles[0] = 2;
		objects[2].object.forward.i = objects[2].object.up.k = 1.f;
		players[0].unit_index = 0;
		players[0].local_player_index = 0;
		ride.players[0].unit = 0;
		ride.players[1].unit = NONE;
		ride.players[0].last_jump = NONE;
		ride.players[0].last_press_tick = NONE;
		for (tick = 1140; tick < 300 + ride_phase_tick(_ride_driven, RIDE_CROSS) + 5; tick++)
		{
			now = tick;
			/* (miss the 1230 slot, release after acquisition's hold at 1236) */
			ride.players[0].recovering = tick >= 1220 && tick < 1230;
			ride.players[0].recovery_ready = tick >= 1220 ? 1236 : 0;
			network_test_ride_action(0, &action);
			if (!TEST_FLAG(action.control_flags, _unit_control_jump_bit))
				continue;
			CHECK(tick < 1220 || tick >= 1236, "pressed before acquisition hold ended");
			CHECK(previous == NONE || tick - previous >= 90, "backlog pressed faster than normal period");
			CHECK(tick < 300 + ride_phase_tick(_ride_driven, RIDE_CROSS), "backlog widened the jump phase");
			previous = tick;
			count++;
		}
		CHECK(count == 10 && ride.players[0].presses == 10, "suppressed jump was skipped rather than reissued");
		/* (a recovery lasting past the phase leaves the required outcome short) */
		ride.players[0].presses = 9;
		now = 300 + ride_phase_tick(_ride_driven, RIDE_CROSS);
		network_test_ride_action(0, &action);
		CHECK(!TEST_FLAG(action.control_flags, _unit_control_jump_bit) && ride.players[0].presses == 9,
			"missed jump made up outside its phase");
		return 0;
	}
	CASE("ride-driven-recovers")
	{
		long tick;
		ride.enabled = ride.prepared = TRUE;
		ride.scenario = _ride_driven;
		ride.board_tick = 300;
		ride.vehicles[0] = 2;
		players[0].unit_index = 0;
		players[0].local_player_index = 0;
		players[1].unit_index = 1;
		ride.players[0].unit = 0;
		ride.players[0].assigned = 2;
		ride.players[0].jumps = 1;
		ride.players[1].unit = NONE;
		reported_deck.vehicle_index = 2;
		reported_deck.ticks = 60;
		host = TRUE;
		for (tick = 1180; tick < 1220; tick++)
		{
			now = tick;
			network_test_ride_before_objects();
		}
		CHECK(decks[0], "missed driven jump never recovered");
		return 0;
	}
	CASE("ride-driven-steady")
	{
		struct player_action action;
		long tick;
		ride.enabled = ride.prepared = TRUE;
		ride.scenario = _ride_driven;
		ride.board_tick = 300;
		ride.vehicles[0] = 2;
		players[0].unit_index = 0;
		players[1].unit_index = 1;
		players[1].local_player_index = 1;
		objects[1].object.parent_object_index = 2;
		objects[2].object.forward.i = objects[2].object.up.k = 1.f;
		ride.players[0].unit = 0;
		ride.players[0].assigned = 2;
		ride.players[0].jumps = 1;
		ride.players[1].unit = NONE;
		host = TRUE;
		for (tick = 1180; tick < 1185; tick++)
		{
			now = tick;
			network_test_ride_before_objects();
		}
		network_test_ride_action(1, &action);
		CHECK(action.desired_facing.yaw == 0.f, "recovery pilot still turns");
		CHECK(action.throttle.i == 0.f && action.throttle.j == 0.f,
			"recovery pilot keeps throttle");
		CHECK(TEST_FLAG(action.control_flags, _unit_control_jump_bit), "recovery pilot does not brake");
		/* (the same brake through acquisition's hold, then the original drive) */
		ride.players[0].recovering = FALSE;
		ride.players[0].recovery_ready = now + 6;
		now += 5;
		network_test_ride_action(1, &action);
		CHECK(action.throttle.i == 0.f && TEST_FLAG(action.control_flags, _unit_control_jump_bit),
			"brake released during acquisition hold");
		now++;
		network_test_ride_action(1, &action);
		CHECK(action.throttle.i == RIDE_DRIVE_THROTTLE && action.desired_facing.yaw == RIDE_DRIVE_TURN &&
			!TEST_FLAG(action.control_flags, _unit_control_jump_bit), "scripted drive did not resume after hold");
		return 0;
	}
	CASE("ride-driven-waits-for-motion")
	{
		long tick;
		ride.scenario = _ride_driven;
		ride.board_tick = 300;
		players[0].unit_index = 0;
		players[1].unit_index = 1;
		ride.players[0].unit = 0;
		ride.players[0].assigned = 2;
		ride.players[0].jumps = 1;
		ride.players[1].unit = NONE;
		host = TRUE;
		recovery_angular.k = .07f;
		for (tick = 1180; tick < 1200; tick++)
			ride_driven_recover(tick);
		CHECK(!decks[0], "placed before angular motion settled");
		recovery_angular.k = 0.f;
		recovery_linear.i = .1f;
		for (; tick < 1220; tick++)
			ride_driven_recover(tick);
		CHECK(!decks[0], "placed before linear motion settled");
		recovery_linear.i = 0.f;
		for (; tick < 1225; tick++)
			ride_driven_recover(tick);
		CHECK(!decks[0], "placed before six stopped ticks");
		for (; tick < 1228; tick++)
			ride_driven_recover(tick);
		CHECK(decks[0], "steady vehicle never placed");
		return 0;
	}
	CASE("ride-driven-no-jump")
	{
		long tick;
		ride.scenario = _ride_driven;
		ride.board_tick = 300;
		players[0].unit_index = 0;
		players[1].unit_index = 1;
		ride.players[0].unit = 0;
		ride.players[1].unit = NONE;
		host = TRUE;
		for (tick = 1180; tick < 1220; tick++)
			ride_driven_recover(tick);
		CHECK(!decks[0], "recovery without a jump departure");
		return 0;
	}
	CASE("ride-driven-timeout")
	{
		long tick;
		ride.scenario = _ride_driven;
		ride.board_tick = 300;
		players[0].unit_index = 0;
		players[1].unit_index = 1;
		ride.players[0].unit = 0;
		ride.players[0].jumps = 1;
		ride.players[1].unit = NONE;
		host = TRUE;
		for (tick = 1180; tick < 1363; tick++)
			ride_driven_recover(tick);
		CHECK(reboard_error, "unacquired recovery never times out");
		return 0;
	}
	CASE("ride-driven-limit")
	{
		long tick;
		ride.scenario = _ride_driven;
		ride.board_tick = 300;
		players[0].unit_index = 0;
		players[1].unit_index = 1;
		ride.players[0].unit = 0;
		ride.players[0].jumps = 1;
		ride.players[0].recoveries = 10;
		ride.players[1].unit = NONE;
		host = TRUE;
		for (tick = 1180; tick < 1183; tick++)
			ride_driven_recover(tick);
		CHECK(reboard_error, "unbounded recoveries");
		return 0;
	}
	CASE("ride-reboard-authority")
	{
		short index;

		ride.scenario = _ride_convoy;
		ride.board_tick = 300;
		for (index = 0; index < 2; index++)
		{
			players[index].unit_index = index;
			players[index].local_player_index = index ? NONE : 0;
			ride.players[index].unit = index;
			ride.players[index].ordinal = index;
			ride.players[index].reboard_cycle = NONE;
			ride.players[index].off_tick = NONE;
		}
		/* A setup teleport must move the host's remote copy too: its prediction
		validator cannot accept a client jumping several wu back onto a vehicle. */
		host = TRUE;
		ride_reboard(1180);
		CHECK(ride.players[0].reboarded && ride.players[1].reboarded, "host did not reboard both players");
		CHECK(ride.players[1].landed_off == 0 && ride.players[1].landings == 0 && ride.players[1].reboard_cycle == 0,
			"remote placement invented an unobserved landing");
		/* A client still changes only its own player; remote state is replicated. */
		host = FALSE;
		for (index = 0; index < 2; index++)
		{
			decks[index] = FALSE;
			ride.players[index].reboarded = FALSE;
			ride.players[index].reboard_cycle = NONE;
		}
		ride_reboard(1180);
		CHECK(ride.players[0].reboarded && !ride.players[1].reboarded, "client changed a remote player");
		host = TRUE;
		ride.scenario = _ride_straight;
		decks[1] = FALSE;
		ride_reboard(1180);
		CHECK(!ride.players[1].reboarded, "another scenario's setup changed");
		board_failed = TRUE;
		decks[0] = FALSE;
		ride.players[0].reboard_cycle = NONE;
		ride_reboard(1270);
		CHECK(reboard_error, "failed setup teleport was ignored");
		return 0;
	}
	CASE("ride-brush-observe")
	{
		CHECK(ride_phase(_ride_brush, 1949) == _ride_phase_step_off, "brush step truncated");
		CHECK(ride_phase(_ride_brush, 1950) == _ride_phase_observe, "brush contact occurs before observation");
		return 0;
	}
	CASE("ride-motion")
	{
		/* straight: its speed until the fifth jump, a brake over 60 ticks, then still */
		ride_motion(_ride_straight, 1199, &forward, &velocity, &angular);
		NEAR(velocity.i, RIDE_STRAIGHT_SPEED);
		ride_motion(_ride_straight, 1230, &forward, &velocity, &angular);
		NEAR(velocity.i, RIDE_STRAIGHT_SPEED*.5f);
		ride_motion(_ride_straight, 1260, &forward, &velocity, &angular);
		NEAR(velocity.i, 0.f);
		/* circle, gun and co-op: a circle, all the time */
		ride_motion(_ride_circle, 10, &forward, &velocity, &angular);
		NEAR(velocity.i, RIDE_CIRCLE_SPEED);
		NEAR(angular.k, RIDE_CIRCLE_TURN);
		ride_motion(_ride_gun, 2000, &forward, &velocity, &angular);
		NEAR(angular.k, RIDE_CIRCLE_TURN);
		ride_motion(_ride_coop_double, 2300, &forward, &velocity, &angular);
		NEAR(velocity.i, RIDE_CIRCLE_SPEED);
		/* rise: up, held, down, still */
		ride_motion(_ride_rise, 299, &forward, &velocity, &angular);
		NEAR(velocity.k, RIDE_RISE_SPEED);
		NEAR(velocity.i, 0.f);
		ride_motion(_ride_rise, 300, &forward, &velocity, &angular);
		NEAR(velocity.k, 0.f);
		ride_motion(_ride_rise, 600, &forward, &velocity, &angular);
		NEAR(velocity.k, -RIDE_RISE_SPEED);
		ride_motion(_ride_rise, 900, &forward, &velocity, &angular);
		NEAR(velocity.k, 0.f);
		/* convoy: straight on; brush: still until the riders stand beside it */
		ride_motion(_ride_convoy, 1800, &forward, &velocity, &angular);
		NEAR(velocity.i, RIDE_CONVOY_SPEED);
		ride_motion(_ride_brush, 1949, &forward, &velocity, &angular);
		NEAR(velocity.i, 0.f);
		NEAR(velocity.j, 0.f);
		ride_motion(_ride_brush, 1950, &forward, &velocity, &angular);
		NEAR(velocity.i, 0.f);
		NEAR(velocity.j, -RIDE_BRUSH_SPEED);
		NEAR(angular.k, 0.f);
		ride_motion(_ride_brush, 2080, &forward, &velocity, &angular);
		NEAR(velocity.i, RIDE_BRUSH_SPEED);
		NEAR(velocity.j, 0.f);
		/* driven: its driver's throttle alone */
		ride_motion(_ride_driven, 100, &forward, &velocity, &angular);
		NEAR(velocity.i, 0.f);
		NEAR(angular.k, 0.f);
		/* along the vehicle's heading */
		forward.i = 0.f;
		forward.j = 1.f;
		ride_motion(_ride_convoy, 10, &forward, &velocity, &angular);
		NEAR(velocity.j, RIDE_CONVOY_SPEED);
		NEAR(velocity.i, 0.f);
		/* (before its board tick it stands, held where it was placed: the ground may slope) */
		ride_motion(_ride_convoy, -1, &forward, &velocity, &angular);
		NEAR(velocity.j, 0.f);
		NEAR(velocity.i, 0.f);
		ride_motion(_ride_convoy, 0, &forward, &velocity, &angular);
		NEAR(velocity.j, RIDE_CONVOY_SPEED);
		return 0;
	}
	CASE("ride-jump-late")
	{
		struct rider rider;
		long elapsed;
		long jump;

		/* every tick asked: each scheduled tick presses, RIDE_JUMPS in all */
		rider_start(&rider);
		for (elapsed = 0; elapsed < RIDE_END; elapsed++)
		{
			boolean due = elapsed >= RIDE_JUMP && elapsed < RIDE_CROSS && (elapsed - RIDE_JUMP) % RIDE_JUMP_PERIOD == 0;

			CHECK(rider_asks(&rider, elapsed) == due, "tick %ld", elapsed);
		}
		CHECK(rider.presses == RIDE_JUMPS && rider.first_press == RIDE_JUMP, "%d jumps, the first at %ld", rider.presses, rider.first_press);
		/* a frame that skipped the scheduled tick (1110, the fourth jump's): the first request after it presses */
		rider_start(&rider);
		for (elapsed = 0; elapsed < RIDE_JUMP + 3*RIDE_JUMP_PERIOD; elapsed++)
			rider_asks(&rider, elapsed);
		CHECK(rider.presses == 3, "%d jumps before the skip", rider.presses);
		for (jump = 0; jump < 7; jump++)
			CHECK(!rider_asks(&rider, RIDE_JUMP + 3*RIDE_JUMP_PERIOD - 1), "a jump before its tick");
		CHECK(rider_asks(&rider, RIDE_JUMP + 3*RIDE_JUMP_PERIOD + 5), "the skipped jump lost");
		CHECK(rider.presses == 4 && rider.press_tick == RIDE_JUMP + 3*RIDE_JUMP_PERIOD + 5, "%d jumps", rider.presses);
		/* (the tick's input asked again still jumps, and counts once; the next tick does not) */
		CHECK(rider_asks(&rider, RIDE_JUMP + 3*RIDE_JUMP_PERIOD + 5) && rider.presses == 4, "a repeated ask pressed twice or not at all");
		CHECK(!rider_asks(&rider, RIDE_JUMP + 3*RIDE_JUMP_PERIOD + 6), "the jump held");
		/* its own scheduled tick asked repeatedly: one press */
		rider_start(&rider);
		for (jump = 0; jump < 8; jump++)
			CHECK(rider_asks(&rider, RIDE_JUMP), "the repeated ask at the scheduled tick");
		CHECK(rider.presses == 1, "%d presses", rider.presses);
		/* jumps whose slots were never asked in are not made up later: only the asked slot's own */
		rider_start(&rider);
		CHECK(rider_asks(&rider, RIDE_JUMP + 5*RIDE_JUMP_PERIOD) && rider.presses == 1, "the next slot's jump");
		CHECK(!rider_asks(&rider, RIDE_JUMP + 5*RIDE_JUMP_PERIOD + 1) && rider.presses == 1, "two jumps made up");
		/* nor after the jump phase: the last jump's slot (1650 to 1739) never asked in, asked at the crossing */
		rider_start(&rider);
		CHECK(!rider_asks(&rider, RIDE_CROSS) && !rider_asks(&rider, RIDE_CROSS + 1) && rider.presses == 0, "a jump pressed after the jumps");
		return 0;
	}
	CASE("ride-script")
	{
		struct player_action action;
		long elapsed;

		for (elapsed = 0; elapsed < RIDE_END; elapsed++)
		{
			ride_script_action(_ride_straight, elapsed, FALSE, .5f, NONE, FALSE, FALSE, 0.f, NONE, &action);
			CHECK(action.desired_facing.yaw == .5f && action.desired_weapon_index == NONE, "facing or weapon at %ld", elapsed);
		}
		ride_script_action(_ride_straight, 1200, FALSE, 0.f, NONE, FALSE, FALSE, 0.f, NONE, &action);
		CHECK(TEST_FLAG(action.control_flags, _unit_control_jump_bit), "no jump at the brake");
		/* walking forward, then back */
		ride_script_action(_ride_circle, 600, FALSE, 0.f, NONE, FALSE, FALSE, 0.f, NONE, &action);
		NEAR(action.throttle.i, RIDE_WALK_THROTTLE);
		ride_script_action(_ride_circle, 600 + RIDE_WALK_PERIOD - 1, FALSE, 0.f, NONE, FALSE, FALSE, 0.f, NONE, &action);
		NEAR(action.throttle.i, RIDE_WALK_THROTTLE);
		ride_script_action(_ride_circle, 600 + RIDE_WALK_PERIOD, FALSE, 0.f, NONE, FALSE, FALSE, 0.f, NONE, &action);
		NEAR(action.throttle.i, -RIDE_WALK_THROTTLE);
		ride_script_action(_ride_circle, 600 + 2*RIDE_WALK_PERIOD, FALSE, 0.f, NONE, FALSE, FALSE, 0.f, NONE, &action);
		NEAR(action.throttle.i, RIDE_WALK_THROTTLE);
		/* the gun's shuffle starts away from the mount and returns, never ahead of its boarding spot */
		ride_script_action(_ride_gun, 600, FALSE, 0.f, NONE, FALSE, FALSE, 0.f, NONE, &action);
		NEAR(action.throttle.i, -RIDE_WALK_THROTTLE);
		ride_script_action(_ride_gun, 600 + RIDE_WALK_PERIOD, FALSE, 0.f, NONE, FALSE, FALSE, 0.f, NONE, &action);
		NEAR(action.throttle.i, RIDE_WALK_THROTTLE);
		ride_script_action(_ride_coop, 600, FALSE, 0.f, NONE, FALSE, FALSE, 0.f, NONE, &action);
		NEAR(action.throttle.i, -RIDE_WALK_THROTTLE);
		ride_script_action(_ride_coop_double, 600 + RIDE_WALK_PERIOD, FALSE, 0.f, NONE, FALSE, FALSE, 0.f, NONE, &action);
		NEAR(action.throttle.i, RIDE_WALK_THROTTLE);
		ride_script_action(_ride_convoy, 600, FALSE, 0.f, NONE, FALSE, FALSE, 0.f, NONE, &action);
		NEAR(action.throttle.i, -RIDE_WALK_THROTTLE);
		/* a leg moves a rider at most 0.1 wu (0.011 wu a tick at this throttle, measured on the Warthog's bed, which at
		y 0 runs x -0.9 to -0.3: the ride-board case checks every spot and a leg from it on the deck) */
		CHECK(RIDE_WALK_PERIOD*.011f <= .1f, "a walking leg of %d ticks", RIDE_WALK_PERIOD);
		/* crossing: the convoy's riders to the left, nobody else */
		ride_script_action(_ride_convoy, 1800, FALSE, 0.f, NONE, FALSE, FALSE, 0.f, NONE, &action);
		NEAR(action.throttle.j, RIDE_SIDE_THROTTLE);
		ride_script_action(_ride_circle, 1800, FALSE, 0.f, NONE, FALSE, FALSE, 0.f, NONE, &action);
		NEAR(action.throttle.j, 0.f);
		/* stepping off to the right; brush's riders only a step */
		ride_script_action(_ride_circle, 2000, FALSE, 0.f, NONE, FALSE, FALSE, 0.f, NONE, &action);
		NEAR(action.throttle.j, -RIDE_SIDE_THROTTLE);
		ride_script_action(_ride_brush, 1949, FALSE, 0.f, NONE, FALSE, FALSE, 0.f, NONE, &action);
		NEAR(action.throttle.j, -RIDE_SIDE_THROTTLE);
		ride_script_action(_ride_brush, 1950, FALSE, 0.f, NONE, FALSE, FALSE, 0.f, NONE, &action);
		NEAR(action.throttle.j, 0.f);
		/* a driver: its throttle and a facing off the vehicle's, never a jump */
		ride_script_action(_ride_driven, 1200, TRUE, .5f, NONE, FALSE, FALSE, 0.f, NONE, &action);
		NEAR(action.throttle.i, RIDE_DRIVE_THROTTLE);
		NEAR(action.desired_facing.yaw, .5f + RIDE_DRIVE_TURN);
		CHECK(!TEST_FLAG(action.control_flags, _unit_control_jump_bit), "a driver jumped");
		/* the generic script stays neutral; the Longsword's pilot uses its own input programme */
		ride_script_action(_ride_longsword, RIDE_WALK + 3, TRUE, .5f, NONE, FALSE, FALSE, 0.f, NONE, &action);
		CHECK(action.throttle.i == 0.f && action.throttle.j == 0.f && action.desired_facing.yaw == .5f, "the pilot drives");
		return 0;
	}
	CASE("ride-brush-path")
	{
		/* observed host centres after stepping right, in the vehicle's starting frame */
		static real const riders[2][2] = { { -.7224f, -.6231f }, { .7429f, -.8213f } };
		real x = 0.f, y = 0.f, yaw = 0.f;
		boolean touched[2] = { FALSE, FALSE };
		real maximum_speed = 0.f;
		long tick;
		short rider;

		for (tick = RIDE_STEP_OFF; tick < 2400; tick++)
		{
			forward.i = (real)cos(yaw);
			forward.j = (real)sin(yaw);
			ride_motion(_ride_brush, tick, &forward, &velocity, &angular);
			if (sqrt((double)(velocity.i*velocity.i + velocity.j*velocity.j)) > maximum_speed)
				maximum_speed = (real)sqrt((double)(velocity.i*velocity.i + velocity.j*velocity.j));
			x += velocity.i;
			y += velocity.j;
			yaw += angular.k;
			for (rider = 0; rider < 2; rider++)
			{
				real dx = riders[rider][0] - x, dy = riders[rider][1] - y;
				real along = dx*(real)cos(yaw) + dy*(real)sin(yaw);
				real across = -dx*(real)sin(yaw) + dy*(real)cos(yaw);

				/* contact with the flank plus pill, not a centre deep inside the hull.
				Both contacts must occur within 60 ticks of stepping off. */
				if (tick < RIDE_STEP_OFF + 60 && fabs(along) <= .9f && fabs(fabs(across) - .5f) <= .03f)
					touched[rider] = TRUE;
			}
		}
		CHECK(touched[0] && touched[1], "brush misses rear=%d front=%d", !touched[0], !touched[1]);
		CHECK(y >= -.35f && y <= -.32f, "brush rams sideways: %f", y);
		CHECK(maximum_speed <= .035, "brush is faster than a walk");
		CHECK(x > 5.f, "the vehicle never passes the riders");
		return 0;
	}
	CASE("ride-level-choice")
	{
		CHECK(ride_level_scenario(_ride_straight), "straight has no level run");
		CHECK(ride_level_scenario(_ride_convoy), "convoy has no level run");
		CHECK(ride_level_scenario(_ride_brush), "brush starts on the tilted spawn bank");
		CHECK(ride_level_scenario(_ride_coop) && ride_level_scenario(_ride_coop_double), "co-op boards on the tilted campaign spawn");
		CHECK(!ride_level_scenario(_ride_circle) && !ride_level_scenario(_ride_gun), "circle moved unnecessarily");
		return 0;
	}
	CASE("ride-height")
	{
		real height = 99.f;

		/* a ray from z 3 down 6 hitting halfway: the deck at 0; a pill whose
		base (the lower sphere's centre) is its radius 0.2 over it stands on it */
		CHECK(ride_height_from_hit(TRUE, .5f, 3.f, -6.f, .2f, .2f, &height), "a hit refused");
		NEAR(height, 0.f);
		/* sunk 0.15 into it */
		CHECK(ride_height_from_hit(TRUE, .5f, 3.f, -6.f, .05f, .2f, &height), "a hit refused");
		NEAR(height, -.15f);
		/* no hit, or a hit's part not a number: none, never 0 */
		height = 99.f;
		CHECK(!ride_height_from_hit(FALSE, .5f, 3.f, -6.f, .2f, .2f, &height) && height == 99.f, "a miss measured");
		CHECK(!ride_height_from_hit(TRUE, NAN, 3.f, -6.f, .2f, .2f, &height) && height == 99.f, "no part measured");
		return 0;
	}
	CASE("ride-level-spot")
	{
		real_point3d position = { 0.f, 0.f, 0.f };
		real yaw = -1.f;

		/* no level run where it stands, and every run from the ring at 20 crosses the step at 22: the ring at 30's first point,
		kept the vehicle's rest height (0.6) over its ground, facing the first yaw with a level run ahead and behind (due
		east has the step at 22 behind it) */
		CHECK(ride_level_spot(&position, .6f, 0.f, RIDE_LEVEL_RUN, RIDE_SEARCH_RADIUS, 0.f, &yaw), "no spot found");
		NEAR(position.x, 30.f);
		NEAR(position.y, 0.f);
		NEAR(position.z, 12.f + .6f);
		NEAR(yaw, _pi/4.f);
		/* a level run where it stands: not moved, but its heading needs the ground behind level too (due east has the
		stairs at 22 wu behind it) */
		position.x = 30.f;
		position.y = 0.f;
		position.z = 12.f;
		CHECK(ride_level_spot(&position, .6f, 0.f, RIDE_LEVEL_RUN, RIDE_SEARCH_RADIUS, 0.f, &yaw), "the level spot refused");
		NEAR(position.x, 30.f);
		NEAR(position.z, 12.f);
		NEAR(yaw, _pi/4.f);
		/* a level run where it stands, a long way from the stairs: not moved */
		position.x = 40.f;
		position.y = 0.f;
		position.z = 12.f;
		CHECK(ride_level_spot(&position, .6f, 0.f, RIDE_LEVEL_RUN, RIDE_SEARCH_RADIUS, 0.f, &yaw), "the level spot refused");
		NEAR(position.x, 40.f);
		NEAR(position.z, 12.f);
		NEAR(yaw, 0.f);
		/* the east rough: the first ring's point whose heading runs west onto the flat, not yaw 0 */
		rough_east = TRUE;
		position.x = position.y = position.z = 0.f;
		CHECK(ride_level_spot(&position, .6f, 0.f, RIDE_LEVEL_RUN, RIDE_SEARCH_RADIUS, 0.f, &yaw), "no spot found");
		NEAR(position.x, 20.f*(real)cos(_pi*3.f/8.f));
		NEAR(position.y, 20.f*(real)sin(_pi*3.f/8.f));
		NEAR(position.z, 12.f + .6f);
		NEAR(yaw, _pi*7.f/8.f);
		return 0;
	}
	CASE("ride-level-lane")
	{
		real_point3d position = { 0.f, 0.f, 0.f };
		real yaw = -1.f;

		/* the same spot, but a convoy's lane to its left starts on a step: the next yaw, whose lane is level */
		rough_lane = TRUE;
		CHECK(ride_level_spot(&position, .6f, RIDE_CONVOY_LANE, RIDE_LEVEL_RUN, RIDE_SEARCH_RADIUS, 0.f, &yaw), "no spot found");
		NEAR(position.x, 30.f);
		NEAR(position.z, 12.f + .6f);
		NEAR(yaw, _pi*3.f/8.f);
		/* its own spot too: not moved, the yaw changed */
		position.x = 30.f;
		position.y = position.z = 0.f;
		position.z = 12.f;
		CHECK(ride_level_spot(&position, .6f, RIDE_CONVOY_LANE, RIDE_LEVEL_RUN, RIDE_SEARCH_RADIUS, 0.f, &yaw), "the level spot refused");
		NEAR(position.x, 30.f);
		NEAR(position.z, 12.f);
		NEAR(yaw, _pi*3.f/8.f);
		/* a step on the lane 1 wu left (where the second vehicle ends up), not the far one: the same yaw */
		rough_lane = FALSE;
		rough_near = TRUE;
		position.x = position.y = position.z = 0.f;
		CHECK(ride_level_spot(&position, .6f, RIDE_CONVOY_LANE, RIDE_LEVEL_RUN, RIDE_SEARCH_RADIUS, 0.f, &yaw), "no spot found");
		NEAR(position.x, 30.f);
		NEAR(yaw, _pi*3.f/8.f);
		/* without a lane, the step is not in the way */
		rough_lane = TRUE;
		position.x = position.y = position.z = 0.f;
		CHECK(ride_level_spot(&position, .6f, 0.f, RIDE_LEVEL_RUN, RIDE_SEARCH_RADIUS, 0.f, &yaw), "no spot found");
		NEAR(yaw, _pi/4.f);
		return 0;
	}
	CASE("ride-board")
	{
		/* the Warthog's measured deck (a rider's room over it counted), x from -1.0 to 1.0 down the rows, y from -0.6 to 0.6
		across the columns, in tenths */
		static char const *const deck[21] =
		{
			".............", "...XXXXXXX...", "..XXXXXXXXX..", "..XXXXXXXXX..", "...XXXXXXX...", "...XXXXXXX...", "....XXXXX....",
			"....XXXXX....", "..XXXX.XXXX..", "..XXXX.XXXX..", "..XXXX.XXXX..", "....XX.XX....", "....XXXXX....", "...X.XXX.X...",
			"...X.XXX.X...", "...XXXXXXX...", "...XXXXXXX...", "..XXXXXXXXX..", "....XXXXX....", ".............", ".............",
		};
		short scenario;

		for (scenario = _ride_straight; scenario <= _ride_coop_double; scenario++)
		{
			real_point3d spots[2][3];
			short ordinal;
			short index;

			for (ordinal = 0; ordinal < 2; ordinal++)
			{
				for (index = 0; index < 3; index++)
				{
					real x;
					real y;
					/* (a walking leg moves a rider 0.011 wu a tick, along the vehicle's x) */
					real leg = (real)RIDE_WALK_PERIOD*.011f;

					ride_board_spot(scenario, ordinal, index, &x, &y);
					spots[ordinal][index].x = x;
					spots[ordinal][index].y = y;
					CHECK(deck_has(deck, x, y) && deck_has(deck, x - leg, y) && (scenario == _ride_gun || deck_has(deck, x + leg, y)),
						"scenario %d rider %d candidate %d at (%g, %g), or a leg from it, is off the deck", scenario, ordinal, index,
						(double)x, (double)y);
					/* (the gun mount: the map's cells well above the bed's 0.23 to 0.31 (0.5 and over, x -0.5 to -0.3); a pill
					of radius 0.2 clears every one by a margin, a leg's ends too) */
					if (scenario == _ride_gun && ordinal == 0)
					{
						static real const mount[7][2] =
						{
							{ -.5f, 0.f }, { -.4f, -.1f }, { -.4f, 0.f }, { -.4f, .1f }, { -.3f, -.1f }, { -.3f, 0.f }, { -.3f, .1f },
						};
						short cell;

						/* live collision-feature probes at the spawn: x -0.75/+y overlaps the mount; -0.8 clears it */
						CHECK(x <= -.8f && x - leg >= -.9f, "gun shuffle enters the overlapping mount or leaves the bed: %g", (double)x);
						for (cell = 0; cell < 7; cell++)
						{
							real nearest = (real)sqrt((double)((x - mount[cell][0])*(x - mount[cell][0]) + (y - mount[cell][1])*(y - mount[cell][1])));
							real ahead = (real)sqrt((double)((x - leg - mount[cell][0])*(x - leg - mount[cell][0]) +
								(y - mount[cell][1])*(y - mount[cell][1])));

							CHECK(nearest >= .21 && ahead >= .21, "the gun's rider at (%g, %g) touches the mount cell %d", (double)x,
								(double)y, cell);
						}
					}
					if ((scenario == _ride_coop || scenario == _ride_coop_double) && ordinal == 0)
					{
						/* campaign's settled pitch: y 0 overlaps at -0.8; at y +0.2,
						-0.775 to -0.9 clear the full-pill collision features */
						CHECK(y == .2f && x <= -.8f && x - leg >= -.9f, "campaign pill overlaps the mount: %g,%g", x, y);
					}
					if (scenario == _ride_convoy)
					{
						CHECK(y == .2f, "convoy rider is not on the inner side: %g", y);
						if (!ordinal)
							CHECK(x <= -.8f && x - leg >= -.9f, "convoy setup pill touches the mount: %g", x);
					}
				}
			}
			/* (two pills 0.4 wide: their centres at least that far apart) */
			for (index = 0; index < 3; index++)
			{
				real across_x = spots[0][index].x - spots[1][index].x;
				real across_y = spots[0][index].y - spots[1][index].y;

				CHECK(sqrt((double)(across_x*across_x + across_y*across_y)) >= .4, "scenario %d candidate %d: the riders overlap", scenario, index);
			}
		}
		/* each scenario's intent: gun's rider under the gun on its left, brush's both on the right, the others in the bed
		and on the hood */
		ride_board_spot(_ride_gun, 0, 0, &forward.i, &forward.j);
		NEAR(forward.i, -.8f);
		NEAR(forward.j, .2f);
		ride_board_spot(_ride_brush, 1, 0, &forward.i, &forward.j);
		NEAR(forward.i, .6f);
		NEAR(forward.j, -.2f);
		ride_board_spot(_ride_circle, 0, 0, &forward.i, &forward.j);
		NEAR(forward.i, -.75f);
		NEAR(forward.j, 0.f);
		return 0;
	}
	CASE("ride-reboard")
	{
		long cycle_start = RIDE_JUMP + 3*RIDE_JUMP_PERIOD;

		/* a rider off the deck, on the ground, once the jump's air time has passed: due, once a cycle */
		CHECK(ride_reboard_due(cycle_start + RIDE_REBOARD_AFTER, FALSE, FALSE, 2), "not due");
		CHECK(!ride_reboard_due(cycle_start + 1, FALSE, FALSE, 2), "due at the jump");
		CHECK(!ride_reboard_due(cycle_start + 30, FALSE, FALSE, 2), "due before the air time");
		CHECK(!ride_reboard_due(cycle_start + RIDE_REBOARD_AFTER, FALSE, FALSE, 3), "due twice in a cycle");
		CHECK(ride_reboard_due(cycle_start + RIDE_JUMP_PERIOD + RIDE_REBOARD_AFTER, FALSE, FALSE, 3), "not due next cycle");
		CHECK(!ride_reboard_due(cycle_start + RIDE_REBOARD_AFTER, TRUE, FALSE, 2), "due in the air");
		CHECK(!ride_reboard_due(cycle_start + RIDE_REBOARD_AFTER, FALSE, TRUE, 2), "due on the deck");
		CHECK(!ride_reboard_due(RIDE_JUMP - 1, FALSE, FALSE, NONE), "due before the jumps");
		CHECK(!ride_reboard_due(RIDE_CROSS + RIDE_REBOARD_AFTER + 5, FALSE, FALSE, NONE), "due after the jumps");
		return 0;
	}
	CASE("ride-level-run")
	{
		real_point3d position = { 0.f, 0.f, 0.f };
		real yaw = -1.f;

		/* a scenario's level run covers the whole drive and 10 wu more: the convoy's 96 wu, straight's cruise and brake */
		CHECK(ride_level_run(_ride_convoy) >= RIDE_END*RIDE_CONVOY_SPEED + 10.f - .001f, "convoy's run %.1f", ride_level_run(_ride_convoy));
		CHECK(ride_level_run(_ride_straight) >= RIDE_STRAIGHT_SPEED*(RIDE_BRAKE + RIDE_BRAKE_TICKS/2.f) + 10.f - .001f,
			"straight's run %.1f", ride_level_run(_ride_straight));
		CHECK(ride_level_run(_ride_brush) >= RIDE_BRUSH_SPEED*(RIDE_END - RIDE_STEP_OFF - RIDE_BRUSH_STEP_TICKS) + 10.f - .001f,
			"brush's run %.1f", ride_level_run(_ride_brush));
		CHECK(ride_level_run(_ride_coop) >= 2.f*RIDE_CIRCLE_SPEED/RIDE_CIRCLE_TURN + 10.f - .001f, "co-op's run %.1f",
			ride_level_run(_ride_coop));
		/* ground level for no more than the run a short drive needs: a spot for that drive, none for the convoy's */
		rough_cells = TRUE;
		CHECK(ride_level_spot(&position, .6f, 0.f, ride_level_run(_ride_brush), RIDE_SEARCH_RADIUS, 0.f, &yaw), "the short drive's spot refused");
		position.x = position.y = position.z = 0.f;
		CHECK(!ride_level_spot(&position, .6f, 0.f, ride_level_run(_ride_convoy), RIDE_SEARCH_RADIUS, 0.f, &yaw), "the convoy's drive runs over a step");
		/* (turned left, it still has no run for the whole drive anywhere: refused, not accepted for its 70 wu one) */
		position.x = position.y = position.z = 0.f;
		CHECK(!ride_level_spot(&position, .6f, 0.f, ride_level_run(_ride_convoy), RIDE_SEARCH_RADIUS, ride_level_turn(_ride_convoy), &yaw),
			"the turned convoy's drive runs over a step");
		return 0;
	}
	CASE("ride-level-turn")
	{
		real_point3d position = { 100.f, 0.f, 0.f };
		real yaw = -1.f;

		CHECK(ride_level_turn(_ride_convoy) == RIDE_CONVOY_TURN && RIDE_CONVOY_TURN > .17f && RIDE_CONVOY_TURN < .175f,
			"the convoy's turn %f", ride_level_turn(_ride_convoy));
		CHECK(ride_level_turn(_ride_straight) == 0.f && ride_level_turn(_ride_brush) == 0.f && ride_level_turn(_ride_coop) == 0.f,
			"another scenario turned");
		/* a heading clear for the whole drive is kept as it is: the turn is only for one that is not */
		CHECK(ride_level_spot(&position, .6f, RIDE_CONVOY_LANE, 106.f, RIDE_SEARCH_RADIUS, RIDE_CONVOY_TURN, &yaw), "a clear heading refused");
		NEAR(position.x, 100.f);
		NEAR(yaw, 0.f);
		/* level for 70 wu due east, a step across it before 106; the heading ten degrees left is clear, the one ten degrees right is not */
		rough_wedge = TRUE;
		CHECK(ride_level_spot(&position, .6f, RIDE_CONVOY_LANE, 106.f, RIDE_SEARCH_RADIUS, 0.f, &yaw) && !(position.x == 100.f && position.y == 0.f && yaw == 0.f),
			"the fixture's own heading is clear for 106");
		position.x = 100.f;
		position.y = position.z = 0.f;
		CHECK(ride_level_spot(&position, .6f, RIDE_CONVOY_LANE, 106.f, RIDE_SEARCH_RADIUS, RIDE_CONVOY_TURN, &yaw), "the turned heading refused");
		NEAR(position.x, 100.f);
		NEAR(position.y, 0.f);
		NEAR(yaw, RIDE_CONVOY_TURN);
		return 0;
	}
	CASE("ride-level-cross")
	{
		real_point3d position = { 0.f, 0.f, 0.f };
		real yaw = -1.f;

		/* a slope across the heading (level along it, as a contour is) that the convoy's lanes lie on: the second vehicle would
		stand 1 wu higher than the first, 1 wu across; the run itself is level, so only the lanes' heights can refuse it */
		sidehill = 1.06f;
		CHECK(ride_level_spot(&position, .6f, 0.f, 70.f, RIDE_SEARCH_RADIUS, 0.f, &yaw) && yaw == 0.f, "a lone vehicle on a contour refused");
		position.x = position.y = position.z = 0.f;
		CHECK(!ride_level_spot(&position, .6f, RIDE_CONVOY_LANE, 70.f, RIDE_SEARCH_RADIUS, 0.f, &yaw), "lanes on a 46 degree slope accepted");
		position.x = position.y = position.z = 0.f;
		CHECK(!ride_level_spot(&position, .6f, RIDE_CONVOY_LANE, 106.f, RIDE_SEARCH_RADIUS, RIDE_CONVOY_TURN, &yaw), "turned lanes on a 46 degree slope accepted");
		/* the 16 degree slope (0.28 a wu) a convoy stayed upright on is no hazard */
		sidehill = .28f;
		position.x = position.y = position.z = 0.f;
		CHECK(ride_level_spot(&position, .6f, RIDE_CONVOY_LANE, 70.f, RIDE_SEARCH_RADIUS, 0.f, &yaw), "lanes on a 16 degree slope refused");
		NEAR(position.x, 0.f);
		return 0;
	}
	CASE("ride-level-reach")
	{
		real_point3d position = { 0.f, 0.f, 0.f };
		real yaw = -1.f;

		CHECK(ride_level_reach(_ride_convoy) > RIDE_SEARCH_RADIUS, "the convoy's reach %.0f", ride_level_reach(_ride_convoy));
		CHECK(ride_level_reach(_ride_straight) == RIDE_SEARCH_RADIUS && ride_level_reach(_ride_brush) == RIDE_SEARCH_RADIUS &&
			ride_level_reach(_ride_coop) == RIDE_SEARCH_RADIUS, "another scenario's reach changed");
		/* a ring's points: sixteen out to RIDE_SEARCH_RADIUS, then a multiple of sixteen (about as close together as the first rings') */
		CHECK(ride_level_ring_points(10.f) == 16 && ride_level_ring_points(RIDE_SEARCH_RADIUS) == 16, "a near ring's points");
		CHECK(ride_level_ring_points(RIDE_SEARCH_RADIUS + 10.f) == 32 && ride_level_ring_points(2.f*RIDE_SEARCH_RADIUS) == 32, "a ring's points");
		CHECK(ride_level_ring_points(2.f*RIDE_SEARCH_RADIUS + 10.f) == 48 && ride_level_ring_points(300.f) == 48, "a far ring's points");
		/* stairs everywhere but the flat ground beyond x 205 (a run of 106 and 10 behind needs a start past 215): nothing within
		RIDE_SEARCH_RADIUS, the ring at 210 (the first with a point whose heading and 10 wu behind it are over the flat: its first, due east,
		with the first yaw that does not run back over the stairs) */
		rough_all = TRUE;
		rough_far = TRUE;
		CHECK(!ride_level_spot(&position, .6f, 0.f, 106.f, RIDE_SEARCH_RADIUS, 0.f, &yaw), "a spot within the usual reach");
		position.x = position.y = position.z = 0.f;
		CHECK(ride_level_spot(&position, .6f, 0.f, 106.f, ride_level_reach(_ride_convoy), 0.f, &yaw), "no spot in the convoy's reach");
		NEAR(position.x, 210.f);
		NEAR(position.y, 0.f);
		NEAR(position.z, .6f);
		NEAR(yaw, _pi*3.f/8.f);
		/* the flat ground only north of y 40: no point due east of the start is on it, and the ring at 220 has thirty-two points: its second,
		at 11.25 degrees (a step of 22.5 degrees, a sixteen-point ring's, would put it at (203, 84), off the flat) */
		far_north = TRUE;
		position.x = position.y = position.z = 0.f;
		CHECK(ride_level_spot(&position, .6f, 0.f, 106.f, ride_level_reach(_ride_convoy), 0.f, &yaw), "no spot north in the convoy's reach");
		NEAR(position.x, 220.f*(real)cos(_pi/16.f));
		NEAR(position.y, 220.f*(real)sin(_pi/16.f));
		return 0;
	}
	CASE("ride-convoy-hold")
	{
		/* the second vehicle's sideways speed (positive: to the first's left) for how far it is from its target offset (target less actual) */
		NEAR(ride_convoy_lateral(0.f), 0.f);
		CHECK(ride_convoy_lateral(.01f) > 0.f && ride_convoy_lateral(.01f) < RIDE_CONVOY_HOLD_CAP, "a small error to the left: %f", ride_convoy_lateral(.01f));
		CHECK(ride_convoy_lateral(-.01f) < 0.f && ride_convoy_lateral(-.01f) > -RIDE_CONVOY_HOLD_CAP, "a small error to the right: %f", ride_convoy_lateral(-.01f));
		NEAR(ride_convoy_lateral(.01f), -ride_convoy_lateral(-.01f));
		NEAR(ride_convoy_lateral(.01f)*2.f, ride_convoy_lateral(.02f));
		NEAR(ride_convoy_lateral(5.f), RIDE_CONVOY_HOLD_CAP);
		NEAR(ride_convoy_lateral(-5.f), -RIDE_CONVOY_HOLD_CAP);
		CHECK(RIDE_CONVOY_HOLD_CAP > 0.f && RIDE_CONVOY_HOLD_CAP <= .02f, "a cap of %f", RIDE_CONVOY_HOLD_CAP);
		/* its turn rate for how far its yaw is from the first's (target less actual, wrapped to a half turn either way) */
		NEAR(ride_convoy_turn(0.f), 0.f);
		CHECK(ride_convoy_turn(.001f) > 0.f && ride_convoy_turn(-.001f) < 0.f, "the turn's sign");
		NEAR(ride_convoy_turn(.001f), ride_convoy_turn(.001f + 2.f*_pi));
		NEAR(ride_convoy_turn(-.001f), ride_convoy_turn(-.001f - 2.f*_pi));
		NEAR(ride_convoy_turn(1.f), RIDE_CONVOY_HOLD_TURN_CAP);
		NEAR(ride_convoy_turn(-1.f), -RIDE_CONVOY_HOLD_TURN_CAP);
		/* (just over a half turn to the left is a short way to the right) */
		CHECK(ride_convoy_turn(_pi + .1f) < 0.f, "the long way round");
		return 0;
	}
	CASE("ride-convoy-along")
	{
		short axis;

		host = TRUE;
		ride.enabled = ride.prepared = TRUE;
		ride.scenario = _ride_convoy;
		ride.vehicles[0] = 0;
		ride.vehicles[1] = 1;
		ride.convoy_offset = 1.1f;
		for (axis = 0; axis < 2; axis++)
		{
			real_vector3d *first;
			real_vector3d *second;

			memset(objects, 0, sizeof(objects));
			ride.convoy_yaw = axis ? _pi/2.f : 0.f;
			objects[0].object.forward.i = axis ? 0.f : 1.f;
			objects[0].object.forward.j = axis ? 1.f : 0.f;
			objects[0].object.up.k = 1.f;
			objects[1].object.forward = objects[0].object.forward;
			objects[1].object.up = objects[0].object.up;
			objects[1].object.position.x = axis ? -1.1f : .53f;
			objects[1].object.position.y = axis ? .53f : 1.1f;
			ride.before_tick = NONE;
			network_test_ride_before_objects();
			first = &objects[0].object.translational_velocity;
			second = &objects[1].object.translational_velocity;
			NEAR(axis ? first->j : first->i, .04f);
			CHECK((axis ? second->j : second->i) < .04f, "second vehicle stays ahead of the crossing lane");
			NEAR(axis ? second->i : second->j, 0.f);
			/* behind: catch up; even large errors use only the formation cap. */
			objects[1].object.position.x = axis ? -1.1f : -5.f;
			objects[1].object.position.y = axis ? -5.f : 1.1f;
			ride.before_tick = NONE;
			network_test_ride_before_objects();
			NEAR(axis ? second->j : second->i, .06f);
			objects[1].object.position.x = axis ? -1.1f : 5.f;
			objects[1].object.position.y = axis ? 5.f : 1.1f;
			ride.before_tick = NONE;
			network_test_ride_before_objects();
			NEAR(axis ? second->j : second->i, .02f);
		}
		return 0;
	}
	CASE("ride-convoy-hull")
	{
		real_point3d edge;

		objects[0].object.forward.i = 1.f;
		objects[0].object.up.k = 1.f;
		CHECK(ride_deck_edge(0, 1.f, &edge), "left hull not found");
		CHECK(edge.y >= .725f, "placement misses the wider wheel: %g", edge.y);
		CHECK(ride_deck_edge(0, -1.f, &edge), "right hull not found");
		CHECK(edge.y <= -.725f, "placement misses the opposite wheel: %g", edge.y);
		objects[0].object.position.z = .5f;
		objects[0].object.position.x = .3f;
		ride.convoy_yaw = 1.f;
		ride.vehicles[0] = 0;
		ride.vehicles[1] = 1;
		sidehill = .1f;
		CHECK(ride_convoy_place(), "hull-safe placement failed");
		CHECK(objects[1].object.position.y >= 1.6f, "wheel clearance is too small: %g", objects[1].object.position.y);
		NEAR(ride.convoy_offset, objects[1].object.position.y);
		NEAR(ride.convoy_origin.x, .3f);
		NEAR(ride.convoy_yaw, 0.f);
		return 0;
	}
	CASE("ride-convoy-board-speed")
	{
		objects[0].object.forward.i = 1.f;
		objects[0].object.up.k = 1.f;
		objects[0].object.translational_velocity.i = .04f;
		objects[0].object.translational_velocity.j = -.03f;
		objects[0].object.translational_velocity.k = .005f;
		objects[2].object.type = _object_type_biped;
		objects[2].object.parent_object_index = NONE;
		ride.scenario = _ride_convoy;
		CHECK(ride_board_actual(2, 0, 0), "convoy board failed");
		NEAR(objects[2].object.translational_velocity.i, .04f);
		NEAR(objects[2].object.translational_velocity.j, -.03f);
		NEAR(objects[2].object.translational_velocity.k, .005f);
		ride.scenario = _ride_straight;
		objects[2].object.translational_velocity.i = -.01f;
		CHECK(ride_board_actual(2, 0, 0), "other board failed");
		NEAR(objects[2].object.translational_velocity.i, -.01f);
		return 0;
	}
	CASE("ride-convoy-presses")
	{
		struct player_action action;

		ride.enabled = ride.prepared = TRUE;
		ride.scenario = _ride_convoy;
		ride.vehicles[0] = 0;
		ride.vehicles[1] = 1;
		objects[0].object.forward.i = 1.f;
		objects[1].object.parent_object_index = NONE;
		players[0].unit_index = 1;
		/* (the rider stands on the first vehicle, so its step-off goes right) */
		decks[1] = TRUE;
		reported_deck.vehicle_index = ride.vehicles[0];
		reported_deck.ticks = 60;
		ride.players[0].last_jump = NONE;
		ride.players[0].last_press_tick = NONE;
		for (now = 0; now < 2400; now++)
			network_test_ride_action(0, &action);
		CHECK(ride.players[0].presses == 11, "ten jumps and one cross hop, got %d", ride.players[0].presses);
		ride.players[0].last_jump = 9;
		ride.players[0].last_press_tick = NONE;
		now = 1740;
		network_test_ride_action(0, &action);
		CHECK(!TEST_FLAG(action.control_flags, _unit_control_jump_bit), "no run-up");
		now = 1835;
		network_test_ride_action(0, &action);
		CHECK(TEST_FLAG(action.control_flags, _unit_control_jump_bit), "late frame misses cross hop");
		network_test_ride_action(0, &action);
		CHECK(TEST_FLAG(action.control_flags, _unit_control_jump_bit), "same-tick request loses hop");
		now++;
		network_test_ride_action(0, &action);
		CHECK(!TEST_FLAG(action.control_flags, _unit_control_jump_bit), "hop repeats next tick");
		/* still approach until physical contact during the cross phase. */
		now = 1800;
		network_test_ride_action(0, &action);
		NEAR(action.throttle.j, .3f);
		ride.players[0].crossings = 1;
		now++;
		network_test_ride_action(0, &action);
		NEAR(action.throttle.j, 0.f);
		CHECK(!TEST_FLAG(action.control_flags, _unit_control_jump_bit), "hop after arrival");
		now = 1920;
		network_test_ride_action(0, &action);
		NEAR(action.throttle.j, -.3f);
		return 0;
	}
	CASE("ride-convoy-cross-phase")
	{
		ride.scenario = _ride_convoy;
		objects[0].object.forward.i = objects[1].object.forward.i = 1.f;
		objects[0].object.up.k = objects[1].object.up.k = 1.f;
		decks[0] = TRUE;
		reported_deck.ticks = 60;
		reported_deck.vehicle_index = 1;
		ride.players[0].previous_vehicle = 0;
		ride.players[0].previous_ticks = 60;
		ride.players[0].board_tick = 300;
		ride_rider_log("sample", 0, 0, 1700, TRUE);
		CHECK(ride.players[0].crossings == 0, "jump-phase switch counted");
		CHECK(ride.players[0].board_tick == 1700, "jump-phase switch did not restart settling");
		reported_deck.vehicle_index = 0;
		ride_rider_log("sample", 0, 0, 1710, TRUE);
		CHECK(ride.players[0].crossings == 0, "jump round trip counted");
		CHECK(ride.players[0].board_tick == 1710, "jump return did not restart settling");
		reported_deck.vehicle_index = 1;
		reported_deck.ticks = 59;
		ride_rider_log("sample", 0, 0, 1750, TRUE);
		CHECK(ride.players[0].crossings == 0, "grace is not receiving-deck contact");
		CHECK(ride.players[0].board_tick == 1710, "grace restarted settling before physical contact");
		reported_deck.ticks = 60;
		ride_rider_log("sample", 0, 0, 1751, TRUE);
		CHECK(ride.players[0].crossings == 1, "cross-phase physical contact missing");
		CHECK(ride.players[0].board_tick == 1751, "cross-phase switch did not restart settling");
		ride_rider_log("sample", 0, 0, 1752, TRUE);
		CHECK(ride.players[0].crossings == 1, "persistent contact counted again");
		CHECK(ride.players[0].board_tick == 1751, "persistent contact keeps restarting settling");
		reported_deck.vehicle_index = 0;
		ride_rider_log("sample", 0, 0, 1920, TRUE);
		CHECK(ride.players[0].crossings == 1, "step-off switch counted");
		CHECK(ride.players[0].board_tick == 1920, "step-off switch did not restart settling");
		return 0;
	}
	CASE("ride-convoy-corridor")
	{
		host = TRUE;
		ride.enabled = ride.prepared = TRUE;
		ride.scenario = _ride_convoy;
		ride.vehicles[0] = 0;
		ride.vehicles[1] = 1;
		ride.convoy_offset = 1.2f;
		objects[0].object.forward.i = (real)cos(.3f);
		objects[0].object.forward.j = (real)sin(.3f);
		objects[0].object.up.k = 1.f;
		objects[0].object.position.y = .4f;
		objects[1].object = objects[0].object;
		objects[1].object.position.x = .53f;
		objects[1].object.position.y = 1.6f;
		/* rolling cannot rotate or shrink the host's horizontal lane. */
		objects[0].object.up.j = .6f;
		objects[0].object.up.k = .8f;
		ride.before_tick = NONE;
		now = 2550;
		network_test_ride_before_objects();
		CHECK(objects[0].object.translational_velocity.j < 0.f, "lead leaves searched corridor");
		NEAR(objects[0].object.translational_velocity.i, .04f);
		NEAR(objects[0].object.translational_velocity.j, -.02f);
		CHECK(objects[0].object.angular_velocity.k < 0.f, "lead yaw drift accumulates");
		CHECK(objects[1].object.translational_velocity.i < objects[0].object.translational_velocity.i,
			"receiving rear stays ahead");
		CHECK(objects[1].object.translational_velocity.j < 0.f, "follower leaves lane");
		return 0;
	}
	CASE("ride-convoy-height")
	{
		/* the second vehicle as high over its own ground as the first is over its: on ground 1.03 higher, 1.03 higher */
		NEAR(ride_convoy_height(45.585f, 45.018f, 46.048f), 46.615f);
		NEAR(ride_convoy_height(45.585f, 45.018f, 45.018f), 45.585f);
		NEAR(ride_convoy_height(45.585f, 45.018f, 44.0f), 44.567f);
		return 0;
	}
	CASE("ride-level-best")
	{
		real_point3d position = { 10.f, 0.f, 0.f };
		real yaw = -1.f;

		/* both sides of x 40 pass the limit; the lanes lie 0.3 up a wu on the left of it, 0.1 on the right, or level
		(heading due north there: the lanes to its west run level): the first spot is not the one taken, the least sloped is */
		rough_two = TRUE;
		CHECK(ride_level_spot(&position, .6f, RIDE_CONVOY_LANE, 70.f, RIDE_SEARCH_RADIUS, 0.f, &yaw), "no spot found");
		CHECK(position.x >= 40.f, "the first, steeper spot taken: %.1f", position.x);
		NEAR(yaw, _pi/2.f);
		return 0;
	}
	CASE("ride-level-none")
	{
		real_point3d position = { 0.f, 0.f, 0.f };
		real yaw = -1.f;

		rough_all = TRUE;
		CHECK(!ride_level_spot(&position, .6f, 0.f, RIDE_LEVEL_RUN, RIDE_SEARCH_RADIUS, 0.f, &yaw), "a spot found on ground with no run");
		NEAR(position.x, 0.f);
		return 0;
	}
	CASE("ride-tilt-lands")
	{
		struct player_action action;

		/* (the failed ground ray used to supply zero clearance in mid-air) */
		ride_pilot_action(11, .5f, 0, &action);
		CHECK(TEST_FLAG(action.control_flags, _unit_control_crouch_modifier_bit), "descent stopped before ground contact");
		ride_pilot_action(11, .5f, 1, &action);
		CHECK(!TEST_FLAG(action.control_flags, _unit_control_crouch_modifier_bit), "descent continued after ground contact");
		ride_pilot_action(12, .5f, 0, &action);
		CHECK(!TEST_FLAG(action.control_flags, _unit_control_crouch_modifier_bit), "door phase requested descent");
		return 0;
	}
	CASE("ride-door-needs-landing")
	{
		struct player_action action;
		long grounded;

		ride.enabled = ride.prepared = TRUE;
		ride.scenario = _ride_longsword;
		ride.vehicles[0] = 1;
		players[1].unit_index = 2;
		objects[2].object.parent_object_index = NONE;
		ride.exit_forward = 1.f;
		ride.exit_left = .5f;
		now = ride.board_tick + 8100;
		for (grounded = 0; grounded <= 30; grounded++)
		{
			test_vehicle.vehicle.on_ground_ticks = grounded;
			network_test_ride_action(1, &action);
			CHECK(action.throttle.i == 0.f && action.throttle.j == 0.f, "walk before ramp wait at %ld grounded ticks", grounded);
			CHECK(ramp_opens == (grounded == 30 ? 1 : 0), "ramp opening at %ld grounded ticks", grounded);
			CHECK(!TEST_FLAG(action.control_flags, _unit_control_crouch_modifier_bit), "door crouch before landing/open wait");
		}
		return 0;
	}
	CASE("ride-door-latched")
	{
		struct player_action action;

		ride.enabled = ride.prepared = TRUE;
		ride.scenario = _ride_longsword;
		ride.vehicles[0] = 1;
		players[1].unit_index = 2;
		objects[2].object.parent_object_index = NONE;
		ride.exit_forward = -1.f;
		ride.exit_left = .25f;
		now = ride.board_tick + 8100;
		test_vehicle.vehicle.on_ground_ticks = 30;
		network_test_ride_action(1, &action);
		CHECK(ramp_opens == 1 && ramp_tick == now, "door did not open on landing");
		now += 60;
		test_vehicle.vehicle.on_ground_ticks = 0;
		network_test_ride_action(1, &action);
		CHECK(action.throttle.i == 1.f && action.throttle.j == 0.f, "contact blip stopped the door walk");
		CHECK(ramp_opens == 1, "contact blip reopened ramp");
		now = ride.board_tick + 8099;
		network_test_ride_action(1, &action);
		CHECK(action.throttle.i == 0.f && action.throttle.j == 0.f, "door input outside its episode");
		CHECK(!ride.door_started, "landing latch not reset outside episode");
		now++;
		network_test_ride_action(1, &action);
		CHECK(action.throttle.i == 0.f && action.throttle.j == 0.f, "old landing started a new door episode");
		return 0;
	}
	CASE("ride-live-descent-contact")
	{
		struct player_action action;

		ride.enabled = ride.prepared = TRUE;
		ride.scenario = _ride_longsword;
		ride.vehicles[0] = 1;
		players[0].unit_index = 0;
		objects[0].object.parent_object_index = 1;
		now = ride.board_tick + 4500;
		test_vehicle.vehicle.on_ground_ticks = 0;
		network_test_ride_action(0, &action);
		CHECK(TEST_FLAG(action.control_flags, _unit_control_crouch_modifier_bit), "live pilot did not descend without contact");
		now++;
		test_vehicle.vehicle.on_ground_ticks = 1;
		network_test_ride_action(0, &action);
		CHECK(!TEST_FLAG(action.control_flags, _unit_control_crouch_modifier_bit), "live pilot ignored ground contact");
		return 0;
	}
	CASE("ride-pilot")
	{
		struct player_action action;
		long hold;

		CHECK(ride_pilot_at(299, &hold) == 0, "settle ended early");
		CHECK(ride_pilot_at(300, &hold) == 1 && hold == 0, "takeoff clock missing");
		CHECK(ride_pilot_at(1199, &hold) == 1, "turn before takeoff");
		CHECK(ride_pilot_at(1200, &hold) == 2 && hold == 0, "turn clock missing");
		CHECK(ride_pilot_at(2399, &hold) == 3, "hover before cruise ends");
		CHECK(ride_pilot_at(2400, &hold) == 4 && hold == 0, "hover clock missing");
		CHECK(ride_pilot_at(2699, &hold) == 4 && hold == 299, "pitch before hover ends");
		CHECK(ride_pilot_at(2700, &hold) == 5 && hold == 0, "pitch clock missing");
		CHECK(ride_pilot_at(8099, &hold) == 11, "door before shared tick");
		CHECK(ride_pilot_at(8100, &hold) == 12 && hold == 0, "door clock missing");
		ride_pilot_action(0, .5f, 60, &action);
		NEAR(action.throttle.i, 0.f);
		ride_pilot_action(1, .5f, 60, &action);
		CHECK(TEST_FLAG(action.control_flags, _unit_control_jump_bit), "takeoff lift missing");
		ride_pilot_action(5, .5f, 60, &action);
		CHECK(action.throttle.i > .5f && action.desired_facing.pitch > .3f, "pitch input missing");
		ride_pilot_action(7, .5f, 60, &action);
		CHECK(action.throttle.i > .5f && action.desired_facing.yaw < -.2f, "bank input missing");
		ride_pilot_action(9, .5f, 60, &action);
		CHECK(action.desired_facing.pitch > .3f && action.desired_facing.yaw < -.2f, "combined input missing");
		ride_pilot_action(11, .5f, 0, &action);
		CHECK(TEST_FLAG(action.control_flags, _unit_control_crouch_modifier_bit), "no hover descent");
		ride_pilot_action(12, .5f, 0, &action);
		NEAR(action.throttle.i, 0.f);
		CHECK(!TEST_FLAG(action.control_flags, _unit_control_jump_bit), "pilot leaves seat");
		return 0;
	}
	CASE("ride-pilot-hover")
	{
		struct player_action action;
		long hold;
		long elapsed;

		ride.enabled = ride.prepared = TRUE;
		ride.scenario = _ride_longsword;
		ride.board_tick = 300;
		ride.vehicles[0] = 1;
		ride.tilt_yaw = .5f;
		test_objects[0].object.parent_object_index = 1;
		CHECK(ride_pilot_at(2399, &hold) == 3, "cruise ended early");
		for (elapsed = 2400; elapsed < 2700; elapsed++)
		{
			CHECK(ride_pilot_at(elapsed, &hold) == 4 && hold == elapsed - 2400, "hover missing at %ld", elapsed);
			test_tick = ride.board_tick + elapsed;
			network_test_ride_action(0, &action);
			NEAR(action.throttle.i, 0.f);
			NEAR(action.throttle.j, 0.f);
			NEAR(action.desired_facing.yaw, .5f + _pi/2.f);
			NEAR(action.desired_facing.pitch, 0.f);
			CHECK(action.control_flags == 0, "hover has active controls");
		}
		return 0;
	}
	CASE("ride-rear-corner")
	{
		struct player_action action;

		ride.enabled = ride.prepared = ride.corner = TRUE;
		network_test.mode = _network_test_local;
		ride.scenario = _ride_longsword;
		ride.vehicles[0] = 1;
		ride.board_tick = 300;
		test_player.unit_index = 0;
		test_second_player.unit_index = 2;
		test_objects[0].object.parent_object_index = 1;
		test_objects[2].object.parent_object_index = NONE;
		test_tick = ride.board_tick + 2700 + 150;
		network_test_ride_action(0, &action);
		NEAR(action.desired_facing.pitch, 89.9f*_pi/180.f);
		NEAR(action.throttle.i, 1.f);
		network_test_ride_action(1, &action);
		NEAR(action.throttle.i, -1.f);
		NEAR(action.throttle.j, -.7f);
		test_tick = ride.board_tick + 2700 + 200;
		network_test_ride_action(1, &action);
		NEAR(action.throttle.i, -1.f);
		NEAR(action.throttle.j, -.7f);
		CHECK(!TEST_FLAG(action.control_flags, _unit_control_jump_bit), "walking, not jumping");
		test_tick = ride.board_tick + 2700 + 260;
		network_test_ride_action(1, &action);
		NEAR(action.throttle.i, 1.f);
		NEAR(action.throttle.j, 0.f);
		network_test.mode = 0;
		network_test_ride_action(0, &action);
		NEAR(action.desired_facing.pitch, 75.f*_pi/180.f);
		network_test_ride_action(1, &action);
		NEAR(action.throttle.i, 0.f);
		network_test.mode = _network_test_local;
		ride.corner = FALSE;
		network_test_ride_action(0, &action);
		NEAR(action.desired_facing.pitch, 75.f*_pi/180.f);
		network_test_ride_action(1, &action);
		NEAR(action.throttle.i, 0.f);
		return 0;
	}
	CASE("ride-local-inputs")
	{
		struct player_action action;
		ride.enabled = ride.prepared = TRUE;
		ride.scenario = _ride_longsword;
		ride.board_tick = 300;
		ride.vehicles[0] = 1;
		test_second_player.unit_index = 2;
		test_objects[0].object.parent_object_index = 1;
		test_objects[2].object.parent_object_index = NONE;
		test_tick = 3000;
		network_test_ride_action(0, &action);
		CHECK(action.throttle.i == 1.f && action.desired_facing.pitch > 1.f, "local pilot not flying pitch schedule");
		test_tick = 8400;
		test_vehicle.vehicle.on_ground_ticks = 30;
		ride.exit_forward = -1.f;
		ride.exit_left = .25f;
		memset(&action, 0, sizeof(action));
		network_test_ride_action(1, &action);
		CHECK(ramp_opens == 1, "second local player did not open ramp");
		test_tick += 60;
		network_test_ride_action(1, &action);
		CHECK(action.throttle.i == 1.f && action.throttle.j == 0.f, "second local player not walking door");
		network_test_ride_action(0, &action);
		CHECK(action.throttle.i == 0.f, "pilot received rider door walk");
		return 0;
	}
	CASE("ride-live-jumps")
	{
		struct player_action action;
		long offsets[4] = { 120, 1620, 3120, 3720 };
		short i;

		ride.enabled = ride.prepared = TRUE;
		ride.scenario = _ride_longsword;
		ride.board_tick = 300;
		ride.vehicles[0] = 1;
		ride.players[0].last_jump = NONE;
		ride.players[0].last_press_tick = NONE;
		test_objects[0].object.parent_object_index = NONE;
		for (i = 0; i < 4; i++)
		{
			test_tick = ride.board_tick + offsets[i];
			network_test_ride_action(0, &action);
			CHECK(TEST_FLAG(action.control_flags, _unit_control_jump_bit), "live level jump missing at %ld", offsets[i]);
			network_test_ride_action(0, &action);
			CHECK(ride.players[0].presses == i + 1, "live repeated input counted twice");
			test_tick++;
			network_test_ride_action(0, &action);
			CHECK(!TEST_FLAG(action.control_flags, _unit_control_jump_bit), "live jump held past press");
		}
		return 0;
	}
	CASE("ride-live-climb")
	{
		struct player_action action;
		short presses = 0;
		long age;

		ride.enabled = ride.prepared = TRUE;
		ride.scenario = _ride_longsword;
		ride.board_tick = 300;
		ride.vehicles[0] = 1;
		test_objects[0].object.parent_object_index = 1;
		for (age = 0; age < 30; age++)
		{
			test_tick = 600 + age;
			network_test_ride_action(0, &action);
			presses += TEST_FLAG(action.control_flags, _unit_control_jump_bit) ? 1 : 0;
			CHECK(action.throttle.i == 0.f && action.desired_facing.pitch == 0.f, "vertical takeoff used forward flight");
		}
		CHECK(presses == 22, "takeoff needs startup then seven-of-fifteen lift duty, got %d", presses);
		for (age = 550; age < 900; age++)
		{
			test_tick = 600 + age;
			network_test_ride_action(0, &action);
			CHECK(TEST_FLAG(action.control_flags, _unit_control_jump_bit) == (age % 3 == 0),
				"late takeoff did not stop climbing below the roof");
		}
		return 0;
	}
	CASE("ride-live-right-turn")
	{
		struct player_action action;
		long elapsed;

		ride.enabled = ride.prepared = TRUE;
		ride.scenario = _ride_longsword;
		ride.board_tick = 300;
		ride.vehicles[0] = 1;
		ride.tilt_yaw = .25f;
		test_objects[0].object.parent_object_index = 1;
		for (elapsed = 1200; elapsed < 1500; elapsed++)
		{
			test_tick = ride.board_tick + elapsed;
			network_test_ride_action(0, &action);
			CHECK(fabs(action.desired_facing.yaw - 1.8207963f) < .0001f,
				"live turn is not parked heading plus90 at %ld", elapsed);
			CHECK(action.throttle.i == 0.f && action.desired_facing.pitch == 0.f, "turn flew forward or tilted");
			CHECK(TEST_FLAG(action.control_flags, _unit_control_jump_bit) == ((elapsed - 1200) % 3 == 0),
				"turn hover aliases alternating ticks at %ld", elapsed);
		}
		test_tick = ride.board_tick + 1500;
		network_test_ride_action(0, &action);
		CHECK(action.throttle.i == 0.f, "exit overlaps the rider's jump hold");
		test_tick = ride.board_tick + 1680;
		network_test_ride_action(0, &action);
		CHECK(action.throttle.i > 0.f && action.throttle.i < .01f,
			"level exit starts with abrupt acceleration");
		NEAR(action.desired_facing.pitch, 6.f*_pi/180.f);
		CHECK(!TEST_FLAG(action.control_flags, _unit_control_jump_bit), "exit hover suppresses forward flight");
		test_tick = ride.board_tick + 1919;
		network_test_ride_action(0, &action);
		CHECK(action.throttle.i == .5f, "level exit exceeds the slow cruise input");
		test_tick = ride.board_tick + 2700;
		network_test_ride_action(0, &action);
		CHECK(action.throttle.i == 1.f && action.desired_facing.pitch > 1.f, "first flight before shifted pitch tick");
		CHECK(fabs(action.desired_facing.yaw - 1.8207963f) < .0001f, "flight abandoned the clear heading");
		return 0;
	}
	CASE("ride-live-preboard")
	{
		struct player_action action;

		ride.enabled = TRUE;
		ride.prepared = FALSE;
		ride.scenario = _ride_longsword;
		ride.board_tick = 300;
		ride.vehicles[0] = NONE;
		test_objects[0].object.parent_object_index = 1;
		test_tick = 250;
		memset(&action, 0, sizeof(action));
		action.desired_facing.yaw = -3.f;
		action.throttle.i = 1.f;
		network_test_ride_action(0, &action);
		NEAR(action.desired_facing.yaw, 0.f);
		NEAR(action.throttle.i, 0.f);
		CHECK(action.control_flags == 0, "preboard pilot is not neutral");
		return 0;
	}
	CASE("ride-live-door-skew")
	{
		struct player_action action;
		short side;

		ride.enabled = ride.prepared = TRUE;
		ride.scenario = _ride_longsword;
		ride.board_tick = 300;
		ride.vehicles[0] = 1;
		ride.exit_forward = -1.f;
		ride.exit_left = .25f;
		test_vehicle.vehicle.on_ground_ticks = 30;
		test_objects[0].object.parent_object_index = NONE;
		/* unlike the fixed clock, old local phase histories could disagree */
		for (side = 0; side < 2; side++)
		{
			ride.pilot_step = side ? 7 : 9;
			test_tick = ride.board_tick + 8099;
			network_test_ride_action(0, &action);
			CHECK(action.throttle.i == 0.f, "non-pilot opened door before shared tick");
			CHECK(ramp_opens == side, "ramp opened before shared door phase");
			test_tick++;
			network_test_ride_action(0, &action);
			CHECK(action.throttle.i == 0.f && action.throttle.j == 0.f, "ramp wait missing on side %d", side);
			test_tick += 60;
			network_test_ride_action(0, &action);
			CHECK(action.throttle.i == 1.f && action.throttle.j == 0.f, "live door action missing on side %d", side);
		}
		return 0;
	}
	CASE("ride-door-legs")
	{
		struct player_action action;
		long elapsed;
		long start = 8407;

		ride.enabled = ride.prepared = TRUE;
		ride.scenario = _ride_longsword;
		ride.board_tick = 300;
		ride.vehicles[0] = 1;
		objects[0].object.parent_object_index = NONE;
		now = start;
		test_vehicle.vehicle.on_ground_ticks = 30;
		network_test_ride_action(0, &action);
		for (elapsed = 0; elapsed <= 360; elapsed++)
		{
			real forward = (elapsed >= 60 && elapsed < 92) || (elapsed >= 132 && elapsed < 282) ? 1.f : 0.f;
			real left = elapsed >= 92 && elapsed < 132 ? -1.f : 0.f;
			now = start + elapsed;
			test_vehicle.vehicle.on_ground_ticks = 0;
			network_test_ride_action(0, &action);
			CHECK(action.throttle.i == forward && action.throttle.j == left, "wrong measured door leg at %ld", elapsed);
			CHECK(!!TEST_FLAG(action.control_flags, _unit_control_crouch_modifier_bit) == (elapsed >= 92), "crouch at %ld", elapsed);
			CHECK(ramp_opens == 1 && ramp_tick == start, "ramp reopened or opened at wrong tick");
		}
		return 0;
	}
	CASE("ride-live-recover")
	{
		struct player_action action;

		ride.enabled = ride.prepared = TRUE;
		ride.scenario = _ride_longsword;
		ride.board_tick = 300;
		ride.vehicles[0] = 1;
		test_objects[0].object.parent_object_index = 1;
		test_tick = ride.board_tick + 4200;
		ride.tilt_yaw = .25f;
		network_test_ride_action(0, &action);
		NEAR(action.desired_facing.yaw, 4.9623890f);
		CHECK(action.throttle.i == 0.f && action.desired_facing.pitch == 0.f &&
			TEST_FLAG(action.control_flags, _unit_control_jump_bit), "recovery lacks hover lift to clear contact");
		test_tick = ride.board_tick + 4350;
		network_test_ride_action(0, &action);
		CHECK(action.throttle.i == 1.f && !TEST_FLAG(action.control_flags, _unit_control_jump_bit),
			"recovery did not fly clear after turning");
		return 0;
	}
	CASE("ride-tilt-schedule")
	{
		real pitch;
		real roll;
		long hold;
		real pair = ride_tilt_pair(.5f);
		long end = ride_tilt_end(pair);
		short step;

		/* before the programme: level, no step; the settle holds */
		CHECK(ride_tilt_at(-1, pair, &pitch, &roll, &hold) == NONE && pitch == 0.f && roll == 0.f, "before the board tick");
		CHECK(ride_tilt_at(RIDE_TILT_SETTLE - 1, pair, &pitch, &roll, &hold) == NONE, "the settle is no step");
		/* the first step turns 20 degrees, one a tick, then holds */
		step = ride_tilt_at(RIDE_TILT_SETTLE + 10, pair, &pitch, &roll, &hold);
		CHECK(step == 0 && hold == NONE, "turning: step %d hold %ld", step, hold);
		NEAR(pitch, 10.f*_pi/180.f);
		step = ride_tilt_at(RIDE_TILT_SETTLE + 20, pair, &pitch, &roll, &hold);
		CHECK(step == 0 && hold == 0, "held at 20: step %d hold %ld", step, hold);
		NEAR(pitch, 20.f*_pi/180.f);
		step = ride_tilt_at(RIDE_TILT_SETTLE + 20 + RIDE_TILT_HOLD, pair, &pitch, &roll, &hold);
		CHECK(step == 1 && hold == NONE, "the second step turning back: step %d", step);
		/* a step at no turn holds at once (the pair step's level step after it) */
		NEAR(pair, .9f*(real)acos(.70710678f));
		/* every step's setpoint a turn of at most the cap a tick from the last tick's */
		{
			real last_pitch = 0.f;
			real last_roll = 0.f;
			long tick;

			for (tick = RIDE_TILT_SETTLE; tick < end; tick++)
			{
				ride_tilt_at(tick, pair, &pitch, &roll, &hold);
				CHECK(fabs((double)(pitch - last_pitch)) <= RIDE_TILT_CAP + .00001 &&
					fabs((double)(roll - last_roll)) <= RIDE_TILT_CAP + .00001, "a jump of the setpoint at %ld", tick);
				last_pitch = pitch;
				last_roll = roll;
			}
			/* the last step level, its hold long enough for the descent and then the door walk */
			CHECK(ride_tilt_at(end - 1, pair, &pitch, &roll, &hold) == RIDE_TILT_STEPS - 1 && pitch == 0.f && roll == 0.f &&
				hold == RIDE_TILT_DESCEND + RIDE_TILT_HOLD - 1, "the last step (hold %ld)", hold);
		}
		/* the pair: each angle within the limit, together past it (a surface limit of 60 degrees: 45 is the steeper) */
		CHECK(cos((double)pair) > .70710678 && cos((double)pair)*cos((double)pair) < .70710678, "the pair %g", (double)pair);
		/* a limit steeper than 45 degrees (cos 30) is the rider's own */
		NEAR(ride_tilt_pair(.8660254f), .9f*(real)acos(.8660254f));
		/* the programme: the spec's steps, the inversion, the door last */
		CHECK(RIDE_TILT_STEPS == 27, "%d steps", (int)RIDE_TILT_STEPS);
		CHECK(ride_tilt_steps[24].pitch == 180 && ride_tilt_steps[22].pair, "the inversion and the pair");
		/* the whole programme fits the runner's run */
		CHECK(end < 420*30 - 600, "the programme runs %ld ticks", end);
		return 0;
	}
	CASE("ride-tilt-frame")
	{
		real_vector3d forward;
		real_vector3d up;

		/* pitch 40 and roll 40: total tilt cosine cos40 cos40, forward and up square */
		ride_tilt_frame(0.f, 40.f*_pi/180.f, 40.f*_pi/180.f, &forward, &up);
		NEAR(up.k, (real)(cos(40.*3.14159265/180.)*cos(40.*3.14159265/180.)));
		NEAR(forward.i*up.i + forward.j*up.j + forward.k*up.k, 0.f);
		/* a positive roll: up leans left (+j, the body's left), the left wing down */
		ride_tilt_frame(0.f, 0.f, 20.f*_pi/180.f, &forward, &up);
		CHECK(up.j > 0.f, "a positive roll leans up %g", (double)up.j);
		/* upside down at pitch 180 */
		ride_tilt_frame(0.f, _pi, 0.f, &forward, &up);
		NEAR(up.k, -1.f);
		NEAR(forward.i, -1.f);
		/* turned to the heading */
		ride_tilt_frame(_pi/2.f, 0.f, 0.f, &forward, &up);
		NEAR(forward.i, 0.f);
		NEAR(forward.j, 1.f);
		return 0;
	}
	CASE("ride-tilt-action")
	{
		struct player_action action;
		real pair = ride_tilt_pair(.5f);
		real pitch;
		real roll;
		long hold;
		long tick;
		long turning = NONE;
		long held = NONE;
		long door = NONE;
		short jumps = 0;
		short last = NONE;

		for (tick = 0; tick < ride_tilt_end(pair); tick++)
		{
			short step = ride_tilt_at(tick, pair, &pitch, &roll, &hold);

			ride_tilt_action(tick, pair, .3f, .5f, -.25f, last, FALSE, &action);
			if (TEST_FLAG(action.control_flags, _unit_control_jump_bit))
			{
				CHECK(step != NONE && ride_tilt_steps[step].jump && hold >= RIDE_TILT_JUMP_AT, "a jump at %ld", tick);
				last = ride_press_slot(_ride_longsword, tick, pair);
				CHECK(last == step, "the press slot is the step");
				jumps++;
			}
			if (step == 2 && hold == NONE && turning == NONE)
				turning = tick;
			if (step == 2 && hold == RIDE_TILT_JUMP_AT)
				held = tick;
			if (step == RIDE_TILT_STEPS - 1 && hold == RIDE_TILT_DESCEND)
				door = tick;
			NEAR(action.desired_facing.yaw, .3f);
		}
		/* one press in each jump step's hold (40 pitch, 60 pitch, 40 roll, 45/45, the pair, the inversion) */
		CHECK(jumps == 6, "%d jumps", jumps);
		/* none while turning */
		ride_tilt_action(turning, pair, 0.f, .5f, -.25f, NONE, FALSE, &action);
		CHECK(!TEST_FLAG(action.control_flags, _unit_control_jump_bit), "a jump while turning");
		/* a tick asked again presses again; a step already pressed does not */
		ride_tilt_action(held, pair, 0.f, .5f, -.25f, 2, TRUE, &action);
		CHECK(TEST_FLAG(action.control_flags, _unit_control_jump_bit), "the press asked again lost");
		ride_tilt_action(held + 1, pair, 0.f, .5f, -.25f, 2, FALSE, &action);
		CHECK(!TEST_FLAG(action.control_flags, _unit_control_jump_bit), "a step pressed twice");
		/* the door walk: the exit throttle once the vehicle is set down, nothing while it descends */
		ride_tilt_action(door, pair, 0.f, .5f, -.25f, NONE, FALSE, &action);
		NEAR(action.throttle.i, .5f);
		NEAR(action.throttle.j, -.25f);
		ride_tilt_action(door - 1, pair, 0.f, .5f, -.25f, NONE, FALSE, &action);
		NEAR(action.throttle.i, 0.f);
		/* other scenarios count presses by the jump phase's slot */
		CHECK(ride_press_slot(_ride_straight, RIDE_JUMP + RIDE_JUMP_PERIOD, pair) == 1, "the jump slot");
		return 0;
	}
	CASE("ride-tilt-spot")
	{
		real x;
		real y;
		real forward;
		real left;

		CHECK(ride_spot_parse("1.25,-0.5,0.3,-1", &x, &y, &forward, &left), "a spot refused");
		NEAR(x, 1.25f);
		NEAR(y, -.5f);
		NEAR(forward, .3f);
		NEAR(left, -1.f);
		CHECK(!ride_spot_parse("", &x, &y, &forward, &left), "no spot: the survey");
		CHECK(!ride_spot_parse("1,2,3", &x, &y, &forward, &left), "three numbers");
		CHECK(!ride_spot_parse("1,2,3,4,5", &x, &y, &forward, &left), "five numbers");
		CHECK(!ride_spot_parse("1,2,3,4x", &x, &y, &forward, &left), "trailing text");
		CHECK(!ride_spot_parse("nan,0,0,0", &x, &y, &forward, &left), "not a number");
		CHECK(!ride_spot_parse("1000,0,0,0", &x, &y, &forward, &left), "off the vehicle");
		CHECK(ride_vehicle_named(_ride_longsword, "vehicles\\fighterbomber\\fighterbomber_mp"), "the Longsword not found");
		CHECK(!ride_vehicle_named(_ride_longsword, "vehicles\\warthog\\warthog") &&
			!ride_vehicle_named(_ride_longsword, "vehicles\\g_warthog\\g_warthog") &&
			!ride_vehicle_named(_ride_longsword, "vehicles\\misslehog\\misslehog"), "a Warthog taken for it");
		CHECK(ride_vehicle_named(_ride_straight, "vehicles\\warthog\\warthog") &&
			!ride_vehicle_named(_ride_straight, "vehicles\\fighterbomber\\fighterbomber_mp"), "the other scenarios' Warthog");
		return 0;
	}
	CASE("ride-drive-after-equals-next")
	{
		struct { short scenario; long elapsed; boolean two; } rows[] = {
			{ _ride_straight, RIDE_BRAKE + 10, FALSE }, { _ride_circle, 50, FALSE }, { _ride_gun, 50, FALSE },
			{ _ride_coop, 50, FALSE }, { _ride_rise, RIDE_RISE_TICKS - 1, FALSE },
			{ _ride_brush, RIDE_STEP_OFF + RIDE_BRUSH_STEP_TICKS - 1, FALSE }, { _ride_convoy, 50, TRUE } };
		short row;
		short index;

		for (row = 0; row < 7; row++)
		{
			real_vector3d written_velocity[2];
			real_vector3d written_angular[2];

			drive_setup(rows[row].scenario, rows[row].elapsed);
			network_test_ride_before_objects();
			drive_step();
			network_test_ride_after_objects();
			for (index = 0; index < 1 + rows[row].two; index++)
			{
				written_velocity[index] = objects[index].object.translational_velocity;
				written_angular[index] = objects[index].object.angular_velocity;
			}
			now++;
			network_test_ride_before_objects();
			for (index = 0; index < 1 + rows[row].two; index++)
			{
				CHECK(!memcmp(&written_velocity[index], &objects[index].object.translational_velocity, sizeof(real_vector3d)),
					"row %d vehicle %d: the velocity sent is not the next step's (%g against %g)", row, index,
					(double)written_velocity[index].i, (double)objects[index].object.translational_velocity.i);
				CHECK(!memcmp(&written_angular[index], &objects[index].object.angular_velocity, sizeof(real_vector3d)),
					"row %d vehicle %d: the angular velocity sent is not the next step's", row, index);
			}
		}
		return 0;
	}
	CASE("ride-drive-host-motion-unchanged")
	{
		short scenario;

		for (scenario = 0; scenario < 2; scenario++)
		{
			static struct object_datum reference[120];
			static struct object_datum candidate[120];
			short run;
			short tick;

			for (run = 0; run < 2; run++)
			{
				drive_setup(scenario ? _ride_straight : _ride_circle, 0);
				for (tick = 0; tick < 120; tick++)
				{
					real_vector3d held_velocity;
					real_vector3d held_angular;

					network_test_ride_before_objects();
					drive_step();
					held_velocity = objects[0].object.translational_velocity;
					held_angular = objects[0].object.angular_velocity;
					network_test_ride_after_objects();
					if (!run)
					{
						/* (the base's drive: the before hook only) */
						objects[0].object.translational_velocity = held_velocity;
						objects[0].object.angular_velocity = held_angular;
					}
					(run ? candidate : reference)[tick] = objects[0];
					now++;
				}
			}
			for (tick = 1; tick < 120; tick++)
			{
				CHECK(!memcmp(&reference[tick].object.position, &candidate[tick].object.position, sizeof(real_point3d)),
					"scenario %d tick %d: the host's vehicle moved differently", scenario, tick);
				CHECK(candidate[tick].object.translational_velocity.i > reference[tick].object.translational_velocity.i*1.04f,
					"scenario %d tick %d: the captured velocity is the post-step one (%g, not above %g)", scenario, tick,
					(double)candidate[tick].object.translational_velocity.i, (double)reference[tick].object.translational_velocity.i);
			}
		}
		return 0;
	}
	CASE("ride-drive-client-untouched")
	{
		drive_setup(_ride_circle, 50);
		host = FALSE;
		objects[0].object.translational_velocity.i = .0123f;
		network_test_ride_after_objects();
		NEAR(objects[0].object.translational_velocity.i, .0123f);
		NEAR(objects[0].object.angular_velocity.k, 0.f);
		return 0;
	}
	CASE("ride-drive-driven-untouched")
	{
		short scenario;

		for (scenario = 0; scenario < 2; scenario++)
		{
			drive_setup(scenario ? _ride_longsword : _ride_driven, 50);
			objects[0].object.translational_velocity.i = .0123f;
			network_test_ride_after_objects();
			NEAR(objects[0].object.translational_velocity.i, .0123f);
			NEAR(objects[0].object.angular_velocity.k, 0.f);
		}
		return 0;
	}
	CASE("ride-drive-after-end-untouched")
	{
		drive_setup(_ride_circle, ride_phase_tick(_ride_circle, RIDE_END));
		objects[0].object.translational_velocity.i = .0123f;
		network_test_ride_after_objects();
		CHECK(ride.ended, "the programme did not end");
		NEAR(objects[0].object.translational_velocity.i, .0123f);
		drive_setup(_ride_circle, 50);
		samples_fail = TRUE;
		objects[0].object.translational_velocity.i = .0123f;
		network_test_ride_after_objects();
		samples_fail = FALSE;
		CHECK(ride.failed, "the programme did not fail");
		NEAR(objects[0].object.translational_velocity.i, .0123f);
		return 0;
	}
	CASE("ride-drive-after-rest-untouched")
	{
		drive_setup(_ride_circle, 50);
		SET_FLAG(objects[0].object.flags, _object_at_rest_bit, TRUE);
		objects[0].object.translational_velocity.i = .0123f;
		objects[0].object.angular_velocity.k = .0045f;
		activations = 0;
		network_test_ride_after_objects();
		NEAR(objects[0].object.translational_velocity.i, .0123f);
		NEAR(objects[0].object.angular_velocity.k, .0045f);
		CHECK(TEST_FLAG(objects[0].object.flags, _object_at_rest_bit), "a vehicle at rest was woken");
		CHECK(!activations, "a vehicle at rest was activated");
		drive_setup(_ride_circle, 50);
		activations = 0;
		network_test_ride_after_objects();
		CHECK(!TEST_FLAG(objects[0].object.flags, _object_at_rest_bit), "the at-rest flag changed");
		CHECK(!activations, "a moving vehicle was activated");
		CHECK(objects[0].object.translational_velocity.i > .03f, "a moving vehicle was not driven");
		return 0;
	}
	CASE("ride-drive-samples-post-step")
	{
		drive_setup(_ride_circle, 50);
		network_test_ride_before_objects();
		drive_step();
		network_test_ride_after_objects();
		CHECK(sample_calls == 1, "the samples ran %d times", (int)sample_calls);
		CHECK(hypot((double)sample_velocity[0].i, (double)sample_velocity[0].j) < .04*.96 &&
			hypot((double)sample_velocity[0].i, (double)sample_velocity[0].j) > .04*.94, "the sample holds %g, not the post-step velocity", (double)sample_velocity[0].i);
		CHECK(hypot((double)objects[0].object.translational_velocity.i, (double)objects[0].object.translational_velocity.j) > .04*.99,
			"the state sent holds %g", (double)objects[0].object.translational_velocity.i);
		return 0;
	}
	fprintf(stderr, "unknown case: %s\n", case_name);
	return 2;
}

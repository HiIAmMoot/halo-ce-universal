/* The real sample loop and observer, with engine-owned attribution in a fake world. */
#include "harness.h"
#include <math.h>
#include <stdarg.h>
#define RIDE_PLAYERS 2
#define MAXIMUM_ATTACKERS_PER_UNIT 4
#define _object_mask_vehicle 2
#define _object_dead_bit 2
#define _ride_longsword 9
#include "ride_life_types.inc"
struct unit_attacker { unsigned long game_time_stamp; real damage_inflicted; long object_index, player_index; };
struct object_datum { struct { real_point3d position; real_vector3d translational_velocity; } object; };
struct unit_datum {
    struct { real_point3d position; real body_vitality, shield_vitality; unsigned long damage_flags; } object;
    struct { short last_damage_category; long time_of_death; struct unit_attacker attackers[4]; } unit;
};
struct player_datum { long unit_index; short local_player_index; };
struct data_iterator { long datum_index; };
struct object_iterator { long index; };
static struct { short scenario; struct ride_player players[2]; } ride;
static struct unit_datum unit;
static struct object_datum vehicles[4];
static struct player_datum player;
static void *player_data;
static boolean gone, ended;
static char logged[1500];
static short records;
static struct unit_datum *unit_try_and_get(long index) { return index == 1 && !gone ? &unit : NULL; }
static void object_iterator_new(struct object_iterator *i, long mask, long flags) { i->index = 1; }
static boolean object_iterator_next(struct object_iterator *i) { return ++i->index <= 3; }
static struct object_datum *object_get(long index) { return &vehicles[index]; }
static real distance3d(real_point3d const *a, real_point3d const *b) { real x=a->x-b->x, y=a->y-b->y, z=a->z-b->z; return sqrtf(x*x+y*y+z*z); }
static real magnitude3d(real_vector3d const *v) { return sqrtf(v->i*v->i+v->j*v->j+v->k*v->k); }
static void data_iterator_new(struct data_iterator *i, void *data) { i->datum_index = NONE; }
static void *data_iterator_next(struct data_iterator *i) { return ++i->datum_index == 0 ? &player : NULL; }
static void ride_error(char const *reason) { ended = TRUE; }
static void ride_rider_log(char const *kind, short p, long u, long t, boolean local) { }
static void ride_cabin_log(short p, long u, long t) { }
static void ride_log(char const *kind, char const *format, ...) {
    va_list args;
    if (strcmp(kind, "life")) return;
    va_start(args, format); vsnprintf(logged, sizeof(logged), format, args); va_end(args); records++;
}
#include "ride_life_under_test.inc"
int main(int argc, char **argv) {
    short i;
    char const *case_name = argc > 1 ? argv[1] : "";
    ride.players[0].unit = player.unit_index = 1;
    ride.players[1].unit = NONE;
    vehicles[2].object.position.x = 1.f;
    vehicles[3].object.position.x = 10.f;
    vehicles[2].object.translational_velocity.i = .03f;
    vehicles[2].object.translational_velocity.j = .04f;
    for (i=0; i<4; i++) unit.unit.attackers[i].object_index = NONE;
    ride_samples(2400);
    CHECK(records == 0, "living rider logged as dead");
    CASE("life-dead") {
        unit.object.damage_flags = FLAG(_object_dead_bit);
        unit.unit.time_of_death = 2500;
        unit.unit.last_damage_category = 17;
        unit.unit.attackers[0].object_index = 2;
        unit.unit.attackers[0].player_index = 99;
        unit.unit.attackers[0].game_time_stamp = 2499;
        unit.unit.attackers[0].damage_inflicted = 1.f;
        ride_samples(2500);
        CHECK(records == 1 && strstr(logged, "reason=dead"), "death not recorded");
        CHECK(strstr(logged, "unit=1") && strstr(logged, "damage_category=17") &&
            strstr(logged, "attacker_object=2") && strstr(logged, "attacker_player=99"), "engine attribution lost");
        CHECK(strstr(logged, "nearest_vehicle=2") && strstr(logged, "vehicle_speed=0.0500"), "nearest vehicle motion lost");
        ride_samples(2501);
        CHECK(records == 1, "same death repeated every tick");
        return 0;
    }
    CASE("life-changed") {
        player.unit_index = NONE;
        ride_samples(2611);
        CHECK(ended && records == 1 && strstr(logged, "reason=unit-changed"), "life change ended before its record");
        CHECK(strstr(logged, "current_unit=-1") && strstr(logged, "nearest_vehicle=2"), "change context missing");
        return 0;
    }
    CASE("life-gone") {
        player.unit_index = NONE; gone = TRUE;
        ride_samples(2611);
        CHECK(ended && records == 1 && strstr(logged, "recorded=0"), "deleted unit not recorded as unavailable");
        CHECK(strstr(logged, "damage_category=-1") && strstr(logged, "nearest_vehicle=2"), "unavailable cause invented or last position lost");
        return 0;
    }
    return 1;
}

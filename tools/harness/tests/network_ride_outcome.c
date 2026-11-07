/* raw support chatter is not a consumed jump */
#include "harness.h"
#define csmemset memset
#define RIDING_TICKS 60
enum { _ride_driven, _ride_brush, _ride_phase_jump };
#include "types.inc"
static struct { short scenario; } ride;
#include "under_test.inc"
int main(int argc, char **argv) {
	char const *case_name = argv[1];
	struct ride_player rider;
	long tick;
	memset(&rider, 0, sizeof(rider)); ride.scenario = _ride_brush;
	CASE("remote-takeoff") {
		ride_outcome_sample(&rider, 89, TRUE, TRUE, FALSE);
		CHECK(ride_outcome_observe(&rider, 90, FALSE, .07f), "remote physical take-off not observed");
		ride_outcome_sample(&rider, 90, FALSE, FALSE, FALSE);
		CHECK(!ride_outcome_observe(&rider, 91, FALSE, .0665f), "air episode opened twice");
		ride_outcome_sample(&rider, 91, FALSE, FALSE, FALSE);
		ride_outcome_sample(&rider, 110, TRUE, TRUE, FALSE);
		ride_outcome_sample(&rider, 111, TRUE, TRUE, FALSE);
		CHECK(rider.jumps == 1 && rider.landings == 1, "remote episode not exactly once"); return 0;
	}
	CASE("remote-chatter") {
		for (tick = 1; tick <= 1000; tick++) {
			ride_outcome_observe(&rider, tick, tick & 1, .01f);
			ride_outcome_sample(&rider, tick, tick & 1, tick & 1, FALSE);
		}
		CHECK(rider.jumps == 0 && rider.landings == 0 && rider.support_losses > 0, "remote chatter fabricated launches"); return 0;
	}
	CASE("remote-no-support") {
		CHECK(!ride_outcome_observe(&rider, 90, FALSE, .07f), "air velocity without prior support opened episode");
		CHECK(rider.jumps == 0, "remote with no take-offs satisfies launch coverage"); return 0;
	}
	CASE("owner-observation") {
		rider.local = TRUE;
		ride_outcome_sample(&rider, 89, TRUE, TRUE, FALSE);
		CHECK(!ride_outcome_observe(&rider, 90, FALSE, .07f), "owner counted unconsumed observed launch"); return 0;
	}
	CASE("chatter") {
		for (tick = 1; tick <= 1000; tick++) ride_outcome_sample(&rider, tick, tick & 1, tick & 1, FALSE);
		CHECK(rider.jumps == 0 && rider.landings == 0, "60/59 chatter fabricated outcomes"); return 0;
	}
	CASE("ten-episodes") {
		for (tick = 1; tick <= 10; tick++) {
			ride_outcome_launch(&rider, tick*90);
			ride_outcome_launch(&rider, tick*90);
			ride_outcome_sample(&rider, tick*90, FALSE, FALSE, FALSE);
			ride_outcome_sample(&rider, tick*90 + 1, TRUE, TRUE, FALSE);
			ride_outcome_sample(&rider, tick*90 + 2, FALSE, FALSE, FALSE);
			ride_outcome_sample(&rider, tick*90 + 3, TRUE, TRUE, FALSE);
		}
		CHECK(rider.jumps == 10 && rider.landings == 10, "not once per consumed episode"); return 0;
	}
	CASE("off-deck") {
		ride_outcome_launch(&rider, 90);
		ride_outcome_sample(&rider, 91, TRUE, FALSE, FALSE);
		ride_outcome_sample(&rider, 92, TRUE, FALSE, FALSE);
		ride_outcome_sample(&rider, 93, TRUE, TRUE, FALSE);
		CHECK(rider.jumps == 1 && rider.landings == 1 && rider.landed_off == 1, "off-deck landing double counted"); return 0;
	}
	CASE("driven-recovery") {
		ride.scenario = _ride_driven;
		ride_outcome_launch(&rider, 90);
		ride_outcome_sample(&rider, 91, TRUE, FALSE, FALSE);
		ride_outcome_sample(&rider, 92, TRUE, TRUE, TRUE);
		ride_outcome_sample(&rider, 93, TRUE, TRUE, TRUE);
		CHECK(rider.jumps == 1 && rider.landings == 0 && rider.episode_recoveries == 1, "recovery counted as landing or twice"); return 0;
	}
	CASE("uncommanded") {
		ride_outcome_sample(&rider, 89, TRUE, TRUE, FALSE);
		ride_outcome_sample(&rider, 90, FALSE, FALSE, FALSE);
		ride_outcome_sample(&rider, 91, FALSE, FALSE, FALSE);
		CHECK(rider.support_losses == 1 && rider.jumps == 0, "uncommanded loss not distinct"); return 0;
	}
	return 2;
}

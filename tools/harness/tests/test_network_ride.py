"""The riding tests' script (port/linux/game/network_test.c, debug.network_test_ride: port/linux/NETCODE.md).

    python -m pytest -q tools/harness/tests/test_network_ride.py
"""

import re
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from harness import CHECK_FAILED, build, constant, enum_with, function, mutated, read, run, structure  # noqa: E402

SOURCE = "port/linux/game/network_test.c"
CASES = ["ride-phases", "ride-motion", "ride-script", "ride-height", "ride-level-spot", "ride-level-lane", "ride-level-none",
         "ride-board", "ride-reboard", "ride-brush-path", "ride-level-choice", "ride-brush-observe", "ride-coop-flanks",
         "ride-jump-late", "ride-level-run", "ride-level-turn", "ride-level-cross", "ride-level-reach", "ride-convoy-height", "ride-level-best", "ride-convoy-hold", "ride-reboard-authority", "ride-convoy-along", "ride-convoy-hull", "ride-convoy-board-speed", "ride-convoy-presses", "ride-convoy-cross-phase", "ride-convoy-corridor",
          "ride-tilt-schedule", "ride-tilt-frame", "ride-tilt-action", "ride-tilt-spot", "ride-pilot", "ride-pilot-hover", "ride-live-jumps", "ride-live-door-skew", "ride-live-climb", "ride-live-recover", "ride-live-right-turn", "ride-live-preboard"]
CASES += ["ride-local-inputs", "ride-rear-corner"]
CASES += ["ride-tilt-lands", "ride-door-needs-landing"]
CASES += ["ride-door-latched", "ride-live-descent-contact"]
CASES += ["ride-door-legs"]
CASES += ["ride-driven-recovers", "ride-driven-steady", "ride-driven-no-jump", "ride-driven-timeout", "ride-driven-limit"]
CASES += ["ride-driven-waits-for-motion"]
CASES += ["ride-driven-deferred-presses"]
CASES += ["ride-driven-recovery-budget"]
CASES += ["ride-driven-post-physics", "ride-driven-prepare-before-score"]
CASES += ["ride-driven-deck-landings"]
CASES += ["ride-stock-preacquisition", "ride-first-jump-supported", "ride-setup-shuffle", "ride-walk-anchored-shuffle", "ride-walk-offset-owner-only", "ride-jump-recentre", "ride-jump-recentre-caller"]
CASES += ["ride-setup-corrections"]
CASES += ["ride-board-support-required"]
CASES += ["ride-contact-wiring", "ride-contact-transaction", "ride-setup-contact-age", "ride-observation-disabled"]
CASES += ["ride-convoy-step-off-outboard"]
CASES += ["ride-drive-after-equals-next", "ride-drive-host-motion-unchanged", "ride-drive-client-untouched", "ride-drive-driven-untouched", "ride-drive-after-end-untouched", "ride-drive-after-rest-untouched", "ride-drive-samples-post-step"]
FUNCTIONS = ["ride_phase_tick", "ride_phase", "ride_motion", "ride_jump_slot", "ride_step_off_side", "ride_script_action", "ride_height_from_hit", "ride_level_along", "ride_level_beside", "ride_level_across",
             "ride_level_flanks", "ride_level_heading", "ride_convoy_height", "ride_convoy_lateral", "ride_convoy_turn", "ride_level_ring_points", "ride_level_spot", "ride_level_run", "ride_level_reach", "ride_level_turn", "ride_tilt_ramp", "ride_tilt_at", "ride_tilt_end", "ride_tilt_pair",
              "ride_tilt_frame", "ride_tilt_action", "ride_press_slot", "ride_spot_parse", "ride_vehicle_named", "ride_board_spot", "ride_board_candidate", "ride_reboard_due", "ride_level_scenario", "ride_pilot_at", "ride_pilot_action", "ride_tilt_landed", "ride_contact_height",
                "ride_reboard", "ride_driven_recover", "ride_driven_holding", "ride_setup_supported", "ride_drive_motion", "network_test_ride_before_objects", "ride_deck_edge", "ride_convoy_place", "network_test_ride_action", "ride_outcome_launch", "ride_outcome_observe", "ride_outcome_sample", "ride_rider_log", "ride_drive_after", "network_test_ride_after_objects", "network_test_ride_correction"]
LIVE = ("ride_board_candidate", "ride_reboard", "ride_driven_recover", "ride_driven_holding", "ride_setup_supported", "ride_drive_motion", "network_test_ride_before_objects", "ride_deck_edge", "ride_convoy_place", "network_test_ride_action", "ride_outcome_launch", "ride_outcome_observe", "ride_outcome_sample", "ride_rider_log", "ride_drive_after", "network_test_ride_after_objects", "network_test_ride_correction")

CONTACT = ("ride_contact_player", "network_test_ride_launch", "network_test_ride_contact_begin",
           "network_test_ride_contact", "network_test_ride_contact_end")
FUNCTIONS += list(CONTACT)
LIVE += CONTACT

# the convoy's cross-slope check in ride_level_heading
CROSS = "\t\tif (cross > RIDE_CONVOY_CROSS_RISE)\n\t\t\tcontinue;\n"

FIRST = "if (!found || rise < best - RIDE_CONVOY_CROSS_TIE)"
FIRST_HEADING = "if (!found || cross < best - RIDE_CONVOY_CROSS_TIE)"

# a fault in network_test.c, and the case that must catch it
NEGATIVE_CONTROLS = {
    "drive-after-missing": ("\tif (!ride.failed && !ride.ended)\n\t\tride_drive_after(tick);\n", "", "ride-drive-after-equals-next"),
    "drive-after-this-tick": ("ride_drive_motion(index, tick + 1 - ride.board_tick,", "ride_drive_motion(index, tick - ride.board_tick,", "ride-drive-after-equals-next"),
    "drive-after-on-client": ("if (!ride_host() || ride.scenario == _ride_driven || ride.scenario == _ride_longsword)", "if (ride.scenario == _ride_driven || ride.scenario == _ride_longsword)", "ride-drive-client-untouched"),
    "drive-after-driven": ("if (!ride_host() || ride.scenario == _ride_driven || ride.scenario == _ride_longsword)", "if (!ride_host() || ride.scenario == _ride_longsword)", "ride-drive-driven-untouched"),
    "drive-after-ended": ("\tif (!ride.failed && !ride.ended)\n\t\tride_drive_after(tick);\n", "\tif (!ride.failed)\n\t\tride_drive_after(tick);\n", "ride-drive-after-end-untouched"),
    "drive-after-clears-rest": ("if (!object || TEST_FLAG(object->object.flags, _object_at_rest_bit))\n\t\t\tcontinue;", "if (!object)\n\t\t\tcontinue;\n\t\tSET_FLAG(object->object.flags, _object_at_rest_bit, FALSE);", "ride-drive-after-rest-untouched"),
    "drive-after-activates": ("\t\tobject->object.angular_velocity = angular;\n\t}\n}\n\nvoid network_test_ride_after_objects(", "\t\tobject->object.angular_velocity = angular;\n\t\tobject_activate(ride.vehicles[index]);\n\t}\n}\n\nvoid network_test_ride_after_objects(", "ride-drive-after-rest-untouched"),
    "drive-after-writes-resting": ("if (!object || TEST_FLAG(object->object.flags, _object_at_rest_bit))\n\t\t\tcontinue;", "if (!object)\n\t\t\tcontinue;", "ride-drive-after-rest-untouched"),
    "drive-after-before-samples": ("\tride.after_tick = tick;\n\tride_samples(tick);\n", "\tride.after_tick = tick;\n\tride_drive_after(tick);\n\tride_samples(tick);\n", "ride-drive-samples-post-step"),
    "drive-before-dropped": ("\t\tride_drive_motion(index, tick - ride.board_tick, object, &velocity, &angular);\n\t\tobject->object.translational_velocity = velocity;", "\t\tvelocity = object->object.translational_velocity;\n\t\tangular = object->object.angular_velocity;\n\t\tobject->object.translational_velocity = velocity;", "ride-drive-host-motion-unchanged"),
    "convoy-step-inboard": ("ride_step_off_side(scenario, deck_vehicle) * RIDE_SIDE_THROTTLE", "-RIDE_SIDE_THROTTLE", "ride-convoy-step-off-outboard"),
    "convoy-step-assigned": ("deck_vehicle = deck.vehicle_index;", "deck_vehicle = ride.players[absolute].assigned;", "ride-convoy-step-off-outboard"),
    "convoy-step-forgets-departure": ("deck_vehicle = ride.players[absolute].previous_vehicle;", "deck_vehicle = NONE;", "ride-convoy-step-off-outboard"),
    "contact-stale-call-site": ("rider->contact_valid && rider->contact_tick == tick &&", "rider->contact_valid &&", "ride-contact-wiring"),
    "contact-ticks-call-site": ("rider->contact_valid && rider->contact_tick == tick &&", "(rider->contact_valid || (present && deck.ticks == RIDING_TICKS)) && rider->contact_tick == tick &&", "ride-contact-wiring"),
    "contact-airborne-call-site": ("!TEST_FLAG(biped->biped.flags, _biped_airborne_bit), rider->contact_vehicle, vehicle_index,", "TRUE, rider->contact_vehicle, vehicle_index,", "ride-contact-wiring"),
    "contact-end-wrong-support": ("grounded && rider->contact_vehicle == support;", "grounded;", "ride-contact-transaction"),
    "contact-end-airborne": ("grounded && rider->contact_vehicle == support;", "rider->contact_vehicle == support;", "ride-contact-transaction"),
    "contact-end-stale": ("rider->contact_valid && rider->contact_tick == game_time_get() &&", "rider->contact_valid &&", "ride-contact-transaction"),
    "contact-begin-keeps-old": ("rider->contact_valid = FALSE;", "(void)0;", "ride-contact-transaction"),
    "setup-stale-contact": (" || rider->contact_tick != tick - 1", "", "ride-setup-contact-age"),
    "observation-test-off": ("if (!ride.enabled || ride.failed || ride.ended || !ride.prepared)\n\t\treturn NULL;", "if (ride.failed || ride.ended || !ride.prepared)\n\t\treturn NULL;", "ride-observation-disabled"),
    "observation-unprepared": ("if (!ride.enabled || ride.failed || ride.ended || !ride.prepared)\n\t\treturn NULL;", "if (!ride.enabled || ride.failed || ride.ended)\n\t\treturn NULL;", "ride-observation-disabled"),
    "stock-prepares-too-late": ("ride.scenario != _ride_longsword ? RIDE_DRIVEN_PREBOARD : 0", "ride.scenario == _ride_driven ? RIDE_DRIVEN_PREBOARD : 0", "ride-stock-preacquisition"),
    "setup-support-ignored": ("tick == ride.board_tick + RIDE_JUMP && !ride_setup_supported(tick))", "tick == ride.board_tick + RIDE_JUMP && FALSE)", "ride-first-jump-supported"),
    "setup-board-support-ignored": ("tick == ride.board_tick && !ride_setup_supported(tick))", "tick == ride.board_tick && FALSE)", "ride-board-support-required"),
    "walk-unanchored": (["if (walk_offset >= RIDE_WALK_RANGE)\n\t\t\taction->throttle.i = -RIDE_WALK_THROTTLE;\n\t\telse if (walk_offset <= -RIDE_WALK_RANGE)\n\t\t\taction->throttle.i = RIDE_WALK_THROTTLE;\n"], [""], "ride-walk-anchored-shuffle"),
    "walk-anchor-sign": (["if (walk_offset >= RIDE_WALK_RANGE)\n\t\t\taction->throttle.i = -RIDE_WALK_THROTTLE;\n\t\telse if (walk_offset <= -RIDE_WALK_RANGE)\n\t\t\taction->throttle.i = RIDE_WALK_THROTTLE;\n"], ["if (walk_offset >= RIDE_WALK_RANGE)\n\t\t\taction->throttle.i = RIDE_WALK_THROTTLE;\n\t\telse if (walk_offset <= -RIDE_WALK_RANGE)\n\t\t\taction->throttle.i = -RIDE_WALK_THROTTLE;\n"],
                         "ride-walk-anchored-shuffle"),
    "walk-anchor-before-inversion": (["\t\tif (walk_offset >= RIDE_WALK_RANGE)\n\t\t\taction->throttle.i = -RIDE_WALK_THROTTLE;\n\t\telse if (walk_offset <= -RIDE_WALK_RANGE)\n\t\t\taction->throttle.i = RIDE_WALK_THROTTLE;\n", "\t\t/* (under the gun, start away from the mount, then return to the spot) */\n"],
                                     ["", "\t\tif (walk_offset >= RIDE_WALK_RANGE)\n\t\t\taction->throttle.i = -RIDE_WALK_THROTTLE;\n\t\telse if (walk_offset <= -RIDE_WALK_RANGE)\n\t\t\taction->throttle.i = RIDE_WALK_THROTTLE;\n\t\t/* (under the gun, start away from the mount, then return to the spot) */\n"],
                                     "ride-walk-anchored-shuffle"),
    "walk-offset-not-owner-only": ("ride.players[absolute].local &&\n\t\t\t(phase == _ride_phase_walk", "\n\t\t\t(phase == _ride_phase_walk", "ride-walk-offset-owner-only"),
    "walk-origin-lost": ("rider->walk_origin = deck.place.x;", "rider->walk_origin = 0.f;", "ride-walk-offset-owner-only"),
    "walk-origin-every-call": ("rider->walk_origin_valid = TRUE;", "rider->walk_origin_valid = FALSE;", "ride-walk-offset-owner-only"),
    "walk-node-ignored": ([" || rider->walk_node != deck.node_index", "rider->walk_origin_valid && rider->walk_node == deck.node_index"], ["", "rider->walk_origin_valid"], "ride-walk-offset-owner-only"),
    "recentre-airborne": ("else if (grounded && (elapsed", "else if ((elapsed", "ride-jump-recentre"),
    "recentre-never": ("else if (grounded &&", "else if (FALSE &&", "ride-jump-recentre"),
    "recentre-in-settle": (" && (elapsed - RIDE_JUMP) % RIDE_JUMP_PERIOD < RIDE_JUMP_PERIOD - RIDE_JUMP_SETTLE)", ")", "ride-jump-recentre"),
    "recentre-caller-airborne": ("grounded = !TEST_FLAG(biped->biped.flags, _biped_airborne_bit) && deck.ticks == RIDING_TICKS;", "grounded = deck.ticks == RIDING_TICKS;", "ride-jump-recentre-caller"),
    "recentre-caller-unsupported": ("grounded = !TEST_FLAG(biped->biped.flags, _biped_airborne_bit) && deck.ticks == RIDING_TICKS;", "grounded = !TEST_FLAG(biped->biped.flags, _biped_airborne_bit);", "ride-jump-recentre-caller"),
    "recentre-caller-other-node": ("if (rider->walk_origin_valid && rider->walk_node == deck.node_index)\n\t\t\t\t{", "if (rider->walk_origin_valid)\n\t\t\t\t{", "ride-jump-recentre-caller"),
    "stock-shuffle-reversed": ("if (scenario == _ride_gun || scenario == _ride_coop || scenario == _ride_coop_double || scenario == _ride_convoy)", "if (scenario == _ride_gun || scenario == _ride_coop || scenario == _ride_coop_double || scenario == _ride_convoy || scenario == _ride_brush || scenario == _ride_rise)", "ride-setup-shuffle"),
    "recovery-counted-as-landing": ("rider->episode_recoveries++;", "rider->episode_recoveries++; rider->landings++;", "ride-driven-deck-landings"),
    "driven-counts-window-as-contact": ("riding && deck.vehicle_index == rider->assigned ? rider->recovery_contacts + 1 : 0", "present && deck.vehicle_index == rider->assigned ? rider->recovery_contacts + 1 : 0", "ride-driven-post-physics"),
    "driven-prepares-at-score": ("RIDE_DRIVEN_PREBOARD = RIDE_PREPARE_TICKS/2", "RIDE_DRIVEN_PREBOARD = 0", "ride-driven-prepare-before-score"),
    "driven-scores-setup": ("tick < ride.board_tick || tick == ride.after_tick", "tick == ride.after_tick", "ride-driven-prepare-before-score"),
    "driven-pilot-starts-during-setup": ("tick < ride.board_tick || ride_driven_holding(tick)", "ride_driven_holding(tick)", "ride-driven-prepare-before-score"),
    "driven-old-jump-length": ("RIDE_DRIVEN_EXTRA = RIDE_RECOVERY_MAX*(RIDE_RECOVERY_TIMEOUT + RIDE_RECOVERY_SETTLE)", "RIDE_DRIVEN_EXTRA = 0", "ride-driven-recovery-budget"),
    "driven-shifts-every-scenario": ("scenario == _ride_driven && tick >= RIDE_CROSS", "tick >= RIDE_CROSS", "ride-driven-recovery-budget"),
    "driven-recovery-drops-deferred-press": ("else if (ride.scenario == _ride_driven && !driver && absolute < RIDE_PLAYERS &&", "else if (FALSE && !driver && absolute < RIDE_PLAYERS &&", "ride-driven-deferred-presses"),
    "driven-recovery-disabled": ("ride_driven_recover(tick);", "", "ride-driven-recovers"),
    "driven-recovery-ignores-motion": ("rider->recovery_steady >= RIDE_RECOVERY_CONTACT", "TRUE", "ride-driven-waits-for-motion"),
    "driven-recovery-still-turns": ("if (ride.scenario == _ride_driven && (tick < ride.board_tick || ride_driven_holding(tick)))", "if (FALSE)", "ride-driven-steady"),
    "driven-recovery-keeps-throttle": ("action->throttle.i = action->throttle.j = 0.f;", "action->throttle.i = RIDE_DRIVE_THROTTLE;", "ride-driven-steady"),
    "driven-recovery-no-brake": ("SET_FLAG(action->control_flags, _unit_control_jump_bit, TRUE);\n\t\t\taction->desired_facing.yaw = ride_heading", "SET_FLAG(action->control_flags, _unit_control_jump_bit, FALSE);\n\t\t\taction->desired_facing.yaw = ride_heading", "ride-driven-steady"),
    "driven-recovery-ignores-linear": ("dot_product3d(&frame.translational_velocity, &frame.translational_velocity) < RIDE_RECOVERY_LINEAR*RIDE_RECOVERY_LINEAR", "TRUE", "ride-driven-waits-for-motion"),
    "driven-recovery-one-stopped-tick": ("rider->recovery_steady >= RIDE_RECOVERY_CONTACT", "rider->recovery_steady >= 1", "ride-driven-waits-for-motion"),
    "driven-recovery-no-timeout": ("tick - rider->recovery_start >= RIDE_RECOVERY_TIMEOUT", "FALSE", "ride-driven-timeout"),
    "driven-recovery-no-limit": ("++rider->recoveries > RIDE_RECOVERY_MAX", "++rider->recoveries > 1000", "ride-driven-limit"),
    "driven-recovery-before-jump": ("rider->jumps == 0 || ", "", "ride-driven-no-jump"),
    "ramp-never-opened": ("unit_open(ride.vehicles[0]);", "", "ride-door-latched"),
    "ramp-before-landing": ("if (ride.door_started)\n\t\t\t{", "if (TRUE)\n\t\t\t{", "ride-door-needs-landing"),
    "door-legs-backward": ("action->throttle.i = (walk >= 60 && walk < 92) || (walk >= 132 && walk < 282) ? 1.f : 0.f;", "action->throttle.i = -1.f;", "ride-door-legs"),
    "door-latch-removed": ("else if (!ride.door_started)", "else", "ride-door-latched"),
    "door-latch-not-reset": ("ride.door_started = FALSE;", "ride.door_started = ride.door_started;", "ride-door-latched"),
    "pilot-contact-always-zero": ("vehicle_datum_get(ride.vehicles[0])->vehicle.on_ground_ticks, action);", "0, action);", "ride-live-descent-contact"),
    "pilot-contact-always-grounded": ("vehicle_datum_get(ride.vehicles[0])->vehicle.on_ground_ticks, action);", "1, action);", "ride-live-descent-contact"),
    # (the retargeted legacy name now inverts the active pilot's contact predicate, not a gain)
    "landing-asymptotic": ("if (step == 11 && grounded == 0)", "if (step == 11 && grounded > .3f)", "ride-tilt-lands"),
    "door-before-landing": ("!driver && step == 12 && ride.door_started", "!driver && step == 12", "ride-door-needs-landing"),
    "door-one-contact": ("return grounded >= 30;", "return grounded >= 1;", "ride-door-needs-landing"),
    "convoy-extra-cross-jump": ("action->throttle.j = RIDE_SIDE_THROTTLE;", "action->throttle.j = RIDE_SIDE_THROTTLE;\n\t\t\tSET_FLAG(action->control_flags, _unit_control_jump_bit, TRUE);", "ride-convoy-presses"),
    "convoy-historical-stop": ("if (driver || absolute >= RIDE_PLAYERS)\n\t\treturn;", "if (ride.scenario == _ride_convoy && absolute < RIDE_PLAYERS && ride.players[absolute].crossings > 0)\n\t\taction->throttle.j = 0.f;\n\tif (driver || absolute >= RIDE_PLAYERS)\n\t\treturn;", "ride-convoy-presses"),
    "convoy-board-no-motion": ("unit->object.translational_velocity = object_get(vehicle_index)->object.translational_velocity;",
                               "unit->object.translational_velocity.i = unit->object.translational_velocity.j = unit->object.translational_velocity.k = 0.f;", "ride-convoy-board-speed"),
    "convoy-board-every-motion": ("if (ride.scenario == _ride_convoy)\n\t\t\tunit->object.translational_velocity", "if (TRUE)\n\t\t\tunit->object.translational_velocity", "ride-convoy-board-speed"),
    "convoy-hull-bed-only": ("for (row = -12; row <= 12; row++)", "for (row = -6; row <= -6; row++)", "ride-convoy-hull"),
    "convoy-hull-centre-miss": ("if (!ride_deck_surface(vehicle_index, &point, 0.f, -1.f, &surface))\n\t\t\t\tcontinue;", "if (!ride_deck_surface(vehicle_index, &point, 0.f, -1.f, &surface))\n\t\t\t\tbreak;", "ride-convoy-hull"),
    "convoy-hull-no-clearance": ("- RIDE_CONVOY_HULL_GAP;", "- RIDE_CONVOY_GAP;", "ride-convoy-hull"),
    "convoy-hull-last-row": ("if (step > widest)", "if (step >= 0*widest)", "ride-convoy-hull"),
    "convoy-hull-old-tolerance": ("gap <= RIDE_CONVOY_HULL_GAP + RIDE_CONVOY_MAXIMUM_GAP", "gap <= RIDE_CONVOY_MAXIMUM_GAP", "ride-convoy-hull"),
    "convoy-reboard-client-only": ("!(ride.scenario == _ride_convoy && ride_host())", "TRUE", "ride-reboard-authority"),
    "convoy-reboard-client-remote": ("!(ride.scenario == _ride_convoy && ride_host())", "FALSE", "ride-reboard-authority"),
    "convoy-reboard-every-scenario": ("!(ride.scenario == _ride_convoy && ride_host())", "!ride_host()", "ride-reboard-authority"),
    "convoy-along-none": ("velocity->i += forward.i*along;\n\t\t\tvelocity->j += forward.j*along;",
                           "velocity->i += 0.f*along;\n\t\t\tvelocity->j += 0.f*along;", "ride-convoy-along"),
    "convoy-cross-wrong-phase": ("if (phase == _ride_phase_cross)\n\t\t\trider->crossings++;", "if (TRUE)\n\t\t\trider->crossings++;", "ride-convoy-cross-phase"),
    "convoy-settle-cross-only": ("if (riding && rider->previous_vehicle != NONE && rider->previous_vehicle != deck.vehicle_index)",
                                  "if (riding && phase == _ride_phase_cross && rider->previous_vehicle != NONE && rider->previous_vehicle != deck.vehicle_index)",
                                  "ride-convoy-cross-phase"),
    "convoy-cross-count-window": ("riding = present && deck.ticks == RIDING_TICKS;", "riding = present;", "ride-convoy-cross-phase"),
    "convoy-cross-none": ("rider->crossings++;", "rider->crossings += 0;", "ride-convoy-cross-phase"),
    "convoy-cross-no-stop": ("ride.players[absolute].crossings > 0)", "FALSE)", "ride-convoy-presses"),
    "convoy-cross-no-hop": ("last_jump < RIDE_JUMPS || (pressed_now && last_jump == RIDE_JUMPS)", "FALSE", "ride-convoy-presses"),
    "convoy-cross-hop-no-repeat": ("last_jump < RIDE_JUMPS || (pressed_now && last_jump == RIDE_JUMPS)", "last_jump < RIDE_JUMPS", "ride-convoy-presses"),
    "convoy-cross-hop-no-run-up": ("RIDE_CONVOY_HOP_TICKS = 8,", "RIDE_CONVOY_HOP_TICKS = 0,", "ride-convoy-presses"),
    "convoy-cross-hop-wrong-slot": ("ride_phase(ride.scenario, tick - ride.board_tick) == _ride_phase_cross ? RIDE_JUMPS :", "FALSE ? RIDE_JUMPS :", "ride-convoy-presses"),
    "convoy-corridor-follow-drift": ("forward.j = (real)sin(ride.convoy_yaw);", "forward.j = (real)sin(own_yaw);", "ride-convoy-corridor"),
    "convoy-corridor-no-restore": ("velocity->j += left.j*sideways;", "velocity->j += 0.f*sideways;", "ride-convoy-corridor"),
    "convoy-corridor-yaw-unheld": ("angular->k += ride_convoy_turn(ride.convoy_yaw - own_yaw);", "angular->k += 0.f*ride_convoy_turn(ride.convoy_yaw - own_yaw);", "ride-convoy-corridor"),
    "convoy-corridor-origin-lost": ("ride.convoy_origin = first->object.position;", "ride.convoy_origin.x = 0.f;", "ride-convoy-hull"),
    "convoy-corridor-yaw-lost": ("ride.convoy_yaw = (real)atan2(first->object.forward.j, first->object.forward.i);", "ride.convoy_yaw += 0.f;", "ride-convoy-hull"),
    "convoy-along-sign": ("along = ride_convoy_lateral(-((", "along = ride_convoy_lateral(((", "ride-convoy-along"),
    "convoy-along-x-only": ("velocity->j += forward.j*along;", "", "ride-convoy-along"),
    "convoy-boards-centre": ("scenario == _ride_convoy ? RIDE_BOARD_SIDE : ", "", "ride-board"),
    "convoy-boards-mount": ("scenario == _ride_convoy ? under_gun[index] :\n\t\t", "", "ride-board"),
    "convoy-walks-mount": (" || scenario == _ride_convoy)\n\t\t\taction->throttle.i", ")\n\t\t\taction->throttle.i", "ride-script"),
    "corner-walk-disabled": ("if (ride.corner && network_test.mode == _network_test_local && step == 5)", "if (FALSE)", "ride-rear-corner"),
    "corner-ungated-by-mode": ("ride.corner && network_test.mode == _network_test_local && step == 5", "ride.corner && step == 5", "ride-rear-corner"),
    "local-rider-input-suppressed": ("local_player_index >= MAXIMUM_LOCAL_PLAYERS", "local_player_index >= 1", "ride-local-inputs"),
    "hover-skipped": ("RIDE_PILOT_LEVEL, RIDE_PILOT_HOVER, RIDE_PILOT_PITCH,",
                      "RIDE_PILOT_LEVEL, RIDE_PILOT_PITCH,", "ride-pilot-hover"),
    "hover-throttle": ("action->desired_facing.pitch = 0.f;\n\tif (step == 1 || step == 2)",
                       "action->desired_facing.pitch = 0.f;\n\taction->throttle.i = step == 4 ? .5f : 0.f;\n\tif (step == 1 || step == 2)", "ride-pilot-hover"),
    "pilot-live-exit-sink": ("action->desired_facing.pitch = 6.f*_pi/180.f;",
                              "action->desired_facing.pitch = _pi/180.f;", "ride-live-right-turn"),
    "pilot-live-preboard-uncontrolled": ("if (!ride.prepared)\n\t{\n\t\tif (driver)",
                                          "if (!ride.prepared)\n\t{\n\t\tif (FALSE)", "ride-live-preboard"),
    "pilot-live-no-level-exit": ("action->throttle.i = hold < 420 ? (hold - 179)/480.f : .5f;",
                                  "action->throttle.i = 0.f;", "ride-live-right-turn"),
    "pilot-live-abrupt-exit": ("action->throttle.i = hold < 420 ? (hold - 179)/480.f : .5f;",
                                "action->throttle.i = 1.f;", "ride-live-right-turn"),
    "pilot-live-turn-hover-alias": ("hold % 3 == 0", "hold % 2 == 0", "ride-live-right-turn"),
    "pilot-live-turn-left": ("ride.tilt_yaw + (step >= 2 ? _pi/2.f : 0.f)",
                              "ride.tilt_yaw - (step >= 2 ? _pi/2.f : 0.f)", "ride-live-right-turn"),
    "pilot-live-no-turn": ("ride.tilt_yaw + (step >= 2 ? _pi/2.f : 0.f)",
                            "ride.tilt_yaw", "ride-live-right-turn"),
    "pilot-recovery-no-flight": ("if (hold >= 150)\n\t\t\t{", "if (FALSE)\n\t\t\t{", "ride-live-recover"),
    "pilot-recovery-no-turn": ("action->desired_facing.yaw = ride.tilt_yaw + _pi/2.f + _pi;", "action->desired_facing.yaw = 0.f;", "ride-live-recover"),
    "pilot-recovery-no-lift": ("if (step == 10)\n\t\tSET_FLAG(action->control_flags, _unit_control_jump_bit, TRUE);",
                                "if (FALSE)\n\t\tSET_FLAG(action->control_flags, _unit_control_jump_bit, TRUE);", "ride-live-recover"),
    "pilot-climb-full-lift": ("if (driver && step == 1 && hold >= 20 && hold % 15 >= 7)", "if (FALSE)", "ride-live-climb"),
    "pilot-door-early": ("#define RIDE_PILOT_DOOR 8100", "#define RIDE_PILOT_DOOR 8000", "ride-live-door-skew"),
    "pilot-no-climb": ("if (step == 1 || step == 2)", "if (FALSE)", "ride-pilot"),
    "pilot-no-pitch": ("if (step == 5)", "if (FALSE)", "ride-pilot"),
    "pilot-no-combined-pitch": ("if (step == 9)", "if (FALSE)", "ride-pilot"),
    "pilot-live-no-jump": ("if (!driver && (step == 0 || step == 3 || step == 6 || step == 8) && hold >= 120 &&",
                            "if (FALSE &&", "ride-live-jumps"),
    "pilot-live-no-door": ("action->throttle.i = (walk >= 60 && walk < 92) || (walk >= 132 && walk < 282) ? 1.f : 0.f;", "action->throttle.i = 0.f;", "ride-live-door-skew"),
    "pilot-live-local-phase": ("short step = ride_pilot_at(tick - ride.board_tick, &hold);\n\n\t\tride_pilot_action",
                               "short step = (hold = 0, ride.pilot_step);\n\n\t\tride_pilot_action", "ride-live-door-skew"),
    "pilot-no-throttle": ("if (step == 5 || step == 7 || step == 9)\n\t{\n\t\taction->throttle.i = 1.f;",
                            "if (step == 5 || step == 7 || step == 9)\n\t{\n\t\taction->throttle.i = 0.f;", "ride-pilot"),
    "pilot-no-bank": ("action->desired_facing.yaw = yaw - 1.2f;", "action->desired_facing.yaw = yaw;", "ride-pilot"),
    "pilot-no-descent": ("if (step == 11 && grounded == 0)", "if (FALSE)", "ride-pilot"),
    "brush-bank-unchecked": (" || scenario == _ride_brush;", ";", "ride-level-choice"),
    "coop-bank-unchecked": ("scenario == _ride_coop || scenario == _ride_coop_double || scenario == _ride_brush;",
                            "scenario == _ride_brush;", "ride-level-choice"),
    "coop-left-flank-unchecked": ("!ride_level_beside(point, angle, length, half) || ", "", "ride-coop-flanks"),
    "coop-right-flank-unchecked": (" || !ride_level_beside(point, angle, length, -half)", "", "ride-coop-flanks"),
    "coop-cross-slope-unchecked": ("fabs(left_ground - ground) > RIDE_COOP_CROSS_RISE ||\n\t\t\tfabs(right_ground - ground) > RIDE_COOP_CROSS_RISE",
                                   "FALSE", "ride-coop-flanks"),
    "brush-no-approach": ("elapsed < RIDE_STEP_OFF + RIDE_BRUSH_STEP_TICKS + RIDE_BRUSH_APPROACH_TICKS", "FALSE", "ride-brush-path"),
    "brush-window-expired": ("elapsed >= RIDE_STEP_OFF + RIDE_BRUSH_STEP_TICKS", "elapsed >= RIDE_OBSERVE", "ride-brush-path"),
    "brush-rams": ("RIDE_BRUSH_APPROACH_TICKS = 11,", "RIDE_BRUSH_APPROACH_TICKS = 40,", "ride-brush-path"),
    "brush-observe-too-late": ("scenario == _ride_brush ? RIDE_STEP_OFF + RIDE_BRUSH_STEP_TICKS : ride_phase_tick(scenario, RIDE_OBSERVE)",
                                "ride_phase_tick(scenario, RIDE_OBSERVE)", "ride-brush-observe"),
    "brush-approaches-left": ("velocity->j = -forward->i*RIDE_BRUSH_SPEED;", "velocity->j = forward->i*RIDE_BRUSH_SPEED;", "ride-brush-path"),
    "brush-too-fast": ("#define RIDE_BRUSH_SPEED 0.03f", "#define RIDE_BRUSH_SPEED 0.07f", "ride-brush-path"),
    "phase-off-by-one": ("\tif (elapsed < RIDE_WALK)\n", "\tif (elapsed <= RIDE_WALK)\n", "ride-phases"),
    "nine-jumps": ("RIDE_JUMP_PERIOD = 90,", "RIDE_JUMP_PERIOD = 100,", "ride-jump-late"),
    "jump-exact-tick-only": ("ride_jump_slot(elapsed) > last_jump || (pressed_now && ride_jump_slot(elapsed) == last_jump)",
                             "(elapsed - RIDE_JUMP) % RIDE_JUMP_PERIOD == 0", "ride-jump-late"),
    "jump-slot-wrong-period": ("return (short)((elapsed - RIDE_JUMP)/RIDE_JUMP_PERIOD);", "return (short)((elapsed - RIDE_JUMP)/100);", "ride-jump-late"),
    "jump-pressed-twice-a-slot": ("ride_jump_slot(elapsed) > last_jump ||", "ride_jump_slot(elapsed) >= last_jump ||", "ride-jump-late"),
    "jump-repeat-ask-unpressed": ("ride_jump_slot(elapsed) > last_jump || (pressed_now && ride_jump_slot(elapsed) == last_jump)",
                                  "ride_jump_slot(elapsed) > last_jump", "ride-jump-late"),
    "jump-past-the-phase": ("\tif (elapsed < ride_phase_tick(scenario, RIDE_CROSS))\n\t\treturn _ride_phase_jump;", "\tif (elapsed < ride_phase_tick(scenario, RIDE_STEP_OFF))\n\t\treturn _ride_phase_jump;",
                            "ride-jump-late"),
    "brake-never": ("\t\tif (elapsed < RIDE_BRAKE)\n", "\t\tif (TRUE)\n", "ride-motion"),
    "rise-never-down": ("\t\telse if (elapsed >= 2*RIDE_RISE_TICKS && elapsed < 3*RIDE_RISE_TICKS)\n",
                        "\t\telse if (FALSE)\n", "ride-motion"),
    "brush-moves-early": ("\t\tif (elapsed >= RIDE_STEP_OFF + RIDE_BRUSH_STEP_TICKS)\n", "\t\tif (TRUE)\n", "ride-motion"),
    "cross-everyone": ("\t\tif (scenario == _ride_convoy)\n", "\t\tif (TRUE)\n", "ride-script"),
    "step-off-left": ("\t\t\taction->throttle.j = ride_step_off_side(scenario, deck_vehicle) * RIDE_SIDE_THROTTLE;\n", "\t\t\taction->throttle.j = RIDE_SIDE_THROTTLE;\n",
                      "ride-script"),
    "driver-straight": ("\t\taction->desired_facing.yaw = yaw + RIDE_DRIVE_TURN;\n", "\t\taction->desired_facing.yaw = yaw;\n",
                        "ride-script"),
    "height-from-base": ("*height = (base_z - radius) - (start_z + ray_k*t);", "*height = base_z - (start_z + ray_k*t);",
                         "ride-height"),
    "miss-is-zero": ("\tif (!hit || !(t >= 0.f && t <= 1.f))\n\t\treturn FALSE;\n",
                     "\tif (!hit || !(t >= 0.f && t <= 1.f))\n\t{\n\t\t*height = 0.f;\n\t\treturn TRUE;\n\t}\n", "ride-height"),
    "spot-search-skipped": ("\tfor (radius = RIDE_SEARCH_STEP; !done && radius <= reach; radius += RIDE_SEARCH_STEP)\n",
                            "\tfor (radius = RIDE_SEARCH_STEP; radius <= 0.f; radius += RIDE_SEARCH_STEP)\n", "ride-level-spot"),
    "spot-farthest-first": ("\tfor (radius = RIDE_SEARCH_STEP; !done && radius <= reach; radius += RIDE_SEARCH_STEP)\n",
                            "\tfor (radius = RIDE_SEARCH_RADIUS; radius >= RIDE_SEARCH_STEP; radius -= RIDE_SEARCH_STEP)\n",
                            "ride-level-spot"),
    "spot-unbounded": ("!done && radius <= reach; radius += RIDE_SEARCH_STEP)\n", "radius <= 1000.f; radius += RIDE_SEARCH_STEP)\n",
                       "ride-level-none"),
    "spot-ground-ignored": ("\t\t\tat.z = ground + rest;\n", "", "ride-level-spot"),
    "spot-rest-dropped": ("\t\t\tat.z = ground + rest;\n", "\t\t\tat.z = ground;\n", "ride-level-spot"),
    "spot-behind-skipped": ("\t\tif (behind > 0.f && !ride_level_along(point, angle + _pi, behind))\n", "\t\tif (FALSE)\n",
                            "ride-level-spot"),
    # (these two also drop the cross-slope check, which sees the lane patches too)
    "spot-lane-skipped": (["\t\tif (lane > 0.f && !(ride_level_beside(point, angle, length, RIDE_CONVOY_LANE_NEAR) &&\n"
                           "\t\t\tride_level_beside(point, angle, length, lane)))\n", CROSS, FIRST, FIRST_HEADING], ["\t\tif (FALSE)\n", "", "if (!found)", "if (!found)"], "ride-level-lane"),
    "spot-near-lane-skipped": (["ride_level_beside(point, angle, length, RIDE_CONVOY_LANE_NEAR) &&\n\t\t\t", CROSS, FIRST, FIRST_HEADING], ["", "", "if (!found)", "if (!found)"], "ride-level-lane"),
    "spot-own-unchecked": ("\tif (ride_level_heading(position, run, RIDE_LEVEL_BEHIND, lane, turn, yaw, &rise))\n",
                           "\tif (ride_level_heading(position, run, 0.f, 0.f, turn, yaw, &rise))\n", "ride-level-spot"),
    "level-turn-none": ("(turn != 0.f && (heading & 1) ? turn : 0.f)", "0.f", "ride-level-turn"),
    "level-turn-right": ("#define RIDE_CONVOY_TURN (10.f*_pi/180.f)", "#define RIDE_CONVOY_TURN (-10.f*_pi/180.f)", "ride-level-turn"),
    "level-turn-first": ("(turn != 0.f && (heading & 1) ? turn : 0.f)", "(turn != 0.f && !(heading & 1) ? turn : 0.f)", "ride-level-turn"),
    "level-cross-unchecked": (CROSS, "", "ride-level-cross"),
    "level-cross-too-loose": ("#define RIDE_CONVOY_CROSS_RISE 0.35f", "#define RIDE_CONVOY_CROSS_RISE 1.5f", "ride-level-cross"),
    "level-cross-too-strict": ("#define RIDE_CONVOY_CROSS_RISE 0.35f", "#define RIDE_CONVOY_CROSS_RISE 0.1f", "ride-level-cross"),
    "level-reach-fixed": ("radius <= reach; radius += RIDE_SEARCH_STEP)\n\t{\n\t\tshort points", "radius <= RIDE_SEARCH_RADIUS; radius += RIDE_SEARCH_STEP)\n\t{\n\t\tshort points", "ride-level-reach"),
    "level-ring-step-of-sixteen": ("real angle = (real)point*_pi/(real)(points/2);", "real angle = (real)point*_pi/8.f;", "ride-level-reach"),
    "level-ring-stays-sixteen": ("return 16*(short)ceil(radius/RIDE_SEARCH_RADIUS);", "return 16;", "ride-level-reach"),
    "convoy-height-of-the-first": ("return second_ground + (first_z - first_ground);", "return first_z;", "ride-convoy-height"),
    "convoy-height-ground-only": ("return second_ground + (first_z - first_ground);", "return second_ground;", "ride-convoy-height"),
    "convoy-hold-none": ("return speed > RIDE_CONVOY_HOLD_CAP ? RIDE_CONVOY_HOLD_CAP : speed < -RIDE_CONVOY_HOLD_CAP ? -RIDE_CONVOY_HOLD_CAP : speed;", "return 0.f;", "ride-convoy-hold"),
    "convoy-hold-wrong-sign": ("real speed = RIDE_CONVOY_HOLD_GAIN*error;", "real speed = -RIDE_CONVOY_HOLD_GAIN*error;", "ride-convoy-hold"),
    "convoy-hold-uncapped": ("return speed > RIDE_CONVOY_HOLD_CAP ? RIDE_CONVOY_HOLD_CAP : speed < -RIDE_CONVOY_HOLD_CAP ? -RIDE_CONVOY_HOLD_CAP : speed;", "return speed;", "ride-convoy-hold"),
    "convoy-turn-none": ("return rate > RIDE_CONVOY_HOLD_TURN_CAP ? RIDE_CONVOY_HOLD_TURN_CAP : rate < -RIDE_CONVOY_HOLD_TURN_CAP ? -RIDE_CONVOY_HOLD_TURN_CAP : rate;", "return 0.f*rate;", "ride-convoy-hold"),
    "convoy-turn-wrong-sign": ("rate = RIDE_CONVOY_HOLD_TURN_GAIN*error;", "rate = -RIDE_CONVOY_HOLD_TURN_GAIN*error;", "ride-convoy-hold"),
    "convoy-turn-uncapped": ("return rate > RIDE_CONVOY_HOLD_TURN_CAP ? RIDE_CONVOY_HOLD_TURN_CAP : rate < -RIDE_CONVOY_HOLD_TURN_CAP ? -RIDE_CONVOY_HOLD_TURN_CAP : rate;", "return rate;", "ride-convoy-hold"),
    "convoy-turn-unwrapped": ("while (error > _pi)", "while (FALSE)", "ride-convoy-hold"),
    "convoy-moves-before-board": ("if (elapsed < 0)\n\t\treturn;", "if (elapsed < 0 && scenario != _ride_convoy)\n\t\treturn;", "ride-motion"),
    "level-run-fixed": ("return drive + RIDE_LEVEL_MARGIN > RIDE_LEVEL_RUN ? drive + RIDE_LEVEL_MARGIN : RIDE_LEVEL_RUN;",
                        "return RIDE_LEVEL_RUN + 0.f*drive;", "ride-level-run"),
    "level-run-no-margin": ("return drive + RIDE_LEVEL_MARGIN > RIDE_LEVEL_RUN ? drive + RIDE_LEVEL_MARGIN : RIDE_LEVEL_RUN;",
                            "return drive > RIDE_LEVEL_RUN ? drive : RIDE_LEVEL_RUN;", "ride-level-run"),
    "level-spot-fixed-run": ("ride_level_heading(&at, run, RIDE_LEVEL_BEHIND, lane, turn, &candidate, &rise)",
                             "ride_level_heading(&at, RIDE_LEVEL_RUN, RIDE_LEVEL_BEHIND, lane, turn, &candidate, &rise)", "ride-level-run"),
    "spot-yaw-lost": ("\t\t\t\t\tspot_yaw = candidate;\n", "\t\t\t\t\tspot_yaw = 0.f;\n", "ride-level-spot"),
    "level-best-first-found": ("if (!found || rise < best - RIDE_CONVOY_CROSS_TIE)", "if (!found)", "ride-level-best"),
    "board-sides-restored": ("\t*y = scenario == _ride_convoy ? RIDE_BOARD_SIDE : scenario == _ride_brush ? -RIDE_BOARD_SIDE :\n\t\t(scenario == _ride_gun || scenario == _ride_coop || scenario == _ride_coop_double) && !ordinal ? RIDE_BOARD_SIDE : 0.f;\n",
                            "\t*y = scenario == _ride_brush || ordinal ? -.45f : .45f;\n", "ride-board"),
    "walk-leg-long": ("RIDE_WALK_PERIOD = 8,", "RIDE_WALK_PERIOD = 30,", "ride-script"),
    "reboard-too-early": ("RIDE_REBOARD_AFTER = 40,", "RIDE_REBOARD_AFTER = 0,", "ride-reboard"),
    "reboard-in-the-air": ("if (ride_phase(_ride_straight, elapsed) != _ride_phase_jump || airborne || deck)",
                           "if (ride_phase(_ride_straight, elapsed) != _ride_phase_jump || deck)", "ride-reboard"),
    "reboard-on-the-deck": ("if (ride_phase(_ride_straight, elapsed) != _ride_phase_jump || airborne || deck)",
                            "if (ride_phase(_ride_straight, elapsed) != _ride_phase_jump || airborne)", "ride-reboard"),
    "reboard-every-tick": ("return cycle != done_cycle;", "return cycle >= 0;", "ride-reboard"),
    "reboard-outside-the-jumps": ("if (ride_phase(_ride_straight, elapsed) != _ride_phase_jump || airborne || deck)",
                                  "if (airborne || deck)", "ride-reboard"),
    "board-gun-touches-mount": ("static real const under_gun[3] = { -.8f, -.805f, -.81f };",
                                "static real const under_gun[3] = { -.7f, -.75f, -.8f };", "ride-board"),
    "gun-walks-into-mount": ("action->throttle.i = -action->throttle.i;", "action->throttle.i = action->throttle.i;", "ride-script"),
    "coop-walks-into-mount": ("if (scenario == _ride_gun || scenario == _ride_coop || scenario == _ride_coop_double || scenario == _ride_convoy)",
                              "if (scenario == _ride_gun)", "ride-script"),
    "coop-boards-into-mount": ("(scenario == _ride_gun || scenario == _ride_coop || scenario == _ride_coop_double) ?",
                              "(scenario == _ride_gun) ?", "ride-board"),
    "coop-centre-pill-overlaps": ("(scenario == _ride_gun || scenario == _ride_coop || scenario == _ride_coop_double) && !ordinal ?",
                                  "(scenario == _ride_gun) && !ordinal ?", "ride-board"),
    "board-hood-leg-off": ("static real const hood[3] = { .6f, .65f, .7f };", "static real const hood[3] = { .6f, .5f, .7f };",
                           "ride-board"),
    "board-riders-overlap": ("hood[index]", "bed[index]", "ride-board"),
    "tilt-cap-doubled": ("#define RIDE_TILT_CAP (_pi/180.f)", "#define RIDE_TILT_CAP (_pi/90.f)", "ride-tilt-schedule"),
    "tilt-no-hold": ("if (elapsed < start + ramp + RIDE_TILT_HOLD || step == RIDE_TILT_STEPS - 1)",
                     "if (step == RIDE_TILT_STEPS - 1)", "ride-tilt-schedule"),
    "tilt-pair-past-limit": ("return .9f*(real)acos(limit);", "return 1.1f*(real)acos(limit);", "ride-tilt-schedule"),
    "tilt-pair-m-only": ("real limit = minimum_normal_k > .70710678f ? minimum_normal_k : .70710678f;",
                         "real limit = minimum_normal_k;", "ride-tilt-schedule"),
    "tilt-roll-sign": ("\tup->j = (real)sin(roll);\n", "\tup->j = -(real)sin(roll);\n", "ride-tilt-frame"),
    "tilt-no-yaw": ("\t\tforward->i = i*cosine - forward->j*sine;\n", "\t\tforward->i = i;\n", "ride-tilt-frame"),
    "tilt-door-missing": ("\t\taction->throttle.i = exit_forward;\n", "", "ride-tilt-action"),
    "tilt-jump-every-tick": ("ride_tilt_steps[step].jump && hold >= RIDE_TILT_JUMP_AT &&\n\t\t(step > last_jump || (pressed_now && step == last_jump))", "TRUE", "ride-tilt-action"),
    "tilt-door-while-descending": ("if (step == RIDE_TILT_STEPS - 1 && hold >= RIDE_TILT_DESCEND)",
                                   "if (step == RIDE_TILT_STEPS - 1)", "ride-tilt-action"),
    "tilt-no-descent": ("\tend += RIDE_TILT_DESCEND;\n", "", "ride-tilt-schedule"),
    "longsword-pilot-drives": ("\t\tif (scenario == _ride_longsword)\n\t\t\treturn;\n", "", "ride-script"),
    "spot-nan-accepted": ("\t\tif (!(fabs(values[index]) < 100.f))\n", "\t\tif (FALSE)\n", "ride-tilt-spot"),
    "spot-extra-accepted": ("&values[3], &extra) != 4)", "&values[3], &extra) < 4)", "ride-tilt-spot"),
    "longsword-any-vehicle": ("return strstr(name, \"fighterbomber\") != NULL;", "return TRUE;", "ride-tilt-spot"),
    "longsword-old-tag-name": ("return strstr(name, \"fighterbomber\") != NULL;", "return strstr(name, \"longsword\") != NULL;", "ride-tilt-spot"),
    "longsword-phase-jumps": ("\t\treturn elapsed < RIDE_TILT_SETTLE ? _ride_phase_stand : _ride_phase_observe;\n",
                              "\t\treturn _ride_phase_jump;\n", "ride-phases"),
}


def generated(control=None, flanks=False):
    source = read(SOURCE)
    if control:
        before, after, _ = NEGATIVE_CONTROLS[control]
        for one, other in zip(before, after) if isinstance(before, list) else [(before, after)]:
            source = mutated(source, one, other)
    types = (f"#define HALO_PORT_MAXIMUM_NETWORK_PLAYERS "
             f"{constant(read('port/linux/include/halo_port_limits.h'), 'HALO_PORT_MAXIMUM_NETWORK_PLAYERS')}\n")
    types += enum_with(read("source/units/units.h"), "_unit_control_jump_bit") + "\n"
    types += structure(read("source/game/players.h"), "player_action") + "\n"
    types += "".join(line + "\n" for line in re.findall(r"^#define RIDE_\w+ .*$", source, re.M))
    for member in ("RIDE_BOARD_TICK", "_ride_straight", "_ride_phase_prepare"):
        types += enum_with(source, member) + "\n"
    types += structure(source, "ride_player") + "\n"
    types += structure(source, "ride_tilt_step") + "\n"
    types += re.search(r"^static struct ride_tilt_step const ride_tilt_steps\[\] =.*?^\};", source, re.M | re.S).group(0) + "\n"
    code = "".join(function(source, name) + "\n" for name in FUNCTIONS if not (flanks and name in LIVE))
    if not flanks:
        code += function(source, "ride_board").replace("static boolean ride_board(", "static boolean ride_board_actual(")
    return (("ride_types.inc", types), ("ride_under_test.inc", code))


@pytest.mark.parametrize("case", CASES)
def test_case(case):
    fixture = "network_ride_flanks" if case == "ride-coop-flanks" else "network_ride"
    status, output = run(build(fixture, generated(flanks=fixture == "network_ride_flanks")), case)
    assert status == 0, output


@pytest.mark.parametrize("control", NEGATIVE_CONTROLS)
def test_negative_control(control):
    case = NEGATIVE_CONTROLS[control][-1]
    fixture = "network_ride_flanks" if case == "ride-coop-flanks" else "network_ride"
    status, output = run(build(fixture, generated(control, flanks=fixture == "network_ride_flanks")), case)
    assert status == CHECK_FAILED, f"a fault ({control}) passed '{NEGATIVE_CONTROLS[control][-1]}': the test cannot see it"
    assert "check failed:" in output, f"a fault ({control}) failed without a harness assertion: {output}"


if __name__ == "__main__":
    sys.exit(pytest.main(["-q", __file__]))

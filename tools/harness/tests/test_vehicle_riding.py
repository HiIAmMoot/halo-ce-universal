"""vehicle_riding.c compiled whole against fake settings, connection and look impulses: a player standing on a
moving vehicle (docs: port/linux/README.md, game.vehicle_riding).

    python -m pytest -q tools/harness/tests/test_vehicle_riding.py
"""

import re
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from harness import CHECK_FAILED, build, constant, enum_with, mutated, read, run  # noqa: E402

CASES = ["carry", "dyaw-vertical", "node-carry", "stuck", "contact-point", "guards", "gate", "deferred",
         "look-carry", "two-local-players", "latch"]

NEGATIVE_CONTROLS = {
    "last-from-vehicle-now": (("vehicle_riding_point_to_world(&state->vehicle_position, &state->vehicle_forward, "
                               "&state->vehicle_up, &state->local_point, &last);",
                               "vehicle_riding_point_to_world(&vehicle->position, &vehicle->forward, "
                               "&vehicle->up, &state->local_point, &last);"), "carry"),
    "whole-angle": (("return angle*axis.k/sine2;", "return angle;"), "dyaw-vertical"),
    "carry-about-body-origin": (("vehicle_riding_point_to_world(&vehicle->position, &vehicle->forward, &vehicle->up, "
                                 "&state->local_point, &carried);",
                                 "vehicle_riding_point_to_world(&vehicle->origin, &vehicle->forward, &vehicle->up, "
                                 "&state->local_point, &carried);"), "node-carry"),
    "carry-from-velocity": (("riding_between(&last, &carried, &step->displacement);",
                             "step->displacement = vehicle->translational_velocity;"), "stuck"),
    "carry-from-base": (("riding_between(&last, &carried, &step->displacement);",
                         "riding_between(base, &moved_base, &step->displacement);"), "contact-point"),
    "moved-alone-reversed": (("riding_distance(base, &last) > riding_moved_alone",
                              "riding_distance(base, &last) < riding_moved_alone"), "guards"),
    "step-not-per-update": (("distance > riding_teleport_step*step->updates",
                             "distance > riding_teleport_step"), "guards"),
    "unupdated-move-kept": (("(step->updates == 0 && distance > 0.000001f) ||", ""), "guards"),
    "teleport-step-reversed": (("distance > riding_teleport_step*step->updates",
                                "distance < riding_teleport_step*step->updates"), "guards"),
    "drop-forgets-falling": (("\tif (falling)\n", "\tif (FALSE)\n"), "guards"),
    "connection-at-latch": (("riding_setting && game_connection() == _game_connection_local && !game_in_editor()",
                             "riding_setting && !game_in_editor()"), "gate"),
    "dead-ride": (("return has_player && !dead && vehicle_riding_enabled();",
                   "return has_player && vehicle_riding_enabled();"), "gate"),
    "defer-riders-only": (("return applies && state->ticks > 0;",
                           "return applies && vehicle_riding_is_riding(state);"), "deferred"),
    "kept-not-reset": (("riding_kept_dyaws[local_player_index] = dyaw;",
                        "if (dyaw != 0.f) riding_kept_dyaws[local_player_index] = dyaw;"), "look-carry"),
    "single-kept": (("riding_kept_dyaws[local_player_index] = dyaw;", "riding_kept_dyaws[0] = dyaw;"),
                    "two-local-players"),
    "impulse-to-first": (("player_control_permanent_impulse(local_player_index, &impulse);",
                          "player_control_permanent_impulse(0, &impulse);"), "two-local-players"),
    "latch-keeps-turn": (("\t\triding_kept_dyaws[local_player_index] = 0.f;\n", "\n"), "latch"),
}


def without_includes(text):
    return re.sub(r"^#include .*$", "", text, flags=re.M)


def generated(fault=None):
    game = read("source/game/game.h")
    enums = enum_with(game, "_game_connection_local") + "\n"
    enums += f"#define MAXIMUM_LOCAL_PLAYERS {constant(read('source/game/players.h'), 'MAXIMUM_LOCAL_PLAYERS')}\n"
    code = read("port/linux/game/vehicle_riding.c")
    if fault:
        code = mutated(code, *fault)
    text = without_includes(read("port/linux/game/vehicle_riding.h")) + "\n" + without_includes(code)
    return (("enums.inc", enums), ("under_test.inc", text))


@pytest.mark.parametrize("case", CASES)
def test_case(case):
    status, output = run(build("vehicle_riding", generated()), case)
    assert status == 0, output


@pytest.mark.parametrize("control", NEGATIVE_CONTROLS)
def test_negative_control(control):
    fault, case = NEGATIVE_CONTROLS[control]
    status, output = run(build("vehicle_riding", generated(fault)), case)
    assert status == CHECK_FAILED, f"vehicle_riding.c with a fault ({control}) passed '{case}': the test cannot see it"


if __name__ == "__main__":
    sys.exit(pytest.main(["-q", __file__]))

"""vehicle_riding.c compiled whole against fake settings, connection and look impulses: a player standing on a
moving vehicle (docs: port/linux/README.md, game.vehicle_riding).

    python -m pytest -q tools/harness/tests/test_vehicle_riding.py
"""

import re
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from harness import CHECK_FAILED, build, constant, enum_with, function, inline, mutated, read, run, structure  # noqa: E402

CASES = ["carry", "dyaw-vertical", "node-carry", "stuck", "contact-point", "guards", "gate", "deferred",
         "look-carry", "two-local-players", "latch", "momentum", "no-double-count", "spans", "double-speed",
         "landing-frame", "seat-exit-onto-deck", "refresh", "elevator", "local-frame", "node-change", "falling",
         "jump-deck-frame", "collision-decision", "collision-others", "claim", "damage-speed-default", "suspended"]

# a fault in vehicle_riding.c, and the case that must catch it
NEGATIVE_CONTROLS = {
    "falling-stricter-off-rising": (("return relative > world_velocity_k ? relative : world_velocity_k;",
                                     "return relative;"), "falling"),
    "tracked-not-used": (("\telse if (support_object_index == tracked_object_index)\n\t\t*support_velocity = *tracked_deck_velocity;\n",
                          ""), "landing-frame"),
    "suspended-adds": (("\t\t\t*new_velocity = step->velocity_now;\n", "\t\t\tadd_vectors3d(new_velocity, &step->velocity_now, new_velocity);\n"),
                       "suspended"),
    "last-from-vehicle-now": (("vehicle_riding_point_to_world(&state->vehicle_position, &state->vehicle_forward, "
                               "&state->vehicle_up, &state->local_point, &last);",
                               "vehicle_riding_point_to_world(&vehicle->position, &vehicle->forward, "
                               "&vehicle->up, &state->local_point, &last);"), "carry"),
    "whole-angle": (("return rotation.k;", "return magnitude3d(&rotation);"), "dyaw-vertical"),
    "carry-about-body-origin": (("vehicle_riding_point_to_world(&vehicle->position, &vehicle->forward, &vehicle->up, "
                                 "&state->local_point, &carried);",
                                 "vehicle_riding_point_to_world(&vehicle->origin, &vehicle->forward, &vehicle->up, "
                                 "&state->local_point, &carried);"), "node-carry"),
    "carry-from-velocity": (("vector_from_points3d(&last, &carried, &step->displacement);",
                             "step->displacement = vehicle->translational_velocity;"), "stuck"),
    "carry-from-base": (("vector_from_points3d(&last, &carried, &step->displacement);",
                         "vector_from_points3d(base, &moved_base, &step->displacement);"), "contact-point"),
    "moved-alone-reversed": (("distance3d(base, &last) > riding_moved_alone",
                              "distance3d(base, &last) < riding_moved_alone"), "guards"),
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
    "no-momentum": (("\t\t\tadd_vectors3d(new_velocity, &step->velocity_now, new_velocity);\n", "\t\t\t;\n"), "momentum"),
    "seed-no-subtraction": (("subtract_vectors3d(world_velocity, frame_velocity, velocity);",
                             "*velocity = *world_velocity;"), "no-double-count"),
    "no-span-divisor": (("per_tick = 1.f/(step->updates*riding_period(vehicle, double_speed));",
                         "per_tick = 1.f/riding_period(vehicle, double_speed);"), "spans"),
    "no-period": (("per_tick = 1.f/(step->updates*riding_period(vehicle, double_speed));",
                   "per_tick = 1.f/step->updates;"), "double-speed"),
    "skipped-tick-not-held": (("\t\tstep->tracked_deck_velocity = state->velocity;\n",
                               "\t\tstep->tracked_deck_velocity = step->displacement;\n"), "double-speed"),
    "deck-point-per-update": (("scale_vector3d(velocity, 1.f/riding_period(vehicle, double_speed), velocity);", ""),
                              "double-speed"),
    "frame-not-added": (("add_vectors3d(velocity, frame_velocity, &relative);", "relative = *velocity;"),
                        "landing-frame"),
    "support-ridden-always": (("else if (support_object_index == riding_object_index)",
                               "else if (support_object_index != NONE)"), "landing-frame"),
    "unclipped": (("riding_clip(new_velocity, support_velocity, support_normal);", ";"), "landing-frame"),
    "refresh-only-on-landing": (("\t\tstate->velocity = step->velocity_now;\n\t\tif (supported",
                                 "\t\tif (supported"), "refresh"),
    "no-elevator-clear": (("\t\t\t&state->local_point);\n\t\treturn TRUE;",
                           "\t\t\t&state->local_point);\n\t\treturn FALSE;"), "elevator"),
    "node-not-taken": (("\t\tstate->node_index = support->node_index;\n", "\n"), "node-change"),
    "left-sign-in-world": (("\tmatrix4x3_transform_point(&frame, local, point);",
                            "\tnegate_vector3d(&frame.left, &frame.left);\n\tmatrix4x3_transform_point(&frame, local, point);"),
                           "local-frame"),
    "window-rederives-place": (("\t\triding_snapshot(state, &step->vehicle);\n",
                                "\t\triding_snapshot(state, &step->vehicle);\n"
                                "\t\tvehicle_riding_point_to_local(&step->vehicle.position, &step->vehicle.forward, "
                                "&step->vehicle.up, new_base, &state->local_point);\n"), "local-frame"),
    "falling-only-while-riding": (("if (vehicle_riding_is_riding(state) || state->falling)",
                                   "if (vehicle_riding_is_riding(state))"), "falling"),
    "falling-whole-window": (("if (vehicle_riding_is_riding(state) || state->falling)",
                              "if (state->ticks > 0 || state->falling)"), "falling"),
    "jump-world": (("\t\t*jump_frame = state->velocity;\n", "\n"), "jump-deck-frame"),
    "centre-velocity": (("cross_product3d(&vehicle->angular_velocity, &offset, &turning);",
                         "turning.i = turning.j = turning.k = 0.f;"), "collision-decision"),
    "window-for-every-vehicle": (("\tif (state->object_index != vehicle_index)\n", "\tif (FALSE)\n"), "collision-others"),
    "window-where-not-applied": (("\tif (!applies || state->ticks <= 0)\n", "\tif (state->ticks <= 0)\n"),
                                 "collision-others"),
    "second-vehicle-as-pedestrian": (("return vehicle_riding_is_riding(state) ? _vehicle_riding_collision_fix : _vehicle_riding_collision_original;",
                                      "return _vehicle_riding_collision_original;"), "collision-others"),
    "claim-ignored": (("\tif (riding_object_index == NONE || best < 0 || objects[best] == riding_object_index)\n\t\treturn best;\n",
                       "\treturn best;\n"), "claim"),
    "claim-takes-walls": (("objects[index] == riding_object_index && normal_ks[index] >= minimum_normal_k",
                           "objects[index] == riding_object_index"), "claim"),
    "claim-strict-limit": (("normal_ks[index] >= minimum_normal_k", "normal_ks[index] > minimum_normal_k"), "claim"),
    "no-factor-two": (("speed = 2.f*(real)sqrt(", "speed = (real)sqrt("), "damage-speed-default"),
    "damage-on-failed-fix": (("return decision == _vehicle_riding_collision_fix_and_damage;",
                              "return original_damage || decision == _vehicle_riding_collision_fix_and_damage;"),
                             "collision-decision"),
    "per-second": (("/TICKS_PER_SECOND;", ";"), "damage-speed-default"),
}


def without_includes(text):
    return re.sub(r"^#include .*$", "", text, flags=re.M)


NL = chr(10)
REAL_MATH_INLINES = ["square_root", "arctangent", "set_real_point3d", "vector_from_points3d", "scale_vector3d",
                     "magnitude_squared3d", "magnitude3d", "normalize3d", "distance_squared3d", "distance3d",
                     "dot_product3d", "cross_product3d", "add_vectors3d", "subtract_vectors3d", "negate_vector3d"]
MATRIX_FUNCTIONS = ["matrix4x3_inverse", "matrix4x3_multiply", "matrix4x3_rotation_to_quaternion",
                    "matrix4x3_rotation_from_vectors", "matrix4x3_from_point_and_vectors", "matrix4x3_transform_point",
                    "matrix4x3_inverse_transform_point", "vector_from_matrices4x3"]


def engine():
    """The engine's own vector and frame maths, extracted: the code under test calls it and does not copy it."""
    real_math = read("source/math/real_math.h")
    matrix_math = read("source/math/matrix_math.c")
    text = "".join(re.search(r"^#define " + name + r".*$", real_math, re.M).group(0) + NL
                   for name in ("_real_epsilon", "_pi"))
    for name in ("real_quaternion", "real_matrix4x3"):
        text += structure(real_math, name) + NL + f"typedef struct {name} {name};" + NL
    text += "".join(inline(real_math, name) + NL for name in REAL_MATH_INLINES)
    text += function(read("source/math/real_math.c"), "quaternion_to_angle_and_vector") + NL
    # (the 4x3 next-index table: one blob in the original, matrix_math.c)
    text += re.search(r"^static short data_0030790c.*$", matrix_math, re.M).group(0) + NL
    text += "#define matrix4x3_next (data_0030790c)" + NL
    text += "".join(function(matrix_math, name) + NL for name in MATRIX_FUNCTIONS)
    return text


def generated(fault=None):
    game = read("source/game/game.h")
    enums = enum_with(game, "_game_connection_local") + "\n"
    enums += f"#define MAXIMUM_LOCAL_PLAYERS {constant(read('source/game/players.h'), 'MAXIMUM_LOCAL_PLAYERS')}\n"
    code = read("port/linux/game/vehicle_riding.c")
    if fault:
        code = mutated(code, *fault)
    text = without_includes(read("port/linux/game/vehicle_riding.h")) + "\n" + without_includes(code)
    return (("enums.inc", enums), ("engine.inc", engine()), ("under_test.inc", text))


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

"""The test teleport must fit the whole pill, not just its vertical deck ray."""
from pathlib import Path
import sys
import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from harness import CHECK_FAILED, build, enum_with, function, mutated, read, run, structure

SOURCE = "port/linux/game/network_test.c"
CASES = ["board-rejects-overlap", "board-no-clear-spot", "board-full-radius", "board-deck-velocity", "board-recovery-search"]
CASES += ["board-circle-clearance"]
CASES += ["board-all-scenario-fallback", "board-all-scenario-exhaustion"]
NEGATIVE_CONTROLS = {
    "driven-recovery-keeps-fall": ("vehicle_riding_deck_point_velocity(&vehicle, &base, game_players_are_double_speed(),\n\t\t\t\t&unit->object.translational_velocity);", "(void)0;", "board-deck-velocity"),
    "driven-recovery-no-search": ("index < 84", "index < 3", "board-recovery-search"),
    "all-scenario-no-fallback": ("index < 84", "index < 3", "board-all-scenario-fallback"),
    "board-overlap-ignored": ("ray && ride_board_clear(vehicle_index, &surface, height, width)",
                               "ray", "board-rejects-overlap"),
    "board-clearance-driven-only": ("ray && ride_board_clear(vehicle_index, &surface, height, width)",
                                    "ray && (ride.scenario != _ride_driven || ride_board_clear(vehicle_index, &surface, height, width))",
                                    "board-circle-clearance"),
    "board-shrunken-radius": ("height*.5f + width, height, width, &features);",
                              "height*.5f + width, height, width - .015625f, &features);", "board-full-radius"),
}


def generated(control=None):
    source = read(SOURCE)
    if control:
        before, after, _ = NEGATIVE_CONTROLS[control]
        source = mutated(source, before, after)
    types = enum_with(source, "_ride_straight")
    header = read("source/physics/collision_features.h")
    types += enum_with(header, "MAXIMUM_COLLISION_FEATURES_PER_TEST")
    types += enum_with(header, "_collision_feature_sphere")
    for name in ("collision_sphere", "collision_cylinder", "collision_prism", "collision_feature_list", "collision_plane"):
        types += structure(header, name)
    geometry = read("source/physics/collision_features.c")
    code = function(geometry, "collision_cylinder_test_point")
    code += function(geometry, "collision_features_test_point")
    # Before the fix there is no clearance helper; RED still compiles the real caller.
    if "static boolean ride_board_clear(" in source:
        code += function(source, "ride_board_clear")
    riding = read("port/linux/game/vehicle_riding.c")
    code += function(riding, "riding_period") + function(riding, "vehicle_riding_deck_point_velocity")
    code += function(source, "ride_board_candidate") + function(source, "ride_board")
    return (("ride_clearance_types.inc", types), ("ride_clearance_under_test.inc", code))


@pytest.mark.parametrize("case", CASES)
def test_case(case):
    status, output = run(build("network_ride_clearance", generated()), case)
    assert status == 0, output


@pytest.mark.parametrize("control", NEGATIVE_CONTROLS)
def test_negative_control(control):
    status, output = run(build("network_ride_clearance", generated(control)), NEGATIVE_CONTROLS[control][-1])
    assert status == CHECK_FAILED, output

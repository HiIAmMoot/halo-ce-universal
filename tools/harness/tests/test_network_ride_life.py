"""The real sample caller must record death/change attribution before it ends."""
from pathlib import Path
import sys
import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from harness import CHECK_FAILED, build, function, mutated, read, run, structure

SOURCE = "port/linux/game/network_test.c"
CASES = ["life-dead", "life-changed", "life-gone"]
NEGATIVE_CONTROLS = {
    "life-caller-silent": ("ride_life_log(absolute, player->unit_index, tick);", "", "life-changed"),
    "life-death-silent": ("!changed && (!dead || rider->life_logged)", "!changed", "life-dead"),
    "life-farthest-vehicle": ("candidate < distance", "candidate > distance", "life-dead"),
    "life-no-speed": ("speed = magnitude3d(&vehicle->object.translational_velocity);", "speed = 0.f;", "life-dead"),
}


def generated(control=None):
    source = read(SOURCE)
    if control:
        source = mutated(source, *NEGATIVE_CONTROLS[control][:2])
    code = function(source, "ride_life_log") if "static void ride_life_log(" in source else ""
    caller = function(source, "ride_samples")
    # The real player loop, through its error/sample boundary; vehicle telemetry is unrelated.
    code += caller[:caller.index("\tfor (index = 0; index < ride_vehicle_count(); index++)")] + "}\n"
    return (("ride_life_types.inc", structure(source, "ride_player")), ("ride_life_under_test.inc", code))


@pytest.mark.parametrize("case", CASES)
def test_case(case):
    status, output = run(build("network_ride_life", generated()), case)
    assert status == 0, output


@pytest.mark.parametrize("control", NEGATIVE_CONTROLS)
def test_negative_control(control):
    status, output = run(build("network_ride_life", generated(control)), NEGATIVE_CONTROLS[control][-1])
    assert status == CHECK_FAILED and "check failed:" in output, output

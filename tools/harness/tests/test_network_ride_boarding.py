"""The actual pre-object branch must place the rider before seating the pilot."""
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from harness import CHECK_FAILED, build, function, mutated, read, run


def generated(fault=None):
    code = function(read("port/linux/game/network_test.c"), "network_test_ride_before_objects")
    # Motion after this boundary is unrelated; the preparation calls above it are unmodified.
    code = code[:code.index("\t/* (the convoy's vehicles are held")] + "}\n"
    if fault:
        code = mutated(code, *fault)
    return (("under_test.inc", code),)


def test_rider_before_pilot_on_live_preparation_path():
    status, output = run(build("network_ride_boarding", generated()), "order")
    assert status == 0, output


def test_stock_preparation_order_unchanged():
    status, output = run(build("network_ride_boarding", generated()), "stock")
    assert status == 0, output


NEGATIVE_CONTROLS = {
    "pilot-seated-during-selection": ("(ride.scenario != _ride_longsword && !ride_assign_driver(TRUE))", "!ride_assign_driver(TRUE)"),
    "pilot-before-rider": ("ride.prepared = ride_prepare();", "ride.prepared = ride_assign_driver(TRUE) && ride_prepare();"),
    "pilot-never-seated": ("ride.prepared = ride_assign_driver(TRUE);", "ride.prepared = TRUE;"),
}


@pytest.mark.parametrize("control", NEGATIVE_CONTROLS)
def test_boarding_order_control_dies_on_assertion(control):
    status, output = run(build("network_ride_boarding", generated(NEGATIVE_CONTROLS[control])), "order")
    assert status == CHECK_FAILED, output

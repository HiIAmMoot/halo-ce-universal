"""bipeds.c's biped_riding_vehicle_writing and biped_riding_vehicle_written (a vehicle's state correction carries the
biped it newly touches), taken from the sources and compiled with vehicle_riding.c against fakes of the pill, the sweep and
the touch test.

    python -m pytest -q tools/harness/tests/test_write_carry.py
"""

import re
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
sys.path.insert(0, str(Path(__file__).resolve().parent))
from harness import CHECK_FAILED, build, function, mutated, read, run, structure  # noqa: E402
from test_vehicle_riding import generated as riding_generated  # noqa: E402

BIPEDS = "source/units/bipeds.c"
CASES = ["write-carry-reboarded", "write-carry-window", "write-carry-riding-skipped", "write-carry-preexisting-touch",
         "write-carry-preexisting-rolled-touch", "write-carry-destination-touching", "write-carry-blocked",
         "write-carry-teleport-write", "write-carry-other-window", "write-carry-unrecorded",
         "write-carry-seated-dead-off"]
BIPED_FUNCTIONS = ["biped_riding_vehicle_writing", "biped_riding_vehicle_written"]

# a fault in vehicle_riding.c ("riding") or bipeds.c ("bipeds"), and the case that must catch it
NEGATIVE_CONTROLS = {
    "write-carry-never": (("bipeds", "\tstruct object_iterator iterator;\n\tstruct object_datum const *vehicle = object_get(vehicle_index);\n\tstruct biped_datum *biped;\n\n\tobject_iterator_new(&iterator, _object_mask_biped, 0);\n\twhile ((biped = (struct biped_datum *)object_iterator_next(&iterator)) != NULL)\n\t{\n\t\tstruct biped_riding_write_record *record;",
                           "\tstruct object_iterator iterator;\n\tstruct object_datum const *vehicle = object_get(vehicle_index);\n\tstruct biped_datum *biped;\n\n\tif (TRUE)\n\t\treturn;\n\tobject_iterator_new(&iterator, _object_mask_biped, 0);\n\twhile ((biped = (struct biped_datum *)object_iterator_next(&iterator)) != NULL)\n\t{\n\t\tstruct biped_riding_write_record *record;"),
                          "write-carry-reboarded"),
    "write-carry-riding-too": (("riding", "state->ticks > 0 && state->ticks < RIDING_TICKS &&", "state->ticks > 0 && state->ticks <= RIDING_TICKS &&"),
                               "write-carry-riding-skipped"),
    "write-carry-preexisting": (("riding", " && !touched_before", ""), "write-carry-preexisting-touch"),
    "write-carry-cause-from-destination": (("bipeds", "record->touched_before, touching,",
                                            "biped_riding_touches(vehicle_index, &carried, global_zero_vector3d, height, width), touching,"),
                                           "write-carry-preexisting-rolled-touch"),
    "write-carry-keeps-touching-reach": (("bipeds", "\t\t\tif (!biped_riding_touches(vehicle_index, &reached, global_zero_vector3d, height, width))\n",
                                          "\t\t\tif (TRUE)\n"), "write-carry-blocked"),
    "write-carry-keeps-touching-reach-destination": (("bipeds", "\t\t\tif (!biped_riding_touches(vehicle_index, &reached, global_zero_vector3d, height, width))\n",
                                                      "\t\t\tif (TRUE)\n"), "write-carry-destination-touching"),
    "write-carry-unbounded": (("riding", " && distance <= riding_moved_alone", ""), "write-carry-teleport-write"),
    "write-carry-no-rebase": (("bipeds", "\t\t\t\tif (riding.ticks > 0)\n\t\t\t\t\tbiped_riding_rebase(iterator.index);\n", ""),
                              "write-carry-window"),
    "write-carry-sets-velocity": (("bipeds", "\t\t\t\tobject_set_position(iterator.index, &moved, NULL, NULL);\n",
                                   "\t\t\t\tobject_set_position(iterator.index, &moved, NULL, NULL);\n\t\t\t\tbiped->object.translational_velocity = vehicle->object.translational_velocity;\n"),
                                  "write-carry-reboarded"),
    "write-carry-through": (("bipeds", "\t\t\tbiped_riding_carry(iterator.index, biped, definition, vehicle_index, &way, height, width, &reached);\n",
                             "\t\t\treached = carried;\n\t\t\t(void)definition;\n"), "write-carry-blocked"),
    "write-carry-stale-record": (("bipeds", " || record->vehicle_index != vehicle_index", ""), "write-carry-unrecorded"),
}


def generated(control=None):
    which, before, after = NEGATIVE_CONTROLS[control][0] if control else (None, None, None)
    riding = dict(riding_generated((before, after) if which == "riding" else None))
    bipeds = read(BIPEDS)
    record = structure(bipeds, "biped_riding_write_record")
    table = re.search(r"^static struct biped_riding_write_record biped_riding_writes\[.*\];$", bipeds, re.M).group(0)
    code = record + "\n\n" + table + "\n\n" + "".join(function(bipeds, name) + "\n" for name in BIPED_FUNCTIONS)
    if which == "bipeds":
        code = mutated(code, before, after)
    return (("enums.inc", riding["enums.inc"]), ("engine.inc", riding["engine.inc"]),
            ("under_test.inc", riding["under_test.inc"]), ("bipeds_write_carry.inc", code))


@pytest.mark.parametrize("case", CASES)
def test_case(case):
    status, output = run(build("write_carry", generated()), case)
    assert status == 0, output


@pytest.mark.parametrize("control", NEGATIVE_CONTROLS)
def test_negative_control(control):
    status, output = run(build("write_carry", generated(control)), NEGATIVE_CONTROLS[control][1])
    assert status == CHECK_FAILED and "check failed:" in output, \
        f"a fault ({control}) passed '{NEGATIVE_CONTROLS[control][1]}': the test cannot see it ({output})"


if __name__ == "__main__":
    sys.exit(pytest.main(["-q", __file__]))

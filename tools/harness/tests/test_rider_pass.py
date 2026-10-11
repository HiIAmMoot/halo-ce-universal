"""objects.c's update loops over a fake object list: riders update after their vehicle, and the update counts.

    python -m pytest -q tools/harness/tests/test_rider_pass.py
"""

import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from harness import CHECK_FAILED, build, constant, enum_with, function, mutated, read, run  # noqa: E402

CASES = ["after-vehicle", "dies-between-loops", "deactivated-between-loops", "parented-between-loops", "double-speed",
         "counts"]

NEGATIVE_CONTROLS = {
    "defers-nothing": (("if (deferred(object_index))", "if (FALSE)"), "after-vehicle"),
    "retests-riding": (("if (rider_header &&", "if (rider_header && deferred(object_index) &&"),
                       "dies-between-loops"),
    "no-slot-retest": (("\t\t\tTEST_FLAG(rider_header->flags, _object_header_active_bit) &&\n", ""),
                       "deactivated-between-loops"),
    "no-double-speed-gate": (("(!dont_update_object ||\n\t\t\t\t(TEST_FLAG(_object_mask_unit, rider_header->type)",
                              "(TRUE ||\n\t\t\t\t(TEST_FLAG(_object_mask_unit, rider_header->type)"), "double-speed"),
    "no-parent-retest": (("\t\t\tobject_get(object_index)->object.parent_object_index == NONE &&\n", ""),
                         "parented-between-loops"),
    "unbounded-count": (("&& absolute_index < MAXIMUM_OBJECTS_PER_MAP)\n\t\t++", ")\n\t\t++"), "counts"),
}


def generated(fault=None):
    objects_h = read("source/objects/objects.h")
    types = read("source/objects/object_types.h")
    # (the object types and their masks are one enum)
    enums = "\n".join([enum_with(objects_h, "_object_header_active_bit"), enum_with(types, "_object_type_biped")])
    enums += f"\n#define MAXIMUM_OBJECTS_PER_MAP {constant(objects_h, 'MAXIMUM_OBJECTS_PER_MAP')}\n"
    objects = read("source/objects/objects.c")
    text = "".join(function(objects, name) + "\n"
                   for name in ("object_count_update", "object_update_count", "objects_update_passes"))
    if fault:
        text = mutated(text, *fault)
    return (("enums.inc", enums), ("under_test.inc", text))


@pytest.mark.parametrize("case", CASES)
def test_case(case):
    status, output = run(build("rider_pass", generated()), case)
    assert status == 0, output


@pytest.mark.parametrize("control", NEGATIVE_CONTROLS)
def test_negative_control(control):
    fault, case = NEGATIVE_CONTROLS[control]
    status, output = run(build("rider_pass", generated(fault)), case)
    assert status == CHECK_FAILED, f"objects.c with a fault ({control}) passed '{case}': the test cannot see it"
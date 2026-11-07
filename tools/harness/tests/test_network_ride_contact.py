"""Physical contact, not a centre ray or a received riding claim."""
import sys
from pathlib import Path
import pytest
sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from harness import read, function, mutated, build, run, CHECK_FAILED

CASES = ['edge', 'unsupported', 'ticks-only', 'other-vehicle', 'slope']
CONTROLS = {
    'no-physical-gate': ('!physical || ', '', 'ticks-only'),
    'wrong-vehicle': ('support != vehicle || ', '', 'other-vehicle'),
    'height-from-base-only': ('contact->z - (normal->i*', '0.f*contact->z - (normal->i*', 'edge'),
}

def generated(control=None):
    source = read('port/linux/game/network_test.c')
    if 'static boolean ride_contact_height(' in source:
        code = function(source, 'ride_contact_height')
    else:
        code = function(source, 'ride_height_from_hit') + function(source, 'ride_height')
        code += '''static boolean ride_contact_height(boolean physical, long support, long vehicle,
 real_point3d const *contact, real_vector3d const *normal, real_point3d const *base, real radius, real *height)
 { return ride_height(vehicle, base, .3f, radius, height); }'''
    if control:
        before, after, _ = CONTROLS[control]
        code = mutated(code, before, after)
    return (('under_test.inc', code),)

@pytest.mark.parametrize('case', CASES)
def test_case(case):
    status, output = run(build('network_ride_contact', generated()), case)
    assert status == 0, output

@pytest.mark.parametrize('control', CONTROLS)
def test_control(control):
    status, output = run(build('network_ride_contact', generated(control)), CONTROLS[control][2])
    assert status == CHECK_FAILED, output

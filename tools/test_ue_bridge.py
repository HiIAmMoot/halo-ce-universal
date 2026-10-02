"""pytest for the UE bridge (port/ue_bridge): the C unit tests, the same
under AddressSanitizer, and (from Task 5) the cross-process cases.

    python -m pytest tools/test_ue_bridge.py -v
"""

import os
import subprocess

import pytest

from tools import ue_bridge_tests


@pytest.fixture(scope="session")
def test_exe():
    return ue_bridge_tests.build()


def test_unit_tests_pass(test_exe):
    result = subprocess.run([str(test_exe)], capture_output=True, text=True, timeout=120)
    assert result.returncode == 0, result.stdout + result.stderr


def test_unit_tests_pass_under_asan():
    if not os.environ.get("HALO_UE_BRIDGE_ASAN_CLANG"):
        pytest.skip("HALO_UE_BRIDGE_ASAN_CLANG is not set")
    exe = ue_bridge_tests.build(asan=True)
    result = subprocess.run([str(exe)], capture_output=True, text=True, timeout=300)
    assert result.returncode == 0, result.stdout + result.stderr

"""Real-map probes of the UE bridge's export: the game, started on a map with
the bridge on and no renderer, and the bridge read back by the read-world role.

    python -m pytest tools/test_ue_bridge_maps.py -v

Needs build/windows/halo.exe and the owner's maps in build/windows/maps
(never committed); every test skips without them. Close any running halo.exe
and HaloCEUE first.
"""

from __future__ import annotations

import json
import os
import re
import subprocess
import time
from dataclasses import dataclass
from pathlib import Path

import pytest

from tools import ue_bridge_tests

ROOT = Path(__file__).resolve().parents[1]
GAME_DIR = ROOT / "build" / "windows"
GAME = GAME_DIR / "halo.exe"
MAPS = {
    "a10": "levels\\a10\\a10",
    "bloodgulch": "levels\\test\\bloodgulch\\bloodgulch",
}


def map_available(name: str) -> bool:
    return GAME.is_file() and (GAME_DIR / "maps" / f"{name}.map").is_file()


@dataclass
class GameRun:
    process: subprocess.Popen
    log: Path

    def stop(self) -> None:
        self.process.kill()
        self.process.wait(timeout=30)


@pytest.fixture(scope="session")
def roles_exe() -> Path:
    return ue_bridge_tests.build_roles()


def run_game(scenario: str, extra_env: dict[str, str] | None = None) -> GameRun:
    env = dict(os.environ)
    env.update({"HALO_UE_BRIDGE": "1", "HALO_UE_BRIDGE_START_MAP": scenario, "HALO_FULLSCREEN": "0"})
    env.update(extra_env or {})
    log = GAME_DIR / "debug.txt"
    process = subprocess.Popen([str(GAME)], cwd=GAME_DIR, env=env)
    return GameRun(process, log)


def read_world(roles: Path) -> dict | None:
    result = subprocess.run([str(roles), "--role=read-world"], capture_output=True, text=True, timeout=60)
    if result.returncode != 0:
        return None
    return json.loads(result.stdout)


def wait_for_export(roles: Path, scenario: str, timeout: float = 180.0) -> dict:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        world = read_world(roles)
        if world and world["export_epoch"] and world["export_epoch"] == world["load_epoch"] and world["map"] == scenario:
            return world
        time.sleep(1.0)
    pytest.fail(f"no export of {scenario} within {timeout} s")


def export_line(log: Path, scenario: str) -> tuple[int, int]:
    """the export's milliseconds and its normal decode mismatches, from the game's debug.txt"""
    lines = log.read_text(errors="replace").splitlines()
    for line in reversed(lines):
        match = re.search(r"exported (\S+): .* in (\d+) ms; (\d+) normal decode mismatches", line)
        if match and match.group(1) == scenario:
            return int(match.group(2)), int(match.group(3))
    pytest.fail(f"no export line for {scenario} in {log}")


def first_bsp_export_ms(log: Path, scenario: str) -> int:
    """the milliseconds of the BSP line just before the map's export line (the map's own first BSP)"""
    lines = log.read_text(errors="replace").splitlines()
    end = max((i for i, line in enumerate(lines) if f"exported {scenario}:" in line), default=None)
    if end is None:
        pytest.fail(f"no export line for {scenario} in {log}")
    for line in reversed(lines[:end]):
        match = re.search(r"ue bridge: BSP \d+: .* exported in (\d+) ms", line)
        if match:
            return int(match.group(1))
    pytest.fail(f"no BSP export line before the export of {scenario} in {log}")


@pytest.mark.parametrize("name", list(MAPS))
def test_export_is_complete_and_consistent(roles_exe, name):
    if not map_available(name):
        pytest.skip(f"{name}.map is not in {GAME_DIR / 'maps'}")
    run = run_game(MAPS[name])
    try:
        world = wait_for_export(roles_exe, MAPS[name])
        assert world["export_complete"] == 1, world
        assert world["missing"] == 0, world
        # the game publishes its limits; UE refuses every model when one is left 0
        assert all(limit > 0 for limit in world["limits"]), world
        assert world["definitions"] > 0 and world["models"] > 0 and world["static_definitions"] > 0, world
        assert world["weights_out_of_range"] == 0, world
        loaded = [bsp for bsp in world["bsps"] if bsp["ready"]]
        assert len(loaded) >= 1, world
        for bsp in loaded:
            assert bsp["batches"] > 0 and bsp["triangles"] > 0, bsp
        # the winding convention: right-handed faces agree with the vertex normals
        assert world["model_normals_agree"] >= 0.9, world
        for bsp in loaded:
            assert bsp["normals_agree"] >= 0.95, bsp
        milliseconds, mismatches = export_line(run.log, MAPS[name])
        bsp_milliseconds = first_bsp_export_ms(run.log, MAPS[name])
        # the BSP's grouping must not grow with a cluster's surfaces times its shaders
        # (a10's BSP 0: 40,946 surfaces in 33 clusters took 74 ms when it did)
        assert bsp_milliseconds < 10, bsp_milliseconds
        # the shared normal decode reads what the game's own does
        assert mismatches == 0
        # the spec's 15 s silence rule for hosts: stop and report well before it (section 5.3)
        assert milliseconds < 5000
    finally:
        run.stop()


def hold_line(log: Path) -> tuple[str, int]:
    for line in reversed(log.read_text(errors="replace").splitlines()):
        match = re.search(r"hold ended: (.+) after (\d+) ms", line)
        if match:
            return match.group(1), int(match.group(2))
    pytest.fail(f"no hold line in {log}")


def start_fake_ue(roles: Path, ready_after_ms: int) -> subprocess.Popen:
    # roles.c takes "--name value" pairs; a long run so the stand-in renderer outlives the test
    return subprocess.Popen([str(roles), "--role=fake-ue", "--ready-after-ms", str(ready_after_ms), "--run-ms", "600000"])


@pytest.mark.parametrize("name", list(MAPS))
def test_ticks_carry_every_object_in_order(roles_exe, name):
    if not map_available(name):
        pytest.skip(f"{name}.map is not in {GAME_DIR / 'maps'}")
    run = run_game(MAPS[name])
    try:
        wait_for_export(roles_exe, MAPS[name])
        time.sleep(5.0)
        world = read_world(roles_exe)
        tick = world["tick"]
        assert tick["objects"] > 0 and tick["ascending"] == 1 and tick["truncated"] == 0, tick
        assert tick["load_epoch"] == world["load_epoch"], tick
        # scenery that never moves comes to rest on its second tick
        assert tick["at_rest"] > 0, tick
        assert 0 <= tick["active_bsp"] < len(world["bsps"]), tick
        frame = world["frame"]
        assert frame["camera_valid"] == 1 and 0.1 < frame["vertical_fov"] < 3.0, frame
        print(f"{name}: objects {tick['objects']}, at rest {tick['at_rest']}, used {tick['used']} bytes, bsp {tick['active_bsp']}")
    finally:
        run.stop()


def test_game_holds_until_the_renderer_is_ready(roles_exe):
    if not map_available("a10"):
        pytest.skip("a10.map is not in build/windows/maps")
    renderer = start_fake_ue(roles_exe, 3000)
    # the stand-in renderer is killed at the end; the game must not take that as its cue to quit mid-assert
    run = run_game(MAPS["a10"], {"HALO_UE_BRIDGE_ON_PEER_EXIT": "continue"})
    try:
        wait_for_export(roles_exe, MAPS["a10"])
        time.sleep(8.0)
        reason, held = hold_line(run.log)
        assert reason == "ready" and 2500 <= held < 10000, (reason, held)
    finally:
        run.stop()
        renderer.kill()


def test_game_hold_ends_at_the_cap(roles_exe):
    if not map_available("a10"):
        pytest.skip("a10.map is not in build/windows/maps")
    renderer = start_fake_ue(roles_exe, -1)
    run = run_game(MAPS["a10"], {"HALO_UE_BRIDGE_ON_PEER_EXIT": "continue"})
    try:
        wait_for_export(roles_exe, MAPS["a10"])
        time.sleep(13.0)
        reason, held = hold_line(run.log)
        assert reason == "timed out" and held >= 10000, (reason, held)
        # the game runs on after the cap
        first = read_world(roles_exe)["tick"]["id"]
        time.sleep(1.0)
        assert read_world(roles_exe)["tick"]["id"] > first
    finally:
        run.stop()
        renderer.kill()

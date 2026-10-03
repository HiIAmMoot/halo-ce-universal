"""pytest for the UE bridge (port/ue_bridge): the C unit tests, the same
under AddressSanitizer, and (from Task 5) the cross-process cases.

    python -m pytest tools/test_ue_bridge.py -v
"""

import ctypes
import os
import subprocess
import threading
import time
from pathlib import Path

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


EXCEPTION_ACCESS_VIOLATION = 0xC0000005
HUNG_PEER_EXIT_CODE = 0x48414E47


@pytest.fixture(scope="session")
def roles_exe():
    exe = ue_bridge_tests.build_roles()
    # another bridge (a running halo.exe or HaloCEUE) would attach to these tests' processes
    if subprocess.run([str(exe), "--role=probe-directory"]).returncode != 0:
        pytest.fail("a UE bridge directory already exists: close halo.exe and HaloCEUE before these tests")
    return exe


@pytest.fixture
def spawn(roles_exe):
    started: list[subprocess.Popen] = []

    def run(role: str, *args, cwd: Path | None = None) -> subprocess.Popen:
        process = subprocess.Popen([str(roles_exe), f"--role={role}", *map(str, args)], cwd=cwd,
                                   stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        started.append(process)
        return process

    yield run
    for process in started:
        if process.poll() is None:
            process.kill()
            process.wait()


def wait_for_file(path: Path, timeout: float = 5.0) -> None:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if path.exists() and path.read_text().strip():
            return
        time.sleep(0.02)
    raise AssertionError(f"{path} never appeared")


def start_game(spawn, tmp_path: Path, *args) -> subprocess.Popen:
    ready = tmp_path / "game.ready"
    game = spawn("fake-game", "--log", tmp_path / "debug.txt", "--ready-file", ready, *args)
    wait_for_file(ready)
    return game


def start_ue(spawn, tmp_path: Path, *args) -> tuple[subprocess.Popen, Path]:
    ready = tmp_path / "ue.ready"
    session = tmp_path / "session"
    ue = spawn("fake-ue", "--session-dir", session, "--ready-file", ready, *args)
    wait_for_file(ready)
    return ue, session


def finish(process: subprocess.Popen, timeout: float) -> int:
    return process.wait(timeout=timeout)


def game_report(directory: Path) -> dict[str, str]:
    text = (directory / "game_report.txt").read_text()
    return dict(line.split("=", 1) for line in text.splitlines() if "=" in line)


def is_minidump(path: Path) -> bool:
    return path.exists() and path.read_bytes()[:4] == b"MDMP"


def test_game_quits_when_ue_exits(spawn, tmp_path):
    game = start_game(spawn, tmp_path)
    ue, session = start_ue(spawn, tmp_path, "--exit-after-ms", 1000)
    assert finish(ue, 10) == 0
    assert finish(game, 5) == 0
    assert game_report(session)["peer_action"] == "peer_exited"


def test_game_quits_when_ue_crashes(spawn, tmp_path):
    game = start_game(spawn, tmp_path)
    ue, session = start_ue(spawn, tmp_path, "--crash-after-ms", 1000)
    assert finish(ue, 10) == EXCEPTION_ACCESS_VIOLATION
    assert finish(game, 5) == 0
    report = game_report(session)
    assert report["peer_action"] == "peer_crashed"
    assert report["peer_exit_code"] == "0xc0000005"


def test_game_kills_hung_standalone_ue(spawn, tmp_path):
    game = start_game(spawn, tmp_path)
    ue, session = start_ue(spawn, tmp_path, "--hang-after-ms", 500, "--hang-timeout-ms", 1500)
    assert finish(ue, 10) == HUNG_PEER_EXIT_CODE
    assert finish(game, 5) == 0
    assert game_report(session)["peer_action"] == "peer_hung"


def test_game_never_kills_hung_editor_ue(spawn, tmp_path):
    game = start_game(spawn, tmp_path)
    ue, session = start_ue(spawn, tmp_path, "--editor", "--hang-after-ms", 500, "--hang-timeout-ms", 1500,
                           "--run-ms", 8000)
    assert finish(game, 8) == 0
    assert game_report(session)["peer_action"] == "peer_hung_editor"
    assert finish(ue, 10) == 0


def test_no_hang_action_while_ue_debugger_flag_set(spawn, tmp_path):
    game = start_game(spawn, tmp_path)
    ue, session = start_ue(spawn, tmp_path, "--debugger-flag", "--hang-after-ms", 200, "--hang-timeout-ms", 1000,
                           "--run-ms", 4000)
    time.sleep(3.0)
    assert game.poll() is None
    assert finish(ue, 5) == 0
    assert finish(game, 5) == 0
    assert game_report(session)["peer_action"] == "peer_exited"


def test_game_crash_writes_its_own_dump_then_waits_for_ues(spawn, tmp_path):
    game = start_game(spawn, tmp_path, "--crash-after-ms", 1500)
    ue, session = start_ue(spawn, tmp_path, "--dump-on-crashing")
    started = time.monotonic()
    assert finish(game, 10) == EXCEPTION_ACCESS_VIOLATION
    assert time.monotonic() - started < 8.0  # released by ue_dump_done, not the 10 s cap
    assert is_minidump(session / "game_crash_self.dmp")
    assert is_minidump(session / "game_crash.dmp")
    assert finish(ue, 5) == 0


def test_game_leaves_a_crashing_ue_alone(spawn, tmp_path):
    game = start_game(spawn, tmp_path)
    ue, session = start_ue(spawn, tmp_path, "--crash-after-ms", 500, "--crash-linger-ms", 4000,
                           "--hang-timeout-ms", 1500)
    # silent for 4 s, beyond its 1.5 s timeout, but crashing: the game must not terminate it
    assert finish(ue, 10) == EXCEPTION_ACCESS_VIOLATION
    assert finish(game, 5) == 0
    assert game_report(session)["peer_action"] == "peer_crashed"


def test_no_hang_action_while_ue_busy(spawn, tmp_path):
    game = start_game(spawn, tmp_path)
    ue, session = start_ue(spawn, tmp_path, "--busy-flag", "--hang-after-ms", 200, "--hang-timeout-ms", 1000,
                           "--run-ms", 4000)
    time.sleep(3.0)
    assert game.poll() is None
    assert finish(ue, 5) == 0
    assert finish(game, 5) == 0


def test_game_report_carries_the_header_and_ues_crash_folder(spawn, tmp_path):
    game = start_game(spawn, tmp_path)
    ue, session = start_ue(spawn, tmp_path, "--exit-after-ms", 1000)
    assert finish(ue, 10) == 0
    assert finish(game, 5) == 0
    assert (session / "header.bin").stat().st_size == 0x1000
    assert game_report(session)["ue_crashes_dir"].endswith("Crashes")


def test_crash_self_dumps_when_no_ue(spawn, tmp_path):
    game = start_game(spawn, tmp_path, "--crash-after-ms", 500)
    assert finish(game, 5) == EXCEPTION_ACCESS_VIOLATION
    assert is_minidump(tmp_path / "game_crash_self.dmp")


def test_crash_wait_is_capped_when_ue_ignores_it(spawn, tmp_path):
    started = time.monotonic()
    game = start_game(spawn, tmp_path, "--crash-after-ms", 1000)
    ue, session = start_ue(spawn, tmp_path)
    assert finish(game, 16) == EXCEPTION_ACCESS_VIOLATION
    elapsed = time.monotonic() - started
    assert 10.0 < elapsed < 14.0
    assert is_minidump(session / "game_crash_self.dmp")
    assert finish(ue, 5) == 0


def test_continue_mode_keeps_game_running(spawn, tmp_path):
    game = start_game(spawn, tmp_path, "--continue", "--run-ms", 5000)
    ue, session = start_ue(spawn, tmp_path, "--exit-after-ms", 800)
    assert finish(ue, 5) == 0
    time.sleep(1.5)
    assert game.poll() is None
    assert finish(game, 8) == 0
    assert game_report(session)["peer_action"] == "peer_exited"


def probe_entry(spawn) -> dict[str, str]:
    probe = spawn("probe-directory", "--print")
    out, _ = probe.communicate(timeout=10)
    return dict(line.split("=", 1) for line in out.splitlines() if "=" in line)


def test_published_log_path_is_made_absolute(spawn, tmp_path):
    ready = tmp_path / "game.ready"
    # the game's own default is "./debug.txt": UE would resolve that against its own folder
    game = spawn("fake-game", "--log", "./debug.txt", "--ready-file", ready, cwd=tmp_path)
    wait_for_file(ready)
    published = probe_entry(spawn)["game_log_path"]
    assert os.path.normcase(os.path.realpath(published)) == os.path.normcase(os.path.realpath(tmp_path / "debug.txt"))
    assert os.path.isabs(published) and "\\.\\" not in published and "/./" not in published
    game.kill()


def test_concurrent_games_leave_the_directory_coherent(spawn, tmp_path):
    probe_ready = tmp_path / "probe.ready"
    stop = tmp_path / "probe.stop"
    go = tmp_path / "go"
    probe = spawn("probe-directory", "--check-ms", 120000, "--stop-file", stop, "--ready-file", probe_ready)
    wait_for_file(probe_ready)
    games = []
    for index in range(2):
        ready = tmp_path / f"game{index}.ready"
        games.append(spawn("fake-game", "--log", tmp_path / f"debug{index}.txt", "--ready-file", ready,
                           "--cycles", 60000, "--go-file", go))
        wait_for_file(ready)
    go.write_text("go")
    for game in games:
        assert finish(game, 120) == 0
    # a lock that is never released makes the other game wait out its cap, and that is logged
    for index in range(2):
        assert "directory lock" not in (tmp_path / f"debug{index}.txt").read_text()
    stop.write_text("stop")
    out, _ = probe.communicate(timeout=10)
    assert probe.returncode == 0, out
    report = dict(line.split("=", 1) for line in out.splitlines() if "=" in line)
    assert int(report["reads"]) > 0
    assert report["incoherent"] == "0"
    assert report["final"] == "empty"


MUTEX_NAME = "Local\\HaloCEUE.Bridge.DirectoryLock"
WAIT_OBJECT_0 = 0


def hold_mutex_until(release: threading.Event, held: threading.Event, abandon: bool) -> None:
    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel32.CreateMutexW.restype = ctypes.c_void_p
    kernel32.WaitForSingleObject.argtypes = [ctypes.c_void_p, ctypes.c_uint32]
    kernel32.ReleaseMutex.argtypes = [ctypes.c_void_p]
    handle = kernel32.CreateMutexW(None, False, MUTEX_NAME)
    assert kernel32.WaitForSingleObject(handle, 5000) == WAIT_OBJECT_0
    held.set()
    release.wait(30)
    if not abandon:
        kernel32.ReleaseMutex(handle)
    # an abandoned mutex: this thread ends owning it (the handle stays open on purpose)


def test_a_hung_directory_lock_holder_delays_game_start_by_the_cap_only(spawn, tmp_path):
    release, held = threading.Event(), threading.Event()
    holder = threading.Thread(target=hold_mutex_until, args=(release, held, False))
    holder.start()
    try:
        assert held.wait(5)
        started = time.monotonic()
        game = start_game(spawn, tmp_path, "--run-ms", 1500)
        waited = time.monotonic() - started
        assert 1.5 < waited < 4.5  # 2 s cap: not instant, and never hung
        assert "directory lock" in (tmp_path / "debug.txt").read_text()
        assert finish(game, 10) == 0
    finally:
        release.set()
        holder.join()


def test_an_abandoned_directory_lock_is_taken_over_without_waiting(spawn, tmp_path):
    release, held = threading.Event(), threading.Event()
    holder = threading.Thread(target=hold_mutex_until, args=(release, held, True))
    holder.start()
    assert held.wait(5)
    release.set()
    holder.join()
    started = time.monotonic()
    game = start_game(spawn, tmp_path, "--run-ms", 1500)
    assert time.monotonic() - started < 1.5
    assert "directory lock" not in (tmp_path / "debug.txt").read_text()
    assert finish(game, 10) == 0


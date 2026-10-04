"""pytest for the UE bridge (port/ue_bridge): the C unit tests, the same
under AddressSanitizer, and (from Task 5) the cross-process cases.

    python -m pytest tools/test_ue_bridge.py -v

The cross-process tests use the bridge's fixed session-local section and mutex names,
so they can't run in parallel (pytest-xdist) or beside a running halo.exe.
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
STATUS_STACK_OVERFLOW = 0xC00000FD

# byte offsets into the game's header section and the stop codes: copies of the layout in
# port/ue_bridge/ue_bridge_format.h, whose static asserts pin them. Change both together.
TICK_RING_PUBLISHED_OFFSET = 60  # tick_ring.published
FRAME_RING_PUBLISHED_OFFSET = 76  # frame_ring.published
GAME_HEARTBEAT_OFFSET = 600
GAME_STOPPING_OFFSET = 624
GAME_REFRESH_HZ_OFFSET = 1216
GAME_FRAME_TARGET_HZ_OFFSET = 1220
STOP_EXIT = 1
FILE_MAP_READ = 4


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

    def run(role: str, *args, cwd: Path | None = None, capture: bool = False) -> subprocess.Popen:
        # an unread pipe would fill and block the role; only a test that reads the output asks for one
        output = subprocess.PIPE if capture else subprocess.DEVNULL
        process = subprocess.Popen([str(roles_exe), f"--role={role}", *map(str, args)], cwd=cwd,
                                   stdout=output, stderr=subprocess.STDOUT if capture else subprocess.DEVNULL, text=True)
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


def start_game(spawn, tmp_path: Path, *args, timeout: float = 5.0) -> subprocess.Popen:
    ready = tmp_path / "game.ready"
    game = spawn("fake-game", "--log", tmp_path / "debug.txt", "--ready-file", ready, *args)
    wait_for_file(ready, timeout)
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


def test_game_leaves_an_exiting_ue_alone_while_it_shuts_down(spawn, tmp_path):
    game = start_game(spawn, tmp_path)
    # EXIT published at 0.5 s, then silent for 6 s: far beyond its 1.5 s timeout, but exiting
    ue, session = start_ue(spawn, tmp_path, "--exit-after-ms", 500, "--exit-linger-ms", 6000, "--hang-timeout-ms", 1500)
    assert finish(ue, 12) == 0
    assert finish(game, 5) == 0
    assert game_report(session)["peer_action"] == "peer_exited"


@pytest.mark.slow
def test_game_ends_an_exiting_ue_that_never_finishes_after_the_grace(spawn, tmp_path):
    game = start_game(spawn, tmp_path, "--run-ms", 90000)
    # the grace is the crashing limit (30 s): past it a silent exiting UE has hung
    ue, session = start_ue(spawn, tmp_path, "--exit-after-ms", 500, "--exit-linger-ms", 60000, "--hang-timeout-ms", 1500)
    started = time.monotonic()
    assert finish(ue, 45) == HUNG_PEER_EXIT_CODE
    assert time.monotonic() - started > 25.0
    assert finish(game, 5) == 0
    assert game_report(session)["peer_action"] == "peer_hung"


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
    # the game's own filter runs first, so its crash lines are in debug.txt before UE copies it
    assert "crash:" in (tmp_path / "debug.txt").read_text()


def test_a_crash_hook_that_outlived_the_bridge_does_not_park_a_later_crash(spawn, tmp_path):
    game = spawn("fake-game", "--log", tmp_path / "debug.txt", "--crash-after-stop")
    # exit 9: the second thread's fault found the crash ownership still taken and parked
    assert finish(game, 15) == 0


def test_stack_overflow_still_writes_the_self_dump(spawn, tmp_path):
    game = start_game(spawn, tmp_path, "--overflow-after-ms", 500)
    assert finish(game, 20) == STATUS_STACK_OVERFLOW
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


def test_the_games_bridge_messages_reach_its_debug_file(spawn, tmp_path):
    """the real game's platform_log is stderr, which a windowed game never shows: the watcher's lines
    (and the roles' platform_log, which here is stdout only) must be in debug.txt"""
    game = start_game(spawn, tmp_path, "--run-ms", 4000)
    ue, _ = start_ue(spawn, tmp_path, "--exit-after-ms", 800)
    assert finish(ue, 10) == 0
    assert finish(game, 8) == 0
    log = (tmp_path / "debug.txt").read_text()
    assert f"ue bridge: renderer {ue.pid} attached" in log
    assert "ue bridge: renderer peer_exited" in log


def test_a_game_with_the_bridge_off_logs_no_bridge_line(spawn, tmp_path):
    game = start_game(spawn, tmp_path, "--disabled", "--run-ms", 1000)
    assert finish(game, 8) == 0
    assert "ue bridge" not in (tmp_path / "debug.txt").read_text()


def test_continue_mode_keeps_game_running(spawn, tmp_path):
    game = start_game(spawn, tmp_path, "--continue", "--run-ms", 5000)
    ue, session = start_ue(spawn, tmp_path, "--exit-after-ms", 800)
    assert finish(ue, 5) == 0
    time.sleep(1.5)
    assert game.poll() is None
    assert finish(game, 8) == 0
    assert game_report(session)["peer_action"] == "peer_exited"


def lock_messages(log: Path) -> list[str]:
    """the log's directory-lock lines, at most a few: a failing assertion shows them, not the whole log"""
    return [line for line in log.read_text().splitlines() if "directory lock" in line][:3]


def probe_entry(spawn) -> dict[str, str]:
    probe = spawn("probe-directory", "--print", capture=True)
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
    """a smoke test: it asserts final == empty and reads > 0 (and no lock timeouts in the logs). The
    exclusion and the lock release are pinned by the deterministic tests below it."""
    probe_ready = tmp_path / "probe.ready"
    stop = tmp_path / "probe.stop"
    go = tmp_path / "go"
    probe = spawn("probe-directory", "--check-ms", 60000, "--stop-file", stop, "--ready-file", probe_ready,
                   capture=True)
    wait_for_file(probe_ready)
    games = []
    for index in range(2):
        ready = tmp_path / f"game{index}.ready"
        games.append(spawn("fake-game", "--log", tmp_path / f"debug{index}.txt", "--ready-file", ready,
                           "--cycles", 60000, "--go-file", go))
        wait_for_file(ready)
    go.write_text("go")
    for game in games:
        assert finish(game, 40) == 0
    # a lock that is never released makes the other game wait out its cap, and that is logged
    for index in range(2):
        assert lock_messages(tmp_path / f"debug{index}.txt") == []
    stop.write_text("stop")
    out, _ = probe.communicate(timeout=10)
    assert probe.returncode == 0, out
    report = dict(line.split("=", 1) for line in out.splitlines() if "=" in line)
    assert int(report["reads"]) > 0
    assert report["incoherent"] == "0"
    assert report["final"] == "empty"


def test_two_games_are_never_inside_the_directory_lock_together(spawn, tmp_path):
    go = tmp_path / "go"
    games = []
    for index in range(2):
        ready = tmp_path / f"game{index}.ready"
        games.append(spawn("fake-game", "--log", tmp_path / f"debug{index}.txt", "--ready-file", ready,
                           "--cycles", 100, "--hold-ms", 3, "--go-file", go))
        wait_for_file(ready)
    go.write_text("go")
    # exit code 6: the game saw the other inside the lock too
    assert [finish(game, 40) for game in games] == [0, 0]


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
    # an abandoned mutex: this thread ends owning it. The handle stays open on
    # purpose: closing the last handle destroys the named mutex, the game would
    # create a fresh one, and WAIT_ABANDONED would never be exercised.


def test_a_second_game_starts_without_waiting_for_a_live_first_game(spawn, tmp_path):
    for name in ("a", "b"):
        (tmp_path / name).mkdir()
    start_game(spawn, tmp_path / "a")
    started = time.monotonic()
    start_game(spawn, tmp_path / "b")
    # the first game's start released the lock; one that kept it would hold this start to the 2 s cap
    assert time.monotonic() - started < 1.5
    assert lock_messages(tmp_path / "b" / "debug.txt") == []


def test_a_hung_directory_lock_holder_delays_game_start_by_the_cap_only(spawn, tmp_path):
    release, held = threading.Event(), threading.Event()
    holder = threading.Thread(target=hold_mutex_until, args=(release, held, False))
    holder.start()
    try:
        assert held.wait(5)
        started = time.monotonic()
        game = start_game(spawn, tmp_path, "--run-ms", 1500, timeout=10)
        # it did wait out the 2 s cap, and then started anyway
        assert time.monotonic() - started > 1.5
        assert any("directory lock timed out" in line for line in lock_messages(tmp_path / "debug.txt"))
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
    game = start_game(spawn, tmp_path, "--run-ms", 1500)
    assert lock_messages(tmp_path / "debug.txt") == []
    assert finish(game, 10) == 0


class HeaderView:
    """the game's header section, held open: the section outlives the game while this handle does,
    so the stop the game published can be read after it has exited"""

    def __init__(self, spawn):
        kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
        kernel32.OpenFileMappingA.restype = ctypes.c_void_p
        kernel32.MapViewOfFile.restype = ctypes.c_void_p
        kernel32.MapViewOfFile.argtypes = [ctypes.c_void_p, ctypes.c_uint32, ctypes.c_uint32, ctypes.c_uint32, ctypes.c_size_t]
        section = probe_entry(spawn)["section"]
        self.handle = kernel32.OpenFileMappingA(FILE_MAP_READ, False, section.encode())
        assert self.handle, "the game's section is not there"
        self.kernel32 = kernel32
        self.view = kernel32.MapViewOfFile(self.handle, FILE_MAP_READ, 0, 0, 0x1000)
        if not self.view:
            # the fixture only closes views that were built, and a failed constructor builds none
            kernel32.CloseHandle(ctypes.c_void_p(self.handle))
            raise AssertionError("the game's section cannot be mapped")

    def u64(self, offset: int) -> int:
        return ctypes.c_uint64.from_address(self.view + offset).value

    def u32(self, offset: int) -> int:
        return ctypes.c_uint32.from_address(self.view + offset).value

    def close(self) -> None:
        self.kernel32.UnmapViewOfFile(ctypes.c_void_p(self.view))
        self.kernel32.CloseHandle(ctypes.c_void_p(self.handle))


@pytest.fixture
def header_view(spawn):
    views: list[HeaderView] = []

    def open_view() -> HeaderView:
        views.append(HeaderView(spawn))
        return views[-1]

    yield open_view
    for view in views:
        view.close()


def test_exit_when_file_appears_the_game_exits_and_publishes_the_stop(spawn, header_view, tmp_path):
    flag = tmp_path / "exit.flag"
    game = start_game(spawn, tmp_path, "--run-ms", 60000, "--exit-when", flag)
    view = header_view()
    time.sleep(0.5)
    assert game.poll() is None
    assert view.u32(GAME_STOPPING_OFFSET) == 0
    flag.write_text("1")
    assert finish(game, 5) == 0
    assert view.u32(GAME_STOPPING_OFFSET) == STOP_EXIT
    # the role's own log line: a run that ended on the file must not read as "run time over"
    assert "exit file" in (tmp_path / "debug.txt").read_text()


def test_the_game_publishes_its_refresh_rate_and_frame_target(spawn, header_view, tmp_path):
    game = start_game(spawn, tmp_path, "--run-ms", 60000, "--refresh-hz", 144, "--frame-target-hz", 60)
    view = header_view()
    assert view.u32(GAME_REFRESH_HZ_OFFSET) == 144
    assert view.u32(GAME_FRAME_TARGET_HZ_OFFSET) == 60


def test_a_game_that_publishes_no_target_leaves_both_fields_zero(spawn, header_view, tmp_path):
    game = start_game(spawn, tmp_path, "--run-ms", 60000)
    view = header_view()
    assert view.u32(GAME_REFRESH_HZ_OFFSET) == 0
    assert view.u32(GAME_FRAME_TARGET_HZ_OFFSET) == 0


def test_a_changed_frame_target_is_republished(spawn, header_view, tmp_path):
    flag = tmp_path / "target.txt"
    game = start_game(spawn, tmp_path, "--run-ms", 60000, "--refresh-hz", 144, "--frame-target-hz", 144, "--target-file", flag)
    view = header_view()
    assert view.u32(GAME_FRAME_TARGET_HZ_OFFSET) == 144
    flag.write_text("30")
    deadline = time.time() + 5
    while view.u32(GAME_FRAME_TARGET_HZ_OFFSET) != 30 and time.time() < deadline:
        time.sleep(0.05)
    assert view.u32(GAME_FRAME_TARGET_HZ_OFFSET) == 30
    assert view.u32(GAME_REFRESH_HZ_OFFSET) == 144


def test_crash_when_file_appears_the_game_crashes(spawn, tmp_path):
    flag = tmp_path / "crash.flag"
    game = start_game(spawn, tmp_path, "--run-ms", 60000, "--crash-when", flag)
    time.sleep(0.5)
    assert game.poll() is None
    flag.write_text("1")
    assert finish(game, 15) == EXCEPTION_ACCESS_VIOLATION
    assert is_minidump(tmp_path / "game_crash_self.dmp")


def test_hang_when_file_appears_the_heartbeat_stops_and_the_game_stays_alive(spawn, header_view, tmp_path):
    flag = tmp_path / "hang.flag"
    game = start_game(spawn, tmp_path, "--run-ms", 60000, "--hang-when", flag)
    view = header_view()
    first = view.u64(GAME_HEARTBEAT_OFFSET)
    time.sleep(0.5)
    assert view.u64(GAME_HEARTBEAT_OFFSET) > first
    assert view.u32(FRAME_RING_PUBLISHED_OFFSET) > 0 and view.u32(TICK_RING_PUBLISHED_OFFSET) > 0
    flag.write_text("1")
    time.sleep(0.5)
    stalled = (view.u64(GAME_HEARTBEAT_OFFSET), view.u32(FRAME_RING_PUBLISHED_OFFSET), view.u32(TICK_RING_PUBLISHED_OFFSET))
    time.sleep(1.0)
    assert (view.u64(GAME_HEARTBEAT_OFFSET), view.u32(FRAME_RING_PUBLISHED_OFFSET), view.u32(TICK_RING_PUBLISHED_OFFSET)) == stalled
    assert game.poll() is None

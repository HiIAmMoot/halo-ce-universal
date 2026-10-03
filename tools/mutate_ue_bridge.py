"""Mutation testing for the UE bridge: every mutant must make its named test
fail. Run after a task's tests pass:

    python tools/mutate_ue_bridge.py [task] [--path FILE ...]

--path (repeatable, relative to the repository root) keeps only the mutants of
those files; with a task it is an AND. Use it to avoid rewriting files another
build is reading.

Mutants are single-line replacements (the checkout's line endings vary).
"""

from __future__ import annotations

import argparse
import os
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path

# run as a script, Python puts tools/ on the path, not the repository root
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from tools import ue_bridge_tests  # noqa: E402


@dataclass(frozen=True)
class Mutant:
    task: str
    path: str  # relative to the repository root
    original: str  # one line, occurring exactly once in the file
    mutated: str
    killed_by: str  # a test-name filter for ue_bridge_tests.exe, or "pytest:<test name>" for a cross-process test


MUTANTS = [
    Mutant("1", "port/ue_bridge/ue_bridge_ring.c",
           "return ueb_load_u32(&slot->sequence) == before;", "return 1;",
           "slot_try_read_detects_change_during_copy"),
    Mutant("1", "port/ue_bridge/ue_bridge_ring.c",
           "if (before & 1u)", "if (0)",
           "ring_read_takes_previous_while_newest_is_odd"),
    Mutant("1", "port/ue_bridge/ue_bridge_ring.c",
           "<= section_size;", "<= section_size + slot_size;",
           "ring_valid_rejects_a_ring_past_the_section"),
    Mutant("1", "port/ue_bridge/ue_bridge_ring.c",
           "ueb_store_u32(&ring->published, ring->published + 1u);", ";",
           "ring_write_then_read_newest"),
    Mutant("1", "port/ue_bridge/ue_bridge_ring.c",
           "if (slot_count < 2u", "if (slot_count < 1u",
           "ring_valid_rejects_fewer_than_two_slots"),
    # a 64-bit store split into two 32-bit halves: the torn-store stress test must see it
    Mutant("1", "port/ue_bridge/ue_bridge_ring.h",
           "__atomic_store_n(address, value, __ATOMIC_SEQ_CST);",
           "((volatile uint32_t *)address)[0] = (uint32_t)value; ((volatile uint32_t *)address)[1] = (uint32_t)(value >> 32);",
           "ring_heartbeat_store_is_never_torn"),
    Mutant("2", "port/ue_bridge/ue_bridge_policy.c",
           "if (peer->stopping == UE_BRIDGE_STOP_CRASH || ue_bridge_exit_code_is_crash(peer->exit_code))",
           "if (ue_bridge_exit_code_is_crash(peer->exit_code))",
           "policy_published_crash_is_crashed_whatever_the_exit_code"),
    Mutant("2", "port/ue_bridge/ue_bridge_policy.c",
           "return exit_code >= 0xC0000000u;", "return exit_code > 0xC0000000u;",
           "policy_exit_code_classification"),
    Mutant("2", "port/ue_bridge/ue_bridge_policy.c",
           "if (peer->debugger_attached || peer->busy || peer->heartbeat_qpc == 0 || qpc_frequency == 0)",
           "if (peer->busy || peer->heartbeat_qpc == 0 || qpc_frequency == 0)",
           "policy_debugger_suppresses_hang"),
    Mutant("2", "port/ue_bridge/ue_bridge_policy.c",
           "if (peer->debugger_attached || peer->busy || peer->heartbeat_qpc == 0 || qpc_frequency == 0)",
           "if (peer->debugger_attached || peer->heartbeat_qpc == 0 || qpc_frequency == 0)",
           "policy_busy_suppresses_hang"),
    Mutant("2", "port/ue_bridge/ue_bridge_policy.c",
           "if (now_qpc <= peer->heartbeat_qpc)", "if (0)",
           "policy_heartbeat_ahead_of_now_is_not_a_hang"),
    Mutant("2", "port/ue_bridge/ue_bridge_policy.c",
           "timeout_ms = peer->hang_timeout_ms ? peer->hang_timeout_ms : UE_BRIDGE_DEFAULT_HANG_TIMEOUT_MS;",
           "timeout_ms = UE_BRIDGE_DEFAULT_HANG_TIMEOUT_MS;",
           "policy_stall_below_peer_timeout_is_not_a_hang"),
    Mutant("2", "port/ue_bridge/ue_bridge_policy.c",
           "if (age_ms <= timeout_ms)", "if (age_ms < timeout_ms)",
           "policy_stall_below_peer_timeout_is_not_a_hang"),
    Mutant("2", "port/ue_bridge/ue_bridge_policy.c",
           "return peer->is_editor ? UE_BRIDGE_ACTION_PEER_HUNG_EDITOR : UE_BRIDGE_ACTION_PEER_HUNG;",
           "return UE_BRIDGE_ACTION_PEER_HUNG;",
           "policy_editor_hang_is_hung_editor"),
    Mutant("2", "port/ue_bridge/ue_bridge_policy.c",
           "if (peer->crashing && (peer->crashing_for_ms <= UE_BRIDGE_CRASHING_LIMIT_MS || peer->debugger_attached))", "if (0)",
           "policy_crashing_peer_is_crashing"),
    Mutant("2", "port/ue_bridge/ue_bridge_policy.c",
           "if (peer->crashing && (peer->crashing_for_ms <= UE_BRIDGE_CRASHING_LIMIT_MS || peer->debugger_attached))", "if (peer->crashing)",
           "policy_crashing_too_long_is_hung"),
    Mutant("2", "port/ue_bridge/ue_bridge_policy.c",
           "return !continue_on_peer_exit;", "return 1;",
           "policy_shutdown_follows_continue_mode"),
    Mutant("2", "port/ue_bridge/ue_bridge_policy.c",
           "if (peer->crashing && (peer->crashing_for_ms <= UE_BRIDGE_CRASHING_LIMIT_MS || peer->debugger_attached))",
           "if (peer->crashing && peer->crashing_for_ms <= UE_BRIDGE_CRASHING_LIMIT_MS)",
           "policy_debugger_suppresses_crashing_too_long"),
    Mutant("3", "port/linux/src/ue_bridge.c",
           "if (bridge.section_view || !settings->enabled)", "if (bridge.section_view)",
           "core_disabled_maps_nothing"),
    Mutant("3", "port/linux/src/ue_bridge.c",
           "if (!bridge.session_id)", "if (0)",
           "core_zero_random_session_becomes_one"),
    Mutant("3", "port/linux/src/ue_bridge.c",
           "uint32_t odd = ueb_load_u32(&directory->sequence) | 1u;",
           "uint32_t odd = ueb_load_u32(&directory->sequence) + 1u;",
           "core_directory_left_odd_by_dead_game_is_repaired"),
    Mutant("3", "port/linux/src/ue_bridge.c",
           "if (directory->session_id == bridge.session_id)", "if (1)",
           "core_stop_keeps_a_newer_games_directory_entry"),
    Mutant("3", "port/linux/src/ue_bridge.c",
           "if (source && strlen(source) < capacity)", "if (source && strlen(source) <= capacity)",
           "core_log_path_that_does_not_fit_is_published_empty"),
    Mutant("3", "port/linux/src/ue_bridge.c",
           "if (source && strlen(source) < capacity)", "if (source && strlen(source) + 1 < capacity)",
           "core_log_path_that_just_fits_is_published_whole"),
    Mutant("3", "port/linux/src/ue_bridge.c",
           "utf8[0] = 0;", ";",
           "core_log_path_from_a_silently_failing_conversion_is_empty"),
    Mutant("3", "port/linux/src/ue_bridge.c",
           "bridge.os->debugger_present() ? 1u : 0u", "0u",
           "core_heartbeat_tracks_qpc_and_debugger"),
    Mutant("3", "port/linux/src/ue_bridge.c",
           "slot->id = tick;", "slot->id = 0;",
           "core_publish_tick_and_frame"),
    Mutant("3", "port/linux/src/ue_bridge.c",
           "ueb_store_u32(&header->game_busy, busy ? 1u : 0u);", ";",
           "core_busy_flag_round_trips"),
    Mutant("3", "port/linux/src/ue_bridge.c",
           "os->unmap_section(bridge.section_view, bridge.section_handle);", ";",
           "core_directory_failure_unmaps_section"),
    Mutant("3", "port/linux/src/ue_bridge.c",
           "ueb_store_u32(&header->game_stopping, stopping);", ";",
           "core_stop_clears_own_directory_entry_and_unmaps"),
    Mutant("3", "port/linux/src/ue_bridge.c",
           "header->magic = UE_BRIDGE_MAGIC;",
           "{ void *v, *h; v = os->map_section(UE_BRIDGE_DIRECTORY_NAME, UE_BRIDGE_DIRECTORY_SIZE, &h); publish_directory((volatile struct ue_bridge_directory *)v, pid, section_name); } header->magic = UE_BRIDGE_MAGIC;",
           "core_header_is_complete_when_the_directory_is_mapped"),
    Mutant("3", "port/linux/src/ue_bridge.c",
           "uint32_t sequence = directory_begin_write(directory);",
           "uint32_t sequence = ueb_load_u32(&directory->sequence) - 1u;",
           "core_concurrent_reader_never_sees_a_mixed_directory_entry"),
    Mutant("3", "port/linux/src/ue_bridge.c",
           "uint32_t odd = directory_begin_write(directory);",
           "directory->session_id = bridge.session_id; uint32_t odd = directory_begin_write(directory);",
           "core_concurrent_reader_never_sees_a_mixed_directory_entry"),
    Mutant("3", "port/linux/src/ue_bridge.c",
           "ue_bridge_publish_stopping(stopping);",
           "bridge.os->unmap_section(section_view, bridge.section_handle); bridge.os->unmap_section(bridge.directory_view, bridge.directory_handle); ue_bridge_publish_stopping(stopping);",
           "core_stop_writes_before_it_unmaps"),
    Mutant("3", "port/linux/src/ue_bridge.c",
           "ue_bridge_ring_end_write(&header->tick_ring, slot);",
           "slot->publish_qpc = 0; ue_bridge_ring_end_write(&header->tick_ring, slot); slot->publish_qpc = bridge.os->qpc();",
           "core_concurrent_reader_never_sees_a_tick_without_its_payload"),
    Mutant("3", "port/linux/src/ue_bridge.c",
           "bridge.os->unlock_directory();", ";",
           "core_start_and_stop_each_take_the_directory_lock_once"),
    Mutant("4", "port/linux/src/ue_bridge_game.c",
           "if (!config_boolean(\"ue_bridge.enabled\"))", "if (0)",
           "game_disabled_starts_nothing"),
    Mutant("4", "port/linux/src/ue_bridge_game.c",
           "watch.continue_on_peer_exit = strcmp(on_peer_exit, \"continue\") == 0;",
           "watch.continue_on_peer_exit = 0;",
           "game_continue_setting_reaches_watcher"),
    Mutant("4", "port/linux/src/ue_bridge_game.c",
           "if (!ue_bridge_platform_start_watcher(&watch))",
           "if (!ue_bridge_platform_start_watcher(&watch) && 0)",
           "game_watcher_failure_stops_bridge"),
    Mutant("4", "port/linux/src/ue_bridge_game.c",
           "if (start_attempted)", "if (start_attempted && 0)",
           "game_starts_only_once"),
    Mutant("4", "port/linux/src/ue_bridge_game.c",
           "ue_bridge_platform_stop_watcher();", ";",
           "game_shutdown_stops_watcher_and_bridge"),
    Mutant("4", "port/linux/src/ue_bridge_game.c",
           "ue_bridge_set_busy(loading);", ";",
           "game_loading_sets_and_clears_busy"),
    Mutant("4", "port/linux/src/ue_bridge_game.c",
           "ue_bridge_publish_stopping(UE_BRIDGE_STOP_CRASH);", ";",
           "game_halted_stops_heartbeat_and_publishes_crash"),
    Mutant("4", "port/linux/src/ue_bridge_game.c",
           "if (started && !halted)", "if (started)",
           "game_halted_stops_heartbeat_and_publishes_crash"),
    Mutant("4", "port/linux/src/ue_bridge_game.c",
           "if (!halted)", "if (1)",
           "game_halted_stops_heartbeat_and_publishes_crash"),
    Mutant("4", "port/linux/src/ue_bridge_game.c",
           "ue_bridge_set_busy(0);", ";",
           "game_halted_stops_heartbeat_and_publishes_crash"),
    Mutant("4", "port/linux/src/ue_bridge_game.c",
           "halted = 0;", ";",
           "game_shutdown_after_halt_beats_again_on_restart"),
    Mutant("4", "port/linux/src/ue_bridge_game.c",
           "int stopping = halted ? UE_BRIDGE_STOP_CRASH : UE_BRIDGE_STOP_EXIT;",
           "int stopping = UE_BRIDGE_STOP_EXIT;",
           "game_shutdown_after_halt_keeps_the_published_crash"),
    Mutant("4", "port/linux/src/ue_bridge.c",
           "ueb_store_u32(&header->game_stopping, stopping);", "(void)stopping;",
           "core_publish_stopping_sets_flag_only_when_active"),
    Mutant("5", "port/linux/src/ue_bridge.c",
           "__atomic_store_n(&bridge.section_view, NULL, __ATOMIC_SEQ_CST);", ";",
           "core_stop_hides_the_header_from_the_crash_filter_before_unmapping_it"),
    Mutant("5", "port/windows/src/win32_ue_bridge.c",
           "if (action == UE_BRIDGE_ACTION_PEER_HUNG)", "if (0)",
           "pytest:test_game_kills_hung_standalone_ue"),
    Mutant("5", "port/windows/src/win32_ue_bridge.c",
           "if (ue_bridge_policy_shuts_down(action, watch_config.continue_on_peer_exit))", "if (1)",
           "pytest:test_continue_mode_keeps_game_running"),
    Mutant("5", "port/windows/src/win32_ue_bridge.c",
           "GetTickCount() - start < UE_BRIDGE_DUMP_WAIT_MS", "(GetTickCount() - start < UE_BRIDGE_DUMP_WAIT_MS || 1)",
           "pytest:test_crash_wait_is_capped_when_ue_ignores_it"),
    Mutant("5", "port/windows/src/win32_ue_bridge.c",
           "previous_filter = SetUnhandledExceptionFilter(bridge_crash_filter);", "(void)bridge_crash_filter;",
           "pytest:test_crash_self_dumps_when_no_ue"),
    Mutant("5", "port/windows/src/win32_ue_bridge.c",
           "write_self_dump(dump_exception, dump_header, dump_crashing_thread);", ";",
           "pytest:test_crash_self_dumps_when_no_ue"),
    Mutant("5", "port/windows/src/win32_ue_bridge.c",
           "view->debugger_attached = ueb_load_u32(&header->ue_debugger_attached) != 0;", "view->debugger_attached = 0;",
           "pytest:test_no_hang_action_while_ue_debugger_flag_set"),
    Mutant("5", "port/windows/src/win32_ue_bridge.c",
           "view->is_editor = ueb_load_u32(&header->ue_is_editor) != 0;", "view->is_editor = 0;",
           "pytest:test_game_never_kills_hung_editor_ue"),
    Mutant("5", "port/windows/src/win32_ue_bridge.c",
           "view->hang_timeout_ms = ueb_load_u32(&header->ue_hang_timeout_ms);", "view->hang_timeout_ms = 0;",
           "pytest:test_game_kills_hung_standalone_ue"),
    Mutant("5", "port/windows/src/win32_ue_bridge.c",
           "view->crashing = view->stopping == UE_BRIDGE_STOP_CRASH;", "view->crashing = 0;",
           "pytest:test_game_leaves_a_crashing_ue_alone"),
    Mutant("5", "port/windows/src/win32_ue_bridge.c",
           "view->busy = ueb_load_u32(&header->ue_busy) != 0;", "view->busy = 0;",
           "pytest:test_no_hang_action_while_ue_busy"),
    Mutant("5", "port/windows/src/win32_ue_bridge.c",
           "write_header_snapshot(directory, header);", "(void)write_header_snapshot;",
           "pytest:test_game_report_carries_the_header_and_ues_crash_folder"),
    Mutant("5", "port/windows/src/win32_ue_bridge.c",
           "result = WaitForSingleObject(directory_mutex, DIRECTORY_LOCK_WAIT_MS);", "result = WAIT_TIMEOUT;",
           "pytest:test_two_games_are_never_inside_the_directory_lock_together"),
    Mutant("5", "port/windows/src/win32_ue_bridge.c",
           "result = WaitForSingleObject(directory_mutex, DIRECTORY_LOCK_WAIT_MS);", "result = WaitForSingleObject(directory_mutex, INFINITE);",
           "pytest:test_a_hung_directory_lock_holder_delays_game_start_by_the_cap_only"),
    Mutant("5", "port/windows/src/win32_ue_bridge.c",
           "if (result == WAIT_OBJECT_0 || result == WAIT_ABANDONED)", "if (result == WAIT_OBJECT_0)",
           "pytest:test_an_abandoned_directory_lock_is_taken_over_without_waiting"),
    Mutant("5", "port/windows/src/win32_ue_bridge.c",
           "ReleaseMutex(directory_mutex);", ";",
           "pytest:test_two_games_are_never_inside_the_directory_lock_together"),
    Mutant("5", "port/windows/src/win32_ue_bridge.c",
           "DWORD length = GetFullPathNameA(path, sizeof(full), full, NULL);", "DWORD length = (strcpy(full, path), (DWORD)strlen(path));",
           "pytest:test_published_log_path_is_made_absolute"),
    Mutant("5", "port/ue_bridge/ue_bridge_policy.c",
           "return now_ms - clock->since_ms;", "return now_ms > clock->since_ms ? now_ms - clock->since_ms : 0;",
           "policy_crash_clock_is_wrap_safe"),
    Mutant("5", "port/ue_bridge/ue_bridge_policy.c",
           "clock->since_ms = now_ms;", "clock->since_ms = now_ms | 1u;",
           "policy_crash_clock_first_sighting_at_an_even_time_is_zero"),
    Mutant("5", "port/ue_bridge/ue_bridge_policy.c",
           "clock->active = 0;", ";",
           "policy_crash_clock_resets_when_crashing_stops"),
    Mutant("5", "port/ue_bridge/ue_bridge_policy.c",
           "clock->active = 1;", ";",
           "policy_crash_clock_elapsed_time_grows"),
    Mutant("5", "port/windows/src/win32_ue_bridge.c",
           "result = previous_filter ? previous_filter(exception) : EXCEPTION_CONTINUE_SEARCH;", "result = EXCEPTION_CONTINUE_SEARCH; (void)previous_filter;",
           "pytest:test_crash_self_dumps_when_no_ue"),
    Mutant("5", "port/windows/src/win32_ue_bridge.c",
           "TerminateProcess(GetCurrentProcess(), code);", ";",
           "pytest:test_crash_self_dumps_when_no_ue"),
    Mutant("5", "port/windows/src/win32_ue_bridge.c",
           "SetThreadStackGuarantee(&guarantee);", "dump_thread = NULL; (void)guarantee;",
           "pytest:test_stack_overflow_still_writes_the_self_dump"),
    Mutant("5", "port/windows/src/win32_ue_bridge.c",
           "result = WaitForSingleObject(directory_mutex, DIRECTORY_LOCK_WAIT_MS);", "return;",
           "pytest:test_two_games_are_never_inside_the_directory_lock_together"),
    Mutant("5", "port/windows/src/win32_ue_bridge.c",
           "ReleaseMutex(directory_mutex);", ";",
           "pytest:test_a_second_game_starts_without_waiting_for_a_live_first_game"),
    Mutant("10", "port/ue_bridge/tests/roles.c",
           "if (file_exists(exit_when))", "if (file_exists(exit_when) && 0)",
           "pytest:test_exit_when_file_appears"),
    Mutant("10", "port/ue_bridge/tests/roles.c",
           "hanging = 1;", "{ hanging = 1; ue_bridge_publish_frame(++frame, 0.5f); }",
           "pytest:test_hang_when_file_appears"),
    Mutant("10", "port/ue_bridge/tests/roles.c",
           'ended = "exit file";', 'ended = "run time over";',
           "pytest:test_exit_when_file_appears"),
    Mutant("10", "port/ue_bridge/tests/roles.c",
           "if ((crash_after >= 0 && elapsed >= crash_after) || file_exists(crash_when))",
           "if ((crash_after >= 0 && elapsed >= crash_after) || (file_exists(crash_when) && 0))",
           "pytest:test_crash_when_file_appears"),
    Mutant("10", "port/ue_bridge/tests/roles.c",
           "if (!hanging && (hang_after < 0 || elapsed < hang_after))", "if ((hanging || 1) && (hang_after < 0 || elapsed < hang_after))",
           "pytest:test_hang_when_file_appears"),
]


PYTEST_ENV = {**os.environ,
              # a plugin installed in the user's environment (web3's) crashes pytest on start
              "PYTEST_DISABLE_PLUGIN_AUTOLOAD": "1"}

KILLED, SURVIVED, INVALID = "killed", "survived", "invalid"


def run_pytest(test: str) -> subprocess.CompletedProcess:
    try:
        return subprocess.run([sys.executable, "-m", "pytest", "tools/test_ue_bridge.py", "-q", "-x", "-k", test],
                              cwd=ue_bridge_tests.ROOT, capture_output=True, text=True, env=PYTEST_ENV, timeout=300)
    except subprocess.TimeoutExpired as expired:
        # neither a pass nor a failure: reported as an invalid run
        return subprocess.CompletedProcess(expired.cmd, -1, "", "timed out")


def verify_clean_tree(mutants: list[Mutant]) -> None:
    """every pytest-named test must pass on the unmutated tree, or a failure under a mutant proves nothing"""
    for test in sorted({m.killed_by[len("pytest:"):] for m in mutants if m.killed_by.startswith("pytest:")}):
        result = run_pytest(test)
        if result.returncode != 0 or "passed" not in result.stdout:
            raise SystemExit(f"{test} does not pass on the clean tree (exit {result.returncode}); not mutating:\n{result.stdout[-2000:]}")


C_TEST_TIMEOUT_S = 120


def judge(mutant: Mutant) -> tuple[str, str]:
    """(verdict, reason). Killed only by a real test failure: exit 1 with 'failed' in the output. A
    collection or setup error, 'no tests ran' (exit 5), a build failure, a crash and a timeout say
    nothing about the test, so they are invalid."""
    if mutant.killed_by.startswith("pytest:"):
        result = run_pytest(mutant.killed_by[len("pytest:"):])
        if result.returncode == 1 and " failed" in result.stdout:
            return KILLED, ""
        return (SURVIVED, "") if result.returncode == 0 else (INVALID, f"pytest exit {result.returncode}")
    try:
        exe = ue_bridge_tests.build()
    except subprocess.CalledProcessError:
        return INVALID, "does not compile"
    try:
        result = subprocess.run([str(exe), mutant.killed_by], capture_output=True, timeout=C_TEST_TIMEOUT_S)
    except subprocess.TimeoutExpired:
        return INVALID, f"test runner timed out after {C_TEST_TIMEOUT_S}s"
    if result.returncode == 1:
        return KILLED, ""
    return (SURVIVED, "") if result.returncode == 0 else (INVALID, f"test runner exit {result.returncode}")


def outcome(mutant: Mutant) -> tuple[str, str]:
    path = ue_bridge_tests.ROOT / mutant.path
    # bytes, not text: text mode would rewrite line endings, so restoring would not be byte for byte
    data = path.read_bytes()
    original = mutant.original.encode("utf-8")
    if data.count(original) != 1:
        raise RuntimeError(f"{mutant.path}: {mutant.original!r} must occur exactly once")
    try:
        path.write_bytes(data.replace(original, mutant.mutated.encode("utf-8")))
        return judge(mutant)
    finally:
        path.write_bytes(data)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("task", nargs="?")
    parser.add_argument("--path", action="append", default=[])
    args = parser.parse_args()
    paths = {p.replace("\\", "/") for p in args.path}
    selected = [m for m in MUTANTS
                if (args.task is None or m.task == args.task) and (not paths or m.path in paths)]
    if not selected:
        raise SystemExit("no mutant matches the task and path filters")
    verify_clean_tree(selected)
    survivors = []
    invalid = []
    for mutant in selected:
        result, reason = outcome(mutant)
        if result == SURVIVED:
            survivors.append(mutant)
        elif result == INVALID:
            invalid.append((mutant, reason))
    for m in survivors:
        print(f"SURVIVED task {m.task}: {m.path}: {m.original!r} -> {m.mutated!r} ({m.killed_by} still passes)")
    for m, reason in invalid:
        print(f"INVALID task {m.task}: {m.path}: {m.original!r} -> {m.mutated!r} ({m.killed_by}: {reason})")
    print(f"{len(survivors)} survivor(s), {len(invalid)} invalid run(s)")
    return 1 if survivors or invalid else 0


if __name__ == "__main__":
    sys.exit(main())

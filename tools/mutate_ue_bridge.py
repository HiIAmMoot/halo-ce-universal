"""Mutation testing for the UE bridge: every mutant must make its named test
fail. Run after a task's tests pass:

    python tools/mutate_ue_bridge.py [task]

Mutants are single-line replacements (the checkout's line endings vary).
"""

from __future__ import annotations

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
    killed_by: str  # a test-name filter for ue_bridge_tests.exe


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
]


def killed(mutant: Mutant) -> bool:
    path = ue_bridge_tests.ROOT / mutant.path
    text = path.read_text(encoding="utf-8")
    if text.count(mutant.original) != 1:
        raise RuntimeError(f"{mutant.path}: {mutant.original!r} must occur exactly once")
    try:
        path.write_text(text.replace(mutant.original, mutant.mutated), encoding="utf-8")
        try:
            exe = ue_bridge_tests.build()
        except subprocess.CalledProcessError:
            return True  # a mutant that doesn't compile is killed by the build
        return subprocess.run([str(exe), mutant.killed_by], capture_output=True).returncode == 1
    finally:
        path.write_text(text, encoding="utf-8")


def main() -> int:
    task = sys.argv[1] if len(sys.argv) > 1 else None
    survivors = [m for m in MUTANTS if (task is None or m.task == task) and not killed(m)]
    for m in survivors:
        print(f"SURVIVED task {m.task}: {m.path}: {m.original!r} -> {m.mutated!r} ({m.killed_by} still passes)")
    print(f"{len(survivors)} survivor(s)")
    return 1 if survivors else 0


if __name__ == "__main__":
    sys.exit(main())

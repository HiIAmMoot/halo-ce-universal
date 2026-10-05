# The UE bridge

The game can publish itself to the HaloCEUE renderer through shared memory. HaloCEUE is a separate Unreal Engine 5 project and runs on Windows only. The simulation is unchanged: the game writes, and the renderer reads.

## Enabling it

In `config.toml`:

    [ue_bridge]
    enabled = true
    on_peer_exit = "shutdown"   # or "continue"

For a single run, set `HALO_UE_BRIDGE=1` (and optionally `HALO_UE_BRIDGE_ON_PEER_EXIT`) instead. With the bridge off, the game behaves exactly as it does without it.

## What it does

- **Shared memory.** The game creates the directory section `Local\HaloCEUE.Bridge.Directory` and the bridge section `Local\HaloCEUE.Bridge.<pid>.<session id>`; `ue_bridge_format.h` defines both. Writes to the directory are serialized across processes by the named mutex `Local\HaloCEUE.Bridge.DirectoryLock`.
- **Several bridged games.** More than one bridged game can run at once. The directory names the game started last. A renderer stays with the game it attached to until that game stops.
- **What it publishes.**
  - A tick stamp after every tick.
  - A frame stamp and a heartbeat every frame.
  - A "busy" flag while a map loads or a modal message box is open, so neither reads as a hang.
  - The display's refresh rate and the frame-rate target the game's limiter aims for (0: uncapped), so the renderer can cap itself to the same rate.
  - `load_epoch`, bumped on map load, and `state_epoch`, bumped on a checkpoint revert or a saved-game load.
- **Watching the renderer.** Once a renderer attaches, the game watches its process and heartbeat. When the renderer exits, crashes or hangs, the game writes `game_report.txt` and `header.bin` to the session folder, then quits. With `on_peer_exit = "continue"`, it keeps running and waits for a renderer instead.
  - A hung standalone renderer is terminated with exit code `0x48414E47`. It has already dumped itself to `ue_hang.dmp` 2 s before its timeout.
  - An editor is never terminated: it can hold unsaved work.
- **When the game crashes:**
  1. It writes its crash lines to `debug.txt` as always.
  2. It always writes its own `game_crash_self.dmp`. Only this dump holds the exception record and the 32-bit faulting context. It is written from a dedicated thread, so a stack overflow is covered too.
  3. It waits up to 10 s for the renderer's out-of-process `game_crash.dmp`.
  4. It ends itself with the exception code, so the Windows Error Reporting dialog can't keep it, or the renderer, waiting.
- **When the game halts on a fatal error** (`halt_and_catch_fire`): it publishes a crash and stops heartbeating. The renderer then dumps the game and ends it.

Without an attached renderer, the game's report files go next to `debug.txt`.

The bridge's log lines go to stderr and also to the game's `debug.txt`.

The bridge section's name carries the game's PID and a random session id. If a section of that name already exists, another process made it: the bridge doesn't start, a line in `debug.txt` says so, and the game runs without it. For the same reason, don't run the renderer elevated: it trusts the sections the directory names, and an elevated renderer would act on whatever process a lower-privileged squatter pointed it at.

## Files

| File | |
|---|---|
| `ue_bridge_format.h` | the shared layout; both compilers check every offset |
| `ue_bridge_frame_rate.h` | the display's refresh rate and the frame-rate target the game publishes |
| `ue_bridge_ring.c`, `.h` | the lock-free rings and the atomic helpers |
| `ue_bridge_policy.c`, `.h` | what one side does about the other, and the crash clock |
| `../linux/src/ue_bridge.c`, `.h` | the game side's core (sections, directory, publishing) |
| `../linux/src/ue_bridge_game.c` | settings, hooks, start and stop |
| `../linux/src/ue_bridge_platform.h` | what a platform provides |
| `../linux/src/ue_bridge_platform_null.c` | the platform where the bridge never starts (Linux, Android) |
| `../linux/src/ue_bridge_log.c` | `ue_bridge_log`: a line to stderr and to `debug.txt` |
| `../linux/src/ue_bridge_quit.c` | the quit request (an SDL quit event) |
| `../linux/src/ue_bridge_shared.c` | compiles the shared ring and policy into the game |
| `../windows/src/win32_ue_bridge.c` | sections, the directory lock, the watcher, the crash hook, dumps |
| `tests/` | the C tests and the role processes (`ue_bridge_roles.exe`) |

The renderer also compiles `ue_bridge_format.h`, `ue_bridge_ring.c` and `ue_bridge_policy.c`, as C++.

## Tests

    python -m pytest tools/test_ue_bridge.py -v
    python tools/mutate_ue_bridge.py [task]

`HALO_UE_BRIDGE_ASAN_CLANG` adds an AddressSanitizer run. Point it at an x86 `clang.exe` that ships the i386 AddressSanitizer runtime, such as Visual Studio's. The cross-process tests use fixed `Local\` names, so they can't run in parallel, and no bridged game may be running while they do.

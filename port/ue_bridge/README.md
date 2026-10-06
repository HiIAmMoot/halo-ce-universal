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

## Version 3 (M2a)

Format 3 adds the world: the loaded map's geometry in a load region, the objects in the tick slots, and the game's camera in the frame slots.

- **Section size.** `ue_bridge.section_mb` (`HALO_UE_BRIDGE_SECTION_MB`, default 96, 16 to 1024) sizes the whole section. The geometry of the loaded map must fit in it; when it doesn't, the export is marked incomplete (`export_complete` 0, the root's `missing` bits) and the game's log says to raise the setting.
- **Layout.** The header (4 KB), then the tick ring (4 slots of 2 MB), the frame ring (8 slots of 4 KB), and the load region, which takes the rest. `ue_bridge_layout_compute` lays it out; the game uses its own copy and never reads the layout back from the header.
- **The load region.** It starts with the root: the map name, the game's own limits (nodes per model, regions per model, permutations per region, regions per object), and tables by offset and count: definitions, models and one entry per scenario BSP. Models hold nodes, regions, geometries and the shader tags; a BSP entry holds one batch per cluster and shader. Every table is validated by offset on the renderer's side.
- **Export.** The map's definitions and models are exported when the map starts, with the first BSP. Every other BSP is appended into its entry the first time the game loads it, and its `ready` flag is set last. The load sequence is odd while the game writes, and the renderer copies the region under it.
- **Tick records.** One record per object, in ascending absolute index: definition, flags (at rest, hidden), the permutation of each region, and one matrix per node (scale, forward, left, up, position, in world units). `active_bsp` is the scenario's BSP index, -1 for none.
- **The frame camera.** The frame slot carries window 0's render camera: position, forward, up, vertical field of view in radians, near and far planes, and `camera_valid` (0 when no window camera was drawn).
- **The load hold.** For a local game with a renderer attached, the game holds at a map start until the renderer writes `ue_ready` equal to `load_epoch`, at most 10 s (`UE_BRIDGE_LOAD_HOLD_MS`). A reader that exits or hangs ends the hold at once. The log says `hold ended: ready after N ms`.
- **Start map.** `ue_bridge.start_map` (`HALO_UE_BRIDGE_START_MAP`) names the scenario to start instead of the main menu, as the `map_name` command takes it (`levels\a10\a10`). It overrides what `init.txt` did, and nothing is written to `init.txt`.
- **Roles for the tests.** `read-world` prints the live bridge's export and latest tick and frame as JSON, including `ue_ready` (`tools/test_ue_bridge_maps.py`). `fake-world` is `fake-game` with a synthetic map exported through the shared writers (one quad model, two definitions, one BSP), a tick every 33 ms with two objects, and a camera frame every 16 ms; HaloCEUE's `Scripts/world_test.py` runs UE against it. `fake-ue --ready-after-ms N` writes `ue_ready` for each load epoch once the export for it is complete.

## Version 4 (M2a fix wave)

- **Header.** `game_truncated_ticks` counts ticks published with `UE_BRIDGE_TICK_TRUNCATED` since the game started (the objects past the cut are not written). The game logs the first one of each load epoch.
- **Load root.** `repaired_vertices` and `clamped_node_counts` are what the export saw in the exported models that the renderer repairs on its side, summed over models.
- **Order.** The map's first tick (the placed objects) is published before the load sequence goes even, so a renderer that reads the map always finds a tick of its epoch.
- **Limits.** A map with more definitions than a 16-bit record holds, or more models than a definition's 16-bit model index holds, gets no definition or model table: the export is incomplete, counted and logged. Every shared allocation is sized by `ue_bridge_padded_array_bytes`, which fails instead of wrapping.

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

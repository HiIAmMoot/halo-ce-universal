/*
UE_BRIDGE.H

The game side of the bridge to the HaloCEUE renderer (port/ue_bridge, and the
Phase 0 design in the HaloCEUE repository): the bridge section and the
directory entry, the tick and frame rings, the epochs and the heartbeat.
ue_bridge_game.c starts it when config.toml's ue_bridge.enabled is true. The
operating system comes in through a table (ue_bridge_platform.h on the game,
a fake in port/ue_bridge/tests), so this file has no platform calls.
*/

#ifndef UE_BRIDGE_H
#define UE_BRIDGE_H

#include <stdint.h>

#include "../../ue_bridge/ue_bridge_format.h"
#include "../../ue_bridge/ue_bridge_load.h"
#include "../../ue_bridge/ue_bridge_tick.h"

struct ue_bridge_os
{
	/* maps the named read-write section of size bytes, creating it zero-filled
	when it doesn't exist (an existing one keeps its contents); NULL on
	failure. *handle receives what unmap_section takes. */
	void *(*map_section)(const char *name, uint32_t size, void **handle);
	void (*unmap_section)(void *view, void *handle);
	uint64_t (*qpc)(void);
	uint64_t (*qpc_frequency)(void);
	uint32_t (*pid)(void);
	uint64_t (*random64)(void);
	int (*debugger_present)(void);
	void (*log)(const char *message);
	/* the platform's path encoding to UTF-8 (the header's); NULL when paths
	already are UTF-8. A converted path that doesn't fit capacity yields an
	empty string, never a truncated one (a cut can split a UTF-8 sequence). */
	void (*path_to_utf8)(const char *path, char *utf8, uint32_t capacity);
	/* optional (NULL: this is the only bridged game, as in the tests): one
	machine-wide lock around every write of the directory entry. Several
	bridged games can run on one machine (system link on one PC), so the
	entry's seqlock needs a single writer at a time. The entry names the last
	game that started; an earlier game's stop leaves it alone. */
	void (*lock_directory)(void);
	void (*unlock_directory)(void);
	/* optional (NULL: map_section): as map_section, but NULL when the section
	already exists. The bridge section's name carries the game's PID and a random
	session id, so one that exists was made by another process, and the game must
	not publish into it. The directory is shared by design and keeps map_section. */
	void *(*map_new_section)(const char *name, uint32_t size, void **handle);
};

struct ue_bridge_settings
{
	int enabled;
	/* in the platform's path encoding; NULL for none */
	const char *log_path;
	uint32_t max_objects;
	/* bytes, a multiple of 4096 */
	uint32_t section_size;
	uint32_t tick_slot_size;
};

/* 1 when the bridge is active (already, or now). The bridge keeps the os pointer, so
the os table must outlive it (until ue_bridge_stop returns); settings and its
log_path are copied. */
int ue_bridge_start(const struct ue_bridge_settings *settings, const struct ue_bridge_os *os);
/* publishes stopping (a UE_BRIDGE_STOP_*), withdraws this game's directory
entry and unmaps; UE keeps reading its own mapping of the section. A caller
that runs a watcher thread (ue_bridge_platform_start_watcher) stops it first:
the watcher reads the header on every pass and must not find it unmapped. */
void ue_bridge_stop(uint32_t stopping);
int ue_bridge_active(void);
uint64_t ue_bridge_session_id(void);
/* NULL when inactive */
volatile struct ue_bridge_header *ue_bridge_header(void);
/* the bridge section's first byte and size; NULL and 0 when inactive */
volatile uint8_t *ue_bridge_section(void);
uint32_t ue_bridge_section_size(void);
/* the ring and region geometry this game laid out, from its own settings; never
read back from the header, which UE can write. NULL when inactive. */
const struct ue_bridge_layout *ue_bridge_trusted_layout(void);

/* an empty tick: no records */
void ue_bridge_publish_tick(uint64_t tick);
void ue_bridge_publish_frame(uint64_t frame, float interpolation_fraction);

/* the next tick slot, begun: add records with ue_bridge_tick_writer_add,
then ue_bridge_tick_end publishes it; NULL when inactive */
struct ue_bridge_tick_writer *ue_bridge_tick_begin(uint64_t tick);
void ue_bridge_tick_end(int16_t active_bsp);

/* window 0's render camera, in world units and radians */
struct ue_bridge_camera
{
	float position[3];
	float forward[3];
	float up[3];
	float vertical_fov;
	float z_near;
	float z_far;
};

/* camera NULL: no window camera was drawn this frame */
void ue_bridge_publish_frame_camera(uint64_t frame, float interpolation_fraction, uint64_t tick, const struct ue_bridge_camera *camera);

/* the load handshake (Phase 0 design, section 5.6) */
#define UE_BRIDGE_LOAD_HOLD_MS 10000u

enum ue_bridge_hold
{
	UE_BRIDGE_HOLD_NONE = 0,
	UE_BRIDGE_HOLD_WAITING,
	UE_BRIDGE_HOLD_READY,
	UE_BRIDGE_HOLD_TIMED_OUT,
	UE_BRIDGE_HOLD_READER_GONE
};

/* 1 when a renderer is attached, not stopping, and heartbeating within its
own published hang timeout (or under a debugger) */
int ue_bridge_reader_present(void);
/* the watcher's word that the renderer's process has ended (or that a new one
attached: 0); ends a hold at once and refuses a new one. Any thread. */
void ue_bridge_note_peer_exited(int exited);
/* holds for the current load_epoch when a reader is present; 1 when holding */
int ue_bridge_hold_begin(void);
/* WAITING while the hold lasts; once it ends, why (game_holding cleared),
and NONE after that. *held_ms receives the time held so far. */
enum ue_bridge_hold ue_bridge_hold_poll(uint32_t *held_ms);
/* the display's refresh rate and the frame rate the game aims for (0:
uncapped), as ue_bridge_frame_rate.h computes them; stored as given */
void ue_bridge_publish_frame_rate(uint32_t refresh_hz, uint32_t target_hz);
void ue_bridge_heartbeat(void);
void ue_bridge_bump_load_epoch(void);
void ue_bridge_bump_state_epoch(void);
/* game_busy: 1 while the main loop is legitimately stalled (a map load), so
the renderer doesn't take the silence for a hang */
void ue_bridge_set_busy(int busy);
/* game_stopping (a UE_BRIDGE_STOP_*) while the bridge stays mapped: the game
is dying or halted but its process lives on, so ue_bridge_stop isn't called */
void ue_bridge_publish_stopping(uint32_t stopping);

/* the load region's writer for a whole-map export, from the region's start:
load_sequence goes odd and the root is cleared; NULL when inactive */
struct ue_bridge_load_writer *ue_bridge_load_begin(void);
/* publishes the export for the current load_epoch: export_complete,
export_epoch, then load_sequence even */
void ue_bridge_load_end(int complete);
/* the same writer, after what the map export wrote, for a BSP appended on
its first load; NULL when no export of the current load_epoch has ended */
struct ue_bridge_load_writer *ue_bridge_load_append(void);
/* the region's root; NULL when inactive */
struct ue_bridge_load_root *ue_bridge_load_root(void);
/* records where the game put the BSP table (the root's bsps fields are for
UE: the game never reads them back); a table outside the region is refused */
void ue_bridge_load_set_bsp_table(uint32_t offset, uint32_t count);
/* the entry at index of the recorded table; NULL past it */
struct ue_bridge_bsp_entry *ue_bridge_load_bsp_slot(uint32_t index);
/* marks the BSP's entry ready, after its data; complete 0 also clears
export_complete (the BSP didn't fit) */
void ue_bridge_load_publish_bsp(short structure_bsp_index, int complete);

#endif

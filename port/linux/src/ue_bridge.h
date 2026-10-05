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

void ue_bridge_publish_tick(uint64_t tick);
void ue_bridge_publish_frame(uint64_t frame, float interpolation_fraction);
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

#endif

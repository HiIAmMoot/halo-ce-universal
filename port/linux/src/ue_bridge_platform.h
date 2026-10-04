/*
UE_BRIDGE_PLATFORM.H

What the UE bridge (ue_bridge.c, ue_bridge_game.c) needs from the platform.
Windows has it (port/windows/src/win32_ue_bridge.c); everywhere else
ue_bridge_platform_null.c says there is no bridge.
*/

#ifndef UE_BRIDGE_PLATFORM_H
#define UE_BRIDGE_PLATFORM_H

#include "ue_bridge.h"

struct ue_bridge_watch_config
{
	/* asks the game to quit through its normal path; called on the watcher thread */
	void (*request_quit)(void);
	int continue_on_peer_exit;
};

/* ue_bridge_log.c: a line to stderr and to the game's debug.txt (no newline in
format); not for the crash filter */
void ue_bridge_log(const char *format, ...)
#if defined(__GNUC__)
	__attribute__((format(printf, 1, 2)))
#endif
	;

/* NULL where the bridge isn't available */
const struct ue_bridge_os *ue_bridge_platform_os(void);
/* puts the bridge's unhandled-exception filter in front of the game's own */
void ue_bridge_platform_install_crash_hook(void);
/* 1 when the thread watching the renderer runs */
int ue_bridge_platform_start_watcher(const struct ue_bridge_watch_config *config);
void ue_bridge_platform_stop_watcher(void);

/* ue_bridge_quit.c */
void ue_bridge_request_quit(void);

/* ue_bridge_game.c: the hooks (game sources see these through
halo_linux_source_fixups.h too) */
void ue_bridge_game_pump(void);
void ue_bridge_game_loading(int loading);
void ue_bridge_game_tick(long tick);
void ue_bridge_game_frame_begin(long frame, float interpolation_fraction);
void ue_bridge_game_map_loaded(void);
void ue_bridge_game_state_loaded(void);
/* the game halted on a fatal error (halt_and_catch_fire) and still presents
frames: stops the heartbeat and publishes a crash */
void ue_bridge_game_halted(void);
/* stops the bridge and the watcher (the exit handler calls it) */
void ue_bridge_game_shutdown(void);

#endif

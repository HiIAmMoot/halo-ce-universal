/*
UE_BRIDGE_GAME.C

The game's use of the UE bridge (ue_bridge.h): started by the first hook when
config.toml's ue_bridge.enabled is true, fed by the port's tick, frame and map
functions (render_interpolation.c) and by the game state's after-load procs
(game_state.c), and stopped at exit.
*/

#include "port_config.h"
#include "ue_bridge.h"
#include "ue_bridge_platform.h"
#include "../include/halo_port_capacity.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* platform.h's; declared here so the tests can link this file without the platform layer */
const char *platform_data_root(void);

static int start_attempted;
static int started;
/* a halted game keeps presenting frames, and so keeps pumping; its heartbeat
must go stale so the renderer sees it is dead */
static int halted;
static int exit_handler_registered;
static char log_path[1024];
static int frame_rate_published;
static uint32_t published_refresh_hz;
static uint32_t published_target_hz;

static void at_exit(void)
{
	ue_bridge_game_shutdown();
}

static void ensure_started(void)
{
	struct ue_bridge_settings settings;
	struct ue_bridge_watch_config watch;
	const struct ue_bridge_os *os;
	const char *on_peer_exit;

	if (start_attempted)
		return;
	start_attempted = 1;
	if (!config_boolean("ue_bridge.enabled"))
		return;
	os = ue_bridge_platform_os();
	if (!os)
		return;
	snprintf(log_path, sizeof(log_path), "%s/debug.txt", platform_data_root());
	settings.enabled = 1;
	settings.log_path = log_path;
	settings.max_objects = HALO_PORT_MAXIMUM_OBJECTS_PER_MAP;
	on_peer_exit = config_string("ue_bridge.on_peer_exit");
	watch.request_quit = ue_bridge_request_quit;
	watch.continue_on_peer_exit = strcmp(on_peer_exit, "continue") == 0;
	if (!watch.continue_on_peer_exit && strcmp(on_peer_exit, "shutdown") != 0)
		ue_bridge_log("ue bridge: ue_bridge.on_peer_exit \"%s\" is neither \"shutdown\" nor \"continue\"; using \"shutdown\"", on_peer_exit);
	if (!ue_bridge_start(&settings, os))
		return;
	ue_bridge_platform_install_crash_hook();
	if (!ue_bridge_platform_start_watcher(&watch))
	{
		ue_bridge_log("ue bridge: cannot start the watcher thread; the bridge is off");
		ue_bridge_stop(UE_BRIDGE_STOP_EXIT);
		return;
	}
	started = 1;
	frame_rate_published = 0;
	ue_bridge_game_frame_rate_poll();
	if (!exit_handler_registered)
	{
		exit_handler_registered = 1;
		atexit(at_exit);
	}
	ue_bridge_log("ue bridge: on, session %016llx", (unsigned long long)ue_bridge_session_id());
}

void ue_bridge_game_shutdown(void)
{
	if (started)
	{
		/* a halted game that later exits (halt re-entered, quit event) must keep its CRASH */
		int stopping = halted ? UE_BRIDGE_STOP_CRASH : UE_BRIDGE_STOP_EXIT;

		started = 0;
		halted = 0;
		/* the watcher first: it reads the header on every pass, and ue_bridge_stop unmaps it */
		ue_bridge_platform_stop_watcher();
		ue_bridge_stop(stopping);
	}
	start_attempted = 0;
}

void ue_bridge_game_frame_rate_poll(void)
{
	float refresh;
	uint32_t refresh_hz, target_hz;

	if (!started)
		return;
	platform_frame_rate(&refresh, &target_hz);
	refresh_hz = (uint32_t)(refresh + 0.5f);
	if (frame_rate_published && refresh_hz == published_refresh_hz && target_hz == published_target_hz)
		return;
	frame_rate_published = 1;
	published_refresh_hz = refresh_hz;
	published_target_hz = target_hz;
	ue_bridge_publish_frame_rate(refresh_hz, target_hz);
	if (target_hz)
		ue_bridge_log("ue bridge: frame rate target %lu Hz, display %lu Hz", (unsigned long)target_hz, (unsigned long)refresh_hz);
	else
		ue_bridge_log("ue bridge: frame rate uncapped, display %lu Hz", (unsigned long)refresh_hz);
}

/* every main-loop pass (platform_pump_events): menus and pauses included */
void ue_bridge_game_pump(void)
{
	ensure_started();
	if (started && !halted)
		ue_bridge_heartbeat();
}

/* around a map load (main_new_map), when the main loop doesn't pass for seconds */
void ue_bridge_game_loading(int loading)
{
	ensure_started();
	if (started)
		ue_bridge_set_busy(loading);
}

/* around a modal dialog (SDL_ShowSimpleMessageBox runs its own message loop
inside platform_pump_events): the heartbeat stops while the player reads it,
and the renderer would otherwise dump and terminate the game as hung */
void ue_bridge_game_modal(int open)
{
	ensure_started();
	if (started)
		ue_bridge_set_busy(open);
}

void ue_bridge_game_tick(long tick)
{
	ensure_started();
	if (started)
		ue_bridge_publish_tick((uint64_t)tick);
}

void ue_bridge_game_frame_begin(long frame, float interpolation_fraction)
{
	ensure_started();
	if (started)
	{
		if (!halted)
			ue_bridge_heartbeat();
		ue_bridge_publish_frame((uint64_t)frame, interpolation_fraction);
	}
}

void ue_bridge_game_map_loaded(void)
{
	ensure_started();
	if (started)
		ue_bridge_bump_load_epoch();
}

/* halt_and_catch_fire (main.c): the busy flag would otherwise excuse the
silence for ever after a failed load */
void ue_bridge_game_halted(void)
{
	if (!started || halted)
		return;
	halted = 1;
	ue_bridge_set_busy(0);
	ue_bridge_publish_stopping(UE_BRIDGE_STOP_CRASH);
}

/* a checkpoint revert or a saved game: game_state.c's after-load procs */
void ue_bridge_game_state_loaded(void)
{
	if (started)
		ue_bridge_bump_state_epoch();
}

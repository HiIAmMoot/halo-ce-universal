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
#include "ue_bridge_world.h"
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
static long last_tick;
static int time_held;
static struct ue_bridge_camera frame_camera;
static int frame_camera_valid;

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
	long section_mb;

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
	section_mb = config_integer("ue_bridge.section_mb");
	if (section_mb < (long)UE_BRIDGE_MIN_SECTION_MB || section_mb > (long)(UE_BRIDGE_MAX_SECTION_SIZE >> 20))
	{
		ue_bridge_log("ue bridge: ue_bridge.section_mb %ld is outside %lu to %lu; using %lu",
			section_mb, (unsigned long)UE_BRIDGE_MIN_SECTION_MB, (unsigned long)(UE_BRIDGE_MAX_SECTION_SIZE >> 20), (unsigned long)UE_BRIDGE_DEFAULT_SECTION_MB);
		section_mb = UE_BRIDGE_DEFAULT_SECTION_MB;
	}
	settings.section_size = (uint32_t)section_mb << 20;
	settings.tick_slot_size = UE_BRIDGE_TICK_SLOT_SIZE;
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
	ue_bridge_log("ue bridge: section %lu MB; largest free address block after it %lu MB",
		(unsigned long)(settings.section_size >> 20), (unsigned long)(ue_bridge_platform_largest_free_block() >> 20));
}

void ue_bridge_game_shutdown(void)
{
	if (started)
	{
		/* a halted game that later exits (halt re-entered, quit event) must keep its CRASH */
		int stopping = halted ? UE_BRIDGE_STOP_CRASH : UE_BRIDGE_STOP_EXIT;

		started = 0;
		halted = 0;
		last_tick = 0;
		time_held = 0;
		frame_camera_valid = 0;
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

static void end_hold(void)
{
	static const char *const reasons[] = { "", "", "ready", "timed out", "the renderer went away" };
	uint32_t held_ms = 0;
	enum ue_bridge_hold state = ue_bridge_hold_poll(&held_ms);

	if (state == UE_BRIDGE_HOLD_WAITING)
	{
		/* main_new_map can switch BSP after the map's initialization, and
		scenario_switch_structure_bsp ends with main_start_time: stop time
		again on every pass, or that switch would end the hold early */
		ue_bridge_world_stop_time();
		return;
	}
	if (state == UE_BRIDGE_HOLD_NONE)
		return;
	time_held = 0;
	ue_bridge_world_start_time();
	ue_bridge_log("ue bridge: hold ended: %s after %lu ms", reasons[state], (unsigned long)held_ms);
}

/* every main-loop pass (platform_pump_events): menus and pauses included */
void ue_bridge_game_pump(void)
{
	ensure_started();
	if (started && !halted)
		ue_bridge_heartbeat();
	if (started && time_held)
		end_hold();
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
	last_tick = tick;
	if (started)
		ue_bridge_world_publish_tick((uint64_t)tick);
}

void ue_bridge_game_frame_begin(long frame, float interpolation_fraction)
{
	(void)frame;
	(void)interpolation_fraction;
	ensure_started();
	/* (two tests, not one: a mutant in tools/mutate_ue_bridge.py names the halted guard, and
	`if (!started || halted)` already occurs in ue_bridge_game_halted) */
	if (!started)
		return;
	if (!halted)
		ue_bridge_heartbeat();
}

/* main_game_render, once a window's camera is set (main.c); split screen:
player 1's window only (Phase 0 design, section 8) */
void ue_bridge_game_window_camera(long window_index, struct render_camera const *camera)
{
	if (started && window_index == 0)
		frame_camera_valid = ue_bridge_world_camera(camera, &frame_camera);
}

/* render_interpolation_frame_end: the frame's render state is complete */
void ue_bridge_game_frame_end(long frame, long tick, float interpolation_fraction)
{
	if (!started)
		return;
	ue_bridge_publish_frame_camera((uint64_t)frame, interpolation_fraction, (uint64_t)tick, frame_camera_valid ? &frame_camera : NULL);
	frame_camera_valid = 0;
}

void ue_bridge_game_map_loaded(void)
{
	ensure_started();
	if (started)
		ue_bridge_bump_load_epoch();
}

/* the end of game_initialize_for_new_map (game.c): every map start, the
main menu's, a reset's and a network game's included */
void ue_bridge_game_map_ready(void)
{
	ensure_started();
	if (!started)
		return;
	ue_bridge_world_export_map();
	/* the objects as placed: UE builds the meshes they need before it says ready */
	ue_bridge_world_publish_tick((uint64_t)last_tick);
	if (!time_held && ue_bridge_world_hold_allowed() && ue_bridge_hold_begin())
	{
		time_held = 1;
		ue_bridge_world_stop_time();
		ue_bridge_log("ue bridge: holding the map start for the renderer (at most %lu ms)", (unsigned long)UE_BRIDGE_LOAD_HOLD_MS);
	}
}

/* scenario_switch_structure_bsp (scenario.c), once the BSP is loaded */
void ue_bridge_game_structure_bsp_loaded(short structure_bsp_index)
{
	if (started)
		ue_bridge_world_export_bsp(structure_bsp_index);
}

/* main_loop, after console_startup has run init.txt: a start map from the
settings overrides it, so a launcher never has to write the owner's init.txt */
void ue_bridge_game_console_started(void)
{
	const char *map;

	ensure_started();
	if (!started)
		return;
	map = config_string("ue_bridge.start_map");
	if (map && map[0])
	{
		ue_bridge_log("ue bridge: starting %s (ue_bridge.start_map)", map);
		ue_bridge_world_set_start_map(map);
	}
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

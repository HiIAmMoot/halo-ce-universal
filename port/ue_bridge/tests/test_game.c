/*
TEST_GAME.C

The game adapter (port/linux/src/ue_bridge_game.c) with its settings, its
platform layer and the core's operating system faked.
*/

#include "ueb_test.h"
#include "../../linux/src/ue_bridge_platform.h"
#include "ue_bridge_ring.h"

#include <stdarg.h>
#include <string.h>

void ue_bridge_game_pump(void);
void ue_bridge_game_loading(int loading);
void ue_bridge_game_tick(long tick);
void ue_bridge_game_frame_begin(long frame, float interpolation_fraction);
void ue_bridge_game_map_loaded(void);
void ue_bridge_game_state_loaded(void);
void ue_bridge_game_shutdown(void);
void ue_bridge_game_halted(void);

/* ---------- the settings and the platform layer the adapter calls */

static int setting_enabled;
static const char *setting_on_peer_exit;
/* every line logged since the last reset */
static char log_lines[2048];

int config_boolean(const char *name)
{
	return strcmp(name, "ue_bridge.enabled") == 0 && setting_enabled;
}

const char *config_string(const char *name)
{
	return strcmp(name, "ue_bridge.on_peer_exit") == 0 && setting_on_peer_exit ? setting_on_peer_exit : "";
}

void platform_log(const char *format, ...)
{
	char line[512];
	va_list arguments;

	va_start(arguments, format);
	vsnprintf(line, sizeof(line), format, arguments);
	va_end(arguments);
	if (strlen(log_lines) + strlen(line) + 2 <= sizeof(log_lines))
	{
		strcat(log_lines, line);
		strcat(log_lines, "\n");
	}
}

const char *platform_data_root(void)
{
	return "D:/data";
}

static uint64_t game_section[UE_BRIDGE_SECTION_SIZE / 8];
static uint64_t game_directory[UE_BRIDGE_DIRECTORY_SIZE / 8];
static int game_maps;
static uint64_t game_now;

static void *game_map(const char *name, uint32_t size, void **handle)
{
	(void)size;
	game_maps++;
	*handle = 0;
	return strcmp(name, UE_BRIDGE_DIRECTORY_NAME) == 0 ? (void *)game_directory : (void *)game_section;
}

static void game_unmap(void *view, void *handle) { (void)view; (void)handle; }
static uint64_t game_qpc(void) { return game_now; }
static uint64_t game_frequency(void) { return 10000000ull; }
static uint32_t game_pid(void) { return 42; }
static uint64_t game_random64(void) { return 7; }
static int game_debugger_present(void) { return 0; }
static void game_os_log(const char *message) { (void)message; }

static const struct ue_bridge_os game_os =
{
	game_map, game_unmap, game_qpc, game_frequency, game_pid, game_random64, game_debugger_present, game_os_log
};

static int platform_available;
static int platform_os_requests;
static int crash_hooks_installed;
static int watcher_starts;
static int watcher_stops;
static int watcher_start_result;
static struct ue_bridge_watch_config last_watch;

const struct ue_bridge_os *ue_bridge_platform_os(void)
{
	platform_os_requests++;
	return platform_available ? &game_os : 0;
}

void ue_bridge_platform_install_crash_hook(void) { crash_hooks_installed++; }

int ue_bridge_platform_start_watcher(const struct ue_bridge_watch_config *config)
{
	watcher_starts++;
	last_watch = *config;
	return watcher_start_result;
}

void ue_bridge_platform_stop_watcher(void) { watcher_stops++; }

void ue_bridge_request_quit(void) { }

static void game_reset(void)
{
	ue_bridge_game_shutdown();
	/* the core is global: another suite may have left it running on its own fake */
	ue_bridge_stop(UE_BRIDGE_STOP_EXIT);
	setting_enabled = 1;
	setting_on_peer_exit = "shutdown";
	memset(log_lines, 0, sizeof(log_lines));
	memset(game_section, 0, sizeof(game_section));
	memset(game_directory, 0, sizeof(game_directory));
	game_maps = 0;
	game_now = 100;
	platform_available = 1;
	platform_os_requests = 0;
	crash_hooks_installed = 0;
	watcher_starts = 0;
	watcher_stops = 0;
	watcher_start_result = 1;
	memset(&last_watch, 0, sizeof(last_watch));
}

static volatile struct ue_bridge_header *section_header(void)
{
	return (volatile struct ue_bridge_header *)game_section;
}

/* ---------- tests */

static void game_disabled_starts_nothing(void)
{
	game_reset();
	setting_enabled = 0;
	ue_bridge_game_frame_begin(1, 0.5f);
	ue_bridge_game_tick(1);
	ue_bridge_game_map_loaded();
	UEB_CHECK(platform_os_requests == 0);
	UEB_CHECK(game_maps == 0);
	UEB_CHECK(crash_hooks_installed == 0);
	UEB_CHECK(watcher_starts == 0);
}

static void game_starts_on_first_hook_with_settings(void)
{
	game_reset();
	ue_bridge_game_frame_begin(1, 0.5f);
	UEB_CHECK(section_header()->magic == UE_BRIDGE_MAGIC);
	UEB_CHECK(strcmp((const char *)section_header()->game_log_path, "D:/data/debug.txt") == 0);
	UEB_CHECK(section_header()->max_objects == 8192);
	UEB_CHECK(crash_hooks_installed == 1);
	UEB_CHECK(watcher_starts == 1);
	UEB_CHECK(last_watch.request_quit == ue_bridge_request_quit);
	UEB_CHECK(last_watch.continue_on_peer_exit == 0);
}

static void game_starts_only_once(void)
{
	game_reset();
	ue_bridge_game_frame_begin(1, 0.5f);
	ue_bridge_game_tick(1);
	ue_bridge_game_map_loaded();
	UEB_CHECK(platform_os_requests == 1);
	UEB_CHECK(crash_hooks_installed == 1);
	UEB_CHECK(watcher_starts == 1);
}

static void game_continue_setting_reaches_watcher(void)
{
	game_reset();
	setting_on_peer_exit = "continue";
	ue_bridge_game_frame_begin(1, 0.5f);
	UEB_CHECK(last_watch.continue_on_peer_exit == 1);
}

static void game_unknown_on_peer_exit_logs_and_shuts_down(void)
{
	game_reset();
	setting_on_peer_exit = "sometimes";
	ue_bridge_game_frame_begin(1, 0.5f);
	UEB_CHECK(last_watch.continue_on_peer_exit == 0);
	UEB_CHECK(strstr(log_lines, "\"sometimes\" is neither") != 0);
}

static void game_no_platform_stays_off(void)
{
	game_reset();
	platform_available = 0;
	ue_bridge_game_frame_begin(1, 0.5f);
	ue_bridge_game_tick(1);
	UEB_CHECK(platform_os_requests == 1);
	UEB_CHECK(game_maps == 0);
	UEB_CHECK(crash_hooks_installed == 0);
	UEB_CHECK(watcher_starts == 0);
}

static void game_watcher_failure_stops_bridge(void)
{
	game_reset();
	watcher_start_result = 0;
	ue_bridge_game_frame_begin(1, 0.5f);
	UEB_CHECK(!ue_bridge_active());
	UEB_CHECK(section_header()->game_stopping == UE_BRIDGE_STOP_EXIT);
	ue_bridge_game_tick(2);
	UEB_CHECK(section_header()->tick_ring.published == 0);
}

static void game_hooks_publish(void)
{
	struct ue_bridge_slot tick;
	struct ue_bridge_frame_slot frame;
	volatile struct ue_bridge_header *header;

	game_reset();
	ue_bridge_game_tick(5);
	header = section_header();
	game_now = 900;
	ue_bridge_game_frame_begin(9, 0.75f);
	ue_bridge_game_map_loaded();
	ue_bridge_game_state_loaded();
	UEB_CHECK(ue_bridge_ring_read_newest((const volatile uint8_t *)header, &header->tick_ring, &tick, sizeof(tick), 0) == UE_BRIDGE_READ_NEWEST);
	UEB_CHECK(tick.id == 5);
	UEB_CHECK(ue_bridge_ring_read_newest((const volatile uint8_t *)header, &header->frame_ring, &frame, sizeof(frame), 0) == UE_BRIDGE_READ_NEWEST);
	UEB_CHECK(frame.slot.id == 9);
	UEB_CHECK(frame.interpolation_fraction == 0.75f);
	UEB_CHECK(header->game_heartbeat_qpc == 900);
	UEB_CHECK(header->load_epoch == 1);
	UEB_CHECK(header->state_epoch == 1);
}

static void game_pump_starts_and_heartbeats(void)
{
	game_reset();
	game_now = 4242;
	ue_bridge_game_pump();
	UEB_CHECK(ue_bridge_active());
	UEB_CHECK(section_header()->game_heartbeat_qpc == 4242);
	game_now = 5000;
	ue_bridge_game_pump();
	UEB_CHECK(section_header()->game_heartbeat_qpc == 5000);
}

static void game_loading_sets_and_clears_busy(void)
{
	game_reset();
	ue_bridge_game_loading(1);
	UEB_CHECK(ue_bridge_active());
	UEB_CHECK(section_header()->game_busy == 1);
	ue_bridge_game_loading(0);
	UEB_CHECK(section_header()->game_busy == 0);
}

static void game_state_loaded_before_start_does_nothing(void)
{
	game_reset();
	ue_bridge_game_state_loaded();
	UEB_CHECK(platform_os_requests == 0);
	UEB_CHECK(game_maps == 0);
}

static void game_shutdown_stops_watcher_and_bridge(void)
{
	game_reset();
	ue_bridge_game_frame_begin(1, 0.5f);
	ue_bridge_game_shutdown();
	UEB_CHECK(watcher_stops == 1);
	UEB_CHECK(!ue_bridge_active());
	UEB_CHECK(section_header()->game_stopping == UE_BRIDGE_STOP_EXIT);
	ue_bridge_game_shutdown();
	UEB_CHECK(watcher_stops == 1);
}

static void game_halted_stops_heartbeat_and_publishes_crash(void)
{
	volatile struct ue_bridge_header *header;

	game_reset();
	game_now = 300;
	ue_bridge_game_pump();
	ue_bridge_game_loading(1);
	header = section_header();
	UEB_CHECK(header->game_busy == 1);
	ue_bridge_game_halted();
	UEB_CHECK(header->game_busy == 0);
	UEB_CHECK(header->game_stopping == UE_BRIDGE_STOP_CRASH);
	UEB_CHECK(header->game_heartbeat_qpc == 300);
	game_now = 800;
	ue_bridge_game_pump();
	ue_bridge_game_frame_begin(2, 0.5f);
	UEB_CHECK(header->game_heartbeat_qpc == 300);
}

static void game_halted_before_start_does_nothing(void)
{
	game_reset();
	ue_bridge_game_halted();
	UEB_CHECK(platform_os_requests == 0);
	UEB_CHECK(game_maps == 0);
}

static void game_shutdown_after_halt_keeps_the_published_crash(void)
{
	game_reset();
	ue_bridge_game_pump();
	ue_bridge_game_halted();
	ue_bridge_game_shutdown();
	UEB_CHECK(section_header()->game_stopping == UE_BRIDGE_STOP_CRASH);
}

static void game_shutdown_after_halt_beats_again_on_restart(void)
{
	game_reset();
	ue_bridge_game_pump();
	ue_bridge_game_halted();
	ue_bridge_game_shutdown();
	game_now = 900;
	ue_bridge_game_pump();
	game_now = 1000;
	ue_bridge_game_pump();
	UEB_CHECK(section_header()->game_heartbeat_qpc == 1000);
}

const struct ueb_test ueb_game_tests[] =
{
	{ "game_disabled_starts_nothing", game_disabled_starts_nothing },
	{ "game_starts_on_first_hook_with_settings", game_starts_on_first_hook_with_settings },
	{ "game_starts_only_once", game_starts_only_once },
	{ "game_continue_setting_reaches_watcher", game_continue_setting_reaches_watcher },
	{ "game_unknown_on_peer_exit_logs_and_shuts_down", game_unknown_on_peer_exit_logs_and_shuts_down },
	{ "game_no_platform_stays_off", game_no_platform_stays_off },
	{ "game_watcher_failure_stops_bridge", game_watcher_failure_stops_bridge },
	{ "game_hooks_publish", game_hooks_publish },
	{ "game_pump_starts_and_heartbeats", game_pump_starts_and_heartbeats },
	{ "game_loading_sets_and_clears_busy", game_loading_sets_and_clears_busy },
	{ "game_state_loaded_before_start_does_nothing", game_state_loaded_before_start_does_nothing },
	{ "game_shutdown_stops_watcher_and_bridge", game_shutdown_stops_watcher_and_bridge },
	{ "game_halted_stops_heartbeat_and_publishes_crash", game_halted_stops_heartbeat_and_publishes_crash },
	{ "game_halted_before_start_does_nothing", game_halted_before_start_does_nothing },
	{ "game_shutdown_after_halt_keeps_the_published_crash", game_shutdown_after_halt_keeps_the_published_crash },
	{ "game_shutdown_after_halt_beats_again_on_restart", game_shutdown_after_halt_beats_again_on_restart },
	{ 0, 0 }
};

/*
TEST_GAME.C

The game adapter (port/linux/src/ue_bridge_game.c) with its settings, its
platform layer and the core's operating system faked.
*/

#include "ueb_test.h"
#include "../../linux/src/ue_bridge_platform.h"
#include "../../linux/src/ue_bridge_world.h"
#include "ue_bridge_ring.h"

#include <stdarg.h>
#include <string.h>

void ue_bridge_game_pump(void);
void ue_bridge_game_loading(int loading);
void ue_bridge_game_modal(int open);
void ue_bridge_game_tick(long tick);
void ue_bridge_game_frame_begin(long frame, float interpolation_fraction);
void ue_bridge_game_map_loaded(void);
void ue_bridge_game_state_loaded(void);
void ue_bridge_game_shutdown(void);
void ue_bridge_game_halted(void);
void ue_bridge_game_frame_rate_poll(void);

/* ---------- the settings and the platform layer the adapter calls */

static int setting_enabled;
static const char *setting_start_map;
static int world_map_exports;
static int world_bsp_exports;
static short world_last_bsp;
static char world_start_map[64];
static int world_start_map_calls;
static long setting_section_mb;
static const char *setting_on_peer_exit;
/* every line logged since the last reset */
static char log_lines[2048];
/* what reached errors.c's write_to_error_file, the game's debug.txt, and whether it asked for the date */
static char debug_lines[2048];
static int debug_dated = 1;

int config_boolean(const char *name)
{
	return strcmp(name, "ue_bridge.enabled") == 0 && setting_enabled;
}

long config_integer(const char *name)
{
	return strcmp(name, "ue_bridge.section_mb") == 0 ? setting_section_mb : 0;
}

const char *config_string(const char *name)
{
	if (strcmp(name, "ue_bridge.start_map") == 0)
		return setting_start_map ? setting_start_map : "";
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

void write_to_error_file(char *string, unsigned char date)
{
	if (strlen(debug_lines) + strlen(string) < sizeof(debug_lines))
		strcat(debug_lines, string);
	debug_dated = debug_dated && date;
}

static float platform_refresh_hz;
static uint32_t platform_target_hz;
static int platform_frame_rate_reads;

void platform_frame_rate(float *refresh_hz, uint32_t *target_hz)
{
	platform_frame_rate_reads++;
	*refresh_hz = platform_refresh_hz;
	*target_hz = platform_target_hz;
}

const char *platform_data_root(void)
{
	return "D:/data";
}

static int world_tick_publishes;
static int world_hold_allowed;
static int world_time_stops;
static int world_time_starts;
static struct ue_bridge_camera world_fake_camera;

void ue_bridge_world_publish_tick(uint64_t tick) { (void)tick; world_tick_publishes++; }
int ue_bridge_world_camera(const struct render_camera *camera, struct ue_bridge_camera *out) { (void)camera; *out = world_fake_camera; return 1; }
int ue_bridge_world_hold_allowed(void) { return world_hold_allowed; }
void ue_bridge_world_stop_time(void) { world_time_stops++; }
void ue_bridge_world_start_time(void) { world_time_starts++; }

int ue_bridge_world_export_map(void) { world_map_exports++; return 1; }
void ue_bridge_world_export_bsp(short structure_bsp_index) { world_bsp_exports++; world_last_bsp = structure_bsp_index; }
void ue_bridge_world_set_start_map(const char *scenario_name)
{
	world_start_map_calls++;
	strncpy(world_start_map, scenario_name, sizeof(world_start_map) - 1);
}
unsigned short ue_bridge_export_definition_index(long definition_tag_index) { (void)definition_tag_index; return 0; }

#define TEST_SECTION_SIZE (UE_BRIDGE_MIN_SECTION_MB << 20)

static uint64_t game_section[TEST_SECTION_SIZE / 8];
static uint32_t last_mapped_size;
static uint64_t game_directory[UE_BRIDGE_DIRECTORY_SIZE / 8];
static int game_maps;
static uint64_t game_now;

static void *game_map(const char *name, uint32_t size, void **handle)
{
	game_maps++;
	*handle = 0;
	if (strcmp(name, UE_BRIDGE_DIRECTORY_NAME) == 0)
		return game_directory;
	last_mapped_size = size;
	return game_section;
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

uint32_t ue_bridge_platform_largest_free_block(void) { return 0; }

static void game_reset(void)
{
	ue_bridge_game_shutdown();
	/* the core is global: another suite may have left it running on its own fake */
	ue_bridge_stop(UE_BRIDGE_STOP_EXIT);
	setting_enabled = 1;
	setting_on_peer_exit = "shutdown";
	setting_start_map = 0;
	world_map_exports = 0;
	world_bsp_exports = 0;
	world_last_bsp = 0;
	memset(world_start_map, 0, sizeof(world_start_map));
	world_start_map_calls = 0;
	world_tick_publishes = 0;
	world_hold_allowed = 1;
	world_time_stops = 0;
	world_time_starts = 0;
	memset(&world_fake_camera, 0, sizeof(world_fake_camera));
	memset(log_lines, 0, sizeof(log_lines));
	memset(debug_lines, 0, sizeof(debug_lines));
	debug_dated = 1;
	memset(game_section, 0, sizeof(game_section));
	memset(game_directory, 0, sizeof(game_directory));
	game_maps = 0;
	setting_section_mb = UE_BRIDGE_MIN_SECTION_MB;
	last_mapped_size = 0;
	game_now = 100;
	platform_refresh_hz = 144.0f;
	platform_target_hz = 144;
	platform_frame_rate_reads = 0;
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

static void game_section_size_comes_from_the_setting(void)
{
	game_reset();
	ue_bridge_game_pump();
	UEB_CHECK(last_mapped_size == TEST_SECTION_SIZE);
	UEB_CHECK(section_header()->section_size == TEST_SECTION_SIZE);
	UEB_CHECK(section_header()->load_region.size >= UE_BRIDGE_MIN_LOAD_SIZE);
	UEB_CHECK(section_header()->tick_ring.slot_size == UE_BRIDGE_TICK_SLOT_SIZE);
}

static void game_section_setting_out_of_range_uses_the_default_and_logs(void)
{
	game_reset();
	setting_section_mb = UE_BRIDGE_MIN_SECTION_MB - 1;
	ue_bridge_game_pump();
	UEB_CHECK(strstr(log_lines, "ue_bridge.section_mb") != 0);
	UEB_CHECK(last_mapped_size == UE_BRIDGE_DEFAULT_SECTION_MB << 20);
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

/* the real platform_log is stderr, invisible for a windowed game: the lines must also reach debug.txt */
static void game_log_lines_reach_the_debug_file(void)
{
	char const *line;

	game_reset();
	setting_on_peer_exit = "sometimes";
	ue_bridge_game_frame_begin(1, 0.5f);
	line = strstr(debug_lines, "ue bridge: ue_bridge.on_peer_exit \"sometimes\"");
	UEB_CHECK(line != 0);
	UEB_CHECK(strstr(debug_lines, "ue bridge: on, session ") != 0);
	/* the game's debug.txt lines end in CRLF and carry its timestamp */
	UEB_CHECK(strstr(debug_lines, "using \"shutdown\"\r\n") != 0);
	UEB_CHECK(debug_dated == 1);
	UEB_CHECK(strstr(log_lines, "ue bridge: on, session ") != 0);
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
	struct ue_bridge_frame_slot frame;
	volatile struct ue_bridge_header *header;

	game_reset();
	ue_bridge_game_tick(5);
	header = section_header();
	game_now = 900;
	ue_bridge_game_frame_begin(9, 0.75f);
	ue_bridge_game_frame_end(9, 5, 0.75f);
	ue_bridge_game_map_loaded();
	ue_bridge_game_state_loaded();
	UEB_CHECK(world_tick_publishes == 1);
	UEB_CHECK(ue_bridge_ring_read_newest((const volatile uint8_t *)header, &header->frame_ring, &frame, sizeof(frame), 0) == UE_BRIDGE_READ_NEWEST);
	UEB_CHECK(frame.slot.id == 9);
	UEB_CHECK(frame.interpolation_fraction == 0.75f);
	UEB_CHECK(header->game_heartbeat_qpc == 900);
	UEB_CHECK(header->load_epoch == 1);
	UEB_CHECK(header->state_epoch == 1);
}

static void game_start_publishes_the_frame_rate(void)
{
	game_reset();
	platform_refresh_hz = 59.94f;
	platform_target_hz = 60;
	ue_bridge_game_pump();
	UEB_CHECK(section_header()->game_refresh_hz == 60);
	UEB_CHECK(section_header()->game_frame_target_hz == 60);
	UEB_CHECK(strstr(log_lines, "frame rate target 60 Hz") != 0);
}

static void game_frame_rate_poll_republishes_on_a_change(void)
{
	game_reset();
	ue_bridge_game_pump();
	UEB_CHECK(section_header()->game_frame_target_hz == 144);
	platform_target_hz = 60;
	ue_bridge_game_frame_rate_poll();
	UEB_CHECK(section_header()->game_frame_target_hz == 60);
	platform_target_hz = 0;
	platform_refresh_hz = 75.0f;
	ue_bridge_game_frame_rate_poll();
	UEB_CHECK(section_header()->game_frame_target_hz == 0);
	UEB_CHECK(section_header()->game_refresh_hz == 75);
	platform_refresh_hz = 60.0f;
	ue_bridge_game_frame_rate_poll();
	UEB_CHECK(section_header()->game_refresh_hz == 60);
}

static void game_frame_rate_poll_logs_only_a_change(void)
{
	game_reset();
	ue_bridge_game_pump();
	memset(log_lines, 0, sizeof(log_lines));
	ue_bridge_game_frame_rate_poll();
	ue_bridge_game_frame_rate_poll();
	UEB_CHECK(log_lines[0] == 0);
	platform_target_hz = 0;
	ue_bridge_game_frame_rate_poll();
	UEB_CHECK(strstr(log_lines, "uncapped") != 0);
}

static void game_frame_rate_poll_does_nothing_before_start_or_when_disabled(void)
{
	game_reset();
	setting_enabled = 0;
	ue_bridge_game_frame_rate_poll();
	UEB_CHECK(platform_frame_rate_reads == 0);
	UEB_CHECK(platform_os_requests == 0);
}

static void game_frame_rate_poll_after_a_restart_publishes_again(void)
{
	game_reset();
	ue_bridge_game_pump();
	ue_bridge_game_shutdown();
	memset(game_section, 0, sizeof(game_section));
	ue_bridge_game_pump();
	UEB_CHECK(section_header()->game_frame_target_hz == 144);
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

/* an open modal dialog stops the main loop as a load does: the renderer must see busy, not a hang */
static void game_modal_dialog_sets_and_clears_busy(void)
{
	game_reset();
	ue_bridge_game_pump();
	ue_bridge_game_modal(1);
	UEB_CHECK(section_header()->game_busy == 1);
	ue_bridge_game_modal(0);
	UEB_CHECK(section_header()->game_busy == 0);
}

static void game_modal_dialog_with_the_bridge_off_does_nothing(void)
{
	game_reset();
	setting_enabled = 0;
	ue_bridge_game_modal(1);
	ue_bridge_game_modal(0);
	UEB_CHECK(platform_os_requests == 0);
	UEB_CHECK(game_maps == 0);
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

static void game_map_ready_exports(void)
{
	game_reset();
	ue_bridge_game_map_ready();
	UEB_CHECK(world_map_exports == 1);
}

static void game_map_ready_with_the_bridge_off_exports_nothing(void)
{
	game_reset();
	setting_enabled = 0;
	ue_bridge_game_map_ready();
	UEB_CHECK(world_map_exports == 0);
}

static void game_bsp_loaded_exports_only_once_started(void)
{
	game_reset();
	ue_bridge_game_structure_bsp_loaded(2);
	UEB_CHECK(world_bsp_exports == 0);
	ue_bridge_game_pump();
	ue_bridge_game_structure_bsp_loaded(2);
	UEB_CHECK(world_bsp_exports == 1 && world_last_bsp == 2);
}

static void game_console_started_applies_the_start_map(void)
{
	game_reset();
	setting_start_map = "levels\\a10\\a10";
	ue_bridge_game_console_started();
	UEB_CHECK(strcmp(world_start_map, "levels\\a10\\a10") == 0);
	UEB_CHECK(strstr(log_lines, "ue_bridge.start_map") != 0);
}

static void game_console_started_without_a_start_map_does_nothing(void)
{
	game_reset();
	ue_bridge_game_console_started();
	/* an empty setting must not reach main_set_map_name at all: "" would still cancel the main menu */
	UEB_CHECK(world_start_map_calls == 0);
	game_reset();
	setting_enabled = 0;
	setting_start_map = "levels\\a10\\a10";
	ue_bridge_game_console_started();
	UEB_CHECK(world_start_map_calls == 0);
}

static void present_reader(void)
{
	volatile struct ue_bridge_header *header = section_header();

	header->ue_hang_timeout_ms = 10000;
	header->ue_heartbeat_qpc = game_now;
	header->ue_attached = 1;
}

static void game_map_ready_publishes_a_tick_and_holds_for_a_present_reader(void)
{
	game_reset();
	ue_bridge_game_pump();
	present_reader();
	/* a map's epoch: ue_ready starts at 0 */
	ue_bridge_game_map_loaded();
	ue_bridge_game_map_ready();
	UEB_CHECK(world_map_exports == 1);
	UEB_CHECK(world_tick_publishes == 1);
	UEB_CHECK(world_time_stops == 1);
	UEB_CHECK(section_header()->game_holding == 1);
	ue_bridge_game_pump();
	UEB_CHECK(world_time_starts == 0);
	/* each waiting pass stops time again: a BSP switch's main_start_time must not end the hold */
	UEB_CHECK(world_time_stops == 2);
	section_header()->ue_ready = section_header()->load_epoch;
	ue_bridge_game_pump();
	UEB_CHECK(world_time_starts == 1);
	UEB_CHECK(strstr(log_lines, "hold ended: ready") != 0);
}

static void game_map_ready_without_a_reader_does_not_hold(void)
{
	game_reset();
	ue_bridge_game_map_ready();
	UEB_CHECK(world_time_stops == 0);
	UEB_CHECK(section_header()->game_holding == 0);
}

static void game_map_ready_in_a_network_game_does_not_hold(void)
{
	game_reset();
	ue_bridge_game_pump();
	present_reader();
	world_hold_allowed = 0;
	ue_bridge_game_map_ready();
	UEB_CHECK(world_time_stops == 0);
}

static void game_hold_times_out_and_starts_time(void)
{
	game_reset();
	ue_bridge_game_pump();
	present_reader();
	/* a map's epoch: ue_ready starts at 0 */
	ue_bridge_game_map_loaded();
	ue_bridge_game_map_ready();
	game_now += 101000000ull;
	section_header()->ue_heartbeat_qpc = game_now;
	ue_bridge_game_pump();
	UEB_CHECK(world_time_starts == 1);
	UEB_CHECK(strstr(log_lines, "hold ended: timed out") != 0);
}

static void game_tick_publishes_through_the_world(void)
{
	game_reset();
	ue_bridge_game_tick(5);
	UEB_CHECK(world_tick_publishes == 1);
}

static void game_frame_end_publishes_window_zero_camera(void)
{
	struct ue_bridge_frame_slot frame;
	volatile struct ue_bridge_header *header;

	game_reset();
	ue_bridge_game_pump();
	header = section_header();
	world_fake_camera.position[0] = 4.0f;
	ue_bridge_game_window_camera(1, 0);
	ue_bridge_game_frame_end(3, 20, 0.25f);
	ue_bridge_ring_read_newest((const volatile uint8_t *)header, &header->frame_ring, &frame, sizeof(frame), 0);
	UEB_CHECK(frame.slot.id == 3 && frame.camera_valid == 0);
	ue_bridge_game_window_camera(0, 0);
	ue_bridge_game_frame_end(4, 20, 0.5f);
	ue_bridge_ring_read_newest((const volatile uint8_t *)header, &header->frame_ring, &frame, sizeof(frame), 0);
	UEB_CHECK(frame.slot.id == 4 && frame.camera_valid == 1 && frame.camera_position[0] == 4.0f && frame.tick_id == 20);
	/* a camera is good for the frame it was taken in only */
	ue_bridge_game_frame_end(5, 20, 0.75f);
	ue_bridge_ring_read_newest((const volatile uint8_t *)header, &header->frame_ring, &frame, sizeof(frame), 0);
	UEB_CHECK(frame.slot.id == 5 && frame.camera_valid == 0);
}

const struct ueb_test ueb_game_tests[] =
{
	{ "game_disabled_starts_nothing", game_disabled_starts_nothing },
	{ "game_starts_on_first_hook_with_settings", game_starts_on_first_hook_with_settings },
	{ "game_section_size_comes_from_the_setting", game_section_size_comes_from_the_setting },
	{ "game_section_setting_out_of_range_uses_the_default_and_logs", game_section_setting_out_of_range_uses_the_default_and_logs },
	{ "game_starts_only_once", game_starts_only_once },
	{ "game_continue_setting_reaches_watcher", game_continue_setting_reaches_watcher },
	{ "game_unknown_on_peer_exit_logs_and_shuts_down", game_unknown_on_peer_exit_logs_and_shuts_down },
	{ "game_log_lines_reach_the_debug_file", game_log_lines_reach_the_debug_file },
	{ "game_no_platform_stays_off", game_no_platform_stays_off },
	{ "game_watcher_failure_stops_bridge", game_watcher_failure_stops_bridge },
	{ "game_hooks_publish", game_hooks_publish },
	{ "game_start_publishes_the_frame_rate", game_start_publishes_the_frame_rate },
	{ "game_frame_rate_poll_republishes_on_a_change", game_frame_rate_poll_republishes_on_a_change },
	{ "game_frame_rate_poll_logs_only_a_change", game_frame_rate_poll_logs_only_a_change },
	{ "game_frame_rate_poll_does_nothing_before_start_or_when_disabled", game_frame_rate_poll_does_nothing_before_start_or_when_disabled },
	{ "game_frame_rate_poll_after_a_restart_publishes_again", game_frame_rate_poll_after_a_restart_publishes_again },
	{ "game_pump_starts_and_heartbeats", game_pump_starts_and_heartbeats },
	{ "game_loading_sets_and_clears_busy", game_loading_sets_and_clears_busy },
	{ "game_modal_dialog_sets_and_clears_busy", game_modal_dialog_sets_and_clears_busy },
	{ "game_modal_dialog_with_the_bridge_off_does_nothing", game_modal_dialog_with_the_bridge_off_does_nothing },
	{ "game_state_loaded_before_start_does_nothing", game_state_loaded_before_start_does_nothing },
	{ "game_shutdown_stops_watcher_and_bridge", game_shutdown_stops_watcher_and_bridge },
	{ "game_halted_stops_heartbeat_and_publishes_crash", game_halted_stops_heartbeat_and_publishes_crash },
	{ "game_halted_before_start_does_nothing", game_halted_before_start_does_nothing },
	{ "game_shutdown_after_halt_keeps_the_published_crash", game_shutdown_after_halt_keeps_the_published_crash },
	{ "game_shutdown_after_halt_beats_again_on_restart", game_shutdown_after_halt_beats_again_on_restart },
	{ "game_map_ready_exports", game_map_ready_exports },
	{ "game_map_ready_with_the_bridge_off_exports_nothing", game_map_ready_with_the_bridge_off_exports_nothing },
	{ "game_bsp_loaded_exports_only_once_started", game_bsp_loaded_exports_only_once_started },
	{ "game_console_started_applies_the_start_map", game_console_started_applies_the_start_map },
	{ "game_console_started_without_a_start_map_does_nothing", game_console_started_without_a_start_map_does_nothing },
	{ "game_map_ready_publishes_a_tick_and_holds_for_a_present_reader", game_map_ready_publishes_a_tick_and_holds_for_a_present_reader },
	{ "game_map_ready_without_a_reader_does_not_hold", game_map_ready_without_a_reader_does_not_hold },
	{ "game_map_ready_in_a_network_game_does_not_hold", game_map_ready_in_a_network_game_does_not_hold },
	{ "game_hold_times_out_and_starts_time", game_hold_times_out_and_starts_time },
	{ "game_tick_publishes_through_the_world", game_tick_publishes_through_the_world },
	{ "game_frame_end_publishes_window_zero_camera", game_frame_end_publishes_window_zero_camera },
	{ 0, 0 }
};

/*
TEST_CORE.C

The game-side core (port/linux/src/ue_bridge.c) against a fake operating
system: sections are static buffers that outlive their mappings (as a named
section does while another process holds it), and every map is counted
against its unmap.
*/

#include "ueb_test.h"
#include "../../linux/src/ue_bridge.h"
#include "ue_bridge_ring.h"

#include <string.h>

#define FAKE_SECTIONS 4

static struct
{
	char name[UE_BRIDGE_NAME_CHARS];
	uint64_t storage[UE_BRIDGE_SECTION_SIZE / 8];
	int used;
} fake_sections[FAKE_SECTIONS];

static int fake_maps;
static int fake_unmaps;
static const char *fake_fail_name;
static uint64_t fake_now;
static uint64_t fake_random;
static int fake_debugger;
static char fake_last_section_name[UE_BRIDGE_NAME_CHARS];

static void *fake_map(const char *name, uint32_t size, void **handle)
{
	int index;

	if (fake_fail_name && strstr(name, fake_fail_name))
		return 0;
	if (size > sizeof(fake_sections[0].storage))
		return 0;
	if (strcmp(name, UE_BRIDGE_DIRECTORY_NAME) != 0)
		strncpy(fake_last_section_name, name, sizeof(fake_last_section_name) - 1);
	for (index = 0; index < FAKE_SECTIONS; index++)
	{
		if (fake_sections[index].used && strcmp(fake_sections[index].name, name) == 0)
			break;
	}
	if (index == FAKE_SECTIONS)
	{
		for (index = 0; index < FAKE_SECTIONS && fake_sections[index].used; index++)
			;
		if (index == FAKE_SECTIONS)
			return 0;
		fake_sections[index].used = 1;
		strncpy(fake_sections[index].name, name, sizeof(fake_sections[index].name) - 1);
		memset(fake_sections[index].storage, 0, sizeof(fake_sections[index].storage));
	}
	fake_maps++;
	*handle = &fake_sections[index];
	return fake_sections[index].storage;
}

static void fake_unmap(void *view, void *handle)
{
	(void)view;
	(void)handle;
	fake_unmaps++;
}

static uint64_t fake_qpc(void) { return fake_now; }
static uint64_t fake_frequency(void) { return 10000000ull; }
static uint32_t fake_pid(void) { return 1234; }
static uint64_t fake_random64(void) { return fake_random; }
static int fake_debugger_present(void) { return fake_debugger; }
static void fake_log(const char *message) { (void)message; }

static const struct ue_bridge_os fake_os =
{
	fake_map, fake_unmap, fake_qpc, fake_frequency, fake_pid, fake_random64, fake_debugger_present, fake_log
};

static void fake_reset(void)
{
	ue_bridge_stop(UE_BRIDGE_STOP_EXIT);
	memset(fake_sections, 0, sizeof(fake_sections));
	fake_maps = 0;
	fake_unmaps = 0;
	fake_fail_name = 0;
	fake_now = 1000;
	fake_random = 0x1122334455667788ull;
	fake_debugger = 0;
	memset(fake_last_section_name, 0, sizeof(fake_last_section_name));
}

static struct ue_bridge_settings enabled_settings(void)
{
	struct ue_bridge_settings settings;

	settings.enabled = 1;
	settings.log_path = "C:/halo/debug.txt";
	settings.max_objects = 8192;
	return settings;
}

static volatile struct ue_bridge_directory *fake_directory(void)
{
	int index;

	for (index = 0; index < FAKE_SECTIONS; index++)
	{
		if (fake_sections[index].used && strcmp(fake_sections[index].name, UE_BRIDGE_DIRECTORY_NAME) == 0)
			return (volatile struct ue_bridge_directory *)fake_sections[index].storage;
	}
	return 0;
}

static volatile struct ue_bridge_header *fake_bridge_section(void)
{
	int index;

	for (index = 0; index < FAKE_SECTIONS; index++)
	{
		if (fake_sections[index].used && strcmp(fake_sections[index].name, UE_BRIDGE_DIRECTORY_NAME) != 0)
			return (volatile struct ue_bridge_header *)fake_sections[index].storage;
	}
	return 0;
}

static void core_disabled_maps_nothing(void)
{
	struct ue_bridge_settings settings = enabled_settings();

	fake_reset();
	settings.enabled = 0;
	UEB_CHECK(!ue_bridge_start(&settings, &fake_os));
	UEB_CHECK(fake_maps == 0);
	UEB_CHECK(!ue_bridge_active());
	UEB_CHECK(ue_bridge_header() == 0);
}

static void core_calls_before_start_are_harmless(void)
{
	fake_reset();
	ue_bridge_publish_tick(1);
	ue_bridge_publish_frame(1, 0.5f);
	ue_bridge_heartbeat();
	ue_bridge_bump_load_epoch();
	ue_bridge_bump_state_epoch();
	ue_bridge_stop(UE_BRIDGE_STOP_EXIT);
	UEB_CHECK(fake_maps == 0);
	UEB_CHECK(fake_unmaps == 0);
}

static void core_start_fills_header(void)
{
	struct ue_bridge_settings settings = enabled_settings();
	volatile struct ue_bridge_header *header;

	fake_reset();
	UEB_CHECK(ue_bridge_start(&settings, &fake_os));
	header = ue_bridge_header();
	UEB_CHECK(header != 0);
	UEB_CHECK(header->magic == UE_BRIDGE_MAGIC);
	UEB_CHECK(header->version == UE_BRIDGE_VERSION);
	UEB_CHECK(header->header_size == UE_BRIDGE_HEADER_SIZE);
	UEB_CHECK(header->section_size == UE_BRIDGE_SECTION_SIZE);
	UEB_CHECK(header->session_id == 0x1122334455667788ull);
	UEB_CHECK(header->qpc_frequency == 10000000ull);
	UEB_CHECK(header->game_pid == 1234);
	UEB_CHECK(header->max_objects == 8192);
	UEB_CHECK(header->game_hang_timeout_ms == UE_BRIDGE_DEFAULT_HANG_TIMEOUT_MS);
	UEB_CHECK(header->game_heartbeat_qpc == 1000);
	UEB_CHECK(strcmp((const char *)header->game_log_path, "C:/halo/debug.txt") == 0);
	UEB_CHECK(header->game_stopping == UE_BRIDGE_STOP_NONE);
	UEB_CHECK(ue_bridge_session_id() == 0x1122334455667788ull);
}

static void core_rings_are_valid_and_do_not_overlap(void)
{
	struct ue_bridge_settings settings = enabled_settings();
	volatile struct ue_bridge_header *header;

	fake_reset();
	UEB_CHECK(ue_bridge_start(&settings, &fake_os));
	header = ue_bridge_header();
	UEB_CHECK(ue_bridge_ring_valid(&header->tick_ring, UE_BRIDGE_SECTION_SIZE, sizeof(struct ue_bridge_slot)));
	UEB_CHECK(ue_bridge_ring_valid(&header->frame_ring, UE_BRIDGE_SECTION_SIZE, sizeof(struct ue_bridge_frame_slot)));
	UEB_CHECK(header->tick_ring.offset >= UE_BRIDGE_HEADER_SIZE);
	UEB_CHECK(header->tick_ring.offset + header->tick_ring.slot_size * header->tick_ring.slot_count <= header->frame_ring.offset);
}

static void core_section_name_carries_pid_and_session(void)
{
	struct ue_bridge_settings settings = enabled_settings();

	fake_reset();
	UEB_CHECK(ue_bridge_start(&settings, &fake_os));
	UEB_CHECK(strcmp(fake_last_section_name, "Local\\HaloCEUE.Bridge.1234.1122334455667788") == 0);
}

static void core_publishes_directory_entry(void)
{
	struct ue_bridge_settings settings = enabled_settings();
	volatile struct ue_bridge_directory *directory;

	fake_reset();
	UEB_CHECK(ue_bridge_start(&settings, &fake_os));
	directory = fake_directory();
	UEB_CHECK(directory != 0);
	UEB_CHECK((directory->sequence & 1u) == 0);
	UEB_CHECK(directory->sequence != 0);
	UEB_CHECK(directory->magic == UE_BRIDGE_MAGIC);
	UEB_CHECK(directory->version == UE_BRIDGE_VERSION);
	UEB_CHECK(directory->game_pid == 1234);
	UEB_CHECK(directory->session_id == 0x1122334455667788ull);
	UEB_CHECK(strcmp((const char *)directory->section_name, "Local\\HaloCEUE.Bridge.1234.1122334455667788") == 0);
}

static void core_directory_left_odd_by_dead_game_is_repaired(void)
{
	struct ue_bridge_settings settings = enabled_settings();
	volatile struct ue_bridge_directory *directory;
	void *handle;

	fake_reset();
	/* a game that died while rewriting the entry */
	directory = (volatile struct ue_bridge_directory *)fake_map(UE_BRIDGE_DIRECTORY_NAME, UE_BRIDGE_DIRECTORY_SIZE, &handle);
	directory->sequence = 5;
	UEB_CHECK(ue_bridge_start(&settings, &fake_os));
	UEB_CHECK((directory->sequence & 1u) == 0);
	UEB_CHECK(directory->sequence > 5);
}

static void core_zero_random_session_becomes_one(void)
{
	struct ue_bridge_settings settings = enabled_settings();

	fake_reset();
	fake_random = 0;
	UEB_CHECK(ue_bridge_start(&settings, &fake_os));
	UEB_CHECK(ue_bridge_session_id() == 1);
	UEB_CHECK(fake_directory()->session_id == 1);
}

static void core_long_log_path_is_truncated_and_terminated(void)
{
	struct ue_bridge_settings settings = enabled_settings();
	char path[700];

	fake_reset();
	memset(path, 'a', sizeof(path) - 1);
	path[sizeof(path) - 1] = 0;
	settings.log_path = path;
	UEB_CHECK(ue_bridge_start(&settings, &fake_os));
	UEB_CHECK(strlen((const char *)ue_bridge_header()->game_log_path) == UE_BRIDGE_PATH_BYTES - 1);
}

static void fake_upper_case_path(const char *path, char *utf8, uint32_t capacity)
{
	uint32_t index;

	for (index = 0; index + 1 < capacity && path[index]; index++)
		utf8[index] = (char)(path[index] >= 'a' && path[index] <= 'z' ? path[index] - 'a' + 'A' : path[index]);
	utf8[index] = 0;
}

static void core_log_path_goes_through_path_to_utf8(void)
{
	struct ue_bridge_settings settings = enabled_settings();
	struct ue_bridge_os os = fake_os;

	fake_reset();
	os.path_to_utf8 = fake_upper_case_path;
	UEB_CHECK(ue_bridge_start(&settings, &os));
	UEB_CHECK(strcmp((const char *)ue_bridge_header()->game_log_path, "C:/HALO/DEBUG.TXT") == 0);
	ue_bridge_stop(UE_BRIDGE_STOP_EXIT);
}

static void core_null_log_path_is_empty(void)
{
	struct ue_bridge_settings settings = enabled_settings();

	fake_reset();
	settings.log_path = 0;
	UEB_CHECK(ue_bridge_start(&settings, &fake_os));
	UEB_CHECK(ue_bridge_header()->game_log_path[0] == 0);
}

static void core_publish_tick_and_frame(void)
{
	struct ue_bridge_settings settings = enabled_settings();
	volatile struct ue_bridge_header *header;
	struct ue_bridge_slot tick;
	struct ue_bridge_frame_slot frame;

	fake_reset();
	UEB_CHECK(ue_bridge_start(&settings, &fake_os));
	header = ue_bridge_header();
	fake_now = 5000;
	ue_bridge_publish_tick(41);
	ue_bridge_publish_tick(42);
	fake_now = 6000;
	ue_bridge_publish_frame(7, 0.25f);
	UEB_CHECK(ue_bridge_ring_read_newest((const volatile uint8_t *)header, &header->tick_ring, &tick, sizeof(tick), 0) == UE_BRIDGE_READ_NEWEST);
	UEB_CHECK(tick.id == 42);
	UEB_CHECK(tick.publish_qpc == 5000);
	UEB_CHECK(ue_bridge_ring_read_newest((const volatile uint8_t *)header, &header->frame_ring, &frame, sizeof(frame), 0) == UE_BRIDGE_READ_NEWEST);
	UEB_CHECK(frame.slot.id == 7);
	UEB_CHECK(frame.slot.publish_qpc == 6000);
	UEB_CHECK(frame.interpolation_fraction == 0.25f);
}

static void core_epochs_bump(void)
{
	struct ue_bridge_settings settings = enabled_settings();
	volatile struct ue_bridge_header *header;

	fake_reset();
	UEB_CHECK(ue_bridge_start(&settings, &fake_os));
	header = ue_bridge_header();
	ue_bridge_bump_load_epoch();
	ue_bridge_bump_load_epoch();
	ue_bridge_bump_state_epoch();
	UEB_CHECK(header->load_epoch == 2);
	UEB_CHECK(header->state_epoch == 1);
}

static void core_busy_flag_round_trips(void)
{
	struct ue_bridge_settings settings = enabled_settings();

	fake_reset();
	UEB_CHECK(ue_bridge_start(&settings, &fake_os));
	ue_bridge_set_busy(1);
	UEB_CHECK(ue_bridge_header()->game_busy == 1);
	ue_bridge_set_busy(0);
	UEB_CHECK(ue_bridge_header()->game_busy == 0);
}

static void core_heartbeat_tracks_qpc_and_debugger(void)
{
	struct ue_bridge_settings settings = enabled_settings();
	volatile struct ue_bridge_header *header;

	fake_reset();
	UEB_CHECK(ue_bridge_start(&settings, &fake_os));
	header = ue_bridge_header();
	fake_now = 777;
	fake_debugger = 1;
	ue_bridge_heartbeat();
	UEB_CHECK(header->game_heartbeat_qpc == 777);
	UEB_CHECK(header->game_debugger_attached == 1);
	fake_debugger = 0;
	ue_bridge_heartbeat();
	UEB_CHECK(header->game_debugger_attached == 0);
}

static void core_stop_clears_own_directory_entry_and_unmaps(void)
{
	struct ue_bridge_settings settings = enabled_settings();
	volatile struct ue_bridge_directory *directory;
	volatile struct ue_bridge_header *section;

	fake_reset();
	UEB_CHECK(ue_bridge_start(&settings, &fake_os));
	ue_bridge_stop(UE_BRIDGE_STOP_EXIT);
	directory = fake_directory();
	section = fake_bridge_section();
	UEB_CHECK(!ue_bridge_active());
	UEB_CHECK(ue_bridge_header() == 0);
	UEB_CHECK(section->game_stopping == UE_BRIDGE_STOP_EXIT);
	UEB_CHECK(directory->game_pid == 0);
	UEB_CHECK(directory->session_id == 0);
	UEB_CHECK(directory->section_name[0] == 0);
	UEB_CHECK((directory->sequence & 1u) == 0);
	UEB_CHECK(fake_maps == fake_unmaps);
}

static void core_stop_keeps_a_newer_games_directory_entry(void)
{
	struct ue_bridge_settings settings = enabled_settings();

	fake_reset();
	UEB_CHECK(ue_bridge_start(&settings, &fake_os));
	/* another game started since and wrote itself in */
	fake_directory()->session_id = 999;
	fake_directory()->game_pid = 5678;
	ue_bridge_stop(UE_BRIDGE_STOP_EXIT);
	UEB_CHECK(fake_directory()->session_id == 999);
	UEB_CHECK(fake_directory()->game_pid == 5678);
}

static void core_section_failure_starts_nothing(void)
{
	struct ue_bridge_settings settings = enabled_settings();

	fake_reset();
	fake_fail_name = "Bridge.1234";
	UEB_CHECK(!ue_bridge_start(&settings, &fake_os));
	UEB_CHECK(!ue_bridge_active());
	UEB_CHECK(fake_maps == fake_unmaps);
}

static void core_directory_failure_unmaps_section(void)
{
	struct ue_bridge_settings settings = enabled_settings();

	fake_reset();
	fake_fail_name = "Directory";
	UEB_CHECK(!ue_bridge_start(&settings, &fake_os));
	UEB_CHECK(!ue_bridge_active());
	UEB_CHECK(fake_maps == 1);
	UEB_CHECK(fake_unmaps == 1);
}

static void core_restart_after_stop(void)
{
	struct ue_bridge_settings settings = enabled_settings();

	fake_reset();
	UEB_CHECK(ue_bridge_start(&settings, &fake_os));
	ue_bridge_stop(UE_BRIDGE_STOP_EXIT);
	fake_random = 0xABCDull;
	UEB_CHECK(ue_bridge_start(&settings, &fake_os));
	UEB_CHECK(ue_bridge_session_id() == 0xABCDull);
	UEB_CHECK(fake_directory()->session_id == 0xABCDull);
	ue_bridge_stop(UE_BRIDGE_STOP_EXIT);
	UEB_CHECK(fake_maps == fake_unmaps);
}

static void core_second_start_while_active_is_a_no_op(void)
{
	struct ue_bridge_settings settings = enabled_settings();

	fake_reset();
	UEB_CHECK(ue_bridge_start(&settings, &fake_os));
	UEB_CHECK(ue_bridge_start(&settings, &fake_os));
	UEB_CHECK(fake_maps == 2);
}

const struct ueb_test ueb_core_tests[] =
{
	{ "core_disabled_maps_nothing", core_disabled_maps_nothing },
	{ "core_calls_before_start_are_harmless", core_calls_before_start_are_harmless },
	{ "core_start_fills_header", core_start_fills_header },
	{ "core_rings_are_valid_and_do_not_overlap", core_rings_are_valid_and_do_not_overlap },
	{ "core_section_name_carries_pid_and_session", core_section_name_carries_pid_and_session },
	{ "core_publishes_directory_entry", core_publishes_directory_entry },
	{ "core_directory_left_odd_by_dead_game_is_repaired", core_directory_left_odd_by_dead_game_is_repaired },
	{ "core_zero_random_session_becomes_one", core_zero_random_session_becomes_one },
	{ "core_long_log_path_is_truncated_and_terminated", core_long_log_path_is_truncated_and_terminated },
	{ "core_log_path_goes_through_path_to_utf8", core_log_path_goes_through_path_to_utf8 },
	{ "core_null_log_path_is_empty", core_null_log_path_is_empty },
	{ "core_publish_tick_and_frame", core_publish_tick_and_frame },
	{ "core_epochs_bump", core_epochs_bump },
	{ "core_busy_flag_round_trips", core_busy_flag_round_trips },
	{ "core_heartbeat_tracks_qpc_and_debugger", core_heartbeat_tracks_qpc_and_debugger },
	{ "core_stop_clears_own_directory_entry_and_unmaps", core_stop_clears_own_directory_entry_and_unmaps },
	{ "core_stop_keeps_a_newer_games_directory_entry", core_stop_keeps_a_newer_games_directory_entry },
	{ "core_section_failure_starts_nothing", core_section_failure_starts_nothing },
	{ "core_directory_failure_unmaps_section", core_directory_failure_unmaps_section },
	{ "core_restart_after_stop", core_restart_after_stop },
	{ "core_second_start_while_active_is_a_no_op", core_second_start_while_active_is_a_no_op },
	{ 0, 0 }
};

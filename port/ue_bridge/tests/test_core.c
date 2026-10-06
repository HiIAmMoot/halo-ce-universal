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
#include <windows.h>

/* a small section: the layout puts 64 KB tick slots and a 1 MB load region in it */
#define TEST_SECTION_SIZE (0x200000u)
#define TEST_TICK_SLOT_SIZE (0x10000u)

#define FAKE_SECTIONS 4

static struct
{
	char name[UE_BRIDGE_NAME_CHARS];
	/* page-aligned: the directory's first page holds nothing but the directory, so a test can watch writes to it */
	uint64_t storage[TEST_SECTION_SIZE / 8] __attribute__((aligned(4096)));
	int used;
} fake_sections[FAKE_SECTIONS];

static int fake_maps;
static int fake_unmaps;
static const char *fake_fail_name;
static uint64_t fake_now;
static uint64_t fake_random;
static int fake_debugger;
static char fake_last_section_name[UE_BRIDGE_NAME_CHARS];
static int fake_recycle;
static int fake_directory_maps;
static int fake_header_complete_at_directory_map;
static int fake_first_unmap_seen;
static uint32_t fake_stopping_at_first_unmap;
static uint64_t fake_session_at_first_unmap;
static uint32_t fake_sequence_at_first_unmap;
static int fake_section_unmapped;
static volatile struct ue_bridge_header *fake_header_api_at_section_unmap;

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

/* what UE needs from the header once the directory points at it */
static int header_is_complete(volatile struct ue_bridge_header *header)
{
	return header != 0
		&& header->magic == UE_BRIDGE_MAGIC
		&& header->version == UE_BRIDGE_VERSION
		&& header->header_size == UE_BRIDGE_HEADER_SIZE
		&& header->section_size == TEST_SECTION_SIZE
		&& header->load_region.size >= UE_BRIDGE_MIN_LOAD_SIZE
		&& header->session_id != 0
		&& header->qpc_frequency != 0
		&& header->game_pid != 0
		&& header->max_objects != 0
		&& header->game_hang_timeout_ms != 0
		&& header->tick_ring.slot_count != 0
		&& header->frame_ring.slot_count != 0
		&& header->game_log_path[0] != 0
		&& header->game_heartbeat_qpc != 0;
}

static void *fake_map(const char *name, uint32_t size, void **handle)
{
	int index;

	if (fake_fail_name && strstr(name, fake_fail_name))
		return 0;
	if (size > sizeof(fake_sections[0].storage))
		return 0;
	if (strcmp(name, UE_BRIDGE_DIRECTORY_NAME) != 0)
		strncpy(fake_last_section_name, name, sizeof(fake_last_section_name) - 1);
	else
	{
		fake_directory_maps++;
		fake_header_complete_at_directory_map = fake_header_complete_at_directory_map && header_is_complete(fake_bridge_section());
	}
	for (index = 0; index < FAKE_SECTIONS; index++)
	{
		if (fake_sections[index].used && strcmp(fake_sections[index].name, name) == 0)
			break;
	}
	if (index == FAKE_SECTIONS)
	{
		/* the stress tests start thousands of games, each with a section of its own: a stopped game's is dead */
		if (fake_recycle && strcmp(name, UE_BRIDGE_DIRECTORY_NAME) != 0)
		{
			for (index = 0; index < FAKE_SECTIONS; index++)
			{
				if (strcmp(fake_sections[index].name, UE_BRIDGE_DIRECTORY_NAME) != 0)
					fake_sections[index].used = 0;
			}
		}
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
	volatile struct ue_bridge_header *section = fake_bridge_section();
	volatile struct ue_bridge_directory *directory = fake_directory();

	(void)handle;
	if (section && view == (void *)section)
	{
		fake_section_unmapped = 1;
		fake_header_api_at_section_unmap = ue_bridge_header();
	}
	if (!fake_first_unmap_seen && section && directory)
	{
		fake_first_unmap_seen = 1;
		fake_stopping_at_first_unmap = section->game_stopping;
		fake_session_at_first_unmap = directory->session_id;
		fake_sequence_at_first_unmap = directory->sequence;
	}
	fake_unmaps++;
}

static uint64_t fake_qpc(void) { return fake_now; }
static uint64_t fake_frequency(void) { return 10000000ull; }
static uint32_t fake_pid(void) { return 1234; }
static uint64_t fake_random64(void) { return fake_random; }
static int fake_debugger_present(void) { return fake_debugger; }
/* every line logged since fake_reset */
static char fake_log_text[1024];

static void fake_log(const char *message)
{
	if (strlen(fake_log_text) + strlen(message) + 2 <= sizeof(fake_log_text))
	{
		strcat(fake_log_text, message);
		strcat(fake_log_text, "\n");
	}
}

static const struct ue_bridge_os fake_os =
{
	fake_map, fake_unmap, fake_qpc, fake_frequency, fake_pid, fake_random64, fake_debugger_present, fake_log
};

/* the directory as the lock sees it: an entry written outside the lock shows
as a change between the lock's two ends and the call that wrote it */
static int fake_lock_calls;
static int fake_unlock_calls;
static int fake_lock_held;
static int fake_lock_misuse;
static uint64_t fake_session_at_lock;
static uint64_t fake_session_at_unlock;
static uint32_t fake_sequence_at_unlock;

static void fake_lock(void)
{
	volatile struct ue_bridge_directory *directory = fake_directory();

	if (fake_lock_held)
		fake_lock_misuse = 1;
	fake_lock_held = 1;
	fake_lock_calls++;
	fake_session_at_lock = directory ? directory->session_id : 0;
}

static void fake_unlock(void)
{
	volatile struct ue_bridge_directory *directory = fake_directory();

	if (!fake_lock_held)
		fake_lock_misuse = 1;
	fake_lock_held = 0;
	fake_unlock_calls++;
	fake_session_at_unlock = directory ? directory->session_id : 0;
	fake_sequence_at_unlock = directory ? directory->sequence : 1;
}

static const struct ue_bridge_os locking_os =
{
	fake_map, fake_unmap, fake_qpc, fake_frequency, fake_pid, fake_random64, fake_debugger_present, fake_log,
	0, fake_lock, fake_unlock
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
	fake_recycle = 0;
	fake_directory_maps = 0;
	fake_header_complete_at_directory_map = 1;
	fake_first_unmap_seen = 0;
	fake_stopping_at_first_unmap = 0;
	fake_session_at_first_unmap = 0;
	fake_sequence_at_first_unmap = 1;
	fake_section_unmapped = 0;
	fake_header_api_at_section_unmap = (volatile struct ue_bridge_header *)1;
	fake_log_text[0] = 0;
	fake_lock_calls = 0;
	fake_unlock_calls = 0;
	fake_lock_held = 0;
	fake_lock_misuse = 0;
	fake_session_at_lock = 0;
	fake_session_at_unlock = 0;
	fake_sequence_at_unlock = 1;
}

static struct ue_bridge_settings enabled_settings(void)
{
	struct ue_bridge_settings settings;

	settings.enabled = 1;
	settings.log_path = "C:/halo/debug.txt";
	settings.max_objects = 8192;
	settings.section_size = TEST_SECTION_SIZE;
	settings.tick_slot_size = TEST_TICK_SLOT_SIZE;
	return settings;
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
	ue_bridge_publish_frame_rate(60, 60);
	ue_bridge_heartbeat();
	ue_bridge_bump_load_epoch();
	ue_bridge_bump_state_epoch();
	ue_bridge_set_busy(1);
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
	UEB_CHECK(header->section_size == TEST_SECTION_SIZE);
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
	UEB_CHECK(ue_bridge_ring_valid(&header->tick_ring, TEST_SECTION_SIZE, sizeof(struct ue_bridge_tick_header)));
	UEB_CHECK(ue_bridge_ring_valid(&header->frame_ring, TEST_SECTION_SIZE, sizeof(struct ue_bridge_frame_slot)));
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

/* a truncated path could end inside a UTF-8 sequence, and UE would read a path that is not the game's */
static void core_log_path_that_does_not_fit_is_published_empty(void)
{
	struct ue_bridge_settings settings = enabled_settings();
	/* one byte over the largest path that fits */
	char path[UE_BRIDGE_PATH_BYTES + 1];

	fake_reset();
	memset(path, 'a', sizeof(path) - 1);
	path[sizeof(path) - 1] = 0;
	settings.log_path = path;
	UEB_CHECK(ue_bridge_start(&settings, &fake_os));
	UEB_CHECK(ue_bridge_header()->game_log_path[0] == 0);
	UEB_CHECK(strstr(fake_log_text, "log path") != 0);
}

static void core_log_path_that_just_fits_is_published_whole(void)
{
	struct ue_bridge_settings settings = enabled_settings();
	char path[UE_BRIDGE_PATH_BYTES];

	fake_reset();
	memset(path, 'a', sizeof(path) - 1);
	path[sizeof(path) - 1] = 0;
	settings.log_path = path;
	UEB_CHECK(ue_bridge_start(&settings, &fake_os));
	UEB_CHECK(strcmp((const char *)ue_bridge_header()->game_log_path, path) == 0);
	UEB_CHECK(strstr(fake_log_text, "log path") == 0);
}

static int silent_failure_buffer_first_byte;

static void fake_silent_failure_path(const char *path, char *utf8, uint32_t capacity)
{
	(void)path;
	(void)capacity;
	silent_failure_buffer_first_byte = utf8[0];
}

/* Fills the stack where ue_bridge_start's buffer will sit with short non-empty strings: all 'z' would
read as a path too long to fit, which is also published empty, and hide a missing reset. */
static void __attribute__((noinline)) poison_the_stack(void)
{
	volatile char garbage[2048];
	size_t index;

	for (index = 0; index < sizeof(garbage); index++)
		garbage[index] = index % 8 == 7 ? 0 : 'z';
}

static void core_log_path_from_a_silently_failing_conversion_is_empty(void)
{
	struct ue_bridge_settings settings = enabled_settings();
	static struct ue_bridge_os os;

	fake_reset();
	os = fake_os;
	os.path_to_utf8 = fake_silent_failure_path;
	silent_failure_buffer_first_byte = -1;
	poison_the_stack();
	UEB_CHECK(ue_bridge_start(&settings, &os));
	/* what the conversion sees on entry, whatever the stack layout */
	UEB_CHECK(silent_failure_buffer_first_byte == 0);
	UEB_CHECK(ue_bridge_header()->game_log_path[0] == 0);
	UEB_CHECK(strstr(fake_log_text, "log path") != 0);
	ue_bridge_stop(UE_BRIDGE_STOP_EXIT);
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
	/* static: the core keeps the pointer, so it must not die with this frame if a check fails */
	static struct ue_bridge_os os;

	fake_reset();
	os = fake_os;
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
	/* no path was given, so none was dropped */
	UEB_CHECK(strstr(fake_log_text, "log path") == 0);
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

static void core_publish_frame_rate_writes_both_fields_and_republishes(void)
{
	struct ue_bridge_settings settings = enabled_settings();
	volatile struct ue_bridge_header *header;

	fake_reset();
	ue_bridge_publish_frame_rate(144, 60);
	UEB_CHECK(ue_bridge_start(&settings, &fake_os));
	header = ue_bridge_header();
	UEB_CHECK(header->game_refresh_hz == 0 && header->game_frame_target_hz == 0);
	ue_bridge_publish_frame_rate(144, 60);
	UEB_CHECK(header->game_refresh_hz == 144);
	UEB_CHECK(header->game_frame_target_hz == 60);
	ue_bridge_publish_frame_rate(60, 0);
	UEB_CHECK(header->game_refresh_hz == 60);
	UEB_CHECK(header->game_frame_target_hz == 0);
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

static void core_publish_stopping_sets_flag_only_when_active(void)
{
	struct ue_bridge_settings settings = enabled_settings();

	fake_reset();
	ue_bridge_publish_stopping(UE_BRIDGE_STOP_CRASH);
	UEB_CHECK(ue_bridge_header() == 0);
	UEB_CHECK(ue_bridge_start(&settings, &fake_os));
	UEB_CHECK(ue_bridge_header()->game_stopping == UE_BRIDGE_STOP_NONE);
	ue_bridge_publish_stopping(UE_BRIDGE_STOP_CRASH);
	UEB_CHECK(ue_bridge_header()->game_stopping == UE_BRIDGE_STOP_CRASH);
	UEB_CHECK(ue_bridge_active());
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
	UEB_CHECK(fake_maps == 0);
	UEB_CHECK(fake_maps == fake_unmaps);
}

/* a map_new_section that refuses a name that already exists, as the Windows layer's does */
static void *fake_new_section(const char *name, uint32_t size, void **handle)
{
	int index;

	for (index = 0; index < FAKE_SECTIONS; index++)
	{
		if (fake_sections[index].used && strcmp(fake_sections[index].name, name) == 0)
			return 0;
	}
	return fake_map(name, size, handle);
}

/* the section's name carries the PID and the session id: one that exists was made by someone else */
static void core_start_fails_when_the_section_already_exists(void)
{
	struct ue_bridge_settings settings = enabled_settings();
	static struct ue_bridge_os os;
	void *handle;
	volatile uint32_t *squatter;

	fake_reset();
	os = fake_os;
	os.map_new_section = fake_new_section;
	squatter = (volatile uint32_t *)fake_map("Local\\HaloCEUE.Bridge.1234.1122334455667788", TEST_SECTION_SIZE, &handle);
	UEB_CHECK(squatter != 0);
	squatter[0] = 0xDEADBEEFu;
	fake_maps = 0;
	UEB_CHECK(!ue_bridge_start(&settings, &os));
	UEB_CHECK(!ue_bridge_active());
	UEB_CHECK(fake_directory() == 0);
	UEB_CHECK(squatter[0] == 0xDEADBEEFu);
	UEB_CHECK(fake_maps == 0);
	UEB_CHECK(strstr(fake_log_text, "section") != 0);
}

static void core_start_succeeds_when_the_section_is_new(void)
{
	struct ue_bridge_settings settings = enabled_settings();
	static struct ue_bridge_os os;

	fake_reset();
	os = fake_os;
	os.map_new_section = fake_new_section;
	UEB_CHECK(ue_bridge_start(&settings, &os));
	UEB_CHECK(ue_bridge_header()->magic == UE_BRIDGE_MAGIC);
	ue_bridge_stop(UE_BRIDGE_STOP_EXIT);
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

static void core_header_is_complete_when_the_directory_is_mapped(void)
{
	struct ue_bridge_settings settings = enabled_settings();

	fake_reset();
	UEB_CHECK(ue_bridge_start(&settings, &fake_os));
	UEB_CHECK(fake_directory_maps == 1);
	UEB_CHECK(fake_header_complete_at_directory_map);
}

static void core_stop_writes_before_it_unmaps(void)
{
	struct ue_bridge_settings settings = enabled_settings();

	fake_reset();
	UEB_CHECK(ue_bridge_start(&settings, &fake_os));
	ue_bridge_stop(UE_BRIDGE_STOP_CRASH);
	UEB_CHECK(fake_first_unmap_seen);
	UEB_CHECK(fake_stopping_at_first_unmap == UE_BRIDGE_STOP_CRASH);
	UEB_CHECK(fake_session_at_first_unmap == 0);
	UEB_CHECK((fake_sequence_at_first_unmap & 1u) == 0);
}

static void core_stop_hides_the_header_from_the_crash_filter_before_unmapping_it(void)
{
	struct ue_bridge_settings settings = enabled_settings();

	fake_reset();
	UEB_CHECK(ue_bridge_start(&settings, &fake_os));
	UEB_CHECK(ue_bridge_header() != 0);
	ue_bridge_stop(UE_BRIDGE_STOP_EXIT);
	UEB_CHECK(fake_section_unmapped);
	UEB_CHECK(fake_header_api_at_section_unmap == 0);
}

static void core_start_and_stop_each_take_the_directory_lock_once(void)
{
	struct ue_bridge_settings settings = enabled_settings();

	fake_reset();
	UEB_CHECK(ue_bridge_start(&settings, &locking_os));
	UEB_CHECK(fake_lock_calls == 1);
	UEB_CHECK(fake_unlock_calls == 1);
	/* the entry was written inside the lock, not before it or after it */
	UEB_CHECK(fake_session_at_lock == 0);
	UEB_CHECK(fake_session_at_unlock == 0x1122334455667788ull);
	UEB_CHECK((fake_sequence_at_unlock & 1u) == 0);
	ue_bridge_stop(UE_BRIDGE_STOP_EXIT);
	UEB_CHECK(fake_lock_calls == 2);
	UEB_CHECK(fake_unlock_calls == 2);
	UEB_CHECK(fake_session_at_lock == 0x1122334455667788ull);
	UEB_CHECK(fake_session_at_unlock == 0);
	UEB_CHECK((fake_sequence_at_unlock & 1u) == 0);
	UEB_CHECK(!fake_lock_misuse);
}

/* The seqlock only protects a field written after the sequence turns odd. A write
watch makes that deterministic: the directory's page is read-only while the lock
is held, and the first write to it must be the sequence itself. Timing cannot
hide a field written before it, as it can from the concurrent reader. */
static volatile LONG fake_watching;
static void *volatile fake_first_write;

static LONG CALLBACK first_write_watch(EXCEPTION_POINTERS *info)
{
	volatile struct ue_bridge_directory *directory = fake_directory();
	EXCEPTION_RECORD *record = info->ExceptionRecord;
	uintptr_t address;
	DWORD old;

	if (!fake_watching || !directory || record->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || record->ExceptionInformation[0] != 1)
		return EXCEPTION_CONTINUE_SEARCH;
	address = (uintptr_t)record->ExceptionInformation[1];
	if (address < (uintptr_t)directory || address >= (uintptr_t)directory + 4096u)
		return EXCEPTION_CONTINUE_SEARCH;
	if (!fake_first_write)
		fake_first_write = (void *)address;
	VirtualProtect((void *)directory, 4096u, PAGE_READWRITE, &old);
	return EXCEPTION_CONTINUE_EXECUTION;
}

static void watch_lock(void)
{
	DWORD old;

	fake_lock();
	fake_first_write = 0;
	VirtualProtect((void *)fake_directory(), 4096u, PAGE_READONLY, &old);
}

static void watch_unlock(void)
{
	DWORD old;

	VirtualProtect((void *)fake_directory(), 4096u, PAGE_READWRITE, &old);
	fake_unlock();
}

static const struct ue_bridge_os watching_os =
{
	fake_map, fake_unmap, fake_qpc, fake_frequency, fake_pid, fake_random64, fake_debugger_present, fake_log,
	0, watch_lock, watch_unlock
};

static void core_directory_writes_begin_with_the_sequence(void)
{
	struct ue_bridge_settings settings = enabled_settings();
	void *handler;
	void *start_first;

	fake_reset();
	handler = AddVectoredExceptionHandler(1, first_write_watch);
	UEB_CHECK(handler != 0);
	/* the page must exist before it is protected, so the first start creates it unwatched */
	UEB_CHECK(ue_bridge_start(&settings, &fake_os));
	ue_bridge_stop(UE_BRIDGE_STOP_EXIT);
	fake_watching = 1;
	UEB_CHECK(ue_bridge_start(&settings, &watching_os));
	start_first = fake_first_write;
	ue_bridge_stop(UE_BRIDGE_STOP_EXIT);
	fake_watching = 0;
	RemoveVectoredExceptionHandler(handler);
	UEB_CHECK(start_first == (void *)&fake_directory()->sequence);
	/* the stop's withdrawal is the second write section: its first write is the sequence too */
	UEB_CHECK(fake_first_write == (void *)&fake_directory()->sequence);
}

/* ---------- two-thread stress tests */

#define STRESS_MILLISECONDS 500u
#define STRESS_ID_QPC(id) ((id) * 3u + 7u)

static volatile LONG stress_stop;

static DWORD WINAPI directory_writer(void *parameter)
{
	struct ue_bridge_settings settings = enabled_settings();
	uint64_t round = 0;

	(void)parameter;
	while (!stress_stop)
	{
		fake_random = 0x1000 + round++;
		ue_bridge_start(&settings, &fake_os);
		ue_bridge_publish_tick(round);
		ue_bridge_stop(UE_BRIDGE_STOP_EXIT);
	}
	return 0;
}

/* 1 when the entry read under its seqlock is either empty or names the
section its own pid and session_id say; *accepted counts completed reads */
static int directory_read_is_coherent(volatile struct ue_bridge_directory *directory, uint32_t *accepted)
{
	uint32_t before = ueb_load_u32(&directory->sequence);
	uint32_t pid;
	uint64_t session;
	char name[UE_BRIDGE_NAME_CHARS];
	char expected[UE_BRIDGE_NAME_CHARS];
	uint32_t index;

	if (before & 1u)
		return 1;
	pid = directory->game_pid;
	session = directory->session_id;
	for (index = 0; index < sizeof(name); index++)
		name[index] = directory->section_name[index];
	name[sizeof(name) - 1] = 0;
	ueb_fence();
	if (ueb_load_u32(&directory->sequence) != before)
		return 1;
	(*accepted)++;
	if (pid == 0 && session == 0 && name[0] == 0)
		return 1;
	snprintf(expected, sizeof(expected), "Local\\HaloCEUE.Bridge.%lu.%016llx", (unsigned long)pid, (unsigned long long)session);
	return strcmp(name, expected) == 0;
}

static void core_concurrent_reader_never_sees_a_mixed_directory_entry(void)
{
	struct ue_bridge_settings settings = enabled_settings();
	volatile struct ue_bridge_directory *directory;
	HANDLE writer;
	DWORD start;
	uint32_t accepted = 0;
	int mixed = 0;

	fake_reset();
	/* once, so the directory exists before the reader looks at it */
	UEB_CHECK(ue_bridge_start(&settings, &fake_os));
	ue_bridge_stop(UE_BRIDGE_STOP_EXIT);
	directory = fake_directory();
	UEB_CHECK(directory != 0);
	fake_recycle = 1;
	stress_stop = 0;
	writer = CreateThread(0, 0, directory_writer, 0, 0, 0);
	UEB_CHECK(writer != 0);
	start = GetTickCount();
	while (GetTickCount() - start < STRESS_MILLISECONDS)
	{
		if (!directory_read_is_coherent(directory, &accepted))
			mixed = 1;
	}
	InterlockedExchange(&stress_stop, 1);
	WaitForSingleObject(writer, INFINITE);
	CloseHandle(writer);
	UEB_CHECK(accepted > 0);
	UEB_CHECK(!mixed);
}

static DWORD WINAPI tick_writer(void *parameter)
{
	uint64_t id = 1;

	(void)parameter;
	while (!stress_stop)
	{
		fake_now = STRESS_ID_QPC(id);
		ue_bridge_publish_tick(id);
		id++;
	}
	return 0;
}

static void core_concurrent_reader_never_sees_a_tick_without_its_payload(void)
{
	struct ue_bridge_settings settings = enabled_settings();
	volatile struct ue_bridge_header *header;
	HANDLE writer;
	DWORD start;
	uint32_t reads = 0;
	int mismatched = 0;

	fake_reset();
	UEB_CHECK(ue_bridge_start(&settings, &fake_os));
	header = ue_bridge_header();
	stress_stop = 0;
	writer = CreateThread(0, 0, tick_writer, 0, 0, 0);
	UEB_CHECK(writer != 0);
	start = GetTickCount();
	while (GetTickCount() - start < STRESS_MILLISECONDS)
	{
		struct ue_bridge_slot tick;
		enum ue_bridge_read_result result = ue_bridge_ring_read_newest((const volatile uint8_t *)header, &header->tick_ring, &tick, sizeof(tick), 0);

		if (result == UE_BRIDGE_READ_NEWEST || result == UE_BRIDGE_READ_PREVIOUS)
		{
			reads++;
			if (tick.publish_qpc != STRESS_ID_QPC(tick.id))
				mismatched = 1;
		}
	}
	InterlockedExchange(&stress_stop, 1);
	WaitForSingleObject(writer, INFINITE);
	CloseHandle(writer);
	UEB_CHECK(reads > 0);
	UEB_CHECK(!mismatched);
}

static void start_bridge(void)
{
	struct ue_bridge_settings settings = enabled_settings();

	fake_reset();
	UEB_CHECK(ue_bridge_start(&settings, &fake_os));
}

static volatile struct ue_bridge_load_root *test_root(volatile struct ue_bridge_header *header)
{
	return (volatile struct ue_bridge_load_root *)((volatile uint8_t *)header + header->load_region.offset);
}

static void load_begin_marks_the_sequence_odd_and_clears_the_root(void)
{
	volatile struct ue_bridge_header *header;
	struct ue_bridge_load_writer *writer;

	start_bridge();
	header = fake_bridge_section();
	test_root(header)->magic = 0x12345678u;
	writer = ue_bridge_load_begin();
	UEB_CHECK(writer != 0);
	UEB_CHECK(header->load_sequence & 1u);
	UEB_CHECK(header->export_epoch == 0);
	UEB_CHECK(test_root(header)->magic == 0);
	UEB_CHECK(writer->used == 0 && writer->capacity == header->load_region.size);
	ue_bridge_stop(UE_BRIDGE_STOP_EXIT);
}

static void load_begin_uses_the_game_layout_not_the_header(void)
{
	volatile struct ue_bridge_header *header;
	struct ue_bridge_load_writer *writer;
	uint32_t trusted;

	start_bridge();
	header = fake_bridge_section();
	trusted = header->load_region.size;
	/* UE can write the header: a damaged region size must not steer the game's writes */
	header->load_region.size = trusted * 4u;
	writer = ue_bridge_load_begin();
	UEB_CHECK(writer->capacity == trusted);
	UEB_CHECK(ue_bridge_trusted_layout()->load_region.size == trusted);
	ue_bridge_stop(UE_BRIDGE_STOP_EXIT);
}

static void load_end_publishes_the_epoch_and_completeness(void)
{
	volatile struct ue_bridge_header *header;

	start_bridge();
	header = fake_bridge_section();
	ue_bridge_bump_load_epoch();
	ue_bridge_bump_load_epoch();
	ue_bridge_load_begin();
	ue_bridge_load_end(0);
	UEB_CHECK((header->load_sequence & 1u) == 0);
	UEB_CHECK(header->load_sequence != 0);
	UEB_CHECK(header->export_epoch == 2);
	UEB_CHECK(header->export_complete == 0);
	ue_bridge_load_begin();
	ue_bridge_load_end(1);
	UEB_CHECK(header->export_complete == 1);
	ue_bridge_stop(UE_BRIDGE_STOP_EXIT);
}

static void load_append_continues_after_the_export_of_this_epoch_only(void)
{
	struct ue_bridge_load_writer *writer;

	start_bridge();
	UEB_CHECK(ue_bridge_load_append() == 0);
	writer = ue_bridge_load_begin();
	ue_bridge_load_reserve(writer, 1, 64);
	UEB_CHECK(ue_bridge_load_append() == 0);
	ue_bridge_load_end(1);
	UEB_CHECK(ue_bridge_load_append() == writer);
	UEB_CHECK(writer->used == 64);
	/* a new map's epoch: the last map's region is no place to append */
	ue_bridge_bump_load_epoch();
	UEB_CHECK(ue_bridge_load_append() == 0);
	ue_bridge_stop(UE_BRIDGE_STOP_EXIT);
}

static void load_publish_bsp_sets_ready_and_can_clear_completeness(void)
{
	volatile struct ue_bridge_header *header;
	struct ue_bridge_load_writer *writer;
	volatile struct ue_bridge_bsp_entry *entries;

	start_bridge();
	header = fake_bridge_section();
	writer = ue_bridge_load_begin();
	/* a five-BSP table right after the root, as the map export lays it out */
	ue_bridge_load_reserve(writer, 1, sizeof(struct ue_bridge_load_root));
	test_root(header)->bsps.offset = ue_bridge_load_reserve(writer, 5, sizeof(struct ue_bridge_bsp_entry));
	test_root(header)->bsps.count = 5;
	ue_bridge_load_set_bsp_table(test_root(header)->bsps.offset, 5);
	entries = (volatile struct ue_bridge_bsp_entry *)((volatile uint8_t *)test_root(header) + test_root(header)->bsps.offset);
	ue_bridge_load_end(1);
	ue_bridge_load_publish_bsp(3, 1);
	UEB_CHECK(entries[3].ready == 1);
	UEB_CHECK(header->export_complete == 1);
	/* an index past the table is ignored, completeness included */
	ue_bridge_load_publish_bsp(5, 0);
	UEB_CHECK(header->export_complete == 1);
	ue_bridge_load_publish_bsp(4, 0);
	UEB_CHECK(header->export_complete == 0);
	ue_bridge_stop(UE_BRIDGE_STOP_EXIT);
}

static void load_bsp_table_is_the_games_own_not_the_roots(void)
{
	volatile struct ue_bridge_header *header;
	struct ue_bridge_load_writer *writer;
	volatile struct ue_bridge_bsp_entry *entries;
	uint32_t offset;

	start_bridge();
	header = fake_bridge_section();
	writer = ue_bridge_load_begin();
	ue_bridge_load_reserve(writer, 1, sizeof(struct ue_bridge_load_root));
	offset = ue_bridge_load_reserve(writer, 5, sizeof(struct ue_bridge_bsp_entry));
	ue_bridge_load_set_bsp_table(offset, 5);
	entries = (volatile struct ue_bridge_bsp_entry *)((volatile uint8_t *)test_root(header) + offset);
	ue_bridge_load_end(1);
	/* UE can write the region: a moved or grown table in the root must not move the game's writes */
	test_root(header)->bsps.offset = 0x100000u;
	test_root(header)->bsps.count = 1000;
	UEB_CHECK(ue_bridge_load_bsp_slot(2) == (struct ue_bridge_bsp_entry *)(uintptr_t)&entries[2]);
	UEB_CHECK(ue_bridge_load_bsp_slot(5) == 0);
	ue_bridge_load_publish_bsp(1, 1);
	UEB_CHECK(entries[1].ready == 1);
	/* a table that doesn't lie inside the region is refused */
	ue_bridge_load_set_bsp_table(header->load_region.size, 5);
	UEB_CHECK(ue_bridge_load_bsp_slot(0) == 0);
	ue_bridge_stop(UE_BRIDGE_STOP_EXIT);
}

static void tick_begin_end_publishes_records_and_epochs(void)
{
	volatile struct ue_bridge_header *header;
	struct ue_bridge_tick_writer *writer;
	struct ue_bridge_matrix node;
	static uint8_t out[TEST_TICK_SLOT_SIZE];
	uint32_t bytes = 0;
	const struct ue_bridge_tick_header *tick = (const struct ue_bridge_tick_header *)out;

	start_bridge();
	header = fake_bridge_section();
	ue_bridge_bump_load_epoch();
	memset(&node, 0, sizeof(node));
	node.scale = 1.0f;
	writer = ue_bridge_tick_begin(41);
	UEB_CHECK(writer != 0);
	UEB_CHECK(ue_bridge_tick_writer_add(writer, 0x00020001u, 3, 0, 0, 0, &node, 1));
	fake_now = 777;
	ue_bridge_tick_end(1);
	UEB_CHECK(ue_bridge_ring_read_newest_used((const volatile uint8_t *)header, &header->tick_ring,
		offsetof(struct ue_bridge_tick_header, used), out, sizeof(out), &bytes, 0) == UE_BRIDGE_READ_NEWEST);
	UEB_CHECK(tick->slot.id == 41);
	/* stamped when published, not when begun: latency is measured from here */
	UEB_CHECK(tick->slot.publish_qpc == 777);
	UEB_CHECK(tick->object_count == 1 && tick->load_epoch == 1 && tick->active_bsp == 1);
	UEB_CHECK(bytes == tick->used);
	ue_bridge_stop(UE_BRIDGE_STOP_EXIT);
}

static void frame_camera_is_published_or_marked_invalid(void)
{
	volatile struct ue_bridge_header *header;
	struct ue_bridge_frame_slot frame;
	struct ue_bridge_camera camera;

	start_bridge();
	header = fake_bridge_section();
	memset(&camera, 0, sizeof(camera));
	camera.position[1] = 2.5f;
	camera.vertical_fov = 1.2f;
	ue_bridge_publish_frame_camera(9, 0.5f, 30, &camera);
	UEB_CHECK(ue_bridge_ring_read_newest((const volatile uint8_t *)header, &header->frame_ring, &frame, sizeof(frame), 0) == UE_BRIDGE_READ_NEWEST);
	UEB_CHECK(frame.slot.id == 9 && frame.tick_id == 30 && frame.camera_valid == 1);
	UEB_CHECK(frame.camera_position[1] == 2.5f && frame.vertical_fov == 1.2f && frame.interpolation_fraction == 0.5f);
	ue_bridge_publish_frame_camera(10, 0.0f, 30, 0);
	ue_bridge_ring_read_newest((const volatile uint8_t *)header, &header->frame_ring, &frame, sizeof(frame), 0);
	UEB_CHECK(frame.slot.id == 10 && frame.camera_valid == 0);
	ue_bridge_stop(UE_BRIDGE_STOP_EXIT);
}

/* a renderer as UE publishes itself: attached, heartbeating, its own timeout */
static void attach_reader(volatile struct ue_bridge_header *header, uint64_t heartbeat)
{
	header->ue_hang_timeout_ms = 10000;
	header->ue_heartbeat_qpc = heartbeat;
	header->ue_attached = 1;
	header->ue_stopping = UE_BRIDGE_STOP_NONE;
}

static void hold_needs_a_fresh_reader(void)
{
	volatile struct ue_bridge_header *header;

	start_bridge();
	header = fake_bridge_section();
	fake_now = 200000000ull;
	UEB_CHECK(!ue_bridge_hold_begin());
	attach_reader(header, fake_now - 150000000ull);
	UEB_CHECK(!ue_bridge_hold_begin());
	header->ue_heartbeat_qpc = fake_now - 10000000ull;
	UEB_CHECK(ue_bridge_hold_begin());
	UEB_CHECK(header->game_holding == 1);
	ue_bridge_stop(UE_BRIDGE_STOP_EXIT);
}

static void hold_ends_when_ready(void)
{
	volatile struct ue_bridge_header *header;
	uint32_t held = 0;

	start_bridge();
	header = fake_bridge_section();
	ue_bridge_bump_load_epoch();
	fake_now = 100000000ull;
	attach_reader(header, fake_now);
	UEB_CHECK(ue_bridge_hold_begin());
	UEB_CHECK(ue_bridge_hold_poll(&held) == UE_BRIDGE_HOLD_WAITING);
	fake_now += 25000000ull;
	header->ue_heartbeat_qpc = fake_now;
	header->ue_ready = 1;
	UEB_CHECK(ue_bridge_hold_poll(&held) == UE_BRIDGE_HOLD_READY);
	UEB_CHECK(held == 2500);
	UEB_CHECK(header->game_holding == 0);
	UEB_CHECK(ue_bridge_hold_poll(&held) == UE_BRIDGE_HOLD_NONE);
	ue_bridge_stop(UE_BRIDGE_STOP_EXIT);
}

static void hold_ends_at_the_cap(void)
{
	volatile struct ue_bridge_header *header;
	uint32_t held = 0;

	start_bridge();
	header = fake_bridge_section();
	ue_bridge_bump_load_epoch();
	fake_now = 100000000ull;
	attach_reader(header, fake_now);
	ue_bridge_hold_begin();
	fake_now += 99990000ull;
	header->ue_heartbeat_qpc = fake_now;
	UEB_CHECK(ue_bridge_hold_poll(&held) == UE_BRIDGE_HOLD_WAITING);
	fake_now += 20000ull;
	header->ue_heartbeat_qpc = fake_now;
	UEB_CHECK(ue_bridge_hold_poll(&held) == UE_BRIDGE_HOLD_TIMED_OUT);
	UEB_CHECK(held >= UE_BRIDGE_LOAD_HOLD_MS);
	UEB_CHECK(header->game_holding == 0);
	ue_bridge_stop(UE_BRIDGE_STOP_EXIT);
}

static void hold_ends_when_ue_detaches(void)
{
	volatile struct ue_bridge_header *header;
	uint32_t held = 0;

	start_bridge();
	header = fake_bridge_section();
	/* ue_ready starts at 0: an epoch of 0 would read as ready */
	ue_bridge_bump_load_epoch();
	fake_now = 100000000ull;
	attach_reader(header, fake_now);
	ue_bridge_hold_begin();
	header->ue_attached = 0;
	UEB_CHECK(ue_bridge_hold_poll(&held) == UE_BRIDGE_HOLD_READER_GONE);
	attach_reader(header, fake_now);
	ue_bridge_hold_begin();
	header->ue_stopping = UE_BRIDGE_STOP_CRASH;
	UEB_CHECK(ue_bridge_hold_poll(&held) == UE_BRIDGE_HOLD_READER_GONE);
	ue_bridge_stop(UE_BRIDGE_STOP_EXIT);
}

static void hold_keeps_waiting_while_ue_is_in_a_debugger(void)
{
	volatile struct ue_bridge_header *header;
	uint32_t held = 0;

	start_bridge();
	header = fake_bridge_section();
	/* ue_ready starts at 0: an epoch of 0 would read as ready */
	ue_bridge_bump_load_epoch();
	fake_now = 100000000ull;
	attach_reader(header, fake_now);
	ue_bridge_hold_begin();
	/* a short timeout, so the 5 s old heartbeat below is stale and only the debugger keeps the reader present */
	header->ue_hang_timeout_ms = 2000;
	header->ue_debugger_attached = 1;
	fake_now += 50000000ull;
	/* a stale heartbeat under a debugger is a breakpoint, not a gone renderer; the cap still applies */
	UEB_CHECK(ue_bridge_hold_poll(&held) == UE_BRIDGE_HOLD_WAITING);
	ue_bridge_stop(UE_BRIDGE_STOP_EXIT);
}

/* the game's own geometry, whatever the header says: UE can write the header,
and the descriptor steers where the game writes */
static void tick_and_frame_writes_use_the_trusted_layout_not_the_header(void)
{
	volatile struct ue_bridge_header *header;
	const struct ue_bridge_layout *layout;
	struct ue_bridge_matrix node;
	struct ue_bridge_tick_writer *writer;
	const struct ue_bridge_tick_header *tick;
	const struct ue_bridge_frame_slot *frame;
	uint32_t index;
	uint32_t published;

	start_bridge();
	header = fake_bridge_section();
	layout = ue_bridge_trusted_layout();
	memset(&node, 0, sizeof(node));
	/* a write count that isn't 0 modulo either ring's slot count, so a corrupt count shows */
	for (index = 0; index < 3; index++)
	{
		ue_bridge_publish_tick(index);
		ue_bridge_publish_frame_camera(index, 0.0f, index, 0);
	}
	header->tick_ring.offset = layout->frame_ring.offset;
	header->tick_ring.slot_size = 0x80;
	header->tick_ring.slot_count = 2;
	header->frame_ring.offset = layout->tick_ring.offset;
	header->frame_ring.slot_size = 0x40;
	header->frame_ring.slot_count = 2;
	published = header->tick_ring.published;
	writer = ue_bridge_tick_begin(77);
	UEB_CHECK(writer != 0);
	/* three records: 48 + 3 * 68 bytes, past the corrupt slot_size */
	UEB_CHECK(ue_bridge_tick_writer_add(writer, 1, 0, 0, 0, 0, &node, 1));
	UEB_CHECK(ue_bridge_tick_writer_add(writer, 2, 0, 0, 0, 0, &node, 1));
	UEB_CHECK(ue_bridge_tick_writer_add(writer, 3, 0, 0, 0, 0, &node, 1));
	ue_bridge_tick_end(0);
	tick = (const struct ue_bridge_tick_header *)((const uint8_t *)header + layout->tick_ring.offset +
		(published % layout->tick_ring.slot_count) * layout->tick_ring.slot_size);
	UEB_CHECK(tick->slot.id == 77 && tick->object_count == 3 && !(tick->flags & UE_BRIDGE_TICK_TRUNCATED));
	published = header->frame_ring.published;
	ue_bridge_publish_frame_camera(88, 0.5f, 9, 0);
	frame = (const struct ue_bridge_frame_slot *)((const uint8_t *)header + layout->frame_ring.offset +
		(published % layout->frame_ring.slot_count) * layout->frame_ring.slot_size);
	UEB_CHECK(frame->slot.id == 88 && frame->tick_id == 9);
	ue_bridge_stop(UE_BRIDGE_STOP_EXIT);
}

static void hold_ends_exactly_at_the_cap(void)
{
	volatile struct ue_bridge_header *header;
	uint32_t held = 0;

	start_bridge();
	header = fake_bridge_section();
	ue_bridge_bump_load_epoch();
	fake_now = 100000000ull;
	attach_reader(header, fake_now);
	ue_bridge_hold_begin();
	fake_now += (uint64_t)UE_BRIDGE_LOAD_HOLD_MS * 10000ull;
	header->ue_heartbeat_qpc = fake_now;
	UEB_CHECK(ue_bridge_hold_poll(&held) == UE_BRIDGE_HOLD_TIMED_OUT);
	UEB_CHECK(held == UE_BRIDGE_LOAD_HOLD_MS);
	ue_bridge_stop(UE_BRIDGE_STOP_EXIT);
}

/* the watcher saw the renderer's process end: no ue_stopping needed, no staleness to wait out */
static void hold_ends_at_once_when_the_renderer_process_exits(void)
{
	volatile struct ue_bridge_header *header;
	uint32_t held = 0;

	start_bridge();
	header = fake_bridge_section();
	ue_bridge_bump_load_epoch();
	fake_now = 100000000ull;
	attach_reader(header, fake_now);
	UEB_CHECK(ue_bridge_hold_begin());
	UEB_CHECK(ue_bridge_hold_poll(&held) == UE_BRIDGE_HOLD_WAITING);
	ue_bridge_note_peer_exited(1);
	UEB_CHECK(!ue_bridge_reader_present());
	UEB_CHECK(ue_bridge_hold_poll(&held) == UE_BRIDGE_HOLD_READER_GONE);
	UEB_CHECK(header->game_holding == 0);
	/* a hold needs a reader: this one's process is gone, whatever the header still says */
	UEB_CHECK(!ue_bridge_hold_begin());
	ue_bridge_note_peer_exited(0);
	UEB_CHECK(ue_bridge_hold_begin());
	ue_bridge_stop(UE_BRIDGE_STOP_EXIT);
}

/* UE can write the whole header: a tick or an export stamped from a rewritten epoch would
pass a stale slot off as the current map's, or hide a new map */
static void epochs_are_the_games_own_not_read_back_from_the_header(void)
{
	volatile struct ue_bridge_header *header;
	struct ue_bridge_tick_writer *writer;
	static uint8_t out[TEST_TICK_SLOT_SIZE];
	uint32_t bytes = 0;
	const struct ue_bridge_tick_header *tick = (const struct ue_bridge_tick_header *)out;

	start_bridge();
	header = fake_bridge_section();
	UEB_CHECK(ue_bridge_load_epoch() == 0);
	ue_bridge_bump_load_epoch();
	ue_bridge_bump_state_epoch();
	ue_bridge_bump_state_epoch();
	header->load_epoch = 0x7777;
	header->state_epoch = 0x8888;
	UEB_CHECK(ue_bridge_load_epoch() == 1);
	writer = ue_bridge_tick_begin(1);
	UEB_CHECK(writer != 0);
	ue_bridge_tick_end(0);
	UEB_CHECK(ue_bridge_ring_read_newest_used((const volatile uint8_t *)header, &header->tick_ring,
		offsetof(struct ue_bridge_tick_header, used), out, sizeof(out), &bytes, 0) == UE_BRIDGE_READ_NEWEST);
	UEB_CHECK(tick->load_epoch == 1 && tick->state_epoch == 2);
	/* a bump counts from the game's own epoch, and writes it back over the rewrite */
	ue_bridge_bump_load_epoch();
	ue_bridge_bump_state_epoch();
	UEB_CHECK(header->load_epoch == 2 && header->state_epoch == 3);
	ue_bridge_stop(UE_BRIDGE_STOP_EXIT);
}

static void load_end_and_append_follow_the_games_epoch_not_the_headers(void)
{
	volatile struct ue_bridge_header *header;
	struct ue_bridge_load_writer *writer;

	start_bridge();
	header = fake_bridge_section();
	ue_bridge_bump_load_epoch();
	writer = ue_bridge_load_begin();
	header->load_epoch = 9;
	ue_bridge_load_end(1);
	UEB_CHECK(header->export_epoch == 1);
	UEB_CHECK(ue_bridge_load_append() == writer);
	ue_bridge_stop(UE_BRIDGE_STOP_EXIT);
}

static void hold_ready_means_the_games_epoch_not_the_headers(void)
{
	volatile struct ue_bridge_header *header;
	uint32_t held = 0;

	start_bridge();
	header = fake_bridge_section();
	ue_bridge_bump_load_epoch();
	fake_now = 100000000ull;
	attach_reader(header, fake_now);
	UEB_CHECK(ue_bridge_hold_begin());
	header->load_epoch = 0x55;
	header->ue_ready = 0x55;
	UEB_CHECK(ue_bridge_hold_poll(&held) == UE_BRIDGE_HOLD_WAITING);
	header->ue_ready = 1;
	UEB_CHECK(ue_bridge_hold_poll(&held) == UE_BRIDGE_HOLD_READY);
	ue_bridge_stop(UE_BRIDGE_STOP_EXIT);
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
	{ "core_log_path_that_does_not_fit_is_published_empty", core_log_path_that_does_not_fit_is_published_empty },
	{ "core_log_path_that_just_fits_is_published_whole", core_log_path_that_just_fits_is_published_whole },
	{ "core_log_path_from_a_silently_failing_conversion_is_empty", core_log_path_from_a_silently_failing_conversion_is_empty },
	{ "core_log_path_goes_through_path_to_utf8", core_log_path_goes_through_path_to_utf8 },
	{ "core_null_log_path_is_empty", core_null_log_path_is_empty },
	{ "core_publish_tick_and_frame", core_publish_tick_and_frame },
	{ "core_publish_frame_rate_writes_both_fields_and_republishes", core_publish_frame_rate_writes_both_fields_and_republishes },
	{ "core_epochs_bump", core_epochs_bump },
	{ "core_busy_flag_round_trips", core_busy_flag_round_trips },
	{ "core_publish_stopping_sets_flag_only_when_active", core_publish_stopping_sets_flag_only_when_active },
	{ "core_heartbeat_tracks_qpc_and_debugger", core_heartbeat_tracks_qpc_and_debugger },
	{ "core_stop_clears_own_directory_entry_and_unmaps", core_stop_clears_own_directory_entry_and_unmaps },
	{ "core_stop_keeps_a_newer_games_directory_entry", core_stop_keeps_a_newer_games_directory_entry },
	{ "core_section_failure_starts_nothing", core_section_failure_starts_nothing },
	{ "core_start_fails_when_the_section_already_exists", core_start_fails_when_the_section_already_exists },
	{ "core_start_succeeds_when_the_section_is_new", core_start_succeeds_when_the_section_is_new },
	{ "core_directory_failure_unmaps_section", core_directory_failure_unmaps_section },
	{ "core_restart_after_stop", core_restart_after_stop },
	{ "core_second_start_while_active_is_a_no_op", core_second_start_while_active_is_a_no_op },
	{ "core_header_is_complete_when_the_directory_is_mapped", core_header_is_complete_when_the_directory_is_mapped },
	{ "core_stop_writes_before_it_unmaps", core_stop_writes_before_it_unmaps },
	{ "core_stop_hides_the_header_from_the_crash_filter_before_unmapping_it", core_stop_hides_the_header_from_the_crash_filter_before_unmapping_it },
	{ "core_start_and_stop_each_take_the_directory_lock_once", core_start_and_stop_each_take_the_directory_lock_once },
	{ "core_directory_writes_begin_with_the_sequence", core_directory_writes_begin_with_the_sequence },
	{ "core_concurrent_reader_never_sees_a_mixed_directory_entry", core_concurrent_reader_never_sees_a_mixed_directory_entry },
	{ "core_concurrent_reader_never_sees_a_tick_without_its_payload", core_concurrent_reader_never_sees_a_tick_without_its_payload },
	{ "load_begin_marks_the_sequence_odd_and_clears_the_root", load_begin_marks_the_sequence_odd_and_clears_the_root },
	{ "load_begin_uses_the_game_layout_not_the_header", load_begin_uses_the_game_layout_not_the_header },
	{ "load_end_publishes_the_epoch_and_completeness", load_end_publishes_the_epoch_and_completeness },
	{ "load_append_continues_after_the_export_of_this_epoch_only", load_append_continues_after_the_export_of_this_epoch_only },
	{ "load_publish_bsp_sets_ready_and_can_clear_completeness", load_publish_bsp_sets_ready_and_can_clear_completeness },
	{ "load_bsp_table_is_the_games_own_not_the_roots", load_bsp_table_is_the_games_own_not_the_roots },
	{ "tick_begin_end_publishes_records_and_epochs", tick_begin_end_publishes_records_and_epochs },
	{ "frame_camera_is_published_or_marked_invalid", frame_camera_is_published_or_marked_invalid },
	{ "hold_needs_a_fresh_reader", hold_needs_a_fresh_reader },
	{ "hold_ends_when_ready", hold_ends_when_ready },
	{ "hold_ends_at_the_cap", hold_ends_at_the_cap },
	{ "hold_ends_when_ue_detaches", hold_ends_when_ue_detaches },
	{ "hold_keeps_waiting_while_ue_is_in_a_debugger", hold_keeps_waiting_while_ue_is_in_a_debugger },
	{ "tick_and_frame_writes_use_the_trusted_layout_not_the_header", tick_and_frame_writes_use_the_trusted_layout_not_the_header },
	{ "hold_ends_exactly_at_the_cap", hold_ends_exactly_at_the_cap },
	{ "hold_ends_at_once_when_the_renderer_process_exits", hold_ends_at_once_when_the_renderer_process_exits },
	{ "epochs_are_the_games_own_not_read_back_from_the_header", epochs_are_the_games_own_not_read_back_from_the_header },
	{ "load_end_and_append_follow_the_games_epoch_not_the_headers", load_end_and_append_follow_the_games_epoch_not_the_headers },
	{ "hold_ready_means_the_games_epoch_not_the_headers", hold_ready_means_the_games_epoch_not_the_headers },
	{ 0, 0 }
};

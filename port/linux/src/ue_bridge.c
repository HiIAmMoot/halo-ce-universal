/*
UE_BRIDGE.C

See ue_bridge.h.
*/

#include "ue_bridge.h"

#include "../../ue_bridge/ue_bridge_ring.h"

#include <stdio.h>
#include <string.h>

static struct
{
	const struct ue_bridge_os *os;
	void *section_view;
	void *section_handle;
	void *directory_view;
	void *directory_handle;
	uint64_t session_id;
	uint32_t section_size;
	struct ue_bridge_layout layout;
	/* the epochs as the game counted them: the header's copies sit in memory UE can write */
	uint32_t load_epoch;
	uint32_t state_epoch;
	/* the published count of truncated ticks, kept here for the same reason, and the epoch the
	first of them was logged in (valid only while truncation_logged) */
	uint32_t truncated_ticks;
	uint32_t truncation_logged_epoch;
	int truncation_logged;
	struct ue_bridge_load_writer load_writer;
	int load_live;
	uint32_t load_live_epoch;
	/* the BSP table as the game laid it out: the root's copy sits in memory UE can write */
	uint32_t bsp_offset;
	uint32_t bsp_count;
	struct ue_bridge_tick_writer tick_writer;
	volatile struct ue_bridge_slot *tick_slot;
	int holding;
	uint64_t hold_started;
	/* written by the watcher thread, read by the game's */
	int peer_exited;
} bridge;

static volatile struct ue_bridge_header *bridge_header(void)
{
	return (volatile struct ue_bridge_header *)bridge.section_view;
}

static void bridge_log(const char *message)
{
	if (bridge.os && bridge.os->log)
		bridge.os->log(message);
}

static void copy_string(volatile char *destination, size_t capacity, const char *source)
{
	size_t index = 0;

	if (source)
	{
		for (; index + 1 < capacity && source[index]; index++)
			destination[index] = source[index];
	}
	destination[index] = 0;
}

/* A path that doesn't fit is published empty, not cut: a cut could split a UTF-8
sequence, and UE would read a path that is not the game's. */
static void copy_path(volatile char *destination, size_t capacity, const char *source)
{
	if (source && strlen(source) < capacity)
		copy_string(destination, capacity, source);
	else
		destination[0] = 0;
}

static void directory_lock(void)
{
	if (bridge.os->lock_directory)
		bridge.os->lock_directory();
}

static void directory_unlock(void)
{
	if (bridge.os->unlock_directory)
		bridge.os->unlock_directory();
}

/* the odd sequence to write the entry under; an entry a dead game left odd
stays odd until this write ends it even */
static uint32_t directory_begin_write(volatile struct ue_bridge_directory *directory)
{
	uint32_t odd = ueb_load_u32(&directory->sequence) | 1u;

	ueb_store_u32(&directory->sequence, odd);
	ueb_fence();
	return odd;
}

static void directory_end_write(volatile struct ue_bridge_directory *directory, uint32_t odd)
{
	ueb_fence();
	ueb_store_u32(&directory->sequence, odd + 1u);
}

static void publish_directory(volatile struct ue_bridge_directory *directory, uint32_t pid, const char *section_name)
{
	uint32_t odd = directory_begin_write(directory);

	directory->magic = UE_BRIDGE_MAGIC;
	directory->version = UE_BRIDGE_VERSION;
	directory->game_pid = pid;
	directory->session_id = bridge.session_id;
	copy_string(directory->section_name, sizeof(directory->section_name), section_name);
	directory_end_write(directory, odd);
}

static void withdraw_directory(volatile struct ue_bridge_directory *directory)
{
	uint32_t sequence = directory_begin_write(directory);

	/* a newer game has written itself in since: its entry stays. Compared
	inside the lock and the write section, or that game could write in
	between and lose its entry to this one. */
	if (directory->session_id == bridge.session_id)
	{
		directory->game_pid = 0;
		directory->session_id = 0;
		directory->section_name[0] = 0;
	}
	directory_end_write(directory, sequence);
}

int ue_bridge_start(const struct ue_bridge_settings *settings, const struct ue_bridge_os *os)
{
	char section_name[UE_BRIDGE_NAME_CHARS];
	volatile struct ue_bridge_header *header;
	struct ue_bridge_layout layout;
	uint32_t pid;

	if (bridge.section_view || !settings->enabled)
		return bridge.section_view != 0;
	if (!ue_bridge_layout_compute(settings->section_size, settings->tick_slot_size, &layout))
	{
		if (os->log)
			os->log("ue bridge: the section size leaves no room for the rings and a load region");
		return 0;
	}
	bridge.os = os;
	pid = os->pid();
	bridge.session_id = os->random64();
	/* the directory's 0 means "no game" */
	if (!bridge.session_id)
		bridge.session_id = 1;
	snprintf(section_name, sizeof(section_name), "Local\\HaloCEUE.Bridge.%lu.%016llx",
		(unsigned long)pid, (unsigned long long)bridge.session_id);
	bridge.section_view = (os->map_new_section ? os->map_new_section : os->map_section)(section_name, settings->section_size, &bridge.section_handle);
	if (!bridge.section_view)
	{
		bridge_log("ue bridge: cannot create the bridge section; the bridge is off");
		memset(&bridge, 0, sizeof(bridge));
		return 0;
	}
	header = bridge_header();
	header->magic = UE_BRIDGE_MAGIC;
	header->version = UE_BRIDGE_VERSION;
	header->header_size = UE_BRIDGE_HEADER_SIZE;
	header->section_size = settings->section_size;
	header->session_id = bridge.session_id;
	header->qpc_frequency = os->qpc_frequency();
	header->game_pid = pid;
	header->max_objects = settings->max_objects;
	header->game_hang_timeout_ms = UE_BRIDGE_DEFAULT_HANG_TIMEOUT_MS;
	header->tick_ring = layout.tick_ring;
	header->frame_ring = layout.frame_ring;
	header->load_region = layout.load_region;
	bridge.section_size = settings->section_size;
	bridge.layout = layout;
	if (os->path_to_utf8 && settings->log_path)
	{
		char utf8[UE_BRIDGE_PATH_BYTES];

		/* a conversion that fails without writing must leave an empty string, not stack garbage */
		utf8[0] = 0;
		os->path_to_utf8(settings->log_path, utf8, sizeof(utf8));
		copy_path(header->game_log_path, sizeof(header->game_log_path), utf8);
	}
	else
	{
		copy_path(header->game_log_path, sizeof(header->game_log_path), settings->log_path);
	}
	/* UE copies debug.txt from this path: say why it will find none */
	if (settings->log_path && settings->log_path[0] && header->game_log_path[0] == 0)
		bridge_log("ue bridge: the log path doesn't fit the header or doesn't convert to UTF-8; UE gets no debug.txt path");
	ue_bridge_heartbeat();

	bridge.directory_view = os->map_section(UE_BRIDGE_DIRECTORY_NAME, UE_BRIDGE_DIRECTORY_SIZE, &bridge.directory_handle);
	if (!bridge.directory_view)
	{
		bridge_log("ue bridge: cannot open the bridge directory");
		os->unmap_section(bridge.section_view, bridge.section_handle);
		memset(&bridge, 0, sizeof(bridge));
		return 0;
	}
	/* last: UE finds the section through this entry, so the header above must be complete first */
	directory_lock();
	publish_directory((volatile struct ue_bridge_directory *)bridge.directory_view, pid, section_name);
	directory_unlock();
	return 1;
}

void ue_bridge_publish_stopping(uint32_t stopping)
{
	volatile struct ue_bridge_header *header = bridge_header();

	if (header)
		ueb_store_u32(&header->game_stopping, stopping);
}

void ue_bridge_stop(uint32_t stopping)
{
	volatile struct ue_bridge_header *header = bridge_header();
	void *section_view = bridge.section_view;

	if (!header)
		return;
	ue_bridge_publish_stopping(stopping);
	directory_lock();
	withdraw_directory((volatile struct ue_bridge_directory *)bridge.directory_view);
	directory_unlock();
	/* the crash filter reads ue_bridge_header() from the crashing thread, and a
	crash during the stop must not make it read a view that is being unmapped.
	This narrows the window but cannot close it: a filter that already holds the
	pointer can still touch the view after the unmap. */
	__atomic_store_n(&bridge.section_view, NULL, __ATOMIC_SEQ_CST);
	bridge.os->unmap_section(bridge.directory_view, bridge.directory_handle);
	bridge.os->unmap_section(section_view, bridge.section_handle);
	memset(&bridge, 0, sizeof(bridge));
}

int ue_bridge_active(void)
{
	return bridge.section_view != 0;
}

int ue_bridge_snapshot_at_rest(const void *previous, const void *latest, uint32_t bytes, int continuing)
{
	return bridge.section_view != 0 && ue_bridge_nodes_at_rest(previous, latest, bytes, continuing);
}

uint64_t ue_bridge_session_id(void)
{
	return bridge.session_id;
}

volatile struct ue_bridge_header *ue_bridge_header(void)
{
	return bridge_header();
}

volatile uint8_t *ue_bridge_section(void)
{
	return (volatile uint8_t *)bridge.section_view;
}

uint32_t ue_bridge_section_size(void)
{
	return bridge.section_view ? bridge.section_size : 0;
}

const struct ue_bridge_layout *ue_bridge_trusted_layout(void)
{
	return bridge.section_view ? &bridge.layout : 0;
}

/* a slot of a ring at the game's own geometry; only the write count is taken
from the shared descriptor (ue_bridge_ring_end_write advances it there) */
static volatile struct ue_bridge_slot *trusted_begin_write(volatile struct ue_bridge_header *header,
	volatile struct ue_bridge_ring_desc *shared, const struct ue_bridge_ring_desc *trusted)
{
	struct ue_bridge_ring_desc ring = *trusted;

	ring.published = ueb_load_u32(&shared->published);
	return ue_bridge_ring_begin_write((volatile uint8_t *)header, &ring);
}

struct ue_bridge_tick_writer *ue_bridge_tick_begin(uint64_t tick)
{
	volatile struct ue_bridge_header *header = bridge_header();

	if (!header)
		return NULL;
	bridge.tick_slot = trusted_begin_write(header, &header->tick_ring, &bridge.layout.tick_ring);
	bridge.tick_slot->id = tick;
	ue_bridge_tick_writer_begin(&bridge.tick_writer, (void *)(uintptr_t)bridge.tick_slot, bridge.layout.tick_ring.slot_size);
	return &bridge.tick_writer;
}

void ue_bridge_tick_end(int16_t active_bsp)
{
	volatile struct ue_bridge_header *header = bridge_header();

	if (!header || !bridge.tick_slot)
		return;
	ue_bridge_tick_writer_end(&bridge.tick_writer, bridge.load_epoch, bridge.state_epoch, active_bsp);
	if (bridge.tick_writer.flags & UE_BRIDGE_TICK_TRUNCATED)
	{
		ueb_store_u32(&header->game_truncated_ticks, ++bridge.truncated_ticks);
		/* once per epoch: a map that overflows its slot would otherwise log every tick */
		if (!bridge.truncation_logged || bridge.truncation_logged_epoch != bridge.load_epoch)
		{
			char message[160];

			bridge.truncation_logged = 1;
			bridge.truncation_logged_epoch = bridge.load_epoch;
			snprintf(message, sizeof(message), "ue bridge: a tick holds only %lu of its objects (the slot is full); the rest stay frozen in the renderer (%lu truncated ticks so far)",
				(unsigned long)bridge.tick_writer.object_count, (unsigned long)bridge.truncated_ticks);
			bridge_log(message);
		}
	}
	/* stamped as it is published: 10.1 measures from here */
	bridge.tick_slot->publish_qpc = bridge.os->qpc();
	ue_bridge_ring_end_write(&header->tick_ring, bridge.tick_slot);
	bridge.tick_slot = NULL;
}

void ue_bridge_publish_tick(uint64_t tick)
{
	if (ue_bridge_tick_begin(tick))
		ue_bridge_tick_end(-1);
}

void ue_bridge_publish_frame_camera(uint64_t frame, float interpolation_fraction, uint64_t tick, const struct ue_bridge_camera *camera)
{
	volatile struct ue_bridge_header *header = bridge_header();
	volatile struct ue_bridge_frame_slot *slot;
	int index;

	if (!header)
		return;
	slot = (volatile struct ue_bridge_frame_slot *)trusted_begin_write(header, &header->frame_ring, &bridge.layout.frame_ring);
	slot->slot.id = frame;
	slot->interpolation_fraction = interpolation_fraction;
	slot->tick_id = tick;
	slot->camera_valid = camera ? 1u : 0u;
	if (camera)
	{
		for (index = 0; index < 3; index++)
		{
			slot->camera_position[index] = camera->position[index];
			slot->camera_forward[index] = camera->forward[index];
			slot->camera_up[index] = camera->up[index];
		}
		slot->vertical_fov = camera->vertical_fov;
		slot->z_near = camera->z_near;
		slot->z_far = camera->z_far;
	}
	slot->slot.publish_qpc = bridge.os->qpc();
	ue_bridge_ring_end_write(&header->frame_ring, &slot->slot);
}

void ue_bridge_publish_frame(uint64_t frame, float interpolation_fraction)
{
	ue_bridge_publish_frame_camera(frame, interpolation_fraction, 0, NULL);
}

void ue_bridge_publish_frame_rate(uint32_t refresh_hz, uint32_t target_hz)
{
	volatile struct ue_bridge_header *header = bridge_header();

	if (!header)
		return;
	/* two stores, not one atomic pair: UE re-evaluates both every tick, so a mixed read lasts one tick */
	ueb_store_u32(&header->game_refresh_hz, refresh_hz);
	ueb_store_u32(&header->game_frame_target_hz, target_hz);
}

void ue_bridge_heartbeat(void)
{
	volatile struct ue_bridge_header *header = bridge_header();

	if (!header)
		return;
	/* ueb_store_u64 must stay: a plain 64-bit store tears on i686, and no test
	can catch that on x86 at run time */
	ueb_store_u64(&header->game_heartbeat_qpc, bridge.os->qpc());
	ueb_store_u32(&header->game_debugger_attached, bridge.os->debugger_present() ? 1u : 0u);
}

void ue_bridge_bump_load_epoch(void)
{
	volatile struct ue_bridge_header *header = bridge_header();

	if (header)
		ueb_store_u32(&header->load_epoch, ++bridge.load_epoch);
}

uint32_t ue_bridge_load_epoch(void)
{
	return bridge.section_view ? bridge.load_epoch : 0u;
}

void ue_bridge_bump_state_epoch(void)
{
	volatile struct ue_bridge_header *header = bridge_header();

	if (header)
		ueb_store_u32(&header->state_epoch, ++bridge.state_epoch);
}

struct ue_bridge_load_writer *ue_bridge_load_begin(void)
{
	volatile struct ue_bridge_header *header = bridge_header();
	volatile uint8_t *region;

	if (!header)
		return NULL;
	/* odd before the first byte changes: a UE copying the region throws its copy away */
	ueb_store_u32(&header->load_sequence, ueb_load_u32(&header->load_sequence) | 1u);
	ueb_store_u32(&header->export_epoch, 0);
	ueb_fence();
	/* the game's own layout, never the header's copy: UE can write the header,
	and these bytes steer the game's writes */
	region = (volatile uint8_t *)header + bridge.layout.load_region.offset;
	memset((void *)(uintptr_t)region, 0, sizeof(struct ue_bridge_load_root));
	ue_bridge_load_writer_init(&bridge.load_writer, region, bridge.layout.load_region.size, 0);
	bridge.load_live = 0;
	bridge.bsp_offset = 0;
	bridge.bsp_count = 0;
	return &bridge.load_writer;
}

void ue_bridge_load_end(int complete)
{
	volatile struct ue_bridge_header *header = bridge_header();

	if (!header)
		return;
	ueb_store_u32(&header->export_complete, complete ? 1u : 0u);
	ueb_store_u32(&header->export_epoch, bridge.load_epoch);
	ueb_fence();
	ueb_store_u32(&header->load_sequence, (ueb_load_u32(&header->load_sequence) | 1u) + 1u);
	bridge.load_live = 1;
	bridge.load_live_epoch = bridge.load_epoch;
}

void ue_bridge_load_discard(void)
{
	bridge.load_live = 0;
}

struct ue_bridge_load_writer *ue_bridge_load_append(void)
{
	volatile struct ue_bridge_header *header = bridge_header();

	if (!header || !bridge.load_live || bridge.load_live_epoch != bridge.load_epoch)
		return NULL;
	return &bridge.load_writer;
}

struct ue_bridge_load_root *ue_bridge_load_root(void)
{
	volatile struct ue_bridge_header *header = bridge_header();

	return header ? (struct ue_bridge_load_root *)(uintptr_t)((volatile uint8_t *)header + bridge.layout.load_region.offset) : NULL;
}

void ue_bridge_load_set_bsp_table(uint32_t offset, uint32_t count)
{
	bridge.bsp_offset = 0;
	bridge.bsp_count = 0;
	if (!bridge.section_view || !ue_bridge_table_valid(offset, count, sizeof(struct ue_bridge_bsp_entry), bridge.layout.load_region.size))
		return;
	bridge.bsp_offset = offset;
	bridge.bsp_count = count;
}

struct ue_bridge_bsp_entry *ue_bridge_load_bsp_slot(uint32_t index)
{
	if (!bridge.section_view || index >= bridge.bsp_count)
		return NULL;
	return (struct ue_bridge_bsp_entry *)(uintptr_t)((volatile uint8_t *)bridge.section_view + bridge.layout.load_region.offset + bridge.bsp_offset) + index;
}

void ue_bridge_load_publish_bsp(short structure_bsp_index, int complete)
{
	volatile struct ue_bridge_header *header = bridge_header();
	struct ue_bridge_bsp_entry *entry;

	if (!header || structure_bsp_index < 0)
		return;
	entry = ue_bridge_load_bsp_slot((uint32_t)structure_bsp_index);
	if (!entry)
		return;
	if (!complete)
		ueb_store_u32(&header->export_complete, 0);
	/* last, with release order: UE reads the entry only once this is 1 */
	ueb_store_u32(&entry->ready, 1u);
}

void ue_bridge_set_busy(int busy)
{
	volatile struct ue_bridge_header *header = bridge_header();

	if (header)
		ueb_store_u32(&header->game_busy, busy ? 1u : 0u);
}

static uint32_t elapsed_ms(uint64_t since)
{
	uint64_t frequency = bridge.os->qpc_frequency();
	uint64_t now = bridge.os->qpc();
	uint64_t milliseconds;

	/* a heartbeat UE stamped after this clock read is fresh, not 584 years old */
	if (!frequency || now <= since)
		return 0u;
	milliseconds = (now - since) * 1000u / frequency;
	/* a heartbeat of 0 (never written) is days old: it must not wrap into a fresh one */
	return milliseconds > UINT32_MAX ? UINT32_MAX : (uint32_t)milliseconds;
}

void ue_bridge_note_peer_exited(int exited)
{
	__atomic_store_n(&bridge.peer_exited, exited ? 1 : 0, __ATOMIC_SEQ_CST);
}

int ue_bridge_reader_present(void)
{
	volatile struct ue_bridge_header *header = bridge_header();
	uint32_t timeout;

	if (!header || !ueb_load_u32(&header->ue_attached) || ueb_load_u32(&header->ue_stopping) != UE_BRIDGE_STOP_NONE)
		return 0;
	/* a renderer killed or crashed hard publishes neither a stop nor a detach, and its
	heartbeat only goes stale after its own hang timeout, which is the hold's cap */
	if (__atomic_load_n(&bridge.peer_exited, __ATOMIC_SEQ_CST))
		return 0;
	if (ueb_load_u32(&header->ue_debugger_attached))
		return 1;
	timeout = ueb_load_u32(&header->ue_hang_timeout_ms);
	return elapsed_ms(ueb_load_u64(&header->ue_heartbeat_qpc)) < (timeout ? timeout : UE_BRIDGE_DEFAULT_HANG_TIMEOUT_MS);
}

int ue_bridge_hold_begin(void)
{
	volatile struct ue_bridge_header *header = bridge_header();

	if (!header || !ue_bridge_reader_present())
		return 0;
	bridge.holding = 1;
	bridge.hold_started = bridge.os->qpc();
	ueb_store_u32(&header->game_holding, 1u);
	return 1;
}

enum ue_bridge_hold ue_bridge_hold_poll(uint32_t *held_ms)
{
	volatile struct ue_bridge_header *header = bridge_header();
	enum ue_bridge_hold state;

	if (!header || !bridge.holding)
		return UE_BRIDGE_HOLD_NONE;
	*held_ms = elapsed_ms(bridge.hold_started);
	if (ueb_load_u32(&header->ue_ready) == bridge.load_epoch)
		state = UE_BRIDGE_HOLD_READY;
	else if (!ue_bridge_reader_present())
		state = UE_BRIDGE_HOLD_READER_GONE;
	else if (*held_ms >= UE_BRIDGE_LOAD_HOLD_MS)
		state = UE_BRIDGE_HOLD_TIMED_OUT;
	else
		return UE_BRIDGE_HOLD_WAITING;
	bridge.holding = 0;
	ueb_store_u32(&header->game_holding, 0);
	return state;
}

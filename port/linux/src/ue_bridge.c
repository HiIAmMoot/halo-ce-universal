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
	uint32_t pid;

	if (bridge.section_view || !settings->enabled)
		return bridge.section_view != 0;
	bridge.os = os;
	pid = os->pid();
	bridge.session_id = os->random64();
	/* the directory's 0 means "no game" */
	if (!bridge.session_id)
		bridge.session_id = 1;
	snprintf(section_name, sizeof(section_name), "Local\\HaloCEUE.Bridge.%lu.%016llx",
		(unsigned long)pid, (unsigned long long)bridge.session_id);
	bridge.section_view = os->map_section(section_name, UE_BRIDGE_SECTION_SIZE, &bridge.section_handle);
	if (!bridge.section_view)
	{
		bridge_log("ue bridge: cannot create the bridge section");
		memset(&bridge, 0, sizeof(bridge));
		return 0;
	}
	header = bridge_header();
	header->magic = UE_BRIDGE_MAGIC;
	header->version = UE_BRIDGE_VERSION;
	header->header_size = UE_BRIDGE_HEADER_SIZE;
	header->section_size = UE_BRIDGE_SECTION_SIZE;
	header->session_id = bridge.session_id;
	header->qpc_frequency = os->qpc_frequency();
	header->game_pid = pid;
	header->max_objects = settings->max_objects;
	header->game_hang_timeout_ms = UE_BRIDGE_DEFAULT_HANG_TIMEOUT_MS;
	header->tick_ring.offset = UE_BRIDGE_HEADER_SIZE;
	header->tick_ring.slot_size = UE_BRIDGE_SLOT_SIZE;
	header->tick_ring.slot_count = UE_BRIDGE_TICK_SLOTS;
	header->frame_ring.offset = UE_BRIDGE_HEADER_SIZE + UE_BRIDGE_SLOT_SIZE * UE_BRIDGE_TICK_SLOTS;
	header->frame_ring.slot_size = UE_BRIDGE_SLOT_SIZE;
	header->frame_ring.slot_count = UE_BRIDGE_FRAME_SLOTS;
	if (os->path_to_utf8 && settings->log_path)
	{
		char utf8[UE_BRIDGE_PATH_BYTES];

		os->path_to_utf8(settings->log_path, utf8, sizeof(utf8));
		copy_string(header->game_log_path, sizeof(header->game_log_path), utf8);
	}
	else
	{
		copy_string(header->game_log_path, sizeof(header->game_log_path), settings->log_path);
	}
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
	bridge.os->unmap_section(bridge.directory_view, bridge.directory_handle);
	bridge.os->unmap_section(section_view, bridge.section_handle);
	memset(&bridge, 0, sizeof(bridge));
}

int ue_bridge_active(void)
{
	return bridge.section_view != 0;
}

uint64_t ue_bridge_session_id(void)
{
	return bridge.session_id;
}

volatile struct ue_bridge_header *ue_bridge_header(void)
{
	return bridge_header();
}

void ue_bridge_publish_tick(uint64_t tick)
{
	volatile struct ue_bridge_header *header = bridge_header();
	volatile struct ue_bridge_slot *slot;

	if (!header)
		return;
	slot = ue_bridge_ring_begin_write((volatile uint8_t *)header, &header->tick_ring);
	slot->id = tick;
	slot->publish_qpc = bridge.os->qpc();
	ue_bridge_ring_end_write(&header->tick_ring, slot);
}

void ue_bridge_publish_frame(uint64_t frame, float interpolation_fraction)
{
	volatile struct ue_bridge_header *header = bridge_header();
	volatile struct ue_bridge_frame_slot *slot;

	if (!header)
		return;
	slot = (volatile struct ue_bridge_frame_slot *)ue_bridge_ring_begin_write((volatile uint8_t *)header, &header->frame_ring);
	slot->slot.id = frame;
	slot->slot.publish_qpc = bridge.os->qpc();
	slot->interpolation_fraction = interpolation_fraction;
	ue_bridge_ring_end_write(&header->frame_ring, &slot->slot);
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
		ueb_store_u32(&header->load_epoch, header->load_epoch + 1u);
}

void ue_bridge_bump_state_epoch(void)
{
	volatile struct ue_bridge_header *header = bridge_header();

	if (header)
		ueb_store_u32(&header->state_epoch, header->state_epoch + 1u);
}

void ue_bridge_set_busy(int busy)
{
	volatile struct ue_bridge_header *header = bridge_header();

	if (header)
		ueb_store_u32(&header->game_busy, busy ? 1u : 0u);
}

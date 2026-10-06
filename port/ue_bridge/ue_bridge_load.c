/*
UE_BRIDGE_LOAD.C

See ue_bridge_load.h.
*/

#include "ue_bridge_load.h"

int ue_bridge_layout_compute(uint32_t section_size, uint32_t tick_slot_size, struct ue_bridge_layout *layout)
{
	uint64_t tick_end, frame_end, load_offset;

	if (section_size % 4096u != 0 || section_size > UE_BRIDGE_MAX_SECTION_SIZE || tick_slot_size % 8u != 0 ||
		tick_slot_size < sizeof(struct ue_bridge_tick_header))
	{
		return 0;
	}
	tick_end = (uint64_t)UE_BRIDGE_HEADER_SIZE + (uint64_t)tick_slot_size * UE_BRIDGE_TICK_SLOTS;
	frame_end = tick_end + (uint64_t)UE_BRIDGE_FRAME_SLOT_SIZE * UE_BRIDGE_FRAME_SLOTS;
	/* page-aligned, so UE could map the region alone later */
	load_offset = (frame_end + 4095u) & ~(uint64_t)4095u;
	if (load_offset + UE_BRIDGE_MIN_LOAD_SIZE > section_size)
		return 0;
	layout->tick_ring.offset = UE_BRIDGE_HEADER_SIZE;
	layout->tick_ring.slot_size = tick_slot_size;
	layout->tick_ring.slot_count = UE_BRIDGE_TICK_SLOTS;
	layout->tick_ring.published = 0;
	layout->frame_ring.offset = (uint32_t)tick_end;
	layout->frame_ring.slot_size = UE_BRIDGE_FRAME_SLOT_SIZE;
	layout->frame_ring.slot_count = UE_BRIDGE_FRAME_SLOTS;
	layout->frame_ring.published = 0;
	layout->load_region.offset = (uint32_t)load_offset;
	layout->load_region.size = section_size - (uint32_t)load_offset;
	return 1;
}

void ue_bridge_load_writer_init(struct ue_bridge_load_writer *writer, volatile uint8_t *base, uint32_t capacity, uint32_t used)
{
	writer->base = base;
	writer->capacity = capacity;
	writer->used = used;
	writer->overflow = 0;
}

uint32_t ue_bridge_load_reserve(struct ue_bridge_load_writer *writer, uint32_t count, uint32_t element_size)
{
	uint64_t start = ((uint64_t)writer->used + 7u) & ~(uint64_t)7u;
	uint64_t end = start + (uint64_t)count * element_size;

	if (writer->overflow || end > writer->capacity)
	{
		writer->overflow = 1;
		return UE_BRIDGE_NO_OFFSET;
	}
	writer->used = (uint32_t)end;
	return (uint32_t)start;
}

volatile void *ue_bridge_load_at(const struct ue_bridge_load_writer *writer, uint32_t offset)
{
	return writer->base + offset;
}

void *ue_bridge_load_pointer(const struct ue_bridge_load_writer *writer, uint32_t offset)
{
	return (void *)(uintptr_t)(writer->base + offset);
}

int ue_bridge_padded_array_bytes(uint64_t count, size_t element_size, size_t *bytes)
{
	uint64_t elements;

	if (count == UINT64_MAX)
		return 0;
	elements = count + 1u;
	if (element_size && elements > (uint64_t)SIZE_MAX / element_size)
		return 0;
	*bytes = (size_t)(elements * element_size);
	return 1;
}

int ue_bridge_table_valid(uint32_t offset, uint32_t count, uint32_t element_size, uint32_t capacity)
{
	uint32_t alignment = element_size >= 4u ? 4u : element_size;

	if (count == 0)
		return 1;
	if (offset == UE_BRIDGE_NO_OFFSET || (alignment > 1u && offset % alignment != 0))
		return 0;
	/* in 64 bits: a corrupt count must not wrap back inside the region */
	return (uint64_t)offset + (uint64_t)count * element_size <= capacity;
}

struct ue_bridge_bsp_entry *ue_bridge_load_bsp_entry(volatile uint8_t *region, uint32_t index)
{
	const struct ue_bridge_load_root *root = (const struct ue_bridge_load_root *)(uintptr_t)region;

	if (index >= root->bsps.count)
		return 0;
	return (struct ue_bridge_bsp_entry *)(uintptr_t)(region + root->bsps.offset) + index;
}

/*
UE_BRIDGE_RING.C

See ue_bridge_ring.h.
*/

#include "ue_bridge_ring.h"

#include <string.h>

int ue_bridge_ring_valid(const volatile struct ue_bridge_ring_desc *ring, uint32_t section_size, uint32_t min_slot_size)
{
	uint32_t offset = ring->offset;
	uint32_t slot_size = ring->slot_size;
	uint32_t slot_count = ring->slot_count;

	if (slot_count < 2u || slot_size < min_slot_size || slot_size % 8u != 0 || offset % 8u != 0)
		return 0;
	/* in 64 bits: a corrupt descriptor must not wrap back inside the section */
	return (uint64_t)offset + (uint64_t)slot_size * slot_count <= section_size;
}

static volatile struct ue_bridge_slot *ring_slot(volatile uint8_t *base, const volatile struct ue_bridge_ring_desc *ring, uint32_t write)
{
	return (volatile struct ue_bridge_slot *)(base + ring->offset + (write % ring->slot_count) * ring->slot_size);
}

volatile struct ue_bridge_slot *ue_bridge_ring_begin_write(volatile uint8_t *base, volatile struct ue_bridge_ring_desc *ring)
{
	volatile struct ue_bridge_slot *slot = ring_slot(base, ring, ring->published);

	ueb_store_u32(&slot->sequence, ueb_load_u32(&slot->sequence) + 1u);
	/* the odd sequence must be visible before any of the payload */
	ueb_fence();
	return slot;
}

void ue_bridge_ring_end_write(volatile struct ue_bridge_ring_desc *ring, volatile struct ue_bridge_slot *slot)
{
	ueb_fence();
	ueb_store_u32(&slot->sequence, ueb_load_u32(&slot->sequence) + 1u);
	ueb_store_u32(&ring->published, ring->published + 1u);
}

int ue_bridge_slot_try_read(const volatile struct ue_bridge_slot *slot, uint32_t slot_size, void *out, uint32_t out_size,
	ue_bridge_read_hook between, void *context)
{
	uint32_t before = ueb_load_u32(&slot->sequence);
	uint32_t size = out_size < slot_size ? out_size : slot_size;

	if (before & 1u)
		return 0;
	/* a plain copy of memory the writer may be changing: the sequence
	re-check below throws it away if so */
	memcpy(out, (const void *)(uintptr_t)slot, size);
	if (between)
		between(context);
	ueb_fence();
	return ueb_load_u32(&slot->sequence) == before;
}

enum ue_bridge_read_result ue_bridge_ring_read_newest(const volatile uint8_t *base, const volatile struct ue_bridge_ring_desc *ring,
	void *out, uint32_t out_size, uint32_t *published)
{
	uint32_t count = ueb_load_u32(&ring->published);
	uint32_t attempt;

	if (published)
		*published = count;
	if (count == 0)
		return UE_BRIDGE_READ_NONE;
	for (attempt = 0; attempt < 2u && attempt < count; attempt++)
	{
		const volatile struct ue_bridge_slot *slot = ring_slot((volatile uint8_t *)(uintptr_t)base, ring, count - 1u - attempt);

		if (ue_bridge_slot_try_read(slot, ring->slot_size, out, out_size, 0, 0))
			return attempt == 0 ? UE_BRIDGE_READ_NEWEST : UE_BRIDGE_READ_PREVIOUS;
	}
	return UE_BRIDGE_READ_TORN;
}

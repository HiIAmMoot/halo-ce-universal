/*
UE_BRIDGE_LOAD.H

The bridge section's layout and the load region (ue_bridge_format.h). The
game writes the region with a writer that never runs past it; UE checks every
table against its own mapping with ue_bridge_table_valid before reading it.

Compiled as C by clang (the game, its tests) and as C++ by MSVC (the plugin).
*/

#ifndef UE_BRIDGE_LOAD_H
#define UE_BRIDGE_LOAD_H

#include "ue_bridge_format.h"

#ifdef __cplusplus
extern "C" {
#endif

struct ue_bridge_layout
{
	struct ue_bridge_ring_desc tick_ring;
	struct ue_bridge_ring_desc frame_ring;
	struct ue_bridge_region_desc load_region;
};

/* 1 and the layout when section_size (a multiple of 4096, at most
UE_BRIDGE_MAX_SECTION_SIZE) holds the header, UE_BRIDGE_TICK_SLOTS slots of
tick_slot_size, the frame ring and a load region of at least
UE_BRIDGE_MIN_LOAD_SIZE */
int ue_bridge_layout_compute(uint32_t section_size, uint32_t tick_slot_size, struct ue_bridge_layout *layout);

struct ue_bridge_load_writer
{
	volatile uint8_t *base;
	uint32_t capacity;
	uint32_t used;
	/* set by the first reservation that didn't fit; every later one fails
	too, so an export never leaves a hole in the middle of what it wrote */
	uint32_t overflow;
};

void ue_bridge_load_writer_init(struct ue_bridge_load_writer *writer, volatile uint8_t *base, uint32_t capacity, uint32_t used);
/* count elements of element_size, 8-byte aligned; UE_BRIDGE_NO_OFFSET (and
overflow set) when they don't fit */
uint32_t ue_bridge_load_reserve(struct ue_bridge_load_writer *writer, uint32_t count, uint32_t element_size);
volatile void *ue_bridge_load_at(const struct ue_bridge_load_writer *writer, uint32_t offset);
/* the same, for plain stores: the region is written only by the game, and UE
copies it under the load sequence, so the writers (ue_bridge_model.c,
ue_bridge_bsp.c) need no volatile. One definition here, because the game and
the plugin compile every shared file into one translation unit. */
void *ue_bridge_load_pointer(const struct ue_bridge_load_writer *writer, uint32_t offset);
/* 1 when count elements of element_size at offset lie inside capacity bytes
and offset is aligned to the element (to 4 at most); an empty table is
always valid */
int ue_bridge_table_valid(uint32_t offset, uint32_t count, uint32_t element_size, uint32_t capacity);
/* the entry at index of the bsps table of the root at region's offset 0; 0
past the table (a zeroed root has none). For the game and its test fakes
only: it trusts the root, which UE never may (UE copies the table with
ue_bridge_table_valid instead). */
struct ue_bridge_bsp_entry *ue_bridge_load_bsp_entry(volatile uint8_t *region, uint32_t index);

#ifdef __cplusplus
}
#endif

#endif

/*
UE_BRIDGE_TICK.H

Object records in a tick slot (ue_bridge_format.h): a full snapshot of every
object, in the order the game iterates them (ascending absolute index).

Compiled as C by clang (the game, its tests) and as C++ by MSVC (the plugin).
*/

#ifndef UE_BRIDGE_TICK_H
#define UE_BRIDGE_TICK_H

#include "ue_bridge_format.h"

#ifdef __cplusplus
extern "C" {
#endif

struct ue_bridge_tick_writer
{
	uint8_t *slot;
	uint32_t capacity;
	uint32_t used;
	uint32_t object_count;
	uint16_t flags;
};

uint32_t ue_bridge_object_record_size(uint8_t region_count, uint16_t node_count);
/* records start after the struct ue_bridge_tick_header at slot */
void ue_bridge_tick_writer_begin(struct ue_bridge_tick_writer *writer, void *slot, uint32_t capacity);
/* 1 when the record fit; 0 when it didn't, and from then on for every
record: the slot is marked truncated */
int ue_bridge_tick_writer_add(struct ue_bridge_tick_writer *writer, uint32_t datum_index, uint16_t definition, uint16_t flags,
	const uint8_t *permutations, uint8_t region_count, const struct ue_bridge_matrix *nodes, uint16_t node_count);
/* fills the tick header's fields after struct ue_bridge_slot */
void ue_bridge_tick_writer_end(struct ue_bridge_tick_writer *writer, uint32_t load_epoch, uint32_t state_epoch, int16_t active_bsp);
/* UE_BRIDGE_OBJECT_AT_REST's rule: 1 when the record continues from the tick
before and its node matrices are byte-identical to that tick's
(render_interpolation.c keeps both snapshots) */
int ue_bridge_nodes_at_rest(const void *previous, const void *latest, uint32_t bytes, int continuing);

#ifdef __cplusplus
}
#endif

#endif

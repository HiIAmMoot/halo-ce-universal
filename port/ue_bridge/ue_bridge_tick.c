/*
UE_BRIDGE_TICK.C

See ue_bridge_tick.h.
*/

#include "ue_bridge_tick.h"

#include <string.h>

uint32_t ue_bridge_object_record_size(uint8_t region_count, uint16_t node_count)
{
	return (uint32_t)sizeof(struct ue_bridge_object_record) + (((uint32_t)region_count + 3u) & ~3u) +
		(uint32_t)node_count * (uint32_t)sizeof(struct ue_bridge_matrix);
}

void ue_bridge_tick_writer_begin(struct ue_bridge_tick_writer *writer, void *slot, uint32_t capacity)
{
	writer->slot = (uint8_t *)slot;
	writer->capacity = capacity;
	writer->used = sizeof(struct ue_bridge_tick_header);
	writer->object_count = 0;
	writer->flags = 0;
}

int ue_bridge_tick_writer_add(struct ue_bridge_tick_writer *writer, uint32_t datum_index, uint16_t definition, uint16_t flags,
	const uint8_t *permutations, uint8_t region_count, const struct ue_bridge_matrix *nodes, uint16_t node_count)
{
	uint32_t size = ue_bridge_object_record_size(region_count, node_count);
	struct ue_bridge_object_record record;
	uint8_t *at;

	if ((writer->flags & UE_BRIDGE_TICK_TRUNCATED) || (uint64_t)writer->used + size > writer->capacity)
	{
		writer->flags |= UE_BRIDGE_TICK_TRUNCATED;
		return 0;
	}
	at = writer->slot + writer->used;
	record.datum_index = datum_index;
	record.definition = definition;
	record.flags = flags;
	record.node_count = node_count;
	record.region_count = region_count;
	record.reserved = 0;
	record.size = size;
	memcpy(at, &record, sizeof(record));
	at += sizeof(record);
	if (region_count)
	{
		memset(at, 0, ((uint32_t)region_count + 3u) & ~3u);
		memcpy(at, permutations, region_count);
	}
	at += ((uint32_t)region_count + 3u) & ~3u;
	if (node_count)
		memcpy(at, nodes, (size_t)node_count * sizeof(struct ue_bridge_matrix));
	writer->used += size;
	writer->object_count++;
	return 1;
}

void ue_bridge_tick_writer_end(struct ue_bridge_tick_writer *writer, uint32_t load_epoch, uint32_t state_epoch, int16_t active_bsp)
{
	struct ue_bridge_tick_header *header = (struct ue_bridge_tick_header *)writer->slot;

	header->used = writer->used;
	header->object_count = writer->object_count;
	header->load_epoch = load_epoch;
	header->state_epoch = state_epoch;
	header->active_bsp = active_bsp;
	header->flags = writer->flags;
	header->reserved = 0;
}

int ue_bridge_nodes_at_rest(const void *previous, const void *latest, uint32_t bytes, int continuing)
{
	return continuing && memcmp(previous, latest, bytes) == 0;
}

/*
UE_BRIDGE_MODEL.C

See ue_bridge_model.h.
*/

#include "ue_bridge_model.h"

#include <stdlib.h>
#include <string.h>

/* count + 1 elements through the shared overflow-checked sizing; NULL when it doesn't fit.
(Named for the file: every shared file is compiled into one translation unit.) */
static void *model_allocate(uint64_t count, size_t element_size)
{
	size_t bytes;

	return ue_bridge_padded_array_bytes(count, element_size, &bytes) ? malloc(bytes) : 0;
}

/* named for the file: every shared file is compiled into one translation unit */
static int model_write_part(struct ue_bridge_load_writer *writer, const struct ue_bridge_model_source_part *source, uint32_t node_count,
	struct ue_bridge_part *part, uint32_t *repaired)
{
	uint32_t index;
	/* in 64 bits: three indices a triangle can pass 2^32 on a damaged strip length */
	uint64_t list_capacity = source->strip_length >= 3u ? 3u * (uint64_t)(source->strip_length - 2u) : 0u;
	ue_bridge_model_index *list = 0;
	uint32_t list_count = 0;

	part->shader = source->shader;
	part->reserved = 0;
	part->vertices.count = source->vertex_count;
	part->vertices.offset = ue_bridge_load_reserve(writer, source->vertex_count, sizeof(struct ue_bridge_model_vertex));
	if (writer->overflow)
		return 0;
	for (index = 0; index < source->vertex_count; index++)
	{
		struct ue_bridge_model_vertex vertex;

		ue_bridge_decode_model_vertex(&source->vertices[index], &vertex);
		/* the renderer's rule: the second node only carries weight while the first doesn't take all of it,
		and a weight that is not a number counts as not all (the comparison is false for NaN) */
		if (vertex.node[0] >= node_count || (!(vertex.weight >= 1.0f) && vertex.node[1] >= node_count))
			(*repaired)++;
		memcpy((struct ue_bridge_model_vertex *)ue_bridge_load_pointer(writer, part->vertices.offset) + index, &vertex, sizeof(vertex));
	}
	if (list_capacity > UINT32_MAX)
	{
		writer->overflow = 1;
		return 0;
	}
	if (list_capacity)
	{
		list = (ue_bridge_model_index *)model_allocate(list_capacity, sizeof(*list));
		if (!list)
		{
			writer->overflow = 1;
			return 0;
		}
		list_count = ue_bridge_strip_to_list(source->strip, source->strip_length, list);
	}
	part->indices.count = list_count;
	part->indices.offset = ue_bridge_load_reserve(writer, list_count, sizeof(*list));
	if (!writer->overflow && list_count)
		memcpy(ue_bridge_load_pointer(writer, part->indices.offset), list, list_count * sizeof(*list));
	free(list);
	return !writer->overflow;
}

static int model_write_geometry(struct ue_bridge_load_writer *writer, const struct ue_bridge_model_source_geometry *source, uint32_t node_count,
	struct ue_bridge_geometry *geometry, uint32_t *repaired)
{
	uint32_t index, kept = 0, written = 0;

	for (index = 0; index < source->part_count; index++)
		kept += source->parts[index].skip ? 0u : 1u;
	geometry->parts.count = kept;
	geometry->parts.offset = ue_bridge_load_reserve(writer, kept, sizeof(struct ue_bridge_part));
	for (index = 0; index < source->part_count && !writer->overflow; index++)
	{
		struct ue_bridge_part part;

		if (source->parts[index].skip)
			continue;
		if (!model_write_part(writer, &source->parts[index], node_count, &part, repaired))
			return 0;
		memcpy((struct ue_bridge_part *)ue_bridge_load_pointer(writer, geometry->parts.offset) + written++, &part, sizeof(part));
	}
	return !writer->overflow;
}

int ue_bridge_model_export(struct ue_bridge_load_writer *writer, const struct ue_bridge_model_source *source, struct ue_bridge_model *model,
	struct ue_bridge_model_counts *counts)
{
	uint32_t repaired = 0, clamped = 0;
	uint32_t start = writer->used;
	struct ue_bridge_model result;
	/* Halo geometry index -> exported index, -1 while unused */
	int16_t *remap = 0;
	int16_t used_count = 0;
	uint32_t node_index, region_index, permutation_index, geometry_index;
	int level;

	memset(&result, 0, sizeof(result));
	result.tag_index = source->tag_index;
	memcpy(result.detail_cutoff_pixels, source->detail_cutoff_pixels, sizeof(result.detail_cutoff_pixels));
	memcpy(result.node_counts, source->node_counts, sizeof(result.node_counts));
	for (level = 0; level < (int)UE_BRIDGE_DETAIL_LEVELS; level++)
	{
		if (source->node_counts[level] < 0 || (uint32_t)source->node_counts[level] > source->node_count)
			clamped++;
	}
	if (source->geometry_count)
	{
		remap = (int16_t *)model_allocate(source->geometry_count, sizeof(int16_t));
		if (!remap)
			goto failed;
		memset(remap, 0xFF, source->geometry_count * sizeof(int16_t));
	}

	result.nodes.count = source->node_count;
	result.nodes.offset = ue_bridge_load_reserve(writer, source->node_count, sizeof(struct ue_bridge_node));
	for (node_index = 0; node_index < source->node_count && !writer->overflow; node_index++)
	{
		struct ue_bridge_node node;

		node.parent = source->nodes[node_index].parent;
		node.reserved = 0;
		node.default_local = source->nodes[node_index].default_local;
		node.default_inverse = source->nodes[node_index].default_inverse;
		memcpy((struct ue_bridge_node *)ue_bridge_load_pointer(writer, result.nodes.offset) + node_index, &node, sizeof(node));
	}

	result.regions.count = source->region_count;
	result.regions.offset = ue_bridge_load_reserve(writer, source->region_count, sizeof(struct ue_bridge_region));
	for (region_index = 0; region_index < source->region_count && !writer->overflow; region_index++)
	{
		const struct ue_bridge_model_source_region *region = &source->regions[region_index];
		struct ue_bridge_region exported;

		exported.permutations.count = region->permutation_count;
		exported.permutations.offset = ue_bridge_load_reserve(writer, region->permutation_count, sizeof(struct ue_bridge_permutation));
		for (permutation_index = 0; permutation_index < region->permutation_count && !writer->overflow; permutation_index++)
		{
			struct ue_bridge_permutation permutation;

			for (level = 0; level < (int)UE_BRIDGE_DETAIL_LEVELS; level++)
			{
				int16_t halo = region->permutations[permutation_index][level];

				if (halo < 0 || (uint32_t)halo >= source->geometry_count)
				{
					permutation.geometry[level] = -1;
					continue;
				}
				if (remap[halo] < 0)
					remap[halo] = used_count++;
				permutation.geometry[level] = remap[halo];
			}
			permutation.reserved = 0;
			memcpy((struct ue_bridge_permutation *)ue_bridge_load_pointer(writer, exported.permutations.offset) + permutation_index,
				&permutation, sizeof(permutation));
		}
		if (!writer->overflow)
			memcpy((struct ue_bridge_region *)ue_bridge_load_pointer(writer, result.regions.offset) + region_index, &exported, sizeof(exported));
	}

	result.geometries.count = (uint32_t)used_count;
	result.geometries.offset = ue_bridge_load_reserve(writer, (uint32_t)used_count, sizeof(struct ue_bridge_geometry));
	for (geometry_index = 0; geometry_index < source->geometry_count && !writer->overflow; geometry_index++)
	{
		struct ue_bridge_geometry geometry;

		if (remap[geometry_index] < 0)
			continue;
		if (!model_write_geometry(writer, &source->geometries[geometry_index], source->node_count, &geometry, &repaired))
			break;
		memcpy((struct ue_bridge_geometry *)ue_bridge_load_pointer(writer, result.geometries.offset) + remap[geometry_index], &geometry, sizeof(geometry));
	}

	result.shaders.count = source->shader_count;
	result.shaders.offset = ue_bridge_load_reserve(writer, source->shader_count, sizeof(int32_t));
	if (!writer->overflow && source->shader_count)
		memcpy(ue_bridge_load_pointer(writer, result.shaders.offset), source->shader_tags, source->shader_count * sizeof(int32_t));

	if (writer->overflow)
		goto failed;
	free(remap);
	*model = result;
	if (counts)
	{
		counts->repaired_vertices += repaired;
		counts->clamped_node_counts += clamped;
	}
	return 1;

failed:
	free(remap);
	writer->overflow = 1;
	writer->used = start;
	return 0;
}

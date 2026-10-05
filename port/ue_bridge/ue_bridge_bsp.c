/*
UE_BRIDGE_BSP.C

See ue_bridge_bsp.h.
*/

#include "ue_bridge_bsp.h"

#include <stdlib.h>
#include <string.h>

#define UNLISTED (-2)
/* a vertex stamp is batch * BSP_STAMP_MATERIALS + material + 1, so the
materials of one BSP must stay below it (checked in ue_bridge_bsp_export) */
#define BSP_STAMP_MATERIALS 65536u

uint32_t ue_bridge_bsp_unpack_cluster_list(const int32_t *packed, uint32_t packed_count, int32_t *surfaces, uint32_t capacity)
{
	uint32_t position = 0, written = 0;

	while (position + 3u <= packed_count)
	{
		uint32_t count = (uint32_t)packed[position + 2u];
		uint32_t index;

		position += 3u;
		/* a group that runs past the list leaves position at its end, which
		ends the outer loop too */
		for (index = 0; index < count && position < packed_count && written < capacity; index++)
			surfaces[written++] = packed[position++];
	}
	return written;
}

/* the material holding the surface, by binary search; -1 for none */
static int32_t material_of(const struct ue_bridge_bsp_source *source, uint32_t surface)
{
	uint32_t low = 0, high = source->material_count;

	while (low < high)
	{
		uint32_t middle = (low + high) / 2u;
		const struct ue_bridge_bsp_source_material *material = &source->materials[middle];

		if (surface < material->first_surface)
			high = middle;
		else if (surface >= material->first_surface + material->surface_count)
			low = middle + 1u;
		else
			return (int32_t)middle;
	}
	return -1;
}

struct bsp_scratch
{
	/* per surface: its cluster, -1 for none, UNLISTED while unassigned */
	int32_t *owner;
	/* per surface: its material, -1 for none */
	int32_t *material;
	/* surfaces sorted by owner (cluster -1 first), from a counting sort */
	uint32_t *order;
	uint32_t *cluster_start;
	/* per material vertex: the stamp (batch and material) it was last added
	under, and its index in that batch */
	uint64_t *stamp;
	ue_bridge_bsp_index *batch_index;
	uint32_t largest_material;
};

static void scratch_free(struct bsp_scratch *scratch)
{
	free(scratch->owner);
	free(scratch->material);
	free(scratch->order);
	free(scratch->cluster_start);
	free(scratch->stamp);
	free(scratch->batch_index);
}

/* writes the batch of the given surfaces (all of one cluster and shader);
stamp numbers the batch so the vertex remap never needs clearing */
static int write_batch(struct ue_bridge_load_writer *writer, const struct ue_bridge_bsp_source *source, struct bsp_scratch *scratch,
	const uint32_t *surfaces, uint32_t surface_count, uint64_t stamp, int16_t cluster, int32_t shader, struct ue_bridge_bsp_batch *batch)
{
	uint32_t vertex_count = 0, index, corner;
	int32_t current_material = -1;
	uint64_t material_stamp = 0;

	/* pass 1: count the batch's vertices; a surface's three indices are into
	its material's vertices, so the remap is per material within the batch */
	for (index = 0; index < surface_count; index++)
	{
		uint32_t surface = surfaces[index];
		int32_t material = scratch->material[surface];

		if (material != current_material)
		{
			current_material = material;
			material_stamp = stamp * BSP_STAMP_MATERIALS + (uint64_t)material + 1u;
		}
		for (corner = 0; corner < 3u; corner++)
		{
			uint16_t vertex = source->surfaces[surface * 3u + corner];

			if (scratch->stamp[vertex] != material_stamp)
			{
				scratch->stamp[vertex] = material_stamp;
				scratch->batch_index[vertex] = vertex_count++;
			}
		}
	}
	batch->cluster = cluster;
	batch->reserved = 0;
	batch->shader_tag = shader;
	batch->vertices.count = vertex_count;
	batch->vertices.offset = ue_bridge_load_reserve(writer, vertex_count, sizeof(struct ue_bridge_bsp_vertex));
	batch->indices.count = surface_count * 3u;
	batch->indices.offset = ue_bridge_load_reserve(writer, surface_count * 3u, sizeof(ue_bridge_bsp_index));
	if (writer->overflow)
		return 0;
	/* pass 2: the same walk, writing; stamps are taken past pass 1's */
	current_material = -1;
	vertex_count = 0;
	for (index = 0; index < surface_count; index++)
	{
		uint32_t surface = surfaces[index];
		int32_t material = scratch->material[surface];
		const struct ue_bridge_bsp_source_material *source_material = &source->materials[material];

		if (material != current_material)
		{
			current_material = material;
			material_stamp = (stamp + 1u) * BSP_STAMP_MATERIALS + (uint64_t)material + 1u;
		}
		for (corner = 0; corner < 3u; corner++)
		{
			uint16_t vertex = source->surfaces[surface * 3u + corner];

			if (scratch->stamp[vertex] != material_stamp)
			{
				struct ue_bridge_bsp_vertex decoded;

				scratch->stamp[vertex] = material_stamp;
				scratch->batch_index[vertex] = vertex_count;
				ue_bridge_decode_environment_vertex(&source_material->vertices[vertex], &decoded);
				memcpy((struct ue_bridge_bsp_vertex *)ue_bridge_load_pointer(writer, batch->vertices.offset) + vertex_count, &decoded, sizeof(decoded));
				vertex_count++;
			}
			((ue_bridge_bsp_index *)ue_bridge_load_pointer(writer, batch->indices.offset))[index * 3u + corner] = scratch->batch_index[vertex];
		}
	}
	return 1;
}

int ue_bridge_bsp_export(struct ue_bridge_load_writer *writer, const struct ue_bridge_bsp_source *source, struct ue_bridge_bsp_entry *entry)
{
	uint32_t start = writer->used;
	struct bsp_scratch scratch;
	struct ue_bridge_bsp_entry result;
	uint32_t surface, cluster_index, index;
	uint64_t stamp = 1;
	uint32_t batch_count = 0, batch_capacity;
	struct ue_bridge_bsp_batch *batches = 0;
	uint32_t *run = 0;
	int32_t cluster;

	memset(&scratch, 0, sizeof(scratch));
	memset(&result, 0, sizeof(result));
	result.cluster_count = source->cluster_count;
	if (source->material_count >= BSP_STAMP_MATERIALS)
		goto failed;
	for (index = 0; index < source->material_count; index++)
	{
		if (source->materials[index].vertex_count > scratch.largest_material)
			scratch.largest_material = source->materials[index].vertex_count;
	}
	scratch.owner = (int32_t *)malloc((source->surface_count + 1u) * sizeof(int32_t));
	scratch.material = (int32_t *)malloc((source->surface_count + 1u) * sizeof(int32_t));
	scratch.order = (uint32_t *)malloc((source->surface_count + 1u) * sizeof(uint32_t));
	scratch.cluster_start = (uint32_t *)calloc(source->cluster_count + 2u, sizeof(uint32_t));
	scratch.stamp = (uint64_t *)calloc(scratch.largest_material + 1u, sizeof(uint64_t));
	scratch.batch_index = (ue_bridge_bsp_index *)malloc((scratch.largest_material + 1u) * sizeof(ue_bridge_bsp_index));
	run = (uint32_t *)malloc((source->surface_count + 1u) * sizeof(uint32_t));
	if (!scratch.owner || !scratch.material || !scratch.order || !scratch.cluster_start || !scratch.stamp || !scratch.batch_index || !run)
		goto failed;

	for (surface = 0; surface < source->surface_count; surface++)
	{
		int32_t material = material_of(source, surface);

		/* a surface outside every material, or naming a vertex its material
		doesn't have, is dropped: the tag is damaged, and the vertex remap
		below must never index past a material */
		for (index = 0; material >= 0 && index < 3u; index++)
		{
			if (source->surfaces[surface * 3u + index] >= source->materials[material].vertex_count)
				material = -1;
		}
		scratch.owner[surface] = UNLISTED;
		scratch.material[surface] = material;
	}
	for (cluster_index = 0; cluster_index < source->cluster_count; cluster_index++)
	{
		for (index = 0; index < source->cluster_surface_counts[cluster_index]; index++)
		{
			int32_t listed = source->cluster_surfaces[cluster_index][index];

			if (listed < 0 || (uint32_t)listed >= source->surface_count)
				continue;
			if (scratch.owner[listed] == UNLISTED)
				scratch.owner[listed] = (int32_t)cluster_index;
			else
				result.duplicate_surfaces++;
		}
	}
	/* counting sort by owner, cluster -1 in bucket 0; surfaces with no
	material (outside every range) are dropped */
	for (surface = 0; surface < source->surface_count; surface++)
	{
		if (scratch.owner[surface] == UNLISTED)
		{
			scratch.owner[surface] = -1;
			result.unclustered_surfaces++;
		}
		if (scratch.material[surface] >= 0)
			scratch.cluster_start[scratch.owner[surface] + 2]++;
	}
	for (cluster_index = 1; cluster_index < source->cluster_count + 2u; cluster_index++)
		scratch.cluster_start[cluster_index] += scratch.cluster_start[cluster_index - 1u];
	for (surface = 0; surface < source->surface_count; surface++)
	{
		if (scratch.material[surface] >= 0)
			scratch.order[scratch.cluster_start[scratch.owner[surface] + 1]++] = surface;
	}
	/* cluster_start[c + 1] is now the end of cluster c's run (c from -1) */

	/* at most one batch per cluster per material */
	batch_capacity = (source->cluster_count + 1u) * (source->material_count ? source->material_count : 1u);
	batches = (struct ue_bridge_bsp_batch *)malloc(batch_capacity * sizeof(struct ue_bridge_bsp_batch));
	if (!batches)
		goto failed;
	for (cluster = -1; cluster < (int32_t)source->cluster_count; cluster++)
	{
		uint32_t begin = cluster == -1 ? 0u : scratch.cluster_start[cluster];
		uint32_t end = scratch.cluster_start[cluster + 1];
		uint32_t first;

		/* each distinct shader of the run, in order of first appearance */
		for (first = begin; first < end; first++)
		{
			int32_t shader = source->materials[scratch.material[scratch.order[first]]].shader_tag;
			uint32_t run_count = 0, seen, scan;

			for (seen = begin; seen < first; seen++)
			{
				if (source->materials[scratch.material[scratch.order[seen]]].shader_tag == shader)
					break;
			}
			if (seen < first)
				continue;
			for (scan = first; scan < end; scan++)
			{
				if (source->materials[scratch.material[scratch.order[scan]]].shader_tag == shader)
					run[run_count++] = scratch.order[scan];
			}
			if (!write_batch(writer, source, &scratch, run, run_count, stamp, (int16_t)cluster, shader, &batches[batch_count]))
				goto failed;
			stamp += 2u;
			batch_count++;
		}
	}
	result.batches.count = batch_count;
	result.batches.offset = ue_bridge_load_reserve(writer, batch_count, sizeof(struct ue_bridge_bsp_batch));
	if (writer->overflow)
		goto failed;
	if (batch_count)
		memcpy(ue_bridge_load_pointer(writer, result.batches.offset), batches, batch_count * sizeof(struct ue_bridge_bsp_batch));
	free(batches);
	free(run);
	scratch_free(&scratch);
	result.tag_index = entry->tag_index;
	result.ready = 0;
	*entry = result;
	return 1;

failed:
	free(batches);
	free(run);
	scratch_free(&scratch);
	writer->overflow = 1;
	writer->used = start;
	return 0;
}

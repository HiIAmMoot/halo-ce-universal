/*
UE_BRIDGE_MODEL.H

A model written into the load region (ue_bridge_format.h) from a plain
description the game fills from its tags (port/linux/game/ue_bridge_export.c).
Every geometry a permutation uses at any detail level is written once.

Compiled as C by clang (the game, its tests) and as C++ by MSVC (the plugin).
*/

#ifndef UE_BRIDGE_MODEL_H
#define UE_BRIDGE_MODEL_H

#include "ue_bridge_codec.h"
#include "ue_bridge_load.h"

#ifdef __cplusplus
extern "C" {
#endif

struct ue_bridge_model_source_part
{
	int16_t shader;
	/* nonzero: left out, as render_model_parts leaves it out (stripped, or a
	shader no model may use) */
	int skip;
	const struct ue_bridge_compressed_model_vertex *vertices;
	uint32_t vertex_count;
	const uint16_t *strip;
	uint32_t strip_length;
};

struct ue_bridge_model_source_geometry
{
	const struct ue_bridge_model_source_part *parts;
	uint32_t part_count;
};

struct ue_bridge_model_source_region
{
	/* geometry_indices per permutation, Halo's numbering */
	const int16_t (*permutations)[UE_BRIDGE_DETAIL_LEVELS];
	uint32_t permutation_count;
};

struct ue_bridge_model_source_node
{
	int16_t parent;
	struct ue_bridge_matrix default_local;
	struct ue_bridge_matrix default_inverse;
};

struct ue_bridge_model_source
{
	int32_t tag_index;
	float detail_cutoff_pixels[UE_BRIDGE_DETAIL_LEVELS];
	int16_t node_counts[UE_BRIDGE_DETAIL_LEVELS];
	const struct ue_bridge_model_source_node *nodes;
	uint32_t node_count;
	const struct ue_bridge_model_source_region *regions;
	uint32_t region_count;
	const struct ue_bridge_model_source_geometry *geometries;
	uint32_t geometry_count;
	const int32_t *shader_tags;
	uint32_t shader_count;
};

/* what an export saw that the renderer repairs on its side, summed over
models (the load root's repaired_vertices and clamped_node_counts) */
struct ue_bridge_model_counts
{
	/* written vertices whose first node, or whose second while it carries
	weight (a weight under 1, or not a number), is not one of the model's nodes */
	uint32_t repaired_vertices;
	/* detail levels whose node count is negative or past the model's nodes */
	uint32_t clamped_node_counts;
};

/* 1 when the whole model fit in the region and *model describes it; 0 when
it didn't, with *model untouched and the writer's used size as it was (its
overflow stays set). On success the model's counts are added to *counts (NULL:
not wanted); a model that fails adds nothing. */
int ue_bridge_model_export(struct ue_bridge_load_writer *writer, const struct ue_bridge_model_source *source, struct ue_bridge_model *model,
	struct ue_bridge_model_counts *counts);

#ifdef __cplusplus
}
#endif

#endif

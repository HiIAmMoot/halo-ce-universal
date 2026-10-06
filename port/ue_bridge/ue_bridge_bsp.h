/*
UE_BRIDGE_BSP.H

A structure BSP's render surfaces written into the load region
(ue_bridge_format.h) as one batch per cluster x shader, from a plain
description the game fills from its tags (port/linux/game/ue_bridge_export.c).

Compiled as C by clang (the game, its tests) and as C++ by MSVC (the plugin).
*/

#ifndef UE_BRIDGE_BSP_H
#define UE_BRIDGE_BSP_H

#include "ue_bridge_codec.h"
#include "ue_bridge_load.h"

#ifdef __cplusplus
extern "C" {
#endif

/* a lightmap material, flattened: its surfaces are the BSP's surfaces
first_surface to first_surface + surface_count - 1 */
struct ue_bridge_bsp_source_material
{
	int32_t shader_tag;
	uint32_t first_surface;
	uint32_t surface_count;
	const struct ue_bridge_compressed_environment_vertex *vertices;
	uint32_t vertex_count;
};

struct ue_bridge_bsp_source
{
	/* three vertex indices per surface, into its material's vertices */
	const uint16_t *surfaces;
	uint32_t surface_count;
	/* sorted by first_surface, ranges not overlapping */
	const struct ue_bridge_bsp_source_material *materials;
	uint32_t material_count;
	/* per cluster, its surfaces (ue_bridge_bsp_unpack_cluster_list's output) */
	const int32_t *const *cluster_surfaces;
	const uint32_t *cluster_surface_counts;
	uint32_t cluster_count;
};

/* capacity + count, held at limit: a cluster's subclusters can list more
surfaces than the BSP has, and the sum of their counts can wrap 32 bits. A
cluster's surfaces are listed once each, so the BSP's surface count bounds it. */
uint32_t ue_bridge_bsp_capacity_add(uint32_t capacity, uint32_t count, uint32_t limit);

/* a cluster's packed surface list ([lightmap, material, count, surfaces...]
groups) as its surfaces; stops at a group that runs past packed_count or
past capacity; returns the surfaces written */
uint32_t ue_bridge_bsp_unpack_cluster_list(const int32_t *packed, uint32_t packed_count, int32_t *surfaces, uint32_t capacity);

/* 1 when every batch fit and *entry describes them (the caller sets
tag_index, then ready last); 0 with the writer's used size as it was */
int ue_bridge_bsp_export(struct ue_bridge_load_writer *writer, const struct ue_bridge_bsp_source *source, struct ue_bridge_bsp_entry *entry);

#ifdef __cplusplus
}
#endif

#endif

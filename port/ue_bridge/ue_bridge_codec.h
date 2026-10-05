/*
UE_BRIDGE_CODEC.H

The game's compressed vertices, decoded for the load region
(ue_bridge_format.h), and Halo's triangle strips as lists. Model vertices are
decoded as the Xbox GPU reads them (the model vertex declaration in
rasterizer_xbox_vertex_shaders_initialize.c); normals with the decomp's
uncompress_int32_to_real_vector3d formula.

Compiled as C by clang (the game, its tests) and as C++ by MSVC (the plugin).
*/

#ifndef UE_BRIDGE_CODEC_H
#define UE_BRIDGE_CODEC_H

#include "ue_bridge_format.h"

#ifdef __cplusplus
extern "C" {
#endif

/* struct model_vertex_compressed (rasterizer_model_types.h) */
struct ue_bridge_compressed_model_vertex
{
	float position[3];
	uint32_t normal;
	uint32_t binormal;
	uint32_t tangent;
	int16_t texcoord[2];
	/* node index times 3: the GPU reads three constant registers per node */
	uint8_t nodes[2];
	int16_t node_weight;
};

/* struct environment_vertex_compressed (rasterizer_geometry.c) */
struct ue_bridge_compressed_environment_vertex
{
	float position[3];
	uint32_t normal;
	uint32_t binormal;
	uint32_t tangent;
	float texcoord[2];
};

UEB_STATIC_ASSERT(sizeof(struct ue_bridge_compressed_model_vertex) == 32, "compressed model vertex size");
UEB_STATIC_ASSERT(sizeof(struct ue_bridge_compressed_environment_vertex) == 32, "compressed environment vertex size");

/* NORMPACKED3: 11 bits x, 11 bits y, 10 bits z */
void ue_bridge_unpack_normal(uint32_t packed, float out[3]);
void ue_bridge_decode_model_vertex(const struct ue_bridge_compressed_model_vertex *in, struct ue_bridge_model_vertex *out);
void ue_bridge_decode_environment_vertex(const struct ue_bridge_compressed_environment_vertex *in, struct ue_bridge_bsp_vertex *out);
/* a D3D triangle strip as a list, degenerate triangles dropped; list holds
3 * (strip_length - 2) entries at most; returns the entries written */
uint32_t ue_bridge_strip_to_list(const uint16_t *strip, uint32_t strip_length, ue_bridge_model_index *list);

#ifdef __cplusplus
}
#endif

#endif

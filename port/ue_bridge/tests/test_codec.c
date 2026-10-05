/*
TEST_CODEC.C

Vertex decode and strip conversion (port/ue_bridge/ue_bridge_codec.c),
against hand-built inputs.
*/

#include "ueb_test.h"
#include "ue_bridge_codec.h"

#include <math.h>
#include <string.h>

static int near(float a, float b)
{
	return fabsf(a - b) < 1e-6f;
}

/* x in bits 0-10, y in 11-21, z in 22-31, each two's complement */
static uint32_t pack(int x, int y, int z)
{
	return ((uint32_t)x & 0x7FFu) | (((uint32_t)y & 0x7FFu) << 11) | (((uint32_t)z & 0x3FFu) << 22);
}

static void unpack_normal_extremes(void)
{
	float n[3];

	ue_bridge_unpack_normal(pack(1023, -1024, 511), n);
	UEB_CHECK(near(n[0], 1.0f));
	UEB_CHECK(near(n[1], -1.0f));
	UEB_CHECK(near(n[2], 1.0f));
	ue_bridge_unpack_normal(pack(0, 0, -512), n);
	/* the decomp's formula maps 0 to half a step above zero, not to zero */
	UEB_CHECK(near(n[0], 1.0f / 2047.0f));
	UEB_CHECK(near(n[1], 1.0f / 2047.0f));
	UEB_CHECK(near(n[2], -1.0f));
}

static void unpack_normal_keeps_the_fields_apart(void)
{
	float n[3];

	/* a set sign bit in x must not leak into y */
	ue_bridge_unpack_normal(pack(-1, 0, 0), n);
	UEB_CHECK(near(n[0], -1.0f / 2047.0f));
	UEB_CHECK(near(n[1], 1.0f / 2047.0f));
}

static void decode_model_vertex_as_the_gpu_reads_it(void)
{
	struct ue_bridge_compressed_model_vertex in;
	struct ue_bridge_model_vertex out;

	memset(&in, 0, sizeof(in));
	in.position[0] = 1.5f;
	in.position[1] = -2.0f;
	in.position[2] = 0.25f;
	in.normal = pack(1023, 0, 0);
	in.texcoord[0] = 32767;
	in.texcoord[1] = -16384;
	in.nodes[0] = 6;
	in.nodes[1] = 9;
	in.node_weight = 24575;
	ue_bridge_decode_model_vertex(&in, &out);
	UEB_CHECK(out.position[0] == 1.5f && out.position[1] == -2.0f && out.position[2] == 0.25f);
	UEB_CHECK(near(out.normal[0], 1.0f));
	UEB_CHECK(near(out.uv[0], 1.0f));
	UEB_CHECK(near(out.uv[1], -16384.0f / 32767.0f));
	UEB_CHECK(out.node[0] == 2 && out.node[1] == 3);
	UEB_CHECK(near(out.weight, 24575.0f / 32767.0f));
}

static void decode_environment_vertex(void)
{
	struct ue_bridge_compressed_environment_vertex in;
	struct ue_bridge_bsp_vertex out;

	memset(&in, 0, sizeof(in));
	in.position[2] = 7.0f;
	in.normal = pack(0, 1023, 0);
	in.texcoord[0] = 0.5f;
	in.texcoord[1] = 2.0f;
	ue_bridge_decode_environment_vertex(&in, &out);
	UEB_CHECK(out.position[2] == 7.0f);
	UEB_CHECK(near(out.normal[1], 1.0f));
	UEB_CHECK(out.uv[0] == 0.5f && out.uv[1] == 2.0f);
}

static void strip_alternates_winding(void)
{
	static const uint16_t strip[] = { 0, 1, 2, 3, 4 };
	ue_bridge_model_index list[9];

	UEB_CHECK(ue_bridge_strip_to_list(strip, 5, list) == 9);
	/* a strip flips facing every triangle, and the game's faces wind the other
	way from the right-handed cross product (measured on bloodgulch and a10): the
	even triangles swap their first two, the odd ones keep order, so every
	triangle's cross product agrees with its vertex normals */
	UEB_CHECK(list[0] == 1 && list[1] == 0 && list[2] == 2);
	UEB_CHECK(list[3] == 1 && list[4] == 2 && list[5] == 3);
	UEB_CHECK(list[6] == 3 && list[7] == 2 && list[8] == 4);
}

static void strip_drops_degenerate_triangles(void)
{
	/* the usual join between two strips: 0 1 2 | 2 5 | 5 6 7 */
	static const uint16_t strip[] = { 0, 1, 2, 2, 5, 5, 6, 7 };
	ue_bridge_model_index list[18];
	uint32_t count = ue_bridge_strip_to_list(strip, 8, list);

	UEB_CHECK(count == 6);
	UEB_CHECK(list[0] == 1 && list[1] == 0 && list[2] == 2);
	/* index 5's triangle (5 6 7) is odd: kept */
	UEB_CHECK(list[3] == 5 && list[4] == 6 && list[5] == 7);
}

static void strip_shorter_than_a_triangle_is_empty(void)
{
	static const uint16_t strip[] = { 0, 1 };
	ue_bridge_model_index list[3];

	UEB_CHECK(ue_bridge_strip_to_list(strip, 2, list) == 0);
	UEB_CHECK(ue_bridge_strip_to_list(strip, 0, list) == 0);
}

const struct ueb_test ueb_codec_tests[] =
{
	{ "unpack_normal_extremes", unpack_normal_extremes },
	{ "unpack_normal_keeps_the_fields_apart", unpack_normal_keeps_the_fields_apart },
	{ "decode_model_vertex_as_the_gpu_reads_it", decode_model_vertex_as_the_gpu_reads_it },
	{ "decode_environment_vertex", decode_environment_vertex },
	{ "strip_alternates_winding", strip_alternates_winding },
	{ "strip_drops_degenerate_triangles", strip_drops_degenerate_triangles },
	{ "strip_shorter_than_a_triangle_is_empty", strip_shorter_than_a_triangle_is_empty },
	{ 0, 0 }
};

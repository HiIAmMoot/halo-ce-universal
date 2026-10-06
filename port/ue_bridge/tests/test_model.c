/*
TEST_MODEL.C

Model export (port/ue_bridge/ue_bridge_model.c) from a hand-built source.
*/

#include "ueb_test.h"
#include "ue_bridge_model.h"

#include <string.h>

static uint64_t region[65536 / 8];

static struct ue_bridge_compressed_model_vertex quad[4];
static const uint16_t quad_strip[] = { 0, 1, 2, 3 };
static struct ue_bridge_compressed_model_vertex single[3];
static const uint16_t single_strip[] = { 0, 1, 2 };

static struct ue_bridge_model_source_part parts_a[2];
static struct ue_bridge_model_source_part parts_b[1];
static struct ue_bridge_model_source_geometry geometries[3];
/* geometry 2 is used by no permutation */
static const int16_t permutations[2][UE_BRIDGE_DETAIL_LEVELS] = { { 1, 1, 0, 0, 0 }, { -1, -1, -1, 1, 7 } };
static struct ue_bridge_model_source_region regions[1];
static struct ue_bridge_model_source_node nodes[2];
static const int32_t shader_tags[2] = { 0x1111, 0x2222 };
static struct ue_bridge_model_source source;

static void build_source(void)
{
	int index;

	memset(quad, 0, sizeof(quad));
	memset(single, 0, sizeof(single));
	for (index = 0; index < 4; index++)
	{
		quad[index].position[0] = (float)index;
		quad[index].node_weight = 32767;
	}
	single[0].nodes[0] = 3;
	memset(parts_a, 0, sizeof(parts_a));
	parts_a[0].shader = 1;
	parts_a[0].vertices = quad;
	parts_a[0].vertex_count = 4;
	parts_a[0].strip = quad_strip;
	parts_a[0].strip_length = 4;
	/* a stripped part: render_model_parts never draws it */
	parts_a[1] = parts_a[0];
	parts_a[1].skip = 1;
	memset(parts_b, 0, sizeof(parts_b));
	parts_b[0].shader = 0;
	parts_b[0].vertices = single;
	parts_b[0].vertex_count = 3;
	parts_b[0].strip = single_strip;
	parts_b[0].strip_length = 3;
	geometries[0].parts = parts_a;
	geometries[0].part_count = 2;
	geometries[1].parts = parts_b;
	geometries[1].part_count = 1;
	geometries[2].parts = parts_b;
	geometries[2].part_count = 1;
	regions[0].permutations = permutations;
	regions[0].permutation_count = 2;
	memset(nodes, 0, sizeof(nodes));
	nodes[0].parent = -1;
	nodes[0].default_local.scale = 1.0f;
	nodes[1].parent = 0;
	nodes[1].default_local.position[2] = 0.5f;
	nodes[1].default_inverse.position[2] = -0.5f;
	memset(&source, 0, sizeof(source));
	source.tag_index = 0x00420042;
	source.detail_cutoff_pixels[4] = 300.0f;
	source.node_counts[4] = 2;
	source.nodes = nodes;
	source.node_count = 2;
	source.regions = regions;
	source.region_count = 1;
	source.geometries = geometries;
	source.geometry_count = 3;
	source.shader_tags = shader_tags;
	source.shader_count = 2;
}

#define AT(type, offset) ((type *)((uint8_t *)region + (offset)))

static void model_export_writes_nodes_regions_and_shaders(void)
{
	struct ue_bridge_load_writer writer;
	struct ue_bridge_model model;
	struct ue_bridge_node *exported_nodes;

	build_source();
	ue_bridge_load_writer_init(&writer, (volatile uint8_t *)region, sizeof(region), 0);
	UEB_CHECK(ue_bridge_model_export(&writer, &source, &model, 0));
	UEB_CHECK(model.tag_index == 0x00420042);
	UEB_CHECK(model.detail_cutoff_pixels[4] == 300.0f);
	UEB_CHECK(model.node_counts[4] == 2);
	UEB_CHECK(model.nodes.count == 2);
	exported_nodes = AT(struct ue_bridge_node, model.nodes.offset);
	UEB_CHECK(exported_nodes[0].parent == -1 && exported_nodes[1].parent == 0);
	UEB_CHECK(exported_nodes[1].default_local.position[2] == 0.5f);
	UEB_CHECK(exported_nodes[1].default_inverse.position[2] == -0.5f);
	UEB_CHECK(model.shaders.count == 2);
	UEB_CHECK(AT(int32_t, model.shaders.offset)[1] == 0x2222);
	UEB_CHECK(model.regions.count == 1);
}

static void model_export_keeps_each_used_geometry_once(void)
{
	struct ue_bridge_load_writer writer;
	struct ue_bridge_model model;
	struct ue_bridge_region *exported_regions;
	struct ue_bridge_permutation *exported_permutations;

	build_source();
	ue_bridge_load_writer_init(&writer, (volatile uint8_t *)region, sizeof(region), 0);
	UEB_CHECK(ue_bridge_model_export(&writer, &source, &model, 0));
	/* geometries 1 and 0 are used, in that order of first use; 2 is unused; 7 is out of range */
	UEB_CHECK(model.geometries.count == 2);
	exported_regions = AT(struct ue_bridge_region, model.regions.offset);
	UEB_CHECK(exported_regions[0].permutations.count == 2);
	exported_permutations = AT(struct ue_bridge_permutation, exported_regions[0].permutations.offset);
	UEB_CHECK(exported_permutations[0].geometry[0] == 0 && exported_permutations[0].geometry[1] == 0);
	UEB_CHECK(exported_permutations[0].geometry[2] == 1 && exported_permutations[0].geometry[4] == 1);
	UEB_CHECK(exported_permutations[1].geometry[0] == -1);
	UEB_CHECK(exported_permutations[1].geometry[3] == 0);
	UEB_CHECK(exported_permutations[1].geometry[4] == -1);
}

static void model_export_decodes_parts_and_skips_stripped_ones(void)
{
	struct ue_bridge_load_writer writer;
	struct ue_bridge_model model;
	struct ue_bridge_geometry *exported_geometries;
	struct ue_bridge_part *part;
	struct ue_bridge_model_vertex *vertices;
	ue_bridge_model_index *indices;

	build_source();
	ue_bridge_load_writer_init(&writer, (volatile uint8_t *)region, sizeof(region), 0);
	UEB_CHECK(ue_bridge_model_export(&writer, &source, &model, 0));
	exported_geometries = AT(struct ue_bridge_geometry, model.geometries.offset);
	/* exported geometry 1 is Halo geometry 0: two parts, one stripped */
	UEB_CHECK(exported_geometries[1].parts.count == 1);
	part = AT(struct ue_bridge_part, exported_geometries[1].parts.offset);
	UEB_CHECK(part->shader == 1);
	UEB_CHECK(part->vertices.count == 4);
	UEB_CHECK(part->indices.count == 6);
	vertices = AT(struct ue_bridge_model_vertex, part->vertices.offset);
	UEB_CHECK(vertices[3].position[0] == 3.0f);
	UEB_CHECK(vertices[0].weight == 1.0f);
	indices = AT(ue_bridge_model_index, part->indices.offset);
	UEB_CHECK(indices[0] == 1 && indices[1] == 0 && indices[2] == 2);
	UEB_CHECK(indices[3] == 1 && indices[4] == 2 && indices[5] == 3);
	/* exported geometry 0 is Halo geometry 1 */
	part = AT(struct ue_bridge_part, exported_geometries[0].parts.offset);
	UEB_CHECK(AT(struct ue_bridge_model_vertex, part->vertices.offset)[0].node[0] == 1);
	/* nothing of the stripped part was written: a source without it uses the same bytes */
	{
		uint32_t with_stripped = writer.used;

		geometries[0].part_count = 1;
		ue_bridge_load_writer_init(&writer, (volatile uint8_t *)region, sizeof(region), 0);
		UEB_CHECK(ue_bridge_model_export(&writer, &source, &model, 0));
		UEB_CHECK(writer.used == with_stripped);
	}
}

static void model_export_into_a_full_region_writes_nothing(void)
{
	struct ue_bridge_load_writer writer;
	struct ue_bridge_model model;

	build_source();
	memset(&model, 0x5A, sizeof(model));
	ue_bridge_load_writer_init(&writer, (volatile uint8_t *)region, 600, 40);
	UEB_CHECK(!ue_bridge_model_export(&writer, &source, &model, 0));
	UEB_CHECK(writer.used == 40);
	UEB_CHECK(writer.overflow);
	UEB_CHECK(model.tag_index == 0x5A5A5A5A);
}

/* the remap is geometry_count int16s: a count that wraps a 32-bit size_t must fail the export */
static void model_export_refuses_a_geometry_count_whose_remap_would_wrap(void)
{
	static const uint32_t counts[] = { 0x80000000u, 0x80000002u };
	uint32_t index;

	for (index = 0; index < sizeof(counts) / sizeof(counts[0]); index++)
	{
		struct ue_bridge_load_writer writer;
		struct ue_bridge_model model;

		build_source();
		source.geometry_count = counts[index];
		ue_bridge_load_writer_init(&writer, (volatile uint8_t *)region, sizeof(region), 24);
		UEB_CHECK(!ue_bridge_model_export(&writer, &source, &model, 0));
		UEB_CHECK(writer.used == 24);
	}
}

/* a strip of 0x55555558 words lists 3 * 0x55555556 indices, which is 2 in 32 bits */
static void model_export_refuses_a_strip_whose_triangle_list_would_wrap(void)
{
	struct ue_bridge_load_writer writer;
	struct ue_bridge_model model;

	build_source();
	parts_b[0].strip_length = 0x55555558u;
	ue_bridge_load_writer_init(&writer, (volatile uint8_t *)region, sizeof(region), 24);
	UEB_CHECK(!ue_bridge_model_export(&writer, &source, &model, 0));
	UEB_CHECK(writer.used == 24);
}

/* The renderer repairs a vertex whose node bytes name a node the model lacks, and clamps a detail level's
node count into the model's nodes; the export counts both, for the load root. A part left out is not counted,
and neither is a geometry no permutation uses. */
static void model_export_counts_repaired_vertices_and_clamped_node_counts(void)
{
	struct ue_bridge_load_writer writer;
	struct ue_bridge_model model;
	struct ue_bridge_model_counts counts;

	build_source();
	/* the quad has a stripped twin, so it is written once: its first node out of range */
	quad[1].nodes[0] = 3 * 5;
	/* weight 1: the second node carries nothing, so its bad byte is left alone */
	quad[0].nodes[1] = 3 * 9;
	/* weight 0: the second node carries it all, and it is out of range */
	single[2].nodes[1] = 3 * 7;
	source.node_counts[1] = -1;
	source.node_counts[2] = 3;
	memset(&counts, 0, sizeof(counts));
	ue_bridge_load_writer_init(&writer, (volatile uint8_t *)region, sizeof(region), 0);
	UEB_CHECK(ue_bridge_model_export(&writer, &source, &model, &counts));
	UEB_CHECK(counts.repaired_vertices == 2);
	UEB_CHECK(counts.clamped_node_counts == 2);
	/* the counts add up across models */
	ue_bridge_load_writer_init(&writer, (volatile uint8_t *)region, sizeof(region), 0);
	UEB_CHECK(ue_bridge_model_export(&writer, &source, &model, &counts));
	UEB_CHECK(counts.repaired_vertices == 4 && counts.clamped_node_counts == 4);
}

static void model_export_counts_nothing_for_a_model_that_does_not_fit(void)
{
	struct ue_bridge_load_writer writer;
	struct ue_bridge_model model;
	struct ue_bridge_model_counts counts;

	build_source();
	quad[1].nodes[0] = 3 * 5;
	source.node_counts[2] = 3;
	memset(&counts, 0, sizeof(counts));
	ue_bridge_load_writer_init(&writer, (volatile uint8_t *)region, 600, 40);
	UEB_CHECK(!ue_bridge_model_export(&writer, &source, &model, &counts));
	UEB_CHECK(counts.repaired_vertices == 0 && counts.clamped_node_counts == 0);
}

const struct ueb_test ueb_model_tests[] =
{
	{ "model_export_writes_nodes_regions_and_shaders", model_export_writes_nodes_regions_and_shaders },
	{ "model_export_keeps_each_used_geometry_once", model_export_keeps_each_used_geometry_once },
	{ "model_export_decodes_parts_and_skips_stripped_ones", model_export_decodes_parts_and_skips_stripped_ones },
	{ "model_export_into_a_full_region_writes_nothing", model_export_into_a_full_region_writes_nothing },
	{ "model_export_refuses_a_geometry_count_whose_remap_would_wrap", model_export_refuses_a_geometry_count_whose_remap_would_wrap },
	{ "model_export_refuses_a_strip_whose_triangle_list_would_wrap", model_export_refuses_a_strip_whose_triangle_list_would_wrap },
	{ "model_export_counts_repaired_vertices_and_clamped_node_counts", model_export_counts_repaired_vertices_and_clamped_node_counts },
	{ "model_export_counts_nothing_for_a_model_that_does_not_fit", model_export_counts_nothing_for_a_model_that_does_not_fit },
	{ 0, 0 }
};

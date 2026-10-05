/*
TEST_BSP.C

BSP export (port/ue_bridge/ue_bridge_bsp.c): cluster surface lists to
cluster x shader batches.
*/

#include "ueb_test.h"
#include "ue_bridge_bsp.h"

#include <string.h>

static uint64_t region[65536 / 8];

#define AT(type, offset) ((type *)((uint8_t *)region + (offset)))

/* two materials with the same shader, one with another; 6 surfaces */
static struct ue_bridge_compressed_environment_vertex vertices_a[4];
static struct ue_bridge_compressed_environment_vertex vertices_b[3];
static struct ue_bridge_compressed_environment_vertex vertices_c[3];
static const uint16_t surfaces[6][3] =
{
	{ 0, 1, 2 }, { 0, 2, 3 },	/* material 0 (shader 0x10) */
	{ 0, 1, 2 }, { 2, 1, 0 },	/* material 1 (shader 0x10) */
	{ 0, 1, 2 },			/* material 2 (shader 0x20) */
	{ 1, 2, 0 }			/* material 2, listed by no cluster */
};
static struct ue_bridge_bsp_source_material materials[3];
/* cluster 0: surfaces 0, 2, 4; cluster 1: 1, 3 and 0 again (a duplicate) */
static const int32_t cluster0[] = { 0, 2, 4 };
static const int32_t cluster1[] = { 1, 3, 0 };
static const int32_t *const cluster_surfaces[] = { cluster0, cluster1 };
static const uint32_t cluster_counts[] = { 3, 3 };
static struct ue_bridge_bsp_source source;

static void build_source(void)
{
	int index;

	memset(vertices_a, 0, sizeof(vertices_a));
	memset(vertices_b, 0, sizeof(vertices_b));
	memset(vertices_c, 0, sizeof(vertices_c));
	for (index = 0; index < 4; index++)
		vertices_a[index].position[0] = (float)(10 + index);
	for (index = 0; index < 3; index++)
	{
		vertices_b[index].position[0] = (float)(20 + index);
		vertices_c[index].position[0] = (float)(30 + index);
	}
	materials[0].shader_tag = 0x10;
	materials[0].first_surface = 0;
	materials[0].surface_count = 2;
	materials[0].vertices = vertices_a;
	materials[0].vertex_count = 4;
	materials[1].shader_tag = 0x10;
	materials[1].first_surface = 2;
	materials[1].surface_count = 2;
	materials[1].vertices = vertices_b;
	materials[1].vertex_count = 3;
	materials[2].shader_tag = 0x20;
	materials[2].first_surface = 4;
	materials[2].surface_count = 2;
	materials[2].vertices = vertices_c;
	materials[2].vertex_count = 3;
	memset(&source, 0, sizeof(source));
	source.surfaces = &surfaces[0][0];
	source.surface_count = 6;
	source.materials = materials;
	source.material_count = 3;
	source.cluster_surfaces = cluster_surfaces;
	source.cluster_surface_counts = cluster_counts;
	source.cluster_count = 2;
}

static const struct ue_bridge_bsp_batch *find_batch(const struct ue_bridge_bsp_entry *entry, int16_t cluster, int32_t shader)
{
	const struct ue_bridge_bsp_batch *batches = AT(struct ue_bridge_bsp_batch, entry->batches.offset);
	uint32_t index;

	for (index = 0; index < entry->batches.count; index++)
	{
		if (batches[index].cluster == cluster && batches[index].shader_tag == shader)
			return &batches[index];
	}
	return 0;
}

static void unpack_cluster_list_skips_group_headers(void)
{
	/* lightmap 0 material 1: 2 surfaces; lightmap 1 material 0: 1 surface */
	static const int32_t packed[] = { 0, 1, 2, 7, 8, 1, 0, 1, 9 };
	int32_t surfaces_out[8];

	UEB_CHECK(ue_bridge_bsp_unpack_cluster_list(packed, 9, surfaces_out, 8) == 3);
	UEB_CHECK(surfaces_out[0] == 7 && surfaces_out[1] == 8 && surfaces_out[2] == 9);
}

static void unpack_cluster_list_stops_at_a_group_past_the_end(void)
{
	/* the group claims 5 surfaces, only 1 follows */
	static const int32_t packed[] = { 0, 0, 5, 3 };
	int32_t surfaces_out[8];

	UEB_CHECK(ue_bridge_bsp_unpack_cluster_list(packed, 4, surfaces_out, 8) == 1);
	UEB_CHECK(surfaces_out[0] == 3);
}

static void bsp_export_groups_by_cluster_and_shader(void)
{
	struct ue_bridge_load_writer writer;
	struct ue_bridge_bsp_entry entry;
	const struct ue_bridge_bsp_batch *batch;

	build_source();
	ue_bridge_load_writer_init(&writer, (volatile uint8_t *)region, sizeof(region), 0);
	UEB_CHECK(ue_bridge_bsp_export(&writer, &source, &entry));
	UEB_CHECK(entry.cluster_count == 2);
	/* cluster 0: shader 0x10 (surfaces 0 and 2, from two materials) and 0x20
	(surface 4); cluster 1: shader 0x10 (1 and 3); cluster -1: 0x20 (5) */
	UEB_CHECK(entry.batches.count == 4);
	batch = find_batch(&entry, 0, 0x10);
	UEB_CHECK(batch && batch->indices.count == 6);
	/* material 0's three vertices and material 1's three, each once */
	UEB_CHECK(batch->vertices.count == 6);
	batch = find_batch(&entry, 0, 0x20);
	UEB_CHECK(batch && batch->indices.count == 3);
	batch = find_batch(&entry, 1, 0x10);
	UEB_CHECK(batch && batch->indices.count == 6);
	batch = find_batch(&entry, -1, 0x20);
	UEB_CHECK(batch && batch->indices.count == 3);
}

static void bsp_export_counts_unclustered_and_duplicate_surfaces(void)
{
	struct ue_bridge_load_writer writer;
	struct ue_bridge_bsp_entry entry;

	build_source();
	ue_bridge_load_writer_init(&writer, (volatile uint8_t *)region, sizeof(region), 0);
	UEB_CHECK(ue_bridge_bsp_export(&writer, &source, &entry));
	UEB_CHECK(entry.unclustered_surfaces == 1);
	UEB_CHECK(entry.duplicate_surfaces == 1);
}

static void bsp_export_remaps_vertices_per_batch(void)
{
	struct ue_bridge_load_writer writer;
	struct ue_bridge_bsp_entry entry;
	const struct ue_bridge_bsp_batch *batch;
	const struct ue_bridge_bsp_vertex *vertices;
	const ue_bridge_bsp_index *indices;
	uint32_t index;

	build_source();
	ue_bridge_load_writer_init(&writer, (volatile uint8_t *)region, sizeof(region), 0);
	UEB_CHECK(ue_bridge_bsp_export(&writer, &source, &entry));
	batch = find_batch(&entry, 0, 0x10);
	vertices = AT(struct ue_bridge_bsp_vertex, batch->vertices.offset);
	indices = AT(ue_bridge_bsp_index, batch->indices.offset);
	/* surface 0 (material 0: vertices 10 11 12), then surface 2 (material 1: 20 21 22) */
	UEB_CHECK(vertices[indices[0]].position[0] == 10.0f);
	UEB_CHECK(vertices[indices[1]].position[0] == 11.0f);
	UEB_CHECK(vertices[indices[2]].position[0] == 12.0f);
	UEB_CHECK(vertices[indices[3]].position[0] == 20.0f);
	UEB_CHECK(vertices[indices[5]].position[0] == 22.0f);
	for (index = 0; index < batch->indices.count; index++)
		UEB_CHECK(indices[index] < batch->vertices.count);
}

static void bsp_export_skips_surfaces_out_of_range(void)
{
	static const int32_t bad_cluster[] = { 99, -4, 1, 6 };
	static const int32_t *const bad_surfaces[] = { bad_cluster };
	static const uint32_t bad_counts[] = { 4 };
	struct ue_bridge_load_writer writer;
	struct ue_bridge_bsp_entry entry;

	build_source();
	source.cluster_surfaces = bad_surfaces;
	source.cluster_surface_counts = bad_counts;
	source.cluster_count = 1;
	ue_bridge_load_writer_init(&writer, (volatile uint8_t *)region, sizeof(region), 0);
	UEB_CHECK(ue_bridge_bsp_export(&writer, &source, &entry));
	UEB_CHECK(find_batch(&entry, 0, 0x10) && find_batch(&entry, 0, 0x10)->indices.count == 3);
	UEB_CHECK(entry.unclustered_surfaces == 5);
	/* an index past the table (99, and 6, one past the last surface) must be
	skipped before it is looked up, not read out of bounds and counted as a
	duplicate */
	UEB_CHECK(entry.duplicate_surfaces == 0);
}

#define HUGE_CLUSTERS 131072u
static const int32_t *huge_surfaces[HUGE_CLUSTERS];
static uint32_t huge_counts[HUGE_CLUSTERS];
static struct ue_bridge_bsp_source_material huge_materials[32768];

static void bsp_export_does_not_store_a_cluster_the_index_type_cannot_hold(void)
{
	static const int32_t last[] = { 5 };
	struct ue_bridge_load_writer writer;
	struct ue_bridge_bsp_entry entry;
	const struct ue_bridge_bsp_batch *batches;
	uint32_t index;

	build_source();
	memset(huge_counts, 0, sizeof(huge_counts));
	huge_surfaces[0] = cluster0;
	huge_counts[0] = 3;
	huge_surfaces[1] = cluster1;
	huge_counts[1] = 3;
	/* one cluster past the type: its index would wrap to the lowest value */
	huge_surfaces[UE_BRIDGE_MAX_CLUSTERS] = last;
	huge_counts[UE_BRIDGE_MAX_CLUSTERS] = 1;
	source.cluster_surfaces = huge_surfaces;
	source.cluster_surface_counts = huge_counts;
	source.cluster_count = UE_BRIDGE_MAX_CLUSTERS + 1u;
	ue_bridge_load_writer_init(&writer, (volatile uint8_t *)region, sizeof(region), 0);
	UEB_CHECK(ue_bridge_bsp_export(&writer, &source, &entry));
	batches = AT(struct ue_bridge_bsp_batch, entry.batches.offset);
	for (index = 0; index < entry.batches.count; index++)
		UEB_CHECK(batches[index].cluster >= -1);
	/* the surface it listed stays reachable, in the no-cluster batch */
	UEB_CHECK(find_batch(&entry, -1, 0x20) && find_batch(&entry, -1, 0x20)->indices.count == 3);
	UEB_CHECK(entry.unclustered_surfaces == 1);
}

static void bsp_export_sizes_its_batches_by_surfaces_not_by_a_wrapping_product(void)
{
	struct ue_bridge_load_writer writer;
	struct ue_bridge_bsp_entry entry;
	uint32_t index;

	build_source();
	memset(huge_counts, 0, sizeof(huge_counts));
	huge_surfaces[0] = cluster0;
	huge_counts[0] = 3;
	huge_surfaces[1] = cluster1;
	huge_counts[1] = 3;
	for (index = 0; index < 3; index++)
		huge_materials[index] = materials[index];
	for (; index < 32768; index++)
		memset(&huge_materials[index], 0, sizeof(huge_materials[index])), huge_materials[index].first_surface = 6;
	source.materials = huge_materials;
	source.material_count = 32768;
	source.cluster_surfaces = huge_surfaces;
	source.cluster_surface_counts = huge_counts;
	/* (clusters + 1) * materials is 2^32: a batch array sized by it is empty */
	source.cluster_count = HUGE_CLUSTERS - 1u;
	ue_bridge_load_writer_init(&writer, (volatile uint8_t *)region, sizeof(region), 0);
	UEB_CHECK(ue_bridge_bsp_export(&writer, &source, &entry));
	UEB_CHECK(entry.batches.count == 4);
}

static void bsp_export_drops_a_surface_naming_a_missing_vertex(void)
{
	static uint16_t damaged[6][3];
	struct ue_bridge_load_writer writer;
	struct ue_bridge_bsp_entry entry;

	build_source();
	memcpy(damaged, surfaces, sizeof(damaged));
	/* material 0 has 4 vertices */
	damaged[1][2] = 9;
	source.surfaces = &damaged[0][0];
	ue_bridge_load_writer_init(&writer, (volatile uint8_t *)region, sizeof(region), 0);
	UEB_CHECK(ue_bridge_bsp_export(&writer, &source, &entry));
	UEB_CHECK(find_batch(&entry, 1, 0x10) && find_batch(&entry, 1, 0x10)->indices.count == 3);
}

static void bsp_export_into_a_full_region_leaves_the_entry_unready(void)
{
	struct ue_bridge_load_writer writer;
	struct ue_bridge_bsp_entry entry;

	build_source();
	memset(&entry, 0, sizeof(entry));
	ue_bridge_load_writer_init(&writer, (volatile uint8_t *)region, 200, 16);
	UEB_CHECK(!ue_bridge_bsp_export(&writer, &source, &entry));
	UEB_CHECK(writer.used == 16);
	UEB_CHECK(entry.ready == 0 && entry.batches.count == 0);
}

const struct ueb_test ueb_bsp_tests[] =
{
	{ "unpack_cluster_list_skips_group_headers", unpack_cluster_list_skips_group_headers },
	{ "unpack_cluster_list_stops_at_a_group_past_the_end", unpack_cluster_list_stops_at_a_group_past_the_end },
	{ "bsp_export_groups_by_cluster_and_shader", bsp_export_groups_by_cluster_and_shader },
	{ "bsp_export_counts_unclustered_and_duplicate_surfaces", bsp_export_counts_unclustered_and_duplicate_surfaces },
	{ "bsp_export_remaps_vertices_per_batch", bsp_export_remaps_vertices_per_batch },
	{ "bsp_export_skips_surfaces_out_of_range", bsp_export_skips_surfaces_out_of_range },
	{ "bsp_export_does_not_store_a_cluster_the_index_type_cannot_hold", bsp_export_does_not_store_a_cluster_the_index_type_cannot_hold },
	{ "bsp_export_sizes_its_batches_by_surfaces_not_by_a_wrapping_product", bsp_export_sizes_its_batches_by_surfaces_not_by_a_wrapping_product },
	{ "bsp_export_drops_a_surface_naming_a_missing_vertex", bsp_export_drops_a_surface_naming_a_missing_vertex },
	{ "bsp_export_into_a_full_region_leaves_the_entry_unready", bsp_export_into_a_full_region_leaves_the_entry_unready },
	{ 0, 0 }
};

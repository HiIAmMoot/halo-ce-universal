/*
UE_BRIDGE_EXPORT.C

The load region for the HaloCEUE renderer (port/ue_bridge/ue_bridge_format.h;
the Phase 0 design's section 5.3): the loaded map's object definitions and
models, and its BSPs, read through the game's own tag structures and written
by the shared writers (ue_bridge_model.c, ue_bridge_bsp.c). Runs on the game
thread at the end of a map's initialization (ue_bridge_game_map_ready) and
when a BSP is loaded (ue_bridge_game_structure_bsp_loaded): the game holds one
BSP in memory at a time, so a BSP is exported the first time it is loaded.
*/

#include "cseries.h"
#include "cache/cache_files.h"
#include "math/real_math.h"
#include "models/model_definitions.h"
#include "objects/object_definitions.h"
#include "rasterizer/rasterizer_geometry.h"
#include "rasterizer/rasterizer_model_types.h"
#include "scenario/scenario.h"
#include "scenario/scenario_definitions.h"
#include "shaders/shader_definitions.h"
#include "shaders/shaders.h"
#include "structures/structure_bsp_definitions.h"

#include "../src/ue_bridge.h"
#include "../src/ue_bridge_platform.h"
#include "../src/ue_bridge_world.h"
#include "../../ue_bridge/ue_bridge_bsp.h"
#include "../../ue_bridge/ue_bridge_model.h"

#include <stdlib.h>
#include <string.h>

/* models.c keeps its geometry structures private; these mirror them, and the
asserts tie them to the sizes models.c itself asserts */
struct export_shader_reference
{
	struct tag_reference shader;
	short permutation_index;
	word pad;
	long unused[3];
};

struct export_geometry
{
	byte reserved[0x24];
	struct tag_block parts;
};

struct export_geometry_part
{
	unsigned long flags;
	short shader_index;
	char previous_part_index;
	char next_part_index;
	short centroid_primary_node_index;
	short centroid_secondary_node_index;
	real centroid_primary_node_weight;
	real centroid_secondary_node_weight;
	real_point3d centroid;
	struct tag_block uncompressed_vertices;
	struct tag_block compressed_vertices;
	struct tag_block triangles;
	struct triangle_buffer triangle_buffer;
	struct vertex_buffer vertex_buffer;
};

/* An Xbox resource (D3DVertexBuffer, D3DIndexBuffer) starts Common, Data, Lock;
Data is a physical address, which the game's memory window (platform.h's
PLATFORM_CONTIGUOUS_BASE; the header needs windows.h, so it can't be included
here) maps at this base. */
struct export_resource
{
	unsigned long common;
	unsigned long data;
	unsigned long lock;
};

#define EXPORT_CONTIGUOUS_BASE 0x80000000UL

/* models.c's _model_geometry_part_stripped_bit */
#define EXPORT_PART_STRIPPED_BIT 0

/* structure_visibility.c's view of a cluster: the subclusters at 0x34, the
lens flare markers' two words, then the packed surface list at 0x44 */
struct export_cluster
{
	byte before[0x34];
	struct tag_block subclusters;
	byte lens_flare_markers[4];
	struct tag_block surface_indices;
	byte after[0x18];
};

/* the subcluster at structure_visibility.c: its bounds, then its surface list */
struct export_subcluster
{
	byte bounds[0x18];
	struct tag_block surface_indices;
};

typedef char export_subcluster_size[sizeof(struct export_subcluster) == 0x24 ? 1 : -1];
typedef char export_shader_reference_size[sizeof(struct export_shader_reference) == 0x20 ? 1 : -1];
typedef char export_geometry_part_size[sizeof(struct export_geometry_part) == 0x68 ? 1 : -1];
typedef char export_cluster_size[sizeof(struct export_cluster) == sizeof(struct structure_cluster) ? 1 : -1];
typedef char export_model_vertex_size[sizeof(struct model_vertex_compressed) == sizeof(struct ue_bridge_compressed_model_vertex) ? 1 : -1];
typedef char export_matrix_size[sizeof(real_matrix4x3) == sizeof(struct ue_bridge_matrix) ? 1 : -1];
typedef char export_surface_size[sizeof(struct structure_surface) == 3 * sizeof(uint16_t) ? 1 : -1];
/* the format's detail levels are the decomp's. models.c's own constant
(NUMBER_OF_DETAIL_LEVELS_PER_MODEL) is private to it, so these tie the format to
the tag structures' arrays that constant sizes. */
#define EXPORT_DETAIL_LEVELS (sizeof(((struct model *)0)->detail_cutoff_pixels) / sizeof(real))
typedef char export_detail_levels[UE_BRIDGE_DETAIL_LEVELS == EXPORT_DETAIL_LEVELS ? 1 : -1];
typedef char export_node_count_levels[sizeof(((struct model *)0)->node_counts) / sizeof(short) == UE_BRIDGE_DETAIL_LEVELS ? 1 : -1];
typedef char export_permutation_levels[sizeof(((struct model_region_permutation *)0)->geometry_indices) / sizeof(short) == UE_BRIDGE_DETAIL_LEVELS ? 1 : -1];
/* a cluster index is stored in a 16-bit field (the shared exporter clamps what
doesn't fit; this says the game never needs it to) */
typedef char export_cluster_limit[MAXIMUM_CLUSTERS_PER_STRUCTURE <= UE_BRIDGE_MAX_CLUSTERS ? 1 : -1];
typedef char export_datum_limit[(UNSIGNED_SHORT_MAX + 1) == UE_BRIDGE_DATUM_ABSOLUTE_LIMIT ? 1 : -1];
typedef char export_definition_limit[UE_BRIDGE_NO_DEFINITION == UE_BRIDGE_MAX_DEFINITIONS ? 1 : -1];
/* the format's field widths hold the game's limits: a vertex's node index is
a byte, a tick record's region_count is a byte, and tag and datum absolute
indices stay below UE_BRIDGE_DATUM_ABSOLUTE_LIMIT. A port that raises a
limit past a width fails here, not in the field. */
typedef char export_node_index_width[MAXIMUM_NODES_PER_MODEL <= 256 ? 1 : -1];
typedef char export_region_count_width[MAXIMUM_REGIONS_PER_OBJECT <= 255 ? 1 : -1];

static struct
{
	int exported;
	/* parts whose first normal the shared decode reads differently from the game */
	unsigned long normal_mismatches;
	/* map exports whose definition or model count passed what the format's 16-bit indices hold, since the game started */
	unsigned long refused_exports;
	/* by tag absolute index */
	unsigned short definition_of_tag[UE_BRIDGE_DATUM_ABSOLUTE_LIMIT];
	short model_of_tag[UE_BRIDGE_DATUM_ABSOLUTE_LIMIT];
} export_state;

unsigned short ue_bridge_export_definition_index(long definition_tag_index)
{
	if (!export_state.exported || definition_tag_index == NONE)
		return UE_BRIDGE_NO_DEFINITION;
	return export_state.definition_of_tag[DATUM_INDEX_TO_ABSOLUTE_INDEX(definition_tag_index)];
}

/* count + 1 zeroed elements, sized by the shared overflow-checked helper; NULL
when they don't fit a size_t (a crafted map's block counts reach it unchecked).
The game's cseries.h redefines malloc and free as its tracked allocator and
leaves calloc alone, so a calloc block freed here halts the game on a bad
header: allocate through malloc, as the game does. */
static void *export_zeroed_allocate(uint64_t count, size_t size)
{
	size_t bytes;
	void *block;

	if (!ue_bridge_padded_array_bytes(count, size, &bytes))
		return 0;
	block = malloc(bytes);
	if (block)
		memset(block, 0, bytes);
	return block;
}

static boolean part_shader_is_drawable(struct model *model, short shader_index)
{
	struct export_shader_reference *reference;

	if (shader_index < 0 || shader_index >= model->shaders.count)
		return FALSE;
	reference = TAG_BLOCK_GET_ELEMENT(&model->shaders, shader_index, struct export_shader_reference);
	return reference->shader.index != NONE &&
		shader_type_is_valid_for_model(shader_definition_get(reference->shader.index)->base.type);
}

static int export_model(struct ue_bridge_load_writer *writer, long model_tag_index, struct ue_bridge_model *out)
{
	struct model *model = model_definition_get(model_tag_index);
	struct ue_bridge_model_source source;
	struct ue_bridge_model_source_node *nodes;
	struct ue_bridge_model_source_region *regions;
	int16_t (*permutations)[UE_BRIDGE_DETAIL_LEVELS];
	struct ue_bridge_model_source_geometry *geometries;
	struct ue_bridge_model_source_part *parts;
	int32_t *shader_tags;
	long index, inner, cursor;
	uint64_t permutation_total = 0, part_total = 0;
	int result = 0;

	nodes = 0;
	regions = 0;
	permutations = 0;
	geometries = 0;
	parts = 0;
	shader_tags = 0;
	/* A negative count would become a huge unsigned one in the source's
	fields, and one in a sum would hide a later block's real size: the model
	is damaged, so it isn't exported. */
	if (model->nodes.count < 0 || model->regions.count < 0 || model->geometries.count < 0 || model->shaders.count < 0)
		goto done;
	for (index = 0; index < model->regions.count; index++)
	{
		long count = TAG_BLOCK_GET_ELEMENT(&model->regions, index, struct model_region)->permutations.count;

		if (count < 0)
			goto done;
		permutation_total += (uint64_t)count;
	}
	for (index = 0; index < model->geometries.count; index++)
	{
		long count = TAG_BLOCK_GET_ELEMENT(&model->geometries, index, struct export_geometry)->parts.count;

		if (count < 0)
			goto done;
		part_total += (uint64_t)count;
	}
	nodes = (struct ue_bridge_model_source_node *)export_zeroed_allocate((uint64_t)model->nodes.count, sizeof(*nodes));
	regions = (struct ue_bridge_model_source_region *)export_zeroed_allocate((uint64_t)model->regions.count, sizeof(*regions));
	permutations = (int16_t (*)[UE_BRIDGE_DETAIL_LEVELS])export_zeroed_allocate(permutation_total, sizeof(*permutations));
	geometries = (struct ue_bridge_model_source_geometry *)export_zeroed_allocate((uint64_t)model->geometries.count, sizeof(*geometries));
	parts = (struct ue_bridge_model_source_part *)export_zeroed_allocate(part_total, sizeof(*parts));
	shader_tags = (int32_t *)export_zeroed_allocate((uint64_t)model->shaders.count, sizeof(*shader_tags));
	if (!nodes || !regions || !permutations || !geometries || !parts || !shader_tags)
		goto done;

	memset(&source, 0, sizeof(source));
	source.tag_index = model_tag_index;
	for (index = 0; index < (long)UE_BRIDGE_DETAIL_LEVELS; index++)
	{
		source.detail_cutoff_pixels[index] = model->detail_cutoff_pixels[index];
		source.node_counts[index] = model->node_counts[index];
	}
	for (index = 0; index < model->nodes.count; index++)
	{
		struct model_node *node = TAG_BLOCK_GET_ELEMENT(&model->nodes, index, struct model_node);
		real_matrix4x3 local;

		/* the game's own conversion: its quaternions are not the usual convention */
		matrix4x3_from_point_and_quaternion(&local, &node->default_translation, &node->default_rotation);
		nodes[index].parent = node->parent_node_index;
		memcpy(&nodes[index].default_local, &local, sizeof(local));
		memcpy(&nodes[index].default_inverse, &node->runtime_default_inverse_matrix, sizeof(real_matrix4x3));
	}
	for (index = 0, cursor = 0; index < model->regions.count; index++)
	{
		struct model_region *region = TAG_BLOCK_GET_ELEMENT(&model->regions, index, struct model_region);

		regions[index].permutations = (const int16_t (*)[UE_BRIDGE_DETAIL_LEVELS])&permutations[cursor];
		regions[index].permutation_count = region->permutations.count;
		for (inner = 0; inner < region->permutations.count; inner++, cursor++)
		{
			struct model_region_permutation *permutation =
				TAG_BLOCK_GET_ELEMENT(&region->permutations, inner, struct model_region_permutation);

			memcpy(permutations[cursor], permutation->geometry_indices, sizeof(permutations[cursor]));
		}
	}
	for (index = 0, cursor = 0; index < model->geometries.count; index++)
	{
		struct export_geometry *geometry = TAG_BLOCK_GET_ELEMENT(&model->geometries, index, struct export_geometry);

		geometries[index].parts = &parts[cursor];
		geometries[index].part_count = geometry->parts.count;
		for (inner = 0; inner < geometry->parts.count; inner++, cursor++)
		{
			struct export_geometry_part *part = TAG_BLOCK_GET_ELEMENT(&geometry->parts, inner, struct export_geometry_part);
			struct ue_bridge_model_source_part *exported = &parts[cursor];
			/* The tag's vertex and triangle blocks are empty once the game has loaded
			the model: the data lives in the part's buffers (rasterizer_model_draw
			draws from them), the vertices in the buffer's resource (the part's
			base_address is not that memory: reading it runs off its block). A
			strip of count triangles is count + 2 words
			(rasterizer_triangle_buffer_new sizes it 2 * count + 4 bytes). */
			struct export_resource *vertex_resource = (struct export_resource *)part->vertex_buffer.hardware_format;
			boolean has_vertices = part->vertex_buffer.type == _rasterizer_vertex_type_model_compressed &&
				part->vertex_buffer.offset == 0 && part->vertex_buffer.count > 0 && vertex_resource && vertex_resource->data;
			boolean has_strip = part->triangle_buffer.type == _triangle_buffer_type_precompiled_strip &&
				part->triangle_buffer.count > 0;

			exported->shader = part->shader_index;
			exported->vertices = has_vertices ?
				(const struct ue_bridge_compressed_model_vertex *)(void *)(vertex_resource->data | EXPORT_CONTIGUOUS_BASE) : 0;
			exported->vertex_count = has_vertices ? (uint32_t)part->vertex_buffer.count : 0u;
			exported->strip = has_strip ? (const uint16_t *)part->triangle_buffer.base_address : 0;
			exported->strip_length = has_strip ? (uint32_t)part->triangle_buffer.count + 2u : 0u;
			exported->skip = TEST_FLAG(part->flags, EXPORT_PART_STRIPPED_BIT) ||
				!part_shader_is_drawable(model, part->shader_index) ||
				!exported->vertices || !exported->vertex_count || !exported->strip;
			if (!exported->skip)
			{
				/* the shared decode must agree with the game's own */
				real_vector3d game = uncompress_int32_to_real_vector3d(exported->vertices[0].normal);
				float shared[3];

				ue_bridge_unpack_normal(exported->vertices[0].normal, shared);
				if (game.i != shared[0] || game.j != shared[1] || game.k != shared[2])
					export_state.normal_mismatches++;
			}
		}
	}
	for (index = 0; index < model->shaders.count; index++)
		shader_tags[index] = TAG_BLOCK_GET_ELEMENT(&model->shaders, index, struct export_shader_reference)->shader.index;

	source.nodes = nodes;
	source.node_count = model->nodes.count;
	source.regions = regions;
	source.region_count = model->regions.count;
	source.geometries = geometries;
	source.geometry_count = model->geometries.count;
	source.shader_tags = shader_tags;
	source.shader_count = model->shaders.count;
	result = ue_bridge_model_export(writer, &source, out);

done:
	free(nodes);
	free(regions);
	free(permutations);
	free(geometries);
	free(parts);
	free(shader_tags);
	return result;
}

static int compare_materials(void const *a, void const *b)
{
	uint32_t first_a = ((struct ue_bridge_bsp_source_material const *)a)->first_surface;
	uint32_t first_b = ((struct ue_bridge_bsp_source_material const *)b)->first_surface;

	return first_a < first_b ? -1 : first_a > first_b;
}

static long global_scenario_get_structure_bsp_tag(short structure_bsp_index)
{
	return TAG_BLOCK_GET_ELEMENT(&global_scenario->structure_bsp_references, structure_bsp_index,
		struct scenario_structure_bsp_reference)->structure_bsp.index;
}

/* the loaded BSP into the region; 1 when it fit */
static int export_loaded_bsp(struct ue_bridge_load_writer *writer, short structure_bsp_index)
{
	struct structure_bsp *bsp;
	struct ue_bridge_load_root *root = ue_bridge_load_root();
	struct ue_bridge_bsp_entry *entry;
	struct ue_bridge_bsp_source source;
	struct ue_bridge_bsp_source_material *materials = 0;
	int32_t **cluster_surfaces = 0;
	uint32_t *cluster_counts = 0;
	/* per surface: the last cluster whose subclusters listed it */
	long *last_cluster_listing = 0;
	long lightmap_index, material_index, cluster_index, cursor = 0;
	uint64_t material_total = 0;
	unsigned long started_ms = system_milliseconds();
	int result = 0;

	if (structure_bsp_index == NONE || structure_bsp_index != global_structure_bsp_index || structure_bsp_index < 0)
		return 0;
	/* no entry: the BSP table itself didn't fit, and the map export set the missing bit */
	entry = ue_bridge_load_bsp_slot((uint32_t)structure_bsp_index);
	if (!entry)
		return 0;
	/* global_structure_bsp has no extern; its getter asserts it is set, which
	the index check above guarantees */
	bsp = global_structure_bsp_get();
	/* a negative count would become a huge unsigned one in the source below */
	if (bsp->lightmaps.count < 0 || bsp->clusters.count < 0 || bsp->surfaces.count < 0)
		goto done;
	for (lightmap_index = 0; lightmap_index < bsp->lightmaps.count; lightmap_index++)
	{
		long count = TAG_BLOCK_GET_ELEMENT(&bsp->lightmaps, lightmap_index, struct structure_lightmap)->materials.count;

		if (count < 0)
			goto done;
		material_total += (uint64_t)count;
	}
	materials = (struct ue_bridge_bsp_source_material *)export_zeroed_allocate(material_total, sizeof(*materials));
	cluster_surfaces = (int32_t **)export_zeroed_allocate((uint64_t)bsp->clusters.count, sizeof(*cluster_surfaces));
	cluster_counts = (uint32_t *)export_zeroed_allocate((uint64_t)bsp->clusters.count, sizeof(*cluster_counts));
	last_cluster_listing = (long *)export_zeroed_allocate((uint64_t)bsp->surfaces.count, sizeof(*last_cluster_listing));
	if (!materials || !cluster_surfaces || !cluster_counts || !last_cluster_listing)
		goto done;
	for (cluster_index = 0; cluster_index < bsp->surfaces.count; cluster_index++)
		last_cluster_listing[cluster_index] = NONE;
	for (lightmap_index = 0; lightmap_index < bsp->lightmaps.count; lightmap_index++)
	{
		struct structure_lightmap *lightmap = TAG_BLOCK_GET_ELEMENT(&bsp->lightmaps, lightmap_index, struct structure_lightmap);

		for (material_index = 0; material_index < lightmap->materials.count; material_index++)
		{
			struct structure_material *material = TAG_BLOCK_GET_ELEMENT(&lightmap->materials, material_index, struct structure_material);

			/* the Xbox's vertices are always compressed; anything else is skipped */
			if (material->vertices.type != _rasterizer_vertex_type_environment_compressed || !material->compressed_vertex_data.address)
				continue;
			materials[cursor].shader_tag = material->shader.index;
			materials[cursor].first_surface = (uint32_t)material->first_surface_index;
			materials[cursor].surface_count = (uint32_t)material->surface_count;
			materials[cursor].vertices = (const struct ue_bridge_compressed_environment_vertex *)material->compressed_vertex_data.address;
			materials[cursor].vertex_count = (uint32_t)material->vertices.count;
			cursor++;
		}
	}
	qsort(materials, cursor, sizeof(*materials), compare_materials);
	for (cluster_index = 0; cluster_index < bsp->clusters.count; cluster_index++)
	{
		struct export_cluster *cluster = TAG_BLOCK_GET_ELEMENT(&bsp->clusters, cluster_index, struct export_cluster);
		uint32_t packed_count;
		uint32_t capacity;
		long subcluster_index;
		size_t list_bytes;

		if (cluster->surface_indices.count < 0 || cluster->subclusters.count < 0)
			goto done;
		packed_count = (uint32_t)cluster->surface_indices.count;
		capacity = packed_count;
		if (!packed_count)
		{
			/* the dedup below writes each surface at most once, so the BSP's surface count bounds the sum */
			for (subcluster_index = 0; subcluster_index < cluster->subclusters.count; subcluster_index++)
			{
				long listed_count = TAG_BLOCK_GET_ELEMENT(&cluster->subclusters, subcluster_index, struct export_subcluster)->surface_indices.count;

				if (listed_count < 0)
					goto done;
				capacity = ue_bridge_bsp_capacity_add(capacity, (uint32_t)listed_count, (uint32_t)bsp->surfaces.count);
			}
		}
		if (!ue_bridge_padded_array_bytes(capacity, sizeof(int32_t), &list_bytes))
			goto done;
		cluster_surfaces[cluster_index] = (int32_t *)malloc(list_bytes);
		if (!cluster_surfaces[cluster_index])
			goto done;
		if (packed_count)
		{
			cluster_counts[cluster_index] = ue_bridge_bsp_unpack_cluster_list(
				(const int32_t *)cluster->surface_indices.address, packed_count, cluster_surfaces[cluster_index], capacity);
			continue;
		}
		/* The shipped maps' clusters carry no surface list of their own (the PC
		tags leave it empty); the subclusters' lists are what the game draws
		from (structure_visibility_traverse_subclusters). A surface in two
		subclusters of one cluster is listed once here, so the exporter's
		duplicate count means a surface in two clusters. */
		for (subcluster_index = 0; subcluster_index < cluster->subclusters.count; subcluster_index++)
		{
			struct export_subcluster *subcluster = TAG_BLOCK_GET_ELEMENT(&cluster->subclusters, subcluster_index, struct export_subcluster);
			long listed;

			for (listed = 0; listed < subcluster->surface_indices.count; listed++)
			{
				long surface = *TAG_BLOCK_GET_ELEMENT(&subcluster->surface_indices, listed, long);

				if (surface < 0 || surface >= bsp->surfaces.count || last_cluster_listing[surface] == cluster_index)
					continue;
				last_cluster_listing[surface] = cluster_index;
				cluster_surfaces[cluster_index][cluster_counts[cluster_index]++] = (int32_t)surface;
			}
		}
	}
	memset(&source, 0, sizeof(source));
	source.surfaces = (const uint16_t *)bsp->surfaces.address;
	source.surface_count = (uint32_t)bsp->surfaces.count;
	source.materials = materials;
	source.material_count = (uint32_t)cursor;
	source.cluster_surfaces = (const int32_t *const *)cluster_surfaces;
	source.cluster_surface_counts = cluster_counts;
	source.cluster_count = (uint32_t)bsp->clusters.count;
	entry->tag_index = global_scenario_get_structure_bsp_tag(structure_bsp_index);
	result = ue_bridge_bsp_export(writer, &source, entry);
	if (result)
		ue_bridge_load_publish_bsp(structure_bsp_index, 1);
	ue_bridge_log("ue bridge: BSP %d: %lu batches from %lu clusters, %lu surfaces in no cluster, %lu in two; %s in %lu ms",
		(int)structure_bsp_index, (unsigned long)entry->batches.count, (unsigned long)source.cluster_count,
		(unsigned long)entry->unclustered_surfaces, (unsigned long)entry->duplicate_surfaces, result ? "exported" : "did not fit",
		(unsigned long)(system_milliseconds() - started_ms));

done:
	/* Every failure past the entry (the region is full, or an allocation failed)
	lands here: the BSP is still marked ready, with no batches, so UE stops
	waiting for it, and the missing bit says why, on the map export's path and
	on a later switch's alike. An export must never publish itself complete
	with a loaded BSP absent. */
	if (!result)
	{
		root->missing |= UE_BRIDGE_MISSING_BSPS;
		ue_bridge_load_publish_bsp(structure_bsp_index, 0);
	}
	if (cluster_surfaces)
	{
		for (cluster_index = 0; cluster_index < bsp->clusters.count; cluster_index++)
			free(cluster_surfaces[cluster_index]);
	}
	free(cluster_surfaces);
	free(cluster_counts);
	free(last_cluster_listing);
	free(materials);
	return result;
}

int ue_bridge_world_export_map(void)
{
	struct ue_bridge_load_writer *writer = ue_bridge_load_begin();
	struct ue_bridge_load_root *root = ue_bridge_load_root();
	struct ue_bridge_definition *definitions = 0;
	struct ue_bridge_model *models = 0;
	struct tag_iterator iterator;
	long tag_index, *model_tags = 0;
	uint32_t definition_count = 0, model_count = 0, exported_models = 0, index;
	unsigned long started_ms = system_milliseconds();
	uint32_t definitions_offset, models_offset, bsps_offset, bsp_count;
	int complete;

	if (!writer)
		return 0;
	export_state.exported = 0;
	export_state.normal_mismatches = 0;
	memset(export_state.definition_of_tag, 0xFF, sizeof(export_state.definition_of_tag));
	memset(export_state.model_of_tag, 0xFF, sizeof(export_state.model_of_tag));
	/* the root sits at the region's start */
	ue_bridge_load_reserve(writer, 1, sizeof(struct ue_bridge_load_root));
	root->magic = UE_BRIDGE_LOAD_MAGIC;
	root->load_epoch = ue_bridge_load_epoch();
	csstrncpy(root->map_name, tag_get_name(global_scenario_index), sizeof(root->map_name) - 1);
	/* the game's limits, so UE validates against this game and not a copy of its constants */
	root->max_nodes_per_model = MAXIMUM_NODES_PER_MODEL;
	root->max_regions_per_model = MAXIMUM_REGIONS_PER_MODEL;
	root->max_permutations_per_region = MAXIMUM_PERMUTATIONS_PER_MODEL_REGION;
	root->max_regions_per_object = MAXIMUM_REGIONS_PER_OBJECT;
	/* every scenario BSP gets an entry, however many there are; first after
	the root, because a BSP switch later appends into its entry */
	bsp_count = (uint32_t)global_scenario->structure_bsp_references.count;
	bsps_offset = ue_bridge_load_reserve(writer, bsp_count, sizeof(struct ue_bridge_bsp_entry));
	if (bsps_offset == UE_BRIDGE_NO_OFFSET)
	{
		root->missing |= UE_BRIDGE_MISSING_BSPS;
	}
	else
	{
		/* load_begin clears only the root: an entry must start unready */
		memset(ue_bridge_load_pointer(writer, bsps_offset), 0, bsp_count * sizeof(struct ue_bridge_bsp_entry));
		root->bsps.offset = bsps_offset;
		root->bsps.count = bsp_count;
		ue_bridge_load_set_bsp_table(bsps_offset, bsp_count);
	}

	/* definitions numbered in tag order; their models in order of first use */
	tag_iterator_new(&iterator, OBJECT_DEFINITION_TAG);
	while ((tag_index = tag_iterator_next(&iterator)) != NONE)
	{
		long model_tag = object_definition_get(tag_index)->object.model.index;

		/* past the limit the counts keep growing (the check below refuses the tables) but no index is stored, so none can wrap */
		if (definition_count < UE_BRIDGE_MAX_DEFINITIONS)
			export_state.definition_of_tag[DATUM_INDEX_TO_ABSOLUTE_INDEX(tag_index)] = (unsigned short)definition_count;
		definition_count++;
		if (model_tag != NONE && export_state.model_of_tag[DATUM_INDEX_TO_ABSOLUTE_INDEX(model_tag)] < 0)
		{
			if (model_count < UE_BRIDGE_MAX_MODELS)
				export_state.model_of_tag[DATUM_INDEX_TO_ABSOLUTE_INDEX(model_tag)] = (short)model_count;
			model_count++;
		}
	}
	if (!ue_bridge_index_counts_fit(definition_count, model_count))
	{
		export_state.refused_exports++;
		ue_bridge_log("ue bridge: export of %s refused (%lu so far): %lu definitions and %lu models, past what the format's 16-bit indices hold (%lu and %lu); no definition or model table is written",
			root->map_name, export_state.refused_exports, (unsigned long)definition_count, (unsigned long)model_count,
			(unsigned long)UE_BRIDGE_MAX_DEFINITIONS, (unsigned long)UE_BRIDGE_MAX_MODELS);
		root->missing |= UE_BRIDGE_MISSING_DEFINITIONS | UE_BRIDGE_MISSING_MODELS;
		memset(export_state.definition_of_tag, 0xFF, sizeof(export_state.definition_of_tag));
		memset(export_state.model_of_tag, 0xFF, sizeof(export_state.model_of_tag));
		definition_count = 0;
		model_count = 0;
	}
	model_tags = (long *)export_zeroed_allocate(model_count, sizeof(long));
	models = (struct ue_bridge_model *)export_zeroed_allocate(model_count, sizeof(*models));
	definitions = (struct ue_bridge_definition *)export_zeroed_allocate(definition_count, sizeof(*definitions));
	if (!model_tags || !models || !definitions)
	{
		writer->overflow = 1;
	}
	else
	{
		tag_iterator_new(&iterator, OBJECT_DEFINITION_TAG);
		while ((tag_index = tag_iterator_next(&iterator)) != NONE)
		{
			long model_tag = object_definition_get(tag_index)->object.model.index;

			/* (a refused export left every model unnumbered) */
			if (model_tag != NONE && export_state.model_of_tag[DATUM_INDEX_TO_ABSOLUTE_INDEX(model_tag)] >= 0)
				model_tags[export_state.model_of_tag[DATUM_INDEX_TO_ABSOLUTE_INDEX(model_tag)]] = model_tag;
		}
	}

	/* the definition table first: it is small, and without it nothing can be drawn */
	definitions_offset = ue_bridge_load_reserve(writer, definition_count, sizeof(struct ue_bridge_definition));
	if (writer->overflow)
		root->missing |= UE_BRIDGE_MISSING_DEFINITIONS;
	models_offset = ue_bridge_load_reserve(writer, model_count, sizeof(struct ue_bridge_model));
	for (index = 0; index < model_count && !writer->overflow; index++)
	{
		if (!export_model(writer, model_tags[index], &models[index]))
			break;
		exported_models++;
	}
	if (exported_models < model_count)
		root->missing |= UE_BRIDGE_MISSING_MODELS;
	if (!(root->missing & UE_BRIDGE_MISSING_DEFINITIONS))
	{
		index = 0;
		tag_iterator_new(&iterator, OBJECT_DEFINITION_TAG);
		while ((tag_index = tag_iterator_next(&iterator)) != NONE)
		{
			struct object_definition *definition = object_definition_get(tag_index);
			long model_tag = definition->object.model.index;
			short model = model_tag == NONE ? -1 : export_state.model_of_tag[DATUM_INDEX_TO_ABSOLUTE_INDEX(model_tag)];

			definitions[index].tag_index = tag_index;
			definitions[index].object_type = definition->object.type;
			definitions[index].model = model >= 0 && (uint32_t)model < exported_models ? model : -1;
			definitions[index].animation_graph_tag = definition->object.animation_graph.index;
			definitions[index].bounding_radius = definition->object.bounding_radius;
			index++;
		}
		memcpy((void *)(uintptr_t)ue_bridge_load_at(writer, definitions_offset), definitions, definition_count * sizeof(*definitions));
		root->definitions.offset = definitions_offset;
		root->definitions.count = definition_count;
	}
	if (models_offset != UE_BRIDGE_NO_OFFSET && exported_models)
	{
		memcpy((void *)(uintptr_t)ue_bridge_load_at(writer, models_offset), models, exported_models * sizeof(*models));
		root->models.offset = models_offset;
		root->models.count = exported_models;
	}
	free(model_tags);
	free(models);
	free(definitions);

	if (global_structure_bsp_index != NONE)
		export_loaded_bsp(writer, global_structure_bsp_index);
	complete = root->missing == 0;
	ue_bridge_load_end(complete);
	export_state.exported = 1;
	ue_bridge_log("ue bridge: exported %s: %lu definitions, %lu of %lu models, %lu of %lu KB in %lu ms; %lu normal decode mismatches%s",
		root->map_name, (unsigned long)root->definitions.count, (unsigned long)exported_models, (unsigned long)model_count,
		(unsigned long)(writer->used >> 10), (unsigned long)(writer->capacity >> 10),
		(unsigned long)(system_milliseconds() - started_ms), export_state.normal_mismatches,
		complete ? "" : "; INCOMPLETE (raise ue_bridge.section_mb)");
	return complete;
}

void ue_bridge_world_export_bsp(short structure_bsp_index)
{
	struct ue_bridge_load_writer *writer = ue_bridge_load_append();
	struct ue_bridge_load_root *root = ue_bridge_load_root();
	struct ue_bridge_bsp_entry *entry;

	/* A map's own first BSP is loaded by scenario_load, inside game_load, before
	game_initialize_for_new_map bumps load_epoch: at that point the append
	writer still belongs to the previous map. The map name tells them apart,
	and the map export picks the BSP up itself. Also: no export yet, or this
	BSP is in already. */
	if (!writer || structure_bsp_index < 0 ||
		strncmp(root->map_name, tag_get_name(global_scenario_index), sizeof(root->map_name) - 1) != 0)
	{
		return;
	}
	entry = ue_bridge_load_bsp_slot((uint32_t)structure_bsp_index);
	if (!entry || entry->ready)
		return;
	export_loaded_bsp(writer, structure_bsp_index);
}

void ue_bridge_world_set_start_map(const char *scenario_name)
{
	main_set_map_name(scenario_name);
}

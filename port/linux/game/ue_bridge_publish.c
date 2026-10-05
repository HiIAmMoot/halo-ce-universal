/*
UE_BRIDGE_PUBLISH.C

The game's per-tick and per-frame state for the HaloCEUE renderer
(port/ue_bridge/ue_bridge_format.h; the Phase 0 design's sections 4.5 and
4.6): every object, as the object array holds it after the tick, and window
0's camera as the classic renderer drew it. Also the load hold's game-side
calls.
*/

#include "cseries.h"
#include "game/game.h"
#include "main/main.h"
#include "models/model_definitions.h"
#include "objects/object_definitions.h"
#include "objects/objects.h"
#include "render/render_cameras.h"
#include "scenario/scenario.h"

#include "../src/ue_bridge.h"
#include "../src/ue_bridge_world.h"

typedef char publish_matrix_size[sizeof(real_matrix4x3) == sizeof(struct ue_bridge_matrix) ? 1 : -1];

void ue_bridge_world_publish_tick(uint64_t tick)
{
	struct ue_bridge_tick_writer *writer = ue_bridge_tick_begin(tick);
	struct object_iterator iterator;
	struct object_datum *object;

	if (!writer)
		return;
	/* the object array's own order: ascending absolute index, as the records must be */
	object_iterator_new(&iterator, _object_mask_all, 0);
	while ((object = (struct object_datum *)object_iterator_next(&iterator)) != NULL)
	{
		unsigned short definition = ue_bridge_export_definition_index(object->definition_index);
		long model_tag = object_definition_get(object->definition_index)->object.model.index;
		long node_count = object->object.node_matrices.size / (long)sizeof(real_matrix4x3);
		uint8_t region_count = 0;
		uint16_t flags = 0;

		if (definition == UE_BRIDGE_NO_DEFINITION || node_count < 0 || node_count > UINT16_MAX)
			continue;
		if (model_tag != NONE)
			region_count = (uint8_t)MIN(model_definition_get(model_tag)->regions.count, (long)MAXIMUM_REGIONS_PER_OBJECT);
		if (TEST_FLAG(object->object.flags, _object_invisible_bit))
			flags |= UE_BRIDGE_OBJECT_HIDDEN;
		if (render_interpolation_object_at_rest(iterator.index))
			flags |= UE_BRIDGE_OBJECT_AT_REST;
		/* outside a frame these are the tick's own matrices, not a blend */
		if (!ue_bridge_tick_writer_add(writer, (uint32_t)iterator.index, definition, flags, object->object.region_permutations,
			region_count, (const struct ue_bridge_matrix *)object_get_node_matrices(iterator.index), (uint16_t)node_count))
		{
			break;
		}
	}
	ue_bridge_tick_end(global_structure_bsp_index);
}

int ue_bridge_world_camera(const struct render_camera *camera, struct ue_bridge_camera *out)
{
	if (!camera)
		return 0;
	out->position[0] = camera->position.x;
	out->position[1] = camera->position.y;
	out->position[2] = camera->position.z;
	out->forward[0] = camera->forward.i;
	out->forward[1] = camera->forward.j;
	out->forward[2] = camera->forward.k;
	out->up[0] = camera->up.i;
	out->up[1] = camera->up.j;
	out->up[2] = camera->up.k;
	out->vertical_fov = camera->vertical_field_of_view;
	out->z_near = camera->z_near;
	out->z_far = camera->z_far;
	return 1;
}

int ue_bridge_world_hold_allowed(void)
{
	return game_connection() == _game_connection_local;
}

void ue_bridge_world_stop_time(void)
{
	main_stop_time();
}

void ue_bridge_world_start_time(void)
{
	main_start_time();
}

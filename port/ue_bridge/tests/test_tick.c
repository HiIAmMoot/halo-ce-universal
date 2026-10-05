/*
TEST_TICK.C

The tick-record writer (port/ue_bridge/ue_bridge_tick.c).
*/

#include "ueb_test.h"
#include "ue_bridge_tick.h"

#include <string.h>

static uint64_t slot_storage[4096 / 8];

static void identity_nodes(struct ue_bridge_matrix *nodes, int count)
{
	int index;

	memset(nodes, 0, sizeof(*nodes) * count);
	for (index = 0; index < count; index++)
	{
		nodes[index].scale = 1.0f;
		nodes[index].forward[0] = 1.0f;
		nodes[index].left[1] = 1.0f;
		nodes[index].up[2] = 1.0f;
		nodes[index].position[0] = (float)index;
	}
}

static void record_size_pads_permutations_to_four(void)
{
	UEB_CHECK(ue_bridge_object_record_size(0, 0) == 16);
	UEB_CHECK(ue_bridge_object_record_size(3, 2) == 16 + 4 + 2 * 52);
	UEB_CHECK(ue_bridge_object_record_size(4, 1) == 16 + 4 + 52);
	UEB_CHECK(ue_bridge_object_record_size(5, 0) == 16 + 8);
}

static void tick_writer_lays_out_records(void)
{
	struct ue_bridge_tick_writer writer;
	struct ue_bridge_matrix nodes[2];
	static const uint8_t permutations[3] = { 1, 0, 2 };
	const struct ue_bridge_tick_header *header = (const struct ue_bridge_tick_header *)slot_storage;
	const uint8_t *bytes = (const uint8_t *)slot_storage;
	const struct ue_bridge_object_record *first, *second;

	identity_nodes(nodes, 2);
	memset(slot_storage, 0, sizeof(slot_storage));
	ue_bridge_tick_writer_begin(&writer, slot_storage, sizeof(slot_storage));
	UEB_CHECK(ue_bridge_tick_writer_add(&writer, 0x00070003u, 12, UE_BRIDGE_OBJECT_AT_REST, permutations, 3, nodes, 2));
	UEB_CHECK(ue_bridge_tick_writer_add(&writer, 0x00010009u, 4, 0, 0, 0, nodes, 1));
	ue_bridge_tick_writer_end(&writer, 5, 6, 2);
	UEB_CHECK(header->object_count == 2);
	UEB_CHECK(header->load_epoch == 5 && header->state_epoch == 6 && header->active_bsp == 2);
	UEB_CHECK(header->flags == 0);
	first = (const struct ue_bridge_object_record *)(bytes + sizeof(struct ue_bridge_tick_header));
	UEB_CHECK(first->datum_index == 0x00070003u && first->definition == 12 && first->flags == UE_BRIDGE_OBJECT_AT_REST);
	UEB_CHECK(first->region_count == 3 && first->node_count == 2 && first->size == 124);
	UEB_CHECK(((const uint8_t *)(first + 1))[2] == 2);
	UEB_CHECK(((const struct ue_bridge_matrix *)((const uint8_t *)(first + 1) + 4))[1].position[0] == 1.0f);
	second = (const struct ue_bridge_object_record *)((const uint8_t *)first + first->size);
	UEB_CHECK(second->datum_index == 0x00010009u && second->size == 16 + 52);
	UEB_CHECK(header->used == sizeof(struct ue_bridge_tick_header) + 124 + 68);
}

static void tick_writer_marks_truncated_and_keeps_whole_records(void)
{
	struct ue_bridge_tick_writer writer;
	struct ue_bridge_matrix nodes[1];
	const struct ue_bridge_tick_header *header = (const struct ue_bridge_tick_header *)slot_storage;
	uint32_t capacity = sizeof(struct ue_bridge_tick_header) + 68 + 40;

	identity_nodes(nodes, 1);
	memset(slot_storage, 0, sizeof(slot_storage));
	ue_bridge_tick_writer_begin(&writer, slot_storage, capacity);
	UEB_CHECK(ue_bridge_tick_writer_add(&writer, 1, 0, 0, 0, 0, nodes, 1));
	UEB_CHECK(!ue_bridge_tick_writer_add(&writer, 2, 0, 0, 0, 0, nodes, 1));
	/* once truncated, a record that would fit is refused too: what is
	missing is always the end of the list, never a hole in it */
	UEB_CHECK(!ue_bridge_tick_writer_add(&writer, 3, 0, 0, 0, 0, 0, 0));
	ue_bridge_tick_writer_end(&writer, 1, 1, 0);
	UEB_CHECK(header->object_count == 1);
	UEB_CHECK(header->flags & UE_BRIDGE_TICK_TRUNCATED);
	UEB_CHECK(header->used == sizeof(struct ue_bridge_tick_header) + 68);
}

/* spec section 9.1: unchanged matrices set the flag, any changed byte clears it */
static void at_rest_needs_identical_bytes_and_a_continuing_record(void)
{
	struct ue_bridge_matrix previous[3], latest[3];
	int byte;

	identity_nodes(previous, 3);
	memcpy(latest, previous, sizeof(latest));
	UEB_CHECK(ue_bridge_nodes_at_rest(previous, latest, sizeof(latest), 1));
	/* a first snapshot has nothing to compare with */
	UEB_CHECK(!ue_bridge_nodes_at_rest(previous, latest, sizeof(latest), 0));
	for (byte = 0; byte < (int)sizeof(latest); byte += 17)
	{
		((uint8_t *)latest)[byte] ^= 0x01;
		UEB_CHECK(!ue_bridge_nodes_at_rest(previous, latest, sizeof(latest), 1));
		((uint8_t *)latest)[byte] ^= 0x01;
	}
	/* the last byte too: the comparison covers every node, not the first */
	((uint8_t *)latest)[sizeof(latest) - 1] ^= 0x80;
	UEB_CHECK(!ue_bridge_nodes_at_rest(previous, latest, sizeof(latest), 1));
}

const struct ueb_test ueb_tick_tests[] =
{
	{ "record_size_pads_permutations_to_four", record_size_pads_permutations_to_four },
	{ "tick_writer_lays_out_records", tick_writer_lays_out_records },
	{ "tick_writer_marks_truncated_and_keeps_whole_records", tick_writer_marks_truncated_and_keeps_whole_records },
	{ "at_rest_needs_identical_bytes_and_a_continuing_record", at_rest_needs_identical_bytes_and_a_continuing_record },
	{ 0, 0 }
};

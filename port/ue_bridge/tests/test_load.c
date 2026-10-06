/*
TEST_LOAD.C

The section layout and the load-region writer (port/ue_bridge/ue_bridge_load.c).
*/

#include "ueb_test.h"
#include "ue_bridge_load.h"

#include <string.h>

static uint64_t region[4096 / 8];

static void layout_default_section(void)
{
	struct ue_bridge_layout layout;

	UEB_CHECK(ue_bridge_layout_compute(96u << 20, UE_BRIDGE_TICK_SLOT_SIZE, &layout));
	UEB_CHECK(layout.tick_ring.offset == UE_BRIDGE_HEADER_SIZE);
	UEB_CHECK(layout.tick_ring.slot_size == UE_BRIDGE_TICK_SLOT_SIZE);
	UEB_CHECK(layout.tick_ring.slot_count == UE_BRIDGE_TICK_SLOTS);
	UEB_CHECK(layout.frame_ring.offset == UE_BRIDGE_HEADER_SIZE + UE_BRIDGE_TICK_SLOT_SIZE * UE_BRIDGE_TICK_SLOTS);
	UEB_CHECK(layout.frame_ring.slot_size == UE_BRIDGE_FRAME_SLOT_SIZE);
	UEB_CHECK(layout.load_region.offset == layout.frame_ring.offset + UE_BRIDGE_FRAME_SLOT_SIZE * UE_BRIDGE_FRAME_SLOTS);
	UEB_CHECK(layout.load_region.offset % 4096u == 0);
	UEB_CHECK(layout.load_region.offset + layout.load_region.size == (96u << 20));
}

static void layout_rejects_a_section_without_room_for_a_load_region(void)
{
	struct ue_bridge_layout layout;
	uint32_t rings = UE_BRIDGE_HEADER_SIZE + 0x10000u * UE_BRIDGE_TICK_SLOTS + UE_BRIDGE_FRAME_SLOT_SIZE * UE_BRIDGE_FRAME_SLOTS;

	UEB_CHECK(!ue_bridge_layout_compute(rings + UE_BRIDGE_MIN_LOAD_SIZE - 4096u, 0x10000u, &layout));
	UEB_CHECK(ue_bridge_layout_compute(rings + UE_BRIDGE_MIN_LOAD_SIZE, 0x10000u, &layout));
}

static void layout_rejects_a_section_over_the_maximum(void)
{
	struct ue_bridge_layout layout;

	UEB_CHECK(!ue_bridge_layout_compute(UE_BRIDGE_MAX_SECTION_SIZE + 4096u, UE_BRIDGE_TICK_SLOT_SIZE, &layout));
}

static void load_reserve_aligns_and_advances(void)
{
	struct ue_bridge_load_writer writer;
	uint32_t first, second;

	ue_bridge_load_writer_init(&writer, (volatile uint8_t *)region, sizeof(region), 0);
	first = ue_bridge_load_reserve(&writer, 3, 4);
	second = ue_bridge_load_reserve(&writer, 1, 16);
	UEB_CHECK(first == 0);
	UEB_CHECK(second == 16);
	UEB_CHECK(writer.used == 32);
	UEB_CHECK(!writer.overflow);
}

static void load_reserve_past_the_capacity_sets_overflow_and_reserves_nothing(void)
{
	struct ue_bridge_load_writer writer;

	ue_bridge_load_writer_init(&writer, (volatile uint8_t *)region, 64, 48);
	UEB_CHECK(ue_bridge_load_reserve(&writer, 1, 24) == UE_BRIDGE_NO_OFFSET);
	UEB_CHECK(writer.overflow);
	UEB_CHECK(writer.used == 48);
	/* once overflowed, nothing more is reserved even when it would fit */
	UEB_CHECK(ue_bridge_load_reserve(&writer, 1, 8) == UE_BRIDGE_NO_OFFSET);
}

static void load_reserve_rejects_a_size_that_overflows_32_bits(void)
{
	struct ue_bridge_load_writer writer;

	ue_bridge_load_writer_init(&writer, (volatile uint8_t *)region, sizeof(region), 0);
	UEB_CHECK(ue_bridge_load_reserve(&writer, 0x40000000u, 8) == UE_BRIDGE_NO_OFFSET);
	UEB_CHECK(writer.overflow);
}

static void table_valid_checks_bounds_in_64_bits(void)
{
	UEB_CHECK(ue_bridge_table_valid(0, 0, 36, 100));
	UEB_CHECK(ue_bridge_table_valid(64, 1, 36, 100));
	UEB_CHECK(!ue_bridge_table_valid(68, 1, 36, 100));
	UEB_CHECK(!ue_bridge_table_valid(8, 0x80000000u, 2, 100));
	UEB_CHECK(!ue_bridge_table_valid(UE_BRIDGE_NO_OFFSET, 1, 1, 100));
	/* an empty table may carry any offset: there is nothing to read */
	UEB_CHECK(ue_bridge_table_valid(UE_BRIDGE_NO_OFFSET, 0, 36, 100));
	/* misaligned tables are refused: the reader casts them */
	UEB_CHECK(!ue_bridge_table_valid(2, 1, 4, 100));
}

static void load_bsp_entry_indexes_the_counted_table(void)
{
	static uint64_t region[64];
	struct ue_bridge_load_writer writer;
	struct ue_bridge_load_root *root = (struct ue_bridge_load_root *)region;
	uint32_t offset;

	memset(region, 0, sizeof(region));
	/* a zeroed root, as before any export: no table, no entry */
	UEB_CHECK(ue_bridge_load_bsp_entry((volatile uint8_t *)region, 0) == 0);
	ue_bridge_load_writer_init(&writer, (volatile uint8_t *)region, sizeof(region), 0);
	ue_bridge_load_reserve(&writer, 1, sizeof(*root));
	offset = ue_bridge_load_reserve(&writer, 3, sizeof(struct ue_bridge_bsp_entry));
	root->bsps.offset = offset;
	root->bsps.count = 3;
	UEB_CHECK(ue_bridge_load_bsp_entry((volatile uint8_t *)region, 2) == (struct ue_bridge_bsp_entry *)((uint8_t *)region + offset) + 2);
	UEB_CHECK(ue_bridge_load_bsp_entry((volatile uint8_t *)region, 3) == 0);
}

/* count + 1 elements: the one spare keeps a zero count from asking for zero bytes */
static void padded_array_bytes_counts_one_spare_element(void)
{
	size_t bytes = 0;

	UEB_CHECK(ue_bridge_padded_array_bytes(3, 4, &bytes) && bytes == 16);
	UEB_CHECK(ue_bridge_padded_array_bytes(0, 12, &bytes) && bytes == 12);
}

static void padded_array_bytes_refuses_what_wraps_size_t(void)
{
	size_t bytes = 0x1234;

	/* the largest count whose padded array still fits, then one more */
	if (sizeof(size_t) == 4)
	{
		UEB_CHECK(ue_bridge_padded_array_bytes(0x3FFFFFFEu, 4, &bytes) && bytes == 0xFFFFFFFCu);
		bytes = 0x1234;
		UEB_CHECK(!ue_bridge_padded_array_bytes(0x3FFFFFFFu, 4, &bytes));
		UEB_CHECK(!ue_bridge_padded_array_bytes(0xFFFFFFFFu, 1, &bytes));
		UEB_CHECK(!ue_bridge_padded_array_bytes(0x80000000u, 2, &bytes));
	}
	/* a count that is -1 as a long: its own +1 would wrap to 0 */
	UEB_CHECK(!ue_bridge_padded_array_bytes(UINT64_MAX, 1, &bytes));
	UEB_CHECK(!ue_bridge_padded_array_bytes((uint64_t)(int64_t)-2, 4, &bytes));
	/* a failure leaves the result alone */
	UEB_CHECK(bytes == 0x1234);
}

/* a tick record's definition is 16 bits with 0xFFFF for none, a definition's model 16 bits signed with -1 for none:
the export must refuse a map with more than either holds, never wrap an index into another */
static void index_counts_fit_holds_the_sixteen_bit_fields(void)
{
	UEB_CHECK(ue_bridge_index_counts_fit(0, 0));
	UEB_CHECK(ue_bridge_index_counts_fit(UE_BRIDGE_MAX_DEFINITIONS, UE_BRIDGE_MAX_MODELS));
	UEB_CHECK(!ue_bridge_index_counts_fit(UE_BRIDGE_MAX_DEFINITIONS + 1u, 0));
	UEB_CHECK(!ue_bridge_index_counts_fit(0, UE_BRIDGE_MAX_MODELS + 1u));
	UEB_CHECK(!ue_bridge_index_counts_fit(0xFFFFFFFFu, 0));
	UEB_CHECK(!ue_bridge_index_counts_fit(0, 0xFFFFFFFFu));
}

const struct ueb_test ueb_load_tests[] =
{
	{ "layout_default_section", layout_default_section },
	{ "layout_rejects_a_section_without_room_for_a_load_region", layout_rejects_a_section_without_room_for_a_load_region },
	{ "layout_rejects_a_section_over_the_maximum", layout_rejects_a_section_over_the_maximum },
	{ "load_reserve_aligns_and_advances", load_reserve_aligns_and_advances },
	{ "load_reserve_past_the_capacity_sets_overflow_and_reserves_nothing", load_reserve_past_the_capacity_sets_overflow_and_reserves_nothing },
	{ "load_reserve_rejects_a_size_that_overflows_32_bits", load_reserve_rejects_a_size_that_overflows_32_bits },
	{ "table_valid_checks_bounds_in_64_bits", table_valid_checks_bounds_in_64_bits },
	{ "load_bsp_entry_indexes_the_counted_table", load_bsp_entry_indexes_the_counted_table },
	{ "padded_array_bytes_counts_one_spare_element", padded_array_bytes_counts_one_spare_element },
	{ "padded_array_bytes_refuses_what_wraps_size_t", padded_array_bytes_refuses_what_wraps_size_t },
	{ "index_counts_fit_holds_the_sixteen_bit_fields", index_counts_fit_holds_the_sixteen_bit_fields },
	{ 0, 0 }
};

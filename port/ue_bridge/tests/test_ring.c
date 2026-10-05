/*
TEST_RING.C

The seqlock ring: single-threaded protocol cases, and two-thread stress tests
for torn slots and torn 64-bit stores.
*/

#include "ueb_test.h"
#include "ue_bridge_ring.h"

#include <string.h>
#include <windows.h>

#define TEST_SECTION_SIZE 4096u

static uint64_t test_section_storage[TEST_SECTION_SIZE / 8];

static volatile uint8_t *test_section(void)
{
	memset(test_section_storage, 0, sizeof(test_section_storage));
	return (volatile uint8_t *)test_section_storage;
}

static void test_ring_init(volatile struct ue_bridge_ring_desc *ring, uint32_t offset, uint32_t slot_size, uint32_t slot_count)
{
	ring->offset = offset;
	ring->slot_size = slot_size;
	ring->slot_count = slot_count;
	ring->published = 0;
}

static void write_slot(volatile uint8_t *base, volatile struct ue_bridge_ring_desc *ring, uint64_t id)
{
	volatile struct ue_bridge_slot *slot = ue_bridge_ring_begin_write(base, ring);

	slot->id = id;
	slot->publish_qpc = id * 10u;
	ue_bridge_ring_end_write(ring, slot);
}

static void ring_valid_accepts_a_sane_ring(void)
{
	struct ue_bridge_ring_desc ring;

	test_ring_init(&ring, 64, 64, 8);
	UEB_CHECK(ue_bridge_ring_valid(&ring, TEST_SECTION_SIZE, sizeof(struct ue_bridge_slot)));
}

static void ring_valid_rejects_fewer_than_two_slots(void)
{
	struct ue_bridge_ring_desc ring;

	test_ring_init(&ring, 64, 64, 1);
	UEB_CHECK(!ue_bridge_ring_valid(&ring, TEST_SECTION_SIZE, sizeof(struct ue_bridge_slot)));
	test_ring_init(&ring, 64, 64, 0);
	UEB_CHECK(!ue_bridge_ring_valid(&ring, TEST_SECTION_SIZE, sizeof(struct ue_bridge_slot)));
}

static void ring_valid_rejects_small_or_unaligned_slots(void)
{
	struct ue_bridge_ring_desc ring;

	test_ring_init(&ring, 64, 16, 8);
	UEB_CHECK(!ue_bridge_ring_valid(&ring, TEST_SECTION_SIZE, sizeof(struct ue_bridge_slot)));
	test_ring_init(&ring, 64, 60, 8);
	UEB_CHECK(!ue_bridge_ring_valid(&ring, TEST_SECTION_SIZE, sizeof(struct ue_bridge_slot)));
	test_ring_init(&ring, 60, 64, 8);
	UEB_CHECK(!ue_bridge_ring_valid(&ring, TEST_SECTION_SIZE, sizeof(struct ue_bridge_slot)));
}

static void ring_valid_rejects_a_ring_past_the_section(void)
{
	struct ue_bridge_ring_desc ring;

	test_ring_init(&ring, TEST_SECTION_SIZE - 64u * 7u, 64, 8);
	UEB_CHECK(!ue_bridge_ring_valid(&ring, TEST_SECTION_SIZE, sizeof(struct ue_bridge_slot)));
	/* a descriptor whose size wraps in 32 bits */
	test_ring_init(&ring, 64, 0x80000000u, 2);
	UEB_CHECK(!ue_bridge_ring_valid(&ring, TEST_SECTION_SIZE, sizeof(struct ue_bridge_slot)));
}

static void ring_read_none_before_first_write(void)
{
	volatile uint8_t *base = test_section();
	volatile struct ue_bridge_ring_desc *ring = (volatile struct ue_bridge_ring_desc *)base;
	struct ue_bridge_slot out;
	uint32_t published = 99;

	test_ring_init(ring, 64, 64, 4);
	UEB_CHECK(ue_bridge_ring_read_newest(base, ring, &out, sizeof(out), &published) == UE_BRIDGE_READ_NONE);
	UEB_CHECK(published == 0);
}

static void ring_write_then_read_newest(void)
{
	volatile uint8_t *base = test_section();
	volatile struct ue_bridge_ring_desc *ring = (volatile struct ue_bridge_ring_desc *)base;
	struct ue_bridge_slot out;
	uint32_t published = 0;
	uint64_t id;

	test_ring_init(ring, 64, 64, 4);
	for (id = 1; id <= 10; id++)
		write_slot(base, ring, id);
	UEB_CHECK(ue_bridge_ring_read_newest(base, ring, &out, sizeof(out), &published) == UE_BRIDGE_READ_NEWEST);
	UEB_CHECK(published == 10);
	UEB_CHECK(out.id == 10);
	UEB_CHECK(out.publish_qpc == 100);
	UEB_CHECK((out.sequence & 1u) == 0);
}

static void ring_read_takes_previous_while_newest_is_odd(void)
{
	volatile uint8_t *base = test_section();
	volatile struct ue_bridge_ring_desc *ring = (volatile struct ue_bridge_ring_desc *)base;
	volatile struct ue_bridge_slot *newest;
	struct ue_bridge_slot out;

	test_ring_init(ring, 64, 64, 4);
	write_slot(base, ring, 1);
	write_slot(base, ring, 2);
	/* a reader that found the newest slot mid-write (as after a wrap) */
	newest = (volatile struct ue_bridge_slot *)(base + 64 + 64);
	newest->sequence |= 1u;
	UEB_CHECK(ue_bridge_ring_read_newest(base, ring, &out, sizeof(out), 0) == UE_BRIDGE_READ_PREVIOUS);
	UEB_CHECK(out.id == 1);
}

static void ring_read_reports_torn_when_both_slots_are_odd(void)
{
	volatile uint8_t *base = test_section();
	volatile struct ue_bridge_ring_desc *ring = (volatile struct ue_bridge_ring_desc *)base;
	struct ue_bridge_slot out;

	test_ring_init(ring, 64, 64, 4);
	write_slot(base, ring, 1);
	write_slot(base, ring, 2);
	((volatile struct ue_bridge_slot *)(base + 64))->sequence |= 1u;
	((volatile struct ue_bridge_slot *)(base + 128))->sequence |= 1u;
	UEB_CHECK(ue_bridge_ring_read_newest(base, ring, &out, sizeof(out), 0) == UE_BRIDGE_READ_TORN);
}

struct rewrite_context
{
	volatile uint8_t *base;
	volatile struct ue_bridge_ring_desc *ring;
};

static void rewrite_the_slot_being_read(void *context)
{
	struct rewrite_context *rewrite = (struct rewrite_context *)context;
	uint32_t index;

	/* four writes bring the writer back round to the slot the reader holds */
	for (index = 0; index < 4; index++)
		write_slot(rewrite->base, rewrite->ring, 100 + index);
}

static void slot_try_read_detects_change_during_copy(void)
{
	volatile uint8_t *base = test_section();
	volatile struct ue_bridge_ring_desc *ring = (volatile struct ue_bridge_ring_desc *)base;
	struct rewrite_context context;
	struct ue_bridge_slot out;

	test_ring_init(ring, 64, 64, 4);
	write_slot(base, ring, 1);
	context.base = base;
	context.ring = ring;
	UEB_CHECK(!ue_bridge_slot_try_read((const volatile struct ue_bridge_slot *)(base + 64), 64, &out, sizeof(out),
		rewrite_the_slot_being_read, &context));
	UEB_CHECK(ue_bridge_slot_try_read((const volatile struct ue_bridge_slot *)(base + 64), 64, &out, sizeof(out), 0, 0));
}

/* ---------- two-thread stress tests */

#define STRESS_SLOT_SIZE 256u
#define STRESS_WORDS ((STRESS_SLOT_SIZE - sizeof(struct ue_bridge_slot)) / 8u)
#define STRESS_MILLISECONDS 500u

struct stress_slot
{
	struct ue_bridge_slot slot;
	uint64_t words[STRESS_WORDS];
};

static volatile LONG stress_stop;

static DWORD WINAPI stress_writer(void *parameter)
{
	volatile uint8_t *base = (volatile uint8_t *)parameter;
	volatile struct ue_bridge_ring_desc *ring = (volatile struct ue_bridge_ring_desc *)base;
	uint64_t id = 1;

	while (!stress_stop)
	{
		volatile struct stress_slot *slot = (volatile struct stress_slot *)ue_bridge_ring_begin_write(base, ring);
		uint32_t word;

		slot->slot.id = id;
		for (word = 0; word < STRESS_WORDS; word++)
			slot->words[word] = id;
		ue_bridge_ring_end_write(ring, &slot->slot);
		id++;
	}
	return 0;
}

static void ring_concurrent_reader_never_sees_mixed_payload(void)
{
	static uint64_t storage[(64 + STRESS_SLOT_SIZE * 4) / 8];
	volatile uint8_t *base = (volatile uint8_t *)storage;
	volatile struct ue_bridge_ring_desc *ring = (volatile struct ue_bridge_ring_desc *)base;
	HANDLE writer;
	DWORD start;
	uint32_t reads = 0;
	int mixed = 0;

	memset(storage, 0, sizeof(storage));
	test_ring_init(ring, 64, STRESS_SLOT_SIZE, 4);
	stress_stop = 0;
	writer = CreateThread(0, 0, stress_writer, (void *)base, 0, 0);
	UEB_CHECK(writer != 0);
	start = GetTickCount();
	while (GetTickCount() - start < STRESS_MILLISECONDS)
	{
		struct stress_slot out;
		enum ue_bridge_read_result result = ue_bridge_ring_read_newest(base, ring, &out, sizeof(out), 0);

		if (result == UE_BRIDGE_READ_NEWEST || result == UE_BRIDGE_READ_PREVIOUS)
		{
			uint32_t word;

			reads++;
			for (word = 0; word < STRESS_WORDS; word++)
			{
				if (out.words[word] != out.slot.id)
					mixed = 1;
			}
		}
	}
	InterlockedExchange(&stress_stop, 1);
	WaitForSingleObject(writer, INFINITE);
	CloseHandle(writer);
	UEB_CHECK(reads > 0);
	UEB_CHECK(!mixed);
}

static volatile uint64_t stress_heartbeat;

static DWORD WINAPI heartbeat_writer(void *parameter)
{
	uint64_t flip = 0;

	(void)parameter;
	while (!stress_stop)
	{
		/* the two values differ in both halves, so a torn load matches neither */
		ueb_store_u64(&stress_heartbeat, flip ? 0xFFFFFFFF00000000ull : 0x00000000FFFFFFFFull);
		flip ^= 1;
	}
	return 0;
}

static void ring_heartbeat_store_is_never_torn(void)
{
	HANDLE writer;
	DWORD start;
	int torn = 0;

	ueb_store_u64(&stress_heartbeat, 0x00000000FFFFFFFFull);
	stress_stop = 0;
	writer = CreateThread(0, 0, heartbeat_writer, 0, 0, 0);
	UEB_CHECK(writer != 0);
	start = GetTickCount();
	while (GetTickCount() - start < STRESS_MILLISECONDS)
	{
		uint64_t value = ueb_load_u64(&stress_heartbeat);

		if (value != 0xFFFFFFFF00000000ull && value != 0x00000000FFFFFFFFull)
			torn = 1;
	}
	InterlockedExchange(&stress_stop, 1);
	WaitForSingleObject(writer, INFINITE);
	CloseHandle(writer);
	UEB_CHECK(!torn);
}

/* a slot whose used size sits at offset 24, as the tick header's does */
static void slot_read_used_copies_only_the_used_bytes(void)
{
	static uint64_t storage[256];
	volatile struct ue_bridge_slot *slot = (volatile struct ue_bridge_slot *)storage;
	uint8_t out[2048];
	uint32_t bytes = 0;

	memset(storage, 0xAB, sizeof(storage));
	slot->sequence = 2;
	*(volatile uint32_t *)((volatile uint8_t *)slot + 24) = 100;
	memset(out, 0, sizeof(out));
	UEB_CHECK(ue_bridge_slot_try_read_used(slot, sizeof(storage), 24, out, sizeof(out), &bytes, 0, 0));
	UEB_CHECK(bytes == 100);
	UEB_CHECK(out[99] == 0xAB);
	UEB_CHECK(out[100] == 0);
}

static void slot_read_used_clamps_a_used_size_past_the_slot(void)
{
	static uint64_t storage[16];
	volatile struct ue_bridge_slot *slot = (volatile struct ue_bridge_slot *)storage;
	uint8_t out[1024];
	uint32_t bytes = 0;

	slot->sequence = 2;
	*(volatile uint32_t *)((volatile uint8_t *)slot + 24) = 0x7FFFFFFFu;
	UEB_CHECK(!ue_bridge_slot_try_read_used(slot, sizeof(storage), 24, out, sizeof(out), &bytes, 0, 0));
	/* past the 128-byte slot but inside the output: the slot bound alone must refuse it */
	*(volatile uint32_t *)((volatile uint8_t *)slot + 24) = 200;
	UEB_CHECK(!ue_bridge_slot_try_read_used(slot, sizeof(storage), 24, out, sizeof(out), &bytes, 0, 0));
	/* a used size below the field that holds it is no slot */
	*(volatile uint32_t *)((volatile uint8_t *)slot + 24) = 8;
	UEB_CHECK(!ue_bridge_slot_try_read_used(slot, sizeof(storage), 24, out, sizeof(out), &bytes, 0, 0));
}

static void bump_sequence(void *context)
{
	volatile struct ue_bridge_slot *slot = (volatile struct ue_bridge_slot *)context;

	slot->sequence += 2;
}

static void slot_read_used_detects_change_during_copy(void)
{
	static uint64_t storage[64];
	volatile struct ue_bridge_slot *slot = (volatile struct ue_bridge_slot *)storage;
	uint8_t out[512];
	uint32_t bytes = 0;

	slot->sequence = 2;
	*(volatile uint32_t *)((volatile uint8_t *)slot + 24) = 64;
	UEB_CHECK(!ue_bridge_slot_try_read_used(slot, sizeof(storage), 24, out, sizeof(out), &bytes, bump_sequence, (void *)slot));
}

static void slot_read_used_refuses_an_output_too_small(void)
{
	static uint64_t storage[64];
	volatile struct ue_bridge_slot *slot = (volatile struct ue_bridge_slot *)storage;
	uint8_t out[32];
	uint32_t bytes = 0;

	slot->sequence = 2;
	*(volatile uint32_t *)((volatile uint8_t *)slot + 24) = 64;
	UEB_CHECK(!ue_bridge_slot_try_read_used(slot, sizeof(storage), 24, out, sizeof(out), &bytes, 0, 0));
}

const struct ueb_test ueb_ring_tests[] =
{
	{ "ring_valid_accepts_a_sane_ring", ring_valid_accepts_a_sane_ring },
	{ "ring_valid_rejects_fewer_than_two_slots", ring_valid_rejects_fewer_than_two_slots },
	{ "ring_valid_rejects_small_or_unaligned_slots", ring_valid_rejects_small_or_unaligned_slots },
	{ "ring_valid_rejects_a_ring_past_the_section", ring_valid_rejects_a_ring_past_the_section },
	{ "ring_read_none_before_first_write", ring_read_none_before_first_write },
	{ "ring_write_then_read_newest", ring_write_then_read_newest },
	{ "ring_read_takes_previous_while_newest_is_odd", ring_read_takes_previous_while_newest_is_odd },
	{ "ring_read_reports_torn_when_both_slots_are_odd", ring_read_reports_torn_when_both_slots_are_odd },
	{ "slot_try_read_detects_change_during_copy", slot_try_read_detects_change_during_copy },
	{ "ring_concurrent_reader_never_sees_mixed_payload", ring_concurrent_reader_never_sees_mixed_payload },
	{ "ring_heartbeat_store_is_never_torn", ring_heartbeat_store_is_never_torn },
	{ "slot_read_used_copies_only_the_used_bytes", slot_read_used_copies_only_the_used_bytes },
	{ "slot_read_used_clamps_a_used_size_past_the_slot", slot_read_used_clamps_a_used_size_past_the_slot },
	{ "slot_read_used_detects_change_during_copy", slot_read_used_detects_change_during_copy },
	{ "slot_read_used_refuses_an_output_too_small", slot_read_used_refuses_an_output_too_small },
	{ 0, 0 }
};

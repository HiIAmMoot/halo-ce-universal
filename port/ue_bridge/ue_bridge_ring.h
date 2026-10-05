/*
UE_BRIDGE_RING.H

Lock-free rings of fixed-size slots in the bridge section (ue_bridge_format.h).
The game is the only writer; UE reads. A slot's sequence is odd while it is
written; a reader copies a slot only between two equal, even sequences, so it
never keeps half of one write and half of another. This is also what protects
the slots' 64-bit fields, which the 32-bit game stores in two halves.

Compiled as C by clang (the game, its tests) and as C++ by MSVC (the plugin).
*/

#ifndef UE_BRIDGE_RING_H
#define UE_BRIDGE_RING_H

#include "ue_bridge_format.h"

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_MSC_VER) && !defined(__clang__)

#ifndef _WIN64
#error "the MSVC side of the bridge is 64-bit only: its 64-bit loads and stores rely on it"
#endif
#include <intrin.h>

/* x64: aligned loads and stores of up to 8 bytes are single accesses, and the
hardware keeps stores in order and loads in order; only the compiler needs
fencing */
static __inline uint32_t ueb_load_u32(const volatile uint32_t *address)
{
	uint32_t value = *address;
	_ReadWriteBarrier();
	return value;
}

static __inline void ueb_store_u32(volatile uint32_t *address, uint32_t value)
{
	_ReadWriteBarrier();
	*address = value;
	_ReadWriteBarrier();
}

static __inline uint64_t ueb_load_u64(const volatile uint64_t *address)
{
	uint64_t value = *address;
	_ReadWriteBarrier();
	return value;
}

static __inline void ueb_store_u64(volatile uint64_t *address, uint64_t value)
{
	_ReadWriteBarrier();
	*address = value;
	_ReadWriteBarrier();
}

static __inline void ueb_fence(void)
{
	_mm_mfence();
}

#else

/* clang and gcc, including the 32-bit game: __atomic on a 64-bit value is one
access (cmpxchg8b or an SSE move), never two 32-bit halves */
static inline uint32_t ueb_load_u32(const volatile uint32_t *address)
{
	return __atomic_load_n(address, __ATOMIC_ACQUIRE);
}

static inline void ueb_store_u32(volatile uint32_t *address, uint32_t value)
{
	__atomic_store_n(address, value, __ATOMIC_RELEASE);
}

static inline uint64_t ueb_load_u64(const volatile uint64_t *address)
{
	return __atomic_load_n(address, __ATOMIC_ACQUIRE);
}

static inline void ueb_store_u64(volatile uint64_t *address, uint64_t value)
{
	__atomic_store_n(address, value, __ATOMIC_SEQ_CST);
}

static inline void ueb_fence(void)
{
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
}

#endif

enum ue_bridge_read_result
{
	UE_BRIDGE_READ_NONE = 0,
	UE_BRIDGE_READ_NEWEST = 1,
	UE_BRIDGE_READ_PREVIOUS = 2,
	UE_BRIDGE_READ_TORN = 3
};

typedef void (*ue_bridge_read_hook)(void *context);

/* 1 when the ring has at least two slots of at least min_slot_size bytes,
8-byte aligned, all inside a section of section_size bytes */
int ue_bridge_ring_valid(const volatile struct ue_bridge_ring_desc *ring, uint32_t section_size, uint32_t min_slot_size);

/* the slot the next write goes to, marked as being written */
volatile struct ue_bridge_slot *ue_bridge_ring_begin_write(volatile uint8_t *base, volatile struct ue_bridge_ring_desc *ring);

/* marks the slot complete and publishes it */
void ue_bridge_ring_end_write(volatile struct ue_bridge_ring_desc *ring, volatile struct ue_bridge_slot *slot);

/* copies min(out_size, slot_size) bytes of the slot into out; 1 when the copy
is one complete write. between, when set, runs between the copy and the
re-check (tests use it to change the slot mid-read). out_size must not exceed
slot_size, or the tail of out is left unwritten. slot must lie inside the
reader's own mapping. */
int ue_bridge_slot_try_read(const volatile struct ue_bridge_slot *slot, uint32_t slot_size, void *out, uint32_t out_size,
	ue_bridge_read_hook between, void *context);

/* as ue_bridge_slot_try_read, but copies only the slot's used bytes: the
uint32_t at used_offset, read inside the same sequence check. 0 when that
size is below used_offset + 4, past slot_size or past out_capacity. *bytes
receives the bytes copied. */
int ue_bridge_slot_try_read_used(const volatile struct ue_bridge_slot *slot, uint32_t slot_size, uint32_t used_offset,
	void *out, uint32_t out_capacity, uint32_t *bytes, ue_bridge_read_hook between, void *context);

/* the newest complete slot, or the one before it when the newest is being
rewritten; *published (when set) receives the ring's write count.
The descriptor must have passed ue_bridge_ring_valid against the size of the
reader's own mapping, never a size read from the shared header. A reader that
does not trust the writer passes its own copy of the validated geometry and
reads only published live. out_size must not exceed slot_size, or the tail of
out is left unwritten. */
enum ue_bridge_read_result ue_bridge_ring_read_newest(const volatile uint8_t *base, const volatile struct ue_bridge_ring_desc *ring,
	void *out, uint32_t out_size, uint32_t *published);

/* ue_bridge_ring_read_newest, by used size */
enum ue_bridge_read_result ue_bridge_ring_read_newest_used(const volatile uint8_t *base, const volatile struct ue_bridge_ring_desc *ring,
	uint32_t used_offset, void *out, uint32_t out_capacity, uint32_t *bytes, uint32_t *published);

#ifdef __cplusplus
}
#endif

#endif

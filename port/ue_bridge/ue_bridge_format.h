/*
UE_BRIDGE_FORMAT.H

The shared memory between the game (32-bit, clang) and the HaloCEUE renderer
(64-bit, MSVC). See the HaloCEUE repository's Phase 0 design,
docs/superpowers/specs/2026-10-02-phase0-bridge-design.md, sections 4 and 8.1.

Both compilers check every offset below at compile time, so a change that
lays a field out differently on either side fails to build. Any change to
this file bumps UE_BRIDGE_VERSION.

The game creates two named sections:
- the directory (UE_BRIDGE_DIRECTORY_NAME, fixed): which game is current, and
  the name of its bridge section;
- the bridge section ("Local\HaloCEUE.Bridge.<pid>.<session id>"): the header,
  then the rings.
*/

#ifndef UE_BRIDGE_FORMAT_H
#define UE_BRIDGE_FORMAT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
#define UEB_STATIC_ASSERT(condition, message) static_assert(condition, message)
#else
#define UEB_STATIC_ASSERT(condition, message) _Static_assert(condition, message)
#endif

#define UE_BRIDGE_MAGIC 0x45554248u
#define UE_BRIDGE_VERSION 1u

#define UE_BRIDGE_DIRECTORY_NAME "Local\\HaloCEUE.Bridge.Directory"
#define UE_BRIDGE_DIRECTORY_SIZE 0x1000u
#define UE_BRIDGE_NAME_CHARS 96u
/* UTF-8 paths: MAX_PATH UTF-16 units at up to two bytes each in this range */
#define UE_BRIDGE_PATH_BYTES 520u

#define UE_BRIDGE_SECTION_SIZE 0x10000u
#define UE_BRIDGE_HEADER_SIZE 0x1000u
#define UE_BRIDGE_SLOT_SIZE 64u
#define UE_BRIDGE_TICK_SLOTS 8u
#define UE_BRIDGE_FRAME_SLOTS 8u

#define UE_BRIDGE_DEFAULT_HANG_TIMEOUT_MS 10000u
#define UE_BRIDGE_EDITOR_HANG_TIMEOUT_MS 60000u
#define UE_BRIDGE_DUMP_WAIT_MS 10000u

/* game_stopping, ue_stopping */
enum
{
	UE_BRIDGE_STOP_NONE = 0,
	UE_BRIDGE_STOP_EXIT = 1,
	UE_BRIDGE_STOP_CRASH = 2
};

struct ue_bridge_directory
{
	uint32_t magic;
	uint32_t version;
	/* odd while the game rewrites the entry */
	uint32_t sequence;
	uint32_t game_pid;
	uint64_t session_id;
	/* NUL-terminated ASCII */
	char section_name[UE_BRIDGE_NAME_CHARS];
};

struct ue_bridge_ring_desc
{
	/* from the start of the bridge section */
	uint32_t offset;
	uint32_t slot_size;
	uint32_t slot_count;
	/* complete writes so far; the newest complete slot is (published - 1) % slot_count */
	uint32_t published;
};

struct ue_bridge_crash_record
{
	uint32_t exception_code;
	uint32_t exception_address;
	uint32_t thread_id;
	uint32_t reserved;
};

struct ue_bridge_header
{
	/* written once by the game, before it publishes the directory entry */
	uint32_t magic;
	uint32_t version;
	uint32_t header_size;
	uint32_t section_size;
	uint64_t session_id;
	uint64_t qpc_frequency;
	uint32_t game_pid;
	uint32_t max_objects;
	uint32_t game_hang_timeout_ms;
	uint32_t reserved0;
	struct ue_bridge_ring_desc tick_ring;
	struct ue_bridge_ring_desc frame_ring;
	/* UTF-8, NUL-terminated: the game's debug.txt */
	char game_log_path[UE_BRIDGE_PATH_BYTES];

	/* written by the game while it runs */
	uint64_t game_heartbeat_qpc;
	uint32_t load_epoch;
	uint32_t state_epoch;
	uint32_t game_debugger_attached;
	uint32_t game_busy;
	uint32_t game_stopping;
	/* 1 while the game's crash hook waits for ue_dump_done */
	uint32_t game_crashing;
	struct ue_bridge_crash_record crash;

	/* written by UE */
	uint64_t ue_heartbeat_qpc;
	uint32_t ue_pid;
	uint32_t ue_attached;
	uint32_t ue_ready;
	uint32_t ue_debugger_attached;
	uint32_t ue_busy;
	uint32_t ue_hang_timeout_ms;
	uint32_t ue_is_editor;
	uint32_t ue_dump_done;
	uint32_t ue_stopping;
	uint32_t reserved1;
	/* UTF-8, NUL-terminated: this session's folder */
	char ue_session_dir[UE_BRIDGE_PATH_BYTES];
};

struct ue_bridge_slot
{
	/* odd while the game writes the slot */
	uint32_t sequence;
	uint32_t reserved;
	/* the tick or frame number */
	uint64_t id;
	uint64_t publish_qpc;
};

struct ue_bridge_frame_slot
{
	struct ue_bridge_slot slot;
	float interpolation_fraction;
	uint32_t reserved;
};

UEB_STATIC_ASSERT(sizeof(struct ue_bridge_directory) == 120, "directory size");
UEB_STATIC_ASSERT(offsetof(struct ue_bridge_directory, sequence) == 8, "directory sequence");
UEB_STATIC_ASSERT(offsetof(struct ue_bridge_directory, game_pid) == 12, "directory game_pid");
UEB_STATIC_ASSERT(offsetof(struct ue_bridge_directory, session_id) == 16, "directory session_id");
UEB_STATIC_ASSERT(offsetof(struct ue_bridge_directory, section_name) == 24, "directory section_name");

UEB_STATIC_ASSERT(sizeof(struct ue_bridge_ring_desc) == 16, "ring descriptor size");
UEB_STATIC_ASSERT(sizeof(struct ue_bridge_crash_record) == 16, "crash record size");

UEB_STATIC_ASSERT(sizeof(struct ue_bridge_header) == 1216, "header size");
UEB_STATIC_ASSERT(offsetof(struct ue_bridge_header, session_id) == 16, "header session_id");
UEB_STATIC_ASSERT(offsetof(struct ue_bridge_header, qpc_frequency) == 24, "header qpc_frequency");
UEB_STATIC_ASSERT(offsetof(struct ue_bridge_header, game_pid) == 32, "header game_pid");
UEB_STATIC_ASSERT(offsetof(struct ue_bridge_header, game_hang_timeout_ms) == 40, "header game_hang_timeout_ms");
UEB_STATIC_ASSERT(offsetof(struct ue_bridge_header, tick_ring) == 48, "header tick_ring");
UEB_STATIC_ASSERT(offsetof(struct ue_bridge_header, frame_ring) == 64, "header frame_ring");
UEB_STATIC_ASSERT(offsetof(struct ue_bridge_header, game_log_path) == 80, "header game_log_path");
UEB_STATIC_ASSERT(offsetof(struct ue_bridge_header, game_heartbeat_qpc) == 600, "header game_heartbeat_qpc");
UEB_STATIC_ASSERT(offsetof(struct ue_bridge_header, load_epoch) == 608, "header load_epoch");
UEB_STATIC_ASSERT(offsetof(struct ue_bridge_header, state_epoch) == 612, "header state_epoch");
UEB_STATIC_ASSERT(offsetof(struct ue_bridge_header, game_debugger_attached) == 616, "header game_debugger_attached");
UEB_STATIC_ASSERT(offsetof(struct ue_bridge_header, game_busy) == 620, "header game_busy");
UEB_STATIC_ASSERT(offsetof(struct ue_bridge_header, game_stopping) == 624, "header game_stopping");
UEB_STATIC_ASSERT(offsetof(struct ue_bridge_header, game_crashing) == 628, "header game_crashing");
UEB_STATIC_ASSERT(offsetof(struct ue_bridge_header, crash) == 632, "header crash");
UEB_STATIC_ASSERT(offsetof(struct ue_bridge_header, ue_heartbeat_qpc) == 648, "header ue_heartbeat_qpc");
UEB_STATIC_ASSERT(offsetof(struct ue_bridge_header, ue_pid) == 656, "header ue_pid");
UEB_STATIC_ASSERT(offsetof(struct ue_bridge_header, ue_attached) == 660, "header ue_attached");
UEB_STATIC_ASSERT(offsetof(struct ue_bridge_header, ue_ready) == 664, "header ue_ready");
UEB_STATIC_ASSERT(offsetof(struct ue_bridge_header, ue_debugger_attached) == 668, "header ue_debugger_attached");
UEB_STATIC_ASSERT(offsetof(struct ue_bridge_header, ue_busy) == 672, "header ue_busy");
UEB_STATIC_ASSERT(offsetof(struct ue_bridge_header, ue_hang_timeout_ms) == 676, "header ue_hang_timeout_ms");
UEB_STATIC_ASSERT(offsetof(struct ue_bridge_header, ue_is_editor) == 680, "header ue_is_editor");
UEB_STATIC_ASSERT(offsetof(struct ue_bridge_header, ue_dump_done) == 684, "header ue_dump_done");
UEB_STATIC_ASSERT(offsetof(struct ue_bridge_header, ue_stopping) == 688, "header ue_stopping");
UEB_STATIC_ASSERT(offsetof(struct ue_bridge_header, ue_session_dir) == 696, "header ue_session_dir");
/* the 64-bit fields read without the seqlock must be naturally aligned to be read in one access */
UEB_STATIC_ASSERT(offsetof(struct ue_bridge_header, game_heartbeat_qpc) % 8 == 0, "game heartbeat alignment");
UEB_STATIC_ASSERT(offsetof(struct ue_bridge_header, ue_heartbeat_qpc) % 8 == 0, "UE heartbeat alignment");
UEB_STATIC_ASSERT(sizeof(struct ue_bridge_header) <= UE_BRIDGE_HEADER_SIZE, "header fits its region");

UEB_STATIC_ASSERT(sizeof(struct ue_bridge_slot) == 24, "slot size");
UEB_STATIC_ASSERT(offsetof(struct ue_bridge_slot, id) == 8, "slot id");
UEB_STATIC_ASSERT(offsetof(struct ue_bridge_slot, publish_qpc) == 16, "slot publish_qpc");
UEB_STATIC_ASSERT(sizeof(struct ue_bridge_frame_slot) == 32, "frame slot size");
UEB_STATIC_ASSERT(sizeof(struct ue_bridge_frame_slot) <= UE_BRIDGE_SLOT_SIZE, "frame slot fits");
UEB_STATIC_ASSERT(UE_BRIDGE_HEADER_SIZE + UE_BRIDGE_SLOT_SIZE * (UE_BRIDGE_TICK_SLOTS + UE_BRIDGE_FRAME_SLOTS) <= UE_BRIDGE_SECTION_SIZE,
	"rings fit the section");

#endif

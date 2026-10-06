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
  then the rings and the load region.
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
#define UE_BRIDGE_VERSION 4u

#define UE_BRIDGE_DIRECTORY_NAME "Local\\HaloCEUE.Bridge.Directory"
#define UE_BRIDGE_DIRECTORY_SIZE 0x1000u
#define UE_BRIDGE_NAME_CHARS 96u
/* UTF-8 paths: MAX_PATH UTF-16 units at up to two bytes each below U+0800.
Characters from U+0800 up (CJK among them) take three, so a path of those
near MAX_PATH doesn't fit, and is then published empty (and logged) */
#define UE_BRIDGE_PATH_BYTES 520u

#define UE_BRIDGE_HEADER_SIZE 0x1000u

/* The section's size comes from the game's settings (ue_bridge.section_mb);
the layout is ue_bridge_layout_compute's (ue_bridge_load.h). Version 3 adds
the load region and the tick and frame payloads (Phase 0 design, sections
4.4 to 4.6, milestone M2). Version 4 adds the header's game_truncated_ticks. */
#define UE_BRIDGE_MIN_SECTION_MB 16u
#define UE_BRIDGE_DEFAULT_SECTION_MB 96u
#define UE_BRIDGE_MAX_SECTION_SIZE 0x40000000u
#define UE_BRIDGE_TICK_SLOT_SIZE 0x200000u
#define UE_BRIDGE_TICK_SLOTS 4u
#define UE_BRIDGE_FRAME_SLOT_SIZE 0x1000u
#define UE_BRIDGE_FRAME_SLOTS 8u
#define UE_BRIDGE_MIN_LOAD_SIZE 0x100000u
#define UE_BRIDGE_NO_OFFSET 0xFFFFFFFFu
/* Halo's model detail levels (NUMBER_OF_DETAIL_LEVELS_PER_MODEL, asserted
equal in port/linux/game/ue_bridge_export.c); every array of levels, in the
format, the shared C and the plugin, is sized by this one constant */
#define UE_BRIDGE_DETAIL_LEVELS 5u
/* a datum's absolute index is its low 16 bits (Halo's datum design): the
bound of every table indexed by one, on both sides */
#define UE_BRIDGE_DATUM_ABSOLUTE_LIMIT 65536u

/* index widths, one per geometry kind. A model part's are Halo's own: its
triangle strips are 16-bit, so a part has at most 65536 vertices. A BSP
batch merges many surfaces, so its indices are 32-bit. */
typedef uint16_t ue_bridge_model_index;
typedef uint32_t ue_bridge_bsp_index;
/* a batch's cluster; -1 is "no cluster", so a cluster index the type can't
hold must never be stored (it would wrap into -1 or another cluster) */
typedef int16_t ue_bridge_cluster_index;
/* clusters the type holds: indices 0 through its maximum */
#define UE_BRIDGE_MAX_CLUSTERS ((uint32_t)INT16_MAX + 1u)
/* a definition's model is an int16 with -1 for none: models the type holds */
#define UE_BRIDGE_MAX_MODELS ((uint32_t)INT16_MAX + 1u)
/* a tick record's definition is a uint16 with 0xFFFF (ue_bridge_world.h's
UE_BRIDGE_NO_DEFINITION) for none: definitions the field holds */
#define UE_BRIDGE_MAX_DEFINITIONS 0xFFFFu

struct ue_bridge_region_desc
{
	/* from the start of the bridge section */
	uint32_t offset;
	uint32_t size;
};

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

	/* written by the game while it runs. Here at the end, among UE's fields,
	because no reserved space was left in the game's: moving a field would have
	shifted every offset after it. UE caps itself to the game's frame rate
	(spec section 6.5). */
	/* the display's refresh rate, rounded; 0: the display reports none */
	uint32_t game_refresh_hz;
	/* the frames a second the game's limiter aims for; 0: uncapped */
	uint32_t game_frame_target_hz;

	/* version 3. load_region is written once by the game, before it publishes
	the directory entry; the rest while it runs. A map export rewrites the
	region from its start under an odd load_sequence; a BSP exported on its
	first switch is appended, and its bsps table entry turns ready only after its
	data, so appending needs no sequence change. */
	struct ue_bridge_region_desc load_region;
	uint32_t load_sequence;
	/* the load_epoch whose map the region describes; 0: none yet */
	uint32_t export_epoch;
	/* 1: the whole map fit; 0: the root's missing bits name what didn't */
	uint32_t export_complete;
	/* 1 while the game holds its map start for UE (the load handshake) */
	uint32_t game_holding;

	/* version 4, written by the game while it runs */
	/* ticks published with UE_BRIDGE_TICK_TRUNCATED since the game started:
	the objects past the cut were not written, so UE shows them frozen */
	uint32_t game_truncated_ticks;
	uint32_t reserved2;
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

/* real_matrix4x3: a uniform scale, the basis (x forward, y left, z up) and
the position, in world units */
struct ue_bridge_matrix
{
	float scale;
	float forward[3];
	float left[3];
	float up[3];
	float position[3];
};

/* a tick slot: this header, then object_count records in ascending absolute
index, each a struct ue_bridge_object_record, then region_count permutation
bytes padded to 4, then node_count struct ue_bridge_matrix */
struct ue_bridge_tick_header
{
	struct ue_bridge_slot slot;
	/* bytes in use from the slot's start, records included */
	uint32_t used;
	uint32_t object_count;
	/* the epochs the game was in when it wrote the slot: a slot from before a
	map change must never reach the next map's caches */
	uint32_t load_epoch;
	uint32_t state_epoch;
	/* the scenario's BSP index, -1 for none */
	int16_t active_bsp;
	uint16_t flags;
	uint32_t reserved;
};

/* the objects didn't all fit: what is missing is not gone */
#define UE_BRIDGE_TICK_TRUNCATED 0x0001u

struct ue_bridge_object_record
{
	/* absolute index in the low 16 bits, salt in the high 16 */
	uint32_t datum_index;
	/* into the load region's definition table */
	uint16_t definition;
	uint16_t flags;
	uint16_t node_count;
	uint8_t region_count;
	uint8_t reserved;
	/* this record's bytes, permutations and nodes included; a multiple of 4 */
	uint32_t size;
};

#define UE_BRIDGE_OBJECT_HIDDEN 0x0001u
/* the node matrices are byte-identical to the previous tick's */
#define UE_BRIDGE_OBJECT_AT_REST 0x0002u

struct ue_bridge_frame_slot
{
	struct ue_bridge_slot slot;
	float interpolation_fraction;
	/* 0 when no window camera was drawn this frame (a menu, a load) */
	uint32_t camera_valid;
	/* window 0's render camera as the classic renderer used it (camera
	effects such as shake included), in world units */
	float camera_position[3];
	float camera_forward[3];
	float camera_up[3];
	/* radians */
	float vertical_fov;
	float z_near;
	float z_far;
	/* the game's tick count (render_interpolation.c's) when the frame was drawn */
	uint64_t tick_id;
};

#define UE_BRIDGE_LOAD_MAGIC 0x44414F4Cu

/* missing bits in the root: what the region had no room for */
#define UE_BRIDGE_MISSING_DEFINITIONS 0x0001u
#define UE_BRIDGE_MISSING_MODELS 0x0002u
#define UE_BRIDGE_MISSING_BSPS 0x0004u

/* offset from the load region's start, and an element count */
struct ue_bridge_table
{
	uint32_t offset;
	uint32_t count;
};

struct ue_bridge_bsp_entry
{
	int32_t tag_index;
	/* set last, once the batches are written; never cleared within a load epoch */
	uint32_t ready;
	/* struct ue_bridge_bsp_batch */
	struct ue_bridge_table batches;
	uint32_t cluster_count;
	/* surfaces no cluster lists, exported in cluster -1 batches */
	uint32_t unclustered_surfaces;
	/* surfaces more than one cluster lists, kept in the first */
	uint32_t duplicate_surfaces;
	uint32_t reserved;
};

/* at the load region's offset 0 */
struct ue_bridge_load_root
{
	uint32_t magic;
	uint32_t load_epoch;
	char map_name[64];
	/* struct ue_bridge_definition */
	struct ue_bridge_table definitions;
	/* struct ue_bridge_model */
	struct ue_bridge_table models;
	/* struct ue_bridge_bsp_entry, one per scenario BSP; reserved right after
	the root, so it is in even when nothing else fits */
	struct ue_bridge_table bsps;
	uint32_t missing;
	/* The game's own limits (MAXIMUM_NODES_PER_MODEL, MAXIMUM_REGIONS_PER_MODEL,
	MAXIMUM_PERMUTATIONS_PER_MODEL_REGION, MAXIMUM_REGIONS_PER_OBJECT). UE
	validates against these and keeps no copy of the game's constants, so a
	port that raises them needs no UE change. */
	uint32_t max_nodes_per_model;
	uint32_t max_regions_per_model;
	uint32_t max_permutations_per_region;
	uint32_t max_regions_per_object;
	uint32_t reserved;
};

struct ue_bridge_definition
{
	int32_t tag_index;
	int16_t object_type;
	/* into the model table; -1 for none */
	int16_t model;
	/* -1 (NONE): the object is drawn as a static mesh */
	int32_t animation_graph_tag;
	/* world units */
	float bounding_radius;
};

struct ue_bridge_model
{
	int32_t tag_index;
	/* detail level 0 (lowest) to 4 (highest), as render_model picks them */
	float detail_cutoff_pixels[UE_BRIDGE_DETAIL_LEVELS];
	int16_t node_counts[UE_BRIDGE_DETAIL_LEVELS];
	uint16_t reserved;
	/* struct ue_bridge_node */
	struct ue_bridge_table nodes;
	/* struct ue_bridge_region */
	struct ue_bridge_table regions;
	/* struct ue_bridge_geometry: each Halo geometry once, however many
	permutations and detail levels use it */
	struct ue_bridge_table geometries;
	/* int32_t: a shader tag index per model shader slot */
	struct ue_bridge_table shaders;
};

struct ue_bridge_node
{
	/* -1: the root */
	int16_t parent;
	uint16_t reserved;
	/* the default pose relative to the parent, built by the game's own
	matrix4x3_from_point_and_quaternion: Halo's stored quaternions give the
	transpose of the usual rotation, so UE never rebuilds them */
	struct ue_bridge_matrix default_local;
	/* runtime_default_inverse_matrix */
	struct ue_bridge_matrix default_inverse;
};

struct ue_bridge_region
{
	/* struct ue_bridge_permutation */
	struct ue_bridge_table permutations;
};

struct ue_bridge_permutation
{
	/* into the model's geometries per detail level, -1 for none */
	int16_t geometry[UE_BRIDGE_DETAIL_LEVELS];
	uint16_t reserved;
};

struct ue_bridge_geometry
{
	/* struct ue_bridge_part */
	struct ue_bridge_table parts;
};

struct ue_bridge_part
{
	/* the model's shader slot */
	int16_t shader;
	uint16_t reserved;
	/* struct ue_bridge_model_vertex */
	struct ue_bridge_table vertices;
	/* ue_bridge_model_index triangle list; front faces (right-handed cross product) agree with the vertex normals, the reverse of the game's D3DCULL_CCW order */
	struct ue_bridge_table indices;
};

/* 40 bytes: position 0, normal 12, uv 24, node 32, reserved 34, weight 36 */
struct ue_bridge_model_vertex
{
	/* model space, default pose, world units */
	float position[3];
	float normal[3];
	float uv[2];
	/* model-wide node indices, bytes as in Halo's own vertex (which stores
	them times 3); ue_bridge_export.c asserts MAXIMUM_NODES_PER_MODEL fits */
	uint8_t node[2];
	uint16_t reserved;
	/* node[0]'s weight; node[1] has 1 - weight */
	float weight;
};

struct ue_bridge_bsp_batch
{
	/* -1 for surfaces no cluster lists */
	ue_bridge_cluster_index cluster;
	uint16_t reserved;
	int32_t shader_tag;
	/* struct ue_bridge_bsp_vertex */
	struct ue_bridge_table vertices;
	/* ue_bridge_bsp_index triangle list; front faces (right-handed cross product) agree with the vertex normals, the reverse of the game's D3DCULL_CCW order */
	struct ue_bridge_table indices;
};

struct ue_bridge_bsp_vertex
{
	float position[3];
	float normal[3];
	float uv[2];
};

UEB_STATIC_ASSERT(sizeof(struct ue_bridge_directory) == 120, "directory size");
UEB_STATIC_ASSERT(offsetof(struct ue_bridge_directory, magic) == 0, "directory magic");
UEB_STATIC_ASSERT(offsetof(struct ue_bridge_directory, version) == 4, "directory version");
UEB_STATIC_ASSERT(offsetof(struct ue_bridge_directory, sequence) == 8, "directory sequence");
UEB_STATIC_ASSERT(offsetof(struct ue_bridge_directory, game_pid) == 12, "directory game_pid");
UEB_STATIC_ASSERT(offsetof(struct ue_bridge_directory, session_id) == 16, "directory session_id");
UEB_STATIC_ASSERT(offsetof(struct ue_bridge_directory, section_name) == 24, "directory section_name");

UEB_STATIC_ASSERT(sizeof(struct ue_bridge_ring_desc) == 16, "ring descriptor size");
UEB_STATIC_ASSERT(offsetof(struct ue_bridge_ring_desc, offset) == 0, "ring descriptor offset");
UEB_STATIC_ASSERT(offsetof(struct ue_bridge_ring_desc, slot_size) == 4, "ring descriptor slot_size");
UEB_STATIC_ASSERT(offsetof(struct ue_bridge_ring_desc, slot_count) == 8, "ring descriptor slot_count");
UEB_STATIC_ASSERT(offsetof(struct ue_bridge_ring_desc, published) == 12, "ring descriptor published");
UEB_STATIC_ASSERT(sizeof(struct ue_bridge_crash_record) == 16, "crash record size");
UEB_STATIC_ASSERT(offsetof(struct ue_bridge_crash_record, exception_code) == 0, "crash record exception_code");
UEB_STATIC_ASSERT(offsetof(struct ue_bridge_crash_record, exception_address) == 4, "crash record exception_address");
UEB_STATIC_ASSERT(offsetof(struct ue_bridge_crash_record, thread_id) == 8, "crash record thread_id");
UEB_STATIC_ASSERT(offsetof(struct ue_bridge_crash_record, reserved) == 12, "crash record reserved");

UEB_STATIC_ASSERT(offsetof(struct ue_bridge_header, magic) == 0, "header magic");
UEB_STATIC_ASSERT(offsetof(struct ue_bridge_header, version) == 4, "header version");
UEB_STATIC_ASSERT(offsetof(struct ue_bridge_header, header_size) == 8, "header header_size");
UEB_STATIC_ASSERT(offsetof(struct ue_bridge_header, section_size) == 12, "header section_size");
UEB_STATIC_ASSERT(offsetof(struct ue_bridge_header, session_id) == 16, "header session_id");
UEB_STATIC_ASSERT(offsetof(struct ue_bridge_header, qpc_frequency) == 24, "header qpc_frequency");
UEB_STATIC_ASSERT(offsetof(struct ue_bridge_header, game_pid) == 32, "header game_pid");
UEB_STATIC_ASSERT(offsetof(struct ue_bridge_header, max_objects) == 36, "header max_objects");
UEB_STATIC_ASSERT(offsetof(struct ue_bridge_header, game_hang_timeout_ms) == 40, "header game_hang_timeout_ms");
UEB_STATIC_ASSERT(offsetof(struct ue_bridge_header, reserved0) == 44, "header reserved0");
UEB_STATIC_ASSERT(offsetof(struct ue_bridge_header, tick_ring) == 48, "header tick_ring");
UEB_STATIC_ASSERT(offsetof(struct ue_bridge_header, tick_ring.offset) == 48, "header tick_ring.offset");
UEB_STATIC_ASSERT(offsetof(struct ue_bridge_header, tick_ring.slot_size) == 52, "header tick_ring.slot_size");
UEB_STATIC_ASSERT(offsetof(struct ue_bridge_header, tick_ring.slot_count) == 56, "header tick_ring.slot_count");
UEB_STATIC_ASSERT(offsetof(struct ue_bridge_header, tick_ring.published) == 60, "header tick_ring.published");
UEB_STATIC_ASSERT(offsetof(struct ue_bridge_header, frame_ring) == 64, "header frame_ring");
UEB_STATIC_ASSERT(offsetof(struct ue_bridge_header, frame_ring.offset) == 64, "header frame_ring.offset");
UEB_STATIC_ASSERT(offsetof(struct ue_bridge_header, frame_ring.slot_size) == 68, "header frame_ring.slot_size");
UEB_STATIC_ASSERT(offsetof(struct ue_bridge_header, frame_ring.slot_count) == 72, "header frame_ring.slot_count");
UEB_STATIC_ASSERT(offsetof(struct ue_bridge_header, frame_ring.published) == 76, "header frame_ring.published");
UEB_STATIC_ASSERT(offsetof(struct ue_bridge_header, game_log_path) == 80, "header game_log_path");
UEB_STATIC_ASSERT(offsetof(struct ue_bridge_header, game_heartbeat_qpc) == 600, "header game_heartbeat_qpc");
UEB_STATIC_ASSERT(offsetof(struct ue_bridge_header, load_epoch) == 608, "header load_epoch");
UEB_STATIC_ASSERT(offsetof(struct ue_bridge_header, state_epoch) == 612, "header state_epoch");
UEB_STATIC_ASSERT(offsetof(struct ue_bridge_header, game_debugger_attached) == 616, "header game_debugger_attached");
UEB_STATIC_ASSERT(offsetof(struct ue_bridge_header, game_busy) == 620, "header game_busy");
UEB_STATIC_ASSERT(offsetof(struct ue_bridge_header, game_stopping) == 624, "header game_stopping");
UEB_STATIC_ASSERT(offsetof(struct ue_bridge_header, game_crashing) == 628, "header game_crashing");
UEB_STATIC_ASSERT(offsetof(struct ue_bridge_header, crash) == 632, "header crash");
UEB_STATIC_ASSERT(offsetof(struct ue_bridge_header, crash.exception_code) == 632, "header crash.exception_code");
UEB_STATIC_ASSERT(offsetof(struct ue_bridge_header, crash.exception_address) == 636, "header crash.exception_address");
UEB_STATIC_ASSERT(offsetof(struct ue_bridge_header, crash.thread_id) == 640, "header crash.thread_id");
UEB_STATIC_ASSERT(offsetof(struct ue_bridge_header, crash.reserved) == 644, "header crash.reserved");
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
UEB_STATIC_ASSERT(offsetof(struct ue_bridge_header, reserved1) == 692, "header reserved1");
UEB_STATIC_ASSERT(offsetof(struct ue_bridge_header, ue_session_dir) == 696, "header ue_session_dir");
UEB_STATIC_ASSERT(offsetof(struct ue_bridge_header, game_refresh_hz) == 1216, "header game_refresh_hz");
UEB_STATIC_ASSERT(offsetof(struct ue_bridge_header, game_frame_target_hz) == 1220, "header game_frame_target_hz");
/* the 64-bit fields read without the seqlock must be naturally aligned to be read in one access */
UEB_STATIC_ASSERT(offsetof(struct ue_bridge_header, game_heartbeat_qpc) % 8 == 0, "game heartbeat alignment");
UEB_STATIC_ASSERT(offsetof(struct ue_bridge_header, ue_heartbeat_qpc) % 8 == 0, "UE heartbeat alignment");
UEB_STATIC_ASSERT(sizeof(struct ue_bridge_header) <= UE_BRIDGE_HEADER_SIZE, "header fits its region");

UEB_STATIC_ASSERT(sizeof(struct ue_bridge_slot) == 24, "slot size");
UEB_STATIC_ASSERT(offsetof(struct ue_bridge_slot, sequence) == 0, "slot sequence");
UEB_STATIC_ASSERT(offsetof(struct ue_bridge_slot, reserved) == 4, "slot reserved");
UEB_STATIC_ASSERT(offsetof(struct ue_bridge_slot, id) == 8, "slot id");
UEB_STATIC_ASSERT(offsetof(struct ue_bridge_slot, publish_qpc) == 16, "slot publish_qpc");
UEB_STATIC_ASSERT(offsetof(struct ue_bridge_header, load_region) == 1224, "header load_region");
UEB_STATIC_ASSERT(offsetof(struct ue_bridge_header, load_sequence) == 1232, "header load_sequence");
UEB_STATIC_ASSERT(offsetof(struct ue_bridge_header, export_epoch) == 1236, "header export_epoch");
UEB_STATIC_ASSERT(offsetof(struct ue_bridge_header, export_complete) == 1240, "header export_complete");
UEB_STATIC_ASSERT(offsetof(struct ue_bridge_header, game_holding) == 1244, "header game_holding");
UEB_STATIC_ASSERT(offsetof(struct ue_bridge_header, game_truncated_ticks) == 1248, "header game_truncated_ticks");
UEB_STATIC_ASSERT(offsetof(struct ue_bridge_header, reserved2) == 1252, "header reserved2");
UEB_STATIC_ASSERT(sizeof(struct ue_bridge_header) == 1256, "header size");
UEB_STATIC_ASSERT(offsetof(struct ue_bridge_region_desc, offset) == 0, "region desc offset");
UEB_STATIC_ASSERT(offsetof(struct ue_bridge_region_desc, size) == 4, "region desc size field");
UEB_STATIC_ASSERT(sizeof(struct ue_bridge_region_desc) == 8, "region desc size");
UEB_STATIC_ASSERT(offsetof(struct ue_bridge_matrix, scale) == 0, "matrix scale");
UEB_STATIC_ASSERT(offsetof(struct ue_bridge_matrix, forward) == 4, "matrix forward");
UEB_STATIC_ASSERT(offsetof(struct ue_bridge_matrix, left) == 16, "matrix left");
UEB_STATIC_ASSERT(offsetof(struct ue_bridge_matrix, up) == 28, "matrix up");
UEB_STATIC_ASSERT(offsetof(struct ue_bridge_matrix, position) == 40, "matrix position");
UEB_STATIC_ASSERT(sizeof(struct ue_bridge_matrix) == 52, "matrix size");
UEB_STATIC_ASSERT(offsetof(struct ue_bridge_tick_header, slot) == 0, "tick header slot");
UEB_STATIC_ASSERT(offsetof(struct ue_bridge_tick_header, used) == 24, "tick header used");
UEB_STATIC_ASSERT(offsetof(struct ue_bridge_tick_header, object_count) == 28, "tick header object_count");
UEB_STATIC_ASSERT(offsetof(struct ue_bridge_tick_header, load_epoch) == 32, "tick header load_epoch");
UEB_STATIC_ASSERT(offsetof(struct ue_bridge_tick_header, state_epoch) == 36, "tick header state_epoch");
UEB_STATIC_ASSERT(offsetof(struct ue_bridge_tick_header, active_bsp) == 40, "tick header active_bsp");
UEB_STATIC_ASSERT(offsetof(struct ue_bridge_tick_header, flags) == 42, "tick header flags");
UEB_STATIC_ASSERT(offsetof(struct ue_bridge_tick_header, reserved) == 44, "tick header reserved");
UEB_STATIC_ASSERT(sizeof(struct ue_bridge_tick_header) == 48, "tick header size");
UEB_STATIC_ASSERT(offsetof(struct ue_bridge_object_record, datum_index) == 0, "object record datum_index");
UEB_STATIC_ASSERT(offsetof(struct ue_bridge_object_record, definition) == 4, "object record definition");
UEB_STATIC_ASSERT(offsetof(struct ue_bridge_object_record, flags) == 6, "object record flags");
UEB_STATIC_ASSERT(offsetof(struct ue_bridge_object_record, node_count) == 8, "object record node_count");
UEB_STATIC_ASSERT(offsetof(struct ue_bridge_object_record, region_count) == 10, "object record region_count");
UEB_STATIC_ASSERT(offsetof(struct ue_bridge_object_record, reserved) == 11, "object record reserved");
UEB_STATIC_ASSERT(offsetof(struct ue_bridge_object_record, size) == 12, "object record size field");
UEB_STATIC_ASSERT(sizeof(struct ue_bridge_object_record) == 16, "object record size");
UEB_STATIC_ASSERT(offsetof(struct ue_bridge_frame_slot, slot) == 0, "frame slot slot");
UEB_STATIC_ASSERT(offsetof(struct ue_bridge_frame_slot, interpolation_fraction) == 24, "frame slot interpolation_fraction");
UEB_STATIC_ASSERT(offsetof(struct ue_bridge_frame_slot, camera_valid) == 28, "frame slot camera_valid");
UEB_STATIC_ASSERT(offsetof(struct ue_bridge_frame_slot, camera_position) == 32, "frame slot camera_position");
UEB_STATIC_ASSERT(offsetof(struct ue_bridge_frame_slot, camera_forward) == 44, "frame slot camera_forward");
UEB_STATIC_ASSERT(offsetof(struct ue_bridge_frame_slot, camera_up) == 56, "frame slot camera_up");
UEB_STATIC_ASSERT(offsetof(struct ue_bridge_frame_slot, vertical_fov) == 68, "frame slot vertical_fov");
UEB_STATIC_ASSERT(offsetof(struct ue_bridge_frame_slot, z_near) == 72, "frame slot z_near");
UEB_STATIC_ASSERT(offsetof(struct ue_bridge_frame_slot, z_far) == 76, "frame slot z_far");
UEB_STATIC_ASSERT(offsetof(struct ue_bridge_frame_slot, tick_id) == 80, "frame slot tick_id");
UEB_STATIC_ASSERT(sizeof(struct ue_bridge_frame_slot) == 88, "frame slot size");
UEB_STATIC_ASSERT(sizeof(struct ue_bridge_frame_slot) <= UE_BRIDGE_FRAME_SLOT_SIZE, "frame slot fits");
UEB_STATIC_ASSERT(offsetof(struct ue_bridge_table, offset) == 0, "table offset");
UEB_STATIC_ASSERT(offsetof(struct ue_bridge_table, count) == 4, "table count");
UEB_STATIC_ASSERT(sizeof(struct ue_bridge_table) == 8, "table size");
UEB_STATIC_ASSERT(offsetof(struct ue_bridge_bsp_entry, tag_index) == 0, "bsp entry tag_index");
UEB_STATIC_ASSERT(offsetof(struct ue_bridge_bsp_entry, ready) == 4, "bsp entry ready");
UEB_STATIC_ASSERT(offsetof(struct ue_bridge_bsp_entry, batches) == 8, "bsp entry batches");
UEB_STATIC_ASSERT(offsetof(struct ue_bridge_bsp_entry, cluster_count) == 16, "bsp entry cluster_count");
UEB_STATIC_ASSERT(offsetof(struct ue_bridge_bsp_entry, unclustered_surfaces) == 20, "bsp entry unclustered_surfaces");
UEB_STATIC_ASSERT(offsetof(struct ue_bridge_bsp_entry, duplicate_surfaces) == 24, "bsp entry duplicate_surfaces");
UEB_STATIC_ASSERT(offsetof(struct ue_bridge_bsp_entry, reserved) == 28, "bsp entry reserved");
UEB_STATIC_ASSERT(sizeof(struct ue_bridge_bsp_entry) == 32, "bsp entry size");
UEB_STATIC_ASSERT(offsetof(struct ue_bridge_load_root, magic) == 0, "load root magic");
UEB_STATIC_ASSERT(offsetof(struct ue_bridge_load_root, load_epoch) == 4, "load root load_epoch");
UEB_STATIC_ASSERT(offsetof(struct ue_bridge_load_root, map_name) == 8, "load root map_name");
UEB_STATIC_ASSERT(offsetof(struct ue_bridge_load_root, definitions) == 72, "load root definitions");
UEB_STATIC_ASSERT(offsetof(struct ue_bridge_load_root, models) == 80, "load root models");
UEB_STATIC_ASSERT(offsetof(struct ue_bridge_load_root, bsps) == 88, "load root bsps");
UEB_STATIC_ASSERT(offsetof(struct ue_bridge_load_root, missing) == 96, "load root missing");
UEB_STATIC_ASSERT(offsetof(struct ue_bridge_load_root, max_nodes_per_model) == 100, "load root max_nodes_per_model");
UEB_STATIC_ASSERT(offsetof(struct ue_bridge_load_root, max_regions_per_model) == 104, "load root max_regions_per_model");
UEB_STATIC_ASSERT(offsetof(struct ue_bridge_load_root, max_permutations_per_region) == 108, "load root max_permutations_per_region");
UEB_STATIC_ASSERT(offsetof(struct ue_bridge_load_root, max_regions_per_object) == 112, "load root max_regions_per_object");
UEB_STATIC_ASSERT(offsetof(struct ue_bridge_load_root, reserved) == 116, "load root reserved");
UEB_STATIC_ASSERT(sizeof(struct ue_bridge_load_root) == 120, "load root size");
UEB_STATIC_ASSERT(offsetof(struct ue_bridge_definition, tag_index) == 0, "definition tag_index");
UEB_STATIC_ASSERT(offsetof(struct ue_bridge_definition, object_type) == 4, "definition object_type");
UEB_STATIC_ASSERT(offsetof(struct ue_bridge_definition, model) == 6, "definition model");
UEB_STATIC_ASSERT(offsetof(struct ue_bridge_definition, animation_graph_tag) == 8, "definition animation_graph_tag");
UEB_STATIC_ASSERT(offsetof(struct ue_bridge_definition, bounding_radius) == 12, "definition bounding_radius");
UEB_STATIC_ASSERT(sizeof(struct ue_bridge_definition) == 16, "definition size");
UEB_STATIC_ASSERT(offsetof(struct ue_bridge_model, tag_index) == 0, "model tag_index");
UEB_STATIC_ASSERT(offsetof(struct ue_bridge_model, detail_cutoff_pixels) == 4, "model detail_cutoff_pixels");
UEB_STATIC_ASSERT(offsetof(struct ue_bridge_model, node_counts) == 24, "model node_counts");
UEB_STATIC_ASSERT(offsetof(struct ue_bridge_model, reserved) == 34, "model reserved");
UEB_STATIC_ASSERT(offsetof(struct ue_bridge_model, nodes) == 36, "model nodes");
UEB_STATIC_ASSERT(offsetof(struct ue_bridge_model, regions) == 44, "model regions");
UEB_STATIC_ASSERT(offsetof(struct ue_bridge_model, geometries) == 52, "model geometries");
UEB_STATIC_ASSERT(offsetof(struct ue_bridge_model, shaders) == 60, "model shaders");
UEB_STATIC_ASSERT(sizeof(struct ue_bridge_model) == 68, "model size");
UEB_STATIC_ASSERT(offsetof(struct ue_bridge_node, parent) == 0, "node parent");
UEB_STATIC_ASSERT(offsetof(struct ue_bridge_node, reserved) == 2, "node reserved");
UEB_STATIC_ASSERT(offsetof(struct ue_bridge_node, default_local) == 4, "node default_local");
UEB_STATIC_ASSERT(offsetof(struct ue_bridge_node, default_inverse) == 56, "node default_inverse");
UEB_STATIC_ASSERT(sizeof(struct ue_bridge_node) == 108, "node size");
UEB_STATIC_ASSERT(offsetof(struct ue_bridge_region, permutations) == 0, "region permutations");
UEB_STATIC_ASSERT(sizeof(struct ue_bridge_region) == 8, "region size");
UEB_STATIC_ASSERT(offsetof(struct ue_bridge_permutation, geometry) == 0, "permutation geometry");
UEB_STATIC_ASSERT(offsetof(struct ue_bridge_permutation, reserved) == 10, "permutation reserved");
UEB_STATIC_ASSERT(sizeof(struct ue_bridge_permutation) == 12, "permutation size");
UEB_STATIC_ASSERT(offsetof(struct ue_bridge_geometry, parts) == 0, "geometry parts");
UEB_STATIC_ASSERT(sizeof(struct ue_bridge_geometry) == 8, "geometry size");
UEB_STATIC_ASSERT(offsetof(struct ue_bridge_part, shader) == 0, "part shader");
UEB_STATIC_ASSERT(offsetof(struct ue_bridge_part, reserved) == 2, "part reserved");
UEB_STATIC_ASSERT(offsetof(struct ue_bridge_part, vertices) == 4, "part vertices");
UEB_STATIC_ASSERT(offsetof(struct ue_bridge_part, indices) == 12, "part indices");
UEB_STATIC_ASSERT(sizeof(struct ue_bridge_part) == 20, "part size");
UEB_STATIC_ASSERT(offsetof(struct ue_bridge_model_vertex, position) == 0, "model vertex position");
UEB_STATIC_ASSERT(offsetof(struct ue_bridge_model_vertex, normal) == 12, "model vertex normal");
UEB_STATIC_ASSERT(offsetof(struct ue_bridge_model_vertex, uv) == 24, "model vertex uv");
UEB_STATIC_ASSERT(offsetof(struct ue_bridge_model_vertex, node) == 32, "model vertex node");
UEB_STATIC_ASSERT(offsetof(struct ue_bridge_model_vertex, reserved) == 34, "model vertex reserved");
UEB_STATIC_ASSERT(offsetof(struct ue_bridge_model_vertex, weight) == 36, "model vertex weight");
UEB_STATIC_ASSERT(sizeof(struct ue_bridge_model_vertex) == 40, "model vertex size");
UEB_STATIC_ASSERT(sizeof(ue_bridge_model_index) == 2, "model index width");
UEB_STATIC_ASSERT(sizeof(ue_bridge_bsp_index) == 4, "bsp index width");
UEB_STATIC_ASSERT(sizeof(ue_bridge_cluster_index) == 2, "cluster index width");
UEB_STATIC_ASSERT(offsetof(struct ue_bridge_bsp_batch, cluster) == 0, "bsp batch cluster");
UEB_STATIC_ASSERT(offsetof(struct ue_bridge_bsp_batch, reserved) == 2, "bsp batch reserved");
UEB_STATIC_ASSERT(offsetof(struct ue_bridge_bsp_batch, shader_tag) == 4, "bsp batch shader_tag");
UEB_STATIC_ASSERT(offsetof(struct ue_bridge_bsp_batch, vertices) == 8, "bsp batch vertices");
UEB_STATIC_ASSERT(offsetof(struct ue_bridge_bsp_batch, indices) == 16, "bsp batch indices");
UEB_STATIC_ASSERT(sizeof(struct ue_bridge_bsp_batch) == 24, "bsp batch size");
UEB_STATIC_ASSERT(offsetof(struct ue_bridge_bsp_vertex, position) == 0, "bsp vertex position");
UEB_STATIC_ASSERT(offsetof(struct ue_bridge_bsp_vertex, normal) == 12, "bsp vertex normal");
UEB_STATIC_ASSERT(offsetof(struct ue_bridge_bsp_vertex, uv) == 24, "bsp vertex uv");
UEB_STATIC_ASSERT(sizeof(struct ue_bridge_bsp_vertex) == 32, "bsp vertex size");

#endif

/*
TEST_FORMAT.C

The layout the 32-bit game and the 64-bit renderer share. The header checks
the same offsets at compile time in both compilers; these tests check them
again at run time so a failure names the field.
*/

#include "ueb_test.h"
#include "ue_bridge_format.h"

#include <stddef.h>

static void format_directory_layout(void)
{
	UEB_CHECK(sizeof(struct ue_bridge_directory) == 120);
	UEB_CHECK(offsetof(struct ue_bridge_directory, sequence) == 8);
	UEB_CHECK(offsetof(struct ue_bridge_directory, game_pid) == 12);
	UEB_CHECK(offsetof(struct ue_bridge_directory, session_id) == 16);
	UEB_CHECK(offsetof(struct ue_bridge_directory, section_name) == 24);
}

static void format_header_layout(void)
{
	UEB_CHECK(sizeof(struct ue_bridge_header) == 1256);
	UEB_CHECK(offsetof(struct ue_bridge_header, session_id) == 16);
	UEB_CHECK(offsetof(struct ue_bridge_header, qpc_frequency) == 24);
	UEB_CHECK(offsetof(struct ue_bridge_header, max_objects) == 36);
	UEB_CHECK(offsetof(struct ue_bridge_header, tick_ring) == 48);
	UEB_CHECK(offsetof(struct ue_bridge_header, tick_ring.published) == 60);
	UEB_CHECK(offsetof(struct ue_bridge_header, frame_ring.published) == 76);
	UEB_CHECK(offsetof(struct ue_bridge_header, crash.thread_id) == 640);
	UEB_CHECK(offsetof(struct ue_bridge_header, frame_ring) == 64);
	UEB_CHECK(offsetof(struct ue_bridge_header, game_log_path) == 80);
	UEB_CHECK(offsetof(struct ue_bridge_header, game_heartbeat_qpc) == 600);
	UEB_CHECK(offsetof(struct ue_bridge_header, load_epoch) == 608);
	UEB_CHECK(offsetof(struct ue_bridge_header, crash) == 632);
	UEB_CHECK(offsetof(struct ue_bridge_header, ue_heartbeat_qpc) == 648);
	UEB_CHECK(offsetof(struct ue_bridge_header, ue_pid) == 656);
	UEB_CHECK(offsetof(struct ue_bridge_header, ue_dump_done) == 684);
	UEB_CHECK(offsetof(struct ue_bridge_header, ue_session_dir) == 696);
	UEB_CHECK(offsetof(struct ue_bridge_header, game_refresh_hz) == 1216);
	UEB_CHECK(offsetof(struct ue_bridge_header, game_frame_target_hz) == 1220);
	UEB_CHECK(sizeof(struct ue_bridge_header) <= UE_BRIDGE_HEADER_SIZE);
}

static void format_slot_layout(void)
{
	UEB_CHECK(sizeof(struct ue_bridge_slot) == 24);
	UEB_CHECK(offsetof(struct ue_bridge_slot, id) == 8);
	UEB_CHECK(sizeof(struct ue_bridge_frame_slot) == 88);
	UEB_CHECK(offsetof(struct ue_bridge_frame_slot, interpolation_fraction) == 24);
}

static void format_version_is_four(void)
{
	UEB_CHECK(UE_BRIDGE_VERSION == 4u);
}

static void format_header_tail_offsets(void)
{
	UEB_CHECK(offsetof(struct ue_bridge_header, load_region) == 1224);
	UEB_CHECK(offsetof(struct ue_bridge_header, load_sequence) == 1232);
	UEB_CHECK(offsetof(struct ue_bridge_header, export_epoch) == 1236);
	UEB_CHECK(offsetof(struct ue_bridge_header, export_complete) == 1240);
	UEB_CHECK(offsetof(struct ue_bridge_header, game_holding) == 1244);
	UEB_CHECK(offsetof(struct ue_bridge_header, game_truncated_ticks) == 1248);
	UEB_CHECK(offsetof(struct ue_bridge_header, reserved2) == 1252);
	UEB_CHECK(sizeof(struct ue_bridge_header) == 1256);
}

static void format_payload_sizes(void)
{
	UEB_CHECK(sizeof(struct ue_bridge_matrix) == 52);
	UEB_CHECK(sizeof(struct ue_bridge_tick_header) == 48);
	UEB_CHECK(offsetof(struct ue_bridge_tick_header, used) == 24);
	UEB_CHECK(sizeof(struct ue_bridge_object_record) == 16);
	UEB_CHECK(sizeof(struct ue_bridge_frame_slot) == 88);
	UEB_CHECK(offsetof(struct ue_bridge_frame_slot, camera_position) == 32);
	UEB_CHECK(offsetof(struct ue_bridge_frame_slot, tick_id) == 80);
	UEB_CHECK(sizeof(struct ue_bridge_load_root) == 120);
	UEB_CHECK(offsetof(struct ue_bridge_load_root, bsps) == 88);
	UEB_CHECK(offsetof(struct ue_bridge_load_root, max_nodes_per_model) == 100);
	UEB_CHECK(offsetof(struct ue_bridge_load_root, reserved) == 116);
	UEB_CHECK(sizeof(struct ue_bridge_definition) == 16);
	UEB_CHECK(sizeof(struct ue_bridge_model) == 68);
	UEB_CHECK(sizeof(struct ue_bridge_node) == 108);
	UEB_CHECK(sizeof(struct ue_bridge_part) == 20);
	UEB_CHECK(sizeof(struct ue_bridge_model_vertex) == 40);
	UEB_CHECK(offsetof(struct ue_bridge_model_vertex, node) == 32);
	UEB_CHECK(offsetof(struct ue_bridge_model_vertex, weight) == 36);
	UEB_CHECK(sizeof(struct ue_bridge_bsp_batch) == 24);
	UEB_CHECK(sizeof(struct ue_bridge_bsp_vertex) == 32);
}

const struct ueb_test ueb_format_tests[] =
{
	{ "format_directory_layout", format_directory_layout },
	{ "format_header_layout", format_header_layout },
	{ "format_version_is_four", format_version_is_four },
	{ "format_header_tail_offsets", format_header_tail_offsets },
	{ "format_payload_sizes", format_payload_sizes },
	{ "format_slot_layout", format_slot_layout },
	{ 0, 0 }
};

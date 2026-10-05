/*
HALO_LINUX_SOURCE_FIXUPS.H

Game-only workarounds for source that MSVC accepts but clang rejects, where
editing the source itself would change the byte-matched MSVC output (see
port/linux/README.md for how each was checked).
*/

#ifndef __HALO_LINUX_SOURCE_FIXUPS_H
#define __HALO_LINUX_SOURCE_FIXUPS_H

/* rasterizer.h declares rasterizer_debug_drawing_begin(boolean opaque) while
rasterizer_xbox_debug.h declares a second `long zbias` parameter, and
rasterizer_debug.c includes both and passes two arguments. MSVC tolerates
the mismatch; the definition ignores zbias. Adding the parameter to
rasterizer.h perturbs MSVC's register allocation elsewhere, so instead every
declaration and call collapses to the one-parameter form here. */
#define rasterizer_debug_drawing_begin(opaque, ...) (rasterizer_debug_drawing_begin)(opaque)

/* frames between the 30 Hz ticks (port/linux/game/render_interpolation.c);
the platform layer reads the display.interpolation setting */
struct observer_result;
struct render_camera;
struct real_matrix4x3;
int halo_interpolation_enabled(void);
float game_time_get_tick_fraction(void);
void render_interpolation_tick(void);
void render_interpolation_reset(void);
void render_interpolation_frame_begin(void);
void render_interpolation_frame_end(void);
float render_interpolation_fraction(void);
struct real_matrix4x3 *render_interpolation_object_node_matrices(long object_index);
struct observer_result const *render_interpolation_camera(short local_player_index,
	struct observer_result const *observer);
void render_interpolation_first_person(short local_player_index, struct real_matrix4x3 *node_matrices,
	short node_count, struct render_camera const *camera);
float render_interpolation_game_time_sec(long ticks);

/* the HaloCEUE renderer's bridge (port/linux/src/ue_bridge_game.c) */
void ue_bridge_game_loading(int loading);
void ue_bridge_game_tick(long tick);
void ue_bridge_game_frame_begin(long frame, float interpolation_fraction);
void ue_bridge_game_window_camera(long window_index, struct render_camera const *camera);
void ue_bridge_game_frame_end(long frame, long tick, float interpolation_fraction);
/* cseries.h's boolean, spelled out: this header precedes every game header */
unsigned char render_interpolation_object_at_rest(long object_index);
void ue_bridge_game_map_loaded(void);
void ue_bridge_game_map_ready(void);
void ue_bridge_game_structure_bsp_loaded(short structure_bsp_index);
void ue_bridge_game_console_started(void);
void ue_bridge_game_state_loaded(void);
void ue_bridge_game_halted(void);

/* the width of the screen the game draws, 480 lines tall: the device's or
the display's shape, or 640 (port/linux/src/d3d8_gl.c) */
long halo_screen_width(void);
/* takes up a new width between frames (F11); returns the width */
long halo_screen_commit(void);
/* while TRUE, drawing shifts right to center 640-column layouts */
void halo_screen_ui_offset(unsigned char centered);
/* the mouse in the menus (source/interface/ui_widget.c) */
#include "halo_ui_pointer.h"

#endif

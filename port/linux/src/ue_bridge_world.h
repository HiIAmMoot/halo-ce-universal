/*
UE_BRIDGE_WORLD.H

The game's side of the UE bridge's export and publishing, which reads the
decomp's tags and objects (port/linux/game/ue_bridge_export.c,
ue_bridge_publish.c). ue_bridge_game.c calls these from its hooks; the tests
in port/ue_bridge/tests fake them, so the adapter links without the game.
*/

#ifndef UE_BRIDGE_WORLD_H
#define UE_BRIDGE_WORLD_H

#include <stdint.h>

#define UE_BRIDGE_NO_DEFINITION 0xFFFFu

struct render_camera;
struct ue_bridge_camera;

/* the loaded map's object definitions, models and loaded BSP into the load
region, from ue_bridge_load_begin on; 1 when everything fit. It leaves the
load sequence open: the caller ends it with ue_bridge_load_end(the result),
after the map's first tick is published. */
int ue_bridge_world_export_map(void);
/* the BSP the game just loaded, appended to this map's export if it isn't
in it yet */
void ue_bridge_world_export_bsp(short structure_bsp_index);
/* main_set_map_name */
void ue_bridge_world_set_start_map(const char *scenario_name);
/* the definition table index of an object definition tag;
UE_BRIDGE_NO_DEFINITION when it wasn't exported */
unsigned short ue_bridge_export_definition_index(long definition_tag_index);

/* every object into a tick slot (ue_bridge_tick_begin to ue_bridge_tick_end) */
void ue_bridge_world_publish_tick(uint64_t tick);
/* 1 and *out from the game's render camera */
int ue_bridge_world_camera(const struct render_camera *camera, struct ue_bridge_camera *out);
/* the load hold applies to local games only in M2: a network client reports
loaded inside the call that loads the map (Phase 0 design, section 5.6) */
int ue_bridge_world_hold_allowed(void);
/* main_stop_time, main_start_time */
void ue_bridge_world_stop_time(void);
void ue_bridge_world_start_time(void);

#endif

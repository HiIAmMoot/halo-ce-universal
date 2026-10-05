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

/* the loaded map's object definitions, models and loaded BSP into the load
region (ue_bridge_load_begin to ue_bridge_load_end); 1 when everything fit */
int ue_bridge_world_export_map(void);
/* the BSP the game just loaded, appended to this map's export if it isn't
in it yet */
void ue_bridge_world_export_bsp(short structure_bsp_index);
/* main_set_map_name */
void ue_bridge_world_set_start_map(const char *scenario_name);
/* the definition table index of an object definition tag;
UE_BRIDGE_NO_DEFINITION when it wasn't exported */
unsigned short ue_bridge_export_definition_index(long definition_tag_index);

#endif

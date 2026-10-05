/*
UE_BRIDGE_SHARED.C

The UE bridge code the game shares with the HaloCEUE plugin (port/ue_bridge).
The build compiles the C files of port/linux/src and port/windows/src, not
port/ue_bridge, so they are compiled through this file.
*/

#include "../../ue_bridge/ue_bridge_ring.c"
#include "../../ue_bridge/ue_bridge_policy.c"
#include "../../ue_bridge/ue_bridge_load.c"
#include "../../ue_bridge/ue_bridge_codec.c"
#include "../../ue_bridge/ue_bridge_model.c"

/*
UE_BRIDGE_PLATFORM_NULL.C

No UE bridge: the HaloCEUE renderer is Windows only. Windows replaces this
file with port/windows/src/win32_ue_bridge.c (port/windows/port.json).
*/

#include "ue_bridge_platform.h"

#include <stddef.h>

const struct ue_bridge_os *ue_bridge_platform_os(void)
{
	return NULL;
}

void ue_bridge_platform_install_crash_hook(void)
{
}

int ue_bridge_platform_start_watcher(const struct ue_bridge_watch_config *config)
{
	(void)config;
	return 0;
}

void ue_bridge_platform_stop_watcher(void)
{
}

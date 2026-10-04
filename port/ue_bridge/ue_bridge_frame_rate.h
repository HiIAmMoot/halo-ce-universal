/*
UE_BRIDGE_FRAME_RATE.H

The game's frame-rate rules in one place: the frame limiter
(platform_video_swap in port/linux/src/sdl_platform.c) sleeps by
ue_bridge_frame_limit_rate, and the bridge publishes ue_bridge_frame_target_hz
for UE to cap itself to (design spec section 6.5). Both come from the same
functions so that what UE aims for cannot drift from what the game does.
*/

#ifndef UE_BRIDGE_FRAME_RATE_H
#define UE_BRIDGE_FRAME_RATE_H

#include <stdint.h>

/* the original game's frames a second, which display.interpolation off keeps */
#define UE_BRIDGE_ORIGINAL_FRAME_RATE 30u
/* what the limiter assumes when the display reports no refresh rate */
#define UE_BRIDGE_DEFAULT_REFRESH_HZ 60.0f

static inline float ue_bridge_refresh_or_default(float refresh_hz)
{
	return refresh_hz > 0.0f ? refresh_hz : UE_BRIDGE_DEFAULT_REFRESH_HZ;
}

/* the rate the limiter sleeps to: 0 for none. With vsync the swap itself
paces the game and the limiter stays out of it; display.max_fps 0 is twice the
refresh rate, and a negative one is no limit. */
static inline float ue_bridge_frame_limit_rate(int vsync, long max_fps, float refresh_hz)
{
	if (vsync || max_fps < 0)
		return 0.0f;
	if (max_fps == 0)
		return 2.0f * ue_bridge_refresh_or_default(refresh_hz);
	return (float)max_fps;
}

static inline uint32_t ue_bridge_refresh_hz_published(float refresh_hz)
{
	return (uint32_t)(ue_bridge_refresh_or_default(refresh_hz) + 0.5f);
}

/* the frames a second the game aims for: 0 when nothing holds it back. Without
interpolation the game draws once per 30 Hz tick whatever the limiter allows,
so a limit below 30 is the only thing that can lower it. */
static inline uint32_t ue_bridge_frame_target_hz(int vsync, long max_fps, int interpolation, float refresh_hz)
{
	uint32_t target;

	if (vsync)
		target = ue_bridge_refresh_hz_published(refresh_hz);
	else
		target = (uint32_t)(ue_bridge_frame_limit_rate(0, max_fps, refresh_hz) + 0.5f);
	if (!interpolation && (target == 0 || target > UE_BRIDGE_ORIGINAL_FRAME_RATE))
		target = UE_BRIDGE_ORIGINAL_FRAME_RATE;
	return target;
}

#endif

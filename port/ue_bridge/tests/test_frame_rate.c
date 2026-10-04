/*
TEST_FRAME_RATE.C

The pacing rules the game's frame limiter and the bridge's published
frame-rate target share (ue_bridge_frame_rate.h).
*/

#include "ueb_test.h"
#include "ue_bridge_frame_rate.h"

static void frame_rate_limit_is_none_with_vsync_or_a_negative_maximum(void)
{
	UEB_CHECK(ue_bridge_frame_limit_rate(1, 0, 60.0f) == 0.0f);
	UEB_CHECK(ue_bridge_frame_limit_rate(1, 90, 60.0f) == 0.0f);
	UEB_CHECK(ue_bridge_frame_limit_rate(0, -1, 60.0f) == 0.0f);
}

static void frame_rate_limit_zero_maximum_is_twice_the_refresh_rate(void)
{
	UEB_CHECK(ue_bridge_frame_limit_rate(0, 0, 60.0f) == 120.0f);
	UEB_CHECK(ue_bridge_frame_limit_rate(0, 0, 144.0f) == 288.0f);
}

static void frame_rate_limit_unknown_refresh_rate_counts_as_60(void)
{
	UEB_CHECK(ue_bridge_frame_limit_rate(0, 0, 0.0f) == 120.0f);
	UEB_CHECK(ue_bridge_frame_limit_rate(0, 0, -5.0f) == 120.0f);
}

static void frame_rate_limit_positive_maximum_is_taken_as_it_is(void)
{
	UEB_CHECK(ue_bridge_frame_limit_rate(0, 90, 144.0f) == 90.0f);
	UEB_CHECK(ue_bridge_frame_limit_rate(0, 500, 60.0f) == 500.0f);
}

static void frame_rate_target_with_vsync_is_the_refresh_rate(void)
{
	UEB_CHECK(ue_bridge_frame_target_hz(1, 0, 1, 60.0f) == 60u);
	UEB_CHECK(ue_bridge_frame_target_hz(1, 30, 1, 144.0f) == 144u);
	UEB_CHECK(ue_bridge_frame_target_hz(1, -1, 1, 75.0f) == 75u);
}

static void frame_rate_target_without_vsync_follows_the_maximum(void)
{
	UEB_CHECK(ue_bridge_frame_target_hz(0, 90, 1, 144.0f) == 90u);
	UEB_CHECK(ue_bridge_frame_target_hz(0, 0, 1, 144.0f) == 288u);
	UEB_CHECK(ue_bridge_frame_target_hz(0, 0, 1, 0.0f) == 120u);
}

static void frame_rate_target_without_vsync_or_limit_is_zero(void)
{
	UEB_CHECK(ue_bridge_frame_target_hz(0, -1, 1, 144.0f) == 0u);
}

static void frame_rate_target_without_interpolation_is_the_original_30(void)
{
	UEB_CHECK(ue_bridge_frame_target_hz(1, 0, 0, 144.0f) == 30u);
	UEB_CHECK(ue_bridge_frame_target_hz(0, -1, 0, 144.0f) == 30u);
	UEB_CHECK(ue_bridge_frame_target_hz(0, 0, 0, 60.0f) == 30u);
	UEB_CHECK(ue_bridge_frame_target_hz(0, 90, 0, 60.0f) == 30u);
}

static void frame_rate_target_without_interpolation_keeps_a_lower_limit(void)
{
	UEB_CHECK(ue_bridge_frame_target_hz(0, 20, 0, 60.0f) == 20u);
	UEB_CHECK(ue_bridge_frame_target_hz(1, 0, 0, 24.0f) == 24u);
}

static void frame_rate_target_with_vsync_rounds_the_refresh_rate(void)
{
	UEB_CHECK(ue_bridge_frame_target_hz(1, 0, 1, 59.94f) == 60u);
	UEB_CHECK(ue_bridge_frame_target_hz(1, 0, 1, 143.9f) == 144u);
}

/* vsync paces the game at the display's real rate, which is unknown here: a
guessed 60 would cap UE below the game */
static void frame_rate_target_with_vsync_and_an_unknown_refresh_rate_is_uncapped(void)
{
	UEB_CHECK(ue_bridge_frame_target_hz(1, 0, 1, 0.0f) == 0u);
	UEB_CHECK(ue_bridge_frame_target_hz(1, 90, 1, -1.0f) == 0u);
	UEB_CHECK(ue_bridge_frame_target_hz(1, 0, 0, 0.0f) == 30u);
}

const struct ueb_test ueb_frame_rate_tests[] =
{
	{ "frame_rate_limit_is_none_with_vsync_or_a_negative_maximum", frame_rate_limit_is_none_with_vsync_or_a_negative_maximum },
	{ "frame_rate_limit_zero_maximum_is_twice_the_refresh_rate", frame_rate_limit_zero_maximum_is_twice_the_refresh_rate },
	{ "frame_rate_limit_unknown_refresh_rate_counts_as_60", frame_rate_limit_unknown_refresh_rate_counts_as_60 },
	{ "frame_rate_limit_positive_maximum_is_taken_as_it_is", frame_rate_limit_positive_maximum_is_taken_as_it_is },
	{ "frame_rate_target_with_vsync_is_the_refresh_rate", frame_rate_target_with_vsync_is_the_refresh_rate },
	{ "frame_rate_target_without_vsync_follows_the_maximum", frame_rate_target_without_vsync_follows_the_maximum },
	{ "frame_rate_target_without_vsync_or_limit_is_zero", frame_rate_target_without_vsync_or_limit_is_zero },
	{ "frame_rate_target_without_interpolation_is_the_original_30", frame_rate_target_without_interpolation_is_the_original_30 },
	{ "frame_rate_target_without_interpolation_keeps_a_lower_limit", frame_rate_target_without_interpolation_keeps_a_lower_limit },
	{ "frame_rate_target_with_vsync_rounds_the_refresh_rate", frame_rate_target_with_vsync_rounds_the_refresh_rate },
	{ "frame_rate_target_with_vsync_and_an_unknown_refresh_rate_is_uncapped", frame_rate_target_with_vsync_and_an_unknown_refresh_rate_is_uncapped },
	{ 0, 0 }
};

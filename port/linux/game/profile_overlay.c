/*
PROFILE_OVERLAY.C

Render the live profiling overlay.
*/

#ifdef HALO_PROFILE

/* while visible, profile_global_enable times every game section, even
without a recording: the overlay itself therefore has a profiling cost. */

#include "cseries.h"
#include "game/game.h"
#include "interface/interface.h"
#include "rasterizer/rasterizer.h"
#include "text/font_group.h"
#include "network_distributed.h"
#include "profile_overlay.h"
#include "profile_overlay_lines.h"
#include "profile_trace.h"
#include "render/render.h"

#include <string.h>

/* ---------- globals */

static boolean profile_overlay_on;
static struct profile_overlay_lines profile_overlay;
/* the overlay's lines as one string, made when they are: a frame only draws it */
static char profile_overlay_text[PROFILE_OVERLAY_LINE_COUNT * (PROFILE_OVERLAY_LINE_SIZE + 2) + 1];
/* the second's start, on profile_trace_clock (nanoseconds) */
static unsigned long long profile_overlay_second_start;

/* ---------- private code */

static void profile_overlay_make_text(
	void)
{
	profile_overlay_lines_text(&profile_overlay, profile_overlay_text);
}

static void profile_overlay_second(
	unsigned long long now)
{
	struct profile_trace_status status;
	struct profile_net_live live;
	char const *role = "local";
	int clients = 0;

	profile_trace_status(&status);
	profile_net_live(&live);
	switch (game_connection())
	{
	case _game_connection_network_server:
	{
		long machine_indices[HALO_PORT_MAXIMUM_NETWORK_MACHINES];

		role = "host";
		clients = distributed_client_machines(machine_indices, HALO_PORT_MAXIMUM_NETWORK_MACHINES);
		break;
	}
	case _game_connection_network_client:
		role = "client";
		break;
	default:
		break;
	}
	profile_overlay_lines_second(&profile_overlay, &status, role, clients, &live,
		(double)(now - profile_overlay_second_start) / 1000000000.0);
	profile_overlay_second_start = now;
	profile_overlay_make_text();
}

/* ---------- public code */

void profile_overlay_toggle(
	boolean on)
{
	if (on && !profile_overlay_on)
	{
		profile_overlay_lines_reset(&profile_overlay);
		profile_overlay_make_text();
		profile_overlay_second_start = profile_trace_clock();
	}
	profile_overlay_on = on;
	profile_net_set_overlay(on);
}

boolean profile_overlay_visible(
	void)
{
	return profile_overlay_on;
}

void profile_overlay_note_frame(
	double frame_ms,
	int ticks,
	const double *tick_ms,
	double network_ms)
{
	profile_overlay_lines_frame(&profile_overlay, frame_ms, ticks, tick_ms, network_ms);
}

void profile_overlay_note_section(
	const char *name,
	double ms)
{
	profile_overlay_lines_section(&profile_overlay, name, ms);
}

void profile_overlay_render(
	void)
{
	unsigned long long now;
	rectangle2d bounds;
	struct font_header *font;
	short line_height;
	int line_count;

	if (!profile_overlay_on)
		return;
	now = profile_trace_clock();
	if (now - profile_overlay_second_start >= 1000000000ULL)
		profile_overlay_second(now);
	bounds = render.camera.window_bounds;
	font = font_definition_get(interface_get_tag_index(_interface_font_terminal));
	line_height = font->ascending_height + font->descending_height + font->leading_height;
	line_count = PROFILE_OVERLAY_LINE_COUNT - 1 +
		(profile_overlay.lines[PROFILE_OVERLAY_CONTINUATION_LINE][0] != 0);
	bounds.y0 = bounds.y1 - line_count * line_height;
	interface_set_bitmap_text_draw_mode(_interface_font_terminal, NONE, 1 /* right */, 0, _interface_color_table_dialog, 0);
	rasterizer_draw_string(&bounds, NULL, NULL, 0, profile_overlay_text);
}

#endif

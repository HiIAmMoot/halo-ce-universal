/*
PROFILE_OVERLAY_LINES.H

Expose formatting for profiling overlay lines.
*/

#ifndef __PROFILE_OVERLAY_LINES_H
#define __PROFILE_OVERLAY_LINES_H

#ifdef HALO_PROFILE

#include "profile_trace.h"
#include "profile_net.h"

/* ---------- constants */

enum
{
	PROFILE_OVERLAY_LINE_COUNT = 7,
	PROFILE_OVERLAY_LINE_SIZE = 96,
	PROFILE_OVERLAY_LINE_LIMIT = 56,
	PROFILE_OVERLAY_SLOW_LINE_LIMIT = PROFILE_OVERLAY_LINE_LIMIT,
	PROFILE_OVERLAY_CONTINUATION_LINE = 4,
	PROFILE_OVERLAY_SECTIONS = 48,
};

/* what profile_overlay's word asks for (profile_overlay_switch) */
enum profile_overlay_switch
{
	_profile_overlay_switch_toggle = 0,
	_profile_overlay_switch_on,
	_profile_overlay_switch_off,
	_profile_overlay_switch_invalid,
};

/* ---------- structures */

struct profile_overlay_lines
{
	/* the second so far */
	long frames;
	double frame_ms_total;
	double frame_ms_maximum;
	long ticks;
	double tick_ms_total;
	double tick_ms_maximum;
	struct
	{
		const char *name;
		double ms;
	} sections[PROFILE_OVERLAY_SECTIONS];
	int section_count;
	/* the network totals at the second's start */
	struct profile_net_live previous;
	int previous_valid;
	/* what the overlay shows, made once a second */
	char lines[PROFILE_OVERLAY_LINE_COUNT][PROFILE_OVERLAY_LINE_SIZE];
};

/* ---------- prototypes/PROFILE_OVERLAY_LINES.C */

/* the console's word after profile_overlay, as the console gives it (past the
spaces, perhaps before a closing parenthesis): nothing, on or off */
int profile_overlay_switch(const char *word);
void profile_overlay_lines_reset(struct profile_overlay_lines *overlay);
void profile_overlay_lines_text(const struct profile_overlay_lines *overlay, char *text);
/* a frame: its time, and each of its ticks' (game_tick, with its share of
network_distributed_tick's time in the frame) */
void profile_overlay_lines_frame(struct profile_overlay_lines *overlay, double frame_ms, int ticks,
	const double *tick_ms, double network_ms);
/* a section's time in the frame (only the overlay's own: game_tick.* and
network_distributed_tick) */
void profile_overlay_lines_section(struct profile_overlay_lines *overlay, const char *name, double ms);
/* the second's end: the lines, then a new second */
void profile_overlay_lines_second(struct profile_overlay_lines *overlay, const struct profile_trace_status *status,
	const char *role, int clients, const struct profile_net_live *live, double seconds);

#endif

#endif

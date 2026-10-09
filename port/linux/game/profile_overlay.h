/*
PROFILE_OVERLAY.H

Expose profiling overlay controls and frame feeds.
*/

#ifndef __PROFILE_OVERLAY_H
#define __PROFILE_OVERLAY_H
#pragma once

#ifdef HALO_PROFILE

void profile_overlay_toggle(boolean on);
boolean profile_overlay_visible(void);
/* profile.c, each frame: the last frame's time and its ticks', and the
sections' time in it */
void profile_overlay_note_frame(double frame_ms, int ticks, const double *tick_ms, double network_ms);
void profile_overlay_note_section(const char *name, double ms);
/* interface_draw_fullscreen_overlays, after render_debug_profile */
void profile_overlay_render(void);

#endif

#endif

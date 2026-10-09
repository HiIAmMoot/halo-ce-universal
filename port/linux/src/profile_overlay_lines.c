/*
PROFILE_OVERLAY_LINES.C

Format live CPU and network measurements for the overlay.
*/

#ifdef HALO_PROFILE

#include "profile_overlay_lines.h"

#include <stdio.h>
#include <string.h>

/* ---------- private code */

static int profile_overlay_wire_active(
	const struct profile_net_live *live,
	const struct profile_net_live *previous)
{
	return live->wire_packets[0] != previous->wire_packets[0] || live->wire_packets[1] != previous->wire_packets[1];
}

/* ---------- public code */

int profile_overlay_switch(
	const char *word)
{
	static const char *const words[] = { "on", "off" };
	int which;

	/* (the console gives ")" for "(profile_overlay)") */
	while (*word == ' ' || *word == '\t' || *word == ')')
		word++;
	if (!*word)
		return _profile_overlay_switch_toggle;
	for (which = 0; which < 2; which++)
	{
		const char *letter = words[which];
		int index = 0;

		while (letter[index] && (word[index] | 0x20) == letter[index])
			index++;
		if (letter[index])
			continue;
		word += index;
		while (*word == ' ' || *word == '\t' || *word == ')')
			word++;
		return *word ? _profile_overlay_switch_invalid : which == 0 ? _profile_overlay_switch_on : _profile_overlay_switch_off;
	}
	return _profile_overlay_switch_invalid;
}

void profile_overlay_lines_reset(
	struct profile_overlay_lines *overlay)
{
	memset(overlay, 0, sizeof(*overlay));
	strcpy(overlay->lines[0], "profile idle");
}

void profile_overlay_lines_text(
	const struct profile_overlay_lines *overlay,
	char *text)
{
	int line;

	text[0] = 0;
	for (line = 0; line < PROFILE_OVERLAY_LINE_COUNT; line++)
	{
		if (line == PROFILE_OVERLAY_CONTINUATION_LINE && !overlay->lines[line][0])
			continue;
		strcat(text, overlay->lines[line]);
		strcat(text, "|n");
	}
}

void profile_overlay_lines_frame(
	struct profile_overlay_lines *overlay,
	double frame_ms,
	int ticks,
	const double *tick_ms,
	double network_ms)
{
	int tick;

	overlay->frames++;
	overlay->frame_ms_total += frame_ms;
	if (frame_ms > overlay->frame_ms_maximum)
		overlay->frame_ms_maximum = frame_ms;
	for (tick = 0; tick < ticks; tick++)
	{
		double ms = tick_ms[tick] + network_ms / ticks;

		overlay->ticks++;
		overlay->tick_ms_total += ms;
		if (ms > overlay->tick_ms_maximum)
			overlay->tick_ms_maximum = ms;
	}
}

void profile_overlay_lines_section(
	struct profile_overlay_lines *overlay,
	const char *name,
	double ms)
{
	int index;

	if (strncmp(name, "game_tick.", 10) != 0 && strcmp(name, "network_distributed_tick") != 0)
		return;
	for (index = 0; index < overlay->section_count; index++)
	{
		if (strcmp(overlay->sections[index].name, name) == 0)
		{
			overlay->sections[index].ms += ms;
			return;
		}
	}
	if (overlay->section_count < PROFILE_OVERLAY_SECTIONS)
	{
		overlay->sections[overlay->section_count].name = name;
		overlay->sections[overlay->section_count].ms = ms;
		overlay->section_count++;
	}
}

void profile_overlay_lines_second(
	struct profile_overlay_lines *overlay,
	const struct profile_trace_status *status,
	const char *role,
	int clients,
	const struct profile_net_live *live,
	double seconds)
{
	const struct profile_net_live *previous = overlay->previous_valid ? &overlay->previous : live;
	double rates[4], packets[2];
	char *line;
	int slowest[3] = { -1, -1, -1 };
	int index, slot, wire, continued = 0;

	switch (status->state)
	{
	case _profile_trace_recording:
		snprintf(overlay->lines[0], PROFILE_OVERLAY_LINE_SIZE, "REC %02d:%02d part %ld  mem %d%%",
			(int)status->seconds / 60, (int)status->seconds % 60, status->part, status->memory_percent);
		break;
	case _profile_trace_armed:
		strcpy(overlay->lines[0], "profile armed");
		break;
	case _profile_trace_finishing:
		snprintf(overlay->lines[0], PROFILE_OVERLAY_LINE_SIZE, "profile writing %s", status->name);
		break;
	default:
		strcpy(overlay->lines[0], "profile idle");
		break;
	}
	if (strcmp(role, "host") == 0)
		snprintf(overlay->lines[1], PROFILE_OVERLAY_LINE_SIZE, "host  %d clients", clients);
	else
		snprintf(overlay->lines[1], PROFILE_OVERLAY_LINE_SIZE, "%s", role);

	line = overlay->lines[2];
	snprintf(line, PROFILE_OVERLAY_LINE_SIZE, "frame %.1f ms avg %.1f max   ",
		overlay->frames ? overlay->frame_ms_total / overlay->frames : 0.0, overlay->frame_ms_maximum);
	if (overlay->ticks)
	{
		snprintf(line + strlen(line), PROFILE_OVERLAY_LINE_SIZE - strlen(line), "tick %.1f ms avg %.1f max",
			overlay->tick_ms_total / overlay->ticks, overlay->tick_ms_maximum);
	}
	else
	{
		snprintf(line + strlen(line), PROFILE_OVERLAY_LINE_SIZE - strlen(line), "tick --");
	}

	/* the three sections with the most time per tick */
	for (index = 0; index < overlay->section_count; index++)
	{
		for (slot = 0; slot < 3; slot++)
		{
			if (slowest[slot] < 0 || overlay->sections[index].ms > overlay->sections[slowest[slot]].ms)
			{
				memmove(&slowest[slot + 1], &slowest[slot], sizeof(slowest[0]) * (size_t)(2 - slot));
				slowest[slot] = index;
				break;
			}
		}
	}
	line = overlay->lines[3];
	overlay->lines[PROFILE_OVERLAY_CONTINUATION_LINE][0] = 0;
	strcpy(line, "tick slow:");
	for (slot = 0; slot < 3 && overlay->ticks; slot++)
	{
		const char *name;
		size_t name_length;
		char section[PROFILE_OVERLAY_LINE_SIZE];
		const char *separator;
		size_t line_length;

		if (slowest[slot] < 0)
			break;
		name = overlay->sections[slowest[slot]].name;
		if (strncmp(name, "game_tick.", 10) == 0)
			name += 10;
		name_length = strlen(name);
		if (name_length >= 5 && strcmp(name + name_length - 5, "_tick") == 0)
			name_length -= 5;
		snprintf(section, sizeof(section), "%.*s %.1f", (int)name_length, name,
			overlay->sections[slowest[slot]].ms / overlay->ticks);
		separator = slot ? "  " : " ";
		line_length = strlen(line);
		if (!continued && line_length + strlen(separator) + strlen(section) > PROFILE_OVERLAY_LINE_LIMIT)
		{
			line = overlay->lines[PROFILE_OVERLAY_CONTINUATION_LINE];
			continued = 1;
			separator = "";
		}
		else if (continued)
			separator = "  ";
		line_length = strlen(line);
		if (line_length + strlen(separator) + strlen(section) > PROFILE_OVERLAY_LINE_LIMIT)
			break;
		snprintf(line + strlen(line), PROFILE_OVERLAY_LINE_SIZE - strlen(line), "%s%s", separator, section);
	}
	if (!overlay->ticks || slowest[0] < 0)
		strcpy(line, "tick slow: --");

	if (seconds <= 0.0)
		seconds = 1.0;
	rates[0] = (double)(live->game_bytes[0] - previous->game_bytes[0]) / seconds / 1000.0;
	rates[1] = (double)(live->game_bytes[1] - previous->game_bytes[1]) / seconds / 1000.0;
	rates[2] = (double)(live->wire_bytes[0] - previous->wire_bytes[0]) / seconds / 1000.0;
	rates[3] = (double)(live->wire_bytes[1] - previous->wire_bytes[1]) / seconds / 1000.0;
	packets[0] = (double)(live->game_packets[0] - previous->game_packets[0]) / seconds;
	packets[1] = (double)(live->game_packets[1] - previous->game_packets[1]) / seconds;
	wire = profile_overlay_wire_active(live, previous);
	if (wire)
	{
		snprintf(overlay->lines[5], PROFILE_OVERLAY_LINE_SIZE, "up %.1f KB/s (wire %.1f)  down %.1f KB/s (wire %.1f)",
			rates[0], rates[2], rates[1], rates[3]);
		snprintf(overlay->lines[6], PROFILE_OVERLAY_LINE_SIZE, "ping %ld ms (wire %ld)  loss %.1f%%  packets %.0f up %.0f down",
			live->ping_ms, live->wire_round_trip_ms, live->loss_percent, packets[0], packets[1]);
	}
	else
	{
		snprintf(overlay->lines[5], PROFILE_OVERLAY_LINE_SIZE, "up %.1f KB/s  down %.1f KB/s", rates[0], rates[1]);
		snprintf(overlay->lines[6], PROFILE_OVERLAY_LINE_SIZE, "ping %ld ms  loss 0.0%%  packets %.0f up %.0f down",
			live->ping_ms, packets[0], packets[1]);
	}
	/* (a value too wide for the gap, such as a seconds-long hitch, loses the
	line's tail rather than running into the motion tracker at 4:3) */
	for (index = 0; index < PROFILE_OVERLAY_LINE_COUNT; index++)
		overlay->lines[index][PROFILE_OVERLAY_LINE_LIMIT] = 0;

	overlay->previous = *live;
	overlay->previous_valid = 1;
	overlay->frames = 0;
	overlay->frame_ms_total = overlay->frame_ms_maximum = 0.0;
	overlay->ticks = 0;
	overlay->tick_ms_total = overlay->tick_ms_maximum = 0.0;
	overlay->section_count = 0;
}

#endif

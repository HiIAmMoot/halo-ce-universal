/*
UE_BRIDGE_LOG.C

The UE bridge's messages. platform_log is stderr, which a windowed game never
shows, so a bridge line must also go through the game's own log (errors.c's
write_to_error_file, debug.txt with its timestamp) for a player, and for the
renderer's session report, to see the game's side of a session.
*/

#include "ue_bridge_platform.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* platform.h's and errors.c's, declared here so the tests can link this file without either */
void platform_log(const char *format, ...);
void write_to_error_file(char *string, unsigned char date);

/* Callable from the watcher thread: the game's log opens its file once even
if threads race to, and writes each line in one call. Never from the crash
filter: the filter must stay on the paths it already had (the game's own
filter, whose lines are the game's), since this formats into a buffer and
takes the CRT's file lock. */
void ue_bridge_log(const char *format, ...)
{
	/* the game's lines end in CRLF (errors.c's error()) */
	char line[512];
	va_list arguments;
	size_t length;

	va_start(arguments, format);
	vsnprintf(line, sizeof(line) - 2, format, arguments);
	va_end(arguments);
	platform_log("%s", line);
	length = strlen(line);
	line[length] = '\r';
	line[length + 1] = '\n';
	line[length + 2] = 0;
	write_to_error_file(line, 1);
}

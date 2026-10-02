/*
UE_BRIDGE_QUIT.C

How the UE bridge's watcher thread quits the game: the quit event closing the
window sends (sdl_platform.c), so the game leaves through its normal exit and
its exit handlers.
*/

#include <SDL3/SDL.h>

#include "ue_bridge_platform.h"

void ue_bridge_request_quit(void)
{
	SDL_Event event;

	SDL_zero(event);
	event.type = SDL_EVENT_QUIT;
	SDL_PushEvent(&event);
}

// The program icon for the window.  On Windows the executables carry it as a resource
// (port/res/720degrees.rc), which SDL uses by itself; elsewhere it is set from icon_rgba.h.
#pragma once
#include <SDL3/SDL.h>
#ifndef _WIN32
#include "icon_rgba.h"
#endif

inline void set_window_icon(SDL_Window *window)
{
#ifndef _WIN32
	if (SDL_Surface *icon = SDL_CreateSurfaceFrom(ICON_W, ICON_H, SDL_PIXELFORMAT_RGBA32, (void *)ICON_RGBA, ICON_W * 4))
	{
		SDL_SetWindowIcon(window, icon);
		SDL_DestroySurface(icon);
	}
#else
	(void)window;
#endif
}

/* Window and its renderer (SDL3, gfx/rhi.h: an OpenGL context or a Direct3D device). Owned by the main/render
   thread. */
#ifndef NUWORLD_PLATFORM_PLATFORM_H
#define NUWORLD_PLATFORM_PLATFORM_H

#include <stdbool.h>

#include <SDL3/SDL.h>

typedef struct NwPlatform {
    SDL_Window *window;
    SDL_GLContext gl; /* the OpenGL renderer's context, NULL under the others */
    bool rhi;         /* the renderer runs on the window */
} NwPlatform;

bool nw_platform_init(NwPlatform *p, const char *title, int width, int height);
void nw_platform_shutdown(NwPlatform *p);

#endif

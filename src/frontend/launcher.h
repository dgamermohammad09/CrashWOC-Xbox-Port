/* The launcher: before the game starts, every setting and debug switch of the F1 overlay on one full-screen page in
   the colours of Crash Bandicoot: The Wrath of Cortex (the orange of Crash and the Wumpa fruit, the purple of
   Cortex's vortex), for mouse, keyboard and a controller (the Xbox has nothing else). Main thread only. */
#ifndef NUWORLD_FRONTEND_LAUNCHER_H
#define NUWORLD_FRONTEND_LAUNCHER_H

#include <SDL3/SDL.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum NwLauncherResult {
    NW_LAUNCHER_PLAY,
    NW_LAUNCHER_QUIT,
} NwLauncherResult;

/* Called with every event, then once a frame with NULL: applies what the player changed (the window, the volume). */
typedef void (*NwLauncherTick)(void *user, const SDL_Event *event);

/* Needs Dear ImGui up (nw_debug_init) and the items registered; returns when Play or Quit is chosen or the window
   is closed. */
NwLauncherResult nw_launcher_run(SDL_Window *window, const char *version, NwLauncherTick tick, void *user);

/* The animated backdrop (vortex, Wumpa fruit) into the current ImGui frame's background, also behind the first-run
   wizard. */
void nw_launcher_backdrop(void);

#ifdef __cplusplus
}
#endif

#endif

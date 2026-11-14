#ifndef NUWORLD_PLATFORM_TESTPAD_H
#define NUWORLD_PLATFORM_TESTPAD_H

#include <SDL3/SDL_video.h>

/* Test hook: with NW_TEST_PAD=<script> a virtual controller plays the script (see testpad.c). Once, after SDL_Init. */
#ifdef __cplusplus
extern "C" {
#endif
void nw_testpad_start(void);
/* Each loop calls it before presenting: saves the frame when the script asked for a shot. */
void nw_testpad_frame(SDL_Window *window);
#ifdef __cplusplus
}
#endif

#endif

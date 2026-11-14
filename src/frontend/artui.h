/* Drawing shared by the launcher, the first-time setup and the F1 overlay (Dear ImGui, C++ only): the palette, the
   backdrop, the game's 3D font from the disc (frontend/gameart.h) with a drawn fallback, its images, the metal
   monitor panel of the pause menu, and controller / keyboard button prompts (assets/prompts). Main thread only. */
#ifndef NUWORLD_FRONTEND_ARTUI_H
#define NUWORLD_FRONTEND_ARTUI_H

#include <SDL3/SDL.h>

#include "imgui.h"

extern "C" {
#include "frontend/gameart.h"
}

/* The palette: Crash's orange, the Wumpa fruit, the menu's gold, the night sky of the title screen. */
enum : unsigned {
    NW_C_ORANGE = 0xff8a1c,
    NW_C_ORANGE_HI = 0xffb340,
    NW_C_ORANGE_DEEP = 0xd9530a,
    NW_C_GOLD = 0xffd23f,
    NW_C_CREAM = 0xfff4e0,
    NW_C_MUTED = 0xb8b2d4,
    NW_C_INK = 0x1a0c04,
    NW_C_NIGHT = 0x070a1f,
    NW_C_NIGHT_HI = 0x1d1146,
    NW_C_PANEL = 0x0f1430,
    NW_C_PANEL_HI = 0x1b2350,
    NW_C_LINE = 0x39427a,
    NW_C_GREEN = 0x5fd35a,
    NW_C_RED = 0xff5a4a,
};

ImU32 nw_c(unsigned rgb, float a = 1.0f);
ImVec4 nw_cv(unsigned rgb, float a = 1.0f);

/* Night sky, stars and the title's turning shards over the whole viewport (the background draw list). */
void nw_art_backdrop(float t, float shards);

/* Text in the game's letters (capitals): h is the letters' cell height in pixels; mono uses Text3D's even spacing.
   Without the font: the UI's bold face in the ink's gradient with a dark outline. Returns the width. */
float nw_art_text(ImDrawList *dl, ImVec2 p, float h, NwArtInk ink, const char *text, float alpha = 1.0f,
                  bool mono = false);
float nw_art_text_width(float h, const char *text, bool mono = false);

/* An image of the disc (frame of its sheet) into a..b; false when the disc has none. */
bool nw_art_image(ImDrawList *dl, NwArtId id, int frame, ImVec2 a, ImVec2 b, ImU32 tint = IM_COL32_WHITE);

/* The pause menu's metal monitor around a..b (the disc's PAUSE.GSC texture, nine-sliced; a drawn steel frame
   without it). header / footer: the dark slots' heights; *screen gets the screen's rectangle. */
void nw_art_panel(ImDrawList *dl, ImVec2 a, ImVec2 b, float s, float header, float footer, ImVec2 *screen0,
                  ImVec2 *screen1, ImVec2 *head0 = nullptr, ImVec2 *head1 = nullptr, ImVec2 *foot0 = nullptr,
                  ImVec2 *foot1 = nullptr);

/* A glossy card (setup, overlay): rounded, a lit top edge, a soft shadow. */
void nw_art_card(ImDrawList *dl, ImVec2 a, ImVec2 b, float s, bool lit = false);

/* Button prompts: the icon (assets/prompts name, "xbox_button_color_a", "keyboard_enter"...) and a label; returns the
   width taken. pad picks the controller's icons. */
typedef struct NwArtPrompt {
    const char *pad, *key, *label;
} NwArtPrompt;
float nw_art_prompts(ImDrawList *dl, ImVec2 p, float h, const NwArtPrompt *prompts, int count, bool pad, bool right);
/* The last device used: true for a controller. Follows nw_art_input(). */
bool nw_art_pad_used(void);
void nw_art_input(const SDL_Event *e);

/* Buttons of the setup and the overlay: kind 0 a dark one, 1 the orange main action. Focused (controller,
   keyboard) or hovered they get a gold rim. Drawn at the cursor, size in pixels; true when pressed. */
bool nw_art_button(const char *label, ImVec2 size, int kind = 0);
/* A progress bar at the cursor; frac < 0: moving stripes (no total yet). */
void nw_art_progress(ImVec2 size, float frac, const char *overlay);
/* The setup's steps (1 Find disc, 2 Import, 3 Play) centred on cx; step 0-2 is the current one. */
void nw_art_steps(ImDrawList *dl, float cx, float y, float width, float h, int step);

/* The UI's name: NW_APP_NAME, " - Xbox Port Beta" on the Xbox build. */
const char *nw_art_app_title(void);

void nw_art_shutdown(void);

#endif

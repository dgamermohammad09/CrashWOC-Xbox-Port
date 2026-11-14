/* The game's own art for the launcher, read at run time from the imported disc (nothing of it is in the repository or
   the package): the title logo (LEVELS\B\LOGOS\TITLE.GHG), crates, the Wumpa fruit and the heads of STUFF\*.GSC
   drawn by a small software rasteriser, the 3D font of STUFF\FONT.GSC as a glyph atlas in its colours, the pause
   menu's metal panels (STUFF\PAUSE.GSC) and the menu sounds of SFX.DAT. Without a disc nothing is loaded and the
   callers draw their own fallbacks. Main thread only. */
#ifndef NUWORLD_FRONTEND_GAMEART_H
#define NUWORLD_FRONTEND_GAMEART_H

#include <stdbool.h>
#include <stdint.h>

#include "gfx/rhi.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum NwArtId {
    NW_ART_LOGO,        /* the 3D title logo, front view */
    NW_ART_WUMPA,       /* a Wumpa fruit turning: frames */
    NW_ART_CRATE,       /* the wooden crate */
    NW_ART_CRATE_Q,     /* the ? crate */
    NW_ART_CRATE_AKU,   /* the Aku Aku crate */
    NW_ART_CRATE_NITRO,
    NW_ART_CRATE_TNT,
    NW_ART_CRATE_CHECK, /* the checkpoint crate */
    NW_ART_CRASH,       /* Crash's head (the HUD's) */
    NW_ART_COCO,        /* Coco's head */
    NW_ART_PANEL,       /* the pause menu's metal monitor, as the texture */
    NW_ART_PANEL2,      /* its second panel, with the green lights */
    NW_ART_COUNT,
} NwArtId;

typedef struct NwArtImage {
    NwRhiTexture *tex; /* NULL: not on this disc / not loaded */
    int w, h;          /* of one frame, texels */
    int frames, cols;  /* frames laid out left to right, cols per row */
} NwArtImage;

/* The font's colours: Text3D's materials over the letters' faces. */
typedef enum NwArtInk {
    NW_INK_GOLD,   /* the menus' letters */
    NW_INK_ORANGE, /* the chosen item flashes orange and blue (the game's DrawMenuEntry) */
    NW_INK_BLUE,
    NW_INK_GREEN,
    NW_INK_COUNT,
} NwArtInk;

typedef struct NwArtGlyph {
    float u0, v0, u1, v1; /* in the atlas, 0..1 */
    float w;              /* its width over the cell's height */
} NwArtGlyph;

typedef struct NwArtFont {
    NwRhiTexture *tex;
    float advance; /* Text3D's monospaced step over the cell's height */
    NwArtGlyph glyphs[NW_INK_COUNT][128];
    bool have[128]; /* the glyph exists (capitals, digits, some punctuation) */
} NwArtFont;

typedef enum NwArtSound {
    NW_SOUND_MOVE,   /* MenuMove: GENERIC\CLET_BP */
    NW_SOUND_SELECT, /* MenuSelect: GENERIC\SEL_BP */
    NW_SOUND_BACK,   /* MenuBack: GENERIC\UDLT_BP */
    NW_SOUND_COUNT,
} NwArtSound;

/* Reads root's imported disc, takes a few hundred ms; false without one. Loads once; nw_gameart_free drops it. */
bool nw_gameart_load(const char *root);
void nw_gameart_free(void);
bool nw_gameart_loaded(void);

const NwArtImage *nw_gameart_image(NwArtId id);
/* NULL without the font. */
const NwArtFont *nw_gameart_font(void);

/* The menu sounds through the platform's audio device: open it for the launcher, close it before the game opens its
   own (nw_audio_open). Without the sounds or the device play does nothing. */
bool nw_gameart_audio_open(void);
void nw_gameart_audio_close(void);
void nw_gameart_sound(NwArtSound s);

/* --dump-art <dir>: every texture of the front-end files as PNG, every special object and model rendered, a list,
   and what nw_gameart_load makes. */
int nw_gameart_dump(const char *root, const char *dir);

#ifdef __cplusplus
}
#endif

#endif

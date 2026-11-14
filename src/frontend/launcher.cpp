#include "frontend/launcher.h"

#include <math.h>

#include "imgui.h"
#include "imgui_internal.h"

#include "app.h"
#include "frontend/artui.h"
#include "frontend/ui.h"

extern "C" {
#include "frontend/debug.h"
#include "gfx/rhi.h"
#include "platform/testpad.h"
}

#define NW_LAUNCHER_MENUS 16
#define NW_LAUNCHER_CATS 24
#define NW_LAUNCHER_FADE 0.28 /* seconds of a screen's slide-in */

typedef struct NwLauncherMenu {
    char name[48];  /* the registered top-level menu */
    char label[48]; /* shown, in capitals */
    unsigned items; /* what it shows before the game starts */
} NwLauncherMenu;

/* A menu's page on the right: one of its panels, or its loose switches and actions together (panel -1). */
typedef struct NwLauncherCat {
    char label[48];
    int panel;
} NwLauncherCat;

/* Before the game: the Debug menu's switches only (its panels and actions work on the running game), no Scene. */
static bool nw_launcher_shows(const char *top, NwDebugItemKind kind)
{
    if (SDL_strcmp(top, "Scene") == 0) {
        return false;
    }
    return SDL_strcmp(top, "Debug") != 0 || kind == NW_DEBUG_ITEM_TOGGLE;
}

static void nw_launcher_top(const char *menu, char *out, size_t size)
{
    const char *slash = SDL_strchr(menu, '/');
    size_t n = slash ? (size_t)(slash - menu) : SDL_strlen(menu);
    SDL_strlcpy(out, menu, SDL_min(n + 1, size));
}

static void nw_launcher_upper(char *out, size_t size, const char *in)
{
    size_t c = 0;
    for (; in[c] && c + 1 < size; c++) {
        out[c] = (char)SDL_toupper((unsigned char)in[c]);
    }
    out[c] = '\0';
}

static int nw_launcher_menus(NwLauncherMenu *menus)
{
    int count = 0;
    NwDebugItemKind kind;
    const char *menu, *label;
    for (unsigned i = 0; nw_debug_item(i, &kind, &menu, &label); i++) {
        char top[48];
        nw_launcher_top(menu, top, sizeof(top));
        if (!nw_launcher_shows(top, kind)) {
            continue;
        }
        int m = 0;
        while (m < count && SDL_strcmp(menus[m].name, top) != 0) {
            m++;
        }
        if (m == count) {
            if (count == NW_LAUNCHER_MENUS) {
                continue;
            }
            SDL_zero(menus[count]);
            SDL_strlcpy(menus[count].name, top, sizeof(menus[count].name));
            nw_launcher_upper(menus[count].label, sizeof(menus[count].label), top);
            count++;
        }
        menus[m].items++;
    }
    return count;
}

/* The pages of menu m: its panels in order, the loose items as GENERAL where the first of them was registered. */
static int nw_launcher_cats(const NwLauncherMenu *m, NwLauncherCat *cats)
{
    int count = 0;
    bool loose = false;
    NwDebugItemKind kind;
    const char *menu, *label;
    for (unsigned i = 0; nw_debug_item(i, &kind, &menu, &label) && count < NW_LAUNCHER_CATS; i++) {
        char top[48];
        nw_launcher_top(menu, top, sizeof(top));
        if (SDL_strcmp(top, m->name) != 0 || !nw_launcher_shows(top, kind)) {
            continue;
        }
        if (kind == NW_DEBUG_ITEM_PANEL) {
            nw_launcher_upper(cats[count].label, sizeof(cats[count].label), label);
            cats[count++].panel = (int)i;
        } else if (!loose) {
            loose = true;
            SDL_strlcpy(cats[count].label, "GENERAL", sizeof(cats[count].label));
            cats[count++].panel = -1;
        }
    }
    return count;
}

static void nw_launcher_page(const NwLauncherMenu *m, const NwLauncherCat *cat)
{
    if (cat->panel >= 0) {
        nw_debug_item_draw((unsigned)cat->panel);
        return;
    }
    NwDebugItemKind kind;
    const char *menu, *label;
    for (unsigned i = 0; nw_debug_item(i, &kind, &menu, &label); i++) {
        char top[48];
        nw_launcher_top(menu, top, sizeof(top));
        if (kind != NW_DEBUG_ITEM_PANEL && SDL_strcmp(top, m->name) == 0 && nw_launcher_shows(top, kind)) {
            nw_debug_item_draw(i);
        }
    }
}

/* Test hook: NW_LAUNCHER_SHOT=<png> saves the page a second in, NW_LAUNCHER_PAGE=<n> opens menu n first (unset: the
   main menu), NW_LAUNCHER_CAT=<k> its page k, NW_LAUNCHER_FOCUS=1 with the first option focused, NW_LAUNCHER_EXIT=1
   quits after the shot (the Xbox build cannot be screenshotted from outside). */
static void nw_launcher_shot(SDL_Window *window, const char *path)
{
    int w, h;
    SDL_GetWindowSizeInPixels(window, &w, &h);
    SDL_Surface *s = SDL_CreateSurface(w, h, SDL_PIXELFORMAT_ABGR8888);
    if (!s) {
        return;
    }
    bool ok = nw_rhi_window_read(w, h, s->pixels, s->pitch) && SDL_SavePNG(s, path);
    SDL_Log("launcher: shot %s (%s): %s", path, nw_rhi_kind_name(nw_rhi_kind()), ok ? "saved" : SDL_GetError());
    SDL_DestroySurface(s);
}

/* Keys and buttons the screens read themselves (ImGui's navigation only runs inside the options). */
static bool nw_key(ImGuiKey a, ImGuiKey b, ImGuiKey c = ImGuiKey_None, bool repeat = true)
{
    return ImGui::IsKeyPressed(a, repeat) || ImGui::IsKeyPressed(b, repeat) || (c != ImGuiKey_None && ImGui::IsKeyPressed(c, repeat));
}

static bool nw_key_up(void)
{
    return nw_key(ImGuiKey_UpArrow, ImGuiKey_GamepadDpadUp, ImGuiKey_GamepadLStickUp);
}

static bool nw_key_down(void)
{
    return nw_key(ImGuiKey_DownArrow, ImGuiKey_GamepadDpadDown, ImGuiKey_GamepadLStickDown);
}

static bool nw_key_accept(void)
{
    return nw_key(ImGuiKey_Enter, ImGuiKey_KeypadEnter, ImGuiKey_GamepadFaceDown, false) ||
           ImGui::IsKeyPressed(ImGuiKey_Space, false);
}

static bool nw_key_back(void)
{
    return nw_key(ImGuiKey_Escape, ImGuiKey_Backspace, ImGuiKey_GamepadFaceRight, false);
}

static float nw_ease(float x)
{
    x = SDL_clamp(x, 0.0f, 1.0f);
    return 1.0f - (1.0f - x) * (1.0f - x) * (1.0f - x);
}

typedef struct NwLauncher {
    NwLauncherMenu menus[NW_LAUNCHER_MENUS];
    int nmenus;
    int screen;   /* 0 the main menu, 1 a menu's options */
    int main_sel; /* 0 play, 1..nmenus the menus, nmenus + 1 quit */
    int menu;
    NwLauncherCat cats[NW_LAUNCHER_CATS];
    int ncats, cat;
    int zone;        /* on the options screen: 0 the pages, 1 the options */
    bool focus_opts; /* focus the first option next frame */
    double since;    /* when the screen or page changed */
    const char *desc;
    char desc_buf[512];
} NwLauncher;

static void nw_launcher_open(NwLauncher *l, int menu)
{
    l->menu = menu;
    l->ncats = nw_launcher_cats(&l->menus[menu], l->cats);
    l->cat = 0;
    l->zone = 0;
    l->screen = 1;
    l->since = ImGui::GetTime();
}

/* The chosen item's letters as the game's menus flash them (DrawMenuEntry): orange and blue, 5 frames each. */
static NwArtInk nw_launcher_flash(float t)
{
    return fmodf(t * 6.0f, 1.0f) < 0.5f ? NW_INK_ORANGE : NW_INK_BLUE;
}

/* The wumpa fruit cursor, turning; a drawn fruit without the disc's. */
static void nw_launcher_wumpa(ImDrawList *dl, ImVec2 c, float r, float t)
{
    if (!nw_art_image(dl, NW_ART_WUMPA, (int)(t * 18.0f), ImVec2(c.x - r, c.y - r), ImVec2(c.x + r, c.y + r))) {
        dl->AddCircleFilled(c, r * 0.7f, nw_c(0xff5a10), 24);
        dl->AddCircleFilled(ImVec2(c.x - r * 0.2f, c.y - r * 0.2f), r * 0.35f, nw_c(0xffb060, 0.8f), 16);
    }
}

static int nw_launcher_main(NwLauncher *l, float W, float H, ImVec2 o, float s, float t)
{
    ImDrawList *dl = ImGui::GetForegroundDrawList();
    int items = l->nmenus + 2, result = -1;
    float in = nw_ease((float)(ImGui::GetTime() - l->since) / (float)NW_LAUNCHER_FADE);
    /* the logo */
    const NwArtImage *logo = nw_gameart_image(NW_ART_LOGO);
    float lh = H * 0.40f, bob = sinf(t * 1.2f) * H * 0.006f;
    if (logo) {
        float lw = lh * (float)logo->w / (float)logo->h;
        ImVec2 a(o.x + (W - lw) * 0.5f, o.y + H * 0.045f + bob + (1.0f - in) * -H * 0.03f);
        nw_art_image(dl, NW_ART_LOGO, 0, a, ImVec2(a.x + lw, a.y + lh), nw_c(0xffffff, in));
    } else {
        float th = H * 0.09f, tw = nw_art_text_width(th, "CRASH BANDICOOT");
        nw_art_text(dl, ImVec2(o.x + (W - tw) * 0.5f, o.y + H * 0.14f), th, NW_INK_ORANGE, "CRASH BANDICOOT", in);
        float sh = H * 0.05f, sw = nw_art_text_width(sh, "THE WRATH OF CORTEX");
        nw_art_text(dl, ImVec2(o.x + (W - sw) * 0.5f, o.y + H * 0.25f), sh, NW_INK_GREEN, "THE WRATH OF CORTEX", in);
    }
    /* crate stacks on the ground either side: two crates and one on top */
    struct {
        NwArtId id;
        float col, row;
    } stack[2][3] = {
        { { NW_ART_CRATE, 0.0f, 0.0f }, { NW_ART_CRATE_AKU, 1.0f, 0.0f }, { NW_ART_CRATE_Q, 0.5f, 1.0f } },
        { { NW_ART_CRATE_NITRO, 0.0f, 0.0f }, { NW_ART_CRATE_TNT, 1.0f, 0.0f }, { NW_ART_CRATE_CHECK, 0.5f, 1.0f } },
    };
    float cs = SDL_min(H * 0.17f, W * 0.1f), stepx = cs * 0.8f, stepy = cs * 0.74f, ground = o.y + H * 0.895f;
    for (int side = 0; side < 2; side++) {
        float x0 = side ? o.x + W * 0.965f - cs - stepx : o.x + W * 0.035f;
        for (int k = 0; k < 3; k++) {
            float x = x0 + stack[side][k].col * stepx, y = ground - cs - stack[side][k].row * stepy;
            y -= (1.0f - in) * H * 0.05f * (1.0f + stack[side][k].row);
            nw_art_image(dl, stack[side][k].id, 0, ImVec2(x, y), ImVec2(x + cs, y + cs), nw_c(0xffffff, in));
        }
    }
    /* the menu */
    float ih = H * 0.052f, step = H * 0.06f, y0 = o.y + H * 0.485f;
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    bool moved = ImGui::GetIO().MouseDelta.x != 0.0f || ImGui::GetIO().MouseDelta.y != 0.0f;
    for (int i = 0; i < items; i++) {
        const char *label = i == 0 ? "PLAY" : i == items - 1 ? "QUIT" : l->menus[i - 1].label;
        bool sel = i == l->main_sel;
        float pulse = sel ? 1.0f + 0.045f * sinf(t * 6.0f) : 1.0f;
        float h = ih * pulse * (i == 0 ? 1.12f : 1.0f);
        float w = nw_art_text_width(h, label);
        float delay = SDL_clamp(in * 1.4f - (float)i * 0.06f, 0.0f, 1.0f);
        ImVec2 p(o.x + (W - w) * 0.5f + (1.0f - delay) * W * 0.05f, y0 + (float)i * step + (ih - h) * 0.5f);
        if (moved && mouse.x >= p.x - h && mouse.x <= p.x + w + h && mouse.y >= p.y && mouse.y < p.y + h) {
            if (l->main_sel != i) {
                nw_gameart_sound(NW_SOUND_MOVE);
            }
            l->main_sel = i;
        }
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && mouse.x >= p.x - h && mouse.x <= p.x + w + h &&
            mouse.y >= p.y && mouse.y < p.y + h) {
            l->main_sel = i;
            result = 100; /* accept below */
        }
        if (sel) {
            /* a glow behind the chosen item, the fruit either side */
            dl->AddRectFilledMultiColor(ImVec2(p.x - h * 2.2f, p.y + h * 0.1f), ImVec2(o.x + W * 0.5f, p.y + h * 0.9f),
                                        nw_c(0x2a8cff, 0.0f), nw_c(0x2a8cff, 0.22f * delay),
                                        nw_c(0x2a8cff, 0.22f * delay), nw_c(0x2a8cff, 0.0f));
            dl->AddRectFilledMultiColor(ImVec2(o.x + W * 0.5f, p.y + h * 0.1f), ImVec2(p.x + w + h * 2.2f, p.y + h * 0.9f),
                                        nw_c(0x2a8cff, 0.22f * delay), nw_c(0x2a8cff, 0.0f), nw_c(0x2a8cff, 0.0f),
                                        nw_c(0x2a8cff, 0.22f * delay));
            nw_launcher_wumpa(dl, ImVec2(p.x - h * 0.9f, p.y + h * 0.5f), h * 0.62f, t);
            nw_launcher_wumpa(dl, ImVec2(p.x + w + h * 0.9f, p.y + h * 0.5f), h * 0.62f, t + 0.6f);
        }
        nw_art_text(dl, p, h, sel ? nw_launcher_flash(t) : NW_INK_GOLD, label, delay);
    }
    if (nw_key_up()) {
        l->main_sel = (l->main_sel + items - 1) % items;
        nw_gameart_sound(NW_SOUND_MOVE);
    }
    if (nw_key_down()) {
        l->main_sel = (l->main_sel + 1) % items;
        nw_gameart_sound(NW_SOUND_MOVE);
    }
    if (nw_key_back() && l->main_sel != items - 1) {
        l->main_sel = items - 1;
        nw_gameart_sound(NW_SOUND_BACK);
    }
    if (nw_key_accept() || result == 100) {
        result = -1;
        nw_gameart_sound(l->main_sel == items - 1 ? NW_SOUND_BACK : NW_SOUND_SELECT);
        if (l->main_sel == 0) {
            result = NW_LAUNCHER_PLAY;
        } else if (l->main_sel == items - 1) {
            result = NW_LAUNCHER_QUIT;
        } else {
            nw_launcher_open(l, l->main_sel - 1);
        }
    }
    /* prompts and the name */
    static const NwArtPrompt prompts[] = {
        { "xbox_button_color_a", "keyboard_enter", "Select" },
        { "xbox_button_menu", NULL, "Play" },
    };
    float ph = H * 0.045f;
    nw_art_prompts(dl, ImVec2(o.x + W * 0.04f, o.y + H * 0.93f), ph, prompts, nw_art_pad_used() ? 2 : 1,
                   nw_art_pad_used(), false);
    (void)s;
    return result;
}

static int nw_launcher_options(NwLauncher *l, float W, float H, ImVec2 o, float s, float t)
{
    ImDrawList *fg = ImGui::GetForegroundDrawList();
    ImDrawList *dl = ImGui::GetWindowDrawList();
    float in = nw_ease((float)(ImGui::GetTime() - l->since) / (float)NW_LAUNCHER_FADE);
    float slide = (1.0f - in) * W * 0.04f;
    const NwLauncherMenu *m = &l->menus[l->menu];
    /* heading and the small logo */
    float hh = H * 0.075f;
    nw_art_text(fg, ImVec2(o.x + W * 0.05f - slide, o.y + H * 0.05f), hh, NW_INK_GOLD, m->label, in);
    const NwArtImage *logo = nw_gameart_image(NW_ART_LOGO);
    if (logo) {
        float lh = H * 0.15f, lw = lh * (float)logo->w / (float)logo->h;
        nw_art_image(fg, NW_ART_LOGO, 0, ImVec2(o.x + W * 0.955f - lw, o.y + H * 0.02f),
                     ImVec2(o.x + W * 0.955f, o.y + H * 0.02f + lh), nw_c(0xffffff, in));
    }
    /* the pages */
    float ch = H * 0.036f, cstep = H * 0.062f, cy0 = o.y + H * 0.2f, cx = o.x + W * 0.05f + slide * 0.5f;
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    for (int i = 0; i < l->ncats; i++) {
        bool sel = i == l->cat;
        ImVec2 p(cx + (sel ? H * 0.045f : 0.0f), cy0 + (float)i * cstep);
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && mouse.x >= cx && mouse.x < o.x + W * 0.29f && mouse.y >= p.y - ch * 0.3f &&
            mouse.y < p.y + ch * 1.3f) {
            if (l->cat != i) {
                nw_gameart_sound(NW_SOUND_MOVE);
                l->cat = i;
                l->since = ImGui::GetTime() - NW_LAUNCHER_FADE * 0.5;
            }
            l->zone = 0;
        }
        if (sel) {
            fg->AddRectFilledMultiColor(ImVec2(cx - H * 0.01f, p.y - ch * 0.25f), ImVec2(o.x + W * 0.29f, p.y + ch * 1.25f),
                                        nw_c(0x2a8cff, l->zone ? 0.10f : 0.28f), nw_c(0x2a8cff, 0.0f),
                                        nw_c(0x2a8cff, 0.0f), nw_c(0x2a8cff, l->zone ? 0.10f : 0.28f));
            nw_launcher_wumpa(fg, ImVec2(cx + H * 0.012f, p.y + ch * 0.5f), ch * 0.62f, t);
        }
        NwArtId head = SDL_strcmp(l->cats[i].label, "CRASH") == 0 ? NW_ART_CRASH
                       : SDL_strcmp(l->cats[i].label, "COCO") == 0 ? NW_ART_COCO
                                                                  : NW_ART_COUNT;
        float tw = nw_art_text(fg, p, ch, sel ? nw_launcher_flash(t) : NW_INK_GOLD, l->cats[i].label, in * (sel || !l->zone ? 1.0f : 0.6f));
        if (head != NW_ART_COUNT) {
            nw_art_image(fg, head, 0, ImVec2(p.x + tw + ch * 0.3f, p.y - ch * 0.2f), ImVec2(p.x + tw + ch * 1.7f, p.y + ch * 1.2f),
                         nw_c(0xffffff, in));
        }
    }
    /* the monitor */
    ImVec2 a(o.x + W * 0.31f + slide, o.y + H * 0.17f), b(o.x + W * 0.955f + slide, o.y + H * 0.885f), s0, s1, h0, h1, f0, f1;
    float fs = ImGui::GetFontSize();
    nw_art_panel(dl, a, b, s, H * 0.04f, fs * 2.9f, &s0, &s1, &h0, &h1, &f0, &f1);
    if (l->ncats > 0) {
        const NwLauncherCat *cat = &l->cats[l->cat];
        float th = (h1.y - h0.y) * 0.8f;
        nw_art_text(dl, ImVec2(h0.x + th * 0.5f, (h0.y + h1.y - th) * 0.5f), th, NW_INK_GOLD, cat->label, in);
        ImGui::SetCursorScreenPos(s0);
        ImGuiWindowFlags flags = ImGuiWindowFlags_NoBackground;
        if (l->zone != 1) {
            flags |= ImGuiWindowFlags_NoNavInputs;
        }
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(fs * 0.4f, fs * 0.4f));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(fs * 0.5f, fs * 0.25f));
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, in);
        ImGui::PushStyleColor(ImGuiCol_NavCursor, nw_cv(0, 0.0f)); /* no frame around the list itself */
        ImGui::BeginChild("##options", ImVec2(s1.x - s0.x, s1.y - s0.y), ImGuiChildFlags_None, flags);
        ImGui::PopStyleColor();
        nw_debug_rows_begin();
        ImGui::PushID(l->cat);
        nw_launcher_page(m, cat);
        ImGui::PopID();
        const char *desc = nw_debug_rows_end();
        if (l->focus_opts) {
            /* the first option, without pressing it */
            ImGuiID first = nw_debug_rows_first();
            if (first) {
                ImGui::SetFocusID(first, ImGui::GetCurrentWindow());
                ImGui::SetNavCursorVisible(true);
                ImGui::SetScrollY(0.0f);
            }
            l->focus_opts = false;
        }
        SDL_strlcpy(l->desc_buf, desc, sizeof(l->desc_buf));
        bool clicked = ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) && ImGui::IsMouseClicked(ImGuiMouseButton_Left);
        ImGui::EndChild();
        ImGui::PopStyleVar(3);
        if (clicked && l->zone != 1) {
            l->zone = 1;
        }
        /* the description in the bottom slot */
        const char *text = l->desc_buf[0] ? l->desc_buf
                           : l->zone ? "Choose an option."
                                     : "Pick a page on the left. A / Enter or right: change its options.";
        ImGui::PushClipRect(f0, f1, true);
        ImVec2 tp(f0.x + fs * 0.8f, f0.y + fs * 0.35f);
        dl->AddText(ImGui::GetFont(), fs, tp, nw_c(0x9be7ff, 0.95f * in), text, NULL, f1.x - tp.x - fs * 0.8f);
        ImGui::PopClipRect();
    }
    /* input */
    bool lb = ImGui::IsKeyPressed(ImGuiKey_GamepadL1) || ImGui::IsKeyPressed(ImGuiKey_Q, false);
    bool rb = ImGui::IsKeyPressed(ImGuiKey_GamepadR1) || ImGui::IsKeyPressed(ImGuiKey_E, false) ||
              ImGui::IsKeyPressed(ImGuiKey_Tab, false);
    if (l->zone == 1 && (ImGui::GetIO().WantTextInput || ImGui::GetIO().KeyCtrl)) {
        lb = rb = false;
    }
    if ((lb || rb) && l->ncats > 1) {
        l->cat = (l->cat + (rb ? 1 : l->ncats - 1)) % l->ncats;
        l->since = ImGui::GetTime() - NW_LAUNCHER_FADE * 0.5;
        l->focus_opts = l->zone == 1;
        nw_gameart_sound(NW_SOUND_MOVE);
    } else if (l->zone == 0) {
        if (nw_key_up() && l->ncats) {
            l->cat = (l->cat + l->ncats - 1) % l->ncats;
            l->since = ImGui::GetTime() - NW_LAUNCHER_FADE * 0.5;
            nw_gameart_sound(NW_SOUND_MOVE);
        } else if (nw_key_down() && l->ncats) {
            l->cat = (l->cat + 1) % l->ncats;
            l->since = ImGui::GetTime() - NW_LAUNCHER_FADE * 0.5;
            nw_gameart_sound(NW_SOUND_MOVE);
        } else if ((nw_key_accept() || nw_key(ImGuiKey_RightArrow, ImGuiKey_GamepadDpadRight, ImGuiKey_GamepadLStickRight, false)) &&
                   l->ncats) {
            l->zone = 1;
            l->focus_opts = true;
            nw_gameart_sound(NW_SOUND_SELECT);
        } else if (nw_key_back()) {
            l->screen = 0;
            l->since = ImGui::GetTime();
            nw_gameart_sound(NW_SOUND_BACK);
        }
    } else if (nw_key_back() && !ImGui::IsAnyItemActive() && !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopup)) {
        l->zone = 0;
        ImGui::SetWindowFocus(NULL);
        nw_gameart_sound(NW_SOUND_BACK);
    }
    /* prompts */
    bool pad = nw_art_pad_used();
    static const NwArtPrompt pages[] = {
        { "xbox_button_color_a", "keyboard_enter", "Open" },
        { "xbox_button_color_b", "keyboard_escape", "Back" },
        { "xbox_lb", "keyboard_q", "Page" },
        { "xbox_rb", "keyboard_e", "Page" },
    };
    static const NwArtPrompt opts[] = {
        { "xbox_dpad_left", "keyboard_arrow_left", "Change" },
        { "xbox_button_color_a", "keyboard_enter", "Select" },
        { "xbox_button_color_b", "keyboard_escape", "Back" },
        { "xbox_lb", "keyboard_q", "Page" },
        { "xbox_rb", "keyboard_e", "Page" },
    };
    static const NwArtPrompt play[] = { { "xbox_button_menu", NULL, "Play" } };
    /* a crate stack under the pages, when they leave room */
    float cs = SDL_min(H * 0.12f, W * 0.07f), gx = o.x + W * 0.05f, gy = o.y + H * 0.875f - cs;
    if (cy0 + (float)l->ncats * cstep < gy - cs * 0.8f) {
    nw_art_image(fg, NW_ART_CRATE, 0, ImVec2(gx, gy), ImVec2(gx + cs, gy + cs), nw_c(0xffffff, in * 0.9f));
    nw_art_image(fg, NW_ART_CRATE_Q, 0, ImVec2(gx + cs * 0.8f, gy), ImVec2(gx + cs * 1.8f, gy + cs), nw_c(0xffffff, in * 0.9f));
    nw_art_image(fg, NW_ART_CRATE_AKU, 0, ImVec2(gx + cs * 0.4f, gy - cs * 0.74f), ImVec2(gx + cs * 1.4f, gy + cs * 0.26f),
                 nw_c(0xffffff, in * 0.9f));
    }
    float ph = H * 0.045f;
    if (l->zone) {
        nw_art_prompts(fg, ImVec2(o.x + W * 0.05f, o.y + H * 0.925f), ph, opts, 5, pad, false);
    } else {
        nw_art_prompts(fg, ImVec2(o.x + W * 0.05f, o.y + H * 0.925f), ph, pages, 4, pad, false);
    }
    if (pad) {
        nw_art_prompts(fg, ImVec2(o.x + W * 0.955f, o.y + H * 0.925f), ph, play, 1, pad, true);
    }
    return -1;
}

NwLauncherResult nw_launcher_run(SDL_Window *window, const char *version, NwLauncherTick tick, void *user)
{
    static NwLauncher l;
    SDL_zero(l);
    l.nmenus = nw_launcher_menus(l.menus);
    ImGuiIO &io = ImGui::GetIO();
    ImGuiConfigFlags flags = io.ConfigFlags;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    int result = -1;
    const char *shot = SDL_getenv("NW_LAUNCHER_SHOT"), *page = SDL_getenv("NW_LAUNCHER_PAGE");
    const char *shot_exit = SDL_getenv("NW_LAUNCHER_EXIT"); /* "play" starts the game after the shot */
    int frames = 0, shot_frame = SDL_getenv("NW_LAUNCHER_SHOT_FRAME") ? SDL_atoi(SDL_getenv("NW_LAUNCHER_SHOT_FRAME")) : 60;
    nw_gameart_audio_open();
    if (page && SDL_isdigit((unsigned char)page[0]) && l.nmenus > 0) {
        nw_launcher_open(&l, SDL_clamp(SDL_atoi(page), 0, l.nmenus - 1));
        l.main_sel = l.menu + 1;
        const char *cat = SDL_getenv("NW_LAUNCHER_CAT"), *focus = SDL_getenv("NW_LAUNCHER_FOCUS");
        l.cat = cat ? SDL_clamp(SDL_atoi(cat), 0, SDL_max(l.ncats - 1, 0)) : 0;
        if (focus && SDL_atoi(focus) > 0) {
            l.zone = 1;
            l.focus_opts = true;
        }
    }
    l.since = ImGui::GetTime() - (shot ? 10.0 : 0.0);
    char tag[128];
    SDL_snprintf(tag, sizeof(tag), "%s %s", nw_art_app_title(), version ? version : "");

    while (result < 0) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            nw_ui_event(&event);
            nw_art_input(&event);
            if (tick) {
                tick(user, &event);
            }
            if (event.type == SDL_EVENT_QUIT) {
                result = NW_LAUNCHER_QUIT;
            } else if (event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN && event.gbutton.button == SDL_GAMEPAD_BUTTON_START) {
                nw_gameart_sound(NW_SOUND_SELECT);
                result = NW_LAUNCHER_PLAY;
            }
        }
        if (SDL_GetWindowFlags(window) & SDL_WINDOW_MINIMIZED) {
            SDL_Delay(10);
            continue;
        }
        if (tick) {
            tick(user, NULL);
        }
        int width, height;
        SDL_GetWindowSizeInPixels(window, &width, &height);
        nw_rhi_set_target(NULL, NULL);
        nw_rhi_viewport(0, 0, width, height);
        NwRhiClear clear = {};
        clear.colour = clear.depth = true;
        clear.rgba[0] = 0.03f;
        clear.rgba[1] = 0.04f;
        clear.rgba[2] = 0.12f;
        clear.rgba[3] = 1.0f;
        clear.z = 1.0;
        clear.mask[0] = clear.mask[1] = clear.mask[2] = clear.mask[3] = true;
        clear.depth_write = true;
        nw_rhi_clear(&clear);

        nw_ui_begin();
        float t = (float)ImGui::GetTime();
        const ImGuiViewport *vp = ImGui::GetMainViewport();
        float W = vp->Size.x, H = vp->Size.y, s = SDL_max(H / 1080.0f, 0.5f);
        ImVec2 o = vp->Pos;
        nw_art_backdrop(t, l.screen == 0 ? 1.0f : 0.45f);

        ImGui::SetNextWindowPos(o);
        ImGui::SetNextWindowSize(vp->Size);
        nw_ui_push_font(NW_UI_REGULAR, SDL_max(H / 820.0f, 0.75f) / ImGui::GetStyle().FontScaleDpi);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        ImGui::PushStyleColor(ImGuiCol_ScrollbarBg, nw_cv(0x000000, 0.25f));
        ImGui::PushStyleColor(ImGuiCol_ScrollbarGrab, nw_cv(NW_C_ORANGE, 0.75f));
        ImGui::PushStyleColor(ImGuiCol_NavCursor, nw_cv(NW_C_GOLD));
        ImGui::Begin("##launcher", NULL,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                         ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoBackground |
                         ImGuiWindowFlags_NoNavInputs | ImGuiWindowFlags_NoNavFocus);
        int r = l.screen == 0 ? nw_launcher_main(&l, W, H, o, s, t) : nw_launcher_options(&l, W, H, o, s, t);
        if (r >= 0) {
            result = r;
        }
        ImGui::End();
        ImGui::PopStyleColor(3);
        ImGui::PopStyleVar(2);
        /* the version, bottom right on the main menu */
        if (l.screen == 0) {
            ImFont *font = ImGui::GetFont();
            float ts = H * 0.022f;
            ImVec2 ext = font->CalcTextSizeA(ts, FLT_MAX, 0.0f, tag);
            ImGui::GetForegroundDrawList()->AddText(font, ts, ImVec2(o.x + W * 0.96f - ext.x, o.y + H * 0.945f),
                                                    nw_c(NW_C_MUTED, 0.75f), tag);
        }
        nw_ui_pop_font();
        nw_ui_end();
        if (shot && ++frames == shot_frame) {
            nw_launcher_shot(window, shot);
            if (shot_exit) {
                result = SDL_strcmp(shot_exit, "play") == 0 ? NW_LAUNCHER_PLAY : NW_LAUNCHER_QUIT;
            }
        }
        nw_testpad_frame(window);
        nw_rhi_present();
    }
    nw_gameart_audio_close();
    io.ConfigFlags = flags;
    io.ClearInputKeys();
    io.ClearInputMouse();
    return (NwLauncherResult)result;
}

void nw_launcher_backdrop(void)
{
    nw_art_backdrop((float)ImGui::GetTime(), 1.0f);
}

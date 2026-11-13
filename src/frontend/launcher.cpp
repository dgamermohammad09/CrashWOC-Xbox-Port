#include "frontend/launcher.h"

#include <math.h>

#include "imgui.h"

#include "app.h"
#include "frontend/ui.h"

extern "C" {
#include "frontend/debug.h"
#include "gfx/rhi.h"
}

#define NW_LAUNCHER_MENUS 16
#define NW_PI 3.14159265f

/* The palette: Crash's orange and the Wumpa fruit, the yellow of the game's menu cursor, Cortex's purples. */
enum : unsigned {
    NW_ORANGE = 0xff8a1c,
    NW_ORANGE_DARK = 0xc9500a,
    NW_YELLOW = 0xffd23f,
    NW_CREAM = 0xfff4e0,
    NW_OUTLINE = 0x2a0d02,
    NW_PURPLE_DEEP = 0x12052a,
    NW_PURPLE = 0x30115a,
    NW_PURPLE_MID = 0x4a1a86,
    NW_VIOLET = 0x9b4dff,
    NW_WUMPA = 0xff5a10,
    NW_WUMPA_LIGHT = 0xff9a3c,
    NW_LEAF = 0x3fae2a,
};

static ImU32 nw_col(unsigned rgb, float a = 1.0f)
{
    return IM_COL32(rgb >> 16 & 0xff, rgb >> 8 & 0xff, rgb & 0xff, (int)(a * 255.0f));
}

static ImVec4 nw_vec(unsigned rgb, float a = 1.0f)
{
    return ImGui::ColorConvertU32ToFloat4(nw_col(rgb, a));
}

static float nw_frac(float x)
{
    return x - floorf(x);
}

static void nw_wumpa(ImDrawList *dl, ImVec2 p, float r, float spin)
{
    dl->AddCircleFilled(ImVec2(p.x + r * 0.15f, p.y + r * 0.2f), r, nw_col(0x000000, 0.25f), 32);
    dl->AddCircleFilled(p, r, nw_col(NW_WUMPA), 32);
    dl->AddCircleFilled(ImVec2(p.x - r * 0.15f, p.y - r * 0.15f), r * 0.75f, nw_col(NW_WUMPA_LIGHT, 0.9f), 32);
    dl->AddCircleFilled(ImVec2(p.x - r * 0.35f, p.y - r * 0.4f), r * 0.22f, nw_col(0xffe2b8, 0.85f), 16);
    dl->AddCircle(p, r, nw_col(NW_OUTLINE, 0.6f), 32, r * 0.08f);
    /* the leaf, rocking */
    float a = -NW_PI / 2 + 0.5f + 0.25f * sinf(spin);
    ImVec2 base(p.x + cosf(a) * r * 0.85f, p.y + sinf(a) * r * 0.85f);
    ImVec2 tip(base.x + cosf(a + 0.6f) * r * 0.8f, base.y + sinf(a + 0.6f) * r * 0.8f);
    ImVec2 side(base.x + cosf(a - 0.9f) * r * 0.45f, base.y + sinf(a - 0.9f) * r * 0.45f);
    dl->AddTriangleFilled(base, tip, side, nw_col(NW_LEAF));
    dl->AddTriangle(base, tip, side, nw_col(NW_OUTLINE, 0.6f), r * 0.06f);
}

void nw_launcher_backdrop(void)
{
    const ImGuiViewport *vp = ImGui::GetMainViewport();
    ImDrawList *dl = ImGui::GetBackgroundDrawList();
    float W = vp->Size.x, H = vp->Size.y, t = (float)ImGui::GetTime();
    ImVec2 o = vp->Pos, end(o.x + W, o.y + H);

    dl->AddRectFilledMultiColor(o, end, nw_col(NW_PURPLE_DEEP), nw_col(NW_PURPLE), nw_col(NW_PURPLE_MID),
                                nw_col(NW_PURPLE_DEEP));
    /* Cortex's vortex: rays turning around a glowing eye right of the middle */
    ImVec2 c(o.x + W * 0.68f, o.y + H * 0.46f);
    float R = sqrtf(W * W + H * H);
    const int rays = 28;
    for (int i = 0; i < rays; i++) {
        float a0 = t * 0.12f + (float)i * 2 * NW_PI / rays, a1 = a0 + NW_PI / rays;
        dl->AddTriangleFilled(c, ImVec2(c.x + cosf(a0) * R, c.y + sinf(a0) * R),
                              ImVec2(c.x + cosf(a1) * R, c.y + sinf(a1) * R),
                              i & 1 ? nw_col(NW_VIOLET, 0.07f) : nw_col(NW_ORANGE, 0.06f));
    }
    for (int k = 0; k < 14; k++) {
        dl->AddCircleFilled(c, H * 0.6f * (1.0f - (float)k / 14.0f), nw_col(NW_ORANGE, 0.035f), 64);
    }
    for (int arm = 0; arm < 3; arm++) {
        for (int s = 0; s < 70; s++) {
            float ang = -t * 0.7f + (float)arm * 2 * NW_PI / 3 + (float)s * 0.11f, rad = (float)s * H * 0.0085f;
            dl->PathLineTo(ImVec2(c.x + cosf(ang) * rad, c.y + sinf(ang) * rad));
        }
        dl->PathStroke(nw_col(NW_VIOLET, 0.28f), H * 0.009f);
    }
    /* Wumpa fruit drifting across */
    for (int i = 0; i < 11; i++) {
        float seed = (float)i * 0.618f + 0.11f;
        float speed = 0.018f + 0.02f * nw_frac(seed * 3.7f);
        float r = H * (0.016f + 0.014f * nw_frac(seed * 1.9f));
        float x = o.x - r * 2 + nw_frac(nw_frac(seed * 5.3f) + t * speed) * (W + r * 4);
        float y = o.y + H * (0.08f + 0.86f * nw_frac(seed * 2.3f)) + sinf(t * 1.3f + seed * 9.0f) * H * 0.015f;
        nw_wumpa(dl, ImVec2(x, y), r, t * 2.0f + seed * 7.0f);
    }
    /* darker floor for the hints */
    dl->AddRectFilledMultiColor(ImVec2(o.x, o.y + H * 0.82f), end, nw_col(NW_PURPLE_DEEP, 0.0f),
                                nw_col(NW_PURPLE_DEEP, 0.0f), nw_col(NW_PURPLE_DEEP, 0.95f),
                                nw_col(NW_PURPLE_DEEP, 0.95f));
}

/* Text with the game's thick dark outline; top is drawn over the upper half in a second colour. */
static void nw_title_text(ImDrawList *dl, ImFont *font, float size, ImVec2 p, unsigned fill, unsigned top,
                          const char *text)
{
    float th = SDL_max(size * 0.06f, 2.0f);
    for (int i = 0; i < 16; i++) {
        float a = (float)i * NW_PI / 8;
        dl->AddText(font, size, ImVec2(p.x + cosf(a) * th, p.y + sinf(a) * th), nw_col(NW_OUTLINE), text);
    }
    dl->AddText(font, size, ImVec2(p.x + th * 0.6f, p.y + th * 1.6f), nw_col(NW_OUTLINE), text);
    dl->AddText(font, size, p, nw_col(fill), text);
    ImVec2 ext = font->CalcTextSizeA(size, FLT_MAX, 0.0f, text);
    dl->PushClipRect(p, ImVec2(p.x + ext.x, p.y + ext.y * 0.5f), true);
    dl->AddText(font, size, p, nw_col(top), text);
    dl->PopClipRect();
}

typedef struct NwLauncherMenu {
    char name[48];  /* the registered top-level menu */
    char label[48]; /* shown, in capitals */
    unsigned items; /* what it shows before the game starts */
} NwLauncherMenu;

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
            for (size_t c = 0; top[c] && c + 1 < sizeof(menus[count].label); c++) {
                menus[count].label[c] = (char)SDL_toupper((unsigned char)top[c]);
            }
            count++;
        }
        menus[m].items++;
    }
    return count;
}

static void nw_launcher_style(float s)
{
    ImGui::PushStyleColor(ImGuiCol_Text, nw_vec(NW_CREAM));
    ImGui::PushStyleColor(ImGuiCol_TextDisabled, nw_vec(0xc9b3e8));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, nw_vec(0, 0));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, nw_vec(0, 0));
    ImGui::PushStyleColor(ImGuiCol_PopupBg, nw_vec(NW_PURPLE_DEEP, 0.97f));
    ImGui::PushStyleColor(ImGuiCol_Border, nw_vec(NW_ORANGE, 0.7f));
    ImGui::PushStyleColor(ImGuiCol_FrameBg, nw_vec(NW_PURPLE, 0.95f));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, nw_vec(NW_PURPLE_MID));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, nw_vec(0x5e24a8));
    ImGui::PushStyleColor(ImGuiCol_Button, nw_vec(NW_ORANGE));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, nw_vec(0xffa640));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, nw_vec(NW_ORANGE_DARK));
    ImGui::PushStyleColor(ImGuiCol_CheckMark, nw_vec(NW_YELLOW));
    ImGui::PushStyleColor(ImGuiCol_SliderGrab, nw_vec(NW_ORANGE));
    ImGui::PushStyleColor(ImGuiCol_SliderGrabActive, nw_vec(NW_YELLOW));
    ImGui::PushStyleColor(ImGuiCol_Header, nw_vec(NW_PURPLE_MID));
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, nw_vec(0x5e24a8));
    ImGui::PushStyleColor(ImGuiCol_HeaderActive, nw_vec(NW_ORANGE_DARK));
    ImGui::PushStyleColor(ImGuiCol_Separator, nw_vec(NW_ORANGE, 0.6f));
    ImGui::PushStyleColor(ImGuiCol_NavCursor, nw_vec(NW_YELLOW));
    ImGui::PushStyleColor(ImGuiCol_ScrollbarBg, nw_vec(NW_PURPLE_DEEP, 0.6f));
    ImGui::PushStyleColor(ImGuiCol_ScrollbarGrab, nw_vec(NW_ORANGE, 0.8f));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 8.0f * s);
    ImGui::PushStyleVar(ImGuiStyleVar_GrabRounding, 8.0f * s);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(14.0f * s, 8.0f * s));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(12.0f * s, 10.0f * s));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize, 18.0f * s);
}

static void nw_launcher_unstyle(void)
{
    ImGui::PopStyleVar(6);
    ImGui::PopStyleColor(22);
}

/* A big menu button: orange with the dark outline, the chosen one yellow and pushed out to the right. */
static bool nw_launcher_button(const char *label, bool chosen, ImVec2 size, float s, bool *focused)
{
    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    bool pressed = ImGui::InvisibleButton(label, size);
    *focused = ImGui::IsItemFocused();
    bool hot = chosen || ImGui::IsItemHovered() || *focused;
    float push = hot ? 34.0f * s : 0.0f;
    ImVec2 a(p.x + push, p.y), b(p.x + size.x, p.y + size.y);
    float round = size.y * 0.35f;
    dl->AddRectFilled(ImVec2(a.x + 4 * s, a.y + 6 * s), ImVec2(b.x + 4 * s, b.y + 6 * s), nw_col(0, 0.35f), round);
    dl->AddRectFilledMultiColor(a, b, nw_col(hot ? NW_YELLOW : NW_ORANGE), nw_col(hot ? NW_YELLOW : NW_ORANGE),
                                nw_col(hot ? 0xff9f1c : NW_ORANGE_DARK), nw_col(hot ? 0xff9f1c : NW_ORANGE_DARK));
    dl->AddRect(a, b, nw_col(NW_OUTLINE), round * 0.25f, 4.0f * s);
    nw_ui_push_font(NW_UI_BOLD, 1.0f);
    ImFont *font = ImGui::GetFont();
    float fs = size.y * 0.52f;
    ImVec2 ext = font->CalcTextSizeA(fs, FLT_MAX, 0.0f, label);
    float room = b.x - a.x - 34.0f * s; /* long names shrink to fit the button */
    if (ext.x > room && ext.x > 0.0f) {
        fs *= room / ext.x;
        ext = font->CalcTextSizeA(fs, FLT_MAX, 0.0f, label);
    }
    ImVec2 tp(a.x + 22.0f * s, a.y + (size.y - ext.y) * 0.5f);
    dl->AddText(font, fs, ImVec2(tp.x + 2 * s, tp.y + 2 * s), nw_col(NW_OUTLINE, 0.8f), label);
    dl->AddText(font, fs, tp, nw_col(hot ? NW_OUTLINE : NW_CREAM), label);
    nw_ui_pop_font();
    if (hot) {
        /* the menu cursor: a spinning Wumpa fruit */
        nw_wumpa(dl, ImVec2(p.x + size.y * 0.3f, p.y + size.y * 0.5f), size.y * 0.26f, (float)ImGui::GetTime() * 4.0f);
    }
    return pressed;
}

static void nw_launcher_page(const NwLauncherMenu *m)
{
    NwDebugItemKind kind;
    const char *menu, *label;
    bool first = true;
    ImGui::PushItemWidth(ImGui::GetFontSize() * 14.0f);
    for (unsigned i = 0; nw_debug_item(i, &kind, &menu, &label); i++) {
        char top[48];
        nw_launcher_top(menu, top, sizeof(top));
        if (SDL_strcmp(top, m->name) != 0 || !nw_launcher_shows(top, kind)) {
            continue;
        }
        if (kind == NW_DEBUG_ITEM_PANEL || first) {
            if (!first) {
                ImGui::Spacing();
            }
            nw_ui_push_font(NW_UI_BOLD, 1.15f);
            ImGui::PushStyleColor(ImGuiCol_Text, nw_vec(NW_YELLOW));
            ImGui::SeparatorText(kind == NW_DEBUG_ITEM_PANEL ? label : "Options");
            ImGui::PopStyleColor();
            nw_ui_pop_font();
        }
        first = false;
        nw_debug_item_draw(i);
    }
    ImGui::PopItemWidth();
}

/* Test hook: NW_LAUNCHER_SHOT=<png> saves the page a second in, NW_LAUNCHER_PAGE=<n> opens menu n first,
   NW_LAUNCHER_EXIT=1 quits after the shot (the Xbox build cannot be screenshotted from outside). */
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

NwLauncherResult nw_launcher_run(SDL_Window *window, const char *version, NwLauncherTick tick, void *user)
{
    NwLauncherMenu menus[NW_LAUNCHER_MENUS];
    int nmenus = nw_launcher_menus(menus);
    ImGuiIO &io = ImGui::GetIO();
    ImGuiConfigFlags flags = io.ConfigFlags;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    int chosen = 0; /* the menu on the right */
    int result = -1;
    bool focus_play = true;
    const char *shot = SDL_getenv("NW_LAUNCHER_SHOT"), *page = SDL_getenv("NW_LAUNCHER_PAGE");
    const char *shot_exit = SDL_getenv("NW_LAUNCHER_EXIT"); /* "play" starts the game after the shot */
    int frames = 0;
    if (page) {
        chosen = SDL_clamp(SDL_atoi(page), 0, SDL_max(nmenus - 1, 0));
    }

    while (result < 0) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            nw_ui_event(&event);
            if (tick) {
                tick(user, &event);
            }
            if (event.type == SDL_EVENT_QUIT) {
                result = NW_LAUNCHER_QUIT;
            } else if (event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN && event.gbutton.button == SDL_GAMEPAD_BUTTON_START) {
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
        clear.rgba[0] = 0.07f;
        clear.rgba[1] = 0.02f;
        clear.rgba[2] = 0.16f;
        clear.rgba[3] = 1.0f;
        clear.z = 1.0;
        clear.mask[0] = clear.mask[1] = clear.mask[2] = clear.mask[3] = true;
        clear.depth_write = true;
        nw_rhi_clear(&clear);

        nw_ui_begin();
        nw_launcher_backdrop();
        const ImGuiViewport *vp = ImGui::GetMainViewport();
        float W = vp->Size.x, H = vp->Size.y, s = SDL_max(H / 1080.0f, 0.6f);
        ImVec2 o = vp->Pos;
        ImDrawList *fg = ImGui::GetBackgroundDrawList();

        nw_ui_push_font(NW_UI_BOLD, 1.0f);
        ImFont *bold = ImGui::GetFont();
        nw_ui_pop_font();
        float ts = H * 0.085f;
        nw_title_text(fg, bold, ts, ImVec2(o.x + W * 0.05f, o.y + H * 0.045f), NW_ORANGE, NW_YELLOW,
                      "CRASH BANDICOOT");
        nw_title_text(fg, bold, ts * 0.5f, ImVec2(o.x + W * 0.055f, o.y + H * 0.045f + ts * 1.08f), NW_VIOLET,
                      0xd9b8ff, "THE WRATH OF CORTEX");
        char tag[96];
        SDL_snprintf(tag, sizeof(tag), NW_APP_NAME " %s", version ? version : "");
        fg->AddText(bold, ts * 0.26f, ImVec2(o.x + W * 0.057f, o.y + H * 0.045f + ts * 1.75f), nw_col(NW_CREAM, 0.8f),
                    tag);

        ImGui::SetNextWindowPos(o);
        ImGui::SetNextWindowSize(vp->Size);
        nw_launcher_style(s);
        nw_ui_push_font(NW_UI_REGULAR, SDL_max(H / 1000.0f, 0.9f));
        ImGui::Begin("##launcher", NULL,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                         ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoBackground);

        /* the menu column */
        ImVec2 left(W * 0.05f, H * 0.29f), left_size(W * 0.25f, H * 0.55f);
        ImGui::SetCursorPos(left);
        ImGui::BeginChild("##menu", left_size, ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground);
        float bh = SDL_min(H * 0.062f, left_size.y / (float)(nmenus + 3));
        ImVec2 bsize(left_size.x - 24.0f * s, bh);
        bool focused;
        if (focus_play) {
            ImGui::SetKeyboardFocusHere();
            focus_play = false;
        }
        if (nw_launcher_button("PLAY", false, ImVec2(bsize.x, bh * 1.25f), s, &focused)) {
            result = NW_LAUNCHER_PLAY;
        }
        ImGui::Dummy(ImVec2(1, bh * 0.15f));
        for (int i = 0; i < nmenus; i++) {
            if (nw_launcher_button(menus[i].label, chosen == i, bsize, s, &focused) || focused) {
                chosen = i;
            }
        }
        ImGui::Dummy(ImVec2(1, bh * 0.15f));
        if (nw_launcher_button("QUIT", false, bsize, s, &focused)) {
            result = NW_LAUNCHER_QUIT;
        }
        ImGui::EndChild();

        /* the chosen menu's page: a purple board with the orange rim */
        ImVec2 right(W * 0.33f, H * 0.29f), right_size(W * 0.62f, H * 0.58f);
        ImDrawList *dl = ImGui::GetWindowDrawList();
        ImVec2 a(o.x + right.x, o.y + right.y), b(a.x + right_size.x, a.y + right_size.y);
        dl->AddRectFilled(ImVec2(a.x + 8 * s, a.y + 10 * s), ImVec2(b.x + 8 * s, b.y + 10 * s), nw_col(0, 0.35f),
                          24.0f * s);
        dl->AddRectFilled(a, b, nw_col(NW_PURPLE_DEEP, 0.9f), 24.0f * s);
        dl->AddRect(a, b, nw_col(NW_ORANGE), 24.0f * s, 5.0f * s);
        dl->AddRect(ImVec2(a.x + 7 * s, a.y + 7 * s), ImVec2(b.x - 7 * s, b.y - 7 * s), nw_col(NW_OUTLINE, 0.8f),
                    18.0f * s, 2.0f * s);
        if (nmenus > 0) {
            float pad = 34.0f * s;
            nw_title_text(dl, bold, H * 0.042f, ImVec2(a.x + pad, a.y + pad * 0.6f), NW_ORANGE, NW_YELLOW,
                          menus[chosen].label);
            ImGui::SetCursorPos(ImVec2(right.x + pad, right.y + pad * 0.6f + H * 0.062f));
            ImGui::BeginChild("##page", ImVec2(right_size.x - pad * 1.5f, right_size.y - pad * 1.2f - H * 0.062f),
                              ImGuiChildFlags_NavFlattened, ImGuiWindowFlags_NoBackground);
            nw_launcher_page(&menus[chosen]);
            ImGui::EndChild();
        }
        ImGui::End();
        nw_ui_pop_font();
        nw_launcher_unstyle();

        /* the hints along the bottom */
        const char *hints = "A / Enter / click: choose     B / Esc: back     Menu (Start): play     F1 in the game: "
                            "these settings again";
        fg->AddText(bold, H * 0.024f, ImVec2(o.x + W * 0.05f, o.y + H * 0.925f), nw_col(NW_CREAM, 0.85f), hints);
        nw_ui_end();
        if (shot && ++frames == 60) {
            nw_launcher_shot(window, shot);
            if (shot_exit) {
                result = SDL_strcmp(shot_exit, "play") == 0 ? NW_LAUNCHER_PLAY : NW_LAUNCHER_QUIT;
            }
        }
        nw_rhi_present();
    }
    io.ConfigFlags = flags;
    io.ClearInputKeys();
    io.ClearInputMouse();
    return (NwLauncherResult)result;
}

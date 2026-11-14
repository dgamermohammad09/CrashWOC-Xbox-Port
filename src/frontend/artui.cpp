#include "frontend/artui.h"

#include <math.h>

#include "imgui_internal.h"

#include "app.h"
#include "frontend/ui.h"

extern "C" {
#include "gfx/rhi.h"
}

#ifdef NW_WITH_GAME
/* assets/prompts, embedded for the game's own prompts (cmake/decomp.cmake). */
struct nw_embed_file {
    const char *name;
    const unsigned char *data;
    size_t size;
};
extern "C" const struct nw_embed_file nw_prompt_icons[];
extern "C" const size_t nw_prompt_icons_count;
#endif

#define NW_ART_PI 3.14159265f
#define NW_ART_ICONS 24

static struct {
    struct {
        char name[40];
        NwRhiTexture *tex; /* NULL: missing */
    } icons[NW_ART_ICONS];
    int nicons;
    bool pad;
    bool pad_known;
} nw_artui;

ImU32 nw_c(unsigned rgb, float a)
{
    return IM_COL32(rgb >> 16 & 0xff, rgb >> 8 & 0xff, rgb & 0xff, (int)(SDL_clamp(a, 0.0f, 1.0f) * 255.0f));
}

ImVec4 nw_cv(unsigned rgb, float a)
{
    return ImGui::ColorConvertU32ToFloat4(nw_c(rgb, a));
}

const char *nw_art_app_title(void)
{
    return NW_APP_TITLE;
}

static float nw_art_hash(int i)
{
    unsigned x = (unsigned)i * 2654435761u + 0x9e3779b9u;
    x ^= x >> 15;
    x *= 2246822519u;
    x ^= x >> 13;
    return (float)(x & 0xffffff) / 16777216.0f;
}

void nw_art_backdrop(float t, float shards)
{
    const ImGuiViewport *vp = ImGui::GetMainViewport();
    ImDrawList *dl = ImGui::GetBackgroundDrawList();
    float W = vp->Size.x, H = vp->Size.y;
    ImVec2 o = vp->Pos, end(o.x + W, o.y + H);
    dl->AddRectFilledMultiColor(o, end, nw_c(NW_C_NIGHT), nw_c(0x0c0d2c), nw_c(NW_C_NIGHT_HI), nw_c(0x120a33));
    /* the title's shards: long triangles turning slowly around a point behind the logo */
    if (shards > 0.0f) {
        static const unsigned cols[] = { 0x8a4a1a, 0x18a4ff, 0xff5a10, 0x3fe02a, 0xbfe9ff, 0xffb000, 0x18a4ff, 0xff3d6a };
        ImVec2 c(o.x + W * 0.5f, o.y + H * 0.30f);
        float R = SDL_max(W, H) * 0.75f;
        for (int i = 0; i < 8; i++) {
            float a = t * 0.05f + (float)i * 2 * NW_ART_PI / 8 + nw_art_hash(i) * 0.5f;
            float w = 0.10f + 0.08f * nw_art_hash(i + 20);
            float r0 = R * (0.18f + 0.1f * nw_art_hash(i + 40));
            ImVec2 p0(c.x + cosf(a) * r0 * 0.3f, c.y + sinf(a) * r0 * 0.3f);
            ImVec2 p1(c.x + cosf(a - w) * R, c.y + sinf(a - w) * R), p2(c.x + cosf(a + w) * R, c.y + sinf(a + w) * R);
            dl->AddTriangleFilled(p0, p1, p2, nw_c(cols[i], 0.10f * shards));
        }
    }
    /* stars, twinkling */
    for (int i = 0; i < 260; i++) {
        float x = nw_art_hash(i * 3) * W, y = nw_art_hash(i * 3 + 1) * H, k = nw_art_hash(i * 3 + 2);
        float tw = 0.55f + 0.45f * sinf(t * (0.8f + k * 2.5f) + k * 40.0f);
        float r = H * (0.0008f + 0.0018f * k * k);
        dl->AddCircleFilled(ImVec2(o.x + x, o.y + y), r, nw_c(k > 0.85f ? 0xffe6c0 : 0xd8e4ff, 0.25f + 0.65f * tw * k), 8);
    }
    /* a warm planet glow, upper right, as the title's sun */
    ImVec2 sun(o.x + W * 0.97f, o.y + H * 0.08f);
    for (int k = 12; k > 0; k--) {
        dl->AddCircleFilled(sun, H * 0.03f * (float)k, nw_c(0xff8a1c, 0.012f), 48);
    }
    /* vignette */
    dl->AddRectFilledMultiColor(o, ImVec2(end.x, o.y + H * 0.18f), nw_c(0, 0.45f), nw_c(0, 0.45f), nw_c(0, 0.0f),
                                nw_c(0, 0.0f));
    dl->AddRectFilledMultiColor(ImVec2(o.x, end.y - H * 0.22f), end, nw_c(0, 0.0f), nw_c(0, 0.0f), nw_c(0, 0.6f),
                                nw_c(0, 0.6f));
}

/* ---- text ---- */

static const unsigned nw_art_ink_top[NW_INK_COUNT] = { 0xffe680, 0xffd040, 0x9ee0ff, 0xc8ff7a };
static const unsigned nw_art_ink_bottom[NW_INK_COUNT] = { 0xc8861c, 0xff5200, 0x1a78ff, 0x2fae1a };

static ImFont *nw_art_bold(void)
{
    nw_ui_push_font(NW_UI_BOLD, 1.0f);
    ImFont *f = ImGui::GetFont();
    nw_ui_pop_font();
    return f;
}

static float nw_art_fallback(ImDrawList *dl, ImVec2 p, float h, NwArtInk ink, const char *text, float alpha)
{
    ImFont *font = nw_art_bold();
    float size = h * 0.86f;
    ImVec2 ext = font->CalcTextSizeA(size, FLT_MAX, 0.0f, text);
    if (!dl) {
        return ext.x;
    }
    ImVec2 at(p.x, p.y + (h - ext.y) * 0.5f);
    float th = SDL_max(size * 0.07f, 1.5f);
    for (int i = 0; i < 12; i++) {
        float a = (float)i * NW_ART_PI / 6;
        dl->AddText(font, size, ImVec2(at.x + cosf(a) * th, at.y + sinf(a) * th + th * 0.6f), nw_c(NW_C_INK, alpha), text);
    }
    int v0 = dl->VtxBuffer.Size;
    dl->AddText(font, size, at, nw_c(0xffffff, alpha), text);
    ImGui::ShadeVertsLinearColorGradientKeepAlpha(dl, v0, dl->VtxBuffer.Size, ImVec2(at.x, at.y + ext.y * 0.2f),
                                                  ImVec2(at.x, at.y + ext.y * 0.85f), nw_c(nw_art_ink_top[ink]),
                                                  nw_c(nw_art_ink_bottom[ink]));
    return ext.x;
}

static float nw_art_text_draw(ImDrawList *dl, ImVec2 p, float h, NwArtInk ink, const char *text, float alpha, bool mono)
{
    const NwArtFont *f = nw_gameart_font();
    if (!f) {
        return nw_art_fallback(dl, p, h, ink, text, alpha);
    }
    ImTextureID tex = (ImTextureID)nw_rhi_texture_ui(f->tex);
    float x = p.x;
    for (const char *s = text; *s; s++) {
        unsigned char c = (unsigned char)SDL_toupper((unsigned char)*s);
        if (c == ' ') {
            x += h * (mono ? f->advance : 0.42f);
            continue;
        }
        if (c >= 128 || !f->have[c]) {
            char one[2] = { (char)c, 0 };
            x += nw_art_fallback(dl, ImVec2(x, p.y), h, ink, one, alpha) + h * 0.06f;
            continue;
        }
        const NwArtGlyph *g = &f->glyphs[ink][c];
        float w = g->w * h;
        float step = mono ? f->advance * h : w * 0.9f;
        float gx = mono ? x + (step - w) * 0.5f : x;
        if (dl) {
            float sh = h * 0.045f;
            dl->AddImage(tex, ImVec2(gx + sh, p.y + sh * 1.4f), ImVec2(gx + w + sh, p.y + h + sh * 1.4f), ImVec2(g->u0, g->v0),
                         ImVec2(g->u1, g->v1), nw_c(0x000000, 0.55f * alpha));
            dl->AddImage(tex, ImVec2(gx, p.y), ImVec2(gx + w, p.y + h), ImVec2(g->u0, g->v0), ImVec2(g->u1, g->v1),
                         nw_c(0xffffff, alpha));
        }
        x += step;
    }
    return x - p.x;
}

float nw_art_text(ImDrawList *dl, ImVec2 p, float h, NwArtInk ink, const char *text, float alpha, bool mono)
{
    return nw_art_text_draw(dl, p, h, ink, text, alpha, mono);
}

float nw_art_text_width(float h, const char *text, bool mono)
{
    return nw_art_text_draw(NULL, ImVec2(0, 0), h, NW_INK_GOLD, text, 1.0f, mono);
}

/* ---- images ---- */

bool nw_art_image(ImDrawList *dl, NwArtId id, int frame, ImVec2 a, ImVec2 b, ImU32 tint)
{
    const NwArtImage *im = nw_gameart_image(id);
    if (!im) {
        return false;
    }
    int cols = SDL_max(im->cols, 1), frames = SDL_max(im->frames, 1), rows = (frames + cols - 1) / cols;
    frame = ((frame % frames) + frames) % frames;
    float fu = 1.0f / (float)cols, fv = 1.0f / (float)rows;
    ImVec2 uv0((float)(frame % cols) * fu, (float)(frame / cols) * fv);
    dl->AddImage((ImTextureID)nw_rhi_texture_ui(im->tex), a, b, uv0, ImVec2(uv0.x + fu, uv0.y + fv), tint);
    return true;
}

/* ---- panels ---- */

static void nw_art_slice(ImDrawList *dl, ImTextureID tex, float tw, float th, const float *sx, const float *sy, int nx,
                         int ny, const float *dx, const float *dy)
{
    for (int j = 0; j < ny; j++) {
        for (int i = 0; i < nx; i++) {
            dl->AddImage(tex, ImVec2(dx[i], dy[j]), ImVec2(dx[i + 1], dy[j + 1]), ImVec2(sx[i] / tw, sy[j] / th),
                         ImVec2(sx[i + 1] / tw, sy[j + 1] / th));
        }
    }
}

void nw_art_panel(ImDrawList *dl, ImVec2 a, ImVec2 b, float s, float header, float footer, ImVec2 *screen0,
                  ImVec2 *screen1, ImVec2 *head0, ImVec2 *head1, ImVec2 *foot0, ImVec2 *foot1)
{
    const NwArtImage *im = nw_gameart_image(NW_ART_PANEL);
    float k = s * 1.25f;
    float top = SDL_max(50.0f * k, header + 10.0f * k), bottom = SDL_max(52.0f * k, footer + 14.0f * k);
    /* shadow */
    dl->AddRectFilled(ImVec2(a.x + 10 * s, a.y + 14 * s), ImVec2(b.x + 10 * s, b.y + 14 * s), nw_c(0, 0.45f), 18 * s);
    if (im) {
        ImTextureID tex = (ImTextureID)nw_rhi_texture_ui(im->tex);
        /* texture: borders 28 wide; bands: the top slot 0-50, the screen 50-176, the bottom slot 204-256 */
        static const float sx[4] = { 0, 28, 228, 256 };
        float dx[4] = { a.x, a.x + 28 * k, b.x - 28 * k, b.x };
        static const float sy_top[4] = { 0, 14, 46, 50 }, sy_mid[2] = { 50, 176 }, sy_bot[4] = { 204, 206, 242, 256 };
        float y1 = a.y + top, y2 = b.y - bottom;
        float dy_top[4] = { a.y, a.y + 14 * k, y1 - 4 * k, y1 };
        float dy_mid[2] = { y1, y2 };
        float dy_bot[4] = { y2, y2 + 2 * k, b.y - 14 * k, b.y };
        nw_art_slice(dl, tex, (float)im->w, (float)im->h, sx, sy_top, 3, 3, dx, dy_top);
        nw_art_slice(dl, tex, (float)im->w, (float)im->h, sx, sy_mid, 3, 1, dx, dy_mid);
        nw_art_slice(dl, tex, (float)im->w, (float)im->h, sx, sy_bot, 3, 3, dx, dy_bot);
        /* the screen darker, so text reads well */
        dl->AddRectFilled(ImVec2(a.x + 30 * k, y1 + 3 * k), ImVec2(b.x - 30 * k, y2 - 3 * k), nw_c(0x061018, 0.55f), 4 * k);
        if (screen0) {
            *screen0 = ImVec2(a.x + 34 * k, y1 + 8 * k);
            *screen1 = ImVec2(b.x - 34 * k, y2 - 8 * k);
        }
        if (head0) {
            *head0 = ImVec2(a.x + 30 * k, a.y + 14 * k);
            *head1 = ImVec2(b.x - 30 * k, y1 - 4 * k);
        }
        if (foot0) {
            *foot0 = ImVec2(a.x + 30 * k, y2 + 4 * k);
            *foot1 = ImVec2(b.x - 30 * k, b.y - 14 * k);
        }
        return;
    }
    /* a drawn steel frame */
    float r = 16 * s;
    dl->AddRectFilledMultiColor(a, b, nw_c(0xc3ccd6), nw_c(0x9aa6b3), nw_c(0x5d6874), nw_c(0x7c8794));
    dl->AddRect(a, b, nw_c(0x2b323a), r * 0.3f, 3 * s);
    dl->AddRect(ImVec2(a.x + 3 * s, a.y + 3 * s), ImVec2(b.x - 3 * s, b.y - 3 * s), nw_c(0xffffff, 0.35f), r * 0.3f, 1.5f * s);
    float y1 = a.y + top, y2 = b.y - bottom, m = 18 * s;
    ImVec2 h0(a.x + m, a.y + 10 * s), h1(b.x - m, y1 - 6 * s), s0(a.x + m, y1), s1(b.x - m, y2), f0(a.x + m, y2 + 6 * s),
        f1(b.x - m, b.y - 10 * s);
    dl->AddRectFilled(h0, h1, nw_c(0x161c22), 6 * s);
    dl->AddRectFilledMultiColor(s0, s1, nw_c(0x2c3e48), nw_c(0x2c3e48), nw_c(0x111a20), nw_c(0x111a20));
    dl->AddRect(s0, s1, nw_c(0x0b0f12), 4 * s, 3 * s);
    dl->AddRectFilled(f0, f1, nw_c(0x161c22), 6 * s);
    float br = 4.5f * s;
    ImVec2 bolts[4] = { ImVec2(a.x + 9 * s, a.y + 9 * s), ImVec2(b.x - 9 * s, a.y + 9 * s), ImVec2(a.x + 9 * s, b.y - 9 * s),
                        ImVec2(b.x - 9 * s, b.y - 9 * s) };
    for (const ImVec2 &p : bolts) {
        dl->AddCircleFilled(p, br, nw_c(0x4a525b), 12);
        dl->AddLine(ImVec2(p.x - br * 0.6f, p.y - br * 0.6f), ImVec2(p.x + br * 0.6f, p.y + br * 0.6f), nw_c(0xd0d6dc), 1.2f * s);
    }
    if (screen0) {
        *screen0 = ImVec2(s0.x + 8 * s, s0.y + 8 * s);
        *screen1 = ImVec2(s1.x - 8 * s, s1.y - 8 * s);
    }
    if (head0) {
        *head0 = h0;
        *head1 = h1;
    }
    if (foot0) {
        *foot0 = f0;
        *foot1 = f1;
    }
}

void nw_art_card(ImDrawList *dl, ImVec2 a, ImVec2 b, float s, bool lit)
{
    float r = 14 * s;
    for (int i = 3; i > 0; i--) {
        float g = (float)i * 5 * s;
        dl->AddRectFilled(ImVec2(a.x - g * 0.3f, a.y + g * 0.5f), ImVec2(b.x + g * 0.3f, b.y + g), nw_c(0, 0.12f), r + g);
    }
    dl->AddRectFilled(a, b, nw_c(0x121838, 0.94f), r);
    dl->AddRectFilledMultiColor(ImVec2(a.x + r * 0.3f, a.y), ImVec2(b.x - r * 0.3f, a.y + (b.y - a.y) * 0.5f),
                                nw_c(0xffffff, 0.05f), nw_c(0xffffff, 0.05f), nw_c(0xffffff, 0.0f), nw_c(0xffffff, 0.0f));
    dl->AddRect(a, b, lit ? nw_c(NW_C_ORANGE, 0.95f) : nw_c(0x4a5596, 0.7f), r, (lit ? 2.5f : 1.2f) * s);
}

/* ---- prompts ---- */

static NwRhiTexture *nw_art_icon(const char *name)
{
    for (int i = 0; i < nw_artui.nicons; i++) {
        if (SDL_strcmp(nw_artui.icons[i].name, name) == 0) {
            return nw_artui.icons[i].tex;
        }
    }
    if (nw_artui.nicons == NW_ART_ICONS) {
        return NULL;
    }
    int n = nw_artui.nicons++;
    SDL_strlcpy(nw_artui.icons[n].name, name, sizeof(nw_artui.icons[n].name));
    nw_artui.icons[n].tex = NULL;
#ifdef NW_WITH_GAME
    for (size_t i = 0; i < nw_prompt_icons_count; i++) {
        if (SDL_strcmp(nw_prompt_icons[i].name, name) != 0) {
            continue;
        }
        SDL_IOStream *io = SDL_IOFromConstMem(nw_prompt_icons[i].data, nw_prompt_icons[i].size);
        SDL_Surface *png = io ? SDL_LoadPNG_IO(io, true) : NULL;
        SDL_Surface *rgba = png ? SDL_ConvertSurface(png, SDL_PIXELFORMAT_RGBA32) : NULL;
        SDL_DestroySurface(png);
        if (rgba && rgba->pitch == rgba->w * 4) {
            const void *level = rgba->pixels;
            nw_artui.icons[n].tex = nw_rhi_texture_create(NW_RHI_RGBA8, rgba->w, rgba->h, 1, 1, NW_RHI_TEX_LINEAR, &level);
        }
        SDL_DestroySurface(rgba);
        break;
    }
#endif
    return nw_artui.icons[n].tex;
}

float nw_art_prompts(ImDrawList *dl, ImVec2 p, float h, const NwArtPrompt *prompts, int count, bool pad, bool right)
{
    ImFont *font = nw_art_bold();
    float size = h * 0.62f, gap = h * 0.9f, total = 0.0f;
    float widths[16];
    for (int i = 0; i < count && i < 16; i++) {
        widths[i] = h * 1.08f + font->CalcTextSizeA(size, FLT_MAX, 0.0f, prompts[i].label).x;
        total += widths[i] + (i ? gap : 0.0f);
    }
    float x = right ? p.x - total : p.x;
    for (int i = 0; i < count && i < 16; i++) {
        const char *name = pad ? prompts[i].pad : prompts[i].key;
        NwRhiTexture *icon = name ? nw_art_icon(name) : NULL;
        if (icon) {
            dl->AddImage((ImTextureID)nw_rhi_texture_ui(icon), ImVec2(x, p.y), ImVec2(x + h, p.y + h));
        } else {
            /* a key cap with the name */
            dl->AddRectFilled(ImVec2(x, p.y + h * 0.1f), ImVec2(x + h, p.y + h * 0.9f), nw_c(0xe8e8f0), h * 0.18f);
        }
        dl->AddText(font, size, ImVec2(x + h * 1.08f, p.y + (h - size) * 0.5f), nw_c(NW_C_CREAM, 0.92f), prompts[i].label);
        x += widths[i] + gap;
    }
    return total;
}

bool nw_art_pad_used(void)
{
    if (!nw_artui.pad_known) {
#ifdef NW_UWP
        nw_artui.pad = true;
#else
        int n = 0;
        SDL_JoystickID *pads = SDL_GetGamepads(&n);
        SDL_free(pads);
        nw_artui.pad = n > 0;
#endif
        nw_artui.pad_known = true;
    }
    return nw_artui.pad;
}

void nw_art_input(const SDL_Event *e)
{
    switch (e->type) {
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    case SDL_EVENT_GAMEPAD_AXIS_MOTION:
        if (e->type == SDL_EVENT_GAMEPAD_AXIS_MOTION && SDL_abs(e->gaxis.value) < 16000) {
            break;
        }
        nw_artui.pad = nw_artui.pad_known = true;
        break;
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
        nw_artui.pad = false;
        nw_artui.pad_known = true;
        break;
    default:
        break;
    }
}

void nw_art_shutdown(void)
{
    for (int i = 0; i < nw_artui.nicons; i++) {
        if (nw_artui.icons[i].tex) {
            nw_rhi_texture_destroy(nw_artui.icons[i].tex);
        }
    }
    nw_artui.nicons = 0;
}

/* ---- setup widgets ---- */

bool nw_art_button(const char *label, ImVec2 size, int kind)
{
    ImVec2 p = ImGui::GetCursorScreenPos();
    bool pressed = ImGui::InvisibleButton(label, size);
    bool hovered = ImGui::IsItemHovered(), focused = ImGui::IsItemFocused(), held = ImGui::IsItemActive();
    float alpha = ImGui::GetStyle().Alpha;
    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImVec2 a = p, b(p.x + size.x, p.y + size.y);
    if (held) {
        a.y += 1.5f;
        b.y += 1.5f;
    }
    float r = size.y * 0.24f;
    bool hot = hovered || focused;
    if (hot) {
        dl->AddRect(ImVec2(a.x - 4, a.y - 4), ImVec2(b.x + 4, b.y + 4), nw_c(NW_C_GOLD, 0.35f * alpha), r + 4, 6.0f);
    }
    dl->AddRectFilled(ImVec2(a.x, a.y + size.y * 0.08f), ImVec2(b.x, b.y + size.y * 0.08f), nw_c(0, 0.35f * alpha), r);
    if (kind == 1) {
        dl->AddRectFilled(a, b, nw_c(NW_C_ORANGE, alpha), r);
        int v0 = dl->VtxBuffer.Size;
        dl->AddRectFilled(a, b, nw_c(0xffffff, alpha), r);
        ImGui::ShadeVertsLinearColorGradientKeepAlpha(dl, v0, dl->VtxBuffer.Size, a, ImVec2(a.x, b.y),
                                                      nw_c(hot ? 0xffd25a : NW_C_ORANGE_HI), nw_c(hot ? 0xff7a10 : NW_C_ORANGE_DEEP));
        dl->AddRect(a, b, nw_c(0x7a2a00, alpha), r, 1.5f);
    } else {
        dl->AddRectFilled(a, b, nw_c(hot ? 0x2a3470 : 0x1a2150, 0.95f * alpha), r);
        dl->AddRect(a, b, nw_c(hot ? NW_C_GOLD : 0x56629f, alpha), r, hot ? 2.0f : 1.2f);
    }
    if (focused) {
        dl->AddRect(a, b, nw_c(NW_C_GOLD, alpha), r, 2.5f);
    }
    nw_ui_push_font(NW_UI_BOLD, 1.0f);
    ImFont *font = ImGui::GetFont();
    float fs = ImGui::GetFontSize();
    nw_ui_pop_font();
    const char *end = ImGui::FindRenderedTextEnd(label);
    ImVec2 ts = font->CalcTextSizeA(fs, FLT_MAX, 0.0f, label, end);
    ImVec2 tp((a.x + b.x - ts.x) * 0.5f, (a.y + b.y - ts.y) * 0.5f);
    dl->AddText(font, fs, tp, kind == 1 ? nw_c(NW_C_INK, alpha) : nw_c(NW_C_CREAM, alpha), label, end);
    return pressed;
}

void nw_art_progress(ImVec2 size, float frac, const char *overlay)
{
    ImVec2 a = ImGui::GetCursorScreenPos(), b(a.x + size.x, a.y + size.y);
    ImGui::Dummy(size);
    ImDrawList *dl = ImGui::GetWindowDrawList();
    float r = size.y * 0.5f;
    dl->AddRectFilled(a, b, nw_c(0x05081a, 0.9f), r);
    dl->AddRect(a, b, nw_c(0x56629f), r, 1.2f);
    if (frac < 0.0f) {
        float t = (float)ImGui::GetTime(), w = size.x * 0.28f, x = a.x + fmodf(t * 0.6f, 1.0f) * (size.x + w) - w;
        dl->PushClipRect(a, b, true);
        dl->AddRectFilledMultiColor(ImVec2(x, a.y + 3), ImVec2(x + w, b.y - 3), nw_c(NW_C_ORANGE, 0.0f), nw_c(NW_C_ORANGE),
                                    nw_c(NW_C_ORANGE), nw_c(NW_C_ORANGE, 0.0f));
        dl->PopClipRect();
    } else if (frac > 0.0f) {
        ImVec2 e(a.x + 3 + (size.x - 6) * SDL_min(frac, 1.0f), b.y - 3);
        int v0 = dl->VtxBuffer.Size;
        dl->AddRectFilled(ImVec2(a.x + 3, a.y + 3), e, nw_c(0xffffff), r);
        ImGui::ShadeVertsLinearColorGradientKeepAlpha(dl, v0, dl->VtxBuffer.Size, a, ImVec2(a.x, b.y), nw_c(0xffd25a),
                                                      nw_c(NW_C_ORANGE_DEEP));
    }
    if (overlay && overlay[0]) {
        ImVec2 ts = ImGui::CalcTextSize(overlay);
        dl->AddText(ImVec2((a.x + b.x - ts.x) * 0.5f, (a.y + b.y - ts.y) * 0.5f), nw_c(0xffffff), overlay);
    }
}

void nw_art_steps(ImDrawList *dl, float cx, float y, float width, float h, int step)
{
    static const char *const names[3] = { "Find disc", "Import", "Play" };
    ImFont *font = nw_art_bold();
    float r = h * 0.5f, x0 = cx - width * 0.5f, dx = width / 2.0f;
    for (int i = 0; i < 2; i++) {
        ImVec2 p0(x0 + dx * (float)i + r * 1.3f, y + r), p1(x0 + dx * (float)(i + 1) - r * 1.3f, y + r);
        dl->AddLine(p0, p1, nw_c(0x56629f), h * 0.08f);
        if (i < step) {
            dl->AddLine(p0, p1, nw_c(NW_C_GREEN), h * 0.08f);
        }
    }
    for (int i = 0; i < 3; i++) {
        ImVec2 c(x0 + dx * (float)i, y + r);
        bool done = i < step, cur = i == step;
        if (cur) {
            dl->AddCircleFilled(c, r * 1.35f, nw_c(NW_C_ORANGE, 0.25f), 32);
        }
        dl->AddCircleFilled(c, r, done ? nw_c(NW_C_GREEN) : cur ? nw_c(NW_C_ORANGE) : nw_c(0x1a2150), 32);
        dl->AddCircle(c, r, cur ? nw_c(NW_C_GOLD) : nw_c(0x56629f), 32, h * 0.06f);
        if (done) {
            dl->PathLineTo(ImVec2(c.x - r * 0.42f, c.y + r * 0.02f));
            dl->PathLineTo(ImVec2(c.x - r * 0.1f, c.y + r * 0.34f));
            dl->PathLineTo(ImVec2(c.x + r * 0.45f, c.y - r * 0.32f));
            dl->PathStroke(nw_c(0xffffff), h * 0.11f);
        } else {
            char n[2] = { (char)('1' + i), 0 };
            ImVec2 ts = font->CalcTextSizeA(h * 0.6f, FLT_MAX, 0.0f, n);
            dl->AddText(font, h * 0.6f, ImVec2(c.x - ts.x * 0.5f, c.y - ts.y * 0.5f), cur ? nw_c(NW_C_INK) : nw_c(NW_C_MUTED), n);
        }
        ImVec2 ts = font->CalcTextSizeA(h * 0.5f, FLT_MAX, 0.0f, names[i]);
        dl->AddText(font, h * 0.5f, ImVec2(c.x - ts.x * 0.5f, y + h * 1.25f), cur ? nw_c(NW_C_CREAM) : nw_c(NW_C_MUTED, done ? 1.0f : 0.8f),
                    names[i]);
    }
}

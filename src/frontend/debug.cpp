#include "frontend/debug.h"

#include <float.h>
#include <math.h>
#include <stdarg.h>

#include "app.h"
#include "frontend/artui.h"
#include "frontend/ui.h"
#include "imgui.h"
#include "imgui_internal.h"

extern "C" {
#include "platform/paths.h"
}

#define NW_DEBUG_ITEMS 64

typedef enum NwDebugKind {
    NW_DEBUG_PANEL,
    NW_DEBUG_TOGGLE,
    NW_DEBUG_ACTION,
} NwDebugKind;

typedef struct NwDebugItem {
    NwDebugKind kind;
    const char *menu, *label;
    NwDebugFn fn;
    void *user;
    bool open;
    bool placed; /* its window has a position */
    bool focus;  /* chosen in the menu: brought to the front */
    bool *value;
    SDL_Scancode key;
} NwDebugItem;

static struct {
    bool init;
    bool shown;
    char *root;
    NwDebugItem items[NW_DEBUG_ITEMS];
    uint32_t nitems;
    uint32_t placed; /* panels opened so far, the next one's cascade slot */
} nw_debug;

static NwDebugItem *nw_debug_add(NwDebugKind kind, const char *menu, const char *label)
{
    SDL_assert(nw_debug.nitems < NW_DEBUG_ITEMS);
    if (nw_debug.nitems == NW_DEBUG_ITEMS) {
        return NULL;
    }
    NwDebugItem *item = &nw_debug.items[nw_debug.nitems++];
    *item = NwDebugItem{ kind, menu, label, NULL, NULL, false, false, false, NULL, SDL_SCANCODE_UNKNOWN };
    return item;
}

void nw_debug_panel(const char *menu, const char *title, NwDebugFn draw, void *user, bool open)
{
    NwDebugItem *item = nw_debug_add(NW_DEBUG_PANEL, menu, title);
    if (item) {
        item->fn = draw;
        item->user = user;
        item->open = open;
    }
}

void nw_debug_panel_key(const char *menu, const char *title, SDL_Scancode key, NwDebugFn draw, void *user)
{
    NwDebugItem *item = nw_debug_add(NW_DEBUG_PANEL, menu, title);
    if (item) {
        item->fn = draw;
        item->user = user;
        item->key = key;
    }
}

void nw_debug_toggle(const char *menu, const char *label, bool *value)
{
    NwDebugItem *item = nw_debug_add(NW_DEBUG_TOGGLE, menu, label);
    if (item) {
        item->value = value;
    }
}

void nw_debug_action(const char *menu, const char *label, SDL_Scancode key, NwDebugFn run, void *user)
{
    NwDebugItem *item = nw_debug_add(NW_DEBUG_ACTION, menu, label);
    if (item) {
        item->fn = run;
        item->user = user;
        item->key = key;
    }
}

/* The launcher's rows (nw_debug_rows_begin): every widget a full-width row, its label left and its control right, the
   focused one lit; tooltips go to the description instead of a popup. */
static struct {
    bool on;
    bool same_line; /* nw_debug_same_line came last: the next widget shares a line (a table) and stays compact */
    bool hot;       /* the last row drawn is focused or hovered */
    char desc[512];
    bool focused_desc; /* desc is the focused row's (a hovered one's only when nothing is focused) */
    ImGuiID first;     /* the first row's ID */
} nw_rows;

void nw_debug_rows_begin(void)
{
    nw_rows.on = true;
    nw_rows.same_line = false;
    nw_rows.hot = false;
    nw_rows.desc[0] = '\0';
    nw_rows.focused_desc = false;
    nw_rows.first = 0;
}

unsigned nw_debug_rows_first(void)
{
    return nw_rows.first;
}

const char *nw_debug_rows_end(void)
{
    nw_rows.on = false;
    return nw_rows.desc;
}

static bool nw_rows_here(void)
{
    bool row = nw_rows.on && !nw_rows.same_line;
    nw_rows.same_line = false;
    return row;
}

static ImU32 nw_rows_col(unsigned rgb, float a)
{
    return nw_c(rgb, a * ImGui::GetStyle().Alpha);
}

static void nw_rows_hint(bool focused, const char *text)
{
    if (focused || !nw_rows.focused_desc) {
        SDL_strlcpy(nw_rows.desc, text, sizeof(nw_rows.desc));
        nw_rows.focused_desc = focused;
    }
}

/* -1 / +1 for left / right on the keyboard, the d-pad or the stick, repeating. */
static int nw_rows_dir(void)
{
    if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow) || ImGui::IsKeyPressed(ImGuiKey_GamepadDpadLeft) ||
        ImGui::IsKeyPressed(ImGuiKey_GamepadLStickLeft)) {
        return -1;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_RightArrow) || ImGui::IsKeyPressed(ImGuiKey_GamepadDpadRight) ||
        ImGui::IsKeyPressed(ImGuiKey_GamepadLStickRight)) {
        return 1;
    }
    return 0;
}

typedef struct NwRow {
    ImVec2 a, b;      /* the row */
    ImVec2 c0, c1;    /* the control's box on the right */
    bool pressed, focused, hovered, hot;
} NwRow;

/* An invisible button over the whole row, its highlight and its label. */
static NwRow nw_row(const char *label, const char *hint)
{
    NwRow r;
    float fs = ImGui::GetFontSize(), w = ImGui::GetContentRegionAvail().x, h = fs * 2.0f;
    r.a = ImGui::GetCursorScreenPos();
    r.b = ImVec2(r.a.x + w, r.a.y + h);
    r.pressed = ImGui::InvisibleButton(label, ImVec2(w, h), ImGuiButtonFlags_EnableNav);
    if (!nw_rows.first) {
        nw_rows.first = ImGui::GetItemID();
    }
    r.focused = ImGui::IsItemFocused();
    r.hovered = ImGui::IsItemHovered();
    r.hot = r.focused || r.hovered;
    ImDrawList *dl = ImGui::GetWindowDrawList();
    float round = h * 0.18f;
    if (r.focused) {
        dl->AddRectFilledMultiColor(r.a, r.b, nw_rows_col(NW_C_ORANGE, 0.95f), nw_rows_col(NW_C_ORANGE_DEEP, 0.55f),
                                    nw_rows_col(NW_C_ORANGE_DEEP, 0.55f), nw_rows_col(NW_C_ORANGE, 0.95f));
        dl->AddRect(r.a, r.b, nw_rows_col(NW_C_GOLD, 0.9f), round, fs * 0.08f);
    } else if (r.hovered) {
        dl->AddRectFilled(r.a, r.b, nw_rows_col(0xffffff, 0.08f), round);
    } else {
        dl->AddLine(ImVec2(r.a.x + fs * 0.4f, r.b.y - 0.5f), ImVec2(r.b.x - fs * 0.4f, r.b.y - 0.5f),
                    nw_rows_col(0xffffff, 0.06f));
    }
    const char *end = ImGui::FindRenderedTextEnd(label);
    ImVec2 ts = ImGui::CalcTextSize(label, end);
    dl->AddText(ImVec2(r.a.x + fs * 0.7f, r.a.y + (h - ts.y) * 0.5f), nw_rows_col(r.focused ? 0xffffff : NW_C_CREAM, 1.0f),
                label, end);
    float cw = SDL_min(w * 0.46f, fs * 15.0f);
    r.c0 = ImVec2(r.b.x - cw - fs * 0.5f, r.a.y + h * 0.16f);
    r.c1 = ImVec2(r.b.x - fs * 0.5f, r.b.y - h * 0.16f);
    nw_rows.hot = r.hot;
    if (r.hot && hint) {
        nw_rows_hint(r.focused, hint);
    }
    return r;
}

/* "<  value  >" in the control box. */
static void nw_row_spinner(const NwRow *r, const char *value)
{
    ImDrawList *dl = ImGui::GetWindowDrawList();
    float h = r->c1.y - r->c0.y, aw = h * 0.28f;
    dl->AddRectFilled(r->c0, r->c1, nw_rows_col(0x000000, r->focused ? 0.35f : 0.28f), h * 0.25f);
    ImU32 arrow = nw_rows_col(r->focused ? 0xffffff : NW_C_ORANGE, 1.0f);
    float cy = (r->c0.y + r->c1.y) * 0.5f;
    dl->AddTriangleFilled(ImVec2(r->c0.x + h * 0.3f, cy), ImVec2(r->c0.x + h * 0.3f + aw, cy - aw),
                          ImVec2(r->c0.x + h * 0.3f + aw, cy + aw), arrow);
    dl->AddTriangleFilled(ImVec2(r->c1.x - h * 0.3f, cy), ImVec2(r->c1.x - h * 0.3f - aw, cy - aw),
                          ImVec2(r->c1.x - h * 0.3f - aw, cy + aw), arrow);
    ImVec2 ts = ImGui::CalcTextSize(value);
    float room = r->c1.x - r->c0.x - h * 1.6f;
    ImVec2 at((r->c0.x + r->c1.x - SDL_min(ts.x, room)) * 0.5f, cy - ts.y * 0.5f);
    dl->PushClipRect(ImVec2(r->c0.x + h * 0.8f, r->c0.y), ImVec2(r->c1.x - h * 0.8f, r->c1.y), true);
    dl->AddText(at, nw_rows_col(0xffffff, 1.0f), value);
    dl->PopClipRect();
}

/* -1 / +1 when the row is clicked on its arrows or left / right is pressed on it, +1 for a press elsewhere. */
static int nw_row_step(const NwRow *r)
{
    int d = r->focused ? nw_rows_dir() : 0;
    if (r->pressed) {
        float mx = ImGui::GetIO().MousePos.x, h = r->c1.y - r->c0.y;
        bool mouse = ImGui::GetIO().MouseClicked[0] || ImGui::GetIO().MouseReleased[0];
        d = mouse && mx >= r->c0.x && mx < r->c0.x + h * 1.0f ? -1 : 1;
    }
    return d;
}

static bool nw_row_combo(const char *label, int *index, const char *const *items, int count)
{
    NwRow r = nw_row(label, "Left / right: change");
    int d = count > 0 ? nw_row_step(&r) : 0;
    bool changed = false;
    if (d) {
        *index = ((*index + d) % count + count) % count;
        changed = true;
    }
    nw_row_spinner(&r, count > 0 && *index >= 0 && *index < count ? items[*index] : "-");
    return changed;
}

static void nw_row_switch(const NwRow *r, bool on)
{
    ImDrawList *dl = ImGui::GetWindowDrawList();
    float h = r->c1.y - r->c0.y, w = h * 2.1f;
    ImVec2 a(r->c1.x - w, r->c0.y), b(r->c1.x, r->c1.y);
    dl->AddRectFilled(a, b, on ? nw_rows_col(NW_C_GREEN, 1.0f) : nw_rows_col(0x000000, 0.45f), h * 0.5f);
    dl->AddRect(a, b, nw_rows_col(0xffffff, on ? 0.6f : 0.25f), h * 0.5f, 1.5f);
    float k = h * 0.5f, x = on ? b.x - k : a.x + k;
    dl->AddCircleFilled(ImVec2(x, (a.y + b.y) * 0.5f), k * 0.78f, nw_rows_col(0xffffff, 1.0f), 24);
    const char *text = on ? "ON" : "OFF";
    ImVec2 ts = ImGui::CalcTextSize(text);
    dl->AddText(ImVec2(a.x - ts.x - h * 0.4f, (a.y + b.y - ts.y) * 0.5f), nw_rows_col(on ? NW_C_GOLD : NW_C_MUTED, 1.0f),
                text);
}

static bool nw_row_checkbox(const char *label, bool *value)
{
    NwRow r = nw_row(label, "A / Enter: switch on or off");
    int d = r.focused ? nw_rows_dir() : 0;
    bool changed = false;
    if (r.pressed || (d < 0 && *value) || (d > 0 && !*value)) {
        *value = !*value;
        changed = true;
    }
    nw_row_switch(&r, *value);
    return changed;
}

static bool nw_row_button(const char *label)
{
    NwRow r = nw_row(label, "A / Enter: do it");
    ImDrawList *dl = ImGui::GetWindowDrawList();
    float h = r.c1.y - r.c0.y, cy = (r.c0.y + r.c1.y) * 0.5f, k = h * 0.22f;
    ImU32 c = nw_rows_col(r.focused ? 0xffffff : NW_C_ORANGE, 1.0f);
    dl->AddLine(ImVec2(r.c1.x - k * 2.2f, cy - k), ImVec2(r.c1.x - k * 1.2f, cy), c, k * 0.5f);
    dl->AddLine(ImVec2(r.c1.x - k * 1.2f, cy), ImVec2(r.c1.x - k * 2.2f, cy + k), c, k * 0.5f);
    return r.pressed;
}

/* value in lo..hi as 0..1, log or linear. */
static float nw_row_frac(float v, float lo, float hi, bool log)
{
    if (log && lo > 0.0f && hi > lo) {
        return SDL_clamp((logf(v) - logf(lo)) / (logf(hi) - logf(lo)), 0.0f, 1.0f);
    }
    return hi > lo ? SDL_clamp((v - lo) / (hi - lo), 0.0f, 1.0f) : 0.0f;
}

static float nw_row_unfrac(float f, float lo, float hi, bool log)
{
    if (log && lo > 0.0f && hi > lo) {
        return expf(logf(lo) + f * (logf(hi) - logf(lo)));
    }
    return lo + f * (hi - lo);
}

/* A bar in the control box; the mouse drags it, left / right step it. Returns the new fraction or -1. */
static float nw_row_bar(const NwRow *r, float frac, const char *text)
{
    ImDrawList *dl = ImGui::GetWindowDrawList();
    float h = r->c1.y - r->c0.y, tw = ImGui::CalcTextSize("00000000").x;
    ImVec2 a(r->c0.x, (r->c0.y + r->c1.y) * 0.5f - h * 0.14f), b(r->c1.x - tw - h * 0.3f, a.y + h * 0.28f);
    dl->AddRectFilled(a, b, nw_rows_col(0x000000, 0.45f), h);
    dl->AddRectFilledMultiColor(a, ImVec2(a.x + (b.x - a.x) * frac, b.y), nw_rows_col(NW_C_GOLD, 1.0f),
                                nw_rows_col(NW_C_ORANGE, 1.0f), nw_rows_col(NW_C_ORANGE, 1.0f), nw_rows_col(NW_C_GOLD, 1.0f));
    dl->AddCircleFilled(ImVec2(a.x + (b.x - a.x) * frac, (a.y + b.y) * 0.5f), h * 0.3f,
                        nw_rows_col(r->focused ? 0xffffff : NW_C_CREAM, 1.0f), 20);
    ImVec2 ts = ImGui::CalcTextSize(text);
    dl->AddText(ImVec2(r->c1.x - ts.x, (r->c0.y + r->c1.y - ts.y) * 0.5f), nw_rows_col(0xffffff, 1.0f), text);
    if (ImGui::IsItemActive() && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        float mx = ImGui::GetIO().MousePos.x;
        if (mx >= a.x - h && mx <= b.x + h) {
            return SDL_clamp((mx - a.x) / SDL_max(b.x - a.x, 1.0f), 0.0f, 1.0f);
        }
    }
    return -1.0f;
}

static bool nw_row_slider(const char *label, float *value, float lo, float hi, bool log)
{
    NwRow r = nw_row(label, "Left / right: adjust");
    char text[32];
    SDL_snprintf(text, sizeof(text), "%.3g", *value);
    float f = nw_row_bar(&r, nw_row_frac(*value, lo, hi, log), text);
    float old = *value;
    if (f >= 0.0f) {
        *value = nw_row_unfrac(f, lo, hi, log);
    }
    int d = r.focused ? nw_rows_dir() : 0;
    if (d) {
        *value = nw_row_unfrac(SDL_clamp(nw_row_frac(*value, lo, hi, log) + 0.02f * (float)d, 0.0f, 1.0f), lo, hi, log);
    }
    return *value != old;
}

static bool nw_row_slider_int(const char *label, int *value, int lo, int hi)
{
    NwRow r = nw_row(label, "Left / right: adjust");
    char text[32];
    SDL_snprintf(text, sizeof(text), "%d", *value);
    float f = nw_row_bar(&r, hi > lo ? (float)(*value - lo) / (float)(hi - lo) : 0.0f, text);
    int old = *value;
    if (f >= 0.0f) {
        *value = lo + (int)lroundf(f * (float)(hi - lo));
    }
    int d = r.focused ? nw_rows_dir() : 0;
    if (d) {
        *value = SDL_clamp(*value + d * SDL_max((hi - lo) / 100, 1), lo, hi);
    }
    return *value != old;
}

char *nw_debug_file(const char *folder, const char *ext)
{
    char *dir = nw_debug.root ? nw_paths_dir(nw_debug.root, folder, true) : NULL;
    char *path = NULL;
    SDL_Time now;
    SDL_DateTime t;
    if (dir && SDL_GetCurrentTime(&now) && SDL_TimeToDateTime(now, &t, true)) {
        SDL_asprintf(&path, "%s%04d%02d%02d-%02d%02d%02d-%03d.%s", dir, t.year, t.month, t.day, t.hour, t.minute,
                     t.second, t.nanosecond / 1000000, ext);
    }
    SDL_free(dir);
    return path;
}

void nw_debug_text(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    if (nw_rows.on) {
        nw_rows.same_line = false;
        ImGui::PushStyleColor(ImGuiCol_Text, nw_cv(NW_C_MUTED));
        ImGui::Indent(ImGui::GetFontSize() * 0.7f);
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextV(fmt, ap);
        ImGui::PopTextWrapPos();
        ImGui::Unindent(ImGui::GetFontSize() * 0.7f);
        ImGui::PopStyleColor();
    } else {
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 24.0f);
        ImGui::TextV(fmt, ap);
        ImGui::PopTextWrapPos();
    }
    va_end(ap);
}

void nw_debug_tooltip(const char *text)
{
    if (nw_rows.on) {
        if (ImGui::IsItemFocused() || ImGui::IsItemHovered()) {
            nw_rows_hint(ImGui::IsItemFocused(), text);
        }
        return;
    }
    ImGui::SetItemTooltip("%s", text);
}

void nw_debug_label(const char *text)
{
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(text);
}

bool nw_debug_button(const char *label)
{
    if (nw_rows_here()) {
        return nw_row_button(label);
    }
    return ImGui::Button(label);
}

void nw_debug_enable_begin(bool enabled)
{
    ImGui::BeginDisabled(!enabled);
}

void nw_debug_enable_end(void)
{
    ImGui::EndDisabled();
}

bool nw_debug_checkbox(const char *label, bool *value)
{
    if (nw_rows_here()) {
        return nw_row_checkbox(label, value);
    }
    return ImGui::Checkbox(label, value);
}

void nw_debug_indent(bool in)
{
    if (in) {
        ImGui::Indent();
    } else {
        ImGui::Unindent();
    }
}

void nw_debug_same_line(float column)
{
    nw_rows.same_line = nw_rows.on;
    ImGui::SameLine(column * ImGui::GetFontSize() * (nw_rows.on ? 1.15f : 1.0f));
}

bool nw_debug_slider(const char *label, float *value, float lo, float hi, bool log)
{
    if (nw_rows_here()) {
        return nw_row_slider(label, value, lo, hi, log);
    }
    return ImGui::SliderFloat(label, value, lo, hi, "%.3g", log ? ImGuiSliderFlags_Logarithmic : 0);
}

bool nw_debug_slider_int(const char *label, int *value, int lo, int hi)
{
    if (nw_rows_here()) {
        return nw_row_slider_int(label, value, lo, hi);
    }
    return ImGui::SliderInt(label, value, lo, hi, "%d", ImGuiSliderFlags_AlwaysClamp);
}

bool nw_debug_combo(const char *label, int *index, const char *const *items, int count)
{
    if (nw_rows_here()) {
        return nw_row_combo(label, index, items, count);
    }
    return ImGui::Combo(label, index, items, count);
}

bool nw_debug_filter(const char *hint, char *buf, size_t size)
{
    ImGui::PushID(hint);
    ImGui::SetNextItemWidth(-FLT_MIN);
    bool changed = ImGui::InputTextWithHint("##filter", hint, buf, size);
    ImGui::PopID();
    return changed;
}

void nw_debug_list_begin(const char *id, int rows)
{
    float height = ImGui::GetTextLineHeightWithSpacing() * (float)rows + ImGui::GetStyle().WindowPadding.y * 2;
    ImGui::BeginChild(id, ImVec2(ImGui::GetFontSize() * 20, height), ImGuiChildFlags_Borders);
}

void nw_debug_list_end(void)
{
    ImGui::EndChild();
}

void nw_debug_heading(const char *text)
{
    if (nw_rows.on) {
        nw_rows.same_line = false;
        ImGui::Dummy(ImVec2(1, ImGui::GetFontSize() * 0.5f));
        nw_ui_push_font(NW_UI_BOLD, 1.0f);
        ImGui::PushStyleColor(ImGuiCol_Text, nw_cv(NW_C_GOLD));
        ImGui::Indent(ImGui::GetFontSize() * 0.7f);
        ImGui::TextUnformatted(text);
        ImGui::Unindent(ImGui::GetFontSize() * 0.7f);
        ImGui::PopStyleColor();
        nw_ui_pop_font();
        return;
    }
    ImGui::SeparatorText(text);
}

bool nw_debug_selectable(const char *label, bool selected)
{
    return ImGui::Selectable(label, selected);
}

unsigned nw_debug_items(void)
{
    return nw_debug.nitems;
}

bool nw_debug_item(unsigned i, NwDebugItemKind *kind, const char **menu, const char **label)
{
    if (i >= nw_debug.nitems) {
        return false;
    }
    const NwDebugItem *item = &nw_debug.items[i];
    *kind = item->kind == NW_DEBUG_PANEL ? NW_DEBUG_ITEM_PANEL
            : item->kind == NW_DEBUG_TOGGLE ? NW_DEBUG_ITEM_TOGGLE
                                            : NW_DEBUG_ITEM_ACTION;
    *menu = item->menu;
    *label = item->label;
    return true;
}

void nw_debug_item_draw(unsigned i)
{
    if (i >= nw_debug.nitems) {
        return;
    }
    NwDebugItem *item = &nw_debug.items[i];
    ImGui::PushID((int)i);
    if (item->kind == NW_DEBUG_TOGGLE) {
        nw_debug_checkbox(item->label, item->value);
    } else if (item->kind == NW_DEBUG_ACTION) {
        if (nw_debug_button(item->label)) {
            item->fn(item->user);
        }
    } else {
        item->fn(item->user);
    }
    ImGui::PopID();
}

/* menu is the path's first len characters or a submenu of it. */
static bool nw_debug_under(const char *menu, const char *path, size_t len)
{
    return len == 0 || (SDL_strncmp(menu, path, len) == 0 && (menu[len] == '\0' || menu[len] == '/'));
}

/* A toggle is an outlined checkbox, a panel ends in "..." and comes to the front when chosen. */
static void nw_debug_menu_item(NwDebugItem *item)
{
    if (item->kind == NW_DEBUG_TOGGLE) {
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
        ImGui::PushStyleColor(ImGuiCol_Border, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        ImGui::Checkbox(item->label, item->value);
        ImGui::PopStyleColor();
        ImGui::PopStyleVar();
    } else if (item->kind == NW_DEBUG_PANEL) {
        char label[80];
        SDL_snprintf(label, sizeof(label), "%s...", item->label);
        if (ImGui::MenuItem(label, item->key ? SDL_GetScancodeName(item->key) : NULL)) {
            item->open = item->focus = true;
        }
    } else if (ImGui::MenuItem(item->label, item->key ? SDL_GetScancodeName(item->key) : NULL)) {
        item->fn(item->user);
    }
}

/* The items and submenus under path's first len characters, each where its first item was registered; menu names are
   bold. */
static void nw_debug_menu(const char *path, size_t len)
{
    for (uint32_t i = 0; i < nw_debug.nitems; i++) {
        NwDebugItem *item = &nw_debug.items[i];
        if (!nw_debug_under(item->menu, path, len)) {
            continue;
        }
        if (len && item->menu[len] == '\0') {
            nw_debug_menu_item(item);
            continue;
        }
        const char *name = item->menu + (len ? len + 1 : 0);
        const char *end = SDL_strchr(name, '/');
        size_t n = end ? (size_t)(end - name) : SDL_strlen(name), sub = (size_t)(name - item->menu) + n;
        bool first = true;
        for (uint32_t j = 0; j < i && first; j++) {
            first = !nw_debug_under(nw_debug.items[j].menu, item->menu, sub);
        }
        char label[64];
        SDL_strlcpy(label, name, SDL_min(n + 1, sizeof(label)));
        if (!first) {
            continue;
        }
        nw_ui_push_font(NW_UI_BOLD, 1.0f);
        bool open = ImGui::BeginMenu(label);
        nw_ui_pop_font();
        if (open) {
            nw_debug_menu(item->menu, sub);
            ImGui::EndMenu();
        }
    }
}

/* A panel first opens on a cascade from the top left, in the order panels open; every frame it is kept inside the
   work area (the screen below the menu bar), no larger than it, scrolling what does not fit. */
static void nw_debug_panels(void)
{
    const ImGuiViewport *vp = ImGui::GetMainViewport();
    float margin = ImGui::GetStyle().WindowPadding.x, step = ImGui::GetFrameHeight() * 1.5f;
    ImVec2 lo = vp->WorkPos, hi(vp->WorkPos.x + vp->WorkSize.x, vp->WorkPos.y + vp->WorkSize.y);
    ImVec2 room(SDL_max(vp->WorkSize.x - 2 * margin, step), SDL_max(vp->WorkSize.y - 2 * margin, step));
    uint32_t rows = SDL_max((uint32_t)(room.y / 3 / step), 1u);
    for (uint32_t i = 0; i < nw_debug.nitems; i++) {
        NwDebugItem *item = &nw_debug.items[i];
        if (item->kind != NW_DEBUG_PANEL || !item->open) {
            continue;
        }
        char name[128];
        SDL_snprintf(name, sizeof(name), "%s###%s/%s", item->label, item->menu, item->label);
        if (!item->placed) {
            uint32_t k = nw_debug.placed++;
            float x = step * (float)(k % rows + k / rows * 4), y = step * (float)(k % rows);
            ImGui::SetNextWindowPos(ImVec2(lo.x + margin + x, lo.y + margin + y));
            item->placed = true;
        }
        if (item->focus) {
            ImGui::SetNextWindowFocus();
            item->focus = false;
        }
        ImGui::SetNextWindowSizeConstraints(ImVec2(0, 0), room);
        bool shown = ImGui::Begin(name, &item->open, ImGuiWindowFlags_AlwaysAutoResize);
        ImVec2 pos = ImGui::GetWindowPos(), size = ImGui::GetWindowSize();
        ImVec2 fit(SDL_max(SDL_min(pos.x, hi.x - size.x), lo.x), SDL_max(SDL_min(pos.y, hi.y - size.y), lo.y));
        if (fit.x != pos.x || fit.y != pos.y) {
            ImGui::SetWindowPos(fit);
        }
        if (shown) {
            ImGui::PushItemWidth(ImGui::GetFontSize() * 12.0f);
            item->fn(item->user);
            ImGui::PopItemWidth();
        }
        ImGui::End();
    }
}

static void nw_debug_set_shown(bool shown)
{
    nw_debug.shown = shown;
    ImGui::GetIO().ClearInputKeys();
    ImGui::GetIO().ClearInputMouse();
}

/* Closing hides the overlay too when no other panel is open. */
static void nw_debug_key_panel(NwDebugItem *panel)
{
    panel->open = !nw_debug.shown || !panel->open;
    bool other = false;
    for (uint32_t i = 0; i < nw_debug.nitems; i++) {
        other = other || (nw_debug.items[i].kind == NW_DEBUG_PANEL && nw_debug.items[i].open);
    }
    if (panel->open != nw_debug.shown && (panel->open || !other)) {
        nw_debug_set_shown(panel->open);
    }
}

bool nw_debug_init(NwPlatform *p, const char *data_root)
{
    if (!nw_ui_init(p, true)) {
        return false;
    }
    nw_debug.root = SDL_strdup(data_root);
    /* Mouse only: the keyboard and the pad are the game's. */
    ImGui::GetIO().ConfigFlags &= ~(ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad);
    nw_debug.init = true;
    return true;
}

void nw_debug_shutdown(void)
{
    if (nw_debug.init) {
        nw_ui_shutdown();
    }
    SDL_free(nw_debug.root);
    nw_debug.root = NULL;
    nw_debug.init = false;
    nw_debug.shown = false;
    nw_debug.nitems = 0;
    nw_debug.placed = 0;
}

bool nw_debug_event(const SDL_Event *e)
{
    if (!nw_debug.init) {
        return false;
    }
    if (e->type == SDL_EVENT_KEY_DOWN && e->key.scancode == SDL_SCANCODE_F1) {
        if (!e->key.repeat) {
            nw_debug_set_shown(!nw_debug.shown);
        }
        return true;
    }
    for (uint32_t i = 0; e->type == SDL_EVENT_KEY_DOWN && i < nw_debug.nitems; i++) {
        NwDebugItem *item = &nw_debug.items[i];
        if (item->kind == NW_DEBUG_ACTION && item->key && item->key == e->key.scancode) {
            if (!e->key.repeat) {
                item->fn(item->user);
            }
            return true;
        }
        if (item->kind == NW_DEBUG_PANEL && item->key && item->key == e->key.scancode) {
            if (!e->key.repeat) {
                nw_debug_key_panel(item);
            }
            return true;
        }
    }
    /* Hidden, no ImGui frame drains its input queue. */
    if (!nw_debug.shown) {
        if (e->type >= SDL_EVENT_WINDOW_FIRST && e->type <= SDL_EVENT_WINDOW_LAST) {
            nw_ui_event(e);
        }
        return false;
    }
    nw_ui_event(e);
    const ImGuiIO &io = ImGui::GetIO();
    switch (e->type) {
    case SDL_EVENT_MOUSE_MOTION:
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP:
    case SDL_EVENT_MOUSE_WHEEL:
        return io.WantCaptureMouse;
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP:
    case SDL_EVENT_TEXT_INPUT:
        return io.WantCaptureKeyboard;
    default:
        return false;
    }
}

bool nw_debug_wants_keyboard(void)
{
    return nw_debug.shown && ImGui::GetIO().WantCaptureKeyboard;
}

void nw_debug_show(bool shown)
{
    nw_debug.shown = shown;
    const char *panel = shown ? SDL_getenv("NW_OVERLAY_PANEL") : NULL;
    for (uint32_t i = 0; panel && i < nw_debug.nitems; i++) {
        if (nw_debug.items[i].kind == NW_DEBUG_PANEL && SDL_strcmp(nw_debug.items[i].label, panel) == 0) {
            nw_debug.items[i].open = true;
        }
    }
}

void nw_debug_frame(void)
{
    if (!nw_debug.shown) {
        return;
    }
    nw_ui_begin();
    if (ImGui::BeginMainMenuBar()) {
        char rate[32];
        SDL_snprintf(rate, sizeof(rate), "%.0f fps", ImGui::GetIO().Framerate);
        if (nw_gameart_font()) {
            /* the game's letters, as its own menus */
            float h = ImGui::GetFrameHeight() * 0.92f;
            ImVec2 p = ImGui::GetCursorScreenPos();
            float w = nw_art_text(ImGui::GetWindowDrawList(), ImVec2(p.x, p.y + (ImGui::GetFrameHeight() - h) * 0.5f), h,
                                  NW_INK_ORANGE, "NUWORLD");
            ImGui::Dummy(ImVec2(w + h * 0.3f, ImGui::GetFrameHeight()));
            ImGui::SetItemTooltip("%s", nw_art_app_title());
        } else {
            ImGui::TextDisabled("%s", nw_art_app_title());
        }
        nw_debug_menu("", 0);
        if (ImGui::GetContentRegionAvail().x > ImGui::CalcTextSize(rate).x + ImGui::GetFontSize()) { /* room left */
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize(rate).x);
            ImGui::TextUnformatted(rate);
        }
        ImGui::EndMainMenuBar();
    }
    nw_debug_panels();
    nw_ui_end();
}

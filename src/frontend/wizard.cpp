#include "frontend/wizard.h"

#include <float.h>
#include <math.h>

#include "imgui.h"
#include "imgui_internal.h"

#include "app.h"
#include "frontend/artui.h"
#include "frontend/launcher.h"
#include "frontend/ui.h"

extern "C" {
#include "frontend/import.h"
#include "gfx/rhi.h"
#include "platform/paths.h"
#include "vfs/extract.h"

extern const unsigned char nw_beta_menu_png[];
extern const size_t nw_beta_menu_png_size;
}

#define NW_WIZARD_RESCAN_MS 1000
#define NW_WIZARD_FEEDBACK_URL "https://github.com/denzi-gh/crashwoc-nuworld/issues/new?template=0-beta_feedback.yml"
#define NW_WIZARD_DISCORD_URL "https://discord.gg/kmCPpW4KvJ"
#define NW_WIZARD_EXTENSIONS ".iso;.cue;.bin"

typedef enum NwWizardPage {
    NW_PAGE_PICK,
    NW_PAGE_RUN,
    NW_PAGE_DONE,
    NW_PAGE_FAILED,
    NW_PAGE_WELCOME,
} NwWizardPage;

struct NwWizard {
    SDL_Window *window;
    char *root;
    char *import_dir;
    Uint32 dialog_event; /* file dialog results (+1: a folder's), pushed from the dialog's thread */
    NwWizardPage page;
    NwImport *job;
    const NwManifest *imported;
    char *image;      /* the image being or last imported */
    bool had_disc;    /* a disc was imported before the wizard opened */
    NwImageList list; /* import folder */
    Uint64 scanned;   /* SDL_GetTicks of the last scan, 0 = never */
    bool copying;     /* a picker is open or copying the picked files into the import folder (Xbox) */
    Uint64 copy_since;
    char notice[512];
    char error[512];
    NwRhiTexture *shot; /* the welcome page's debug menu screenshot, NULL = not loaded */
    int shot_w, shot_h;
    bool zoom; /* the screenshot fills the window */
    bool focus; /* focus the page's main button next frame */
#ifdef NW_UWP
    char **usb; /* disc images on removable drives (Xbox): full paths */
    size_t nusb;
    Uint64 usb_scanned;
#endif
};

static const SDL_DialogFileFilter nw_wizard_filters[] = {
    { "Disc images (.iso, .cue, .bin)", "iso;cue;bin" },
    { "All files", "*" },
};

static const char *nw_wizard_basename(const char *path)
{
    const char *name = path;
    for (const char *p = path; *p; p++) {
        if (*p == '/' || *p == '\\') {
            name = p + 1;
        }
    }
    return name;
}

static void nw_wizard_start(NwWizard *w, const char *image)
{
    char *copy = SDL_strdup(image);
    SDL_free(w->image);
    w->image = copy;
    w->notice[0] = '\0';
    w->job = copy ? nw_import_start(copy, w->root, nw_disc_manifests) : NULL;
    if (w->job) {
        w->page = NW_PAGE_RUN;
    } else {
        SDL_strlcpy(w->error, SDL_GetError(), sizeof(w->error));
        w->page = NW_PAGE_FAILED;
    }
    w->focus = true;
}

/* The picked file to import: a .cue before its .bin, else an .iso, else the first. */
static const char *nw_wizard_best(const char *const *files)
{
    static const char *const order[] = { ".cue", ".iso", ".bin" };
    for (size_t k = 0; k < SDL_arraysize(order); k++) {
        for (int i = 0; files[i]; i++) {
            size_t n = SDL_strlen(files[i]);
            if (n >= 4 && SDL_strcasecmp(files[i] + n - 4, order[k]) == 0) {
                return files[i];
            }
        }
    }
    return files[0];
}

/* Runs on the dialog's thread: hands the result to the main thread as a user event (both NULL: cancelled). */
static void SDLCALL nw_wizard_picked(void *user, const char *const *files, int filter)
{
    SDL_Event e;
    (void)filter;
    SDL_zero(e);
    e.type = (Uint32)(uintptr_t)user;
    e.user.data1 = files && files[0] ? SDL_strdup(nw_wizard_best(files)) : NULL;
    e.user.data2 = files ? NULL : SDL_strdup(SDL_GetError());
    if (!SDL_PushEvent(&e)) {
        SDL_free(e.user.data1);
        SDL_free(e.user.data2);
    }
}

#ifndef NW_UWP
/* Opens dir in the file manager through a file:// URL. */
static void nw_wizard_open_folder(const char *dir)
{
    size_t n = SDL_strlen(dir);
    char *url = (char *)SDL_malloc(n * 3 + 9), *out = url;
    if (!url) {
        return;
    }
    out += SDL_snprintf(out, 9, "file://%s", dir[0] == '/' ? "" : "/");
    for (const unsigned char *p = (const unsigned char *)dir; *p; p++) {
        if (*p == '\\') {
            *out++ = '/';
        } else if (SDL_isalnum(*p) || SDL_strchr("/:-_.~", *p)) {
            *out++ = (char)*p;
        } else {
            out += SDL_snprintf(out, 4, "%%%02X", *p);
        }
    }
    *out = '\0';
    if (!SDL_OpenURL(url)) {
        SDL_Log("cannot open %s: %s", url, SDL_GetError());
    }
    SDL_free(url);
}
#endif

NwWizard *nw_wizard_create(SDL_Window *window, const char *root)
{
    NwWizard *w = (NwWizard *)SDL_calloc(1, sizeof(*w));
    if (!w) {
        return NULL;
    }
    w->window = window;
    w->root = SDL_strdup(root);
    w->import_dir = nw_paths_dir(root, NW_PATHS_IMPORT, false);
    w->dialog_event = SDL_RegisterEvents(2);
    w->had_disc = nw_extract_has_disc(root);
    w->focus = true;
    if (!w->root || !w->import_dir) {
        nw_wizard_destroy(w);
        return NULL;
    }
    /* test hook: NW_WIZARD_PAGE=welcome|done|failed|copying shows that page (screenshots) */
    const char *page = SDL_getenv("NW_WIZARD_PAGE");
    if (page) {
        if (SDL_strcmp(page, "welcome") == 0) {
            w->page = NW_PAGE_WELCOME;
        } else if (SDL_strcmp(page, "done") == 0) {
            w->page = NW_PAGE_DONE;
        } else if (SDL_strcmp(page, "failed") == 0) {
            w->page = NW_PAGE_FAILED;
            w->image = SDL_strdup("Crash Bandicoot - The Wrath of Cortex.iso");
            SDL_strlcpy(w->error, "not a disc NuWorld knows: SYSTEM.CNF names SLES_123.45", sizeof(w->error));
        } else if (SDL_strcmp(page, "copying") == 0) {
            w->copying = true;
            w->copy_since = SDL_GetTicks();
        }
    }
    return w;
}

void nw_wizard_destroy(NwWizard *w)
{
    if (!w) {
        return;
    }
    nw_import_free(w->job);
    nw_import_list_free(&w->list);
#ifdef NW_UWP
    for (size_t i = 0; i < w->nusb; i++) {
        SDL_free(w->usb[i]);
    }
    SDL_free(w->usb);
#endif
    if (w->shot) {
        nw_rhi_texture_destroy(w->shot);
    }
    SDL_free(w->image);
    SDL_free(w->import_dir);
    SDL_free(w->root);
    SDL_free(w);
}

static void nw_wizard_rescan(NwWizard *w)
{
    nw_import_list_free(&w->list);
    if (!nw_import_scan(w->import_dir, &w->list)) {
        SDL_Log("cannot list %s: %s", w->import_dir, SDL_GetError());
    }
    w->scanned = SDL_GetTicks() ? SDL_GetTicks() : 1;
}

/* The first image of the import folder to import: a .cue before an .iso before a .bin; NULL with none. */
static char *nw_wizard_first_image(NwWizard *w, size_t *count)
{
    nw_wizard_rescan(w);
    *count = w->list.count;
    if (!w->list.count) {
        return NULL;
    }
    const char **names = (const char **)SDL_calloc(w->list.count + 1, sizeof(*names));
    if (!names) {
        return NULL;
    }
    for (size_t i = 0; i < w->list.count; i++) {
        names[i] = w->list.names[i];
    }
    char *path = NULL;
    SDL_asprintf(&path, "%s%s", w->import_dir, nw_wizard_best(names));
    SDL_free(names);
    return path;
}

void nw_wizard_event(NwWizard *w, const SDL_Event *e)
{
    nw_art_input(e);
    if (e->type == SDL_EVENT_DROP_FILE && e->drop.data) {
        if (w->page != NW_PAGE_RUN) {
            nw_wizard_start(w, e->drop.data);
        }
    } else if (w->dialog_event && (e->type == w->dialog_event || e->type == w->dialog_event + 1)) {
        char *path = (char *)e->user.data1, *why = (char *)e->user.data2;
        bool folder = e->type == w->dialog_event + 1;
        w->copying = false;
        if (path && w->page == NW_PAGE_PICK) {
            if (folder) {
                /* the picker copied the folder's images into the import folder (Xbox) or it is a folder elsewhere */
                size_t count = 0;
                char *image = nw_wizard_first_image(w, &count);
                if (!image) {
                    NwImageList there;
                    if (nw_import_scan(path, &there) && there.count) {
                        SDL_asprintf(&image, "%s%s%s", path,
                                     path[0] && SDL_strchr("/\\", path[SDL_strlen(path) - 1]) ? "" : "/", there.names[0]);
                        count = there.count;
                    }
                    if (there.names) {
                        nw_import_list_free(&there);
                    }
                }
                if (image && count <= 2) { /* one image, or a .cue with its .bin */
                    nw_wizard_start(w, image);
                } else if (image) {
                    SDL_strlcpy(w->notice, "Several disc images found: pick one in the list below.", sizeof(w->notice));
                } else {
                    SDL_strlcpy(w->notice, "No .iso, .cue or .bin in that folder.", sizeof(w->notice));
                }
                SDL_free(image);
            } else {
                nw_wizard_rescan(w);
                nw_wizard_start(w, path);
            }
        } else if (why) {
            SDL_snprintf(w->notice, sizeof(w->notice),
                         "No file dialog available (%s). Drop the image here or use the import folder.", why);
        }
        SDL_free(path);
        SDL_free(why);
    }
}

#ifdef NW_UWP
#define NW_WIZARD_USB_RESCAN_MS 5000

/* Xbox: the drive roots D: to Z: and their NuWorld and CrashWOC folders. The app can read a USB drive when its
   NTFS permissions let app packages in (xbox/README.md), or when it is FAT32/exFAT. */
static void nw_wizard_scan_usb(NwWizard *w)
{
    static const char *const subs[] = { "", "NuWorld\\", "CrashWOC\\" };
    for (size_t i = 0; i < w->nusb; i++) {
        SDL_free(w->usb[i]);
    }
    SDL_free(w->usb);
    w->usb = NULL;
    w->nusb = 0;
    size_t cap = 0;
    for (char letter = 'D'; letter <= 'Z'; letter++) {
        for (size_t s = 0; s < SDL_arraysize(subs); s++) {
            char dir[64];
            SDL_snprintf(dir, sizeof(dir), "%c:\\%s", letter, subs[s]);
            if (s == 0 && !SDL_GetPathInfo(dir, NULL)) {
                break; /* no such drive */
            }
            NwImageList list;
            if (!nw_import_scan(dir, &list)) {
                continue;
            }
            for (size_t i = 0; i < list.count; i++) {
                if (w->nusb == cap) {
                    cap = cap ? cap * 2 : 8;
                    char **grown = (char **)SDL_realloc(w->usb, cap * sizeof(*grown));
                    if (!grown) {
                        break;
                    }
                    w->usb = grown;
                }
                char *path = NULL;
                if (SDL_asprintf(&path, "%s%s", dir, list.names[i]) >= 0) {
                    w->usb[w->nusb++] = path;
                }
            }
            nw_import_list_free(&list);
        }
    }
}
#endif

/* ---- drawing helpers ---- */

static float nw_wizard_s(void)
{
    return SDL_max(ImGui::GetMainViewport()->Size.y / 1080.0f, 0.55f);
}

static void nw_wizard_h1(const char *text)
{
    nw_ui_push_font(NW_UI_BOLD, 1.6f);
    ImGui::PushStyleColor(ImGuiCol_Text, nw_cv(NW_C_CREAM));
    ImGui::TextUnformatted(text);
    ImGui::PopStyleColor();
    nw_ui_pop_font();
}

static void nw_wizard_muted(const char *text)
{
    ImGui::PushStyleColor(ImGuiCol_Text, nw_cv(NW_C_MUTED));
    ImGui::TextWrapped("%s", text);
    ImGui::PopStyleColor();
}

static void nw_wizard_section(const char *text)
{
    ImGui::Dummy(ImVec2(1, ImGui::GetFontSize() * 0.4f));
    nw_ui_push_font(NW_UI_BOLD, 0.82f);
    ImGui::PushStyleColor(ImGuiCol_Text, nw_cv(NW_C_ORANGE));
    char up[96];
    size_t i = 0;
    for (; text[i] && i + 1 < sizeof(up); i++) {
        up[i] = (char)SDL_toupper((unsigned char)text[i]);
    }
    up[i] = '\0';
    ImGui::TextUnformatted(up);
    ImGui::PopStyleColor();
    nw_ui_pop_font();
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::GetWindowDrawList()->AddLine(ImVec2(p.x, p.y - 2), ImVec2(p.x + ImGui::GetContentRegionAvail().x, p.y - 2),
                                        nw_c(0x3a4580), 1.0f);
    ImGui::Dummy(ImVec2(1, ImGui::GetFontSize() * 0.15f));
}

/* A status line with a coloured dot: 0 info, 1 good, 2 warning, 3 bad. */
static void nw_wizard_status(int kind, const char *text)
{
    static const unsigned cols[4] = { 0x8fb4ff, NW_C_GREEN, NW_C_GOLD, NW_C_RED };
    float fs = ImGui::GetFontSize();
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(p.x + fs * 0.35f, p.y + fs * 0.62f), fs * 0.22f, nw_c(cols[kind]), 16);
    ImGui::Indent(fs * 0.9f);
    ImGui::PushStyleColor(ImGuiCol_Text, nw_cv(kind == 0 ? NW_C_CREAM : cols[kind]));
    ImGui::TextWrapped("%s", text);
    ImGui::PopStyleColor();
    ImGui::Unindent(fs * 0.9f);
}

/* A row of the image lists: the file's name and an Import button; true when pressed. */
static bool nw_wizard_image_row(const char *name, const char *where, int id)
{
    float fs = ImGui::GetFontSize(), w = ImGui::GetContentRegionAvail().x, h = fs * 2.6f;
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList *dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), nw_c(0x0b1030, 0.85f), fs * 0.4f);
    dl->AddRect(p, ImVec2(p.x + w, p.y + h), nw_c(0x3a4580), fs * 0.4f, 1.0f);
    /* a disc icon */
    ImVec2 c(p.x + h * 0.5f, p.y + h * 0.5f);
    dl->AddCircleFilled(c, h * 0.3f, nw_c(0xc9d2e8), 24);
    dl->AddCircleFilled(c, h * 0.3f, nw_c(0x8fb4ff, 0.35f), 24);
    dl->AddCircleFilled(c, h * 0.09f, nw_c(0x0b1030), 16);
    float bw = fs * 6.5f;
    dl->PushClipRect(ImVec2(p.x + h, p.y), ImVec2(p.x + w - bw - fs, p.y + h), true);
    nw_ui_push_font(NW_UI_BOLD, 1.0f);
    dl->AddText(ImGui::GetFont(), fs, ImVec2(p.x + h, p.y + h * 0.16f), nw_c(NW_C_CREAM), name);
    nw_ui_pop_font();
    dl->AddText(ImVec2(p.x + h, p.y + h * 0.16f + fs * 1.15f), nw_c(NW_C_MUTED), where);
    dl->PopClipRect();
    ImGui::SetCursorScreenPos(ImVec2(p.x + w - bw - fs * 0.45f, p.y + (h - fs * 1.9f) * 0.5f));
    ImGui::PushID(id);
    bool pressed = nw_art_button("Import", ImVec2(bw, fs * 1.9f), 1);
    ImGui::PopID();
    ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + h + fs * 0.35f));
    ImGui::Dummy(ImVec2(1, 0));
    return pressed;
}

static bool nw_wizard_in_body; /* the page's scrolling body is open */

/* The buttons at the bottom right of the card, below the page's scrolling body (which this closes); count labels,
   returns the pressed one or -1. kinds: 1 the main one. */
static int nw_wizard_buttons(const char *const *labels, const int *kinds, int count)
{
    if (nw_wizard_in_body) {
        ImGui::EndChild();
        nw_wizard_in_body = false;
    }
    float fs = ImGui::GetFontSize(), bh = fs * 2.3f, gap = fs * 0.7f, total = 0.0f;
    float widths[4];
    nw_ui_push_font(NW_UI_BOLD, 1.0f);
    for (int i = 0; i < count; i++) {
        widths[i] = SDL_max(ImGui::CalcTextSize(labels[i], NULL, true).x + fs * 3.0f, fs * 8.0f);
        total += widths[i] + (i ? gap : 0.0f);
    }
    nw_ui_pop_font();
    float avail = ImGui::GetContentRegionAvail().x;
    ImGui::SetCursorPosY(ImGui::GetWindowHeight() - bh - fs * 0.15f);
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + SDL_max(avail - total, 0.0f));
    int pressed = -1;
    for (int i = 0; i < count; i++) {
        if (i) {
            ImGui::SameLine(0.0f, gap);
        }
        if (nw_art_button(labels[i], ImVec2(widths[i], bh), kinds[i])) {
            pressed = i;
        }
    }
    return pressed;
}

static void nw_wizard_focus_here(NwWizard *w)
{
    if (w->focus) {
        ImGui::SetFocusID(ImGui::GetItemID(), ImGui::GetCurrentWindow()); /* focused, not pressed */
        ImGui::SetNavCursorVisible(nw_art_pad_used());
        w->focus = false;
    }
}

/* ---- pages ---- */

static NwWizardResult nw_wizard_pick(NwWizard *w)
{
    NwWizardResult result = NW_WIZARD_OPEN;
    Uint64 now = SDL_GetTicks();
    if (!w->scanned || now - w->scanned >= NW_WIZARD_RESCAN_MS) {
        nw_wizard_rescan(w);
    }
#ifdef NW_UWP
    if (!w->usb_scanned || now - w->usb_scanned >= NW_WIZARD_USB_RESCAN_MS) {
        nw_wizard_scan_usb(w);
        w->usb_scanned = now ? now : 1;
    }
#endif
    float fs = ImGui::GetFontSize();
    nw_wizard_h1("Find your disc");
    nw_wizard_muted("NuWorld plays your own copy of Crash Bandicoot: The Wrath of Cortex for PlayStation 2: PAL v1.03 "
                    "(SLES-50386) or USA v1.00 (SLUS-20238), as an .iso or a .bin with its .cue. It is copied and "
                    "checked once; nothing of the game comes with NuWorld.");
    ImGui::Dummy(ImVec2(1, fs * 0.4f));
    if (w->had_disc) {
        nw_wizard_status(1, "A disc is already imported. It is only replaced if the new import works.");
    }
    if (w->notice[0]) {
        nw_wizard_status(2, w->notice);
    }
    ImGui::Dummy(ImVec2(1, fs * 0.3f));

    /* the big choices */
    float avail = ImGui::GetContentRegionAvail().x, gap = fs * 0.8f, bh = fs * 3.0f, bw = (avail - gap) * 0.5f;
    if (w->copying) {
        nw_wizard_section("Copying from the drive");
        nw_wizard_muted("The picked files are copied into NuWorld's import folder. A 500 MB image takes a minute or two; "
                        "the import starts when the copy is done.");
        ImGui::Dummy(ImVec2(1, fs * 0.3f));
        nw_art_progress(ImVec2(avail, fs * 1.4f), -1.0f, "");
        char t[64];
        SDL_snprintf(t, sizeof(t), "%u s", (unsigned)((SDL_GetTicks() - w->copy_since) / 1000));
        nw_wizard_muted(t);
    } else {
#ifdef NW_UWP
        if (nw_art_button("Choose disc image...", ImVec2(bw, bh), 1)) {
            SDL_SetHint("SDL_WINRT_PICKER_COPY_TO", w->import_dir);
            SDL_SetHint("SDL_WINRT_PICKER_EXTENSIONS", NW_WIZARD_EXTENSIONS);
            w->copying = true;
            w->copy_since = SDL_GetTicks();
            w->notice[0] = '\0';
            SDL_ShowOpenFileDialog(nw_wizard_picked, (void *)(uintptr_t)w->dialog_event, w->window, nw_wizard_filters,
                                   (int)SDL_arraysize(nw_wizard_filters), NULL, true);
        }
        nw_wizard_focus_here(w);
        ImGui::SameLine(0.0f, gap);
        if (nw_art_button("Choose folder...", ImVec2(bw, bh), 0)) {
            SDL_SetHint("SDL_WINRT_PICKER_COPY_TO", w->import_dir);
            SDL_SetHint("SDL_WINRT_PICKER_EXTENSIONS", NW_WIZARD_EXTENSIONS);
            w->copying = true;
            w->copy_since = SDL_GetTicks();
            w->notice[0] = '\0';
            SDL_ShowOpenFolderDialog(nw_wizard_picked, (void *)(uintptr_t)(w->dialog_event + 1), w->window, NULL, false);
        }
        nw_wizard_muted("Pick the .iso, or the .cue and its .bin together, on a USB drive or anywhere the Xbox can "
                        "reach; or a folder holding them.");
#else
        ImGui::BeginDisabled(!w->dialog_event);
        if (nw_art_button("Choose disc image...", ImVec2(bw, bh), 1)) {
            SDL_ShowOpenFileDialog(nw_wizard_picked, (void *)(uintptr_t)w->dialog_event, w->window, nw_wizard_filters,
                                   (int)SDL_arraysize(nw_wizard_filters), NULL, false);
        }
        nw_wizard_focus_here(w);
        ImGui::EndDisabled();
        ImGui::SameLine(0.0f, gap);
        if (nw_art_button("Open import folder", ImVec2(bw, bh), 0)) {
            nw_wizard_open_folder(w->import_dir);
        }
        nw_wizard_muted("Or drop the image onto this window, or copy it into the import folder (Steam Deck: in "
                        "Desktop Mode) and pick it below.");
#endif
    }

#ifdef NW_UWP
    nw_wizard_section("On a USB drive");
    for (size_t i = 0; i < w->nusb; i++) {
        if (nw_wizard_image_row(nw_wizard_basename(w->usb[i]), w->usb[i], (int)(1000 + i))) {
            char *path = SDL_strdup(w->usb[i]);
            if (path) {
                nw_wizard_start(w, path);
                SDL_free(path);
            }
        }
    }
    if (!w->nusb) {
        nw_wizard_muted("No disc images at a USB drive's root or in a NuWorld folder on it.");
    }
#endif
    nw_wizard_section("In the import folder");
    for (size_t i = 0; i < w->list.count; i++) {
        if (nw_wizard_image_row(w->list.names[i], w->import_dir, (int)i)) {
            char *path = NULL;
            if (SDL_asprintf(&path, "%s%s", w->import_dir, w->list.names[i]) >= 0) {
                nw_wizard_start(w, path);
                SDL_free(path);
            }
        }
    }
    if (!w->list.count) {
#ifdef NW_UWP
        nw_wizard_muted("Empty. The pickers copy into it, or upload an image with the Xbox Device Portal (File "
                        "explorer, LocalState, crashwoc-nuworld, import).");
#else
        nw_wizard_muted("Empty.");
#endif
    }
    ImGui::PushStyleColor(ImGuiCol_Text, nw_cv(NW_C_MUTED, 0.7f));
    ImGui::TextWrapped("%s", w->import_dir);
    ImGui::PopStyleColor();

    static const char *const both[] = { "Start game", "Quit" }, *const quit[] = { "Quit" };
    static const int both_kinds[] = { 1, 0 }, quit_kinds[] = { 0 };
    int pressed = w->had_disc ? nw_wizard_buttons(both, both_kinds, 2) : nw_wizard_buttons(quit, quit_kinds, 1);
    if (w->had_disc && pressed == 0) {
        result = NW_WIZARD_CLOSED;
    } else if (pressed >= 0) {
        result = NW_WIZARD_QUIT;
    }
    return result;
}

static void nw_wizard_run(NwWizard *w)
{
    uint64_t done, total;
    NwImportState state = nw_import_poll(w->job, &done, &total);
    if (state != NW_IMPORT_RUNNING) {
        if (state == NW_IMPORT_FAILED) {
            SDL_strlcpy(w->error, nw_import_error(w->job), sizeof(w->error));
        }
        w->imported = state == NW_IMPORT_DONE ? nw_import_manifest(w->job) : NULL;
        nw_import_free(w->job);
        w->job = NULL;
        w->page = state == NW_IMPORT_DONE ? NW_PAGE_DONE : state == NW_IMPORT_FAILED ? NW_PAGE_FAILED : NW_PAGE_PICK;
        w->focus = true;
        if (state == NW_IMPORT_CANCELLED) {
            SDL_strlcpy(w->notice, "Import cancelled. Nothing was changed.", sizeof(w->notice));
        }
        return;
    }
    float fs = ImGui::GetFontSize(), avail = ImGui::GetContentRegionAvail().x;
    nw_wizard_h1("Importing");
    nw_wizard_muted(nw_wizard_basename(w->image));
    ImGui::Dummy(ImVec2(1, fs));
    if (!total) {
        nw_wizard_status(0, "Checking the disc...");
        nw_art_progress(ImVec2(avail, fs * 1.6f), -1.0f, "");
    } else {
        char overlay[32], mb[64];
        float fraction = (float)((double)done / (double)total);
        SDL_snprintf(overlay, sizeof(overlay), "%d%%", (int)(done * 100 / total));
        nw_wizard_status(0, "Copying and checking every file...");
        nw_art_progress(ImVec2(avail, fs * 1.6f), fraction, overlay);
        SDL_snprintf(mb, sizeof(mb), "%llu of %llu MB", (unsigned long long)(done >> 20), (unsigned long long)(total >> 20));
        nw_wizard_muted(mb);
    }
    ImGui::Dummy(ImVec2(1, fs));
    static const char *const cancel[] = { "Cancel" };
    static const int kinds[] = { 0 };
    if (nw_wizard_buttons(cancel, kinds, 1) == 0) {
        nw_import_cancel(w->job);
    }
}

static void nw_wizard_done(NwWizard *w)
{
    char matches[96], where[600];
    SDL_snprintf(matches, sizeof(matches), "Every file matches %s.", w->imported ? w->imported->name : "the disc");
    SDL_snprintf(where, sizeof(where),
                 "The game files are in %s" NW_DISC_DIR ". You will not need the disc image again: you can delete it.",
                 w->root);
    nw_wizard_h1("Disc imported");
    ImGui::Dummy(ImVec2(1, ImGui::GetFontSize() * 0.4f));
    nw_wizard_status(1, matches);
    nw_wizard_status(0, where);
    ImGui::Dummy(ImVec2(1, ImGui::GetFontSize()));
    static const char *const next[] = { "Continue" };
    static const int kinds[] = { 1 };
    if (nw_wizard_buttons(next, kinds, 1) == 0) {
        w->page = NW_PAGE_WELCOME;
        w->focus = true;
    }
    nw_wizard_focus_here(w);
}

/* Uploads the embedded screenshot once; the page goes on without it if that fails. */
static void nw_wizard_load_shot(NwWizard *w)
{
    SDL_IOStream *io = SDL_IOFromConstMem(nw_beta_menu_png, nw_beta_menu_png_size);
    SDL_Surface *png = io ? SDL_LoadPNG_IO(io, true) : NULL;
    SDL_Surface *rgba = png ? SDL_ConvertSurface(png, SDL_PIXELFORMAT_RGBA32) : NULL;
    SDL_DestroySurface(png);
    if (!rgba || rgba->pitch != rgba->w * 4) {
        SDL_Log("wizard: screenshot: %s", SDL_GetError());
        SDL_DestroySurface(rgba);
        return;
    }
    const void *level = rgba->pixels;
    w->shot = nw_rhi_texture_create(NW_RHI_RGBA8, rgba->w, rgba->h, 1, 1, NW_RHI_TEX_LINEAR, &level);
    w->shot_w = rgba->w;
    w->shot_h = rgba->h;
    SDL_DestroySurface(rgba);
}

static NwWizardResult nw_wizard_welcome(NwWizard *w)
{
    float fs = ImGui::GetFontSize();
    nw_wizard_h1("Welcome to the NuWorld beta");
    nw_wizard_muted("Thanks for testing! This beta has one goal: fine-tuning N. Hanced Movement, NuWorld's reworked "
                    "movement for Crash, Coco and the vehicles.");
    if (!w->shot && w->shot_w >= 0) {
        nw_wizard_load_shot(w);
        if (!w->shot) {
            w->shot_w = -1; /* failed, do not retry every frame */
        }
    }
    nw_wizard_section("Settings and the debug menu");
    float avail = ImGui::GetContentRegionAvail().x;
    if (w->shot) {
        float width = avail * 0.34f, height = width * (float)w->shot_h / (float)w->shot_w;
        ImVec2 p = ImGui::GetCursorScreenPos();
        ImGui::Image((ImTextureID)nw_rhi_texture_ui(w->shot), ImVec2(width, height));
        ImGui::GetWindowDrawList()->AddRect(p, ImVec2(p.x + width, p.y + height), nw_c(0x3a4580), 4.0f, 1.5f);
        if (ImGui::IsItemHovered()) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        }
        if (ImGui::IsItemClicked()) {
            w->zoom = true;
        }
        ImGui::SameLine(0.0f, fs * 1.2f);
    }
    ImGui::BeginGroup();
    ImGui::PushTextWrapPos(0.0f);
    nw_wizard_status(0, "Before the game starts, the launcher has every setting: video, audio, controls, game speed, "
                        "camera, and N. Hanced Movement's parameters.");
    nw_wizard_status(0, "In the game, F1 opens the same settings over it. Under N. Hanced Movement each of Crash, Coco "
                        "and the vehicles has its own page and key.");
    nw_wizard_status(0, "Retail and N. Hanced at the top of a page switch between the original movement and the beta's "
                        "defaults. Changes are saved at once.");
    ImGui::PopTextWrapPos();
    ImGui::EndGroup();
    nw_wizard_section("Feedback");
    nw_wizard_muted("Something not working, or a parameter you would like? Create an issue on GitHub with the Beta "
                    "Feedback template, or start a thread on the Discord server.");
    ImGui::Dummy(ImVec2(1, fs * 0.3f));
    if (nw_art_button("Beta feedback on GitHub", ImVec2(fs * 13.0f, fs * 2.2f), 0)) {
        SDL_OpenURL(NW_WIZARD_FEEDBACK_URL);
    }
    ImGui::SameLine(0.0f, fs * 0.7f);
    if (nw_art_button("Discord", ImVec2(fs * 8.0f, fs * 2.2f), 0)) {
        SDL_OpenURL(NW_WIZARD_DISCORD_URL);
    }
    static const char *const start[] = { "Start game" };
    static const int kinds[] = { 1 };
    NwWizardResult r = nw_wizard_buttons(start, kinds, 1) == 0 ? NW_WIZARD_CLOSED : NW_WIZARD_OPEN;
    nw_wizard_focus_here(w);
    return r;
}

/* The screenshot over the whole window; a click, B or Escape closes it. */
static void nw_wizard_zoom(NwWizard *w)
{
    const ImGuiViewport *vp = ImGui::GetMainViewport();
    float scale = SDL_min(vp->WorkSize.x / (float)w->shot_w, vp->WorkSize.y / (float)w->shot_h);
    ImVec2 size((float)w->shot_w * scale, (float)w->shot_h * scale);
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::SetNextWindowFocus();
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.0f, 0.0f, 0.0f, 0.85f));
    ImGui::Begin("##zoom", NULL, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
    ImGui::SetCursorPos(ImVec2((vp->WorkSize.x - size.x) * 0.5f, (vp->WorkSize.y - size.y) * 0.5f));
    ImGui::Image((ImTextureID)nw_rhi_texture_ui(w->shot), size);
    if ((ImGui::IsWindowHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) || ImGui::IsKeyPressed(ImGuiKey_Escape) ||
        ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight)) {
        w->zoom = false;
    }
    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);
}

static void nw_wizard_failed(NwWizard *w)
{
    nw_wizard_h1("Import failed");
    nw_wizard_muted(nw_wizard_basename(w->image ? w->image : ""));
    ImGui::Dummy(ImVec2(1, ImGui::GetFontSize() * 0.4f));
    nw_wizard_status(3, w->error);
    nw_wizard_status(0, "NuWorld runs PAL v1.03 (SLES-50386) and USA v1.00 (SLUS-20238). The data folder is unchanged.");
    ImGui::Dummy(ImVec2(1, ImGui::GetFontSize()));
    static const char *const buttons[] = { "Try again", "Back" };
    static const int kinds[] = { 1, 0 };
    int pressed = nw_wizard_buttons(buttons, kinds, 2);
    if (pressed == 0 && w->image) {
        nw_wizard_start(w, w->image); /* copies the path before freeing the old one */
        return;
    }
    if (pressed == 1) {
        w->page = NW_PAGE_PICK;
        w->focus = true;
    }
}

/* The title above the card: the game's name drawn in the UI's own face (no game art before the import). */
static void nw_wizard_header(float W, float H, ImVec2 o, float top)
{
    ImDrawList *dl = ImGui::GetForegroundDrawList();
    float h1 = H * 0.062f, h2 = H * 0.034f;
    float w1 = nw_art_text_width(h1, "CRASH BANDICOOT"), w2 = nw_art_text_width(h2, "THE WRATH OF CORTEX");
    nw_art_text(dl, ImVec2(o.x + (W - w1) * 0.5f, o.y + top), h1, NW_INK_ORANGE, "CRASH BANDICOOT");
    nw_art_text(dl, ImVec2(o.x + (W - w2) * 0.5f, o.y + top + h1 * 0.98f), h2, NW_INK_GREEN, "THE WRATH OF CORTEX");
    char tag[128];
    SDL_snprintf(tag, sizeof(tag), "%s  |  First-time setup", nw_art_app_title());
    ImFont *font = ImGui::GetFont();
    float ts = H * 0.02f;
    ImVec2 ext = font->CalcTextSizeA(ts, FLT_MAX, 0.0f, tag);
    dl->AddText(font, ts, ImVec2(o.x + (W - ext.x) * 0.5f, o.y + top + h1 * 0.98f + h2 * 1.15f), nw_c(NW_C_MUTED), tag);
}

NwWizardResult nw_wizard_draw(NwWizard *w)
{
    NwWizardResult result = NW_WIZARD_OPEN;
    const ImGuiViewport *vp = ImGui::GetMainViewport();
    float W = vp->Size.x, H = vp->Size.y, s = nw_wizard_s(), t = (float)ImGui::GetTime();
    ImVec2 o = vp->Pos;
    nw_art_backdrop(t, 0.8f);
    nw_ui_push_font(NW_UI_REGULAR, SDL_max(H / 900.0f, 0.8f) / ImGui::GetStyle().FontScaleDpi);

    float top = H * 0.035f;
    nw_wizard_header(W, H, o, top);
    int step = w->page == NW_PAGE_PICK || w->page == NW_PAGE_FAILED ? 0 : w->page == NW_PAGE_RUN ? 1 : 2;
    if (w->page == NW_PAGE_PICK && w->copying) {
        step = 0;
    }
    float sy = top + H * 0.145f;
    nw_art_steps(ImGui::GetForegroundDrawList(), o.x + W * 0.5f, o.y + sy, SDL_min(W * 0.42f, H * 0.75f), H * 0.042f, step);

    /* the card */
    float cw = SDL_min(W * 0.86f, H * 1.45f), cy = sy + H * 0.115f, ch = H * 0.93f - cy;
    ImVec2 a(o.x + (W - cw) * 0.5f, o.y + cy), b(a.x + cw, a.y + ch);
    nw_art_card(ImGui::GetBackgroundDrawList(), a, b, s, false);
    float pad = H * 0.035f;
    ImGui::SetNextWindowPos(ImVec2(a.x + pad, a.y + pad * 0.8f));
    ImGui::SetNextWindowSize(ImVec2(cw - pad * 2, ch - pad * 1.4f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ImGui::GetFontSize() * 0.5f, ImGui::GetFontSize() * 0.45f));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, nw_cv(0, 0.0f));
    ImGui::PushStyleColor(ImGuiCol_NavCursor, nw_cv(0, 0.0f)); /* the buttons draw their own */
    ImGui::Begin("##setup", NULL,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoBringToFrontOnFocus);
    {
        /* the page scrolls above the button row */
        float fs = ImGui::GetFontSize();
        ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize, fs * 0.6f);
        ImGui::BeginChild("##body", ImVec2(0.0f, ImGui::GetContentRegionAvail().y - fs * 3.0f), ImGuiChildFlags_NavFlattened);
        ImGui::PopStyleVar();
        nw_wizard_in_body = true;
    }
    switch (w->page) {
    case NW_PAGE_PICK:
        result = nw_wizard_pick(w);
        break;
    case NW_PAGE_RUN:
        nw_wizard_run(w);
        break;
    case NW_PAGE_DONE:
        nw_wizard_done(w);
        break;
    case NW_PAGE_FAILED:
        nw_wizard_failed(w);
        break;
    case NW_PAGE_WELCOME:
        result = nw_wizard_welcome(w);
        break;
    }
    if (nw_wizard_in_body) {
        ImGui::EndChild();
        nw_wizard_in_body = false;
    }
    ImGui::End();
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(3);

    /* prompts */
    static const NwArtPrompt prompts[] = {
        { "xbox_button_color_a", "keyboard_enter", "Select" },
        { "xbox_dpad_down", "keyboard_tab", "Move" },
    };
    float ph = H * 0.036f;
    nw_art_prompts(ImGui::GetForegroundDrawList(), ImVec2(o.x + (W - cw) * 0.5f, o.y + H * 0.947f), ph, prompts, 2,
                   nw_art_pad_used(), false);
    nw_ui_pop_font();
    if (w->zoom && w->shot) {
        nw_wizard_zoom(w);
    }
    return result;
}

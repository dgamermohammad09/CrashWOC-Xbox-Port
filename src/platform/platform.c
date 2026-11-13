#include "platform/platform.h"

#include "app.h"
#include "gfx/rhi.h"

static void nw_platform_fail(const char *what)
{
    char msg[512];
    SDL_snprintf(msg, sizeof(msg), "%s:\n%s", what, SDL_GetError());
    SDL_Log("%s", msg);
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, NW_APP_NAME, msg, NULL);
}

#ifndef SDL_PLATFORM_WINDOWS
extern const unsigned char nw_icon_png[];
extern const size_t nw_icon_png_size;

/* Windows takes the icon from the exe resource. */
static void nw_platform_icon(SDL_Window *window)
{
    SDL_IOStream *io = SDL_IOFromConstMem(nw_icon_png, nw_icon_png_size);
    SDL_Surface *icon = io ? SDL_LoadPNG_IO(io, true) : NULL;
    if (icon) {
        SDL_SetWindowIcon(window, icon);
        SDL_DestroySurface(icon);
    }
}
#endif

bool nw_platform_init(NwPlatform *p, const char *title, int width, int height)
{
    SDL_zerop(p);

    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_GAMEPAD)) {
        nw_platform_fail("SDL could not start");
        return false;
    }

    /* The renderer's window: OpenGL's needs SDL_WINDOW_OPENGL and its context attributes, Direct3D's nothing. */
    SDL_WindowFlags api = nw_rhi_prepare_window();
    p->window = SDL_CreateWindow(title, width, height, api | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (!p->window) {
        nw_platform_fail("Could not create the window");
        nw_platform_shutdown(p);
        return false;
    }
#ifndef SDL_PLATFORM_WINDOWS
    nw_platform_icon(p->window);
#endif

    if (!nw_rhi_init(p->window)) {
        nw_platform_fail(nw_rhi_kind() == NW_RHI_OPENGL ? "OpenGL 3.3 is required" : "The renderer could not start");
        nw_platform_shutdown(p);
        return false;
    }
    p->gl = nw_rhi_kind() == NW_RHI_OPENGL ? nw_rhi_native(0) : NULL;
    p->rhi = true;
    SDL_Log("renderer: %s: %s", nw_rhi_kind_name(nw_rhi_kind()), nw_rhi_description());
    return true;
}

void nw_platform_shutdown(NwPlatform *p)
{
    if (p->rhi) {
        nw_rhi_shutdown();
    }
    if (p->window) {
        SDL_DestroyWindow(p->window);
    }
    SDL_zerop(p);
    SDL_Quit();
}

/* src/gfx/rhi.h over Direct3D 11: a device and a flip-model swap chain on the SDL window (its HWND on the desktop,
   its CoreWindow in the UWP build), no OpenGL. The programs are tools/gen_shaders.py's bytecode of the same GLSL the
   OpenGL backend compiles. Everything is kept in the GL layout (src/gfx/rhi.h): the vertex stages flip y, so a
   texture holds the same rows, and the window alone is drawn upside down back. What Direct3D 11 lacks is drawn:
   scaled and depth blits, masked or scissored clears (src/gfx/shaders/d3d11_util.hlsl). */
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <d3d11.h>
#include <dxgi1_2.h>

#include "gfx/rhi_d3d11_shaders.h"
#include "gfx/rhi_impl.h"

#ifndef DXGI_SWAP_EFFECT_FLIP_DISCARD
#define DXGI_SWAP_EFFECT_FLIP_DISCARD ((DXGI_SWAP_EFFECT)4)
#endif

#define NW_D3D_UNITS 9    /* the texture units vu.c binds: 0-8 */
#define NW_D3D_LAYOUTS 8  /* distinct vertex layouts */
#define NW_D3D_STATES 64  /* cached state objects of a kind */

struct NwRhiBuffer {
    NwRhiBufferKind kind;
    NwRhiFormat texels;
    ID3D11Buffer *buf;
    ID3D11ShaderResourceView *srv;
    size_t cap;
};

struct NwRhiTexture {
    NwRhiFormat format;
    int w, h, levels, samples;
    unsigned flags;
    ID3D11Texture2D *tex;
    ID3D11ShaderResourceView *srv;
    ID3D11RenderTargetView *rtv;
    ID3D11DepthStencilView *dsv;
};

struct NwRhiProgram {
    const NwD3dProgram *src;
    ID3D11VertexShader *vs[2];
    ID3D11PixelShader *ps;
    ID3D11Buffer *cb[2];
    uint8_t *data[2];
    bool dirty[2];
    ID3D11InputLayout *il[2][NW_D3D_LAYOUTS];
};

struct NwRhiVertexArray {
    int layout; /* in nw_d3d.layouts, -1 none */
    NwRhiBuffer *vb, *ib;
    size_t offset;
};

typedef struct NwD3dState {
    uint64_t key;
    IUnknown *obj;
} NwD3dState;

static struct {
    SDL_Window *window;
    ID3D11Device *dev;
    ID3D11DeviceContext *ctx;
    IDXGISwapChain1 *swap;
    ID3D11RenderTargetView *back; /* the swap chain's */
    int back_w, back_h;
    D3D_FEATURE_LEVEL level;
    NwRhiCaps caps;
    char description[256];
    int interval;
    /* what the draws take */
    ID3D11RenderTargetView *rtv;
    ID3D11DepthStencilView *dsv;
    bool to_window;
    int target_h;
    int viewport[4];
    NwRhiProgram *prog;
    ID3D11ShaderResourceView *srv[NW_D3D_UNITS];
    uint64_t blend_key, ds_key;
    uint8_t stencil_ref;
    bool wireframe;
    /* blend: on, ops, factors, mask */
    bool blend_on;
    uint8_t blend[6];
    uint8_t mask;
    bool depth_test, depth_write;
    uint8_t depth_func;
    bool stencil_test;
    uint8_t stencil_func, stencil_read, stencil_write, stencil_ops[3];
    /* caches */
    NwRhiVertexLayout layouts[NW_D3D_LAYOUTS];
    int nlayouts;
    NwD3dState blends[NW_D3D_STATES], dss[NW_D3D_STATES], rss[4];
    int nblends, ndss;
    ID3D11SamplerState *point, *linear;
    ID3D11Buffer *dummy;    /* GL's (0, 0, 0, 1) for inputs a layout lacks: float, uint, int */
    ID3D11Buffer *util_cb;
    ID3D11VertexShader *util_vs;
    ID3D11PixelShader *util_ps[NW_D3D_UTILS];
} nw_d3d;

static bool nw_d3d_fail(const char *what, HRESULT hr)
{
    return SDL_SetError("Direct3D 11: %s failed (0x%08lx)", what, (unsigned long)hr);
}

static void nw_d3d_release(void *p)
{
    if (p) {
        IUnknown_Release((IUnknown *)p);
    }
}

/* ---- device and window ---- */

static SDL_WindowFlags nw_d3d_prepare_window(void)
{
    return 0;
}

static bool nw_d3d_back_buffer(void)
{
    ID3D11Texture2D *tex = NULL;
    HRESULT hr = IDXGISwapChain1_GetBuffer(nw_d3d.swap, 0, &IID_ID3D11Texture2D, (void **)&tex);
    if (FAILED(hr)) {
        return nw_d3d_fail("GetBuffer", hr);
    }
    hr = ID3D11Device_CreateRenderTargetView(nw_d3d.dev, (ID3D11Resource *)tex, NULL, &nw_d3d.back);
    ID3D11Texture2D_Release(tex);
    return SUCCEEDED(hr) || nw_d3d_fail("CreateRenderTargetView", hr);
}

/* The swap chain at the window's size. */
static void nw_d3d_fit_window(void)
{
    int w = 0, h = 0;
    SDL_GetWindowSizeInPixels(nw_d3d.window, &w, &h);
    if (w <= 0 || h <= 0 || (w == nw_d3d.back_w && h == nw_d3d.back_h)) {
        return;
    }
    ID3D11DeviceContext_OMSetRenderTargets(nw_d3d.ctx, 0, NULL, NULL);
    nw_d3d_release(nw_d3d.back);
    nw_d3d.back = NULL;
    HRESULT hr = IDXGISwapChain1_ResizeBuffers(nw_d3d.swap, 0, (UINT)w, (UINT)h, DXGI_FORMAT_UNKNOWN, 0);
    if (FAILED(hr)) {
        SDL_Log("Direct3D 11: ResizeBuffers %dx%d failed (0x%08lx)", w, h, (unsigned long)hr);
    } else {
        nw_d3d.back_w = w;
        nw_d3d.back_h = h;
    }
    nw_d3d_back_buffer();
}

static HRESULT nw_d3d_device(D3D_DRIVER_TYPE type, UINT flags)
{
    static const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
    HRESULT hr = D3D11CreateDevice(NULL, type, NULL, flags, levels, 2, D3D11_SDK_VERSION, &nw_d3d.dev, &nw_d3d.level,
                                   &nw_d3d.ctx);
    if (hr == E_INVALIDARG) { /* a runtime without 11_1 */
        hr = D3D11CreateDevice(NULL, type, NULL, flags, levels + 1, 1, D3D11_SDK_VERSION, &nw_d3d.dev, &nw_d3d.level,
                               &nw_d3d.ctx);
    }
    return hr;
}

static bool nw_d3d_swap_chain(void)
{
    IDXGIDevice *dxgi = NULL;
    IDXGIAdapter *adapter = NULL;
    IDXGIFactory2 *factory = NULL;
    HRESULT hr = ID3D11Device_QueryInterface(nw_d3d.dev, &IID_IDXGIDevice, (void **)&dxgi);
    if (SUCCEEDED(hr)) {
        hr = IDXGIDevice_GetAdapter(dxgi, &adapter);
    }
    if (SUCCEEDED(hr)) {
        hr = IDXGIAdapter_GetParent(adapter, &IID_IDXGIFactory2, (void **)&factory);
    }
    if (SUCCEEDED(hr)) {
        DXGI_ADAPTER_DESC ad;
        if (SUCCEEDED(IDXGIAdapter_GetDesc(adapter, &ad))) {
            char *name = SDL_iconv_string("UTF-8", "UTF-16LE", (const char *)ad.Description,
                                          (SDL_wcslen((const wchar_t *)ad.Description) + 1) * sizeof(WCHAR));
            SDL_snprintf(nw_d3d.description, sizeof(nw_d3d.description), "Direct3D 11 (feature level %x_%x), %s",
                         (unsigned)nw_d3d.level >> 12, ((unsigned)nw_d3d.level >> 8) & 0xF, name ? name : "?");
            SDL_free(name);
        }
    }
    int w = 0, h = 0;
    SDL_GetWindowSizeInPixels(nw_d3d.window, &w, &h);
    DXGI_SWAP_CHAIN_DESC1 sd = { 0 };
    sd.Width = (UINT)SDL_max(w, 1);
    sd.Height = (UINT)SDL_max(h, 1);
    sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 2;
    sd.Scaling = DXGI_SCALING_STRETCH;
    sd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    if (SUCCEEDED(hr)) {
        SDL_PropertiesID props = SDL_GetWindowProperties(nw_d3d.window);
#ifdef NW_UWP
        /* SDL3-uwp's CoreWindow (SDL_PROP_WINDOW_WINRT_WINDOW_POINTER, an IInspectable). */
        IUnknown *core = SDL_GetPointerProperty(props, "SDL.window.winrt.window", NULL);
        sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
        hr = core ? IDXGIFactory2_CreateSwapChainForCoreWindow(factory, (IUnknown *)nw_d3d.dev, core, &sd, NULL,
                                                               &nw_d3d.swap)
                  : E_POINTER;
#else
        HWND hwnd = SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WIN32_HWND_POINTER, NULL);
        sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        hr = hwnd ? IDXGIFactory2_CreateSwapChainForHwnd(factory, (IUnknown *)nw_d3d.dev, hwnd, &sd, NULL, NULL,
                                                         &nw_d3d.swap)
                  : E_POINTER;
        if (SUCCEEDED(hr)) {
            IDXGIFactory2_MakeWindowAssociation(factory, hwnd, DXGI_MWA_NO_ALT_ENTER);
        }
#endif
    }
    nw_d3d_release(factory);
    nw_d3d_release(adapter);
    nw_d3d_release(dxgi);
    if (FAILED(hr)) {
        return nw_d3d_fail("the swap chain", hr);
    }
    nw_d3d.back_w = (int)sd.Width;
    nw_d3d.back_h = (int)sd.Height;
    return nw_d3d_back_buffer();
}

static bool nw_d3d_init(SDL_Window *window)
{
    SDL_zero(nw_d3d);
    nw_d3d.window = window;
    nw_d3d.interval = 1;
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    const char *debug = SDL_getenv("NW_D3D11_DEBUG");
    if (debug && *debug == '1') {
        flags |= D3D11_CREATE_DEVICE_DEBUG;
    }
    /* NW_D3D11_DRIVER=warp: the software rasterizer (also taken when no GPU gives feature level 11_0). */
    const char *driver = SDL_getenv("NW_D3D11_DRIVER");
    bool warp = driver && SDL_strcasecmp(driver, "warp") == 0;
    HRESULT hr = nw_d3d_device(warp ? D3D_DRIVER_TYPE_WARP : D3D_DRIVER_TYPE_HARDWARE, flags);
    if (FAILED(hr) && !warp) {
        SDL_Log("Direct3D 11: no feature level 11_0 GPU (0x%08lx); WARP", (unsigned long)hr);
        hr = nw_d3d_device(D3D_DRIVER_TYPE_WARP, flags);
    }
    if (FAILED(hr)) {
        return nw_d3d_fail("D3D11CreateDevice", hr);
    }
    if (!nw_d3d_swap_chain()) {
        return false;
    }
    nw_d3d.caps.max_texture = D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION;
    for (UINT n = 2; n <= 8; n *= 2) {
        UINT a = 0, b = 0;
        ID3D11Device_CheckMultisampleQualityLevels(nw_d3d.dev, DXGI_FORMAT_R8G8B8A8_UNORM, n, &a);
        ID3D11Device_CheckMultisampleQualityLevels(nw_d3d.dev, DXGI_FORMAT_D24_UNORM_S8_UINT, n, &b);
        if (a && b) {
            nw_d3d.caps.max_samples = (int)n;
        }
    }
    D3D11_SAMPLER_DESC sd = { 0 };
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
    ID3D11Device_CreateSamplerState(nw_d3d.dev, &sd, &nw_d3d.point);
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    ID3D11Device_CreateSamplerState(nw_d3d.dev, &sd, &nw_d3d.linear);
    static const uint32_t one = 0x3F800000u;
    const uint32_t dummy[12] = { 0, 0, 0, one, 0, 0, 0, 1, 0, 0, 0, 1 };
    D3D11_BUFFER_DESC bd = { sizeof(dummy), D3D11_USAGE_IMMUTABLE, D3D11_BIND_VERTEX_BUFFER, 0, 0, 0 };
    D3D11_SUBRESOURCE_DATA init = { dummy, 0, 0 };
    ID3D11Device_CreateBuffer(nw_d3d.dev, &bd, &init, &nw_d3d.dummy);
    D3D11_BUFFER_DESC cbd = { 64, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0 };
    ID3D11Device_CreateBuffer(nw_d3d.dev, &cbd, NULL, &nw_d3d.util_cb);
    hr = ID3D11Device_CreateVertexShader(nw_d3d.dev, nw_d3d11_util[NW_D3D_BLIT_VS].code,
                                         nw_d3d11_util[NW_D3D_BLIT_VS].size, NULL, &nw_d3d.util_vs);
    for (int i = NW_D3D_BLIT_POINT; SUCCEEDED(hr) && i < NW_D3D_UTILS; i++) {
        hr = ID3D11Device_CreatePixelShader(nw_d3d.dev, nw_d3d11_util[i].code, nw_d3d11_util[i].size, NULL,
                                            &nw_d3d.util_ps[i]);
    }
    if (FAILED(hr)) {
        return nw_d3d_fail("the blit shaders", hr);
    }
    nw_d3d.mask = 0xF;
    nw_d3d.depth_write = true;
    nw_d3d.depth_func = NW_RHI_LESS;
    nw_d3d.stencil_read = nw_d3d.stencil_write = 0xFF;
    SDL_Log("%s", nw_d3d.description);
    return true;
}

static void nw_d3d_shutdown(void)
{
    if (nw_d3d.ctx) {
        ID3D11DeviceContext_ClearState(nw_d3d.ctx);
        ID3D11DeviceContext_Flush(nw_d3d.ctx);
    }
    for (int i = 0; i < nw_d3d.nblends; i++) {
        nw_d3d_release(nw_d3d.blends[i].obj);
    }
    for (int i = 0; i < nw_d3d.ndss; i++) {
        nw_d3d_release(nw_d3d.dss[i].obj);
    }
    for (int i = 0; i < 4; i++) {
        nw_d3d_release(nw_d3d.rss[i].obj);
    }
    for (int i = 0; i < NW_D3D_UTILS; i++) {
        nw_d3d_release(nw_d3d.util_ps[i]);
    }
    nw_d3d_release(nw_d3d.util_vs);
    nw_d3d_release(nw_d3d.util_cb);
    nw_d3d_release(nw_d3d.dummy);
    nw_d3d_release(nw_d3d.point);
    nw_d3d_release(nw_d3d.linear);
    nw_d3d_release(nw_d3d.back);
    nw_d3d_release(nw_d3d.swap);
    nw_d3d_release(nw_d3d.ctx);
    nw_d3d_release(nw_d3d.dev);
    SDL_zero(nw_d3d);
}

static void *nw_d3d_native(int which)
{
    return which == 0 ? (void *)nw_d3d.dev : which == 1 ? (void *)nw_d3d.ctx : NULL;
}

static const char *nw_d3d_description(void)
{
    return nw_d3d.description;
}

static const NwRhiCaps *nw_d3d_caps(void)
{
    return &nw_d3d.caps;
}

static bool nw_d3d_set_vsync(int interval)
{
    if (interval < 0) {
        return SDL_SetError("Direct3D 11: no adaptive vsync");
    }
    nw_d3d.interval = interval;
    return true;
}

static int nw_d3d_vsync(void)
{
    return nw_d3d.interval;
}

static void nw_d3d_present(void)
{
    HRESULT hr = IDXGISwapChain1_Present(nw_d3d.swap, nw_d3d.interval ? 1 : 0, 0);
    if (FAILED(hr)) {
        static bool told;
        if (!told) {
            SDL_Log("Direct3D 11: Present failed (0x%08lx)", (unsigned long)hr);
            told = true;
        }
    }
}

/* ---- buffers ---- */

static NwRhiBuffer *nw_d3d_buffer_create(NwRhiBufferKind kind, NwRhiFormat texels)
{
    NwRhiBuffer *b = SDL_calloc(1, sizeof(*b));
    if (b) {
        b->kind = kind;
        b->texels = texels;
    }
    return b;
}

static void nw_d3d_buffer_data(NwRhiBuffer *b, size_t size, const void *data, bool stream)
{
    if (!size) {
        return;
    }
    if (size > b->cap) {
        nw_d3d_release(b->srv);
        nw_d3d_release(b->buf);
        b->srv = NULL;
        b->buf = NULL;
        b->cap = 0;
        size_t cap = stream ? SDL_max(size + size / 2, (size_t)4096) : size;
        cap = (cap + 15) & ~(size_t)15;
        static const UINT binds[] = { D3D11_BIND_VERTEX_BUFFER, D3D11_BIND_INDEX_BUFFER,
                                      D3D11_BIND_SHADER_RESOURCE };
        D3D11_BUFFER_DESC bd = { (UINT)cap, D3D11_USAGE_DEFAULT, binds[b->kind], 0, 0, 0 };
        HRESULT hr = ID3D11Device_CreateBuffer(nw_d3d.dev, &bd, NULL, &b->buf);
        if (FAILED(hr)) {
            nw_d3d_fail("CreateBuffer", hr);
            return;
        }
        if (b->kind == NW_RHI_TEXELS) {
            bool r32 = b->texels == NW_RHI_R32F;
            D3D11_SHADER_RESOURCE_VIEW_DESC sv = { 0 };
            sv.Format = r32 ? DXGI_FORMAT_R32_FLOAT : DXGI_FORMAT_R32G32B32A32_FLOAT;
            sv.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
            sv.Buffer.FirstElement = 0;
            sv.Buffer.NumElements = (UINT)(cap / (r32 ? 4 : 16));
            ID3D11Device_CreateShaderResourceView(nw_d3d.dev, (ID3D11Resource *)b->buf, &sv, &b->srv);
        }
        b->cap = cap;
    }
    D3D11_BOX box = { 0, 0, 0, (UINT)size, 1, 1 };
    ID3D11DeviceContext_UpdateSubresource(nw_d3d.ctx, (ID3D11Resource *)b->buf, 0, &box, data, 0, 0);
}

static void nw_d3d_buffer_destroy(NwRhiBuffer *b)
{
    if (b) {
        for (int i = 0; i < NW_D3D_UNITS; i++) {
            if (b->srv && nw_d3d.srv[i] == b->srv) {
                nw_d3d.srv[i] = NULL;
            }
        }
        nw_d3d_release(b->srv);
        nw_d3d_release(b->buf);
        SDL_free(b);
    }
}

/* ---- textures ---- */

static int nw_d3d_bpp(NwRhiFormat f)
{
    return f == NW_RHI_R8UI ? 1 : f == NW_RHI_RGBA32F ? 16 : 4;
}

static NwRhiTexture *nw_d3d_texture_create(NwRhiFormat format, int width, int height, int levels, int samples,
                                           unsigned flags, const void *const *data)
{
    NwRhiTexture *t = SDL_calloc(1, sizeof(*t));
    if (!t) {
        return NULL;
    }
    *t = (NwRhiTexture){ .format = format, .w = width, .h = height, .levels = levels, .samples = samples,
                         .flags = flags };
    bool ms = samples > 1;
    D3D11_TEXTURE2D_DESC td = { 0 };
    td.Width = (UINT)width;
    td.Height = (UINT)height;
    td.MipLevels = (UINT)levels;
    td.ArraySize = 1;
    td.SampleDesc.Count = ms ? (UINT)samples : 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    DXGI_FORMAT srv = DXGI_FORMAT_UNKNOWN;
    switch (format) {
    case NW_RHI_RGBA8:
        td.Format = srv = DXGI_FORMAT_R8G8B8A8_UNORM;
        if (flags & NW_RHI_TEX_TARGET) {
            td.BindFlags |= D3D11_BIND_RENDER_TARGET;
        }
        if ((flags & NW_RHI_TEX_MIPGEN) && !ms) {
            td.MipLevels = 0;
            td.BindFlags |= D3D11_BIND_RENDER_TARGET;
            td.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
        }
        break;
    case NW_RHI_D24S8:
        td.Format = DXGI_FORMAT_R24G8_TYPELESS;
        srv = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
        td.BindFlags |= D3D11_BIND_DEPTH_STENCIL;
        break;
    case NW_RHI_RGBA8UI:
        td.Format = srv = DXGI_FORMAT_R8G8B8A8_UINT;
        break;
    case NW_RHI_R8UI:
        td.Format = srv = DXGI_FORMAT_R8_UINT;
        break;
    default:
        td.Format = srv = DXGI_FORMAT_R32G32B32A32_FLOAT;
        break;
    }
    D3D11_SUBRESOURCE_DATA init[16];
    for (int i = 0; data && i < levels && i < 16; i++) {
        init[i].pSysMem = data[i];
        init[i].SysMemPitch = (UINT)(SDL_max(width >> i, 1) * nw_d3d_bpp(format));
        init[i].SysMemSlicePitch = 0;
    }
    HRESULT hr = ID3D11Device_CreateTexture2D(nw_d3d.dev, &td, data && td.MipLevels ? init : NULL, &t->tex);
    if (SUCCEEDED(hr) && data && !td.MipLevels) {
        ID3D11DeviceContext_UpdateSubresource(nw_d3d.ctx, (ID3D11Resource *)t->tex, 0, NULL, data[0],
                                              init[0].SysMemPitch, 0);
    }
    if (SUCCEEDED(hr)) {
        D3D11_SHADER_RESOURCE_VIEW_DESC sv = { 0 };
        sv.Format = srv;
        sv.ViewDimension = ms ? D3D11_SRV_DIMENSION_TEXTURE2DMS : D3D11_SRV_DIMENSION_TEXTURE2D;
        sv.Texture2D.MipLevels = (UINT)-1;
        hr = ID3D11Device_CreateShaderResourceView(nw_d3d.dev, (ID3D11Resource *)t->tex, &sv, &t->srv);
    }
    if (SUCCEEDED(hr) && format == NW_RHI_D24S8) {
        D3D11_DEPTH_STENCIL_VIEW_DESC dv = { 0 };
        dv.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
        dv.ViewDimension = ms ? D3D11_DSV_DIMENSION_TEXTURE2DMS : D3D11_DSV_DIMENSION_TEXTURE2D;
        hr = ID3D11Device_CreateDepthStencilView(nw_d3d.dev, (ID3D11Resource *)t->tex, &dv, &t->dsv);
    } else if (SUCCEEDED(hr) && (td.BindFlags & D3D11_BIND_RENDER_TARGET)) {
        D3D11_RENDER_TARGET_VIEW_DESC rv = { 0 };
        rv.Format = td.Format;
        rv.ViewDimension = ms ? D3D11_RTV_DIMENSION_TEXTURE2DMS : D3D11_RTV_DIMENSION_TEXTURE2D;
        hr = ID3D11Device_CreateRenderTargetView(nw_d3d.dev, (ID3D11Resource *)t->tex, &rv, &t->rtv);
    }
    if (FAILED(hr)) {
        nw_d3d_fail("a texture", hr);
        SDL_Log("%s (%dx%d format %d samples %d)", SDL_GetError(), width, height, (int)format, samples);
        nw_d3d_release(t->srv);
        nw_d3d_release(t->tex);
        SDL_free(t);
        return NULL;
    }
    return t;
}

static void nw_d3d_texture_update(NwRhiTexture *t, int level, int x, int y, int width, int height, const void *data)
{
    D3D11_BOX box = { (UINT)x, (UINT)y, 0, (UINT)(x + width), (UINT)(y + height), 1 };
    ID3D11DeviceContext_UpdateSubresource(nw_d3d.ctx, (ID3D11Resource *)t->tex, (UINT)level, &box, data,
                                          (UINT)(width * nw_d3d_bpp(t->format)), 0);
}

static void nw_d3d_texture_destroy(NwRhiTexture *t)
{
    if (!t) {
        return;
    }
    for (int i = 0; i < NW_D3D_UNITS; i++) {
        if (nw_d3d.srv[i] == t->srv) {
            nw_d3d.srv[i] = NULL;
        }
    }
    if ((t->rtv && nw_d3d.rtv == t->rtv) || (t->dsv && nw_d3d.dsv == t->dsv)) {
        ID3D11DeviceContext_OMSetRenderTargets(nw_d3d.ctx, 0, NULL, NULL);
        nw_d3d.rtv = NULL;
        nw_d3d.dsv = NULL;
    }
    nw_d3d_release(t->rtv);
    nw_d3d_release(t->dsv);
    nw_d3d_release(t->srv);
    nw_d3d_release(t->tex);
    SDL_free(t);
}

static void nw_d3d_texture_size(const NwRhiTexture *t, int *width, int *height)
{
    *width = t ? t->w : 0;
    *height = t ? t->h : 0;
}

static uint64_t nw_d3d_texture_ui(NwRhiTexture *t)
{
    return t ? (uint64_t)(uintptr_t)t->srv : 0;
}

static void nw_d3d_generate_mips(NwRhiTexture *t)
{
    if (t->flags & NW_RHI_TEX_MIPGEN) {
        ID3D11DeviceContext_GenerateMips(nw_d3d.ctx, t->srv);
    }
}

static bool nw_d3d_texture_read(NwRhiTexture *t, int x, int y, int width, int height, void *rgba, int pitch)
{
    D3D11_TEXTURE2D_DESC td = { 0 };
    td.Width = (UINT)width;
    td.Height = (UINT)height;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_STAGING;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D *staging = NULL;
    HRESULT hr = ID3D11Device_CreateTexture2D(nw_d3d.dev, &td, NULL, &staging);
    if (FAILED(hr)) {
        return nw_d3d_fail("a staging texture", hr);
    }
    D3D11_BOX box = { (UINT)x, (UINT)y, 0, (UINT)(x + width), (UINT)(y + height), 1 };
    ID3D11DeviceContext_CopySubresourceRegion(nw_d3d.ctx, (ID3D11Resource *)staging, 0, 0, 0, 0,
                                              (ID3D11Resource *)t->tex, 0, &box);
    D3D11_MAPPED_SUBRESOURCE map;
    hr = ID3D11DeviceContext_Map(nw_d3d.ctx, (ID3D11Resource *)staging, 0, D3D11_MAP_READ, 0, &map);
    if (SUCCEEDED(hr)) {
        for (int i = 0; i < height; i++) {
            SDL_memcpy((uint8_t *)rgba + (size_t)i * (size_t)pitch, (const uint8_t *)map.pData + (size_t)i * map.RowPitch,
                       (size_t)width * 4);
        }
        ID3D11DeviceContext_Unmap(nw_d3d.ctx, (ID3D11Resource *)staging, 0);
    }
    ID3D11Texture2D_Release(staging);
    return SUCCEEDED(hr) || nw_d3d_fail("Map", hr);
}

/* ---- programs ---- */

static void nw_d3d_program_destroy(NwRhiProgram *p);

static NwRhiProgram *nw_d3d_program_create(const NwRhiProgramDesc *d)
{
    const NwD3dProgram *src = NULL;
    for (int i = 0; i < nw_d3d11_nsets && !src; i++) {
        if (SDL_strcmp(nw_d3d11_sets[i].name, d->set) == 0 && d->variant >= 0 && d->variant < nw_d3d11_sets[i].count) {
            src = &nw_d3d11_sets[i].programs[d->variant];
        }
    }
    if (!src) {
        SDL_SetError("Direct3D 11: no bytecode for %s %d (tools/gen_shaders.py)", d->set, d->variant);
        return NULL;
    }
    NwRhiProgram *p = SDL_calloc(1, sizeof(*p));
    if (!p) {
        return NULL;
    }
    p->src = src;
    HRESULT hr = ID3D11Device_CreateVertexShader(nw_d3d.dev, src->vs[0].code, src->vs[0].size, NULL, &p->vs[0]);
    if (SUCCEEDED(hr) && src->vs[1].code != src->vs[0].code) {
        hr = ID3D11Device_CreateVertexShader(nw_d3d.dev, src->vs[1].code, src->vs[1].size, NULL, &p->vs[1]);
    } else if (SUCCEEDED(hr)) {
        p->vs[1] = p->vs[0];
        ID3D11VertexShader_AddRef(p->vs[1]);
    }
    if (SUCCEEDED(hr)) {
        hr = ID3D11Device_CreatePixelShader(nw_d3d.dev, src->ps.code, src->ps.size, NULL, &p->ps);
    }
    for (int s = 0; SUCCEEDED(hr) && s < 2; s++) {
        if (!src->cbuffer[s]) {
            continue;
        }
        D3D11_BUFFER_DESC bd = { src->cbuffer[s], D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0 };
        hr = ID3D11Device_CreateBuffer(nw_d3d.dev, &bd, NULL, &p->cb[s]);
        p->data[s] = SDL_calloc(1, src->cbuffer[s]);
        p->dirty[s] = true;
    }
    if (FAILED(hr)) {
        nw_d3d_fail("a program", hr);
        nw_d3d_program_destroy(p);
        return NULL;
    }
    return p;
}

static void nw_d3d_program_destroy(NwRhiProgram *p)
{
    if (!p) {
        return;
    }
    if (nw_d3d.prog == p) {
        nw_d3d.prog = NULL;
    }
    for (int v = 0; v < 2; v++) {
        nw_d3d_release(p->vs[v]);
        nw_d3d_release(p->cb[v]);
        SDL_free(p->data[v]);
        for (int l = 0; l < NW_D3D_LAYOUTS; l++) {
            nw_d3d_release(p->il[v][l]);
        }
    }
    nw_d3d_release(p->ps);
    SDL_free(p);
}

static int nw_d3d_uniform(NwRhiProgram *p, const char *name)
{
    for (uint32_t i = 0; i < p->src->nuniforms; i++) {
        if (SDL_strcmp(p->src->uniforms[i].name, name) == 0) {
            return (int)i;
        }
    }
    return -1;
}

/* GL's std140 default block: arrays and matrix columns 16 bytes apart. */
static void nw_d3d_set_uniform(NwRhiProgram *p, int u, NwRhiUniformType type, int components, int count,
                               const void *values)
{
    if (!p || u < 0) {
        return;
    }
    const NwD3dUniform *un = &p->src->uniforms[u];
    count = SDL_min(count, (int)un->array);
    const uint8_t *v = values;
    for (int s = 0; s < 2; s++) {
        if (un->offset[s] == NW_D3D_ABSENT || !p->data[s]) {
            continue;
        }
        uint8_t *dst = p->data[s] + un->offset[s];
        size_t room = p->src->cbuffer[s] - un->offset[s];
        if (type == NW_RHI_MAT4) {
            SDL_memcpy(dst, v, SDL_min(room, (size_t)count * 64));
        } else if (type == NW_RHI_MAT3) {
            for (int c = 0; c < 3 && (size_t)c * 16 + 12 <= room; c++) {
                SDL_memcpy(dst + c * 16, v + c * 12, 12);
            }
        } else if (count == 1) {
            SDL_memcpy(dst, v, SDL_min(room, (size_t)components * 4));
        } else {
            for (int i = 0; i < count && (size_t)i * 16 + (size_t)components * 4 <= room; i++) {
                SDL_memcpy(dst + i * 16, v + (size_t)i * (size_t)components * 4, (size_t)components * 4);
            }
        }
        p->dirty[s] = true;
    }
}

static void nw_d3d_use_program(NwRhiProgram *p)
{
    nw_d3d.prog = p;
}

/* ---- vertex arrays ---- */

static NwRhiVertexArray *nw_d3d_vertex_array_create(void)
{
    NwRhiVertexArray *va = SDL_calloc(1, sizeof(*va));
    if (va) {
        va->layout = -1;
    }
    return va;
}

static int nw_d3d_layout_id(const NwRhiVertexLayout *l)
{
    for (int i = 0; i < nw_d3d.nlayouts; i++) {
        if (SDL_memcmp(&nw_d3d.layouts[i], l, sizeof(*l)) == 0) {
            return i;
        }
    }
    if (nw_d3d.nlayouts == NW_D3D_LAYOUTS) {
        SDL_Log("Direct3D 11: more than %d vertex layouts", NW_D3D_LAYOUTS);
        return -1;
    }
    SDL_memcpy(&nw_d3d.layouts[nw_d3d.nlayouts], l, sizeof(*l));
    return nw_d3d.nlayouts++;
}

static void nw_d3d_vertex_array_set(NwRhiVertexArray *va, const NwRhiVertexLayout *layout, NwRhiBuffer *vertices,
                                    size_t offset, NwRhiBuffer *indices)
{
    if (!va) {
        return;
    }
    va->layout = nw_d3d_layout_id(layout);
    va->vb = vertices;
    va->offset = offset;
    if (indices) {
        va->ib = indices;
    }
}

static void nw_d3d_vertex_array_destroy(NwRhiVertexArray *va)
{
    SDL_free(va);
}

/* ---- state ---- */

static void nw_d3d_apply_viewport(void)
{
    const int *v = nw_d3d.viewport;
    float y = nw_d3d.to_window ? (float)(nw_d3d.target_h - v[1] - v[3]) : (float)v[1];
    D3D11_VIEWPORT vp = { (float)v[0], y, (float)v[2], (float)v[3], 0.0f, 1.0f };
    ID3D11DeviceContext_RSSetViewports(nw_d3d.ctx, 1, &vp);
}

static void nw_d3d_bind_target(void)
{
    ID3D11DeviceContext_OMSetRenderTargets(nw_d3d.ctx, nw_d3d.rtv ? 1 : 0, nw_d3d.rtv ? &nw_d3d.rtv : NULL,
                                           nw_d3d.dsv);
}

static void nw_d3d_set_target(NwRhiTexture *colour, NwRhiTexture *depth)
{
    if (!colour && !depth) {
        nw_d3d_fit_window();
        nw_d3d.rtv = nw_d3d.back;
        nw_d3d.dsv = NULL;
        nw_d3d.to_window = true;
        nw_d3d.target_h = nw_d3d.back_h;
    } else {
        nw_d3d.rtv = colour ? colour->rtv : NULL;
        nw_d3d.dsv = depth ? depth->dsv : NULL;
        nw_d3d.to_window = false;
        nw_d3d.target_h = colour ? colour->h : depth->h;
    }
    /* A texture bound for reading cannot be drawn into: the units holding one go empty, as Direct3D would. */
    ID3D11ShaderResourceView *none[NW_D3D_UNITS] = { 0 };
    ID3D11DeviceContext_PSSetShaderResources(nw_d3d.ctx, 0, NW_D3D_UNITS, none);
    ID3D11DeviceContext_VSSetShaderResources(nw_d3d.ctx, 0, NW_D3D_UNITS, none);
    nw_d3d_bind_target();
}

static void nw_d3d_viewport(int x, int y, int width, int height)
{
    nw_d3d.viewport[0] = x;
    nw_d3d.viewport[1] = y;
    nw_d3d.viewport[2] = width;
    nw_d3d.viewport[3] = height;
    nw_d3d_apply_viewport();
}

static void nw_d3d_viewport_get(int out[4])
{
    SDL_memcpy(out, nw_d3d.viewport, sizeof(nw_d3d.viewport));
}

static void nw_d3d_blend(bool on, NwRhiBlendOp op, NwRhiBlendFactor src, NwRhiBlendFactor dst, NwRhiBlendOp aop,
                         NwRhiBlendFactor asrc, NwRhiBlendFactor adst)
{
    nw_d3d.blend_on = on;
    const uint8_t b[6] = { (uint8_t)op, (uint8_t)src, (uint8_t)dst, (uint8_t)aop, (uint8_t)asrc, (uint8_t)adst };
    SDL_memcpy(nw_d3d.blend, b, sizeof(b));
}

static void nw_d3d_colour_mask(bool r, bool g, bool b, bool a)
{
    nw_d3d.mask = (uint8_t)(r | g << 1 | b << 2 | a << 3);
}

static void nw_d3d_depth(bool test, NwRhiCompare func)
{
    nw_d3d.depth_test = test;
    nw_d3d.depth_func = (uint8_t)func;
}

static void nw_d3d_depth_write(bool on)
{
    nw_d3d.depth_write = on;
}

static void nw_d3d_stencil(bool test, NwRhiCompare func, uint8_t ref, uint8_t read_mask, NwRhiStencilOp fail,
                           NwRhiStencilOp zfail, NwRhiStencilOp pass)
{
    nw_d3d.stencil_test = test;
    if (test) {
        nw_d3d.stencil_func = (uint8_t)func;
        nw_d3d.stencil_ref = ref;
        nw_d3d.stencil_read = read_mask;
        nw_d3d.stencil_ops[0] = (uint8_t)fail;
        nw_d3d.stencil_ops[1] = (uint8_t)zfail;
        nw_d3d.stencil_ops[2] = (uint8_t)pass;
    }
}

static void nw_d3d_stencil_write_mask(uint8_t mask)
{
    nw_d3d.stencil_write = mask;
}

static void nw_d3d_wireframe(bool on)
{
    nw_d3d.wireframe = on;
}

static void nw_d3d_bind_texture(int unit, NwRhiTexture *t)
{
    if (unit >= 0 && unit < NW_D3D_UNITS) {
        nw_d3d.srv[unit] = t ? t->srv : NULL;
    }
}

static void nw_d3d_bind_texels(int unit, NwRhiBuffer *b)
{
    if (unit >= 0 && unit < NW_D3D_UNITS) {
        nw_d3d.srv[unit] = b ? b->srv : NULL;
    }
}

static const D3D11_BLEND nw_d3d_factors[] = { D3D11_BLEND_ZERO,       D3D11_BLEND_ONE,       D3D11_BLEND_SRC_ALPHA,
                                              D3D11_BLEND_INV_SRC_ALPHA, D3D11_BLEND_DEST_COLOR, D3D11_BLEND_DEST_ALPHA,
                                              D3D11_BLEND_INV_DEST_ALPHA };
static const D3D11_BLEND_OP nw_d3d_ops[] = { D3D11_BLEND_OP_ADD, D3D11_BLEND_OP_SUBTRACT, D3D11_BLEND_OP_REV_SUBTRACT };
static const D3D11_STENCIL_OP nw_d3d_stencil_ops[] = { D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_INCR,
                                                       D3D11_STENCIL_OP_DECR, D3D11_STENCIL_OP_REPLACE };

static ID3D11BlendState *nw_d3d_blend_state(bool on, const uint8_t b[6], uint8_t mask)
{
    uint64_t key = (uint64_t)on | (uint64_t)mask << 1;
    for (int i = 0; on && i < 6; i++) {
        key |= (uint64_t)b[i] << (5 + i * 4);
    }
    for (int i = 0; i < nw_d3d.nblends; i++) {
        if (nw_d3d.blends[i].key == key) {
            return (ID3D11BlendState *)nw_d3d.blends[i].obj;
        }
    }
    D3D11_BLEND_DESC bd = { 0 };
    D3D11_RENDER_TARGET_BLEND_DESC *rt = &bd.RenderTarget[0];
    rt->BlendEnable = on;
    rt->BlendOp = nw_d3d_ops[on ? b[0] : 0];
    rt->SrcBlend = nw_d3d_factors[on ? b[1] : 1];
    rt->DestBlend = nw_d3d_factors[on ? b[2] : 0];
    rt->BlendOpAlpha = nw_d3d_ops[on ? b[3] : 0];
    rt->SrcBlendAlpha = nw_d3d_factors[on ? b[4] : 1];
    rt->DestBlendAlpha = nw_d3d_factors[on ? b[5] : 0];
    rt->RenderTargetWriteMask = mask;
    ID3D11BlendState *s = NULL;
    if (FAILED(ID3D11Device_CreateBlendState(nw_d3d.dev, &bd, &s))) {
        return NULL;
    }
    if (nw_d3d.nblends < NW_D3D_STATES) {
        nw_d3d.blends[nw_d3d.nblends++] = (NwD3dState){ key, (IUnknown *)s };
    } else {
        ID3D11BlendState_Release(s); /* used once, released after the draw takes it (the context holds it) */
    }
    return s;
}

static ID3D11DepthStencilState *nw_d3d_ds_state(bool test, uint8_t func, bool write, bool stencil, uint8_t sfunc,
                                                uint8_t read, uint8_t swrite, const uint8_t ops[3])
{
    uint64_t key = (uint64_t)test | (uint64_t)func << 1 | (uint64_t)write << 4 | (uint64_t)stencil << 5;
    if (stencil) {
        key |= (uint64_t)sfunc << 6 | (uint64_t)read << 9 | (uint64_t)swrite << 17 | (uint64_t)ops[0] << 25 |
               (uint64_t)ops[1] << 27 | (uint64_t)ops[2] << 29;
    }
    for (int i = 0; i < nw_d3d.ndss; i++) {
        if (nw_d3d.dss[i].key == key) {
            return (ID3D11DepthStencilState *)nw_d3d.dss[i].obj;
        }
    }
    D3D11_DEPTH_STENCIL_DESC dd = { 0 };
    dd.DepthEnable = test;
    dd.DepthWriteMask = write ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;
    dd.DepthFunc = (D3D11_COMPARISON_FUNC)(D3D11_COMPARISON_NEVER + func);
    dd.StencilEnable = stencil;
    dd.StencilReadMask = read;
    dd.StencilWriteMask = swrite;
    D3D11_DEPTH_STENCILOP_DESC f = { nw_d3d_stencil_ops[ops[0]], nw_d3d_stencil_ops[ops[1]],
                                     nw_d3d_stencil_ops[ops[2]],
                                     (D3D11_COMPARISON_FUNC)(D3D11_COMPARISON_NEVER + sfunc) };
    dd.FrontFace = dd.BackFace = f;
    ID3D11DepthStencilState *s = NULL;
    if (FAILED(ID3D11Device_CreateDepthStencilState(nw_d3d.dev, &dd, &s))) {
        return NULL;
    }
    if (nw_d3d.ndss < NW_D3D_STATES) {
        nw_d3d.dss[nw_d3d.ndss++] = (NwD3dState){ key, (IUnknown *)s };
    } else {
        ID3D11DepthStencilState_Release(s);
    }
    return s;
}

static ID3D11RasterizerState *nw_d3d_rs_state(bool wireframe, bool scissor)
{
    int i = wireframe | scissor << 1;
    if (!nw_d3d.rss[i].obj) {
        D3D11_RASTERIZER_DESC rd = { 0 };
        rd.FillMode = wireframe ? D3D11_FILL_WIREFRAME : D3D11_FILL_SOLID;
        rd.CullMode = D3D11_CULL_NONE;
        rd.DepthClipEnable = TRUE;
        rd.ScissorEnable = scissor;
        rd.MultisampleEnable = TRUE;
        ID3D11Device_CreateRasterizerState(nw_d3d.dev, &rd, (ID3D11RasterizerState **)&nw_d3d.rss[i].obj);
    }
    return (ID3D11RasterizerState *)nw_d3d.rss[i].obj;
}

static const DXGI_FORMAT nw_d3d_attrib_formats[][4] = {
    [NW_RHI_F32] = { DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R32G32_FLOAT, DXGI_FORMAT_R32G32B32_FLOAT,
                     DXGI_FORMAT_R32G32B32A32_FLOAT },
    [NW_RHI_S16_FLOAT] = { DXGI_FORMAT_R16_SINT, DXGI_FORMAT_R16G16_SINT, DXGI_FORMAT_R16G16B16A16_SINT,
                           DXGI_FORMAT_R16G16B16A16_SINT },
    [NW_RHI_U8_FLOAT] = { DXGI_FORMAT_R8_UINT, DXGI_FORMAT_R8G8_UINT, DXGI_FORMAT_R8G8B8A8_UINT,
                          DXGI_FORMAT_R8G8B8A8_UINT },
    [NW_RHI_U8_INT] = { DXGI_FORMAT_R8_UINT, DXGI_FORMAT_R8G8_UINT, DXGI_FORMAT_R8G8B8A8_UINT,
                        DXGI_FORMAT_R8G8B8A8_UINT },
    [NW_RHI_U16_INT] = { DXGI_FORMAT_R16_UINT, DXGI_FORMAT_R16G16_UINT, DXGI_FORMAT_R16G16B16A16_UINT,
                         DXGI_FORMAT_R16G16B16A16_UINT },
    [NW_RHI_U32_INT] = { DXGI_FORMAT_R32_UINT, DXGI_FORMAT_R32G32_UINT, DXGI_FORMAT_R32G32B32_UINT,
                         DXGI_FORMAT_R32G32B32A32_UINT },
};

/* The program's vertex stage v reading layout l: its inputs from the vertex buffer (slot 0), or GL's (0, 0, 0, 1) from
   the dummy (slot 1) where the layout has none. */
static ID3D11InputLayout *nw_d3d_input_layout(NwRhiProgram *p, int v, int lid)
{
    if (p->il[v][lid]) {
        return p->il[v][lid];
    }
    static const DXGI_FORMAT dummy[3] = { DXGI_FORMAT_R32G32B32A32_FLOAT, DXGI_FORMAT_R32G32B32A32_UINT,
                                          DXGI_FORMAT_R32G32B32A32_SINT };
    const NwRhiVertexLayout *l = &nw_d3d.layouts[lid];
    D3D11_INPUT_ELEMENT_DESC el[16];
    int n = 0;
    for (int i = 0; i < p->src->ninputs[v] && n < 16; i++) {
        const NwD3dInput *in = &p->src->inputs[v][i];
        const NwRhiAttrib *a = NULL;
        for (int k = 0; k < l->count && !a; k++) {
            a = l->attribs[k].location == in->location ? &l->attribs[k] : NULL;
        }
        D3D11_INPUT_ELEMENT_DESC *e = &el[n++];
        e->SemanticName = "TEXCOORD";
        e->SemanticIndex = in->location;
        if (a) {
            e->Format = nw_d3d_attrib_formats[a->type][a->components - 1];
            e->InputSlot = 0;
            e->AlignedByteOffset = a->offset;
            e->InputSlotClass = l->instanced ? D3D11_INPUT_PER_INSTANCE_DATA : D3D11_INPUT_PER_VERTEX_DATA;
            e->InstanceDataStepRate = l->instanced ? 1 : 0;
        } else {
            e->Format = dummy[in->kind];
            e->InputSlot = 1;
            e->AlignedByteOffset = in->kind * 16u;
            e->InputSlotClass = D3D11_INPUT_PER_VERTEX_DATA;
            e->InstanceDataStepRate = 0;
        }
    }
    HRESULT hr = ID3D11Device_CreateInputLayout(nw_d3d.dev, el, (UINT)n, p->src->vs[v].code, p->src->vs[v].size,
                                                &p->il[v][lid]);
    if (FAILED(hr)) {
        static bool told;
        if (!told) {
            SDL_Log("Direct3D 11: CreateInputLayout failed (0x%08lx)", (unsigned long)hr);
            told = true;
        }
    }
    return p->il[v][lid];
}

/* Everything a draw takes, set; false when it cannot draw. */
static bool nw_d3d_apply(NwRhiVertexArray *va)
{
    NwRhiProgram *p = nw_d3d.prog;
    if (!p || !va || va->layout < 0 || !va->vb || !va->vb->buf) {
        return false;
    }
    const NwRhiVertexLayout *l = &nw_d3d.layouts[va->layout];
    int v = 0;
    for (int k = 0; k < l->count; k++) {
        v |= l->attribs[k].location == 1 && l->attribs[k].type == NW_RHI_S16_FLOAT;
    }
    ID3D11InputLayout *il = nw_d3d_input_layout(p, v, va->layout);
    if (!il) {
        return false;
    }
    ID3D11DeviceContext *c = nw_d3d.ctx;
    ID3D11DeviceContext_IASetInputLayout(c, il);
    ID3D11DeviceContext_IASetPrimitiveTopology(c, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11Buffer *vbs[2] = { va->vb->buf, nw_d3d.dummy };
    UINT strides[2] = { l->stride, 0 }, offsets[2] = { (UINT)va->offset, 0 };
    ID3D11DeviceContext_IASetVertexBuffers(c, 0, 2, vbs, strides, offsets);
    for (int s = 0; s < 2; s++) {
        if (p->cb[s] && p->dirty[s]) {
            ID3D11DeviceContext_UpdateSubresource(c, (ID3D11Resource *)p->cb[s], 0, NULL, p->data[s], 0, 0);
            p->dirty[s] = false;
        }
    }
    ID3D11DeviceContext_VSSetShader(c, p->vs[v], NULL, 0);
    ID3D11DeviceContext_PSSetShader(c, p->ps, NULL, 0);
    ID3D11DeviceContext_VSSetConstantBuffers(c, 0, 1, &p->cb[0]);
    ID3D11DeviceContext_PSSetConstantBuffers(c, 0, 1, &p->cb[1]);
    ID3D11DeviceContext_VSSetShaderResources(c, 0, NW_D3D_UNITS, nw_d3d.srv);
    ID3D11DeviceContext_PSSetShaderResources(c, 0, NW_D3D_UNITS, nw_d3d.srv);
    ID3D11SamplerState *samplers[NW_D3D_UNITS];
    for (int i = 0; i < NW_D3D_UNITS; i++) {
        samplers[i] = nw_d3d.point;
    }
    ID3D11DeviceContext_PSSetSamplers(c, 0, NW_D3D_UNITS, samplers);
    const float factor[4] = { 0 };
    ID3D11DeviceContext_OMSetBlendState(c, nw_d3d_blend_state(nw_d3d.blend_on, nw_d3d.blend, nw_d3d.mask), factor,
                                        0xFFFFFFFFu);
    ID3D11DeviceContext_OMSetDepthStencilState(
        c,
        nw_d3d_ds_state(nw_d3d.depth_test, nw_d3d.depth_func, nw_d3d.depth_write, nw_d3d.stencil_test,
                        nw_d3d.stencil_func, nw_d3d.stencil_read, nw_d3d.stencil_write, nw_d3d.stencil_ops),
        nw_d3d.stencil_ref);
    ID3D11DeviceContext_RSSetState(c, nw_d3d_rs_state(nw_d3d.wireframe, false));
    nw_d3d_apply_viewport();
    return true;
}

/* ---- work ---- */

static void nw_d3d_draw(NwRhiVertexArray *va, uint32_t first, uint32_t count)
{
    if (nw_d3d_apply(va)) {
        ID3D11DeviceContext_Draw(nw_d3d.ctx, count, first);
    }
}

static void nw_d3d_draw_indexed(NwRhiVertexArray *va, NwRhiBuffer *indices, uint32_t first, uint32_t count)
{
    NwRhiBuffer *ib = indices ? indices : va ? va->ib : NULL;
    if (ib && ib->buf && nw_d3d_apply(va)) {
        ID3D11DeviceContext_IASetIndexBuffer(nw_d3d.ctx, ib->buf, DXGI_FORMAT_R32_UINT, 0);
        ID3D11DeviceContext_DrawIndexed(nw_d3d.ctx, count, first, 0);
    }
}

static void nw_d3d_draw_instanced(NwRhiVertexArray *va, uint32_t first, uint32_t count, uint32_t instances)
{
    if (nw_d3d_apply(va)) {
        ID3D11DeviceContext_DrawInstanced(nw_d3d.ctx, count, instances, first, 0);
    }
}

typedef struct NwD3dUtilConsts {
    float map[4], scale[4], colour[4], depth[4];
} NwD3dUtilConsts;

/* One triangle over the viewport with a util pixel shader; the draw state is set again by the next draw. */
static void nw_d3d_util_draw(int ps, const NwD3dUtilConsts *k, ID3D11BlendState *blend, ID3D11DepthStencilState *ds,
                             UINT ref, bool scissor)
{
    ID3D11DeviceContext *c = nw_d3d.ctx;
    ID3D11DeviceContext_UpdateSubresource(c, (ID3D11Resource *)nw_d3d.util_cb, 0, NULL, k, 0, 0);
    ID3D11DeviceContext_IASetInputLayout(c, NULL);
    ID3D11DeviceContext_IASetPrimitiveTopology(c, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_VSSetShader(c, nw_d3d.util_vs, NULL, 0);
    ID3D11DeviceContext_PSSetShader(c, nw_d3d.util_ps[ps], NULL, 0);
    ID3D11DeviceContext_PSSetConstantBuffers(c, 0, 1, &nw_d3d.util_cb);
    ID3D11DeviceContext_PSSetSamplers(c, 0, 1, &nw_d3d.linear);
    const float factor[4] = { 0 };
    ID3D11DeviceContext_OMSetBlendState(c, blend, factor, 0xFFFFFFFFu);
    ID3D11DeviceContext_OMSetDepthStencilState(c, ds, ref);
    ID3D11DeviceContext_RSSetState(c, nw_d3d_rs_state(false, scissor));
    ID3D11DeviceContext_Draw(c, 3, 0);
}

static void nw_d3d_clear(const NwRhiClear *c)
{
    ID3D11DeviceContext *ctx = nw_d3d.ctx;
    bool all = c->mask[0] && c->mask[1] && c->mask[2] && c->mask[3];
    bool colour = c->colour && nw_d3d.rtv && (c->mask[0] || c->mask[1] || c->mask[2] || c->mask[3]);
    bool depth = c->depth && c->depth_write && nw_d3d.dsv;
    bool stencil = c->stencil && c->stencil_write && nw_d3d.dsv;
    if (colour && !c->scissor && all) {
        ID3D11DeviceContext_ClearRenderTargetView(ctx, nw_d3d.rtv, c->rgba);
        colour = false;
    }
    if ((depth || stencil) && !c->scissor && (!stencil || c->stencil_write == 0xFF)) {
        UINT flags = (depth ? D3D11_CLEAR_DEPTH : 0) | (stencil ? D3D11_CLEAR_STENCIL : 0);
        ID3D11DeviceContext_ClearDepthStencilView(ctx, nw_d3d.dsv, flags, (FLOAT)c->z, c->s);
        depth = stencil = false;
    }
    if (!colour && !depth && !stencil) {
        return;
    }
    /* The rest drawn: the whole target, cut to the scissor, writing what the masks let through. */
    int w = nw_d3d.to_window ? nw_d3d.back_w : 0, h = nw_d3d.target_h;
    if (!nw_d3d.to_window) {
        ID3D11Resource *r = NULL;
        D3D11_TEXTURE2D_DESC td;
        if (nw_d3d.rtv) {
            ID3D11RenderTargetView_GetResource(nw_d3d.rtv, &r);
        } else {
            ID3D11DepthStencilView_GetResource(nw_d3d.dsv, &r);
        }
        ID3D11Texture2D_GetDesc((ID3D11Texture2D *)r, &td);
        IUnknown_Release(r);
        w = (int)td.Width;
    }
    D3D11_VIEWPORT vp = { 0.0f, 0.0f, (float)w, (float)h, 0.0f, 1.0f };
    ID3D11DeviceContext_RSSetViewports(ctx, 1, &vp);
    if (c->scissor) {
        const int *s = c->scissor;
        int top = nw_d3d.to_window ? h - s[1] - s[3] : s[1];
        D3D11_RECT r = { s[0], top, s[0] + s[2], top + s[3] };
        ID3D11DeviceContext_RSSetScissorRects(ctx, 1, &r);
    }
    uint8_t mask = colour ? (uint8_t)(c->mask[0] | c->mask[1] << 1 | c->mask[2] << 2 | c->mask[3] << 3) : 0;
    static const uint8_t none[6] = { 0 };
    static const uint8_t replace[3] = { NW_RHI_REPLACE, NW_RHI_REPLACE, NW_RHI_REPLACE };
    ID3D11DepthStencilState *ds = nw_d3d_ds_state(depth, NW_RHI_ALWAYS, depth, stencil, NW_RHI_ALWAYS, 0xFF,
                                                  c->stencil_write, replace);
    NwD3dUtilConsts k = { { 0 }, { 0 }, { c->rgba[0], c->rgba[1], c->rgba[2], c->rgba[3] }, { (float)c->z } };
    nw_d3d_util_draw(NW_D3D_CLEAR, &k, nw_d3d_blend_state(false, none, mask), ds, c->s, c->scissor != NULL);
    nw_d3d_apply_viewport();
}

/* A blit drawn: into the destination's colour (or depth), its rectangle the viewport, the source read by ps. */
static void nw_d3d_blit_draw(const NwRhiBlit *b, int ps, NwRhiTexture *src, ID3D11RenderTargetView *rtv,
                             ID3D11DepthStencilView *dsv, int target_h)
{
    ID3D11DeviceContext *c = nw_d3d.ctx;
    bool window = !b->dst_colour && !b->dst_depth;
    ID3D11ShaderResourceView *none[3] = { 0 };
    ID3D11DeviceContext_PSSetShaderResources(c, 0, 3, none);
    ID3D11DeviceContext_OMSetRenderTargets(c, rtv ? 1 : 0, rtv ? &rtv : NULL, dsv);
    int dw = b->dst[2] - b->dst[0], dh = b->dst[3] - b->dst[1];
    float top = window ? (float)(target_h - b->dst[3]) : (float)b->dst[1];
    D3D11_VIEWPORT vp = { (float)b->dst[0], top, (float)dw, (float)dh, 0.0f, 1.0f };
    ID3D11DeviceContext_RSSetViewports(c, 1, &vp);
    int slot = ps == NW_D3D_BLIT_DEPTH ? 1 : ps == NW_D3D_BLIT_DEPTH_MS ? 2 : 0;
    ID3D11DeviceContext_PSSetShaderResources(c, (UINT)slot, 1, &src->srv);
    NwD3dUtilConsts k = {
        { (float)b->src[0], (float)b->src[1], (float)b->dst[0], (float)b->dst[1] },
        { (float)(b->src[2] - b->src[0]) / (float)dw, (float)(b->src[3] - b->src[1]) / (float)dh,
          window ? (float)target_h : 0.0f, (float)b->src_level },
        { 0 },
        { 0 },
    };
    static const uint8_t none6[6] = { 0 };
    static const uint8_t keep[3] = { 0 };
    bool depth = ps == NW_D3D_BLIT_DEPTH || ps == NW_D3D_BLIT_DEPTH_MS;
    nw_d3d_util_draw(ps, &k, nw_d3d_blend_state(false, none6, depth ? 0 : 0xF),
                     nw_d3d_ds_state(depth, NW_RHI_ALWAYS, depth, false, NW_RHI_ALWAYS, 0xFF, 0xFF, keep), 0, false);
    ID3D11DeviceContext_PSSetShaderResources(c, 0, 3, none);
}

static void nw_d3d_blit(const NwRhiBlit *b)
{
    ID3D11DeviceContext *c = nw_d3d.ctx;
    bool window = !b->dst_colour && !b->dst_depth;
    int sw = b->src[2] - b->src[0], sh = b->src[3] - b->src[1];
    int dw = b->dst[2] - b->dst[0], dh = b->dst[3] - b->dst[1];
    if (sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0) {
        return;
    }
    bool same = !window && sw == dw && sh == dh && !b->src_level;
    if (window) {
        nw_d3d_fit_window();
    }
    if (b->colour && b->src_colour) {
        NwRhiTexture *s = b->src_colour, *d = b->dst_colour;
        bool full = same && sw == s->w && sh == s->h && d && sw == d->w && sh == d->h;
        if (same && d && s->samples > 1 && d->samples <= 1 && full) {
            ID3D11DeviceContext_ResolveSubresource(c, (ID3D11Resource *)d->tex, 0, (ID3D11Resource *)s->tex, 0,
                                                   DXGI_FORMAT_R8G8B8A8_UNORM);
        } else if (same && d && s->samples == d->samples && (s->samples <= 1 || full)) {
            D3D11_BOX box = { (UINT)b->src[0], (UINT)b->src[1], 0, (UINT)b->src[2], (UINT)b->src[3], 1 };
            ID3D11DeviceContext_CopySubresourceRegion(c, (ID3D11Resource *)d->tex, 0, (UINT)b->dst[0], (UINT)b->dst[1],
                                                      0, (ID3D11Resource *)s->tex, 0, &box);
        } else if (s->samples <= 1) {
            nw_d3d_blit_draw(b, b->linear ? NW_D3D_BLIT_LINEAR : NW_D3D_BLIT_POINT, s, window ? nw_d3d.back : d->rtv,
                             NULL, window ? nw_d3d.back_h : d->h);
        }
    }
    if ((b->depth || b->stencil) && b->src_depth && b->dst_depth) {
        NwRhiTexture *s = b->src_depth, *d = b->dst_depth;
        bool full = same && sw == s->w && sh == s->h && sw == d->w && sh == d->h;
        if (full && s->samples == d->samples) {
            ID3D11DeviceContext_CopyResource(c, (ID3D11Resource *)d->tex, (ID3D11Resource *)s->tex);
        } else if (d->samples <= 1) {
            /* Depth alone; an MSAA resolve's stencil is not carried over (xbox/RENDERERS.md). */
            nw_d3d_blit_draw(b, s->samples > 1 ? NW_D3D_BLIT_DEPTH_MS : NW_D3D_BLIT_DEPTH, s, NULL, d->dsv, d->h);
        }
    }
    nw_d3d_bind_target();
    nw_d3d_apply_viewport();
}

#define NW_D3D_ENTRY_X(name, ret, params, args) .name = nw_d3d_##name,
#define NW_D3D_ENTRY_V(name, params, args) .name = nw_d3d_##name,
const NwRhiBackend nw_rhi_d3d11 = { .prepare_window = nw_d3d_prepare_window,
                                    NW_RHI_CALLS(NW_D3D_ENTRY_X, NW_D3D_ENTRY_V) };

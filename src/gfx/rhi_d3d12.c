/* src/gfx/rhi.h over Direct3D 12: a device (the GPU's, else WARP), one direct queue, one command list recorded per
   frame with NW_RHI_D3D12_FRAMES frames in flight, and a flip-model swap chain on the SDL window (its HWND on the
   desktop, its CoreWindow in the UWP build). The programs are the Direct3D 11 renderer's shader model 5.0 bytecode
   (tools/gen_shaders.py; Direct3D 12 takes it as is), so every convention of rhi_d3d11.c holds: the vertex stages flip
   y, textures keep GL's rows, only the window is drawn upside down back.

   What Direct3D 11 did by itself is done here:
   - State: one pipeline state object per program, vertex layout, blend, depth-stencil, fill mode and target formats,
     cached (nw_dx_pso). One root signature: b0 of the vertex stage, b0 of the pixel stage (root CBVs), t0-t8 (a
     descriptor table), s0-s8 point-clamp static samplers. The blits and clears of d3d11_util.hlsl have their own.
   - Memory: a per-frame upload ring. Uniforms, stream buffers (rewritten many times a frame) and texture uploads are
     written into it; a stream buffer keeps a CPU copy and is written again when drawn after the ring moved on (a
     new frame or a wait). Other buffers get a new default-heap buffer per nw_rhi_buffer_data, the old one released
     when the GPU is done with it.
   - Descriptors: every texture has a CPU-only SRV; a draw copies its nine units into a table of the frame's part of
     the one shader-visible heap (shared with Dear ImGui, whose descriptors live in a fixed part).
   - Resource states: tracked per texture (all subresources together) and changed by barriers right before use. */
#define COBJMACROS
#define WIDL_C_INLINE_WRAPPERS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <d3d12.h>
#include <dxgi1_4.h>

#include "gfx/rhi_d3d11_shaders.h"
#include "gfx/rhi_d3d12.h"
#include "gfx/rhi_impl.h"

#ifndef D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING
#define D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING 0x1688
#endif
#ifndef DXGI_SWAP_EFFECT_FLIP_DISCARD
#define DXGI_SWAP_EFFECT_FLIP_DISCARD ((DXGI_SWAP_EFFECT)4)
#endif

#define NW_DX_FRAMES NW_RHI_D3D12_FRAMES
#define NW_DX_BACK 2                    /* swap chain buffers */
#define NW_DX_UNITS 9                   /* the texture units vu.c binds: 0-8 */
#define NW_DX_LAYOUTS 16                /* distinct vertex layouts */
#define NW_DX_RING ((uint64_t)32 << 20) /* a frame's upload ring */
#define NW_DX_UI 256                    /* shader-visible descriptors kept for Dear ImGui */
#define NW_DX_TABLES 32768              /* a frame's shader-visible descriptors for draws */
#define NW_DX_SRVS 65536                /* CPU-only SRVs (one a texture) */
#define NW_DX_RTVS 4096
#define NW_DX_DSVS 1024
#define NW_DX_PSOS 4096 /* cached pipeline states (a power of two) */
#define NW_DX_BARRIERS 32
#define NW_DX_SRV (D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)
#define NW_DX_READ (D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER | D3D12_RESOURCE_STATE_INDEX_BUFFER | NW_DX_SRV)

struct NwRhiBuffer {
    NwRhiBufferKind kind;
    NwRhiFormat texels;
    bool stream;
    size_t size;
    ID3D12Resource *res; /* where draws read the data: own (static) or the ring (stream) */
    uint64_t off;
    uint64_t epoch;  /* stream: the ring epoch res/off belong to */
    uint64_t serial; /* new with every placement (descriptor tables compare it) */
    uint8_t *shadow; /* stream: the data, for the next placement */
    size_t shadow_cap;
    ID3D12Resource *own;
};

struct NwRhiTexture {
    NwRhiFormat format;
    int w, h, levels, samples;
    unsigned flags;
    ID3D12Resource *res;
    D3D12_RESOURCE_STATES state; /* of all subresources */
    DXGI_FORMAT fmt;             /* the resource's */
    int srv, rtv, dsv, ui;       /* descriptor slots, -1 none */
    uint64_t serial;
};

struct NwRhiProgram {
    const NwD3dProgram *src;
    uint8_t *data[2];
    bool dirty[2];
    D3D12_GPU_VIRTUAL_ADDRESS cb[2];
    uint64_t cb_epoch[2];
};

struct NwRhiVertexArray {
    int layout; /* in nw_dx.layouts, -1 none */
    NwRhiBuffer *vb, *ib;
    size_t offset;
};

/* Everything a pipeline state object is made of. */
typedef struct NwDxPsoKey {
    const NwRhiProgram *prog; /* NULL: a util shader */
    uint8_t util, v, layout, wireframe;
    uint8_t blend_on, blend[6], mask;
    uint8_t depth_test, depth_func, depth_write, stencil_test;
    uint8_t stencil_func, stencil_read, stencil_write, stencil_ops[3];
    uint8_t colour, depth, samples, pad;
} NwDxPsoKey;

typedef struct NwDxPso {
    NwDxPsoKey key;
    ID3D12PipelineState *pso;
} NwDxPso;

typedef struct NwDxSlots {
    int *free;
    int nfree, next, cap;
} NwDxSlots;

typedef struct NwDxLater {
    IUnknown *obj;
    int ui; /* a Dear ImGui slot to free, -1 none */
} NwDxLater;

static struct {
    SDL_Window *window;
    IDXGIFactory4 *factory;
    ID3D12Device *dev;
    ID3D12CommandQueue *queue;
    ID3D12CommandAllocator *alloc[NW_DX_FRAMES];
    ID3D12GraphicsCommandList *list;
    ID3D12Fence *fence;
    HANDLE event;
    UINT64 fence_value, frame_fence[NW_DX_FRAMES];
    int frame;
    uint64_t epoch; /* new whenever the ring and the tables start over */
    uint64_t serial;
    bool lost;
    /* the window */
    IDXGISwapChain3 *swap;
    ID3D12Resource *back[NW_DX_BACK];
    D3D12_RESOURCE_STATES back_state[NW_DX_BACK];
    int back_rtv[NW_DX_BACK];
    UINT back_index;
    int back_w, back_h;
    bool warp;
    NwRhiCaps caps;
    char description[256];
    int interval;
    /* memory */
    ID3D12Resource *ring[NW_DX_FRAMES];
    uint8_t *ring_map[NW_DX_FRAMES];
    D3D12_GPU_VIRTUAL_ADDRESS ring_va[NW_DX_FRAMES];
    uint64_t ring_used;
    NwDxLater *later[NW_DX_FRAMES];
    int nlater[NW_DX_FRAMES], later_cap[NW_DX_FRAMES];
    ID3D12Resource *dummy; /* GL's (0, 0, 0, 1) for inputs a layout lacks: float, uint, int */
    /* descriptors */
    ID3D12DescriptorHeap *gpu_heap, *cpu_heap, *rtv_heap, *dsv_heap;
    D3D12_CPU_DESCRIPTOR_HANDLE gpu_cpu0, cpu0, rtv0, dsv0;
    D3D12_GPU_DESCRIPTOR_HANDLE gpu0;
    UINT inc_srv, inc_rtv, inc_dsv;
    NwDxSlots srvs, rtvs, dsvs, uis;
    int table_used;
    int null_tex, null_buf, null_ms, scratch_srv, scratch_rtv;
    /* pipelines */
    ID3D12RootSignature *root, *util_root;
    NwDxPso *psos;
    int npsos;
    NwRhiVertexLayout layouts[NW_DX_LAYOUTS];
    int nlayouts;
    /* barriers waiting to be recorded */
    D3D12_RESOURCE_BARRIER barriers[NW_DX_BARRIERS];
    int nbarriers;
    /* what the draws take */
    NwRhiTexture *col, *ds;
    bool to_window;
    int target_w, target_h;
    int viewport[4];
    NwRhiProgram *prog;
    NwRhiTexture *tex[NW_DX_UNITS];
    NwRhiBuffer *buf[NW_DX_UNITS];
    uint8_t stencil_ref;
    bool wireframe;
    bool blend_on;
    uint8_t blend[6];
    uint8_t mask;
    bool depth_test, depth_write;
    uint8_t depth_func;
    bool stencil_test;
    uint8_t stencil_func, stencil_read, stencil_write, stencil_ops[3];
    /* what the command list has */
    ID3D12RootSignature *bound_root;
    ID3D12PipelineState *bound_pso;
    D3D12_GPU_VIRTUAL_ADDRESS bound_cb[2];
    UINT64 bound_table;
    int bound_ref;
    bool target_dirty, viewport_dirty;
    uint64_t table_sig[NW_DX_UNITS], table_epoch;
    D3D12_GPU_DESCRIPTOR_HANDLE table;
} nw_dx;

static bool nw_dx_fail(const char *what, HRESULT hr)
{
    return SDL_SetError("Direct3D 12: %s failed (0x%08lx)", what, (unsigned long)hr);
}

static void nw_dx_release(void *p)
{
    if (p) {
        IUnknown_Release((IUnknown *)p);
    }
}

static uint64_t nw_dx_align(uint64_t v, uint64_t a)
{
    return (v + a - 1) & ~(a - 1);
}

/* ---- descriptor slots ---- */

static bool nw_dx_slots_init(NwDxSlots *s, int cap)
{
    s->free = SDL_malloc(sizeof(int) * (size_t)cap);
    s->nfree = s->next = 0;
    s->cap = cap;
    return s->free != NULL;
}

static int nw_dx_slot_get(NwDxSlots *s)
{
    if (s->nfree) {
        return s->free[--s->nfree];
    }
    if (s->next < s->cap) {
        return s->next++;
    }
    static bool told;
    if (!told) {
        SDL_Log("Direct3D 12: out of descriptors (%d)", s->cap);
        told = true;
    }
    return -1;
}

static void nw_dx_slot_put(NwDxSlots *s, int i)
{
    if (i >= 0 && s->nfree < s->cap) {
        s->free[s->nfree++] = i;
    }
}

static D3D12_CPU_DESCRIPTOR_HANDLE nw_dx_cpu(int slot)
{
    D3D12_CPU_DESCRIPTOR_HANDLE h = { nw_dx.cpu0.ptr + (SIZE_T)slot * nw_dx.inc_srv };
    return h;
}

static D3D12_CPU_DESCRIPTOR_HANDLE nw_dx_rtv(int slot)
{
    D3D12_CPU_DESCRIPTOR_HANDLE h = { nw_dx.rtv0.ptr + (SIZE_T)slot * nw_dx.inc_rtv };
    return h;
}

static D3D12_CPU_DESCRIPTOR_HANDLE nw_dx_dsv(int slot)
{
    D3D12_CPU_DESCRIPTOR_HANDLE h = { nw_dx.dsv0.ptr + (SIZE_T)slot * nw_dx.inc_dsv };
    return h;
}

/* ---- releasing after the GPU ---- */

static void nw_dx_later(IUnknown *obj, int ui)
{
    if (!obj && ui < 0) {
        return;
    }
    int f = nw_dx.frame;
    if (nw_dx.nlater[f] == nw_dx.later_cap[f]) {
        int cap = nw_dx.later_cap[f] ? nw_dx.later_cap[f] * 2 : 64;
        NwDxLater *n = SDL_realloc(nw_dx.later[f], sizeof(*n) * (size_t)cap);
        if (!n) {
            return; /* leaked rather than released early */
        }
        nw_dx.later[f] = n;
        nw_dx.later_cap[f] = cap;
    }
    nw_dx.later[f][nw_dx.nlater[f]++] = (NwDxLater){ obj, ui };
}

static void nw_dx_release_later(int f)
{
    for (int i = 0; i < nw_dx.nlater[f]; i++) {
        nw_dx_release(nw_dx.later[f][i].obj);
        nw_dx_slot_put(&nw_dx.uis, nw_dx.later[f][i].ui);
    }
    nw_dx.nlater[f] = 0;
}

/* ---- the command list ---- */

static void nw_dx_barriers_flush(void)
{
    if (nw_dx.nbarriers) {
        ID3D12GraphicsCommandList_ResourceBarrier(nw_dx.list, (UINT)nw_dx.nbarriers, nw_dx.barriers);
        nw_dx.nbarriers = 0;
    }
}

static void nw_dx_barrier(ID3D12Resource *res, UINT sub, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
{
    if (before == after) {
        return;
    }
    if (nw_dx.nbarriers == NW_DX_BARRIERS) {
        nw_dx_barriers_flush();
    }
    D3D12_RESOURCE_BARRIER *b = &nw_dx.barriers[nw_dx.nbarriers++];
    SDL_zerop(b);
    b->Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b->Transition.pResource = res;
    b->Transition.Subresource = sub;
    b->Transition.StateBefore = before;
    b->Transition.StateAfter = after;
}

static void nw_dx_state(NwRhiTexture *t, D3D12_RESOURCE_STATES s)
{
    if (t && t->state != s) {
        nw_dx_barrier(t->res, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, t->state, s);
        t->state = s;
    }
}

static void nw_dx_back_state(D3D12_RESOURCE_STATES s)
{
    UINT i = nw_dx.back_index;
    if (nw_dx.back[i] && nw_dx.back_state[i] != s) {
        nw_dx_barrier(nw_dx.back[i], D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, nw_dx.back_state[i], s);
        nw_dx.back_state[i] = s;
    }
}

/* The command list starts over: nothing is bound. */
static void nw_dx_invalidate(void)
{
    nw_dx.bound_root = NULL;
    nw_dx.bound_pso = NULL;
    nw_dx.bound_cb[0] = nw_dx.bound_cb[1] = 0;
    nw_dx.bound_table = 0;
    nw_dx.bound_ref = -1;
    nw_dx.target_dirty = true;
    nw_dx.viewport_dirty = true;
}

static void nw_dx_begin(void)
{
    int f = nw_dx.frame;
    ID3D12CommandAllocator_Reset(nw_dx.alloc[f]);
    ID3D12GraphicsCommandList_Reset(nw_dx.list, nw_dx.alloc[f], NULL);
    nw_dx.ring_used = 0;
    nw_dx.table_used = 0;
    nw_dx.epoch++;
    nw_dx_invalidate();
}

static void nw_dx_wait(UINT64 value)
{
    if (ID3D12Fence_GetCompletedValue(nw_dx.fence) < value) {
        ID3D12Fence_SetEventOnCompletion(nw_dx.fence, value, nw_dx.event);
        WaitForSingleObject(nw_dx.event, 10000);
    }
}

static void nw_dx_lost(HRESULT hr, const char *what)
{
    if (!nw_dx.lost) {
        HRESULT why = ID3D12Device_GetDeviceRemovedReason(nw_dx.dev);
        SDL_Log("Direct3D 12: %s failed (0x%08lx), device removed reason 0x%08lx", what, (unsigned long)hr,
                (unsigned long)why);
        nw_dx.lost = FAILED(why);
    }
}

static void nw_dx_submit(void)
{
    nw_dx_barriers_flush();
    HRESULT hr = ID3D12GraphicsCommandList_Close(nw_dx.list);
    if (FAILED(hr)) {
        nw_dx_lost(hr, "Close");
        return;
    }
    ID3D12CommandList *lists[1] = { (ID3D12CommandList *)nw_dx.list };
    ID3D12CommandQueue_ExecuteCommandLists(nw_dx.queue, 1, lists);
}

/* Everything recorded runs and is waited for; the list starts over (texture_read, a full ring, a resize). */
static void nw_dx_flush(void)
{
    nw_dx_submit();
    ID3D12CommandQueue_Signal(nw_dx.queue, nw_dx.fence, ++nw_dx.fence_value);
    nw_dx_wait(nw_dx.fence_value);
    for (int f = 0; f < NW_DX_FRAMES; f++) {
        nw_dx_release_later(f);
    }
    nw_dx_begin();
}

/* size bytes of the frame's upload ring (or a buffer of their own when that big): its resource, offset and memory. */
static bool nw_dx_alloc(uint64_t size, uint64_t align, ID3D12Resource **res, uint64_t *off, uint8_t **cpu)
{
    if (size > NW_DX_RING / 2) {
        D3D12_HEAP_PROPERTIES hp = { D3D12_HEAP_TYPE_UPLOAD, D3D12_CPU_PAGE_PROPERTY_UNKNOWN, D3D12_MEMORY_POOL_UNKNOWN, 0, 0 };
        D3D12_RESOURCE_DESC rd = { D3D12_RESOURCE_DIMENSION_BUFFER, 0, size, 1, 1, 1, DXGI_FORMAT_UNKNOWN, { 1, 0 },
                                   D3D12_TEXTURE_LAYOUT_ROW_MAJOR, D3D12_RESOURCE_FLAG_NONE };
        ID3D12Resource *r = NULL;
        HRESULT hr = ID3D12Device_CreateCommittedResource(nw_dx.dev, &hp, D3D12_HEAP_FLAG_NONE, &rd,
                                                          D3D12_RESOURCE_STATE_GENERIC_READ, NULL, &IID_ID3D12Resource,
                                                          (void **)&r);
        void *map = NULL;
        D3D12_RANGE none = { 0, 0 };
        if (SUCCEEDED(hr)) {
            hr = ID3D12Resource_Map(r, 0, &none, &map);
        }
        if (FAILED(hr)) {
            nw_dx_release(r);
            return nw_dx_fail("an upload buffer", hr);
        }
        nw_dx_later((IUnknown *)r, -1);
        *res = r;
        *off = 0;
        *cpu = map;
        return true;
    }
    uint64_t o = nw_dx_align(nw_dx.ring_used, align);
    if (o + size > NW_DX_RING) {
        nw_dx_flush();
        o = 0;
    }
    nw_dx.ring_used = o + size;
    *res = nw_dx.ring[nw_dx.frame];
    *off = o;
    *cpu = nw_dx.ring_map[nw_dx.frame] + o;
    return true;
}

/* n descriptors of the frame's part of the shader-visible heap: the first's index. */
static int nw_dx_table_alloc(int n)
{
    if (nw_dx.table_used + n > NW_DX_TABLES) {
        nw_dx_flush();
    }
    int i = NW_DX_UI + nw_dx.frame * NW_DX_TABLES + nw_dx.table_used;
    nw_dx.table_used += n;
    return i;
}

static D3D12_CPU_DESCRIPTOR_HANDLE nw_dx_gpu_cpu(int i)
{
    D3D12_CPU_DESCRIPTOR_HANDLE h = { nw_dx.gpu_cpu0.ptr + (SIZE_T)i * nw_dx.inc_srv };
    return h;
}

static D3D12_GPU_DESCRIPTOR_HANDLE nw_dx_gpu(int i)
{
    D3D12_GPU_DESCRIPTOR_HANDLE h = { nw_dx.gpu0.ptr + (UINT64)i * nw_dx.inc_srv };
    return h;
}

/* ---- device and window ---- */

static SDL_WindowFlags nw_dx_prepare_window(void)
{
    return 0;
}

static bool nw_dx_back_buffers(void)
{
    for (int i = 0; i < NW_DX_BACK; i++) {
        HRESULT hr = IDXGISwapChain3_GetBuffer(nw_dx.swap, (UINT)i, &IID_ID3D12Resource, (void **)&nw_dx.back[i]);
        if (FAILED(hr)) {
            return nw_dx_fail("GetBuffer", hr);
        }
        if (nw_dx.back_rtv[i] < 0) {
            nw_dx.back_rtv[i] = nw_dx_slot_get(&nw_dx.rtvs);
        }
        ID3D12Device_CreateRenderTargetView(nw_dx.dev, nw_dx.back[i], NULL, nw_dx_rtv(nw_dx.back_rtv[i]));
        nw_dx.back_state[i] = D3D12_RESOURCE_STATE_PRESENT;
    }
    nw_dx.back_index = IDXGISwapChain3_GetCurrentBackBufferIndex(nw_dx.swap);
    return true;
}

/* The swap chain at the window's size (all GPU work done first: the old buffers go). */
static void nw_dx_fit_window(void)
{
    int w = 0, h = 0;
    SDL_GetWindowSizeInPixels(nw_dx.window, &w, &h);
    if (w <= 0 || h <= 0 || (w == nw_dx.back_w && h == nw_dx.back_h)) {
        return;
    }
    nw_dx_flush();
    for (int i = 0; i < NW_DX_BACK; i++) {
        nw_dx_release(nw_dx.back[i]);
        nw_dx.back[i] = NULL;
    }
    HRESULT hr = IDXGISwapChain3_ResizeBuffers(nw_dx.swap, 0, (UINT)w, (UINT)h, DXGI_FORMAT_UNKNOWN, 0);
    if (FAILED(hr)) {
        SDL_Log("Direct3D 12: ResizeBuffers %dx%d failed (0x%08lx)", w, h, (unsigned long)hr);
    } else {
        nw_dx.back_w = w;
        nw_dx.back_h = h;
    }
    nw_dx_back_buffers();
}

static void nw_dx_adapter_name(IDXGIAdapter1 *adapter)
{
    DXGI_ADAPTER_DESC1 ad;
    char *name = NULL;
    if (adapter && SUCCEEDED(IDXGIAdapter1_GetDesc1(adapter, &ad))) {
        name = SDL_iconv_string("UTF-8", "UTF-16LE", (const char *)ad.Description,
                                (SDL_wcslen((const wchar_t *)ad.Description) + 1) * sizeof(WCHAR));
    }
    SDL_snprintf(nw_dx.description, sizeof(nw_dx.description), "Direct3D 12 (feature level 11_0), %s%s",
                 name ? name : "?", nw_dx.warp ? " (WARP)" : "");
    SDL_free(name);
}

/* The GPU's device, else (no Direct3D 12 driver, or NW_D3D12_DRIVER=warp) Microsoft's WARP. */
static bool nw_dx_device(void)
{
    UINT flags = 0;
    const char *debug = SDL_getenv("NW_D3D12_DEBUG");
    if (debug && *debug == '1') {
        ID3D12Debug *dbg = NULL;
        if (SUCCEEDED(D3D12GetDebugInterface(&IID_ID3D12Debug, (void **)&dbg))) {
            ID3D12Debug_EnableDebugLayer(dbg);
            ID3D12Debug_Release(dbg);
            flags |= DXGI_CREATE_FACTORY_DEBUG;
        } else {
            SDL_Log("Direct3D 12: no debug layer (Graphics Tools not installed)");
        }
    }
    HRESULT hr = CreateDXGIFactory2(flags, &IID_IDXGIFactory4, (void **)&nw_dx.factory);
    if (FAILED(hr)) {
        return nw_dx_fail("CreateDXGIFactory2", hr);
    }
    const char *driver = SDL_getenv("NW_D3D12_DRIVER");
    bool warp = driver && SDL_strcasecmp(driver, "warp") == 0;
    IDXGIAdapter1 *adapter = NULL;
    if (!warp) {
        for (UINT i = 0; !nw_dx.dev && IDXGIFactory4_EnumAdapters1(nw_dx.factory, i, &adapter) != DXGI_ERROR_NOT_FOUND;
             i++) {
            DXGI_ADAPTER_DESC1 ad;
            if (SUCCEEDED(IDXGIAdapter1_GetDesc1(adapter, &ad)) && !(ad.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
                hr = D3D12CreateDevice((IUnknown *)adapter, D3D_FEATURE_LEVEL_11_0, &IID_ID3D12Device,
                                       (void **)&nw_dx.dev);
            }
            if (!nw_dx.dev) {
                IDXGIAdapter1_Release(adapter);
                adapter = NULL;
            }
        }
        if (!nw_dx.dev) {
            SDL_Log("Direct3D 12: no GPU with a Direct3D 12 driver (0x%08lx); WARP", (unsigned long)hr);
        }
    }
    if (!nw_dx.dev) {
        hr = IDXGIFactory4_EnumWarpAdapter(nw_dx.factory, &IID_IDXGIAdapter1, (void **)&adapter);
        if (SUCCEEDED(hr)) {
            hr = D3D12CreateDevice((IUnknown *)adapter, D3D_FEATURE_LEVEL_11_0, &IID_ID3D12Device,
                                   (void **)&nw_dx.dev);
        }
        if (FAILED(hr)) {
            nw_dx_release(adapter);
            return nw_dx_fail("D3D12CreateDevice (WARP)", hr);
        }
        nw_dx.warp = true;
    }
    nw_dx_adapter_name(adapter);
    nw_dx_release(adapter);
    return true;
}

static bool nw_dx_swap_chain(void)
{
    int w = 0, h = 0;
    SDL_GetWindowSizeInPixels(nw_dx.window, &w, &h);
    DXGI_SWAP_CHAIN_DESC1 sd = { 0 };
    sd.Width = (UINT)SDL_max(w, 1);
    sd.Height = (UINT)SDL_max(h, 1);
    sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = NW_DX_BACK;
    sd.Scaling = DXGI_SCALING_STRETCH;
    sd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    IDXGISwapChain1 *swap = NULL;
    SDL_PropertiesID props = SDL_GetWindowProperties(nw_dx.window);
    HRESULT hr;
#ifdef NW_UWP
    /* SDL3-uwp's CoreWindow (SDL_PROP_WINDOW_WINRT_WINDOW_POINTER, an IInspectable). */
    IUnknown *core = SDL_GetPointerProperty(props, "SDL.window.winrt.window", NULL);
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    hr = core ? IDXGIFactory4_CreateSwapChainForCoreWindow(nw_dx.factory, (IUnknown *)nw_dx.queue, core, &sd, NULL,
                                                           &swap)
              : E_POINTER;
#else
    HWND hwnd = SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WIN32_HWND_POINTER, NULL);
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    hr = hwnd ? IDXGIFactory4_CreateSwapChainForHwnd(nw_dx.factory, (IUnknown *)nw_dx.queue, hwnd, &sd, NULL, NULL,
                                                     &swap)
              : E_POINTER;
    if (SUCCEEDED(hr)) {
        IDXGIFactory4_MakeWindowAssociation(nw_dx.factory, hwnd, DXGI_MWA_NO_ALT_ENTER);
    }
#endif
    if (SUCCEEDED(hr)) {
        hr = IDXGISwapChain1_QueryInterface(swap, &IID_IDXGISwapChain3, (void **)&nw_dx.swap);
    }
    nw_dx_release(swap);
    if (FAILED(hr)) {
        return nw_dx_fail("the swap chain", hr);
    }
    nw_dx.back_w = (int)sd.Width;
    nw_dx.back_h = (int)sd.Height;
    return nw_dx_back_buffers();
}

static bool nw_dx_heap(D3D12_DESCRIPTOR_HEAP_TYPE type, UINT n, bool visible, ID3D12DescriptorHeap **out)
{
    D3D12_DESCRIPTOR_HEAP_DESC hd = { type, n,
                                      visible ? D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE : D3D12_DESCRIPTOR_HEAP_FLAG_NONE,
                                      0 };
    HRESULT hr = ID3D12Device_CreateDescriptorHeap(nw_dx.dev, &hd, &IID_ID3D12DescriptorHeap, (void **)out);
    return SUCCEEDED(hr) || nw_dx_fail("CreateDescriptorHeap", hr);
}

static ID3D12RootSignature *nw_dx_root_signature(const D3D12_ROOT_SIGNATURE_DESC *rs)
{
    ID3DBlob *blob = NULL, *err = NULL;
    ID3D12RootSignature *root = NULL;
    HRESULT hr = D3D12SerializeRootSignature(rs, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err);
    if (SUCCEEDED(hr)) {
        hr = ID3D12Device_CreateRootSignature(nw_dx.dev, 0, ID3D10Blob_GetBufferPointer(blob),
                                              ID3D10Blob_GetBufferSize(blob), &IID_ID3D12RootSignature,
                                              (void **)&root);
    } else if (err) {
        SDL_Log("Direct3D 12: root signature: %s", (const char *)ID3D10Blob_GetBufferPointer(err));
    }
    nw_dx_release(blob);
    nw_dx_release(err);
    if (FAILED(hr)) {
        nw_dx_fail("a root signature", hr);
    }
    return root;
}

static D3D12_STATIC_SAMPLER_DESC nw_dx_sampler(UINT reg, D3D12_FILTER filter)
{
    D3D12_STATIC_SAMPLER_DESC s = { 0 };
    s.Filter = filter;
    s.AddressU = s.AddressV = s.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    s.MaxAnisotropy = 1;
    s.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
    s.BorderColor = D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK;
    s.MaxLOD = D3D12_FLOAT32_MAX;
    s.ShaderRegister = reg;
    s.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    return s;
}

/* The programs': b0 of each stage, t0-t8, s0-s8 (point, clamp). The util shaders': b0, t0-t2, s0 (linear). */
static bool nw_dx_root_signatures(void)
{
    D3D12_DESCRIPTOR_RANGE range = { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, NW_DX_UNITS, 0, 0, 0 };
    D3D12_ROOT_PARAMETER params[3];
    SDL_zeroa(params);
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[2].DescriptorTable.NumDescriptorRanges = 1;
    params[2].DescriptorTable.pDescriptorRanges = &range;
    params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_STATIC_SAMPLER_DESC samplers[NW_DX_UNITS];
    for (int i = 0; i < NW_DX_UNITS; i++) {
        samplers[i] = nw_dx_sampler((UINT)i, D3D12_FILTER_MIN_MAG_MIP_POINT);
    }
    D3D12_ROOT_SIGNATURE_DESC rs = { 3, params, NW_DX_UNITS, samplers,
                                     D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT };
    nw_dx.root = nw_dx_root_signature(&rs);
    D3D12_DESCRIPTOR_RANGE urange = { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 3, 0, 0, 0 };
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    params[1] = params[2];
    params[1].DescriptorTable.pDescriptorRanges = &urange;
    D3D12_STATIC_SAMPLER_DESC linear = nw_dx_sampler(0, D3D12_FILTER_MIN_MAG_MIP_LINEAR);
    D3D12_ROOT_SIGNATURE_DESC urs = { 2, params, 1, &linear, D3D12_ROOT_SIGNATURE_FLAG_NONE };
    nw_dx.util_root = nw_dx_root_signature(&urs);
    return nw_dx.root && nw_dx.util_root;
}

static void nw_dx_null_srv(int slot, D3D12_SRV_DIMENSION dim, DXGI_FORMAT fmt)
{
    D3D12_SHADER_RESOURCE_VIEW_DESC sv;
    SDL_zero(sv);
    sv.Format = fmt;
    sv.ViewDimension = dim;
    sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    if (dim == D3D12_SRV_DIMENSION_TEXTURE2D) {
        sv.Texture2D.MipLevels = 1;
    } else if (dim == D3D12_SRV_DIMENSION_BUFFER) {
        sv.Buffer.NumElements = 1;
    }
    ID3D12Device_CreateShaderResourceView(nw_dx.dev, NULL, &sv, nw_dx_cpu(slot));
}

static bool nw_dx_init(SDL_Window *window)
{
    SDL_zero(nw_dx);
    nw_dx.window = window;
    nw_dx.interval = 1;
    for (int i = 0; i < NW_DX_BACK; i++) {
        nw_dx.back_rtv[i] = -1;
    }
    if (!nw_dx_device()) {
        return false;
    }
    D3D12_COMMAND_QUEUE_DESC qd = { D3D12_COMMAND_LIST_TYPE_DIRECT, 0, D3D12_COMMAND_QUEUE_FLAG_NONE, 0 };
    HRESULT hr = ID3D12Device_CreateCommandQueue(nw_dx.dev, &qd, &IID_ID3D12CommandQueue, (void **)&nw_dx.queue);
    for (int f = 0; SUCCEEDED(hr) && f < NW_DX_FRAMES; f++) {
        hr = ID3D12Device_CreateCommandAllocator(nw_dx.dev, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                 &IID_ID3D12CommandAllocator, (void **)&nw_dx.alloc[f]);
    }
    if (SUCCEEDED(hr)) {
        hr = ID3D12Device_CreateCommandList(nw_dx.dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, nw_dx.alloc[0], NULL,
                                            &IID_ID3D12GraphicsCommandList, (void **)&nw_dx.list);
    }
    if (SUCCEEDED(hr)) {
        hr = ID3D12Device_CreateFence(nw_dx.dev, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence, (void **)&nw_dx.fence);
    }
    if (FAILED(hr)) {
        return nw_dx_fail("the command queue", hr);
    }
    nw_dx.event = CreateEventExW(NULL, NULL, 0, EVENT_ALL_ACCESS);
    if (!nw_dx_slots_init(&nw_dx.srvs, NW_DX_SRVS) || !nw_dx_slots_init(&nw_dx.rtvs, NW_DX_RTVS) ||
        !nw_dx_slots_init(&nw_dx.dsvs, NW_DX_DSVS) || !nw_dx_slots_init(&nw_dx.uis, NW_DX_UI)) {
        return SDL_OutOfMemory();
    }
    if (!nw_dx_heap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, NW_DX_UI + NW_DX_FRAMES * NW_DX_TABLES, true,
                    &nw_dx.gpu_heap) ||
        !nw_dx_heap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, NW_DX_SRVS, false, &nw_dx.cpu_heap) ||
        !nw_dx_heap(D3D12_DESCRIPTOR_HEAP_TYPE_RTV, NW_DX_RTVS, false, &nw_dx.rtv_heap) ||
        !nw_dx_heap(D3D12_DESCRIPTOR_HEAP_TYPE_DSV, NW_DX_DSVS, false, &nw_dx.dsv_heap)) {
        return false;
    }
    nw_dx.gpu_cpu0 = ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(nw_dx.gpu_heap);
    nw_dx.gpu0 = ID3D12DescriptorHeap_GetGPUDescriptorHandleForHeapStart(nw_dx.gpu_heap);
    nw_dx.cpu0 = ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(nw_dx.cpu_heap);
    nw_dx.rtv0 = ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(nw_dx.rtv_heap);
    nw_dx.dsv0 = ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(nw_dx.dsv_heap);
    nw_dx.inc_srv = ID3D12Device_GetDescriptorHandleIncrementSize(nw_dx.dev, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    nw_dx.inc_rtv = ID3D12Device_GetDescriptorHandleIncrementSize(nw_dx.dev, D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    nw_dx.inc_dsv = ID3D12Device_GetDescriptorHandleIncrementSize(nw_dx.dev, D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
    nw_dx.null_tex = nw_dx_slot_get(&nw_dx.srvs);
    nw_dx.null_buf = nw_dx_slot_get(&nw_dx.srvs);
    nw_dx.null_ms = nw_dx_slot_get(&nw_dx.srvs);
    nw_dx.scratch_srv = nw_dx_slot_get(&nw_dx.srvs);
    nw_dx.scratch_rtv = nw_dx_slot_get(&nw_dx.rtvs);
    nw_dx_null_srv(nw_dx.null_tex, D3D12_SRV_DIMENSION_TEXTURE2D, DXGI_FORMAT_R8G8B8A8_UNORM);
    nw_dx_null_srv(nw_dx.null_buf, D3D12_SRV_DIMENSION_BUFFER, DXGI_FORMAT_R32G32B32A32_FLOAT);
    nw_dx_null_srv(nw_dx.null_ms, D3D12_SRV_DIMENSION_TEXTURE2DMS, DXGI_FORMAT_R32_FLOAT);
    if (!nw_dx_swap_chain() || !nw_dx_root_signatures()) {
        return false;
    }
    /* the upload rings and the dummy vertex */
    D3D12_HEAP_PROPERTIES hp = { D3D12_HEAP_TYPE_UPLOAD, D3D12_CPU_PAGE_PROPERTY_UNKNOWN, D3D12_MEMORY_POOL_UNKNOWN, 0, 0 };
    D3D12_RESOURCE_DESC rd = { D3D12_RESOURCE_DIMENSION_BUFFER, 0, NW_DX_RING, 1, 1, 1, DXGI_FORMAT_UNKNOWN, { 1, 0 },
                               D3D12_TEXTURE_LAYOUT_ROW_MAJOR, D3D12_RESOURCE_FLAG_NONE };
    D3D12_RANGE none = { 0, 0 };
    for (int f = 0; f < NW_DX_FRAMES; f++) {
        hr = ID3D12Device_CreateCommittedResource(nw_dx.dev, &hp, D3D12_HEAP_FLAG_NONE, &rd,
                                                  D3D12_RESOURCE_STATE_GENERIC_READ, NULL, &IID_ID3D12Resource,
                                                  (void **)&nw_dx.ring[f]);
        if (SUCCEEDED(hr)) {
            hr = ID3D12Resource_Map(nw_dx.ring[f], 0, &none, (void **)&nw_dx.ring_map[f]);
        }
        if (FAILED(hr)) {
            return nw_dx_fail("the upload ring", hr);
        }
        nw_dx.ring_va[f] = ID3D12Resource_GetGPUVirtualAddress(nw_dx.ring[f]);
    }
    static const uint32_t one = 0x3F800000u;
    const uint32_t dummy[12] = { 0, 0, 0, one, 0, 0, 0, 1, 0, 0, 0, 1 };
    rd.Width = 256;
    void *map = NULL;
    hr = ID3D12Device_CreateCommittedResource(nw_dx.dev, &hp, D3D12_HEAP_FLAG_NONE, &rd,
                                              D3D12_RESOURCE_STATE_GENERIC_READ, NULL, &IID_ID3D12Resource,
                                              (void **)&nw_dx.dummy);
    if (SUCCEEDED(hr) && SUCCEEDED(ID3D12Resource_Map(nw_dx.dummy, 0, &none, &map))) {
        SDL_memcpy(map, dummy, sizeof(dummy));
        ID3D12Resource_Unmap(nw_dx.dummy, 0, NULL);
    }
    nw_dx.psos = SDL_calloc(NW_DX_PSOS, sizeof(*nw_dx.psos));
    if (!nw_dx.psos) {
        return SDL_OutOfMemory();
    }
    nw_dx.caps.max_texture = D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION;
    for (UINT n = 2; n <= 8; n *= 2) {
        D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS a = { DXGI_FORMAT_R8G8B8A8_UNORM, n, 0, 0 };
        D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS b = { DXGI_FORMAT_D24_UNORM_S8_UINT, n, 0, 0 };
        ID3D12Device_CheckFeatureSupport(nw_dx.dev, D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS, &a, sizeof(a));
        ID3D12Device_CheckFeatureSupport(nw_dx.dev, D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS, &b, sizeof(b));
        if (a.NumQualityLevels && b.NumQualityLevels) {
            nw_dx.caps.max_samples = (int)n;
        }
    }
    nw_dx.mask = 0xF;
    nw_dx.depth_write = true;
    nw_dx.depth_func = NW_RHI_LESS;
    nw_dx.stencil_read = nw_dx.stencil_write = 0xFF;
    nw_dx.epoch = 1;
    nw_dx_invalidate();
    SDL_Log("%s", nw_dx.description);
    return true;
}

static void nw_dx_shutdown(void)
{
    if (nw_dx.queue && nw_dx.fence && nw_dx.list) {
        nw_dx_submit();
        ID3D12CommandQueue_Signal(nw_dx.queue, nw_dx.fence, ++nw_dx.fence_value);
        nw_dx_wait(nw_dx.fence_value);
    }
    for (int f = 0; f < NW_DX_FRAMES; f++) {
        nw_dx_release_later(f);
        SDL_free(nw_dx.later[f]);
        nw_dx_release(nw_dx.ring[f]);
        nw_dx_release(nw_dx.alloc[f]);
    }
    for (int i = 0; nw_dx.psos && i < NW_DX_PSOS; i++) {
        nw_dx_release(nw_dx.psos[i].pso);
    }
    SDL_free(nw_dx.psos);
    for (int i = 0; i < NW_DX_BACK; i++) {
        nw_dx_release(nw_dx.back[i]);
    }
    nw_dx_release(nw_dx.dummy);
    nw_dx_release(nw_dx.root);
    nw_dx_release(nw_dx.util_root);
    nw_dx_release(nw_dx.gpu_heap);
    nw_dx_release(nw_dx.cpu_heap);
    nw_dx_release(nw_dx.rtv_heap);
    nw_dx_release(nw_dx.dsv_heap);
    nw_dx_release(nw_dx.swap);
    nw_dx_release(nw_dx.list);
    nw_dx_release(nw_dx.fence);
    nw_dx_release(nw_dx.queue);
    nw_dx_release(nw_dx.dev);
    nw_dx_release(nw_dx.factory);
    if (nw_dx.event) {
        CloseHandle(nw_dx.event);
    }
    SDL_free(nw_dx.srvs.free);
    SDL_free(nw_dx.rtvs.free);
    SDL_free(nw_dx.dsvs.free);
    SDL_free(nw_dx.uis.free);
    SDL_zero(nw_dx);
}

/* The window as the target, its buffer ready to be drawn into (Dear ImGui records after this). */
static void nw_dx_prepare_target(void);

static void *nw_dx_native(int which)
{
    switch (which) {
    case 0:
        return nw_dx.dev;
    case 1:
        return nw_dx.queue;
    case 2:
        nw_dx_prepare_target();
        nw_dx_barriers_flush();
        nw_dx_invalidate(); /* Dear ImGui sets its own root signature, pipeline and heaps */
        return nw_dx.list;
    case 3:
        return nw_dx.gpu_heap;
    default:
        return NULL;
    }
}

bool nw_rhi_d3d12_ui_alloc(uint64_t *cpu, uint64_t *gpu)
{
    int slot = nw_dx_slot_get(&nw_dx.uis);
    if (slot < 0) {
        return false;
    }
    *cpu = nw_dx_gpu_cpu(slot).ptr;
    *gpu = nw_dx_gpu(slot).ptr;
    return true;
}

void nw_rhi_d3d12_ui_free(uint64_t gpu)
{
    if (gpu >= nw_dx.gpu0.ptr) {
        nw_dx_later(NULL, (int)((gpu - nw_dx.gpu0.ptr) / nw_dx.inc_srv));
    }
}

void nw_rhi_d3d12_idle(void)
{
    if (nw_dx.list) {
        nw_dx_flush();
    }
}

static const char *nw_dx_description(void)
{
    return nw_dx.description;
}

static const NwRhiCaps *nw_dx_caps(void)
{
    return &nw_dx.caps;
}

static bool nw_dx_set_vsync(int interval)
{
    if (interval < 0) {
        return SDL_SetError("Direct3D 12: no adaptive vsync");
    }
    nw_dx.interval = interval;
    return true;
}

static int nw_dx_vsync(void)
{
    return nw_dx.interval;
}

static void nw_dx_present(void)
{
    nw_dx_back_state(D3D12_RESOURCE_STATE_PRESENT);
    nw_dx_submit();
    HRESULT hr = IDXGISwapChain3_Present(nw_dx.swap, nw_dx.interval ? 1 : 0, 0);
    if (FAILED(hr)) {
        nw_dx_lost(hr, "Present");
    }
    ID3D12CommandQueue_Signal(nw_dx.queue, nw_dx.fence, ++nw_dx.fence_value);
    nw_dx.frame_fence[nw_dx.frame] = nw_dx.fence_value;
    nw_dx.frame = (nw_dx.frame + 1) % NW_DX_FRAMES;
    nw_dx_wait(nw_dx.frame_fence[nw_dx.frame]);
    nw_dx_release_later(nw_dx.frame);
    nw_dx_begin();
    nw_dx.back_index = IDXGISwapChain3_GetCurrentBackBufferIndex(nw_dx.swap);
}

/* ---- buffers ---- */

static NwRhiBuffer *nw_dx_buffer_create(NwRhiBufferKind kind, NwRhiFormat texels)
{
    NwRhiBuffer *b = SDL_calloc(1, sizeof(*b));
    if (b) {
        b->kind = kind;
        b->texels = texels;
    }
    return b;
}

/* The buffer's data where the next draw reads it; false when it has none. */
static bool nw_dx_buffer_ready(NwRhiBuffer *b)
{
    if (!b || !b->size) {
        return false;
    }
    if (!b->stream) {
        return b->own != NULL;
    }
    if (b->res && b->epoch == nw_dx.epoch) {
        return true;
    }
    uint8_t *cpu = NULL;
    if (!nw_dx_alloc(b->size, 256, &b->res, &b->off, &cpu)) {
        b->res = NULL;
        return false;
    }
    SDL_memcpy(cpu, b->shadow, b->size);
    b->epoch = nw_dx.epoch;
    b->serial = ++nw_dx.serial;
    return true;
}

static void nw_dx_buffer_data(NwRhiBuffer *b, size_t size, const void *data, bool stream)
{
    if (!size) {
        return;
    }
    if (stream) {
        if (size > b->shadow_cap) {
            size_t cap = SDL_max(size + size / 2, (size_t)4096);
            uint8_t *n = SDL_realloc(b->shadow, cap);
            if (!n) {
                return;
            }
            b->shadow = n;
            b->shadow_cap = cap;
        }
        SDL_memcpy(b->shadow, data, size);
        b->stream = true;
        b->size = size;
        b->res = NULL; /* placed at the next draw that takes it */
        return;
    }
    /* A new buffer of its own: draws recorded before keep the old one until the GPU is done with them. */
    D3D12_HEAP_PROPERTIES hp = { D3D12_HEAP_TYPE_DEFAULT, D3D12_CPU_PAGE_PROPERTY_UNKNOWN, D3D12_MEMORY_POOL_UNKNOWN, 0, 0 };
    D3D12_RESOURCE_DESC rd = { D3D12_RESOURCE_DIMENSION_BUFFER, 0, nw_dx_align(size, 256), 1, 1, 1, DXGI_FORMAT_UNKNOWN,
                               { 1, 0 }, D3D12_TEXTURE_LAYOUT_ROW_MAJOR, D3D12_RESOURCE_FLAG_NONE };
    ID3D12Resource *own = NULL;
    HRESULT hr = ID3D12Device_CreateCommittedResource(nw_dx.dev, &hp, D3D12_HEAP_FLAG_NONE, &rd,
                                                      D3D12_RESOURCE_STATE_COMMON, NULL, &IID_ID3D12Resource,
                                                      (void **)&own);
    if (FAILED(hr)) {
        nw_dx_fail("CreateCommittedResource (buffer)", hr);
        return;
    }
    ID3D12Resource *src = NULL;
    uint64_t off = 0;
    uint8_t *cpu = NULL;
    if (!nw_dx_alloc(size, 256, &src, &off, &cpu)) {
        ID3D12Resource_Release(own);
        return;
    }
    SDL_memcpy(cpu, data, size);
    nw_dx_barriers_flush();
    ID3D12GraphicsCommandList_CopyBufferRegion(nw_dx.list, own, 0, src, off, size);
    nw_dx_barrier(own, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_COPY_DEST, NW_DX_READ);
    nw_dx_later((IUnknown *)b->own, -1);
    b->own = b->res = own;
    b->off = 0;
    b->stream = false;
    b->size = size;
    b->serial = ++nw_dx.serial;
}

static void nw_dx_buffer_destroy(NwRhiBuffer *b)
{
    if (!b) {
        return;
    }
    for (int i = 0; i < NW_DX_UNITS; i++) {
        if (nw_dx.buf[i] == b) {
            nw_dx.buf[i] = NULL;
        }
    }
    nw_dx_later((IUnknown *)b->own, -1);
    SDL_free(b->shadow);
    SDL_free(b);
}

/* ---- textures ---- */

static int nw_dx_bpp(NwRhiFormat f)
{
    return f == NW_RHI_R8UI ? 1 : f == NW_RHI_RGBA32F ? 16 : 4;
}

/* Rows of a level's rectangle (tightly packed) through the ring into the texture (in COPY_DEST). */
static void nw_dx_upload(NwRhiTexture *t, int level, int x, int y, int width, int height, const void *data)
{
    if (width <= 0 || height <= 0 || !data) {
        return;
    }
    size_t row = (size_t)width * (size_t)nw_dx_bpp(t->format);
    uint64_t pitch = nw_dx_align(row, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT);
    ID3D12Resource *src = NULL;
    uint64_t off = 0;
    uint8_t *cpu = NULL;
    if (!nw_dx_alloc(pitch * (uint64_t)height, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT, &src, &off, &cpu)) {
        return;
    }
    for (int i = 0; i < height; i++) {
        SDL_memcpy(cpu + (size_t)i * pitch, (const uint8_t *)data + (size_t)i * row, row);
    }
    nw_dx_state(t, D3D12_RESOURCE_STATE_COPY_DEST);
    nw_dx_barriers_flush();
    D3D12_TEXTURE_COPY_LOCATION dst, from;
    SDL_zero(dst);
    SDL_zero(from);
    dst.pResource = t->res;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.SubresourceIndex = (UINT)level;
    from.pResource = src;
    from.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    from.PlacedFootprint.Offset = off;
    from.PlacedFootprint.Footprint.Format = t->fmt;
    from.PlacedFootprint.Footprint.Width = (UINT)width;
    from.PlacedFootprint.Footprint.Height = (UINT)height;
    from.PlacedFootprint.Footprint.Depth = 1;
    from.PlacedFootprint.Footprint.RowPitch = (UINT)pitch;
    ID3D12GraphicsCommandList_CopyTextureRegion(nw_dx.list, &dst, (UINT)x, (UINT)y, 0, &from, NULL);
}

static void nw_dx_texture_destroy(NwRhiTexture *t);

static NwRhiTexture *nw_dx_texture_create(NwRhiFormat format, int width, int height, int levels, int samples,
                                          unsigned flags, const void *const *data)
{
    NwRhiTexture *t = SDL_calloc(1, sizeof(*t));
    if (!t) {
        return NULL;
    }
    bool ms = samples > 1;
    *t = (NwRhiTexture){ .format = format, .w = width, .h = height, .levels = levels, .samples = ms ? samples : 1,
                         .flags = flags, .srv = -1, .rtv = -1, .dsv = -1, .ui = -1, .serial = ++nw_dx.serial };
    D3D12_RESOURCE_DESC rd;
    SDL_zero(rd);
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = (UINT64)width;
    rd.Height = (UINT)height;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = (UINT16)levels;
    rd.SampleDesc.Count = (UINT)t->samples;
    rd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    DXGI_FORMAT srv = DXGI_FORMAT_UNKNOWN;
    D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COPY_DEST;
    switch (format) {
    case NW_RHI_RGBA8:
        rd.Format = srv = DXGI_FORMAT_R8G8B8A8_UNORM;
        if (flags & (NW_RHI_TEX_TARGET | NW_RHI_TEX_MIPGEN)) {
            rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
            state = D3D12_RESOURCE_STATE_RENDER_TARGET;
        }
        if ((flags & NW_RHI_TEX_MIPGEN) && !ms) {
            int n = 1;
            while ((width >> n) || (height >> n)) {
                n++;
            }
            rd.MipLevels = (UINT16)n;
            t->levels = n;
        }
        break;
    case NW_RHI_D24S8:
        rd.Format = DXGI_FORMAT_R24G8_TYPELESS;
        srv = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
        rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
        state = D3D12_RESOURCE_STATE_DEPTH_WRITE;
        break;
    case NW_RHI_RGBA8UI:
        rd.Format = srv = DXGI_FORMAT_R8G8B8A8_UINT;
        break;
    case NW_RHI_R8UI:
        rd.Format = srv = DXGI_FORMAT_R8_UINT;
        break;
    default:
        rd.Format = srv = DXGI_FORMAT_R32G32B32A32_FLOAT;
        break;
    }
    t->fmt = rd.Format;
    t->state = state;
    D3D12_HEAP_PROPERTIES hp = { D3D12_HEAP_TYPE_DEFAULT, D3D12_CPU_PAGE_PROPERTY_UNKNOWN, D3D12_MEMORY_POOL_UNKNOWN, 0, 0 };
    HRESULT hr = ID3D12Device_CreateCommittedResource(nw_dx.dev, &hp, D3D12_HEAP_FLAG_NONE, &rd, state, NULL,
                                                      &IID_ID3D12Resource, (void **)&t->res);
    if (FAILED(hr)) {
        nw_dx_fail("a texture", hr);
        SDL_Log("%s (%dx%d format %d samples %d)", SDL_GetError(), width, height, (int)format, samples);
        SDL_free(t);
        return NULL;
    }
    t->srv = nw_dx_slot_get(&nw_dx.srvs);
    if (t->srv >= 0) {
        D3D12_SHADER_RESOURCE_VIEW_DESC sv;
        SDL_zero(sv);
        sv.Format = srv;
        sv.ViewDimension = ms ? D3D12_SRV_DIMENSION_TEXTURE2DMS : D3D12_SRV_DIMENSION_TEXTURE2D;
        sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sv.Texture2D.MipLevels = (UINT)-1;
        ID3D12Device_CreateShaderResourceView(nw_dx.dev, t->res, &sv, nw_dx_cpu(t->srv));
    }
    if (format == NW_RHI_D24S8) {
        t->dsv = nw_dx_slot_get(&nw_dx.dsvs);
        D3D12_DEPTH_STENCIL_VIEW_DESC dv;
        SDL_zero(dv);
        dv.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
        dv.ViewDimension = ms ? D3D12_DSV_DIMENSION_TEXTURE2DMS : D3D12_DSV_DIMENSION_TEXTURE2D;
        if (t->dsv >= 0) {
            ID3D12Device_CreateDepthStencilView(nw_dx.dev, t->res, &dv, nw_dx_dsv(t->dsv));
        }
    } else if (rd.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) {
        t->rtv = nw_dx_slot_get(&nw_dx.rtvs);
        D3D12_RENDER_TARGET_VIEW_DESC rv;
        SDL_zero(rv);
        rv.Format = rd.Format;
        rv.ViewDimension = ms ? D3D12_RTV_DIMENSION_TEXTURE2DMS : D3D12_RTV_DIMENSION_TEXTURE2D;
        if (t->rtv >= 0) {
            ID3D12Device_CreateRenderTargetView(nw_dx.dev, t->res, &rv, nw_dx_rtv(t->rtv));
        }
    }
    if (t->srv < 0 || (format == NW_RHI_D24S8 && t->dsv < 0) ||
        ((rd.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) && t->rtv < 0)) {
        SDL_SetError("Direct3D 12: out of descriptors");
        nw_dx_texture_destroy(t);
        return NULL;
    }
    if (data) {
        int n = (flags & NW_RHI_TEX_MIPGEN) ? 1 : levels;
        for (int i = 0; i < n; i++) {
            nw_dx_upload(t, i, 0, 0, SDL_max(width >> i, 1), SDL_max(height >> i, 1), data[i]);
        }
        nw_dx_state(t, (flags & NW_RHI_TEX_MIPGEN) ? D3D12_RESOURCE_STATE_RENDER_TARGET : NW_DX_SRV);
    }
    return t;
}

static void nw_dx_texture_update(NwRhiTexture *t, int level, int x, int y, int width, int height, const void *data)
{
    nw_dx_upload(t, level, x, y, width, height, data);
}

static void nw_dx_texture_destroy(NwRhiTexture *t)
{
    if (!t) {
        return;
    }
    for (int i = 0; i < NW_DX_UNITS; i++) {
        if (nw_dx.tex[i] == t) {
            nw_dx.tex[i] = NULL;
        }
    }
    if (nw_dx.col == t || nw_dx.ds == t) {
        nw_dx.col = nw_dx.col == t ? NULL : nw_dx.col;
        nw_dx.ds = nw_dx.ds == t ? NULL : nw_dx.ds;
        nw_dx.target_dirty = true;
    }
    nw_dx_slot_put(&nw_dx.srvs, t->srv);
    nw_dx_slot_put(&nw_dx.rtvs, t->rtv);
    nw_dx_slot_put(&nw_dx.dsvs, t->dsv);
    nw_dx_later((IUnknown *)t->res, t->ui);
    SDL_free(t);
}

static void nw_dx_texture_size(const NwRhiTexture *t, int *width, int *height)
{
    *width = t ? t->w : 0;
    *height = t ? t->h : 0;
}

static uint64_t nw_dx_texture_ui(NwRhiTexture *t)
{
    if (!t) {
        return 0;
    }
    if (t->ui < 0) {
        t->ui = nw_dx_slot_get(&nw_dx.uis);
        if (t->ui < 0) {
            return 0;
        }
        ID3D12Device_CopyDescriptorsSimple(nw_dx.dev, 1, nw_dx_gpu_cpu(t->ui), nw_dx_cpu(t->srv),
                                           D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    }
    nw_dx_state(t, NW_DX_SRV); /* Dear ImGui samples it later in this frame */
    return nw_dx_gpu(t->ui).ptr;
}

/* ---- programs ---- */

static void nw_dx_program_destroy(NwRhiProgram *p);

static NwRhiProgram *nw_dx_program_create(const NwRhiProgramDesc *d)
{
    const NwD3dProgram *src = NULL;
    for (int i = 0; i < nw_d3d11_nsets && !src; i++) {
        if (SDL_strcmp(nw_d3d11_sets[i].name, d->set) == 0 && d->variant >= 0 && d->variant < nw_d3d11_sets[i].count) {
            src = &nw_d3d11_sets[i].programs[d->variant];
        }
    }
    if (!src) {
        SDL_SetError("Direct3D 12: no bytecode for %s %d (tools/gen_shaders.py)", d->set, d->variant);
        return NULL;
    }
    NwRhiProgram *p = SDL_calloc(1, sizeof(*p));
    if (!p) {
        return NULL;
    }
    p->src = src;
    for (int s = 0; s < 2; s++) {
        if (src->cbuffer[s]) {
            p->data[s] = SDL_calloc(1, src->cbuffer[s]);
            p->dirty[s] = true;
            if (!p->data[s]) {
                nw_dx_program_destroy(p);
                return NULL;
            }
        }
    }
    return p;
}

static void nw_dx_program_destroy(NwRhiProgram *p)
{
    if (!p) {
        return;
    }
    if (nw_dx.prog == p) {
        nw_dx.prog = NULL;
    }
    /* its pipeline states go, the table is built again from the others */
    int keep = 0;
    NwDxPso *old = NULL;
    if (nw_dx.psos && nw_dx.npsos) {
        old = SDL_malloc(sizeof(*old) * (size_t)nw_dx.npsos);
    }
    for (int i = 0; old && i < NW_DX_PSOS; i++) {
        if (!nw_dx.psos[i].pso) {
            continue;
        }
        if (nw_dx.psos[i].key.prog == p) {
            nw_dx_later((IUnknown *)nw_dx.psos[i].pso, -1);
        } else {
            old[keep++] = nw_dx.psos[i];
        }
        nw_dx.psos[i].pso = NULL;
    }
    if (old) {
        nw_dx.npsos = 0;
        for (int i = 0; i < keep; i++) {
            uint32_t h = 2166136261u;
            const uint8_t *k = (const uint8_t *)&old[i].key;
            for (size_t j = 0; j < sizeof(old[i].key); j++) {
                h = (h ^ k[j]) * 16777619u;
            }
            for (uint32_t j = h & (NW_DX_PSOS - 1);; j = (j + 1) & (NW_DX_PSOS - 1)) {
                if (!nw_dx.psos[j].pso) {
                    nw_dx.psos[j] = old[i];
                    nw_dx.npsos++;
                    break;
                }
            }
        }
        SDL_free(old);
    }
    nw_dx.bound_pso = NULL;
    for (int s = 0; s < 2; s++) {
        SDL_free(p->data[s]);
    }
    SDL_free(p);
}

static int nw_dx_uniform(NwRhiProgram *p, const char *name)
{
    for (uint32_t i = 0; i < p->src->nuniforms; i++) {
        if (SDL_strcmp(p->src->uniforms[i].name, name) == 0) {
            return (int)i;
        }
    }
    return -1;
}

/* GL's std140 default block: arrays and matrix columns 16 bytes apart (as rhi_d3d11.c). */
static void nw_dx_set_uniform(NwRhiProgram *p, int u, NwRhiUniformType type, int components, int count,
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

static void nw_dx_use_program(NwRhiProgram *p)
{
    nw_dx.prog = p;
}

/* ---- vertex arrays ---- */

static NwRhiVertexArray *nw_dx_vertex_array_create(void)
{
    NwRhiVertexArray *va = SDL_calloc(1, sizeof(*va));
    if (va) {
        va->layout = -1;
    }
    return va;
}

static int nw_dx_layout_id(const NwRhiVertexLayout *l)
{
    for (int i = 0; i < nw_dx.nlayouts; i++) {
        if (SDL_memcmp(&nw_dx.layouts[i], l, sizeof(*l)) == 0) {
            return i;
        }
    }
    if (nw_dx.nlayouts == NW_DX_LAYOUTS) {
        SDL_Log("Direct3D 12: more than %d vertex layouts", NW_DX_LAYOUTS);
        return -1;
    }
    SDL_memcpy(&nw_dx.layouts[nw_dx.nlayouts], l, sizeof(*l));
    return nw_dx.nlayouts++;
}

static void nw_dx_vertex_array_set(NwRhiVertexArray *va, const NwRhiVertexLayout *layout, NwRhiBuffer *vertices,
                                   size_t offset, NwRhiBuffer *indices)
{
    if (!va) {
        return;
    }
    va->layout = nw_dx_layout_id(layout);
    va->vb = vertices;
    va->offset = offset;
    if (indices) {
        va->ib = indices;
    }
}

static void nw_dx_vertex_array_destroy(NwRhiVertexArray *va)
{
    SDL_free(va);
}

/* ---- state ---- */

static void nw_dx_set_target(NwRhiTexture *colour, NwRhiTexture *depth)
{
    if (!colour && !depth) {
        nw_dx_fit_window();
        nw_dx.col = nw_dx.ds = NULL;
        nw_dx.to_window = true;
        nw_dx.target_w = nw_dx.back_w;
        nw_dx.target_h = nw_dx.back_h;
    } else {
        nw_dx.col = colour;
        nw_dx.ds = depth;
        nw_dx.to_window = false;
        nw_dx.target_w = colour ? colour->w : depth->w;
        nw_dx.target_h = colour ? colour->h : depth->h;
    }
    nw_dx.target_dirty = true;
    nw_dx.viewport_dirty = true;
}

static void nw_dx_viewport(int x, int y, int width, int height)
{
    nw_dx.viewport[0] = x;
    nw_dx.viewport[1] = y;
    nw_dx.viewport[2] = width;
    nw_dx.viewport[3] = height;
    nw_dx.viewport_dirty = true;
}

static void nw_dx_viewport_get(int out[4])
{
    SDL_memcpy(out, nw_dx.viewport, sizeof(nw_dx.viewport));
}

static void nw_dx_blend(bool on, NwRhiBlendOp op, NwRhiBlendFactor src, NwRhiBlendFactor dst, NwRhiBlendOp aop,
                        NwRhiBlendFactor asrc, NwRhiBlendFactor adst)
{
    nw_dx.blend_on = on;
    const uint8_t b[6] = { (uint8_t)op, (uint8_t)src, (uint8_t)dst, (uint8_t)aop, (uint8_t)asrc, (uint8_t)adst };
    SDL_memcpy(nw_dx.blend, b, sizeof(b));
}

static void nw_dx_colour_mask(bool r, bool g, bool b, bool a)
{
    nw_dx.mask = (uint8_t)(r | g << 1 | b << 2 | a << 3);
}

static void nw_dx_depth(bool test, NwRhiCompare func)
{
    nw_dx.depth_test = test;
    nw_dx.depth_func = (uint8_t)func;
}

static void nw_dx_depth_write(bool on)
{
    nw_dx.depth_write = on;
}

static void nw_dx_stencil(bool test, NwRhiCompare func, uint8_t ref, uint8_t read_mask, NwRhiStencilOp fail,
                          NwRhiStencilOp zfail, NwRhiStencilOp pass)
{
    nw_dx.stencil_test = test;
    if (test) {
        nw_dx.stencil_func = (uint8_t)func;
        nw_dx.stencil_ref = ref;
        nw_dx.stencil_read = read_mask;
        nw_dx.stencil_ops[0] = (uint8_t)fail;
        nw_dx.stencil_ops[1] = (uint8_t)zfail;
        nw_dx.stencil_ops[2] = (uint8_t)pass;
    }
}

static void nw_dx_stencil_write_mask(uint8_t mask)
{
    nw_dx.stencil_write = mask;
}

static void nw_dx_wireframe(bool on)
{
    nw_dx.wireframe = on;
}

static void nw_dx_bind_texture(int unit, NwRhiTexture *t)
{
    if (unit >= 0 && unit < NW_DX_UNITS) {
        nw_dx.tex[unit] = t;
        nw_dx.buf[unit] = NULL;
    }
}

static void nw_dx_bind_texels(int unit, NwRhiBuffer *b)
{
    if (unit >= 0 && unit < NW_DX_UNITS) {
        nw_dx.buf[unit] = b;
        nw_dx.tex[unit] = NULL;
    }
}

/* ---- pipeline states ---- */

static const D3D12_BLEND nw_dx_factors[] = { D3D12_BLEND_ZERO,       D3D12_BLEND_ONE,       D3D12_BLEND_SRC_ALPHA,
                                             D3D12_BLEND_INV_SRC_ALPHA, D3D12_BLEND_DEST_COLOR, D3D12_BLEND_DEST_ALPHA,
                                             D3D12_BLEND_INV_DEST_ALPHA };
static const D3D12_BLEND_OP nw_dx_ops[] = { D3D12_BLEND_OP_ADD, D3D12_BLEND_OP_SUBTRACT, D3D12_BLEND_OP_REV_SUBTRACT };
static const D3D12_STENCIL_OP nw_dx_stencil_ops[] = { D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_INCR, D3D12_STENCIL_OP_DECR,
                                                      D3D12_STENCIL_OP_REPLACE };

static const DXGI_FORMAT nw_dx_attrib_formats[][4] = {
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
static UINT nw_dx_input_layout(const NwRhiProgram *p, int v, int lid, D3D12_INPUT_ELEMENT_DESC el[16])
{
    static const DXGI_FORMAT dummy[3] = { DXGI_FORMAT_R32G32B32A32_FLOAT, DXGI_FORMAT_R32G32B32A32_UINT,
                                          DXGI_FORMAT_R32G32B32A32_SINT };
    const NwRhiVertexLayout *l = &nw_dx.layouts[lid];
    UINT n = 0;
    for (int i = 0; i < p->src->ninputs[v] && n < 16; i++) {
        const NwD3dInput *in = &p->src->inputs[v][i];
        const NwRhiAttrib *a = NULL;
        for (int k = 0; k < l->count && !a; k++) {
            a = l->attribs[k].location == in->location ? &l->attribs[k] : NULL;
        }
        D3D12_INPUT_ELEMENT_DESC *e = &el[n++];
        e->SemanticName = "TEXCOORD";
        e->SemanticIndex = in->location;
        if (a) {
            e->Format = nw_dx_attrib_formats[a->type][a->components - 1];
            e->InputSlot = 0;
            e->AlignedByteOffset = a->offset;
            e->InputSlotClass =
                l->instanced ? D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA : D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;
            e->InstanceDataStepRate = l->instanced ? 1 : 0;
        } else {
            e->Format = dummy[in->kind];
            e->InputSlot = 1;
            e->AlignedByteOffset = in->kind * 16u;
            e->InputSlotClass = D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;
            e->InstanceDataStepRate = 0;
        }
    }
    return n;
}

static ID3D12PipelineState *nw_dx_pso(const NwDxPsoKey *k)
{
    uint32_t h = 2166136261u;
    const uint8_t *kb = (const uint8_t *)k;
    for (size_t j = 0; j < sizeof(*k); j++) {
        h = (h ^ kb[j]) * 16777619u;
    }
    uint32_t slot = h & (NW_DX_PSOS - 1);
    for (;; slot = (slot + 1) & (NW_DX_PSOS - 1)) {
        if (!nw_dx.psos[slot].pso) {
            break;
        }
        if (SDL_memcmp(&nw_dx.psos[slot].key, k, sizeof(*k)) == 0) {
            return nw_dx.psos[slot].pso;
        }
    }
    D3D12_GRAPHICS_PIPELINE_STATE_DESC d;
    SDL_zero(d);
    D3D12_INPUT_ELEMENT_DESC el[16];
    if (k->prog) {
        const NwD3dProgram *src = k->prog->src;
        d.pRootSignature = nw_dx.root;
        d.VS.pShaderBytecode = src->vs[k->v].code;
        d.VS.BytecodeLength = src->vs[k->v].size;
        d.PS.pShaderBytecode = src->ps.code;
        d.PS.BytecodeLength = src->ps.size;
        d.InputLayout.pInputElementDescs = el;
        d.InputLayout.NumElements = nw_dx_input_layout(k->prog, k->v, k->layout, el);
    } else {
        d.pRootSignature = nw_dx.util_root;
        d.VS.pShaderBytecode = nw_d3d11_util[NW_D3D_BLIT_VS].code;
        d.VS.BytecodeLength = nw_d3d11_util[NW_D3D_BLIT_VS].size;
        d.PS.pShaderBytecode = nw_d3d11_util[k->util].code;
        d.PS.BytecodeLength = nw_d3d11_util[k->util].size;
    }
    D3D12_RENDER_TARGET_BLEND_DESC *rt = &d.BlendState.RenderTarget[0];
    rt->BlendEnable = k->blend_on;
    rt->BlendOp = nw_dx_ops[k->blend[0]];
    rt->SrcBlend = nw_dx_factors[k->blend_on ? k->blend[1] : 1];
    rt->DestBlend = nw_dx_factors[k->blend[2]];
    rt->BlendOpAlpha = nw_dx_ops[k->blend[3]];
    rt->SrcBlendAlpha = nw_dx_factors[k->blend_on ? k->blend[4] : 1];
    rt->DestBlendAlpha = nw_dx_factors[k->blend[5]];
    rt->LogicOp = D3D12_LOGIC_OP_NOOP;
    rt->RenderTargetWriteMask = k->mask;
    d.SampleMask = 0xFFFFFFFFu;
    d.RasterizerState.FillMode = k->wireframe ? D3D12_FILL_MODE_WIREFRAME : D3D12_FILL_MODE_SOLID;
    d.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    d.RasterizerState.DepthClipEnable = TRUE;
    d.RasterizerState.MultisampleEnable = TRUE;
    D3D12_DEPTH_STENCIL_DESC *ds = &d.DepthStencilState;
    ds->DepthEnable = k->depth_test;
    ds->DepthWriteMask = k->depth_write ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
    ds->DepthFunc = (D3D12_COMPARISON_FUNC)(D3D12_COMPARISON_FUNC_NEVER + k->depth_func);
    ds->StencilEnable = k->stencil_test;
    ds->StencilReadMask = k->stencil_read;
    ds->StencilWriteMask = k->stencil_write;
    D3D12_DEPTH_STENCILOP_DESC f = { nw_dx_stencil_ops[k->stencil_ops[0]], nw_dx_stencil_ops[k->stencil_ops[1]],
                                     nw_dx_stencil_ops[k->stencil_ops[2]],
                                     (D3D12_COMPARISON_FUNC)(D3D12_COMPARISON_FUNC_NEVER + k->stencil_func) };
    ds->FrontFace = ds->BackFace = f;
    d.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    d.NumRenderTargets = k->colour ? 1 : 0;
    d.RTVFormats[0] = k->colour ? DXGI_FORMAT_R8G8B8A8_UNORM : DXGI_FORMAT_UNKNOWN;
    d.DSVFormat = k->depth ? DXGI_FORMAT_D24_UNORM_S8_UINT : DXGI_FORMAT_UNKNOWN;
    d.SampleDesc.Count = k->samples;
    ID3D12PipelineState *pso = NULL;
    HRESULT hr = ID3D12Device_CreateGraphicsPipelineState(nw_dx.dev, &d, &IID_ID3D12PipelineState, (void **)&pso);
    if (FAILED(hr)) {
        static int told;
        if (told++ < 4) {
            SDL_Log("Direct3D 12: CreateGraphicsPipelineState failed (0x%08lx), %s %d", (unsigned long)hr,
                    k->prog ? "program" : "util", k->util);
        }
        return NULL;
    }
    if (nw_dx.npsos < NW_DX_PSOS * 3 / 4) {
        nw_dx.psos[slot] = (NwDxPso){ *k, pso };
        nw_dx.npsos++;
    } else {
        static bool told;
        if (!told) {
            SDL_Log("Direct3D 12: more than %d pipeline states", NW_DX_PSOS * 3 / 4);
            told = true;
        }
        nw_dx_later((IUnknown *)pso, -1); /* used once, released after the GPU */
    }
    return pso;
}

/* The key's target part: the bound target's formats; no depth test or write without a depth buffer. */
static void nw_dx_key_target(NwDxPsoKey *k)
{
    k->colour = nw_dx.to_window || nw_dx.col;
    k->depth = !nw_dx.to_window && nw_dx.ds;
    k->samples = (uint8_t)(nw_dx.to_window ? 1 : nw_dx.col ? nw_dx.col->samples : nw_dx.ds->samples);
    if (!k->depth) {
        k->depth_test = k->depth_write = k->stencil_test = 0;
        k->depth_func = 0;
    }
    if (!k->stencil_test) {
        k->stencil_func = k->stencil_read = k->stencil_write = 0;
        SDL_memset(k->stencil_ops, 0, sizeof(k->stencil_ops));
    }
    if (!k->blend_on) {
        SDL_memset(k->blend, 0, sizeof(k->blend));
    }
}

/* ---- the target ---- */

static D3D12_CPU_DESCRIPTOR_HANDLE nw_dx_target_rtv(void)
{
    return nw_dx.to_window ? nw_dx_rtv(nw_dx.back_rtv[nw_dx.back_index]) : nw_dx_rtv(nw_dx.col ? nw_dx.col->rtv : 0);
}

/* The target's buffers in their drawing states, set on the command list when it changed. */
static void nw_dx_prepare_target(void)
{
    if (nw_dx.to_window) {
        nw_dx_back_state(D3D12_RESOURCE_STATE_RENDER_TARGET);
    } else {
        nw_dx_state(nw_dx.col, D3D12_RESOURCE_STATE_RENDER_TARGET);
        nw_dx_state(nw_dx.ds, D3D12_RESOURCE_STATE_DEPTH_WRITE);
    }
    if (nw_dx.target_dirty) {
        nw_dx_barriers_flush();
        D3D12_CPU_DESCRIPTOR_HANDLE rtv = nw_dx_target_rtv();
        D3D12_CPU_DESCRIPTOR_HANDLE dsv = nw_dx_dsv(nw_dx.ds && !nw_dx.to_window ? nw_dx.ds->dsv : 0);
        bool colour = nw_dx.to_window || nw_dx.col;
        bool depth = !nw_dx.to_window && nw_dx.ds;
        ID3D12GraphicsCommandList_OMSetRenderTargets(nw_dx.list, colour ? 1 : 0, colour ? &rtv : NULL, FALSE,
                                                     depth ? &dsv : NULL);
        nw_dx.target_dirty = false;
    }
}

static void nw_dx_apply_viewport(void)
{
    const int *v = nw_dx.viewport;
    float y = nw_dx.to_window ? (float)(nw_dx.target_h - v[1] - v[3]) : (float)v[1];
    D3D12_VIEWPORT vp = { (float)v[0], y, (float)v[2], (float)v[3], 0.0f, 1.0f };
    D3D12_RECT all = { 0, 0, nw_dx.target_w, nw_dx.target_h };
    ID3D12GraphicsCommandList_RSSetViewports(nw_dx.list, 1, &vp);
    ID3D12GraphicsCommandList_RSSetScissorRects(nw_dx.list, 1, &all);
    nw_dx.viewport_dirty = false;
}

static void nw_dx_set_root(ID3D12RootSignature *root)
{
    if (nw_dx.bound_root != root) {
        if (!nw_dx.bound_root) {
            ID3D12GraphicsCommandList_SetDescriptorHeaps(nw_dx.list, 1, &nw_dx.gpu_heap);
            ID3D12GraphicsCommandList_IASetPrimitiveTopology(nw_dx.list, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        }
        ID3D12GraphicsCommandList_SetGraphicsRootSignature(nw_dx.list, root);
        nw_dx.bound_root = root;
        nw_dx.bound_cb[0] = nw_dx.bound_cb[1] = 0;
        nw_dx.bound_table = 0;
    }
}

/* ---- draws ---- */

/* The nine units' descriptors as a table of the frame's heap (the last one again when nothing changed). */
static void nw_dx_units_table(void)
{
    uint64_t sig[NW_DX_UNITS];
    for (int i = 0; i < NW_DX_UNITS; i++) {
        NwRhiTexture *t = nw_dx.tex[i];
        bool target = t && !nw_dx.to_window && (t == nw_dx.col || t == nw_dx.ds);
        sig[i] = t && !target ? t->serial : nw_dx.buf[i] && nw_dx.buf[i]->res ? nw_dx.buf[i]->serial : 0;
    }
    if (nw_dx.table_epoch == nw_dx.epoch && SDL_memcmp(sig, nw_dx.table_sig, sizeof(sig)) == 0) {
        return;
    }
    int base = nw_dx_table_alloc(NW_DX_UNITS);
    for (int i = 0; i < NW_DX_UNITS; i++) {
        D3D12_CPU_DESCRIPTOR_HANDLE dst = nw_dx_gpu_cpu(base + i);
        NwRhiBuffer *b = nw_dx.buf[i];
        if (sig[i] && nw_dx.tex[i]) {
            ID3D12Device_CopyDescriptorsSimple(nw_dx.dev, 1, dst, nw_dx_cpu(nw_dx.tex[i]->srv),
                                               D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        } else if (sig[i] && b) {
            bool r32 = b->texels == NW_RHI_R32F;
            UINT elem = r32 ? 4 : 16;
            D3D12_SHADER_RESOURCE_VIEW_DESC sv;
            SDL_zero(sv);
            sv.Format = r32 ? DXGI_FORMAT_R32_FLOAT : DXGI_FORMAT_R32G32B32A32_FLOAT;
            sv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
            sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            sv.Buffer.FirstElement = b->off / elem;
            sv.Buffer.NumElements = (UINT)SDL_max(b->size / elem, (size_t)1);
            ID3D12Device_CreateShaderResourceView(nw_dx.dev, b->res, &sv, dst);
        } else {
            bool texels = i >= 3 && i <= 5; /* u_pool, u_weights, u_deltas */
            ID3D12Device_CopyDescriptorsSimple(nw_dx.dev, 1, dst, nw_dx_cpu(texels ? nw_dx.null_buf : nw_dx.null_tex),
                                               D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        }
    }
    SDL_memcpy(nw_dx.table_sig, sig, sizeof(sig));
    nw_dx.table_epoch = nw_dx.epoch;
    nw_dx.table = nw_dx_gpu(base);
}

/* Everything a draw takes, recorded; false when it cannot draw. */
static bool nw_dx_apply(NwRhiVertexArray *va, NwRhiBuffer *ib)
{
    NwRhiProgram *p = nw_dx.prog;
    if (!p || !va || va->layout < 0 || nw_dx.lost || (!nw_dx.to_window && !nw_dx.col && !nw_dx.ds)) {
        return false;
    }
    const NwRhiVertexLayout *l = &nw_dx.layouts[va->layout];
    int v = 0;
    for (int k = 0; k < l->count; k++) {
        v |= l->attribs[k].location == 1 && l->attribs[k].type == NW_RHI_S16_FLOAT;
    }
    /* the memory first: a full ring starts the list over, and everything is placed again */
    for (int tries = 0;; tries++) {
        uint64_t epoch = nw_dx.epoch;
        if (!nw_dx_buffer_ready(va->vb) || (ib && !nw_dx_buffer_ready(ib))) {
            return false;
        }
        for (int i = 0; i < NW_DX_UNITS; i++) {
            if (nw_dx.buf[i]) {
                nw_dx_buffer_ready(nw_dx.buf[i]);
            }
        }
        for (int s = 0; s < 2; s++) {
            if (p->data[s] && (p->dirty[s] || p->cb_epoch[s] != nw_dx.epoch)) {
                ID3D12Resource *res = NULL;
                uint64_t off = 0;
                uint8_t *cpu = NULL;
                if (!nw_dx_alloc(p->src->cbuffer[s], D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT, &res, &off,
                                 &cpu)) {
                    return false;
                }
                SDL_memcpy(cpu, p->data[s], p->src->cbuffer[s]);
                p->cb[s] = ID3D12Resource_GetGPUVirtualAddress(res) + off;
                p->cb_epoch[s] = nw_dx.epoch;
                p->dirty[s] = false;
            }
        }
        nw_dx_units_table();
        if (epoch == nw_dx.epoch || tries == 2) {
            break;
        }
    }
    NwDxPsoKey k;
    SDL_zero(k);
    k.prog = p;
    k.v = (uint8_t)v;
    k.layout = (uint8_t)va->layout;
    k.wireframe = nw_dx.wireframe;
    k.blend_on = nw_dx.blend_on;
    SDL_memcpy(k.blend, nw_dx.blend, sizeof(k.blend));
    k.mask = nw_dx.mask;
    k.depth_test = nw_dx.depth_test;
    k.depth_func = nw_dx.depth_func;
    k.depth_write = nw_dx.depth_write;
    k.stencil_test = nw_dx.stencil_test;
    k.stencil_func = nw_dx.stencil_func;
    k.stencil_read = nw_dx.stencil_read;
    k.stencil_write = nw_dx.stencil_write;
    SDL_memcpy(k.stencil_ops, nw_dx.stencil_ops, sizeof(k.stencil_ops));
    nw_dx_key_target(&k);
    ID3D12PipelineState *pso = nw_dx_pso(&k);
    if (!pso) {
        return false;
    }
    /* states: the units read, the target drawn into */
    for (int i = 0; i < NW_DX_UNITS; i++) {
        NwRhiTexture *t = nw_dx.tex[i];
        if (t && (nw_dx.to_window || (t != nw_dx.col && t != nw_dx.ds))) {
            nw_dx_state(t, NW_DX_SRV);
        }
    }
    nw_dx_prepare_target();
    nw_dx_barriers_flush();
    ID3D12GraphicsCommandList *c = nw_dx.list;
    nw_dx_set_root(nw_dx.root);
    if (nw_dx.bound_pso != pso) {
        ID3D12GraphicsCommandList_SetPipelineState(c, pso);
        nw_dx.bound_pso = pso;
    }
    if (nw_dx.viewport_dirty) {
        nw_dx_apply_viewport();
    }
    if (nw_dx.bound_ref != nw_dx.stencil_ref) {
        ID3D12GraphicsCommandList_OMSetStencilRef(c, nw_dx.stencil_ref);
        nw_dx.bound_ref = nw_dx.stencil_ref;
    }
    for (int s = 0; s < 2; s++) {
        if (p->data[s] && nw_dx.bound_cb[s] != p->cb[s]) {
            ID3D12GraphicsCommandList_SetGraphicsRootConstantBufferView(c, (UINT)s, p->cb[s]);
            nw_dx.bound_cb[s] = p->cb[s];
        }
    }
    if (nw_dx.bound_table != nw_dx.table.ptr) {
        ID3D12GraphicsCommandList_SetGraphicsRootDescriptorTable(c, 2, nw_dx.table);
        nw_dx.bound_table = nw_dx.table.ptr;
    }
    NwRhiBuffer *vb = va->vb;
    D3D12_VERTEX_BUFFER_VIEW vbv[2];
    vbv[0].BufferLocation = ID3D12Resource_GetGPUVirtualAddress(vb->res) + vb->off + va->offset;
    vbv[0].SizeInBytes = (UINT)(vb->size > va->offset ? vb->size - va->offset : 0);
    vbv[0].StrideInBytes = l->stride;
    vbv[1].BufferLocation = ID3D12Resource_GetGPUVirtualAddress(nw_dx.dummy);
    vbv[1].SizeInBytes = 48;
    vbv[1].StrideInBytes = 0;
    ID3D12GraphicsCommandList_IASetVertexBuffers(c, 0, 2, vbv);
    if (ib) {
        D3D12_INDEX_BUFFER_VIEW ibv = { ID3D12Resource_GetGPUVirtualAddress(ib->res) + ib->off, (UINT)ib->size,
                                        DXGI_FORMAT_R32_UINT };
        ID3D12GraphicsCommandList_IASetIndexBuffer(c, &ibv);
    }
    return true;
}

static void nw_dx_draw(NwRhiVertexArray *va, uint32_t first, uint32_t count)
{
    if (nw_dx_apply(va, NULL)) {
        ID3D12GraphicsCommandList_DrawInstanced(nw_dx.list, count, 1, first, 0);
    }
}

static void nw_dx_draw_indexed(NwRhiVertexArray *va, NwRhiBuffer *indices, uint32_t first, uint32_t count)
{
    NwRhiBuffer *ib = indices ? indices : va ? va->ib : NULL;
    if (ib && nw_dx_apply(va, ib)) {
        ID3D12GraphicsCommandList_DrawIndexedInstanced(nw_dx.list, count, 1, first, 0, 0);
    }
}

static void nw_dx_draw_instanced(NwRhiVertexArray *va, uint32_t first, uint32_t count, uint32_t instances)
{
    if (nw_dx_apply(va, NULL)) {
        ID3D12GraphicsCommandList_DrawInstanced(nw_dx.list, count, instances, first, 0);
    }
}

/* ---- util draws: clears and blits (d3d11_util.hlsl) ---- */

typedef struct NwDxUtilConsts {
    float map[4], scale[4], colour[4], depth[4];
} NwDxUtilConsts;

/* Where a util draw goes: a colour view and/or a depth one, of samples; the source SRVs of t0-t2 (0: none). */
typedef struct NwDxUtil {
    int ps;
    bool colour, depth;
    D3D12_CPU_DESCRIPTOR_HANDLE rtv, dsv;
    int samples;
    uint8_t mask;
    bool depth_write, stencil;
    uint8_t stencil_write, ref;
    D3D12_VIEWPORT vp;
    D3D12_RECT scissor;
    int srv[3]; /* CPU-only SRV slots, -1 none */
    NwDxUtilConsts k;
} NwDxUtil;

/* One triangle over the viewport with a util pixel shader; the caller has the buffers in their states. */
static void nw_dx_util_draw(const NwDxUtil *u)
{
    ID3D12Resource *res = NULL;
    uint64_t off = 0;
    uint8_t *cpu = NULL;
    int base = 0;
    for (int tries = 0; tries < 2; tries++) {
        uint64_t epoch = nw_dx.epoch;
        if (!nw_dx_alloc(sizeof(u->k), D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT, &res, &off, &cpu)) {
            return;
        }
        base = nw_dx_table_alloc(3);
        if (epoch == nw_dx.epoch) {
            break;
        }
    }
    SDL_memcpy(cpu, &u->k, sizeof(u->k));
    for (int i = 0; i < 3; i++) {
        int s = u->srv[i] >= 0 ? u->srv[i] : i == 2 ? nw_dx.null_ms : nw_dx.null_tex;
        ID3D12Device_CopyDescriptorsSimple(nw_dx.dev, 1, nw_dx_gpu_cpu(base + i), nw_dx_cpu(s),
                                           D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    }
    NwDxPsoKey k;
    SDL_zero(k);
    k.util = (uint8_t)u->ps;
    k.mask = u->mask;
    k.depth_test = k.depth_write = u->depth_write;
    k.depth_func = NW_RHI_ALWAYS;
    k.stencil_test = u->stencil;
    k.stencil_func = NW_RHI_ALWAYS;
    k.stencil_read = 0xFF;
    k.stencil_write = u->stencil_write;
    k.stencil_ops[0] = k.stencil_ops[1] = k.stencil_ops[2] = NW_RHI_REPLACE;
    k.colour = u->colour;
    k.depth = u->depth;
    k.samples = (uint8_t)u->samples;
    if (!k.depth) {
        k.depth_test = k.depth_write = k.stencil_test = 0;
    }
    if (!k.stencil_test) {
        k.stencil_func = k.stencil_read = k.stencil_write = 0;
        SDL_memset(k.stencil_ops, 0, sizeof(k.stencil_ops));
    }
    ID3D12PipelineState *pso = nw_dx_pso(&k);
    if (!pso) {
        return;
    }
    ID3D12GraphicsCommandList *c = nw_dx.list;
    nw_dx_barriers_flush();
    ID3D12GraphicsCommandList_OMSetRenderTargets(c, u->colour ? 1 : 0, u->colour ? &u->rtv : NULL, FALSE,
                                                 u->depth ? &u->dsv : NULL);
    nw_dx_set_root(nw_dx.util_root);
    ID3D12GraphicsCommandList_SetPipelineState(c, pso);
    ID3D12GraphicsCommandList_SetGraphicsRootConstantBufferView(c, 0, ID3D12Resource_GetGPUVirtualAddress(res) + off);
    ID3D12GraphicsCommandList_SetGraphicsRootDescriptorTable(c, 1, nw_dx_gpu(base));
    ID3D12GraphicsCommandList_RSSetViewports(c, 1, &u->vp);
    ID3D12GraphicsCommandList_RSSetScissorRects(c, 1, &u->scissor);
    ID3D12GraphicsCommandList_OMSetStencilRef(c, u->ref);
    ID3D12GraphicsCommandList_DrawInstanced(c, 3, 1, 0, 0);
    /* the next draw sets its own again */
    nw_dx.bound_pso = NULL;
    nw_dx.bound_ref = -1;
    nw_dx.target_dirty = true;
    nw_dx.viewport_dirty = true;
}

static void nw_dx_clear(const NwRhiClear *c)
{
    if (nw_dx.lost || (!nw_dx.to_window && !nw_dx.col && !nw_dx.ds)) {
        return;
    }
    bool has_colour = nw_dx.to_window || nw_dx.col;
    bool has_depth = !nw_dx.to_window && nw_dx.ds;
    bool all = c->mask[0] && c->mask[1] && c->mask[2] && c->mask[3];
    bool colour = c->colour && has_colour && (c->mask[0] || c->mask[1] || c->mask[2] || c->mask[3]);
    bool depth = c->depth && c->depth_write && has_depth;
    bool stencil = c->stencil && c->stencil_write && has_depth;
    if (!colour && !depth && !stencil) {
        return;
    }
    int w = nw_dx.target_w, h = nw_dx.target_h;
    D3D12_RECT r = { 0, 0, w, h };
    if (c->scissor) {
        const int *s = c->scissor;
        int top = nw_dx.to_window ? h - s[1] - s[3] : s[1];
        r.left = SDL_clamp(s[0], 0, w);
        r.right = SDL_clamp(s[0] + s[2], 0, w);
        r.top = SDL_clamp(top, 0, h);
        r.bottom = SDL_clamp(top + s[3], 0, h);
        if (r.left >= r.right || r.top >= r.bottom) {
            return;
        }
    }
    nw_dx_prepare_target();
    nw_dx_barriers_flush();
    UINT nr = c->scissor ? 1 : 0;
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = nw_dx_target_rtv();
    D3D12_CPU_DESCRIPTOR_HANDLE dsv = nw_dx_dsv(has_depth ? nw_dx.ds->dsv : 0);
    if (colour && all) {
        ID3D12GraphicsCommandList_ClearRenderTargetView(nw_dx.list, rtv, c->rgba, nr, nr ? &r : NULL);
        colour = false;
    }
    if ((depth || stencil) && (!stencil || c->stencil_write == 0xFF)) {
        D3D12_CLEAR_FLAGS flags = (depth ? D3D12_CLEAR_FLAG_DEPTH : 0) | (stencil ? D3D12_CLEAR_FLAG_STENCIL : 0);
        ID3D12GraphicsCommandList_ClearDepthStencilView(nw_dx.list, dsv, flags, (FLOAT)c->z, c->s, nr, nr ? &r : NULL);
        depth = stencil = false;
    }
    if (!colour && !depth && !stencil) {
        return;
    }
    /* The rest drawn: the whole target, cut to the scissor, writing what the masks let through. */
    NwDxUtil u;
    SDL_zero(u);
    u.ps = NW_D3D_CLEAR;
    u.colour = has_colour;
    u.depth = has_depth;
    u.rtv = rtv;
    u.dsv = dsv;
    u.samples = nw_dx.to_window ? 1 : nw_dx.col ? nw_dx.col->samples : nw_dx.ds->samples;
    u.mask = colour ? (uint8_t)(c->mask[0] | c->mask[1] << 1 | c->mask[2] << 2 | c->mask[3] << 3) : 0;
    u.depth_write = depth;
    u.stencil = stencil;
    u.stencil_write = c->stencil_write;
    u.ref = c->s;
    u.vp = (D3D12_VIEWPORT){ 0.0f, 0.0f, (float)w, (float)h, 0.0f, 1.0f };
    u.scissor = r;
    u.srv[0] = u.srv[1] = u.srv[2] = -1;
    u.k.colour[0] = c->rgba[0];
    u.k.colour[1] = c->rgba[1];
    u.k.colour[2] = c->rgba[2];
    u.k.colour[3] = c->rgba[3];
    u.k.depth[0] = (float)c->z;
    nw_dx_util_draw(&u);
}

/* A blit drawn: into the destination's colour (or depth), its rectangle the viewport, the source read by ps. */
static void nw_dx_blit_draw(const NwRhiBlit *b, int ps, NwRhiTexture *src, NwRhiTexture *dst)
{
    bool window = !b->dst_colour && !b->dst_depth;
    bool depth = ps == NW_D3D_BLIT_DEPTH || ps == NW_D3D_BLIT_DEPTH_MS;
    int target_h = window ? nw_dx.back_h : dst->h;
    int target_w = window ? nw_dx.back_w : dst->w;
    nw_dx_state(src, NW_DX_SRV);
    NwDxUtil u;
    SDL_zero(u);
    u.ps = ps;
    if (window) {
        nw_dx_back_state(D3D12_RESOURCE_STATE_RENDER_TARGET);
        u.colour = true;
        u.rtv = nw_dx_rtv(nw_dx.back_rtv[nw_dx.back_index]);
        u.samples = 1;
    } else if (depth) {
        nw_dx_state(dst, D3D12_RESOURCE_STATE_DEPTH_WRITE);
        u.depth = true;
        u.dsv = nw_dx_dsv(dst->dsv);
        u.samples = dst->samples;
        u.depth_write = true;
    } else {
        nw_dx_state(dst, D3D12_RESOURCE_STATE_RENDER_TARGET);
        u.colour = true;
        u.rtv = nw_dx_rtv(dst->rtv);
        u.samples = dst->samples;
    }
    u.mask = depth ? 0 : 0xF;
    int dw = b->dst[2] - b->dst[0], dh = b->dst[3] - b->dst[1];
    float top = window ? (float)(target_h - b->dst[3]) : (float)b->dst[1];
    u.vp = (D3D12_VIEWPORT){ (float)b->dst[0], top, (float)dw, (float)dh, 0.0f, 1.0f };
    u.scissor = (D3D12_RECT){ 0, 0, target_w, target_h };
    u.srv[0] = u.srv[1] = u.srv[2] = -1;
    u.srv[ps == NW_D3D_BLIT_DEPTH ? 1 : ps == NW_D3D_BLIT_DEPTH_MS ? 2 : 0] = src->srv;
    NwDxUtilConsts k = {
        { (float)b->src[0], (float)b->src[1], (float)b->dst[0], (float)b->dst[1] },
        { (float)(b->src[2] - b->src[0]) / (float)dw, (float)(b->src[3] - b->src[1]) / (float)dh,
          window ? (float)target_h : 0.0f, (float)b->src_level },
        { 0 },
        { 0 },
    };
    u.k = k;
    nw_dx_util_draw(&u);
}

static void nw_dx_copy_region(NwRhiTexture *d, int dx, int dy, NwRhiTexture *s, const int src[4])
{
    nw_dx_state(s, D3D12_RESOURCE_STATE_COPY_SOURCE);
    nw_dx_state(d, D3D12_RESOURCE_STATE_COPY_DEST);
    nw_dx_barriers_flush();
    D3D12_TEXTURE_COPY_LOCATION to, from;
    SDL_zero(to);
    SDL_zero(from);
    to.pResource = d->res;
    to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    from.pResource = s->res;
    from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_BOX box = { (UINT)src[0], (UINT)src[1], 0, (UINT)src[2], (UINT)src[3], 1 };
    ID3D12GraphicsCommandList_CopyTextureRegion(nw_dx.list, &to, (UINT)dx, (UINT)dy, 0, &from, &box);
}

static void nw_dx_copy_all(NwRhiTexture *d, NwRhiTexture *s)
{
    nw_dx_state(s, D3D12_RESOURCE_STATE_COPY_SOURCE);
    nw_dx_state(d, D3D12_RESOURCE_STATE_COPY_DEST);
    nw_dx_barriers_flush();
    ID3D12GraphicsCommandList_CopyResource(nw_dx.list, d->res, s->res);
}

static void nw_dx_blit(const NwRhiBlit *b)
{
    if (nw_dx.lost) {
        return;
    }
    bool window = !b->dst_colour && !b->dst_depth;
    int sw = b->src[2] - b->src[0], sh = b->src[3] - b->src[1];
    int dw = b->dst[2] - b->dst[0], dh = b->dst[3] - b->dst[1];
    if (sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0) {
        return;
    }
    bool same = !window && sw == dw && sh == dh && !b->src_level;
    if (window) {
        nw_dx_fit_window();
        if (nw_dx.to_window) {
            nw_dx.target_w = nw_dx.back_w;
            nw_dx.target_h = nw_dx.back_h;
        }
    }
    if (b->colour && b->src_colour) {
        NwRhiTexture *s = b->src_colour, *d = b->dst_colour;
        bool full = same && sw == s->w && sh == s->h && d && sw == d->w && sh == d->h;
        if (same && d && s->samples > 1 && d->samples <= 1 && full) {
            nw_dx_state(s, D3D12_RESOURCE_STATE_RESOLVE_SOURCE);
            nw_dx_state(d, D3D12_RESOURCE_STATE_RESOLVE_DEST);
            nw_dx_barriers_flush();
            ID3D12GraphicsCommandList_ResolveSubresource(nw_dx.list, d->res, 0, s->res, 0,
                                                         DXGI_FORMAT_R8G8B8A8_UNORM);
        } else if (same && d && s->samples == d->samples && s->samples > 1 && full) {
            nw_dx_copy_all(d, s);
        } else if (same && d && s->samples == d->samples && s->samples <= 1) {
            nw_dx_copy_region(d, b->dst[0], b->dst[1], s, b->src);
        } else if (s->samples <= 1 && (window || d->rtv >= 0)) {
            nw_dx_blit_draw(b, b->linear ? NW_D3D_BLIT_LINEAR : NW_D3D_BLIT_POINT, s, d);
        }
    }
    if ((b->depth || b->stencil) && b->src_depth && b->dst_depth) {
        NwRhiTexture *s = b->src_depth, *d = b->dst_depth;
        bool full = same && sw == s->w && sh == s->h && sw == d->w && sh == d->h;
        if (full && s->samples == d->samples) {
            nw_dx_copy_all(d, s);
        } else if (d->samples <= 1) {
            /* Depth alone; an MSAA resolve's stencil is not carried over (xbox/RENDERERS.md). */
            nw_dx_blit_draw(b, s->samples > 1 ? NW_D3D_BLIT_DEPTH_MS : NW_D3D_BLIT_DEPTH, s, d);
        }
    }
    nw_dx.target_dirty = true;
    nw_dx.viewport_dirty = true;
}

/* A target's smaller levels, each drawn (bilinear, a 2x2 average) from the one before. */
static void nw_dx_generate_mips(NwRhiTexture *t)
{
    if (!(t->flags & NW_RHI_TEX_MIPGEN) || t->levels <= 1 || nw_dx.lost) {
        return;
    }
    nw_dx_state(t, D3D12_RESOURCE_STATE_RENDER_TARGET);
    for (int i = 1; i < t->levels; i++) {
        int sw = SDL_max(t->w >> (i - 1), 1), sh = SDL_max(t->h >> (i - 1), 1);
        int dw = SDL_max(t->w >> i, 1), dh = SDL_max(t->h >> i, 1);
        nw_dx_barrier(t->res, (UINT)(i - 1), D3D12_RESOURCE_STATE_RENDER_TARGET, NW_DX_SRV);
        D3D12_SHADER_RESOURCE_VIEW_DESC sv;
        SDL_zero(sv);
        sv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sv.Texture2D.MostDetailedMip = (UINT)(i - 1);
        sv.Texture2D.MipLevels = 1;
        ID3D12Device_CreateShaderResourceView(nw_dx.dev, t->res, &sv, nw_dx_cpu(nw_dx.scratch_srv));
        D3D12_RENDER_TARGET_VIEW_DESC rv;
        SDL_zero(rv);
        rv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        rv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        rv.Texture2D.MipSlice = (UINT)i;
        ID3D12Device_CreateRenderTargetView(nw_dx.dev, t->res, &rv, nw_dx_rtv(nw_dx.scratch_rtv));
        NwDxUtil u;
        SDL_zero(u);
        u.ps = NW_D3D_BLIT_LINEAR;
        u.colour = true;
        u.rtv = nw_dx_rtv(nw_dx.scratch_rtv);
        u.samples = 1;
        u.mask = 0xF;
        u.vp = (D3D12_VIEWPORT){ 0.0f, 0.0f, (float)dw, (float)dh, 0.0f, 1.0f };
        u.scissor = (D3D12_RECT){ 0, 0, dw, dh };
        u.srv[0] = nw_dx.scratch_srv;
        u.srv[1] = u.srv[2] = -1;
        u.k.scale[0] = (float)sw / (float)dw;
        u.k.scale[1] = (float)sh / (float)dh;
        nw_dx_util_draw(&u);
    }
    /* every level ends readable */
    nw_dx_barrier(t->res, (UINT)(t->levels - 1), D3D12_RESOURCE_STATE_RENDER_TARGET, NW_DX_SRV);
    t->state = NW_DX_SRV;
}

/* ---- read back ---- */

/* The rectangle of res's subresource 0 (in state, which it is left in) as RGBA8 rows; waits for the GPU. */
static bool nw_dx_read(ID3D12Resource *res, int x, int y, int width, int height, void *rgba, int pitch)
{
    if (width <= 0 || height <= 0) {
        return true;
    }
    UINT row = (UINT)nw_dx_align((uint64_t)width * 4, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT);
    D3D12_HEAP_PROPERTIES hp = { D3D12_HEAP_TYPE_READBACK, D3D12_CPU_PAGE_PROPERTY_UNKNOWN, D3D12_MEMORY_POOL_UNKNOWN, 0, 0 };
    D3D12_RESOURCE_DESC rd = { D3D12_RESOURCE_DIMENSION_BUFFER, 0, (UINT64)row * (UINT64)height, 1, 1, 1,
                               DXGI_FORMAT_UNKNOWN, { 1, 0 }, D3D12_TEXTURE_LAYOUT_ROW_MAJOR,
                               D3D12_RESOURCE_FLAG_NONE };
    ID3D12Resource *rb = NULL;
    HRESULT hr = ID3D12Device_CreateCommittedResource(nw_dx.dev, &hp, D3D12_HEAP_FLAG_NONE, &rd,
                                                      D3D12_RESOURCE_STATE_COPY_DEST, NULL, &IID_ID3D12Resource,
                                                      (void **)&rb);
    if (FAILED(hr)) {
        return nw_dx_fail("a readback buffer", hr);
    }
    nw_dx_barriers_flush();
    D3D12_TEXTURE_COPY_LOCATION to, from;
    SDL_zero(to);
    SDL_zero(from);
    to.pResource = rb;
    to.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    to.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    to.PlacedFootprint.Footprint.Width = (UINT)width;
    to.PlacedFootprint.Footprint.Height = (UINT)height;
    to.PlacedFootprint.Footprint.Depth = 1;
    to.PlacedFootprint.Footprint.RowPitch = row;
    from.pResource = res;
    from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_BOX box = { (UINT)x, (UINT)y, 0, (UINT)(x + width), (UINT)(y + height), 1 };
    ID3D12GraphicsCommandList_CopyTextureRegion(nw_dx.list, &to, 0, 0, 0, &from, &box);
    nw_dx_flush();
    void *map = NULL;
    D3D12_RANGE range = { 0, (SIZE_T)row * (SIZE_T)height };
    hr = ID3D12Resource_Map(rb, 0, &range, &map);
    if (SUCCEEDED(hr)) {
        for (int i = 0; i < height; i++) {
            SDL_memcpy((uint8_t *)rgba + (size_t)i * (size_t)pitch, (const uint8_t *)map + (size_t)i * row,
                       (size_t)width * 4);
        }
        D3D12_RANGE nothing = { 0, 0 };
        ID3D12Resource_Unmap(rb, 0, &nothing);
    }
    ID3D12Resource_Release(rb);
    return SUCCEEDED(hr) || nw_dx_fail("Map", hr);
}

static bool nw_dx_texture_read(NwRhiTexture *t, int x, int y, int width, int height, void *rgba, int pitch)
{
    if (t->samples > 1 || t->format != NW_RHI_RGBA8) {
        return SDL_SetError("Direct3D 12: only single-sampled RGBA8 textures are read");
    }
    nw_dx_state(t, D3D12_RESOURCE_STATE_COPY_SOURCE);
    return nw_dx_read(t->res, x, y, width, height, rgba, pitch);
}

/* The swap chain's rows are the window's from the top (only textures keep GL's rows). */
static bool nw_dx_window_read(int width, int height, void *rgba, int pitch)
{
    ID3D12Resource *back = nw_dx.back[nw_dx.back_index];
    if (!back) {
        return SDL_SetError("Direct3D 12: no window buffer");
    }
    nw_dx_back_state(D3D12_RESOURCE_STATE_COPY_SOURCE);
    return nw_dx_read(back, 0, 0, SDL_min(width, nw_dx.back_w), SDL_min(height, nw_dx.back_h), rgba, pitch);
}

#define NW_DX_ENTRY_X(name, ret, params, args) .name = nw_dx_##name,
#define NW_DX_ENTRY_V(name, params, args) .name = nw_dx_##name,
const NwRhiBackend nw_rhi_d3d12 = { .prepare_window = nw_dx_prepare_window,
                                    NW_RHI_CALLS(NW_DX_ENTRY_X, NW_DX_ENTRY_V) };

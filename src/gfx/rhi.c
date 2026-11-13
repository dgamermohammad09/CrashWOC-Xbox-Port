/* The backend table and the public calls, each one through the running backend's entry. */
#include "gfx/rhi_impl.h"

static const NwRhiBackend *const nw_rhi_backends[NW_RHI_KINDS] = {
    [NW_RHI_OPENGL] = &nw_rhi_gl,
#ifdef NW_RHI_HAVE_D3D11
    [NW_RHI_D3D11] = &nw_rhi_d3d11,
#endif
#ifdef NW_RHI_HAVE_D3D12
    [NW_RHI_D3D12] = &nw_rhi_d3d12,
#endif
};

static const char *const nw_rhi_names[NW_RHI_KINDS] = { "opengl", "d3d11", "d3d12", "vulkan" };

static struct {
    bool selected;
    NwRhiKind want;
    NwRhiKind kind;
    const NwRhiBackend *b;
} nw_rhi = { false, NW_RHI_OPENGL, NW_RHI_OPENGL, &nw_rhi_gl };

const char *nw_rhi_kind_name(NwRhiKind kind)
{
    return kind < NW_RHI_KINDS ? nw_rhi_names[kind] : "?";
}

bool nw_rhi_kind_parse(const char *name, NwRhiKind *out)
{
    static const struct {
        const char *name;
        NwRhiKind kind;
    } aliases[] = { { "gl", NW_RHI_OPENGL }, { "dx11", NW_RHI_D3D11 }, { "dx12", NW_RHI_D3D12 }, { "vk", NW_RHI_VULKAN } };
    for (int i = 0; name && i < NW_RHI_KINDS; i++) {
        if (SDL_strcasecmp(name, nw_rhi_names[i]) == 0) {
            *out = (NwRhiKind)i;
            return true;
        }
    }
    for (size_t i = 0; name && i < SDL_arraysize(aliases); i++) {
        if (SDL_strcasecmp(name, aliases[i].name) == 0) {
            *out = aliases[i].kind;
            return true;
        }
    }
    return false;
}

bool nw_rhi_kind_available(NwRhiKind kind)
{
    return kind < NW_RHI_KINDS && nw_rhi_backends[kind];
}

NwRhiKind nw_rhi_kind_default(void)
{
#if defined(NW_UWP) && defined(NW_RHI_HAVE_D3D11)
    return NW_RHI_D3D11;
#else
    return NW_RHI_OPENGL;
#endif
}

void nw_rhi_select(NwRhiKind kind)
{
    nw_rhi.want = kind;
    nw_rhi.selected = true;
}

NwRhiKind nw_rhi_selected(void)
{
    NwRhiKind k = nw_rhi.selected ? nw_rhi.want : nw_rhi_kind_default();
    return nw_rhi_kind_available(k) ? k : nw_rhi_kind_default();
}

NwRhiKind nw_rhi_kind(void)
{
    return nw_rhi.kind;
}

SDL_WindowFlags nw_rhi_prepare_window(void)
{
    if (nw_rhi.selected && !nw_rhi_kind_available(nw_rhi.want)) {
        SDL_Log("renderer %s is not built in; using %s", nw_rhi_kind_name(nw_rhi.want),
                nw_rhi_kind_name(nw_rhi_kind_default()));
    }
    nw_rhi.kind = nw_rhi_selected();
    nw_rhi.b = nw_rhi_backends[nw_rhi.kind];
    return nw_rhi.b->prepare_window();
}

#define NW_RHI_WRAP_X(name, ret, params, args) \
    ret nw_rhi_##name params                   \
    {                                          \
        return nw_rhi.b->name args;            \
    }
#define NW_RHI_WRAP_V(name, params, args) \
    void nw_rhi_##name params             \
    {                                     \
        nw_rhi.b->name args;              \
    }
NW_RHI_CALLS(NW_RHI_WRAP_X, NW_RHI_WRAP_V)

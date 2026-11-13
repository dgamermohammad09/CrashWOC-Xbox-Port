/* What a backend of src/gfx/rhi.h implements: one function per public call, in a table rhi.c dispatches to.
   Adding a backend (xbox/RENDERERS.md): write rhi_<name>.c defining `const NwRhiBackend nw_rhi_<name>` with every
   entry filled, list it in rhi.c's table under its NwRhiKind, add the file to CMakeLists.txt. */
#ifndef NUWORLD_GFX_RHI_IMPL_H
#define NUWORLD_GFX_RHI_IMPL_H

#include "gfx/rhi.h"

/* X(name, return type, parameters, arguments) for the calls that return a value, V(...) for those that do not. */
#define NW_RHI_CALLS(X, V)                                                                                           \
    X(init, bool, (SDL_Window * window), (window))                                                                   \
    V(shutdown, (void), ())                                                                                          \
    X(native, void *, (int which), (which))                                                                          \
    X(description, const char *, (void), ())                                                                         \
    X(caps, const NwRhiCaps *, (void), ())                                                                           \
    X(set_vsync, bool, (int interval), (interval))                                                                   \
    X(vsync, int, (void), ())                                                                                        \
    V(present, (void), ())                                                                                           \
    X(buffer_create, NwRhiBuffer *, (NwRhiBufferKind kind, NwRhiFormat texels), (kind, texels))                      \
    V(buffer_data, (NwRhiBuffer * b, size_t size, const void *data, bool stream), (b, size, data, stream))           \
    V(buffer_destroy, (NwRhiBuffer * b), (b))                                                                        \
    X(texture_create, NwRhiTexture *,                                                                                \
      (NwRhiFormat format, int width, int height, int levels, int samples, unsigned flags, const void *const *data), \
      (format, width, height, levels, samples, flags, data))                                                         \
    V(texture_update, (NwRhiTexture * t, int level, int x, int y, int width, int height, const void *data),         \
      (t, level, x, y, width, height, data))                                                                         \
    V(texture_destroy, (NwRhiTexture * t), (t))                                                                      \
    V(texture_size, (const NwRhiTexture *t, int *width, int *height), (t, width, height))                            \
    X(texture_ui, uint64_t, (NwRhiTexture * t), (t))                                                                 \
    V(generate_mips, (NwRhiTexture * t), (t))                                                                        \
    X(texture_read, bool, (NwRhiTexture * t, int x, int y, int width, int height, void *rgba, int pitch),           \
      (t, x, y, width, height, rgba, pitch))                                                                         \
    X(program_create, NwRhiProgram *, (const NwRhiProgramDesc *d), (d))                                              \
    V(program_destroy, (NwRhiProgram * p), (p))                                                                      \
    X(uniform, int, (NwRhiProgram * p, const char *name), (p, name))                                                 \
    V(set_uniform,                                                                                                   \
      (NwRhiProgram * p, int uniform, NwRhiUniformType type, int components, int count, const void *values),         \
      (p, uniform, type, components, count, values))                                                                 \
    V(use_program, (NwRhiProgram * p), (p))                                                                          \
    X(vertex_array_create, NwRhiVertexArray *, (void), ())                                                           \
    V(vertex_array_set,                                                                                              \
      (NwRhiVertexArray * va, const NwRhiVertexLayout *layout, NwRhiBuffer *vertices, size_t offset,                 \
       NwRhiBuffer *indices),                                                                                        \
      (va, layout, vertices, offset, indices))                                                                       \
    V(vertex_array_destroy, (NwRhiVertexArray * va), (va))                                                           \
    V(set_target, (NwRhiTexture * colour, NwRhiTexture * depth), (colour, depth))                                    \
    V(viewport, (int x, int y, int width, int height), (x, y, width, height))                                        \
    V(viewport_get, (int out[4]), (out))                                                                             \
    V(blend,                                                                                                         \
      (bool on, NwRhiBlendOp op, NwRhiBlendFactor src, NwRhiBlendFactor dst, NwRhiBlendOp alpha_op,                  \
       NwRhiBlendFactor alpha_src, NwRhiBlendFactor alpha_dst),                                                      \
      (on, op, src, dst, alpha_op, alpha_src, alpha_dst))                                                            \
    V(colour_mask, (bool r, bool g, bool b, bool a), (r, g, b, a))                                                   \
    V(depth, (bool test, NwRhiCompare func), (test, func))                                                           \
    V(depth_write, (bool on), (on))                                                                                  \
    V(stencil,                                                                                                       \
      (bool test, NwRhiCompare func, uint8_t ref, uint8_t read_mask, NwRhiStencilOp fail, NwRhiStencilOp zfail,      \
       NwRhiStencilOp pass),                                                                                         \
      (test, func, ref, read_mask, fail, zfail, pass))                                                               \
    V(stencil_write_mask, (uint8_t mask), (mask))                                                                    \
    V(wireframe, (bool on), (on))                                                                                    \
    V(bind_texture, (int unit, NwRhiTexture *t), (unit, t))                                                          \
    V(bind_texels, (int unit, NwRhiBuffer *b), (unit, b))                                                            \
    V(draw, (NwRhiVertexArray * va, uint32_t first, uint32_t count), (va, first, count))                             \
    V(draw_indexed, (NwRhiVertexArray * va, NwRhiBuffer * indices, uint32_t first, uint32_t count),                  \
      (va, indices, first, count))                                                                                   \
    V(draw_instanced, (NwRhiVertexArray * va, uint32_t first, uint32_t count, uint32_t instances),                   \
      (va, first, count, instances))                                                                                 \
    V(clear, (const NwRhiClear *c), (c))                                                                             \
    V(blit, (const NwRhiBlit *b), (b))

#define NW_RHI_MEMBER_X(name, ret, params, args) ret(*name) params;
#define NW_RHI_MEMBER_V(name, params, args) void(*name) params;
typedef struct NwRhiBackend {
    SDL_WindowFlags (*prepare_window)(void);
    NW_RHI_CALLS(NW_RHI_MEMBER_X, NW_RHI_MEMBER_V)
} NwRhiBackend;
#undef NW_RHI_MEMBER_X
#undef NW_RHI_MEMBER_V

extern const NwRhiBackend nw_rhi_gl;
#ifdef NW_RHI_HAVE_D3D11
extern const NwRhiBackend nw_rhi_d3d11;
#endif

#endif

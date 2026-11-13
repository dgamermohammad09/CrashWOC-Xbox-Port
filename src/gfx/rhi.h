/* The renderer's GPU interface (render hardware interface): exactly what src/gfx/backend.c, src/gfx/vu.c and the
   frontend's Dear ImGui need, over OpenGL 3.3 (rhi_gl.c) or Direct3D 11 (rhi_d3d11.c). Render thread only.

   Conventions, all backends (see xbox/RENDERERS.md):
   - Coordinates are OpenGL's: a texture's row 0 is the bottom of what is drawn into it (clip-space y -1), viewport,
     scissor and blit rectangles count rows from row 0, a fragment's position is (column + 0.5, row + 0.5). Backends
     whose clip space points the other way (Direct3D, Vulkan) flip y in the vertex shader so that every texture holds
     the same bytes in the same rows as under GL; only the window (the swap chain) is flipped back when shown.
   - Clip-space z runs -w..w (GL); backends with 0..w remap it in the vertex shader. Depth buffers hold 0 (near) .. 1.
   - Programs are GLSL (src/gfx/shaders/), one source per stage built in variants by #defines; backends that cannot
     compile GLSL at runtime take bytecode tools/gen_shaders.py made from the same source at build time.
   - Uniforms are GLSL default-block uniforms set by name, kept per program like GL's.
   - Texture units are fixed per sampler name (vu.c); textures and texel buffers bind to units, not to programs. */
#ifndef NUWORLD_GFX_RHI_H
#define NUWORLD_GFX_RHI_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <SDL3/SDL.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum NwRhiKind {
    NW_RHI_OPENGL,
    NW_RHI_D3D11,
    NW_RHI_D3D12,  /* reserved: not implemented yet */
    NW_RHI_VULKAN, /* reserved: not implemented yet */
    NW_RHI_KINDS
} NwRhiKind;

/* "opengl", "d3d11", "d3d12", "vulkan" (settings.toml's [video] renderer and --renderer). */
const char *nw_rhi_kind_name(NwRhiKind kind);
/* A name to its kind (also "gl", "dx11"); false when unknown. */
bool nw_rhi_kind_parse(const char *name, NwRhiKind *out);
/* Built into this executable. */
bool nw_rhi_kind_available(NwRhiKind kind);
/* OpenGL on the desktop, Direct3D 11 in the UWP (Xbox) build. */
NwRhiKind nw_rhi_kind_default(void);

/* The backend the next window takes (nw_platform_init); one unavailable falls back to the default. */
void nw_rhi_select(NwRhiKind kind);
NwRhiKind nw_rhi_selected(void);
/* The backend running (valid after nw_rhi_init). */
NwRhiKind nw_rhi_kind(void);
/* Before SDL_CreateWindow: window attributes (GL's context attributes) and the flags the window needs. */
SDL_WindowFlags nw_rhi_prepare_window(void);
/* The device (and GL context) on the window; false with an SDL error. */
bool nw_rhi_init(SDL_Window *window);
void nw_rhi_shutdown(void);
/* The backend's native handles for Dear ImGui: GL the SDL_GLContext; D3D11 the ID3D11Device and context. */
void *nw_rhi_native(int which);
/* "OpenGL 4.6 (renderer, vendor)" or "Direct3D 11 (adapter)". */
const char *nw_rhi_description(void);

typedef struct NwRhiCaps {
    int max_texture; /* the largest texture side */
    int max_samples; /* MSAA samples of an RGBA8 + D24S8 target */
} NwRhiCaps;
const NwRhiCaps *nw_rhi_caps(void);

/* 0 off, 1 on, -1 adaptive; false when the interval cannot be had. */
bool nw_rhi_set_vsync(int interval);
int nw_rhi_vsync(void);
/* Shows the window's frame. */
void nw_rhi_present(void);

/* ---- resources ---- */

typedef struct NwRhiBuffer NwRhiBuffer;
typedef struct NwRhiTexture NwRhiTexture;
typedef struct NwRhiProgram NwRhiProgram;
typedef struct NwRhiVertexArray NwRhiVertexArray;

typedef enum NwRhiFormat {
    NW_RHI_RGBA8,   /* unsigned normalised; render targets, the UI */
    NW_RHI_D24S8,   /* depth (sampled as its 0..1 value) and stencil */
    NW_RHI_RGBA8UI, /* unsigned integer, read by texelFetch */
    NW_RHI_R8UI,
    NW_RHI_R32F,    /* texel buffers */
    NW_RHI_RGBA32F,
} NwRhiFormat;

typedef enum NwRhiBufferKind {
    NW_RHI_VERTICES,
    NW_RHI_INDICES, /* uint32 */
    NW_RHI_TEXELS,  /* a texel buffer (GLSL samplerBuffer) of the buffer's format */
} NwRhiBufferKind;

NwRhiBuffer *nw_rhi_buffer_create(NwRhiBufferKind kind, NwRhiFormat texels);
/* The buffer's whole contents, replaced (it may be drawn from before, which keeps the old ones); stream: rewritten
   every frame. */
void nw_rhi_buffer_data(NwRhiBuffer *b, size_t size, const void *data, bool stream);
void nw_rhi_buffer_destroy(NwRhiBuffer *b);

enum {
    NW_RHI_TEX_TARGET = 1 << 0, /* drawn into (colour or depth-stencil) */
    NW_RHI_TEX_MIPGEN = 1 << 1, /* nw_rhi_generate_mips fills its smaller levels (a target's) */
    NW_RHI_TEX_LINEAR = 1 << 2, /* sampled bilinear (the UI); the programs only texelFetch */
};

/* levels mip levels (1 for targets: MIPGEN gives a target its chain), samples > 1 an MSAA target (drawn into and
   resolved by nw_rhi_blit, never sampled); data, when not NULL, holds each level's rows, tightly packed. */
NwRhiTexture *nw_rhi_texture_create(NwRhiFormat format, int width, int height, int levels, int samples,
                                    unsigned flags, const void *const *data);
/* Part of a level, rows tightly packed, row 0 first. */
void nw_rhi_texture_update(NwRhiTexture *t, int level, int x, int y, int width, int height, const void *data);
void nw_rhi_texture_destroy(NwRhiTexture *t);
/* Level 0's size. */
void nw_rhi_texture_size(const NwRhiTexture *t, int *width, int *height);
/* An RGBA8 texture as Dear ImGui's ImTextureID. */
uint64_t nw_rhi_texture_ui(NwRhiTexture *t);
/* A target's smaller levels from level 0, averaged. */
void nw_rhi_generate_mips(NwRhiTexture *t);
/* RGBA8 rows of a colour target, from row y up: out row i is texture row y + i. False with an SDL error. */
bool nw_rhi_texture_read(NwRhiTexture *t, int x, int y, int width, int height, void *rgba, int pitch);
/* The window's frame drawn so far (before nw_rhi_present), RGBA8 of its pixel size, top row first (screenshots). */
bool nw_rhi_window_read(int width, int height, void *rgba, int pitch);

/* ---- programs ---- */

typedef enum NwRhiUniformType {
    NW_RHI_FLOAT, /* count components a value, 1-4 */
    NW_RHI_INT,
    NW_RHI_UINT,
    NW_RHI_MAT3, /* column-major, as GL takes them untransposed */
    NW_RHI_MAT4,
} NwRhiUniformType;

/* set names a shader set (precompiled backends find its bytecode by set and variant); vs and fs are its GLSL bodies
   for the backends that compile them, after `#version 330 core` and defines; samplers the sampler uniforms' names in
   the order of their texture units (NULL-terminated). */
typedef struct NwRhiProgramDesc {
    const char *set;
    int variant;
    const char *defines;
    const char *vs, *fs;
    size_t vs_size, fs_size;
    const char *const *samplers;
} NwRhiProgramDesc;

NwRhiProgram *nw_rhi_program_create(const NwRhiProgramDesc *d);
void nw_rhi_program_destroy(NwRhiProgram *p);
/* The uniform's handle, -1 when the program does not use it (setting -1 does nothing). */
int nw_rhi_uniform(NwRhiProgram *p, const char *name);
/* count values (array elements) of components each; MAT3 and MAT4 ignore components. */
void nw_rhi_set_uniform(NwRhiProgram *p, int uniform, NwRhiUniformType type, int components, int count,
                        const void *values);
void nw_rhi_use_program(NwRhiProgram *p);

/* ---- vertex input ---- */

typedef enum NwRhiAttribType {
    NW_RHI_F32,        /* float */
    NW_RHI_S16_FLOAT,  /* int16 converted to float, not normalised */
    NW_RHI_U8_FLOAT,   /* uint8 converted to float, not normalised */
    NW_RHI_U8_INT,     /* integer inputs (GLSL uvec) */
    NW_RHI_U16_INT,
    NW_RHI_U32_INT,
} NwRhiAttribType;

typedef struct NwRhiAttrib {
    uint8_t location;
    uint8_t type; /* NwRhiAttribType */
    uint8_t components;
    uint16_t offset;
} NwRhiAttrib;

#define NW_RHI_MAX_ATTRIBS 9

typedef struct NwRhiVertexLayout {
    uint16_t stride;
    uint8_t count;
    bool instanced; /* one vertex a instance */
    NwRhiAttrib attribs[NW_RHI_MAX_ATTRIBS];
} NwRhiVertexLayout;

/* A vertex buffer read by a layout from a byte offset on, and an index buffer (GL's vertex array object). */
NwRhiVertexArray *nw_rhi_vertex_array_create(void);
void nw_rhi_vertex_array_set(NwRhiVertexArray *va, const NwRhiVertexLayout *layout, NwRhiBuffer *vertices,
                             size_t offset, NwRhiBuffer *indices);
void nw_rhi_vertex_array_destroy(NwRhiVertexArray *va);

/* ---- state, all of it kept until changed ---- */

typedef enum NwRhiCompare {
    NW_RHI_NEVER,
    NW_RHI_LESS,
    NW_RHI_EQUAL,
    NW_RHI_LEQUAL,
    NW_RHI_GREATER,
    NW_RHI_NOTEQUAL,
    NW_RHI_GEQUAL,
    NW_RHI_ALWAYS
} NwRhiCompare;

typedef enum NwRhiBlendOp { NW_RHI_ADD, NW_RHI_SUBTRACT, NW_RHI_REV_SUBTRACT } NwRhiBlendOp;
typedef enum NwRhiBlendFactor {
    NW_RHI_ZERO,
    NW_RHI_ONE,
    NW_RHI_SRC_ALPHA,
    NW_RHI_INV_SRC_ALPHA,
    NW_RHI_DST_COLOR,
    NW_RHI_DST_ALPHA,
    NW_RHI_INV_DST_ALPHA
} NwRhiBlendFactor;
typedef enum NwRhiStencilOp { NW_RHI_KEEP, NW_RHI_INCR_WRAP, NW_RHI_DECR_WRAP, NW_RHI_REPLACE } NwRhiStencilOp;

/* The draws go to colour (RGBA8, maybe MSAA) and depth (D24S8 of the same size and samples, or NULL); both NULL:
   the window. */
void nw_rhi_set_target(NwRhiTexture *colour, NwRhiTexture *depth);
/* In the target's rows from row 0 (the window's from its bottom). */
void nw_rhi_viewport(int x, int y, int width, int height);
void nw_rhi_viewport_get(int out[4]);
void nw_rhi_blend(bool on, NwRhiBlendOp op, NwRhiBlendFactor src, NwRhiBlendFactor dst, NwRhiBlendOp alpha_op,
                  NwRhiBlendFactor alpha_src, NwRhiBlendFactor alpha_dst);
void nw_rhi_colour_mask(bool r, bool g, bool b, bool a);
void nw_rhi_depth(bool test, NwRhiCompare func);
void nw_rhi_depth_write(bool on);
void nw_rhi_stencil(bool test, NwRhiCompare func, uint8_t ref, uint8_t read_mask, NwRhiStencilOp fail,
                    NwRhiStencilOp zfail, NwRhiStencilOp pass);
void nw_rhi_stencil_write_mask(uint8_t mask);
void nw_rhi_wireframe(bool on);
/* NULL: textures; texel buffers bind with nw_rhi_bind_texels. */
void nw_rhi_bind_texture(int unit, NwRhiTexture *t);
void nw_rhi_bind_texels(int unit, NwRhiBuffer *b);

/* ---- work ---- */

void nw_rhi_draw(NwRhiVertexArray *va, uint32_t first, uint32_t count);
/* indices NULL: the vertex array's own; first counts indices. */
void nw_rhi_draw_indexed(NwRhiVertexArray *va, NwRhiBuffer *indices, uint32_t first, uint32_t count);
void nw_rhi_draw_instanced(NwRhiVertexArray *va, uint32_t first, uint32_t count, uint32_t instances);

/* What a clear writes in the target: its own masks (not the draws'), within scissor (x, y, w, h) when not NULL. */
typedef struct NwRhiClear {
    bool colour, depth, stencil;
    float rgba[4];
    double z;
    uint8_t s;
    bool mask[4];       /* colour channels written */
    bool depth_write;   /* depth written (else a depth clear does nothing) */
    uint8_t stencil_write;
    const int *scissor;
} NwRhiClear;
void nw_rhi_clear(const NwRhiClear *c);

/* A rectangle (x0, y0, x1, y1) of one target's buffers into another's, as glBlitFramebuffer: same-size colour and
   depth-stencil copies, MSAA resolves (same size), scaled colour (nearest or linear) and depth (nearest). The
   source's colour is read from level src_level. dst_colour and dst_depth NULL: the window. Leaves the target set. */
typedef struct NwRhiBlit {
    NwRhiTexture *src_colour, *src_depth;
    NwRhiTexture *dst_colour, *dst_depth;
    int src_level;
    int src[4], dst[4];
    bool colour, depth, stencil;
    bool linear;
} NwRhiBlit;
void nw_rhi_blit(const NwRhiBlit *b);

#ifdef __cplusplus
}
#endif

#endif

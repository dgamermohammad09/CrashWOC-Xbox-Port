/* The Vulkan SPIR-V tools/gen_shaders.py --target vulkan builds from src/gfx/shaders/ (build/gen/vk_shaders.c). The
   inputs and uniforms are described as for Direct3D 11 (rhi_d3d11_shaders.h), from the same GLSL. */
#ifndef NUWORLD_GFX_RHI_VK_SHADERS_H
#define NUWORLD_GFX_RHI_VK_SHADERS_H

#include "gfx/rhi_d3d11_shaders.h"

typedef struct NwVkShader {
    const uint32_t *code;
    size_t size; /* bytes */
} NwVkShader;

/* vs[0] takes float texcoords, vs[1] int16 ones; ubo[] the stages' default uniform block sizes (binding 16 of the
   vertex stage, 17 of the fragment stage; 0 for none). */
typedef struct NwVkProgram {
    NwVkShader vs[2];
    const NwD3dInput *inputs[2];
    uint8_t ninputs[2];
    NwVkShader fs;
    uint32_t ubo[2];
    const NwD3dUniform *uniforms;
    uint32_t nuniforms;
} NwVkProgram;

typedef struct NwVkSet {
    const char *name;
    const NwVkProgram *programs;
    int count;
} NwVkSet;

extern const NwVkSet nw_vk_sets[];
extern const int nw_vk_nsets;

/* The sampler bindings 0-8 (the texture units): 1 a combined image sampler, 2 a uniform texel buffer, 0 unused. */
#define NW_VK_UNITS 9
extern const uint8_t nw_vk_units[NW_VK_UNITS];

/* d3d11_util.hlsl for Vulkan (NW_D3D_BLIT_VS ... NW_D3D_CLEAR): b0 binding 0, t0-t2 bindings 1-3, s0 binding 4. */
extern const NwVkShader nw_vk_util[NW_D3D_UTILS];

#endif

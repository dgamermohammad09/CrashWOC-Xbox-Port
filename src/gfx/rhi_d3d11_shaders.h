/* The Direct3D 11 bytecode tools/gen_shaders.py builds from src/gfx/shaders/ (build/gen/d3d11_shaders.c). */
#ifndef NUWORLD_GFX_RHI_D3D11_SHADERS_H
#define NUWORLD_GFX_RHI_D3D11_SHADERS_H

#include <stddef.h>
#include <stdint.h>

typedef struct NwD3dShader {
    const unsigned char *code;
    size_t size;
} NwD3dShader;

/* A vertex stage input: its location (TEXCOORD<location>), 0 float, 1 uint, 2 int, and its components. */
typedef struct NwD3dInput {
    uint8_t location, kind, components;
} NwD3dInput;

#define NW_D3D_ABSENT 0xFFFF

/* A default-block uniform: its byte offset in the vertex and the pixel stage's cbuffer b0 (NW_D3D_ABSENT where the
   stage does not use it), its array length (1 when not an array; elements 16 bytes apart). */
typedef struct NwD3dUniform {
    const char *name;
    uint16_t offset[2];
    uint16_t array;
} NwD3dUniform;

/* vs[0] takes float texcoords, vs[1] int16 ones (the same build when the program reads none). */
typedef struct NwD3dProgram {
    NwD3dShader vs[2];
    const NwD3dInput *inputs[2];
    uint8_t ninputs[2];
    NwD3dShader ps;
    uint32_t cbuffer[2]; /* the stages' b0 sizes, 0 for none */
    const NwD3dUniform *uniforms;
    uint32_t nuniforms;
} NwD3dProgram;

typedef struct NwD3dSet {
    const char *name; /* NwRhiProgramDesc.set */
    const NwD3dProgram *programs;
    int count; /* NwRhiProgramDesc.variant indexes them */
} NwD3dSet;

extern const NwD3dSet nw_d3d11_sets[];
extern const int nw_d3d11_nsets;

/* src/gfx/shaders/d3d11_util.hlsl: a full-viewport triangle, the blits and the masked clear. */
enum {
    NW_D3D_BLIT_VS,
    NW_D3D_BLIT_POINT,
    NW_D3D_BLIT_LINEAR,
    NW_D3D_BLIT_DEPTH,
    NW_D3D_BLIT_DEPTH_MS,
    NW_D3D_CLEAR,
    NW_D3D_UTILS
};
extern const NwD3dShader nw_d3d11_util[NW_D3D_UTILS];

#endif

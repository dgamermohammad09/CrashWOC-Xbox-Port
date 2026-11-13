/* What Dear ImGui's Direct3D 12 backend (src/frontend/ui.cpp) needs from src/gfx/rhi_d3d12.c beyond nw_rhi_native():
   nw_rhi_native(0) is the ID3D12Device, (1) the ID3D12CommandQueue, (2) the ID3D12GraphicsCommandList with the
   window bound as the target (Dear ImGui records into it), (3) the shader-visible ID3D12DescriptorHeap. */
#ifndef NUWORLD_GFX_RHI_D3D12_H
#define NUWORLD_GFX_RHI_D3D12_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Frames the CPU records ahead of the GPU. */
#define NW_RHI_D3D12_FRAMES 2

/* One descriptor of nw_rhi_native(3)'s heap that stays until freed (D3D12_CPU/GPU_DESCRIPTOR_HANDLE.ptr). */
bool nw_rhi_d3d12_ui_alloc(uint64_t *cpu, uint64_t *gpu);
void nw_rhi_d3d12_ui_free(uint64_t gpu);
/* Waits until the GPU has finished everything submitted (before Dear ImGui releases its buffers). */
void nw_rhi_d3d12_idle(void);

#ifdef __cplusplus
}
#endif

#endif

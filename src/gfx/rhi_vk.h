/* What Dear ImGui's Vulkan backend (src/frontend/ui.cpp) needs from src/gfx/rhi_vk.c beyond nw_rhi_native():
   nw_rhi_native(0) is the VkInstance, (1) the VkPhysicalDevice, (2) the VkDevice, (3) the VkQueue, (4) the queue
   family (as an integer), (5) the VkCommandBuffer recording inside a dynamic rendering of the window (Dear ImGui
   records into it), (6) the window's VkFormat (as an integer), (7) the window's image count. */
#ifndef NUWORLD_GFX_RHI_VK_H
#define NUWORLD_GFX_RHI_VK_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The instance's API version (VkApplicationInfo.apiVersion). */
uint32_t nw_rhi_vk_api_version(void);
/* A Vulkan function of the running driver (instance or device level), NULL when it has none. */
void (*nw_rhi_vk_proc(const char *name))(void);
/* How nw_rhi_texture_ui() makes Dear ImGui textures: add(view, layout) gives a VkDescriptorSet, remove frees it. */
void nw_rhi_vk_ui_textures(uint64_t (*add)(uint64_t view, int layout), void (*remove)(uint64_t set));
/* Waits until the GPU has finished everything submitted. */
void nw_rhi_vk_idle(void);

#ifdef __cplusplus
}
#endif

#endif

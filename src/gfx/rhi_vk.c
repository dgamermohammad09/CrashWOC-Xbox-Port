/* src/gfx/rhi.h over Vulkan 1.2 with VK_KHR_dynamic_rendering and VK_KHR_push_descriptor (no render pass objects, no
   descriptor pools). The driver is the system's Vulkan loader (vulkan-1.dll) when it has a GPU, else Mesa's Dozen
   (vulkan_dzn.dll, Vulkan on Direct3D 12) loaded as an ICD directly: next to the executable, NW_VK_ICD=<dll>, or in
   the UWP package (LoadPackagedLibrary; Xbox has no Vulkan loader). Without a Direct3D 12 GPU Dozen is told to use
   WARP (DZN_DEBUG=warp).

   The window: a VK_KHR_win32_surface swap chain on the desktop. Dozen has no surface for a UWP CoreWindow, so there
   (or with NW_VK_PRESENT=copy) the window is an offscreen image, copied to the CPU at every present and shown by a
   small Direct3D 11 swap chain (nw_vk_copy_*): the simplest path that works, at the cost of a wait and a copy a
   frame.

   The programs are tools/gen_shaders.py --target vulkan's SPIR-V of the same GLSL. Vulkan's framebuffer row 0 is the
   first row in memory as GL's is, so unlike Direct3D nothing is flipped for textures; the vertex stages only map z to
   0..w. The window is a framebuffer like Direct3D's (row 0 at the top), so the window rules of rhi_d3d11.c hold:
   viewports on it are mirrored and blits to it run d3d11_util.hlsl upside down (the same util shaders, compiled to
   SPIR-V by glslang).

   Memory, state and synchronisation follow rhi_d3d12.c: a per-frame upload ring (uniforms, stream buffers kept on the
   CPU and placed again after the ring moved on, texture uploads), a device-local buffer per static
   nw_rhi_buffer_data, image layouts tracked per texture and changed by full barriers outside the rendering, pipelines
   cached by state. */
#define VK_NO_PROTOTYPES
#define VK_USE_PLATFORM_WIN32_KHR
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <stdlib.h>
#include <vulkan/vulkan.h>

#include <d3d11.h>
#include <d3d12.h>
#include <dxgi1_2.h>

#include "gfx/rhi_impl.h"
#include "gfx/rhi_vk.h"
#include "gfx/rhi_vk_shaders.h"

#ifndef DXGI_SWAP_EFFECT_FLIP_DISCARD
#define DXGI_SWAP_EFFECT_FLIP_DISCARD ((DXGI_SWAP_EFFECT)4)
#endif

#define NW_VK_FRAMES 2
#define NW_VK_IMAGES 8                  /* swap chain images at most */
#define NW_VK_LAYOUTS 16                /* distinct vertex layouts */
#define NW_VK_RING ((VkDeviceSize)32 << 20) /* a frame's upload ring */
#define NW_VK_PSOS 4096                 /* cached pipelines (a power of two) */
#define NW_VK_SETS 4096                 /* a frame's descriptor sets (a full pool waits for the GPU) */
#define NW_VK_TOMB ((VkPipeline)(uintptr_t)~(uintptr_t)0) /* a destroyed program's cache slot */

/* ---- the driver's functions ---- */

#define NW_VK_GLOBAL(X) X(vkCreateInstance) X(vkEnumerateInstanceExtensionProperties)
#define NW_VK_INSTANCE(X)                                                                                              \
    X(vkDestroyInstance) X(vkEnumeratePhysicalDevices) X(vkGetPhysicalDeviceProperties)                                \
    X(vkGetPhysicalDeviceQueueFamilyProperties) X(vkGetPhysicalDeviceMemoryProperties) X(vkGetPhysicalDeviceFeatures)  \
    X(vkGetPhysicalDeviceFormatProperties) X(vkEnumerateDeviceExtensionProperties) X(vkCreateDevice)                   \
    X(vkGetDeviceProcAddr)
#define NW_VK_SURFACE(X)                                                                                               \
    X(vkCreateWin32SurfaceKHR) X(vkDestroySurfaceKHR) X(vkGetPhysicalDeviceSurfaceSupportKHR)                          \
    X(vkGetPhysicalDeviceSurfaceCapabilitiesKHR) X(vkGetPhysicalDeviceSurfaceFormatsKHR)                               \
    X(vkGetPhysicalDeviceSurfacePresentModesKHR)
#define NW_VK_DEVICE(X)                                                                                                \
    X(vkDestroyDevice) X(vkGetDeviceQueue) X(vkDeviceWaitIdle) X(vkQueueSubmit) X(vkCreateCommandPool)                 \
    X(vkDestroyCommandPool) X(vkResetCommandPool) X(vkAllocateCommandBuffers) X(vkBeginCommandBuffer)                  \
    X(vkEndCommandBuffer) X(vkCreateFence) X(vkDestroyFence) X(vkWaitForFences) X(vkResetFences) X(vkCreateSemaphore)  \
    X(vkDestroySemaphore) X(vkCreateBuffer) X(vkDestroyBuffer) X(vkGetBufferMemoryRequirements) X(vkAllocateMemory)    \
    X(vkFreeMemory) X(vkBindBufferMemory) X(vkMapMemory) X(vkUnmapMemory) X(vkCreateImage) X(vkDestroyImage)           \
    X(vkGetImageMemoryRequirements) X(vkBindImageMemory) X(vkCreateImageView) X(vkDestroyImageView)                    \
    X(vkCreateBufferView) X(vkDestroyBufferView) X(vkCreateSampler) X(vkDestroySampler) X(vkCreateShaderModule)        \
    X(vkDestroyShaderModule) X(vkCreateDescriptorSetLayout) X(vkDestroyDescriptorSetLayout) X(vkCreatePipelineLayout)  \
    X(vkDestroyPipelineLayout) X(vkCreateGraphicsPipelines) X(vkDestroyPipeline) X(vkCmdPipelineBarrier)               \
    X(vkCmdCopyBuffer) X(vkCmdCopyBufferToImage) X(vkCmdCopyImageToBuffer) X(vkCmdCopyImage) X(vkCmdBlitImage)         \
    X(vkCmdResolveImage) X(vkCmdBeginRenderingKHR) X(vkCmdEndRenderingKHR) X(vkCmdBindPipeline)                        \
    X(vkCmdBindVertexBuffers) X(vkCmdBindIndexBuffer) X(vkCmdSetViewport) X(vkCmdSetScissor)                          \
    X(vkCmdSetStencilReference) X(vkCmdDraw) X(vkCmdDrawIndexed) X(vkCmdClearAttachments) X(vkCreateDescriptorPool)    \
    X(vkDestroyDescriptorPool) X(vkResetDescriptorPool) X(vkAllocateDescriptorSets) X(vkUpdateDescriptorSets)          \
    X(vkCmdBindDescriptorSets)
#define NW_VK_SWAPCHAIN(X)                                                                                             \
    X(vkCreateSwapchainKHR) X(vkDestroySwapchainKHR) X(vkGetSwapchainImagesKHR) X(vkAcquireNextImageKHR)               \
    X(vkQueuePresentKHR)

#define NW_VK_MEMBER(name) PFN_##name name;
static struct {
    NW_VK_GLOBAL(NW_VK_MEMBER)
    NW_VK_INSTANCE(NW_VK_MEMBER)
    NW_VK_SURFACE(NW_VK_MEMBER)
    NW_VK_DEVICE(NW_VK_MEMBER)
    NW_VK_SWAPCHAIN(NW_VK_MEMBER)
} vk;
#undef NW_VK_MEMBER

typedef VkResult(VKAPI_PTR *NwVkNegotiate)(uint32_t *version);

/* ---- objects ---- */

struct NwRhiBuffer {
    NwRhiBufferKind kind;
    NwRhiFormat texels;
    bool stream;
    size_t size;
    VkBuffer buf; /* where draws read the data: own (static) or the ring (stream) */
    VkDeviceSize off;
    VkBufferView view; /* texels: over buf from off */
    uint64_t epoch;
    uint64_t serial;
    uint8_t *shadow;
    size_t shadow_cap;
    VkBuffer own;
    VkDeviceMemory own_mem;
};

struct NwRhiTexture {
    NwRhiFormat format;
    int w, h, levels, samples;
    unsigned flags;
    VkImage img;
    VkDeviceMemory mem;
    VkImageView view; /* sampled (depth: its depth aspect) */
    VkImageView att;  /* drawn into: level 0 */
    VkImageLayout layout;
    VkFormat fmt;
    VkImageAspectFlags aspect;
    uint64_t serial;
    uint64_t ui; /* Dear ImGui's descriptor set, 0 none */
};

struct NwRhiProgram {
    const NwVkProgram *src;
    VkShaderModule vs[2], fs;
    uint8_t *data[2];
    bool dirty[2];
    VkDeviceSize ubo_off[2];
    VkBuffer ubo_buf[2];
    uint64_t ubo_epoch[2];
};

struct NwRhiVertexArray {
    int layout;
    NwRhiBuffer *vb, *ib;
    size_t offset;
};

typedef struct NwVkPsoKey {
    const NwRhiProgram *prog; /* NULL: a util shader */
    uint8_t util, v, layout, wireframe;
    uint8_t blend_on, blend[6], mask;
    uint8_t depth_test, depth_func, depth_write, stencil_test;
    uint8_t stencil_func, stencil_read, stencil_write, stencil_ops[3];
    uint8_t colour, depth, samples, pad; /* colour: 0 none, 1 RGBA8, 2 BGRA8 */
} NwVkPsoKey;

typedef struct NwVkPso {
    NwVkPsoKey key;
    VkPipeline pipe;
} NwVkPso;

typedef enum NwVkKind { NW_VK_BUFFER, NW_VK_MEMORY, NW_VK_IMAGE, NW_VK_VIEW, NW_VK_BUFFER_VIEW, NW_VK_PIPELINE, NW_VK_UI } NwVkKind;

typedef struct NwVkLater {
    NwVkKind kind;
    uint64_t h;
} NwVkLater;

/* Where a rendering draws: a colour view (and its image's layout) and/or a depth-stencil one. */
typedef struct NwVkTarget {
    VkImage col_img, ds_img;
    VkImageView col, ds;
    VkImageLayout *col_layout, *ds_layout;
    VkFormat col_fmt;
    int w, h, samples;
} NwVkTarget;

static struct {
    HMODULE lib;
    PFN_vkGetInstanceProcAddr gipa;
    bool icd, dozen, surface_wanted;
    uint32_t api;
    VkInstance inst;
    VkPhysicalDevice phys;
    VkDevice dev;
    VkQueue queue;
    uint32_t family;
    VkPhysicalDeviceProperties props;
    VkPhysicalDeviceMemoryProperties memprops;
    bool wireframe_ok, surface_ext;
    VkFormat depth_fmt;
    /* frames */
    VkCommandPool pool[NW_VK_FRAMES];
    VkDescriptorPool dpool[NW_VK_FRAMES];
    VkCommandBuffer cmd[NW_VK_FRAMES];
    VkFence fence[NW_VK_FRAMES];
    bool submitted[NW_VK_FRAMES];
    VkSemaphore acquired[NW_VK_FRAMES];
    int frame;
    uint64_t epoch, serial;
    bool lost;
    /* the upload ring, releases */
    VkBuffer ring[NW_VK_FRAMES];
    VkDeviceMemory ring_mem[NW_VK_FRAMES];
    uint8_t *ring_map[NW_VK_FRAMES];
    VkDeviceSize ring_used;
    NwVkLater *later[NW_VK_FRAMES];
    int nlater[NW_VK_FRAMES], later_cap[NW_VK_FRAMES];
    /* the window */
    SDL_Window *window;
    bool copy;
    VkSurfaceKHR surface;
    VkSwapchainKHR swap;
    VkFormat win_fmt;
    uint32_t nimages, image;
    VkImage images[NW_VK_IMAGES];
    VkImageView views[NW_VK_IMAGES];
    VkImageLayout layouts[NW_VK_IMAGES];
    VkSemaphore done[NW_VK_IMAGES];
    bool have_image, acquire_pending, swap_dirty;
    int win_w, win_h;
    /* the window in copy mode: an image, read back and shown by Direct3D 11 */
    NwRhiTexture *win_tex;
    VkBuffer win_rb;
    VkDeviceMemory win_rb_mem;
    uint8_t *win_rb_map;
    ID3D11Device *d3d;
    ID3D11DeviceContext *d3dctx;
    IDXGISwapChain1 *d3dswap;
    ID3D11Texture2D *d3dback;
    int interval;
    NwRhiCaps caps;
    char description[256];
    /* pipelines */
    VkSampler point, linear;
    VkDescriptorSetLayout set_layout, util_set_layout;
    VkPipelineLayout layout, util_layout;
    VkShaderModule util_mod[NW_D3D_UTILS];
    NwVkPso *psos;
    int npsos;
    NwRhiVertexLayout vlayouts[NW_VK_LAYOUTS];
    int nvlayouts;
    NwRhiTexture *dummy_tex, *dummy_ms;
    VkBuffer dummy_buf;
    VkDeviceMemory dummy_mem;
    VkBufferView dummy_view;
    /* the command buffer's rendering */
    bool recording, rendering;
    VkImageView r_col, r_ds;
    /* what the draws take */
    NwRhiTexture *col, *ds;
    bool to_window;
    int target_w, target_h;
    int viewport[4];
    NwRhiProgram *prog;
    NwRhiTexture *tex[NW_VK_UNITS];
    NwRhiBuffer *buf[NW_VK_UNITS];
    uint8_t stencil_ref;
    bool wireframe;
    bool blend_on;
    uint8_t blend[6];
    uint8_t mask;
    bool depth_test, depth_write;
    uint8_t depth_func;
    bool stencil_test;
    uint8_t stencil_func, stencil_read, stencil_write, stencil_ops[3];
    /* what the command buffer has */
    VkPipeline bound_pipe;
    VkDescriptorSet set, bound_set;
    int bound_ref;
    bool viewport_dirty;
    uint64_t push_sig[NW_VK_UNITS + 4];
    /* Dear ImGui */
    uint64_t (*ui_add)(uint64_t view, int layout);
    void (*ui_remove)(uint64_t set);
} nw_vk;

static bool nw_vk_fail(const char *what, VkResult r)
{
    return SDL_SetError("Vulkan: %s failed (%d)", what, (int)r);
}

static VkDeviceSize nw_vk_align(VkDeviceSize v, VkDeviceSize a)
{
    return a ? (v + a - 1) / a * a : v;
}

/* ---- memory ---- */

static int nw_vk_memory_type(uint32_t bits, VkMemoryPropertyFlags want, VkMemoryPropertyFlags nice)
{
    int best = -1;
    for (uint32_t i = 0; i < nw_vk.memprops.memoryTypeCount; i++) {
        VkMemoryPropertyFlags f = nw_vk.memprops.memoryTypes[i].propertyFlags;
        if (!(bits & (1u << i)) || (f & want) != want) {
            continue;
        }
        if ((f & nice) == nice) {
            return (int)i;
        }
        if (best < 0) {
            best = (int)i;
        }
    }
    return best;
}

static bool nw_vk_alloc_memory(VkMemoryRequirements req, VkMemoryPropertyFlags want, VkMemoryPropertyFlags nice,
                               VkDeviceMemory *out)
{
    int type = nw_vk_memory_type(req.memoryTypeBits, want, nice);
    if (type < 0) {
        type = nw_vk_memory_type(req.memoryTypeBits, 0, 0);
    }
    VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, req.size, (uint32_t)type };
    VkResult r = type >= 0 ? vk.vkAllocateMemory(nw_vk.dev, &ai, NULL, out) : VK_ERROR_OUT_OF_DEVICE_MEMORY;
    return r == VK_SUCCESS || nw_vk_fail("vkAllocateMemory", r);
}

/* A buffer with its own memory; host: mapped (into *map). */
static bool nw_vk_buffer(VkDeviceSize size, VkBufferUsageFlags usage, bool host, VkBuffer *buf, VkDeviceMemory *mem,
                         void **map)
{
    VkBufferCreateInfo bi = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, size, usage, VK_SHARING_MODE_EXCLUSIVE, 0,
                              NULL };
    VkResult r = vk.vkCreateBuffer(nw_vk.dev, &bi, NULL, buf);
    if (r != VK_SUCCESS) {
        return nw_vk_fail("vkCreateBuffer", r);
    }
    VkMemoryRequirements req;
    vk.vkGetBufferMemoryRequirements(nw_vk.dev, *buf, &req);
    VkMemoryPropertyFlags want = host ? VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
                                      : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    if (!nw_vk_alloc_memory(req, want, host ? VK_MEMORY_PROPERTY_HOST_CACHED_BIT : 0, mem)) {
        vk.vkDestroyBuffer(nw_vk.dev, *buf, NULL);
        *buf = VK_NULL_HANDLE;
        return false;
    }
    vk.vkBindBufferMemory(nw_vk.dev, *buf, *mem, 0);
    if (host) {
        r = vk.vkMapMemory(nw_vk.dev, *mem, 0, VK_WHOLE_SIZE, 0, map);
        if (r != VK_SUCCESS) {
            return nw_vk_fail("vkMapMemory", r);
        }
    }
    return true;
}

/* ---- releasing after the GPU ---- */

static void nw_vk_later(NwVkKind kind, uint64_t h)
{
    if (!h) {
        return;
    }
    int f = nw_vk.frame;
    if (nw_vk.nlater[f] == nw_vk.later_cap[f]) {
        int cap = nw_vk.later_cap[f] ? nw_vk.later_cap[f] * 2 : 64;
        NwVkLater *n = SDL_realloc(nw_vk.later[f], sizeof(*n) * (size_t)cap);
        if (!n) {
            return;
        }
        nw_vk.later[f] = n;
        nw_vk.later_cap[f] = cap;
    }
    nw_vk.later[f][nw_vk.nlater[f]++] = (NwVkLater){ kind, h };
}

static void nw_vk_destroy(NwVkKind kind, uint64_t h)
{
    VkDevice d = nw_vk.dev;
    switch (kind) {
    case NW_VK_BUFFER:
        vk.vkDestroyBuffer(d, (VkBuffer)h, NULL);
        break;
    case NW_VK_MEMORY:
        vk.vkFreeMemory(d, (VkDeviceMemory)h, NULL);
        break;
    case NW_VK_IMAGE:
        vk.vkDestroyImage(d, (VkImage)h, NULL);
        break;
    case NW_VK_VIEW:
        vk.vkDestroyImageView(d, (VkImageView)h, NULL);
        break;
    case NW_VK_BUFFER_VIEW:
        vk.vkDestroyBufferView(d, (VkBufferView)h, NULL);
        break;
    case NW_VK_PIPELINE:
        vk.vkDestroyPipeline(d, (VkPipeline)h, NULL);
        break;
    case NW_VK_UI:
        if (nw_vk.ui_remove) {
            nw_vk.ui_remove(h);
        }
        break;
    }
}

static void nw_vk_release_later(int f)
{
    for (int i = 0; i < nw_vk.nlater[f]; i++) {
        nw_vk_destroy(nw_vk.later[f][i].kind, nw_vk.later[f][i].h);
    }
    nw_vk.nlater[f] = 0;
}

/* ---- the command buffer ---- */

static VkCommandBuffer nw_vk_cmd(void)
{
    return nw_vk.cmd[nw_vk.frame];
}

static void nw_vk_end_rendering(void)
{
    if (nw_vk.rendering) {
        vk.vkCmdEndRenderingKHR(nw_vk_cmd());
        nw_vk.rendering = false;
    }
}

static void nw_vk_invalidate(void)
{
    nw_vk.bound_pipe = VK_NULL_HANDLE;
    nw_vk.bound_set = VK_NULL_HANDLE;
    nw_vk.bound_ref = -1;
    nw_vk.viewport_dirty = true;
}

/* Everything before, done before everything after (memory and execution); outside a rendering. */
static void nw_vk_memory_barrier(void)
{
    nw_vk_end_rendering();
    VkMemoryBarrier mb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_MEMORY_WRITE_BIT,
                           VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT };
    vk.vkCmdPipelineBarrier(nw_vk_cmd(), VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1,
                            &mb, 0, NULL, 0, NULL);
}

static void nw_vk_image_barrier(VkImage img, VkImageAspectFlags aspect, uint32_t level, uint32_t levels,
                                VkImageLayout from, VkImageLayout to)
{
    nw_vk_end_rendering();
    VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                               NULL,
                               VK_ACCESS_MEMORY_WRITE_BIT,
                               VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
                               from,
                               to,
                               VK_QUEUE_FAMILY_IGNORED,
                               VK_QUEUE_FAMILY_IGNORED,
                               img,
                               { aspect, level, levels, 0, 1 } };
    vk.vkCmdPipelineBarrier(nw_vk_cmd(), VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0,
                            NULL, 0, NULL, 1, &b);
}

static void nw_vk_layout(VkImage img, VkImageAspectFlags aspect, VkImageLayout *cur, VkImageLayout to)
{
    if (*cur != to) {
        nw_vk_image_barrier(img, aspect, 0, VK_REMAINING_MIP_LEVELS, *cur, to);
        *cur = to;
    }
}

static void nw_vk_tex_layout(NwRhiTexture *t, VkImageLayout to)
{
    if (t) {
        nw_vk_layout(t->img, t->aspect, &t->layout, to);
    }
}

static void nw_vk_flush(void);

/* A descriptor set of the frame's pool (a full pool: everything recorded runs first and the pool starts over). */
static VkDescriptorSet nw_vk_alloc_set(VkDescriptorSetLayout layout)
{
    for (int tries = 0; tries < 2; tries++) {
        VkDescriptorSetAllocateInfo ai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, NULL,
                                           nw_vk.dpool[nw_vk.frame], 1, &layout };
        VkDescriptorSet set = VK_NULL_HANDLE;
        VkResult r = vk.vkAllocateDescriptorSets(nw_vk.dev, &ai, &set);
        if (r == VK_SUCCESS) {
            return set;
        }
        if (tries == 0) {
            nw_vk_flush();
        } else {
            nw_vk_fail("vkAllocateDescriptorSets", r);
        }
    }
    return VK_NULL_HANDLE;
}

static void nw_vk_begin(void)
{
    int f = nw_vk.frame;
    vk.vkResetCommandPool(nw_vk.dev, nw_vk.pool[f], 0);
    vk.vkResetDescriptorPool(nw_vk.dev, nw_vk.dpool[f], 0);
    VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL,
                                    VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, NULL };
    vk.vkBeginCommandBuffer(nw_vk.cmd[f], &bi);
    nw_vk.recording = true;
    nw_vk.rendering = false;
    nw_vk.ring_used = 0;
    nw_vk.epoch++;
    nw_vk_invalidate();
}

static void nw_vk_lost(VkResult r, const char *what)
{
    if (!nw_vk.lost) {
        SDL_Log("Vulkan: %s failed (%d)", what, (int)r);
        nw_vk.lost = r == VK_ERROR_DEVICE_LOST;
    }
}

/* The command buffer submitted (waiting for the acquired window image, signalling done when not NULL). */
static void nw_vk_submit(VkSemaphore done)
{
    nw_vk_end_rendering();
    int f = nw_vk.frame;
    vk.vkEndCommandBuffer(nw_vk.cmd[f]);
    nw_vk.recording = false;
    VkPipelineStageFlags stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, 0, NULL, &stage, 1, &nw_vk.cmd[f], done ? 1u : 0u, &done };
    if (nw_vk.acquire_pending) {
        si.waitSemaphoreCount = 1;
        si.pWaitSemaphores = &nw_vk.acquired[f];
        nw_vk.acquire_pending = false;
    }
    VkResult r = vk.vkQueueSubmit(nw_vk.queue, 1, &si, nw_vk.fence[f]);
    if (r != VK_SUCCESS) {
        nw_vk_lost(r, "vkQueueSubmit");
    } else {
        nw_vk.submitted[f] = true;
    }
}

/* The frame's last submission done (a frame never submitted has nothing to wait for). */
static void nw_vk_wait_frame(int f)
{
    if (nw_vk.submitted[f]) {
        vk.vkWaitForFences(nw_vk.dev, 1, &nw_vk.fence[f], VK_TRUE, UINT64_MAX);
        vk.vkResetFences(nw_vk.dev, 1, &nw_vk.fence[f]);
        nw_vk.submitted[f] = false;
    }
}

/* Everything recorded runs and is waited for; the command buffer starts over. */
static void nw_vk_flush(void)
{
    nw_vk_submit(VK_NULL_HANDLE);
    nw_vk_wait_frame(nw_vk.frame);
    for (int f = 0; f < NW_VK_FRAMES; f++) {
        nw_vk_release_later(f);
    }
    nw_vk_begin();
}

/* size bytes of the frame's upload ring (or a buffer of their own when that big). */
static bool nw_vk_alloc(VkDeviceSize size, VkDeviceSize align, VkBuffer *buf, VkDeviceSize *off, uint8_t **cpu)
{
    if (size > NW_VK_RING / 2) {
        VkDeviceMemory mem = VK_NULL_HANDLE;
        void *map = NULL;
        if (!nw_vk_buffer(size,
                          VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
                              VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT,
                          true, buf, &mem, &map)) {
            return false;
        }
        nw_vk_later(NW_VK_BUFFER, (uint64_t)*buf);
        nw_vk_later(NW_VK_MEMORY, (uint64_t)mem);
        *off = 0;
        *cpu = map;
        return true;
    }
    VkDeviceSize o = nw_vk_align(nw_vk.ring_used, align);
    if (o + size > NW_VK_RING) {
        nw_vk_flush();
        o = 0;
    }
    nw_vk.ring_used = o + size;
    *buf = nw_vk.ring[nw_vk.frame];
    *off = o;
    *cpu = nw_vk.ring_map[nw_vk.frame] + o;
    return true;
}

/* ---- the driver ---- */

static bool nw_vk_has_ext(const VkExtensionProperties *e, uint32_t n, const char *name)
{
    for (uint32_t i = 0; i < n; i++) {
        if (SDL_strcmp(e[i].extensionName, name) == 0) {
            return true;
        }
    }
    return false;
}

static PFN_vkVoidFunction nw_vk_instance_proc(const char *name)
{
    return nw_vk.gipa(nw_vk.inst, name);
}

#define NW_VK_LOAD_GLOBAL(name) vk.name = (PFN_##name)nw_vk.gipa(NULL, #name);
#define NW_VK_LOAD_INSTANCE(name) vk.name = (PFN_##name)nw_vk_instance_proc(#name);
#define NW_VK_LOAD_DEVICE(name)                                                                                        \
    vk.name = (PFN_##name)vk.vkGetDeviceProcAddr(nw_vk.dev, #name);                                                    \
    if (!vk.name) {                                                                                                    \
        vk.name = (PFN_##name)nw_vk_instance_proc(#name);                                                              \
    }

/* Is there a Direct3D 12 GPU (Dozen needs one, or WARP)? */
static bool nw_vk_d3d12_gpu(void)
{
    IDXGIFactory1 *factory = NULL;
    bool found = false;
    if (FAILED(CreateDXGIFactory1(&IID_IDXGIFactory1, (void **)&factory))) {
        return false;
    }
    IDXGIAdapter1 *a = NULL;
    for (UINT i = 0; !found && IDXGIFactory1_EnumAdapters1(factory, i, &a) != DXGI_ERROR_NOT_FOUND; i++) {
        DXGI_ADAPTER_DESC1 ad;
        if (SUCCEEDED(IDXGIAdapter1_GetDesc1(a, &ad)) && !(ad.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
            found = SUCCEEDED(D3D12CreateDevice((IUnknown *)a, D3D_FEATURE_LEVEL_11_0, &IID_ID3D12Device, NULL));
        }
        IDXGIAdapter1_Release(a);
    }
    IDXGIFactory1_Release(factory);
    return found;
}

/* Dozen as an ICD: its vk_icdGetInstanceProcAddr, after the interface negotiation a loader would do. */
static bool nw_vk_load_dozen(void)
{
    if (!SDL_getenv("DZN_DEBUG") && !nw_vk_d3d12_gpu()) {
        /* both environments: Dozen's C runtime may be its own (desktop) or the shared one (UWP) */
        SetEnvironmentVariableA("DZN_DEBUG", "warp");
        SDL_setenv_unsafe("DZN_DEBUG", "warp", 1);
        _putenv("DZN_DEBUG=warp");
        SDL_Log("Vulkan: no Direct3D 12 GPU; Dozen on WARP (DZN_DEBUG=warp)");
    }
#ifdef NW_UWP
    nw_vk.lib = LoadPackagedLibrary(L"vulkan_dzn.dll", 0);
#else
    const char *path = SDL_getenv("NW_VK_ICD");
    WCHAR wpath[MAX_PATH];
    if (path && *path) {
        MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath, MAX_PATH);
    } else {
        DWORD n = GetModuleFileNameW(NULL, wpath, MAX_PATH);
        while (n && wpath[n - 1] != L'\\' && wpath[n - 1] != L'/') {
            n--;
        }
        wpath[n] = 0;
        SDL_wcslcat(wpath, L"vulkan_dzn.dll", MAX_PATH);
    }
    /* Dozen loads dxil.dll (the shader signer) at run time: from its own folder too */
    WCHAR dir[MAX_PATH];
    SDL_wcslcpy(dir, wpath, MAX_PATH);
    for (size_t n = SDL_wcslen(dir); n && dir[n - 1] != L'\\' && dir[n - 1] != L'/'; n--) {
        dir[n - 1] = 0;
    }
    SetDllDirectoryW(dir[0] ? dir : NULL);
    nw_vk.lib = LoadLibraryExW(wpath, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
#endif
    if (!nw_vk.lib) {
        return SDL_SetError("Vulkan: no driver (vulkan-1.dll has no GPU and vulkan_dzn.dll did not load: %lu)",
                            (unsigned long)GetLastError());
    }
    NwVkNegotiate negotiate = (NwVkNegotiate)(void *)GetProcAddress(nw_vk.lib, "vk_icdNegotiateLoaderICDInterfaceVersion");
    nw_vk.gipa = (PFN_vkGetInstanceProcAddr)(void *)GetProcAddress(nw_vk.lib, "vk_icdGetInstanceProcAddr");
    if (!nw_vk.gipa) {
        return SDL_SetError("Vulkan: vulkan_dzn.dll has no vk_icdGetInstanceProcAddr");
    }
    uint32_t version = 5;
    if (negotiate) {
        negotiate(&version);
    }
    nw_vk.icd = true;
    nw_vk.dozen = true;
    /* Dozen 26.2 on WARP: once its win32 swap chain exists, pipelines fail to build (VK_ERROR_OUT_OF_HOST_MEMORY)
       and presents fail (xbox/RENDERERS.md); its window goes through Direct3D 11 as on Xbox, unless
       NW_VK_PRESENT=surface */
    if (!nw_vk.surface_wanted) {
        nw_vk.copy = true;
    }
    return true;
}

/* An instance and a physical device with what this renderer needs; false (quietly) when there is none. */
static bool nw_vk_instance(void)
{
    NW_VK_GLOBAL(NW_VK_LOAD_GLOBAL)
    if (!vk.vkCreateInstance || !vk.vkEnumerateInstanceExtensionProperties) {
        return false;
    }
    PFN_vkEnumerateInstanceVersion version = (PFN_vkEnumerateInstanceVersion)nw_vk.gipa(NULL, "vkEnumerateInstanceVersion");
    uint32_t api = VK_API_VERSION_1_0;
    if (version) {
        version(&api);
    }
    nw_vk.api = api >= VK_API_VERSION_1_2 ? VK_API_VERSION_1_2 : api >= VK_API_VERSION_1_1 ? VK_API_VERSION_1_1
                                                                                            : VK_API_VERSION_1_0;
    VkExtensionProperties ext[64];
    uint32_t next = 64;
    vk.vkEnumerateInstanceExtensionProperties(NULL, &next, ext);
    const char *exts[2];
    uint32_t n = 0;
    bool want_surface = !nw_vk.copy;
    if (want_surface && nw_vk_has_ext(ext, next, VK_KHR_SURFACE_EXTENSION_NAME) &&
        nw_vk_has_ext(ext, next, VK_KHR_WIN32_SURFACE_EXTENSION_NAME)) {
        exts[n++] = VK_KHR_SURFACE_EXTENSION_NAME;
        exts[n++] = VK_KHR_WIN32_SURFACE_EXTENSION_NAME;
        nw_vk.surface_ext = true;
    }
    VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "NuWorld", 1, "NuWorld", 1, nw_vk.api };
    VkInstanceCreateInfo ci = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &app, 0, NULL, n, exts };
    VkResult r = vk.vkCreateInstance(&ci, NULL, &nw_vk.inst);
    if (r != VK_SUCCESS) {
        SDL_Log("Vulkan: vkCreateInstance failed (%d)", (int)r);
        return false;
    }
    NW_VK_INSTANCE(NW_VK_LOAD_INSTANCE)
    if (nw_vk.surface_ext) {
        NW_VK_SURFACE(NW_VK_LOAD_INSTANCE)
    }
    VkPhysicalDevice devs[8];
    uint32_t ndevs = 8;
    vk.vkEnumeratePhysicalDevices(nw_vk.inst, &ndevs, devs);
    for (uint32_t i = 0; i < ndevs && !nw_vk.phys; i++) {
        VkExtensionProperties de[256];
        uint32_t nde = 256;
        vk.vkEnumerateDeviceExtensionProperties(devs[i], NULL, &nde, de);
        VkPhysicalDeviceProperties p;
        vk.vkGetPhysicalDeviceProperties(devs[i], &p);
        if (p.apiVersion < VK_API_VERSION_1_1 || !nw_vk_has_ext(de, nde, VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME) ||
            (nw_vk.surface_ext && !nw_vk_has_ext(de, nde, VK_KHR_SWAPCHAIN_EXTENSION_NAME))) {
            SDL_Log("Vulkan: %s (Vulkan %u.%u) lacks 1.1, dynamic rendering (%d) or a swap chain (%d)", p.deviceName,
                    VK_API_VERSION_MAJOR(p.apiVersion), VK_API_VERSION_MINOR(p.apiVersion),
                    nw_vk_has_ext(de, nde, VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME),
                    nw_vk_has_ext(de, nde, VK_KHR_SWAPCHAIN_EXTENSION_NAME));
            continue;
        }
        VkQueueFamilyProperties q[16];
        uint32_t nq = 16;
        vk.vkGetPhysicalDeviceQueueFamilyProperties(devs[i], &nq, q);
        for (uint32_t k = 0; k < nq; k++) {
            if (q[k].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
                nw_vk.phys = devs[i];
                nw_vk.family = k;
                nw_vk.props = p;
                break;
            }
        }
    }
    if (!nw_vk.phys) {
        vk.vkDestroyInstance(nw_vk.inst, NULL);
        nw_vk.inst = VK_NULL_HANDLE;
        return false;
    }
    if (nw_vk.props.apiVersion < nw_vk.api) {
        nw_vk.api = VK_MAKE_API_VERSION(0, 1, VK_API_VERSION_MINOR(nw_vk.props.apiVersion), 0);
    }
    return true;
}

/* The system's loader with a GPU, else Dozen. */
static bool nw_vk_driver(void)
{
#ifndef NW_UWP
    const char *icd = SDL_getenv("NW_VK_ICD");
    if (!icd || !*icd) {
        nw_vk.lib = LoadLibraryW(L"vulkan-1.dll");
        if (nw_vk.lib) {
            nw_vk.gipa = (PFN_vkGetInstanceProcAddr)(void *)GetProcAddress(nw_vk.lib, "vkGetInstanceProcAddr");
            if (nw_vk.gipa && nw_vk_instance()) {
                return true;
            }
            FreeLibrary(nw_vk.lib);
            nw_vk.lib = NULL;
            SDL_Log("Vulkan: the system's loader has no usable GPU; Dozen");
        }
    }
#endif
    if (!nw_vk_load_dozen()) {
        return false;
    }
    return nw_vk_instance() || SDL_SetError("Vulkan: no Vulkan 1.1 device with dynamic rendering");
}

static bool nw_vk_device(void)
{
    vk.vkGetPhysicalDeviceMemoryProperties(nw_vk.phys, &nw_vk.memprops);
    VkPhysicalDeviceFeatures have, want;
    vk.vkGetPhysicalDeviceFeatures(nw_vk.phys, &have);
    SDL_zero(want);
    want.fillModeNonSolid = have.fillModeNonSolid;
    nw_vk.wireframe_ok = have.fillModeNonSolid;
    VkPhysicalDeviceDynamicRenderingFeaturesKHR dr = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES_KHR,
                                                       NULL, VK_TRUE };
    const char *exts[2] = { VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME, VK_KHR_SWAPCHAIN_EXTENSION_NAME };
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qi = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0, nw_vk.family, 1, &prio };
    VkDeviceCreateInfo ci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &dr, 0, 1, &qi, 0, NULL,
                              nw_vk.surface_ext ? 2u : 1u, exts, &want };
    VkResult r = vk.vkCreateDevice(nw_vk.phys, &ci, NULL, &nw_vk.dev);
    if (r != VK_SUCCESS) {
        return nw_vk_fail("vkCreateDevice", r);
    }
    NW_VK_DEVICE(NW_VK_LOAD_DEVICE)
    if (nw_vk.surface_ext) {
        NW_VK_SWAPCHAIN(NW_VK_LOAD_DEVICE)
    }
    if (!vk.vkCmdBeginRenderingKHR) {
        return SDL_SetError("Vulkan: the driver lacks vkCmdBeginRenderingKHR");
    }
    vk.vkGetDeviceQueue(nw_vk.dev, nw_vk.family, 0, &nw_vk.queue);
    /* D24S8 as GL's, else D32S8 */
    VkFormatProperties fp;
    vk.vkGetPhysicalDeviceFormatProperties(nw_vk.phys, VK_FORMAT_D24_UNORM_S8_UINT, &fp);
    nw_vk.depth_fmt = (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)
                          ? VK_FORMAT_D24_UNORM_S8_UINT
                          : VK_FORMAT_D32_SFLOAT_S8_UINT;
    return true;
}

/* ---- textures (images) ---- */

static int nw_vk_bpp(NwRhiFormat f)
{
    return f == NW_RHI_R8UI ? 1 : f == NW_RHI_RGBA32F ? 16 : 4;
}

static VkImageView nw_vk_view(VkImage img, VkFormat fmt, VkImageAspectFlags aspect, uint32_t level, uint32_t levels)
{
    VkImageViewCreateInfo vi = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                                 NULL,
                                 0,
                                 img,
                                 VK_IMAGE_VIEW_TYPE_2D,
                                 fmt,
                                 { VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
                                   VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY },
                                 { aspect, level, levels, 0, 1 } };
    VkImageView v = VK_NULL_HANDLE;
    VkResult r = vk.vkCreateImageView(nw_vk.dev, &vi, NULL, &v);
    if (r != VK_SUCCESS) {
        nw_vk_fail("vkCreateImageView", r);
    }
    return v;
}

static void nw_vk_texture_destroy(NwRhiTexture *t);

/* Rows of a level's rectangle (tightly packed) through the ring into the texture. */
static void nw_vk_upload(NwRhiTexture *t, int level, int x, int y, int width, int height, const void *data)
{
    if (width <= 0 || height <= 0 || !data) {
        return;
    }
    VkDeviceSize size = (VkDeviceSize)width * (VkDeviceSize)height * (VkDeviceSize)nw_vk_bpp(t->format);
    VkBuffer src = VK_NULL_HANDLE;
    VkDeviceSize off = 0;
    uint8_t *cpu = NULL;
    if (!nw_vk_alloc(size, 16, &src, &off, &cpu)) {
        return;
    }
    SDL_memcpy(cpu, data, (size_t)size);
    nw_vk_tex_layout(t, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkBufferImageCopy c = { off, 0, 0, { VK_IMAGE_ASPECT_COLOR_BIT, (uint32_t)level, 0, 1 }, { x, y, 0 },
                            { (uint32_t)width, (uint32_t)height, 1 } };
    vk.vkCmdCopyBufferToImage(nw_vk_cmd(), src, t->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
}

static NwRhiTexture *nw_vk_texture_create(NwRhiFormat format, int width, int height, int levels, int samples,
                                          unsigned flags, const void *const *data)
{
    NwRhiTexture *t = SDL_calloc(1, sizeof(*t));
    if (!t) {
        return NULL;
    }
    bool ms = samples > 1;
    *t = (NwRhiTexture){ .format = format, .w = width, .h = height, .levels = levels, .samples = ms ? samples : 1,
                         .flags = flags, .layout = VK_IMAGE_LAYOUT_UNDEFINED, .serial = ++nw_vk.serial,
                         .aspect = VK_IMAGE_ASPECT_COLOR_BIT };
    VkImageUsageFlags usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    switch (format) {
    case NW_RHI_RGBA8:
        t->fmt = VK_FORMAT_R8G8B8A8_UNORM;
        if (flags & (NW_RHI_TEX_TARGET | NW_RHI_TEX_MIPGEN)) {
            usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        }
        if ((flags & NW_RHI_TEX_MIPGEN) && !ms) {
            int n = 1;
            while ((width >> n) || (height >> n)) {
                n++;
            }
            t->levels = n;
        }
        break;
    case NW_RHI_D24S8:
        t->fmt = nw_vk.depth_fmt;
        t->aspect = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
        usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
        break;
    case NW_RHI_RGBA8UI:
        t->fmt = VK_FORMAT_R8G8B8A8_UINT;
        break;
    case NW_RHI_R8UI:
        t->fmt = VK_FORMAT_R8_UINT;
        break;
    default:
        t->fmt = VK_FORMAT_R32G32B32A32_SFLOAT;
        break;
    }
    VkImageCreateInfo ii = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                             NULL,
                             0,
                             VK_IMAGE_TYPE_2D,
                             t->fmt,
                             { (uint32_t)width, (uint32_t)height, 1 },
                             (uint32_t)t->levels,
                             1,
                             (VkSampleCountFlagBits)t->samples,
                             VK_IMAGE_TILING_OPTIMAL,
                             usage,
                             VK_SHARING_MODE_EXCLUSIVE,
                             0,
                             NULL,
                             VK_IMAGE_LAYOUT_UNDEFINED };
    VkResult r = vk.vkCreateImage(nw_vk.dev, &ii, NULL, &t->img);
    if (r != VK_SUCCESS) {
        nw_vk_fail("vkCreateImage", r);
        SDL_Log("%s (%dx%d format %d samples %d)", SDL_GetError(), width, height, (int)format, samples);
        SDL_free(t);
        return NULL;
    }
    VkMemoryRequirements req;
    vk.vkGetImageMemoryRequirements(nw_vk.dev, t->img, &req);
    if (!nw_vk_alloc_memory(req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, &t->mem)) {
        nw_vk_texture_destroy(t);
        return NULL;
    }
    vk.vkBindImageMemory(nw_vk.dev, t->img, t->mem, 0);
    t->view = nw_vk_view(t->img, t->fmt, format == NW_RHI_D24S8 ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT,
                         0, (uint32_t)t->levels);
    if (usage & (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT)) {
        t->att = nw_vk_view(t->img, t->fmt, t->aspect, 0, 1);
    }
    if (!t->view) {
        nw_vk_texture_destroy(t);
        return NULL;
    }
    if (data) {
        int n = (flags & NW_RHI_TEX_MIPGEN) ? 1 : levels;
        for (int i = 0; i < n; i++) {
            nw_vk_upload(t, i, 0, 0, SDL_max(width >> i, 1), SDL_max(height >> i, 1), data[i]);
        }
        nw_vk_tex_layout(t, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    } else if (format == NW_RHI_D24S8) {
        nw_vk_tex_layout(t, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
    } else if (usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) {
        nw_vk_tex_layout(t, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    } else {
        nw_vk_tex_layout(t, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
    return t;
}

static void nw_vk_texture_update(NwRhiTexture *t, int level, int x, int y, int width, int height, const void *data)
{
    nw_vk_upload(t, level, x, y, width, height, data);
}

static void nw_vk_texture_destroy(NwRhiTexture *t)
{
    if (!t) {
        return;
    }
    for (int i = 0; i < NW_VK_UNITS; i++) {
        if (nw_vk.tex[i] == t) {
            nw_vk.tex[i] = NULL;
        }
    }
    if (nw_vk.col == t || nw_vk.ds == t) {
        nw_vk.col = nw_vk.col == t ? NULL : nw_vk.col;
        nw_vk.ds = nw_vk.ds == t ? NULL : nw_vk.ds;
    }
    if (nw_vk.rendering && (t->att == nw_vk.r_col || t->att == nw_vk.r_ds)) {
        nw_vk_end_rendering();
    }
    nw_vk_later(NW_VK_UI, t->ui);
    nw_vk_later(NW_VK_VIEW, (uint64_t)t->att);
    nw_vk_later(NW_VK_VIEW, (uint64_t)t->view);
    nw_vk_later(NW_VK_IMAGE, (uint64_t)t->img);
    nw_vk_later(NW_VK_MEMORY, (uint64_t)t->mem);
    SDL_free(t);
}

static void nw_vk_texture_size(const NwRhiTexture *t, int *width, int *height)
{
    *width = t ? t->w : 0;
    *height = t ? t->h : 0;
}

static uint64_t nw_vk_texture_ui(NwRhiTexture *t)
{
    if (!t || !nw_vk.ui_add) {
        return 0;
    }
    nw_vk_tex_layout(t, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    if (!t->ui) {
        t->ui = nw_vk.ui_add((uint64_t)t->view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
    return t->ui;
}

/* ---- the window ---- */

static bool nw_vk_acquire(void);

static void nw_vk_swap_destroy_views(void)
{
    for (uint32_t i = 0; i < nw_vk.nimages; i++) {
        vk.vkDestroyImageView(nw_vk.dev, nw_vk.views[i], NULL);
        vk.vkDestroySemaphore(nw_vk.dev, nw_vk.done[i], NULL);
        nw_vk.views[i] = VK_NULL_HANDLE;
        nw_vk.done[i] = VK_NULL_HANDLE;
    }
    nw_vk.nimages = 0;
}

/* The surface's swap chain at the window's size (all GPU work done). */
static bool nw_vk_swapchain(void)
{
    VkSurfaceCapabilitiesKHR sc;
    vk.vkGetPhysicalDeviceSurfaceCapabilitiesKHR(nw_vk.phys, nw_vk.surface, &sc);
    int w = 0, h = 0;
    SDL_GetWindowSizeInPixels(nw_vk.window, &w, &h);
    VkExtent2D ext = sc.currentExtent;
    if (ext.width == 0xFFFFFFFFu) {
        ext.width = (uint32_t)SDL_max(w, 1);
        ext.height = (uint32_t)SDL_max(h, 1);
    }
    if (!ext.width || !ext.height) {
        return false; /* minimised */
    }
    VkSurfaceFormatKHR fmts[32];
    uint32_t nf = 32;
    vk.vkGetPhysicalDeviceSurfaceFormatsKHR(nw_vk.phys, nw_vk.surface, &nf, fmts);
    VkSurfaceFormatKHR fmt = nf ? fmts[0] : (VkSurfaceFormatKHR){ VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR };
    for (uint32_t i = 0; i < nf; i++) {
        if (fmts[i].format == VK_FORMAT_R8G8B8A8_UNORM ||
            (fmts[i].format == VK_FORMAT_B8G8R8A8_UNORM && fmt.format != VK_FORMAT_R8G8B8A8_UNORM)) {
            fmt = fmts[i];
        }
    }
    VkPresentModeKHR modes[8];
    uint32_t nm = 8;
    vk.vkGetPhysicalDeviceSurfacePresentModesKHR(nw_vk.phys, nw_vk.surface, &nm, modes);
    VkPresentModeKHR mode = VK_PRESENT_MODE_FIFO_KHR;
    for (uint32_t i = 0; i < nm && !nw_vk.interval; i++) {
        if (modes[i] == VK_PRESENT_MODE_IMMEDIATE_KHR ||
            (modes[i] == VK_PRESENT_MODE_MAILBOX_KHR && mode == VK_PRESENT_MODE_FIFO_KHR)) {
            mode = modes[i];
        }
    }
    uint32_t count = SDL_max(sc.minImageCount, 2u);
    if (sc.maxImageCount && count > sc.maxImageCount) {
        count = sc.maxImageCount;
    }
    VkImageUsageFlags usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                              (sc.supportedUsageFlags & (VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT));
    VkSwapchainKHR old = nw_vk.swap;
    VkSwapchainCreateInfoKHR ci = { VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
                                    NULL,
                                    0,
                                    nw_vk.surface,
                                    count,
                                    fmt.format,
                                    fmt.colorSpace,
                                    ext,
                                    1,
                                    usage,
                                    VK_SHARING_MODE_EXCLUSIVE,
                                    0,
                                    NULL,
                                    VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR,
                                    VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
                                    mode,
                                    VK_TRUE,
                                    old };
    if (!(sc.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR)) {
        ci.preTransform = sc.currentTransform;
    }
    if (!(sc.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR)) {
        ci.compositeAlpha = VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
    }
    VkResult r = vk.vkCreateSwapchainKHR(nw_vk.dev, &ci, NULL, &nw_vk.swap);
    nw_vk_swap_destroy_views();
    if (old) {
        vk.vkDestroySwapchainKHR(nw_vk.dev, old, NULL);
    }
    if (r != VK_SUCCESS) {
        nw_vk.swap = VK_NULL_HANDLE;
        return nw_vk_fail("vkCreateSwapchainKHR", r);
    }
    nw_vk.nimages = NW_VK_IMAGES;
    vk.vkGetSwapchainImagesKHR(nw_vk.dev, nw_vk.swap, &nw_vk.nimages, nw_vk.images);
    VkSemaphoreCreateInfo si = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, NULL, 0 };
    for (uint32_t i = 0; i < nw_vk.nimages; i++) {
        nw_vk.views[i] = nw_vk_view(nw_vk.images[i], fmt.format, VK_IMAGE_ASPECT_COLOR_BIT, 0, 1);
        nw_vk.layouts[i] = VK_IMAGE_LAYOUT_UNDEFINED;
        vk.vkCreateSemaphore(nw_vk.dev, &si, NULL, &nw_vk.done[i]);
    }
    nw_vk.win_fmt = fmt.format;
    SDL_Log("Vulkan: swap chain %ux%u, format %d, %u images, present mode %d", ext.width, ext.height, (int)fmt.format,
            nw_vk.nimages, (int)mode);
    nw_vk.win_w = (int)ext.width;
    nw_vk.win_h = (int)ext.height;
    nw_vk.have_image = false;
    nw_vk.swap_dirty = false;
    return true;
}

static bool nw_vk_acquire(void)
{
    if (nw_vk.copy || !nw_vk.swap || nw_vk.have_image) {
        return true;
    }
    for (int tries = 0; tries < 2; tries++) {
        VkResult r = vk.vkAcquireNextImageKHR(nw_vk.dev, nw_vk.swap, UINT64_MAX, nw_vk.acquired[nw_vk.frame],
                                              VK_NULL_HANDLE, &nw_vk.image);
        if (r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR) {
            nw_vk.have_image = true;
            nw_vk.acquire_pending = true;
            return true;
        }
        if (r != VK_ERROR_OUT_OF_DATE_KHR) {
            return nw_vk_fail("vkAcquireNextImageKHR", r);
        }
        vk.vkDeviceWaitIdle(nw_vk.dev);
        if (!nw_vk_swapchain()) {
            return false;
        }
    }
    return false;
}

/* Copy mode: Direct3D 11 shows the frames (a CoreWindow or HWND swap chain, as rhi_d3d11.c). */
static bool nw_vk_copy_init(void)
{
    static const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };
    HRESULT hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, levels, 2, D3D11_SDK_VERSION, &nw_vk.d3d,
                                   NULL, &nw_vk.d3dctx);
    if (FAILED(hr)) {
        hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_WARP, NULL, 0, levels, 2, D3D11_SDK_VERSION, &nw_vk.d3d, NULL,
                               &nw_vk.d3dctx);
    }
    if (FAILED(hr)) {
        return SDL_SetError("Vulkan: the presenting Direct3D 11 device failed (0x%08lx)", (unsigned long)hr);
    }
    IDXGIDevice *dxgi = NULL;
    IDXGIAdapter *adapter = NULL;
    IDXGIFactory2 *factory = NULL;
    hr = ID3D11Device_QueryInterface(nw_vk.d3d, &IID_IDXGIDevice, (void **)&dxgi);
    if (SUCCEEDED(hr)) {
        hr = IDXGIDevice_GetAdapter(dxgi, &adapter);
    }
    if (SUCCEEDED(hr)) {
        hr = IDXGIAdapter_GetParent(adapter, &IID_IDXGIFactory2, (void **)&factory);
    }
    int w = 0, h = 0;
    SDL_GetWindowSizeInPixels(nw_vk.window, &w, &h);
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
        SDL_PropertiesID props = SDL_GetWindowProperties(nw_vk.window);
#ifdef NW_UWP
        IUnknown *core = SDL_GetPointerProperty(props, "SDL.window.winrt.window", NULL);
        sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
        hr = core ? IDXGIFactory2_CreateSwapChainForCoreWindow(factory, (IUnknown *)nw_vk.d3d, core, &sd, NULL,
                                                               &nw_vk.d3dswap)
                  : E_POINTER;
#else
        HWND hwnd = SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WIN32_HWND_POINTER, NULL);
        sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        hr = hwnd ? IDXGIFactory2_CreateSwapChainForHwnd(factory, (IUnknown *)nw_vk.d3d, hwnd, &sd, NULL, NULL,
                                                         &nw_vk.d3dswap)
                  : E_POINTER;
#endif
    }
    if (factory) {
        IDXGIFactory2_Release(factory);
    }
    if (adapter) {
        IDXGIAdapter_Release(adapter);
    }
    if (dxgi) {
        IDXGIDevice_Release(dxgi);
    }
    if (SUCCEEDED(hr)) {
        hr = IDXGISwapChain1_GetBuffer(nw_vk.d3dswap, 0, &IID_ID3D11Texture2D, (void **)&nw_vk.d3dback);
    }
    if (FAILED(hr)) {
        return SDL_SetError("Vulkan: the presenting swap chain failed (0x%08lx)", (unsigned long)hr);
    }
    nw_vk.win_fmt = VK_FORMAT_R8G8B8A8_UNORM;
    return true;
}

/* Copy mode: the window's image and its readback buffer at w x h. */
static bool nw_vk_copy_size(int w, int h)
{
    if (nw_vk.win_tex) {
        nw_vk_texture_destroy(nw_vk.win_tex);
        nw_vk.win_tex = NULL;
    }
    if (nw_vk.win_rb) {
        nw_vk_later(NW_VK_BUFFER, (uint64_t)nw_vk.win_rb);
        nw_vk_later(NW_VK_MEMORY, (uint64_t)nw_vk.win_rb_mem);
        nw_vk.win_rb = VK_NULL_HANDLE;
        nw_vk.win_rb_mem = VK_NULL_HANDLE;
    }
    nw_vk.win_tex = nw_vk_texture_create(NW_RHI_RGBA8, w, h, 1, 1, NW_RHI_TEX_TARGET, NULL);
    void *map = NULL;
    if (!nw_vk.win_tex || !nw_vk_buffer((VkDeviceSize)w * (VkDeviceSize)h * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true,
                                        &nw_vk.win_rb, &nw_vk.win_rb_mem, &map)) {
        return false;
    }
    nw_vk.win_rb_map = map;
    nw_vk.win_w = w;
    nw_vk.win_h = h;
    if (nw_vk.d3dback) {
        ID3D11Texture2D_Release(nw_vk.d3dback);
        nw_vk.d3dback = NULL;
        HRESULT hr = IDXGISwapChain1_ResizeBuffers(nw_vk.d3dswap, 0, (UINT)w, (UINT)h, DXGI_FORMAT_UNKNOWN, 0);
        if (FAILED(hr)) {
            SDL_Log("Vulkan: ResizeBuffers %dx%d failed (0x%08lx)", w, h, (unsigned long)hr);
        }
        IDXGISwapChain1_GetBuffer(nw_vk.d3dswap, 0, &IID_ID3D11Texture2D, (void **)&nw_vk.d3dback);
    }
    return true;
}

/* The window at its size: a new swap chain (or copy image) when it changed. */
static void nw_vk_fit_window(void)
{
    int w = 0, h = 0;
    SDL_GetWindowSizeInPixels(nw_vk.window, &w, &h);
    if (w <= 0 || h <= 0 || (w == nw_vk.win_w && h == nw_vk.win_h && !nw_vk.swap_dirty)) {
        return;
    }
    if (nw_vk.copy) {
        nw_vk_copy_size(w, h);
        return;
    }
    nw_vk_flush();
    vk.vkDeviceWaitIdle(nw_vk.dev);
    if (nw_vk_swapchain()) {
        nw_vk_acquire();
    }
}

/* The window as a rendering target: its image (acquired), view, layout and size. */
static bool nw_vk_window_target(NwVkTarget *t)
{
    SDL_zerop(t);
    t->samples = 1;
    if (nw_vk.copy) {
        if (!nw_vk.win_tex) {
            return false;
        }
        t->col_img = nw_vk.win_tex->img;
        t->col = nw_vk.win_tex->att;
        t->col_layout = &nw_vk.win_tex->layout;
        t->col_fmt = VK_FORMAT_R8G8B8A8_UNORM;
    } else {
        if (!nw_vk_acquire() || !nw_vk.have_image) {
            return false;
        }
        t->col_img = nw_vk.images[nw_vk.image];
        t->col = nw_vk.views[nw_vk.image];
        t->col_layout = &nw_vk.layouts[nw_vk.image];
        t->col_fmt = nw_vk.win_fmt;
    }
    t->w = nw_vk.win_w;
    t->h = nw_vk.win_h;
    return true;
}

/* ---- device, start and end ---- */

static SDL_WindowFlags nw_vk_prepare_window(void)
{
    return 0;
}

static VkShaderModule nw_vk_module(const NwVkShader *s)
{
    VkShaderModuleCreateInfo ci = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, s->size, s->code };
    VkShaderModule m = VK_NULL_HANDLE;
    VkResult r = vk.vkCreateShaderModule(nw_vk.dev, &ci, NULL, &m);
    if (r != VK_SUCCESS) {
        nw_vk_fail("vkCreateShaderModule", r);
    }
    return m;
}

static bool nw_vk_layouts(void)
{
    /* immutable samplers: the programs only texelFetch, and a driver on Direct3D 12 makes them static samplers */
    VkDescriptorSetLayoutBinding b[NW_VK_UNITS + 2];
    for (int i = 0; i < NW_VK_UNITS; i++) {
        bool texels = nw_vk_units[i] == 2;
        b[i] = (VkDescriptorSetLayoutBinding){ (uint32_t)i,
                                               texels ? VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER
                                                      : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                               1, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                                               texels ? NULL : &nw_vk.point };
    }
    b[NW_VK_UNITS] = (VkDescriptorSetLayoutBinding){ 16, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1,
                                                     VK_SHADER_STAGE_VERTEX_BIT, NULL };
    b[NW_VK_UNITS + 1] = (VkDescriptorSetLayoutBinding){ 17, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1,
                                                         VK_SHADER_STAGE_FRAGMENT_BIT, NULL };
    VkDescriptorSetLayoutCreateInfo li = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, NULL, 0,
                                           NW_VK_UNITS + 2, b };
    VkResult r = vk.vkCreateDescriptorSetLayout(nw_vk.dev, &li, NULL, &nw_vk.set_layout);
    VkDescriptorSetLayoutBinding u[5] = {
        { 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, NULL },
        { 1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL },
        { 2, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL },
        { 3, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL },
        { 4, VK_DESCRIPTOR_TYPE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL },
    };
    li.bindingCount = 5;
    li.pBindings = u;
    if (r == VK_SUCCESS) {
        r = vk.vkCreateDescriptorSetLayout(nw_vk.dev, &li, NULL, &nw_vk.util_set_layout);
    }
    VkPipelineLayoutCreateInfo pi = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, NULL, 0, 1, &nw_vk.set_layout, 0,
                                      NULL };
    if (r == VK_SUCCESS) {
        r = vk.vkCreatePipelineLayout(nw_vk.dev, &pi, NULL, &nw_vk.layout);
    }
    pi.pSetLayouts = &nw_vk.util_set_layout;
    if (r == VK_SUCCESS) {
        r = vk.vkCreatePipelineLayout(nw_vk.dev, &pi, NULL, &nw_vk.util_layout);
    }
    return r == VK_SUCCESS || nw_vk_fail("the pipeline layouts", r);
}

static bool nw_vk_init(SDL_Window *window)
{
    SDL_zero(vk);
    SDL_zero(nw_vk);
    nw_vk.window = window;
    nw_vk.interval = 1;
#ifdef NW_UWP
    nw_vk.copy = true; /* Dozen has no surface for a CoreWindow */
#else
    const char *present = SDL_getenv("NW_VK_PRESENT");
    nw_vk.copy = present && SDL_strcasecmp(present, "copy") == 0;
    nw_vk.surface_wanted = present && SDL_strcasecmp(present, "surface") == 0;
#endif
    if (!nw_vk_driver() || !nw_vk_device()) {
        return false;
    }
    if (!nw_vk.copy && !nw_vk.surface_ext) {
        SDL_Log("Vulkan: no VK_KHR_win32_surface; the window is copied through Direct3D 11");
        nw_vk.copy = true;
    }
    VkCommandPoolCreateInfo pc = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL,
                                   VK_COMMAND_POOL_CREATE_TRANSIENT_BIT, nw_vk.family };
    VkFenceCreateInfo fc = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, NULL, 0 };
    VkSemaphoreCreateInfo sc = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, NULL, 0 };
    for (int f = 0; f < NW_VK_FRAMES; f++) {
        VkResult r = vk.vkCreateCommandPool(nw_vk.dev, &pc, NULL, &nw_vk.pool[f]);
        VkCommandBufferAllocateInfo ai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, nw_vk.pool[f],
                                           VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1 };
        if (r == VK_SUCCESS) {
            r = vk.vkAllocateCommandBuffers(nw_vk.dev, &ai, &nw_vk.cmd[f]);
        }
        if (r == VK_SUCCESS) {
            r = vk.vkCreateFence(nw_vk.dev, &fc, NULL, &nw_vk.fence[f]);
        }
        if (r == VK_SUCCESS) {
            r = vk.vkCreateSemaphore(nw_vk.dev, &sc, NULL, &nw_vk.acquired[f]);
        }
        static const VkDescriptorPoolSize sizes[] = {
            { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, NW_VK_SETS * 6 },
            { VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, NW_VK_SETS * 3 },
            { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, NW_VK_SETS * 2 },
            { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, NW_VK_SETS / 4 * 3 },
            { VK_DESCRIPTOR_TYPE_SAMPLER, NW_VK_SETS / 4 },
        };
        VkDescriptorPoolCreateInfo dp = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, NULL, 0, NW_VK_SETS,
                                          SDL_arraysize(sizes), sizes };
        if (r == VK_SUCCESS) {
            r = vk.vkCreateDescriptorPool(nw_vk.dev, &dp, NULL, &nw_vk.dpool[f]);
        }
        void *map = NULL;
        if (r != VK_SUCCESS) {
            return nw_vk_fail("the command buffers", r);
        }
        if (!nw_vk_buffer(NW_VK_RING,
                          VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
                              VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT |
                              VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT,
                          true, &nw_vk.ring[f], &nw_vk.ring_mem[f], &map)) {
            return false;
        }
        nw_vk.ring_map[f] = map;
    }
    nw_vk.epoch = 1;
    nw_vk_begin();
    /* the window */
    if (nw_vk.copy) {
        int w = 0, h = 0;
        SDL_GetWindowSizeInPixels(window, &w, &h);
        if (!nw_vk_copy_init() || !nw_vk_copy_size(SDL_max(w, 1), SDL_max(h, 1))) {
            return false;
        }
    } else {
        SDL_PropertiesID props = SDL_GetWindowProperties(window);
        HWND hwnd = SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WIN32_HWND_POINTER, NULL);
        VkWin32SurfaceCreateInfoKHR si = { VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR, NULL, 0,
                                           GetModuleHandleW(NULL), hwnd };
        VkResult r = vk.vkCreateWin32SurfaceKHR(nw_vk.inst, &si, NULL, &nw_vk.surface);
        VkBool32 ok = VK_FALSE;
        if (r == VK_SUCCESS) {
            vk.vkGetPhysicalDeviceSurfaceSupportKHR(nw_vk.phys, nw_vk.family, nw_vk.surface, &ok);
        }
        if (r != VK_SUCCESS || !ok) {
            return SDL_SetError("Vulkan: the window's surface failed (%d, supported %d)", (int)r, (int)ok);
        }
        if (!nw_vk_swapchain() || !nw_vk_acquire()) {
            return false;
        }
    }
    /* samplers, layouts, util shaders, the dummies */
    VkSamplerCreateInfo smp = { .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    smp.magFilter = smp.minFilter = VK_FILTER_NEAREST;
    smp.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    smp.addressModeU = smp.addressModeV = smp.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    smp.maxLod = VK_LOD_CLAMP_NONE;
    vk.vkCreateSampler(nw_vk.dev, &smp, NULL, &nw_vk.point);
    smp.magFilter = smp.minFilter = VK_FILTER_LINEAR;
    smp.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    vk.vkCreateSampler(nw_vk.dev, &smp, NULL, &nw_vk.linear);
    if (!nw_vk_layouts()) {
        return false;
    }
    for (int i = 0; i < NW_D3D_UTILS; i++) {
        if (!(nw_vk.util_mod[i] = nw_vk_module(&nw_vk_util[i]))) {
            return false;
        }
    }
    nw_vk.psos = SDL_calloc(NW_VK_PSOS, sizeof(*nw_vk.psos));
    if (!nw_vk.psos) {
        return SDL_OutOfMemory();
    }
    const VkPhysicalDeviceLimits *lim = &nw_vk.props.limits;
    nw_vk.caps.max_texture = (int)lim->maxImageDimension2D;
    VkSampleCountFlags counts = lim->framebufferColorSampleCounts & lim->framebufferDepthSampleCounts &
                                lim->framebufferStencilSampleCounts;
    for (int n = 2; n <= 8; n *= 2) {
        if (counts & (VkSampleCountFlags)n) {
            nw_vk.caps.max_samples = n;
        }
    }
    static const uint8_t white[4] = { 0, 0, 0, 255 };
    const void *level0 = white;
    nw_vk.dummy_tex = nw_vk_texture_create(NW_RHI_RGBA8, 1, 1, 1, 1, 0, &level0);
    nw_vk.dummy_ms = nw_vk_texture_create(NW_RHI_RGBA8, 1, 1, 1, nw_vk.caps.max_samples >= 2 ? 2 : 1,
                                          NW_RHI_TEX_TARGET, NULL);
    void *map = NULL;
    if (!nw_vk.dummy_tex || !nw_vk.dummy_ms ||
        !nw_vk_buffer(256,
                      VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT |
                          VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                      true,
                      &nw_vk.dummy_buf, &nw_vk.dummy_mem, &map)) {
        return false;
    }
    static const uint32_t one = 0x3F800000u;
    const uint32_t dummy[12] = { 0, 0, 0, one, 0, 0, 0, 1, 0, 0, 0, 1 };
    SDL_memcpy(map, dummy, sizeof(dummy));
    VkBufferViewCreateInfo bv = { VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO, NULL, 0, nw_vk.dummy_buf,
                                  VK_FORMAT_R32G32B32A32_SFLOAT, 0, 48 };
    vk.vkCreateBufferView(nw_vk.dev, &bv, NULL, &nw_vk.dummy_view);
    nw_vk_tex_layout(nw_vk.dummy_ms, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    nw_vk.mask = 0xF;
    nw_vk.depth_write = true;
    nw_vk.depth_func = NW_RHI_LESS;
    nw_vk.stencil_read = nw_vk.stencil_write = 0xFF;
    uint32_t v = nw_vk.props.apiVersion;
    SDL_snprintf(nw_vk.description, sizeof(nw_vk.description), "Vulkan %u.%u (%s%s)%s", VK_API_VERSION_MAJOR(v),
                 VK_API_VERSION_MINOR(v), nw_vk.props.deviceName, nw_vk.dozen ? ", Mesa Dozen" : "",
                 nw_vk.copy ? ", shown through Direct3D 11" : "");
    SDL_Log("%s", nw_vk.description);
    return true;
}

static void nw_vk_shutdown(void)
{
    if (nw_vk.dev) {
        if (nw_vk.recording) {
            nw_vk_submit(VK_NULL_HANDLE);
        }
        vk.vkDeviceWaitIdle(nw_vk.dev);
        for (int f = 0; f < NW_VK_FRAMES; f++) {
            nw_vk_release_later(f);
        }
        if (nw_vk.win_tex) {
            nw_vk_texture_destroy(nw_vk.win_tex);
        }
        nw_vk_texture_destroy(nw_vk.dummy_tex);
        nw_vk_texture_destroy(nw_vk.dummy_ms);
        nw_vk_later(NW_VK_BUFFER_VIEW, (uint64_t)nw_vk.dummy_view);
        nw_vk_later(NW_VK_BUFFER, (uint64_t)nw_vk.dummy_buf);
        nw_vk_later(NW_VK_MEMORY, (uint64_t)nw_vk.dummy_mem);
        nw_vk_later(NW_VK_BUFFER, (uint64_t)nw_vk.win_rb);
        nw_vk_later(NW_VK_MEMORY, (uint64_t)nw_vk.win_rb_mem);
        for (int f = 0; f < NW_VK_FRAMES; f++) {
            nw_vk_release_later(f);
            SDL_free(nw_vk.later[f]);
            vk.vkDestroyBuffer(nw_vk.dev, nw_vk.ring[f], NULL);
            vk.vkFreeMemory(nw_vk.dev, nw_vk.ring_mem[f], NULL);
            vk.vkDestroyCommandPool(nw_vk.dev, nw_vk.pool[f], NULL);
            vk.vkDestroyDescriptorPool(nw_vk.dev, nw_vk.dpool[f], NULL);
            vk.vkDestroyFence(nw_vk.dev, nw_vk.fence[f], NULL);
            vk.vkDestroySemaphore(nw_vk.dev, nw_vk.acquired[f], NULL);
        }
        for (int i = 0; nw_vk.psos && i < NW_VK_PSOS; i++) {
            if (nw_vk.psos[i].pipe && nw_vk.psos[i].pipe != NW_VK_TOMB) {
                vk.vkDestroyPipeline(nw_vk.dev, nw_vk.psos[i].pipe, NULL);
            }
        }
        for (int i = 0; i < NW_D3D_UTILS; i++) {
            vk.vkDestroyShaderModule(nw_vk.dev, nw_vk.util_mod[i], NULL);
        }
        vk.vkDestroyPipelineLayout(nw_vk.dev, nw_vk.layout, NULL);
        vk.vkDestroyPipelineLayout(nw_vk.dev, nw_vk.util_layout, NULL);
        vk.vkDestroyDescriptorSetLayout(nw_vk.dev, nw_vk.set_layout, NULL);
        vk.vkDestroyDescriptorSetLayout(nw_vk.dev, nw_vk.util_set_layout, NULL);
        vk.vkDestroySampler(nw_vk.dev, nw_vk.point, NULL);
        vk.vkDestroySampler(nw_vk.dev, nw_vk.linear, NULL);
        if (nw_vk.swap) {
            nw_vk_swap_destroy_views();
            vk.vkDestroySwapchainKHR(nw_vk.dev, nw_vk.swap, NULL);
        }
        vk.vkDestroyDevice(nw_vk.dev, NULL);
    }
    SDL_free(nw_vk.psos);
    if (nw_vk.surface) {
        vk.vkDestroySurfaceKHR(nw_vk.inst, nw_vk.surface, NULL);
    }
    if (nw_vk.inst) {
        vk.vkDestroyInstance(nw_vk.inst, NULL);
    }
    if (nw_vk.d3dback) {
        ID3D11Texture2D_Release(nw_vk.d3dback);
    }
    if (nw_vk.d3dswap) {
        IDXGISwapChain1_Release(nw_vk.d3dswap);
    }
    if (nw_vk.d3dctx) {
        ID3D11DeviceContext_Release(nw_vk.d3dctx);
    }
    if (nw_vk.d3d) {
        ID3D11Device_Release(nw_vk.d3d);
    }
    /* the driver stays loaded: unloading an ICD under a live process is asking for trouble */
    SDL_zero(nw_vk);
}

static void nw_vk_begin_rendering(const NwVkTarget *t);

static void *nw_vk_native(int which)
{
    switch (which) {
    case 0:
        return nw_vk.inst;
    case 1:
        return nw_vk.phys;
    case 2:
        return nw_vk.dev;
    case 3:
        return nw_vk.queue;
    case 4:
        return (void *)(uintptr_t)nw_vk.family;
    case 5: {
        NwVkTarget t;
        if (!nw_vk_window_target(&t)) {
            return NULL;
        }
        nw_vk_layout(t.col_img, VK_IMAGE_ASPECT_COLOR_BIT, t.col_layout, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        if (!nw_vk.rendering || nw_vk.r_col != t.col || nw_vk.r_ds) {
            nw_vk_end_rendering();
            nw_vk_begin_rendering(&t);
        }
        nw_vk_invalidate(); /* Dear ImGui binds its own pipeline and descriptor sets */
        return nw_vk_cmd();
    }
    case 6:
        return (void *)(uintptr_t)nw_vk.win_fmt;
    case 7:
        return (void *)(uintptr_t)(nw_vk.copy ? 2 : SDL_max(nw_vk.nimages, 2u));
    default:
        return NULL;
    }
}

uint32_t nw_rhi_vk_api_version(void)
{
    return nw_vk.api;
}

void (*nw_rhi_vk_proc(const char *name))(void)
{
    PFN_vkVoidFunction f = NULL;
    if (nw_vk.dev && vk.vkGetDeviceProcAddr) {
        f = vk.vkGetDeviceProcAddr(nw_vk.dev, name);
    }
    if (!f && nw_vk.gipa) {
        f = nw_vk.gipa(nw_vk.inst, name);
    }
    return f;
}

void nw_rhi_vk_ui_textures(uint64_t (*add)(uint64_t view, int layout), void (*remove)(uint64_t set))
{
    nw_vk.ui_add = add;
    nw_vk.ui_remove = remove;
}

void nw_rhi_vk_idle(void)
{
    if (nw_vk.dev && nw_vk.recording) {
        nw_vk_flush();
    }
}

static const char *nw_vk_description(void)
{
    return nw_vk.description;
}

static const NwRhiCaps *nw_vk_caps(void)
{
    return &nw_vk.caps;
}

static bool nw_vk_set_vsync(int interval)
{
    if (interval < 0) {
        return SDL_SetError("Vulkan: no adaptive vsync");
    }
    if (interval != nw_vk.interval) {
        nw_vk.interval = interval;
        nw_vk.swap_dirty = !nw_vk.copy;
    }
    return true;
}

static int nw_vk_vsync(void)
{
    return nw_vk.interval;
}

/* Copy mode: the window's image to the CPU and into the Direct3D 11 swap chain. */
static void nw_vk_copy_present(void)
{
    NwRhiTexture *t = nw_vk.win_tex;
    if (t && nw_vk.win_rb) {
        nw_vk_tex_layout(t, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        VkBufferImageCopy c = { 0, 0, 0, { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, { 0, 0, 0 },
                                { (uint32_t)t->w, (uint32_t)t->h, 1 } };
        vk.vkCmdCopyImageToBuffer(nw_vk_cmd(), t->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, nw_vk.win_rb, 1, &c);
    }
    nw_vk_flush();
    if (t && nw_vk.d3dback) {
        ID3D11DeviceContext_UpdateSubresource(nw_vk.d3dctx, (ID3D11Resource *)nw_vk.d3dback, 0, NULL, nw_vk.win_rb_map,
                                              (UINT)t->w * 4, 0);
    }
    HRESULT hr = IDXGISwapChain1_Present(nw_vk.d3dswap, nw_vk.interval ? 1 : 0, 0);
    if (FAILED(hr)) {
        static bool told;
        if (!told) {
            SDL_Log("Vulkan: Present (Direct3D 11) failed (0x%08lx)", (unsigned long)hr);
            told = true;
        }
    }
}

static void nw_vk_present(void)
{
    if (nw_vk.lost) {
        return;
    }
    if (nw_vk.copy) {
        nw_vk_copy_present();
        return;
    }
    bool show = nw_vk.have_image;
    if (show) {
        nw_vk_layout(nw_vk.images[nw_vk.image], VK_IMAGE_ASPECT_COLOR_BIT, &nw_vk.layouts[nw_vk.image],
                     VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    }
    nw_vk_submit(show ? nw_vk.done[nw_vk.image] : VK_NULL_HANDLE);
    if (show) {
        VkPresentInfoKHR pi = { VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, NULL, 1, &nw_vk.done[nw_vk.image], 1, &nw_vk.swap,
                                &nw_vk.image, NULL };
        VkResult r = vk.vkQueuePresentKHR(nw_vk.queue, &pi);
        if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) {
            nw_vk.swap_dirty = true;
        } else if (r != VK_SUCCESS) {
            nw_vk_lost(r, "vkQueuePresentKHR");
        }
        nw_vk.have_image = false;
    }
    nw_vk.frame = (nw_vk.frame + 1) % NW_VK_FRAMES;
    nw_vk_wait_frame(nw_vk.frame);
    nw_vk_release_later(nw_vk.frame);
    nw_vk_begin();
    if (nw_vk.swap_dirty) {
        nw_vk.win_w = 0; /* a new swap chain at the next nw_vk_fit_window */
    }
    nw_vk_acquire();
}

/* ---- buffers ---- */

static NwRhiBuffer *nw_vk_buffer_create(NwRhiBufferKind kind, NwRhiFormat texels)
{
    NwRhiBuffer *b = SDL_calloc(1, sizeof(*b));
    if (b) {
        b->kind = kind;
        b->texels = texels;
    }
    return b;
}

/* A texel buffer's view over buf from off. */
static VkBufferView nw_vk_texel_view(NwRhiBuffer *b)
{
    bool r32 = b->texels == NW_RHI_R32F;
    VkDeviceSize elem = r32 ? 4 : 16;
    VkDeviceSize range = SDL_max((VkDeviceSize)b->size / elem, (VkDeviceSize)1) * elem;
    VkDeviceSize most = (VkDeviceSize)nw_vk.props.limits.maxTexelBufferElements * elem;
    VkBufferViewCreateInfo vi = { VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO, NULL, 0, b->buf,
                                  r32 ? VK_FORMAT_R32_SFLOAT : VK_FORMAT_R32G32B32A32_SFLOAT, b->off,
                                  SDL_min(range, most) };
    VkBufferView v = VK_NULL_HANDLE;
    vk.vkCreateBufferView(nw_vk.dev, &vi, NULL, &v);
    nw_vk_later(NW_VK_BUFFER_VIEW, (uint64_t)v); /* released after the GPU; placed again when needed */
    return v;
}

static bool nw_vk_buffer_ready(NwRhiBuffer *b)
{
    if (!b || !b->size) {
        return false;
    }
    if (!b->stream) {
        return b->own != VK_NULL_HANDLE;
    }
    if (b->buf && b->epoch == nw_vk.epoch) {
        return true;
    }
    uint8_t *cpu = NULL;
    VkDeviceSize align = SDL_max(nw_vk.props.limits.minTexelBufferOffsetAlignment, (VkDeviceSize)256);
    if (!nw_vk_alloc(b->size, align, &b->buf, &b->off, &cpu)) {
        b->buf = VK_NULL_HANDLE;
        return false;
    }
    SDL_memcpy(cpu, b->shadow, b->size);
    b->epoch = nw_vk.epoch;
    b->serial = ++nw_vk.serial;
    b->view = b->kind == NW_RHI_TEXELS ? nw_vk_texel_view(b) : VK_NULL_HANDLE;
    return true;
}

static void nw_vk_buffer_data(NwRhiBuffer *b, size_t size, const void *data, bool stream)
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
        b->buf = VK_NULL_HANDLE;
        return;
    }
    VkBuffer own = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    static const VkBufferUsageFlags usage[] = { VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                                                VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT };
    if (!nw_vk_buffer(nw_vk_align(size, 256), usage[b->kind] | VK_BUFFER_USAGE_TRANSFER_DST_BIT, false, &own, &mem,
                      NULL)) {
        return;
    }
    VkBuffer src = VK_NULL_HANDLE;
    VkDeviceSize off = 0;
    uint8_t *cpu = NULL;
    if (!nw_vk_alloc(size, 16, &src, &off, &cpu)) {
        vk.vkDestroyBuffer(nw_vk.dev, own, NULL);
        vk.vkFreeMemory(nw_vk.dev, mem, NULL);
        return;
    }
    SDL_memcpy(cpu, data, size);
    nw_vk_end_rendering();
    VkBufferCopy c = { off, 0, size };
    vk.vkCmdCopyBuffer(nw_vk_cmd(), src, own, 1, &c);
    nw_vk_memory_barrier();
    if (!b->stream) {
        nw_vk_later(NW_VK_BUFFER_VIEW, (uint64_t)b->view);
    }
    nw_vk_later(NW_VK_BUFFER, (uint64_t)b->own);
    nw_vk_later(NW_VK_MEMORY, (uint64_t)b->own_mem);
    b->own = b->buf = own;
    b->own_mem = mem;
    b->off = 0;
    b->stream = false;
    b->size = size;
    b->serial = ++nw_vk.serial;
    b->view = VK_NULL_HANDLE;
    if (b->kind == NW_RHI_TEXELS) {
        bool r32 = b->texels == NW_RHI_R32F;
        VkDeviceSize elem = r32 ? 4 : 16;
        VkBufferViewCreateInfo vi = { VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO, NULL, 0, own,
                                      r32 ? VK_FORMAT_R32_SFLOAT : VK_FORMAT_R32G32B32A32_SFLOAT, 0,
                                      SDL_max((VkDeviceSize)size / elem, (VkDeviceSize)1) * elem };
        vk.vkCreateBufferView(nw_vk.dev, &vi, NULL, &b->view);
    }
}

static void nw_vk_buffer_destroy(NwRhiBuffer *b)
{
    if (!b) {
        return;
    }
    for (int i = 0; i < NW_VK_UNITS; i++) {
        if (nw_vk.buf[i] == b) {
            nw_vk.buf[i] = NULL;
        }
    }
    if (!b->stream) {
        nw_vk_later(NW_VK_BUFFER_VIEW, (uint64_t)b->view);
    }
    nw_vk_later(NW_VK_BUFFER, (uint64_t)b->own);
    nw_vk_later(NW_VK_MEMORY, (uint64_t)b->own_mem);
    SDL_free(b->shadow);
    SDL_free(b);
}

/* ---- programs ---- */

static void nw_vk_program_destroy(NwRhiProgram *p);

static NwRhiProgram *nw_vk_program_create(const NwRhiProgramDesc *d)
{
    const NwVkProgram *src = NULL;
    for (int i = 0; i < nw_vk_nsets && !src; i++) {
        if (SDL_strcmp(nw_vk_sets[i].name, d->set) == 0 && d->variant >= 0 && d->variant < nw_vk_sets[i].count) {
            src = &nw_vk_sets[i].programs[d->variant];
        }
    }
    if (!src) {
        SDL_SetError("Vulkan: no SPIR-V for %s %d (tools/gen_shaders.py)", d->set, d->variant);
        return NULL;
    }
    NwRhiProgram *p = SDL_calloc(1, sizeof(*p));
    if (!p) {
        return NULL;
    }
    p->src = src;
    p->vs[0] = nw_vk_module(&src->vs[0]);
    p->vs[1] = src->vs[1].code != src->vs[0].code ? nw_vk_module(&src->vs[1]) : p->vs[0];
    p->fs = nw_vk_module(&src->fs);
    bool ok = p->vs[0] && p->vs[1] && p->fs;
    for (int s = 0; ok && s < 2; s++) {
        if (src->ubo[s]) {
            p->data[s] = SDL_calloc(1, src->ubo[s]);
            p->dirty[s] = true;
            ok = p->data[s] != NULL;
        }
    }
    if (!ok) {
        nw_vk_program_destroy(p);
        return NULL;
    }
    return p;
}

static void nw_vk_program_destroy(NwRhiProgram *p)
{
    if (!p) {
        return;
    }
    if (nw_vk.prog == p) {
        nw_vk.prog = NULL;
    }
    for (int i = 0; nw_vk.psos && i < NW_VK_PSOS; i++) {
        if (nw_vk.psos[i].pipe && nw_vk.psos[i].key.prog == p) {
            /* left in the table as a tombstone that never matches (the program pointer is gone) */
            nw_vk_later(NW_VK_PIPELINE, (uint64_t)nw_vk.psos[i].pipe);
            nw_vk.psos[i].pipe = NW_VK_TOMB;
            nw_vk.psos[i].key.util = 0xFF;
        }
    }
    nw_vk.bound_pipe = VK_NULL_HANDLE;
    if (p->vs[1] && p->vs[1] != p->vs[0]) {
        vk.vkDestroyShaderModule(nw_vk.dev, p->vs[1], NULL);
    }
    if (p->vs[0]) {
        vk.vkDestroyShaderModule(nw_vk.dev, p->vs[0], NULL);
    }
    if (p->fs) {
        vk.vkDestroyShaderModule(nw_vk.dev, p->fs, NULL);
    }
    for (int s = 0; s < 2; s++) {
        SDL_free(p->data[s]);
    }
    SDL_free(p);
}

static int nw_vk_uniform(NwRhiProgram *p, const char *name)
{
    for (uint32_t i = 0; i < p->src->nuniforms; i++) {
        if (SDL_strcmp(p->src->uniforms[i].name, name) == 0) {
            return (int)i;
        }
    }
    return -1;
}

/* GL's std140 default block: arrays and matrix columns 16 bytes apart (as rhi_d3d11.c). */
static void nw_vk_set_uniform(NwRhiProgram *p, int u, NwRhiUniformType type, int components, int count,
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
        size_t room = p->src->ubo[s] - un->offset[s];
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

static void nw_vk_use_program(NwRhiProgram *p)
{
    nw_vk.prog = p;
}

/* ---- vertex arrays ---- */

static NwRhiVertexArray *nw_vk_vertex_array_create(void)
{
    NwRhiVertexArray *va = SDL_calloc(1, sizeof(*va));
    if (va) {
        va->layout = -1;
    }
    return va;
}

static int nw_vk_layout_id(const NwRhiVertexLayout *l)
{
    for (int i = 0; i < nw_vk.nvlayouts; i++) {
        if (SDL_memcmp(&nw_vk.vlayouts[i], l, sizeof(*l)) == 0) {
            return i;
        }
    }
    if (nw_vk.nvlayouts == NW_VK_LAYOUTS) {
        SDL_Log("Vulkan: more than %d vertex layouts", NW_VK_LAYOUTS);
        return -1;
    }
    SDL_memcpy(&nw_vk.vlayouts[nw_vk.nvlayouts], l, sizeof(*l));
    return nw_vk.nvlayouts++;
}

static void nw_vk_vertex_array_set(NwRhiVertexArray *va, const NwRhiVertexLayout *layout, NwRhiBuffer *vertices,
                                   size_t offset, NwRhiBuffer *indices)
{
    if (!va) {
        return;
    }
    va->layout = nw_vk_layout_id(layout);
    va->vb = vertices;
    va->offset = offset;
    if (indices) {
        va->ib = indices;
    }
}

static void nw_vk_vertex_array_destroy(NwRhiVertexArray *va)
{
    SDL_free(va);
}

/* ---- state ---- */

static void nw_vk_set_target(NwRhiTexture *colour, NwRhiTexture *depth)
{
    if (!colour && !depth) {
        nw_vk_fit_window();
        nw_vk.col = nw_vk.ds = NULL;
        nw_vk.to_window = true;
        nw_vk.target_w = nw_vk.win_w;
        nw_vk.target_h = nw_vk.win_h;
    } else {
        nw_vk.col = colour;
        nw_vk.ds = depth;
        nw_vk.to_window = false;
        nw_vk.target_w = colour ? colour->w : depth->w;
        nw_vk.target_h = colour ? colour->h : depth->h;
    }
    nw_vk.viewport_dirty = true;
}

static void nw_vk_viewport(int x, int y, int width, int height)
{
    nw_vk.viewport[0] = x;
    nw_vk.viewport[1] = y;
    nw_vk.viewport[2] = width;
    nw_vk.viewport[3] = height;
    nw_vk.viewport_dirty = true;
}

static void nw_vk_viewport_get(int out[4])
{
    SDL_memcpy(out, nw_vk.viewport, sizeof(nw_vk.viewport));
}

static void nw_vk_blend(bool on, NwRhiBlendOp op, NwRhiBlendFactor src, NwRhiBlendFactor dst, NwRhiBlendOp aop,
                        NwRhiBlendFactor asrc, NwRhiBlendFactor adst)
{
    nw_vk.blend_on = on;
    const uint8_t b[6] = { (uint8_t)op, (uint8_t)src, (uint8_t)dst, (uint8_t)aop, (uint8_t)asrc, (uint8_t)adst };
    SDL_memcpy(nw_vk.blend, b, sizeof(b));
}

static void nw_vk_colour_mask(bool r, bool g, bool b, bool a)
{
    nw_vk.mask = (uint8_t)(r | g << 1 | b << 2 | a << 3);
}

static void nw_vk_depth(bool test, NwRhiCompare func)
{
    nw_vk.depth_test = test;
    nw_vk.depth_func = (uint8_t)func;
}

static void nw_vk_depth_write(bool on)
{
    nw_vk.depth_write = on;
}

static void nw_vk_stencil(bool test, NwRhiCompare func, uint8_t ref, uint8_t read_mask, NwRhiStencilOp fail,
                          NwRhiStencilOp zfail, NwRhiStencilOp pass)
{
    nw_vk.stencil_test = test;
    if (test) {
        nw_vk.stencil_func = (uint8_t)func;
        nw_vk.stencil_ref = ref;
        nw_vk.stencil_read = read_mask;
        nw_vk.stencil_ops[0] = (uint8_t)fail;
        nw_vk.stencil_ops[1] = (uint8_t)zfail;
        nw_vk.stencil_ops[2] = (uint8_t)pass;
    }
}

static void nw_vk_stencil_write_mask(uint8_t mask)
{
    nw_vk.stencil_write = mask;
}

static void nw_vk_wireframe(bool on)
{
    nw_vk.wireframe = on;
}

static void nw_vk_bind_texture(int unit, NwRhiTexture *t)
{
    if (unit >= 0 && unit < NW_VK_UNITS) {
        nw_vk.tex[unit] = t;
        nw_vk.buf[unit] = NULL;
    }
}

static void nw_vk_bind_texels(int unit, NwRhiBuffer *b)
{
    if (unit >= 0 && unit < NW_VK_UNITS) {
        nw_vk.buf[unit] = b;
        nw_vk.tex[unit] = NULL;
    }
}

/* ---- pipelines ---- */

static const VkBlendFactor nw_vk_factors[] = { VK_BLEND_FACTOR_ZERO,      VK_BLEND_FACTOR_ONE,
                                               VK_BLEND_FACTOR_SRC_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
                                               VK_BLEND_FACTOR_DST_COLOR, VK_BLEND_FACTOR_DST_ALPHA,
                                               VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA };
static const VkBlendOp nw_vk_ops[] = { VK_BLEND_OP_ADD, VK_BLEND_OP_SUBTRACT, VK_BLEND_OP_REVERSE_SUBTRACT };
static const VkStencilOp nw_vk_stencil_ops[] = { VK_STENCIL_OP_KEEP, VK_STENCIL_OP_INCREMENT_AND_WRAP,
                                                 VK_STENCIL_OP_DECREMENT_AND_WRAP, VK_STENCIL_OP_REPLACE };

static const VkFormat nw_vk_attrib_formats[][4] = {
    [NW_RHI_F32] = { VK_FORMAT_R32_SFLOAT, VK_FORMAT_R32G32_SFLOAT, VK_FORMAT_R32G32B32_SFLOAT,
                     VK_FORMAT_R32G32B32A32_SFLOAT },
    [NW_RHI_S16_FLOAT] = { VK_FORMAT_R16_SINT, VK_FORMAT_R16G16_SINT, VK_FORMAT_R16G16B16A16_SINT,
                           VK_FORMAT_R16G16B16A16_SINT },
    [NW_RHI_U8_FLOAT] = { VK_FORMAT_R8_UINT, VK_FORMAT_R8G8_UINT, VK_FORMAT_R8G8B8A8_UINT, VK_FORMAT_R8G8B8A8_UINT },
    [NW_RHI_U8_INT] = { VK_FORMAT_R8_UINT, VK_FORMAT_R8G8_UINT, VK_FORMAT_R8G8B8A8_UINT, VK_FORMAT_R8G8B8A8_UINT },
    [NW_RHI_U16_INT] = { VK_FORMAT_R16_UINT, VK_FORMAT_R16G16_UINT, VK_FORMAT_R16G16B16A16_UINT,
                         VK_FORMAT_R16G16B16A16_UINT },
    [NW_RHI_U32_INT] = { VK_FORMAT_R32_UINT, VK_FORMAT_R32G32_UINT, VK_FORMAT_R32G32B32_UINT,
                         VK_FORMAT_R32G32B32A32_UINT },
};

static VkFormat nw_vk_colour_format(uint8_t code)
{
    return code == 1 ? VK_FORMAT_R8G8B8A8_UNORM : code == 2 ? VK_FORMAT_B8G8R8A8_UNORM : VK_FORMAT_UNDEFINED;
}

static uint8_t nw_vk_colour_code(VkFormat f)
{
    return f == VK_FORMAT_R8G8B8A8_UNORM ? 1 : f == VK_FORMAT_B8G8R8A8_UNORM ? 2 : f ? 1 : 0;
}

static VkPipeline nw_vk_pso(const NwVkPsoKey *k)
{
    uint32_t h = 2166136261u;
    const uint8_t *kb = (const uint8_t *)k;
    for (size_t j = 0; j < sizeof(*k); j++) {
        h = (h ^ kb[j]) * 16777619u;
    }
    uint32_t slot = h & (NW_VK_PSOS - 1);
    for (;; slot = (slot + 1) & (NW_VK_PSOS - 1)) {
        if (!nw_vk.psos[slot].pipe) {
            break;
        }
        if (SDL_memcmp(&nw_vk.psos[slot].key, k, sizeof(*k)) == 0) {
            return nw_vk.psos[slot].pipe;
        }
    }
    VkPipelineShaderStageCreateInfo st[2] = {
        { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_VERTEX_BIT,
          k->prog ? k->prog->vs[k->v] : nw_vk.util_mod[NW_D3D_BLIT_VS], "main", NULL },
        { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_FRAGMENT_BIT,
          k->prog ? k->prog->fs : nw_vk.util_mod[k->util], "main", NULL },
    };
    VkVertexInputBindingDescription vb[2];
    VkVertexInputAttributeDescription va[16];
    uint32_t nva = 0;
    if (k->prog) {
        const NwRhiVertexLayout *l = &nw_vk.vlayouts[k->layout];
        vb[0] = (VkVertexInputBindingDescription){ 0, l->stride,
                                                   l->instanced ? VK_VERTEX_INPUT_RATE_INSTANCE
                                                                : VK_VERTEX_INPUT_RATE_VERTEX };
        vb[1] = (VkVertexInputBindingDescription){ 1, 0, VK_VERTEX_INPUT_RATE_VERTEX };
        static const VkFormat dummy[3] = { VK_FORMAT_R32G32B32A32_SFLOAT, VK_FORMAT_R32G32B32A32_UINT,
                                           VK_FORMAT_R32G32B32A32_SINT };
        const NwVkProgram *src = k->prog->src;
        for (int i = 0; i < src->ninputs[k->v] && nva < 16; i++) {
            const NwD3dInput *in = &src->inputs[k->v][i];
            const NwRhiAttrib *a = NULL;
            for (int j = 0; j < l->count && !a; j++) {
                a = l->attribs[j].location == in->location ? &l->attribs[j] : NULL;
            }
            va[nva++] = a ? (VkVertexInputAttributeDescription){ in->location, 0,
                                                                  nw_vk_attrib_formats[a->type][a->components - 1],
                                                                  a->offset }
                          : (VkVertexInputAttributeDescription){ in->location, 1, dummy[in->kind], in->kind * 16u };
        }
    }
    VkPipelineVertexInputStateCreateInfo vi = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO, NULL, 0,
                                                k->prog ? 2u : 0u, vb, nva, va };
    VkPipelineInputAssemblyStateCreateInfo ia = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, NULL, 0,
                                                  VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, VK_FALSE };
    VkPipelineViewportStateCreateInfo vp = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, NULL, 0, 1, NULL,
                                             1, NULL };
    VkPipelineRasterizationStateCreateInfo rs = { .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    rs.polygonMode = k->wireframe && nw_vk.wireframe_ok ? VK_POLYGON_MODE_LINE : VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms = { .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    ms.rasterizationSamples = (VkSampleCountFlagBits)k->samples;
    VkPipelineDepthStencilStateCreateInfo ds = { .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
    ds.depthTestEnable = k->depth_test;
    ds.depthWriteEnable = k->depth_write;
    ds.depthCompareOp = (VkCompareOp)k->depth_func;
    ds.stencilTestEnable = k->stencil_test;
    VkStencilOpState so = { nw_vk_stencil_ops[k->stencil_ops[0]], nw_vk_stencil_ops[k->stencil_ops[2]],
                            nw_vk_stencil_ops[k->stencil_ops[1]], (VkCompareOp)k->stencil_func, k->stencil_read,
                            k->stencil_write, 0 };
    ds.front = ds.back = so;
    ds.maxDepthBounds = 1.0f;
    VkPipelineColorBlendAttachmentState cb = { k->blend_on,
                                               nw_vk_factors[k->blend_on ? k->blend[1] : 1],
                                               nw_vk_factors[k->blend[2]],
                                               nw_vk_ops[k->blend[0]],
                                               nw_vk_factors[k->blend_on ? k->blend[4] : 1],
                                               nw_vk_factors[k->blend[5]],
                                               nw_vk_ops[k->blend[3]],
                                               k->mask };
    VkPipelineColorBlendStateCreateInfo cbs = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, NULL, 0,
                                                VK_FALSE, VK_LOGIC_OP_COPY, k->colour ? 1u : 0u, &cb, { 0 } };
    static const VkDynamicState dyn[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR,
                                          VK_DYNAMIC_STATE_STENCIL_REFERENCE };
    VkPipelineDynamicStateCreateInfo dy = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, NULL, 0, 3, dyn };
    VkFormat cf = nw_vk_colour_format(k->colour);
    VkPipelineRenderingCreateInfoKHR ri = { VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR,
                                            NULL,
                                            0,
                                            k->colour ? 1u : 0u,
                                            &cf,
                                            k->depth ? nw_vk.depth_fmt : VK_FORMAT_UNDEFINED,
                                            k->depth ? nw_vk.depth_fmt : VK_FORMAT_UNDEFINED };
    VkGraphicsPipelineCreateInfo pi = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
                                        &ri,
                                        0,
                                        2,
                                        st,
                                        &vi,
                                        &ia,
                                        NULL,
                                        &vp,
                                        &rs,
                                        &ms,
                                        &ds,
                                        &cbs,
                                        &dy,
                                        k->prog ? nw_vk.layout : nw_vk.util_layout,
                                        VK_NULL_HANDLE,
                                        0,
                                        VK_NULL_HANDLE,
                                        -1 };
    VkPipeline pipe = VK_NULL_HANDLE;
    VkResult r = vk.vkCreateGraphicsPipelines(nw_vk.dev, VK_NULL_HANDLE, 1, &pi, NULL, &pipe);
    if (r != VK_SUCCESS) {
        static int told;
        if (told++ < 4) {
            SDL_Log("Vulkan: vkCreateGraphicsPipelines failed (%d), %s %d (colour %d, depth %d, samples %d)", (int)r,
                    k->prog ? "program" : "util", k->util, k->colour, k->depth, k->samples);
        }
        return VK_NULL_HANDLE;
    }
    if (nw_vk.npsos < NW_VK_PSOS * 3 / 4) {
        nw_vk.psos[slot] = (NwVkPso){ *k, pipe };
        nw_vk.npsos++;
    } else {
        nw_vk_later(NW_VK_PIPELINE, (uint64_t)pipe);
    }
    return pipe;
}

/* ---- rendering ---- */

static void nw_vk_begin_rendering(const NwVkTarget *t)
{
    VkRenderingAttachmentInfoKHR ca = { .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO_KHR };
    ca.imageView = t->col;
    ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    ca.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingAttachmentInfoKHR da = ca;
    da.imageView = t->ds;
    da.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    VkRenderingInfoKHR ri = { VK_STRUCTURE_TYPE_RENDERING_INFO_KHR,
                              NULL,
                              0,
                              { { 0, 0 }, { (uint32_t)t->w, (uint32_t)t->h } },
                              1,
                              0,
                              t->col ? 1u : 0u,
                              t->col ? &ca : NULL,
                              t->ds ? &da : NULL,
                              t->ds ? &da : NULL };
    vk.vkCmdBeginRenderingKHR(nw_vk_cmd(), &ri);
    nw_vk.rendering = true;
    nw_vk.r_col = t->col;
    nw_vk.r_ds = t->ds;
}

/* The draws' target: the window or the textures. */
static bool nw_vk_target(NwVkTarget *t)
{
    if (nw_vk.to_window) {
        return nw_vk_window_target(t);
    }
    SDL_zerop(t);
    if (!nw_vk.col && !nw_vk.ds) {
        return false;
    }
    if (nw_vk.col) {
        t->col_img = nw_vk.col->img;
        t->col = nw_vk.col->att;
        t->col_layout = &nw_vk.col->layout;
        t->col_fmt = nw_vk.col->fmt;
    }
    if (nw_vk.ds) {
        t->ds_img = nw_vk.ds->img;
        t->ds = nw_vk.ds->att;
        t->ds_layout = &nw_vk.ds->layout;
    }
    t->w = nw_vk.col ? nw_vk.col->w : nw_vk.ds->w;
    t->h = nw_vk.col ? nw_vk.col->h : nw_vk.ds->h;
    t->samples = nw_vk.col ? nw_vk.col->samples : nw_vk.ds->samples;
    return true;
}

/* The target's images in their attachment layouts, a rendering on them open. */
static void nw_vk_open(const NwVkTarget *t)
{
    if (t->col) {
        nw_vk_layout(t->col_img, VK_IMAGE_ASPECT_COLOR_BIT, t->col_layout, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    }
    if (t->ds) {
        nw_vk_layout(t->ds_img, VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, t->ds_layout,
                     VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
    }
    if (!nw_vk.rendering || nw_vk.r_col != t->col || nw_vk.r_ds != t->ds) {
        nw_vk_end_rendering();
        nw_vk_begin_rendering(t);
    }
}

static void nw_vk_apply_viewport(const NwVkTarget *t)
{
    const int *v = nw_vk.viewport;
    float y = nw_vk.to_window ? (float)(t->h - v[1] - v[3]) : (float)v[1];
    VkViewport vp = { (float)v[0], y, (float)v[2], (float)v[3], 0.0f, 1.0f };
    VkRect2D all = { { 0, 0 }, { (uint32_t)t->w, (uint32_t)t->h } };
    vk.vkCmdSetViewport(nw_vk_cmd(), 0, 1, &vp);
    vk.vkCmdSetScissor(nw_vk_cmd(), 0, 1, &all);
    nw_vk.viewport_dirty = false;
}

/* The descriptor set of the draw (the last one again when nothing changed); units[] the textures the units read. */
static bool nw_vk_units_set(NwRhiProgram *p, NwRhiTexture *const units[NW_VK_UNITS])
{
    uint64_t sig[NW_VK_UNITS + 4];
    for (int i = 0; i < NW_VK_UNITS; i++) {
        NwRhiBuffer *b = nw_vk_units[i] == 2 ? nw_vk.buf[i] : NULL;
        sig[i] = units[i] ? units[i]->serial : b && b->buf && b->view ? b->serial : 0;
    }
    sig[NW_VK_UNITS] = (uint64_t)(uintptr_t)p->ubo_buf[0] ^ (p->ubo_off[0] << 1);
    sig[NW_VK_UNITS + 1] = (uint64_t)(uintptr_t)p->ubo_buf[1] ^ (p->ubo_off[1] << 1);
    sig[NW_VK_UNITS + 2] = (uint64_t)(uintptr_t)p;
    sig[NW_VK_UNITS + 3] = nw_vk.epoch;
    if (nw_vk.set && SDL_memcmp(sig, nw_vk.push_sig, sizeof(sig)) == 0) {
        return true;
    }
    VkDescriptorSet set = nw_vk_alloc_set(nw_vk.set_layout);
    if (!set) {
        return false;
    }
    VkDescriptorImageInfo ii[NW_VK_UNITS];
    VkBufferView tv[NW_VK_UNITS];
    VkDescriptorBufferInfo bi[2];
    VkWriteDescriptorSet w[NW_VK_UNITS + 2];
    uint32_t n = 0;
    for (int i = 0; i < NW_VK_UNITS; i++) {
        VkWriteDescriptorSet *x = &w[n++];
        *x = (VkWriteDescriptorSet){ .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
        x->dstSet = set;
        x->dstBinding = (uint32_t)i;
        x->descriptorCount = 1;
        if (nw_vk_units[i] == 2) {
            tv[i] = sig[i] ? nw_vk.buf[i]->view : nw_vk.dummy_view;
            x->descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER;
            x->pTexelBufferView = &tv[i];
        } else {
            ii[i] = (VkDescriptorImageInfo){ VK_NULL_HANDLE, units[i] ? units[i]->view : nw_vk.dummy_tex->view,
                                             VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
            x->descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            x->pImageInfo = &ii[i];
        }
    }
    /* both uniform bindings are in the layout: a stage without a block reads the other's (never used) */
    for (int s = 0; s < 2; s++) {
        int from = p->data[s] ? s : p->data[1 - s] ? 1 - s : -1;
        if (from < 0) {
            bi[s] = (VkDescriptorBufferInfo){ nw_vk.dummy_buf, 0, 64 };
        } else {
            bi[s] = (VkDescriptorBufferInfo){ p->ubo_buf[from], p->ubo_off[from], p->src->ubo[from] };
        }
        VkWriteDescriptorSet *x = &w[n++];
        *x = (VkWriteDescriptorSet){ .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
        x->dstSet = set;
        x->dstBinding = 16u + (uint32_t)s;
        x->descriptorCount = 1;
        x->descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        x->pBufferInfo = &bi[s];
    }
    vk.vkUpdateDescriptorSets(nw_vk.dev, n, w, 0, NULL);
    SDL_memcpy(nw_vk.push_sig, sig, sizeof(sig));
    nw_vk.set = set;
    return true;
}

/* Everything a draw takes, recorded; false when it cannot draw. */
static bool nw_vk_apply(NwRhiVertexArray *va, NwRhiBuffer *ib)
{
    NwRhiProgram *p = nw_vk.prog;
    NwVkTarget t;
    if (!p || !va || va->layout < 0 || nw_vk.lost || !nw_vk_target(&t)) {
        return false;
    }
    const NwRhiVertexLayout *l = &nw_vk.vlayouts[va->layout];
    int v = 0;
    for (int k = 0; k < l->count; k++) {
        v |= l->attribs[k].location == 1 && l->attribs[k].type == NW_RHI_S16_FLOAT;
    }
    /* the units read (one holding the target reads a dummy, as under Direct3D 11) */
    NwRhiTexture *units[NW_VK_UNITS];
    for (int i = 0; i < NW_VK_UNITS; i++) {
        NwRhiTexture *u = nw_vk.tex[i];
        units[i] = u && (nw_vk.to_window || (u != nw_vk.col && u != nw_vk.ds)) && nw_vk_units[i] != 2 ? u : NULL;
    }
    /* the memory and the descriptors first: a full ring or pool starts the command buffer over */
    for (int tries = 0;; tries++) {
        uint64_t epoch = nw_vk.epoch;
        if (!nw_vk_buffer_ready(va->vb) || (ib && !nw_vk_buffer_ready(ib))) {
            return false;
        }
        for (int i = 0; i < NW_VK_UNITS; i++) {
            if (nw_vk.buf[i]) {
                nw_vk_buffer_ready(nw_vk.buf[i]);
            }
        }
        for (int s = 0; s < 2; s++) {
            if (p->data[s] && (p->dirty[s] || p->ubo_epoch[s] != nw_vk.epoch)) {
                uint8_t *cpu = NULL;
                if (!nw_vk_alloc(p->src->ubo[s], SDL_max(nw_vk.props.limits.minUniformBufferOffsetAlignment, 16),
                                 &p->ubo_buf[s], &p->ubo_off[s], &cpu)) {
                    return false;
                }
                SDL_memcpy(cpu, p->data[s], p->src->ubo[s]);
                p->ubo_epoch[s] = nw_vk.epoch;
                p->dirty[s] = false;
            }
        }
        if (epoch == nw_vk.epoch && !nw_vk_units_set(p, units)) {
            return false;
        }
        if (epoch == nw_vk.epoch || tries == 3) {
            break;
        }
    }
    /* after a flush the window may be a new image */
    if (!nw_vk_target(&t)) {
        return false;
    }
    NwVkPsoKey k;
    SDL_zero(k);
    k.prog = p;
    k.v = (uint8_t)v;
    k.layout = (uint8_t)va->layout;
    k.wireframe = nw_vk.wireframe;
    k.blend_on = nw_vk.blend_on;
    if (k.blend_on) {
        SDL_memcpy(k.blend, nw_vk.blend, sizeof(k.blend));
    }
    k.mask = nw_vk.mask;
    k.colour = t.col ? nw_vk_colour_code(t.col_fmt) : 0;
    k.depth = t.ds != VK_NULL_HANDLE;
    k.samples = (uint8_t)t.samples;
    if (k.depth) {
        k.depth_test = nw_vk.depth_test;
        k.depth_func = nw_vk.depth_func;
        k.depth_write = nw_vk.depth_write;
        k.stencil_test = nw_vk.stencil_test;
        if (k.stencil_test) {
            k.stencil_func = nw_vk.stencil_func;
            k.stencil_read = nw_vk.stencil_read;
            k.stencil_write = nw_vk.stencil_write;
            SDL_memcpy(k.stencil_ops, nw_vk.stencil_ops, sizeof(k.stencil_ops));
        }
    }
    VkPipeline pipe = nw_vk_pso(&k);
    if (!pipe) {
        return false;
    }
    for (int i = 0; i < NW_VK_UNITS; i++) {
        nw_vk_tex_layout(units[i], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
    nw_vk_open(&t);
    VkCommandBuffer c = nw_vk_cmd();
    if (nw_vk.bound_pipe != pipe) {
        vk.vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
        nw_vk.bound_pipe = pipe;
    }
    if (nw_vk.viewport_dirty) {
        nw_vk_apply_viewport(&t);
    }
    if (nw_vk.bound_ref != nw_vk.stencil_ref) {
        vk.vkCmdSetStencilReference(c, VK_STENCIL_FACE_FRONT_AND_BACK, nw_vk.stencil_ref);
        nw_vk.bound_ref = nw_vk.stencil_ref;
    }
    if (nw_vk.bound_set != nw_vk.set) {
        vk.vkCmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_GRAPHICS, nw_vk.layout, 0, 1, &nw_vk.set, 0, NULL);
        nw_vk.bound_set = nw_vk.set;
    }
    NwRhiBuffer *vbuf = va->vb;
    VkBuffer vbs[2] = { vbuf->buf, nw_vk.dummy_buf };
    VkDeviceSize offs[2] = { vbuf->off + va->offset, 0 };
    vk.vkCmdBindVertexBuffers(c, 0, 2, vbs, offs);
    if (ib) {
        vk.vkCmdBindIndexBuffer(c, ib->buf, ib->off, VK_INDEX_TYPE_UINT32);
    }
    return true;
}

static void nw_vk_draw(NwRhiVertexArray *va, uint32_t first, uint32_t count)
{
    if (nw_vk_apply(va, NULL)) {
        vk.vkCmdDraw(nw_vk_cmd(), count, 1, first, 0);
    }
}

static void nw_vk_draw_indexed(NwRhiVertexArray *va, NwRhiBuffer *indices, uint32_t first, uint32_t count)
{
    NwRhiBuffer *ib = indices ? indices : va ? va->ib : NULL;
    if (ib && nw_vk_apply(va, ib)) {
        vk.vkCmdDrawIndexed(nw_vk_cmd(), count, 1, first, 0, 0);
    }
}

static void nw_vk_draw_instanced(NwRhiVertexArray *va, uint32_t first, uint32_t count, uint32_t instances)
{
    if (nw_vk_apply(va, NULL)) {
        vk.vkCmdDraw(nw_vk_cmd(), count, instances, first, 0);
    }
}

/* ---- util draws: clears and blits (d3d11_util.hlsl as SPIR-V) ---- */

typedef struct NwVkUtilConsts {
    float map[4], scale[4], colour[4], depth[4];
} NwVkUtilConsts;

typedef struct NwVkUtil {
    int ps;
    NwVkTarget t; /* in its attachment layouts already */
    uint8_t mask;
    bool depth_write, stencil;
    uint8_t stencil_write, ref;
    VkViewport vp;
    VkRect2D scissor;
    VkImageView src[3]; /* t0-t2, NULL: a dummy */
    NwVkUtilConsts k;
} NwVkUtil;

/* One triangle over the viewport with a util pixel shader, in a rendering of its own. */
static void nw_vk_util_draw(const NwVkUtil *u)
{
    VkBuffer ubuf = VK_NULL_HANDLE;
    VkDeviceSize uoff = 0;
    uint8_t *cpu = NULL;
    VkDescriptorSet set = VK_NULL_HANDLE;
    for (int tries = 0; tries < 3; tries++) {
        uint64_t epoch = nw_vk.epoch;
        if (!nw_vk_alloc(sizeof(u->k), SDL_max(nw_vk.props.limits.minUniformBufferOffsetAlignment, 16), &ubuf, &uoff,
                         &cpu) ||
            !(set = nw_vk_alloc_set(nw_vk.util_set_layout))) {
            return;
        }
        if (epoch == nw_vk.epoch) {
            break;
        }
    }
    SDL_memcpy(cpu, &u->k, sizeof(u->k));
    NwVkPsoKey k;
    SDL_zero(k);
    k.util = (uint8_t)u->ps;
    k.mask = u->mask;
    k.colour = u->t.col ? nw_vk_colour_code(u->t.col_fmt) : 0;
    k.depth = u->t.ds != VK_NULL_HANDLE;
    k.samples = (uint8_t)u->t.samples;
    if (k.depth) {
        k.depth_test = k.depth_write = u->depth_write;
        k.depth_func = k.depth_test ? NW_RHI_ALWAYS : 0;
        k.stencil_test = u->stencil;
        if (k.stencil_test) {
            k.stencil_func = NW_RHI_ALWAYS;
            k.stencil_read = 0xFF;
            k.stencil_write = u->stencil_write;
            k.stencil_ops[0] = k.stencil_ops[1] = k.stencil_ops[2] = NW_RHI_REPLACE;
        }
    }
    VkPipeline pipe = nw_vk_pso(&k);
    if (!pipe) {
        return;
    }
    nw_vk_end_rendering();
    nw_vk_begin_rendering(&u->t);
    VkCommandBuffer c = nw_vk_cmd();
    vk.vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    VkDescriptorBufferInfo bi = { ubuf, uoff, sizeof(u->k) };
    VkDescriptorImageInfo ii[4];
    VkWriteDescriptorSet w[5];
    for (int i = 0; i < 5; i++) {
        w[i] = (VkWriteDescriptorSet){ .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
        w[i].dstSet = set;
        w[i].dstBinding = (uint32_t)i;
        w[i].descriptorCount = 1;
    }
    w[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    w[0].pBufferInfo = &bi;
    for (int i = 0; i < 3; i++) {
        VkImageView dummy = i == 2 ? nw_vk.dummy_ms->view : nw_vk.dummy_tex->view;
        ii[i] = (VkDescriptorImageInfo){ VK_NULL_HANDLE, u->src[i] ? u->src[i] : dummy,
                                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        w[1 + i].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        w[1 + i].pImageInfo = &ii[i];
    }
    ii[3] = (VkDescriptorImageInfo){ nw_vk.linear, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED };
    w[4].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    w[4].pImageInfo = &ii[3];
    vk.vkUpdateDescriptorSets(nw_vk.dev, 5, w, 0, NULL);
    vk.vkCmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_GRAPHICS, nw_vk.util_layout, 0, 1, &set, 0, NULL);
    vk.vkCmdSetViewport(c, 0, 1, &u->vp);
    vk.vkCmdSetScissor(c, 0, 1, &u->scissor);
    vk.vkCmdSetStencilReference(c, VK_STENCIL_FACE_FRONT_AND_BACK, u->ref);
    vk.vkCmdDraw(c, 3, 1, 0, 0);
    nw_vk_end_rendering();
    nw_vk_invalidate();
}

static void nw_vk_clear(const NwRhiClear *c)
{
    NwVkTarget t;
    if (nw_vk.lost || !nw_vk_target(&t)) {
        return;
    }
    bool all = c->mask[0] && c->mask[1] && c->mask[2] && c->mask[3];
    bool colour = c->colour && t.col && (c->mask[0] || c->mask[1] || c->mask[2] || c->mask[3]);
    bool depth = c->depth && c->depth_write && t.ds;
    bool stencil = c->stencil && c->stencil_write && t.ds;
    if (!colour && !depth && !stencil) {
        return;
    }
    int w = t.w, h = t.h;
    VkRect2D r = { { 0, 0 }, { (uint32_t)w, (uint32_t)h } };
    if (c->scissor) {
        const int *s = c->scissor;
        int top = nw_vk.to_window ? h - s[1] - s[3] : s[1];
        int x0 = SDL_clamp(s[0], 0, w), x1 = SDL_clamp(s[0] + s[2], 0, w);
        int y0 = SDL_clamp(top, 0, h), y1 = SDL_clamp(top + s[3], 0, h);
        if (x0 >= x1 || y0 >= y1) {
            return;
        }
        r = (VkRect2D){ { x0, y0 }, { (uint32_t)(x1 - x0), (uint32_t)(y1 - y0) } };
    }
    VkClearAttachment ca[2];
    uint32_t n = 0;
    if (colour && all) {
        ca[n].aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        ca[n].colorAttachment = 0;
        SDL_memcpy(ca[n].clearValue.color.float32, c->rgba, sizeof(c->rgba));
        n++;
        colour = false;
    }
    if ((depth || stencil) && (!stencil || c->stencil_write == 0xFF)) {
        ca[n].aspectMask = (depth ? VK_IMAGE_ASPECT_DEPTH_BIT : 0) | (stencil ? VK_IMAGE_ASPECT_STENCIL_BIT : 0);
        ca[n].colorAttachment = 0;
        ca[n].clearValue.depthStencil.depth = (float)c->z;
        ca[n].clearValue.depthStencil.stencil = c->s;
        n++;
        depth = stencil = false;
    }
    if (n) {
        nw_vk_open(&t);
        VkClearRect cr = { r, 0, 1 };
        vk.vkCmdClearAttachments(nw_vk_cmd(), n, ca, 1, &cr);
    }
    if (!colour && !depth && !stencil) {
        return;
    }
    /* The rest drawn: the whole target, cut to the scissor, writing what the masks let through. */
    nw_vk_open(&t);
    NwVkUtil u;
    SDL_zero(u);
    u.ps = NW_D3D_CLEAR;
    u.t = t;
    u.mask = colour ? (uint8_t)(c->mask[0] | c->mask[1] << 1 | c->mask[2] << 2 | c->mask[3] << 3) : 0;
    u.depth_write = depth;
    u.stencil = stencil;
    u.stencil_write = c->stencil_write;
    u.ref = c->s;
    u.vp = (VkViewport){ 0.0f, 0.0f, (float)w, (float)h, 0.0f, 1.0f };
    u.scissor = r;
    SDL_memcpy(u.k.colour, c->rgba, sizeof(c->rgba));
    u.k.depth[0] = (float)c->z;
    nw_vk_util_draw(&u);
}

/* A blit drawn: into the destination's colour (or depth), its rectangle the viewport, the source read by ps. */
static void nw_vk_blit_draw(const NwRhiBlit *b, int ps, NwRhiTexture *src, NwRhiTexture *dst)
{
    bool window = !b->dst_colour && !b->dst_depth;
    bool depth = ps == NW_D3D_BLIT_DEPTH || ps == NW_D3D_BLIT_DEPTH_MS;
    NwVkUtil u;
    SDL_zero(u);
    u.ps = ps;
    nw_vk_tex_layout(src, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    if (window) {
        if (!nw_vk_window_target(&u.t)) {
            return;
        }
        nw_vk_layout(u.t.col_img, VK_IMAGE_ASPECT_COLOR_BIT, u.t.col_layout, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    } else if (depth) {
        nw_vk_tex_layout(dst, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
        u.t = (NwVkTarget){ .ds_img = dst->img, .ds = dst->att, .ds_layout = &dst->layout, .w = dst->w, .h = dst->h,
                            .samples = dst->samples };
        u.depth_write = true;
    } else {
        nw_vk_tex_layout(dst, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        u.t = (NwVkTarget){ .col_img = dst->img, .col = dst->att, .col_layout = &dst->layout, .col_fmt = dst->fmt,
                            .w = dst->w, .h = dst->h, .samples = dst->samples };
    }
    int target_h = u.t.h;
    u.mask = depth ? 0 : 0xF;
    int dw = b->dst[2] - b->dst[0], dh = b->dst[3] - b->dst[1];
    float top = window ? (float)(target_h - b->dst[3]) : (float)b->dst[1];
    u.vp = (VkViewport){ (float)b->dst[0], top, (float)dw, (float)dh, 0.0f, 1.0f };
    u.scissor = (VkRect2D){ { 0, 0 }, { (uint32_t)u.t.w, (uint32_t)u.t.h } };
    u.src[ps == NW_D3D_BLIT_DEPTH ? 1 : ps == NW_D3D_BLIT_DEPTH_MS ? 2 : 0] = src->view;
    NwVkUtilConsts k = {
        { (float)b->src[0], (float)b->src[1], (float)b->dst[0], (float)b->dst[1] },
        { (float)(b->src[2] - b->src[0]) / (float)dw, (float)(b->src[3] - b->src[1]) / (float)dh,
          window ? (float)target_h : 0.0f, (float)b->src_level },
        { 0 },
        { 0, (float)SDL_max(src->w >> b->src_level, 1), (float)SDL_max(src->h >> b->src_level, 1), 0 },
    };
    u.k = k;
    nw_vk_util_draw(&u);
}

static void nw_vk_blit(const NwRhiBlit *b)
{
    if (nw_vk.lost) {
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
        nw_vk_fit_window();
        if (nw_vk.to_window) {
            nw_vk.target_w = nw_vk.win_w;
            nw_vk.target_h = nw_vk.win_h;
        }
    }
    VkCommandBuffer c;
    if (b->colour && b->src_colour) {
        NwRhiTexture *s = b->src_colour, *d = b->dst_colour;
        bool full = same && sw == s->w && sh == s->h && d && sw == d->w && sh == d->h;
        bool resolve = same && d && s->samples > 1 && d->samples <= 1 && full;
        bool copy = same && d && s->samples == d->samples && (s->samples <= 1 || full);
        if (resolve || copy) {
            nw_vk_tex_layout(s, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
            nw_vk_tex_layout(d, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            c = nw_vk_cmd();
            VkImageSubresourceLayers sub = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
            VkOffset3D so = { b->src[0], b->src[1], 0 }, dof = { b->dst[0], b->dst[1], 0 };
            VkExtent3D ext = { (uint32_t)sw, (uint32_t)sh, 1 };
            if (s->samples > 1 && d->samples <= 1) {
                VkImageResolve r = { sub, so, sub, dof, ext };
                vk.vkCmdResolveImage(c, s->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, d->img,
                                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
            } else {
                VkImageCopy r = { sub, so, sub, dof, ext };
                vk.vkCmdCopyImage(c, s->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, d->img,
                                  VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
            }
        } else if (s->samples <= 1 && (window || d->att)) {
            nw_vk_blit_draw(b, b->linear ? NW_D3D_BLIT_LINEAR : NW_D3D_BLIT_POINT, s, d);
        }
    }
    if ((b->depth || b->stencil) && b->src_depth && b->dst_depth) {
        NwRhiTexture *s = b->src_depth, *d = b->dst_depth;
        bool full = same && sw == s->w && sh == s->h && sw == d->w && sh == d->h;
        if (full && s->samples == d->samples) {
            nw_vk_tex_layout(s, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
            nw_vk_tex_layout(d, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            VkImageSubresourceLayers sub = { VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, 0, 0, 1 };
            VkImageCopy r = { sub, { 0, 0, 0 }, sub, { 0, 0, 0 }, { (uint32_t)sw, (uint32_t)sh, 1 } };
            vk.vkCmdCopyImage(nw_vk_cmd(), s->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, d->img,
                              VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
        } else if (d->samples <= 1) {
            /* Depth alone; an MSAA resolve's stencil is not carried over (xbox/RENDERERS.md). */
            nw_vk_blit_draw(b, s->samples > 1 ? NW_D3D_BLIT_DEPTH_MS : NW_D3D_BLIT_DEPTH, s, d);
        }
    }
    nw_vk.viewport_dirty = true;
}

/* A target's smaller levels, each a linear blit of the one before (a 2x2 average). */
static void nw_vk_generate_mips(NwRhiTexture *t)
{
    if (!(t->flags & NW_RHI_TEX_MIPGEN) || t->levels <= 1 || nw_vk.lost) {
        return;
    }
    nw_vk_tex_layout(t, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    for (int i = 1; i < t->levels; i++) {
        nw_vk_image_barrier(t->img, VK_IMAGE_ASPECT_COLOR_BIT, (uint32_t)(i - 1), 1,
                            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        VkImageBlit r = { { VK_IMAGE_ASPECT_COLOR_BIT, (uint32_t)(i - 1), 0, 1 },
                          { { 0, 0, 0 }, { SDL_max(t->w >> (i - 1), 1), SDL_max(t->h >> (i - 1), 1), 1 } },
                          { VK_IMAGE_ASPECT_COLOR_BIT, (uint32_t)i, 0, 1 },
                          { { 0, 0, 0 }, { SDL_max(t->w >> i, 1), SDL_max(t->h >> i, 1), 1 } } };
        vk.vkCmdBlitImage(nw_vk_cmd(), t->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, t->img,
                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r, VK_FILTER_LINEAR);
    }
    nw_vk_image_barrier(t->img, VK_IMAGE_ASPECT_COLOR_BIT, 0, (uint32_t)(t->levels - 1),
                        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    nw_vk_image_barrier(t->img, VK_IMAGE_ASPECT_COLOR_BIT, (uint32_t)(t->levels - 1), 1,
                        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    t->layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
}

/* ---- read back ---- */

/* A rectangle of img (in TRANSFER_SRC) as RGBA8 rows; waits for the GPU. bgra: swap red and blue. */
static bool nw_vk_read(VkImage img, int x, int y, int width, int height, void *rgba, int pitch, bool bgra)
{
    if (width <= 0 || height <= 0) {
        return true;
    }
    VkBuffer rb = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    void *map = NULL;
    if (!nw_vk_buffer((VkDeviceSize)width * (VkDeviceSize)height * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true, &rb, &mem,
                      &map)) {
        return false;
    }
    VkBufferImageCopy c = { 0, 0, 0, { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, { x, y, 0 },
                            { (uint32_t)width, (uint32_t)height, 1 } };
    nw_vk_end_rendering();
    vk.vkCmdCopyImageToBuffer(nw_vk_cmd(), img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rb, 1, &c);
    nw_vk_flush();
    for (int i = 0; i < height; i++) {
        uint8_t *dst = (uint8_t *)rgba + (size_t)i * (size_t)pitch;
        SDL_memcpy(dst, (const uint8_t *)map + (size_t)i * (size_t)width * 4, (size_t)width * 4);
        for (int k = 0; bgra && k < width; k++) {
            uint8_t t = dst[k * 4];
            dst[k * 4] = dst[k * 4 + 2];
            dst[k * 4 + 2] = t;
        }
    }
    vk.vkDestroyBuffer(nw_vk.dev, rb, NULL);
    vk.vkFreeMemory(nw_vk.dev, mem, NULL);
    return true;
}

static bool nw_vk_texture_read(NwRhiTexture *t, int x, int y, int width, int height, void *rgba, int pitch)
{
    if (t->samples > 1 || t->format != NW_RHI_RGBA8) {
        return SDL_SetError("Vulkan: only single-sampled RGBA8 textures are read");
    }
    nw_vk_tex_layout(t, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    return nw_vk_read(t->img, x, y, width, height, rgba, pitch, false);
}

/* The window's rows from the top (only textures keep GL's rows). */
static bool nw_vk_window_read(int width, int height, void *rgba, int pitch)
{
    NwVkTarget t;
    if (!nw_vk_window_target(&t)) {
        return SDL_SetError("Vulkan: no window image");
    }
    nw_vk_layout(t.col_img, VK_IMAGE_ASPECT_COLOR_BIT, t.col_layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    return nw_vk_read(t.col_img, 0, 0, SDL_min(width, t.w), SDL_min(height, t.h), rgba, pitch,
                      t.col_fmt == VK_FORMAT_B8G8R8A8_UNORM);
}

#define NW_VK_ENTRY_X(name, ret, params, args) .name = nw_vk_##name,
#define NW_VK_ENTRY_V(name, params, args) .name = nw_vk_##name,
const NwRhiBackend nw_rhi_vulkan = { .prepare_window = nw_vk_prepare_window,
                                     NW_RHI_CALLS(NW_VK_ENTRY_X, NW_VK_ENTRY_V) };

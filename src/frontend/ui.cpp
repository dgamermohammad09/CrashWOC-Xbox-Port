#include "frontend/ui.h"

#include "imgui.h"
#include "imgui_impl_opengl3.h"
#include "imgui_impl_sdl3.h"
#ifdef NW_RHI_HAVE_D3D11
#include "imgui_impl_dx11.h"
#endif
#ifdef NW_RHI_HAVE_D3D12
#include "imgui_impl_dx12.h"

#include "gfx/rhi_d3d12.h"
#endif
#ifdef NW_RHI_HAVE_VULKAN
#include "imgui_impl_vulkan.h"

#include "gfx/rhi_vk.h"
#endif

#include "gfx/rhi.h"

#define NW_UI_FONT_SIZE 20.0f
#define NW_UI_COMPACT_FONT_SIZE 22.0f

/* build/gen/fonts.c, from assets/fonts/ (cmake/embed.cmake). */
extern "C" const unsigned char nw_font_regular[], nw_font_bold[];
extern "C" const size_t nw_font_regular_size, nw_font_bold_size;

static SDL_Window *nw_ui_window;
static ImFont *nw_ui_fonts[2];
static float nw_ui_scale;
static bool nw_ui_compact;

#ifdef NW_RHI_HAVE_D3D12
/* Dear ImGui's textures take descriptors of the renderer's shader-visible heap. */
static void nw_ui_dx12_alloc(ImGui_ImplDX12_InitInfo *, D3D12_CPU_DESCRIPTOR_HANDLE *cpu, D3D12_GPU_DESCRIPTOR_HANDLE *gpu)
{
    uint64_t c = 0, g = 0;
    nw_rhi_d3d12_ui_alloc(&c, &g);
    cpu->ptr = (SIZE_T)c;
    gpu->ptr = g;
}

static void nw_ui_dx12_free(ImGui_ImplDX12_InitInfo *, D3D12_CPU_DESCRIPTOR_HANDLE, D3D12_GPU_DESCRIPTOR_HANDLE gpu)
{
    nw_rhi_d3d12_ui_free(gpu.ptr);
}
#endif

#ifdef NW_RHI_HAVE_VULKAN
/* Dear ImGui's Vulkan functions come from the renderer's driver (there is no loader to link on Xbox). */
static void VKAPI_PTR nw_ui_vk_missing(void)
{
}

/* Functions the driver lacks (the surface ones when the window is not a Vulkan surface) are only used by Dear
   ImGui's own window helpers, which NuWorld never calls: a stub keeps ImGui_ImplVulkan_LoadFunctions happy. */
static PFN_vkVoidFunction nw_ui_vk_load(const char *name, void *)
{
    PFN_vkVoidFunction f = (PFN_vkVoidFunction)nw_rhi_vk_proc(name);
    if (!f) {
        SDL_Log("Vulkan: Dear ImGui's %s is not in the driver", name);
    }
    return f ? f : nw_ui_vk_missing;
}

static uint64_t nw_ui_vk_add(uint64_t view, int layout)
{
    return (uint64_t)(uintptr_t)ImGui_ImplVulkan_AddTexture((VkImageView)(uintptr_t)view, (VkImageLayout)layout);
}

static void nw_ui_vk_remove(uint64_t set)
{
    ImGui_ImplVulkan_RemoveTexture((VkDescriptorSet)(uintptr_t)set);
}

static VkFormat nw_ui_vk_format;
#endif

static ImFont *nw_ui_add_font(const unsigned char *data, size_t size, const char *name)
{
    ImFontConfig cfg;
    cfg.FontDataOwnedByAtlas = false; /* static data */
    SDL_strlcpy(cfg.Name, name, sizeof(cfg.Name));
    return ImGui::GetIO().Fonts->AddFontFromMemoryTTF((void *)data, (int)size, NW_UI_FONT_SIZE, &cfg);
}

static ImVec4 nw_ui_rgb(unsigned rgb, float a = 1.0f)
{
    return ImVec4((float)(rgb >> 16 & 0xff) / 255.0f, (float)(rgb >> 8 & 0xff) / 255.0f, (float)(rgb & 0xff) / 255.0f, a);
}

/* The Wrath of Cortex's colours: Cortex's deep purple panels, Crash's orange, the yellow menu cursor; sized for scale
   (the display's content scale). */
static void nw_ui_style(float scale)
{
    ImGuiStyle style;
    ImGui::StyleColorsDark(&style);
    style.WindowPadding = ImVec2(28.0f, 24.0f);
    style.FramePadding = ImVec2(14.0f, 8.0f);
    style.ItemSpacing = ImVec2(10.0f, 10.0f);
    style.WindowRounding = 10.0f;
    style.ChildRounding = 6.0f;
    style.FrameRounding = 6.0f;
    style.GrabRounding = 6.0f;
    style.WindowBorderSize = 1.0f;
    style.SeparatorTextBorderSize = 2.0f;

    ImVec4 *c = style.Colors;
    c[ImGuiCol_Text] = nw_ui_rgb(0xfff4e0);
    c[ImGuiCol_TextDisabled] = nw_ui_rgb(0xc0a6e0);
    c[ImGuiCol_WindowBg] = nw_ui_rgb(0x170733, 0.94f);
    c[ImGuiCol_ChildBg] = nw_ui_rgb(0x12052a);
    c[ImGuiCol_PopupBg] = nw_ui_rgb(0x170733);
    c[ImGuiCol_MenuBarBg] = nw_ui_rgb(0x2a0f4d);
    c[ImGuiCol_Border] = nw_ui_rgb(0xff8a1c);
    c[ImGuiCol_FrameBg] = nw_ui_rgb(0x30115a);
    c[ImGuiCol_FrameBgHovered] = nw_ui_rgb(0x4a1a86);
    c[ImGuiCol_FrameBgActive] = nw_ui_rgb(0x5e24a8);
    c[ImGuiCol_Button] = nw_ui_rgb(0xff8a1c);
    c[ImGuiCol_ButtonHovered] = nw_ui_rgb(0xffa640);
    c[ImGuiCol_ButtonActive] = nw_ui_rgb(0xc9500a);
    c[ImGuiCol_Header] = nw_ui_rgb(0x4a1a86);
    c[ImGuiCol_HeaderHovered] = nw_ui_rgb(0x5e24a8);
    c[ImGuiCol_HeaderActive] = nw_ui_rgb(0xc9500a);
    c[ImGuiCol_Separator] = nw_ui_rgb(0xff8a1c, 0.6f);
    c[ImGuiCol_CheckMark] = nw_ui_rgb(0xffd23f);
    c[ImGuiCol_SliderGrab] = nw_ui_rgb(0xff8a1c);
    c[ImGuiCol_SliderGrabActive] = nw_ui_rgb(0xffd23f);
    c[ImGuiCol_TitleBg] = nw_ui_rgb(0x2a0f4d);
    c[ImGuiCol_TitleBgActive] = nw_ui_rgb(0x4a1a86);
    c[ImGuiCol_TableRowBgAlt] = nw_ui_rgb(0xffffff, 0.04f);
    c[ImGuiCol_PlotHistogram] = nw_ui_rgb(0xff8a1c);
    c[ImGuiCol_ScrollbarBg] = nw_ui_rgb(0x12052a);
    c[ImGuiCol_ScrollbarGrab] = nw_ui_rgb(0xff8a1c, 0.8f);
    c[ImGuiCol_NavCursor] = nw_ui_rgb(0xffd23f);
    c[ImGuiCol_TextLink] = nw_ui_rgb(0xffd23f);
    style.WindowBorderSize = 3.0f;
    style.FrameBorderSize = 0.0f;

    /* Over the running game: tight, translucent, orange only on what is set. */
    if (nw_ui_compact) {
        style.WindowPadding = ImVec2(12.0f, 10.0f);
        style.FramePadding = ImVec2(10.0f, 6.0f);
        style.ItemSpacing = ImVec2(10.0f, 7.0f);
        style.ItemInnerSpacing = ImVec2(5.0f, 4.0f);
        style.IndentSpacing = 20.0f;
        style.ScrollbarSize = 16.0f;
        style.GrabMinSize = 8.0f;
        style.WindowRounding = 6.0f;
        style.ChildRounding = 4.0f;
        style.FrameRounding = 3.0f;
        style.GrabRounding = 3.0f;
        style.PopupRounding = 4.0f;
        style.SeparatorTextBorderSize = 1.0f;
        style.WindowBorderSize = 2.0f;
        c[ImGuiCol_WindowBg] = nw_ui_rgb(0x170733, 0.94f);
        c[ImGuiCol_TitleBg] = nw_ui_rgb(0x2a0f4d, 0.94f);
        c[ImGuiCol_TitleBgActive] = nw_ui_rgb(0xc9500a);
        c[ImGuiCol_TitleBgCollapsed] = nw_ui_rgb(0x2a0f4d, 0.75f);
        c[ImGuiCol_Button] = nw_ui_rgb(0x4a1a86);
        c[ImGuiCol_ButtonHovered] = nw_ui_rgb(0x5e24a8);
        c[ImGuiCol_ButtonActive] = nw_ui_rgb(0xc9500a);
    }

    style.FontSizeBase = nw_ui_compact ? NW_UI_COMPACT_FONT_SIZE : NW_UI_FONT_SIZE;
    style.ScaleAllSizes(scale);
    style.FontScaleDpi = scale;
    ImGui::GetStyle() = style;
    nw_ui_scale = scale;
}

static float nw_ui_content_scale(void)
{
    float scale = SDL_GetDisplayContentScale(SDL_GetDisplayForWindow(nw_ui_window));
    return scale > 0.0f ? scale : 1.0f;
}

bool nw_ui_init(NwPlatform *p, bool compact)
{
    nw_ui_compact = compact;
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = NULL;
    io.LogFilename = NULL;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;

    nw_ui_window = p->window;
    nw_ui_fonts[NW_UI_REGULAR] = nw_ui_add_font(nw_font_regular, nw_font_regular_size, "Atkinson Hyperlegible");
    nw_ui_fonts[NW_UI_BOLD] = nw_ui_add_font(nw_font_bold, nw_font_bold_size, "Atkinson Hyperlegible Bold");
    nw_ui_style(nw_ui_content_scale());

#ifdef NW_RHI_HAVE_D3D11
    /* Direct3D 11: the renderer's device and context. */
    if (nw_rhi_kind() == NW_RHI_D3D11) {
        if (!nw_ui_fonts[NW_UI_REGULAR] || !nw_ui_fonts[NW_UI_BOLD] || !ImGui_ImplSDL3_InitForD3D(p->window)) {
            ImGui::DestroyContext();
            return SDL_SetError("Dear ImGui could not start");
        }
        if (!ImGui_ImplDX11_Init((ID3D11Device *)nw_rhi_native(0), (ID3D11DeviceContext *)nw_rhi_native(1))) {
            ImGui_ImplSDL3_Shutdown();
            ImGui::DestroyContext();
            return SDL_SetError("Dear ImGui could not start Direct3D 11");
        }
        return true;
    }
#endif
#ifdef NW_RHI_HAVE_D3D12
    /* Direct3D 12: the renderer's device, queue and heap; it records into the renderer's command list. */
    if (nw_rhi_kind() == NW_RHI_D3D12) {
        if (!nw_ui_fonts[NW_UI_REGULAR] || !nw_ui_fonts[NW_UI_BOLD] || !ImGui_ImplSDL3_InitForD3D(p->window)) {
            ImGui::DestroyContext();
            return SDL_SetError("Dear ImGui could not start");
        }
        ImGui_ImplDX12_InitInfo info;
        info.Device = (ID3D12Device *)nw_rhi_native(0);
        info.CommandQueue = (ID3D12CommandQueue *)nw_rhi_native(1);
        info.NumFramesInFlight = NW_RHI_D3D12_FRAMES + 1;
        info.RTVFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
        info.DSVFormat = DXGI_FORMAT_UNKNOWN;
        info.SrvDescriptorHeap = (ID3D12DescriptorHeap *)nw_rhi_native(3);
        info.SrvDescriptorAllocFn = nw_ui_dx12_alloc;
        info.SrvDescriptorFreeFn = nw_ui_dx12_free;
        if (!ImGui_ImplDX12_Init(&info)) {
            ImGui_ImplSDL3_Shutdown();
            ImGui::DestroyContext();
            return SDL_SetError("Dear ImGui could not start Direct3D 12");
        }
        return true;
    }
#endif
#ifdef NW_RHI_HAVE_VULKAN
    /* Vulkan: the renderer's device; it records into the renderer's command buffer, in its rendering of the window. */
    if (nw_rhi_kind() == NW_RHI_VULKAN) {
        if (!nw_ui_fonts[NW_UI_REGULAR] || !nw_ui_fonts[NW_UI_BOLD] || !ImGui_ImplSDL3_InitForVulkan(p->window)) {
            ImGui::DestroyContext();
            return SDL_SetError("Dear ImGui could not start");
        }
        ImGui_ImplVulkan_InitInfo info = {};
        info.ApiVersion = nw_rhi_vk_api_version();
        info.Instance = (VkInstance)nw_rhi_native(0);
        info.PhysicalDevice = (VkPhysicalDevice)nw_rhi_native(1);
        info.Device = (VkDevice)nw_rhi_native(2);
        info.Queue = (VkQueue)nw_rhi_native(3);
        info.QueueFamily = (uint32_t)(uintptr_t)nw_rhi_native(4);
        info.DescriptorPoolSize = 64;
        info.MinImageCount = 2;
        info.ImageCount = (uint32_t)(uintptr_t)nw_rhi_native(7);
        info.UseDynamicRendering = true;
        nw_ui_vk_format = (VkFormat)(uintptr_t)nw_rhi_native(6);
        VkPipelineRenderingCreateInfoKHR &ri = info.PipelineInfoMain.PipelineRenderingCreateInfo;
        ri = {};
        ri.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR;
        ri.colorAttachmentCount = 1;
        ri.pColorAttachmentFormats = &nw_ui_vk_format;
        info.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
        if (!ImGui_ImplVulkan_LoadFunctions(info.ApiVersion, nw_ui_vk_load) || !ImGui_ImplVulkan_Init(&info)) {
            ImGui_ImplSDL3_Shutdown();
            ImGui::DestroyContext();
            return SDL_SetError("Dear ImGui could not start Vulkan");
        }
        nw_rhi_vk_ui_textures(nw_ui_vk_add, nw_ui_vk_remove);
        return true;
    }
#endif
    if (!nw_ui_fonts[NW_UI_REGULAR] || !nw_ui_fonts[NW_UI_BOLD] || !ImGui_ImplSDL3_InitForOpenGL(p->window, p->gl)) {
        ImGui::DestroyContext();
        return SDL_SetError("Dear ImGui could not start");
    }
    if (!ImGui_ImplOpenGL3_Init("#version 330 core")) {
        ImGui_ImplSDL3_Shutdown();
        ImGui::DestroyContext();
        return SDL_SetError("Dear ImGui could not start OpenGL");
    }
    return true;
}

void nw_ui_shutdown(void)
{
#ifdef NW_RHI_HAVE_D3D11
    if (nw_rhi_kind() == NW_RHI_D3D11) {
        ImGui_ImplDX11_Shutdown();
    } else
#endif
#ifdef NW_RHI_HAVE_D3D12
    if (nw_rhi_kind() == NW_RHI_D3D12) {
        nw_rhi_d3d12_idle(); /* its buffers may still be read */
        ImGui_ImplDX12_Shutdown();
    } else
#endif
#ifdef NW_RHI_HAVE_VULKAN
    if (nw_rhi_kind() == NW_RHI_VULKAN) {
        nw_rhi_vk_idle();
        nw_rhi_vk_ui_textures(NULL, NULL);
        ImGui_ImplVulkan_Shutdown();
    } else
#endif
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    nw_ui_window = NULL;
}

void nw_ui_event(const SDL_Event *e)
{
    ImGui_ImplSDL3_ProcessEvent(e);
    if (e->type == SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED || e->type == SDL_EVENT_WINDOW_DISPLAY_CHANGED) {
        float scale = nw_ui_content_scale();
        if (scale != nw_ui_scale) {
            nw_ui_style(scale);
        }
    }
}

void nw_ui_begin(void)
{
#ifdef NW_RHI_HAVE_D3D11
    if (nw_rhi_kind() == NW_RHI_D3D11) {
        ImGui_ImplDX11_NewFrame();
    } else
#endif
#ifdef NW_RHI_HAVE_D3D12
    if (nw_rhi_kind() == NW_RHI_D3D12) {
        ImGui_ImplDX12_NewFrame();
    } else
#endif
#ifdef NW_RHI_HAVE_VULKAN
    if (nw_rhi_kind() == NW_RHI_VULKAN) {
        ImGui_ImplVulkan_NewFrame();
    } else
#endif
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
}

void nw_ui_end(void)
{
    ImGui::Render();
    nw_rhi_set_target(NULL, NULL); /* the window */
#ifdef NW_RHI_HAVE_D3D11
    if (nw_rhi_kind() == NW_RHI_D3D11) {
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        return;
    }
#endif
#ifdef NW_RHI_HAVE_D3D12
    if (nw_rhi_kind() == NW_RHI_D3D12) {
        ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), (ID3D12GraphicsCommandList *)nw_rhi_native(2));
        return;
    }
#endif
#ifdef NW_RHI_HAVE_VULKAN
    if (nw_rhi_kind() == NW_RHI_VULKAN) {
        VkCommandBuffer cmd = (VkCommandBuffer)nw_rhi_native(5);
        if (cmd) {
            ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmd);
        }
        return;
    }
#endif
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
}

void nw_ui_push_font(NwUiFont font, float scale)
{
    ImGui::PushFont(nw_ui_fonts[font], ImGui::GetStyle().FontSizeBase * scale);
}

void nw_ui_pop_font(void)
{
    ImGui::PopFont();
}

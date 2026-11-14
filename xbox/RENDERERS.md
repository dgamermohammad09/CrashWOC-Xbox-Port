# Renderers

NuWorld draws through one small GPU interface, `src/gfx/rhi.h` (render hardware interface). Four backends implement
it:

| Renderer | File | Where | Default |
|---|---|---|---|
| OpenGL 3.3 core | `src/gfx/rhi_gl.c` | desktop; UWP through Mesa (`opengl32.dll` on D3D12) | desktop |
| Direct3D 11 (feature level 11_0+) | `src/gfx/rhi_d3d11.c` | desktop (HWND swap chain) and UWP (CoreWindow swap chain) | UWP / Xbox |
| Direct3D 12 (feature level 11_0+) | `src/gfx/rhi_d3d12.c` | desktop (HWND) and UWP (CoreWindow); WARP when no D3D12 GPU | - |
| Vulkan 1.1+ with `VK_KHR_dynamic_rendering` | `src/gfx/rhi_vk.c` | desktop (system loader, else Mesa Dozen); UWP: Dozen, experimental | - |

Pick one with `settings.toml` `[video] renderer = "auto" | "opengl" | "d3d11" | "d3d12" | "vulkan"`, the launcher's /
F1 menu's *Video -> Renderer* entry (takes effect at the next start), or `--renderer opengl|d3d11|d3d12|vulkan` (also
`gl`, `dx11`, `dx12`, `vk`). `auto` is `nw_rhi_kind_default()`: OpenGL on a PC, Direct3D 11 in the UWP build. A
renderer that is not built in falls back to the default with a log line.

| Renderer | How it runs on Xbox | Verified (this VM: no D3D12 GPU, so D3D12 = WARP) | Gaps |
|---|---|---|---|
| OpenGL | Mesa 26.2.2 `opengl32.dll` (gallium d3d12) on Direct3D 12, in the package | desktop: reference; UWP: title menu (Mesa on WARP) | slower than native; not on real Xbox yet |
| Direct3D 11 | native, CoreWindow swap chain (default) | desktop: bit-exact vs GL (title); UWP: title menu | not on real Xbox yet |
| Direct3D 12 | native, CoreWindow swap chain | desktop (WARP): bit-exact vs D3D11 WARP, title and in level; UWP (WARP): title menu | no GPU run anywhere yet (VM has none); not on real Xbox |
| Vulkan | Mesa Dozen (`vulkan_dzn.dll`, Vulkan on D3D12) loaded with `LoadPackagedLibrary`, rendering offscreen, each frame copied to the CPU and shown by a Direct3D 11 swap chain | desktop (Dozen on WARP): same as D3D WARP to 0.003 mean, title and in level; UWP (Dozen on WARP): title menu | experimental: a GPU wait + CPU copy per frame; Dozen's own win32 surface fails here (see Gaps); no real Vulkan GPU tested; not on real Xbox |

Environment switches: `NW_D3D11_DRIVER=warp` / `NW_D3D12_DRIVER=warp` force Microsoft's software rasterizer;
`NW_D3D11_DEBUG=1` / `NW_D3D12_DEBUG=1` ask for the debug layer (needs Windows' Graphics Tools). Direct3D 12 takes
the first hardware adapter that creates a feature level 11_0 device and falls back to WARP by itself (logged:
`Direct3D 12: no GPU with a Direct3D 12 driver (...); WARP`). Vulkan: `NW_VK_ICD=<vulkan_dzn.dll or another ICD>`
skips the system loader; `NW_VK_PRESENT=copy|surface` picks the window path; Dozen gets `DZN_DEBUG=warp` set for it
when there is no Direct3D 12 GPU. None of the Direct3D renderers needs OpenGL: no GL context and no
`SDL_WINDOW_OPENGL` (`nw_rhi_prepare_window()` returns 0), so Mesa's OpenGL is not even loaded.

## The abstraction

Who calls it:

- `src/gfx/backend.c` (targets, blits, clears, the window frame) and `src/gfx/vu.c` (the VU1 microcode as GPU
  programs) - all game drawing.
- `src/frontend/ui.cpp` (Dear ImGui: `imgui_impl_opengl3`, `imgui_impl_dx11`, `imgui_impl_dx12` or
  `imgui_impl_vulkan` on the device from `nw_rhi_native()`), `launcher.cpp`, `wizard.cpp` (textures via
  `nw_rhi_texture_ui()`), `help.c`, `platform.c`,
  `main.c` (`nw_rhi_present()`).

No file outside `src/gfx/rhi_*.c` touches a graphics API (`src/gfx/gl.h` is included by `rhi_gl.c` only, plus the
`particle_gl` unit test).

Conventions every backend keeps (they are why D3D11 frames come out bit-identical to GL's):

- **GL coordinates.** A texture's row 0 is the bottom of what is drawn into it; viewports, scissors and blit
  rectangles count rows from row 0. Backends whose clip space has y pointing the other way (D3D, Vulkan) flip y in
  the vertex stage (`spirv-cross --flip-vert-y`), so every texture holds the same bytes in the same rows as under GL.
  Only the window is flipped back: blits to the window run upside down and viewports on the window are mirrored.
- **Clip z -w..w** as GL; D3D remaps to 0..w in the vertex stage (`--fixup-clipspace`). Depth buffers hold 0..1.
- **One shader source.** Programs are GLSL in `src/gfx/shaders/` (`vu.vs.glsl`, `vu.fs.glsl`, `gem.fs.glsl`), built
  in variants by `#define`s (`NwRhiProgramDesc.set` + `variant`). GL compiles them at run time; other backends take
  bytecode built from the same text at build time by `tools/gen_shaders.py`.
- **Uniforms by name**, kept per program like GL's default block. D3D11 keeps a CPU copy of each stage's cbuffer b0
  and uploads it when dirty at the next draw (offsets from `spirv-cross --reflect`).
- **Fixed texture units** per sampler name (`vu.c`); textures and texel buffers bind to units, not programs.
- **State is sticky** (blend, depth, stencil, masks, wireframe, viewport) as in GL; D3D11 builds and caches state
  objects from it at draw time.

Layering:

```
src/gfx/rhi.h        public API (C, render thread only)
src/gfx/rhi.c        backend table, selection (nw_rhi_select / _selected / _kind_default), one-line wrappers
src/gfx/rhi_impl.h   NW_RHI_CALLS(X, V): the list of entry points; NwRhiBackend = one function pointer each
src/gfx/rhi_gl.c     const NwRhiBackend nw_rhi_gl
src/gfx/rhi_d3d11.c  const NwRhiBackend nw_rhi_d3d11      (+ rhi_d3d11_shaders.h, shaders/d3d11_util.hlsl)
src/gfx/rhi_d3d12.c  const NwRhiBackend nw_rhi_d3d12      (+ rhi_d3d12.h for Dear ImGui; the D3D11 DXBC)
src/gfx/rhi_vk.c     const NwRhiBackend nw_rhi_vulkan     (+ rhi_vk.h for Dear ImGui, rhi_vk_shaders.h)
cmake/d3d11.cmake    NUWORLD_D3D11 option, shader generation, links d3d11 dxgi dxguid (+ d3dcompiler for ImGui)
cmake/d3d12.cmake    NUWORLD_D3D12 (needs NUWORLD_D3D11's shaders), imgui_impl_dx12, links d3d12 dxgi
cmake/vulkan.cmake   NUWORLD_VULKAN (needs tools/vulkan-headers), SPIR-V generation, imgui_impl_vulkan with
                     IMGUI_IMPL_VULKAN_NO_PROTOTYPES; copies tools/mesa-dzn's vulkan_dzn.dll + dxil.dll next to the
                     desktop exe
tools/gen_shaders.py GLSL -> SPIR-V (glslang) -> HLSL (SPIRV-Cross) -> DXBC (fxc), embedded as build/gen/*.c;
                     --target vulkan: GLSL -> SPIR-V only (build/gen/vk_shaders.c)
```

What Direct3D 11 lacks and is drawn instead (`d3d11_util.hlsl`, one full-viewport triangle): scaled colour blits
(point/linear), depth blits and MSAA depth resolves (`SV_Depth`), and clears with a colour mask, a stencil write mask
or a scissor. Same-size copies use `CopySubresourceRegion`, same-size MSAA colour resolves `ResolveSubresource`.
GL's integer-to-float vertex formats do not exist in D3D: the colour comes in as `uvec4` and the int16 texcoords as
`ivec4` (`NW_SPIRV` / `NW_TEX_TYPE` in `vu.vs.glsl`), so each vertex program is built twice (float and int16
texcoords) and the backend picks the build from the vertex layout. Inputs a layout lacks read GL's (0, 0, 0, 1)
from a small dummy vertex buffer.

## Direct3D 12 (`rhi_d3d12.c`)

- **Device**: the first hardware adapter with a feature level 11_0 Direct3D 12 device, else WARP
  (`IDXGIFactory4::EnumWarpAdapter`). One direct queue, one command list recorded per frame, 2 frames in flight
  (`NW_RHI_D3D12_FRAMES`), a fence per frame. Swap chain: `CreateSwapChainForHwnd` (FLIP_DISCARD) on the desktop,
  `CreateSwapChainForCoreWindow` (FLIP_SEQUENTIAL) in UWP, 2 RGBA8 buffers; resizing waits for the GPU.
- **Shaders**: the Direct3D 11 renderer's shader model 5.0 DXBC as is (Direct3D 12 takes 5.0; no dxc, no DXIL, no
  `dxil.dll`). Root signature: root CBV b0 for the vertex stage, root CBV b0 for the pixel stage, a descriptor table
  t0-t8, static point-clamp samplers s0-s8. The util shaders (`d3d11_util.hlsl`) have their own (b0, t0-t2, linear s0).
- **Pipeline states** are cached by everything they are built from (`NwDxPsoKey`: program + vertex build, layout,
  blend, mask, depth-stencil, fill, target formats and samples), up to 3072, open addressing.
- **Memory**: a 32 MB upload ring per frame. Uniforms (dirty or from an older ring epoch), stream buffers (a CPU copy
  each, placed in the ring at the draw that needs them), texture uploads. Static `nw_rhi_buffer_data` makes a new
  default-heap buffer (copy + barrier), the old one released after the GPU. A full ring submits and waits.
- **Descriptors**: one shader-visible CBV/SRV heap shared with Dear ImGui (256 fixed slots for it and
  `nw_rhi_texture_ui()`, then a per-frame part for draw tables); every texture has a CPU-only SRV that draws copy into a
  9-entry table (reused while the bindings do not change). Texel buffers get their SRV written at the draw.
- **States**: tracked per texture (all subresources together), barriers batched right before use. A unit bound to the
  texture being drawn into reads a null SRV, as Direct3D 11 unbinds it. Mip generation: one util draw per level
  (linear, a 2x2 average) with per-subresource barriers.
- **Readback** (`texture_read`, `window_read`): copy into a readback buffer, submit, wait, map.
- **Dear ImGui**: `imgui_impl_dx12` with the renderer's device, queue and heap (`rhi_d3d12.h`), recording into the
  renderer's command list (`nw_rhi_native(2)` binds the window first).

## Vulkan (`rhi_vk.c`)

- **Driver**: no Vulkan loader is linked. Desktop: `vulkan-1.dll` when it has a usable device, else Mesa Dozen
  (`vulkan_dzn.dll`, Vulkan on Direct3D 12) next to the exe or `NW_VK_ICD`, used as an ICD directly
  (`vk_icdNegotiateLoaderICDInterfaceVersion`, `vk_icdGetInstanceProcAddr`). UWP: Dozen from the package with
  `LoadPackagedLibrary` (Xbox has no loader). No Direct3D 12 GPU: `DZN_DEBUG=warp` is set before Dozen loads. Needs
  Vulkan 1.1 + `VK_KHR_dynamic_rendering` (no render pass objects). Dozen on WARP has no `VK_KHR_push_descriptor`, so
  descriptor sets come from a per-frame pool (4096 sets; full: submit, wait, reset) with immutable point samplers.
- **Shaders**: `gen_shaders.py --target vulkan` builds the same GLSL to SPIR-V with glslang only. Vulkan's framebuffer
  row 0 is the first row in memory like GL's row 0, so **nothing is flipped** for textures; the vertex stage only maps
  z to 0..w (a wrapper around `main()`). Uniform blocks: binding 16 (vertex), 17 (fragment); units 0-8 keep their
  bindings (texel buffers as uniform texel buffers). `d3d11_util.hlsl` goes through glslang's HLSL front end
  (`-DNW_VULKAN`: `ps_linear` takes its source size from the constants, Dozen fails to build `GetDimensions` of a mip
  level). The window is a framebuffer with row 0 at the top as in Direct3D, so the window rules match Direct3D's.
- **Memory, pipelines, layouts**: as Direct3D 12 (upload ring with epochs, a device buffer per static upload, a
  pipeline cache keyed the same way, image layouts tracked per texture with full barriers outside the rendering).
  Mip generation: `vkCmdBlitImage` (linear) per level.
- **The window**:
  - surface: `VK_KHR_win32_surface` + swap chain (FIFO, or IMMEDIATE/MAILBOX with vsync off), image acquired per
    frame; used with a real Vulkan driver, or with `NW_VK_PRESENT=surface`.
  - copy (Dozen, always in UWP: Dozen has no surface for a CoreWindow): the window is an RGBA8 image; `present` copies
    it to a host buffer, waits for the GPU and `UpdateSubresource`s it into a Direct3D 11 swap chain (HWND or
    CoreWindow). The simplest path that works on both; it costs a GPU wait and a ~3.5 MB CPU copy per 1280x720 frame.
- **Dear ImGui**: `imgui_impl_vulkan` with dynamic rendering, its functions from the renderer's driver
  (`ImGui_ImplVulkan_LoadFunctions` + `nw_rhi_vk_proc`; missing surface functions get a stub, ImGui's window helpers
  are not used), recording into the renderer's command buffer inside a rendering of the window (`nw_rhi_native(5)`);
  `nw_rhi_texture_ui()` gives an `ImGui_ImplVulkan_AddTexture` descriptor set.
- **Package**: `xbox/package.sh` ships `deps/mesa-uwp/bin/vulkan_dzn.dll` and the newer `dxil.dll` (1.9, from
  `tools/mesa-dzn`): Dozen crashed inside the Windows SDK's 1.6 validator on the first game shader. Mesa's OpenGL runs
  with the newer one too (checked: UWP title menu).

## Adding a backend

1. Add `src/gfx/rhi_<name>.c` defining `const NwRhiBackend nw_rhi_<name>` with `.prepare_window` and every entry of
   `NW_RHI_CALLS` (`NW_RHI_CALLS(ENTRY_X, ENTRY_V)` fills the table, see the end of `rhi_d3d11.c`). The compiler
   flags a missing one only as a NULL pointer, so check the table is complete.
2. In `rhi_impl.h` declare it under `#ifdef NW_RHI_HAVE_<NAME>`; in `rhi.c` put it in `nw_rhi_backends[]` under its
   `NwRhiKind` (`video.c`'s menu lists a kind as "not built in" until then). Set `nw_rhi_kind_default()` if it should
   become a platform's default.
3. A `cmake/<name>.cmake` like `d3d11.cmake`: the option, the sources, `NW_RHI_HAVE_<NAME>`, libraries, the Dear
   ImGui backend file (`imgui_impl_dx12.cpp` / `imgui_impl_vulkan.cpp`), and `include()` it from `CMakeLists.txt`.
4. Shaders: extend `tools/gen_shaders.py`. The SPIR-V step already exists (`glslang -V -R --aml --sdub NwUniforms`,
   bindings from `NW_BINDING(n)`) - Vulkan can embed it directly (keep `--flip-vert-y`-style flipping or use a
   negative-height viewport; see the conventions). D3D12 can reuse the DXBC as is, or run `dxc` on the same HLSL for
   DXIL (SM 6). The reflection data (cbuffer offsets, vertex inputs) is backend-independent.
5. `src/frontend/ui.cpp`: init/new-frame/render/shutdown for the new ImGui backend (the D3D11 branches show where);
   `nw_rhi_native()` hands it the device objects. `nw_rhi_texture_ui()` returns what that ImGui backend uses as
   `ImTextureID` (D3D11: the SRV; D3D12: a GPU descriptor handle; Vulkan: a `VkDescriptorSet`).
6. Things the D3D11 backend had to solve that a D3D12/Vulkan one will meet too: textures bound for reading while
   drawn into (D3D11 unbinds them at `set_target`; D3D12/Vulkan need barriers), buffers rewritten several times per
   frame (`nw_rhi_buffer_data` with `stream`: the old contents must stay valid for draws already recorded - use a
   per-frame upload ring), masked/scissored clears and scaled blits as draws, MSAA depth resolve, `texture_read` and
   `window_read` (synchronous readback; only used by `--capture` and the screenshot hooks).
7. Prove it: `--capture` (below) against an OpenGL run, the launcher shot and the window shot.

## Verification (2026-11-13, VMware SVGA 3D, Windows 11 VM)

Frame proof: `NuWorld.exe --data-dir <testdata> --renderer <r> --capture <vblanks> --capture-dir <dir>` (hidden
window, stepped clock, the game's GS-size frame read back with `nw_rhi_texture_read`), then
`python tools/capdiff.py <ref> <test> [--diff <dir>]` (per-PNG mean abs difference 0-255, max, % of pixels > 8 / > 32).
Results are kept under `crashwoc-xbox/verify/` (outside the repo):

| Test | Frames | Result |
|---|---|---|
| Title cut, D3D11 hardware vs OpenGL (`verify/gl_rhi` vs `verify/d3d_title`) | 397-401, 897-901 | **bit-exact**: mean 0.0000, max 0 on all 10 |
| Title cut, D3D11 WARP vs OpenGL (`NW_D3D11_DRIVER=warp`, `verify/warp_title`) | same | worst mean 0.0288, > 32 on <= 0.003% of pixels |
| In level 1 (`--level 1 --capture 1500,2500`), frames 2497-2501, D3D11 vs GL (`verify/gl_lvl` vs `verify/d3d_lvl`) | 5 | mean 0.0006-0.0008, max 14, > 8 on 0.004%, > 32 on 0% |
| In level 1, frames 1497-1501 | 4 | mean 0.28, but all > 32 pixels lie in Crash's box (x 252-340, y 185-252); outside it mean 0.0014, max 10 |
| OpenGL vs OpenGL, same command, two runs (`verify/gl_lvl` vs `verify/gl_lvl2`) | 9 | worst mean 0.062, max 135 (run-to-run noise) |
| Title cut, **Direct3D 12 (WARP)** vs D3D11 WARP (`verify/warp_title` vs `verify/d3d12_title`) | 397-401, 897-901 | **bit-exact**: mean 0.0000, max 0 on all 10 |
| Title cut, Direct3D 12 (WARP) vs OpenGL (`verify/gl_rhi` vs `verify/d3d12_title`) | same | 397-401 mean 0.0048, max 1; 897-901 worst mean 0.0288, > 32 on <= 0.003% (WARP's rasterization, as D3D11 WARP) |
| In level 1, Direct3D 12 (WARP) vs D3D11 WARP (`verify/warp_lvl` vs `verify/d3d12_lvl`) | 1497-1501, 2497-2501 | worst mean 0.0011, max 9 |
| In level 1, Direct3D 12 (WARP) vs OpenGL (`verify/gl_lvl` vs `verify/d3d12_lvl`) | 9 | 2497-2501 mean 0.052, max 13, > 32 on 0%; 1497-1501 mean 0.33 (Crash's idle animation, below) |
| Title cut, **Vulkan (Dozen on WARP)** vs OpenGL (`verify/gl_rhi` vs `verify/vk_title`) | 397-401, 897-901 | 397-401 mean 0.0048, max 1; 897-901 worst mean 0.0271, > 32 on <= 0.002% |
| In level 1, Vulkan vs Direct3D 12, both on WARP (`verify/d3d12_lvl` vs `verify/vk_lvl`) | 9 | worst mean 0.0031 (vs D3D11 WARP `verify/warp_lvl`: 0.0025) |
| In level 1, Vulkan vs OpenGL (`verify/gl_lvl` vs `verify/vk_lvl`) | 9 | 2497-2501 mean 0.052, max 13, > 32 on 0%; 1497-1501 mean 0.33 (idle animation) |

Frame 1500 shows Crash in a different idle animation (arms out under GL, looking aside under D3D11). It is the same
on every run of each renderer (D3D11 vs D3D11: mean 0.0018, max 17), and at 2500 both renderers draw the same skinned
Crash in the same pose to 0.0007. So the skinning is right: the difference is when the idle animation started. The
background level load runs in real time, so a renderer that takes a different time to load can shift game timing.
Diff images: `verify/d3d_title_diff/`, `verify/d3d_lvl_diff/`, `verify/warp_title_diff/`; side by side:
`verify/lvl1500_cmp.png`, `verify/lvl2500_cmp.png`.

Not covered by `--capture` (it draws at scale 1 without MSAA), so checked by screenshots:

- Title menu at `scale = 2`, `msaa = 4` (MSAA resolve, scaled blit to the window), window shots by both renderers:
  `verify/win_msaa_cmp.png` (OpenGL top, D3D11 bottom). Both are the same up to animation phase.
- Launcher (Dear ImGui on `imgui_impl_dx11`): `verify/launcher_d3d11.png`, OpenGL `verify/launcher_opengl.png`.
- Setup wizard (`--setup`): `verify/wizard_d3d11.png` vs `verify/wizard_opengl.png`, mean difference 0.14 (the
  animated background).
- Desktop game window, D3D11: `verify/desk_d3d11_game_crop.png`.
- **UWP package** (`out/NuWorld_0.8.316.1363_x64.msix, build 97199bc`, 0 imports outside the UWP API set), run in the AppContainer
  on this PC with `renderer = "auto"`, so D3D11 by default, without the Mesa env lines: log
  `renderer: d3d11: Direct3D 11 (feature level 11_0), VMware SVGA 3D`, launcher `verify/uwp_d3d11_launcher.png`,
  title menu 30 s later `verify/uwp_d3d11_title_menu.png`. The same package with `--renderer opengl` (Mesa on WARP)
  still runs: `verify/uwp_gl_title_menu.png`.

- **Direct3D 12 and Vulkan, desktop** (2026-11-13, build 75e3b7d; this VM has no Direct3D 12 GPU, so Direct3D 12 and
  Dozen run on WARP): title menu at `scale = 2`, `msaa = 4` `verify/win_msaa_d3d12.png`, `verify/win_msaa_vulkan.png`;
  launcher `verify/launcher_d3d12.png`, `verify/launcher_vulkan.png`; setup wizard (an `nw_rhi_texture_ui()` image)
  `verify/wizard_d3d12.png`, `verify/wizard_vulkan.png`; Vulkan's three in one: `verify/vulkan_shots.png`.
- **Direct3D 12 and Vulkan, UWP package** (`out/NuWorld_0.8.317.4_x64.msix`, build 1ea30f3, 0 imports outside the UWP
  API set) in the AppContainer on this PC, `--renderer d3d12` / `--renderer vulkan` in `args.txt`: logs
  `renderer: d3d12: Direct3D 12 (feature level 11_0), Microsoft Basic Render Driver (WARP)` and
  `renderer: vulkan: Vulkan 1.2 (Microsoft Direct3D12 (Microsoft Basic Render Driver), Mesa Dozen), shown through
  Direct3D 11`; launcher then title menu 30 s later: `verify/uwp_d3d12_launcher.png`, `verify/uwp_d3d12_title.png`,
  `verify/uwp_vulkan_launcher.png`, `verify/uwp_vulkan_title.png`; OpenGL with the newer `dxil.dll`
  `verify/uwp_gl_newdxil_title.png` (`env LIBGL_ALWAYS_SOFTWARE=true`, `env GALLIUM_DRIVER=d3d12`); the three titles
  side by side `verify/uwp_titles_cmp.png`.

Test hooks: `NW_LAUNCHER_SHOT=<png>` (+ `NW_LAUNCHER_EXIT=1|play`, `NW_LAUNCHER_PAGE=<n>`) and
`NW_WINDOW_SHOT=<png>` (+ `NW_WINDOW_SHOT_MS`, default 30000) save the window through `nw_rhi_window_read()` on any
renderer. On the UWP build that is the only way to get a clean frame.

## Gaps and risks

- **Not run on real Xbox hardware yet**, only as the same UWP package on a Windows 11 PC. The swap chain is
  `CreateSwapChainForCoreWindow` (FLIP_SEQUENTIAL, 2 buffers, RGBA8). The programs are shader model 5.0, so the
  device must reach **feature level 11_0**; otherwise it falls back to WARP (slow). A UWP *app* on Xbox gets a smaller
  GPU share than one declared as a *game*. Check that the package's app type is set to Game in Dev Mode.
- An MSAA depth resolve carries depth (sample 0), not stencil (`nw_d3d_blit`); the game's resolves have not needed it.
- `vsync = adaptive` is not available on D3D11 (`nw_rhi_set_vsync(-1)` fails and the caller keeps vsync on).
- Building the D3D11 renderer needs `tools/glslang`, `tools/spirv-cross` (Khronos release binaries, outside the repo)
  and the Windows SDK's `fxc.exe`. Without them `cmake/d3d11.cmake` turns `NUWORLD_D3D11` off and only OpenGL is built.
  Direct3D 12 needs nothing more (it takes Direct3D 11's DXBC; off with it). Vulkan needs `tools/vulkan-headers`
  (`git clone --depth 1 https://github.com/KhronosGroup/Vulkan-Headers`) besides glslang and spirv-cross. A desktop
  Dozen for PCs without a Vulkan driver: `tools/mesa-dzn/` = `x64/vulkan_dzn.dll`, `x64/dxil.dll` (and the ICD json)
  from the pal1000/mesa-dist-win 26.2.6 `release-msvc` archive; the build copies both next to `NuWorld.exe`.
- `imgui_impl_dx11` compiles its two small shaders at run time with `d3dcompiler_47.dll` (part of the UWP API set).
- At most 8 distinct vertex layouts and 64 cached blend / depth-stencil state objects. More are logged, or created
  per draw.
- **Direct3D 12 has only run on WARP** (this VM has no Direct3D 12 GPU), never on a GPU or the Xbox. It only needs
  feature level 11_0 and shader model 5.0 DXBC, which every Xbox One GPU has.
- **Vulkan is experimental on Xbox**: Dozen translates it to Direct3D 12 and every frame waits for the GPU and goes
  through the CPU into a Direct3D 11 swap chain. Expect it to be the slowest renderer; nothing of it has run on Xbox.
- **Dozen's own win32 surface does not work here** (Dozen 26.2.6 on WARP): once its swap chain exists the next
  pipeline fails to build (`VK_ERROR_OUT_OF_HOST_MEMORY`, before any GPU work) and `vkQueuePresentKHR` returns
  `VK_ERROR_OUT_OF_DEVICE_MEMORY`, then the process crashes in D3D12. The same pipelines build without the swap chain.
  Not found why (no D3D12 debug layer on this VM; ~15 tries), so Dozen always presents by copy; the surface path is
  only for a real Vulkan driver and has **not been run on one** (none here). `NW_VK_PRESENT=surface` forces it.
- Vulkan needs `VK_KHR_dynamic_rendering` (Vulkan 1.3 core, Dozen has it); a driver without it is refused.
- Vulkan: at most 4096 descriptor sets a frame before a GPU wait; both new renderers cache at most 3072 pipelines
  (more are made per draw and released after the GPU).
- `vsync = adaptive` is not available on Direct3D 12 or Vulkan either.
- Dozen prints `WARNING: dzn is not a conformant Vulkan implementation` and Dear ImGui's loader logs the surface /
  core 1.3 functions Dozen lacks (stubbed, unused).
- Not compared frame by frame: the debug overlay's wireframe mode and the free-camera scene viewer (`--scene`) under
  D3D11. Both use the same calls as the game, but nobody has looked at them.

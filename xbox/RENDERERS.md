# Renderers

NuWorld draws through one small GPU interface, `src/gfx/rhi.h` (render hardware interface). Two backends implement
it today:

| Renderer | File | Where | Default |
|---|---|---|---|
| OpenGL 3.3 core | `src/gfx/rhi_gl.c` | desktop; UWP through Mesa (`opengl32.dll` on D3D12) | desktop |
| Direct3D 11 (feature level 11_0+) | `src/gfx/rhi_d3d11.c` | desktop (HWND swap chain) and UWP (CoreWindow swap chain) | UWP / Xbox |

Pick one with `settings.toml` `[video] renderer = "auto" | "opengl" | "d3d11"`, the Settings / F1 menu's
*Video -> Renderer* entry (takes effect at the next start), or `--renderer opengl|d3d11` (also `gl`, `dx11`).
`auto` is `nw_rhi_kind_default()`: OpenGL on a PC, Direct3D 11 in the UWP build. A renderer that is not built in
falls back to the default with a log line. `NW_D3D11_DRIVER=warp` forces Microsoft's software rasterizer;
`NW_D3D11_DEBUG=1` asks for the debug layer. The Direct3D 11 renderer needs no OpenGL at all: no GL context and no
`SDL_WINDOW_OPENGL` (`nw_rhi_prepare_window()` returns 0), so Mesa is not even loaded when it runs on Xbox.

## The abstraction

Who calls it:

- `src/gfx/backend.c` (targets, blits, clears, the window frame) and `src/gfx/vu.c` (the VU1 microcode as GPU
  programs) - all game drawing.
- `src/frontend/ui.cpp` (Dear ImGui: `imgui_impl_opengl3` or `imgui_impl_dx11` on the device from
  `nw_rhi_native()`), `launcher.cpp`, `wizard.cpp` (textures via `nw_rhi_texture_ui()`), `help.c`, `platform.c`,
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
cmake/d3d11.cmake    NUWORLD_D3D11 option, shader generation, links d3d11 dxgi dxguid (+ d3dcompiler for ImGui)
tools/gen_shaders.py GLSL -> SPIR-V (glslang) -> HLSL (SPIRV-Cross) -> DXBC (fxc), embedded as build/gen/*.c
```

What Direct3D 11 lacks and is drawn instead (`d3d11_util.hlsl`, one full-viewport triangle): scaled colour blits
(point/linear), depth blits and MSAA depth resolves (`SV_Depth`), and clears with a colour mask, a stencil write mask
or a scissor. Same-size copies use `CopySubresourceRegion`, same-size MSAA colour resolves `ResolveSubresource`.
GL's integer-to-float vertex formats do not exist in D3D: the colour comes in as `uvec4` and the int16 texcoords as
`ivec4` (`NW_SPIRV` / `NW_TEX_TYPE` in `vu.vs.glsl`), so each vertex program is built twice (float and int16
texcoords) and the backend picks the build from the vertex layout. Inputs a layout lacks read GL's (0, 0, 0, 1)
from a small dummy vertex buffer.

## Adding a backend (Direct3D 12, Vulkan)

1. Add `src/gfx/rhi_<name>.c` defining `const NwRhiBackend nw_rhi_<name>` with `.prepare_window` and every entry of
   `NW_RHI_CALLS` (`NW_RHI_CALLS(ENTRY_X, ENTRY_V)` fills the table, see the end of `rhi_d3d11.c`). The compiler
   flags a missing one only as a NULL pointer, so check the table is complete.
2. In `rhi_impl.h` declare it under `#ifdef NW_RHI_HAVE_<NAME>`; in `rhi.c` put it in `nw_rhi_backends[]` under its
   `NwRhiKind` (`NW_RHI_D3D12`, `NW_RHI_VULKAN` already exist and already parse as `d3d12`/`dx12`, `vulkan`/`vk`;
   `video.c`'s menu lists them as "not built in" until then). Set `nw_rhi_kind_default()` if it should become a
   platform's default.
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
- Desktop game window, D3D11: `verify/desk_d3d11_game_crop.png`.
- **UWP package** (`out/NuWorld_0.8.316.1356_x64.msix`, 0 imports outside the UWP API set), run in the AppContainer
  on this PC with `renderer = "auto"`, so D3D11 by default, without the Mesa env lines: log
  `renderer: d3d11: Direct3D 11 (feature level 11_0), VMware SVGA 3D`, launcher `verify/uwp_d3d11_launcher.png`,
  title menu 30 s later `verify/uwp_d3d11_title_menu.png`. The same package with `--renderer opengl` (Mesa on WARP)
  still runs: `verify/uwp_gl_title_menu.png`.

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
- `imgui_impl_dx11` compiles its two small shaders at run time with `d3dcompiler_47.dll` (part of the UWP API set).
- At most 8 distinct vertex layouts and 64 cached blend / depth-stencil state objects. More are logged, or created
  per draw.
- Not compared frame by frame: the debug overlay's wireframe mode and the free-camera scene viewer (`--scene`) under
  D3D11. Both use the same calls as the game, but nobody has looked at them.

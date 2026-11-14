# NuWorld on Xbox (Dev Mode)

NuWorld built as a native x64 UWP app for an Xbox One / Series X|S in Developer Mode. It's the same game code as the
PC build, compiled for the Xbox's sandbox. There's no emulator.

| Part | On Xbox |
|---|---|
| Game code (decomp + port) | Built with llvm-mingw (clang, UCRT), the same sources as the PC build, `NW_UWP` defined |
| Window, input, controller | SDL3-uwp (XboxEmulationHub fork, CoreWindow, Windows.Gaming.Input) |
| Renderer | Direct3D 11 by default (`src/gfx/rhi_d3d11.c`, swap chain on the CoreWindow, no OpenGL); also Direct3D 12, OpenGL (Mesa) and Vulkan (Mesa Dozen, experimental), `renderer = "..."` / `--renderer ...` (table below, [RENDERERS.md](RENDERERS.md)) |
| Sound | XAudio2 (`src/platform/audio.c`; SDL's WASAPI device cannot open in this app) |
| Data folder | `LocalState\crashwoc-nuworld\` (settings, saves, log, imported disc) |

### Renderers

| Renderer (`renderer =`) | How it runs on Xbox | Verified (PC; this VM has no Direct3D 12 GPU) | Gaps |
|---|---|---|---|
| `d3d11` (default) | native Direct3D 11, CoreWindow swap chain | desktop: bit-exact vs OpenGL; UWP: title menu | not on real Xbox yet |
| `d3d12` | native Direct3D 12, CoreWindow swap chain (WARP when no D3D12 GPU) | desktop (WARP): bit-exact vs D3D11 WARP, title + in level; UWP (WARP): title menu | never on a GPU / Xbox yet |
| `opengl` | Mesa 26.2.2 UWP `opengl32.dll`: OpenGL 4.6 on Direct3D 12 | desktop: the reference; UWP (WARP): title menu | slower than native |
| `vulkan` | Mesa Dozen `vulkan_dzn.dll` (Vulkan on Direct3D 12), offscreen, each frame copied through the CPU into a Direct3D 11 swap chain | desktop (Dozen on WARP): matches D3D12 to 0.003 mean, title + in level; UWP (WARP): title menu | **experimental**, slowest: a GPU wait + CPU copy per frame; not on Xbox yet |

## Build (Windows)

Needs llvm-mingw (UCRT), CMake + Ninja, Python 3.11+ with libclang, Visual Studio 2022 with the UWP workload (for
SDL3-uwp, `editbin`, `makeappx`, `signtool`), and the workspace layout:

```
<workspace>/
  source/   this repository
  deps/     sdl3-uwp (git clone -b uwp-3.4.20 https://github.com/XboxEmulationHub/SDL3-uwp)
            sdl3-uwp-out/SDL3-UWP/ (its build)  mesa-uwp/bin/ (XboxEmulationHub/mesa-uwp release 26.2.2)
  tools/    llvm-mingw/  py/ (cmake, ninja, libclang)  env.sh  build_sdl3uwp.bat  check_uwp_imports.py
            glslang/ spirv-cross/ (shaders)  vulkan-headers/ (KhronosGroup/Vulkan-Headers)
            mesa-dzn/ (vulkan_dzn.dll + dxil.dll from pal1000/mesa-dist-win 26.2.6 release-msvc, x64)
```

```
source tools/env.sh
tools/build_sdl3uwp.bat                     # once: SDL3-uwp with WGL (OpenGL) on the CoreWindow
cd source && cmake --preset xbox -DPython3_EXECUTABLE=<python> && cmake --build --preset xbox
bash xbox/package.sh build/xbox ../out      # -> out/NuWorld_<version>_x64.msix, NuWorld.cer, Dependencies/
```

`package.sh` fails the import check if the exe calls anything outside the UWP API set
(`tools/check_uwp_imports.py`).

## Install on the Xbox

1. Xbox in Developer Mode, Dev Home > Remote Access on, note the Device Portal address.
2. On the PC open `https://<xbox-ip>:11443`, then **Add**, choose `NuWorld_<version>_x64.msix`, add
   `Dependencies\Microsoft.VCLibs.x64.14.00.appx`, then **Start**.
3. In Dev Home highlight **CrashWOC: NuWorld**, press Menu > **View details** > App type: **Game** (more memory and
   the full GPU).
4. Your disc image (PAL v1.03 or USA v1.00, `.iso` or `.bin` + `.cue`):
   - **USB drive**: copy it to the drive root or a `NuWorld` folder. For NTFS drives, let apps read it once on a PC:
     `icacls X:\ /grant "*S-1-15-2-1:(OI)(CI)RX"`. The first start lists it under *USB drive*, then **Import**.
   - **or Device Portal**: File explorer > LocalAppData > NuWorld... > LocalState > crashwoc-nuworld > import, upload
     the image, then **Import** in the list.

The log is `LocalState\crashwoc-nuworld\nuworld.log`, and `stdout.txt` holds the game's debug output. Both can be
downloaded with the Device Portal file explorer.

## Testing on a PC

`xbox/run_pc.ps1 -Layout out\pkg -Data <folder with disc\>` registers the package (Windows Developer Mode) and runs it
in the same AppContainer sandbox. `args.txt` in the data folder stands in for a command line (one argument per line).
`env NAME=VALUE` lines set environment variables. With the OpenGL renderer, a PC without Direct3D 12 hardware (VMs)
needs `env LIBGL_ALWAYS_SOFTWARE=true` and `env GALLIUM_DRIVER=d3d12` (Mesa on WARP); Direct3D 11 needs nothing,
Direct3D 12 and Vulkan (Dozen) switch to WARP by themselves (logged).
`env NW_WINDOW_SHOT=<png>` saves the game window's frame 30 s in (`NW_WINDOW_SHOT_MS`), `env NW_LAUNCHER_SHOT=<png>`
the launcher's.

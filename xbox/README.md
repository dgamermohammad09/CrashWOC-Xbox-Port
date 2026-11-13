# NuWorld on Xbox (Dev Mode)

NuWorld built as a native x64 UWP app for an Xbox One / Series X|S in Developer Mode. It's the same game code as the
PC build, compiled for the Xbox's sandbox. There's no emulator.

| Part | On Xbox |
|---|---|
| Game code (decomp + port) | Built with llvm-mingw (clang, UCRT), the same sources as the PC build, `NW_UWP` defined |
| Window, input, controller | SDL3-uwp (XboxEmulationHub fork, CoreWindow, Windows.Gaming.Input) |
| OpenGL renderer | Mesa 26.2.2 UWP build (`opengl32.dll` + `libgallium_wgl.dll`): OpenGL 4.6 on Direct3D 12 |
| Sound | XAudio2 (`src/platform/audio.c`; SDL's WASAPI device cannot open in this app) |
| Data folder | `LocalState\crashwoc-nuworld\` (settings, saves, log, imported disc) |

## Build (Windows)

Needs llvm-mingw (UCRT), CMake + Ninja, Python 3.11+ with libclang, Visual Studio 2022 with the UWP workload (for
SDL3-uwp, `editbin`, `makeappx`, `signtool`), and the workspace layout:

```
<workspace>/
  source/   this repository
  deps/     sdl3-uwp (git clone -b uwp-3.4.20 https://github.com/XboxEmulationHub/SDL3-uwp)
            sdl3-uwp-out/SDL3-UWP/ (its build)  mesa-uwp/bin/ (XboxEmulationHub/mesa-uwp release 26.2.2)
  tools/    llvm-mingw/  py/ (cmake, ninja, libclang)  env.sh  build_sdl3uwp.bat  check_uwp_imports.py
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
`env NAME=VALUE` lines set environment variables. A PC without Direct3D 12 hardware (VMs) needs
`env LIBGL_ALWAYS_SOFTWARE=true` (Mesa on WARP).

@echo off
rem Builds SDL3-uwp (XboxEmulationHub fork, VisualC-WinRT project: WGL via Mesa on the CoreWindow) for x64 Release.
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
rem NuWorld's change: Xbox file/folder pickers copy into the app; folder creation inside the sandbox (xbox/patches/sdl3-uwp.patch)
pushd "%~dp0..\deps\sdl3-uwp" || exit /b 1
git apply --check "%~dp0..\source\xbox\patches\sdl3-uwp.patch" 2>nul && git apply "%~dp0..\source\xbox\patches\sdl3-uwp.patch"
popd
cd /d "%~dp0..\deps\sdl3-uwp\VisualC-WinRT" || exit /b 1
msbuild SDL-UWP.vcxproj /m:1 /nologo /v:minimal /p:Configuration=Release /p:Platform=x64 /p:PlatformToolset=v143 /p:WindowsTargetPlatformVersion=10.0.22621.0 /p:OutDir=%~dp0..\deps\sdl3-uwp-out\ /p:IntDir=%~dp0..\deps\sdl3-uwp-obj\
exit /b %errorlevel%

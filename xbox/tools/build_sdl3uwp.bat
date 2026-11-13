@echo off
rem Builds SDL3-uwp (XboxEmulationHub fork, VisualC-WinRT project: WGL via Mesa on the CoreWindow) for x64 Release.
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
cd /d "%~dp0..\deps\sdl3-uwp\VisualC-WinRT" || exit /b 1
msbuild SDL-UWP.vcxproj /m:1 /nologo /v:minimal /p:Configuration=Release /p:Platform=x64 /p:PlatformToolset=v143 /p:WindowsTargetPlatformVersion=10.0.22621.0 /p:OutDir=%~dp0..\deps\sdl3-uwp-out\ /p:IntDir=%~dp0..\deps\sdl3-uwp-obj\
exit /b %errorlevel%

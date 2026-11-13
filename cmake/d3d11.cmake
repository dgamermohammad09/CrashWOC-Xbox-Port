# The Direct3D 11 renderer (src/gfx/rhi_d3d11.c, xbox/RENDERERS.md). Its programs are the renderer's GLSL
# (src/gfx/shaders/) built at configure-free build time by tools/gen_shaders.py: glslang -> SPIR-V, SPIRV-Cross ->
# HLSL, the Windows SDK's fxc -> DXBC, embedded as build/gen/d3d11_shaders.c.
if(NOT WIN32)
  return()
endif()
file(GLOB nw_fxc_found "C:/Program Files (x86)/Windows Kits/10/bin/10.*/x64/fxc.exe")
list(SORT nw_fxc_found)
list(REVERSE nw_fxc_found)
list(GET nw_fxc_found 0 nw_fxc_default)
set(NW_GLSLANG "${CMAKE_CURRENT_SOURCE_DIR}/../tools/glslang/bin/glslang.exe" CACHE FILEPATH
  "glslang (KhronosGroup/glslang release) for the Direct3D 11 renderer's shaders")
set(NW_SPIRV_CROSS "${CMAKE_CURRENT_SOURCE_DIR}/../tools/spirv-cross/bin/spirv-cross.exe" CACHE FILEPATH
  "spirv-cross (KhronosGroup/SPIRV-Cross) for the Direct3D 11 renderer's shaders")
set(NW_FXC "${nw_fxc_default}" CACHE FILEPATH "the Windows SDK's fxc.exe for the Direct3D 11 renderer's shaders")
if(EXISTS "${NW_GLSLANG}" AND EXISTS "${NW_SPIRV_CROSS}" AND EXISTS "${NW_FXC}")
  set(nw_d3d11_default ON)
else()
  set(nw_d3d11_default OFF)
endif()
option(NUWORLD_D3D11 "The Direct3D 11 renderer (needs NW_GLSLANG, NW_SPIRV_CROSS, NW_FXC)" ${nw_d3d11_default})
if(NOT NUWORLD_D3D11)
  message(STATUS "Direct3D 11 renderer: off (glslang, spirv-cross or fxc not found)")
  return()
endif()
if(NOT Python3_EXECUTABLE)
  find_package(Python3 3.11 REQUIRED COMPONENTS Interpreter)
endif()
set(nw_d3d11_out ${CMAKE_CURRENT_BINARY_DIR}/gen/d3d11_shaders.c)
set(nw_shaders ${CMAKE_CURRENT_SOURCE_DIR}/src/gfx/shaders)
add_custom_command(OUTPUT ${nw_d3d11_out}
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_shaders.py --glslang ${NW_GLSLANG}
    --spirv-cross ${NW_SPIRV_CROSS} --fxc ${NW_FXC} --shaders ${nw_shaders}
    --work ${CMAKE_CURRENT_BINARY_DIR}/gen/d3d11 --out ${nw_d3d11_out}
  DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_shaders.py ${nw_shaders}/vu.vs.glsl ${nw_shaders}/vu.fs.glsl
    ${nw_shaders}/gem.fs.glsl ${nw_shaders}/d3d11_util.hlsl
  COMMENT "Direct3D 11 shaders from src/gfx/shaders"
  VERBATIM)
target_sources(crashwoc-nuworld PRIVATE src/gfx/rhi_d3d11.c ${nw_d3d11_out})
target_compile_definitions(crashwoc-nuworld PRIVATE NW_RHI_HAVE_D3D11)
target_link_libraries(crashwoc-nuworld PRIVATE d3d11 dxgi dxguid)
# Dear ImGui's Direct3D 11 backend (it compiles its two small shaders with d3dcompiler_47 at start).
target_sources(nuworld_imgui PRIVATE ${imgui_SOURCE_DIR}/backends/imgui_impl_dx11.cpp)
target_link_libraries(nuworld_imgui PUBLIC d3d11 dxgi d3dcompiler)
message(STATUS "Direct3D 11 renderer: on (${NW_FXC})")

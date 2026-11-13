# The Vulkan renderer (src/gfx/rhi_vk.c, xbox/RENDERERS.md). Its programs are the renderer's GLSL built to SPIR-V by
# tools/gen_shaders.py --target vulkan (glslang alone; d3d11_util.hlsl through glslang's HLSL front end), embedded as
# build/gen/vk_shaders.c. It needs the Khronos Vulkan-Headers (tools/vulkan-headers), no Vulkan SDK or loader library:
# the driver is loaded at run time (vulkan-1.dll, or Mesa's Dozen vulkan_dzn.dll as an ICD; the UWP package ships
# Dozen). Windows only: its UWP window is presented through Direct3D 11.
if(NOT WIN32)
  return()
endif()
set(NW_VULKAN_HEADERS "${CMAKE_CURRENT_SOURCE_DIR}/../tools/vulkan-headers/include" CACHE PATH
  "Vulkan-Headers' include folder (KhronosGroup/Vulkan-Headers) for the Vulkan renderer")
set(NW_DZN_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../tools/mesa-dzn" CACHE PATH
  "a desktop Mesa Dozen (vulkan_dzn.dll, dxil.dll) copied next to NuWorld.exe for the Vulkan renderer")
if(EXISTS "${NW_VULKAN_HEADERS}/vulkan/vulkan.h" AND EXISTS "${NW_GLSLANG}" AND EXISTS "${NW_SPIRV_CROSS}")
  set(nw_vulkan_default ON)
else()
  set(nw_vulkan_default OFF)
endif()
option(NUWORLD_VULKAN "The Vulkan renderer (needs NW_VULKAN_HEADERS, NW_GLSLANG, NW_SPIRV_CROSS)" ${nw_vulkan_default})
if(NOT NUWORLD_VULKAN)
  message(STATUS "Vulkan renderer: off (Vulkan-Headers, glslang or spirv-cross not found)")
  return()
endif()
if(NOT Python3_EXECUTABLE)
  find_package(Python3 3.11 REQUIRED COMPONENTS Interpreter)
endif()
set(nw_vk_out ${CMAKE_CURRENT_BINARY_DIR}/gen/vk_shaders.c)
set(nw_shaders ${CMAKE_CURRENT_SOURCE_DIR}/src/gfx/shaders)
add_custom_command(OUTPUT ${nw_vk_out}
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_shaders.py --target vulkan
    --glslang ${NW_GLSLANG} --spirv-cross ${NW_SPIRV_CROSS} --shaders ${nw_shaders}
    --work ${CMAKE_CURRENT_BINARY_DIR}/gen/vulkan --out ${nw_vk_out}
  DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_shaders.py ${nw_shaders}/vu.vs.glsl ${nw_shaders}/vu.fs.glsl
    ${nw_shaders}/gem.fs.glsl ${nw_shaders}/d3d11_util.hlsl
  COMMENT "Vulkan shaders from src/gfx/shaders"
  VERBATIM)
target_sources(crashwoc-nuworld PRIVATE src/gfx/rhi_vk.c ${nw_vk_out})
target_compile_definitions(crashwoc-nuworld PRIVATE NW_RHI_HAVE_VULKAN)
target_include_directories(crashwoc-nuworld SYSTEM PRIVATE ${NW_VULKAN_HEADERS})
target_link_libraries(crashwoc-nuworld PRIVATE d3d11 d3d12 dxgi dxguid)
# Dear ImGui's Vulkan backend, its functions from the renderer's driver (ImGui_ImplVulkan_LoadFunctions).
target_sources(nuworld_imgui PRIVATE ${imgui_SOURCE_DIR}/backends/imgui_impl_vulkan.cpp)
target_include_directories(nuworld_imgui SYSTEM PUBLIC ${NW_VULKAN_HEADERS})
target_compile_definitions(nuworld_imgui PUBLIC IMGUI_IMPL_VULKAN_NO_PROTOTYPES)
if(NOT NUWORLD_UWP AND EXISTS "${NW_DZN_DIR}/vulkan_dzn.dll")
  add_custom_command(TARGET crashwoc-nuworld POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${NW_DZN_DIR}/vulkan_dzn.dll ${NW_DZN_DIR}/dxil.dll
      $<TARGET_FILE_DIR:crashwoc-nuworld>
    VERBATIM)
endif()
message(STATUS "Vulkan renderer: on (${NW_VULKAN_HEADERS})")

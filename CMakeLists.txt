cmake_minimum_required(VERSION 3.20)
project(RetroPark LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)

if (MSVC)
  add_compile_options(/W4 /permissive-)
endif()

include(cmake/Shaders.cmake)

include(FetchContent)
# doctest v2.4.11 and nlohmann_json v3.11.3 both declare a cmake_minimum_required
# below 3.5, which CMake >= 4.0 refuses to configure. This policy floor (as
# suggested by CMake's own error message) lets FetchContent build them anyway.
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
FetchContent_Declare(doctest
  GIT_REPOSITORY https://github.com/doctest/doctest.git
  GIT_TAG v2.4.11)
FetchContent_Declare(nlohmann_json
  GIT_REPOSITORY https://github.com/nlohmann/json.git
  GIT_TAG v3.11.3)
FetchContent_MakeAvailable(doctest nlohmann_json)

# Library target — sources appended by later tasks.
add_library(retropark STATIC
  src/loader/Manifest.cpp
  src/loader/CoreLoader.cpp
  src/loader/Win32CoreModule.cpp
  src/render/SurfaceRing.cpp
  src/render/FramebufferCopy.cpp)
target_sources(retropark PRIVATE
  src/render/d3d11/D3D11Backend.cpp
  src/render/d3d11/D3D11Compositor.cpp
  src/render/vulkan/VulkanBackend.cpp
  src/render/vulkan/VulkanCompositor.cpp
  src/runtime/BackendFactory.cpp
  src/runtime/Runtime.cpp
  src/audio/AudioPool.cpp
  src/audio/XAudio2Output.cpp
  src/net/NetProtocol.cpp
  src/net/LoopbackTransport.cpp
  src/net/TcpTransport.cpp
  src/net/NetSession.cpp
  src/net/RollbackPredict.cpp)
target_include_directories(retropark PUBLIC include)
target_include_directories(retropark PRIVATE src)
target_compile_features(retropark PUBLIC cxx_std_17)
target_link_libraries(retropark PUBLIC nlohmann_json::nlohmann_json)
target_link_libraries(retropark PUBLIC Vulkan::Vulkan)
if (WIN32)
  target_link_libraries(retropark PUBLIC d3d11 dxgi dxguid d3dcompiler xaudio2 ole32 ws2_32)
endif()

# Vulkan compositor shaders, embedded as SPIR-V headers (see cmake/Shaders.cmake).
set(VK_SHADER_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src/render/vulkan/shaders")
set(VK_GEN_DIR "${CMAKE_BINARY_DIR}/gen")
retropark_embed_shader("${VK_SHADER_DIR}/fullscreen.vert" vert "${VK_GEN_DIR}/vk_fullscreen_vert_generated.h" vk_fullscreen_vert)
retropark_embed_shader("${VK_SHADER_DIR}/sample.frag" frag "${VK_GEN_DIR}/vk_sample_frag_generated.h" vk_sample_frag)
retropark_embed_shader("${VK_SHADER_DIR}/overlay.vert" vert "${VK_GEN_DIR}/vk_overlay_vert_generated.h" vk_overlay_vert)
retropark_embed_shader("${VK_SHADER_DIR}/overlay.frag" frag "${VK_GEN_DIR}/vk_overlay_frag_generated.h" vk_overlay_frag)
add_custom_target(gen_vk_shaders DEPENDS
  "${VK_GEN_DIR}/vk_fullscreen_vert_generated.h"
  "${VK_GEN_DIR}/vk_sample_frag_generated.h"
  "${VK_GEN_DIR}/vk_overlay_vert_generated.h"
  "${VK_GEN_DIR}/vk_overlay_frag_generated.h")
add_dependencies(retropark gen_vk_shaders)
target_include_directories(retropark PRIVATE "${VK_GEN_DIR}")

add_subdirectory(cores/refcore_present)
add_subdirectory(cores/refcore_present_vk)
add_subdirectory(cores/refcore_driven)
add_subdirectory(cores/refcore_rollback)

add_library(retropark_libretro_convert STATIC cores/libretro_shim/PixelConvert.cpp)
target_include_directories(retropark_libretro_convert PUBLIC cores/libretro_shim external/libretro)

# The libretro shim driven core (links the convert lib defined above).
add_subdirectory(cores/libretro_shim)

# The windowed harness depends on LibretroShim (above) for its --content path.
add_subdirectory(harness/windowed)

enable_testing()
add_subdirectory(tests)

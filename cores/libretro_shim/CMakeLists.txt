add_library(LibretroShim SHARED
  LibretroShim.cpp
  HwRenderGL.cpp
  ${CMAKE_SOURCE_DIR}/src/render/gl/GLContext.cpp)
target_include_directories(LibretroShim PRIVATE
  ${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/external/libretro ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(LibretroShim PRIVATE retropark_libretro_convert opengl32 gdi32)

set(SHIM_OUT $<TARGET_FILE_DIR:LibretroShim>/cores/libretro_shim)
add_custom_command(TARGET LibretroShim POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E make_directory ${SHIM_OUT}
  COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:LibretroShim> ${SHIM_OUT}/
  COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_SOURCE_DIR}/core.json ${SHIM_OUT}/)

# Copy the real core next to the shim IF it was downloaded (never required for build).
if (EXISTS ${CMAKE_SOURCE_DIR}/external/libretro-cores/fceumm_libretro.dll)
  add_custom_command(TARGET LibretroShim POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy
      ${CMAKE_SOURCE_DIR}/external/libretro-cores/fceumm_libretro.dll ${SHIM_OUT}/)
endif()

# HW-render (B1) N64 proof: if Mupen64Plus-Next was downloaded, stage a sibling shim dir configured to load
# it (a core.json whose libretro_core = the N64 core), so tests/test_hwrender_n64_e2e can load it. Never
# required for build; the core DLL is git-ignored.
if (EXISTS ${CMAKE_SOURCE_DIR}/external/libretro-cores/mupen64plus_next_libretro.dll)
  set(N64_OUT ${CMAKE_BINARY_DIR}/cores/libretro_shim_n64)
  add_custom_command(TARGET LibretroShim POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory ${N64_OUT}
    COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:LibretroShim> ${N64_OUT}/
    COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_SOURCE_DIR}/core.mupen64.json ${N64_OUT}/core.json
    COMMAND ${CMAKE_COMMAND} -E copy
      ${CMAKE_SOURCE_DIR}/external/libretro-cores/mupen64plus_next_libretro.dll ${N64_OUT}/)
endif()

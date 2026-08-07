add_library(LibretroShim SHARED LibretroShim.cpp)
target_include_directories(LibretroShim PRIVATE
  ${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/external/libretro)
target_link_libraries(LibretroShim PRIVATE retropark_libretro_convert)

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

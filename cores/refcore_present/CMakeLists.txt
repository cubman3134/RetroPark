add_library(refcore_present SHARED RefCore.cpp)
target_include_directories(refcore_present PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(refcore_present PRIVATE d3d11 dxgi dxguid)
# Emit dll + manifest next to a cores/ dir under the build tree for the harness/e2e.
set(RP_CORE_OUT $<TARGET_FILE_DIR:refcore_present>/cores/refcore_present)
add_custom_command(TARGET refcore_present POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E make_directory ${RP_CORE_OUT}
  COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:refcore_present> ${RP_CORE_OUT}/
  COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_SOURCE_DIR}/core.json ${RP_CORE_OUT}/)

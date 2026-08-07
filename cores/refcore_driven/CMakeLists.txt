add_library(refcore_driven SHARED RefCoreDriven.cpp)
target_include_directories(refcore_driven PRIVATE ${CMAKE_SOURCE_DIR}/include)
# Emit dll + manifest next to a cores/ dir under the build tree for the harness/e2e.
set(RP_CORE_OUT $<TARGET_FILE_DIR:refcore_driven>/cores/refcore_driven)
add_custom_command(TARGET refcore_driven POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E make_directory ${RP_CORE_OUT}
  COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:refcore_driven> ${RP_CORE_OUT}/
  COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_SOURCE_DIR}/core.json ${RP_CORE_OUT}/)

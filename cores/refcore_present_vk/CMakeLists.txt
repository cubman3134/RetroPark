add_library(refcore_present_vk SHARED RefCoreVk.cpp)
target_include_directories(refcore_present_vk PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(refcore_present_vk PRIVATE Vulkan::Vulkan)
# Emit dll + manifest next to a cores/ dir under the build tree for the harness/e2e.
set(RP_CORE_VK_OUT $<TARGET_FILE_DIR:refcore_present_vk>/cores/refcore_present_vk)
add_custom_command(TARGET refcore_present_vk POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E make_directory ${RP_CORE_VK_OUT}
  COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:refcore_present_vk> ${RP_CORE_VK_OUT}/
  COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_SOURCE_DIR}/core.json ${RP_CORE_VK_OUT}/)

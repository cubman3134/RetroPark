add_executable(retropark_harness WIN32 main.cpp)
target_link_libraries(retropark_harness PRIVATE retropark)
add_dependencies(retropark_harness refcore_present refcore_present_vk)
target_compile_definitions(retropark_harness PRIVATE
  RP_HARNESS_CORE_DIR="$<TARGET_FILE_DIR:refcore_present>/cores/refcore_present"
  RP_HARNESS_CORE_DIR_VK="$<TARGET_FILE_DIR:refcore_present_vk>/cores/refcore_present_vk")

add_executable(retropark_harness WIN32 main.cpp)
target_link_libraries(retropark_harness PRIVATE retropark)
# Netplay wiring (--netplay-host/--netplay-join) uses rp::net::NetSession/TcpTransport and
# rp::Runtime directly, which live under src/ (PRIVATE to the retropark target, same reason
# tests/CMakeLists.txt adds this for retropark_tests).
target_include_directories(retropark_harness PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_dependencies(retropark_harness refcore_present refcore_present_vk refcore_driven LibretroShim)
target_compile_definitions(retropark_harness PRIVATE
  RP_HARNESS_CORE_DIR="$<TARGET_FILE_DIR:refcore_present>/cores/refcore_present"
  RP_HARNESS_CORE_DIR_VK="$<TARGET_FILE_DIR:refcore_present_vk>/cores/refcore_present_vk"
  RP_HARNESS_DRIVEN_CORE_DIR="$<TARGET_FILE_DIR:refcore_driven>/cores/refcore_driven"
  RP_HARNESS_SHIM_DIR="$<TARGET_FILE_DIR:LibretroShim>/cores/libretro_shim")

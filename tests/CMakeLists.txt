add_executable(retropark_tests
  test_main.cpp
  test_sanity.cpp
  test_abi_compiles.c
  test_manifest.cpp
  test_loader.cpp
  test_loader_ffi.cpp
  test_surface_ring.cpp
  test_framebuffer_copy.cpp
  test_backend_iface.cpp
  test_d3d11_handoff.cpp
  test_compositor.cpp
  test_runtime_api.cpp
  test_backend_factory.cpp
  test_e2e.cpp
  test_shader_embed.cpp
  test_vulkan_device.cpp
  test_vulkan_handoff.cpp
  test_vulkan_compositor.cpp
  test_vulkan_e2e.cpp
  test_driven_upload_d3d11.cpp
  test_driven_upload_vulkan.cpp
  test_driven_e2e.cpp
  test_pixel_convert.cpp
  test_libretro_e2e.cpp
  test_audio_pool.cpp
  test_audio_flow.cpp)
target_link_libraries(retropark_tests PRIVATE retropark doctest::doctest nlohmann_json::nlohmann_json retropark_libretro_convert)
target_include_directories(retropark_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)

set(PROBE_HDR "${CMAKE_CURRENT_BINARY_DIR}/gen/probe_vert_generated.h")
retropark_embed_shader("${CMAKE_CURRENT_SOURCE_DIR}/shaders/probe.vert" vert "${PROBE_HDR}" probe_vert)
add_custom_target(gen_probe_shader DEPENDS "${PROBE_HDR}")
add_dependencies(retropark_tests gen_probe_shader)
target_include_directories(retropark_tests PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/gen")
add_test(NAME retropark_tests COMMAND retropark_tests)
set_tests_properties(retropark_tests PROPERTIES WORKING_DIRECTORY $<TARGET_FILE_DIR:retropark_tests>)

# Must come after the retropark_tests target is defined: mock_core's CMakeLists.txt
# references $<TARGET_FILE_DIR:retropark_tests> and add_dependencies(retropark_tests ...).
add_subdirectory(mock_core)

# e2e test loads the real reference presenting core built by cores/refcore_present;
# depend on it and tell the test where its DLL + core.json land (absolute path via
# generator expression, so it resolves regardless of the test's working directory).
add_dependencies(retropark_tests refcore_present)
target_compile_definitions(retropark_tests PRIVATE
  RP_CORE_DIR="$<TARGET_FILE_DIR:refcore_present>/cores/refcore_present")

# vk e2e test loads the real reference Vulkan presenting core built by
# cores/refcore_present_vk; depend on it and tell the test where its DLL +
# core.json land (absolute path via generator expression).
add_dependencies(retropark_tests refcore_present_vk)
target_compile_definitions(retropark_tests PRIVATE
  RP_VK_CORE_DIR="$<TARGET_FILE_DIR:refcore_present_vk>/cores/refcore_present_vk")

# driven e2e test loads the real reference driven core built by cores/refcore_driven;
# depend on it and tell the test where its DLL + core.json land.
add_dependencies(retropark_tests refcore_driven)
target_compile_definitions(retropark_tests PRIVATE
  RP_DRIVEN_CORE_DIR="$<TARGET_FILE_DIR:refcore_driven>/cores/refcore_driven")

# libretro e2e test loads the emitted libretro_shim package (LibretroShim.dll + core.json
# + fceumm_libretro.dll, if present) and a real NES ROM; depend on the shim target and tell
# the test where its package lands + where NES ROMs live on this machine.
add_dependencies(retropark_tests LibretroShim)
target_compile_definitions(retropark_tests PRIVATE
  RP_SHIM_DIR="$<TARGET_FILE_DIR:LibretroShim>/cores/libretro_shim"
  RP_NES_ROM_DIR="C:/RetroBat/roms/nes")

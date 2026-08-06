add_executable(retropark_tests
  test_main.cpp
  test_sanity.cpp
  test_abi_compiles.c
  test_manifest.cpp
  test_loader.cpp
  test_loader_ffi.cpp
  test_surface_ring.cpp
  test_backend_iface.cpp
  test_d3d11_handoff.cpp
  test_compositor.cpp
  test_runtime_api.cpp
  test_e2e.cpp
  test_shader_embed.cpp
  test_vulkan_device.cpp
  test_vulkan_handoff.cpp)
target_link_libraries(retropark_tests PRIVATE retropark doctest::doctest nlohmann_json::nlohmann_json)
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

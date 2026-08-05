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
  test_compositor.cpp)
target_link_libraries(retropark_tests PRIVATE retropark doctest::doctest nlohmann_json::nlohmann_json)
target_include_directories(retropark_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME retropark_tests COMMAND retropark_tests)
set_tests_properties(retropark_tests PROPERTIES WORKING_DIRECTORY $<TARGET_FILE_DIR:retropark_tests>)

# Must come after the retropark_tests target is defined: mock_core's CMakeLists.txt
# references $<TARGET_FILE_DIR:retropark_tests> and add_dependencies(retropark_tests ...).
add_subdirectory(mock_core)

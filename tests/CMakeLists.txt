add_executable(retropark_tests
  test_main.cpp
  test_sanity.cpp
  test_abi_compiles.c
  test_manifest.cpp
  test_loader.cpp)
target_link_libraries(retropark_tests PRIVATE retropark doctest::doctest nlohmann_json::nlohmann_json)
target_include_directories(retropark_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME retropark_tests COMMAND retropark_tests)

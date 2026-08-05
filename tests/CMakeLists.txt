add_executable(retropark_tests
  test_main.cpp
  test_sanity.cpp)
target_link_libraries(retropark_tests PRIVATE retropark doctest::doctest nlohmann_json::nlohmann_json)
add_test(NAME retropark_tests COMMAND retropark_tests)

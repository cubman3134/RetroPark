add_library(mock_core SHARED MockCore.cpp)
target_link_libraries(mock_core PRIVATE retropark)
set_target_properties(mock_core PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY $<TARGET_FILE_DIR:retropark_tests>
  LIBRARY_OUTPUT_DIRECTORY $<TARGET_FILE_DIR:retropark_tests>)
add_dependencies(retropark_tests mock_core)

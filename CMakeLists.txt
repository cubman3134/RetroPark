cmake_minimum_required(VERSION 3.20)
project(RetroPark LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)

if (MSVC)
  add_compile_options(/W4 /permissive-)
endif()

include(FetchContent)
# doctest v2.4.11 and nlohmann_json v3.11.3 both declare a cmake_minimum_required
# below 3.5, which CMake >= 4.0 refuses to configure. This policy floor (as
# suggested by CMake's own error message) lets FetchContent build them anyway.
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
FetchContent_Declare(doctest
  GIT_REPOSITORY https://github.com/doctest/doctest.git
  GIT_TAG v2.4.11)
FetchContent_Declare(nlohmann_json
  GIT_REPOSITORY https://github.com/nlohmann/json.git
  GIT_TAG v3.11.3)
FetchContent_MakeAvailable(doctest nlohmann_json)

# Library target — sources appended by later tasks.
add_library(retropark STATIC
  src/loader/Manifest.cpp
  src/loader/CoreLoader.cpp)
target_include_directories(retropark PUBLIC include)
target_include_directories(retropark PRIVATE src)
target_compile_features(retropark PUBLIC cxx_std_17)
target_link_libraries(retropark PUBLIC nlohmann_json::nlohmann_json)

enable_testing()
add_subdirectory(tests)

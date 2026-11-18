cmake_minimum_required(VERSION 3.20)
project(atlas_watchcore VERSION 0.1.0 LANGUAGES CXX)

add_library(watchcore STATIC src/watchcore.cpp)
target_include_directories(watchcore PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(watchcore PUBLIC cxx_std_20)
target_compile_options(watchcore PRIVATE -Wall -Wextra -Wpedantic)

include(CTest)
if(BUILD_TESTING)
  add_executable(watchcore_tests tests/watchcore_tests.cpp)
  target_link_libraries(watchcore_tests PRIVATE watchcore)
  target_compile_options(watchcore_tests PRIVATE -Wall -Wextra -Wpedantic)
  add_test(NAME watchcore_tests COMMAND watchcore_tests)
endif()

# Observation only: this binary receives numeric decisions already normalized
# by the authoritative Python watcher; it has no I/O authority beyond stdout.
add_executable(watchcore_shadow src/shadow_security.cpp)
target_link_libraries(watchcore_shadow PRIVATE watchcore)
target_compile_options(watchcore_shadow PRIVATE -Wall -Wextra -Wpedantic)

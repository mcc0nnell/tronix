# Copyright 2026 Robert McConnell
# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20)
project(tronix VERSION 0.1.0 LANGUAGES C CXX)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CELIX_SOURCE "" CACHE PATH "Pinned Apache Celix source")
set(MSGPACK_SOURCE "" CACHE PATH "Pinned msgpack-c source")
if(NOT EXISTS "${CELIX_SOURCE}/CMakeLists.txt" OR NOT EXISTS "${MSGPACK_SOURCE}/include/msgpack.hpp")
  message(FATAL_ERROR "Pinned CELIX_SOURCE and MSGPACK_SOURCE are required")
endif()
find_package(OpenSSL REQUIRED COMPONENTS Crypto)
set(ENABLE_TESTING OFF CACHE BOOL "" FORCE)
set(FRAMEWORK_CURLINIT OFF CACHE BOOL "" FORCE)
set(BUILD_UTILS ON CACHE BOOL "" FORCE)
set(BUILD_FRAMEWORK ON CACHE BOOL "" FORCE)
foreach(k RCM CELIX_DFI CELIX_ETCDLIB LAUNCHER PROMISES PUSHSTREAMS
          HTTP_ADMIN LOG_SERVICE LOG_HELPER SHELL_API SHELL SHELL_TUI
          SHELL_WUI REMOTE_SHELL REMOTE_SERVICE_ADMIN CXX_REMOTE_SERVICE_ADMIN
          EVENT_ADMIN COMPONENTS_READY_CHECK EXAMPLES EXPERIMENTAL)
  set(BUILD${k} OFF CACHE BOOL "" FORCE)
endforeach()
set(CELIX_CXX14 OFF CACHE BOOL "" FORCE)
set(CELIX_CXX17 OFF CACHE BOOL "" FORCE)
add_subdirectory("${CELIX_SOURCE}" "${CMAKE_BINARY_DIR}/apache-celix" EXCLUDE_FROM_ALL)
if(TARGET framework AND CMAKE_C_COMPILER_ID STREQUAL "GNU")
  target_compile_options(framework PRIVATE -Wno-error=maybe-uninitialized)
endif()
set(CELIX_CMAKE_DIRECTORY "${CELIX_SOURCE}/cmake/cmake_celix")
add_celix_bundle(tronix_scheduler
  VERSION 0.1.0 SYMBOLIC_NAME org.tronix.scheduler
  NAME "Tronix scheduling service"
  FILENAME tronix_scheduler SOURCES src/scheduler.c)
target_include_directories(tronix_scheduler PRIVATE include)
target_link_libraries(tronix_scheduler PRIVATE Celix::framework)
add_celix_bundle(tronix_demo
  VERSION 0.1.0 SYMBOLIC_NAME org.tronix.adapter.demo
  NAME "Tronix restricted demo adapter"
  FILENAME tronix_demo SOURCES src/demo.c)
target_include_directories(tronix_demo PRIVATE include)
target_link_libraries(tronix_demo PRIVATE Celix::framework)
add_celix_bundle(tronix_cdp
  VERSION 0.2.0 SYMBOLIC_NAME org.tronix.adapter.cdp
  NAME "Tronix bounded CDP observation adapter"
  FILENAME tronix_cdp SOURCES src/cdp_adapter.cc)
target_include_directories(tronix_cdp PRIVATE include "${MSGPACK_SOURCE}/include")
target_compile_definitions(tronix_cdp PRIVATE MSGPACK_NO_BOOST=1)
target_link_libraries(tronix_cdp PRIVATE Celix::framework)
add_executable(tronix_worker src/worker.cc)
target_include_directories(tronix_worker PRIVATE include "${MSGPACK_SOURCE}/include")
target_compile_definitions(tronix_worker PRIVATE MSGPACK_NO_BOOST=1)
target_link_libraries(tronix_worker PRIVATE Celix::framework OpenSSL::Crypto)
add_executable(tronixd src/tronixd.cc)
target_include_directories(tronixd PRIVATE include "${MSGPACK_SOURCE}/include")
target_compile_definitions(tronixd PRIVATE MSGPACK_NO_BOOST=1)
if(NOT LIBZIP_LIBRARY OR NOT JANSSON_LIBRARY)
  message(FATAL_ERROR "Celix's selected libzip and Jansson libraries must be available")
endif()
target_link_libraries(tronixd PRIVATE Celix::framework pthread OpenSSL::Crypto
  "${LIBZIP_LIBRARY}" "${JANSSON_LIBRARY}")
set_target_properties(tronixd PROPERTIES BUILD_RPATH "${CMAKE_BINARY_DIR}/apache-celix/libs/framework;${CMAKE_BINARY_DIR}/apache-celix/libs/utils")
file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/tronix-paths.env"
  CONTENT "worker=$<TARGET_FILE:tronix_worker>\ndaemon=$<TARGET_FILE:tronixd>\nscheduler=$<TARGET_PROPERTY:tronix_scheduler,BUNDLE_FILE>\ndemo=$<TARGET_PROPERTY:tronix_demo,BUNDLE_FILE>\n")

# Restricted OpenThread network-observation Celix bundle.
# Talks only to an explicitly configured private, owned telemetry bridge.
add_celix_bundle(tronix_thread
  VERSION 0.1.0 SYMBOLIC_NAME org.tronix.adapter.thread
  NAME "Tronix read-only Thread telemetry adapter"
  FILENAME tronix_thread SOURCES src/thread_adapter.cc)
target_include_directories(tronix_thread PRIVATE include "${MSGPACK_SOURCE}/include")
target_compile_definitions(tronix_thread PRIVATE MSGPACK_NO_BOOST=1)
target_link_libraries(tronix_thread PRIVATE Celix::framework)


# Optional restricted Lightpanda Agent Bridge record/replay Celix bundle.
# Use instead of tronix_cdp; the two adapters are mutually exclusive per runtime.
add_celix_bundle(tronix_lightpanda_record
  VERSION 0.1.0 SYMBOLIC_NAME org.tronix.adapter.lightpanda_record
  NAME "Tronix bounded Lightpanda record/replay adapter"
  FILENAME tronix_lightpanda_record SOURCES src/lightpanda_record_adapter.cc)
target_include_directories(tronix_lightpanda_record PRIVATE include "${MSGPACK_SOURCE}/include")
target_compile_definitions(tronix_lightpanda_record PRIVATE MSGPACK_NO_BOOST=1)
target_link_libraries(tronix_lightpanda_record PRIVATE Celix::framework)

# Optional bounded deterministic verifier; TRONIX v1 remains unchanged.
add_celix_bundle(tronix_answer_verifier
  VERSION 0.1.0 SYMBOLIC_NAME org.tronix.answer_verifier
  NAME "TRONIX bounded deterministic answer verifier"
  FILENAME tronix_answer_verifier SOURCES src/answer_verifier.c)
target_include_directories(tronix_answer_verifier PRIVATE include)
target_link_libraries(tronix_answer_verifier PRIVATE Celix::framework OpenSSL::Crypto)
add_executable(tronix_verify_once src/verify_once.cc)
target_include_directories(tronix_verify_once PRIVATE include)
target_link_libraries(tronix_verify_once PRIVATE Celix::framework OpenSSL::Crypto)
set_target_properties(tronix_verify_once PROPERTIES BUILD_RPATH
  "${CMAKE_BINARY_DIR}/apache-celix/libs/framework;${CMAKE_BINARY_DIR}/apache-celix/libs/utils")

# Optional persistent typed verifier, separate from the int64 TRONIX v1 API.
add_executable(tronix_verify_daemon src/verify_daemon.cc)
target_include_directories(tronix_verify_daemon PRIVATE include "${MSGPACK_SOURCE}/include")
target_compile_definitions(tronix_verify_daemon PRIVATE MSGPACK_NO_BOOST=1)
target_link_libraries(tronix_verify_daemon PRIVATE Celix::framework OpenSSL::Crypto)
set_target_properties(tronix_verify_daemon PROPERTIES BUILD_RPATH
  "${CMAKE_BINARY_DIR}/apache-celix/libs/framework;${CMAKE_BINARY_DIR}/apache-celix/libs/utils")

# Isolated C++20 Atlas Watchcore: observation-only native decision core.
# Original deterministic logic is preserved in integrations/atlas-watchcore.
add_library(tronix_watchcore_native STATIC
  integrations/atlas-watchcore/src/watchcore.cpp)
target_include_directories(tronix_watchcore_native PUBLIC
  integrations/atlas-watchcore/include)
target_compile_features(tronix_watchcore_native PUBLIC cxx_std_20)
set_target_properties(tronix_watchcore_native PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_compile_options(tronix_watchcore_native PRIVATE -Wall -Wextra -Wpedantic)

# True Celix service; registers on bundle start, unregisters on stop.
add_celix_bundle(tronix_watchcore_security
  VERSION 0.1.0 SYMBOLIC_NAME org.tronix.watchcore.security
  NAME "TRONIX observation-only Atlas Watchcore decision service"
  FILENAME tronix_watchcore_security SOURCES src/watchcore_bundle.cc)
target_include_directories(tronix_watchcore_security PRIVATE include)
target_link_libraries(tronix_watchcore_security PRIVATE
  tronix_watchcore_native Celix::framework)
target_compile_features(tronix_watchcore_security PRIVATE cxx_std_20)

# External Rizzer MR !74 compatible numeric-only executable: bundles are
# operator-supplied, SHA-256 pinned, privately staged and loaded by Celix.
add_executable(tronix_watchcore_shadow src/watchcore_celix_shadow.cc)
target_include_directories(tronix_watchcore_shadow PRIVATE include)
target_compile_features(tronix_watchcore_shadow PRIVATE cxx_std_20)
target_link_libraries(tronix_watchcore_shadow PRIVATE Celix::framework
  OpenSSL::Crypto "${LIBZIP_LIBRARY}" "${JANSSON_LIBRARY}")
set_target_properties(tronix_watchcore_shadow PROPERTIES BUILD_RPATH
  "${CMAKE_BINARY_DIR}/apache-celix/libs/framework;${CMAKE_BINARY_DIR}/apache-celix/libs/utils")

# Independent original source oracle for differential regression testing.
add_executable(tronix_watchcore_reference
  integrations/atlas-watchcore/src/shadow_security.cpp)
target_link_libraries(tronix_watchcore_reference PRIVATE tronix_watchcore_native)
target_compile_features(tronix_watchcore_reference PRIVATE cxx_std_20)

# Preserve and execute the original standalone core assertions in native CI.
add_executable(tronix_watchcore_unit_tests
  integrations/atlas-watchcore/tests/watchcore_tests.cpp)
target_link_libraries(tronix_watchcore_unit_tests PRIVATE tronix_watchcore_native)
target_compile_features(tronix_watchcore_unit_tests PRIVATE cxx_std_20)

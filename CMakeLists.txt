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

# 6lowBang is a separately reviewed C11 library owned by 6lowStick.
# OFF by default. No implicit network FetchContent or unpinned vendoring.
set(TRONIX_BANG_SOURCE "" CACHE PATH "Absolute path to pinned 6lowStick/native/bang source")
if(TRONIX_BANG_SOURCE)
  if(NOT IS_ABSOLUTE "${TRONIX_BANG_SOURCE}" OR
     NOT EXISTS "${TRONIX_BANG_SOURCE}/include/lowbang.h" OR
     NOT EXISTS "${TRONIX_BANG_SOURCE}/CMakeLists.txt")
    message(FATAL_ERROR "TRONIX_BANG_SOURCE must be a complete, reviewed native/bang source directory")
  endif()
  add_subdirectory("${TRONIX_BANG_SOURCE}" "${CMAKE_BINARY_DIR}/6lowbang-core" EXCLUDE_FROM_ALL)
  # A static native library is linked into a loadable Celix service bundle.
  set_target_properties(lowbang PROPERTIES POSITION_INDEPENDENT_CODE ON)
  add_celix_bundle(tronix_bang
    VERSION 0.1.0 SYMBOLIC_NAME org.tronix.adapter.bang.synthetic
    NAME "Tronix 6lowBang synthetic caption admission adapter"
    FILENAME tronix_bang SOURCES src/bang_adapter.c src/bang_fixture.c)
  target_include_directories(tronix_bang PRIVATE include)
  target_link_libraries(tronix_bang PRIVATE Celix::framework lowbang)
  add_executable(tronix_bang_fixture_test tests/bang-fixture.c src/bang_fixture.c)
  target_include_directories(tronix_bang_fixture_test PRIVATE include)
  target_link_libraries(tronix_bang_fixture_test PRIVATE lowbang)
  add_custom_target(bang_fixture_test
    COMMAND $<TARGET_FILE:tronix_bang_fixture_test>
    DEPENDS tronix_bang_fixture_test
    COMMENT "Exercise the actual 6lowBang C11 protocol without Celix")
  # The real signed caption data path is a separate typed Celix service;
  # never repurpose the integer-returning Tronix operation ABI.
  find_package(Threads REQUIRED)
  add_library(tronix_bang_stream_core STATIC src/bang_stream.c)
  set_target_properties(tronix_bang_stream_core PROPERTIES POSITION_INDEPENDENT_CODE ON)
  target_include_directories(tronix_bang_stream_core PUBLIC include)
  target_link_libraries(tronix_bang_stream_core PUBLIC lowbang Threads::Threads)
  add_celix_bundle(tronix_bang_stream
    VERSION 0.1.0 SYMBOLIC_NAME org.tronix.bang.stream
    NAME "Tronix experimental signed C caption stream"
    FILENAME tronix_bang_stream SOURCES src/bang_stream_adapter.c)
  target_include_directories(tronix_bang_stream PRIVATE include)
  target_link_libraries(tronix_bang_stream PRIVATE Celix::framework tronix_bang_stream_core)
  add_executable(tronix_bang_stream_test tests/bang-stream.c)
  target_link_libraries(tronix_bang_stream_test PRIVATE tronix_bang_stream_core)
  add_custom_target(bang_stream_test
    COMMAND $<TARGET_FILE:tronix_bang_stream_test>
    DEPENDS tronix_bang_stream_test
    COMMENT "Test signed C stream subscription and explicit backpressure")
  # Signed-event lab codec; simulates a two-node OpenThread UDP hop through
  # the external OT CLI rather than routing text through Tronix job values.
  add_executable(bang-thread-codec src/bang_wire.c src/bang_thread_codec.c)
  target_include_directories(bang-thread-codec PRIVATE include)
  target_compile_definitions(bang-thread-codec PRIVATE _POSIX_C_SOURCE=200809L)
  target_link_libraries(bang-thread-codec PRIVATE tronix_bang_stream_core)
  add_custom_target(bang_thread_codec_test
    COMMAND $<TARGET_FILE:bang-thread-codec> selftest
    DEPENDS bang-thread-codec
    COMMENT "Check canonical wire boundaries before Thread simulator tests")
  # Native C/Celix UDP transport, restricted to IPv6 loopback.
  # Decode and pass the signed event to the typed stream service for validation.
  add_celix_bundle(tronix_bang_udp
    VERSION 0.1.0 SYMBOLIC_NAME org.tronix.bang.udp
    NAME "Tronix experimental loopback C signed caption transport"
    FILENAME tronix_bang_udp SOURCES src/bang_udp_adapter.c src/bang_wire.c)
  target_include_directories(tronix_bang_udp PRIVATE include)
  target_link_libraries(tronix_bang_udp PRIVATE Celix::framework lowbang)
  add_executable(bang-udp-celix-node tests/bang-udp-celix-node.c)
  target_include_directories(bang-udp-celix-node PRIVATE include)
  target_link_libraries(bang-udp-celix-node PRIVATE Celix::framework lowbang)
endif()

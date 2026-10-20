cmake_minimum_required(VERSION 3.20)
project(tronix VERSION 0.1.0 LANGUAGES C CXX)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CELIX_SOURCE "" CACHE PATH "Pinned Apache Celix source")
set(MSGPACK_SOURCE "" CACHE PATH "Pinned msgpack-c source")
if(NOT EXISTS "${CELIX_SOURCE}/CMakeLists.txt" OR NOT EXISTS "${MSGPACK_SOURCE}/include/msgpack.hpp")
  message(FATAL_ERROR "Pinned CELIX_SOURCE and MSGPACK_SOURCE are required")
endif()
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
target_link_libraries(tronix_worker PRIVATE Celix::framework)
add_executable(tronixd src/tronixd.cc)
target_include_directories(tronixd PRIVATE include "${MSGPACK_SOURCE}/include")
target_compile_definitions(tronixd PRIVATE MSGPACK_NO_BOOST=1)
target_link_libraries(tronixd PRIVATE Celix::framework pthread)
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

# Synthetic IHB preflight only. No Docker/process execution rights in Celix.
option(TRONIX_ENABLE_IHB_PREFLIGHT "Build read-only IHB synthetic profile preflight" OFF)
set(TRONIX_IHB_PREFLIGHT_SOCKET "" CACHE STRING "Absolute private Unix socket path")
set(TRONIX_IHB_PREFLIGHT_UID "" CACHE STRING "Expected Linux UID of broker")
if(TRONIX_ENABLE_IHB_PREFLIGHT)
  if(NOT TRONIX_IHB_PREFLIGHT_SOCKET MATCHES "^/[a-zA-Z0-9_./-]+$" OR
     NOT TRONIX_IHB_PREFLIGHT_UID MATCHES "^[0-9]+$")
    message(FATAL_ERROR "Preflight requires a trusted absolute socket path and numeric broker UID")
  endif()
  add_celix_bundle(tronix_ihb_preflight
    VERSION 0.1.0 SYMBOLIC_NAME org.tronix.adapter.ihb.preflight
    NAME "Tronix read-only IHB synthetic profile preflight"
    FILENAME tronix_ihb_preflight SOURCES src/ihb_preflight_adapter.c)
  target_include_directories(tronix_ihb_preflight PRIVATE include)
  target_compile_definitions(tronix_ihb_preflight PRIVATE
    TRONIX_IHB_PREFLIGHT_SOCKET=\"${TRONIX_IHB_PREFLIGHT_SOCKET}\"
    TRONIX_IHB_PREFLIGHT_UID=${TRONIX_IHB_PREFLIGHT_UID})
  target_link_libraries(tronix_ihb_preflight PRIVATE Celix::framework)
  file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/tronix-ihb-preflight-socket.txt"
    CONTENT "${TRONIX_IHB_PREFLIGHT_SOCKET}\n")
endif()

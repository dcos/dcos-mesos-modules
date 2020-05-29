cmake_minimum_required(VERSION 3.7.0)

project(dcos-mesos-modules)

set(MESOS_ROOT "../mesos" CACHE PATH "Mesos root directory.")
get_filename_component(MESOS_ROOT ${MESOS_ROOT} ABSOLUTE)
set(BOOST_ROOT_DIR "" CACHE PATH "Boost libs root directory.")

message("MESOS_ROOT is ${MESOS_ROOT}")

# CMAKE MODULE SETUP.
#####################
# Paths that are searched when `include(...)` is called.
list(APPEND CMAKE_MODULE_PATH ${MESOS_ROOT}/cmake)
list(APPEND CMAKE_MODULE_PATH ${MESOS_ROOT}/3rdparty/cmake)
list(APPEND CMAKE_MODULE_PATH ${MESOS_ROOT}/src/cmake)
list(APPEND CMAKE_MODULE_PATH ${CMAKE_SOURCE_DIR}/cmake)

# Macros.
include(CTest)
include(Dependencies)

# Set the default standard to C++11 for all targets.
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Do not use, for example, `-std=gnu++11`.
set(CMAKE_CXX_EXTENSIONS OFF)

list(APPEND DEPENDENCIES_INC
  "${BOOST_INCL}"
  "${CURL_INCL}"
  "${GLOG_INCL}"
  "${MESOS_ROOT}/build/include"
  "${MESOS_ROOT}/include"
  "${PICOJSON_INCL}"
  "${PROCESS_INCL}"
  "${PROTOBUF_INCL}"
  "${RAPIDJSON_INCL}"
  "${STOUT_INCL}"
  "${ZOOKEEPER_INCL}"
)

list(APPEND DEPENDENCIES_LIB
  glog
  mesos
  mesos_protobufs
  process
  protobuf

  $<$<PLATFORM_ID:Linux>:pthread>
)

# TODO(akornatskyy): Agreed to have CMakeLists.txt in each directory (per
# module), however, right now that is not possible because corresponding
# changes require some changes to source code (that would break
# autotools build).
add_subdirectory(dockercfg)
add_subdirectory(logsink)
add_subdirectory(journald)

# overlay
add_library(overlay SHARED
  overlay/messages.pb.cc
  overlay/overlay.pb.cc
  overlay/agent_metrics.cpp
  overlay/agent.cpp
  overlay/supervisor_metrics.cpp
  overlay/master_metrics.cpp
  overlay/master.cpp
)
install(TARGETS overlay LIBRARY DESTINATION lib/mesos)
add_custom_command(
  OUTPUT overlay/messages.pb.cc overlay/overlay.pb.cc
  COMMAND protoc
    -I${CMAKE_CURRENT_SOURCE_DIR}
    --cpp_out=${CMAKE_CURRENT_BINARY_DIR}
    overlay/messages.proto overlay/overlay.proto
  DEPENDS overlay/messages.proto overlay/overlay.proto
)
target_include_directories(overlay PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}"
  "${CMAKE_CURRENT_BINARY_DIR}"
  ${DEPENDENCIES_INC}
)

# metrics
add_library(metrics SHARED
  metrics/messages.pb.cc
  metrics/isolator.cpp
)
install(TARGETS metrics
  ARCHIVE DESTINATION lib/mesos
  LIBRARY DESTINATION lib/mesos
)
add_custom_command(
  OUTPUT metrics/messages.pb.cc
  COMMAND protoc
    -I${CMAKE_CURRENT_SOURCE_DIR}
    --cpp_out=${CMAKE_CURRENT_BINARY_DIR}
    metrics/messages.proto
  DEPENDS metrics/messages.proto
)
target_include_directories(metrics PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}"
  "${CMAKE_CURRENT_BINARY_DIR}"
  ${DEPENDENCIES_INC}
)

if (BUILD_TESTING)
  list(APPEND TEST_DEPENDENCIES_INC
    ${DEPENDENCIES_INC}
    "${GMOCK_INCL}"
    "${GTEST_INCL}"
    "${MESOS_ROOT}/build/src"
    "${MESOS_ROOT}/src"
    "${NVML_INCL}"
  )

  list(APPEND TEST_DEPENDENCIES_LIB
    ${DEPENDENCIES_LIB}
    gmock
  )

  add_subdirectory(tests)

  # Test binary for metrics
  add_executable(test-metrics
    metrics/messages.pb.cc
    tests/metrics_tests.cpp
  )
  # TODO(akornatskyy): Paths defined as "nowhere" are needed to build Mesos
  # test utils (https://github.com/apache/mesos/blob/master/src/tests/utils.cpp).
  # These definitions can be dropped if the test utilities we need here are
  # split from the path-dependent utilities into a separate file.
  add_definitions(
    -DMODULES_BUILD_DIR="${PROJECT_BINARY_DIR}"
    -DSOURCE_DIR="${MESOS_ROOT}"
    -DBUILD_DIR="${MESOS_ROOT}/build"
    -DPKGMODULEDIR="nowhere"
    -DLIBDIR="nowhere"
    -DPKGLIBEXECDIR="nowhere"
    -DTESTLIBEXECDIR="nowhere"
    -DSBINDIR="nowhere"
    -DPKGDATADIR="nowhere"
    -DVERSION="1.10.0"
  )
  target_include_directories(test-metrics PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_CURRENT_BINARY_DIR}"
    ${TEST_DEPENDENCIES_INC}
  )
  target_link_libraries(test-metrics PRIVATE
    metrics
    mesos-tests
    ${TEST_DEPENDENCIES_LIB}
  )
  add_test(NAME Metrics_Tests COMMAND test-metrics)

  # Test binary for overlay
  add_executable(test-overlay tests/overlay_tests.cpp)
  target_include_directories(test-overlay PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_CURRENT_BINARY_DIR}"
    ${TEST_DEPENDENCIES_INC}
  )
  target_link_libraries(test-overlay PRIVATE
    overlay
    mesos-tests
    ${TEST_DEPENDENCIES_LIB}
  )
  add_test(NAME Overlay_Tests COMMAND test-overlay)
endif()

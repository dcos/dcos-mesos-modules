cmake_minimum_required(VERSION 3.7.0)

project(dcos-mesos-modules)

include(CTest) # NOTE: BUILD_TESTING is defined by CTest.

set(MESOS_ROOT "../mesos" CACHE FILEPATH "Mesos root directory.")
get_filename_component(MESOS_ROOT ${MESOS_ROOT} ABSOLUTE)

if (BUILD_TESTING)
  set(
    MESOS_SOURCE
    ""
    CACHE FILEPATH
    "Mesos source tree root directory (needed to build tests).")

  set(
    MESOS_BUILD
    ""
    CACHE FILEPATH
    "Mesos build tree root directory (needed to build tests).")

  get_filename_component(MESOS_SOURCE ${MESOS_SOURCE} ABSOLUTE)
  get_filename_component(MESOS_BUILD ${MESOS_BUILD} ABSOLUTE)
endif()

set(BOOST_ROOT_DIR "" CACHE PATH "Boost libs root directory.")

message("MESOS_ROOT is ${MESOS_ROOT}")

# CMAKE MODULE SETUP.
#####################
include(${CMAKE_SOURCE_DIR}/cmake/Dependencies.cmake)

# Set the default standard to C++11 for all targets.
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Do not use, for example, `-std=gnu++11`.
set(CMAKE_CXX_EXTENSIONS OFF)

# We expect Mesos libraries built with `-DENABLE_SSL=ON`, hence USE_SSL_SOCKET
# should be set. Otherwise, type declarations included from the internal
# Mesos headers might differ from those actually used when building Mesos.
add_definitions(-DUSE_SSL_SOCKET=1)

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
    "${MESOS_SOURCE}/src"
    "${MESOS_BUILD}/src"
    "${NVML_INCL}"
  )

  list(APPEND TEST_DEPENDENCIES_LIB
    ${DEPENDENCIES_LIB}
    gmock
  )

  add_subdirectory(tests)

  # Test binary for metrics
  add_executable(test-metrics
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

  # TODO(asekretenko): After ditching the automake build, adjust the path in
  # `MetricTest` and get rid of this symlink.
  file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/.libs")
  SET(LIBMETRICS_SYMLINK
    "${CMAKE_CURRENT_BINARY_DIR}/.libs/libmetrics-module.so")

  add_custom_target(libmetrics-testing-symlink DEPENDS "${LIBMETRICS_SYMLINK}")
  add_custom_command(
    OUTPUT "${LIBMETRICS_SYMLINK}"
    COMMAND ln -sf "$<TARGET_FILE:metrics>" "${LIBMETRICS_SYMLINK}")

  add_dependencies(libmetrics-testing-symlink metrics)
  add_dependencies(test-metrics libmetrics-testing-symlink)

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

  # TODO(asekretenko): After ditching the automake build, adjust the path in
  # `OverlayTest` and get rid of this symlink.
  file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/.libs")
  SET(LIBOVERLAY_SYMLINK
    "${CMAKE_CURRENT_BINARY_DIR}/.libs/libmesos_network_overlay.so")

  add_custom_target(liboverlay-testing-symlink DEPENDS "${LIBOVERLAY_SYMLINK}")
  add_custom_command(
    OUTPUT "${LIBOVERLAY_SYMLINK}"
    COMMAND ln -sf "$<TARGET_FILE:overlay>" "${LIBOVERLAY_SYMLINK}")

  add_dependencies(liboverlay-testing-symlink metrics)
  add_dependencies(test-overlay liboverlay-testing-symlink)

  add_test(NAME Overlay_Tests COMMAND test-overlay)
endif()

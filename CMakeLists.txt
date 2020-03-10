
if (NOT WIN32)
  cmake_minimum_required(VERSION 3.7.0)
else ()
  cmake_minimum_required(VERSION 3.8.0)
endif ()

project(dcos-mesos-modules)

get_filename_component(MESOS_ROOT ../mesos ABSOLUTE)

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
include(Versions)
include(Dependencies)

# Set the default standard to C++11 for all targets.
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Do not use, for example, `-std=gnu++11`.
set(CMAKE_CXX_EXTENSIONS OFF)

if (CMAKE_CXX_COMPILER_ID MATCHES MSVC)
  # TODO(andschwa): Switch to `/W4` and re-enable possible-loss-of-data warnings.
  #
  # The last two warnings are disabled (well, put into `/W4`) because
  # there is no easy equivalent to enable them for GCC/Clang without
  # also fixing all the warnings from `-Wconversion`.
  add_compile_options(
    # Like `-Wall`; `/W4` is more like `-Wall -Wextra`.
    /W3
    # Disable permissiveness.
    /permissive-
    # C4244 is a possible loss of data warning for integer conversions.
    /w44244
    # C4267 is a possible loss of data warning when converting from `size_t`.
    /w44267)
endif ()

# WINDOWS CONFIGURATION.
########################
if (WIN32)
  # COFF/PE and friends are somewhat limited in the number of sections they
  # allow for an object file. We use this to avoid those problems.
  add_compile_options(/bigobj /vd2)

  # Fix Warning C4530: C++ exception handler used, but unwind semantics are not
  # enabled.
  add_compile_options(/EHsc)

  set(CMAKE_WINDOWS_EXPORT_ALL_SYMBOLS ON)

  # Enable multi-threaded compilation for `cl.exe`.
  add_compile_options(/MP)

  # Force use of Unicode C and C++ Windows APIs.
  add_definitions(-DUNICODE -D_UNICODE -DNOGDI -DNOMINMAX -DGOOGLE_GLOG_DLL_DECL=)

  # Convenience flags to simplify Windows support in C++ source; used to
  # `#ifdef` out some platform-specific parts of Mesos.  We choose to define
  # a new flag rather than using an existing flag (`_WIN32`) because we want
  # to give the build system fine-grained control over what code is #ifdef'd
  # out in the future.  Using only flags defined by our build system to control
  # this logic is the clearest and most stable way of accomplishing this.
  add_definitions(-D__WINDOWS__)
endif ()

add_subdirectory(dockercfg)
add_subdirectory(journald)
add_subdirectory(logsink)

# FIXME: due to import path in 'overlay/messages.proto'
# NOTE: according to [1], overlay module is not required for Windows
# [1] https://github.com/mesosphere/dcos-enterprise/tree/master/packages/mesos/extra/windows-ee.patches
#add_subdirectory(overlay)


# FIXME: due to limitation of protobuf generation process
# and include in source
include_directories(
  .
  build
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
)

add_library(metrics SHARED metrics/isolator.cpp)

set(CMAKE_CURRENT_BINARY_DIR ${CMAKE_CURRENT_BINARY_DIR}/metrics)

protobuf_generate(
  LANGUAGE cpp
  TARGET metrics
  PROTOS metrics/messages.proto
  APPEND_PATH ./metrics
)

target_link_libraries(metrics PRIVATE
  ${OPENSSL_LIBRARIES}
  glog
  http_parser
  mesos
  mesos_protobufs
  process
  protobuf
  zlib
  $<$<PLATFORM_ID:Windows>:Mswsock>
  $<$<PLATFORM_ID:Windows>:Userenv>
  $<$<PLATFORM_ID:Windows>:ws2_32>
)

if (BUILD_TESTING)
  add_subdirectory(tests)

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

  include_directories(
    "${BOOST_INCL}"
    "${CURL_INCL}"
    "${GLOG_INCL}"
    "${GMOCK_INCL}"
    "${GTEST_INCL}"
    "${MESOS_ROOT}/build/include"
    "${MESOS_ROOT}/build/src"
    "${MESOS_ROOT}/include"
    "${MESOS_ROOT}/src"
    "${NVML_INCL}"
    "${PICOJSON_INCL}"
    "${PROCESS_INCL}"
    "${PROTOBUF_INCL}"
    "${RAPIDJSON_INCL}"
    "${STOUT_INCL}"
    "${ZOOKEEPER_INCL}"
  )

  # Test bianry for metrics
  add_executable(test-metrics tests/metrics_tests.cpp)

  target_link_libraries(test-metrics PRIVATE
    bzip2
    curl
    glog
    gmock
    gtest
    http_parser
    mesos
    mesos_protobufs
    mesos-tests
    metrics
    process
    protobuf
    sasl2
    zlib
    zookeeper
    zookeeper_hashtable
    ${OPENSSL_LIBRARIES}
    $<$<PLATFORM_ID:Windows>:IPHlpAPI>
    $<$<PLATFORM_ID:Windows>:crypt32>
    $<$<PLATFORM_ID:Windows>:Secur32>
    $<$<PLATFORM_ID:Windows>:Mswsock>
    $<$<PLATFORM_ID:Windows>:Userenv>
    $<$<PLATFORM_ID:Windows>:ws2_32>
  )

  add_test(NAME Metrics_Tests COMMAND test-metrics --verbose)

  add_dependencies(test-journald libjournaldlogger)
  add_dependencies(test-metrics metrics)
endif()

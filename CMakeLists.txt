
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

#include(cotire)

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


include_directories(
  .
  build
  "${BOOST_INCL}"
  "${GLOG_INCL}"
  "${PROTOBUF_INCL}"
  "${PICOJSON_INCL}"
  "${RAPIDJSON_INCL}"
  "${PROCESS_INCL}"
  "${STOUT_INCL}"
  "${CURL_INCL}"
  "${MESOS_BUILD_INCL}"
  "${MESOS_INCL}"
  # FIXME: due to limitation of protobuf generation process
  # and include in source
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
  ${GLOG_LIB}
  ${HTTP_PARSER_LIB}
  ${PROCESS_LIB}
  ${PROTOBUF_LIB}
  ${ZLIB_LIB_STATIC}
  ${OPENSSL_LIBRARIES}
  ${MESOS_PROTOBUFS_LIB}
  ${MESOS_LIB}
  Mswsock
  Userenv
  Ws2_32
  ${OPENSSL_CRYPTO_LIBRARY}
  ${OPENSSL_SSL_LIBRARY}
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
    "${MESOS_BUILD_INCL}"
    "${MESOS_INCL}"
    "${MESOS_ROOT}/src"
    "${MESOS_ROOT}/build/src"
    "${PROCESS_INCL}"
    "${ZOOKEEPER_INCL}"
    "${NVML_INCL}"
    "${GOOGLEMOCK_INCL}"
    "${GOOGLETEST_INCL}"
    "${STOUT_INCL}"
    "${CURL_INCL}"
    "${PROTOBUF_INCL}"
    "${RAPIDJSON_INCL}"
    "${PICOJSON_INCL}"
    "${GLOG_INCL}"
    "${BOOST_INCL}"
  )

  # Test bianry for metrics
  add_executable(test-metrics tests/metrics_tests.cpp)

  target_link_libraries(test-metrics PUBLIC
    metrics
    mesos-tests
    ${MESOS_PROTOBUFS_LIB}
    ${MESOS_LIB}
    ${PROCESS_LIB}
    ${ZOOKEEPER_LIB}
    ${ZOOKEEPER_HASHTABLE_LIB}
    ${PROTOBUF_LIB}
    ${GLOG_LIB}
    ${ZLIB_LIB_STATIC}
    ${GOOGLEMOCK_LIB}
    ${GOOGLETEST_LIB}
    ${SASL_LIB}
    ${BZIP2_LIB}
    ${CURL_LIB}
    ${HTTP_PARSER_LIB}
    ${OPENSSL_LIBRARIES}
    ${SYS_DEPENDENCY_LIBS}
    IPHlpAPI.lib
    crypt32.lib
    Ws2_32
    Userenv
    Mswsock
    Secur32.lib
  )

  add_test(NAME Metrics_Tests COMMAND test-metrics --verbose)

  add_dependencies(test-journald libjournaldlogger)
  add_dependencies(test-metrics metrics)

endif()

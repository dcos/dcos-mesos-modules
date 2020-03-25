
set(MESOS_TESTS_SRC
  ${MESOS_ROOT}/src/tests/cluster.cpp
  ${MESOS_ROOT}/src/tests/containerizer.cpp
  ${MESOS_ROOT}/src/tests/containerizer/launcher.cpp
  ${MESOS_ROOT}/src/tests/environment.cpp
  ${MESOS_ROOT}/src/tests/flags.cpp
  ${MESOS_ROOT}/src/tests/main.cpp
  ${MESOS_ROOT}/src/tests/mesos.cpp
  ${MESOS_ROOT}/src/tests/mock_registrar.cpp
  ${MESOS_ROOT}/src/tests/mock_slave.cpp
  ${MESOS_ROOT}/src/tests/module.cpp
  ${MESOS_ROOT}/src/tests/utils.cpp
)

add_library(mesos-tests STATIC ${MESOS_TESTS_SRC})

set(MESOS_TESTS_INCL
  "${CMAKE_CURRENT_BINARY_DIR}/.."
  "${GMOCK_INCL}"
  "${GTEST_INCL}"
  "${MESOS_ROOT}/build/include"
  "${MESOS_ROOT}/build/src"
  "${MESOS_ROOT}/include"
  "${MESOS_ROOT}/src"
  "${NVML_INCL}"
  "${PROCESS_INCL}"
  "${ZOOKEEPER_INCL}"
)

target_include_directories(mesos-tests PUBLIC ${MESOS_TESTS_INCL})

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

# Test binary for common

add_executable(test-common common_tests.cpp)

target_include_directories(test-common PUBLIC
  ..
  ../build
  "${BOOST_INCL}"
  "${CURL_INCL}"
  "${GLOG_INCL}"
  "${GMOCK_INCL}"
  "${GTEST_INCL}"
  "${MESOS_ROOT}/build/include"
  "${MESOS_ROOT}/include"
  "${NVML_INCL}"
  "${PICOJSON_INCL}"
  "${PROCESS_INCL}"
  "${PROTOBUF_INCL}"
  "${RAPIDJSON_INCL}"
  "${STOUT_INCL}"
  "${ZOOKEEPER_INCL}"
)

set (MESOSTEST_LIBS
  ${OPENSSL_LIBRARIES}
  bzip2
  curl
  glog
  gmock
  gtest
  http_parser
  mesos
  mesos_protobufs
  process
  protobuf
  sasl2
  zlib
  zookeeper
  zookeeper_hashtable
)

set (SYS_DEPENDENCY_LIBS
  $<$<PLATFORM_ID:Windows>:crypt32>
  $<$<PLATFORM_ID:Windows>:IPHlpAPI>
  $<$<PLATFORM_ID:Windows>:Mswsock>
  $<$<PLATFORM_ID:Windows>:Secur32>
  $<$<PLATFORM_ID:Windows>:Userenv>
  $<$<PLATFORM_ID:Windows>:ws2_32>
)

target_link_libraries(test-common PRIVATE
  mesos-tests
  ${MESOSTEST_LIBS}
  ${SYS_DEPENDENCY_LIBS}
)
add_test(NAME common_Tests COMMAND test-common)

if (UNIX)
  # Test binary for dockercfg
  add_executable(test-dockercfg dockercfg_tests.cpp)
  target_include_directories(test-dockercfg PRIVATE
    ${MESOS_TESTS_INCL}
  )
  target_link_libraries(test-dockercfg PRIVATE
    dockercfg
    mesos-tests
    ${MESOSTEST_LIBS}
    ${SYS_DEPENDENCY_LIBS}
  )
  add_test(NAME DockerCfg_Tests COMMAND test-dockercfg)
endif()

# TODO(akornatskyy): it crashes to load module on Windows.
if (UNIX)
  # Test binary for journald
  add_executable(test-journald journald_tests.cpp)
  target_include_directories(test-journald PRIVATE
    ${MESOS_TESTS_INCL}
  )
  target_link_libraries(test-journald PRIVATE
    libjournaldlogger
    mesos-tests
    ${MESOSTEST_LIBS}
    ${SYS_DEPENDENCY_LIBS}
  )
  add_test(NAME JournalD_Tests COMMAND test-journald)
endif()

if (UNIX)
  # Test binary for logsink
  add_executable(test-logsink logsink_tests.cpp)
  target_include_directories(test-logsink PRIVATE
    ${MESOS_TESTS_INCL}
  )
  target_link_libraries(test-logsink PRIVATE
    logsink
    mesos-tests
    ${MESOSTEST_LIBS}
    ${SYS_DEPENDENCY_LIBS}
  )
  add_test(NAME LogSink_Tests COMMAND test-logsink)
endif()

# FIXME(akornatskyy): fails during configure due to dependencies on proto
# generated files
# Overlay is disabled at the moment see ../CMakeLists.txt
# Test binary for overlay
#add_executable(test-overlay overlay_tests.cpp)
# target_link_libraries(test-overlay PUBLIC
  # overlay
  # mesos-tests
  # ${MESOSTEST_LIBS}
  # ${SYS_DEPENDENCY_LIBS}
# )
# add_test(NAME Overlay_Tests COMMAND test-overlay)

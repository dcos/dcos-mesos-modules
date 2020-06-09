
add_library(mesos-tests STATIC
  ${MESOS_SOURCE}/src/tests/cluster.cpp
  ${MESOS_SOURCE}/src/tests/containerizer.cpp
  ${MESOS_SOURCE}/src/tests/containerizer/launcher.cpp
  ${MESOS_SOURCE}/src/tests/environment.cpp
  ${MESOS_SOURCE}/src/tests/flags.cpp
  ${MESOS_SOURCE}/src/tests/main.cpp
  ${MESOS_SOURCE}/src/tests/mesos.cpp
  ${MESOS_SOURCE}/src/tests/mock_registrar.cpp
  ${MESOS_SOURCE}/src/tests/mock_slave.cpp
  ${MESOS_SOURCE}/src/tests/module.cpp
  ${MESOS_SOURCE}/src/tests/utils.cpp
)

target_include_directories(mesos-tests PUBLIC
  ${TEST_DEPENDENCIES_INC}
)

add_definitions(
  -DMODULES_BUILD_DIR="${PROJECT_BINARY_DIR}"
  -DSOURCE_DIR="${MESOS_SOURCE}"
  -DBUILD_DIR="${MESOS_BUILD}"
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
  ${TEST_DEPENDENCIES_INC}
)
target_link_libraries(test-common PRIVATE
  mesos-tests
  ${TEST_DEPENDENCIES_LIB}
)
add_test(NAME common_Tests COMMAND test-common)

# Test binary for removedockercfg
add_executable(test-removedockercfg dockercfg_tests.cpp)
target_include_directories(test-removedockercfg PRIVATE
  ${TEST_DEPENDENCIES_INC}
)
target_link_libraries(test-removedockercfg PRIVATE
  removedockercfg
  mesos-tests
  ${TEST_DEPENDENCIES_LIB}
)
add_test(NAME RemoveDockerCfg_Tests COMMAND test-removedockercfg)

# Test binary for journald
add_executable(test-journald journald_tests.cpp)
target_include_directories(test-journald PRIVATE
  ..
  ${TEST_DEPENDENCIES_INC}
)
target_link_libraries(test-journald PRIVATE
  journaldlogger
  mesos-tests
  ${TEST_DEPENDENCIES_LIB}
)
add_test(NAME JournalD_Tests COMMAND test-journald)

# Test binary for logsink
add_executable(test-logsink logsink_tests.cpp)
target_include_directories(test-logsink PRIVATE
  ${TEST_DEPENDENCIES_INC}
)
target_link_libraries(test-logsink PRIVATE
  logsink
  mesos-tests
  ${TEST_DEPENDENCIES_LIB}
)
add_test(NAME LogSink_Tests COMMAND test-logsink)

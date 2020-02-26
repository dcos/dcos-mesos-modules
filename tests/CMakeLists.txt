set(MESOS_TESTS_INCL 
  "${MESOS_BUILD_INCL}"
  "${MESOS_INCL}"
  "${MESOS_ROOT}/src"
  "${MESOS_ROOT}/build/src"
  "${PROCESS_INCL}"
  "${ZOOKEEPER_INCL}"
  "${NVML_INCL}"
  "${GOOGLEMOCK_INCL}"
  "${GOOGLETEST_INCL}"
)
    
include_directories(${MESOS_TESTS_INCL})
  
set(MESOS_TESTS_SRC
        ${MESOS_ROOT}/src/tests/main.cpp              
        ${MESOS_ROOT}/src/tests/cluster.cpp                   
        ${MESOS_ROOT}/src/tests/containerizer.cpp         
        ${MESOS_ROOT}/src/tests/environment.cpp               
        ${MESOS_ROOT}/src/tests/flags.cpp                 
        ${MESOS_ROOT}/src/tests/mesos.cpp                 
        ${MESOS_ROOT}/src/tests/module.cpp                    
        ${MESOS_ROOT}/src/tests/mock_registrar.cpp            
        ${MESOS_ROOT}/src/tests/mock_slave.cpp                
        ${MESOS_ROOT}/src/tests/utils.cpp                 
        ${MESOS_ROOT}/src/tests/containerizer/launcher.cpp            
        )

add_definitions(-DMODULES_BUILD_DIR="${PROJECT_BINARY_DIR}" -DSOURCE_DIR="${MESOS_ROOT}" -DBUILD_DIR="${MESOS_ROOT}/build" -DPKGMODULEDIR="nowhere" -DLIBDIR="nowhere" -DPKGLIBEXECDIR="nowhere" -DTESTLIBEXECDIR="nowhere" -DSBINDIR="nowhere" -DPKGDATADIR="nowhere" -DVERSION="1.10.0")

add_library(mesos-tests STATIC ${MESOS_TESTS_SRC})

include_directories(
  ..
  ../build
  "${MESOS_BUILD_INCL}"
  "${MESOS_INCL}"
  "${PROCESS_INCL}"
  "${STOUT_INCL}"
  "${ZOOKEEPER_INCL}" 
  "${NVML_INCL}"
  "${GOOGLEMOCK_INCL}"
  "${GOOGLETEST_INCL}"
  "${CURL_INCL}"
  "${PROTOBUF_INCL}"
  "${RAPIDJSON_INCL}"
  "${PICOJSON_INCL}"
  "${GLOG_INCL}"
  "${BOOST_INCL}"
)




set (MESOSTEST_LIBS 
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
  ${OPENSSL_LIBRARIES})

set (SYS_DEPENDENCY_LIBS 
  IPHlpAPI.lib
  crypt32.lib
  Ws2_32
  Userenv
  Mswsock
  Secur32.lib
)

# Test bianry for journald
add_executable(test-journald journald_tests.cpp)
target_link_libraries(test-journald PUBLIC
  libjournaldlogger
  mesos-tests
  ${MESOSTEST_LIBS}
  ${SYS_DEPENDENCY_LIBS}
)
add_test(NAME JournalD_Tests COMMAND test-journald)

# Test bianry for dockercfg
add_executable(test-dockercfg dockercfg_tests.cpp)

target_link_libraries(test-dockercfg PUBLIC
  dockercfg
  mesos-tests
  ${MESOSTEST_LIBS}
  ${SYS_DEPENDENCY_LIBS}
)

add_test(NAME DockerCfg_Tests COMMAND test-dockercfg)

# Test bianry for logsink
add_executable(test-logsink logsink_tests.cpp)

target_link_libraries(test-logsink PUBLIC
  logsink
  mesos-tests
  ${MESOSTEST_LIBS}
  ${SYS_DEPENDENCY_LIBS}
)

add_test(NAME LogSink_Tests COMMAND test-logsink)



# Test bianry for metrics
add_executable(test-metrics metrics_tests.cpp)

target_link_libraries(test-metrics PUBLIC
  metrics
  mesos-tests
  ${MESOSTEST_LIBS}
  ${SYS_DEPENDENCY_LIBS}
)

add_test(NAME Metrics_Tests COMMAND test-metrics)



# Overlay is disabled at the moment see ../CMakeLists.txt
# Test bianry for overlay
#add_executable(test-overlay overlay_tests.cpp)

# target_link_libraries(test-overlay PUBLIC
  # overlay
  # mesos-tests
  # ${MESOSTEST_LIBS}
  # ${SYS_DEPENDENCY_LIBS}
# )

# add_test(NAME Overlay_Tests COMMAND test-overlay)



# Test bianry for common
add_executable(test-common common_tests.cpp)

target_link_libraries(test-common PUBLIC
  mesos-tests
  ${MESOSTEST_LIBS}
  ${SYS_DEPENDENCY_LIBS}
)

add_test(NAME common_Tests COMMAND test-common)

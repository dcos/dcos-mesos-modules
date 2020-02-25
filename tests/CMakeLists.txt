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

add_definitions(-DMODULES_BUILD_DIR="${abs_top_builddir}" -DSOURCE_DIR="${MESOS_ROOT}" -DBUILD_DIR="${MESOS_ROOT}/build" -DPKGMODULEDIR="nowhere" -DLIBDIR="nowhere" -DPKGLIBEXECDIR="nowhere" -DTESTLIBEXECDIR="nowhere" -DSBINDIR="nowhere" -DPKGDATADIR="nowhere" -DVERSION="1.10.0")

add_library(mesos-tests STATIC ${MESOS_TESTS_SRC})

include_directories(
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

add_executable(test-journald journald_tests.cpp)

target_link_libraries(test-journald PUBLIC
  libjournaldlogger
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
  IPHlpAPI.lib
  crypt32.lib
  Ws2_32
  Userenv
  Mswsock
  Secur32.lib
)



add_test(NAME MesosModuleTests COMMAND test-journald)

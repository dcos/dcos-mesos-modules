
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
        #${MESOS_ROOT}/src/tests/script.cpp                    
        ${MESOS_ROOT}/src/tests/utils.cpp                 
        ${MESOS_ROOT}/src/tests/containerizer/launcher.cpp            
        #${MESOS_ROOT}/src/tests/containerizer/memory_test_helper.cpp
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




# if (WIN32)
    # add_definitions(-D__WINDOWS__ -DNOGDI -DNOMINMAX -D_UNICODE -DUNICODE -DGOOGLE_GLOG_DLL_DECL= -DLIBARCHIVE_STATIC -DHAVE_LIBZ -DENABLE_NVML -D_SCL_SECURE_NO_WARNINGS -D_CRT_SECURE_NO_WARNINGS -D_CRT_NONSTDC_NO_WARNINGS -DCURL_STATICLIB -DLIBSASL_EXPORTS=1 -DZOOKEEPER_VERSION="3.4.8" -DUSE_STATIC_LIB)
# endif ()

# link_directories(
                # ${MESOS_ROOT}/build/src
                # ${3RDPARTY_DIR}/zookeeper-3.4.8/src/zookeeper-3.4.8-build
                # ${3RDPARTY_DIR}/libprocess/src
                # ${3RDPARTY_DIR}/protobuf-3.5.0/src/protobuf-3.5.0-build
                # ${3RDPARTY_DIR}/glog-0.4.0/src/glog-0.4.0-build
                # ${3RDPARTY_DIR}/http_parser-2.6.2/src/http_parser-2.6.2-build
                # ${3RDPARTY_DIR}/zlib-1.2.8/src/zlib-1.2.8-lib/lib
                # ${3RDPARTY_DIR}/bzip2-1.0.6/src/bzip2-1.0.6-lib )

add_executable(test-journald journald_tests.cpp)



target_link_libraries(test-journald PUBLIC
  libjournaldlogger
  mesos-tests
  ${MESOS_PROTOBUFS_LIB}
  ${MESOS_LIB}
  ${PROCESS_LIB}                                
  ${ZOOKEEPER_LIB}
  ${ZOOKEEPER_HASHTABLE_LIB}
  optimized ${PROTOBUF_LIB_RELEASE}
  debug     ${PROTOBUF_LIB_DEBUG}
  optimized ${GLOG_LIB_RELEASE}
  debug     ${GLOG_LIB_DEBUG}
  optimized ${ZLIB_LIB_STATIC_RELEASE}
  debug     ${ZLIB_LIB_STATIC_DEBUG}
  optimized ${GOOGLEMOCK_LIB_RELEASE}
  debug     ${GOOGLEMOCK_LIB_DEBUG}
  optimized ${GOOGLETEST_LIB_RELEASE}
  debug     ${GOOGLETEST_LIB_DEBUG}
  optimized ${SASL_LIB_RELEASE}
  debug     ${SASL_LIB_DEBUG}
  optimized ${BZIP2_LIB_RELEASE}
  debug     ${BZIP2_LIB_DEBUG}
  optimized ${CURL_LIB_RELEASE}
  debug     ${CURL_LIB_DEBUG}
  ${HTTP_PARSER_LIB}
  IPHlpAPI.lib
  crypt32.lib
  Ws2_32
  Userenv
  Mswsock
  Secur32.lib
)


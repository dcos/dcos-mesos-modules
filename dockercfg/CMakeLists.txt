
include_directories(
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
)

add_library(dockercfg SHARED remover.cpp)

target_link_libraries(dockercfg PRIVATE
  optimized ${GLOG_LIB_RELEASE}
  debug     ${GLOG_LIB_DEBUG}
  optimized ${PROTOBUF_LIB_RELEASE}
  debug     ${PROTOBUF_LIB_DEBUG}
  ${PROCESS_LIB}
  ${MESOS_PROTOBUFS_LIB}
  ${MESOS_LIB}
)

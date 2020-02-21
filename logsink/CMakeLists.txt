
include_directories(
  "${BOOST_INCL}"
  "${GLOG_INCL}"
  "${PROTOBUF_INCL}"
  "${PICOJSON_INCL}"
  "${RAPIDJSON_INCL}"
  "${PROCESS_INCL}"
  "${STOUT_INCL}"
  "${MESOS_BUILD_INCL}"
  "${MESOS_INCL}"
)

add_library(logsink SHARED logsink.cpp)

target_link_libraries(logsink PRIVATE
  optimized ${GLOG_LIB_RELEASE}
  debug     ${GLOG_LIB_DEBUG}
  Ws2_32
)

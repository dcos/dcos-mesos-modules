
add_library(logsink SHARED logsink.cpp)

target_include_directories(logsink PRIVATE
  "${BOOST_INCL}"
  "${GLOG_INCL}"
  "${MESOS_ROOT}/build/include"
  "${MESOS_ROOT}/include"
  "${PICOJSON_INCL}"
  "${PROCESS_INCL}"
  "${PROTOBUF_INCL}"
  "${RAPIDJSON_INCL}"
  "${STOUT_INCL}"
)

target_link_libraries(logsink PRIVATE
  glog
  $<$<PLATFORM_ID:Windows>:ws2_32>
)

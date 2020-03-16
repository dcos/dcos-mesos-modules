
add_library(dockercfg SHARED remover.cpp)

target_include_directories(dockercfg PRIVATE
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

target_link_libraries(dockercfg PRIVATE
  glog
  mesos
  mesos_protobufs
  process
  protobuf
)

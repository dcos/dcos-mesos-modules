
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

add_library(libjournaldlogger SHARED lib_journald.cpp)

target_link_libraries(libjournaldlogger PUBLIC
  ${ZLIB_LIB_STATIC}
  ${PROCESS_LIB}
  ${GLOG_LIB}
  ${HTTP_PARSER_LIB}
  ${PROTOBUF_LIB}
  ${OPENSSL_LIBRARIES}
  ${MESOS_PROTOBUFS_LIB}
  ${MESOS_LIB}
  Mswsock
  Userenv
  Ws2_32
)

add_executable(mesos-journald-logger journald.cpp)

target_link_libraries(mesos-journald-logger PRIVATE
  libjournaldlogger
)

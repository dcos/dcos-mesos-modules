
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
  ${ZLIB_LIB}
  ${PROCESS_LIB}
  optimized ${GLOG_LIB_RELEASE}
  debug     ${GLOG_LIB_DEBUG}
  ${HTTP_PARSER_LIB}
  optimized ${PROTOBUF_LIB_RELEASE}
  debug     ${PROTOBUF_LIB_DEBUG}
  ${MESOS_PROTOBUFS_LIB}
  ${MESOS_LIB}
  Mswsock
  Userenv
  Ws2_32
  ${OPENSSL_CRYPTO_LIBRARY}
  ${OPENSSL_SSL_LIBRARY}
)

add_executable(mesos-journald-logger journald.cpp)

target_link_libraries(mesos-journald-logger PRIVATE
  libjournaldlogger
)

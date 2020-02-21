
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
  # FIXME: due to limitation of protobuf generation process
  # and include in source
  ${CMAKE_CURRENT_BINARY_DIR}/..
)

add_library(metrics SHARED isolator.cpp)

protobuf_generate(
  LANGUAGE cpp
  TARGET metrics
  # OUT_VAR PROTO_SRCS PROTO_HDRS
  PROTOS messages.proto
)

target_link_libraries(metrics PRIVATE
  optimized ${GLOG_LIB_RELEASE}
  debug     ${GLOG_LIB_DEBUG}
  ${HTTP_PARSER_LIB}
  ${PROCESS_LIB}
  optimized ${PROTOBUF_LIB_RELEASE}
  debug     ${PROTOBUF_LIB_DEBUG}
  ${ZLIB_LIB}
  ${OPENSSL_LIBRARIES}
  ${MESOS_PROTOBUFS_LIB}
  ${MESOS_LIB}
  Mswsock
  Userenv
  Ws2_32
  ${OPENSSL_CRYPTO_LIBRARY}
  ${OPENSSL_SSL_LIBRARY}
)

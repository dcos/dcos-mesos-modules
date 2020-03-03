
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

target_link_libraries(metrics PRIVATE
  ${GLOG_LIB}
  ${HTTP_PARSER_LIB}
  ${PROCESS_LIB}
  ${PROTOBUF_LIB}
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

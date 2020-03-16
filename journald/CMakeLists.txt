
add_library(libjournaldlogger SHARED lib_journald.cpp)

target_include_directories(libjournaldlogger PUBLIC
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

target_link_libraries(libjournaldlogger PUBLIC
  ${OPENSSL_LIBRARIES}
  glog
  http_parser
  mesos
  mesos_protobufs
  process
  protobuf
  zlib
  $<$<PLATFORM_ID:Windows>:Mswsock>
  $<$<PLATFORM_ID:Windows>:Userenv>
  $<$<PLATFORM_ID:Windows>:ws2_32>
)

add_executable(mesos-journald-logger journald.cpp)

target_link_libraries(mesos-journald-logger PRIVATE
  libjournaldlogger
)

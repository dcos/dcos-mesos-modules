
include_directories(
  "${BOOST_INCL}"
  "${GLOG_INCL}"
  "${PROTOBUF_INCL}"
  "${PICOJSON_INCL}"
  "${RAPIDJSON_INCL}"
  "${PROCESS_INCL}"
  "${STOUT_INCL}"
  "${CURL_INCL}"
  "${ZOOKEEPER_INCL}"
  "${MESOS_ROOT}/build/include"
  "${MESOS_ROOT}/include"
  # FIXME(akornatskyy): due to limitation of protobuf generation process
  # and include in source
  ${CMAKE_CURRENT_BINARY_DIR}/..
  # FIXME(akornatskyy): due to the following in agent.hpp
  # #include <overlay/agent_metrics.hpp>
  # #include <overlay/messages.hpp>
  ${CMAKE_CURRENT_SOURCE_DIR}/..
)

add_library(overlay SHARED
  agent_metrics.cpp
  agent.cpp
  # FIXME(akornatskyy):
  # LNK2019: unresolved external symbol "public: __cdecl mesos:: log::Log::Log
  # `log` is explicitly excluded for Windows in mesos
  # master_metrics.cpp
  # master.cpp
  supervisor_metrics.cpp
)

protobuf_generate(
  LANGUAGE cpp
  TARGET overlay
  # FIXME(akornatskyy): you need to fix path in messages.proto to be
  # import "overlay.proto";
  PROTOS messages.proto overlay.proto
)

message(" ==> ${PROTO_SRCS} ${PROTO_HDRS}")

target_link_libraries(overlay PRIVATE
  ${GLOG_LIB}
  ${HTTP_PARSER_LIB}
  ${PROCESS_LIB}
  ${PROTOBUF_LIB}
  ${ZLIB_LIB_STATIC}
  ${ZOOKEEPER_HASHTABLE_LIB}
  ${ZOOKEEPER_LIB}
  ${MESOS_PROTOBUFS_LIB}
  ${MESOS_LIB}
  Mswsock
  Userenv
  $<$<PLATFORM_ID:Windows>:ws2_32>
)

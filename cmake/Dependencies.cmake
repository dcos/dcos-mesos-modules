set (GLOG_INCL "${MESOS_ROOT}/build/3rdparty/glog-${GLOG_VERSION}/src/glog-${GLOG_VERSION}-install/include")
set (BOOST_INCL "${MESOS_ROOT}/build/3rdparty/boost-${BOOST_VERSION}/src/boost-${BOOST_VERSION}")
set (PROTOBUF_INCL "${MESOS_ROOT}/build/3rdparty/protobuf-${PROTOBUF_VERSION}/src/protobuf-${PROTOBUF_VERSION}/src")
set (PICOJSON_INCL "${MESOS_ROOT}/build/3rdparty/picojson-${PICOJSON_VERSION}/src/picojson-${PICOJSON_VERSION}")
set (RAPIDJSON_INCL "${MESOS_ROOT}/build/3rdparty/rapidjson-${RAPIDJSON_VERSION}/src/rapidjson-${RAPIDJSON_VERSION}/include")
set (PROCESS_INCL "${MESOS_ROOT}/3rdparty/libprocess/include")
set (STOUT_INCL "${MESOS_ROOT}/3rdparty/stout/include")
set (CURL_INCL "${MESOS_ROOT}/build/3rdparty/curl-${CURL_VERSION}/src/curl-${CURL_VERSION}/include")
set (ZOOKEEPER_INCL
  "${MESOS_ROOT}/build/3rdparty/zookeeper-${ZOOKEEPER_VERSION}/src/zookeeper-${ZOOKEEPER_VERSION}/src/c/generated"
  "${MESOS_ROOT}/build/3rdparty/zookeeper-${ZOOKEEPER_VERSION}/src/zookeeper-${ZOOKEEPER_VERSION}/src/c/include"
)

set (MESOS_BUILD_INCL "${MESOS_ROOT}/build/include")
set (MESOS_INCL "${MESOS_ROOT}/include")


find_library(
  GLOG_LIB
  NAMES glog glogd
  PATHS ${MESOS_ROOT}/build/3rdparty/glog-${GLOG_VERSION}/src/glog-${GLOG_VERSION}-build
  PATH_SUFFIXES Debug Release
)

find_library(
  PROTOBUF_LIB
  NAMES libprotobuf libprotobufd
  PATHS ${MESOS_ROOT}/build/3rdparty/protobuf-${PROTOBUF_VERSION}/src/protobuf-${PROTOBUF_VERSION}-build
  PATH_SUFFIXES Debug Release
)

find_library(
  PROCESS_LIB
  NAMES process
  PATHS ${MESOS_ROOT}/build/3rdparty/libprocess/src
  PATH_SUFFIXES Debug Release
)

find_library(
  ZLIB_LIB
  NAMES zlib zlibd
  PATHS  ${MESOS_ROOT}/build/3rdparty/zlib-${ZLIB_VERSION}/src/zlib-${ZLIB_VERSION}-build
  PATH_SUFFIXES Debug Release
)

find_library(
  HTTP_PARSER_LIB
  NAMES http_parser
  PATHS ${MESOS_ROOT}/build/3rdparty/http_parser-${HTTP_PARSER_VERSION}/src/http_parser-${HTTP_PARSER_VERSION}-build
  PATH_SUFFIXES Debug Release
)

find_library(
  ZOOKEEPER_LIB
  NAMES zookeeper
  PATHS ${MESOS_ROOT}/build/3rdparty/zookeeper-${ZOOKEEPER_VERSION}/src/zookeeper-${ZOOKEEPER_VERSION}-build
  PATH_SUFFIXES Debug Release
)

find_library(
  ZOOKEEPER_HASHTABLE_LIB
  NAMES hashtable
  PATHS ${MESOS_ROOT}/build/3rdparty/zookeeper-${ZOOKEEPER_VERSION}/src/zookeeper-${ZOOKEEPER_VERSION}-build
  PATH_SUFFIXES Debug Release
)

find_library(
  MESOS_PROTOBUFS_LIB
  NAMES mesos-protobufs
  PATHS ${MESOS_ROOT}/build/src
  PATH_SUFFIXES Debug Release
)

find_library(
  MESOS_LIB
  NAMES mesos
  PATHS ${MESOS_ROOT}/build/src
  PATH_SUFFIXES Debug Release
)

SET(Protobuf_DIR ${MESOS_ROOT}/build/3rdparty/protobuf-${PROTOBUF_VERSION}/src/protobuf-${PROTOBUF_VERSION}-build/cmake)
find_package(Protobuf CONFIG REQUIRED)

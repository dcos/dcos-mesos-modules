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
set (NVML_INCL "${MESOS_ROOT}/build/3rdparty/nvml-${NVML_VERSION}")
set (GOOGLEMOCK_INCL "${MESOS_ROOT}/build/3rdparty/googletest-${GOOGLETEST_VERSION}/src/googletest-${GOOGLETEST_VERSION}/googlemock/include")
set (GOOGLETEST_INCL "${MESOS_ROOT}/build/3rdparty/googletest-${GOOGLETEST_VERSION}/src/googletest-${GOOGLETEST_VERSION}/googletest/include")


set (MESOS_BUILD_INCL "${MESOS_ROOT}/build/include")
set (MESOS_INCL "${MESOS_ROOT}/include")


find_library(
  GLOG_LIB_RELEASE
  NAMES glog
  PATHS ${MESOS_ROOT}/build/3rdparty/glog-${GLOG_VERSION}/src/glog-${GLOG_VERSION}-build
  PATH_SUFFIXES Release
)

find_library(
  GLOG_LIB_DEBUG
  NAMES glogd
  PATHS ${MESOS_ROOT}/build/3rdparty/glog-${GLOG_VERSION}/src/glog-${GLOG_VERSION}-build
  PATH_SUFFIXES Debug
)


find_library(
  PROTOBUF_LIB_RELEASE
  NAMES libprotobuf
  PATHS ${MESOS_ROOT}/build/3rdparty/protobuf-${PROTOBUF_VERSION}/src/protobuf-${PROTOBUF_VERSION}-build
  PATH_SUFFIXES Release
)

find_library(
  PROTOBUF_LIB_DEBUG
  NAMES libprotobufd
  PATHS ${MESOS_ROOT}/build/3rdparty/protobuf-${PROTOBUF_VERSION}/src/protobuf-${PROTOBUF_VERSION}-build
  PATH_SUFFIXES Debug
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
  ZLIB_LIB_STATIC_DEBUG
  NAMES zlibstaticd
  PATHS  ${MESOS_ROOT}/build/3rdparty/zlib-${ZLIB_VERSION}/src/zlib-${ZLIB_VERSION}-build
  PATH_SUFFIXES Debug
)

find_library(
  ZLIB_LIB_STATIC_RELEASE
  NAMES zlibstatic
  PATHS  ${MESOS_ROOT}/build/3rdparty/zlib-${ZLIB_VERSION}/src/zlib-${ZLIB_VERSION}-build/Release
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

find_package(OpenSSL REQUIRED)

find_library(
  GOOGLEMOCK_LIB_DEBUG
  NAMES gmock
  PATHS ${MESOS_ROOT}/build/3rdparty/googletest-1.8.0/src/googletest-${GOOGLETEST_VERSION}-build/googlemock/Debug
)

find_library(
  GOOGLEMOCK_LIB_RELEASE
  NAMES gmock
  PATHS ${MESOS_ROOT}/build/3rdparty/googletest-1.8.0/src/googletest-${GOOGLETEST_VERSION}-build/googlemock/Release/
)

find_library(
  GOOGLETEST_LIB_DEBUG
  NAMES gtest
  PATHS ${MESOS_ROOT}/build/3rdparty/googletest-1.8.0/src/googletest-${GOOGLETEST_VERSION}-build/googlemock/gtest/Debug
)

find_library(
  GOOGLETEST_LIB_RELEASE
  NAMES gtest
  PATHS  ${MESOS_ROOT}/build/3rdparty/googletest-1.8.0/src/googletest-${GOOGLETEST_VERSION}-build/googlemock/gtest/Debug
)

find_library(
  SASL_LIB_RELEASE
  NAMES libsasl2
  PATHS  ${MESOS_ROOT}/build/3rdparty/sasl2-2.1.27rc3/src/sasl2-2.1.27rc3-build/Release/
)

find_library(
  SASL_LIB_DEBUG
  NAMES libsasl2
  PATHS  ${MESOS_ROOT}/build/3rdparty/sasl2-2.1.27rc3/src/sasl2-2.1.27rc3-build/Debug/
)

find_library(
  BZIP2_LIB_DEBUG
  NAMES bzip2.lib
  PATHS  ${MESOS_ROOT}/build/3rdparty/bzip2-${BZIP2_VERSION}/src/bzip2-${BZIP2_VERSION}-build
  PATH_SUFFIXES Debug
)

find_library(
  BZIP2_LIB_RELEASE
  NAMES bzip2.lib
  PATHS  ${MESOS_ROOT}/build/3rdparty/bzip2-${BZIP2_VERSION}/src/bzip2-${BZIP2_VERSION}-build
  PATH_SUFFIXES Release
)

find_library(
  CURL_LIB_RELEASE
  NAMES libcurl
  PATHS  ${MESOS_ROOT}/build/3rdparty/curl-${CURL_VERSION}/src/curl-${CURL_VERSION}-build/lib/
  PATH_SUFFIXES Release
)

find_library(
  CURL_LIB_DEBUG
  NAMES libcurl-d
  PATHS  ${MESOS_ROOT}/build/3rdparty/curl-${CURL_VERSION}/src/curl-${CURL_VERSION}-build/lib
  PATH_SUFFIXES Debug
)


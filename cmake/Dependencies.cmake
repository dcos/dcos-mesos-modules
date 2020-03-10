set (GLOG_INCL "${MESOS_ROOT}/build/3rdparty/glog-${GLOG_VERSION}/src/glog-${GLOG_VERSION}-install/include")
set (BOOST_INCL "${MESOS_ROOT}/build/3rdparty/boost-${BOOST_VERSION}/src/boost-${BOOST_VERSION}")
set (PROTOBUF_INCL "${MESOS_ROOT}/build/3rdparty/protobuf-${PROTOBUF_VERSION}/src/protobuf-${PROTOBUF_VERSION}/src")
set (PICOJSON_INCL "${MESOS_ROOT}/build/3rdparty/picojson-${PICOJSON_VERSION}/src/picojson-${PICOJSON_VERSION}")
set (RAPIDJSON_INCL "${MESOS_ROOT}/build/3rdparty/rapidjson-${RAPIDJSON_VERSION}/src/rapidjson-${RAPIDJSON_VERSION}/include")
set (PROCESS_INCL "${MESOS_ROOT}/3rdparty/libprocess/include")
set (STOUT_INCL "${MESOS_ROOT}/3rdparty/stout/include")
set (CURL_INCL "${MESOS_ROOT}/build/3rdparty/curl-${CURL_VERSION}/src/curl-${CURL_VERSION}/include")
set (SASL2_INCL "${MESOS_ROOT}/build/3rdparty/sasl2-${SASL2_VERSION}/src/sasl2-${SASL2_VERSION}/include")
set (ZOOKEEPER_INCL
  "${MESOS_ROOT}/build/3rdparty/zookeeper-${ZOOKEEPER_VERSION}/src/zookeeper-${ZOOKEEPER_VERSION}/src/c/generated"
  "${MESOS_ROOT}/build/3rdparty/zookeeper-${ZOOKEEPER_VERSION}/src/zookeeper-${ZOOKEEPER_VERSION}/src/c/include"
)

set (NVML_INCL "${MESOS_ROOT}/build/3rdparty/nvml-${NVML_VERSION}")
set (GMOCK_INCL "${MESOS_ROOT}/build/3rdparty/googletest-${GOOGLETEST_VERSION}/src/googletest-${GOOGLETEST_VERSION}/googlemock/include")
set (GTEST_INCL "${MESOS_ROOT}/build/3rdparty/googletest-${GOOGLETEST_VERSION}/src/googletest-${GOOGLETEST_VERSION}/googletest/include")

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
add_library(glog STATIC IMPORTED)
set_target_properties(glog PROPERTIES IMPORTED_LOCATION_DEBUG "${GLOG_LIB_DEBUG}")
set_target_properties(glog PROPERTIES IMPORTED_LOCATION_RELEASE "${GLOG_LIB_RELEASE}")

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
add_library(protobuf STATIC IMPORTED)
set_target_properties(protobuf PROPERTIES IMPORTED_LOCATION_DEBUG "${PROTOBUF_LIB_DEBUG}")
set_target_properties(protobuf PROPERTIES IMPORTED_LOCATION_RELEASE "${PROTOBUF_LIB_RELEASE}")

find_library(
  PROCESS_LIB_RELEASE
  NAMES process
  PATHS ${MESOS_ROOT}/build/3rdparty/libprocess/src
  PATH_SUFFIXES Release
)
find_library(
  PROCESS_LIB_DEBUG
  NAMES process
  PATHS ${MESOS_ROOT}/build/3rdparty/libprocess/src
  PATH_SUFFIXES Debug
)
add_library(process STATIC IMPORTED)
set_target_properties(process PROPERTIES IMPORTED_LOCATION_DEBUG "${PROCESS_LIB_DEBUG}")
set_target_properties(process PROPERTIES IMPORTED_LOCATION_RELEASE "${PROCESS_LIB_RELEASE}")

find_library(
  ZLIB_LIB_RELEASE
  NAMES zlibstatic
  PATHS  ${MESOS_ROOT}/build/3rdparty/zlib-${ZLIB_VERSION}/src/zlib-${ZLIB_VERSION}-build
  PATH_SUFFIXES Release
)
find_library(
  ZLIB_LIB_DEBUG
  NAMES zlibstaticd
  PATHS  ${MESOS_ROOT}/build/3rdparty/zlib-${ZLIB_VERSION}/src/zlib-${ZLIB_VERSION}-build
  PATH_SUFFIXES Debug
)
add_library(zlib STATIC IMPORTED)
set_target_properties(zlib PROPERTIES IMPORTED_LOCATION_DEBUG "${ZLIB_LIB_DEBUG}")
set_target_properties(zlib PROPERTIES IMPORTED_LOCATION_RELEASE "${ZLIB_LIB_RELEASE}")

find_library(
  BZIP2_LIB_DEBUG
  NAMES bzip2
  PATHS  ${MESOS_ROOT}/build/3rdparty/bzip2-${BZIP2_VERSION}/src/bzip2-${BZIP2_VERSION}-build
  PATH_SUFFIXES Debug
)
find_library(
  BZIP2_LIB_RELEASE
  NAMES bzip2
  PATHS  ${MESOS_ROOT}/build/3rdparty/bzip2-${BZIP2_VERSION}/src/bzip2-${BZIP2_VERSION}-build
  PATH_SUFFIXES Release
)
add_library(bzip2 STATIC IMPORTED)
set_target_properties(bzip2 PROPERTIES IMPORTED_LOCATION_DEBUG "${BZIP2_LIB_DEBUG}")
set_target_properties(bzip2 PROPERTIES IMPORTED_LOCATION_RELEASE "${BZIP2_LIB_RELEASE}")

find_library(
  CURL_LIB_RELEASE
  NAMES libcurl
  PATHS  ${MESOS_ROOT}/build/3rdparty/curl-${CURL_VERSION}/src/curl-${CURL_VERSION}-build/lib
  PATH_SUFFIXES Release
)
find_library(
  CURL_LIB_DEBUG
  NAMES libcurl-d
  PATHS  ${MESOS_ROOT}/build/3rdparty/curl-${CURL_VERSION}/src/curl-${CURL_VERSION}-build/lib
  PATH_SUFFIXES Debug
)
add_library(curl STATIC IMPORTED)
set_target_properties(curl PROPERTIES IMPORTED_LOCATION_DEBUG "${CURL_LIB_DEBUG}")
set_target_properties(curl PROPERTIES IMPORTED_LOCATION_RELEASE "${CURL_LIB_RELEASE}")

find_library(
  SASL2_LIB_RELEASE
  NAMES libsasl2
  PATHS  ${MESOS_ROOT}/build/3rdparty/sasl2-${SASL2_VERSION}/src/sasl2-${SASL2_VERSION}-build
  PATH_SUFFIXES Release
)
find_library(
  SASL2_LIB_DEBUG
  NAMES libsasl2
  PATHS  ${MESOS_ROOT}/build/3rdparty/sasl2-${SASL2_VERSION}/src/sasl2-${SASL2_VERSION}-build
  PATH_SUFFIXES Debug
)
add_library(sasl2 STATIC IMPORTED)
set_target_properties(sasl2 PROPERTIES IMPORTED_LOCATION_DEBUG "${SASL2_LIB_DEBUG}")
set_target_properties(sasl2 PROPERTIES IMPORTED_LOCATION_RELEASE "${SASL2_LIB_RELEASE}")

find_library(
  HTTP_PARSER_LIB_RELEASE
  NAMES http_parser
  PATHS ${MESOS_ROOT}/build/3rdparty/http_parser-${HTTP_PARSER_VERSION}/src/http_parser-${HTTP_PARSER_VERSION}-build
  PATH_SUFFIXES Release
)
find_library(
  HTTP_PARSER_LIB_DEBUG
  NAMES http_parser
  PATHS ${MESOS_ROOT}/build/3rdparty/http_parser-${HTTP_PARSER_VERSION}/src/http_parser-${HTTP_PARSER_VERSION}-build
  PATH_SUFFIXES Debug
)
add_library(http_parser STATIC IMPORTED)
set_target_properties(http_parser PROPERTIES IMPORTED_LOCATION_DEBUG "${HTTP_PARSER_LIB_DEBUG}")
set_target_properties(http_parser PROPERTIES IMPORTED_LOCATION_RELEASE "${HTTP_PARSER_LIB_RELEASE}")

find_library(
  ZOOKEEPER_LIB_RELEASE
  NAMES zookeeper
  PATHS ${MESOS_ROOT}/build/3rdparty/zookeeper-${ZOOKEEPER_VERSION}/src/zookeeper-${ZOOKEEPER_VERSION}-build
  PATH_SUFFIXES Release
)
find_library(
  ZOOKEEPER_LIB_DEBUG
  NAMES zookeeper
  PATHS ${MESOS_ROOT}/build/3rdparty/zookeeper-${ZOOKEEPER_VERSION}/src/zookeeper-${ZOOKEEPER_VERSION}-build
  PATH_SUFFIXES Debug
)
add_library(zookeeper STATIC IMPORTED)
set_target_properties(zookeeper PROPERTIES IMPORTED_LOCATION_DEBUG "${ZOOKEEPER_LIB_DEBUG}")
set_target_properties(zookeeper PROPERTIES IMPORTED_LOCATION_RELEASE "${ZOOKEEPER_LIB_RELEASE}")

find_library(
  ZOOKEEPER_HASHTABLE_LIB_RELEASE
  NAMES hashtable
  PATHS ${MESOS_ROOT}/build/3rdparty/zookeeper-${ZOOKEEPER_VERSION}/src/zookeeper-${ZOOKEEPER_VERSION}-build
  PATH_SUFFIXES Release
)
find_library(
  ZOOKEEPER_HASHTABLE_LIB_DEBUG
  NAMES hashtable
  PATHS ${MESOS_ROOT}/build/3rdparty/zookeeper-${ZOOKEEPER_VERSION}/src/zookeeper-${ZOOKEEPER_VERSION}-build
  PATH_SUFFIXES Debug
)
add_library(zookeeper_hashtable STATIC IMPORTED)
set_target_properties(zookeeper_hashtable PROPERTIES IMPORTED_LOCATION_DEBUG "${ZOOKEEPER_HASHTABLE_LIB_DEBUG}")
set_target_properties(zookeeper_hashtable PROPERTIES IMPORTED_LOCATION_RELEASE "${ZOOKEEPER_HASHTABLE_LIB_RELEASE}")

find_library(
  MESOS_PROTOBUFS_LIB_RELEASE
  NAMES mesos-protobufs
  PATHS ${MESOS_ROOT}/build/src
  PATH_SUFFIXES Release
)
find_library(
  MESOS_PROTOBUFS_LIB_DEBUG
  NAMES mesos-protobufs
  PATHS ${MESOS_ROOT}/build/src
  PATH_SUFFIXES Debug
)
add_library(mesos_protobufs STATIC IMPORTED)
set_target_properties(mesos_protobufs PROPERTIES IMPORTED_LOCATION_DEBUG "${MESOS_PROTOBUFS_LIB_DEBUG}")
set_target_properties(mesos_protobufs PROPERTIES IMPORTED_LOCATION_RELEASE "${MESOS_PROTOBUFS_LIB_RELEASE}")

find_library(
  MESOS_LIB_RELEASE
  NAMES mesos
  PATHS ${MESOS_ROOT}/build/src
  PATH_SUFFIXES Release
)
find_library(
  MESOS_LIB_DEBUG
  NAMES mesos
  PATHS ${MESOS_ROOT}/build/src
  PATH_SUFFIXES Debug
)
add_library(mesos STATIC IMPORTED)
set_target_properties(mesos PROPERTIES IMPORTED_LOCATION_DEBUG "${MESOS_LIB_DEBUG}")
set_target_properties(mesos PROPERTIES IMPORTED_LOCATION_RELEASE "${MESOS_LIB_RELEASE}")

SET(Protobuf_DIR ${MESOS_ROOT}/build/3rdparty/protobuf-${PROTOBUF_VERSION}/src/protobuf-${PROTOBUF_VERSION}-build/cmake)
find_package(Protobuf CONFIG REQUIRED)

# https://cmake.org/cmake/help/v3.6/module/FindOpenSSL.html
find_package(OpenSSL REQUIRED)

find_library(
  GMOCK_LIB_RELEASE
  NAMES gmock
  PATHS "${MESOS_ROOT}/build/3rdparty/googletest-${GOOGLETEST_VERSION}/src/googletest-${GOOGLETEST_VERSION}-build/googlemock/"
  PATH_SUFFIXES Release
)
find_library(
  GMOCK_LIB_DEBUG
  NAMES gmock
  PATHS "${MESOS_ROOT}/build/3rdparty/googletest-${GOOGLETEST_VERSION}/src/googletest-${GOOGLETEST_VERSION}-build/googlemock/"
  PATH_SUFFIXES Debug
)
add_library(gmock STATIC IMPORTED)
set_target_properties(gmock PROPERTIES IMPORTED_LOCATION_DEBUG "${GMOCK_LIB_DEBUG}")
set_target_properties(gmock PROPERTIES IMPORTED_LOCATION_RELEASE "${GMOCK_LIB_RELEASE}")

find_library(
  GTEST_LIB_RELEASE
  NAMES gtest
  PATHS "${MESOS_ROOT}/build/3rdparty/googletest-${GOOGLETEST_VERSION}/src/googletest-${GOOGLETEST_VERSION}-build/googlemock/gtest"
  PATH_SUFFIXES Release
)
find_library(
  GTEST_LIB_DEBUG
  NAMES gtest
  PATHS "${MESOS_ROOT}/build/3rdparty/googletest-${GOOGLETEST_VERSION}/src/googletest-${GOOGLETEST_VERSION}-build/googlemock/gtest"
  PATH_SUFFIXES Debug
)
add_library(gtest STATIC IMPORTED)
set_target_properties(gtest PROPERTIES IMPORTED_LOCATION_DEBUG "${GTEST_LIB_DEBUG}")
set_target_properties(gtest PROPERTIES IMPORTED_LOCATION_RELEASE "${GTEST_LIB_RELEASE}")

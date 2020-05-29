set(LIBRARY_LINKAGE SHARED)

set (BOOST_INCL     ${BOOST_ROOT_DIR}/include)

set (PICOJSON_INCL  ${MESOS_ROOT}/include)
set (RAPIDJSON_INCL ${MESOS_ROOT}/include)
set (STOUT_INCL     ${MESOS_ROOT}/include)
set (ZOOKEEPER_INCL ${MESOS_ROOT}/include/zookeeper)

set (GLOG_INCL ${MESOS_ROOT}/include)
find_library(
  GLOG_LIB
  NAMES glog
  PATHS ${MESOS_ROOT}/lib
  NO_DEFAULT_PATH)
add_library(glog ${LIBRARY_LINKAGE} IMPORTED)
set_target_properties(glog PROPERTIES IMPORTED_LOCATION ${GLOG_LIB})

set (PROCESS_INCL ${MESOS_ROOT}/include)
find_library(
  PROCESS_LIB
  NAMES process
  PATHS ${MESOS_ROOT}/lib
  NO_DEFAULT_PATH)
add_library(process ${LIBRARY_LINKAGE} IMPORTED)
set_target_properties(process PROPERTIES IMPORTED_LOCATION ${PROCESS_LIB})

set (PROTOBUF_INCL ${MESOS_ROOT}/include)
find_library(
  PROTOBUF_LIB
  NAMES protobuf
  PATHS ${MESOS_ROOT}/lib
  NO_DEFAULT_PATH)
add_library(protobuf ${LIBRARY_LINKAGE} IMPORTED)
set_target_properties(protobuf PROPERTIES IMPORTED_LOCATION ${PROTOBUF_LIB})

find_library(
  MESOS_PROTOBUFS_LIB
  NAMES mesos-protobufs
  PATHS ${MESOS_ROOT}/lib
  NO_DEFAULT_PATH)
add_library(mesos_protobufs ${LIBRARY_LINKAGE} IMPORTED)
set_target_properties(mesos_protobufs PROPERTIES IMPORTED_LOCATION ${MESOS_PROTOBUFS_LIB})

find_library(
  MESOS_LIB
  NAMES mesos
  PATHS ${MESOS_ROOT}/lib
  NO_DEFAULT_PATH)
add_library(mesos ${LIBRARY_LINKAGE} IMPORTED)
set_target_properties(mesos PROPERTIES IMPORTED_LOCATION ${MESOS_LIB})

find_program(
  PROTOC
  name protoc
  PATHS ${MESOS_ROOT}/bin
  NO_DEFAULT_PATH)
add_executable(protoc IMPORTED GLOBAL)
set_target_properties(protoc PROPERTIES IMPORTED_LOCATION ${PROTOC})

if (BUILD_TESTING)
  # TODO(akornatskyy): rewrite to use one from mesos install
  set (GMOCK_INCL "${MESOS_ROOT}/build/3rdparty/googletest-${GOOGLETEST_VERSION}/src/googletest-${GOOGLETEST_VERSION}/googlemock/include")
  set (GTEST_INCL "${MESOS_ROOT}/build/3rdparty/googletest-${GOOGLETEST_VERSION}/src/googletest-${GOOGLETEST_VERSION}/googletest/include")
  find_library(
    GMOCK_LIB
    NAMES gmock
    PATHS ${MESOS_ROOT}/build/3rdparty/googletest-${GOOGLETEST_VERSION}/src/googletest-${GOOGLETEST_VERSION}-build/googlemock)
  add_library(gmock ${LIBRARY_LINKAGE} IMPORTED)
  set_target_properties(gmock PROPERTIES IMPORTED_LOCATION "${GMOCK_LIB}")
endif()

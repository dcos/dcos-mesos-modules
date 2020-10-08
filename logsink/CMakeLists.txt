
add_library(logsink SHARED logsink.cpp)
install(TARGETS logsink LIBRARY DESTINATION lib/mesos)

target_include_directories(logsink PRIVATE
  ${DEPENDENCIES_INC}
)

target_link_libraries(logsink PUBLIC
  ${DEPENDENCIES_LIB}
)

# TODO(asekretenko): After ditching autotools build, gather all testing
# of this module (including modules.json) into a subdirectory.
if (BUILD_TESTING)
  file(GENERATE
    OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/modules.json"
    INPUT  "${CMAKE_CURRENT_SOURCE_DIR}/modules.json.cmake-in")
endif()


add_library(removedockercfg SHARED remover.cpp)
install(TARGETS removedockercfg LIBRARY DESTINATION "lib/mesos")

target_include_directories(removedockercfg PRIVATE
  ${DEPENDENCIES_INC}
)

target_link_libraries(removedockercfg PUBLIC
  ${DEPENDENCIES_LIB}
)

# TODO(asekretenko): After ditching autotools build, gather all testing
# of this module (including modules.json) into a subdirectory.
if (BUILD_TESTING)
  file(GENERATE
    OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/modules.json"
    INPUT  "${CMAKE_CURRENT_SOURCE_DIR}/modules.json.cmake-in")
endif()


add_library(logsink SHARED logsink.cpp)
install(TARGETS logsink LIBRARY DESTINATION lib/mesos)

target_include_directories(logsink PRIVATE
  ${DEPENDENCIES_INC}
)

target_link_libraries(logsink PUBLIC
  ${DEPENDENCIES_LIB}
)

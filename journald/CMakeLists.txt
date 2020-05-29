
add_library(journaldlogger SHARED lib_journald.cpp)
install(TARGETS journaldlogger
  ARCHIVE DESTINATION lib/mesos
  LIBRARY DESTINATION lib/mesos
)

target_include_directories(journaldlogger PUBLIC
  ${DEPENDENCIES_INC}
)

target_link_libraries(journaldlogger PUBLIC
  ${DEPENDENCIES_LIB}

  $<$<PLATFORM_ID:Linux>:systemd>
)

add_executable(mesos-journald-logger journald.cpp)
install(TARGETS mesos-journald-logger RUNTIME DESTINATION bin)

target_link_libraries(mesos-journald-logger PRIVATE
  journaldlogger
)

configure_file(modules.json.cmake-in modules.json)


add_library(removedockercfg SHARED remover.cpp)
install(TARGETS removedockercfg LIBRARY DESTINATION "lib/mesos")

target_include_directories(removedockercfg PRIVATE
  ${DEPENDENCIES_INC}
)

target_link_libraries(removedockercfg PUBLIC
  ${DEPENDENCIES_LIB}
)

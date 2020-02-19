
if (NOT WIN32)
  cmake_minimum_required(VERSION 3.7.0)
else ()
  cmake_minimum_required(VERSION 3.8.0)
endif ()

project(dcos-mesos-modules)

get_filename_component(MESOS_ROOT ../mesos ABSOLUTE)

message("MESOS_ROOT is ${MESOS_ROOT}")

# CMAKE MODULE SETUP.
#####################
# Paths that are searched when `include(...)` is called.
list(APPEND CMAKE_MODULE_PATH ${MESOS_ROOT}/cmake)
list(APPEND CMAKE_MODULE_PATH ${MESOS_ROOT}/3rdparty/cmake)
list(APPEND CMAKE_MODULE_PATH ${MESOS_ROOT}/src/cmake)
list(APPEND CMAKE_MODULE_PATH ${CMAKE_SOURCE_DIR}/cmake)

# Macros.
include(Versions)
include(Dependencies)
#include(cotire)

# Set the default standard to C++11 for all targets.
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Do not use, for example, `-std=gnu++11`.
set(CMAKE_CXX_EXTENSIONS OFF)

if (CMAKE_CXX_COMPILER_ID MATCHES MSVC)
  # TODO(andschwa): Switch to `/W4` and re-enable possible-loss-of-data warnings.
  #
  # The last two warnings are disabled (well, put into `/W4`) because
  # there is no easy equivalent to enable them for GCC/Clang without
  # also fixing all the warnings from `-Wconversion`.
  add_compile_options(
    # Like `-Wall`; `/W4` is more like `-Wall -Wextra`.
    /W3
    # Disable permissiveness.
    /permissive-
    # C4244 is a possible loss of data warning for integer conversions.
    /w44244
    # C4267 is a possible loss of data warning when converting from `size_t`.
    /w44267)
endif ()

# WINDOWS CONFIGURATION.
########################
if (WIN32)
  # Fix Warning C4530: C++ exception handler used, but unwind semantics are not
  # enabled.
  add_compile_options(/EHsc)

  set(CMAKE_WINDOWS_EXPORT_ALL_SYMBOLS ON)

  # Enable multi-threaded compilation for `cl.exe`.
  add_compile_options(/MP)

  # Force use of Unicode C and C++ Windows APIs.
  add_definitions(-DUNICODE -D_UNICODE -DNOGDI -DNOMINMAX -DGOOGLE_GLOG_DLL_DECL=)

  # Convenience flags to simplify Windows support in C++ source; used to
  # `#ifdef` out some platform-specific parts of Mesos.  We choose to define
  # a new flag rather than using an existing flag (`_WIN32`) because we want
  # to give the build system fine-grained control over what code is #ifdef'd
  # out in the future.  Using only flags defined by our build system to control
  # this logic is the clearest and most stable way of accomplishing this.
  add_definitions(-D__WINDOWS__)
endif ()


add_subdirectory(dockercfg)
add_subdirectory(logsink)

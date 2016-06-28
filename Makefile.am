AUTOMAKE_OPTIONS = subdir-objects
ACLOCAL_AMFLAGS = -I m4

# We want to install modules in mesos directory.
pkglibdir = $(libdir)/mesos

# Initialize variables here so we can use += operator everywhere else.
pkglib_LTLIBRARIES =
bin_PROGRAMS =
BUILT_SOURCES =
CLEANFILES =

# Add compiler and linker flags for pthreads.
AM_CXXFLAGS = $(PTHREAD_CFLAGS)
AM_LIBS = $(PTHREAD_LIBS)

# Add top-level include dir to our include path.
MESOS_CPPFLAGS += -I$(top_srcdir)/include -Iinclude

# Enable the INT64 support for PicoJson.
# NOTE: PicoJson requires __STDC_FORMAT_MACROS to be defined before importing
# 'inttypes.h'.  Since other libraries may also import this header, it must
# be globally defined so that PicoJson has access to the macros, regardless
# of the order of inclusion.
MESOS_CPPFLAGS += -DPICOJSON_USE_INT64
MESOS_CPPFLAGS += -D__STDC_FORMAT_MACROS

# Setup CPPFLAGS that are used for most source files.
AM_CPPFLAGS = $(MESOS_CPPFLAGS) -Wall -Werror

PROTOC = $(PROTOC_PATH)/protoc

PROTOCFLAGS = -I$(top_srcdir)/

CXX_PROTOS =					\
  include/overlay/overlay.pb.cc			\
  include/overlay/overlay.pb.h			\
  include/overlay/internal/messages.pb.cc	\
  include/overlay/internal/messages.pb.h

# Targets for generating C++ protocol buffer code.
include/overlay/%.pb.cc include/overlay/%.pb.h: $(top_srcdir)/include/overlay/%.proto
	$(PROTOC) $(PROTOCFLAGS) --cpp_out=. $^

BUILT_SOURCES += $(CXX_PROTOS)

CLEANFILES += $(CXX_PROTOS)
 
# Library master/agent anonymous modules.
pkglib_LTLIBRARIES += libmesos_network_overlay.la
libmesos_network_overlay_la_SOURCES =		\
  src/agent/manager.cpp				\
  src/master/manager.cpp			\
  ${CXX_PROTOS}

libmesos_network_overlay_la_LDFLAGS =		\
  -release $(PACKAGE_VERSION)			\
  -shared $(MESOS_LDFLAGS)

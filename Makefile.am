AUTOMAKE_OPTIONS = subdir-objects
ACLOCAL_AMFLAGS = -I m4

if SOURCE_PROVIDED
# Render path to Mesos bundled gmock.
BUNDLE_SUBDIR = 3rdparty

include $(MESOS_ROOT)/$(BUNDLE_SUBDIR)/versions.am
GMOCK = $(MESOS_BUILD_DIR)/$(BUNDLE_SUBDIR)/gmock-$(GMOCK_VERSION)
GTEST = $(GMOCK)/gtest
ZOOKEEPER = $(MESOS_BUILD_DIR)/$(BUNDLE_SUBDIR)/zookeeper-$(ZOOKEEPER_VERSION)/src/c
endif

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
MESOS_CPPFLAGS += -I$(top_srcdir)/include -I$(top_srcdir)/src -Iinclude -I$(top_srcdir)

# Setup CPPFLAGS that are used for most source files.
AM_CPPFLAGS = $(MESOS_CPPFLAGS) -Wall -Werror

# Setup the protobuf compiler.
PROTOC = @PROTOCOMPILER@
PROTOCFLAGS = -I$(srcdir)
if SOURCE_PROVIDED
PROTOCFLAGS += -I$(MESOS_ROOT)/include
endif

###############################################################################
# Dockercfg Remover Hook.
###############################################################################

# Library with the .dockercfg remover hook.
pkglib_LTLIBRARIES += libremovedockercfg.la
libremovedockercfg_la_SOURCES =				\
  dockercfg/remover.cpp

libremovedockercfg_la_LDFLAGS =				\
  -release $(PACKAGE_VERSION)				\
  -shared $(MESOS_LDFLAGS)

###############################################################################
# Journald ContainerLogger Module.
###############################################################################

# Library with the ContainerLogger module.
pkglib_LTLIBRARIES += libjournaldlogger.la
libjournaldlogger_la_SOURCES =				\
  journald/journald.hpp					\
  journald/lib_journald.hpp				\
  journald/lib_journald.cpp

libjournaldlogger_la_LDFLAGS =				\
  -release $(PACKAGE_VERSION)				\
  -shared $(MESOS_LDFLAGS)

# Companion binary for the ContainerLogger module.
bin_PROGRAMS += mesos-journald-logger
mesos_journald_logger_SOURCES =				\
  journald/journald.hpp					\
  journald/journald.cpp

SYSTEMD_JOURNALD = `pkg-config --cflags --libs libsystemd`

mesos_journald_logger_LDFLAGS =				\
  $(MESOS_LDFLAGS)					\
  $(SYSTEMD_JOURNALD)

###############################################################################
# Overlay Modules.
###############################################################################

OVERLAY_PROTOS =					\
  overlay/overlay.pb.cc					\
  overlay/overlay.pb.h					\
  overlay/messages.pb.cc				\
  overlay/messages.pb.h

# Targets for generating C++ protocol buffer code.
overlay/%.pb.cc overlay/%.pb.h: $(top_srcdir)/overlay/%.proto
	$(PROTOC) $(PROTOCFLAGS) --cpp_out=. $^

BUILT_SOURCES += $(OVERLAY_PROTOS)
CLEANFILES += $(OVERLAY_PROTOS)

# Library with both master and agent anonymous modules.
pkglib_LTLIBRARIES += libmesos_network_overlay.la
libmesos_network_overlay_la_SOURCES =			\
  overlay/agent.cpp					\
  overlay/master.cpp					\
  ${OVERLAY_PROTOS}

libmesos_network_overlay_la_LDFLAGS =			\
  -release $(PACKAGE_VERSION)				\
  -shared $(MESOS_LDFLAGS)

###############################################################################
# Unit tests.
###############################################################################

if SOURCE_PROVIDED
check_PROGRAMS=

pkglib_LTLIBRARIES += libmesos_tests.la
libmesos_tests_la_SOURCES =				\
  tests/main.cpp					\
  tests/cluster.cpp					\
  tests/containerizer.cpp				\
  tests/environment.cpp					\
  tests/flags.cpp					\
  tests/mesos.cpp					\
  tests/module.cpp					\
  tests/mock_registrar.cpp				\
  tests/script.cpp					\
  tests/utils.cpp					\
  tests/containerizer/launcher.cpp			\
  tests/containerizer/memory_test_helper.cpp

# Additional flags needed to build the Mesos test helpers.
libmesos_tests_la_CPPFLAGS =				\
  -I$(MESOS_BUILD_DIR)/include/mesos			\
  -I$(ZOOKEEPER)/include				\
  -I$(ZOOKEEPER)/generated				\
  -isystem $(GMOCK)/include				\
  -I$(GTEST)/include					\
  -DMODULES_BUILD_DIR=\"$(abs_top_builddir)\"		\
  -DSOURCE_DIR=\"$(MESOS_ROOT)\"			\
  -DBUILD_DIR=\"$(MESOS_BUILD_DIR)\"			\
  -DPKGMODULEDIR=\"nowhere\"				\
  -DLIBDIR=\"nowhere\"					\
  -DPKGLIBEXECDIR=\"nowhere\"				\
  -DTESTLIBEXECDIR=\"nowhere\"				\
  -DSBINDIR=\"nowhere\"					\
  -DPKGDATADIR=\"nowhere\"				\
  $(AM_CPPFLAGS)

libmesos_tests_la_LDFLAGS =				\
  -release $(PACKAGE_VERSION)				\
  -shared $(MESOS_LDFLAGS)

# Test (make check) binary for the dockercfg hook.
check_PROGRAMS += test-dockercfg

test_dockercfg_SOURCES =				\
  tests/dockercfg_tests.cpp

test_dockercfg_CPPFLAGS =				\
  $(libmesos_tests_la_CPPFLAGS)

test_dockercfg_LDADD =					\
  $(MESOS_LDFLAGS)					\
  $(MESOS_BUILD_DIR)/$(BUNDLE_SUBDIR)/.libs/libgmock.la	\
  $(MESOS_BUILD_DIR)/src/.libs/libmesos.la		\
  libmesos_tests.la					\
  libremovedockercfg.la

# Test (make check) binary for the journald module.
check_PROGRAMS += test-journald

test_journald_SOURCES =					\
  tests/journald_tests.cpp

test_journald_CPPFLAGS =				\
  $(libmesos_tests_la_CPPFLAGS)

test_journald_LDADD =					\
  $(MESOS_LDFLAGS)					\
  $(MESOS_BUILD_DIR)/$(BUNDLE_SUBDIR)/.libs/libgmock.la	\
  $(MESOS_BUILD_DIR)/src/.libs/libmesos.la		\
  libmesos_tests.la					\
  libjournaldlogger.la

# Test (make check) binary for the overlay modules.
check_PROGRAMS += test-overlay

test_overlay_SOURCES =					\
  tests/overlay_tests.cpp

test_overlay_CPPFLAGS =					\
  $(libmesos_tests_la_CPPFLAGS)

test_overlay_LDADD =					\
  $(MESOS_LDFLAGS)					\
  $(MESOS_BUILD_DIR)/$(BUNDLE_SUBDIR)/.libs/libgmock.la	\
  $(MESOS_BUILD_DIR)/src/.libs/libmesos.la		\
  libmesos_tests.la					\
  libmesos_network_overlay.la
endif

check-local: $(check_PROGRAMS)
	./test-dockercfg
	./test-journald --verbose
	LIBPROCESS_IP=127.0.0.1 LIBPROCESS_PORT=5050 ./test-overlay --verbose

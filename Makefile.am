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
check_PROGRAMS =
BUILT_SOURCES =
CLEANFILES =

# Add compiler and linker flags for pthreads.
AM_CXXFLAGS = $(PTHREAD_CFLAGS)
AM_LIBS = $(PTHREAD_LIBS)

# Add top-level include dir to our include path.
MESOS_CPPFLAGS += -I$(top_srcdir)/include -I$(top_srcdir)/src -Iinclude -I$(top_srcdir)

# Setup CPPFLAGS that are used for most source files.
AM_CPPFLAGS = $(MESOS_CPPFLAGS) -Wall -Werror

# Enable the INT64 support for PicoJson.
# NOTE: PicoJson requires __STDC_FORMAT_MACROS to be defined before importing
# 'inttypes.h'.  Since other libraries may also import this header, it must
# be globally defined so that PicoJson has access to the macros, regardless
# of the order of inclusion.
MESOS_CPPFLAGS += -DPICOJSON_USE_INT64
MESOS_CPPFLAGS += -D__STDC_FORMAT_MACROS

PROTOC = $(PROTOC_PATH)/protoc

PROTOCFLAGS = -I$(top_srcdir)/

# Library containing the remove .dockerCfg modules.
pkglib_LTLIBRARIES += libremovedockercfg.la

libremovedockercfg_la_SOURCES = 					\
  src/dockercfg_remover.cpp

libremovedockercfg_la_LDFLAGS = -release $(PACKAGE_VERSION) -shared $(MESOS_LDFLAGS)

if SOURCE_PROVIDED
# Test (make check) binaries.
check_PROGRAMS += test-dockercfgremover

test_dockercfgremover_SOURCES = 					\
	src/tests/main.cpp				\
	src/tests/dockercfg_remover_tests.cpp		\
	tests/cluster.cpp 						\
	tests/containerizer.cpp						\
	tests/environment.cpp						\
	tests/flags.cpp							\
 	tests/mesos.cpp 						\
 	tests/module.cpp						\
 	tests/script.cpp						\
 	tests/utils.cpp							\
 	tests/containerizer/launcher.cpp				\
 	tests/containerizer/memory_test_helper.cpp
 
 test_dockercfgremover_CPPFLAGS =					\
	-isystem $(GMOCK)/include					\
 	-I$(GTEST)/include 						\
	-I$(MESOS_BUILD_DIR)/include/mesos				\
	-I$(ZOOKEEPER)/include						\
	-I$(ZOOKEEPER)/generated					\
	-DMODULES_BUILD_DIR=\"$(abs_top_builddir)\"			\
	$(AM_CPPFLAGS)

# Additional flags needed to build the Mesos test helpers.
test_dockercfgremover_CPPFLAGS +=					\
	-DSOURCE_DIR=\"$(MESOS_ROOT)\"					\
	-DBUILD_DIR=\"$(MESOS_BUILD_DIR)\"				\
	-DPKGMODULEDIR=\"nowhere\"					\
	-DLIBDIR=\"nowhere\"						\
	-DPKGLIBEXECDIR=\"nowhere\"					\
	-DTESTLIBEXECDIR=\"nowhere\"					\
	-DSBINDIR=\"nowhere\"						\
	-DPKGDATADIR=\"nowhere\"

test_dockercfgremover_LDADD = 						\
	$(MESOS_LDFLAGS)						\
	$(MESOS_BUILD_DIR)/$(BUNDLE_SUBDIR)/.libs/libgmock.la 		\
	$(MESOS_BUILD_DIR)/src/.libs/libmesos.la libdcos_security.la
endif

check-local: $(check_PROGRAMS)
	./test-removedokcercfg

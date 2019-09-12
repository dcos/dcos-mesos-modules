[![Build Status](https://jenkins.mesosphere.com/service/jenkins/buildStatus/icon?job=mesos/modules/DCOS_Mesos_Modules-nightly)](https://jenkins.mesosphere.com/service/jenkins/job/mesos/job/modules/job/DCOS_Mesos_Modules-nightly/)

# DC/OS Mesos Modules

Mesos modules provide a way to easily extend inner workings of Mesos by creating
and using shared libraries that are loaded on demand. Modules can be used to
customize Mesos without having to recompiling/relinking for each specific use
case. Modules can isolate external dependencies into separate libraries, thus
resulting into a smaller Mesos core. Modules also make it easy to experiment
with new features. For example, imagine loadable allocators that contain a VM
(Lua, Python, …) which makes it possible to try out new allocator algorithms
written in scripting languages without forcing those dependencies into the
project. Finally, modules provide an easy way for third parties to easily extend
Mesos without having to know all the internal details.

For more details, please see
[Mesos Modules](http://mesos.apache.org/documentation/latest/modules/).

## Prerequisites

### Mesos

To build Mesos modules, you first need to build Mesos.
On a fresh clone of the Mesos repository:
```
cd <mesos-source>
./bootstrap
mkdir build
cd build
../configure --enable-libevent --enable-ssl --enable-install-module-dependencies --enable-launcher-sealing --disable-libtool-wrappers
make
```

In order to compile the Mesos modules tests, `--enable-tests-install`
should be added to the `configure`'s command-line arguments too.

### Systemd journald headers

Some of these modules also require systemd development headers and libraries.
For example on CentOS 7:
```
sudo yum install systemd-devel
```

### Vargrant development environment

You can also use [vagrant](https://www.vagrantup.com/) for your development
environment. The repo contains Vagrantfile that will give you vagrant image with
all development dependencies. The parent directory would be mounted as `/work`
inside that image.

## Build Instructions

To start, generate the required build files and create a build directory:
```
./bootstrap
mkdir build
cd build
```

If building against an installation of Mesos:
```
../configure --with-mesos=/path/to/mesos/installation
```

If building against the Mesos source directory:
```
../configure --with-mesos-root=/path/to/mesos --with-mesos-build-dir=/path/to/mesos/build
```

Finally:
```
make
sudo make check
```

If `make` fails complaining that there are some protobuf options that are
unknown to `protoc`, it is likely that `configure` used `protoc` installed
system-wide. In order to make it use the one from the Mesos build, please
run:

```
../configure --with-mesos=/path/to/mesos/installation \
             --with-protobuf=/path/to/mesos/installation/lib/mesos/3rdparty
```

And then execute `make` again.

## Using Modules

See the `README.md` in each module folder for instructions.

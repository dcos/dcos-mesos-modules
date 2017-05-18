[![Build Status](https://jenkins.mesosphere.com/service/jenkins/buildStatus/icon?job=dcos-mesos-modules)](https://jenkins.mesosphere.com/service/jenkins/job/dcos-mesos-modules)

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
../configure --enable-libevent --enable-ssl --enable-install-module-dependencies
make
```

### Google-protobuf

Building these modules requires an installation of `google-protobuf`.
One way is to do the following:
```
    cd <mesos-source>/build/3rdparty/protobuf-2.6.1
    make distclean
    ./configure --prefix=$HOME/usr
    make
    make install
```

### Systemd journald headers

Some of these modules also require systemd development headers and libraries.
For example on CentOS 7:
```
sudo yum install systemd-devel
```

## Build Instructions

To start, generate the required build files and create a build directory:
```
./bootstrap
mkdir build
cd build
```

> **NOTE**: Depending on where you installed `google-protobuf`, you may need
> to add `--with-protobuf=/path/to/protobuf/installation`.

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
make check
```

## Using Modules

See the `README.md` in each module folder for instructions.

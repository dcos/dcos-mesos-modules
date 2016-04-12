# Building Overlay Module(s)

For more details on Mesos modules, please see
[Mesos Modules](http://mesos.apache.org/documentation/latest/modules/).


## Prerequisites

Building Mesos modules requires system-wide installation of google-protobuf.

One easy way is to do the following:

```
    cd <mesos-root>/build/3rdparty/libprocess/3rdparty/protobuf
    ./configure --prefix=$HOME/usr
    make && make install
```

## Building modules
```
    ./bootstrap
    mkdir build && cd build
    ../configure --with-mesos=/path/to/mesos/installation --with-protobuf=$HOME/usr
    make
```

At this point, the Module libraries are ready in `/build/.libs`.

## Using modules

Here is an example agent launch:

```
    ./build/bin/mesos-slave.sh --master=localhost:5050  \
        --modules=/path/to/mesos-overlay/build/agent_modules.json
```

You can modify it for master as needed.

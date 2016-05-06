# Building Overlay Module(s)

For more details on Mesos modules, please see
[Mesos Modules](http://mesos.apache.org/documentation/latest/modules/).

#Dependencies
* libgflag-dev (2.0.1): commandline flags module for C++.


## Prerequisites

Building Mesos modules requires system-wide installation of google-protobuf.

One easy way is to do the following:

```
    cd <mesos-root>/build/3rdparty/libprocess/3rdparty/protobuf
    ./configure --prefix=$HOME/usr
    make clean && make && make install
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

Here is an example master launch:
```
  ./build/bin/mesos-master.sh --ip=localhost:5050 \
    --modules=/path/to/mesos-overlay/build/master_modules.json
```

## Configuring the Agent module
The Agent module needs to be informed about the Master, and the
directory where it will store the CNI configuration files it
generates. The Agent can be configured with this information using
parameters in the JSON config. Here is an example JSON configuration
for the Agent module:
```
{
  "libraries":
    [
    {
      "file":
        "/home/vagrant/mesosphere/mesos-overlay/build/.libs/libmesos_network_overlay.so",
        "modules":
          [
          {
            "name":
              "com_mesosphere_mesos_AgentOverlayHelper",
            "parameters" : [
            {
              "key" : "master",
              "value" : "10.0.2.15:5050"
            },
            {
              "key" : "cni_dir",
              "value" : "/var/lib/mesos/cni"
            }
            ]
          }
      ]
    }
  ]
}
```
The parameters that the Agent module expects in its JSON configuration
are as follows:
* `master`: The IP address and port used to register with the Master overlay module.
* `cni_dir`: The directory where the CNI configuration for each overlay network will be stored.

## Configuring the Master module
The Master module needs to be informed about the Overlay networks that
need to be configured in the cluster, the address space from which to
allocate the VTEP IPs, and the OUI used to allocate unique VTEP MAC
addresses. The Master module can be configured with this information
using parameters in the JSON config. Here is an example JSON
configuration for the Master module:
```
{
  "libraries":
    [
    {
      "file":
        "/home/vagrant/mesosphere/mesos-overlay/build/.libs/libmesos_network_overlay.so",
        "modules":
          [
          {
            "name":
              "com_mesosphere_mesos_MasterOverlayHelper",
            "parameters" : [
            {
              "key": "overlays",
              "value" : "/var/lib/mesos/overlay-config.json"
            },
            {
              "key" : "vtep_subnet",
              "value": "44.128.0.0/16"
            },
            {
              "key" : "vtep_mac_oui",
              "value" : "70:B3:D5:00:00:00"
            }
            ]
          }
      ]
    }
  ]
}
```

The parameters that the Master module expects in its JSON
configuration are as follows:
* `overlays`: A path to  JSON configuration file that contains the
JSON configuration for each overlay network that needs to exist on
the cluster. You can read about the formation of the overlay 
configuration file in the "Configuring Overlays" section.
* `vtep_subnet`: The address space from which VTEP IP will be
allocated.
* `vtep_mac_oui`: The first 24 bits of the VTEP MAC.


## Configuring Overlays
The overlay configuration is specified through a JSON configuration.
The location of this JSON configuration is specified using the
parameter `overlays` in the Master JSON configuration. Here is an
example JSON configuration to specify overlay networks:

```
[
{
  "name" : "vxlan-1",
    "subnet" : "192.168.0.0/17",
    "prefix" : 24
},
{
  "name" : "vxlan-2",
  "subnet" : "192.168.128.0/17",
  "prefix" : 24
}
]
```

There can be multiple overlays specified in the JSON configuration.
Each overlay instance needs the following parameters specified:
* `name` : A canonical name for the overlay network. This is the
"name" that will be used by frameworks to specify the overlay
network on which they want to launch the container.
* `subnet`: The address space that will be used to allocate IP to
containers launched on this overlay network.
* `prefix`: The address space of the overlay network is spliced into
smaller subnets, that are allocated to each Agent. Splicing the larger
subnet into smaller ones removes the need to have a global IPAM. The
"prefix" specifies the subnet mask used to allocate subnets (from the
overlay address space) to each Agent.

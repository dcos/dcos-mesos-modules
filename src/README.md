#Mesos DCOS anonymous module POC
This module is loaded within the OS process context of a host (master or slave). They are installing a route that allows receiving the results of a shell command [```ip addr```] executed once an HTTP get is received.

##Build
See [Building the Modules](https://github.com/mesosphere/mesos-modules-private).

##Setup
To use these modules on a host, create a json file (e.g. ```dcosanon.json```) with the following contents:

	{
	  "libraries": [
	    {
	      "file": "/path/to/libdcosanonpoc.so",
	      "modules": [
	        {
	          "name": "com_mesosphere_mesos_DCOSAnonymous"
	        }
	      ]
	    }
	  ]
	}

 Now launch the host with the following flags:

	--modules=file:///path/to/dcosanon.json

For validating your setup, check the startup log of the host, it should contain something like

	I0215 01:21:46.947537 277327872 dcos_anon_poc.cpp:164] Adding route for 'ip_addr/ip'

##Use
The hosting process is called __host_info__, it installs a route called __ip__. The default shell command ```ip addr``` may be overriden by supplying the __cmd__ query parameter.

__NOTE__: This POC is terribly unsafe and for evaluation only!

You should be able to run any shell command from within the host.

 ####Example running `pwd` on a slave

	$ curl -i http://192.168.0.100:5051/host_info/ip?cmd=pwd
	HTTP/1.1 200 OK
	Date: Sat, 14 Feb 2015 23:33:38 GMT
	Content-Length: 44

	/Users/till/Development/mesos-private/build

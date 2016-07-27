#Remove .dockercfg Module

Module implementing slavePostFetchHook to remove .dockercfg files from the sandbox.
The problem is that .dockercfg files might contain sensitive information and hence they should not
be present in the users sandbox. Still the .dockercfg might be required for pulling the docker image from private registries. This hook, which is executed after the docker pull removes .dockercfg files from sandbox.

##Setup

Building the module will generate a modules.json file in the removeDockerCfg subfolder in the build directory which should be used to specify the module to be used via the `--modules` agent flag. Additionally the user needs to specify which hooks should be specified by the `--hooks` agent flag. See the example below:


```
./bin/mesos-agent.sh
--master=master_ip:port \
--modules=file://path/to/remove_docker_cfg/modules.json \
--hooks=com_mesosphere_dcos_RemoverHook
```

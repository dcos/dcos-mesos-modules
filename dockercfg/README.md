# Remove .dockercfg Module

This module implements the `slavePostFetchHook` to remove `.dockercfg`
files from the sandbox after the all fetching has completed, but before
any task has been launched.  A `.dockercfg` may be required for pulling
a docker image from private registries.  The problem is that `.dockercfg`
files might contain sensitive information and hence they should not be
present in the task's sandbox.

## Setup

Building the module will generate a `modules.json` file in the
`build/dockercfg` folder, which can be given to the `--modules` agent flag.
Additionally the user needs to specify which hooks should be specified
by the `--hooks` agent flag. See the example below:

```
./bin/mesos-agent.sh \
  --master=master_ip:port \
  --modules=file://path/to/remove_docker_cfg/modules.json \
  --hooks=com_mesosphere_dcos_RemoverHook \
  ...
```

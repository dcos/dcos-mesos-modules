# Journald Container Logger module

The `JournaldContainerLogger` module takes all executor and tasks logs
and pipes them to a configurable destination.  When the module was first 
written, it piped logs to the local systemd journald and so bears the name.
However, in practice, using journald is *not recommended* due to scalability
issues with the journald process.

This Container Logger module supports three output modes and certain combinations
of these modes:
* `logrotate`
* `journald`
* `fluentbit`
* `journald+logrotate`
* `fluentbit+logrotate`

Also see the [DC/OS documentation for task logging](https://docs.mesosphere.com/1.12/monitoring/logging/configure-task-logs/).

## Logrotate mode

This module is based on the `LogrotateContainerLogger` found in the Mesos
source code.  Features implemented in that Container Logger will be found
in this one as well, with some minor differences:
* A `destination_type` parameter containing `"logrotate"` is necessary to
  manage logs via logrotate.
* The parameters for `max_stdout_size` and `max_stderr_size` are changed to
  `logrotate_max_stdout_size` and `logrotate_max_stderr_size` to separate
  them from other logging modes.

See the [Mesos logging documentation](http://mesos.apache.org/documentation/latest/logging/)
for more information.

## Journald mode

When outputing to journald, each log line is tagged with
some information to make filtering and querying feasible:

* `FRAMEWORK_ID`, `AGENT_ID`, `EXECUTOR_ID`, and `CONTAINER_ID`.
  * NOTE: For nested containers, `CONTAINER_ID` is comprised of
    container IDs concatenated together with a `.` separator.
    i.e. `<parent>.<child>.<grandchild>`
* Any labels found inside the `ExecutorInfo`.
* `STREAM` == `STDOUT` or `STDERR`.
* Any values in the `extra_labels` parameter passed to the module.

For example, using `mesos-execute`:

```
mesos-execute --master=<some-master> \
  --name="Snore"                     \
  --command="while true; do echo 'ZZZzzz...'; sleep 0.2; done"
```

This will output:
```
...
Subscribed with ID <FRAMEWORK_ID>
Submitted task 'Snore' to agent '<AGENT_ID>'
Received status update TASK_RUNNING for task 'Snore'
  source: SOURCE_EXECUTOR
...
```

You can then view the logs like:
```
journalctl FRAMEWORK_ID=<FRAMEWORK_ID> -f
```

## Fluentbit mode

When outputing to Fluent Bit, the module opens a TCP connection
to the provided `fluentbit_ip` and `fluentbit_port`.

> **NOTE**: These parameters are required even if the `fluentbit`
> output mode is not used, because individual tasks may choose to
> use this mode via the `CONTAINER_LOGGER_DESTINATION_TYPE`
> environment variable.

Each line of logs is then wrapped in a JSON object (`{"line":"<log line>"}`)
and written over the TCP connection.  This output mode does not attempt
to retry if errors are encountered and will drop lines instead.

> **NOTE**: The destination of the TCP connection is not specific to 
> Fluent Bit, as it is a plain TCP connection.  However, DC/OS uses
> Fluent Bit, hence the mode's name.

## Using the ContainerLogger module

Launch the agent with the module loaded and enabled:
```
mesos-agent.sh --master=<some-master>                      \
  --modules=file:///path/to/journald/modules.json          \
  --container_logger="com_mesosphere_mesos_JournaldLogger" \
  --work_dir=/some/other/work/dir
```

> **NOTE**: If you do not install the mesos source (i.e. `make install`)
> You may need to run `sudo ldconfig /path/to/mesos/build/src/.libs`.

Tasks can customize their logging destination by including environment
variables prefixed with the appropriate `environment_variable_prefix`.
See the [`LoggerFlags` and `environment_variable_prefix` help text](https://github.com/dcos/dcos-mesos-modules/blob/master/journald/lib_journald.hpp).

## Unit tests

> **NOTE**: Due to the hard dependency on systemd, the unit test(s) for
> this module must be run as ROOT.

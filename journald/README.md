# Journald Container Logger module

The `JournaldContainerLogger` module takes all executor and tasks logs
and pipes them to the local systemd journald.  Each log line is tagged
some information to make filtering and querying feasible:

* `FrameworkID`, `AgentID`, `ExecutorID`, and `ContainerID`.
* Any labels found inside the `ExecutorInfo`.
* `STDOUT` or `STDERR`.

## Using the ContainerLogger module

Launch the agent with the module loaded and enabled:
```
mesos-agent.sh --master=<some-master>                      \
  --modules=file:///path/to/journald/modules.json          \
  --container_logger="com_mesosphere_mesos_JournaldLogger" \
  --work_dir=/some/other/work/dir
```

## Run things that output

You can then run any task and view the output via journald.
For example, using the `mesos-execute`:

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

## Unit tests

> **NOTE**: Due to the hard dependency on systemd, the unit test(s) for
> this module must be run as ROOT.

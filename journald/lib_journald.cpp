#include <array>
#include <map>
#include <string>
#include <vector>

#include <mesos/mesos.hpp>
#include <mesos/type_utils.hpp>

#include <mesos/module/container_logger.hpp>

#include <mesos/slave/container_logger.hpp>
#include <mesos/slave/containerizer.hpp>

#include <process/dispatch.hpp>
#include <process/future.hpp>
#include <process/process.hpp>
#include <process/subprocess.hpp>

#include <stout/bytes.hpp>
#include <stout/jsonify.hpp>
#include <stout/try.hpp>
#include <stout/nothing.hpp>
#include <stout/path.hpp>
#include <stout/stringify.hpp>
#include <stout/strings.hpp>

#include <stout/os/environment.hpp>
#include <stout/os/fcntl.hpp>
#include <stout/os/killtree.hpp>

#include "journald.hpp"
#include "lib_journald.hpp"


using namespace mesos;
using namespace process;

using std::array;
using std::string;
using std::vector;

using mesos::slave::ContainerConfig;
using mesos::slave::ContainerLogger;
using mesos::slave::ContainerIO;

// Forward declare some functions located in `src/linux/systemd.cpp`.
// This is not exposed in the Mesos public headers, but is necessary to
// keep the ContainerLogger's companion binaries alive if the agent dies.
namespace systemd {
namespace mesos {

Try<Nothing> extendLifetime(pid_t child);

} // namespace mesos {

bool enabled();

} // namespace systemd {

namespace mesos {
namespace journald {

// The number of characters in `{"key":"","value":""},` which is the
// minimum size of any label when serialized into JSON.
const Bytes LABEL_PADDING_SIZE =
  Bytes(string("{\"key\":\"\",\"value\":\"\"},").size());

class JournaldContainerLoggerProcess :
  public Process<JournaldContainerLoggerProcess>
{
public:
  JournaldContainerLoggerProcess(const Flags& _flags) : flags(_flags) {}

  // Spawns two subprocesses that read from their stdin and write to
  // journald along with labels to disambiguate the logs from other containers.
  Future<ContainerIO> prepare(
      const ContainerID& containerId,
      const ContainerConfig& containerConfig)
  {
    // DEBUG containers are associated with `LAUNCH_NESTED_CONTAINER_SESSION`
    // calls and generally do not need extensive logging because their output
    // is either temporary (i.e. health checks) or sent to the session starter
    // (i.e. `dcos task exec`). Because forking logger subprocesses can be
    // expensive, we simply log to sandbox for these container types.
    if (containerConfig.has_container_class() &&
        containerConfig.container_class() ==
          mesos::slave::ContainerClass::DEBUG) {
      ContainerIO io;

      io.out = ContainerIO::IO::PATH(
          path::join(containerConfig.directory(), "stdout"));

      io.err = ContainerIO::IO::PATH(
          path::join(containerConfig.directory(), "stderr"));

      return io;
    }

    // Prepare the environment for the container logger subprocess.
    // We inherit agent environment variables except for those
    // LIBPROCESS or MESOS prefixed environment variables. See MESOS-6747.
    std::map<std::string, std::string> environment;

    foreachpair (const std::string& key, const std::string& value,
                 os::environment()) {
      if (!strings::startsWith(key, "LIBPROCESS_") &&
          !strings::startsWith(key, "MESOS_")) {
        environment.emplace(key, value);
      }
    }

    // Make sure the libprocess of the subprocess can properly
    // initialize and find the IP. Since we don't need to use the TCP
    // socket for communication, it's OK to use a local address.
    environment.emplace("LIBPROCESS_IP", "127.0.0.1");

    // Use the number of worker threads for libprocess that was passed
    // in through the flags.
    CHECK_GT(flags.libprocess_num_worker_threads, 0u);
    environment["LIBPROCESS_NUM_WORKER_THREADS"] =
      stringify(flags.libprocess_num_worker_threads);

    // Copy the global logger flags.
    // These will act as the defaults in case the container environment
    // overrides a subset of them.
    LoggerFlags overriddenFlags;
    overriddenFlags.destination_type = flags.destination_type;
    overriddenFlags.logrotate_max_stdout_size = flags.logrotate_max_stdout_size;
    overriddenFlags.logrotate_max_stderr_size = flags.logrotate_max_stderr_size;

    // TODO(jieyu): Consider merge labels with container specific
    // extra labels from the environment, instead of overwriting.
    overriddenFlags.extra_labels = flags.extra_labels;

    // Check for overrides of the logger settings in the
    // containers environment variables.
    if (containerConfig.command_info().has_environment()) {
      // Search the environment for prefixed environment variables.
      // We un-prefix those variables before parsing the flag values.
      std::map<std::string, std::string> containerEnvironment;
      foreach (const Environment::Variable variable,
               containerConfig.command_info().environment().variables()) {
        if (strings::startsWith(
              variable.name(), flags.environment_variable_prefix)) {
          std::string unprefixed = strings::lower(strings::remove(
              variable.name(),
              flags.environment_variable_prefix,
              strings::PREFIX));
          containerEnvironment[unprefixed] = variable.value();
        }
      }

      // We will error out if there are unknown flags with the same prefix.
      Try<flags::Warnings> load = overriddenFlags.load(containerEnvironment);

      if (load.isError()) {
        return Failure(
            "Failed to load container logger settings: " + load.error());
      }

      // Log any flag warnings.
      foreach (const flags::Warning& warning, load->warnings) {
        LOG(WARNING) << warning.message;
      }
    }

    // TODO(josephw): Custom options would allow tasks to execute arbitrary
    // scripts in logrotate's `postrotate` clause or add rotation of arbitrary
    // files.  This is disabled in favor of more targeted options in future,
    // such as ints for the number of files to keep, or bools for compression.
    // See: https://issues.apache.org/jira/browse/MESOS-9564
    // and https://jira.mesosphere.com/browse/DCOS-47733.
    overriddenFlags.logrotate_stdout_options = flags.logrotate_stdout_options;
    overriddenFlags.logrotate_stderr_options = flags.logrotate_stderr_options;

    Option<ExecutorInfo> executorInfo;
    if (containerConfig.has_executor_info()) {
      executorInfo = containerConfig.executor_info();
    }

    // Pass in the FrameworkID, ExecutorID, and ContainerID as labels.
    // And include all labels inside the `ExecutorInfo`.
    Label label;
    Labels labels;

    Bytes totalSize;

    if (executorInfo.isSome() && executorInfo->has_labels()) {
      foreach (const Label executorLabel, executorInfo->labels().labels()) {
        totalSize += LABEL_PADDING_SIZE +
          executorLabel.key().size() +
          executorLabel.value().size();

        // Stop copying arbitrary labels once we hit a size limit.
        if (totalSize > flags.max_label_payload_size) {
          break;
        }

        labels.add_labels()->CopyFrom(executorLabel);
      }
    }

    foreachpair (const string& key,
                 const JSON::Value& value,
                 overriddenFlags.extra_labels.values) {
      // Skip invalid labels.
      if (!value.is<JSON::String>()) {
        continue;
      }

      string _value = value.as<JSON::String>().value;

      totalSize += LABEL_PADDING_SIZE + key.size() + _value.size();

      // Stop copying arbitrary labels once we hit a size limit.
      if (totalSize > flags.max_label_payload_size) {
        break;
      }

      label.set_key(key);
      label.set_value(_value);
      labels.add_labels()->CopyFrom(label);
    }

    // NOTE: It is possible for the ExecutorInfo object to be blank in
    // the following cases:
    //   * Prior to Mesos 1.5.0, the ContainerConfig object is not
    //     checkpointed and hence, any containers recovered from earlier
    //     versions will be missing some metadata.
    //     See: https://issues.apache.org/jira/browse/MESOS-6894
    //   * Standalone containers naturally do not have this field set.
    if (executorInfo.isSome() && executorInfo->has_framework_id()) {
      label.set_key("FRAMEWORK_ID");
      label.set_value(executorInfo->framework_id().value());
      labels.add_labels()->CopyFrom(label);
    }

    if (executorInfo.isSome()) {
      label.set_key("EXECUTOR_ID");
      label.set_value(executorInfo->executor_id().value());
      labels.add_labels()->CopyFrom(label);
    }

    // Derive the AgentID from the sandbox directory, which always
    // occurs after the `slaves` directory.
    // See: src/slave/paths.hpp in the Mesos codebase for more info.
    vector<string> sandboxTokens =
      strings::tokenize(containerConfig.directory(), "/");

    Option<string> agentId = None();
    for (int i = sandboxTokens.size() - 2; i >= 0; i -= 2) {
      if (sandboxTokens[i] == "slaves") {
        agentId = sandboxTokens[i + 1];
      }
    }

    // NOTE: AgentID is generally unknown/irrelevant to the containerizer.
    // However, because we expect some degree of log aggregation,
    // we derive the AgentID based on the sandbox directory.
    if (agentId.isSome()) {
      label.set_key("AGENT_ID");
      label.set_value(agentId.get());
      labels.add_labels()->CopyFrom(label);
    }

    label.set_key("CONTAINER_ID");
    label.set_value(stringify(containerId));
    labels.add_labels()->CopyFrom(label);

    // If the container is part of an executor (both nested or top
    // level containers), and the executor is named, use that name to
    // present the logs. Otherwise, default to the ExecutorID.
    // This is the value that shows up in the typical journald view.
    label.set_key("SYSLOG_IDENTIFIER");
    if (executorInfo.isSome()) {
      label.set_value(
          executorInfo->has_name() ?
          executorInfo->name() :
          executorInfo->executor_id().value());
    } else {
      label.set_value("mesos-container");
    }
    labels.add_labels()->CopyFrom(label);

    // NOTE: We manually construct a pipe here instead of using
    // `Subprocess::PIPE` so that the ownership of the FDs is properly
    // represented.  The `Subprocess` spawned below owns the read-end
    // of the pipe and will be solely responsible for closing that end.
    // The ownership of the write-end will be passed to the caller
    // of this function.
    Try<array<int, 2>> pipefd = os::pipe();
    if (pipefd.isError()) {
      return Failure("Failed to create pipe: " + pipefd.error());
    }

    Subprocess::IO::InputFileDescriptors outfds;
    outfds.read = pipefd->at(0);
    outfds.write = pipefd->at(1);

    label.set_key("STREAM");
    label.set_value("STDOUT");
    labels.add_labels()->CopyFrom(label);

    mesos::journald::logger::Flags outFlags;
    outFlags.destination_type = overriddenFlags.destination_type;

    outFlags.journald_labels = stringify(JSON::protobuf(labels));

    outFlags.logrotate_max_size = overriddenFlags.logrotate_max_stdout_size;
    outFlags.logrotate_options = overriddenFlags.logrotate_stdout_options;
    outFlags.logrotate_filename =
      path::join(containerConfig.directory(), "stdout");
    outFlags.logrotate_path = flags.logrotate_path;
    outFlags.user = containerConfig.has_user()
      ? Option<string>(containerConfig.user())
      : Option<string>::none();
    outFlags.fluentbit_ip = flags.fluentbit_ip;
    outFlags.fluentbit_port = flags.fluentbit_port;

    // If we are on systemd, then extend the life of the process as we
    // do with the executor. Any grandchildren's lives will also be
    // extended.
    std::vector<Subprocess::ParentHook> parentHooks;
    if (systemd::enabled()) {
      parentHooks.emplace_back(Subprocess::ParentHook(
          &systemd::mesos::extendLifetime));
    }

    // Spawn a process to handle stdout.
    Try<Subprocess> outProcess = subprocess(
        path::join(flags.companion_dir, mesos::journald::logger::NAME),
        {mesos::journald::logger::NAME},
        Subprocess::FD(outfds.read, Subprocess::IO::OWNED),
        Subprocess::PATH("/dev/null"),
        Subprocess::FD(STDERR_FILENO),
        &outFlags,
        environment,
        None(),
        parentHooks,
        {Subprocess::ChildHook::SETSID()});

    if (outProcess.isError()) {
      os::close(outfds.write.get());
      return Failure("Failed to create logger process: " + outProcess.error());
    }

    // NOTE: We manually construct a pipe here to properly express
    // ownership of the FDs.  See the NOTE above.
    pipefd = os::pipe();
    if (pipefd.isError()) {
      os::close(outfds.write.get());
      os::killtree(outProcess->pid(), SIGKILL);
      return Failure("Failed to create pipe: " + pipefd.error());
    }

    Subprocess::IO::InputFileDescriptors errfds;
    errfds.read = pipefd->at(0);
    errfds.write = pipefd->at(1);

    labels.mutable_labels()->DeleteSubrange(labels.labels().size() - 1, 1);
    label.set_key("STREAM");
    label.set_value("STDERR");
    labels.add_labels()->CopyFrom(label);

    mesos::journald::logger::Flags errFlags;
    errFlags.destination_type = overriddenFlags.destination_type;

    errFlags.journald_labels = stringify(JSON::protobuf(labels));

    errFlags.logrotate_max_size = overriddenFlags.logrotate_max_stderr_size;
    errFlags.logrotate_options = overriddenFlags.logrotate_stderr_options;
    errFlags.logrotate_filename =
      path::join(containerConfig.directory(), "stderr");
    errFlags.logrotate_path = flags.logrotate_path;
    errFlags.user = containerConfig.has_user()
      ? Option<string>(containerConfig.user())
      : Option<string>::none();
    errFlags.fluentbit_ip = flags.fluentbit_ip;
    errFlags.fluentbit_port = flags.fluentbit_port;

    // Spawn a process to handle stderr.
    Try<Subprocess> errProcess = subprocess(
        path::join(flags.companion_dir, mesos::journald::logger::NAME),
        {mesos::journald::logger::NAME},
        Subprocess::FD(errfds.read, Subprocess::IO::OWNED),
        Subprocess::PATH("/dev/null"),
        Subprocess::FD(STDERR_FILENO),
        &errFlags,
        environment,
        None(),
        parentHooks,
        {Subprocess::ChildHook::SETSID()});

    if (errProcess.isError()) {
      os::close(outfds.write.get());
      os::close(errfds.write.get());
      os::killtree(outProcess.get().pid(), SIGKILL);
      return Failure("Failed to create logger process: " + errProcess.error());
    }

    // NOTE: The ownership of these FDs is given to the caller of this function.
    ContainerIO io;
    io.out = ContainerIO::IO::FD(outfds.write.get());
    io.err = ContainerIO::IO::FD(errfds.write.get());
    return io;
  }

protected:
  Flags flags;
};


JournaldContainerLogger::JournaldContainerLogger(const Flags& _flags)
  : flags(_flags),
    process(new JournaldContainerLoggerProcess(flags))
{
  // Spawn and pass validated parameters to the process.
  spawn(process.get());
}


JournaldContainerLogger::~JournaldContainerLogger()
{
  terminate(process.get());
  wait(process.get());
}


Try<Nothing> JournaldContainerLogger::initialize()
{
  return Nothing();
}

Future<ContainerIO> JournaldContainerLogger::prepare(
    const ContainerID& containerId,
    const ContainerConfig& containerConfig)
{
  return dispatch(
      process.get(),
      &JournaldContainerLoggerProcess::prepare,
      containerId,
      containerConfig);
}

} // namespace journald {
} // namespace mesos {


mesos::modules::Module<ContainerLogger>
com_mesosphere_mesos_JournaldLogger(
    MESOS_MODULE_API_VERSION,
    MESOS_VERSION,
    "Mesosphere",
    "help@mesosphere.io",
    "Journald Container Logger module.",
    nullptr,
    [](const Parameters& parameters) -> ContainerLogger* {
      // Convert `parameters` into a map.
      std::map<std::string, std::string> values;
      foreach (const Parameter& parameter, parameters.parameter()) {
        values[parameter.key()] = parameter.value();
      }

      // Load and validate flags from the map.
      mesos::journald::Flags flags;
      Try<flags::Warnings> load = flags.load(values);

      if (load.isError()) {
        LOG(ERROR) << "Failed to parse parameters: " << load.error();
        return nullptr;
      }

      // Log any flag warnings.
      foreach (const flags::Warning& warning, load->warnings) {
        LOG(WARNING) << warning.message;
      }

      return new mesos::journald::JournaldContainerLogger(flags);
    });

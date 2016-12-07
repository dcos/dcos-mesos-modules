#include <map>
#include <string>

#include <mesos/mesos.hpp>

#include <mesos/module/container_logger.hpp>

#include <mesos/slave/container_logger.hpp>

#include <process/dispatch.hpp>
#include <process/future.hpp>
#include <process/process.hpp>
#include <process/subprocess.hpp>

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

using mesos::slave::ContainerLogger;

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

using SubprocessInfo = ContainerLogger::SubprocessInfo;


class JournaldContainerLoggerProcess :
  public Process<JournaldContainerLoggerProcess>
{
public:
  JournaldContainerLoggerProcess(const Flags& _flags) : flags(_flags) {}

  // Spawns two subprocesses that read from their stdin and write to
  // journald along with labels to disambiguate the logs from other containers.
  Future<SubprocessInfo> prepare(
      const ExecutorInfo& executorInfo,
      const std::string& sandboxDirectory,
      const Option<std::string>& user)
  {
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

    // Pass in the FrameworkID, ExecutorID, and ContainerID as labels.
    // And include all labels inside the `ExecutorInfo`.
    Label label;
    Labels labels;
    if (executorInfo.has_labels()) {
      labels = executorInfo.labels();
    }

    // NOTE: This field is required by the master/agent, but the protobuf
    // is optional for backwards compatibility.
    CHECK(executorInfo.has_framework_id());
    label.set_key("FRAMEWORK_ID");
    label.set_value(executorInfo.framework_id().value());
    labels.add_labels()->CopyFrom(label);

    label.set_key("EXECUTOR_ID");
    label.set_value(executorInfo.executor_id().value());
    labels.add_labels()->CopyFrom(label);

    // Derive the AgentID and ContainerID from the sandbox directory.
    // See: src/slave/paths.hpp in the Mesos codebase for more info.
    //
    // * The AgentID always occurs after the `slaves` directory.
    // * The ContainerID deals with nested containers by concatenating the
    //   top-level and sub-container IDs with a `.` separator.
    vector<string> sandboxTokens = strings::tokenize(sandboxDirectory, "/");
    Option<string> agentId = None();
    Option<string> containerId = None();
    for (int i = sandboxTokens.size() - 2; i >= 0; i -= 2) {
      if (sandboxTokens[i] == "slaves") {
        agentId = sandboxTokens[i + 1];
      } else if (sandboxTokens[i] == "runs" ||
                 sandboxTokens[i] == "containers") {
        containerId = sandboxTokens[i + 1] +
          (containerId.isSome() ? "." + containerId.get(): "");
      }
    }

    // NOTE: AgentID is generally unknown/irrelevant to the containerizer.
    // However, because we expect some degree of log aggregation,
    // we derive the AgentID based on the `sandboxDirectory`.
    CHECK(agentId.isSome());
    label.set_key("AGENT_ID");
    label.set_value(agentId.get());
    labels.add_labels()->CopyFrom(label);

    // NOTE: ContainerID isn't passed into the container logger as part of
    // ExecutorInfo.  It can be retrieved from the `sandboxDirectory`.
    CHECK(containerId.isSome());
    label.set_key("CONTAINER_ID");
    label.set_value(containerId.get());
    labels.add_labels()->CopyFrom(label);

    // If the executor is named, use that name to present the logs.
    // Otherwise, default to the ExecutorID.
    // This is the value that shows up in the typical journald view.
    label.set_key("SYSLOG_IDENTIFIER");
    label.set_value(
        executorInfo.has_name() ?
        executorInfo.name() :
        executorInfo.executor_id().value());
    labels.add_labels()->CopyFrom(label);

    // NOTE: We manually construct a pipe here instead of using
    // `Subprocess::PIPE` so that the ownership of the FDs is properly
    // represented.  The `Subprocess` spawned below owns the read-end
    // of the pipe and will be solely responsible for closing that end.
    // The ownership of the write-end will be passed to the caller
    // of this function.
    int pipefd[2];
    if (::pipe(pipefd) == -1) {
      return Failure(ErrnoError("Failed to create pipe").message);
    }

    Subprocess::IO::InputFileDescriptors outfds;
    outfds.read = pipefd[0];
    outfds.write = pipefd[1];

    // NOTE: We need to `cloexec` this FD so that it will be closed when
    // the child subprocess is spawned and so that the FD will not be
    // inherited by the second child for stderr.
    Try<Nothing> cloexec = os::cloexec(outfds.write.get());
    if (cloexec.isError()) {
      os::close(outfds.read);
      os::close(outfds.write.get());
      return Failure("Failed to cloexec: " + cloexec.error());
    }

    label.set_key("STREAM");
    label.set_value("STDOUT");
    labels.add_labels()->CopyFrom(label);

    mesos::journald::logger::Flags outFlags;
    outFlags.labels = stringify(JSON::protobuf(labels));

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
    if (::pipe(pipefd) == -1) {
      os::close(outfds.write.get());
      os::killtree(outProcess.get().pid(), SIGKILL);
      return Failure(ErrnoError("Failed to create pipe").message);
    }

    Subprocess::IO::InputFileDescriptors errfds;
    errfds.read = pipefd[0];
    errfds.write = pipefd[1];

    // NOTE: We need to `cloexec` this FD so that it will be closed when
    // the child subprocess is spawned.
    cloexec = os::cloexec(errfds.write.get());
    if (cloexec.isError()) {
      os::close(outfds.write.get());
      os::close(errfds.read);
      os::close(errfds.write.get());
      os::killtree(outProcess.get().pid(), SIGKILL);
      return Failure("Failed to cloexec: " + cloexec.error());
    }

    labels.mutable_labels()->DeleteSubrange(labels.labels().size() - 1, 1);
    label.set_key("STREAM");
    label.set_value("STDERR");
    labels.add_labels()->CopyFrom(label);

    mesos::journald::logger::Flags errFlags;
    errFlags.labels = stringify(JSON::protobuf(labels));

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
    ContainerLogger::SubprocessInfo info;
    info.out = SubprocessInfo::IO::FD(outfds.write.get());
    info.err = SubprocessInfo::IO::FD(errfds.write.get());
    return info;
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

Future<SubprocessInfo> JournaldContainerLogger::prepare(
    const ExecutorInfo& executorInfo,
    const std::string& sandboxDirectory,
    const Option<std::string>& user)
{
  return dispatch(
      process.get(),
      &JournaldContainerLoggerProcess::prepare,
      executorInfo,
      sandboxDirectory,
      user);
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
    NULL,
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

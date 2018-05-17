#ifndef __JOURNALD_LIB_JOURNALD_HPP__
#define __JOURNALD_LIB_JOURNALD_HPP__

#include <stdio.h>

#include <mesos/slave/container_logger.hpp>
#include <mesos/slave/containerizer.hpp>

#include <stout/bytes.hpp>
#include <stout/flags.hpp>
#include <stout/option.hpp>

#include <stout/os/exists.hpp>

#include "journald.hpp"


namespace mesos {
namespace journald {

class JournaldContainerLoggerProcess;


// These flags are loaded twice: once when the `ContainerLogger` module
// is created and each time before launching executors. The flags loaded
// at module creation act as global default values, whereas flags loaded
// prior to executors can override the global values.
struct LoggerFlags : public virtual flags::FlagsBase
{
  LoggerFlags()
  {
    add(&LoggerFlags::destination_type,
        "destination_type",
        "Determines where logs should be piped.\n"
        "Valid destinations include: 'journald', 'logrotate',\n"
        "or 'journald+logrotate'.",
        "journald",
        [](const std::string& value) -> Option<Error> {
          if (value != "journald" &&
              value != "logrotate" &&
              value != "journald+logrotate") {
            return Error("Invalid destination type: " + value);
          }

          return None();
        });

    add(&LoggerFlags::logrotate_max_stdout_size,
        "logrotate_max_stdout_size",
        "Maximum size, in bytes, of a single stdout log file.\n"
        "Defaults to 10 MB.  Must be at least 1 (memory) page.",
        Megabytes(10),
        &LoggerFlags::validateSize);

    add(&LoggerFlags::logrotate_stdout_options,
        "logrotate_stdout_options",
        "Additional config options to pass into 'logrotate' for stdout.\n"
        "This string will be inserted into a 'logrotate' configuration file.\n"
        "i.e.\n"
        "  /path/to/stdout {\n"
        "    <logrotate_stdout_options>\n"
        "    size <logrotate_max_stdout_size>\n"
        "  }\n"
        "NOTE: The 'size' option will be overridden by this module.");

    add(&LoggerFlags::logrotate_max_stderr_size,
        "logrotate_max_stderr_size",
        "Maximum size, in bytes, of a single stderr log file.\n"
        "Defaults to 10 MB.  Must be at least 1 (memory) page.",
        Megabytes(10),
        &LoggerFlags::validateSize);

    add(&LoggerFlags::logrotate_stderr_options,
        "logrotate_stderr_options",
        "Additional config options to pass into 'logrotate' for stderr.\n"
        "This string will be inserted into a 'logrotate' configuration file.\n"
        "i.e.\n"
        "  /path/to/stderr {\n"
        "    <logrotate_stderr_options>\n"
        "    size <logrotate_max_stderr_size>\n"
        "  }\n"
        "NOTE: The 'size' option will be overridden by this module.");
  }

  static Option<Error> validateSize(const Bytes& value)
  {
    if (value.bytes() < os::pagesize()) {
      return Error(
          "Expected --max_stdout_size and --max_stderr_size of "
          "at least " + stringify(os::pagesize()) + " bytes");
    }

    return None();
  }

  std::string destination_type;

  Bytes logrotate_max_stdout_size;
  Option<std::string> logrotate_stdout_options;

  Bytes logrotate_max_stderr_size;
  Option<std::string> logrotate_stderr_options;
};


struct Flags : public virtual LoggerFlags
{
  Flags()
  {
    add(&Flags::environment_variable_prefix,
        "environment_variable_prefix",
        "Prefix for environment variables meant to modify the behavior of\n"
        "the container logger for the specific executor being launched.\n"
        "The logger will look for the prefixed environment variables in the\n"
        "'ExecutorInfo's 'CommandInfo's 'Environment':\n"
        "  * DESTINATION_TYPE\n"
        "  * LOGROTATE_MAX_STDOUT_SIZE\n"
        "  * LOGROTATE_STDOUT_OPTIONS\n"
        "  * LOGROTATE_MAX_STDERR_SIZE\n"
        "  * LOGROTATE_STDERR_OPTIONS\n"
        "If present, these variables will override the global values set\n"
        "via module parameters.",
        "CONTAINER_LOGGER_");

    add(&Flags::companion_dir,
        "companion_dir",
        None(),
        "Directory where this module's companion binary is located.\n"
        "The journald container logger will find the '" +
        mesos::journald::logger::NAME + "'\n"
        "binary file under this directory.",
        static_cast<const std::string*>(nullptr),
        [](const std::string& value) -> Option<Error> {
          std::string executablePath =
            path::join(value, mesos::journald::logger::NAME);

          if (!os::exists(executablePath)) {
            return Error("Cannot find: " + executablePath);
          }

          return None();
        });

    add(&Flags::logrotate_path,
        "logrotate_path",
        "If specified, the logrotate container logger will use the specified\n"
        "'logrotate' instead of the system's 'logrotate'.",
        "logrotate",
        [](const std::string& value) -> Option<Error> {
          // Check if `logrotate` exists via the help command.
          // TODO(josephw): Consider a more comprehensive check.
          Try<std::string> helpCommand =
            os::shell(value + " --help > /dev/null");

          if (helpCommand.isError()) {
            return Error(
                "Failed to check logrotate: " + helpCommand.error());
          }

          return None();
        });

    add(&Flags::max_label_payload_size,
        "max_label_payload_size",
        "Maximum size of the label data transferred to the\n"
        "logger companion binary. Can be at most one megabyte.",
        Kilobytes(10),
        [](const Bytes& value) -> Option<Error> {
          if (value > Megabytes(1)) {
            return Error(
                "Maximum --max_label_payload_size is one megabyte");
          }

          return None();
        });

    add(&Flags::libprocess_num_worker_threads,
        "libprocess_num_worker_threads",
        "Number of Libprocess worker threads.\n"
        "Defaults to 8.  Must be at least 1.",
        8u,
        [](const size_t& value) -> Option<Error> {
          if (value < 1u) {
            return Error(
                "Expected --libprocess_num_worker_threads of at least 1");
          }

          return None();
        });
  }

  std::string environment_variable_prefix;

  std::string companion_dir;
  std::string logrotate_path;

  Bytes max_label_payload_size;

  size_t libprocess_num_worker_threads;
};


// For backwards compatibility, this module provides three modes:
//   * Log to journald.
//   * Log to journald and the sandbox, with log rotation.
//   * Log to sandbox, with log rotation.
//
// When logging to journald, the `JournaldContainerLogger` is a
// ContainerLogger module that pipes the stdout/stderr of a container
// to journald. The module assumes the stdout/stderr outputs generic
// single line logs (i.e. delineated by newlines) and adds identifying
// labels to each log line; such as the FrameworkID, ExecutorID,
// ContainerID, and labels inside ExecutorInfo.
//
// Logging to sandbox is approximately equivalent to using the
// `LogrotateContainerLogger`, packaged with vanilla Mesos.
// See `Flags` above.
class JournaldContainerLogger : public mesos::slave::ContainerLogger
{
public:
  JournaldContainerLogger(const Flags& _flags);

  virtual ~JournaldContainerLogger();

  // This is a noop.  The journald container logger has nothing to initialize.
  virtual Try<Nothing> initialize();

  virtual process::Future<mesos::slave::ContainerIO> prepare(
      const ContainerID& containerId,
      const mesos::slave::ContainerConfig& containerConfig);

protected:
  Flags flags;
  process::Owned<JournaldContainerLoggerProcess> process;
};

} // namespace journald {
} // namespace mesos {

#endif // __JOURNALD_LIB_JOURNALD_HPP__

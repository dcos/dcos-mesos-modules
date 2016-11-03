#ifndef __JOURNALD_LIB_JOURNALD_HPP__
#define __JOURNALD_LIB_JOURNALD_HPP__

#include <stdio.h>

#include <mesos/slave/container_logger.hpp>

#include <stout/flags.hpp>
#include <stout/option.hpp>

#include <stout/os/exists.hpp>

#include "journald.hpp"


namespace mesos {
namespace journald {

class JournaldContainerLoggerProcess;

struct Flags : public virtual flags::FlagsBase
{
  Flags()
  {
    add(&companion_dir,
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

    add(&libprocess_num_worker_threads,
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

  std::string companion_dir;

  size_t libprocess_num_worker_threads;
};


// The `JournaldContainerLogger` is a ContainerLogger module that pipes
// the stdout/stderr of a container to journald. The module assumes the
// stdout/stderr outputs generic single line logs (i.e. delineated by
// newlines) and adds identifying labels to each log line; such
// as the FrameworkID, ExecutorID, ContainerID, and labels inside
// ExecutorInfo.
class JournaldContainerLogger : public mesos::slave::ContainerLogger
{
public:
  JournaldContainerLogger(const Flags& _flags);

  virtual ~JournaldContainerLogger();

  // This is a noop.  The journald container logger has nothing to initialize.
  virtual Try<Nothing> initialize();

  // TODO(josephw): Remove this after bumping the Mesos version.
  virtual process::Future<Nothing> recover(
      const ExecutorInfo& executorInfo,
      const std::string& sandboxDirectory)
  {
    return Nothing();
  }

  virtual process::Future<mesos::slave::ContainerLogger::SubprocessInfo>
  prepare(
      const ExecutorInfo& executorInfo,
      const std::string& sandboxDirectory);

protected:
  Flags flags;
  process::Owned<JournaldContainerLoggerProcess> process;
};

} // namespace journald {
} // namespace mesos {

#endif // __JOURNALD_LIB_JOURNALD_HPP__

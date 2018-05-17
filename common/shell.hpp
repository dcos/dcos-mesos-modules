#ifndef __OVERLAY_UTILS_HPP__
#define __OVERLAY_UTILS_HPP__

#include <vector>

#include <process/collect.hpp>
#include <process/io.hpp>
#include <process/future.hpp>
#include <process/subprocess.hpp>

#include <stout/os/killtree.hpp>
#include <stout/os/shell.hpp>

namespace mesos {
namespace modules {
namespace common {

// Exec's a command.
inline process::Future<std::string> runCommand(
    const std::string& command,
    const std::vector<std::string>& argv,
    const Duration& timeout = Milliseconds(5000))
{
  Try<process::Subprocess> s = process::subprocess(
      command,
      argv,
      process::Subprocess::PATH("/dev/null"),
      process::Subprocess::PIPE(),
      process::Subprocess::PIPE());

  if (s.isError()) {
    return process::Failure(
      "Unable to execute '" + command + "': " + s.error());
  }
  pid_t pid = s->pid();

  typedef std::tuple<
    process::Future<Option<int>>,
    process::Future<std::string>,
    process::Future<std::string>> ProcessTuple;

  return await(
      s->status(),
      process::io::read(s->out().get()),
      process::io::read(s->err().get()))
    .after(timeout,
                [=](const process::Future<ProcessTuple> &t) ->
                    process::Future<ProcessTuple> {
        // NOTE: Discarding this future has no effect on the subprocess
        os::killtree(pid, SIGKILL);
        return std::make_tuple(-1, "", "timeout after " + stringify(timeout));
    })
    .then([command](const ProcessTuple& t) -> process::Future<std::string> {
        process::Future<Option<int>> status = std::get<0>(t);
        if (!status.isReady()) {
        return process::Failure(
          "Failed to get the exit status of '" + command +"': " +
          (status.isFailed() ? status.failure() : "discarded"));
        }

        if (status->isNone()) {
        return process::Failure("Failed to reap the subprocess");
        }

        process::Future<std::string> out = std::get<1>(t);
        if (!out.isReady()) {
        return process::Failure(
          "Failed to read stderr from the subprocess: " +
          (out.isFailed() ? out.failure() : "discarded"));
        }

        process::Future<std::string> err = std::get<2>(t);
        if (!err.isReady()) {
          return process::Failure(
              "Failed to read stderr from the subprocess: " +
              (err.isFailed() ? err.failure() : "discarded"));
        }

        if (status.get() != 0) {
          return process::Failure(
              "Failed to execute '" + command + "': " + err.get());
        }

        return out.get();
    });
};


// Run `command` as a shell script. This is useful when wanting to
// chain shell commands.
inline process::Future<std::string> runScriptCommand(
    const std::string& command,
    const Duration& timeout = Milliseconds(5000))
{
  std::vector<std::string> argv = {os::Shell::arg0, os::Shell::arg1, command};

  return runCommand(os::Shell::name, argv, timeout);
};

} // namespace common {
} // namespace modules {
} // namespace mesos {

#endif // __OVERLAY_UTILS_HPP__

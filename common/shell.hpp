#ifndef __OVERLAY_UTILS_HPP__
#define __OVERLAY_UTILS_HPP__

#include <vector>

#include <stout/os/kill.hpp>

#include <process/collect.hpp>
#include <process/io.hpp>
#include <process/future.hpp>
#include <process/subprocess.hpp>

namespace mesos {
namespace modules {
namespace common {

// Run `command` as a shell script. This is useful when wanting to
// chain shell commands.
inline process::Future<std::string> runScriptCommand(
    const std::string& command)
{
  Try<process::Subprocess> s = process::subprocess(
      command,
      process::Subprocess::PATH("/dev/null"),
      process::Subprocess::PIPE(),
      process::Subprocess::PIPE());

  if (s.isError()) {
    return process::Failure(
        "Unable to execute '" + command + "': " + s.error());
  }

  pid_t pid = s->pid();

  return await(
      s->status(),
      process::io::read(s->out().get()),
      process::io::read(s->err().get()))
    .onDiscard([pid](){
        LOG(WARNING) << "Subprocess discarded. About to kill PID: " << pid;
        os::kill(pid, SIGKILL);})
    .then([command](
          const std::tuple<process::Future<Option<int>>,
          process::Future<std::string>,
          process::Future<std::string>>& t) -> process::Future<std::string> {
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


// Exec's a command.
inline process::Future<std::string> runCommand(
    const std::string& command,
    const std::vector<std::string>& argv)
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

  return await(
      s->status(),
      process::io::read(s->out().get()),
      process::io::read(s->err().get()))
    .onDiscard([pid](){
        LOG(WARNING) << "Subprocess discarded. About to kill PID: " << pid;
        os::kill(pid, SIGKILL);})
    .then([command](
          const std::tuple<process::Future<Option<int>>,
          process::Future<std::string>,
          process::Future<std::string>>& t) -> process::Future<std::string> {
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

} // namespace common {
} // namespace modules {
} // namespace mesos {

#endif // __OVERLAY_UTILS_HPP__

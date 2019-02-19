#ifndef __SUPERVISOR_OVERLAY_HPP__
#define __SUPERVISOR_OVERLAY_HPP__

#include <queue>

#include <stout/duration.hpp>
#include <stout/lambda.hpp>
#include <stout/try.hpp>

#include <process/delay.hpp>
#include <process/owned.hpp>
#include <process/process.hpp>

namespace mesos {
namespace modules {
namespace overlay {
namespace supervisor {

template <typename T>
class ProcessSupervisor : public process::Process<ProcessSupervisor<T>>
{
public:
  typedef lambda::function<Try<process::Owned<T>>()> Func;

  static Try<process::Owned<ProcessSupervisor<T>>> create(
      const Func& func,
      const Duration& delay)
  {
    Try<process::Owned<T>> result = func();
    if (result.isError()) {
      LOG(ERROR) << "Unable to create the process: " << result.error();
      return Error(result.error());
    }

    return process::Owned<ProcessSupervisor<T>>(
      new ProcessSupervisor(result.get(), func, delay));
  }

  ~ProcessSupervisor()
  {
    LOG(INFO) << "Terminating the child process.";
    state = State::TERMINATING;
    if (process.get() != nullptr) {
      terminate(process.get());
      wait(process.get());
      process.reset();
    }
  }

  const process::Future<process::UPID> child()
  {
    if (process.get() != nullptr && state == State::SPAWNED) {
      return process->self();
    } else {
      process::Promise<process::UPID> *promise =
        new process::Promise<process::UPID>();
      promises.push(promise);
      return promise->future();
    }
  }

protected:
  // Because we're deriving from a templated base class, we have
  // to explicitly bring these hidden base class names into scope.
  using process::Process<ProcessSupervisor<T>>::self;
  using process::Process<ProcessSupervisor<T>>::link;
  typedef ProcessSupervisor<T> Self;

  virtual void initialize() override
  {
    LOG(INFO) << "(Re-)starting the process";

    if (process.get() == nullptr) {
        Try<process::Owned<T>> result = func();
        if (result.isError()) {
          LOG(ERROR) << "Unable to create the process: " << result.error();
          process::delay(delay, self(), &Self::initialize);
          return;
        }
        state = State::UNSPAWNED;
        process = result.get();
    }

    spawn(process.get());
    state = State::SPAWNED;

    LOG(INFO) << "Linking the process: " << process->self();
    link(process->self());

    while (!promises.empty()) {
      process::Promise<process::UPID>* promise = promises.front();
      promise->set(process->self());
      delete promise;
      promises.pop();
    }
  }

  virtual void exited(const process::UPID& pid) override
  {
    LOG(WARNING) << "The process has exited: " << pid;

    if (state != State::TERMINATING) {
      state = State::TERMINATING;
      process.reset();

      // Re-start the process with delay.
      process::delay(delay, self(), &Self::initialize);
    }
  }

private:
  enum class State
  {
    UNSPAWNED,
    SPAWNED,
    TERMINATING
  };

  ProcessSupervisor(
    process::Owned<T> _process,
    const Func& _func,
    const Duration& _delay)
    : func(_func),
      delay(_delay),
      process(_process),
      state(State::UNSPAWNED) {}

  const Func func;
  const Duration delay;
  process::Owned<T> process;
  std::queue<process::Promise<process::UPID>*> promises;
  State state;
};

} // namespace supervisor {
} // namespace overlay {
} // namespace modules {
} // namespace mesos {

#endif // __SUPERVISOR_OVERLAY_HPP__

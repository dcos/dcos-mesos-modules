#include <string>
#include <vector>

#include <sys/uio.h> // For `struct iovec`.

#include <systemd/sd-journal.h>

#include <process/dispatch.hpp>
#include <process/future.hpp>
#include <process/io.hpp>
#include <process/process.hpp>

#include <stout/error.hpp>
#include <stout/exit.hpp>
#include <stout/nothing.hpp>
#include <stout/strings.hpp>
#include <stout/try.hpp>

#include <stout/os/pagesize.hpp>

#include "journald.hpp"


using namespace process;
using namespace mesos::journald::logger;


class JournaldLoggerProcess : public Process<JournaldLoggerProcess>
{
public:
  JournaldLoggerProcess(const Flags& _flags)
    : flags(_flags)
  {
    // Prepare a buffer for reading from the `incoming` pipe.
    length = os::pagesize();
    buffer = new char[length];
  }

  virtual ~JournaldLoggerProcess()
  {
    if (buffer != NULL) {
      delete[] buffer;
      buffer = NULL;
    }

    if (entries != NULL) {
      for (int i = 0; i < num_entries - 1; i++) {
        if (entries != NULL) {
          delete[] (char*) entries[i].iov_base;
          entries[i].iov_base = NULL;
        }
      }

      delete[] entries;
      entries = NULL;
    }
  }

  // Prepares and starts the loop which reads from stdin and writes to journald.
  Future<Nothing> run()
  {
    // Pre-populate the `iovec` with the constant labels.
    num_entries = flags.parsed_labels.labels().size() + 1;
    entries = new struct iovec[num_entries];

    for (int i = 0; i < flags.parsed_labels.labels().size(); i++) {
      const mesos::Label& label = flags.parsed_labels.labels(i);

      const std::string entry =
        strings::upper(label.key()) + "=" + label.value();

      // Copy the label as a C-string.
      entries[i].iov_len = entry.length();
      entries[i].iov_base = new char[entry.length() + 1];
      std::strcpy((char*) entries[i].iov_base, entry.c_str());
    }

    // NOTE: This is a prerequisuite for `io::read`.
    Try<Nothing> nonblock = os::nonblock(STDIN_FILENO);
    if (nonblock.isError()) {
      return Failure("Failed to set nonblocking pipe: " + nonblock.error());
    }

    // NOTE: This does not block.
    loop();

    return promise.future();
  }

  // Reads from stdin and writes to journald.
  void loop()
  {
    io::read(STDIN_FILENO, buffer, length)
      .then([&](size_t readSize) -> Future<Nothing> {
        // Check if EOF has been reached on the input stream.
        // This indicates that the container (whose logs are being
        // piped to this process) has exited.
        if (readSize <= 0) {
          promise.set(Nothing());
          return Nothing();
        }

        // Write the bytes to journald.
        Try<Nothing> result = write(readSize);
        if (result.isError()) {
          promise.fail("Failed to write: " + result.error());
          return Nothing();
        }

        // Use `dispatch` to limit the size of the call stack.
        dispatch(self(), &JournaldLoggerProcess::loop);

        return Nothing();
      });
  }

  // Writes the buffer from stdin to the journald.
  // Any `flags.labels` will be prepended to each line.
  Try<Nothing> write(size_t readSize)
  {
    // We may be reading more than one log line at once,
    // but we need to add labels for each line.
    std::string logs(buffer, readSize);
    std::vector<std::string> lines = strings::split(logs, "\n");

    foreach (const std::string& line, lines) {
      if (line.empty()) {
        continue;
      }

      const std::string entry = "MESSAGE=" + line;

      entries[num_entries - 1].iov_len = entry.length();
      entries[num_entries - 1].iov_base = const_cast<char*>(entry.c_str());

      sd_journal_sendv(entries, num_entries);
    }

    // Even if the write fails, we ignore the error.
    return Nothing();
  }

private:
  Flags flags;

  // For reading from stdin.
  char* buffer;
  size_t length;

  // Used as arguments for `sd_journal_sendv`.
  // This contains one more entry than the number of `--labels`.
  // The last entry holds a pointer to a stack-allocated C-string,
  // which is changed each time we write to journald.
  int num_entries;
  struct iovec* entries;

  // Used to capture when the logging has completed because the
  // underlying process/input has terminated.
  Promise<Nothing> promise;
};


int main(int argc, char** argv)
{
  Flags flags;

  // Load and validate flags from the environment and command line.
  Try<flags::Warnings> load = flags.load(None(), &argc, &argv);

  if (load.isError()) {
    EXIT(EXIT_FAILURE) << flags.usage(load.error());
  }

  // Log any flag warnings.
  foreach (const flags::Warning& warning, load->warnings) {
    LOG(WARNING) << warning.message;
  }

  // Asynchronously control the flow and size of logs.
  JournaldLoggerProcess process(flags);
  spawn(&process);

  // Wait for the logging process to finish.
  Future<Nothing> status = dispatch(process, &JournaldLoggerProcess::run);
  status.await();

  terminate(process);
  wait(process);

  return status.isReady() ? EXIT_SUCCESS : EXIT_FAILURE;
}

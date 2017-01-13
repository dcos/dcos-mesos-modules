#include <string>
#include <vector>

#include <sys/uio.h> // For `struct iovec`.

#include <systemd/sd-journal.h>

#include <process/dispatch.hpp>
#include <process/future.hpp>
#include <process/id.hpp>
#include <process/io.hpp>
#include <process/process.hpp>

#include <stout/error.hpp>
#include <stout/exit.hpp>
#include <stout/nothing.hpp>
#include <stout/strings.hpp>
#include <stout/try.hpp>

#include <stout/os/pagesize.hpp>
#include <stout/os/shell.hpp>
#include <stout/os/su.hpp>

#include "journald.hpp"


using namespace process;
using namespace mesos::journald::logger;


class JournaldLoggerProcess : public Process<JournaldLoggerProcess>
{
public:
  JournaldLoggerProcess(const Flags& _flags)
    : ProcessBase(process::ID::generate("journald-logger")),
      flags(_flags)
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

    if (leading.isSome()) {
      os::close(leading.get());
    }
  }

  // Prepares and starts the loop which reads from stdin and writes to
  // journald or the sandbox, depending on the input flags.
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

    if (flags.destination_type == "logrotate" ||
        flags.destination_type == "journald+logrotate") {
      // Populate the `logrotate` configuration file.
      // See `Flags::logrotate_options` for the format.
      //
      // NOTE: We specify a size of `--logrotate_max_size - length`
      // because `logrotate` has slightly different size semantics.
      // `logrotate` will rotate when the max size is *exceeded*.
      // We rotate to keep files *under* the max size.
      const std::string config =
        "\"" + flags.logrotate_filename.get() + "\" {\n" +
        flags.logrotate_options.getOrElse("") + "\n" +
        "size " + stringify(flags.logrotate_max_size.bytes() - length) + "\n" +
        "}";

      Try<Nothing> result = os::write(
          flags.logrotate_filename.get() + LOGROTATE_CONF_SUFFIX, config);

      if (result.isError()) {
        return Failure(
            "Failed to write logrotate configuration file: " + result.error());
      }
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

        if (flags.destination_type == "journald" ||
            flags.destination_type == "journald+logrotate") {
          // Write the bytes to journald.
          Try<Nothing> result = write_journald(readSize);
          if (result.isError()) {
            promise.fail("Failed to write: " + result.error());
            return Nothing();
          }
        }

        if (flags.destination_type == "logrotate" ||
            flags.destination_type == "journald+logrotate") {
          // Write the bytes to sandbox, with log rotation.
          Try<Nothing> result = write_logrotate(readSize);
          if (result.isError()) {
            promise.fail("Failed to write: " + result.error());
            return Nothing();
          }
        }

        // Use `dispatch` to limit the size of the call stack.
        dispatch(self(), &JournaldLoggerProcess::loop);

        return Nothing();
      });
  }

  // Writes the buffer from stdin to the journald.
  // Any `flags.journald_labels` will be prepended to each line.
  Try<Nothing> write_journald(size_t readSize)
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


  // Writes the buffer from stdin to the leading log file.
  // When the number of written bytes exceeds `--logrotate_max_size`,
  // the leading log file is rotated.  When the number of log files
  // exceed `--max_files`, the oldest log file is deleted.
  Try<Nothing> write_logrotate(size_t readSize)
  {
    // Rotate the log file if it will grow beyond the `--logrotate_max_size`.
    if (bytesWritten + readSize > flags.logrotate_max_size.bytes()) {
      rotate();
    }

    // If the leading log file is not open, open it.
    // NOTE: We open the file in append-mode as `logrotate` may sometimes fail.
    if (leading.isNone()) {
      Try<int> open = os::open(
          flags.logrotate_filename.get(),
          O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC,
          S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);

      if (open.isError()) {
        return Error(
            "Failed to open '" + flags.logrotate_filename.get() +
            "': " + open.error());
      }

      leading = open.get();
    }

    // Write from stdin to `leading`.
    // NOTE: We do not exit on error here since we are prioritizing
    // clearing the STDIN pipe (which would otherwise potentially block
    // the container on write) over log fidelity.
    Try<Nothing> result =
      os::write(leading.get(), std::string(buffer, readSize));

    if (result.isError()) {
      std::cerr << "Failed to write: " << result.error() << std::endl;
    }

    bytesWritten += readSize;

    return Nothing();
  }

  // Calls `logrotate` on the leading log file and resets the `bytesWritten`.
  void rotate()
  {
    if (leading.isSome()) {
      os::close(leading.get());
      leading = None();
    }

    // Call `logrotate` to move around the files.
    // NOTE: If `logrotate` fails for whatever reason, we will ignore
    // the error and continue logging.  In case the leading log file
    // is not renamed, we will continue appending to the existing
    // leading log file.
    os::shell(
        flags.logrotate_path +
        " --state \"" + flags.logrotate_filename.get() + LOGROTATE_STATE_SUFFIX +
        "\" \"" + flags.logrotate_filename.get() + LOGROTATE_CONF_SUFFIX + "\"");

    // Reset the number of bytes written.
    bytesWritten = 0;
  }

private:
  Flags flags;

  // For reading from stdin.
  char* buffer;
  size_t length;

  // For writing and rotating the leading log file.
  Option<int> leading;
  size_t bytesWritten;

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

  // If the `--user` flag is set, change the UID of this process to that user.
  if (flags.user.isSome()) {
    Try<Nothing> result = os::su(flags.user.get());

    if (result.isError()) {
      EXIT(EXIT_FAILURE)
        << ErrnoError("Failed to switch user for journald logger").message;
    }
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

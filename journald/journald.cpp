#include <string>
#include <vector>

#include <sys/uio.h> // For `struct iovec`.

#include <systemd/sd-journal.h>

#include <process/address.hpp>
#include <process/dispatch.hpp>
#include <process/future.hpp>
#include <process/id.hpp>
#include <process/io.hpp>
#include <process/network.hpp>
#include <process/process.hpp>

#include <stout/check.hpp>
#include <stout/error.hpp>
#include <stout/exit.hpp>
#include <stout/json.hpp>
#include <stout/jsonify.hpp>
#include <stout/nothing.hpp>
#include <stout/path.hpp>
#include <stout/strings.hpp>
#include <stout/try.hpp>

#include <stout/os/chdir.hpp>
#include <stout/os/pagesize.hpp>
#include <stout/os/shell.hpp>
#include <stout/os/su.hpp>

#include "journald.hpp"


using namespace process;
using namespace mesos::journald::logger;

// Forward declare a helper found in `src/linux/memfd.hpp`.
namespace mesos {
namespace internal {
namespace memfd {

// Creates an anonymous in-memory file via `memfd_create`.
Try<int_fd> create(const std::string& name, unsigned int flags);

} // namespace memfd {
} // namespace internal {
} // namespace mesos {

class JournaldLoggerProcess : public Process<JournaldLoggerProcess>
{
public:
  static Try<JournaldLoggerProcess*> create(const Flags& flags)
  {
    Option<int_fd> configMemFd;

    // Calculate the size of the buffer that is used for reading from
    // the `incoming` pipe.
    const size_t bufferSize = os::pagesize();

    if (flags.destination_type == "logrotate" ||
        flags.destination_type == "journald+logrotate" ||
        flags.destination_type == "fluentbit+logrotate") {
      // Populate the `logrotate` configuration file.
      // See `Flags::logrotate_options` for the format.
      //
      // NOTE: We specify a size of `--max_size - bufferSize` because
      // `logrotate` has slightly different size semantics. `logrotate`
      // will rotate when the max size is *exceeded*. We rotate to keep
      // files *under* the max size.
      const std::string config =
        "\"" + Path(flags.logrotate_filename.get()).basename() + "\" {\n" +
        flags.logrotate_options.getOrElse("") + "\n" +
        "size " + stringify(flags.logrotate_max_size.bytes() - bufferSize) +
        "\n}";

      // Create a temporary anonymous file, which can be accessed by a child
      // process by opening a `/proc/self/fd/<FD of anonymous file>`.
      // This file is automatically removed on process termination, so we don't
      // need to garbage collect it.
      // We use the `memfd` file to pass the configuration to `logrotate`.
      Try<int_fd> memFd =
        mesos::internal::memfd::create("mesos_logrotate", 0);

      if (memFd.isError()) {
        return Error(
            "Failed to create memfd file '" +
            flags.logrotate_filename.get() + "': " + memFd.error());
      }

      Try<Nothing> write = os::write(memFd.get(), config);
      if (write.isError()) {
        os::close(memFd.get());
        return Error(
            "Failed to write memfd file '" + flags.logrotate_filename.get() +
            "': " + write.error());
      }

      // `logrotate` requires configuration file to have 0644 or 0444
      // permissions.
      if (fchmod(memFd.get(), S_IRUSR | S_IRGRP | S_IROTH) == -1) {
        ErrnoError error("Failed to chmod memfd file '" +
                         flags.logrotate_filename.get() + "'");
        os::close(memFd.get());
        return error;
      }

      configMemFd = memFd.get();
    }

    return new JournaldLoggerProcess(flags, configMemFd, bufferSize);
  }

  virtual ~JournaldLoggerProcess()
  {
    if (configMemFd.isSome()) {
      os::close(configMemFd.get());
    }

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
    // Pre-populate the `iovec` and `fluentbit_object` with the constant labels.
    num_entries = flags.parsed_labels.labels().size() + 1;
    entries = new struct iovec[num_entries];

    for (int i = 0; i < flags.parsed_labels.labels().size(); i++) {
      const mesos::Label& label = flags.parsed_labels.labels(i);
      const std::string key = strings::upper(label.key());

      // Fluentbit is written as a JSON object.
      fluentbit_object.values[key] = label.value();

      // Journald requires conversion into the `iovec` structure,
      // which contains C-strings of concatenated key-values.
      const std::string entry = key + "=" + label.value();

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
    io::read(STDIN_FILENO, buffer, bufferSize)
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
            flags.destination_type == "journald+logrotate" ||
            flags.destination_type == "fluentbit+logrotate") {
          // Write the bytes to sandbox, with log rotation.
          Try<Nothing> result = write_logrotate(readSize);
          if (result.isError()) {
            promise.fail("Failed to write: " + result.error());
            return Nothing();
          }
        }

        if (flags.destination_type == "fluentbit" ||
            flags.destination_type == "fluentbit+logrotate") {
          // Write the bytes to sandbox, with log rotation.
          Try<Nothing> result = write_fluentbit(readSize);
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
      std::string basename = Path(flags.logrotate_filename.get()).basename();

      Try<int> open = os::open(
          basename,
          O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC,
          S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);

      if (open.isError()) {
        return Error("Failed to open '" + basename + "': " + open.error());
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

  // Writes the buffer from stdin to the specified fluentbit address.
  Try<Nothing> write_fluentbit(size_t readSize)
  {
    if (fluentbit_socket.isNone()) {
      // Create a TCP socket.
      fluentbit_socket =
        net::socket(flags.fluentbit_ip->family(), SOCK_STREAM, 0);

      if (fluentbit_socket->isError()) {
        return Error("Failed to create socket: " + fluentbit_socket->error());
      }

      // Try to connect to fluentbit.
      Try<Nothing, SocketError> connect = network::connect(
          fluentbit_socket->get(),
          network::inet::Address(
              flags.fluentbit_ip.get(), flags.fluentbit_port));

      // NOTE: Sending messages to fluentbit is on a best-effort basis.
      // If we cannot connect for whatever reason, lines will be dropped.
      if (connect.isError()) {
        std::cerr
          << "Failed to connect to fluentbit: " << connect.error() << std::endl;

        os::close(fluentbit_socket->get());
        fluentbit_socket = None();
        return Nothing();
      }
    }

    // We may be reading more than one log line at once,
    // but we need to add labels for each line.
    std::string logs(buffer, readSize);
    std::vector<std::string> lines = strings::split(logs, "\n");

    foreach (const std::string& line, lines) {
      if (line.empty()) {
        continue;
      }

      // Overwrite a specific key in the stored object before writing to the
      // socket. Since labels are stored in all capital letters, there will
      // be no key conflicts.
      fluentbit_object.values["line"] = line;

      Try<Nothing> result =
        os::write(fluentbit_socket->get(), jsonify(fluentbit_object));

      // If we fail to write, assume the socket has broken.
      // We'll close the socket and try to reconnect later.
      // In the meantime, the current batch of lines will be dropped.
      if (result.isError()) {
        std::cerr << "Failed to write: " << result.error() << std::endl;

        os::close(fluentbit_socket->get());
        fluentbit_socket = None();
        return Nothing();
      }
    }

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
        " --state \"" + Path(flags.logrotate_filename.get()).basename() +
        LOGROTATE_STATE_SUFFIX + "\" \"" + configPath.get() + "\"");

    // Reset the number of bytes written.
    bytesWritten = 0;
  }

private:
  explicit JournaldLoggerProcess(
      const Flags& _flags,
      const Option<int_fd>& _configMemFd,
      size_t _bufferSize)
    : ProcessBase(process::ID::generate("logrotate-logger")),
      flags(_flags),
      configMemFd(_configMemFd),
      buffer(new char[_bufferSize]),
      bufferSize(_bufferSize),
      leading(None()),
      bytesWritten(0)
  {
    if (configMemFd.isSome()) {
      configPath = "/proc/self/fd/" + stringify(configMemFd.get());
    }
  }

  const Flags flags;
  const Option<int_fd> configMemFd;
  Option<std::string> configPath;

  // For reading from stdin.
  char* buffer;
  const size_t bufferSize;

  // For writing and rotating the leading log file.
  Option<int> leading;
  size_t bytesWritten;

  // Used as arguments for `sd_journal_sendv`.
  // This contains one more entry than the number of `--labels`.
  // The last entry holds a pointer to a stack-allocated C-string,
  // which is changed each time we write to journald.
  int num_entries;
  struct iovec* entries;

  // The connection to the specified fluentbit address.
  Option<Try<int_fd>> fluentbit_socket;

  // Object written to fluentbit, which is pre-filled with labels associated
  // with the stream.  The "line" key is overwritten with the log line,
  // before writing over the socket.
  JSON::Object fluentbit_object;

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

  // Change the current working directory to the container's sandbox directory.
  // This is to handle the case that the nested container's user is a non-root
  // user and different from its parent container's user, in which case the
  // nested container's user (i.e.,`flags.user`) has no permission to access
  // `flags.logrotate_filename` since it has no permission to traverse its
  // parent container's sandbox directory whose permissions is 0750.
  CHECK_SOME(flags.logrotate_filename);

  Try<Nothing> result =
    os::chdir(Path(flags.logrotate_filename.get()).dirname());

  if (result.isError()) {
    EXIT(EXIT_FAILURE) << ErrnoError(
        "Failed to switch working directory for journald logger").message;
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
  Try<JournaldLoggerProcess*> process = JournaldLoggerProcess::create(flags);
  if (process.isError()) {
    EXIT(EXIT_FAILURE)
      << "Failed to create logger process: " << process.error();
  }

  spawn(process.get());

  // Wait for the logging process to finish.
  Future<Nothing> status =
    dispatch(process.get(), &JournaldLoggerProcess::run);

  status.await();

  terminate(process.get());
  wait(process.get());

  delete process.get();

  return status.isReady() ? EXIT_SUCCESS : EXIT_FAILURE;
}

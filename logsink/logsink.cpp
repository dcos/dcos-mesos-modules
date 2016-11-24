#include <string>

#include <glog/logging.h>

#include <mesos/mesos.hpp>
#include <mesos/module.hpp>
#include <mesos/module/anonymous.hpp>

#include <process/owned.hpp>

#include <stout/os.hpp>

#include <stout/os/close.hpp>
#include <stout/os/exists.hpp>
#include <stout/os/mkdir.hpp>
#include <stout/os/open.hpp>
#include <stout/os/write.hpp>

#include "logsink.hpp"


using mesos::Parameter;
using mesos::Parameters;

using mesos::modules::Anonymous;
using mesos::modules::Module;

namespace mesos {
namespace logsink {


FileSink::FileSink(const Flags& _flags) : flags(_flags)
{
  if (!os::exists(flags.output_file)) {
    // Create the log directory (noop if it already exists).
    CHECK_SOME(os::mkdir(Path(flags.output_file).dirname()));
  }

  // Open the file in append mode (or create it if it doesn't exist).
  Try<int> open = os::open(
      flags.output_file,
      O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC,
      S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
  CHECK_SOME(open);

  logFd = open.get();
}


FileSink::~FileSink()
{
  os::close(logFd);
}


void FileSink::send(
    google::LogSeverity severity,
    const char* full_filename,
    const char* base_filename,
    int line,
    const struct ::tm* tm_time,
    const char* message,
    size_t message_len)
{
  os::write(
      logFd,
      ToString(
          severity,
          base_filename,
          line,
          tm_time,
          message,
          // NOTE: The LogSink's message length excludes the newline.
          message_len + 1));
}


void FileSink::WaitTillSent() {}


// An anonymous module that owns and hooks up a new LogSink to glog.
class AnonymousWrapper : public Anonymous
{
public:
  AnonymousWrapper(const Flags& flags) : sink(new FileSink(flags))
  {
    google::AddLogSink(sink.get());

    LOG(INFO) << "Added FileSink for glog logs to: " << flags.output_file;
  };

  ~AnonymousWrapper()
  {
    google::RemoveLogSink(sink.get());

    LOG(INFO) << "Removed FileSink for glog logs";
  };

protected:
  process::Owned<FileSink> sink;
};

} // namespace mesos {
} // namespace logsink {


Module<Anonymous> com_mesosphere_mesos_LogSink(
    MESOS_MODULE_API_VERSION,
    MESOS_VERSION,
    "Mesosphere",
    "help@mesosphere.io",
    "Mesos LogSink Anonymous Module",
    NULL,
    [](const Parameters& parameters) -> Anonymous* {
      // Convert `parameters` into a map.
      std::map<std::string, std::string> values;
      foreach (const Parameter& parameter, parameters.parameter()) {
        values[parameter.key()] = parameter.value();
      }

      // Load and validate flags from the map.
      mesos::logsink::Flags flags;
      Try<flags::Warnings> load = flags.load(values);

      if (load.isError()) {
        LOG(ERROR) << "Failed to parse parameters: " << load.error();
        return nullptr;
      }

      // Log any flag warnings.
      foreach (const flags::Warning& warning, load->warnings) {
        LOG(WARNING) << warning.message;
      }

      return new mesos::logsink::AnonymousWrapper(flags);
    });

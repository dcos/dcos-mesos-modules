#ifndef __LOGSINK_LOGSINK_HPP__
#define __LOGSINK_LOGSINK_HPP__

#include <mutex>
#include <string>

#include <glog/logging.h>

#include <stout/flags.hpp>
#include <stout/synchronized.hpp>


namespace mesos {
namespace logsink {

class FileSinkProcess;

struct Flags : public virtual flags::FlagsBase
{
  Flags()
  {
    add(&Flags::output_file,
        "output_file",
        "Where the LogSink should write all logs.\n"
        "If the file already exists, we will append to the file.\n"
        "If no file exists, a new one will be created.");
  }

  std::string output_file;
};


// A glog LogSink that writes logs to a file.
// We do not use glog's native file-writing capabilities as we prefer
// to control the naming conventions of log files and how logs are rotated.
class FileSink : public google::LogSink
{
public:
  FileSink(const Flags& _flags);

  ~FileSink();

  virtual void send(
      google::LogSeverity severity,
      const char* full_filename,
      const char* base_filename,
      int line,
      const struct ::tm* tm_time,
      const char* message,
      size_t message_len,
      google::int32 usecs);

  // This method implements the now obsolete LogSink interface without the
  // `usecs` argument introduced by a glog patch in MESOS-9687.
  //
  // At the moment of writing defining this method is still necessary due
  // to the need to maintain compatibility between glog and the old LogSink
  // implementations.
  virtual void send(
      google::LogSeverity severity,
      const char* full_filename,
      const char* base_filename,
      int line,
      const struct ::tm* tm_time,
      const char* message,
      size_t message_len);

  virtual void WaitTillSent();

protected:
  Flags flags;
  int_fd logFd;
  std::recursive_mutex mutex;
};

} // namespace logsink {
} // namespace mesos {

extern mesos::modules::Module<mesos::modules::Anonymous>
  com_mesosphere_mesos_LogSink;

#endif // __LOGSINK_LOGSINK_HPP__

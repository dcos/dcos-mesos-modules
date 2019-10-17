#ifndef __JOURNALD_JOURNALD_HPP__
#define __JOURNALD_JOURNALD_HPP__

#include <stdio.h>

#include <string>

#include <mesos/mesos.hpp>

#include <stout/error.hpp>
#include <stout/flags.hpp>
#include <stout/json.hpp>
#include <stout/option.hpp>
#include <stout/protobuf.hpp>

#include <stout/os/pagesize.hpp>
#include <stout/os/shell.hpp>


namespace mesos {
namespace journald {
namespace logger {

#ifndef __WINDOWS__
const std::string NAME = "mesos-journald-logger";
#else
const std::string NAME = "mesos-journald-logger.exe";
#endif // __WINDOWS__
const std::string LOGROTATE_CONF_SUFFIX = ".logrotate.conf";
const std::string LOGROTATE_STATE_SUFFIX = ".logrotate.state";

struct Flags : public virtual flags::FlagsBase
{
  Flags()
  {
    setUsageMessage(
      "Usage: " + NAME + " [options]\n"
      "\n"
      "This command pipes from STDIN to a destination of choice.\n"
      "If supported by the destination, each line (delineated by newline)\n"
      "of STDIN is labeled with --journald_labels before it is written.\n"
      "See '--journald_labels'."
      "\n");

    add(&Flags::destination_type,
        "destination_type",
        "Determines where logs should be piped.\n"
        "Valid destinations include: 'logrotate', 'fluentbit',\n"
#ifndef __WINDOWS__
        "'journald', 'journald+logrotate',\n"
#endif // __WINDOWS__
        "or 'fluentbit+logrotate'.",
        "logrotate",
        [](const std::string& value) -> Option<Error> {
          if (value != "logrotate" &&
              value != "fluentbit" &&
#ifndef __WINDOWS__
              value != "journald" &&
              value != "journald+logrotate" &&
#endif // __WINDOWS
              value != "fluentbit+logrotate") {
            return Error("Invalid destination type: " + value);
          }

          return None();
        });

    // NOTE: This flag is supported on Windows because the same
    // labels are used for journald and fluentbit.
    add(&Flags::journald_labels,
        "journald_labels",
        "Labels to append to each line of logs written to journald.\n"
        "This field should be the jsonified 'Labels' protobuf. i.e.:\n"
        "{\n"
        "  \"labels\": [\n"
        "    {\n"
        "      \"key\": \"SOME_KEY\"\n"
        "      \"value\": \"some_value\"\n"
        "    }, ...\n"
        "  ]\n"
        "}\n"
        "NOTE: All label keys will be converted to uppercase.\n"
        "\n",
        [this](const Option<std::string>& value) -> Option<Error> {
          if (value.isNone()) {
            return None();
          }

          Try<JSON::Object> json = JSON::parse<JSON::Object>(value.get());
          if (json.isError()) {
            return Error(
                "Failed to parse --journald_labels as JSON: " + json.error());
          }

          Try<Labels> _labels = ::protobuf::parse<Labels>(json.get());
          if (_labels.isError()) {
            return Error(
                "Failed to parse --journald_labels as protobuf: " +
                _labels.error());
          }

          parsed_labels = _labels.get();
          return None();
        });

    add(&Flags::logrotate_max_size,
        "logrotate_max_size",
        "Maximum size, in bytes, of a single log file.\n"
        "Defaults to 10 MB.  Must be at least 1 (memory) page.",
        Megabytes(10),
        [](const Bytes& value) -> Option<Error> {
          if (value.bytes() < os::pagesize()) {
            return Error(
                "Expected --logrotate_max_size of at least " +
                stringify(os::pagesize()) + " bytes");
          }
          return None();
        });

    add(&Flags::logrotate_options,
        "logrotate_options",
        "Additional config options to pass into 'logrotate'.\n"
        "This string will be inserted into a 'logrotate' configuration file.\n"
        "i.e.\n"
        "  /path/to/<log_filename> {\n"
        "    <logrotate_options>\n"
        "    size <logrotate_max_size>\n"
        "  }\n"
        "NOTE: The 'size' option will be overridden by this command.");

    add(&Flags::logrotate_filename,
        "logrotate_filename",
        "Absolute path to the leading log file.\n"
        "NOTE: This command will also create two files by appending\n"
        "'" + LOGROTATE_CONF_SUFFIX + "' and '" +
        LOGROTATE_STATE_SUFFIX + "' to the end of\n"
        "'--logrotate_filename'.  These files are used by 'logrotate'.",
        [](const Option<std::string>& value) -> Option<Error> {
          if (value.isNone()) {
            return Error("Missing required option --logrotate_filename");
          }

          if (!path::absolute(value.get())) {
            return Error(
                "Expected --logrotate_filename to be an absolute path");
          }

          return None();
        });

    add(&Flags::logrotate_path,
        "logrotate_path",
        "If specified, this command will use the specified\n"
        "'logrotate' instead of the system's 'logrotate'.",
        "logrotate",
        [](const std::string& value) -> Option<Error> {
          // Check if `logrotate` exists via the help command.
          // TODO(josephw): Consider a more comprehensive check.
          Try<std::string> helpCommand =
#ifndef __WINDOWS__
            os::shell(value + " --help > /dev/null");
#else
            os::shell(value);
#endif // __WINDOWS__

          if (helpCommand.isError()) {
            return Error(
                "Failed to check logrotate: " + helpCommand.error());
          }

          return None();
        });

    add(&Flags::user,
        "user",
        "The user this command should run as.");

    add(&Flags::fluentbit_ip,
        "fluentbit_ip",
        "IP of the Fluent Bit TCP listener.",
        [](const Option<net::IP>& value) -> Option<Error> {
          if (value.isNone()) {
            return Error("--fluentbit_ip is required");
          }

          return None();
        });

    add(&Flags::fluentbit_port,
        "fluentbit_port",
        "Port of the Fluent Bit TCP listener.");
  }

  std::string destination_type;

  Option<std::string> journald_labels;

  // Values populated during validation.
  Labels parsed_labels;

  Bytes logrotate_max_size;
  Option<std::string> logrotate_options;
  Option<std::string> logrotate_filename;
  std::string logrotate_path;
  Option<std::string> user;

  // Only used if the `destination_type` has "fluentbit".
  Option<net::IP> fluentbit_ip;
  int fluentbit_port;
};

} // namespace logger {
} // namespace journald {
} // namespace mesos {

#endif // __JOURNALD_JOURNALD_HPP__

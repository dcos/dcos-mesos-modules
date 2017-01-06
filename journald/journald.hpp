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

const std::string NAME = "mesos-journald-logger";
const std::string CONF_SUFFIX = ".logrotate.conf";
const std::string STATE_SUFFIX = ".logrotate.state";

struct Flags : public virtual flags::FlagsBase
{
  Flags()
  {
    setUsageMessage(
      "Usage: " + NAME + " [options]\n"
      "\n"
      "This command pipes from STDIN to journald.\n"
      "Each line (delineated by newline) of STDIN is labeled with --labels\n"
      "before it is written to journald.  See '--labels'."
      "\n");

    add(&Flags::destination_type,
        "destination_type",
        "Determines where logs should be piped.\n"
        "Valid destinations include: 'journald', 'sandbox', or 'both'.",
        "journald",
        [](const std::string& value) -> Option<Error> {
          if (value != "journald" && value != "sandbox" && value != "both") {
            return Error("Invalid destination type: " + value);
          }

          return None();
        });

    add(&Flags::labels,
        "labels",
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
            return Error("Failed to parse --labels as JSON: " + json.error());
          }

          Try<Labels> _labels = ::protobuf::parse<Labels>(json.get());
          if (_labels.isError()) {
            return Error(
                "Failed to parse --labels as protobuf: " + _labels.error());
          }

          parsed_labels = _labels.get();
          return None();
        });

    add(&Flags::max_size,
        "max_size",
        "Maximum size, in bytes, of a single log file.\n"
        "Defaults to 10 MB.  Must be at least 1 (memory) page.",
        Megabytes(10),
        [](const Bytes& value) -> Option<Error> {
          if (value.bytes() < os::pagesize()) {
            return Error(
                "Expected --max_size of at least " +
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
        "    size <max_size>\n"
        "  }\n"
        "NOTE: The 'size' option will be overriden by this command.");

    add(&Flags::log_filename,
        "log_filename",
        "Absolute path to the leading log file.\n"
        "NOTE: This command will also create two files by appending\n"
        "'" + CONF_SUFFIX + "' and '" + STATE_SUFFIX + "' to the end of\n"
        "'--log_filename'.  These files are used by 'logrotate'.",
        [](const Option<std::string>& value) -> Option<Error> {
          if (value.isNone()) {
            return Error("Missing required option --log_filename");
          }

          if (!path::absolute(value.get())) {
            return Error("Expected --log_filename to be an absolute path");
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
            os::shell(value + " --help > /dev/null");

          if (helpCommand.isError()) {
            return Error(
                "Failed to check logrotate: " + helpCommand.error());
          }

          return None();
        });

    add(&Flags::user,
        "user",
        "The user this command should run as.");
  }

  std::string destination_type;

  Option<std::string> labels;

  // Values populated during validation.
  Labels parsed_labels;

  Bytes max_size;
  Option<std::string> logrotate_options;
  Option<std::string> log_filename;
  std::string logrotate_path;
  Option<std::string> user;
};

} // namespace logger {
} // namespace journald {
} // namespace mesos {

#endif // __JOURNALD_JOURNALD_HPP__

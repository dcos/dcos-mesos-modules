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


namespace mesos {
namespace journald {
namespace logger {

const std::string NAME = "mesos-journald-logger";

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
  }

  Option<std::string> labels;

  // Values populated during validation.
  Labels parsed_labels;
};

} // namespace logger {
} // namespace journald {
} // namespace mesos {

#endif // __JOURNALD_JOURNALD_HPP__

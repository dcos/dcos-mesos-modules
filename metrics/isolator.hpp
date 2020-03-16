#ifndef __METRICS_ISOLATOR_MODULE_HPP__
#define __METRICS_ISOLATOR_MODULE_HPP__

#include <map>
#include <string>
#include <vector>

#include <mesos/mesos.hpp>
#include <mesos/module.hpp>

#include <mesos/slave/isolator.hpp>

#include <process/future.hpp>
#include <process/owned.hpp>
#include <process/process.hpp>

#include <stout/duration.hpp>
#include <stout/flags.hpp>
#include <stout/hashset.hpp>
#include <stout/json.hpp>
#include <stout/option.hpp>
#include <stout/try.hpp>

#include <stout/flags/parse.hpp>

namespace mesosphere {
namespace dcos {
namespace metrics {

// TODO(greggomann): Remove this once we have an equivalent helper in stout.
// See https://reviews.apache.org/r/68543/.
template<typename T>
Try<T> parse(const std::string& body)
{
  Try<JSON::Object> json = JSON::parse<JSON::Object>(body);
  if (json.isError()) {
    return Error("Error parsing input as JSON: " + json.error());
  }

  Try<T> response = ::protobuf::parse<T>(json.get());
  if (response.isError()) {
    return Error(
        "Error parsing JSON into protobuf: " + response.error());
  }

  return response.get();
}

// Forward declaration.
class MetricsIsolatorProcess;

namespace isolator {

struct Flags : virtual flags::FlagsBase
{
  Flags()
  {
    add(&Flags::service_scheme,
        "dcos_metrics_service_scheme",
        "The scheme on which the --dcos_metrics_service_address is reachable.\n"
        "Must be either \"http\" or \"https\".",
        [](const Option<std::string>& value) -> Option<Error> {
          if (value.isNone()) {
            return Error("Missing required option"
                         " --dcos_metrics_service_scheme");
          }

          if (value.get() != "http" && value.get() != "https") {
            return Error("Expected --dcos_metrics_service_scheme"
                         " to be either \"http\" or \"https\"");
          }

          return None();
        });

    add(&Flags::service_network,
        "dcos_metrics_service_network",
        "The network where the --dcos_metrics_service_address is reachable.\n"
#ifndef __WINDOWS__
        "Must be either \"inet\" or \"unix\".",
#else
        "Must be \"inet\" on Windows.",
#endif // __WINDOWS__
        [](const Option<std::string>& value) -> Option<Error> {
          if (value.isNone()) {
            return Error("Missing required option"
                         " --dcos_metrics_service_network");
          }
#ifndef __WINDOWS__
          if (value.get() != "inet" && value.get() != "unix") {
            return Error("Expected --dcos_metrics_service_network"
                         " to be either \"inet\" or \"unix\"");
          }
#else
          if (value.get() != "inet") {
            return Error("Expected --dcos_metrics_service_network"
                         " to be \"inet\"");
          }
#endif // __WINDOWS__
          return None();
        });

    add(&Flags::service_address,
        "dcos_metrics_service_address",
        "The address where the DC/OS metrics service is reachable.",
        [](const Option<std::string>& value) -> Option<Error> {
          if (value.isNone()) {
            return Error("Missing required option"
                         " --dcos_metrics_service_address");
          }

          return None();
        });

    add(&Flags::service_endpoint,
        "dcos_metrics_service_endpoint",
        "The endpoint on the --dcos_metrics_service_address that\n"
        "this module should communicate with.",
        [](const Option<std::string>& value) -> Option<Error> {
          if (value.isNone()) {
            return Error("Missing required option"
                         " --dcos_metrics_service_endpoint");
          }

          return None();
        });

    add(&Flags::request_timeout,
        "request_timeout",
        "The duration after which a request to the metrics service will\n"
        "timeout and be considered failed.",
        Seconds(5));
  }

  // TODO(greggomann): Remove the `Option`s here once we have an overload of
  // `add()` which accepts required flags without a default value and with a
  // validation function.
  Option<std::string> service_scheme;
  Option<std::string> service_network;
  Option<std::string> service_address;
  Option<std::string> service_endpoint;
  Duration request_timeout;
};

} // namespace isolator


class MetricsIsolator : public mesos::slave::Isolator
{
public:
  MetricsIsolator(const isolator::Flags& flags);

  virtual ~MetricsIsolator();

  virtual process::Future<Option<mesos::slave::ContainerLaunchInfo>> prepare(
      const mesos::ContainerID& containerId,
      const mesos::slave::ContainerConfig& containerConfig) override;

  virtual process::Future<Nothing> cleanup(
      const mesos::ContainerID& containerId) override;

  virtual bool supportsNesting() override { return true; }
  virtual bool supportsStandalone() override { return true; }

private:
  process::Owned<MetricsIsolatorProcess> process;
};

} // namespace metrics {
} // namespace dcos {
} // namespace mesosphere {

extern "C" mesos::modules::Module<mesos::slave::Isolator>
  com_mesosphere_dcos_MetricsIsolatorModule;

#endif // __METRICS_ISOLATOR_MODULE_HPP__

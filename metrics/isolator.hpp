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

#include <stout/flags.hpp>
#include <stout/hashset.hpp>
#include <stout/option.hpp>
#include <stout/try.hpp>

namespace mesosphere {
namespace dcos {
namespace metrics {

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
        "Must be either \"inet\" or \"unix\".",
        [](const Option<std::string>& value) -> Option<Error> {
          if (value.isNone()) {
            return Error("Missing required option"
                         " --dcos_metrics_service_network");
          }

          if (value.get() != "inet" && value.get() != "unix") {
            return Error("Expected --dcos_metrics_service_network"
                         " to be either \"inet\" or \"unix\"");
          }

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

    add(&Flags::legacy_state_path_dir,
        "legacy_state_path_dir",
        "The path where the STATSD_UDP_HOST and STATSD_UDP_PORT\n"
        "of a task were persisted by the legacy module.",
        [](const Option<std::string>& value) -> Option<Error> {
          if (value.isNone()) {
            return Error("Missing required option --legacy_state_path_dir");
          }

          if (!path::absolute(value.get())) {
            return Error(
                "Expected --legacy_state_path_dir to be an absolute path");
          }

          return None();
        });
  }

  Option<std::string> service_scheme;
  Option<std::string> service_network;
  Option<std::string> service_address;
  Option<std::string> service_endpoint;
  Option<std::string> legacy_state_path_dir;
};

} // namespace isolator


class MetricsIsolator : public mesos::slave::Isolator
{
public:
  MetricsIsolator(const isolator::Flags& flags);

  virtual ~MetricsIsolator();

  virtual process::Future<Nothing> recover(
      const std::vector<mesos::slave::ContainerState>& states,
      const hashset<mesos::ContainerID>& orphans) override;

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

#endif // __METRICS_ISOLATOR_MODULE_HPP__

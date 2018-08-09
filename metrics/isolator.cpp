#include <map>
#include <string>
#include <vector>

#include <mesos/mesos.hpp>
#include <mesos/module.hpp>

#include <mesos/module/isolator.hpp>

#include <process/future.hpp>
#include <process/process.hpp>

#include <stout/hashset.hpp>
#include <stout/nothing.hpp>
#include <stout/option.hpp>
#include <stout/try.hpp>

#include "isolator.hpp"

#include "metrics/messages.pb.h"

using namespace mesos;
using namespace process;

using std::map;
using std::string;
using std::vector;

using mesos::slave::ContainerLaunchInfo;
using mesos::slave::ContainerState;
using mesos::slave::Isolator;

using mesos::modules::metrics::ContainerStartRequest;
using mesos::modules::metrics::ContainerStartResponse;
using mesos::modules::metrics::ContainerStopRequest;
using mesos::modules::metrics::ContainerStopResponse;
using mesos::modules::metrics::LegacyState;

namespace mesosphere {
namespace dcos {
namespace metrics {

class MetricsIsolatorProcess
  : public process::Process<MetricsIsolatorProcess>
{
public:
  MetricsIsolatorProcess(const isolator::Flags& flags_) : flags(flags_) {}

  virtual Future<Nothing> recover(
      const vector<ContainerState>& states,
      const hashset<ContainerID>& orphans)
  {
    // Search through `flags.legacy_state_path_dir` and compare the
    // containers tracked in there with the vector holding the
    // `ContainerState`. For any containers still active, let the
    // metrics service know about them (via an HTTP request) along
    // with the host/port it should listen on for metrics from them.
    // After receiving a successful ACK, delete the state directory
    // for the container.  For any containers no longer active, delete
    // its state directory immediately. Once we've gone through all
    // the containers, assert that `flags.legacy_state_path_dir` is
    // empty and then remove it.
    return Nothing();
  }

  virtual Future<Option<ContainerLaunchInfo>> prepare(
      const ContainerID& containerId,
      const slave::ContainerConfig& containerConfig)
  {
    // Let the metrics service know about the container being
    // launched via an HTTP request. In the response, grab the
    // STATSD_UDP_HOST and STATSD_UDP_PORT pair being returned and set
    // it in the environment of the `ContainerLaunchInfo` returned
    // from this function. On any errors, return a `Failure()`.
    ContainerLaunchInfo launchInfo;
    return launchInfo;
  }

  virtual Future<Nothing> cleanup(
      const ContainerID& containerId)
  {
    // Let the metrics service know about the container being
    // destroyed via an HTTP request. On any errors, return an
    // `Failure()`.
    return Nothing();
  }

private:
  const isolator::Flags flags;
};


MetricsIsolator::MetricsIsolator(const isolator::Flags& flags)
  : process(new MetricsIsolatorProcess(flags))
{
  spawn(process.get());
}


MetricsIsolator::~MetricsIsolator()
{
  terminate(process.get());
  wait(process.get());
}


Future<Nothing> MetricsIsolator::recover(
      const vector<ContainerState>& states,
      const hashset<ContainerID>& orphans)
{
  return dispatch(process.get(),
                  &MetricsIsolatorProcess::recover,
                  states,
                  orphans);
}


Future<Option<ContainerLaunchInfo>> MetricsIsolator::prepare(
    const ContainerID& containerId,
    const slave::ContainerConfig& containerConfig)
{
  return dispatch(process.get(),
                  &MetricsIsolatorProcess::prepare,
                  containerId,
                  containerConfig);
}


Future<Nothing> MetricsIsolator::cleanup(
    const ContainerID& containerId)
{
  return dispatch(process.get(),
                  &MetricsIsolatorProcess::cleanup,
                  containerId);
}

} // namespace metrics {
} // namespace dcos {
} // namespace mesosphere {

mesos::modules::Module<Isolator>
com_mesosphere_dcos_MetricsIsolatorModule(
    MESOS_MODULE_API_VERSION,
    MESOS_VERSION,
    "Mesosphere",
    "support@mesosphere.com",
    "Metrics Isolator Module.",
    nullptr,
    [](const Parameters& parameters) -> Isolator* {
      // Convert `parameters` into a map.
      map<string, string> values;
      foreach (const Parameter& parameter, parameters.parameter()) {
        values[parameter.key()] = parameter.value();
      }

      mesosphere::dcos::metrics::isolator::Flags flags;

      // Load and validate flags from environment and map.
      Try<flags::Warnings> load = flags.load(values, false, "DCOS_");

      if (load.isError()) {
        LOG(ERROR) << "Failed to parse parameters: " << load.error();
        return nullptr;
      }

      foreach (const flags::Warning& warning, load->warnings) {
        LOG(WARNING) << warning.message;
      }

      return new mesosphere::dcos::metrics::MetricsIsolator(flags);
    });

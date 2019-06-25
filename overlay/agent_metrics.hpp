#ifndef __OVERLAY_AGENT_METRICS_HPP__
#define __OVERLAY_AGENT_METRICS_HPP__

#include <process/metrics/counter.hpp>
#include <process/metrics/push_gauge.hpp>

namespace mesos {
namespace modules {
namespace overlay {
namespace agent {

struct Metrics
{
  Metrics();
  ~Metrics();

  // Indicates that the overlay agent process is in the middle
  // of registering with an overlay master. Under normal
  // cirumstances, it should be one only for a brief period of
  // time after start up.
  process::metrics::PushGauge registering;

  // 1 indicates that it failed to configure overlays.
  process::metrics::PushGauge overlay_config_failed;

  // Counts the number of overlay configuration failures.
  process::metrics::Counter overlay_config_failures;

  // Counts the number of overlays which have no subnets.
  process::metrics::PushGauge overlays_without_subnets;

  // Counts the number any Docker command failed to execute
  // for any reason.
  process::metrics::Counter docker_cmd_failures;

  // Counts the number of UpdateAgentOverlaysMessage messages received.
  // It is an internal metric.
  process::metrics::Counter update_agent_overlays_messages_received;

  // Counts the number of UpdateAgentOverlaysMessage messages dropped
  // for any reason. It is an internal metric.
  process::metrics::Counter update_agent_overlays_messages_dropped;

  // Counts the number of AgentRegisteredMessage messages sent.
  // It is an internal metric.
  process::metrics::Counter agent_registered_messages_sent;

  // Counts the number of AgentRegisteredMessage messages dropped
  // for any reason. It is an internal metric.
  process::metrics::Counter agent_registered_messages_dropped;

  // Counts the number of RegisterAgentMessage messages sent.
  // It is an internal metric.
  process::metrics::Counter register_agent_messages_sent;
};

} // namespace agent {
} // namespace overlay {
} // namespace modules {
} // namespace mesos {


#endif // __OVERLAY_AGENT_METRICS_HPP__

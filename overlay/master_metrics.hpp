#ifndef __OVERLAY_MASTER_METRICS_HPP__
#define __OVERLAY_MASTER_METRICS_HPP__

#include <process/metrics/counter.hpp>
#include <process/metrics/push_gauge.hpp>

namespace mesos {
namespace modules {
namespace overlay {
namespace master {

struct Metrics
{
  Metrics();
  ~Metrics();

  // Indicates that the overlay master process is in the middle
  // of recovering its state. Under normal cirumstances,
  // it should be one only for a brief period of time after
  // start up.
  process::metrics::PushGauge recovering;

  // Counts the number of IPv4 address allocation failures.
  process::metrics::Counter ip_allocation_failures;

  // Counts the number of IPv6 address allocation failures.
  process::metrics::Counter ip6_allocation_failures;

  // Counts the number of IPv4 subnet allocation failures.
  process::metrics::Counter subnet_allocation_failures;

  // Counts the number of IPv6 subnet allocation failures.
  process::metrics::Counter subnet6_allocation_failures;

  // Counts the number of network bridge allocation failures.
  process::metrics::Counter bridge_allocation_failures;

  // Counts the number of RegisterAgentMessage messages received.
  // It is an internal metric.
  process::metrics::Counter register_agent_messages_received;

  // Counts the number of RegisterAgentMessage messages dropped.
  // It is an internal metric.
  process::metrics::Counter register_agent_messages_dropped;

  // Counts the number of UpdateAgentOverlaysMessage messages sent.
  // It is an internal metric.
  process::metrics::Counter update_agent_overlays_messages_sent;

  // Counts the number of AgentRegisteredMessage messages received.
  // It is an internal metric.
  process::metrics::Counter agent_registered_messages_received;

  // Counts the number of AgentRegisteredMessage messages dropped.
  // It is an internal metric.
  process::metrics::Counter agent_registered_messages_dropped;

  // Counts the number of AgentRegisteredAcknowledgement messages sent.
  // It is an internal metric.
  process::metrics::Counter agent_registered_acknowledgements_sent;
};

} // namespace master {
} // namespace overlay {
} // namespace modules {
} // namespace mesos {


#endif // __OVERLAY_MASTER_METRICS_HPP__

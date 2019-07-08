#include "master_metrics.hpp"

#include <process/metrics/metrics.hpp>

namespace mesos {
namespace modules {
namespace overlay {
namespace master {

Metrics::Metrics()
  : recovering("overlay/master/recovering"),

    ip_allocation_failures("overlay/master/ip_allocation_failures"),
    ip6_allocation_failures("overlay/master/ip6_allocation_failures"),
    subnet_allocation_failures("overlay/master/subnet_allocation_failures"),
    subnet6_allocation_failures("overlay/master/subnet6_allocation_failures"),
    bridge_allocation_failures("overlay/master/bridge_allocation_failures"),

    register_agent_messages_received(
      "overlay/master/internal/register_agent_messages_received"),
    register_agent_messages_dropped(
      "overlay/master/internal/register_agent_messages_dropped"),
    update_agent_overlays_messages_sent(
      "overlay/master/internal/update_agent_overlays_messages_sent"),
    agent_registered_messages_received(
      "overlay/master/internal/agent_registered_messages_received"),
    agent_registered_messages_dropped(
      "overlay/master/internal/agent_registered_messages_dropped"),
    agent_registered_acknowledgements_sent(
      "overlay/master/internal/agent_registered_acknowledgements_sent")
{
  process::metrics::add(recovering);

  process::metrics::add(ip_allocation_failures);
  process::metrics::add(ip6_allocation_failures);
  process::metrics::add(subnet_allocation_failures);
  process::metrics::add(subnet6_allocation_failures);
  process::metrics::add(bridge_allocation_failures);

  process::metrics::add(register_agent_messages_received);
  process::metrics::add(register_agent_messages_dropped);
  process::metrics::add(update_agent_overlays_messages_sent);
  process::metrics::add(agent_registered_messages_received);
  process::metrics::add(agent_registered_messages_dropped);
  process::metrics::add(agent_registered_acknowledgements_sent);
}

Metrics::~Metrics()
{
  process::metrics::remove(recovering);

  process::metrics::remove(ip_allocation_failures);
  process::metrics::remove(ip6_allocation_failures);
  process::metrics::remove(subnet_allocation_failures);
  process::metrics::remove(subnet6_allocation_failures);
  process::metrics::remove(bridge_allocation_failures);

  process::metrics::remove(register_agent_messages_received);
  process::metrics::remove(register_agent_messages_dropped);
  process::metrics::remove(update_agent_overlays_messages_sent);
  process::metrics::remove(agent_registered_messages_received);
  process::metrics::remove(agent_registered_messages_dropped);
  process::metrics::remove(agent_registered_acknowledgements_sent);
}

} // namespace master {
} // namespace overlay {
} // namespace modules {
} // namespace mesos {

#include "agent_metrics.hpp"

#include <process/metrics/metrics.hpp>

namespace mesos {
namespace modules {
namespace overlay {
namespace agent {

Metrics::Metrics()
  : registering("overlay/slave/registering"),
    overlay_config_failed("overlay/slave/overlay_config_failed"),
    overlay_config_failures("overlay/slave/overlay_config_failures"),
    overlays_without_subnets("overlay/slave/overlays_without_subnets"),
    docker_cmd_failures("overlay/slave/docker_cmd_failures"),

    update_agent_overlays_messages_received(
      "overlay/slave/internal/update_agent_overlays_messages_received"),
    update_agent_overlays_messages_dropped(
      "overlay/slave/internal/update_agent_overlays_messages_dropped"),
    agent_registered_messages_sent(
      "overlay/slave/internal/agent_registered_messages_sent"),
    agent_registered_messages_dropped(
      "overlay/slave/internal/agent_registered_messages_dropped"),
    register_agent_messages_sent(
      "overlay/slave/internal/register_agent_messages_sent")
{
  process::metrics::add(registering);
  process::metrics::add(overlay_config_failed);
  process::metrics::add(overlay_config_failures);
  process::metrics::add(overlays_without_subnets);
  process::metrics::add(docker_cmd_failures);

  process::metrics::add(update_agent_overlays_messages_received);
  process::metrics::add(update_agent_overlays_messages_dropped);
  process::metrics::add(agent_registered_messages_sent);
  process::metrics::add(agent_registered_messages_dropped);
  process::metrics::add(register_agent_messages_sent);
}

Metrics::~Metrics()
{
  process::metrics::remove(registering);
  process::metrics::remove(overlay_config_failed);
  process::metrics::remove(overlay_config_failures);
  process::metrics::remove(overlays_without_subnets);
  process::metrics::remove(docker_cmd_failures);

  process::metrics::remove(update_agent_overlays_messages_received);
  process::metrics::remove(update_agent_overlays_messages_dropped);
  process::metrics::remove(agent_registered_messages_sent);
  process::metrics::remove(agent_registered_messages_dropped);
  process::metrics::remove(register_agent_messages_sent);
}

} // namespace agent {
} // namespace overlay {
} // namespace modules {
} // namespace mesos {

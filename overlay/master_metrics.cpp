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
    bridge_allocation_failures("overlay/master/bridge_allocation_failures")
{
  process::metrics::add(recovering);
  process::metrics::add(ip_allocation_failures);
  process::metrics::add(ip6_allocation_failures);
  process::metrics::add(subnet_allocation_failures);
  process::metrics::add(subnet6_allocation_failures);
  process::metrics::add(bridge_allocation_failures);
}

Metrics::~Metrics()
{
  process::metrics::remove(recovering);
  process::metrics::remove(ip_allocation_failures);
  process::metrics::remove(ip6_allocation_failures);
  process::metrics::remove(subnet_allocation_failures);
  process::metrics::remove(subnet6_allocation_failures);
  process::metrics::remove(bridge_allocation_failures);
}

} // namespace master {
} // namespace overlay {
} // namespace modules {
} // namespace mesos {

#include "supervisor_metrics.hpp"

#include <process/metrics/metrics.hpp>

namespace mesos {
namespace modules {
namespace overlay {
namespace supervisor {

Metrics::Metrics()
  : process_restarts("overlay/master/process_restarts")
{
  process::metrics::add(process_restarts);
}

Metrics::~Metrics()
{
  process::metrics::remove(process_restarts);
}

} // namespace supervisor {
} // namespace overlay {
} // namespace modules {
} // namespace mesos {

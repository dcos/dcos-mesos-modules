#ifndef __OVERLAY_SUPERVISOR_METRICS_HPP__
#define __OVERLAY_SUPERVISOR_METRICS_HPP__

#include <process/metrics/counter.hpp>
#include <process/metrics/push_gauge.hpp>

namespace mesos {
namespace modules {
namespace overlay {
namespace supervisor {

struct Metrics
{
  Metrics();
  ~Metrics();

  // Indicates the number of restarts of overlay master process
  // that has happened since the Mesos master started up.
  // Under normal circumstances, it should stay zero.
  process::metrics::Counter process_restarts;
};

} // namespace supervisor {
} // namespace overlay {
} // namespace modules {
} // namespace mesos {


#endif // __OVERLAY_SUPERVISOR_METRICS_HPP__

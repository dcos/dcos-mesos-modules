#ifndef __OVERLAY_OVERLAY_HPP__
#define __OVERLAY_OVERLAY_HPP__

// ONLY USEFUL AFTER RUNNING PROTOC.
#include <string>

#include <stout/hashset.hpp>

#include <overlay/overlay.pb.h>

namespace mesos {
namespace modules {
namespace overlay {


constexpr char MESOS_BRIDGE_PREFIX[] = "m-";
constexpr char DOCKER_BRIDGE_PREFIX[] = "d-";
constexpr char MASTER_MANAGER_PROCESS_ID[] = "overlay-master";
constexpr char AGENT_MANAGER_PROCESS_ID[] = "overlay-agent";

const hashset<std::string> RESERVED_NETWORKS = {
  "host",
  "bridge",
  "default",
  "none"};

#define LINUX_NET_DEV_NAME_LENGTH  15
#define MAX_OVERLAY_NAME                                                    \
    ((std::string(DOCKER_BRIDGE_PREFIX).size() >                            \
     std::string(MESOS_BRIDGE_PREFIX).size()) ?                             \
    LINUX_NET_DEV_NAME_LENGTH - std::string(MESOS_BRIDGE_PREFIX).size() :   \
    LINUX_NET_DEV_NAME_LENGTH - std::string(DOCKER_BRIDGE_PREFIX).size())

} // namespace overlay{
} // namespace modules {
} // namespace mesos {

#endif // __OVERLAY_OVERLAY_HPP__

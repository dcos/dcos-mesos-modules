#include <stdio.h>

#include <list>
#include <memory>

#include <stout/check.hpp>
#include <stout/interval.hpp>
#include <stout/json.hpp>
#include <stout/jsonify.hpp>
#include <stout/mac.hpp>
#include <stout/os.hpp>
#include <stout/protobuf.hpp>
#include <stout/stringify.hpp>
#include <stout/try.hpp>

#include <process/collect.hpp>
#include <process/defer.hpp>
#include <process/delay.hpp>
#include <process/future.hpp>
#include <process/help.hpp>
#include <process/http.hpp>
#include <process/io.hpp>
#include <process/process.hpp>
#include <process/protobuf.hpp>
#include <process/subprocess.hpp>

#include <mesos/mesos.hpp>
#include <mesos/module.hpp>
#include <mesos/module/anonymous.hpp>
#include <mesos/state/log.hpp>
#include <mesos/state/protobuf.hpp>
#include <mesos/state/storage.hpp>
#include <mesos/zookeeper/detector.hpp>

#include "master_metrics.hpp"
#include "messages.hpp"
#include "network.hpp"
#include "overlay.hpp"
#include "supervisor.hpp"

namespace http = process::http;

using std::hex;
using std::list;
using std::ostream;
using std::queue;
using std::set;
using std::string;
using std::vector;

using net::MAC;

using process::DESCRIPTION;
using process::HELP;
using process::Owned;
using process::Failure;
using process::Future;
using process::TLDR;
using process::UPID;
using process::USAGE;

using mesos::log::Log;
using mesos::modules::Anonymous;
using mesos::modules::Module;
using mesos::modules::overlay::AgentOverlayInfo;
using mesos::modules::overlay::BackendInfo;
using mesos::modules::overlay::NetworkConfig;
using mesos::modules::overlay::VxLANInfo;
using mesos::modules::overlay::State;
using mesos::modules::overlay::MESOS_ZK;
using mesos::modules::overlay::MESOS_QUORUM;
using mesos::modules::overlay::internal::AgentNetworkConfig;
using mesos::modules::overlay::internal::AgentRegisteredAcknowledgement;
using mesos::modules::overlay::internal::AgentRegisteredMessage;
using mesos::modules::overlay::internal::MasterConfig;
using mesos::modules::overlay::internal::RegisterAgentMessage;
using mesos::modules::overlay::internal::UpdateAgentOverlaysMessage;
using mesos::modules::overlay::supervisor::ProcessSupervisor;
using mesos::Parameters;
using mesos::state::LogStorage;
using mesos::state::protobuf::Variable;
using mesos::state::Storage;

namespace mesos {
namespace modules {
namespace overlay {
namespace master {

constexpr char REPLICATED_LOG_STORE[] = "overlay_replicated_log";
constexpr char REPLICATED_LOG_STORE_KEY[] = "network-state";
constexpr char REPLICATED_LOG_STORE_REPLICAS[] = "overlay_log_replicas";

const string OVERLAY_STATE_ENDPOINT_HELP = HELP(
    TLDR("Allocate overlay network resources for Master."),
    USAGE("/overlay-master/state"),
    DESCRIPTION("Allocate subnets, VTEP IP and the MAC addresses.", "")
);

const string OVERLAY_DELETE_AGENTS_ENDPOINT_HELP = HELP(
    TLDR("Manage Agent records in the overlay state."),
    USAGE("/overlay-master/agents"),
    DESCRIPTION("Purge overlay state records of Agents that are gone.", "")
);

// Helper function to convert std::string to `net::MAC`.
static Try<net::MAC> createMAC(const string& _mac, const bool& oui)
{
  vector<string> tokens = strings::split(_mac, ":");
  if (tokens.size() != 6) {
    return Error(
        "Invalid MAC address. Mac address " +
        _mac + " needs to be in"
        " the format xx:xx:xx:00:00:00");
  }

  uint8_t mac[6];
  for (size_t i = 0; i < tokens.size(); i++) {
    sscanf(tokens[i].c_str(), "%hhx", &mac[i]);
    if (oui) {
      if ( i > 2 && mac[i] != 0) {
        return Error(
            "Invalid OUI MAC address: " + _mac +
            ". Least significant three bytes should not be"
            " set for the OUI");
      }
    }
  }

  return net::MAC(mac);
}


struct Vtep
{
  Vtep(const Network& _network,
       const Option<Network>& _network6,
       const MAC _oui,
       const Option<size_t>& _mtu)
    : network(_network),
      network6(_network6),
      oui(_oui),
      mtu(_mtu)
  {
    freeIP += (
      Bound<IP>::open(network.begin()),
      Bound<IP>::open(network.end()));

    // IPv6
    if (network6.isSome()) {
      freeIP6 +=
        (Bound<IP>::open(network6.get().begin()),
         Bound<IP>::open(network6.get().end()));
    }
  }

  Try<Network> allocateIP()
  {
    if (freeIP.empty()) {
      return Error("Unable to allocate a VTEP IP due to exhaustion");
    }

    IP ip = freeIP.begin()->lower();
    freeIP -= ip;

    return Network(ip, network.prefix());
  }

  Try<Network> allocateIP6()
  {
    if (freeIP6.empty()) {
      return Error("Unable to allocate a VTEP IPv6 due to exhaustion");
    }

    IP ip6 = freeIP6.begin()->lower();
    freeIP6 -= ip6;

    return Network(ip6, network6.get().prefix());
  }

  Try<Nothing> deallocateIP(const Network& ip)
  {
    Try<IP> _ip = IP::convert(ip.address());
    if (_ip.isError()) {
      return Error(_ip.error());
    }

    if (freeIP.contains(_ip.get())) {
      return Error(
        "Unable to deallocate IP: " + stringify(_ip.get()) +
        " that wasn't previously allocated");
    }

    freeIP += _ip.get();
    return Nothing();
  }

  Try<Nothing> deallocateIP6(const Network& ip6)
  {
    Try<IP> _ip = IP::convert(ip6.address());
    if (_ip.isError()) {
      return Error(_ip.error());
    }

    if (freeIP6.contains(_ip.get())) {
      return Error(
        "Unable to deallocate IP: " + stringify(_ip.get()) +
        " that wasn't previously allocated");
    }

    freeIP6 += _ip.get();
    return Nothing();
  }

  Try<Nothing> reserve(const Network& ip)
  {
    uint32_t addr = ntohl(ip.address().in().get().s_addr);
    IP _ip = IP(addr);

    if (!freeIP.contains(_ip)) {
      VLOG(1) << "Current free IPs: " << freeIP;
      return Error(
          "Cannot reserve an unavailable IP: "  + stringify(ip) +
          "(" + stringify(_ip) + ")");
    }

    freeIP -= _ip;

    return Nothing();
  }

  Try<Nothing> reserve6(const Network& ip6)
  {
    in6_addr addr6 = ip6.address().in6().get();
    IP _ip6 = IP(addr6);

    if (!freeIP6.contains(_ip6)) {
      VLOG(1) << "Current free IPv6s: " << freeIP6;
      return Error(
          "Cannot reserve an unavailable IPv6: "  + stringify(ip6) +
          "(" + stringify(_ip6) + ")");
    }

    freeIP6 -= _ip6;

    return Nothing();
  }

  // We generate the VTEP MAC from the IP by taking the least 24 bits
  // of the IP and using the 24 bits as the NIC of the MAC.
  //
  // NOTE: There is a caveat to using this technique to generating the
  // MAC, namely if the CIDR prefix of the IP is less than 8 then we
  // the MAC being generated is not guaranteed to be unique.
  Try<net::MAC> generateMAC(const IP& ip)
  {
    Try<struct in_addr> in = ip.in();

    if (in.isError()) {
      return Error(in.error());
    }

    uint8_t *nic = (uint8_t*)&(in.get().s_addr);

    uint8_t mac[6];

    // Set the OUI.
    mac[0] = oui[0];
    mac[1] = oui[1];
    mac[2] = oui[2];

    // Set the NIC.
    mac[3] = nic[1];
    mac[4] = nic[2];
    mac[5] = nic[3];

    return MAC(mac);
  }

  void reset()
  {
    freeIP = IntervalSet<IP>();

    freeIP +=
      (Bound<IP>::open(network.begin()),
       Bound<IP>::open(network.end()));

    // IPv6
    if (network6.isSome()) {
      freeIP6 = IntervalSet<IP>();

      freeIP6 +=
        (Bound<IP>::open(network6.get().begin()),
         Bound<IP>::open(network6.get().end()));
    }
  }

  // Network allocated to the VTEP.
  Network network;

  Option<Network> network6;

  net::MAC oui;

  Option<size_t> mtu;

  IntervalSet<IP> freeIP;

  IntervalSet<IP> freeIP6;
};

struct Overlay
{
  Overlay(
      const string& _name,
      const Option<Network>& _network,
      const Option<Network>& _network6,
      const Option<uint8_t> _prefix,
      const Option<uint8_t> _prefix6,
      const bool& _enabled)
    : name(_name),
    network(_network),
    network6(_network6),
    prefix(_prefix),
    prefix6(_prefix6),
    enabled(_enabled)
  {
    // IPv4
    if (network.isSome()) {
      uint32_t addr = ntohl(network.get().address().in().get().s_addr);
      uint32_t startMask = ntohl(network.get().netmask().in().get().s_addr);
      uint32_t endMask = 0xffffffff << (32 - prefix.get());
      uint32_t startAddr = addr & startMask;
      uint32_t endAddr = (addr | ~startMask) & endMask;

      Network startSubnet = Network(IP(startAddr), prefix.get());
      Network endSubnet = Network(IP(endAddr), prefix.get());

      LOG(INFO) << name << " IPv4: " << startSubnet << " - " << endSubnet;

      freeNetworks +=
        (Bound<Network>::closed(startSubnet),
         Bound<Network>::closed(endSubnet));
    }

    // IPv6
    if (network6.isSome()) {
      in6_addr startAddr6, endAddr6;
      in6_addr addr6 = network6.get().address().in6().get();
      in6_addr startMask6 = network6.get().netmask().in6().get();
      in6_addr endMask6_ = Network::toMask(prefix6.get(), AF_INET6).in6().get();
      for (int i = 0; i < 16; i++) {
        startAddr6.s6_addr[i] = addr6.s6_addr[i] & startMask6.s6_addr[i];
        endAddr6.s6_addr[i] = addr6.s6_addr[i] | ~startMask6.s6_addr[i];
        endAddr6.s6_addr[i] &= endMask6_.s6_addr[i];
      }

      Network startSubnet6 = Network(IP(startAddr6), prefix6.get());
      Network endSubnet6 = Network(IP(endAddr6), prefix6.get());

      LOG(INFO) << "IPv6: " << startSubnet6 << " - " << endSubnet6;

      freeNetworks6 +=
        (Bound<Network>::closed(startSubnet6),
         Bound<Network>::closed(endSubnet6));
    }
  }

  OverlayInfo getOverlayInfo() const
  {
    OverlayInfo overlay;

    overlay.set_name(name);
    if (network.isSome()) {
      overlay.set_subnet(stringify(network.get()));
      overlay.set_prefix(prefix.get());
    }
    if (network6.isSome()) {
      overlay.set_subnet6(stringify(network6.get()));
      overlay.set_prefix6(prefix6.get());
    }
    overlay.set_enabled(enabled);

    return overlay;
  }

  Try<Network> allocate()
  {
    if (!enabled) {
      return Error("The " + name + "overlay is disabled");
    }

    if (freeNetworks.empty()) {
      return Error("No free subnets available in the " + name + "overlay");
    }

    Network agentSubnet = freeNetworks.begin()->lower();
    freeNetworks -= agentSubnet;

    return agentSubnet;
  }

  Try<Network> allocate6()
  {
    if (!enabled) {
      return Error("The " + name + "overlay is disabled");
    }

    if (freeNetworks6.empty()) {
      return Error("No free IPv6 subnets available in the " + name + "overlay");
    }

    Network agentSubnet6 = freeNetworks6.begin()->lower();
    freeNetworks6 -= agentSubnet6;

    return agentSubnet6;
  }

  Try<Nothing> free(const Network& subnet)
  {
    if (subnet.prefix() != prefix.get()) {
      return Error(
          "Cannot free this network because prefix " +
          stringify(subnet.prefix()) + " does not match Agent prefix " +
          stringify(prefix.get()) + " of the overlay");
    }

    if (subnet.prefix() < network.get().prefix()) {
      return Error(
          "Cannot free this network since it does not belong "
          " to the overlay subnet");
    }

    freeNetworks += Network(subnet.address(), prefix.get());

    return Nothing();
  }

  Try<Nothing> free6(const Network& subnet6)
  {
    if (subnet6.prefix() != prefix6.get()) {
      return Error(
          "Cannot free this IPv6 network because prefix " +
          stringify(subnet6.prefix()) + " does not match Agent prefix " +
          stringify(prefix6.get()) + " of the overlay");
    }

    if (subnet6.prefix() < network6.get().prefix()) {
      return Error(
          "Cannot free this IPv6 network since it does not belong "
          " to the overlay subnet");
    }

    freeNetworks6 += Network(subnet6.address(), prefix6.get());

    return Nothing();
  }

  Try<Nothing> reserve(const Network& subnet)
  {
    uint32_t _mask = ntohl(subnet.netmask().in().get().s_addr);
    uint32_t _address = ntohl(subnet.address().in().get().s_addr);
    Network _subnet = Network(IP(_address & _mask), subnet.prefix());


    if (!freeNetworks.contains(_subnet)) {
      return Error(
          "Unable to reserve unavailable subnet " +
          stringify(subnet) + "(" + stringify(_subnet) + ")");
    }

    freeNetworks -= _subnet;

    return Nothing();
  }

  Try<Nothing> reserve6(const Network& subnet6)
  {
    in6_addr _maskedAddr6;
    in6_addr _addr6 = subnet6.address().in6().get();
    in6_addr _mask6 = subnet6.netmask().in6().get();
    for (int i = 0; i < 16; i++) {
      _maskedAddr6.s6_addr[i] = _addr6.s6_addr[i] & _mask6.s6_addr[i];
    }
    Network _subnet6 = Network(IP(_maskedAddr6), subnet6.prefix());


    if (!freeNetworks6.contains(_subnet6)) {
      return Error(
          "Unable to reserve unavailable IPv6 subnet " +
          stringify(subnet6) + "(" + stringify(_subnet6) + ")");
    }

    freeNetworks6 -= _subnet6;

    return Nothing();
  }

  void reset()
  {
    // Re-initialize `freeNetworks`.
    if (network.isSome()) {
      freeNetworks = IntervalSet<Network>();

      uint32_t addr = ntohl(network.get().address().in().get().s_addr);
      uint32_t startMask = ntohl(network.get().netmask().in().get().s_addr);
      uint32_t endMask = 0xffffffff << (32 - prefix.get());
      uint32_t startAddr = addr & startMask;
      uint32_t endAddr = (addr | ~startMask) & endMask;

      Network startSubnet = Network(IP(startAddr), prefix.get());
      Network endSubnet = Network(IP(endAddr), prefix.get());

      LOG(INFO) << name << " Reset IPv4: " << startSubnet << " - " << endSubnet;

      freeNetworks +=
        (Bound<Network>::open(startSubnet),
         Bound<Network>::open(endSubnet));
    }

    // IPv6
    if (network6.isSome()) {
      freeNetworks6 = IntervalSet<Network>();

      in6_addr startAddr6, endAddr6;
      in6_addr addr6 = network6.get().address().in6().get();
      in6_addr startMask6 = network6.get().netmask().in6().get();
      in6_addr endMask6_ = Network::toMask(prefix6.get(), AF_INET6).in6().get();
      for (int i = 0; i < 16; i++) {
        startAddr6.s6_addr[i] = addr6.s6_addr[i] & startMask6.s6_addr[i];
        endAddr6.s6_addr[i] = addr6.s6_addr[i] | ~startMask6.s6_addr[i];
        endAddr6.s6_addr[i] &= endMask6_.s6_addr[i];
      }

      Network startSubnet6 = Network(IP(startAddr6), prefix6.get());
      Network endSubnet6 = Network(IP(endAddr6), prefix6.get());

      LOG(INFO) << name
                << " Reset IPv6: " << startSubnet6 << " - " << endSubnet6;

      freeNetworks6 +=
        (Bound<Network>::open(startSubnet6),
         Bound<Network>::open(endSubnet6));
    }
  }

  // Canonical name of the network.
  std::string name;

  // Network allocated to this overlay.
  Option<Network> network;

  // IPv6 Network allocated to this overlay
  Option<Network> network6;

  // Prefix length allocated to each agent.
  Option<uint8_t> prefix;

  // IPv6 prefix length allocated to each agent
  Option<uint8_t> prefix6;

  // Free subnets available in this network. The subnets are
  // calcualted using the prefix length set for the agents in
  // `prefix`.
  IntervalSet<Network> freeNetworks;

  // Free IPv6 subnets
  IntervalSet<Network> freeNetworks6;

  // Enalbed/Disabled this overlay network.
  bool enabled;
};


class Agent
{
public:
  static Try<Agent> create(std::shared_ptr<Metrics> metrics,
                           const AgentInfo& agentInfo)
  {
    Try<IP> _ip = IP::parse(agentInfo.ip(), AF_INET);

    if (_ip.isError()) {
      return Error("Unable to create `Agent`: " + _ip.error());
    }

    Option<BackendInfo> backend = None();

    if (agentInfo.overlays_size() > 0) {
      backend = agentInfo.overlays(0).backend();
    }

    Agent agent(metrics, _ip.get(), backend);

    for (int i = 0; i < agentInfo.overlays_size(); i++) {
      agent.addOverlay(agentInfo.overlays(i));
    }

    // We should clear any overlay `State` that might have been set.
    // The `State` will be set once this information is downloaded to
    // the corresponding agent.
    agent.clearOverlaysState();

    return agent;
  }

  Agent(std::shared_ptr<Metrics> metrics, const IP& _ip,
        const Option<BackendInfo>& _backend = None())
    : metrics(metrics),
      backend(_backend),
      ip(_ip) {};
  const IP getIP() const { return ip; };

  void addOverlay(const AgentOverlayInfo& overlay)
  {
    if (overlays.contains(overlay.info().name())) {
      return;
    }

    // Create a new overlay on this agent.
    overlays.emplace(
        overlay.info().name(),
        Owned<AgentOverlayInfo>(new AgentOverlayInfo()));

    overlays[overlay.info().name()]->CopyFrom(overlay);
  }

  bool addOverlays(
      const hashmap<string, Owned<Overlay>>& _overlays,
      const AgentNetworkConfig& networkConfig)
  {
    bool mutated = false;

    if (backend.isNone()) {
      // There is no backend associated with this agent so we can't
      // add overlays to this agent.
      LOG(ERROR) << "No backend available for this agent "
                 << "(" <<  ip
                 << ") hence cannot add any overlays to this agent.";

      return mutated;
    }

    // Walk through all the overlay networks. Allocate a subnet from
    // each overlay to the Agent. Allocate a VTEP IP and MAC for each
    // agent. Queue the message on the Agent. Finally, ask Master to
    // reliably send these messages to the Agent.
    foreachpair (const string& name, const Owned<Overlay>& overlay, _overlays) {
      // Skip if the overlay is present
      if (overlays.contains(name)) {
        continue;
      }

      // Skip if the overlay is disabled
      if (!overlay->enabled) {
        continue;
      }

      AgentOverlayInfo _overlay;
      Option<Network> agentSubnet = None();
      Option<Network> agentSubnet6 = None();

      _overlay.mutable_info()->set_name(name);
      if (overlay->network.isSome()) {
        const std::string& subnet = stringify(overlay->network.get());
        _overlay.mutable_info()->set_subnet(subnet);
        _overlay.mutable_info()->set_prefix(overlay->prefix.get());
      }
      if (overlay->network6.isSome()) {
        const std::string& subnet6 = stringify(overlay->network6.get());
        _overlay.mutable_info()->set_subnet6(subnet6);
        _overlay.mutable_info()->set_prefix6(overlay->prefix6.get());
      }

      if (networkConfig.allocate_subnet()) {
        // IPv4
        if (overlay->network.isSome()) {
          Try<Network> _agentSubnet = overlay->allocate();
          if (_agentSubnet.isError()) {
            ++metrics->subnet_allocation_failures;
            LOG(ERROR) << "Cannot allocate subnet from overlay "
                       << name << " to Agent " << ip << ":"
                       << _agentSubnet.error();
            continue;
          }

          agentSubnet = _agentSubnet.get();
          _overlay.set_subnet(stringify(agentSubnet.get()));
        }

        // IPv6
        if (overlay->network6.isSome()) {
          Try<Network> _agentSubnet6 = overlay->allocate6();
          if (_agentSubnet6.isError()) {
            ++metrics->subnet6_allocation_failures;
            LOG(ERROR) << "Cannot allocate IPv6 subnet from overlay "
                       << name << " to Agent " << ip << ":"
                       << _agentSubnet6.error();
            continue;
          }

          agentSubnet6 = _agentSubnet6.get();
          _overlay.set_subnet6(stringify(agentSubnet6.get()));
        }

        // Allocate bridges for Mesos and Docker.
        Try<Nothing> bridges = allocateBridges(
            &_overlay,
            networkConfig);

        if (bridges.isError()) {
          ++metrics->bridge_allocation_failures;
          LOG(ERROR) << "Unable to allocate bridge for network "
                     << name << ": " << bridges.error();

          if (agentSubnet.isSome()) {
            overlay->free(agentSubnet.get());
          }

          // IPv6
          if (agentSubnet6.isSome()) {
            overlay->free6(agentSubnet6.get());
          }
          continue;
        }
      }

      // By the time we reach here it is guaranteed that the
      // `BackendInfo` has already been set for this agent.
      _overlay.mutable_backend()->CopyFrom(backend.get());

      addOverlay(_overlay);

      // `addOverlay` is guaranteed to add this overlay since we have
      // already checked at the beginning of this loop for the absence
      // of this overlay on this agent.
      mutated = true;
    }

    return mutated;
  }

  list<AgentOverlayInfo> getOverlays() const
  {
    list<AgentOverlayInfo> _overlays;

    foreachvalue (const Owned<AgentOverlayInfo>& overlay, overlays) {
      _overlays.push_back(*overlay);
    }

    return _overlays;
  }

  void clearOverlaysState()
  {
    foreachvalue (Owned<AgentOverlayInfo>& overlay, overlays) {
      overlay->clear_state();
    }
  }

  BackendInfo getBackendInfo() const
  {
    return backend.get();
  }

  void setBackendInfo(Option<BackendInfo> backendInfo)
  {
      if (backendInfo.isSome()) {
          backend = backendInfo.get();
      }
  }

  AgentInfo getAgentInfo() const
  {
    AgentInfo info;

    info.set_ip(stringify(ip));

    foreachvalue(const Owned<AgentOverlayInfo>& overlay, overlays) {
      info.add_overlays()->CopyFrom(*overlay);
    }

    return info;
  }

  void updateOverlayState(const AgentOverlayInfo& overlay)
  {
    const string name = overlay.info().name();
    if (!overlays.contains(name)) {
      LOG(ERROR) << "Got update for unknown network "
                 << overlay.info().name();
    }

    overlays.at(name)->mutable_state()->set_status(overlay.state().status());
  }

private:
  Try<Nothing> allocateBridges(
      AgentOverlayInfo* _overlay,
      const AgentNetworkConfig& networkConfig)
  {
    if (!networkConfig.mesos_bridge() &&
        !networkConfig.docker_bridge()) {
      return Nothing();
    }
    const string name = _overlay->info().name();

    // IPv4
    Option<Network> _network = None();
    if (_overlay->has_subnet()) {
      Try<Network> network = Network::parse(
          _overlay->subnet(),
          AF_INET);

      if (network.isError()) {
        return Error(
            "Unable to parse the subnet of the network '" +
            name + "' : " + network.error());
      }

      _network = Network(network->address(), network->prefix() + 1);
    }

    // IPv6
    Option<Network> _network6 = None();
    if (_overlay->has_subnet6()) {
      Try<Network> network6 = Network::parse(
          _overlay->subnet6(),
          AF_INET6);

      if (network6.isError()) {
        return Error("Unable to parse the IPv6 subnet of the network '" +
            name + "' : " + network6.error());
      }

      _network6 = Network(network6->address(), network6->prefix() + 1);
    }

    // Create the Mesos bridge.
    if (networkConfig.mesos_bridge()) {
      BridgeInfo mesosBridgeInfo;
      if (_network.isSome()) {
        mesosBridgeInfo.set_ip(stringify(_network.get()));
      }
      if (_network6.isSome()) {
        mesosBridgeInfo.set_ip6(stringify(_network6.get()));
      }
      mesosBridgeInfo.set_name(MESOS_BRIDGE_PREFIX + name);
      _overlay->mutable_mesos_bridge()->CopyFrom(mesosBridgeInfo);
    }

    // Create the docker bridge.
    if (networkConfig.docker_bridge()) {
      BridgeInfo dockerBridgeInfo;
      if (_network.isSome()) {
        dockerBridgeInfo.set_ip(stringify(++(_network.get())));
      }
      if (_network6.isSome()) {
        dockerBridgeInfo.set_ip6(stringify(++(_network6.get())));
      }
      dockerBridgeInfo.set_name(DOCKER_BRIDGE_PREFIX + name);
      _overlay->mutable_docker_bridge()->CopyFrom(dockerBridgeInfo);
    }

    return Nothing();
  }

  std::shared_ptr<Metrics> metrics;

  // Currently all overlays on an agent share a single backend.
  //
  // TODO(asridharan): When we introduce support for a per-overlay
  // backend, we should deprecate this field.
  Option<BackendInfo> backend;

  // A list of all overlay networks that reside on this agent.
  hashmap<string, Owned<AgentOverlayInfo>> overlays;

  IP ip;
};

// Defines an operation that can be performed on a `State` object for
// a given `AgentInfo`. Every operation returns a `Future<bool>` that
// will be set once the operation is actually performed on the `State`
// object. The operation is usually considered "performed" when the
// mutated `State` is checkpointed to some storage (usually a
// replicated log).
class Operation : public process::Promise<bool> {
public:
  Operation() : success(false) {}

  virtual ~Operation() {}

  Try<bool> operator()(
      State* networkState,
      hashmap<IP, Agent>* agents)
  {
    const Try<bool> result = perform(networkState, agents);

    success = !result.isError();

    return result;
  }

  virtual const string description() const = 0;

  // Sets the promise based on whether the operation was successful.
  bool set() { return process::Promise<bool>::set(success); }

protected:
  virtual Try<bool> perform(
      State* networkState,
      hashmap<IP, Agent>* agents) = 0;

private:
  bool success;
};


// Add an `AgentInfo` to a `State` object.
class AddAgent : public Operation {
public:
  explicit AddAgent(const AgentInfo& _agentInfo)
  {
    agentInfo.CopyFrom(_agentInfo);
  }

  const std::string description() const
  {
    return "Add operation for agent: " + agentInfo.ip();
  }

protected:
  Try<bool> perform(State* networkState, hashmap<IP, Agent>* agents)
  {
    // Make sure the Agent we are going to add is already present in `agents`.
    Try<IP> agentIP = IP::parse(agentInfo.ip(), AF_INET);
    if (agentIP.isError()) {
      return Error("Unable to parse the Agent IP: " + agentIP.error());
    }

    if (!agents->contains(agentIP.get())) {
      return Error(
          "Could not find the Agent (" + stringify(agentIP.get()) +
          ") that needed to be added to `State`.");
    }

    networkState->add_agents()->CopyFrom(agentInfo);
    return true;
  }

private:
  AgentInfo agentInfo;
};


// Modify an `AgentInfo` in a `State` object.
class ModifyAgent : public Operation {
public:
  explicit ModifyAgent(const AgentInfo& _agentInfo)
  {
    agentInfo.CopyFrom(_agentInfo);
  }

  const std::string description() const
  {
    return "Modify operation for agent: " + agentInfo.ip();
  }

protected:
  Try<bool> perform(State* networkState, hashmap<IP, Agent>* agents)
  {
    // Make sure the Agent we are going to add is already present in `agents`.
    Try<IP> agentIP = IP::parse(agentInfo.ip(), AF_INET);
    if (agentIP.isError()) {
      return Error("Unable to parse the Agent IP: " + agentIP.error());
    }

    if (!agents->contains(agentIP.get())) {
      return Error(
          "Could not find the Agent (" + stringify(agentIP.get()) +
          ") in the `agents` cache, that needed to be added to `State`.");
    }

    for(int i = 0; i < networkState->agents_size(); i++) {
      AgentInfo* agentInfo_ = networkState->mutable_agents(i);
      if (agentInfo_->ip() == agentInfo.ip()) {
        agentInfo_->CopyFrom(agentInfo);
        return true;
      }
    }

    return Error(
        "Could not find the Agent (" + stringify(agentIP.get()) +
        ") in `State`.");
  }

private:
  AgentInfo agentInfo;
};


class DeleteAgents : public Operation {
public:
  explicit DeleteAgents(const hashset<string>& _agentIPs)
  {
    agentIPs = _agentIPs;
  }

  const string description() const
  {
    return "Drop agents operation";
  }

protected:
  Try<bool> perform(State *networkState, hashmap<IP, Agent>* agents)
  {
    LOG(INFO) << "Deleting agents from state";

    vector<int> agentsToDelete;
    int agentsSize = networkState->agents_size();
    for (int i = agentsSize - 1; i >= 0; --i) {
      const AgentInfo& agent = networkState->agents(i);
      if (agentIPs.count(agent.ip()) > 0) {
        LOG(INFO) << "Deleting agent from state: " << agent.ip();
        agentsToDelete.push_back(i);
      }
    }

    google::protobuf::RepeatedPtrField<AgentInfo>* stateAgents =
      networkState->mutable_agents();
    foreach (int index, agentsToDelete) {
      stateAgents->DeleteSubrange(index, 1);
    }

    return true;
  }

private:
  hashset<string> agentIPs;
};


inline ostream& operator<<(ostream& stream, const Operation& operation)
{
  return stream << operation.description();
}


// `ManagerProcess` is responsible for managing all the overlays that
// exist in the Mesos cluster. For each overlay the manager stores the
// network associated with overlay and the prefix length of subnets
// that need to be assigned to Agents. When an Agent registers with
// the manager, the manager picks a network from each overlay that the
// manager is aware off and assigns it to the Agent. When the Agent
// de-registers (or goes away) the manager frees subnets allocated to
// the Agent for all the overlays that existed on that Agent.
class ManagerProcess : public ProtobufProcess<ManagerProcess>
{
public:
  static Try<Owned<ManagerProcess>> createManagerProcess(
      const MasterConfig& masterConfig)
  {
    NetworkConfig networkConfig;
    networkConfig.CopyFrom(masterConfig.network());

    Try<Network> vtepSubnet =
      Network::parse(networkConfig.vtep_subnet(), AF_INET);

    if (vtepSubnet.isError()) {
      return Error(
          "Unable to parse the VTEP Subnet: " + vtepSubnet.error());
    }

    // Make sure the VTEP subnet CIDR is not less than /8
    if (vtepSubnet.get().prefix() < 8) {
      return Error(
          "VTEP MAC are derived from last 24 bits of VTEP IP.  Hence, "
          "in order to guarantee unique VTEP MAC we need the VTEP IP "
          "subnet to be equal to or greater than /8");
    }

    Try<net::MAC> vtepMACOUI = createMAC(networkConfig.vtep_mac_oui(), true);
    if(vtepMACOUI.isError()) {
      return Error(
          "Unable to parse VTEP MAC OUI: " + vtepMACOUI.error());
    }

    // IPv6
    Option<Network> vtepSubnet6 = None();
    if (networkConfig.has_vtep_subnet6()) {
      Try<Network> _vtepSubnet6 =
        Network::parse(networkConfig.vtep_subnet6(), AF_INET6);

      if (_vtepSubnet6.isError()) {
        return Error(
            "Unable to parse the VTEP IPv6 Subnet: " + _vtepSubnet6.error());
      }
      vtepSubnet6 = _vtepSubnet6.get();
    }

    // MTU for VTEP
    Option<size_t> vtepMTU = None();
    if (networkConfig.has_vtep_mtu()) {
      vtepMTU = networkConfig.vtep_mtu();
    }

    hashmap<string, Owned<Overlay>> overlays;
    IntervalSet<IP> addressSpace;

    // Overlay networks cannot have overlapping IP addresses. This
    // lambda keeps track of the current address space and returns an
    // `Error` if it detects an overlay that is going to use an
    // already configured address space.
    auto updateAddressSpace =
      [&addressSpace](const Network &network) -> Try<Nothing> {
        Interval<IP> overlaySpace =
          (Bound<IP>::closed(network.begin()),
           Bound<IP>::closed(network.end()));

        if (addressSpace.intersects(overlaySpace)) {
          return Error("Found overlapping address spaces");
        }

        addressSpace += overlaySpace;

        return Nothing();
    };

    for (int i = 0; i < networkConfig.overlays_size(); i++) {
      OverlayInfo overlay = networkConfig.overlays(i);

      if (overlays.contains(overlay.name())) {
        return Error(
            "Duplicate overlay configuration detected for overlay: " +
            overlay.name());
      }

      if (RESERVED_NETWORKS.contains(overlay.name())) {
        return Error(
          "Overlay network name: " + overlay.name() +
          " is a reserved network name");
      }

      // The overlay name is used to derive the Mesos bridge and
      // Docker bridge names. Since, in Linux, network device names
      // cannot exceed 15 characters, we need to impose the limit on
      // the overlay network name.
      if (overlay.name().size() > MAX_OVERLAY_NAME) {
        return Error(
            "Overlay name: " + overlay.name() +
            " too long cannot, exceed " + stringify(MAX_OVERLAY_NAME) +
            "  characters    IntervalSet<uint8_t> ");
      }

      LOG(INFO) << "Configuring overlay network:" << overlay.name();

      // IPv4
      Option<uint8_t> prefix = None();
      Option<Network> address = None();
      if (overlay.has_subnet()) {
        Try<Network> _address =
          Network::parse(overlay.subnet(), AF_INET);

        if (_address.isError()) {
          return Error(
              "Unable to determine subnet for network: " +
              _address.error());
        }

        Try<Nothing> valid = updateAddressSpace(_address.get());

        if (valid.isError()) {
          return Error(
              "Incorrect address space for the overlay network '" +
              overlay.name() + "': " + valid.error());
        }

        address = _address.get();
        prefix = overlay.prefix();
      }

      // IPv6
      Option<uint8_t> prefix6 = None();
      Option<Network> address6 = None();
      if (overlay.has_subnet6()) {
        Try<Network> _address6 =
          Network::parse(overlay.subnet6(), AF_INET6);

        if (_address6.isError()) {
          return Error(
             "Unable to determine IPv6 subnet for network: " +
              _address6.error());
        }

        Try<Nothing> valid6 = updateAddressSpace(_address6.get());

        if (valid6.isError()) {
          return Error(
              "Incorrect IPv6 address space for the overlay network '" +
              overlay.name() + "': " + valid6.error());
        }

        address6 = _address6.get();
        prefix6 = overlay.prefix6();
      }

      overlays.emplace(
          overlay.name(),
          Owned<Overlay>(new Overlay(
            overlay.name(),
            address,
            address6,
            prefix,
            prefix6,
            overlay.enabled())));
    }

    if (overlays.empty()) {
      return Error(
          "Could not find any overlay configuration. Specify at"
          " least one overlay");
    }

    Storage* storage = nullptr;
    Log* log = nullptr;

    // Check if we need to create the replicated log.
    if (masterConfig.has_replicated_log_dir()) {
      Try<Nothing> mkdir = os::mkdir(masterConfig.replicated_log_dir());
      if (mkdir.isError()) {
        return Error(
            "Unable to create replicated log directory: " +
            mkdir.error());
      }

      LOG(INFO) << "Initializing the replicated log.";

      // The ZK URL and Quorum can be specified through the JSON
      // config or through environment variables.
      Option<string> zkURL = None();
      if (masterConfig.has_zk() && masterConfig.zk().has_url()) {
           zkURL = masterConfig.zk().url();
      } else {
        zkURL = os::getenv(MESOS_ZK);
      }

      Option<uint32_t> quorum = None();
      if (masterConfig.has_zk() && masterConfig.zk().has_quorum()) {
          quorum = masterConfig.zk().quorum();
      } else {
        Option<string> _quorum = os::getenv(MESOS_QUORUM);
        if (_quorum.isSome()) {
          Try<uint32_t> result = numify<uint32_t>(_quorum.get());
          if (result.isError()) {
            return Error(
                "Error parsing environment variable 'MESOS_QUORUM'(" +
                _quorum.get() + "): " + result.error());
          }

          quorum = result.get();
        }
      }

      if (zkURL.isSome() && quorum.isNone()) {
        return Error(
            "Cannot use replicated log with ZK URL '" + zkURL.get() +
            "' without a quorum. Please specify quorum to be "
            "used with zookeeper");
      }

      if (zkURL.isSome() && quorum.isSome()) {
        LOG(INFO)
          << "Using replicated log with zookeeper URL "
          << zkURL.get() << " with a quorum of " << quorum.get()
          << " for leader election.";

        if (strings::startsWith(zkURL.get(), "file://")) {
          const string path = zkURL.get().substr(7);

          Try<std::string> read = os::read(path);

          if (read.isError()) {
            return Error("Error reading zookeeper configuration file '" +
                         path + "': " + read.error());
          }

          zkURL = read.get();
        }

        Try<zookeeper::URL> url = zookeeper::URL::parse(zkURL.get());
        if (url.isError()) {
          return Error("Error parsing ZooKeeper URL: " + url.error());
        }

        log = new Log(
            (int)quorum.get(),
            path::join(
              masterConfig.replicated_log_dir(),
              REPLICATED_LOG_STORE),
            url.get().servers,
            Seconds(masterConfig.zk().session_timeout()),
            path::join(url.get().path, REPLICATED_LOG_STORE_REPLICAS),
            url.get().authentication,
            true,
            "overlay/master/");
      } else {
        // Use replicated log without ZooKeeper.
        LOG(INFO)
          << "Using replicated log with local storage with a quorum of one.";

        log = new Log(
            1,
            path::join(
              masterConfig.replicated_log_dir(),
              REPLICATED_LOG_STORE),
            set<UPID>(),
            true,
            "overlay/master/");
      }

      storage = new LogStorage(log);
    }

    Owned<mesos::state::protobuf::State> replicatedLog;

    if (storage != nullptr) {
      replicatedLog = Owned<mesos::state::protobuf::State>(
          new mesos::state::protobuf::State(storage));
    }

    // Parse replicatedLogTimeout.
    Try<Duration> replicatedLogTimeout = Minutes(5);
    if (masterConfig.has_replicated_log_timeout()) {
      replicatedLogTimeout =
        Duration::parse(masterConfig.replicated_log_timeout());
      if (replicatedLogTimeout.isError()) {
        return Error("Error parsing replicatedLogTimeout: " +
                     replicatedLogTimeout.error());
      }
    }

    return Owned<ManagerProcess>(new ManagerProcess(
          overlays,
          vtepSubnet.get(),
          vtepSubnet6,
          vtepMACOUI.get(),
          vtepMTU,
          networkConfig,
          replicatedLog,
          replicatedLogTimeout.get(),
          storage,
          log));
  }

  ~ManagerProcess()
  {
    LOG(INFO) << "Shutting down the master manager process...";

    replicatedLog.reset();

    if (storage != nullptr)  {
      delete storage;
    }

    if (log != nullptr)  {
      delete log;
    }
  }


protected:
  virtual void initialize()
  {
    LOG(INFO) << "Adding route for '" << self().id << "/state'";
    route("/state",
          OVERLAY_STATE_ENDPOINT_HELP,
          &ManagerProcess::stateEndpoint);

    LOG(INFO) << "Adding route for '" << self().id << "/agents'";
    route("/agents/delete",
          OVERLAY_DELETE_AGENTS_ENDPOINT_HELP,
          &ManagerProcess::deleteAgentsEndpoint);

    // When a new agent comes up or an existing agent reconnects with
    // the master, it'll first send a `RegisterAgentMessage` to the
    // master. The master will reply with `UpdateAgentNetworkMessage`.
    install<RegisterAgentMessage>(&ManagerProcess::registerAgent);

    // When the agent finishes its configuration based on the content
    // in `UpdateAgentNetworkMessage`, it'll reply the master with an
    // `AgentRegisteredMessage`.
    // TODO(jieyu): Master should retry `UpdateAgentNetworkMessage` in
    // case the message gets dropped.
    install<AgentRegisteredMessage>(&ManagerProcess::agentRegistered);
  }

  void registerAgent(
      const UPID& pid,
      const RegisterAgentMessage& registerMessage)
  {
    LOG(INFO) << "Got registration from pid: " << pid;
    ++metrics->register_agent_messages_received;

    if (replicatedLog.get() != nullptr) {
      if (storedState.isNone() && !recovering) {
        // We haven't started recovering.
        LOG(INFO) << MASTER_MANAGER_PROCESS_ID << " moving to `RECOVERING`"
          << " state . Hence, not sending an update to agent"
          << pid;
        ++metrics->register_agent_messages_dropped;
        recover();
        return;
      } else if (storedState.isNone() && recovering) {
        // We are recovering. Drop the message, the agent will try to
        // re-register.
        LOG(INFO) << MASTER_MANAGER_PROCESS_ID << " in `RECOVERING`"
          << " state . Hence, not sending an update to agent"
          << pid;
        ++metrics->register_agent_messages_dropped;
        return;
      } // else -> `storedState.isSome` , we have recovered.
    }

    // Recovery complete.
    Try<IP> agentIP = IP::convert(pid.address.ip);
    if (agentIP.isError()) {
      ++metrics->register_agent_messages_dropped;
      LOG(ERROR) << "Couldn't parse agent ip "
                 << agentIP.error();
      return;
    }

    if (agents.contains(agentIP.get())) {
      LOG(INFO) << "Agent " << pid << " re-registering.";

      Agent* agent = &(agents.at(agentIP.get()));
      // Check if IPv6 address is added to a vtep interface
      // and it is not present on the agent vtep
      BackendInfo backendInfo = agent->getBackendInfo();
      if (vtep.network6.isSome() &&
           !backendInfo.vxlan().has_vtep_ip6()) {
        Try<Network> vtepIP6 = vtep.allocateIP6();
        if (vtepIP6.isError()) {
          ++metrics->register_agent_messages_dropped;
          ++metrics->ip6_allocation_failures;
          LOG(ERROR)
            << "Unable to get VTEP IPv6 for Agent: " << vtepIP6.error()
            << "Cannot fulfill re-registration for Agent: " << pid;
          return;
        }

        backendInfo.mutable_vxlan()->set_vtep_ip6(stringify(vtepIP6.get()));
        agent->setBackendInfo(backendInfo);
      }

      // Check if any new overlay need to be installed on the
      // agent.
      if (agent->addOverlays(overlays, registerMessage.network_config())) {
        // We installed a new overlay on this agent.
        update(Owned<Operation>(
               new ModifyAgent(agent->getAgentInfo())))
          .onAny(defer(self(),
                 &ManagerProcess::_registerAgent,
                 pid,
                 agentIP.get(),
                 lambda::_1));

        return;
      }

      // Ensure that the agent is added to the replicated log.
      const string _agentIP = stringify(agentIP.get());
      for (int i = 0; i < networkState.agents_size(); i++) {
        if (_agentIP == networkState.agents(i).ip()) {
          // Given that `networkState` already has this Agent, the
          // information is already stored in replicated log and hence
          // we can just send an "ACK" to the agent with the
          // configuration info
          _registerAgent(pid, agentIP.get(), true);
          return;
        }
      }

      // The fact that we have reached here implies that the Agent
      // exists in the `agents` database, but its information has not
      // been updated in the replicated log. Simply drop this message
      // since the Agent will re-register.
      LOG(INFO) << "Agent " << pid
                << " info has not been updated in the replicated log."
                << " Hence dropping this registration request.";
      ++metrics->register_agent_messages_dropped;
      return;
    } else {
      // New Agent.
      LOG(INFO) << "New registration from pid: " << pid;

      Try<Network> vtepIP = vtep.allocateIP();
      if (vtepIP.isError()) {
        ++metrics->register_agent_messages_dropped;
        ++metrics->ip_allocation_failures;
        LOG(ERROR)
          << "Unable to get VTEP IP for Agent: " << vtepIP.error()
          << "Cannot fulfill registration for Agent: " << pid;
        return;
      }
      LOG(INFO) << "Allocated VTEP IP : " << vtepIP.get();

      // IPv6
      Option<Network> vtepIP6 = None();
      if (vtep.network6.isSome()) {
        Try<Network> _vtepIP6 = vtep.allocateIP6();
        if (_vtepIP6.isError()) {
          ++metrics->register_agent_messages_dropped;
          ++metrics->ip6_allocation_failures;
          LOG(ERROR)
           << "Unable to get VTEP IPv6 for Agent: " << _vtepIP6.error()
           << "Cannot fulfill registration for Agent: " << pid;
          return;
        }

        vtepIP6 = _vtepIP6.get();
        LOG(INFO) << "Allocated VTEP IPv6 : " << vtepIP6.get();
      }

      Try<IP> _vtepIP = IP::convert(vtepIP.get().address());
      if (_vtepIP.isError()) {
        ++metrics->register_agent_messages_dropped;
        LOG(ERROR) << "Couldn't parse vtep IP " << _vtepIP.error()
                   << "Cannot fulfill registration for Agent: " << pid;
        return;
      }

      Try<net::MAC> vtepMAC = vtep.generateMAC(_vtepIP.get());
      if (vtepMAC.isError()) {
        ++metrics->register_agent_messages_dropped;
        LOG(ERROR)
          << "Unable to get VTEP MAC for Agent: " << vtepMAC.error()
          << "Cannot fulfill registration for Agent: " << pid;
          return;
      }
      VLOG(1) << "Allocated VTEP MAC : " << vtepMAC.get();

      VxLANInfo vxlan;
      vxlan.set_vni(1024);
      vxlan.set_vtep_name("vtep1024");
      vxlan.set_vtep_ip(stringify(vtepIP.get()));
      vxlan.set_vtep_mac(stringify(vtepMAC.get()));

      if (vtepIP6.isSome()) {
        vxlan.set_vtep_ip6(stringify(vtepIP6.get()));
      }

      if (vtep.mtu.isSome()) {
        vxlan.set_vtep_mtu(vtep.mtu.get());
      }

      BackendInfo backend;
      backend.mutable_vxlan()->CopyFrom(vxlan);

      agents.emplace(agentIP.get(), Agent(metrics, agentIP.get(), backend));

      Agent* agent = &(agents.at(agentIP.get()));

      agent->addOverlays(overlays, registerMessage.network_config());

      // Update the `networkState in the replicated log before
      // sending the overlay configuration to the Agent.
      update(Owned<Operation>(
            new AddAgent(agent->getAgentInfo())))
        .onAny(defer(self(),
              &ManagerProcess::_registerAgent,
              pid,
              agentIP.get(),
              lambda::_1));
      return;
    }

    UNREACHABLE();
  }

  // Will be called once the operation is successfully applied to the
  // `networkState`.
  void _registerAgent(const UPID& pid,
                      const IP& agentIP,
                      const Future<bool>& result)
  {
    if (!result.isReady()) {
      ++metrics->register_agent_messages_dropped;
      LOG(WARNING) << "Unable to process registration request from "
                   << pid << " due to: "
                   << (result.isDiscarded() ? "discarded" : result.failure());
      return;
    }

    CHECK(agents.contains(agentIP));

    list<AgentOverlayInfo> _overlays = agents.at(agentIP).getOverlays();

    // Create the network update message and send it to the Agent.
    UpdateAgentOverlaysMessage update;

    foreach(const AgentOverlayInfo& overlay, _overlays) {
      update.add_overlays()->CopyFrom(overlay);
    }

    // Clear the state for all overlays in this update.
    for (int i = 0; i < update.overlays_size(); i++) {
      update.mutable_overlays(i)->clear_state();
    }

    send(pid, update);
    ++metrics->update_agent_overlays_messages_sent;
  }

  Try<Nothing> deleteAgents(const hashset<IP> agentIPs)
  {
    foreach (const IP& ip, agentIPs) {
      LOG(INFO) << "Deleting agent from in-memory data structures: "
                << stringify(ip);
      if (!agents.contains(ip)) {
        LOG(INFO) << "Unable to find an agent with IP: " + stringify(ip);
        continue;
      }

      const Agent* agent = &(agents.at(ip));
      hashset<string> vtepIPs, vtepIP6s;

      foreach (const AgentOverlayInfo& agentOverlay, agent->getOverlays()) {
        string overlayName = agentOverlay.info().name();
        Owned<Overlay> overlay = overlays.at(overlayName);

        if (agentOverlay.has_subnet()) {
          Try<Network> subnet =
            Network::parse(agentOverlay.subnet(), AF_INET);
          if (subnet.isError()) {
            return Error(subnet.error());
          }
          Try<Nothing> result = overlay->free(subnet.get());
          if (result.isError()) {
            return Error(result.error());
          }
        }

        if (agentOverlay.has_subnet6()) {
          Try<Network> subnet6 =
            Network::parse(agentOverlay.subnet6(), AF_INET6);
          if (subnet6.isError()) {
            return Error(subnet6.error());
          }
          Try<Nothing> result = overlay->free6(subnet6.get());
          if (result.isError()) {
            return Error(result.error());
          }
        }

        if (agentOverlay.backend().has_vxlan()) {
          vtepIPs.insert(agentOverlay.backend().vxlan().vtep_ip());

          if (agentOverlay.backend().vxlan().has_vtep_ip6()) {
            vtepIP6s.insert(agentOverlay.backend().vxlan().vtep_ip6());
          }
        }
      }

      foreach (const string s, vtepIPs) {
        Try<Network> ip = Network::parse(s, AF_INET);
        if (ip.isError()) {
          return Error(ip.error());
        }
        Try<Nothing> result = vtep.deallocateIP(ip.get());
        if (result.isError()) {
          return Error(result.error());
        }
      }

      foreach (const string s, vtepIP6s) {
        Try<Network> ip6 = Network::parse(s, AF_INET6);
        if (ip6.isError()) {
          return Error(ip6.error());
        }
        Try<Nothing> result = vtep.deallocateIP6(ip6.get());
        if (result.isError()) {
          return Error(result.error());
        }
      }

      agents.erase(ip);
      LOG(INFO) << "Deleted agent from in-memory data structures: "
                << stringify(ip);
    }

    return Nothing();
  }

  void agentRegistered(const UPID& from, const AgentRegisteredMessage& message)
  {
    ++metrics->agent_registered_messages_received;

    Try<IP> _agentIP = IP::convert(from.address.ip);
    if (_agentIP.isError()) {
      ++metrics->agent_registered_messages_dropped;
      LOG(ERROR) << "Couldn't parse agent IP " << _agentIP.error()
                 << " Got ACK from " << from;
      return;
    }

    if(agents.contains(_agentIP.get())) {
      LOG(INFO) << "Got ACK for addition of networks from " << from;
      for(int i = 0; i < message.overlays_size(); i++) {
        agents.at(_agentIP.get()).updateOverlayState(message.overlays(i));
      }

      // We don't need to store the "state" of an overlay network on
      // an agent in the replicated log so go ahead and update the
      // `networkState` without updating the overlay replicated log.
      for (int i = 0; i < networkState.agents_size(); i++) {
        if (stringify(_agentIP.get()) == networkState.agents(i).ip()) {
          networkState.mutable_agents(i)->CopyFrom(
              agents.at(_agentIP.get()).getAgentInfo());

          LOG(INFO) << "Sending register ACK to: " << from;
          send(from, AgentRegisteredAcknowledgement());
          ++metrics->agent_registered_acknowledgements_sent;
          return;
        }
      }
      ++metrics->agent_registered_messages_dropped;
      LOG(ERROR) << "Unable to find the registered agent in the `networkState`";
    } else {
      ++metrics->agent_registered_messages_dropped;
      LOG(ERROR) << "Got ACK for network message for non-existent PID "
                 << from;
    }
  }

  Future<http::Response> stateEndpoint(const http::Request& request)
  {
    VLOG(1) << "Responding to `state` endpoint";

    return http::OK(
        JSON::protobuf(networkState),
        request.url.query.get("jsonp"));
  }

  Future<http::Response> deleteAgentsEndpoint(const http::Request& request)
  {
    VLOG(1) << "Responding to `agents/delete` endpoint";

    if (request.method != "POST") {
      return http::MethodNotAllowed({"POST"}, request.method);
    }

    if (replicatedLog.get() != nullptr && storedState.isNone()) {
      return http::ServiceUnavailable();
    }

    Try<JSON::Object> json = JSON::parse<JSON::Object>(request.body);
    if (json.isError()) {
      return http::BadRequest();
    }

    Result<JSON::Array> agentIPsJson = json.get().at<JSON::Array>("agents");
    if (agentIPsJson.isError() || agentIPsJson.isNone()) {
      return http::BadRequest();
    }

    hashset<string> agentIPs;
    foreach (const JSON::Value value, agentIPsJson.get().values) {
      if (!value.is<JSON::String>()) {
        return http::BadRequest();
      }
      string agentIPStr = value.as<JSON::String>().value;
      Try<IP> agentIP = IP::parse(agentIPStr, AF_INET);
      if (agentIP.isError()) {
        return http::BadRequest();
      }
      agentIPs.insert(agentIPStr);
    }

    http::Pipe pipe;
    http::Response response;
    response.type = http::Response::PIPE;
    response.code = http::Status::OK;
    response.status = http::Status::string(response.code);
    response.headers = http::Headers({
      {"Content-Type", "application/json"}
    });
    response.reader = Option<http::Pipe::Reader>::some(pipe.reader());

    http::Pipe::Writer writer = pipe.writer();
    update(Owned<Operation>(new DeleteAgents(agentIPs)))
      .onAny(defer(self(),
                   &ManagerProcess::_deleteAgentsEndpoint,
                   writer,
                   agentIPs,
                   lambda::_1));

    return response;
  }

  void _deleteAgentsEndpoint(http::Pipe::Writer writer,
                             const hashset<string> agentIPs,
                             const Future<bool>& result)
  {
    if (!result.isReady()) {
      if (result.isPending()) {
        writer.fail("the future is still pending");
      } else if (result.isDiscarded()) {
        writer.fail("the future has been discarded");
      } else if (result.isAbandoned()) {
        writer.fail("the future has been abandoned");
      } else if (result.isFailed()) {
        writer.fail(result.failure());
      } else {
        writer.fail("the future is not ready due to an unknown reason");
      }
      return;
    }

    hashset<IP> parsedAgentIPs;
    foreach (const string ip, agentIPs) {
      parsedAgentIPs.insert(IP::parse(ip, AF_INET).get());
    }
    Try<Nothing> deletionOutcome = deleteAgents(parsedAgentIPs);
    if (deletionOutcome.isError()) {
      LOG(ERROR) << "Failed to delete agents: " << deletionOutcome.error();
    }
    CHECK(!deletionOutcome.isError());

    string stateJson = jsonify(JSON::protobuf(networkState));
    writer.write(stateJson);
    writer.close();
  }

  void recover()
  {
    // Nothing to recover.
    CHECK_NOTNULL(replicatedLog.get());

    recovering = true;
    metrics->recovering = 1;

    replicatedLog->fetch<overlay::State>(REPLICATED_LOG_STORE_KEY)
      .after(replicatedLogTimeout,
             defer(self(),
                 &ManagerProcess::timeout<Variable<overlay::State>>,
                 "fetch",
                 replicatedLogTimeout,
                 lambda::_1))
      .onAny(defer(self(),
                   &ManagerProcess::_recover,
                   lambda::_1));
  }

  void _recover(Future<Variable<overlay::State>> variable)
  {
    CHECK_NOTNULL(replicatedLog.get());

    if (!variable.isReady()) {
      LOG(WARNING) << "This " << self().id <<"might have been demoted."
                   << "Aborting recovery of replicated log"
                   <<(variable.isDiscarded() ? "discarded"
                       : variable.failure());

      demote();
      return;
    }

    overlay::State _networkState = variable.get().get();

    // Only if the `network_config` is present does it imply that the
    // overlay-master stored state in the replicated log, else  this
    // is the first time an overlay-master is accessing the
    // replicated log and hence the state will be empty.
    if (!_networkState.has_network()) {
      LOG(INFO) << "No network state present, hence nothing to"
                << " recover from replicated log";

      // Update the `storeState` variable so that we know where to
      // update the `State` in the replicated log.
      storedState = variable.get();

      LOG(INFO) << "Moving " << self() << " to `RECOVERED` state.";
      return;
    }

    // Re-populate the agents, the overlay subnets that have been
    // allocated, and the VTEP IP and VTEP MAC that have been
    // allocated. The information stored in the replicated log should
    // not have any errors.
    for (int i = 0; i < _networkState.agents_size(); i++) {
      const AgentInfo& agentInfo = _networkState.agents(i);

      // Clear the `State` of the `AgentInfo`
      Try<Agent> agent = Agent::create(metrics, agentInfo);
      if (agent.isError()) {
        LOG(ERROR) << "Could not recover Agent: "<< agent.error();
        demote();
        return;
      }

      agents.emplace(agent->getIP(), agent.get());
      VLOG(1) << "Recovered agent: " << agent->getIP();

      hashset <string> reservedVTEPIPs, reservedVTEPIP6s;

      for (int j = 0; j < agentInfo.overlays_size(); j++) {
        const AgentOverlayInfo& overlay = agentInfo.overlays(j);

        // clear the overlay state.
        _networkState.mutable_agents(i)->mutable_overlays(j)->clear_state();

        // IPv4
        if (overlay.has_subnet()) {
          Try<Network> network = Network::parse(
              overlay.subnet(),
              AF_INET);

          if (network.isError()) {
            LOG(ERROR) << "Unable to parse the retrieved network: "
                       << overlay.subnet() << ": "
                       << network.error();

            demote();
            return;
          }

          // We should already have this particular overlay at bootup.
          CHECK(overlays.contains(overlay.info().name()));

          LOG(INFO) << "Reserving IPv4 " << stringify(network.get());
          Try<Nothing> result =
            overlays.at(overlay.info().name())->reserve(network.get());
          if (result.isError()) {
            LOG(ERROR) << "Unable to reserve the subnet " << network.get()
                       << ": " << result.error();

            demote();
            return;
          }
        }

        // IPv6
        if (overlay.has_subnet6()) {
          Try<Network> network6 = Network::parse(
              overlay.subnet6(),
              AF_INET6);

          if (network6.isError()) {
            LOG(ERROR) << "Unable to parse the retrieved network: "
              << overlay.subnet6() << ": "
              << network6.error();
            demote();
            return;
          }

          // We should already have this particular overlay at bootup.
          CHECK(overlays.contains(overlay.info().name()));

          LOG(INFO) << "Reserving IPv6 " << stringify(network6.get());
          Try<Nothing> result =
            overlays.at(overlay.info().name())->reserve6(network6.get());
          if (result.isError()) {
            LOG(ERROR) << "Unable to reserve the IPv6 subnet " << network6.get()
                       << ": " << result.error();
            demote();
            return;
          }
        }

        // VTEP IPv4
        string vtepIPStr = overlay.backend().vxlan().vtep_ip();
        if (!reservedVTEPIPs.contains(vtepIPStr)) {
          Try<Network> vtepIP = Network::parse(vtepIPStr, AF_INET);

          if (vtepIP.isError()) {
            LOG(ERROR) << "Unable to parse the retrieved `vtepIP`: "
                       << vtepIPStr << ": " << vtepIP.error();

            demote();
            return;
          }

          // NOTE: We only need to reserve the VTEP IP and not the
          // VTEP MAC since the VTEP MAC is derived from the VTEP IP.
          // Look at the `generateMAC` method in `VTEP` to see how
          // this is done.
          LOG(INFO) << "Reserving VTEP IP: " << vtepIP.get();
          Try<Nothing> result = vtep.reserve(vtepIP.get());
          if (result.isError()) {
            LOG(ERROR) << "Unable to reserve VTEP IP: "
                       << vtepIP.get() << ": " << result.error();

            demote();
            return;
          }

          reservedVTEPIPs.insert(vtepIPStr);
        }

        // VTEP IPv6
        if (overlay.backend().vxlan().has_vtep_ip6()) {
          string vtepIP6Str = overlay.backend().vxlan().vtep_ip6();
          if (!reservedVTEPIP6s.contains(vtepIP6Str)) {
            Try<Network> vtepIP6 = Network::parse(vtepIP6Str, AF_INET6);

            if (vtepIP6.isError()) {
              LOG(ERROR) << "Unable to parse the retrieved `vtep IPv6`: "
                << vtepIP6Str << ": " << vtepIP6.error();
              demote();
              return;
            }

            LOG(INFO) << "Reserving VTEP IPv6: " << vtepIP6.get();
            Try<Nothing> result = vtep.reserve6(vtepIP6.get());
            if (result.isError()) {
              LOG(ERROR) << "Unable to reserve VTEP IPv6: "
                         << vtepIP6.get() << ": " << result.error();
              demote();
              return;
            }

            reservedVTEPIP6s.insert(vtepIP6Str);
          }
        }
      }
    }

    // Recovery done. Copy the recovered state into the `State`
    // object.
    //
    // NOTE: We are retaining the current configuration so that we can
    // remember any new overlay networks that might have been added by
    // the operator during the restart.
    _networkState.mutable_network()->CopyFrom(networkState.network());

    networkState.CopyFrom(_networkState);

    // Update the `storeState` variable so that we know where to
    // update the `State` in the replicated log.
    storedState = variable.get();

    LOG(INFO) << "Moving " << self() << " to `RECOVERED` state.";
    recovering = false;
    metrics->recovering = 0;
    return;
  }

private:
  bool recovering;
  bool storing;

  hashmap<string, Owned<Overlay>> overlays;
  hashmap<IP, Agent> agents;

  Owned<mesos::state::protobuf::State> replicatedLog;
  Duration replicatedLogTimeout;

  Option<Variable<overlay::State>> storedState;

  overlay::State networkState;

  // We need to keep track of `storage` and `log`, since we will need
  // to free them up when the master manager process is deleted.
  Storage* storage;

  Log* log;

  // The set of operations that need to be performed on the
  // `networkState` before writing to the replicated log.
  std::deque<Owned<Operation>> operations;

  Vtep vtep;

  std::shared_ptr<Metrics> metrics;

  ManagerProcess(
      const hashmap<string, Owned<Overlay>>& _overlays,
      const Network& vtepSubnet,
      const Option<Network>& vtepSubnet6,
      const net::MAC& vtepMACOUI,
      const Option<size_t>& vtepMTU,
      const NetworkConfig& _networkConfig,
      const Owned<mesos::state::protobuf::State> _replicatedLog,
      const Duration _replicatedLogTimeout,
      Storage* _storage,
      Log* _log)
    : ProcessBase("overlay-master"),
      recovering(false),
      storing(false),
      overlays(_overlays),
      replicatedLog(_replicatedLog),
      replicatedLogTimeout(_replicatedLogTimeout),
      storedState(None()),
      storage(_storage),
      log(_log),
      vtep(vtepSubnet, vtepSubnet6, vtepMACOUI, vtepMTU),
      metrics(std::make_shared<Metrics>())
  {
    networkState.mutable_network()->CopyFrom(_networkConfig);
  };

  // Updates the `networkState` with the operation provided. If we are
  // using the replicated log we will `queue` the operation and invoke
  // `store, else we will apply the operation immediately.
  // In case the replicated log is being used, the stored operation is
  // applied on a copy of `networkState` and the mutated `State` is
  // written into the overlay replicated log. On a successful write
  // the `networkState` is updated with the `State` that was stored in
  // the replicated log.
  Future<bool> update(const Owned<Operation> operation)
  {
    if (replicatedLog.get() == nullptr) {
      Try<bool> result = (*operation)(&networkState, &agents);
      if (result.isError()) {
        return Failure(
            "Unable to perform operation: " + result.error());
      }

      return result.get();
    }

    operations.push_back(operation);
    Future<bool> future = operation->future();

    if (!storing) {
      store();
    }

    return future;
  }

  void store()
  {
      // We should not be trying to store to the replicated log till
      // recovery is complete.
      CHECK(storedState.isSome());

      storing = true;

      CHECK_NOTNULL(replicatedLog.get());

      overlay::State _networkState;
      _networkState.CopyFrom(networkState);

      foreach (Owned<Operation> operation, operations) {
        (*operation)(&_networkState, &agents);
      }


      Variable<overlay::State> stateVariable = storedState.get();
      stateVariable = stateVariable.mutate(_networkState);

      replicatedLog->store(stateVariable)
        .after(replicatedLogTimeout,
               defer(self(),
                     &ManagerProcess::timeout<Option<Variable<overlay::State>>>,
                     "store",
                     replicatedLogTimeout,
                     lambda::_1))
        .onAny(defer(self(), &ManagerProcess::_store, lambda::_1, operations));

      operations.clear();
  }

  void _store(
      const Future<Option<Variable<overlay::State>>>& variable,
      std::deque<Owned<Operation>> applied)
  {
    storing = false;

    if (!variable.isReady()) {
      LOG(WARNING) << "Not updating `State` due to failure to write to log."
                   << (variable.isDiscarded() ? "discarded"
                       : variable.failure());
      demote();
      return;
    }

    if (variable.get().isNone()) {
      LOG(WARNING) << "Not updating `State` since this Master might"
                   << "have been demoted.";
      demote();
      return;
    }

    LOG(INFO) << "Stored the network state successfully";

    overlay::State storedNetworkState = variable.get().get().get();

    VLOG(1) << "Stored the following network state:";
    if (storedNetworkState.has_network()) {
      VLOG(1) << "VTEP: " << storedNetworkState.network().vtep_subnet();
      if (storedNetworkState.network().has_vtep_subnet6()) {
        VLOG(1) << "VTEP IPv6: " << storedNetworkState.network().vtep_subnet6();
      }
      VLOG(1) << "VTEP OUI: " << storedNetworkState.network().vtep_mac_oui();
      VLOG(1) << "Total overlays: "
              << storedNetworkState.network().overlays_size();
    }

    if (storedNetworkState.agents_size() > 0) {
      VLOG(1) << "Total agents: " << storedNetworkState.agents_size();
    }

    networkState.CopyFrom(storedNetworkState);

    storedState = variable.get();

    // Signal all operations are complete.
    while (!applied.empty()) {
      Owned<Operation> operation = applied.front();
      applied.pop_front();

      operation->set();

      LOG(INFO) << "Applied operation '" << *operation << "'"
                << " successfully.";
    }

    if (!operations.empty()) {
      store();
    }
  }

  template <typename T>
  Future<T> timeout(
      const string& operation,
      const Duration& duration,
      Future<T> future)
  {
    // Helper for treating State operations that timeout as failures.
    future.discard();

    return Failure(
        "Failed to perform " + operation + " within " + stringify(duration));
  }


  void demote()
  {
    LOG(WARNING) << "Demoting (terminating) " << self();

    // To force re-election and agents re-registration, terminate the process.
    // It will be re-started by the Supervisor with a clean state in 3 seconds.
    terminate(self());
  }
};


class Manager : public Anonymous
{
public:
  static Try<Manager*> createManager(const MasterConfig& masterConfig)
  {
    Try<Owned<ProcessSupervisor<ManagerProcess>>> process =
      ProcessSupervisor<ManagerProcess>::create([=]() {
        return ManagerProcess::createManagerProcess(masterConfig);
      }, Seconds(3));

    if (process.isError()) {
      return Error(process.error());
    }

    return new Manager(process.get());
  }

  virtual ~Manager()
  {
    terminate(process.get());
    wait(process.get());
  }

private:
  Manager(Owned<ProcessSupervisor<ManagerProcess>> _process)
  : process(_process)
  {
    spawn(process.get());
  }

  Owned<ProcessSupervisor<ManagerProcess>> process;
};

} // namespace master {
} // namespace overlay {
} // namespace modules {
} // namespace mesos {


using mesos::modules::overlay::master::Manager;
using mesos::modules::overlay::master::ManagerProcess;


Anonymous* createOverlayMasterManager(const Parameters& parameters)
{
  Option<MasterConfig> masterConfig = None();

  foreach (const mesos::Parameter& parameter, parameters.parameter()) {
    LOG(INFO) << "Overlay master parameter '" << parameter.key()
              << "=" << parameter.value() << "'";

    if (parameter.key() == "master_config") {
      if (!os::exists(parameter.value())) {
        LOG(ERROR) << "Unable to find Master configuration"
                   << parameter.value();
        return nullptr;
      }

      Try<string> config = os::read(parameter.value());
      if (config.isError()) {
        LOG(ERROR) << "Unable to read the Master "
                   << "configuration: " << config.error();
        return nullptr;
      }

      auto parseMasterConfig = [](const string& s) -> Try<MasterConfig> {
        Try<JSON::Object> json = JSON::parse<JSON::Object>(s);
        if (json.isError()) {
          return Error("JSON parse failed: " + json.error());
        }

        Try<MasterConfig> parse =
          ::protobuf::parse<MasterConfig>(json.get());

        if (parse.isError()) {
          return Error("Protobuf parse failed: " + parse.error());
        }

        return parse.get();
      };

      Try<MasterConfig> _masterConfig = parseMasterConfig(config.get());
      if (_masterConfig.isError()) {
        LOG(ERROR)
          << "Unable to parse the Master JSON configuration: "
          << _masterConfig.error();
        return nullptr;
      }

      masterConfig = _masterConfig.get();
    }
  }

  if (masterConfig.isNone()) {
    LOG(ERROR) << "Missing `master_config`";
    return nullptr;
  }

  Try<Manager*> manager = Manager::createManager(masterConfig.get());
  if (manager.isError()) {
    LOG(ERROR)
      << "Unable to create the Master manager module: "
      << manager.error();
    return nullptr;
  }

  return manager.get();
}


// Declares a helper module named 'Manager'.
Module<Anonymous> com_mesosphere_mesos_OverlayMasterManager(
    MESOS_MODULE_API_VERSION,
    MESOS_VERSION,
    "Mesosphere",
    "help@mesosphere.io",
    "Master Overlay Helper Module",
    nullptr,
    createOverlayMasterManager);

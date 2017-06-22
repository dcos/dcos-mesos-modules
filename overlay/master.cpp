#include <stdio.h>

#include <list>

#include <stout/check.hpp>
#include <stout/interval.hpp>
#include <stout/ip.hpp>
#include <stout/json.hpp>
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

#include "messages.hpp"
#include "overlay.hpp"

namespace http = process::http;

using std::list;
using std::ostream;
using std::queue;
using std::set;
using std::string;
using std::vector;

using net::IP;
using net::IPNetwork;
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

constexpr Duration PENDING_MESSAGE_PERIOD = Seconds(10);

const string OVERLAY_HELP = HELP(
    TLDR("Allocate overlay network resources for Master."),
    USAGE("/overlay-master/overlays"),
    DESCRIPTION("Allocate subnets, VTEP IP and the MAC addresses.", "")
);


struct Vtep
{
  Vtep(const IPNetwork& _network, const MAC _oui)
    : network(_network), oui(_oui)
  {
    // `endIP` = `32 - network.prefix()` number of LSB bits set.
    //
    // NOTE: The below shift operation results in `32 - network.prefix()
    // remaining bits in the LSB of `endIP`.
    uint32_t endIP = 0xffffffff >> network.prefix();

    freeIP += (Bound<uint32_t>::closed(1), Bound<uint32_t>::closed(endIP - 1));
  }

  Try<IPNetwork> allocateIP()
  {
    if (freeIP.empty()) {
      return Error("Unable to allocate a VTEP IP due to exhaustion");
    }

    uint32_t ip = freeIP.begin()->lower();
    freeIP -= ip;

    uint32_t address = ntohl(network.address().in().get().s_addr);
    address += ip;

    return IPNetwork::create(net::IP(address),network.prefix()) ;
  }

  Try<Nothing> reserve(const net::IPNetwork& ip)
  {
    uint32_t _ip = ntohl(ip.address().in().get().s_addr);
    uint32_t mask = ntohl(network.netmask().in().get().s_addr);

    _ip &= ~mask;

    if (!freeIP.contains(_ip)) {
      VLOG(1) << "Current free IPs: " << freeIP;
      return Error(
          "Cannot reserve an unavailable IP: "  + stringify(ip) +
          "(" + stringify(_ip) + ")");
    }

    freeIP -= _ip;

    return Nothing();
  }

  // We generate the VTEP MAC from the IP by taking the least 24 bits
  // of the IP and using the 24 bits as the NIC of the MAC.
  //
  // NOTE: There is a caveat to using this technique to generating the
  // MAC, namely if the CIDR prefix of the IP is less than 8 then we
  // the MAC being generated is not guaranteed to be unique.
  Try<MAC> generateMAC(const net::IP& ip)
  {
    Try<struct in_addr> in = ip.in();

    if (in.isError()) {
      return Error(in.error());
    }

    uint8_t *nic = (uint8_t*)&(in.get().s_addr);

    uint8_t mac[6];

    //Set the OUI.
    mac[0] = oui[0];
    mac[1] = oui[1];
    mac[2] = oui[2];

    //Set the NIC.
    mac[3] = nic[1];
    mac[4] = nic[2];
    mac[5] = nic[3];

    return MAC(mac);
  }

  void reset()
  {
    uint32_t endIP = 0xffffffff >> network.prefix();

    freeIP = IntervalSet<uint32_t>();
    freeIP += (Bound<uint32_t>::closed(1), Bound<uint32_t>::closed(endIP - 1));
  }

  // Network allocated to the VTEP.
  IPNetwork network;

  MAC oui;

  IntervalSet<uint32_t> freeIP;
};


struct Overlay
{
  Overlay(
      const string& _name,
      const net::IPNetwork& _network,
      const uint8_t _prefix)
    : name(_name),
    network(_network),
    prefix(_prefix)
  {
    // `network` has already been vetted to be an AF_INET address.
    uint32_t endSubnet = 0xffffffff; // 255.255.255.255
    endSubnet = endSubnet >> (network.prefix() + 32 - prefix);

    freeNetworks +=
      (Bound<uint32_t>::closed(0),
       Bound<uint32_t>::closed(endSubnet));
  }

  OverlayInfo getOverlayInfo() const
  {
    OverlayInfo overlay;

    overlay.set_name(name);
    overlay.set_subnet(stringify(network));
    overlay.set_prefix(prefix);

    return overlay;
  }

  Try<net::IPNetwork> allocate()
  {
    if (freeNetworks.empty()) {
      return Error("No free subnets available in the " + name + "overlay");
    }

    uint32_t subnet = freeNetworks.begin()->lower();
    freeNetworks -= subnet;

    uint32_t agentSubnet = ntohl(network.address().in().get().s_addr);

    // Get the suffix of the Agent subnet.
    subnet = subnet << (32 - prefix);
    agentSubnet |= subnet;

    return net::IPNetwork::create(net::IP(agentSubnet), prefix);
  }

  Try<Nothing> free(const net::IPNetwork& subnet)
  {
    if (subnet.prefix() != prefix) {
      return Error(
          "Cannot free this network because prefix " + stringify(subnet.prefix()) +
          " does not match Agent prefix " + stringify(prefix) + " of the overlay");
    }

    if (subnet.prefix() < network.prefix()) {
      return Error(
          "Cannot free this network since it does not belong "
          " to the overlay subnet");
    }

    uint32_t address = ntohl(subnet.address().in().get().s_addr);

    freeNetworks += address;

    return Nothing();
  }

  Try<Nothing> reserve(const net::IPNetwork& subnet)
  {
    uint32_t netmask = ntohl(network.netmask().in().get().s_addr);
    uint32_t _subnet = ntohl(subnet.address().in().get().s_addr);

    // Retrieve the integer representation of subnet in the
    // `IntervalSet`.
    _subnet &= ~netmask;
    _subnet = _subnet >> (32 - subnet.prefix());

    if (!freeNetworks.contains(_subnet)) {
      return Error(
          "Unable to reserve unavailable subnet " +
          stringify(subnet) + "(" + stringify(_subnet) + ")");
    }

    freeNetworks -= _subnet;

    return Nothing();
  }

  void reset()
  {
    // Re-initialize `freeNetworks`.
    freeNetworks = IntervalSet<uint32_t>();

    uint32_t endSubnet = 0xffffffff; // 255.255.255.255
    endSubnet = endSubnet >> (network.prefix() + 32 - prefix);

    freeNetworks +=
      (Bound<uint32_t>::closed(0),
       Bound<uint32_t>::closed(endSubnet));
  }

  // Canonical name of the network.
  std::string name;

  // Network allocated to this overlay.
  net::IPNetwork network;

  // Prefix length allocated to each agent.
  uint8_t prefix;

  // Free subnets available in this network. The subnets are
  // calcualted using the prefix length set for the agents in
  // `prefix`.
  IntervalSet<uint32_t> freeNetworks;
};


class Agent
{
public:
  static Try<Agent> create(const AgentInfo& agentInfo)
  {
    Try<net::IP> _ip = net::IP::parse(agentInfo.ip(), AF_INET);

    if (_ip.isError()) {
      return Error("Unable to create `Agent`: " + _ip.error());
    }

    Agent agent(_ip.get());

    for (int i = 0; i < agentInfo.overlays_size(); i++) {
      agent.addOverlay(agentInfo.overlays(i));
    }

    // We should clear any overlay `State` that might have been set.
    // The `State` will be set once this information is downloaded to
    // the corresponding agent.
    agent.clearOverlaysState();

    return agent;
  }

  Agent(const net::IP& _ip) : ip(_ip) {};

  const net::IP getIP() const { return ip; };

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
                 << overlay.info().name() ;
    }

    overlays.at(name)->mutable_state()->set_status(overlay.state().status());
  }

private:
  net::IP ip;

  // A list of all overlay networks that reside on this agent.
  hashmap<string, Owned<AgentOverlayInfo>> overlays;
};


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
      hashmap<net::IP, Agent>* agents)
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
      hashmap<net::IP, Agent>* agents) = 0;

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
  Try<bool> perform(State* networkState, hashmap<net::IP, Agent>* agents)
  {
    // Make sure the Agent we are going to add is already present in `agents`.
    Try<net::IP> ip = net::IP::parse(agentInfo.ip(), AF_INET);
    if (ip.isError()) {
      return Error("Unable to parse the Agent IP: " + ip.error());
    }

    if (!agents->contains(ip.get())) {
      return Error(
          "Could not find the Agent (" + stringify(ip.get()) +
          ") that needed to be added to `State`.");
    }

    networkState->add_agents()->CopyFrom(agentInfo);
    return true;
  }

private:
  AgentInfo agentInfo;
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

    Try<net::IPNetwork> vtepSubnet =
      net::IPNetwork::parse(networkConfig.vtep_subnet(), AF_INET);
    if (vtepSubnet.isError()) {
      return Error(
          "Unable to parse the VTEP Subnet: " + vtepSubnet.error());
    }

    // Make sure the VTEP subnet CIDR is not less than /8
    if (vtepSubnet.get().prefix() < 8) {
      return Error(
          "VTEP MAC are derived from last 24 bits of VTEP IP.  Hence, "
          "in order to guarantee unique VTEP MAC we need the VTEP IP "
          "subnet to greater than /8");
    }

    Try<net::MAC> vtepMACOUI = createMAC(networkConfig.vtep_mac_oui(), true);
    if(vtepMACOUI.isError()) {
      return Error(
          "Unable to parse VTEP MAC OUI: " + vtepMACOUI.error());
    }

    hashmap<string, Owned<Overlay>> overlays;
    IntervalSet<uint32_t> addressSpace;

    // Overlay networks cannot have overlapping IP addresses. This
    // lambda keeps track of the current address space and returns an
    // `Error` if it detects an overlay that is going to use an
    // already configured address space.
    auto updateAddressSpace =
      [&addressSpace](const IPNetwork &network) -> Try<Nothing> {
        uint32_t startIP = ntohl(network.address().in().get().s_addr);

        uint32_t mask = ntohl(network.netmask().in().get().s_addr);
        mask = ~mask;

        uint32_t endIP = startIP | mask;

        Interval<uint32_t> overlaySpace =
          (Bound<uint32_t>::closed(startIP), Bound<uint32_t> ::closed(endIP));

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
      // cannot excced 15 characters, we need to impose the limit on
      // the overlay network name.
      if (overlay.name().size() > MAX_OVERLAY_NAME) {
        return Error(
            "Overlay name: " + overlay.name() +
            " too long cannot, exceed " + stringify(MAX_OVERLAY_NAME) +
            "  characters");
      }

      LOG(INFO) << "Configuring overlay network:" << overlay.name();

      Try<net::IPNetwork> address =
        net::IPNetwork::parse(overlay.subnet(), AF_INET);
      if (address.isError()) {
        return Error(
            "Unable to determine subnet for network: " +
            stringify(address.get()));
      }

      Try<Nothing> valid = updateAddressSpace(address.get());

      if (valid.isError()) {
        return Error(
            "Incorrect address space for the overlay network '" +
            overlay.name() + "': " + valid.error());
      }

      overlays.emplace(
          overlay.name(),
          Owned<Overlay>(new Overlay(
            overlay.name(),
            address.get(),
            (uint8_t) overlay.prefix())));
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
            "overlay/");
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
            "overlay/");
      }

      storage = new LogStorage(log);
    }

    Owned<mesos::state::protobuf::State> replicatedLog;

    if (storage != nullptr) {
      replicatedLog = Owned<mesos::state::protobuf::State>(
          new mesos::state::protobuf::State(storage));
    }

    return Owned<ManagerProcess>(new ManagerProcess(
          overlays,
          vtepSubnet.get(),
          vtepMACOUI.get(),
          networkConfig,
          replicatedLog,
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
          OVERLAY_HELP,
          &ManagerProcess::state);

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

    if (replicatedLog.get() != nullptr) {
      if (storedState.isNone() && !recovering) {
        // We haven't started recovering.
        LOG(INFO) << MASTER_MANAGER_PROCESS_ID << " moving to `RECOVERING`"
          << " state . Hence, not sending an update to agent"
          << pid;
        recover();
        return;
      } else if (storedState.isNone() && recovering) {
        // We are recovering. Drop the message, the agent will try to
        // re-register.
        LOG(INFO) << MASTER_MANAGER_PROCESS_ID << " in `RECOVERING`"
          << " state . Hence, not sending an update to agent"
          << pid;
        return;
      } // else -> `storedState.isSome` , we have recovered.
    }

    if (agents.contains(pid.address.ip)) {
      LOG(INFO) << "Agent " << pid << " re-registering.";

      // Ensure that the agent is added to the replicated log.
      const string agentIP = stringify(pid.address.ip);
      for (int i = 0; i < networkState.agents_size(); i++) {
        if (agentIP == networkState.agents(i).ip()) {
          // Given that `networkState` already has this Agent, the
          // information is already stored in replicated log and hence
          // we can just send an "ACK" to the agent with the
          // configuration info
          _registerAgent(pid, true);
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
      return;
    } else {
      // New Agent.
      LOG(INFO) << "New registration from pid: " << pid;
      agents.emplace(pid.address.ip, Agent(pid.address.ip));

      Agent* agent = &(agents.at(pid.address.ip));

      Try<net::IPNetwork> vtepIP = vtep.allocateIP();
      if (vtepIP.isError()) {
        LOG(ERROR)
          << "Unable to get VTEP IP for Agent: " << vtepIP.error()
          << "Cannot fulfill registration for Agent: " << pid;
        return;
      }
      LOG(INFO) << "Allocated VTEP IP : " << vtepIP.get();

      Try<net::MAC> vtepMAC = vtep.generateMAC(vtepIP.get().address());
      if (vtepMAC.isError()) {
        LOG(ERROR)
          << "Unable to get VTEP MAC for Agent: " << vtepMAC.error()
          << "Cannot fulfill registration for Agent: " << pid;
          return;
      }
      VLOG(1) << "Allocated VTEP MAC : " << vtepMAC.get();

      // Walk through all the overlay networks. Allocate a subnet from
      // each overlay to the Agent. Allocate a VTEP IP and MAC for each
      // agent. Queue the message on the Agent. Finally, ask Master to
      // reliably send these messages to the Agent.
      foreachpair (const string& name, Owned<Overlay>& overlay, overlays) {
        AgentOverlayInfo _overlay;
        Option<net::IPNetwork> agentSubnet = None();

        _overlay.mutable_info()->set_name(name);
        _overlay.mutable_info()->set_subnet(stringify(overlay->network));
        _overlay.mutable_info()->set_prefix(overlay->prefix);

        if (registerMessage.network_config().allocate_subnet()) {
          Try<net::IPNetwork> _agentSubnet = overlay->allocate();
          if (_agentSubnet.isError()) {
            LOG(ERROR) << "Cannot allocate subnet from overlay "
                       << name << " to Agent " << pid << ":"
                       << _agentSubnet.error();
            continue;
          }

          agentSubnet = _agentSubnet.get();
          _overlay.set_subnet(stringify(agentSubnet.get()));

          // Allocate bridges for Mesos and Docker.
          Try<Nothing> bridges = allocateBridges(
              &_overlay,
              registerMessage.network_config());

          if (bridges.isError()) {
            LOG(ERROR) << "Unable to allocate bridge for network "
                       << name << ": " << bridges.error();
            if ( agentSubnet.isSome()) {
              overlay->free(agentSubnet.get());
            }
            continue;
          }
        }

        VxLANInfo vxlan;
        vxlan.set_vni(1024);
        vxlan.set_vtep_name("vtep1024");
        vxlan.set_vtep_ip(stringify(vtepIP.get()));
        vxlan.set_vtep_mac(stringify(vtepMAC.get()));

        BackendInfo backend;
        backend.mutable_vxlan()->CopyFrom(vxlan);

        _overlay.mutable_backend()->CopyFrom(backend);

        agent->addOverlay(_overlay);
      }

      // Update the `networkState in the replicated log before
      // sending the overlay configuration to the Agent.
      update(Owned<Operation>(
            new AddAgent(agents.at(pid.address.ip).getAgentInfo())))
        .onAny(defer(self(),
              &ManagerProcess::_registerAgent,
              pid,
              lambda::_1));
      return;
    }

    UNREACHABLE();
  }

  // Will be called once the operation is successfully applied to the
  // `networkState`.
  void _registerAgent(const UPID& pid, const Future<bool>& result)
  {
    if (!result.isReady()) {
      LOG(WARNING) << "Unable to process registration request from "
                   << pid << " due to: "
                   << (result.isDiscarded() ? "discarded" : result.failure());
      return;
    }

    CHECK(agents.contains(pid.address.ip));

    list<AgentOverlayInfo> _overlays = agents.at(pid.address.ip).getOverlays();

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
  }

  void agentRegistered(const UPID& from, const AgentRegisteredMessage& message)
  {
    if(agents.contains(from.address.ip)) {
      LOG(INFO) << "Got ACK for addition of networks from " << from;
      for(int i = 0; i < message.overlays_size(); i++) {
        agents.at(from.address.ip).updateOverlayState(message.overlays(i));
      }

      // We don't need to store the "state" of an overlay network on
      // an agent in the replicated log so go ahead and update the
      // `networkState` without updating the overlay replicated log.
      for (int i = 0; i < networkState.agents_size(); i++) {
        if (stringify(from.address.ip) == networkState.agents(i).ip()) {
          networkState.mutable_agents(i)->CopyFrom(
              agents.at(from.address.ip).getAgentInfo());

          LOG(INFO) << "Sending register ACK to: " << from;
          send(from, AgentRegisteredAcknowledgement());
          return;
        }
      }
      LOG(ERROR) << "Unable to find the registered agent in the `networkState`";
    } else {
      LOG(ERROR) << "Got ACK for network message for non-existent PID "
                 << from;
    }
  }

  Future<http::Response> state(const http::Request& request)
  {
    VLOG(1) << "Responding to `state` endpoint";

    return http::OK(
        JSON::protobuf(networkState),
        request.url.query.get("jsonp"));
  }

  void recover()
  {
    // Nothing to recover.
    CHECK_NOTNULL(replicatedLog.get());

    recovering = true;

    replicatedLog->fetch<State>(REPLICATED_LOG_STORE_KEY)
      .onAny(defer(self(),
                   &ManagerProcess::_recover,
                   lambda::_1));
  }

  void _recover(Future<Variable<State>> variable)
  {
    CHECK_NOTNULL(replicatedLog.get());

    if (!variable.isReady()) {
      LOG(WARNING) << "This " << self().id <<"might have been demoted."
                   << "Aborting recovery of replicated log"
                   <<(variable.isDiscarded() ? "discarded" : variable.failure());

      return;
    }

    State _networkState = variable.get().get();

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
      Try<Agent> agent = Agent::create(agentInfo);
      if (agent.isError()) {
        LOG(ERROR) << "Could not recover Agent: "<< agent.error();
        abort();
      }

      agents.emplace(agent->getIP(), agent.get());
      VLOG(1) << "Recovered agent: " << agent->getIP();

      for (int j = 0; j < agentInfo.overlays_size(); j++) {
        const AgentOverlayInfo& overlay = agentInfo.overlays(j);

        // clear the overlay state.
        _networkState.mutable_agents(i)->mutable_overlays(j)->clear_state();

        Try<net::IPNetwork> network = net::IPNetwork::parse(
            overlay.subnet(),
            AF_INET);

        if (network.isError()) {
          LOG(ERROR) << "Unable to parse the retrieved network: "
                     << overlay.subnet() << ": "
                     << network.error();

          abort();
        }

        // We should already have this particular overlay at bootup.
        CHECK(overlays.contains(overlay.info().name()));
        Try<Nothing> result = overlays.at(overlay.info().name())->reserve(network.get());
        if (result.isError()) {
          LOG(ERROR) << "Unable to reserve the subnet " << network.get()
                     << ": " << result.error();

          abort();
        }

        // All overlay instances on an Agent share the same VTEP IP
        // and MAC. Hence, the backend information on all the overlay
        // information would be the same. We therefore need to reserve
        // the VTEP IP and MAC only once.
        if (j == 0) {
          Try<net::IPNetwork> vtepIP =
            net::IPNetwork::parse(overlay.backend().vxlan().vtep_ip(), AF_INET);
          if (vtepIP.isError()) {
            LOG(ERROR) << "Unable to parse the retrieved `vtepIP`: "
                       << overlay.backend().vxlan().vtep_ip() << ": "
                       << vtepIP.error();

            abort();
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

            abort();
          }
        }
      }
    }

    // Recovery done. Copy the recovered state into the `State`
    // object.
    networkState.CopyFrom(_networkState);

    // Update the `storeState` variable so that we know where to
    // update the `State` in the replicated log.
    storedState = variable.get();

    LOG(INFO) << "Moving " << self() << " to `RECOVERED` state.";
    return;
  }

private:
  bool recovering;
  bool storing;

  hashmap<string, Owned<Overlay>> overlays;
  hashmap<net::IP, Agent> agents;

  Owned<mesos::state::protobuf::State> replicatedLog;

  Option<Variable<State>> storedState;

  State networkState;

  // We need to keep track of `storage` and `log`, since we will need
  // to free them up when the master manager process is deleted.
  Storage* storage;

  Log* log;

  // The set of operations that need to be performed on the
  // `networkState` before writing to the replicated log.
  std::deque<Owned<Operation>> operations;

  Vtep vtep;

  ManagerProcess(
      const hashmap<string, Owned<Overlay>>& _overlays,
      const net::IPNetwork& vtepSubnet,
      const net::MAC& vtepMACOUI,
      const NetworkConfig& _networkConfig,
      const Owned<mesos::state::protobuf::State> _replicatedLog,
      Storage* _storage,
      Log* _log)
    : ProcessBase("overlay-master"),
      recovering(false),
      storing(false),
      overlays(_overlays),
      replicatedLog(_replicatedLog),
      storedState(None()),
      storage(_storage),
      log(_log),
      vtep(vtepSubnet, vtepMACOUI)
  {
    networkState.mutable_network()->CopyFrom(_networkConfig);
  };

  Try<Nothing> allocateBridges(
      AgentOverlayInfo* _overlay,
      const AgentNetworkConfig& networkConfig)
  {
    if (!networkConfig.mesos_bridge() &&
        !networkConfig.docker_bridge()) {
      return Nothing();
    }
    const string name = _overlay->info().name();

    Try<IPNetwork> network = net::IPNetwork::parse(
        _overlay->subnet(),
        AF_INET);

    if (network.isError()) {
      return Error("Unable to parse the subnet of the network '" +
          name + "' : " + network.error());
    }

    Try<struct in_addr> subnet = network->address().in();
    Try<struct in_addr> subnetMask = network->netmask().in();

    if (subnet.isError()) {
      return Error("Unable to get a 'struct in_addr' representation of "
          "the network :"  + subnet.error());
    }

    if (subnetMask.isError()) {
      return Error("Unable to get a 'struct in_addr' representation of "
          "the mask :"  + subnetMask.error());
    }

    uint32_t address = ntohl(subnet->s_addr);
    uint32_t mask = ntohl(subnetMask->s_addr) |
      (0x1 << (32 - (network->prefix() + 1)));

    // Create the Mesos bridge.
    if (networkConfig.mesos_bridge()) {
      Try<IPNetwork> mesosSubnet = net::IPNetwork::create((IP(address)), (IP(mask)));

      if (mesosSubnet.isError()) {
        return Error(
            "Could not create Mesos subnet for network '" +
            name + "': " + mesosSubnet.error());
      }

      BridgeInfo mesosBridgeInfo;
      mesosBridgeInfo.set_ip(stringify(mesosSubnet.get()));
      mesosBridgeInfo.set_name(MESOS_BRIDGE_PREFIX + name);
      _overlay->mutable_mesos_bridge()->CopyFrom(mesosBridgeInfo);
    }

    // Create the docker bridge.
    if (networkConfig.docker_bridge()) {
      Try<IPNetwork> dockerSubnet = net::IPNetwork::create(
          IP(address | (0x1 << (32 - (network->prefix() + 1)))),
          IP(mask));

      if (dockerSubnet.isError()) {
        return Error(
            "Could not create Docker subnet for network '" +
            name + "': " + dockerSubnet.error());
      }

      BridgeInfo dockerBridgeInfo;
      dockerBridgeInfo.set_ip(stringify(dockerSubnet.get()));
      dockerBridgeInfo.set_name(DOCKER_BRIDGE_PREFIX + name);
      _overlay->mutable_docker_bridge()->CopyFrom(dockerBridgeInfo);
    }

    return Nothing();
  }

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

      State _networkState;
      _networkState.CopyFrom(networkState);

      foreach (Owned<Operation> operation, operations) {
        (*operation)(&_networkState, &agents);
      }


      Variable<State> stateVariable = storedState.get();
      stateVariable = stateVariable.mutate(_networkState);

      replicatedLog->store(stateVariable)
        .onAny(defer(self(), &ManagerProcess::_store, lambda::_1, operations));

      operations.clear();
  }

  void _store(
      const Future<Option<Variable<State>>>& variable,
      std::deque<Owned<Operation>> applied)
  {
    storing = false;

    if (!variable.isReady()) {
      LOG(WARNING) << "Not updating `State` due to failure to write to log."
                   << (variable.isDiscarded() ? "discarded" : variable.failure());
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

    State storedNetworkState = variable.get().get().get();

    VLOG(1) << "Stored the following network state:";
    if (storedNetworkState.has_network()) {
      VLOG(1) << "VTEP: " << storedNetworkState.network().vtep_subnet();
      VLOG(1) << "VTEP OUI: " << storedNetworkState.network().vtep_mac_oui();
      VLOG(1) << "Total overlays: " << storedNetworkState.network().overlays_size();
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

  void demote()
  {
    // Reset state of the replicated log.
    recovering = false;
    storing = false;
    operations.clear();
    storedState = None();

    // We should forget all agents since when this master becomes
    // the leader they will re-register and get added to the
    // in-memory databse.
    agents.clear();
    networkState.clear_agents();


    // While we should not clear all the overlays (since they are static) we
    // need to de-allocate the address space of the overlays so that
    // when this master becomes the leader it can reserve any
    // addresses that were pending.
    foreachvalue(Owned<Overlay>& overlay, overlays) {
      overlay->reset();
    }

    // We need to de-allocate the VTEP MAC and VTEP addresses
    // allocated to the Agent as well.
    vtep.reset();
  }
};


class Manager : public Anonymous
{
public:
  static Try<Manager*> createManager(const MasterConfig& masterConfig)
  {
    Try<Owned<ManagerProcess>> process =
      ManagerProcess::createManagerProcess(masterConfig);

    if (process.isError()) {
      return Error(
          "Unable to create the `Manager` process: " +
          process.error());
    }

    return new Manager(process.get());
  }

  virtual ~Manager()
  {
    terminate(process.get());
    wait(process.get());
  }

private:
  Manager(Owned<ManagerProcess> _process)
  : process(_process)
  {
    spawn(process.get());
  }

  Owned<ManagerProcess> process;
};

} // namespace master
} // namespace overlay
} // namespace modules
} // namespace mesos


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
          << "Unable to prase the Master JSON configuration: "
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
    NULL,
    createOverlayMasterManager);

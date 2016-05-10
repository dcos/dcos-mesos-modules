/**
 * This file is © 2014 Mesosphere, Inc. (“Mesosphere”). Mesosphere
 * licenses this file to you solely pursuant to the following terms
 * (and you may not use this file except in compliance with such
 * terms):
 *
 * 1) Subject to your compliance with the following terms, Mesosphere
 * hereby grants you a nonexclusive, limited, personal,
 * non-sublicensable, non-transferable, royalty-free license to use
 * this file solely for your internal business purposes.
 *
 * 2) You may not (and agree not to, and not to authorize or enable
 * others to), directly or indirectly:
 *   (a) copy, distribute, rent, lease, timeshare, operate a service
 *   bureau, or otherwise use for the benefit of a third party, this
 *   file; or
 *
 *   (b) remove any proprietary notices from this file.  Except as
 *   expressly set forth herein, as between you and Mesosphere,
 *   Mesosphere retains all right, title and interest in and to this
 *   file.
 *
 * 3) Unless required by applicable law or otherwise agreed to in
 * writing, Mesosphere provides this file on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied,
 * including, without limitation, any warranties or conditions of
 * TITLE, NON-INFRINGEMENT, MERCHANTABILITY, or FITNESS FOR A
 * PARTICULAR PURPOSE.
 *
 * 4) In no event and under no legal theory, whether in tort (including
 * negligence), contract, or otherwise, unless required by applicable
 * law (such as deliberate and grossly negligent acts) or agreed to in
 * writing, shall Mesosphere be liable to you for damages, including
 * any direct, indirect, special, incidental, or consequential damages
 * of any character arising as a result of these terms or out of the
 * use or inability to use this file (including but not limited to
 * damages for loss of goodwill, work stoppage, computer failure or
 * malfunction, or any and all other commercial damages or losses),
 * even if Mesosphere has been advised of the possibility of such
 * damages.
 */

#include <stdio.h>

#include <list>

#include <stout/check.hpp>
#include <stout/interval.hpp>
#include <stout/ip.hpp>
#include <stout/json.hpp>
#include <stout/mac.hpp>
#include <stout/os.hpp>
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

#include <overlay/overlay.hpp>
#include <overlay/internal/messages.hpp>

namespace http = process::http;

using std::list;
using std::queue;
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

using mesos::modules::Anonymous;
using mesos::modules::Module;
using mesos::modules::overlay::AgentNetworkInfo;
using mesos::modules::overlay::BackendInfo;
using mesos::modules::overlay::VtepInfo;
using mesos::modules::overlay::VxLANInfo;
using mesos::modules::overlay::internal::AgentRegisteredAcknowledgement;
using mesos::modules::overlay::internal::AgentRegisteredMessage;
using mesos::modules::overlay::internal::RegisterAgentMessage;
using mesos::modules::overlay::internal::UpdateAgentNetworkMessage;
using mesos::Parameters;

using NetworkState = AgentRegisteredMessage::NetworkState;

namespace mesos {
namespace modules {
namespace overlay {
namespace master {

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
    uint32_t endIP = 0xffffffff >> (32 - network.prefix());
    uint32_t endMAC = 0xffffffff >> 8;

    freeIP += (Bound<uint32_t>::closed(1), Bound<uint32_t>::closed(endIP - 1));
    freeMAC += (Bound<uint32_t>::closed(1), Bound<uint32_t>::closed(endMAC - 1));
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

  Try<Nothing> deAllocate(const IPNetwork& _network)
  {
    if (_network.prefix() != network.prefix()) {
      return Error(
          "Cannot free this network because prefix " +
          stringify(_network.prefix()) + " does not match Agent prefix "
          + stringify(network.prefix()) + " of the overlay");
    }

    if (_network.prefix() < network.prefix()) {
      return Error(
          "Cannot free this network since it does not belong "
          " to the overlay subnet");
    }

    uint32_t address = ntohl(_network.address().in().get().s_addr);
    uint32_t mask = ntohl(_network.netmask().in().get().s_addr);

    address &= ~mask;

    freeIP += address;

    return Nothing();
  }

  Try<MAC> allocateMAC()
  {
    if (freeMAC.empty()) {
      return Error("Unable to allocate VTEP MAC due to exhaustion");
    }

    uint32_t _nic = freeMAC.begin()->lower();
    freeMAC -= _nic;

    _nic = htonl(_nic);

    uint8_t* nic = (uint8_t*)&_nic;

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

  Try<Nothing> deAllocate(const MAC& mac)
  {
    if (mac[0] != oui[0] || mac[1] != oui[1] || mac[2] != oui[2]) {
      return Error("Unable to free MAC for an unknown OUI");
    }

    uint32_t _nic ;

    uint8_t* nic = (uint8_t*) &_nic;
    nic[1] = mac[3];
    nic[2] = mac[4];
    nic[3] = mac[5];

    _nic = ntohl(_nic);

    freeMAC += _nic;

    return Nothing();
  }

  // Network allocated to the VTEP.
  IPNetwork network;

  MAC oui;

  IntervalSet<uint32_t> freeIP;
  IntervalSet<uint32_t> freeMAC;
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

  // Cannonical name of the network.
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
  Agent(const UPID& _pid) : pid(_pid) {};

  void addNetwork(const AgentNetworkInfo& network)
  {
    if (networks.contains(network.network().name())) {
      return;
    }

    networks[network.network().name()].mutable_network()->CopyFrom(network);

    networks[network.network().name()]
      .mutable_state()->set_status(AgentRegisteredMessage::NETWORK_PROVISION);
  }

  const list<NetworkState> Networks()
  {
    list<NetworkState> _networks;

    foreachvalue(const NetworkState& network, networks) {
      _networks.push_back(network);
    }

    return _networks;
  }

  void updateNetworkState(const NetworkState& state)
  {
    const string name = state.network().network().name();
    if (!networks.contains(name)) {
      LOG(ERROR) << "Got update for unknown network "
        << state.network().network().name() ;
    }

    networks[name].mutable_state()->set_status(state.state().status());
  }

private:
  const UPID pid;

  // A list of all overlay networks that reside on this agent.
  hashmap<string, NetworkState> networks;
};


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
  ManagerProcess(
      const Option<JSON::Array> _config,
      const net::IPNetwork& vtepSubnet,
      const net::MAC& vtepMACOUI)
    : ProcessBase("overlay-master"),
      config(_config),
      vtep(vtepSubnet, vtepMACOUI)
  {
    if (config.isSome()) {
      foreach (JSON::Value overlay, config->values) {
        Try<JSON::Object> network = overlay.as<JSON::Object>();
        if (network.isError()) {
          EXIT(EXIT_FAILURE) << "Unable to parse overlay config:"
            << network.error();
        }

        LOG(INFO) << "Configuring overlay network:" << network->values["name"];

        const Try<JSON::String> name =
          network->values["name"].as<JSON::String>();
        if (name.isError()) {
          EXIT(EXIT_FAILURE) << "Unable to determine name of overlay network"
            << name.error();
        }

        const Try<JSON::String> _address =
          network->values["subnet"].as<JSON::String>();
        if (_address.isError()) {
          EXIT(EXIT_FAILURE) << "Unable to determine the address space of "
            << "overlay network " << name->value << ": "
            << _address.error();
        }

        const Try<JSON::Number> prefix =
          network->values["prefix"].as<JSON::Number>();
        if (prefix.isError()) {
          EXIT(EXIT_FAILURE) << "Unable to determine the prefix length for"
            << " Agents in the overlay network " << name->value
            << ": " << prefix.error();
        }


        Try<net::IPNetwork> address =
          net::IPNetwork::parse(_address->value, AF_INET);
        if (address.isError()) {
          EXIT(EXIT_FAILURE) << "Unable to determine subnet for network: "
            << address.get();
        }

        Try<Nothing> valid = updateAddressSpace(address.get());

        if (valid.isError()) {
          EXIT(EXIT_FAILURE) << "Overlay networks need to have 'non-overlapping'"
            << "address spaces: " << valid.error();
        }

        networks.emplace(
            name->value,
            Overlay(name->value, address.get(), prefix->as<uint8_t>()));
      }
    }
  }

protected:
  virtual void initialize()
  {
    LOG(INFO) << "Adding route for '" << self().id << "/overlays'";

    route("/overlays",
          OVERLAY_HELP,
          &ManagerProcess::overlays);

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

  void registerAgent(const UPID& pid)
  {
    list<NetworkState> _networks;

    if (!std::get<1>(agents.emplace(pid, pid))) {
      LOG(INFO) << "Agent " << pid << "re-registering";

      _networks = agents.at(pid).Networks();
    } else {
      LOG(INFO) << "Got registration from pid: " << pid;

      Try<net::IPNetwork> vtepIP = vtep.allocateIP();
      if (vtepIP.isError()) {
        LOG(ERROR) << "Unable to get VTEP IP for Agent: " << vtepIP.error();
      }

      Try<net::MAC> vtepMAC = vtep.allocateMAC();
      if (vtepMAC.isError()) {
        LOG(ERROR) << "Unable to get VTEP MAC for Agent: " << vtepMAC.error();
      }

      // Walk through all the overlay networks. Allocate a subnet from
      // each overlay to the Agent. Allocate a VTEP IP and MAC for each
      // agent. Queue the message on the Agent. Finally, ask Master to
      // reliably send these messages to the Agent.
      foreachpair (const string& name, Overlay& overlay, networks) {
        AgentNetworkInfo network;

        network.mutable_network()->set_name(name);
        network.mutable_network()->set_subnet(stringify(overlay.network));

        Try<net::IPNetwork> agentSubnet = overlay.allocate();
        if (agentSubnet.isError()) {
          LOG(ERROR) << "Cannot allocate subnet from overlay "
            << name << " to Agent " << pid;
          continue;
        }
        network.set_subnet(stringify(agentSubnet.get()));

        // Allocate bridges for CNI and Docker.
        Try<Nothing> bridges = allocateBridges(network);

        if (bridges.isError()) {
          LOG(ERROR) << "Unable to allocate bridge for network "
            << name << ": " << bridges.error();
          overlay.free(agentSubnet.get());
          continue;
        }

        VxLANInfo vxlan;
        vxlan.set_vni(1024);

        VtepInfo _vtep;
        _vtep.set_ip(stringify(vtepIP.get()));
        _vtep.set_mac(stringify(vtepMAC.get()));
        _vtep.set_name("vtep1024");

        vxlan.mutable_vtep()->CopyFrom(_vtep);

        BackendInfo backend;
        backend.mutable_vxlan()->CopyFrom(vxlan);

        network.mutable_backend()->CopyFrom(backend);

        agents.at(pid).addNetwork(network);
      }

      _networks = agents.at(pid).Networks();
    }

    // Create the network update message and send it to the Agent.
    UpdateAgentNetworkMessage update;

    foreach(const NetworkState& network, _networks) {
      update.add_networks()->CopyFrom(network.network());
    }

    send(pid, update);
  }

  void agentRegistered(const UPID& from, const AgentRegisteredMessage& message)
  {
    if(agents.contains(from)) {
      LOG(INFO) << "Got ACK for addition of networks from " << from;
      for(int i=0; i < message.networks_size(); i++) {
        agents.at(from).updateNetworkState(message.networks(i));
      }

      send(from, AgentRegisteredAcknowledgement());
    } else {
      LOG(ERROR) << "Got ACK for network message for non-existent PID "
        << from;
    }
  }

  Try<Nothing> allocateBridges(AgentNetworkInfo& _network)
  {
    const string name = _network.network().name();

    Try<IPNetwork> network = net::IPNetwork::parse(
        _network.subnet(),
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

    // Create the CNI bridge.
    Try<IPNetwork> cniSubnet = net::IPNetwork::create((IP(address)), (IP(mask)));

    if (cniSubnet.isError()) {
      return Error(
          "Could not create Mesos subnet for network '" +
          name + "': " + cniSubnet.error());
    }

    // Create the docker bridge.
    Try<IPNetwork> dockerSubnet = net::IPNetwork::create(
        IP(address | (0x1 << (32 - (network->prefix() + 1)))),
        IP(mask));

    if (dockerSubnet.isError()) {
      return Error(
          "Could not create Docker subnet for network '" +
          name + "': " + dockerSubnet.error());
    }

    //Update the bridge info.
    BridgeInfo cniBridgeInfo;
    cniBridgeInfo.set_ip(stringify(cniSubnet.get()));
    cniBridgeInfo.set_name(CNI_BRIDGE_PREFIX + name);
    _network.add_bridges()->CopyFrom(cniBridgeInfo);

    BridgeInfo dockerBridgeInfo;
    dockerBridgeInfo.set_ip(stringify(dockerSubnet.get()));
    dockerBridgeInfo.set_name(DOCKER_BRIDGE_PREFIX + name);
    _network.add_bridges()->CopyFrom(dockerBridgeInfo);

    return Nothing();
  }

  Try<Nothing> updateAddressSpace(const IPNetwork &network)
  {
    uint32_t startIP = ntohl(network.address().in().get().s_addr);
    uint32_t mask = ntohl(network.netmask().in().get().s_addr);
    mask = ~mask;
    uint32_t endIP = startIP | mask;

    Interval<uint32_t> overlaySpace =
      (Bound<uint32_t>::closed(startIP) , Bound<uint32_t> ::closed(endIP));

    if (addressSpace.intersects(overlaySpace)) {
      return Error("Found overlapping address spaces");
    }

    addressSpace += overlaySpace;

    return Nothing();
  }

  Future<http::Response> overlays(const http::Request& request)
  {
    return http::OK("Hello this is the `ManagerProcess`.");
  }

private:
  IntervalSet<uint32_t> addressSpace;

  Option<JSON::Array> config;

  Vtep vtep;

  hashmap<string, Overlay> networks;
  hashmap<UPID, Agent> agents;
};

class Manager : public Anonymous
{
public:
  Manager(
      const string& _vtepSubnet,
      const string& _vtepMACOUI,
      Option<JSON::Array> overlays)
  {
    VLOG(1) << "Spawning process";

    Try<net::IPNetwork> vtepSubnet = net::IPNetwork::parse(_vtepSubnet, AF_INET);
    if (vtepSubnet.isError()) {
      EXIT(EXIT_FAILURE) << "Unable to parse the VTEP Subnet: "
        << vtepSubnet.error();
    }

    vector<string> tokens = strings::split(_vtepMACOUI, ":");
    CHECK_EQ(6, tokens.size());

    uint8_t mac[6];
    for (size_t i = 0; i < tokens.size(); i++) {
      sscanf(tokens[i].c_str(), "%hhx", &mac[i]);
    }

    net::MAC vtepMACOUI(mac);

    process =
      Owned<ManagerProcess>(
          new ManagerProcess(overlays, vtepSubnet.get(), vtepMACOUI));
    spawn(process.get());
  }

  virtual ~Manager()
  {
    VLOG(1) << "Terminating process";

    terminate(process.get());
    wait(process.get());
  }

private:
  Owned<ManagerProcess> process;
};

} // namespace mesos {
} // namespace modules {
} // namespace overlay {
} // namespace master {

using mesos::modules::overlay::master::Manager;
using mesos::modules::overlay::master::ManagerProcess;

// Module "main".
Anonymous* createOverlayMasterManager(const Parameters& parameters)
{
  Option<JSON::Array> overlays = None();
  Option<string> vtepSubnet = None();
  Option<string> vtepMACOUI = None();

  VLOG(1) << "Parameters:";
  foreach (const mesos::Parameter& parameter, parameters.parameter()) {
    VLOG(1) << parameter.key() << ": " << parameter.value();

    if (parameter.key() == "overlays") {
      if (!os::exists(parameter.value())) {
        EXIT(EXIT_FAILURE) << "Unable to find the overlay configuration";
      }

      Try<string> config = os::read(parameter.value());
      if (config.isError()) {
        EXIT(EXIT_FAILURE) << "Unable to read the overlay configuration: "
                           << config.error();
      }

      Try<JSON::Array> _overlays = JSON::parse<JSON::Array>(config.get());

      if (_overlays.isError()) {
        EXIT(EXIT_FAILURE) << "Unable to prase the overlay JSON configuration: "
                           << _overlays.error();
      }

      if (_overlays->values.size() == 0) {
        EXIT(EXIT_FAILURE) << "The overlay manager needs at least one"
                           << "overlay to be specified";
      }

      overlays = _overlays.get();
    }

    if (parameter.key() == "vtep_subnet") {
      vtepSubnet = parameter.value();
    }

    if (parameter.key() == "vtep_mac_oui") {
      vtepMACOUI = parameter.value();
    }
  }

  if (overlays.isNone()) {
    EXIT(EXIT_FAILURE) << "No overlays specified";
  }

  if (vtepSubnet.isNone()) {
    EXIT(EXIT_FAILURE) << "No Vtep subnet specified";
  }

  if (vtepMACOUI.isNone()) {
    EXIT(EXIT_FAILURE) << "No Vtep MAC OUI specified";
  }

  return new Manager(vtepSubnet.get(), vtepMACOUI.get(), overlays);
}


// Declares a helper module named 'Manager'.
Module<Anonymous> com_mesosphere_mesos_OverlayMasterManager(
    MESOS_MODULE_API_VERSION,
    MESOS_VERSION,
    "Mesosphere",
    "kapil@mesosphere.io",
    "Master Overlay Helper Module.",
    NULL,
    createOverlayMasterManager);


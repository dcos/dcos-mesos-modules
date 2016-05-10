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

#include <list>
#include <sstream>
#include <set>
#include <vector>

#include <stout/abort.hpp>
#include <stout/check.hpp>
#include <stout/duration.hpp>
#include <stout/foreach.hpp>
#include <stout/ip.hpp>
#include <stout/json.hpp>
#include <stout/jsonify.hpp>
#include <stout/os.hpp>
#include <stout/os/exists.hpp>
#include <stout/os/write.hpp>
#include <stout/protobuf.hpp>
#include <stout/try.hpp>

#include <process/collect.hpp>
#include <process/defer.hpp>
#include <process/delay.hpp>
#include <process/future.hpp>
#include <process/help.hpp>
#include <process/http.hpp>
#include <process/io.hpp>
#include <process/owned.hpp>
#include <process/pid.hpp>
#include <process/process.hpp>
#include <process/protobuf.hpp>
#include <process/subprocess.hpp>

#include <mesos/master/detector.hpp>
#include <mesos/mesos.hpp>
#include <mesos/module.hpp>
#include <mesos/module/anonymous.hpp>

#include <overlay/overlay.hpp>
#include <overlay/internal/messages.hpp>

namespace http = process::http;
namespace io = process::io;

using std::list;
using std::string;
using std::tuple;
using std::vector;

using net::IP;
using net::IPNetwork;

using process::DESCRIPTION;
using process::Future;
using process::Failure;
using process::HELP;
using process::NO_SETSID;
using process::Owned;
using process::Promise;
using process::Subprocess;
using process::TLDR;
using process::UPID;
using process::USAGE;

using mesos::master::detector::MasterDetector;
using mesos::modules::Anonymous;
using mesos::modules::Module;
using mesos::modules::overlay::AgentNetworkInfo;
using mesos::modules::overlay::AgentInfo;
using mesos::modules::overlay::BridgeInfo;
using mesos::modules::overlay::internal::AgentRegisteredAcknowledgement;
using mesos::modules::overlay::internal::AgentRegisteredMessage;
using mesos::modules::overlay::internal::RegisterAgentMessage;
using mesos::modules::overlay::internal::UpdateAgentNetworkMessage;
using mesos::Parameters;

using NetworkState = AgentRegisteredMessage::NetworkState;

namespace mesos {
namespace modules {
namespace overlay {
namespace agent {

constexpr char MASTER_MANAGER_PROCESS_ID[] = "overlay-master";
constexpr Duration REGISTER_RETRY_INTERVAL_MAX = Minutes(10);
constexpr Duration INITIAL_BACKOFF_PERIOD = Seconds(30);

const string OVERLAY_HELP = HELP(
  TLDR(
    "Show agent network overlay information."),
  USAGE(
    "/network/overlay"),
  DESCRIPTION(
    "Shows the Agent IP, Agent subnet, VTEP IP, VTEP MAC and bridges.",
    ""));


class ManagerProcess : public ProtobufProcess<ManagerProcess>
{
public:
  void doReliableRegistration(Duration maxBackoff)
  {
    if (overlayMaster.isNone() || state != REGISTERING) {
      return;
    }

    RegisterAgentMessage registerMessage;

    // Send registration to the Overlay-master.
    send(overlayMaster.get(), registerMessage);

    // Bound the maximum backoff by 'REGISTER_RETRY_INTERVAL_MAX'.
    maxBackoff = std::min(maxBackoff, REGISTER_RETRY_INTERVAL_MAX);

    // Determine the delay for next attempt by picking a random
    // duration between 0 and 'maxBackoff'.
    Duration _delay = maxBackoff * ((double) ::random() / RAND_MAX);

    VLOG(1) << "Will retry registration in " << _delay
      << " if necessary";

    // Backoff.
    process::delay(
        _delay,
        self(),
        &ManagerProcess::doReliableRegistration,
        maxBackoff * 2);
  }

  void configured(const process::UPID& from)
  {
    state = REGISTERED;
    LOG(INFO) << "Overlay agent: " << self() << " has been configured by"
      << " overlay master: " << overlayMaster.get();

    connected.set(Nothing());
  }

  void exited(const UPID &pid)
  {
    LOG(INFO) << pid << " exited";
    if (master.isSome() && master.get() == pid) {
      LOG(WARNING) << "Master: " << master.get() << " exited."
        << "Waiting for a new master to be detected.";

      if (state != REGISTERING) {
        state = REGISTERING;
        doReliableRegistration(INITIAL_BACKOFF_PERIOD);
      }
    }
  }

  void detected(const Future<Option<MasterInfo>>& _master)
  {
    if (_master.isFailed()) {
      EXIT(EXIT_FAILURE) << "Failed to detect a master: " <<
        _master.failure();
    }

    Option<mesos::MasterInfo> latestMaster = None();

    if (_master.isReady()) {
      latestMaster = _master.get();

      master = UPID(_master.get()->pid());
      link(master.get());

      overlayMaster = master;
      overlayMaster->id = MASTER_MANAGER_PROCESS_ID;

      LOG(INFO) << "Querying " << overlayMaster.get()
        << " to get overlay configuration";

      state = REGISTERING;
      doReliableRegistration(INITIAL_BACKOFF_PERIOD);
    }

    // Keep detecting new Masters.
    LOG(INFO) << "Detecting new overlay-master";

    detector->detect(latestMaster)
      .onAny(defer(self(), &ManagerProcess::detected, lambda::_1));
  }

  Try<Nothing> createCNI( const string& name, const IPNetwork& network)
  {
    auto cni = [name, network](JSON::ObjectWriter* writer) {
      writer->field("name", name);
      writer->field("type", "bridge");
      writer->field("bridge", CNI_BRIDGE_PREFIX + name);
      writer->field("isGateway", true);
      writer->field("ipMasq", true);

      writer->field("ipam", [network](JSON::ObjectWriter*writer) {
          writer->field("type", "host-local");
          writer->field("subnet", stringify(network));

          writer->field("routes", [](JSON::ArrayWriter* writer) {
            writer->element([](JSON::ObjectWriter* writer) {
              writer->field("dst", "0.0.0.0/0");
              });
            });
          });
    };

    string cniConfig(jsonify(cni));

    return os::write(path::join(cniDir, name +".cni"), cniConfig);
  }

  Future<bool> checkDockerNetwork(const string& name)
  {
    vector<string> argv = {
      "docker",
      "network",
      "inspect",
      DOCKER_BRIDGE_PREFIX + name};

    Try<Subprocess> s = subprocess(
        "docker",
        argv,
        Subprocess::PATH("/dev/null"),
        Subprocess::PATH("/dev/null"),
        Subprocess::PIPE(),
        NO_SETSID,
        None());

    if (s.isError()) {
      return Failure("Unable to execute docker command: " + s.error());
    }

    return s->status()
      .then([name](const Future<Option<int>>& status) -> Future<bool> {
        if (!status.isReady()) {
          return Failure(
            "Failed to get the exit status of 'docker network inspect': " +
            (status.isFailed() ? status.failure() : "discarded"));
        }

        if (status->isNone()) {
          return Failure("Failed to reap the docker subprocess");
        }

        if (status.get() != 0) {
          return  false;
        }

        return true;
      });
  }

  Future<Nothing> createDockerNetwork(
      const string& name,
      const net::IPNetwork& network)
  {
    // First check if the docker network exists before trying to create
    // the network.
    return checkDockerNetwork(name)
      .then(defer(self(),
            &ManagerProcess::_createDockerNetwork,
            name,
            network,
            lambda::_1));
  }

  Future<Nothing> _createDockerNetwork(
      const string& name,
      const IPNetwork& network,
      const Future<bool> exists)
  {
    if (!exists.isReady()) {
      return Failure(
          "Failed to check status of docker network '" + name + "': " +
          (exists.isFailed() ? exists.failure() : "discarded"));
    }

    if (exists.get()) {
      LOG(INFO) << "Docker bridge for network '"
                << name << "' already configured.";
      return Nothing();
    }

    vector<string> argv = {
      "docker",
      "network",
      "create",
      "--driver=bridge",
      "--subnet=" + stringify(network),
      DOCKER_BRIDGE_PREFIX + name };


    Try<Subprocess> s = subprocess(
        "docker",
        argv,
        Subprocess::PATH("/dev/null"),
        Subprocess::PATH("/dev/null"),
        Subprocess::PIPE(),
        NO_SETSID,
        None());

    if (s.isError()) {
      return Failure("Unable to execute docker command: " + s.error());
    }

    return await(s->status(), io::read(s->err().get()))
      .then([name](const tuple<
            Future<Option<int>>,
            Future<string>>& t) -> Future<Nothing> {
        Future<Option<int>> status = std::get<0>(t);
        if (!status.isReady()) {
          return Failure(
            "Failed to get the exit status of 'docker network create': " +
            (status.isFailed() ? status.failure() : "discarded"));
        }

        if (status->isNone()) {
          return Failure("Failed to reap the docker subprocess");
        }

        Future<string> err = std::get<1>(t);
        if (!err.isReady()) {
          return Failure(
            "Failed to read stderr from the docker subprocess: " +
            (err.isFailed() ? err.failure() : "discarded"));
        }

        if (status.get() != 0) {
          return Failure(
              "Failed to user-defined docker network with name '" +
              name +"': " + err.get());
        }

        return Nothing();
      });
  }

  Future<Nothing> configureNetwork(const AgentNetworkInfo& network)
  {
    const string name = network.info().name();

    // Make sure the network exists in the database.
    if (!networks.contains(name)) {
      return Failure("Cannot find the network '" + name + "'");
    }

    Option<BridgeInfo> cniBridge = None();
    Option<BridgeInfo> dockerBridge = None();

    for (int i =0; i < network.bridges_size(); i++) {
      if (strings::contains(network.bridges(i).name(), CNI_BRIDGE_PREFIX)) {
        cniBridge = network.bridges(i);
      }

      if (strings::contains(network.bridges(i).name(), DOCKER_BRIDGE_PREFIX)) {
        dockerBridge = network.bridges(i);
      }
    }

    if (cniBridge.isNone()) {
      return _configureNetwork(
          name,
          Failure(
            "Cannot configure network " + name +
            " due to missing CNI bridge"));
    }

    if (dockerBridge.isNone()) {
      return _configureNetwork(
          name,
          Failure(
            "Cannot configure network " + name +
            " due to missing docker bridge"));
    }

    // Create the CNI network.
    Try<IPNetwork> mesosSubnet =
      IPNetwork::parse(cniBridge.get().ip(), AF_INET);

    if (mesosSubnet.isError()) {
      return _configureNetwork(
          name,
          Failure(
            "Could not create Mesos subnet for network '" +
            network.info().name() + "': " +
            mesosSubnet.error()));
    }

    Try<Nothing> cni = createCNI(network.info().name(), mesosSubnet.get());

    if (cni.isError()) {
      return _configureNetwork(
          name,
          Failure(
            "Unabled to create CNI config: " +
            cni.error()));
    }

    // Create the docker network.
    Try<IPNetwork> dockerSubnet =
      net::IPNetwork::parse(dockerBridge.get().ip(), AF_INET);

    if (dockerSubnet.isError()) {
      return _configureNetwork(
          name,
          Failure(
            "Could not create Docker subnet for network '" +
            network.info().name() + "': " +
            dockerSubnet.error()));
    }

    return createDockerNetwork(network.info().name(), dockerSubnet.get())
      .onAny(defer(self(),
            &ManagerProcess::_configureNetwork,
            network.info().name(),
            lambda::_1));
  }

  Future<Nothing> _configureNetwork(
      const string name,
      const Future<Nothing> result)
  {
    if (!networks.contains(name)) {
      return Failure("Cannot find the network '" + name +"'");
    }

    if (result.isDiscarded() || result.isFailed()) {
      networks[name].mutable_state()->set_status(
          AgentRegisteredMessage::NETWORK_FAILED);
      networks[name].mutable_state()->set_error(
          (result.isFailed() ?  result.failure() : "discarded"));
      // The fact that we couldn't configure the network is not
      // considered a `Failure`. Since, we have successfully added the
      // network to the database we should return a success.
      return Nothing();
    }

    networks[name].mutable_state()->set_status(
        AgentRegisteredMessage::NETWORK_OK);

    return Nothing();
  }

  void addNetworks(
      const process::UPID& from,
      const UpdateAgentNetworkMessage& config)
  {
    list<Future<Nothing>> futures;

    for (int i = 0; i < config.networks_size(); i++) {
      const AgentNetworkInfo& network = config.networks(i);

      const string name = network.info().name();
      LOG(INFO) << "Configuring network '" << name << "' received from " << from;

      if (!networks.contains(name) ||
          (networks.at(name).state().status() ==
           AgentRegisteredMessage::NETWORK_FAILED)) {
        networks[name].mutable_network()->CopyFrom(network);

        futures.push_back(configureNetwork(networks[name].network()));
      } else {
        LOG(WARNING) << "Network '" << name
                     << "' already configured on agent " << self();
      }
    }

    // Wait for all the networks to be configured.
    await(futures)
      .onAny(defer(
            self(),
            &ManagerProcess::_addNetworks));
  }

  void _addNetworks()
  {
    if (overlayMaster.isNone()) {
      LOG(WARNING) << "Dropping ACK for network message due to unknown Master";
      return;
    }

    AgentRegisteredMessage ack;

    foreachvalue(const AgentRegisteredMessage::NetworkState& network, networks) {
      ack.add_networks()->CopyFrom(network);

      LOG(INFO) << "Acknowledging network '"
        << network.network().info().name() << "' with state: "
        << (network.state().status() ==
            AgentRegisteredMessage::NETWORK_OK ?  "OK" : "ERROR");
    }

    send(overlayMaster.get(), ack);
  }

  ManagerProcess(MasterDetector* _detector, const string& _cniDir)
    : ProcessBase("overlay-agent"),
    cniDir(_cniDir),
    detector(_detector){}

  void initialize()
  {
    LOG(INFO) << "Adding route for '" << self().id << "/overlay'";

    route("/overlay",
        OVERLAY_HELP,
        &ManagerProcess::overlay);

    state = REGISTERING;
    detector->detect()
      .onAny(defer(self(), &ManagerProcess::detected, lambda::_1));

    // Install message handlers.
    install<UpdateAgentNetworkMessage>(&ManagerProcess::addNetworks);
    install<AgentRegisteredAcknowledgement>(&ManagerProcess::configured);
  }

  Future<Nothing> ready()
  {
    return connected.future();
  }

  Future<http::Response> overlay(const http::Request& request)
  {
    AgentInfo agent;
    agent.set_ip(stringify(self().address));

    foreachvalue(const NetworkState& network, networks) {
      agent.add_networks()->CopyFrom(network.network());
    }

    return http::OK(
        JSON::protobuf(agent),
        request.url.query.get("jsonp"));
  }

private:
  enum State {
    REGISTERING = 1,
    REGISTERED = 2,
  };

  State  state;

  const string cniDir;

  Option<UPID> overlayMaster;

  Option<UPID> master;

  Owned<MasterDetector> detector;

  hashmap<string, NetworkState> networks;

  Promise<Nothing> connected;
};


class Manager : public Anonymous
{
public:
  Manager(const string& master, const string& cniDir)
  {
    CHECK(os::exists(cniDir));

    Try<MasterDetector*> detector = MasterDetector::create(master);
    if (detector.isError()) {
      LOG(ERROR) << "Unable to create the `MasterDetector`: "
        << detector.error();
      abort();
    }

    VLOG(1) << "Spawning process";

    process =
      Owned<ManagerProcess>(new ManagerProcess(detector.get(), cniDir));
    spawn(process.get());

    // Wait for the overlay-manager to be ready before
    // allowing the Agent to proceed.
    Future<Nothing> ready = process->ready();
    ready.await();

    LOG(INFO) << "Overlay is ready for operation.";
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
} // namespace agent {


using mesos::modules::overlay::agent::Manager;
using mesos::modules::overlay::agent::ManagerProcess;

// Module "main".
Anonymous* createOverlayAgentManager(const Parameters& parameters)
{
  string master;
  Option<string> cniDir = None();

  VLOG(1) << "Parameters:";

  foreach (const mesos::Parameter& parameter, parameters.parameter()) {
    VLOG(1) << parameter.key() << ": " << parameter.value();

    if (parameter.key() == "master") {
      master = parameter.value();
    }

    if (parameter.key() == "cni_dir") {
      cniDir = parameter.value();
    }
  }

  if (cniDir.isNone()) {
    EXIT(EXIT_FAILURE) << "Missing parameter 'cni_dir'";
  }

  return new Manager(master, cniDir.get());
}


// Declares a helper module named 'Manager'.
Module<Anonymous> com_mesosphere_mesos_OverlayAgentManager(
    MESOS_MODULE_API_VERSION,
    MESOS_VERSION,
    "Mesosphere",
    "kapil@mesosphere.io",
    "Agent Overlay Helper Module.",
    NULL,
    createOverlayAgentManager);

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

#include <mesos/http.hpp>
#include <mesos/master/detector.hpp>
#include <mesos/mesos.hpp>
#include <mesos/module.hpp>
#include <mesos/module/anonymous.hpp>

#include "agent.hpp"
#include "constants.hpp"
#include "messages.hpp"
#include "overlay.hpp"

#include "common/shell.hpp"


namespace http = process::http;
namespace io = process::io;
namespace network = process::network;

typedef mesos::modules::overlay::AgentOverlayInfo::State OverlayState;

using std::list;
using std::string;
using std::tuple;
using std::vector;

using net::IP;

using process::delay;

using process::DESCRIPTION;
using process::Future;
using process::Failure;
using process::HELP;
using process::Owned;
using process::Promise;
using process::Subprocess;
using process::TLDR;
using process::UPID;
using process::USAGE;

using mesos::Parameters;

using mesos::master::detector::MasterDetector;

using mesos::modules::common::runScriptCommand;

using mesos::modules::Anonymous;
using mesos::modules::Module;
using mesos::modules::overlay::AgentOverlayInfo;
using mesos::modules::overlay::AgentInfo;
using mesos::modules::overlay::BridgeInfo;
using mesos::modules::overlay::MESOS_MASTER;
using mesos::modules::overlay::MESOS_ZK;
using mesos::modules::overlay::internal::AgentConfig;
using mesos::modules::overlay::internal::AgentNetworkConfig;
using mesos::modules::overlay::internal::AgentRegisteredAcknowledgement;
using mesos::modules::overlay::internal::AgentRegisteredMessage;
using mesos::modules::overlay::internal::RegisterAgentMessage;
using mesos::modules::overlay::internal::UpdateAgentOverlaysMessage;

namespace mesos {
namespace modules {
namespace overlay {
namespace agent {

constexpr Duration REGISTRATION_RETRY_INTERVAL_MAX = Minutes(10);
constexpr Duration INITIAL_BACKOFF_PERIOD = Seconds(5);


static string OVERLAY_HELP()
{
  return HELP(
      TLDR(
          "Show the agent network overlay information."),
      DESCRIPTION(
          "Shows the Agent IP, Agent subnet, VTEP IP, VTEP MAC and bridges."));
}


Try<Owned<ManagerProcess>> ManagerProcess::create(
    const AgentConfig& agentConfig)
{
  Option<string> master = None();
  if (master.isNone() && agentConfig.has_master()) {
    master = agentConfig.master();
  } else {
    master = os::getenv(MESOS_MASTER);

    // If the agent is running as part of the master it will need to
    // get the ZK URL from 'MESOS_ZK'.
    if (master.isNone()) {
      master = os::getenv(MESOS_ZK);
    }
  }

  // We should have learned about the master either from the JSON
  // config of the module, or from the 'MESOS_MASTER' environment
  // variable.
  if (master.isNone()) {
    return Error(
        "Master unspecified, hence cannot create a "
        "`MasterDetector`. Please specify master either through "
        "the JSON config, or 'MESOS_MASTER' environment variable");
  }

  Try<MasterDetector*> detector = MasterDetector::create(master.get());
  if (detector.isError()) {
    return Error("Unable to create master detector: " + detector.error());
  }

  Try<Nothing> mkdir = os::mkdir(agentConfig.cni_dir());
  if (mkdir.isError()) {
    return Error("Failed to create CNI config directory: " + mkdir.error());
  }

  AgentNetworkConfig networkConfig;
  if (agentConfig.has_network_config()) {
      networkConfig.CopyFrom(agentConfig.network_config());
  }

  // It is imperative that MASQUERADE rules are not enforced on
  // overlay traffic. To ensure that overlay traffic is not NATed,
  // the Agent module disables masquerade on Docker and Mesos
  // network, and installs MASQUERADE rules during initialization.
  // The MASQUERADE rule works as follows:
  // * The Agent creates an 'ipset' which will hold all the overlay
  // subnets that are configured on the agent.
  // * The Agent inserts an 'iptables' rule that uses the 'ipset' to
  // ensure that traffic destined to any of the overlay subnet is
  // not masqueraded.
  //
  // NOTE: We need to serialize the execution of the 'ipset' command
  // to ensure that the MASQUERADE rules have been installed
  // correctly before allowing the creation of the
  // `ManagerProcess`.
  //
  // NOTE: We should set up the `ipset` only if `mesos_bridge` or
  // `docker_bridge` have been enabled in the `AgentNetworkConfig`.
  if (networkConfig.mesos_bridge() || networkConfig.docker_bridge()) {
    Try<string> ipsetCommand = strings::format(
        "ipset create -exist %s hash:net counters && "
        "ipset add -exist %s 0.0.0.0/1 && "
        "ipset add -exist %s 128.0.0.0/1 && "
        "ipset add -exist %s 127.0.0.0/1",
        IPSET_OVERLAY,
        IPSET_OVERLAY,
        IPSET_OVERLAY,
        IPSET_OVERLAY);

    if (ipsetCommand.isError()) {
      return Error(
          "Unable to create `ipset` command: " + ipsetCommand.error());
    }

    Future<string> ipset = runScriptCommand(ipsetCommand.get());

    ipset.await();

    if (!ipset.isReady()) {
      return Error(
          "Unable to create ipset:" +
          (ipset.isFailed() ? ipset.failure() : "discarded"));
    }
  }

  return Owned<ManagerProcess>(
      new ManagerProcess(
        agentConfig.cni_dir(),
        networkConfig,
        agentConfig.max_configuration_attempts(),
        Owned<MasterDetector>(detector.get())));
}


Future<Nothing> ManagerProcess::ready()
{
  return connected.future();
}


void ManagerProcess::initialize()
{
  LOG(INFO) << "Initializing overlay agent manager";

  route("/overlay",
      OVERLAY_HELP(),
      &ManagerProcess::overlay);

  state = REGISTERING;

  detector->detect()
    .onAny(defer(self(), &ManagerProcess::detected, lambda::_1));

  // Install message handlers.
  install<UpdateAgentOverlaysMessage>(
      &ManagerProcess::updateAgentOverlays);

  install<AgentRegisteredAcknowledgement>(
      &ManagerProcess::agentRegisteredAcknowledgement);
}


void ManagerProcess::exited(const UPID& pid)
{
  LOG(INFO) << "Overlay master " << pid << " has exited";

  if (overlayMaster.isSome() && overlayMaster.get() == pid) {
    LOG(WARNING) << "Overlay master disconnected! "
                 << "Waiting for a new overlay master to be detected";
  }

  LOG(INFO) << "Moving " << pid << " to `REGISTERING` state.";

  state = REGISTERING;
  doReliableRegistration(INITIAL_BACKOFF_PERIOD);
}

void ManagerProcess::updateAgentOverlays(
    const UPID& from,
    const UpdateAgentOverlaysMessage& message)
{
  LOG(INFO) << "Received 'UpdateAgentOverlaysMessage' from " << from;

  if (state != REGISTERING) {
    LOG(WARNING) << "Ignored 'UpdateAgentOverlaysMessage' from " << from
                 << " because overlay agent is not in DISCONNECTED state";
    return;
  }

  list<Future<Nothing>> futures;
  foreach (const AgentOverlayInfo& overlay, message.overlays()) {
    const string name = overlay.info().name();

    LOG(INFO) << "Configuring overlay network '" << name << "'";

    if (overlays.contains(name) &&
        overlays.at(name).has_state() &&
        overlays.at(name).state().has_status() ) {
      OverlayState::Status status = overlays.at(name).state().status();

      // Even if a single overlay network is in `STATUS_CONFIGURING`
      // we need to drop processing of this message since a network
      // configuration is in progress and we can't update the master
      // till all network configuration has been attempted.
      if (status == OverlayState::STATUS_CONFIGURING) {
        LOG(INFO) << "Since overlay network '"
                  << name << "' is in 'STATUS_CONFIGURING' dropping"
                  << " this `UpdateAgentOverlaysMessage', since an overlay"
                  << " network configuration is pending.";

          return;
      }

      // Here, we assume that the overlay configuration never changes.
      // Therefore, if the overlay is in `STATUS_OK`, we will skip the
      // configuration.
      if (status == OverlayState::STATUS_OK) {
        LOG(INFO) << "Skipping configuration for overlay network '"
                  << name << "' as it has been configured.";

        // We still set a `Future` for this overlay, so as to inform
        // the Master about the state of this overlay network.
        //
        // NOTE: This is important for the case of a master restart.
        // During a master restart the agent would have already had
        // all the overlays configured but it will still need to
        // register with the new master by updating the new master with
        // the state of the configured overlays.
        futures.push_back(Nothing());
        continue;
      }
    }

    CHECK(!overlay.has_state());

    overlays[name] = overlay;

    futures.push_back(configure(name));
  }

  // If we don't have any `Futures` setup that means this was a
  // duplicate update corresponding to one already in progress. We
  // should therefore not setup a response for acknowledging this
  // registration.
  if (futures.empty()) {
    LOG(INFO) << "Looks like we received a duplicate config update from "
              << from << " dropping this message.";
    return;
  }

  // Wait for all the networks to be configured.
  await(futures)
    .onAny(defer(self(),
          &ManagerProcess::_updateAgentOverlays,
          lambda::_1));
}


void ManagerProcess::_updateAgentOverlays(
    const Future<list<Future<Nothing>>>& results)
{
  if (!results.isReady()) {
    LOG(ERROR) << "Unable to configure any overlay: "
               << (results.isDiscarded() ? "discarded" : results.failure());

    return;
  }

  vector<string> messages;
  // Check if there were any failures while configuring the
  // overlays.
  foreach(const Future<Nothing>& result, results.get()) {
    if (!result.isReady()) {
      messages.push_back(
          (result.isDiscarded() ? "discarded" : result.failure()));
    }
  }

  if (!messages.empty()){
    LOG(ERROR) << "Unable to configure some of the overlays on this Agent: "
               << strings::join("\n", messages);
  }

  if (state != REGISTERING) {
    LOG(WARNING) << "Ignored sending registered message because "
                 << "agent is not in REGISTERING state";
    return;
  }

  CHECK_SOME(overlayMaster);

  AgentRegisteredMessage message;

  foreachvalue (const AgentOverlayInfo& overlay, overlays) {
    // Every overlay network should have a status, and it should be
    // either in `STATUS_OK` or `STATUS_FAILED`.
    CHECK(overlay.has_state());
    CHECK(overlay.state().has_status());
    CHECK(overlay.state().status() != OverlayState::STATUS_CONFIGURING);
    CHECK(overlay.state().status() != OverlayState::STATUS_INVALID);

    message.add_overlays()->CopyFrom(overlay);
  }

  LOG(INFO) << "Sending agent registered message to " << overlayMaster.get();

  // NOTE: If this message does not reach the master, the master
  // will keep sending 'UpdateAgentOverlaysMessage', which will
  // essentially cause a retry of sending this message.
  send(overlayMaster.get(), message);

  return;
}


void ManagerProcess::agentRegisteredAcknowledgement(const UPID& from)
{
  LOG(INFO) << "Received agent registered acknowledgment from " << from;

  configAttempts++;

  if (configAttempts > maxConfigAttempts) {
    LOG(ERROR) << "Could not configure some overlay networks after "
               << configAttempts
               << " attempts hence moving to 'REGISTERED' state with "
               << "erroneous overlay configuration";
  } else {
    // We move into `REGISTERED` state only if all overlays have been
    // configured correctly. Dropping the register acknowledgment
    // will force the agent to re-register, trying to re-configure the
    // network again.
    foreachvalue (const AgentOverlayInfo& overlay, overlays) {
      if (!overlay.has_state() ||
          !overlay.state().has_status() ||
          overlay.state().status() != OverlayState::STATUS_OK) {
        LOG(ERROR) << "Overlay " << overlay.info().name() << " has not been "
          << "configured hence dropping register "
          << "acknowledgment from master.";
        return;
      }
    }
  }

  state = REGISTERED;

  connected.set(Nothing());
}


void ManagerProcess::detected(const Future<Option<MasterInfo>>& mesosMaster)
{
  if (mesosMaster.isFailed()) {
    EXIT(EXIT_FAILURE)
      << "Failed to detect Mesos master: "
      << mesosMaster.failure();
  }

  // Need to reset the `configAttempts`, so that we can try
  // configuring overlay networks, learned from the new Master, on
  // this agent.
  configAttempts = 0;
  state = REGISTERING;

  Option<MasterInfo> latestMesosMaster = None();

  if (mesosMaster.isDiscarded()) {
    LOG(INFO) << "Re-detecting Mesos master";
    overlayMaster = None();
  } else if (mesosMaster.get().isNone()) {
    LOG(INFO) << "Lost leading Mesos master";
    overlayMaster = None();
  } else {
    latestMesosMaster = mesosMaster.get();

    Try<net::IP> ip = net::IP::parse(mesosMaster.get()->address().ip());
    if (ip.isError()) {
      EXIT(EXIT_FAILURE)
        << "Failed to parse the IP address of the leading Master: "
        << ip.error();
    }

    overlayMaster = UPID(
        MASTER_MANAGER_PROCESS_ID,
        network::inet::Address(ip.get(), mesosMaster.get()->address().port()));

    link(overlayMaster.get());

    LOG(INFO) << "Detected new overlay master at " << overlayMaster.get();

    doReliableRegistration(INITIAL_BACKOFF_PERIOD);
  }

  // Keep detecting new Mesos masters.
  detector->detect(latestMesosMaster)
    .onAny(defer(self(), &ManagerProcess::detected, lambda::_1));
}


void ManagerProcess::doReliableRegistration(Duration maxBackoff)
{
  if (state == REGISTERED) {
    LOG(INFO) << "Overlay agent is already in REGISTERED state";
    return;
  }

  if (overlayMaster.isNone()) {
    LOG(INFO) << "Overlay master is unknown, ignored registration";
    return;
  }

  RegisterAgentMessage registerMessage;
  registerMessage.mutable_network_config()->CopyFrom(networkConfig);

  // Send registration to the overlay master.
  LOG(INFO) << "Sending registration message to master: "
            << overlayMaster.get();

  send(overlayMaster.get(), registerMessage);

  // Bound the maximum backoff by 'REGISTRATION_RETRY_INTERVAL_MAX'.
  maxBackoff = std::min(maxBackoff, REGISTRATION_RETRY_INTERVAL_MAX);

  // Determine the delay for next attempt by picking a random
  // duration between 0 and 'maxBackoff'.
  Duration backoff = maxBackoff * ((double) ::random() / RAND_MAX);

  VLOG(1) << "Will retry registration in " << backoff << " if necessary";

  delay(backoff,
      self(),
      &ManagerProcess::doReliableRegistration,
      maxBackoff * 2);
}


Future<http::Response> ManagerProcess::overlay(const http::Request& request)
{
  AgentInfo agent;
  agent.set_ip(stringify(self().address.ip));

  foreachvalue (const AgentOverlayInfo& overlay, overlays) {
    agent.add_overlays()->CopyFrom(overlay);
  }

  if (request.acceptsMediaType(APPLICATION_JSON)) {
    return http::OK(
        JSON::protobuf(agent),
        request.url.query.get("jsonp"));
  } else if (request.acceptsMediaType(APPLICATION_PROTOBUF)){
    ContentType responseContentType = ContentType::PROTOBUF;

    http::OK ok(agent.SerializeAsString());
    ok.headers["Content-Type"] = stringify(responseContentType);

    return ok;
  } else {
    return http::UnsupportedMediaType(
        string("Client needs to support either ") +
        APPLICATION_JSON + " or " + APPLICATION_PROTOBUF);
  }
}


Future<Nothing> ManagerProcess::configure(const string& name)
{
  CHECK(overlays.contains(name));

  overlays[name].mutable_state()->set_status(
      OverlayState::STATUS_CONFIGURING);

  return await(configureMesosNetwork(name),
      configureDockerNetwork(name))
    .then(defer(self(),
          &Self::_configure,
          name,
          lambda::_1));
}


Future<Nothing> ManagerProcess::_configure(
    const string& name,
    const tuple<Future<Nothing>, Future<Nothing>>& t)
{
  CHECK(overlays.contains(name));

  Future<Nothing> mesos = std::get<0>(t);
  Future<Nothing> docker = std::get<1>(t);

  vector<string> errors;

  if (!mesos.isReady()) {
    errors.push_back((mesos.isFailed() ? mesos.failure() : "discarded"));
  }

  if (!docker.isReady()) {
    errors.push_back((docker.isFailed() ? docker.failure() : "discarded"));
  }

  auto overlaySuccess = [=](const Future<string>& result) -> Future<Nothing> {
    CHECK(overlays.contains(name));
    overlays[name].mutable_state()->set_status(OverlayState::STATUS_OK);

    return Nothing();
  };

  auto overlayFailure = [=](const string& error) {
    CHECK(overlays.contains(name));

    OverlayState* state = overlays[name].mutable_state();
    state->set_status(OverlayState::STATUS_FAILED);
    state->set_error(error);
  };

  if (!errors.empty()) {
    overlayFailure(strings::join(";", errors));

    return Failure(strings::join(";", errors));
  }

  // The Mesos and Docker Networks have been configured. Setup the
  // ipset rule in `IPSET_OVERLAY`. If the ipset rule succeeds
  // proceed to check the iptables rule exists for masquerading
  // traffic from the overlay subnet. If the iptables rule does not
  // exist insert one in the POSTROUTING chain of the NAT table.
  // The below command is a script consisting of three commands:
  // <set ipset> && <check iptables rule exists> ||
  // <insert iptables rule>
  if (!networkConfig.mesos_bridge() &&
      !networkConfig.docker_bridge()) {
    return overlaySuccess("");
  } else {
    const string overlaySubnet = overlays[name].info().subnet();

    Try<string> command = strings::format(
        "ipset add -exist %s %s" " nomatch &&"
        " iptables -t nat -C POSTROUTING -s %s -m set"
        " --match-set %s dst -j MASQUERADE ||"
        " iptables -t nat -A POSTROUTING -s %s -m"
        " set --match-set %s dst -j MASQUERADE",
        IPSET_OVERLAY,
        overlaySubnet,
        overlaySubnet,
        IPSET_OVERLAY,
        overlaySubnet,
        IPSET_OVERLAY);

    if (command.isError()) {
      overlayFailure(command.error());

      return Failure(
          "Unable to create iptables rule for overlay " +
          name + ": " + command.error());
    }

    LOG(INFO) << "Insert following iptables rule for overlay " << name
              << ": " << command.get();

    // We have to explicitly chain the `onFailed` and `onDiscarded`
    // events since we need to update the `State` of the overlay
    // network on failure to execute the iptables script.
    //
    // NOTE: If we use `onAny` instead of `onFailed` and
    // `onDiscarded`, and handle all conditions (success as well as
    // failure) in the `onAny` callback, it causes a race with the
    // callback setup in the `await` which listens to this future
    // (checkout `updateAgentOverlays` method). Reason being that
    // when a future is READY, all the READY callbacks are invoked
    // before the `onAny` callbacks are invoked. This can result in
    // the callback setup by `await` being invoked before the
    // `onAny` call we seutp in this `Future`. This can cause
    // problems since we check the overlay `State` in
    // `_updateAgentOverlays`, which might not be set if this race
    // were to occur, even though the overlay configuration went
    // through fine.
    return runScriptCommand(command.get())
      .then(defer(self(), overlaySuccess))
      .onFailed(defer(self(), overlayFailure))
      .onDiscarded(defer(self(), lambda::bind(overlayFailure, "discarded")));
  }
}


Future<Nothing> ManagerProcess::configureMesosNetwork(const string& name)
{
  CHECK(overlays.contains(name));

  const AgentOverlayInfo& overlay = overlays[name];

  if (!networkConfig.mesos_bridge()) {
    LOG(INFO) << "Not configuring Mesos network for '" << name
              << "' since operator has disallowed `mesos_bridge`.";

    if (overlay.has_mesos_bridge()) {
      LOG(WARNING) << " We are ignoring request from Master to configure "
                   << "`mesos_bridge` for " << name
                   << " since operator has not configured agent to configure "
                   << "`mesos_bridge`.";
    }
    return Nothing();
  }

  Try<net::IP::Network> subnet = net::IP::Network::parse(
      overlay.mesos_bridge().ip(),
      AF_INET);

  if (subnet.isError()) {
    return Failure("Failed to parse bridge ip: " + subnet.error());
  }

  AgentNetworkConfig _networkConfig;
  _networkConfig.CopyFrom(networkConfig);

  auto config = [name, subnet, overlay, _networkConfig](
      JSON::ObjectWriter* writer) {
    writer->field("name", name);
    writer->field("type", "mesos-cni-port-mapper");
    writer->field("excludeDevices", 
      [overlay](JSON::ArrayWriter* writer) { 
        writer->element(overlay.mesos_bridge().name());
    });
    writer->field("chain", "OVERLAY-DEFAULT-BRIDGE"),
    writer->field("delegate", 
      [subnet, overlay, _networkConfig](JSON::ObjectWriter* writer) {
        writer->field("type", "bridge");
        writer->field("bridge", overlay.mesos_bridge().name());
        writer->field("isGateway", true);
        writer->field("ipMasq", false);
        writer->field("mtu", _networkConfig.overlay_mtu());

        writer->field("ipam", [subnet](JSON::ObjectWriter* writer) {
          writer->field("type", "host-local");
          writer->field("subnet", stringify(subnet.get()));

          writer->field("routes", [](JSON::ArrayWriter* writer) {
            writer->element([](JSON::ObjectWriter* writer) {
              writer->field("dst", "0.0.0.0/0");
            });
          });
        });
      });
  };

  Try<Nothing> write = os::write(
      path::join(cniDir, name + ".conf"),
      jsonify(config));

  if (write.isError()) {
    return Failure("Failed to write CNI config: " + write.error());
  }

  return Nothing();
}


Future<Nothing> ManagerProcess::configureDockerNetwork(const string& name)
{
  CHECK(overlays.contains(name));

  if (!networkConfig.docker_bridge()) {
    LOG(INFO) << "Not configuring Docker network for '" << name
              << "' since operator has disallowed `docker_bridge`.";

    if (overlays[name].has_docker_bridge()) {
      LOG(WARNING) << " We are ignoring request from Master to configure "
                   << "`docker_bridge` for " << name
                   << " since operator has not configured agent to configure "
                   << "`docker_bridge`.";
    }
    return Nothing();
  }

  return checkDockerNetwork(name)
    .then(defer(self(),
          &Self::_configureDockerNetwork,
          name,
          lambda::_1));
}


Future<bool> ManagerProcess::checkDockerNetwork(const string& name)
{
  vector<string> argv = {
    "docker",
    "network",
    "inspect",
    name
  };

  Try<Subprocess> s = subprocess(
      "docker",
      argv,
      Subprocess::PATH("/dev/null"),
      Subprocess::PATH("/dev/null"),
      Subprocess::PATH("/dev/null"));

  if (s.isError()) {
    return Failure("Unable to execute docker network inspect: " + s.error());
  }

  return s->status()
    .then([](const Option<int>& status) -> Future<bool> {
        if (status.isNone()) {
        return Failure("Failed to reap the subprocess");
        }

        if (status.get() != 0) {
        return false;
        }

        return true;
        });
}


Future<Nothing> ManagerProcess::_configureDockerNetwork(
    const string& name,
    bool exists)
{
  if (exists) {
    LOG(INFO) << "Docker network '" << name << "' already exists";
    return Nothing();
  }

  CHECK(overlays.contains(name));
  const AgentOverlayInfo& overlay = overlays[name];

  if (!overlay.has_docker_bridge()) {
    return Failure("Missing Docker bridge info");
  }

  Try<net::IP::Network> subnet = net::IP::Network::parse(
      overlay.docker_bridge().ip(),
      AF_INET);

  if (subnet.isError()) {
    return Failure("Failed to parse bridge ip: " + subnet.error());
  }

  Try<string> dockerCommand = strings::format(
      "docker network create --driver=bridge --subnet=%s "
      "--opt=com.docker.network.bridge.name=%s "
      "--opt=com.docker.network.bridge.enable_ip_masquerade=false "
      "--opt=com.docker.network.driver.mtu=%s %s",
      stringify(subnet.get()),
      overlay.docker_bridge().name(),
      stringify(networkConfig.overlay_mtu()),
      name);
  if (dockerCommand.isError()) {
    return Failure(
        "Failed to create docker network command: " +
        dockerCommand.error());
  }

  return runScriptCommand(dockerCommand.get())
    .then(defer(self(),
          &Self::__configureDockerNetwork,
          name,
          lambda::_1));
}


Future<Nothing> ManagerProcess::__configureDockerNetwork(
    const string& name,
    const Future<string> &result)
{
  CHECK(overlays.contains(name));

  if (!result.isReady()) {
    return Failure(
        "Unable to configure docker network: " +
        (result.isDiscarded() ? "discarded" : result.failure()));
  }

  // We want all overlay instances to talk to each other.
  // However, Docker disallows this. So we will install a de-funct
  // rule in the DOCKER-ISOLATION chain to bypass any isolation
  // docker might be trying to enforce.
  const string iptablesCommand = "iptables -D DOCKER-ISOLATION -j RETURN; "
    "iptables -I DOCKER-ISOLATION 1 -j RETURN";

  return runScriptCommand(iptablesCommand)
    .then([]() -> Future<Nothing> {
        return Nothing();
        });
}

ManagerProcess::ManagerProcess(
    const string& _cniDir,
    const AgentNetworkConfig _networkConfig,
    const uint32_t _maxConfigAttempts,
    Owned<MasterDetector> _detector)
: ProcessBase(AGENT_MANAGER_PROCESS_ID),
  cniDir(_cniDir),
  networkConfig(_networkConfig),
  maxConfigAttempts(_maxConfigAttempts),
  detector(_detector)
{
  configAttempts = 0;

  // Make the Manager wait only if we have to configure mesos
  // networks.
  if (networkConfig.mesos_bridge() == false) {
    connected.set(Nothing());
  }
}


Try<Manager*> Manager::create(const AgentConfig& agentConfig)
{
  Try<Owned<ManagerProcess>> process = ManagerProcess::create(agentConfig);
  if (process.isError()) {
    return Error(
        "Unable to create `ManagerProcess`:" +
        process.error());
  }

  return new Manager(process.get());
}


Manager::~Manager()
{
  terminate(process.get());
  wait(process.get());
}


Manager::Manager(const Owned<ManagerProcess>& _process)
  :process(_process)
{
  spawn(process.get());

  LOG(INFO) << "Overlay agent is ready";
}

} // namespace agent {
} // namespace overlay {
} // namespace modules {
} // namespace mesos {


using mesos::modules::overlay::agent::Manager;
using mesos::modules::overlay::agent::ManagerProcess;


Anonymous* createOverlayAgentManager(const Parameters& parameters)
{
  Option<AgentConfig> agentConfig = None();

  foreach (const mesos::Parameter& parameter, parameters.parameter()) {
    LOG(INFO) << "Overlay agent parameter '" << parameter.key()
              << "=" << parameter.value() << "'";

    if (parameter.key() == "agent_config") {
      if (!os::exists(parameter.value())) {
        LOG(ERROR) << "Unable to find Agent configuration: "
                   << parameter.value();

        return nullptr;
      }

      Try<string> config = os::read(parameter.value());
      if (config.isError()) {
        LOG(ERROR) << "Unable to read the Agent "
                   << "configuration: " << config.error();

        return nullptr;
      }

      auto parseAgentConfig = [](const string& s) -> Try<AgentConfig> {
        Try<JSON::Object> json = JSON::parse<JSON::Object>(s);
        if (json.isError()) {
          return Error("JSON parse failed: " + json.error());
        }

        Try<AgentConfig> parse =
          ::protobuf::parse<AgentConfig>(json.get());

        if (parse.isError()) {
          return Error("Protobuf parse failed: " + parse.error());
        }

        return parse.get();
      };

      Try<AgentConfig> _agentConfig = parseAgentConfig(config.get());
      if (_agentConfig.isError()) {
        LOG(ERROR)
          << "Unable to parse the Agent JSON configuration: "
          << _agentConfig.error();

        return nullptr;
      }

      agentConfig = _agentConfig.get();
    }
  }

  if (agentConfig.isNone()) {
    LOG(ERROR) << "Missing `agent_config`";

    return nullptr;
  }

  Try<Manager*> manager = Manager::create(agentConfig.get());
  if (manager.isError()) {
    LOG(ERROR)
      << "Unable to create Agent manager module: "
      << manager.error();

    return nullptr;
  }

  return manager.get();
}


// Declares a helper module named 'Manager'.
Module<Anonymous> com_mesosphere_mesos_OverlayAgentManager(
    MESOS_MODULE_API_VERSION,
    MESOS_VERSION,
    "Mesosphere",
    "help@mesosphere.io",
    "DCOS Overlay Agent Module",
    nullptr,
    createOverlayAgentManager);

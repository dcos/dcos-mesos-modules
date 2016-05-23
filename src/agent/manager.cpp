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

using process::delay;

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

using mesos::Parameters;

using mesos::master::detector::MasterDetector;

using mesos::modules::Anonymous;
using mesos::modules::Module;
using mesos::modules::overlay::AgentOverlayInfo;
using mesos::modules::overlay::AgentInfo;
using mesos::modules::overlay::BridgeInfo;
using mesos::modules::overlay::internal::AgentRegisteredAcknowledgement;
using mesos::modules::overlay::internal::AgentRegisteredMessage;
using mesos::modules::overlay::internal::RegisterAgentMessage;
using mesos::modules::overlay::internal::UpdateAgentOverlaysMessage;

namespace mesos {
namespace modules {
namespace overlay {
namespace agent {

constexpr Duration REGISTRATION_RETRY_INTERVAL_MAX = Minutes(10);
constexpr Duration INITIAL_BACKOFF_PERIOD = Seconds(30);


static string OVERLAY_HELP()
{
  return HELP(
      TLDR(
          "Show the agent network overlay information."),
      DESCRIPTION(
          "Shows the Agent IP, Agent subnet, VTEP IP, VTEP MAC and bridges."));
}


class ManagerProcess : public ProtobufProcess<ManagerProcess>
{
public:
  ManagerProcess(const string& _cniDir, Owned<MasterDetector> _detector)
    : ProcessBase(AGENT_MANAGER_PROCESS_ID),
      cniDir(_cniDir),
      detector(_detector) {}

  Future<Nothing> ready()
  {
    return connected.future();
  }

protected:
  virtual void initialize()
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

  virtual void exited(const UPID& pid)
  {
    LOG(INFO) << "Overlay master " << pid << " has exited";

    if (overlayMaster.isSome() && overlayMaster.get() == pid) {
      LOG(WARNING)
        << "Overlay master disconnected!"
        << "Waiting for a new overlay master to be detected";
    }
  }

  void updateAgentOverlays(
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

      // TODO(jieyu): Here, we assume that the overlay configuration
      // never changes. Therefore, if the overlay is in OK status, we
      // will skip the configuration. This might change soon.
      if (overlays.contains(name)) {
        LOG(INFO) << "Skipping configuration for overlay network '" << name
                  << "' as it is being (or has been) configured";
        continue;
      }

      CHECK(!overlay.has_state());

      overlays[name] = overlay;

      futures.push_back(configure(name));
    }

    // Wait for all the networks to be configured.
    await(futures)
      .onAny(defer(self(), &ManagerProcess::_updateAgentOverlays));
  }

  void _updateAgentOverlays()
  {
    if (state != REGISTERING) {
      LOG(WARNING) << "Ignored sending registered message because "
                   << "agent is not in REGISTERING state";
      return;
    }

    CHECK_SOME(overlayMaster);

    AgentRegisteredMessage message;

    foreachvalue (const AgentOverlayInfo& overlay, overlays) {
      if (!overlay.has_state()) {
        // That means some overlay network has not been configured
        // yet. This is possible if the master gets some new overlay
        // and triggers another round of registration. The new
        // overlays are still being configured while the configuration
        // for the last registration round has finished. In that case,
        // we should simply wait for the new overlays to be configured
        // and that will trigger another '_updateAgentOverlays()'.
        LOG(INFO) << "Ignored sending registered message because overlay "
                  << "network '" << overlay.info().name()
                  << "' is still being configured";
        return;
      }

      message.add_overlays()->CopyFrom(overlay);
    }

    LOG(INFO) << "Sending agent registered message to " << overlayMaster.get();

    // NOTE: If this message does not reach the master, the master
    // will keep sending 'UpdateAgentOverlaysMessage', which will
    // essentially cause a retry of sending this message.
    send(overlayMaster.get(), message);
  }

  void agentRegisteredAcknowledgement(const UPID& from)
  {
    LOG(INFO) << "Received agent registered acknowledgement from " << from;

    state = REGISTERED;

    connected.set(Nothing());
  }

  void detected(const Future<Option<MasterInfo>>& mesosMaster)
  {
    if (mesosMaster.isFailed()) {
      EXIT(EXIT_FAILURE)
        << "Failed to detect Mesos master: "
        << mesosMaster.failure();
    }

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

      overlayMaster = UPID(mesosMaster.get()->pid());
      overlayMaster->id = MASTER_MANAGER_PROCESS_ID;

      link(overlayMaster.get());

      LOG(INFO) << "Detected new overlay master at " << overlayMaster.get();

      doReliableRegistration(INITIAL_BACKOFF_PERIOD);
    }

    // Keep detecting new Mesos masters.
    detector->detect(latestMesosMaster)
      .onAny(defer(self(), &ManagerProcess::detected, lambda::_1));
  }

  void doReliableRegistration(Duration maxBackoff)
  {
    if (state == REGISTERED) {
      LOG(INFO) << "Overlay agent is already in REGISTERED state";
      return;
    }

    if (overlayMaster.isNone()) {
      LOG(INFO) << "Overlay master is unknown, ignored registration";
      return;
    }

    // Send registration to the overlay master.
    send(overlayMaster.get(), RegisterAgentMessage());

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

  Future<http::Response> overlay(const http::Request& request)
  {
    AgentInfo agent;
    agent.set_ip(stringify(self().address));

    foreachvalue (const AgentOverlayInfo& overlay, overlays) {
      agent.add_overlays()->CopyFrom(overlay);
    }

    return http::OK(
        JSON::protobuf(agent),
        request.url.query.get("jsonp"));
  }

  Future<Nothing> configure(const string& name)
  {
    return await(configureMesosNetwork(name),
                 configureDockerNetwork(name))
      .then(defer(self(),
                  &Self::_configure,
                  name,
                  lambda::_1));
  }

  Future<Nothing> _configure(
      const string& name,
      const tuple<Future<Nothing>, Future<Nothing>>& t)
  {
    CHECK(overlays.contains(name));

    AgentOverlayInfo::State* state = overlays[name].mutable_state();

    Future<Nothing> mesos = std::get<0>(t);
    Future<Nothing> docker = std::get<1>(t);

    vector<string> errors;

    if (!mesos.isReady()) {
      errors.push_back((mesos.isFailed() ? mesos.failure() : "discarded"));
    }

    if (!docker.isReady()) {
      errors.push_back((docker.isFailed() ? docker.failure() : "discarded"));
    }

    if (!errors.empty()) {
      state->set_status(AgentOverlayInfo::State::STATUS_FAILED);
      state->set_error(strings::join(";", errors));
      return Failure(strings::join(";", errors));
    }

    state->set_status(AgentOverlayInfo::State::STATUS_OK);
    return Nothing();
  }

  Future<Nothing> configureMesosNetwork(const string& name)
  {
    CHECK(overlays.contains(name));
    const AgentOverlayInfo& overlay = overlays[name];

    if (!overlay.has_mesos_bridge()) {
      return Failure("Missing Mesos bridge info");
    }

    Try<IPNetwork> subnet = IPNetwork::parse(
        overlay.mesos_bridge().ip(),
        AF_INET);

    if (subnet.isError()) {
      return Failure("Failed to parse bridge ip: " + subnet.error());
    }

    auto config = [name, subnet, overlay](JSON::ObjectWriter* writer) {
      writer->field("name", name);
      writer->field("type", "bridge");
      writer->field("bridge", overlay.mesos_bridge().name());
      writer->field("isGateway", true);
      writer->field("ipMasq", false);

      writer->field("ipam", [subnet](JSON::ObjectWriter* writer) {
        writer->field("type", "host-local");
        writer->field("subnet", stringify(subnet.get()));

        writer->field("routes", [](JSON::ArrayWriter* writer) {
          writer->element([](JSON::ObjectWriter* writer) {
            writer->field("dst", "0.0.0.0/0");
          });
        });
      });
    };

    Try<Nothing> write = os::write(
        path::join(cniDir, name + ".cni"),
        jsonify(config));

    if (write.isError()) {
      return Failure("Failed to write CNI config: " + write.error());
    }

    return Nothing();
  }

  Future<Nothing> configureDockerNetwork(const string& name)
  {
    return checkDockerNetwork(name)
      .then(defer(self(),
                  &Self::_configureDockerNetwork,
                  name,
                  lambda::_1));
  }

  Future<bool> checkDockerNetwork(const string& name)
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

  Future<Nothing> _configureDockerNetwork(
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

    Try<IPNetwork> subnet = IPNetwork::parse(
        overlay.docker_bridge().ip(),
        AF_INET);

    if (subnet.isError()) {
      return Failure("Failed to parse bridge ip: " + subnet.error());
    }

    // TODO(jieyu): We need to set bridge name accordingly.

    vector<string> argv = {
      "docker",
      "network",
      "create",
      "--driver=bridge",
      "--subnet=" + stringify(subnet.get()),
      "--opt=com.docker.network.bridge.name=" +
      overlay.docker_bridge().name(),
      "--opt=\"com.docker.network.bridge.enable_ip_masquerade\"=false",
      name
    };

    Try<Subprocess> s = subprocess(
        "docker",
        argv,
        Subprocess::PATH("/dev/null"),
        Subprocess::PATH("/dev/null"),
        Subprocess::PIPE());

    if (s.isError()) {
      return Failure("Unable to execute docker network create: " + s.error());
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
          return Failure("Failed to reap the subprocess");
          }

          Future<string> err = std::get<1>(t);
          if (!err.isReady()) {
          return Failure(
            "Failed to read stderr from the subprocess: " +
            (err.isFailed() ? err.failure() : "discarded"));
          }

          if (status.get() != 0) {
            return Failure(
                "Failed to create user-defined docker network '" +
                name + "': " + err.get());
          }

          return Nothing();
      });
  }

private:
  enum State
  {
    REGISTERING = 0,
    REGISTERED = 1,
  };

  const string cniDir;
  Owned<MasterDetector> detector;

  State state;
  Promise<Nothing> connected;

  Option<UPID> overlayMaster;

  hashmap<string, AgentOverlayInfo> overlays;
};


class Manager : public Anonymous
{
public:
  Manager(const string& cniDir, Owned<MasterDetector> detector)
  {
    process = Owned<ManagerProcess>(new ManagerProcess(cniDir, detector));
    spawn(process.get());

    // Wait for the overlay-manager to be ready before
    // allowing the Agent to proceed.
    Future<Nothing> ready = process->ready();
    ready.await();

    LOG(INFO) << "Overlay agent is ready";
  }

  virtual ~Manager()
  {
    terminate(process.get());
    wait(process.get());
  }

private:
  Owned<ManagerProcess> process;
};

} // namespace agent {
} // namespace overlay{
} // namespace modules {
} // namespace mesos {


using mesos::modules::overlay::agent::Manager;
using mesos::modules::overlay::agent::ManagerProcess;


// Module "main".
Anonymous* createOverlayAgentManager(const Parameters& parameters)
{
  Option<string> master;
  Option<string> cniDir;

  foreach (const mesos::Parameter& parameter, parameters.parameter()) {
    LOG(INFO) << "Overlay agent parameter '" << parameter.key()
              << "=" << parameter.value() << "'";

    if (parameter.key() == "master") {
      master = parameter.value();
    }

    if (parameter.key() == "cni_dir") {
      cniDir = parameter.value();
    }
  }

  if (master.isNone()) {
    EXIT(EXIT_FAILURE) << "Missing parameter 'master'";
  }

  if (cniDir.isNone()) {
    EXIT(EXIT_FAILURE) << "Missing parameter 'cni_dir'";
  }

  Try<MasterDetector*> detector = MasterDetector::create(master);
  if (detector.isError()) {
    EXIT(EXIT_FAILURE)
      << "Unable to create master detector: " << detector.error();
  }

  Try<Nothing> mkdir = os::mkdir(cniDir.get());
  if (mkdir.isError()) {
    EXIT(EXIT_FAILURE)
      << "Failed to create CNI config directory: " << mkdir.error();
  }

  return new Manager(cniDir.get(), Owned<MasterDetector>(detector.get()));
}


// Declares a helper module named 'Manager'.
Module<Anonymous> com_mesosphere_mesos_OverlayAgentManager(
    MESOS_MODULE_API_VERSION,
    MESOS_VERSION,
    "Mesosphere",
    "help@mesosphere.io",
    "DCOS Overlay Agent Module",
    NULL,
    createOverlayAgentManager);

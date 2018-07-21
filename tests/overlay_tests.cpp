#include <string>
#include <ostream>

#include <gmock/gmock.h>

#include <mesos/hook.hpp>
#include <mesos/mesos.hpp>
#include <mesos/resources.hpp>

#include <mesos/module/module.hpp>
#include <mesos/module/anonymous.hpp>

#include <mesos/scheduler/scheduler.hpp>

#include <mesos/slave/isolator.hpp>

#include <process/future.hpp>
#include <process/gmock.hpp>
#include <process/gtest.hpp>
#include <process/http.hpp>
#include <process/process.hpp>
#include <process/owned.hpp>

#include <stout/gtest.hpp>
#include <stout/json.hpp>
#include <stout/option.hpp>
#include <stout/os.hpp>
#include <stout/path.hpp>
#include <stout/protobuf.hpp>
#include <stout/try.hpp>

#include <stout/os/read.hpp>

#include "common/shell.hpp"

#include "hook/manager.hpp"

#include "master/detector/standalone.hpp"

#include "module/manager.hpp"

#include "overlay/agent.hpp"
#include "overlay/constants.hpp"
#include "overlay/messages.pb.h"
#include "overlay/overlay.hpp"
#include "overlay/overlay.pb.h"


#include "slave/flags.hpp"

#include "tests/mesos.hpp"

namespace overlayAgent = mesos::modules::overlay::agent;
namespace http = process::http;

using namespace mesos::internal::tests;

using std::cout;
using std::endl;
using std::string;

using process::Future;
using process::Owned;
using process::PID;
using process::UPID;

using process::http::OK;
using process::http::Response;

using mesos::internal::master::Master;

using mesos::internal::slave::Containerizer;
using mesos::internal::slave::MesosContainerizerProcess;
using mesos::internal::slave::Slave;

using mesos::master::detector::MasterDetector;

using mesos::modules::common::runCommand;
using mesos::modules::common::runScriptCommand;

using mesos::modules::Anonymous;
using mesos::modules::ModuleManager;
using mesos::modules::overlay::AgentInfo;
using mesos::modules::overlay::AgentOverlayInfo;
using mesos::modules::overlay::AGENT_MANAGER_PROCESS_ID;
using mesos::modules::overlay::MASTER_MANAGER_PROCESS_ID;
using mesos::modules::overlay::RESERVED_NETWORKS;
using mesos::modules::overlay::internal::AgentConfig;
using mesos::modules::overlay::internal::AgentRegisteredAcknowledgement;
using mesos::modules::overlay::internal::AgentRegisteredMessage;
using mesos::modules::overlay::internal::RegisterAgentMessage;
using mesos::modules::overlay::internal::MasterConfig;
using mesos::modules::overlay::OverlayInfo;
using mesos::modules::overlay::State;
using mesos::modules::overlay::agent::IPSET_OVERLAY;

namespace mesos {
namespace overlay {
namespace tests {

constexpr char AGENT_CNI_DIR[] = "cni/";
constexpr char AGENT_CNI_DATA_DIR[] = "cni_data/";
constexpr char AGENT_JSON_CONFIG[] = "agent.json";
constexpr char OVERLAY_SUBNET[] = "192.168.0.0/16";
constexpr char OVERLAY_NAME[] = "mz-overlay";
constexpr char OVERLAY_NAME_2[] = "mz-overlay-2";
constexpr char MASTER_JSON_CONFIG[] = "master.json";
constexpr char MASTER_OVERLAY_MODULE_NAME[] =
  "com_mesosphere_mesos_OverlayMasterManager";

constexpr uint32_t OVERLAY_PREFIX = 24;

class OverlayTest : public MesosTest
{
protected:
  virtual void SetUp()
  {
    MesosTest::SetUp();

    // Define the modules JSON config.
    auto modulesConfig = [](JSON::ObjectWriter* writer) {
      writer->field("libraries", [](JSON::ArrayWriter* writer) {
        writer->element([](JSON::ObjectWriter* writer) {
          writer->field(
              "file",
              path::join(MODULES_BUILD_DIR,
                ".libs/libmesos_network_overlay.so"));

          writer->field("modules", [](JSON::ArrayWriter* writer) {
            auto masterModuleConfig = [](JSON::ObjectWriter* writer) {
            writer->field(
                "name",
                "com_mesosphere_mesos_OverlayMasterManager");

            writer->field("parameters", [](JSON::ArrayWriter* writer) {
              writer->element([](JSON::ObjectWriter* writer) {
                writer->field("key", "master_config");
                writer->field("value", MASTER_JSON_CONFIG);
                });
              });
            };

            auto agentModuleConfig = [](JSON::ObjectWriter* writer) {
            writer->field("name", "com_mesosphere_mesos_OverlayAgentManager");
            writer->field("parameters", [](JSON::ArrayWriter* writer) {
              writer->element([](JSON::ObjectWriter* writer) {
                writer->field("key", "agent_config");
                writer->field("value", AGENT_JSON_CONFIG);
                });
              });
            };

            writer->element(masterModuleConfig);
            writer->element(agentModuleConfig);
          });
        });
      });
    };

    Try<JSON::Object> json = JSON::parse<JSON::Object>(jsonify(modulesConfig));
    ASSERT_SOME(json);

    Try<Modules> _modules = protobuf::parse<Modules>(json.get());
    ASSERT_SOME(_modules);

    modules = _modules.get();

    // Initialize the modules.
    Try<Nothing> result = ModuleManager::load(modules);
    ASSERT_SOME(result);

    // Setup Master and Agent config.
    masterOverlayConfig.mutable_network()->set_vtep_subnet("44.128.0.0/16");
    masterOverlayConfig.mutable_network()->set_vtep_mac_oui(
        "70:B3:D5:00:00:00");

    OverlayInfo overlay;
    overlay.set_name(OVERLAY_NAME);
    overlay.set_subnet(OVERLAY_SUBNET);
    overlay.set_prefix(24);

    masterOverlayConfig.mutable_network()->add_overlays()->CopyFrom(overlay);

    // For the agents, by default, the Docker and Mesos networks are
    // disabled.
    agentOverlayConfig.set_cni_dir(AGENT_CNI_DIR);
    agentOverlayConfig.set_cni_data_dir(AGENT_CNI_DATA_DIR);
    agentOverlayConfig.mutable_network_config()->set_allocate_subnet(true);
    agentOverlayConfig.mutable_network_config()->set_mesos_bridge(false);
    agentOverlayConfig.mutable_network_config()->set_docker_bridge(false);

    // Delete the 'ipset' and 'iptables' rules inserted by the
    // tests in-case these rules were pending from the previous runs.
    // We don't need to worry about whether the commands were
    // successful since this is precautionary.
    Try<string> cleanup = strings::format(
        "iptables -t nat -D POSTROUTING -s %s "
        "-m set --match-set %s dst "
        "-j MASQUERADE; "
        "iptables -t filter -D DOCKER-ISOLATION "
        "-j RETURN; "
        "ipset destroy %s; "
        "docker network rm %s %s",
        OVERLAY_SUBNET,
        stringify(IPSET_OVERLAY),
        stringify(IPSET_OVERLAY),
        OVERLAY_NAME,
        OVERLAY_NAME_2);

    ASSERT_SOME(cleanup);

    Future<string> cleanupResult = runScriptCommand(cleanup.get());

    cleanupResult.await();
  }

  virtual void TearDown()
  {
    stopOverlayAgent();

    LOG(INFO) << "Unloading all modules...";
    // Unload all modules.
    foreach (const Modules::Library& library, modules.libraries()) {
      foreach (const Modules::Library::Module& module, library.modules()) {
        if (module.has_name()) {
          ASSERT_SOME(ModuleManager::unload(module.name()));
        }
      }
    }

    if (agentOverlayConfig.network_config().mesos_bridge() ||
        agentOverlayConfig.network_config().docker_bridge()) {
      // Delete the 'ipset' and 'iptables' rules inserted by the
      // tests.
      // Tests have passed cleanup.
      foreach(const OverlayInfo& overlay,
              masterOverlayConfig.network().overlays()) {
        Future<string> iptables = runCommand(
            "iptables",
            {"iptables",
            "-t", "nat",
            "-D", "POSTROUTING",
            "-s", overlay.subnet(),
            "-m", "set",
            "--match-set", stringify(IPSET_OVERLAY), "dst",
            "-j", "MASQUERADE",
            });
        AWAIT_READY(iptables);
      }

      Future<string> ipset = runCommand(
          "ipset",
          {"ipset",
           "destroy",
           IPSET_OVERLAY});
      AWAIT_READY(ipset);
    }

    if (agentOverlayConfig.network_config().docker_bridge()) {
      // Clean up Docker network.
      Future<string> docker = runCommand(
          "docker",
          {"docker",
           "network",
           "rm",
           OVERLAY_NAME, 
           OVERLAY_NAME_2});
      AWAIT_READY(docker);
    }

    MesosTest::TearDown();
  }

  // Initialized the overlay Master module using the
  // `masterOverlayConfig`.
  Try<Owned<Anonymous>> startOverlayMaster()
  {
    Try<Nothing> write = os::write(MASTER_JSON_CONFIG,
        stringify(JSON::protobuf(masterOverlayConfig)));
    if(write.isError()) {
      return Error("Unabled to write master config: " + write.error());
    }

    Try<Anonymous*> create = ModuleManager::create<Anonymous>(
        MASTER_OVERLAY_MODULE_NAME);
    if (create.isError()) {
      return Error("Unable to create Master module: " + create.error());
    }

    return Owned<Anonymous>(create.get());
  }

  // This takes in a user defined `_masterOverlayConfig` and merges
  // with the already initialized `masterOverlayConfig`.
  Try<Owned<Anonymous>> startOverlayMaster(
      const MasterConfig& _masterOverlayConfig)
  {
    masterOverlayConfig.MergeFrom(_masterOverlayConfig);
    return startOverlayMaster();
  }

  void clearOverlays()
  {
    masterOverlayConfig.mutable_network()->clear_overlays();
  }

  // Initializes the overlay Agent module using the
  // `agentOverlayConfig` initialized during `Setup`. By default the
  // `agentOverlayConfig` has the Mesos and the Docker networks
  // disabled, and only has the subnet allocation enabled.
  Try<Owned<overlayAgent::ManagerProcess>> startOverlayAgent()
  {
    Try<Owned<overlayAgent::ManagerProcess>> _agentModule =
      overlayAgent::ManagerProcess::create(agentOverlayConfig);

    if (_agentModule.isError()) {
      return Error(
          "Unable to create overlay Agent module: " + _agentModule.error());
    }

    agentModule = _agentModule.get();

    spawn(agentModule->get());

    return agentModule.get();
  }

  // Takes in a user-defined `_agentOverlayConfig` and merges with the
  // `agentOverlayConfig`, before initializing the overlay Agent
  // module.
  Try<Owned<overlayAgent::ManagerProcess>> startOverlayAgent(
      const AgentConfig& _agentOverlayConfig)
  {
    agentOverlayConfig.MergeFrom(_agentOverlayConfig);
    return startOverlayAgent();
  }

  Try<Nothing> stopOverlayAgent()
  {
    if (agentModule.isSome()) {
      terminate(agentModule->get());
      wait(agentModule->get());

      agentModule = None();
    } else {
      return Error("Uninitalized Agent module");
    }

    return Nothing();
  }

  Try<State> parseMasterState(const string& state)
  {
    Try<JSON::Object> json = JSON::parse<JSON::Object>(state);
    if (json.isError()) {
      return Error("JSON parse failed: " + json.error());
    }

    return ::protobuf::parse<State>(json.get());
  }

  Try<AgentInfo> parseAgentOverlay(const string& info)
  {
    Try<JSON::Object> json = JSON::parse<JSON::Object>(info);
    if (json.isError()) {
      return Error("JSON parse failed: " + json.error());
    }

    return ::protobuf::parse<AgentInfo>(json.get());
  }

private:
  AgentConfig agentOverlayConfig;

  Modules modules;

  MasterConfig masterOverlayConfig;

  Option<Owned<overlayAgent::ManagerProcess>> agentModule;
};


// Tests the ability of the `Master overlay module` to allocate
// subnets to Agents for a given overlay network.
TEST_F(OverlayTest, checkMasterAgentComm)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  LOG(INFO) << "Master PID: " << master.get()->pid;

  Try<Owned<Anonymous>> masterModule = startOverlayMaster();
  ASSERT_SOME(masterModule);

  // Master `Anonymous` module created successfully. Lets see if we
  // can hit the `state` endpoint of the Master.
  UPID overlayMaster = UPID(
      MASTER_MANAGER_PROCESS_ID,
      master.get()->pid.address);

  Future<Response> masterResponse = process::http::get(
      overlayMaster,
      "state");

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, masterResponse);
  AWAIT_EXPECT_RESPONSE_HEADER_EQ(
      APPLICATION_JSON,
      "Content-Type",
      masterResponse);

  // The `state` end-point is backed by the
  // mesos::modules::overlay::State protobuf, so need to parse the
  // JSON and verify that the correct configuration is being
  // reflected.
  Try<State> state = parseMasterState(masterResponse->body);
  ASSERT_SOME(state);
  ASSERT_EQ(1, state->network().overlays_size());
  ASSERT_EQ(OVERLAY_NAME, state->network().overlays(0).name());
  ASSERT_EQ(OVERLAY_SUBNET, state->network().overlays(0).subnet());
  ASSERT_EQ(24, state->network().overlays(0).prefix());

  // We haven't started the Agent, so make sure there are no Agents
  // reflected at this end-point.
  ASSERT_EQ(0, state->agents_size());

  AgentConfig agentOverlayConfig;
  agentOverlayConfig.set_master(stringify(overlayMaster.address));

  // Setup a future to notify the test that Agent overlay module has
  // registered.
  Future<RegisterAgentMessage> registerAgentMessage =
    FUTURE_PROTOBUF(RegisterAgentMessage(), _, _);

  // Setup a future to notify the test that Agent overlay module has
  // registered.
  Future<AgentRegisteredMessage> agentRegisteredMessage =
    FUTURE_PROTOBUF(AgentRegisteredMessage(), _, _);

  Try<Owned<overlayAgent::ManagerProcess>> agentModule = startOverlayAgent(
      agentOverlayConfig);

  ASSERT_SOME(agentModule);

  AWAIT_READY(registerAgentMessage);

  AWAIT_READY(agentRegisteredMessage);

  // Check that the agent is allowed to progress.
  AWAIT_READY(agentModule.get()->ready());

  // Agent manager has been created. Hit the `overlay` endpoint to
  // check that module is up and responding.
  UPID overlayAgent = UPID(
      AGENT_MANAGER_PROCESS_ID,
      master.get()->pid.address);

  Future<Response> agentResponse = process::http::get(
      overlayAgent,
      "overlay");

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, agentResponse);
  AWAIT_EXPECT_RESPONSE_HEADER_EQ(
      APPLICATION_JSON,
      "Content-Type",
      agentResponse);

  // The `overlay` end-point is backed by the
  // mesos::modules::overlay::AgentInfo protobuf, so need to parse the
  // JSON and verify that the correct configuration is being
  // reflected.
  Try<AgentInfo> info = parseAgentOverlay(agentResponse->body);
  ASSERT_SOME(info);

  // There should be only 1 overlay.
  ASSERT_EQ(1, info->overlays_size());
  EXPECT_EQ(OVERLAY_NAME, info->overlays(0).info().name());

  Try<net::IP::Network> agentNetwork = net::IP::Network::parse(
      info->overlays(0).subnet(), AF_INET);

  ASSERT_SOME(agentNetwork);
  EXPECT_EQ(24, agentNetwork->prefix());

  Try<net::IP::Network> allocatedSubnet = net::IP::Network::parse(
      "192.168.0.0/24", AF_INET);

  ASSERT_SOME(allocatedSubnet);
  EXPECT_EQ(allocatedSubnet.get(), agentNetwork.get());

  // Hit the `state` end-point again. We should be seeing the
  // Agents overlay subnet allocation in the `state` endpoint.
  masterResponse = process::http::get(
      overlayMaster,
      "state");

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, masterResponse);
  AWAIT_EXPECT_RESPONSE_HEADER_EQ(
      APPLICATION_JSON,
      "Content-Type",
      masterResponse);

  state = parseMasterState(masterResponse->body);
  ASSERT_SOME(state);
  ASSERT_EQ(1, state->agents_size());

  AgentInfo masterAgentInfo;
  masterAgentInfo.CopyFrom(state->agents(0));
  EXPECT_EQ(
      info.get().SerializeAsString(),
      masterAgentInfo.SerializeAsString());
}


// Tests the ability of the `Agent overlay module` to create Mesos CNI
// networks when `mesos bridge` has been enabled.
TEST_F(OverlayTest, ROOT_checkMesosNetwork)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  LOG(INFO) << "Master PID: " << master.get()->pid;

  Try<Owned<Anonymous>> masterModule = startOverlayMaster();
  ASSERT_SOME(masterModule);

  // Master `Anonymous` module created successfully. Lets see if we
  // can hit the `state` endpoint of the Master.
  UPID overlayMaster = UPID(
      MASTER_MANAGER_PROCESS_ID,
      master.get()->pid.address);

  AgentConfig agentOverlayConfig;
  agentOverlayConfig.set_master(stringify(overlayMaster.address));
  // Enable Mesos network.
  agentOverlayConfig.mutable_network_config()->set_mesos_bridge(true);

  // Setup a future to notify the test that Agent overlay module has
  // registered.
  Future<AgentRegisteredMessage> agentRegisteredMessage =
    FUTURE_PROTOBUF(AgentRegisteredMessage(), _, _);

  Try<Owned<overlayAgent::ManagerProcess>> agentModule = startOverlayAgent(
      agentOverlayConfig);

  ASSERT_SOME(agentModule);

  AWAIT_READY(agentRegisteredMessage);

  // Check the agent is allowed to progress.
  AWAIT_READY(agentModule.get()->ready());

  // Verify that `ipset` has been created and the `iptables` entries
  // exist.
  Future<string> ipset = runCommand("ipset",
      {"ipset",
      "list",
      IPSET_OVERLAY});
  AWAIT_READY(ipset);

  // Verify that the `IPMASQ` rules have been installed.
  Future<string> iptables = runCommand("iptables",
      {"iptables",
      "-t", "nat",
      "-C", "POSTROUTING",
      "-s", OVERLAY_SUBNET,
      "-m", "set",
      "--match-set", stringify(IPSET_OVERLAY), "dst",
      "-j", "MASQUERADE",
      });
  AWAIT_READY(iptables);

  // Verify the CNI configuration has been installed correctly.
  Try<string> cniConfig = os::read(
      path::join("cni", stringify(OVERLAY_NAME) + ".conf"));

  ASSERT_SOME(cniConfig);

  Try<JSON::Object> json = JSON::parse<JSON::Object>(cniConfig.get());
  ASSERT_SOME(json);

  Result<JSON::String> network = json->find<JSON::String>("name");
  ASSERT_SOME(network);
  EXPECT_EQ(network.get(), OVERLAY_NAME);

  Result<JSON::Boolean> ipMasq = json->find<JSON::Boolean>("ipMasq");
  ASSERT_SOME(ipMasq);
  EXPECT_EQ(ipMasq.get(), false);

  Result<JSON::Number> mtu = json->find<JSON::Number>("mtu");
  ASSERT_SOME(mtu);
  EXPECT_EQ(mtu.get(), 1420);

  Result<JSON::Object> ipam = json->find<JSON::Object>("ipam");
  ASSERT_SOME(ipam);

  Result<JSON::String> subnet = ipam->find<JSON::String>("subnet");
  ASSERT_SOME(subnet);
  EXPECT_EQ(subnet.get(), "192.168.0.0/25");
}


// Tests the ability of the `Agent overlay module` to create Docker
// network.
TEST_F(OverlayTest, ROOT_checkDockerNetwork)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  LOG(INFO) << "Master PID: " << master.get()->pid;

  Try<Owned<Anonymous>> masterModule = startOverlayMaster();
  ASSERT_SOME(masterModule);

  // Master `Anonymous` module created successfully. Lets see if we
  // can hit the `state` endpoint of the Master.
  UPID overlayMaster = UPID(
      MASTER_MANAGER_PROCESS_ID,
      master.get()->pid.address);

  AgentConfig agentOverlayConfig;
  agentOverlayConfig.set_master(stringify(overlayMaster.address));
  // Enable Docker network.
  agentOverlayConfig.mutable_network_config()->set_docker_bridge(true);

  // Setup a future to notify the test that Agent overlay module has
  // registered.
  Future<AgentRegisteredMessage> agentRegisteredMessage =
    FUTURE_PROTOBUF(AgentRegisteredMessage(), _, _);

  Try<Owned<overlayAgent::ManagerProcess>> agentModule = startOverlayAgent(
      agentOverlayConfig);

  ASSERT_SOME(agentModule);

  AWAIT_READY(agentRegisteredMessage);

  // Check the agent is allowed to progress.
  AWAIT_READY(agentModule.get()->ready());

  // Verify that `ipset` has been created and the `iptables` entries
  // exist.
  Future<string> ipset = runCommand("ipset",
      {"ipset",
      "list",
      IPSET_OVERLAY});

  AWAIT_READY(ipset);

  // Verify that the `IPMASQ` rules have been installed.
  Future<string> iptables = runCommand("iptables",
      {"iptables",
      "-t", "nat",
      "-C", "POSTROUTING",
      "-s", OVERLAY_SUBNET,
      "-m", "set",
      "--match-set", stringify(IPSET_OVERLAY), "dst",
      "-j", "MASQUERADE",
      });

  AWAIT_READY(iptables);

  // Verify the docker network has been installed correctly.
  Future<string> docker = runCommand("docker",
      {"docker",
      "network",
      "inspect",
      OVERLAY_NAME});

  AWAIT_READY(docker);

  Try<JSON::Array> json = JSON::parse<JSON::Array>(docker.get());
  ASSERT_SOME(json);
}


// Tests the ability of the `Master overlay module` to recover
// checkpointed overlay `State`.
TEST_F(OverlayTest, ROOT_checkMasterRecovery)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  LOG(INFO) << "Master PID: " << master.get()->pid;

  // Ask overlay Master to use the replicated log by setting
  // `replicated_log_dir`. We are not specifying `zk` configuration so
  // the `quorum` will default to "1".
  MasterConfig masterOverlayConfig;
  masterOverlayConfig
    .set_replicated_log_dir("overlay_replicated_log");

  Try<Owned<Anonymous>> masterModule = startOverlayMaster(masterOverlayConfig);
  ASSERT_SOME(masterModule);

  // Master `Anonymous` module created successfully. Lets see if we
  // can hit the `state` endpoint of the Master.
  UPID overlayMaster = UPID(
      MASTER_MANAGER_PROCESS_ID,
      master.get()->pid.address);

  AgentConfig agentOverlayConfig;
  agentOverlayConfig.set_master(stringify(overlayMaster.address));
  // Enable Mesos network.
  agentOverlayConfig.mutable_network_config()->set_mesos_bridge(true);
  // Enable Docker network.
  agentOverlayConfig.mutable_network_config()->set_docker_bridge(true);

  // Setup a future to notify the test that Agent overlay module has
  // registered.
  Future<AgentRegisteredAcknowledgement> agentRegisteredAcknowledgement =
    FUTURE_PROTOBUF(AgentRegisteredAcknowledgement(), _, _);

  Try<Owned<overlayAgent::ManagerProcess>> agentModule = startOverlayAgent(
      agentOverlayConfig);

  ASSERT_SOME(agentModule );

  AWAIT_READY(agentRegisteredAcknowledgement);

  // Check the agent is allowed to progress.
  AWAIT_READY(agentModule.get()->ready());

  // Agent manager has been created. Hit the `overlay` endpoint to
  // check that module is up and responding.
  UPID overlayAgent = UPID(master.get()->pid);
  overlayAgent.id = AGENT_MANAGER_PROCESS_ID;

  Future<Response> agentResponse = process::http::get(
      overlayAgent,
      "overlay");

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, agentResponse);
  AWAIT_EXPECT_RESPONSE_HEADER_EQ(
      APPLICATION_JSON,
      "Content-Type",
      agentResponse);

  // The `overlay` end-point is backed by the
  // mesos::modules::overlay::AgentInfo protobuf, so need to parse the
  // JSON and verify that the correct configuration is being
  // reflected.
  Try<AgentInfo> info = parseAgentOverlay(agentResponse->body);
  ASSERT_SOME(info);

  // There should be only 1 overlay.
  ASSERT_EQ(1, info->overlays_size());
  EXPECT_EQ(OVERLAY_NAME, info->overlays(0).info().name());

  Try<net::IP::Network> agentNetwork = net::IP::Network::parse(
      info->overlays(0).subnet(), AF_INET);

  ASSERT_SOME(agentNetwork);
  EXPECT_EQ(24, agentNetwork->prefix());

  Try<net::IP::Network> allocatedSubnet = net::IP::Network::parse(
      "192.168.0.0/24", AF_INET);

  ASSERT_SOME(allocatedSubnet);
  EXPECT_EQ(allocatedSubnet.get(), agentNetwork.get());

  // Hit the `state` end-point. We should be seeing the
  // Agents overlay subnet allocation in the `state` endpoint.
  Future<http::Response> masterResponse = process::http::get(
      overlayMaster,
      "state");

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, masterResponse);
  AWAIT_EXPECT_RESPONSE_HEADER_EQ(
      APPLICATION_JSON,
      "Content-Type",
      masterResponse);

  Try<State> state = parseMasterState(masterResponse->body);
  ASSERT_SOME(state);
  ASSERT_EQ(1, state->agents_size());

  AgentInfo masterAgentInfo;
  masterAgentInfo.CopyFrom(state->agents(0));
  EXPECT_EQ(
      info.get().SerializeAsString(),
      masterAgentInfo.SerializeAsString())
      << "Agent response: " << agentResponse->body
      << ", Master response: " << masterResponse->body;

  // Kill the master.
  masterModule->reset();

  masterModule = startOverlayMaster(masterOverlayConfig);
  ASSERT_SOME(masterModule);

  // Re-start the master and wait for the Agent to re-register.
  agentRegisteredAcknowledgement = FUTURE_PROTOBUF(
      AgentRegisteredAcknowledgement(), _, _);

  AWAIT_READY(agentRegisteredAcknowledgement);

  // Hit the master end-point again.
  masterResponse = process::http::get(
      overlayMaster,
      "state");

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, masterResponse);
  AWAIT_EXPECT_RESPONSE_HEADER_EQ(
      APPLICATION_JSON,
      "Content-Type",
      masterResponse);

  state = parseMasterState(masterResponse->body);
  ASSERT_SOME(state);
  ASSERT_EQ(1, state->agents_size());

  AgentInfo recoveredMasterAgentInfo;
  recoveredMasterAgentInfo.CopyFrom(state->agents(0));
  EXPECT_EQ(
      masterAgentInfo.SerializeAsString(),
      recoveredMasterAgentInfo.SerializeAsString());
}


// Tests the ability of the `Agent overlay module` to recover
// `AgentInfo` from the master.
TEST_F(OverlayTest, ROOT_checkAgentRecovery)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  LOG(INFO) << "Master PID: " << master.get()->pid;

  // Ask overlay Master to use the replicated log by setting
  // `replicated_log_dir`. We are not specifying `zk` configuration so
  // the `quorum` will default to "1".
  MasterConfig masterOverlayConfig;
  masterOverlayConfig
    .set_replicated_log_dir("overlay_replicated_log");

  Try<Owned<Anonymous>> masterModule = startOverlayMaster(masterOverlayConfig);
  ASSERT_SOME(masterModule);

  // Master `Anonymous` module created successfully. Lets see if we
  // can hit the `state` endpoint of the Master.
  UPID overlayMaster = UPID(master.get()->pid);
  overlayMaster.id = MASTER_MANAGER_PROCESS_ID;

  AgentConfig agentOverlayConfig;
  agentOverlayConfig.set_master(stringify(overlayMaster.address));
  // Enable Mesos network.
  agentOverlayConfig.mutable_network_config()->set_mesos_bridge(true);
  // Enable Docker network.
  agentOverlayConfig.mutable_network_config()->set_docker_bridge(true);

  // Setup a future to notify the test that Agent overlay module has
  // registered.
  Future<AgentRegisteredAcknowledgement> agentRegisteredAcknowledgement =
    FUTURE_PROTOBUF(AgentRegisteredAcknowledgement(), _, _);

  Try<Owned<overlayAgent::ManagerProcess>> agentModule  = startOverlayAgent(
      agentOverlayConfig);

  ASSERT_SOME(agentModule);

  AWAIT_READY(agentRegisteredAcknowledgement);

  // Check the agent is allowed to progress.
  AWAIT_READY(agentModule.get()->ready());

  // Agent manager has been created. Hit the `overlay` endpoint to
  // check that module is up and responding.
  UPID overlayAgent = UPID(master.get()->pid);
  overlayAgent.id = AGENT_MANAGER_PROCESS_ID;

  Future<Response> agentResponse = process::http::get(
      overlayAgent,
      "overlay");

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, agentResponse);
  AWAIT_EXPECT_RESPONSE_HEADER_EQ(
      APPLICATION_JSON,
      "Content-Type",
      agentResponse);

  // The `overlay` end-point is backed by the
  // mesos::modules::overlay::AgentInfo protobuf, so need to parse the
  // JSON and verify that the correct configuration is being
  // reflected.
  Try<AgentInfo> info = parseAgentOverlay(agentResponse->body);
  ASSERT_SOME(info);

  // There should be only 1 overlay.
  ASSERT_EQ(1, info->overlays_size());
  EXPECT_EQ(OVERLAY_NAME, info->overlays(0).info().name());

  Try<net::IP::Network> agentNetwork = net::IP::Network::parse(
      info->overlays(0).subnet(), AF_INET);

  ASSERT_SOME(agentNetwork);
  EXPECT_EQ(OVERLAY_PREFIX, agentNetwork->prefix());

  Try<net::IP::Network> allocatedSubnet = net::IP::Network::parse(
      "192.168.0.0/24", AF_INET);

  ASSERT_SOME(allocatedSubnet);
  EXPECT_EQ(allocatedSubnet.get(), agentNetwork.get());

  // Re-start the agent and wait for the agent to re-register.
  Future<AgentRegisteredAcknowledgement> agentReRegisteredAcknowledgement =
    FUTURE_PROTOBUF(AgentRegisteredAcknowledgement(), _, _);

  // Kill the agent.
  Try<Nothing> stop = stopOverlayAgent();
  ASSERT_SOME(stop);

  // re-start the agent.
  agentModule  = startOverlayAgent(agentOverlayConfig);
  ASSERT_SOME(agentModule );

  AWAIT_READY(agentReRegisteredAcknowledgement);

  // Check the agent is allowed to progress.
  AWAIT_READY(agentModule.get()->ready());

  // Hit the master end-point again.
  agentResponse = process::http::get(
      overlayAgent,
      "overlay");

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, agentResponse);
  AWAIT_EXPECT_RESPONSE_HEADER_EQ(
      APPLICATION_JSON,
      "Content-Type",
      agentResponse);

  Try<AgentInfo> reRegisterInfo = parseAgentOverlay(agentResponse->body);
  ASSERT_SOME(reRegisterInfo);

  EXPECT_EQ(
      info.get().SerializeAsString(),
      reRegisterInfo.get().SerializeAsString());
}


// Tests the ability of the `Agent overlay module` to honor the
// `AgentNetworkConfig` over the overlay the network configuration
// specified by the Master.
TEST_F(OverlayTest, ROOT_checkAgentNetworkConfigChange)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  LOG(INFO) << "Master PID: " << master.get()->pid;

  // Ask overlay Master to use the replicated log by setting
  // `replicated_log_dir`. We are not specifying `zk` configuration so
  // the `quorum` will default to "1".
  MasterConfig masterOverlayConfig;
  masterOverlayConfig
    .set_replicated_log_dir("overlay_replicated_log");

  Try<Owned<Anonymous>> masterModule = startOverlayMaster(masterOverlayConfig);
  ASSERT_SOME(masterModule);

  // Master `Anonymous` module created successfully. Lets see if we
  // can hit the `state` endpoint of the Master.
  UPID overlayMaster = UPID(master.get()->pid);
  overlayMaster.id = MASTER_MANAGER_PROCESS_ID;

  AgentConfig agentOverlayConfig;
  agentOverlayConfig.set_master(stringify(overlayMaster.address));
  // Enable Mesos network.
  agentOverlayConfig.mutable_network_config()->set_mesos_bridge(true);
  // Enable Docker network.
  agentOverlayConfig.mutable_network_config()->set_docker_bridge(true);

  // Setup a future to notify the test that Agent overlay module has
  // registered.
  Future<AgentRegisteredAcknowledgement> agentRegisteredAcknowledgement =
    FUTURE_PROTOBUF(AgentRegisteredAcknowledgement(), _, _);

  Try<Owned<overlayAgent::ManagerProcess>> agentModule = startOverlayAgent(
      agentOverlayConfig);

  ASSERT_SOME(agentModule);

  AWAIT_READY(agentRegisteredAcknowledgement);

  // Agent manager has been created. Hit the `overlay` endpoint to
  // check that module is up and responding.
  UPID overlayAgent = UPID(master.get()->pid);
  overlayAgent.id = AGENT_MANAGER_PROCESS_ID;

  Future<Response> agentResponse = process::http::get(
      overlayAgent,
      "overlay");

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, agentResponse);
  AWAIT_EXPECT_RESPONSE_HEADER_EQ(
      APPLICATION_JSON,
      "Content-Type",
      agentResponse);

  // The `overlay` end-point is backed by the
  // mesos::modules::overlay::AgentInfo protobuf, so need to parse the
  // JSON and verify that the correct configuration is being
  // reflected.
  Try<AgentInfo> info = parseAgentOverlay(agentResponse->body);
  ASSERT_SOME(info);

  // There should be only 1 overlay.
  ASSERT_EQ(1, info->overlays_size());
  EXPECT_EQ(OVERLAY_NAME, info->overlays(0).info().name());

  Try<net::IP::Network> agentNetwork = net::IP::Network::parse(
      info->overlays(0).subnet(), AF_INET);

  ASSERT_SOME(agentNetwork);
  EXPECT_EQ(24, agentNetwork->prefix());

  Try<net::IP::Network> allocatedSubnet = net::IP::Network::parse(
      "192.168.0.0/24", AF_INET);

  ASSERT_SOME(allocatedSubnet);
  EXPECT_EQ(allocatedSubnet.get(), agentNetwork.get());

  // Check that the `mesos-bridge` and `docker-bridge` are present.
  EXPECT_TRUE(info->overlays(0).has_mesos_bridge());
  EXPECT_TRUE(info->overlays(0).has_docker_bridge());

  // Verify that the `IPMASQ` rules have been installed.
  Future<string> iptables = runCommand("iptables",
      {"iptables",
      "-t", "nat",
      "-C", "POSTROUTING",
      "-s", OVERLAY_SUBNET,
      "-m", "set",
      "--match-set", stringify(IPSET_OVERLAY), "dst",
      "-j", "MASQUERADE",
      });

  AWAIT_READY(iptables);

  // Delete the IPMASQ rules.
  iptables = runCommand("iptables",
      {"iptables",
      "-t", "nat",
      "-D", "POSTROUTING",
      "-s", OVERLAY_SUBNET,
      "-m", "set",
      "--match-set", stringify(IPSET_OVERLAY), "dst",
      "-j", "MASQUERADE",
      });

  AWAIT_READY(iptables);

  // Re-start the agent and wait for the agent to re-register.
  Future<AgentRegisteredAcknowledgement> agentReRegisteredAcknowledgement =
    FUTURE_PROTOBUF(AgentRegisteredAcknowledgement(), _, _);

  // Kill the agent.
  Try<Nothing> stop = stopOverlayAgent();
  ASSERT_SOME(stop);

  // Reset the `mesos_bridge` and `docker_bridge` before restarting.
  agentOverlayConfig.mutable_network_config()->set_mesos_bridge(false);
  agentOverlayConfig.mutable_network_config()->set_docker_bridge(false);

  // re-start the agent.
  agentModule = startOverlayAgent(agentOverlayConfig);
  ASSERT_SOME(agentModule);

  AWAIT_READY(agentReRegisteredAcknowledgement);

  // With the `mesos_bridge` and the `docker_bridge` disabled there
  // should be no IPMASQ rules installed. Check IPMASQ rules don't exist.
  iptables = runCommand("iptables",
      {"iptables",
      "-t", "nat",
      "-C", "POSTROUTING",
      "-s", OVERLAY_SUBNET,
      "-m", "set",
      "--match-set", stringify(IPSET_OVERLAY), "dst",
      "-j", "MASQUERADE",
      });

  AWAIT_FAILED(iptables);

  // Hit the agent end-point again.
  agentResponse = process::http::get(
      overlayAgent,
      "overlay");

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, agentResponse);
  AWAIT_EXPECT_RESPONSE_HEADER_EQ(
      APPLICATION_JSON,
      "Content-Type",
      agentResponse);

  Try<AgentInfo> reRegisterInfo = parseAgentOverlay(agentResponse->body);
  ASSERT_SOME(reRegisterInfo);

  // Though the `mesos_bridge` and `docker_bridge` have been disabled
  // in the `AgentNetworkConfig`, the Master still has them enabled in
  // this agents overlay configuration. So the overlay configuration
  // reflected on the agent should also reflect the same configuration
  // that the Master has tried to applied, i.e. the `mesos_bridge` and
  // `docker_bridge` should have been enabled.
  ASSERT_EQ(1, reRegisterInfo->overlays_size());
  EXPECT_TRUE(reRegisterInfo->overlays(0).has_mesos_bridge());
  EXPECT_TRUE(reRegisterInfo->overlays(0).has_docker_bridge());
}


// Tests if reserved network names are correctly rejected by the
// master overlay module.
TEST_F(OverlayTest, checkReservedNetworks)
{
  // Try creating a master module with an overlay network that has a
  // reserved name. The creation should fail.
  foreach(const string& networkName, RESERVED_NETWORKS) {
    // Whatever `MasterConfig` is given to the fixture is "merged"
    // with the existing `MasterConfig`. So need to clear the existing
    // overlays to test the reserved overlay network. Otherwise any
    // "reserved" network that was added in the previous iteration
    // would persist and would make this test void.
    clearOverlays();

    OverlayInfo overlay;
    overlay.set_name(networkName);
    overlay.set_subnet("9.0.0.0/8");
    overlay.set_prefix(24);

    MasterConfig masterOverlayConfig;
    masterOverlayConfig.mutable_network()->add_overlays()->CopyFrom(overlay);

    Try<Owned<Anonymous>> masterModule = startOverlayMaster(
        masterOverlayConfig);

    ASSERT_ERROR(masterModule);
  }
}


// Tests the ability of the overlay to add virtual networks.
TEST_F(OverlayTest, ROOT_checkAddVirtualNetworks)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  LOG(INFO) << "Master PID: " << master.get()->pid;

  // Ask overlay Master to use the replicated log by setting
  // `replicated_log_dir`. We are not specifying `zk` configuration so
  // the `quorum` will default to "1".
  MasterConfig masterOverlayConfig;
  masterOverlayConfig
    .set_replicated_log_dir("overlay_replicated_log");

  Try<Owned<Anonymous>> masterModule = startOverlayMaster(masterOverlayConfig);
  ASSERT_SOME(masterModule);

  // Master `Anonymous` module created successfully. Lets see if we
  // can hit the `state` endpoint of the Master.
  UPID overlayMaster = UPID(master.get()->pid);
  overlayMaster.id = MASTER_MANAGER_PROCESS_ID;

  AgentConfig agentOverlayConfig;
  agentOverlayConfig.set_master(stringify(overlayMaster.address));
  // Enable Mesos network.
  agentOverlayConfig.mutable_network_config()->set_mesos_bridge(true);
  // Enable Docker network.
  agentOverlayConfig.mutable_network_config()->set_docker_bridge(true);

  // Setup a future to notify the test that Agent overlay module has
  // registered.
  Future<AgentRegisteredAcknowledgement> agentRegisteredAcknowledgement =
    FUTURE_PROTOBUF(AgentRegisteredAcknowledgement(), _, _);

  Try<Owned<overlayAgent::ManagerProcess>> agentModule = startOverlayAgent(
      agentOverlayConfig);

  ASSERT_SOME(agentModule );

  AWAIT_READY(agentRegisteredAcknowledgement);

  // Check the agent is allowed to progress.
  AWAIT_READY(agentModule.get()->ready());

  // Kill the master.
  masterModule->reset();

  // Add an overlay network to the master configuration.
  //
  // NOTE: Since the `masterOverlayConfig` is "merged" with the stored
  // `masterOverlayConfig` ensure this is the only overlay network
  // delcared in the `masterOverlayConfig` object being passed into
  // `startOverlayMaster`.
  OverlayInfo overlay;
  overlay.set_name(OVERLAY_NAME_2);
  overlay.set_subnet("11.0.0.0/8");
  overlay.set_prefix(24);

  masterOverlayConfig.mutable_network()->add_overlays()->CopyFrom(overlay);

  masterModule = startOverlayMaster(masterOverlayConfig);
  ASSERT_SOME(masterModule);

  // Re-start the master and wait for the Agent to re-register.
  agentRegisteredAcknowledgement = FUTURE_PROTOBUF(
      AgentRegisteredAcknowledgement(), _, _);

  AWAIT_READY(agentRegisteredAcknowledgement);

  // Hit the master end-point again.
  Future<Response> masterResponse = process::http::get(
      overlayMaster,
      "state");

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, masterResponse);
  AWAIT_EXPECT_RESPONSE_HEADER_EQ(
      APPLICATION_JSON,
      "Content-Type",
      masterResponse);

  Try<State> state = parseMasterState(masterResponse->body);
  ASSERT_SOME(state);
  ASSERT_EQ(1, state->agents_size());

  ASSERT_TRUE(state->has_network());
  ASSERT_EQ(2, state->network().overlays_size());

  Option<OverlayInfo> _overlay;
  foreach(const OverlayInfo& __overlay, state->network().overlays()) {
    if (__overlay.name() == OVERLAY_NAME_2) {
      _overlay = __overlay;
      break;
    }
  }

  ASSERT_SOME(_overlay);
  ASSERT_EQ(_overlay->subnet(), "11.0.0.0/8");

  // Hit the `overlay` endpoint of the agent to check that module is
  // up and responding.
  UPID overlayAgent = UPID(master.get()->pid);
  overlayAgent.id = AGENT_MANAGER_PROCESS_ID;

  Future<Response> agentResponse = process::http::get(
      overlayAgent,
      "overlay");

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, agentResponse);
  AWAIT_EXPECT_RESPONSE_HEADER_EQ(
      APPLICATION_JSON,
      "Content-Type",
      agentResponse);

  // The `overlay` end-point is backed by the
  // mesos::modules::overlay::AgentInfo protobuf, so need to parse the
  // JSON and verify that the correct configuration is being
  // reflected.
  Try<AgentInfo> info = parseAgentOverlay(agentResponse->body);
  ASSERT_SOME(info);

  // There should be 2 overlays.
  ASSERT_EQ(2, info->overlays_size());

  Option<AgentOverlayInfo> agentOverlay;

  foreach(const AgentOverlayInfo& _agentOverlay, info->overlays()) {
    if (_agentOverlay.info().name() == OVERLAY_NAME_2) {
      agentOverlay = _agentOverlay;
      break;
    }
  }

  ASSERT_SOME(agentOverlay);
  ASSERT_EQ(agentOverlay->subnet(), "11.0.0.0/24");
  ASSERT_EQ(agentOverlay->info().subnet(), "11.0.0.0/8");
}

} // namespace tests {
} // namespace overlay {
} // namespace mesos {


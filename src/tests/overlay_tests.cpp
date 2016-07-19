/* file is © 2016 Mesosphere, Inc. (“Mesosphere”).  Mesosphere licenses
 * this file to you solely pursuant to the following terms (and you may not use
 * this file except in compliance with such terms):
 *
 * 1. Subject to your compliance with the following terms, Mesosphere hereby
 * grants you a nonexclusive, limited, personal, non-sublicensable,
 * non-transferable, royalty-free license to use this file solely for your
 * internal business purposes.
 *
 * 2. You may not (and agree not to, and not to authorize or enable others to),
 * directly or indirectly: (a) copy, distribute, rent, lease, timeshare, operate
 * a service bureau, or otherwise use for the benefit of a third party, this
 * file; or (b) remove any proprietary notices from this file.  Except as
 * expressly set forth herein, as between you and Mesosphere, Mesosphere retains
 * all right, title and interest in and to this file.
 *
 * 3. Unless required by applicable law or otherwise agreed to in writing,
 * Mesosphere provides this file on an "AS IS" BASIS, WITHOUT WARRANTIES OR
 * CONDITIONS OF ANY KIND, either express or implied, including, without
 * limitation, any warranties or conditions of TITLE, NON-INFRINGEMENT,
 * MERCHANTABILITY, or FITNESS FOR A PARTICULAR PURPOSE.
 *
 * 4. In no event and under no legal theory, whether in tort (including
 * negligence), contract, or otherwise, unless required by applicable law (such
 * as deliberate and grossly negligent acts) or agreed to in writing, shall
 * Mesosphere be liable to you for damages, including any direct, indirect,
 * special, incidental, or consequential damages of any character arising as a
 * result of these terms or out of the use or inability to use this file
 * (including but not limited to damages for loss of goodwill, work stoppage,
 * computer failure or malfunction, or any and all other commercial damages or
 * losses), even if Mesosphere has been advised of the possibility of such
 * damages.
 */

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

#include "agent/constants.hpp"

#include "hook/manager.hpp"

#include "master/detector/standalone.hpp"

#include "module/manager.hpp"

#include "overlay/overlay.hpp"
#include "overlay/overlay.pb.h"
#include "overlay/internal/messages.pb.h"
#include "overlay/internal/utils.hpp"

#include "slave/flags.hpp"

#include "tests/mesos.hpp"

using namespace process;

using namespace mesos::internal::tests;

using std::cout;
using std::endl;

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

using mesos::modules::Anonymous;
using mesos::modules::ModuleManager;
using mesos::modules::overlay::AgentInfo;
using mesos::modules::overlay::AGENT_MANAGER_PROCESS_ID;
using mesos::modules::overlay::MASTER_MANAGER_PROCESS_ID;
using mesos::modules::overlay::internal::AgentConfig;
using mesos::modules::overlay::internal::AgentRegisteredMessage;
using mesos::modules::overlay::internal::MasterConfig;
using mesos::modules::overlay::OverlayInfo;
using mesos::modules::overlay::State;
using mesos::modules::overlay::agent::IPSET_OVERLAY;
using mesos::modules::overlay::utils::runCommand;
using mesos::modules::overlay::utils::runScriptCommand;

namespace mesos {
namespace overlay {
namespace tests {

constexpr char OVERLAY_TEST_BASE_DIR[] = "/tmp/overlay-test/";
constexpr char MASTER_JSON_CONFIG[] = "master.json";
constexpr char AGENT_JSON_CONFIG[] = "agent.json";
constexpr char AGENT_CNI_DIR[] = "cni/";
constexpr char MASTER_OVERLAY_MODULE_NAME[] =
  "com_mesosphere_mesos_OverlayMasterManager";
constexpr char AGENT_OVERLAY_MODULE_NAME[] =
  "com_mesosphere_mesos_OverlayAgentManager";

class OverlayTest : public MesosTest 
{ 
protected:
  virtual void SetUp()
  {
    MesosTest::SetUp();

    auto modulesConfig = [](JSON::ObjectWriter* writer) {
      writer->field("libraries", [](JSON::ArrayWriter* writer) {
          writer->element([](JSON::ObjectWriter* writer) {
            writer->field(
              "file",
              path::join(MODULES_BUILD_DIR,
                         ".libs/libmesos_network_overlay.so"));
            writer->field("modules", [](JSON::ArrayWriter* writer) {
              auto masterModuleConfig = [](JSON::ObjectWriter* writer) {
              writer->field("name", "com_mesosphere_mesos_OverlayMasterManager");
              writer->field("parameters", [](JSON::ArrayWriter* writer) {
                writer->element([](JSON::ObjectWriter* writer) {
                  writer->field("key", "master_config");
                  writer->field("value",
                    path::join(OVERLAY_TEST_BASE_DIR, MASTER_JSON_CONFIG));
                  });
                });
              };

              auto agentModuleConfig = [](JSON::ObjectWriter* writer) {
              writer->field("name", "com_mesosphere_mesos_OverlayAgentManager");
              writer->field("parameters", [](JSON::ArrayWriter* writer) {
                writer->element([](JSON::ObjectWriter* writer) {
                  writer->field("key", "agent_config");
                  writer->field("value",
                    path::join(OVERLAY_TEST_BASE_DIR, AGENT_JSON_CONFIG));
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
  }

  virtual void TearDown()
  {
    LOG(INFO) << "Unloading all modules...";
    // Unload all modules.
    foreach (const Modules::Library& library, modules.libraries()) {
      foreach (const Modules::Library::Module& module, library.modules()) {
        if (module.has_name()) {
          ASSERT_SOME(ModuleManager::unload(module.name()));
        }
      }
    }

    // Clean all the directories that we created.
    os::rmdir(OVERLAY_TEST_BASE_DIR);
  }

  void startOverlayMaster(const MasterConfig& masterOverlayConfig)
  {
    Try<Nothing> mkdir = os::mkdir(OVERLAY_TEST_BASE_DIR);
    EXPECT_SOME(mkdir);

    Try<Nothing> write = os::write(
        path::join(OVERLAY_TEST_BASE_DIR, MASTER_JSON_CONFIG),
        stringify(JSON::protobuf(masterOverlayConfig)));
    EXPECT_SOME(write);

    Try<Anonymous*> create = ModuleManager::create<Anonymous>(
        MASTER_OVERLAY_MODULE_NAME);
    ASSERT_SOME(create);

    masterOverlayModule = Owned<Anonymous>(create.get());
  }

  void startOverlayAgent(const AgentConfig& agentOverlayConfig)
  {
    Try<Nothing> mkdir = os::mkdir(Path(AGENT_JSON_CONFIG).dirname());
    EXPECT_SOME(mkdir);

    Try<Nothing> write = os::write(
        path::join(OVERLAY_TEST_BASE_DIR, AGENT_JSON_CONFIG),
        stringify(JSON::protobuf(agentOverlayConfig)));
    EXPECT_SOME(write);

    Try<Anonymous*> create = ModuleManager::create<Anonymous>(
        AGENT_OVERLAY_MODULE_NAME);
    ASSERT_SOME(create);

    agentOverlayModule = Owned<Anonymous>(create.get());
  }

  Try<State> parseMasterState(const string& state)
  {
    Try<JSON::Object> json = JSON::parse<JSON::Object>(state);
    if (json.isError()) {
      return Error("JSON parse failed: " + json.error());
    }

    Try<State> parse = ::protobuf::parse<State>(json.get());

    return parse;
  }

  Try<AgentInfo> parseAgentOverlay(const string& info)
  {
    Try<JSON::Object> json = JSON::parse<JSON::Object>(info);
    if (json.isError()) {
      return Error("JSON parse failed: " + json.error());
    }

    Try<AgentInfo> parse = ::protobuf::parse<AgentInfo>(json.get());

    return parse;
  }

private:
  Modules modules;

  Owned<Anonymous> masterOverlayModule;
  Owned<Anonymous> agentOverlayModule;
};


// Tests the ability of the `Master overlay module` to allocate
// subnets to Agents for a given overlay network.
TEST_F(OverlayTest, checkMasterAgentComm)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  LOG(INFO) << "Master PID: " << master.get()->pid;

  MasterConfig masterOverlayConfig;
  masterOverlayConfig.mutable_network()->set_vtep_subnet("44.128.0.0/16");
  masterOverlayConfig.mutable_network()->set_vtep_mac_oui("70:B3:D5:00:00:00");

  OverlayInfo overlay;
  overlay.set_name("overlay-1");
  overlay.set_subnet("192.168.0.0/16");
  overlay.set_prefix(24);

  masterOverlayConfig.mutable_network()->add_overlays()->CopyFrom(overlay);

  startOverlayMaster(masterOverlayConfig);

  // Master `Anonymous` module created successfully. Lets see if we
  // can hit the `state` endpoint of the Master.
  UPID overlayMaster = UPID(master.get()->pid);
  overlayMaster.id = MASTER_MANAGER_PROCESS_ID;

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
  ASSERT_EQ("overlay-1", state->network().overlays(0).name());
  ASSERT_EQ("192.168.0.0/16", state->network().overlays(0).subnet());
  ASSERT_EQ(24, state->network().overlays(0).prefix());

  // We haven't started the Agent, so make sure there are no Agents
  // reflected at this end-point.
  ASSERT_EQ(0, state->agents_size());

  AgentConfig agentOverlayConfig;
  agentOverlayConfig.set_master(stringify(overlayMaster.address));
  agentOverlayConfig.set_cni_dir(path::join(
        OVERLAY_TEST_BASE_DIR,
        AGENT_CNI_DIR));
  agentOverlayConfig.mutable_network_config()->set_allocate_subnet(true);
  agentOverlayConfig.mutable_network_config()->set_mesos_bridge(false);
  agentOverlayConfig.mutable_network_config()->set_docker_bridge(false);

  // Setup a future to notify the test that Agent overlay module has
  // registered.
  Future<AgentRegisteredMessage> agentRegisteredMessage = 
    FUTURE_PROTOBUF(AgentRegisteredMessage(), _, _);

  startOverlayAgent(agentOverlayConfig);

  AWAIT_READY(agentRegisteredMessage);

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
  EXPECT_EQ("overlay-1", info->overlays(0).info().name());
  
  Try<net::IPNetwork> agentNetwork = net::IPNetwork::parse(
      info->overlays(0).subnet(), AF_INET);
  ASSERT_SOME(agentNetwork);
  EXPECT_EQ(24, agentNetwork->prefix());
  
  Try<net::IPNetwork> allocatedSubnet = net::IPNetwork::parse(
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
  EXPECT_TRUE(
      info.get().SerializeAsString() == masterAgentInfo.SerializeAsString());
}


// Tests the ability of the `Agent overlay module` to create Mesos CNI
// networks when `mesos bridge` has been enabled.
TEST_F(OverlayTest, checkMesosNetwork)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  LOG(INFO) << "Master PID: " << master.get()->pid;

  MasterConfig masterOverlayConfig;
  masterOverlayConfig.mutable_network()->set_vtep_subnet("44.128.0.0/16");
  masterOverlayConfig.mutable_network()->set_vtep_mac_oui("70:B3:D5:00:00:00");

  OverlayInfo overlay;
  overlay.set_name("overlay-1");
  overlay.set_subnet("192.168.0.0/16");
  overlay.set_prefix(24);

  masterOverlayConfig.mutable_network()->add_overlays()->CopyFrom(overlay);

  startOverlayMaster(masterOverlayConfig);

  // Master `Anonymous` module created successfully. Lets see if we
  // can hit the `state` endpoint of the Master.
  UPID overlayMaster = UPID(master.get()->pid);
  overlayMaster.id = MASTER_MANAGER_PROCESS_ID;

  AgentConfig agentOverlayConfig;
  agentOverlayConfig.set_master(stringify(overlayMaster.address));
  agentOverlayConfig.set_cni_dir(path::join(
        OVERLAY_TEST_BASE_DIR,
        AGENT_CNI_DIR));
  agentOverlayConfig.mutable_network_config()->set_allocate_subnet(true);
  agentOverlayConfig.mutable_network_config()->set_mesos_bridge(true);
  agentOverlayConfig.mutable_network_config()->set_docker_bridge(false);

  // Setup a future to notify the test that Agent overlay module has
  // registered.
  Future<AgentRegisteredMessage> agentRegisteredMessage = 
    FUTURE_PROTOBUF(AgentRegisteredMessage(), _, _);

  startOverlayAgent(agentOverlayConfig);

  AWAIT_READY(agentRegisteredMessage);

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
      "-s", "192.168.0.0/16",
      "-m", "set",
      "--match-set", stringify(IPSET_OVERLAY), "dst",
      "-j", "MASQUERADE",
      });
  AWAIT_READY(iptables);

  // Verify the CNI configuration has been installed correctly.
  Try<string> cniConfig = os::read(path::join(
        OVERLAY_TEST_BASE_DIR,
        "cni",
        "overlay-1.cni"));
  ASSERT_SOME(cniConfig);

  Try<JSON::Object> json = JSON::parse<JSON::Object>(cniConfig.get());
  ASSERT_SOME(json);

  Result<JSON::String> network = json->find<JSON::String>("name");
  ASSERT_SOME(network);
  EXPECT_EQ(network.get(), "overlay-1");

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

  // Tests have passed cleanup.
  iptables = runCommand("iptables",
      {"iptables",
      "-t", "nat",
      "-D", "POSTROUTING",
      "-s", "192.168.0.0/16",
      "-m", "set",
      "--match-set", stringify(IPSET_OVERLAY), "dst",
      "-j", "MASQUERADE",
      });
  AWAIT_READY(iptables);

  ipset = runCommand("ipset",
      {"ipset",
      "destroy",
      IPSET_OVERLAY});
  AWAIT_READY(ipset);
}


// Tests the ability of the `Agent overlay module` to create Docker
// network.
TEST_F(OverlayTest, checkDockerNetwork)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  LOG(INFO) << "Master PID: " << master.get()->pid;

  MasterConfig masterOverlayConfig;
  masterOverlayConfig.mutable_network()->set_vtep_subnet("44.128.0.0/16");
  masterOverlayConfig.mutable_network()->set_vtep_mac_oui("70:B3:D5:00:00:00");

  OverlayInfo overlay;
  overlay.set_name("overlay-1");
  overlay.set_subnet("192.168.0.0/16");
  overlay.set_prefix(24);

  masterOverlayConfig.mutable_network()->add_overlays()->CopyFrom(overlay);

  startOverlayMaster(masterOverlayConfig);

  // Master `Anonymous` module created successfully. Lets see if we
  // can hit the `state` endpoint of the Master.
  UPID overlayMaster = UPID(master.get()->pid);
  overlayMaster.id = MASTER_MANAGER_PROCESS_ID;

  AgentConfig agentOverlayConfig;
  agentOverlayConfig.set_master(stringify(overlayMaster.address));
  agentOverlayConfig.set_cni_dir(path::join(
        OVERLAY_TEST_BASE_DIR,
        AGENT_CNI_DIR));
  agentOverlayConfig.mutable_network_config()->set_allocate_subnet(true);
  agentOverlayConfig.mutable_network_config()->set_mesos_bridge(false);
  agentOverlayConfig.mutable_network_config()->set_docker_bridge(true);

  // Setup a future to notify the test that Agent overlay module has
  // registered.
  Future<AgentRegisteredMessage> agentRegisteredMessage = 
    FUTURE_PROTOBUF(AgentRegisteredMessage(), _, _);

  startOverlayAgent(agentOverlayConfig);

  AWAIT_READY(agentRegisteredMessage);

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
      "-s", "192.168.0.0/16",
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
   "overlay-1"});
  AWAIT_READY(docker);

  Try<JSON::Array> json = JSON::parse<JSON::Array>(docker.get());
  ASSERT_SOME(json);

  // Tests have passed clean up
  docker = runCommand("docker",
  {"docker",
   "network",
   "rm",
   "overlay-1"});
  AWAIT_READY(docker);

  iptables = runCommand("iptables",
      {"iptables",
      "-t", "nat",
      "-D", "POSTROUTING",
      "-s", "192.168.0.0/16",
      "-m", "set",
      "--match-set", stringify(IPSET_OVERLAY), "dst",
      "-j", "MASQUERADE",
      });
  AWAIT_READY(iptables);

  ipset = runCommand("ipset",
      {"ipset",
      "destroy",
      IPSET_OVERLAY});
  AWAIT_READY(ipset);
}

} // namespace tests {
} // namespace overlay {
} // namespace mesos {


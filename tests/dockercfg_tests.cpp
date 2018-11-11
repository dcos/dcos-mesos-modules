#include <string>
#include <vector>

#include <gmock/gmock.h>

#include <mesos/hook.hpp>
#include <mesos/mesos.hpp>
#include <mesos/resources.hpp>

#include <mesos/module/module.hpp>

#include <mesos/scheduler/scheduler.hpp>

#include <mesos/slave/isolator.hpp>

#include <process/future.hpp>
#include <process/gmock.hpp>
#include <process/gtest.hpp>
#include <process/process.hpp>
#include <process/owned.hpp>

#include <stout/gtest.hpp>
#include <stout/json.hpp>
#include <stout/option.hpp>
#include <stout/os.hpp>
#include <stout/path.hpp>

#include <stout/os/read.hpp>

#include "hook/manager.hpp"

#include "module/manager.hpp"

#include "slave/flags.hpp"

#include "tests/mesos.hpp"

using namespace process;
using namespace mesos::internal::tests;

using std::string;
using std::vector;

using mesos::internal::HookManager;

using mesos::internal::master::Master;

using mesos::internal::slave::Slave;
using mesos::internal::slave::Containerizer;

using mesos::master::detector::MasterDetector;

using mesos::modules::ModuleManager;

namespace mesos {
namespace dockerRemove {
namespace tests {

const char REMOVER_HOOK_NAME[] = "com_mesosphere_dcos_RemoverHook";


class DockerRemoveTest : public MesosTest {
protected:
  virtual void SetUp()
  {
    MesosTest::SetUp();

    // Read in the example `modules.json`.
    Try<std::string> read =
      os::read(path::join(MODULES_BUILD_DIR, "dockercfg", "modules.json"));
    ASSERT_SOME(read);

    Try<JSON::Object> json = JSON::parse<JSON::Object>(read.get());
    ASSERT_SOME(json);

    Try<Modules> _modules = protobuf::parse<Modules>(json.get());
    ASSERT_SOME(_modules);

    modules = _modules.get();

    // Initialize the modules and hook.
    Try<Nothing> result = ModuleManager::load(modules);
    ASSERT_SOME(result);

    result = HookManager::initialize(REMOVER_HOOK_NAME);
    ASSERT_SOME(result);
  }

  virtual void TearDown()
  {
    // Unload all modules.
    foreach (const Modules::Library& library, modules.libraries()) {
      foreach (const Modules::Library::Module& module, library.modules()) {
        if (module.has_name()) {
          ASSERT_SOME(ModuleManager::unload(module.name()));
        }
      }
    }

    // Attempt to unload the hook.
    HookManager::unload(REMOVER_HOOK_NAME);

    MesosTest::TearDown();
  }

private:
  Modules modules;
};


// Test that the module correctly removes .dockercfg files.
TEST_F(DockerRemoveTest, VerifyRemoveDockerCfgHook)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  // NOTE: Modules and hooks are loaded in the test setup.
  mesos::internal::slave::Flags flags = CreateSlaveFlags();

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> slave = StartSlave(
      detector.get(),
      flags);

  ASSERT_SOME(slave);

  MockScheduler sched;

  MesosSchedulerDriver driver(
      &sched,
      DEFAULT_FRAMEWORK_INFO,
      master.get()->pid,
      DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
  ASSERT_NE(0u, offers->size());

  TaskInfo task = createTask(
      offers.get()[0].slave_id(),
      Resources::parse("cpus:1;mem:128").get(),
      "test ! -f " + path::join(flags.sandbox_directory, ".dockercfg"));

  // Add a URI for a file on the host filesystem. This file will be
  // fetched to the sandbox and will later be deleted by the hook.
  const string file = path::join(sandbox.get(), ".dockercfg");
  ASSERT_SOME(os::touch(file));

  CommandInfo::URI* uri = task.mutable_command()->add_uris();
  uri->set_value(file);

  Future<TaskStatus> statusStarting;
  Future<TaskStatus> statusRunning;
  Future<TaskStatus> statusFinished;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusStarting))
    .WillOnce(FutureArg<1>(&statusRunning))
    .WillOnce(FutureArg<1>(&statusFinished));

  driver.launchTasks(offers.get()[0].id(), {task});

  AWAIT_READY_FOR(statusStarting, Seconds(60));
  EXPECT_EQ(TASK_STARTING, statusStarting.get().state());

  AWAIT_READY_FOR(statusRunning, Seconds(60));
  EXPECT_EQ(TASK_RUNNING, statusRunning.get().state());

  AWAIT_READY_FOR(statusFinished, Seconds(60));
  EXPECT_EQ(TASK_FINISHED, statusFinished.get().state());

  driver.stop();
  driver.join();
}

} // namespace tests {
} // namespace dockerRemove {
} // namespace mesos {

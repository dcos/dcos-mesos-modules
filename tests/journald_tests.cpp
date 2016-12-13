#include <string>
#include <vector>

#include <gmock/gmock.h>

#include <mesos/hook.hpp>
#include <mesos/mesos.hpp>
#include <mesos/resources.hpp>

#include <mesos/module/module.hpp>

#include <mesos/scheduler/scheduler.hpp>

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
#include <stout/protobuf.hpp>
#include <stout/strings.hpp>
#include <stout/try.hpp>

#include <stout/os/read.hpp>

#include "common/shell.hpp"

#include "module/manager.hpp"

#include "slave/flags.hpp"

#include "slave/containerizer/docker.hpp"

#include "tests/mesos.hpp"

using namespace process;

using namespace mesos::internal::tests;

using std::string;
using std::vector;

using mesos::internal::master::Master;

using mesos::internal::slave::DockerContainerizer;
using mesos::internal::slave::Fetcher;
using mesos::internal::slave::MesosContainerizer;
using mesos::internal::slave::Slave;

using mesos::master::detector::MasterDetector;

using mesos::modules::ModuleManager;

using mesos::modules::common::runCommand;

namespace systemd {

// Forward declare a function and required class located in
// `src/linux/systemd.cpp`. This is not exposed in the Mesos public
// headers, but is necessary to start the test.
class Flags : public virtual flags::FlagsBase
{
public:
  Flags() {
    add(&Flags::enabled,
        "enabled",
        "Top level control of systemd support. When enabled, features such as\n"
        "processes life-time extension are enabled unless there is an explicit\n"
        "flag to disable these (see other flags).",
        true);

    add(&Flags::runtime_directory,
        "runtime_directory",
        "The path to the systemd system run time directory\n",
        "/run/systemd/system");

    add(&Flags::cgroups_hierarchy,
        "cgroups_hierarchy",
        "The path to the cgroups hierarchy root\n",
        "/sys/fs/cgroup");
  }

  bool enabled;
  std::string runtime_directory;
  std::string cgroups_hierarchy;
};

Try<Nothing> initialize(const Flags& flags);

} // namespace systemd {


namespace mesos {
namespace journald {
namespace tests {

const char JOURNALD_LOGGER_NAME[] = "com_mesosphere_mesos_JournaldLogger";


class JournaldLoggerTest : public MesosTest {
protected:
  static void SetUpTestCase()
  {
    // NOTE: This code is normally run in `src/slave/main.cpp`.
    systemd::Flags systemdFlags;

    Try<Nothing> initialize = systemd::initialize(systemdFlags);
    if (initialize.isError()) {
      EXIT(EXIT_FAILURE)
        << "Failed to initialize systemd: " + initialize.error();
    }

    // For convenience, set this value to path of libmesos.so.
    // The logger's companion binary needs to be able to find
    // libmesos from the agent's environment.
    os::setenv("LD_LIBRARY_PATH", path::join(BUILD_DIR, "src/.libs"));
  }

  virtual void SetUp()
  {
    MesosTest::SetUp();

    // Read in the example `modules.json`.
    Try<std::string> read =
      os::read(path::join(MODULES_BUILD_DIR, "journald", "modules.json"));
    ASSERT_SOME(read);

    Try<JSON::Object> json = JSON::parse<JSON::Object>(read.get());
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
    // Unload all modules.
    foreach (const Modules::Library& library, modules.libraries()) {
      foreach (const Modules::Library::Module& module, library.modules()) {
        if (module.has_name()) {
          ASSERT_SOME(ModuleManager::unload(module.name()));
        }
      }
    }

    MesosTest::TearDown();
  }

private:
  Modules modules;
};


// Loads the journald ContainerLogger module and runs a task.
// Then queries journald for the associated logs.
TEST_F(JournaldLoggerTest, ROOT_LogToJournald)
{
  // Create a master, agent, and framework.
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  // We'll need access to these flags later.
  mesos::internal::slave::Flags flags = CreateSlaveFlags();

  // Use the journald container logger.
  flags.container_logger = JOURNALD_LOGGER_NAME;

  Fetcher fetcher;

  // We use an actual containerizer + executor since we want something to run.
  Try<MesosContainerizer*> _containerizer =
    MesosContainerizer::create(flags, false, &fetcher);

  CHECK_SOME(_containerizer);
  Owned<MesosContainerizer> containerizer(_containerizer.get());

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> slave =
    StartSlave(detector.get(), containerizer.get(), flags);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

  Future<FrameworkID> frameworkId;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureArg<1>(&frameworkId));

  // Wait for an offer, and start a task.
  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();
  AWAIT_READY(frameworkId);

  AWAIT_READY(offers);
  EXPECT_NE(0u, offers.get().size());

  const std::string specialString = "some-super-unique-string";

  TaskInfo task = createTask(offers.get()[0], "echo " + specialString);

  Future<TaskStatus> statusRunning;
  Future<TaskStatus> statusFinished;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusRunning))
    .WillOnce(FutureArg<1>(&statusFinished))
    .WillRepeatedly(Return());       // Ignore subsequent updates.

  driver.launchTasks(offers.get()[0].id(), {task});

  AWAIT_READY(statusRunning);
  EXPECT_EQ(TASK_RUNNING, statusRunning.get().state());

  AWAIT_READY(statusFinished);
  EXPECT_EQ(TASK_FINISHED, statusFinished.get().state());

  driver.stop();
  driver.join();

  // Query journald via the FrameworkID, AgentID, and ExecutorID
  // and check for the special string.
  Future<string> frameworkQuery = runCommand(
      "journalctl",
      {"journalctl",
       "FRAMEWORK_ID=" + frameworkId.get().value()});

  Future<string> agentQuery = runCommand(
      "journalctl",
      {"journalctl",
       "AGENT_ID=" + offers.get()[0].slave_id().value()});

  Future<string> executorQuery = runCommand(
      "journalctl",
      {"journalctl",
       "EXECUTOR_ID=" + statusRunning->executor_id().value()});

  AWAIT_READY(frameworkQuery);
  ASSERT_TRUE(strings::contains(frameworkQuery.get(), specialString));

  AWAIT_READY(agentQuery);
  ASSERT_TRUE(strings::contains(agentQuery.get(), specialString));

  AWAIT_READY(executorQuery);
  ASSERT_TRUE(strings::contains(executorQuery.get(), specialString));
}


// Loads the journald ContainerLogger module and runs a docker task.
// Then queries journald for the associated logs.
TEST_F(JournaldLoggerTest, ROOT_DOCKER_LogToJournald)
{
  // Create a master, agent, and framework.
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  // We'll need access to these flags later.
  mesos::internal::slave::Flags flags = CreateSlaveFlags();

  // Use the journald container logger.
  flags.container_logger = JOURNALD_LOGGER_NAME;

  Fetcher fetcher;

  Try<DockerContainerizer*> _containerizer =
    DockerContainerizer::create(flags, &fetcher);

  CHECK_SOME(_containerizer);
  Owned<DockerContainerizer> containerizer(_containerizer.get());

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> slave =
    StartSlave(detector.get(), containerizer.get(), flags);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

  Future<FrameworkID> frameworkId;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureArg<1>(&frameworkId));

  // Wait for an offer, and start a task.
  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();
  AWAIT_READY(frameworkId);

  AWAIT_READY(offers);
  EXPECT_NE(0u, offers.get().size());

  const std::string specialString = "some-super-unique-string";

  TaskInfo task = createTask(offers.get()[0], "echo " + specialString);

  ContainerInfo containerInfo;
  containerInfo.set_type(ContainerInfo::DOCKER);

  ContainerInfo::DockerInfo dockerInfo;
  dockerInfo.set_image("alpine");
  containerInfo.mutable_docker()->CopyFrom(dockerInfo);

  task.mutable_container()->CopyFrom(containerInfo);

  Future<TaskStatus> statusRunning;
  Future<TaskStatus> statusFinished;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusRunning))
    .WillOnce(FutureArg<1>(&statusFinished))
    .WillRepeatedly(Return());       // Ignore subsequent updates.

  driver.launchTasks(offers.get()[0].id(), {task});

  AWAIT_READY(statusRunning);
  EXPECT_EQ(TASK_RUNNING, statusRunning.get().state());

  AWAIT_READY(statusFinished);
  EXPECT_EQ(TASK_FINISHED, statusFinished.get().state());

  driver.stop();
  driver.join();

  // Query journald via the FrameworkID, AgentID, and ExecutorID
  // and check for the special string.
  Future<string> frameworkQuery = runCommand(
      "journalctl",
      {"journalctl",
       "FRAMEWORK_ID=" + frameworkId.get().value()});

  Future<string> agentQuery = runCommand(
      "journalctl",
      {"journalctl",
       "AGENT_ID=" + offers.get()[0].slave_id().value()});

  Future<string> executorQuery = runCommand(
      "journalctl",
      {"journalctl",
       "EXECUTOR_ID=" + statusRunning->executor_id().value()});

  AWAIT_READY(frameworkQuery);
  ASSERT_TRUE(strings::contains(frameworkQuery.get(), specialString));

  AWAIT_READY(agentQuery);
  ASSERT_TRUE(strings::contains(agentQuery.get(), specialString));

  AWAIT_READY(executorQuery);
  ASSERT_TRUE(strings::contains(executorQuery.get(), specialString));
}

} // namespace tests {
} // namespace journald {
} // namespace mesos {

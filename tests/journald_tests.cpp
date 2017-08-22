#include <map>
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

using std::vector;

using mesos::internal::master::Master;

using mesos::internal::slave::DockerContainerizer;
using mesos::internal::slave::Fetcher;
using mesos::internal::slave::MesosContainerizer;
using mesos::internal::slave::Slave;

using mesos::master::detector::MasterDetector;

using mesos::modules::ModuleManager;

using mesos::modules::common::runCommand;

using testing::WithParamInterface;

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


class JournaldLoggerTest : public MesosTest,
                           public WithParamInterface<std::string>
{
public:
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
TEST_F(JournaldLoggerTest, ROOT_LogToJournaldWithBigLabel)
{
  // Create a master, agent, and framework.
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  // We'll need access to these flags later.
  mesos::internal::slave::Flags flags = CreateSlaveFlags();

  // Use the journald container logger.
  flags.container_logger = JOURNALD_LOGGER_NAME;

  Fetcher fetcher(flags);

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

  // Add a short label.
  Label* label = task.mutable_labels()->add_labels();
  label->set_key("TINY");
  label->set_value("present");

  // Add a huge label.
  label = task.mutable_labels()->add_labels();
  label->set_key("HUGE");
  {
    std::string fiftyKilobyteString;
    fiftyKilobyteString.reserve(50000u);

    for (int i = 0; i < 5000; i++) {
      fiftyKilobyteString += "0123456789";
    }

    label->set_value(fiftyKilobyteString);
  }

  // Add another short label.
  // This tests an implementation detail, where we exclude labels in their
  // order of occurrence. This means, if you add a huge label at the beginning,
  // all subsequent labels will also be excluded from the metadata.
  label = task.mutable_labels()->add_labels();
  label->set_key("SMALL");
  label->set_value("excluded");

  // Make sure the destination of the logs is journald.
  Environment::Variable* variable =
    task.mutable_command()->mutable_environment()->add_variables();
  variable->set_name("CONTAINER_LOGGER_DESTINATION_TYPE");
  variable->set_value("journald");

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

  // Query journald via the FrameworkID (to disambiguate between test runs)
  // and the freeform labels. The first freeform label should be present,
  // but the second one should not.
  Future<std::string> firstQuery = runCommand(
      "journalctl",
      {"journalctl",
       "FRAMEWORK_ID=" + frameworkId.get().value(),
       "TINY=present"});

  Future<std::string> secondQuery = runCommand(
      "journalctl",
      {"journalctl",
       "FRAMEWORK_ID=" + frameworkId.get().value(),
       "SMALL=excluded"});

  AWAIT_READY(firstQuery);
  ASSERT_TRUE(strings::contains(firstQuery.get(), specialString));

  AWAIT_READY(secondQuery);
  ASSERT_FALSE(strings::contains(secondQuery.get(), specialString));
}


// This checks a specific case where the arguments passed into the
// ContainerLogger will differ if the Mesos agent restarts before
// launching a nested container.
TEST_F(JournaldLoggerTest, ROOT_CGROUPS_LaunchThenRecoverThenLaunchNested)
{
  mesos::internal::slave::Flags flags = CreateSlaveFlags();
  flags.launcher = "linux";
  flags.isolation = "cgroups/cpu,filesystem/linux,namespaces/pid";

  // Use the journald container logger.
  flags.container_logger = JOURNALD_LOGGER_NAME;

  Fetcher fetcher(flags);

  Try<MesosContainerizer*> create = MesosContainerizer::create(
      flags,
      false,
      &fetcher);

  ASSERT_SOME(create);

  Owned<MesosContainerizer> containerizer(create.get());

  // Generate an AgentID to "recover" the MesosContainerizer with.
  mesos::internal::slave::state::SlaveState state;
  state.id = SlaveID();
  state.id.set_value(UUID::random().toString());

  AWAIT_READY(containerizer->recover(state));

  ContainerID containerId;
  containerId.set_value(UUID::random().toString());

  const std::string specialParentString = "special-parent-string";

  // We want to print a special string to stdout and then sleep
  // so that there is enough time to launch a nested container.
  ExecutorInfo executor = createExecutorInfo(
      "executor",
      "echo '" + specialParentString + "' && sleep 1000",
      "cpus:1");

  executor.mutable_framework_id()->set_value(UUID::random().toString());

  // Make a valid ExecutorRunPath, much like the
  // `slave::paths::getExecutorRunPath` helper (that we don't have access to).
  const std::string executorRunPath = path::join(
      flags.work_dir,
      "slaves", stringify(state.id),
      "frameworks", stringify(executor.framework_id()),
      "executors", stringify(executor.executor_id()),
      "runs", stringify(containerId));

  ASSERT_SOME(os::mkdir(executorRunPath));

  // Launch the top-level/parent container.
  // We need to checkpoint it so that it will survive recovery
  // (hence the `forked.pid` argument).
  Future<bool> launch = containerizer->launch(
      containerId,
      createContainerConfig(None(), executor, executorRunPath),
      std::map<std::string, std::string>(),
      path::join(flags.work_dir,
          "meta",
          "slaves", stringify(state.id),
          "frameworks", stringify(executor.framework_id()),
          "executors", stringify(executor.executor_id()),
          "runs", stringify(containerId),
          "pids", "forked.pid"));

  AWAIT_ASSERT_TRUE(launch);

  Future<ContainerStatus> status = containerizer->status(containerId);
  AWAIT_READY(status);
  ASSERT_TRUE(status->has_executor_pid());

  pid_t pid = status->executor_pid();

  // Emulate a Mesos agent restart by destroying and recreating the
  // MesosContainerizer object.
  containerizer.reset();

  create = MesosContainerizer::create(
      flags,
      false,
      &fetcher);

  ASSERT_SOME(create);

  containerizer.reset(create.get());

  // Create a mock `SlaveState`.
  mesos::internal::slave::state::ExecutorState executorState;
  executorState.id = executor.executor_id();
  executorState.info = executor;
  executorState.latest = containerId;

  mesos::internal::slave::state::RunState runState;
  runState.id = containerId;
  runState.forkedPid = pid;
  executorState.runs.put(containerId, runState);

  mesos::internal::slave::state::FrameworkState frameworkState;
  frameworkState.id = executor.framework_id();
  frameworkState.executors.put(executor.executor_id(), executorState);

  mesos::internal::slave::state::SlaveState slaveState;
  slaveState.id = state.id;
  slaveState.frameworks.put(executor.framework_id(), frameworkState);

  // Recover by using the mock `SlaveState`.
  AWAIT_READY(containerizer->recover(slaveState));

  status = containerizer->status(containerId);
  AWAIT_READY(status);
  ASSERT_TRUE(status->has_executor_pid());
  EXPECT_EQ(pid, status->executor_pid());

  // Now launch the nested container.
  ContainerID nestedContainerId;
  nestedContainerId.mutable_parent()->CopyFrom(containerId);
  nestedContainerId.set_value(UUID::random().toString());

  const std::string specialChildString = "special-child-string";

  // This one can be short lived.
  // We just need it to print a child-specific string and then exit.
  launch = containerizer->launch(
      nestedContainerId,
      createContainerConfig(createCommandInfo(
          "echo '" + specialChildString + "'")),
      std::map<std::string, std::string>(),
      None());

  AWAIT_ASSERT_TRUE(launch);

  status = containerizer->status(nestedContainerId);
  AWAIT_READY(status);
  ASSERT_TRUE(status->has_executor_pid());

  // Wait for the nested container to finish.
  Future<Option<mesos::slave::ContainerTermination>> nestedWait =
    containerizer->wait(nestedContainerId);

  AWAIT_READY(nestedWait);
  ASSERT_SOME(nestedWait.get());

  // Destroy the parent container.
  Future<Option<mesos::slave::ContainerTermination>> wait =
    containerizer->wait(containerId);

  containerizer->destroy(containerId);

  AWAIT_READY(wait);
  ASSERT_SOME(wait.get());
  ASSERT_TRUE(wait.get()->has_status());
  EXPECT_WTERMSIG_EQ(SIGKILL, wait.get()->status());

  // Now run two filters based on AGENT_ID and FRAMEWORK_ID.
  // Normally, we would expect to get the same result from each filter.
  // But in fact, we should *not* find the child string inside the
  // query based on FRAMEWORK_ID because the MesosContainerizer
  // will not persist that information after the emulated restart.
  Future<std::string> firstQuery = runCommand(
      "journalctl",
      {"journalctl",
      "AGENT_ID=" + stringify(state.id)});

  Future<std::string> secondQuery = runCommand(
      "journalctl",
      {"journalctl",
       "FRAMEWORK_ID=" + stringify(executor.framework_id())});

  AWAIT_READY(firstQuery);
  EXPECT_TRUE(strings::contains(firstQuery.get(), specialParentString));
  EXPECT_TRUE(strings::contains(firstQuery.get(), specialChildString));

  AWAIT_READY(secondQuery);
  EXPECT_TRUE(strings::contains(secondQuery.get(), specialParentString));
  EXPECT_FALSE(strings::contains(secondQuery.get(), specialChildString));
}


INSTANTIATE_TEST_CASE_P(
    LoggingMode,
    JournaldLoggerTest,
    ::testing::Values(
        std::string("journald"),
        std::string("logrotate"),
        std::string("journald+logrotate")));


// Loads the journald ContainerLogger module and runs a task.
// Then queries journald for the associated logs.
TEST_P(JournaldLoggerTest, ROOT_LogToJournald)
{
  // Create a master, agent, and framework.
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  // We'll need access to these flags later.
  mesos::internal::slave::Flags flags = CreateSlaveFlags();

  // Use the journald container logger.
  flags.container_logger = JOURNALD_LOGGER_NAME;

  Fetcher fetcher(flags);

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

  // Change the destination of the logs based on the parameterized test.
  Environment::Variable* variable =
    task.mutable_command()->mutable_environment()->add_variables();
  variable->set_name("CONTAINER_LOGGER_DESTINATION_TYPE");
  variable->set_value(GetParam());

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

  if (GetParam() == "journald" ||
      GetParam() == "journald+logrotate") {
    // Query journald via the FrameworkID, AgentID, and ExecutorID
    // and check for the special string.
    Future<std::string> frameworkQuery = runCommand(
        "journalctl",
        {"journalctl",
         "FRAMEWORK_ID=" + frameworkId.get().value()});

    Future<std::string> agentQuery = runCommand(
        "journalctl",
        {"journalctl",
         "AGENT_ID=" + offers.get()[0].slave_id().value()});

    Future<std::string> executorQuery = runCommand(
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

  std::string sandboxDirectory = path::join(
      flags.work_dir,
      "slaves",
      offers.get()[0].slave_id().value(),
      "frameworks",
      frameworkId.get().value(),
      "executors",
      statusRunning->executor_id().value(),
      "runs",
      "latest");
  ASSERT_TRUE(os::exists(sandboxDirectory));

  std::string stdoutPath = path::join(sandboxDirectory, "stdout");

  if (GetParam() == "journald") {
    // Check that the sandbox was *not* written to.
    // TODO(josephw): This file exists as other parts of the agent will
    // create it.  We should invert this assertion when we fix this in Mesos.
    ASSERT_TRUE(os::exists(stdoutPath));

    Result<std::string> stdout = os::read(stdoutPath);
    ASSERT_SOME(stdout);
    EXPECT_FALSE(strings::contains(stdout.get(), specialString));
  }

  if (GetParam() == "logrotate" ||
      GetParam() == "journald+logrotate") {
    // Check that the sandbox was written to as well.
    ASSERT_TRUE(os::exists(stdoutPath));

    Result<std::string> stdout = os::read(stdoutPath);
    ASSERT_SOME(stdout);
    EXPECT_TRUE(strings::contains(stdout.get(), specialString));
  }
}


class JournaldLoggerDockerTest : public JournaldLoggerTest {};


// Parameterized based on the suffix of the TaskID.
// This test is needed due to the workaround introduced in:
// https://issues.apache.org/jira/browse/MESOS-1833
INSTANTIATE_TEST_CASE_P(
    TaskIDSuffix,
    JournaldLoggerDockerTest,
    ::testing::Values(
        std::string(""),
        std::string(":something")));


// Loads the journald ContainerLogger module and runs a docker task.
// Then queries journald for the associated logs.
TEST_P(JournaldLoggerDockerTest, ROOT_DOCKER_LogToJournald)
{
  // Create a master, agent, and framework.
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  // We'll need access to these flags later.
  mesos::internal::slave::Flags flags = CreateSlaveFlags();

  // Use the journald container logger.
  flags.container_logger = JOURNALD_LOGGER_NAME;

  Fetcher fetcher(flags);

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

  // Add a parameterized suffix to the TaskID.
  task.mutable_task_id()->set_value(task.task_id().value() + GetParam());

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
  Future<std::string> frameworkQuery = runCommand(
      "journalctl",
      {"journalctl",
       "FRAMEWORK_ID=" + frameworkId.get().value()});

  Future<std::string> agentQuery = runCommand(
      "journalctl",
      {"journalctl",
       "AGENT_ID=" + offers.get()[0].slave_id().value()});

  Future<std::string> executorQuery = runCommand(
      "journalctl",
      {"journalctl",
       "EXECUTOR_ID=" + statusRunning->executor_id().value()});

  AWAIT_READY(frameworkQuery);
  ASSERT_TRUE(strings::contains(frameworkQuery.get(), specialString));

  // When the TaskID is not suffixed with a colon, the journald query should
  // return some result. When the suffix is present, the query should return
  // an empty result.
  AWAIT_READY(agentQuery);
  ASSERT_EQ(
      GetParam().empty(),
      strings::contains(agentQuery.get(), specialString));

  AWAIT_READY(executorQuery);
  ASSERT_TRUE(strings::contains(executorQuery.get(), specialString));
}

} // namespace tests {
} // namespace journald {
} // namespace mesos {

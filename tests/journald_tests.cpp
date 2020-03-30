#include <map>
#include <string>
#include <vector>

#include <gmock/gmock.h>

#include <mesos/hook.hpp>
#include <mesos/mesos.hpp>
#include <mesos/resources.hpp>

#include <mesos/module/module.hpp>

#include <mesos/scheduler/scheduler.hpp>

#include <process/clock.hpp>
#include <process/future.hpp>
#include <process/gmock.hpp>
#include <process/gtest.hpp>
#include <process/network.hpp>
#include <process/owned.hpp>
#include <process/process.hpp>
#include <process/socket.hpp>
#include <process/time.hpp>

#include <stout/duration.hpp>
#include <stout/gtest.hpp>
#include <stout/json.hpp>
#include <stout/option.hpp>
#include <stout/os.hpp>
#include <stout/path.hpp>
#include <stout/protobuf.hpp>
#include <stout/strings.hpp>
#include <stout/try.hpp>

#include <stout/os/pstree.hpp>
#include <stout/os/read.hpp>

#ifdef __linux__
#include "common/shell.hpp"
#endif // __linux__

#include "module/manager.hpp"

#include "slave/flags.hpp"

#include "slave/containerizer/docker.hpp"

#include "tests/mesos.hpp"

using namespace process;

using namespace mesos::internal::tests;

using std::vector;

using mesos::internal::master::Master;

using mesos::internal::slave::Containerizer;
using mesos::internal::slave::DockerContainerizer;
using mesos::internal::slave::Fetcher;
using mesos::internal::slave::MesosContainerizer;
using mesos::internal::slave::Slave;

using mesos::master::detector::MasterDetector;

using mesos::modules::ModuleManager;

#ifdef __linux__
using mesos::modules::common::runCommand;
#endif // __linux__

using testing::WithParamInterface;

#ifdef __linux__
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
        "processes life-time extension are enabled unless there is an \n"
        "explicit flag to disable these (see other flags).",
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
#endif // __linux__


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
#ifdef __linux__
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
#endif // __linux__
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

protected:
  Modules modules;
};


#ifdef __linux__
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

  Future<TaskStatus> statusStarting;
  Future<TaskStatus> statusRunning;
  Future<TaskStatus> statusFinished;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusStarting))
    .WillOnce(FutureArg<1>(&statusRunning))
    .WillOnce(FutureArg<1>(&statusFinished))
    .WillRepeatedly(Return());       // Ignore subsequent updates.

  driver.launchTasks(offers.get()[0].id(), {task});

  AWAIT_READY(statusStarting);
  EXPECT_EQ(TASK_STARTING, statusStarting.get().state());

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
#endif // __linux__


// Loads the journald ContainerLogger module and checks for the
// non-existence of the logrotate config file.
TEST_F(JournaldLoggerTest, ROOT_LogrotateCustomOptions)
{
  const std::string testFile = path::join(sandbox.get(), "CustomRotateOptions");

  // Custom config consists of a postrotate script which creates
  // an empty file in the temporary directory on log rotation.
  const std::string customConfig =
    "postrotate\n touch " + testFile + "\nendscript";

  // There is no way to change a task's custom logrotate options except when
  // loading the module.  So this test will unload the module, change the
  // logrotate options, and then reload the module.
  ASSERT_SOME(ModuleManager::unload(JOURNALD_LOGGER_NAME));

  foreach (Modules::Library& library, *modules.mutable_libraries()) {
    foreach (Modules::Library::Module& module, *library.mutable_modules()) {
      if (module.has_name() && module.name() == JOURNALD_LOGGER_NAME) {
        Parameter* parameter = module.add_parameters();
        parameter->set_key("logrotate_stdout_options");
        parameter->set_value(customConfig);
      }
    }
  }

  // Initialize the modules.
  Try<Nothing> result = ModuleManager::load(modules);
  ASSERT_SOME(result);

  // Create a master, agent, and framework.
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  // TODO(akornatskyy): unknown file: error: C++ exception with description
  // "Access violation - no RTTI data!" thrown in the test body.
#ifdef __linux__
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

  // Start a task that spams stdout with 2 MB of (mostly blank) output.
  // The container logger module is loaded with parameters that limit
  // the log size to files of 10 MB each.  After the task completes,
  // `logrotate` should trigger rotation of stdout logs, so the postrotate
  // script is executed.
  TaskInfo task = createTask(
      offers.get()[0],
#ifndef __WINDOWS__
      "i=0; while [ $i -lt 10240 ]; "
      "do printf '%-1024d\\n' $i; i=$((i+1)); done");
#else
      // There isn't a good alternative to printf on Windows,
      // but this line will print about the same quantity of bits.
      "FOR /L %a IN (1,1,327680) DO echo 12345678901234567890123456789012");
#endif // __WINDOWS__

  // Make sure the destination of the logs is logrotate.
  Environment::Variable* variable =
    task.mutable_command()->mutable_environment()->add_variables();
  variable->set_name("CONTAINER_LOGGER_DESTINATION_TYPE");
  variable->set_value("logrotate");

  // Add an override for the logger's stdout stream.
  // This way of overriding custom options should be disabled,
  // so we will check that these options are ignored.
  // See MESOS-9564 for more context.
  const std::string ignoredtestFile =
    path::join(sandbox.get(), "ShouldNotBeCreated");

  const std::string ignoredConfig =
    "postrotate\n touch " + ignoredtestFile + "\nendscript";

  variable = task.mutable_command()->mutable_environment()->add_variables();
  variable->set_name("CONTAINER_LOGGER_LOGROTATE_STDOUT_OPTIONS");
  variable->set_value(ignoredConfig);

  Future<TaskStatus> statusStarting;
  Future<TaskStatus> statusRunning;
  Future<TaskStatus> statusFinished;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusStarting))
    .WillOnce(FutureArg<1>(&statusRunning))
    .WillOnce(FutureArg<1>(&statusFinished))
    .WillRepeatedly(Return());       // Ignore subsequent updates.

  driver.launchTasks(offers.get()[0].id(), {task});

  AWAIT_READY(statusStarting);
  EXPECT_EQ(TASK_STARTING, statusStarting.get().state());

  AWAIT_READY(statusRunning);
  EXPECT_EQ(TASK_RUNNING, statusRunning.get().state());

#ifndef __WINDOWS__
  AWAIT_READY(statusFinished);
#else
  // Looping and echo-ing is excruciatingly slow on Windows.
  AWAIT_READY_FOR(statusFinished, Seconds(120));
#endif // __WINDOWS__

  EXPECT_EQ(TASK_FINISHED, statusFinished.get().state());

  driver.stop();
  driver.join();

  // The ContainerLogger spawns some helper processes that continue running
  // briefly after the container finishes. Once they finish reading/writing
  // the container's pipe, they should exit.
  const Duration maxReapWaitTime = Seconds(30);
  Try<os::ProcessTree> pstrees = os::pstree(0);
  ASSERT_SOME(pstrees);
  foreach (const os::ProcessTree& pstree, pstrees->children) {
    // Wait for the logger subprocesses to exit, for up to 30 seconds each.
    Duration waited = Duration::zero();
    do {
      if (!os::exists(pstree.process.pid)) {
        break;
      }

      // Push the clock ahead to speed up the reaping of subprocesses.
      Clock::pause();
      Clock::settle();
      Clock::advance(Seconds(1));
      Clock::resume();

      os::sleep(Milliseconds(100));
      waited += Milliseconds(100);
    } while (waited < maxReapWaitTime);

    EXPECT_LE(waited, maxReapWaitTime);
  }

  // Check that the sandbox was written to.
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
  const std::string stdoutPath = path::join(sandboxDirectory, "stdout");
  ASSERT_TRUE(os::exists(stdoutPath));

  // An associated logrotate config file should not exist.
#ifndef __WINDOWS__
  const std::string configPath =
    path::join(sandboxDirectory, "stdout.logrotate.conf");
  ASSERT_FALSE(os::exists(configPath));
#endif // __WINDOWS__

  // Since some logs should have been rotated, the postrotate script should
  // have created this file.
  ASSERT_TRUE(os::exists(testFile));
  ASSERT_FALSE(os::exists(ignoredtestFile));
#endif
}


#ifdef __linux__
// This test verfies that the executor information will be passed to
// the container logger the same way before and after an agent
// restart. Note that this is different than the behavior before Mesos
// 1.5, at which time we don't checkout ContainerConfig.
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
  state.id.set_value(id::UUID::random().toString());

  AWAIT_READY(containerizer->recover(state));

  ContainerID containerId;
  containerId.set_value(id::UUID::random().toString());

  const std::string specialParentString = "special-parent-string";

  // We want to print a special string to stdout and then sleep
  // so that there is enough time to launch a nested container.
  ExecutorInfo executorInfo = createExecutorInfo(
      "executor",
      "echo '" + specialParentString + "' && sleep 1000",
      "cpus:1");

  executorInfo.mutable_framework_id()->set_value(id::UUID::random().toString());

  // Make a valid ExecutorRunPath, much like the
  // `slave::paths::getExecutorRunPath` helper (that we don't have access to).
  const std::string executorRunPath = path::join(
      flags.work_dir,
      "slaves", stringify(state.id),
      "frameworks", stringify(executorInfo.framework_id()),
      "executors", stringify(executorInfo.executor_id()),
      "runs", stringify(containerId));

  ASSERT_SOME(os::mkdir(executorRunPath));

  // Launch the top-level/parent container.
  // We need to checkpoint it so that it will survive recovery
  // (hence the `forked.pid` argument).
  Future<Containerizer::LaunchResult> launch = containerizer->launch(
      containerId,
      createContainerConfig(None(), executorInfo, executorRunPath),
      std::map<std::string, std::string>(),
      path::join(flags.work_dir,
          "meta",
          "slaves", stringify(state.id),
          "frameworks", stringify(executorInfo.framework_id()),
          "executors", stringify(executorInfo.executor_id()),
          "runs", stringify(containerId),
          "pids", "forked.pid"));

  AWAIT_ASSERT_EQ(Containerizer::LaunchResult::SUCCESS, launch);

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
  executorState.id = executorInfo.executor_id();
  executorState.info = executorInfo;
  executorState.latest = containerId;

  mesos::internal::slave::state::RunState runState;
  runState.id = containerId;
  runState.forkedPid = pid;
  executorState.runs.put(containerId, runState);

  mesos::internal::slave::state::FrameworkState frameworkState;
  frameworkState.id = executorInfo.framework_id();
  frameworkState.executors.put(executorInfo.executor_id(), executorState);

  mesos::internal::slave::state::SlaveState slaveState;
  slaveState.id = state.id;
  slaveState.frameworks.put(executorInfo.framework_id(), frameworkState);

  // Recover by using the mock `SlaveState`.
  AWAIT_READY(containerizer->recover(slaveState));

  status = containerizer->status(containerId);
  AWAIT_READY(status);
  ASSERT_TRUE(status->has_executor_pid());
  EXPECT_EQ(pid, status->executor_pid());

  // Now launch the nested container.
  ContainerID nestedContainerId;
  nestedContainerId.mutable_parent()->CopyFrom(containerId);
  nestedContainerId.set_value(id::UUID::random().toString());

  const std::string specialChildString = "special-child-string";

  // This one can be short lived.
  // We just need it to print a child-specific string and then exit.
  launch = containerizer->launch(
      nestedContainerId,
      createContainerConfig(createCommandInfo(
          "echo '" + specialChildString + "'")),
      std::map<std::string, std::string>(),
      None());

  AWAIT_ASSERT_EQ(Containerizer::LaunchResult::SUCCESS, launch);

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
       "FRAMEWORK_ID=" + stringify(executorInfo.framework_id())});

  AWAIT_READY(firstQuery);
  EXPECT_TRUE(strings::contains(firstQuery.get(), specialParentString));
  EXPECT_TRUE(strings::contains(firstQuery.get(), specialChildString));

  AWAIT_READY(secondQuery);
  EXPECT_TRUE(strings::contains(secondQuery.get(), specialParentString));
  EXPECT_TRUE(strings::contains(secondQuery.get(), specialChildString));
}


// This verifies that the logger makes a special case for DEBUG containers
// and simply prints the output of DEBUG containers to the sandbox.
// This is the same behavior as the SandboxContainerLogger.
TEST_F(JournaldLoggerTest, ROOT_CGROUPS_DebugContainersLogToSandbox)
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
  state.id.set_value(id::UUID::random().toString());

  AWAIT_READY(containerizer->recover(state));

  ContainerID containerId;
  containerId.set_value(id::UUID::random().toString());

  const std::string specialParentString = "special-parent-string";

  // We want to print a special string to stdout and then sleep
  // so that there is enough time to launch a nested container.
  ExecutorInfo executorInfo = createExecutorInfo(
      "executor",
      "echo '" + specialParentString + "' && sleep 1000",
      "cpus:1");

  executorInfo.mutable_framework_id()->set_value(id::UUID::random().toString());

  // Make a valid ExecutorRunPath, much like the
  // `slave::paths::getExecutorRunPath` helper (that we don't have access to).
  const std::string executorRunPath = path::join(
      flags.work_dir,
      "slaves", stringify(state.id),
      "frameworks", stringify(executorInfo.framework_id()),
      "executors", stringify(executorInfo.executor_id()),
      "runs", stringify(containerId));

  ASSERT_SOME(os::mkdir(executorRunPath));

  // Launch the top-level container.
  Future<Containerizer::LaunchResult> launch = containerizer->launch(
      containerId,
      createContainerConfig(None(), executorInfo, executorRunPath),
      std::map<std::string, std::string>(),
      None());

  AWAIT_ASSERT_EQ(Containerizer::LaunchResult::SUCCESS, launch);

  Future<ContainerStatus> status = containerizer->status(containerId);
  AWAIT_READY(status);
  ASSERT_TRUE(status->has_executor_pid());

  // Now launch the nested container.
  ContainerID nestedContainerId;
  nestedContainerId.mutable_parent()->CopyFrom(containerId);
  nestedContainerId.set_value(id::UUID::random().toString());

  const std::string specialChildString = "special-child-string";

  // Mark this as a DEBUG container, much like health checks and
  // other nested container sessions.
  launch = containerizer->launch(
      nestedContainerId,
      createContainerConfig(
          createCommandInfo("echo '" + specialChildString + "'"),
          None(),
          mesos::slave::ContainerClass::DEBUG),
      std::map<std::string, std::string>(),
      None());

  AWAIT_ASSERT_EQ(Containerizer::LaunchResult::SUCCESS, launch);

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

  // Filter the output of journald via AGENT_ID. This should contain the
  // output of the parent container, but not the child (debug) container.
  Future<std::string> firstQuery = runCommand(
      "journalctl",
      {"journalctl",
      "AGENT_ID=" + stringify(state.id)});

  AWAIT_READY(firstQuery);
  EXPECT_TRUE(strings::contains(firstQuery.get(), specialParentString));
  EXPECT_FALSE(strings::contains(firstQuery.get(), specialChildString));

  // Check the debug container's sandbox for the expected output.
  const std::string debugContainerStdout =
    path::join(
        executorRunPath,
        "containers",
        nestedContainerId.value(),
        "stdout");

  ASSERT_TRUE(os::exists(debugContainerStdout));

  Result<std::string> stdout = os::read(debugContainerStdout);
  ASSERT_SOME(stdout);
  EXPECT_TRUE(strings::contains(stdout.get(), specialChildString));
}
#endif // __linux__


#ifdef __linux__
INSTANTIATE_TEST_CASE_P(
    LoggingMode,
    JournaldLoggerTest,
    ::testing::Values(
        std::string("journald"),
        std::string("journald+logrotate"),
        std::string("logrotate")));
#else
INSTANTIATE_TEST_CASE_P(
    LoggingMode,
    JournaldLoggerTest,
    ::testing::Values(std::string("logrotate")));
#endif // __linux__


#ifdef __linux__
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

  variable = task.mutable_command()->mutable_environment()->add_variables();
  variable->set_name("CONTAINER_LOGGER_EXTRA_LABELS");
  variable->set_value(
      "{"
      "  \"EXTRA_LABEL\":\"extra_label\","
      "  \"EXTRA_LABEL_INVALID\":10"
      "}");

  Future<TaskStatus> statusStarting;
  Future<TaskStatus> statusRunning;
  Future<TaskStatus> statusFinished;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusStarting))
    .WillOnce(FutureArg<1>(&statusRunning))
    .WillOnce(FutureArg<1>(&statusFinished))
    .WillRepeatedly(Return());       // Ignore subsequent updates.

  driver.launchTasks(offers.get()[0].id(), {task});

  AWAIT_READY(statusStarting);
  EXPECT_EQ(TASK_STARTING, statusStarting.get().state());

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

    Future<std::string> extraLabelQuery = runCommand(
        "journalctl",
        {"journalctl",
         "FRAMEWORK_ID=" + frameworkId.get().value(),
         "EXTRA_LABEL=extra_label"});

    Future<std::string> extraLabelInvalidQuery = runCommand(
        "journalctl",
        {"journalctl",
         "FRAMEWORK_ID=" + frameworkId.get().value(),
         "EXTRA_LABEL_INVALID=10"});

    AWAIT_READY(frameworkQuery);
    ASSERT_TRUE(strings::contains(frameworkQuery.get(), specialString));

    AWAIT_READY(agentQuery);
    ASSERT_TRUE(strings::contains(agentQuery.get(), specialString));

    AWAIT_READY(executorQuery);
    ASSERT_TRUE(strings::contains(executorQuery.get(), specialString));

    AWAIT_READY(extraLabelQuery);
    ASSERT_TRUE(strings::contains(extraLabelQuery.get(), specialString));

    AWAIT_READY(extraLabelInvalidQuery);
    ASSERT_FALSE(strings::contains(
        extraLabelInvalidQuery.get(),
        specialString));
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

  const std::string stdoutPath = path::join(sandboxDirectory, "stdout");

  if (GetParam() == "journald") {
    // Check that the sandbox was *not* written to. This is best-effort with
    // the current container logger architecture: a write may occur *after* a
    // terminal status update is sent.
    //
    // TODO(josephw): This file exists as other parts of the agent will create
    // it. We should invert this assertion when we fix this in Mesos.
    ASSERT_TRUE(os::exists(stdoutPath));

    Result<std::string> stdout = os::read(stdoutPath);
    ASSERT_SOME(stdout);
    EXPECT_FALSE(strings::contains(stdout.get(), specialString))
      << "Not expected " << specialString << " to appear in " << stdout.get();
  }

  if (GetParam() == "logrotate" ||
      GetParam() == "journald+logrotate") {
    // Check that the sandbox was written to as well.
    ASSERT_TRUE(os::exists(stdoutPath));

    // We expect stdout to be updated by the container logger no later than 30s
    // after a terminal status update has been sent.
    bool containsSpecialString = false;
    Result<std::string> stdout = None();
    const Duration timeout = Seconds(30);
    const Time start = Clock::now();

    do {
      stdout = os::read(stdoutPath);
      ASSERT_SOME(stdout);

      if (strings::contains(stdout.get(), specialString)) {
        containsSpecialString = true;
        break;
      }

      os::sleep(Milliseconds(100));
    } while (Clock::now() - start < timeout);

    EXPECT_TRUE(containsSpecialString)
      << "Expected " << specialString << " to appear in " << stdout.get();
  }
}
#endif // __linux__


class FluentbitLoggerTest : public JournaldLoggerTest
{
public:
  virtual void SetUp() override
  {
    // NOTE: We explicitly do not call the JournaldLoggerTest::SetUp function
    // as we need to modify some module parameters before loading them.
    MesosTest::SetUp();

    // Create the mock Fluent Bit server.
    Try<network::inet::Socket> server = network::inet::Socket::create();
    ASSERT_SOME(server);

    fluentbit_server = server.get();

    Try<network::inet::Address> bind =
      fluentbit_server->bind(
          network::inet::Address(net::IP(process::address().ip), 0));
    ASSERT_SOME(bind);

    // This setup assumes one task is run, leading to two connections.
    Try<Nothing> listen = fluentbit_server->listen(2);
    ASSERT_SOME(listen);

    // Read in the example `modules.json`.
    Try<std::string> read =
      os::read(path::join(MODULES_BUILD_DIR, "journald", "modules.json"));
    ASSERT_SOME(read);

    Try<JSON::Object> json = JSON::parse<JSON::Object>(read.get());
    ASSERT_SOME(json);

    Try<Modules> _modules = protobuf::parse<Modules>(json.get());
    ASSERT_SOME(_modules);

    modules = _modules.get();

    // Replace the dummy values for `fluentbit_ip` and `fluentbit_port`
    // with the server bind address above.
    foreach (Modules::Library& library, *modules.mutable_libraries()) {
      foreach (Modules::Library::Module& module, *library.mutable_modules()) {
        if (module.has_name() && module.name() == JOURNALD_LOGGER_NAME) {
          foreach (Parameter& parameter, *module.mutable_parameters()) {
            if (parameter.key() == "fluentbit_ip") {
              parameter.set_value(stringify(bind.get().ip));
            } else if (parameter.key() == "fluentbit_port") {
              parameter.set_value(stringify(bind.get().port));
            }
          }
        }
      }
    }

    // Initialize the modules.
    Try<Nothing> result = ModuleManager::load(modules);
    ASSERT_SOME(result);
  }

protected:
  Option<network::inet::Socket> fluentbit_server;
};


INSTANTIATE_TEST_CASE_P(
    LoggingMode,
    FluentbitLoggerTest,
    ::testing::Values(
        std::string("fluentbit"),
        std::string("fluentbit+logrotate")));


// Sets up a TCP to mock the Fluent Bit component and then loads the
// ContainerLogger module and runs a task.
// We expect a known string logged by the task to reach the mock Fluent Bit.
TEST_P(FluentbitLoggerTest, ROOT_LogToFluentbit)
{
  // Create a master, agent, and framework.
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  // TODO(akornatskyy): unknown file: error: C++ exception with description
  // "Access violation - no RTTI data!" thrown in the test body.
#ifdef __linux__
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

  EXPECT_CALL(sched, registered(&driver, _, _));

  // Wait for an offer, and start a task.
  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
  EXPECT_NE(0u, offers.get().size());

  const std::string specialString = "some-super-unique-string";

  TaskInfo task = createTask(offers.get()[0], "echo " + specialString);

  // Change the destination of the logs based on the parameterized test.
  Environment::Variable* variable =
    task.mutable_command()->mutable_environment()->add_variables();
  variable->set_name("CONTAINER_LOGGER_DESTINATION_TYPE");
  variable->set_value(GetParam());

  Future<TaskStatus> statusStarting;
  Future<TaskStatus> statusRunning;
  Future<TaskStatus> statusFinished;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusStarting))
    .WillOnce(FutureArg<1>(&statusRunning))
    .WillOnce(FutureArg<1>(&statusFinished))
    .WillRepeatedly(Return());       // Ignore subsequent updates.

  driver.launchTasks(offers.get()[0].id(), {task});

  AWAIT_READY(statusStarting);
  EXPECT_EQ(TASK_STARTING, statusStarting.get().state());

  // We expect two connections to be made with our `fluentbit_server`:
  // one for stdout and one for stderr.
  Future<network::inet::Socket> _client1 = fluentbit_server->accept();
  AWAIT_ASSERT_READY(_client1);

  Future<network::inet::Socket> _client2 = fluentbit_server->accept();
  AWAIT_ASSERT_READY(_client2);

  AWAIT_READY(statusRunning);
  EXPECT_EQ(TASK_RUNNING, statusRunning.get().state());

  AWAIT_READY(statusFinished);
  EXPECT_EQ(TASK_FINISHED, statusFinished.get().state());

  driver.stop();
  driver.join();

  // Remove const-ness.
  network::inet::Socket client1 = _client1.get();
  network::inet::Socket client2 = _client2.get();

  // Read everything.  This also acts to wait until the loggers exit,
  // and thereby close the connections.
  Future<std::string> data1 = client1.recv(-1);
  Future<std::string> data2 = client2.recv(-1);

  AWAIT_ASSERT_READY(data1);
  AWAIT_ASSERT_READY(data2);

  // From the server side, we don't actually know which connection is
  // stdout or stderr.  We expect the special string to be in stdout.
  std::string combinedOutput = data1.get() + data2.get();
  ASSERT_TRUE(strings::contains(combinedOutput, specialString));

  // Log lines should also contain some metadata about the source of logs.
  // We check for this summarily, since one of these outputs could be empty.
  // And we don't want to implement a JSON object stream parser.
  EXPECT_TRUE(strings::contains(combinedOutput, "FRAMEWORK_ID"));
  EXPECT_TRUE(strings::contains(combinedOutput, "AGENT_ID"));
  EXPECT_TRUE(strings::contains(combinedOutput, "EXECUTOR_ID"));
  EXPECT_TRUE(strings::contains(combinedOutput, "CONTAINER_ID"));
  EXPECT_TRUE(strings::contains(combinedOutput, "STREAM"));
#endif
}


#ifdef __linux__
class JournaldLoggerDockerTest : public JournaldLoggerTest {};


INSTANTIATE_TEST_CASE_P(
    TaskIDSuffix,
    JournaldLoggerDockerTest,
    ::testing::Values(
        std::string(""),
        std::string(":something")));


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

  Future<TaskStatus> statusStarting;
  Future<TaskStatus> statusRunning;
  Future<TaskStatus> statusFinished;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusStarting))
    .WillOnce(FutureArg<1>(&statusRunning))
    .WillOnce(FutureArg<1>(&statusFinished))
    .WillRepeatedly(Return());       // Ignore subsequent updates.

  driver.launchTasks(offers.get()[0].id(), {task});

  AWAIT_READY(statusStarting);
  EXPECT_EQ(TASK_STARTING, statusStarting.get().state());

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

  AWAIT_READY(agentQuery);
  EXPECT_TRUE(strings::contains(agentQuery.get(), specialString));

  AWAIT_READY(executorQuery);
  ASSERT_TRUE(strings::contains(executorQuery.get(), specialString));
}
#endif // __linux__

} // namespace tests {
} // namespace journald {
} // namespace mesos {

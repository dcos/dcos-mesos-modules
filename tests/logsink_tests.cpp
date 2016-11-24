#include <list>
#include <string>

#include <gmock/gmock.h>

#include <mesos/mesos.hpp>

#include <mesos/module/anonymous.hpp>
#include <mesos/module/module.hpp>

#include <process/clock.hpp>
#include <process/dispatch.hpp>
#include <process/future.hpp>
#include <process/gmock.hpp>
#include <process/gtest.hpp>
#include <process/owned.hpp>
#include <process/process.hpp>

#include <stout/duration.hpp>
#include <stout/gtest.hpp>
#include <stout/os.hpp>
#include <stout/path.hpp>
#include <stout/strings.hpp>

#include <stout/os/exists.hpp>
#include <stout/os/read.hpp>

#include "hook/manager.hpp"

#include "module/manager.hpp"

#include "tests/mesos.hpp"


using namespace mesos::internal::tests;

using process::Owned;

using mesos::Parameter;
using mesos::Parameters;

using mesos::modules::Anonymous;
using mesos::modules::ModuleManager;

namespace mesos {
namespace logsink {
namespace tests {


class LogSinkTest : public MesosTest
{
protected:
  virtual void SetUp()
  {
    MesosTest::SetUp();

    logFile = path::join(sandbox.get(), "test.log");

    // Read in the example `modules.json`.
    Try<std::string> read =
      os::read(path::join(MODULES_BUILD_DIR, "logsink", "modules.json"));
    ASSERT_SOME(read);

    Try<JSON::Object> json = JSON::parse<JSON::Object>(read.get());
    ASSERT_SOME(json);

    Try<Modules> _modules = protobuf::parse<Modules>(json.get());
    ASSERT_SOME(_modules);

    modules = _modules.get();

    // Add the test's sandbox to the module's parameters.
    Parameter* parameter =
      modules.mutable_libraries(0)->mutable_modules(0)->add_parameters();
    parameter->set_key("output_file");
    parameter->set_value(logFile);

    // Initialize the modules.
    Try<Nothing> result = ModuleManager::load(modules);
    ASSERT_SOME(result);

    // Create anonymous modules.
    // This is approximately how the Mesos master
    // and agent load anonymous modules.
    foreach (const string& name, ModuleManager::find<Anonymous>()) {
      LOG(INFO) << "Loading module: " << name;

      Try<Anonymous*> create = ModuleManager::create<Anonymous>(name);
      if (create.isError()) {
        EXIT(EXIT_FAILURE)
          << "Failed to create anonymous module named '" << name << "'";
      }

      anonymouses.push_back(Owned<Anonymous>(create.get()));
    }
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

    anonymouses.clear();

    MesosTest::TearDown();
  }

protected:
  std::string logFile;

private:
  Modules modules;
  std::list<Owned<Anonymous>> anonymouses;
};


// Writes a line to glog and checks the `FileSink`s output file
// for the same string.
TEST_F(LogSinkTest, LogSomething)
{
  // Wait for the log file to be created.
  // This happens asynchronously as the anonymous module is created
  // in a nonblocking fashion.
  Duration waited = Duration::zero();
  do {
    if (os::exists(logFile)) {
      break;
    }

    os::sleep(Milliseconds(100));
    waited += Milliseconds(100);
  } while (waited < Seconds(2));

  ASSERT_TRUE(os::exists(logFile));

  // Now wait for the log sink to write a line that
  // effectively tells us that the sink has been hooked up.
  const std::string logReadyString = "Added FileSink for glog logs";
  Try<std::string> logContents = os::read(logFile);
  ASSERT_SOME(logContents);

  waited = Duration::zero();
  while (waited < Seconds(2)) {
    if (strings::contains(logContents.get(), logReadyString)) {
      break;
    }

    os::sleep(Milliseconds(100));
    waited += Milliseconds(100);

    logContents = os::read(logFile);
    ASSERT_SOME(logContents);
  }

  ASSERT_TRUE(strings::contains(logContents.get(), logReadyString));

  const std::string logString = "This should appear in the log file";
  LOG(INFO) << logString;

  // Check that the log contains our special line.
  logContents = os::read(logFile);
  ASSERT_SOME(logContents);

  ASSERT_TRUE(strings::contains(logContents.get(), logString));
}


class SpamProcess : public process::Process<SpamProcess>
{
public:
  SpamProcess() {}

  process::Future<Nothing> spam()
  {
    LOG(INFO) << "Spam!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!";

    process::dispatch(self(), &SpamProcess::spam);

    return Nothing();
  }
};


// Spawns a bunch of actors that spam the log for a while.
// This test is used to detect deadlock in the LogSink
// and to check for interleaving in the logs.
TEST_F(LogSinkTest, LogStress)
{
  std::list<process::UPID> spammers;
  for (int i = 0; i < 100; i++) {
    process::PID<SpamProcess> spammer =
      process::spawn(new SpamProcess(), true);

    spammers.push_back(spammer);

    process::dispatch(spammer, &SpamProcess::spam);
  }

  Try<std::string> logContents = os::read(logFile);
  ASSERT_SOME(logContents);

  // Let the spammers run for a while.
  os::sleep(Seconds(2));

  foreach (const process::UPID& spammer, spammers) {
    process::terminate(spammer);
  }

  foreach (const process::UPID& spammer, spammers) {
    process::wait(spammer);
  }
}

} // namespace tests {
} // namespace logsink {
} // namespace mesos {

#include <string>

#include <gmock/gmock.h>

#include <mesos/mesos.hpp>

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

#include "module/manager.hpp"

#include "tests/mesos.hpp"

using namespace mesos::internal::tests;

using std::string;

using mesos::modules::ModuleManager;

namespace mesos {
namespace metrics {
namespace tests {

class MetricsTest : public MesosTest
{
protected:
  virtual void SetUp()
  {
    MesosTest::SetUp();

    // Read in the `modules.json` for metrics.
    Try<std::string> read =
      os::read(path::join(MODULES_BUILD_DIR, "metrics", "modules.json"));
    ASSERT_SOME(read);

    Try<JSON::Object> json = JSON::parse<JSON::Object>(read.get());
    ASSERT_SOME(json);

    Try<Modules> _modules = protobuf::parse<Modules>(json.get());
    ASSERT_SOME(_modules);

    modules = _modules.get();

    // Initialize the metrics module.
    Try<Nothing> result = ModuleManager::load(modules);
    ASSERT_SOME(result);
  }

  virtual void TearDown()
  {
    // Unload the metrics module.
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


TEST_F(MetricsTest, EmptyTest)
{
}

} // namespace tests {
} // namespace metrics {
} // namespace mesos {

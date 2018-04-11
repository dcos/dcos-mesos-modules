#include <string>
#include <vector>

#include <gmock/gmock.h>

#include <stout/strings.hpp>

#include "tests/mesos.hpp"

#include "common/shell.hpp"

using namespace std;
using namespace strings;
using namespace process;
using namespace mesos::internal::tests;
using namespace mesos::modules::common;

namespace mesos {
namespace common {
namespace tests {

class CommonTest : public MesosTest {
protected:
  virtual void SetUp()
  {
    MesosTest::SetUp();
  }

  virtual void TearDown()
  {
    MesosTest::TearDown();
  }
};

TEST_F(CommonTest, CheckRunScriptCommandBasic)
{
  Future<string> r = runScriptCommand("echo -n foobar");
  AWAIT_READY(r);
  EXPECT_EQ("foobar", r.get());
}

TEST_F(CommonTest, CheckRunScriptCommandTimeout)
{
  Future<string> r0 = runScriptCommand("sleep 1");
  AWAIT_READY(r0);

  Future<string> r1 = runScriptCommand("sleep 1", Milliseconds(100));
  r1.await();
  CHECK_FAILED(r1);
  EXPECT_EQ(true, endsWith(r1.failure(), "timeout after 100ms"));
}

} // namespace tests {
} // namespace common {
} // namespace mesos {

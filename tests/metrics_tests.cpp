#include <string>
#include <vector>

#include <gmock/gmock.h>

#include <mesos/mesos.hpp>
#include <mesos/module.hpp>

#include <mesos/module/isolator.hpp>
#include <mesos/module/module.hpp>

#include <mesos/slave/containerizer.hpp>
#include <mesos/slave/isolator.hpp>

#include <process/clock.hpp>
#include <process/dispatch.hpp>
#include <process/future.hpp>
#include <process/gmock.hpp>
#include <process/gtest.hpp>
#include <process/http.hpp>
#include <process/owned.hpp>
#include <process/process.hpp>

#include <stout/duration.hpp>
#include <stout/gtest.hpp>
#include <stout/hashmap.hpp>
#include <stout/os.hpp>
#include <stout/path.hpp>
#include <stout/protobuf.hpp>
#include <stout/strings.hpp>

#include <stout/os/exists.hpp>
#include <stout/os/read.hpp>

#include "common/http.hpp"

#include "metrics/messages.pb.h"
#include "metrics/isolator.hpp"

#include "module/manager.hpp"

#include "tests/mesos.hpp"

using namespace mesos::internal::tests;

using mesos::modules::ModuleManager;

using mesos::modules::metrics::ContainerStartRequest;
using mesos::modules::metrics::ContainerStartResponse;

using mesos::slave::Isolator;

using mesosphere::dcos::metrics::MetricsIsolator;
using mesosphere::dcos::metrics::parse;

using process::Clock;
using process::Future;
using process::Owned;

using std::string;
using std::vector;

using testing::DoAll;
using testing::Return;

namespace mesos {
namespace metrics {
namespace tests {

const string METRICS_PROCESS = "metrics-service";
const string API_PATH = "container";
const Duration REQUEST_TIMEOUT = Seconds(5);


class MetricsTest : public MesosTest
{
protected:
  virtual void SetUp()
  {
    MesosTest::SetUp();

    // Construct the metrics module configuration.
    const string ip = stringify(process::address().ip);
    const string port = stringify(process::address().port);
    const Try<string> config = strings::format(R"~(
        {
          "libraries": [ {
            "file": %s,
            "modules": [ {
              "name": "com_mesosphere_dcos_MetricsIsolatorModule",
              "parameters": [
                {"key": "dcos_metrics_service_scheme", "value": "http"},
                {"key": "dcos_metrics_service_network", "value": "inet"},
                {"key": "dcos_metrics_service_address", "value": "%s"},
                {"key": "dcos_metrics_service_endpoint", "value": "%s"},
                {"key": "request_timeout", "value": "%s"}
              ]
            } ]
          } ]
        }
        )~",

        // TODO(tillt): We need to rework this for Windows as soon as
        // there are standalone modules on that platform.
#ifdef __linux__
        JSON::String(path::join(MODULES_BUILD_DIR, ".libs", "libmetrics-module.so")),
#else
        JSON::String(path::join(MODULES_BUILD_DIR, CMAKE_INTDIR, "metrics.dll")),
#endif // __linux__

        ip + ":" + port,
        "/" + METRICS_PROCESS + "/" + API_PATH,
        stringify(REQUEST_TIMEOUT));
    
    ASSERT_SOME(config);

    Try<JSON::Object> json = JSON::parse<JSON::Object>(config.get());
    ASSERT_SOME(json);

    Try<Modules> _modules = protobuf::parse<Modules>(json.get());
    ASSERT_SOME(_modules);

    modules = _modules.get();

    // Initialize the metrics module.
    Try<Nothing> result = ModuleManager::load(modules);
    ASSERT_SOME(result);

    // Create an instance of the module and store it for testing.
    Try<Isolator*> isolator_ =
      ModuleManager::create<Isolator>(
          "com_mesosphere_dcos_MetricsIsolatorModule");

    ASSERT_SOME(isolator_);
    isolator = isolator_.get();
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

    delete isolator;

    MesosTest::TearDown();
  }

  Isolator* isolator;

private:
  Modules modules;
};


// Simulates the DC/OS metrics service.
class MockMetricsServiceProcess
  : public process::Process<MockMetricsServiceProcess>
{
public:
  MockMetricsServiceProcess() : ProcessBase(METRICS_PROCESS) {}

  MOCK_METHOD1(container, Future<process::http::Response>(
      const process::http::Request&));

protected:
  void initialize() override
  {
    route("/" + API_PATH, None(), &MockMetricsServiceProcess::container);
  }
};


class MockMetricsService
{
public:
  MockMetricsService() : mock(new MockMetricsServiceProcess())
  {
    spawn(mock.get());
  }

  ~MockMetricsService()
  {
    terminate(mock.get());
    wait(mock.get());
  }

  Owned<MockMetricsServiceProcess> mock;
};


// When the isolator's `prepare()` method is called with a container ID, it
// should make an API call to the metrics service to retrieve the metrics port
// for that container, and then inject the correct host and port into the
// environment contained in the returned `ContainerLaunchInfo`.
TEST_F(MetricsTest, PrepareSuccess)
{
  MockMetricsService metricsService;

  ContainerStartResponse responseBody;
  responseBody.set_statsd_port(1111);
  responseBody.set_statsd_host("127.0.0.1");

  process::http::Response response(
      string(jsonify(JSON::Protobuf(responseBody))),
      process::http::Status::CREATED,
      "application/json");

  Future<process::http::Request> request;
  EXPECT_CALL(*metricsService.mock, container(_))
    .WillOnce(DoAll(FutureArg<0>(&request),
                    Return(response)));

  const string CONTAINER_ID = "new-container";

  ContainerID containerId;
  containerId.set_value(CONTAINER_ID);

  Future<Option<mesos::slave::ContainerLaunchInfo>> prepared =
    isolator->prepare(containerId, mesos::slave::ContainerConfig());

  AWAIT_READY(request);

  // Verify the contents of the module's request.
  Try<ContainerStartRequest> requestBody =
    parse<ContainerStartRequest>(request->body);

  ASSERT_SOME(requestBody);
  ASSERT_EQ(request->method, "POST");
  ASSERT_EQ(requestBody->container_id(), CONTAINER_ID);

  AWAIT_READY(prepared);

  // Check that the `ContainerLaunchInfo` contains environment variables.
  ASSERT_SOME(prepared.get());
  ASSERT_TRUE(prepared.get()->has_environment());
  ASSERT_GT(prepared.get()->environment().variables().size(), 0);

  // Verify the contents of the returned environment.
  hashmap<string, string> expectedEnvironment =
    {{"STATSD_UDP_HOST", "127.0.0.1"},
     {"STATSD_UDP_PORT", "1111"}};

  foreach (const Environment::Variable& variable,
           prepared.get()->environment().variables()) {
    ASSERT_EQ(variable.type(), Environment::Variable::VALUE);
    ASSERT_TRUE(variable.has_value());
    ASSERT_EQ(variable.value(), expectedEnvironment[variable.name()]);
  }
}


// When the isolator's `prepare()` method is called and the metrics service
// returns a response which does not contain a port, the isolator should return
// a failed future.
TEST_F(MetricsTest, PrepareFailureNoPort)
{
  MockMetricsService metricsService;

  ContainerStartResponse responseBody;
  responseBody.set_statsd_host("127.0.0.1");

  process::http::Response response(
      string(jsonify(JSON::Protobuf(responseBody))),
      process::http::Status::CREATED,
      "application/json");

  EXPECT_CALL(*metricsService.mock, container(_))
    .WillOnce(Return(response));

  const string CONTAINER_ID = "new-container";

  ContainerID containerId;
  containerId.set_value(CONTAINER_ID);

  Future<Option<mesos::slave::ContainerLaunchInfo>> prepared =
    isolator->prepare(containerId, mesos::slave::ContainerConfig());

  AWAIT_FAILED(prepared);
}


// When the isolator's `prepare()` method is called and the metrics service
// returns a response code other than 201 CREATED, the isolator should return
// a failed future.
TEST_F(MetricsTest, PrepareFailureUnexpectedStatusCode)
{
  MockMetricsService metricsService;

  ContainerStartResponse responseBody;
  responseBody.set_statsd_port(1111);
  responseBody.set_statsd_host("127.0.0.1");

  process::http::Response response(
      string(jsonify(JSON::Protobuf(responseBody))),
      process::http::Status::OK,
      "application/json");

  EXPECT_CALL(*metricsService.mock, container(_))
    .WillOnce(Return(response));

  const string CONTAINER_ID = "new-container";

  ContainerID containerId;
  containerId.set_value(CONTAINER_ID);

  Future<Option<mesos::slave::ContainerLaunchInfo>> prepared =
    isolator->prepare(containerId, mesos::slave::ContainerConfig());

  AWAIT_FAILED(prepared);
}


// When the isolator's `prepare()` method is called and the metrics service
// doesn't return a response within the configured request timeout, the isolator
// should return a failed future.
TEST_F(MetricsTest, PrepareFailureRequestTimeout)
{
  Clock::pause();

  MockMetricsService metricsService;

  ContainerStartResponse responseBody;
  responseBody.set_statsd_port(1111);
  responseBody.set_statsd_host("127.0.0.1");

  Future<process::http::Response> response;

  Future<Nothing> requestSent;
  EXPECT_CALL(*metricsService.mock, container(_))
    .WillOnce(DoAll(FutureSatisfy(&requestSent),
                    Return(response)));

  const string CONTAINER_ID = "new-container";

  ContainerID containerId;
  containerId.set_value(CONTAINER_ID);

  Future<Option<mesos::slave::ContainerLaunchInfo>> prepared =
    isolator->prepare(containerId, mesos::slave::ContainerConfig());

  AWAIT_READY(requestSent);

  ASSERT_TRUE(prepared.isPending());

  Clock::advance(REQUEST_TIMEOUT);
  Clock::settle();

#ifdef __linux__
  ASSERT_TRUE(prepared.isFailed());
#endif

  Clock::resume();
}


// When the isolator's `cleanup()` method is called and the module receives a
// 202 ACCEPTED response from the metrics service, the isolator should return a
// ready future.
TEST_F(MetricsTest, CleanupSuccess)
{
  MockMetricsService metricsService;

  process::http::Response response(process::http::Status::ACCEPTED);

  Future<process::http::Request> request;
  EXPECT_CALL(*metricsService.mock, container(_))
    .WillOnce(DoAll(FutureArg<0>(&request),
                    Return(response)));

  const string CONTAINER_ID = "new-container";

  ContainerID containerId;
  containerId.set_value(CONTAINER_ID);

  Future<Nothing> cleanedup = isolator->cleanup(containerId);

  AWAIT_READY(request);

  ASSERT_EQ(request->method, "DELETE");

  // Verify that the DELETE request is made for a path which reflects the
  // target container's ID.
  const vector<string> pathTokens = strings::tokenize(request->url.path, "/");

  ASSERT_EQ(pathTokens.size(), 3);
  ASSERT_EQ(pathTokens[2], CONTAINER_ID);

  AWAIT_READY(cleanedup);
}


// When the isolator's `cleanup()` method is called and the module receives an
// unexpected response code from the metrics service, the isolator should return
// a ready future. In this case, we log an error but do not fail the cleanup so
// that other isolators will have a chance to cleanup as well.
TEST_F(MetricsTest, CleanupSuccessUnexpectedStatusCode)
{
  MockMetricsService metricsService;

  process::http::Response response(process::http::Status::OK);

  EXPECT_CALL(*metricsService.mock, container(_))
    .WillOnce(Return(response));

  const string CONTAINER_ID = "new-container";

  ContainerID containerId;
  containerId.set_value(CONTAINER_ID);

  Future<Nothing> cleanedup = isolator->cleanup(containerId);

  AWAIT_READY(cleanedup);
}


// When the isolator's `cleanup()` method is called and the module's HTTP
// request to the metrics service times out, the isolator should return a ready
// future. In this case, we log an error but do not fail the cleanup so that
// other isolators will have a chance to cleanup as well.
TEST_F(MetricsTest, CleanupSuccessRequestTimeout)
{
  Clock::pause();

  MockMetricsService metricsService;

  process::Promise<process::http::Response> promise;

  Future<Nothing> deleteRequest;
  EXPECT_CALL(*metricsService.mock, container(_))
    .WillOnce(DoAll(FutureSatisfy(&deleteRequest),
              Return(promise.future())));

  const string CONTAINER_ID = "new-container";

  ContainerID containerId;
  containerId.set_value(CONTAINER_ID);

  Future<Nothing> cleanedup = isolator->cleanup(containerId);

  AWAIT_READY(deleteRequest);

  Clock::advance(REQUEST_TIMEOUT);

  AWAIT_READY(cleanedup);

  Clock::resume();
}


TEST_F(MetricsTest, PrepareAndCleanupSuccessDebugContainer)
{
  Clock::pause();

  MockMetricsService metricsService;

  // Since we're passing a DEBUG container to the isolator, we do not expect any
  // requests to be made to the metrics service.
  EXPECT_CALL(*metricsService.mock, container(_))
    .Times(0);

  const string CONTAINER_ID = "new-container";

  ContainerID containerId;
  containerId.set_value(CONTAINER_ID);

  mesos::slave::ContainerConfig containerConfig;
  containerConfig.set_container_class(mesos::slave::ContainerClass::DEBUG);

  Future<Option<mesos::slave::ContainerLaunchInfo>> prepared =
    isolator->prepare(containerId, containerConfig);

  AWAIT_READY(prepared);

  Future<Nothing> cleanedup = isolator->cleanup(containerId);

  AWAIT_READY(cleanedup);

  // Settle the clock to ensure that the metrics service
  // does not receive any requests.
  Clock::settle();

  Clock::resume();
}

} // namespace tests {
} // namespace metrics {
} // namespace mesos {

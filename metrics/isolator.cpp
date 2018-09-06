#include <map>
#include <string>
#include <vector>

#include <mesos/http.hpp>
#include <mesos/mesos.hpp>
#include <mesos/module.hpp>

#include <mesos/module/isolator.hpp>
#include <mesos/module/module.hpp>

#include <process/address.hpp>
#include <process/collect.hpp>
#include <process/defer.hpp>
#include <process/future.hpp>
#include <process/process.hpp>

#include <stout/assert.hpp>
#include <stout/hashset.hpp>
#include <stout/ip.hpp>
#include <stout/nothing.hpp>
#include <stout/numify.hpp>
#include <stout/option.hpp>
#include <stout/os.hpp>
#include <stout/path.hpp>
#include <stout/protobuf.hpp>
#include <stout/strings.hpp>
#include <stout/try.hpp>

#include "isolator.hpp"

#include "metrics/messages.pb.h"

namespace inet = process::network::inet;
namespace unix = process::network::unix;

using namespace mesos;
using namespace process;

using std::map;
using std::string;
using std::vector;

using mesos::slave::ContainerClass;
using mesos::slave::ContainerLaunchInfo;
using mesos::slave::ContainerState;
using mesos::slave::Isolator;

using mesos::modules::metrics::ContainerStartRequest;
using mesos::modules::metrics::ContainerStartResponse;

namespace mesosphere {
namespace dcos {
namespace metrics {

class MetricsIsolatorProcess
  : public process::Process<MetricsIsolatorProcess>
{
public:
  MetricsIsolatorProcess(const isolator::Flags& _flags) : flags(_flags)
  {
    // Set `serviceScheme` based on flags.
    ASSERT(flags.service_scheme.isSome());
    ASSERT(flags.service_scheme.get() == "http" ||
           flags.service_scheme.get() == "https");
    serviceScheme = flags.service_scheme.get();

    // Set `service*Address` based on flags.
    ASSERT(flags.service_address.isSome());

    ASSERT(flags.service_network.isSome());
    ASSERT(flags.service_network.get() == "inet" ||
           flags.service_network.get() == "unix");

    if (flags.service_network.get() == "inet") {
      vector<string> hostport =
        strings::split(flags.service_address.get(), ":");
      if (hostport.size() != 2) {
        LOG(FATAL) << "Unable to split '" << flags.service_address.get() << "'"
                   << " into valid 'host:port' combination";
      }

      Try<net::IP> ip = net::IP::parse(hostport[0]);
      if (ip.isError()) {
        LOG(FATAL) << "Unable to parse '" << hostport[0] << "'"
                   << " as a valid IP address: " << ip.error();
      }

      Try<uint16_t> port = numify<uint16_t>(hostport[1]);
      if (port.isError()) {
        LOG(FATAL) << "Unable parse '" + hostport[1] + "'"
                   << " as a valid port of type 'uint16_t': " + port.error();
      }

      serviceInetAddress = inet::Address(ip.get(), port.get());
    }

    if (flags.service_network.get() == "unix") {
      Try<unix::Address> address =
        unix::Address::create(flags.service_address.get());
      if (address.isError()) {
        LOG(FATAL) << "Unable to convert '" + flags.service_address.get() + "'"
                   << " to valid 'unix' address: " + address.error();
      }

      serviceUnixAddress = address.get();
    }

    // Set `serviceEndpoint` based on flags.
    ASSERT(flags.service_endpoint.isSome());
    serviceEndpoint = flags.service_endpoint.get();
  }

  // Let the metrics service know about the container being launched.
  // In the response, grab the STATSD_UDP_HOST and STATSD_UDP_PORT
  // pair being returned and set it in the environment of the
  // `ContainerLaunchInfo` returned from this function.
  virtual Future<Option<ContainerLaunchInfo>> prepare(
      const ContainerID& containerId,
      const slave::ContainerConfig& containerConfig)
  {
    // For debug containers, we do not make API requests or inject new
    // environment variables. They will inherit the environment of their
    // parent container.
    if (containerConfig.container_class() == ContainerClass::DEBUG) {
      // Add this container ID to the set of debug containers so that we will
      // not make a DELETE request for it during the cleanup phase.
      debugContainers.emplace(containerId);

      return None();
    }

    ContainerStartRequest containerStartRequest;
    containerStartRequest.set_container_id(containerId.value());

    return send(containerStartRequest)
      .onAny(defer(
        self(),
        [=](const Future<http::Response>& response) -> Future<http::Response> {
          if (!response.isReady()) {
            return Failure("Error posting 'ContainerStartRequest' for"
                           " container '" + containerId.value() + "': " +
                           (response.isFailed() ?
                               response.failure() : "Future discarded"));
          }

          return response;
        }))
      .then(defer(
        self(),
        [=](const http::Response& response)
          -> Future<Option<ContainerLaunchInfo>> {
          if (response.code == http::Status::NO_CONTENT) {
            return None();
          }

          if (response.code != http::Status::CREATED) {
            return Failure("Received unexpected response code "
                           " '" + stringify(response.code) + "' when"
                           " posting 'ContainerStartRequest' for container"
                           " '" + containerId.value() + "'");
          }

          Try<ContainerStartResponse> containerStartResponse =
            parse<ContainerStartResponse>(response.body);
          if (containerStartResponse.isError()) {
            return Failure("Error parsing the 'ContainerStartResponse' body for"
                           " container '" + containerId.value() + "': " +
                           containerStartResponse.error());
          }

          // TODO(klueska): For now, we require both `statsd_host` and
          // `statsd_port` to be set. This may not be true in the future.
          // We need to revisit this if/when this changes.
          if (!containerStartResponse->has_statsd_host() ||
              !containerStartResponse->has_statsd_port()) {
            return Failure("Missing 'statsd_host' or 'statsd_port' field in"
                           " 'ContainerStartResponse' for container"
                           " '" + containerId.value() + "'");
          }

          Environment environment;
          Environment::Variable* variable;
          variable = environment.add_variables();
          variable->set_name("STATSD_UDP_HOST");
          variable->set_value(containerStartResponse->statsd_host());
          variable = environment.add_variables();
          variable->set_name("STATSD_UDP_PORT");
          variable->set_value(stringify(containerStartResponse->statsd_port()));

          ContainerLaunchInfo launchInfo;
          launchInfo.mutable_environment()->CopyFrom(environment);

          return launchInfo;
        }));
  }

  virtual Future<Nothing> cleanup(const ContainerID& containerId)
  {
    if (debugContainers.contains(containerId)) {
      debugContainers.erase(containerId);
      return Nothing();
    }

    // If a failed future is returned at any point during the sending of the
    // DELETE request, we do not want to surface that failure to the
    // containerizer, since it would prevent other isolators from cleaning up
    // subsequently. We use `process::await()` to attain a future that becomes
    // ready no matter what state the future associated with the request
    // transitions to.
    return process::await(sendContainerStop(containerId))
      .then(defer(
          self(),
          [=](const Future<Future<http::Response>>& response) {
            if (!response->isReady()) {
              LOG(ERROR)
                << "Failed sending container DELETE request for"
                << " container '" << containerId.value() << "': "
                << (response->isFailed() ?
                      response->failure() : "Future discarded");
            } else if (response.get()->code != http::Status::ACCEPTED) {
              LOG(ERROR)
                << "Received unexpected response code"
                << " '" << stringify(response.get()->code) << "' when"
                << " sending DELETE request for container"
                << " '" << containerId.value() << "'";
            }

            return Nothing();
          }));
  }

  Future<http::Connection> connect()
  {
    if (serviceInetAddress.isSome()) {
      if (flags.service_scheme.get() == "http") {
        return http::connect(serviceInetAddress.get(), http::Scheme::HTTP);
      }
      return http::connect(serviceInetAddress.get(), http::Scheme::HTTPS);
    }

    if (serviceUnixAddress.isSome()) {
      if (flags.service_scheme.get() == "http") {
        return http::connect(serviceUnixAddress.get(), http::Scheme::HTTP);
      }
      return http::connect(serviceUnixAddress.get(), http::Scheme::HTTPS);
    }

    UNREACHABLE();
  }

  Future<http::Response> send(
      const string& endpoint,
      const Option<string>& body,
      const string& method)
  {
    return connect()
      .then(defer(
          self(),
          [=](http::Connection connection) -> Future<http::Response> {
            // Capture a reference to the connection to ensure that it remains
            // open long enough to receive the response.
            connection.disconnected()
              .onAny([connection]() {});

            http::Request request;
            request.method = method;
            request.keepAlive = true;
            request.headers = {
              {"Accept", APPLICATION_JSON},
              {"Content-Type", APPLICATION_JSON}};

            if (body.isSome()) {
              request.body = body.get();
            }

            if (serviceInetAddress.isSome()) {
              request.url = http::URL(
                  serviceScheme,
                  serviceInetAddress->ip,
                  serviceInetAddress->port,
                  endpoint);
            }

            if (serviceUnixAddress.isSome()) {
              request.url.scheme = serviceScheme;
              request.url.domain = "";
              request.url.path = endpoint;
            }

            return connection.send(request);
          }))
      .after(
          flags.request_timeout,
          [](const Future<http::Response>&) {
            return Failure("Request timed out");
          });
  }

  Future<http::Response> send(const ContainerStartRequest& containerStartRequest)
  {
    string body = jsonify(JSON::Protobuf(containerStartRequest));

    return send(serviceEndpoint, body, "POST");
  }

  Future<http::Response> sendContainerStop(const ContainerID& containerId)
  {
    return send(serviceEndpoint + "/" + containerId.value(), None(), "DELETE");
  }

private:
  const isolator::Flags flags;
  string serviceScheme;
  string serviceEndpoint;
  Option<inet::Address> serviceInetAddress;
  Option<unix::Address> serviceUnixAddress;

  // A set of all DEBUG containers the module has observed during `prepare()`.
  // This is used to skip the DELETE request during the cleanup phase, since we
  // do not set up metrics for DEBUG containers.
  hashset<ContainerID> debugContainers;
};


MetricsIsolator::MetricsIsolator(const isolator::Flags& flags)
  : process(new MetricsIsolatorProcess(flags))
{
  spawn(process.get());
}


MetricsIsolator::~MetricsIsolator()
{
  terminate(process.get());
  wait(process.get());
}


Future<Option<ContainerLaunchInfo>> MetricsIsolator::prepare(
    const ContainerID& containerId,
    const slave::ContainerConfig& containerConfig)
{
  return dispatch(process.get(),
                  &MetricsIsolatorProcess::prepare,
                  containerId,
                  containerConfig);
}


Future<Nothing> MetricsIsolator::cleanup(
    const ContainerID& containerId)
{
  return dispatch(process.get(),
                  &MetricsIsolatorProcess::cleanup,
                  containerId);
}

} // namespace metrics {
} // namespace dcos {
} // namespace mesosphere {

mesos::modules::Module<Isolator>
com_mesosphere_dcos_MetricsIsolatorModule(
    MESOS_MODULE_API_VERSION,
    MESOS_VERSION,
    "Mesosphere",
    "support@mesosphere.com",
    "Metrics Isolator Module.",
    nullptr,
    [](const Parameters& parameters) -> Isolator* {
      // Convert `parameters` into a map.
      map<string, string> values;
      foreach (const Parameter& parameter, parameters.parameter()) {
        values[parameter.key()] = parameter.value();
      }

      mesosphere::dcos::metrics::isolator::Flags flags;

      // Load and validate flags.
      Try<flags::Warnings> load = flags.load(values, false);

      if (load.isError()) {
        LOG(ERROR) << "Failed to parse parameters: " << load.error();
        return nullptr;
      }

      foreach (const flags::Warning& warning, load->warnings) {
        LOG(WARNING) << warning.message;
      }

      return new mesosphere::dcos::metrics::MetricsIsolator(flags);
    });

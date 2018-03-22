#ifndef __AGENT_OVERLAY_MANAGER_HPP__
#define __AGENT_OVERLAY_MANAGER_HPP__

#include <process/future.hpp>
#include <process/http.hpp>
#include <process/process.hpp>
#include <process/protobuf.hpp>

#include <mesos/master/detector.hpp>
#include <mesos/mesos.hpp>
#include <mesos/module/anonymous.hpp>

#include <overlay/messages.hpp>

namespace mesos {
namespace modules {
namespace overlay {
namespace agent {

class ManagerProcess : public ProtobufProcess<ManagerProcess>
{
public:
  static Try<process::Owned<ManagerProcess>> create(
      const overlay::internal::AgentConfig& agentConfig);

  process::Future<Nothing> ready();

protected:
  void agentRegisteredAcknowledgement(const process::UPID& from);

  void detected(const process::Future<Option<MasterInfo>>& mesosMaster);

  void doReliableRegistration(Duration maxBackoff);

  virtual void exited(const process::UPID& pid);

  virtual void initialize();

  process::Future<process::http::Response> overlay(
      const process::http::Request& request);

  process::Future<Nothing> configure(const std::string& name);

  process::Future<Nothing> _configure(
      const std::string& name,
      const std::tuple<process::Future<Nothing>, process::Future<Nothing>>& t);

  process::Future<Nothing> configureMesosNetwork(const std::string& name);

  process::Future<Nothing> configureDockerNetwork(const std::string& name);

  process::Future<Nothing> _configureDockerNetwork(
      const std::string& name,
      bool exists);

  process::Future<Nothing> __configureDockerNetwork(
      const std::string& name,
      const process::Future<std::string> &result);

  process::Future<bool> checkDockerNetwork(const std::string& name);

  void updateAgentOverlays(
      const process::UPID& from,
      const overlay::internal::UpdateAgentOverlaysMessage& message);

  void _updateAgentOverlays(
      const process::Future<std::list<process::Future<Nothing>>>& results);

private:
  enum State
  {
    REGISTERING = 0,
    REGISTERED = 1,
    CONFIGURING = 2
  };

  ManagerProcess(
      const std::string& _cniDir,
      const overlay::internal::AgentNetworkConfig _networkConfig,
      const uint32_t _maxConfigAttempts,
      process::Owned<master::detector::MasterDetector> _detector);

  const std::string cniDir;

  const overlay::internal::AgentNetworkConfig networkConfig;

  State state;

  process::Promise<Nothing> connected;

  Option<process::UPID> overlayMaster;

  hashmap<std::string, overlay::AgentOverlayInfo> overlays;

  const uint32_t maxConfigAttempts;

  uint32_t configAttempts;

  process::Owned<master::detector::MasterDetector> detector;
};


class Manager : public Anonymous
{
public:
  static Try<Manager*> create(
      const overlay::internal::AgentConfig& agentConfig);

  virtual ~Manager();

private:
  Manager(const process::Owned<ManagerProcess>& _process);

  process::Owned<ManagerProcess> process;
};

} // namespace agent {
} // namespace overlay {
} // namespace modules {
} // namespace mesos {

#endif // __AGENT_OVERLAY_MANAGER_HPP__

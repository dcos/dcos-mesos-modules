#ifndef __DOCKERCFG_REMOVER_HPP__
#define __DOCKERCFG_REMOVER_HPP__

#include <string>

#include <mesos/hook.hpp>
#include <mesos/mesos.hpp>

#include <process/process.hpp>

#include <stout/nothing.hpp>
#include <stout/option.hpp>
#include <stout/os.hpp>
#include <stout/path.hpp>
#include <stout/try.hpp>

namespace mesos {

class DockerCfgRemoveHook : public mesos::Hook
{
  public:
  virtual Try<Nothing> slavePostFetchHook(
      const ContainerID& containerId,
      const std::string& sandboxDirectory)
  {
    std::string dockerCfgLocation = path::join(sandboxDirectory, ".dockercfg");
    if (os::exists(dockerCfgLocation)) {
      LOG(INFO) << "Removing .dockercfg file from sandbox.'";
      return os::rm(dockerCfgLocation);
    }
    return Nothing();
  }
};

} // namespace mesos {

#endif // __DOCKERCFG_REMOVER_HPP__

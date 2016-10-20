#include <mesos/hook.hpp>
#include <mesos/mesos.hpp>
#include <mesos/module.hpp>

#include <mesos/module/hook.hpp>

#include "remover.hpp"

using namespace mesos;


mesos::modules::Module<Hook>
com_mesosphere_dcos_RemoverHook(
    MESOS_MODULE_API_VERSION,
    MESOS_VERSION,
    "Mesosphere",
    "help@mesosphere.io",
    "Dockercfg Remover Hook module.",
    NULL,
    [](const Parameters& parameters) -> Hook* {
      return new mesos::DockerCfgRemoveHook();
    });

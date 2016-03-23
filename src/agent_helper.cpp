/**
 * This file is © 2014 Mesosphere, Inc. (“Mesosphere”). Mesosphere
 * licenses this file to you solely pursuant to the following terms
 * (and you may not use this file except in compliance with such
 * terms):
 *
 * 1) Subject to your compliance with the following terms, Mesosphere
 * hereby grants you a nonexclusive, limited, personal,
 * non-sublicensable, non-transferable, royalty-free license to use
 * this file solely for your internal business purposes.
 *
 * 2) You may not (and agree not to, and not to authorize or enable
 * others to), directly or indirectly:
 *   (a) copy, distribute, rent, lease, timeshare, operate a service
 *   bureau, or otherwise use for the benefit of a third party, this
 *   file; or
 *
 *   (b) remove any proprietary notices from this file.  Except as
 *   expressly set forth herein, as between you and Mesosphere,
 *   Mesosphere retains all right, title and interest in and to this
 *   file.
 *
 * 3) Unless required by applicable law or otherwise agreed to in
 * writing, Mesosphere provides this file on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied,
 * including, without limitation, any warranties or conditions of
 * TITLE, NON-INFRINGEMENT, MERCHANTABILITY, or FITNESS FOR A
 * PARTICULAR PURPOSE.
 *
 * 4) In no event and under no legal theory, whether in tort (including
 * negligence), contract, or otherwise, unless required by applicable
 * law (such as deliberate and grossly negligent acts) or agreed to in
 * writing, shall Mesosphere be liable to you for damages, including
 * any direct, indirect, special, incidental, or consequential damages
 * of any character arising as a result of these terms or out of the
 * use or inability to use this file (including but not limited to
 * damages for loss of goodwill, work stoppage, computer failure or
 * malfunction, or any and all other commercial damages or losses),
 * even if Mesosphere has been advised of the possibility of such
 * damages.
 */

#include <mesos/mesos.hpp>
#include <mesos/module.hpp>

#include <mesos/module/anonymous.hpp>

#include <stout/check.hpp>
#include <stout/os.hpp>
#include <stout/try.hpp>

#include <process/collect.hpp>
#include <process/defer.hpp>
#include <process/future.hpp>
#include <process/help.hpp>
#include <process/http.hpp>
#include <process/io.hpp>
#include <process/process.hpp>
#include <process/subprocess.hpp>

using namespace mesos;
using namespace mesos::modules;
using namespace process;

using std::string;

const string OVERLAY_HELP = HELP(
  TLDR(
    "Show agent network overlay information."),
  USAGE(
    "/network/overlay"),
  DESCRIPTION(
    "Shows the Agent IP, Agent subnet, VTEP IP, VTEP MAC and bridges.",
    ""));

class AgentOverlayHelperProcess : public Process<AgentOverlayHelperProcess>
{
public:
  AgentOverlayHelperProcess() : ProcessBase("network") {}

  virtual ~AgentOverlayHelperProcess() {}

private:
  void initialize()
  {
    LOG(INFO) << "Adding route for '" << self().id << "/overlay'";

    route("/overlay",
          OVERLAY_HELP,
          &AgentOverlayHelperProcess::overlay);
  }

  void _overlay(
      const Owned<Promise<http::Response>>& promise,
      const http::Request& request,
      const string cmd,
      const Future<std::tuple<Future<Option<int>>, Future<string>>>& future)
  {
    CHECK_READY(future);

    Future<Option<int>> status = std::get<0>(future.get());

    if (!status.isReady()) {
      const string& msg = "Failed to execute '" + cmd + "': " +
                          (status.isFailed() ? status.failure() : "discarded");
      LOG(ERROR) << msg;
      promise->set(http::InternalServerError(msg));
      return;
    }

    if (status.get().isNone()) {
      const string& msg = "Failed to reap the status of the '" + cmd + "'";
      LOG(ERROR) << msg;
      promise->set(http::InternalServerError(msg));
    }

    Future<string> output = std::get<1>(future.get());

    if (!output.isReady()) {
      const string& msg = "Failed to read stdout from '" + cmd + "': " +
                          (output.isFailed() ? output.failure() : "discarded");
      LOG(ERROR) << msg;
      promise->set(http::InternalServerError(msg));
      return;
    }

    VLOG(1) << "Returning command output";
    promise->set(http::OK(output.get()));
  }

  Future<http::Response> overlay(const http::Request& request)
  {
    // Get the query string.
    // const Result<string>& value(request.url.query.get("<query_str>"));

    // Run command to get info.
    const string& cmd("ip addr");

    VLOG(1) << "Running '" << cmd << "'";

    Try<Subprocess> s = subprocess(
        cmd,
        Subprocess::PIPE(),
        Subprocess::PIPE(),
        Subprocess::PIPE());

    if (s.isError()) {
      const string& msg = "Failed to run '" + cmd + "'";
      LOG(ERROR) << msg;
      return http::InternalServerError(msg);
    }

    Owned<Promise<http::Response>> promise(new Promise<http::Response>);

    Future<string> read = io::read(s.get().out().get());

    // Wait until subprocess produces result and exits.
    process::await(s.get().status(), read)
      .onAny(defer(self(), &Self::_overlay, promise, request, cmd, lambda::_1));

    return promise->future();
  }
};


class AgentOverlayHelper : public Anonymous
{
public:
  AgentOverlayHelper()
  {
    VLOG(1) << "Spawning process";

    process = new AgentOverlayHelperProcess();
    spawn(process);
  }

  virtual ~AgentOverlayHelper()
  {
    VLOG(1) << "Terminating process";

    terminate(process);
    wait(process);
    delete process;
  }

private:
  AgentOverlayHelperProcess* process;
};


// Module "main".
Anonymous* createAnonymous(const Parameters& parameters)
{
  return new AgentOverlayHelper();
}


// Declares a helper module named 'AgentOverlayHelper'.
Module<Anonymous> com_mesosphere_mesos_AgentOverlayHelper(
  MESOS_MODULE_API_VERSION,
  MESOS_VERSION,
  "Mesosphere",
  "kapil@mesosphere.io",
  "Agent Overlay Helper Module.",
  NULL,
  createAnonymous);

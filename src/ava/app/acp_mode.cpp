#include "sys.h"
#include "ava/diagnostics/runtime_diagnostics.h"
#include "ava/app/acp/codec.h"
#include "ava/app/acp/peer.h"
#include "ava/app/acp/service.h"
#include "ava/app/acp/transport.h"
#include "ava/app/acp_mode.h"
#include "ava/app/signal_policy.h"
#include "ava/provider/catalog.h"
#include "ava/core/json.h"
#include "ava/core/version.h"

#include <cerrno>
#include <chrono>
#include <cstring>
#include <ostream>
#include <string_view>
#include <thread>
#include <utility>
#include <unistd.h>

namespace ava::app {

int run_acp_mode(std::ostream& error_output, ava::process::ProcessScopeV1 const& application_process_scope,
                 std::shared_ptr<ava::diagnostics::RuntimeDiagnostics> diagnostics)
{
  auto transport = acp::make_fd_record_transport(STDIN_FILENO, STDOUT_FILENO);
  if (!transport)
  {
    error_output << "ACP startup failed: transport unavailable\n";
    return 1;
  }

  auto launch_root = ava::core::launch_workspace_root();
  if (!launch_root)
  {
    error_output << "ACP startup failed: launch workspace is unavailable\n";
    return 1;
  }
  acp::AgentServiceOptions service_options;
  service_options.agent_version = std::string(ava::core::version::kFullVersion);
  service_options.launch_root = *launch_root;
  service_options.open_context.application_process_scope = application_process_scope;
  service_options.open_context.diagnostics = std::move(diagnostics);
  auto provider_catalog = ava::provider::ProviderCatalog::build(service_options.open_context.paths);
  if (!provider_catalog)
  {
    error_output << "ACP startup failed: " << provider_catalog.error().format() << '\n';
    return 1;
  }
  service_options.open_context.provider_catalog = std::move(*provider_catalog);
  auto pinned_options = acp::pin_agent_service_model(std::move(service_options));
  if (!pinned_options)
  {
    error_output << "ACP startup failed: " << pinned_options.error().format() << '\n';
    return 1;
  }
  acp::AgentService service(std::move(*pinned_options));
  acp::JsonRpcPeer peer(
      std::move(*transport), [&service](acp::Request const& request, std::stop_token token) { return service.handle_request(request, token); },
      [&service](acp::Notification const& notification, std::stop_token token) { service.handle_notification(notification, token); },
      [&error_output](std::string_view diagnostic) { error_output << diagnostic << '\n'; });
  peer.set_request_pre_admission_hook([&service](acp::Request const& request) { return service.pre_admit_request(request); });
  peer.set_control_notification_handler([&service](acp::Notification const& notification) { service.handle_control_notification(notification); });
  service.bind_request_terminal_committer([&peer](acp::JsonRpcId const& id) { return peer.commit_inbound_request(id); });
  service.bind_update_sender([&peer](std::string_view session_id, std::string_view update_json) -> ava::core::VoidResult {
    std::string params = "{\"sessionId\":\"" + ava::core::json::escape(session_id) + "\",\"update\":" + std::string(update_json) + "}";
    return peer.send_notification("session/update", std::move(params));
  });
  service.bind_client_request_sender(
      [&peer](std::string method, std::optional<std::string> params, std::chrono::milliseconds timeout, acp::OutboundCallPolicy policy) {
        return peer.send_request(std::move(method), std::move(params), timeout, policy);
      },
      [&peer](acp::JsonRpcId const& id, std::string reason) {
        // ACP cancellation is observable by the client before AVA retires its
        // local waiter. A late response then safely loses pending-call ownership.
        auto params = acp::cancel_request_params_json(id);
        if (params)
          static_cast<void>(peer.send_notification("$/cancel_request", std::move(*params)));
        return peer.cancel_pending_call(id, std::move(reason));
      },
      [&peer](std::string) { peer.shutdown(); });
  ModeSignalLatch signal_latch;
  std::jthread signal_watcher([&](std::stop_token stop_token) {
    while (!stop_token.stop_requested())
    {
      if (signal_latch.poll())
      {
        peer.shutdown();
        return;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
  });
  auto result = peer.run();
  signal_watcher.request_stop();
  signal_watcher.join();
  service.unbind_request_terminal_committer();
  service.unbind_client_request_sender();
  service.unbind_update_sender();
  service.shutdown();
  if (signal_latch.signaled())
    return 130;
  if (!result)
  {
    error_output << "ACP connection closed after a transport or service failure\n";
    return 1;
  }
  return 0;
}

}  // namespace ava::app

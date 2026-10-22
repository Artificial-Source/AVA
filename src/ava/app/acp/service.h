#pragma once

#include "ava/debug/print_members_on.h"
#include "ava/app/acp/session.h"

#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

namespace ava::app::acp {

using RequestTerminalCommitter = std::function<bool(JsonRpcId const& id)>;

struct AgentServiceOptions
{
  std::string agent_version;
  std::filesystem::path launch_root;
  ava::config::XdgPaths paths = ava::config::xdg_paths();
  runtime::OpenContext open_context;
  runtime::RunOptions run_options;
  RuntimeProviderRunBundleFactory provider_bundle_factory;
  std::chrono::milliseconds permission_timeout = kDefaultCallTimeout;
  std::chrono::milliseconds close_grace = kSessionCloseGrace;
  // Aggregate retained session/list cursor memory; defaults preserve the wire policy.
  std::size_t max_session_list_snapshot_bytes = kMaxSessionListSnapshotBytes;
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

class AgentService
{
 private:
  struct PinnedOptions;

 public:
  explicit AgentService(std::string agent_version);
  explicit AgentService(AgentServiceOptions options);
  AgentService(AgentService const&) = delete;
  AgentService& operator=(AgentService const&) = delete;
  ~AgentService();

  void bind_update_sender(SessionUpdateSender sender);
  void unbind_update_sender();
  void bind_client_request_sender(ClientRequestSender sender, ClientRequestCanceler canceler, ClientConnectionAborter aborter = {});
  void unbind_client_request_sender();
  void bind_request_terminal_committer(RequestTerminalCommitter committer);
  void unbind_request_terminal_committer();
  [[nodiscard]] std::expected<std::function<void()>, JsonRpcError> pre_admit_request(Request const& request);
  [[nodiscard]] RequestResult handle_request(Request const& request, std::stop_token stop_token);
  void handle_notification(Notification const& notification, std::stop_token stop_token);
  void handle_control_notification(Notification const& notification);
  [[nodiscard]] bool initialized() const noexcept;
  void shutdown() noexcept;

 private:
  explicit AgentService(PinnedOptions pinned);
  [[nodiscard]] static PinnedOptions pin_options(AgentServiceOptions options);

  struct PromptAdmission
  {
    std::shared_ptr<AcpSessionHost> host;
    std::uint64_t reservation = 0;
    AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
  };

  [[nodiscard]] std::shared_ptr<PromptAdmission> take_prompt_admission(JsonRpcId const& id);
  void rollback_prompt_admission(std::string const& key, std::shared_ptr<PromptAdmission> const& admission) noexcept;
  [[nodiscard]] std::expected<void, JsonRpcError> commit_request_terminal(JsonRpcId const& id);

  std::string agent_version_;
  bool image_prompt_capability_ = false;
  std::optional<ava::core::Error> startup_error_ = std::nullopt;
  std::shared_ptr<SessionUpdateGateway> updates_;
  std::shared_ptr<ClientRequestGateway> client_requests_;
  AcpSessionOptions session_options_;
  std::unique_ptr<AcpSessionRegistry> registry_;
  mutable std::mutex mutex_;
  bool initialized_ = false;
  bool initializing_ = false;
  RequestTerminalCommitter request_terminal_committer_;
  std::mutex admissions_mutex_;
  std::unordered_map<std::string, std::shared_ptr<PromptAdmission>> prompt_admissions_;
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

[[nodiscard]] ava::core::Result<AgentServiceOptions> pin_agent_service_model(AgentServiceOptions options);

}  // namespace ava::app::acp

#pragma once

#include "ava/debug/print_members_on.h"
#include "ava/app/acp/content.h"
#include "ava/app/acp/peer.h"
#include "ava/app/acp/permission.h"
#include "ava/app/acp/session_update.h"
#include "ava/app/runtime/Session.h"
#include "ava/app/runtime_credentials.h"
#include "ava/mcp/config.h"
#include "ava/config/xdg_paths.h"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace ava::app::acp {

inline constexpr std::size_t kMaxConnectionSessions = 32;
inline constexpr auto kSessionCloseGrace = std::chrono::seconds(2);
inline constexpr std::size_t kSessionListPageSize = 50;
inline constexpr std::size_t kMaxSessionListSnapshots = 16;
inline constexpr std::size_t kMaxSessionListSnapshotBytes = 8U * 1024U * 1024U;
inline constexpr std::size_t kMaxSessionListResultBytes = 512U * 1024U;
inline constexpr std::size_t kMaxAcpSessionPermissionGrants = 128;
inline constexpr auto kAcpPermissionPollInterval = std::chrono::milliseconds(10);
inline constexpr ava::session::SessionReadLimits kAcpSessionReadLimits{
    .max_file_bytes = 8U * 1024U * 1024U, .max_line_bytes = 1024U * 1024U, .max_entries = 16384};

using SessionUpdateSender = std::function<ava::core::VoidResult(std::string_view session_id, std::string_view update_json)>;

class SessionUpdateGateway
{
 public:
  void bind(SessionUpdateSender sender);
  void unbind();
  [[nodiscard]] ava::core::VoidResult send(std::string_view session_id, std::string_view update_json) const;

 private:
  mutable std::mutex mutex_;
  SessionUpdateSender sender_;
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

struct AcpSessionOptions
{
  std::filesystem::path launch_root;
  ava::config::XdgPaths paths = ava::config::xdg_paths();
  runtime::OpenContext open_context;
  runtime::RunOptions run_options;
  RuntimeProviderRunBundleFactory provider_bundle_factory;
  // Shared because every host on one connection must retain the same immutable
  // initialize-time capability snapshot.
  std::shared_ptr<ClientCapabilities const> client_capabilities;
  std::weak_ptr<SessionUpdateGateway> updates;
  std::weak_ptr<ClientRequestGateway> client_requests;
  std::chrono::milliseconds permission_timeout = kDefaultCallTimeout;
  std::chrono::milliseconds close_grace = kSessionCloseGrace;
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

class AcpSessionHost
{
 public:
  AcpSessionHost(runtime::session_ts&& unlocked_session, AcpSessionOptions options);
  AcpSessionHost(AcpSessionHost const&) = delete;
  AcpSessionHost& operator=(AcpSessionHost const&) = delete;
  ~AcpSessionHost();

  [[nodiscard]] std::string const& session_id() const noexcept;
  [[nodiscard]] std::filesystem::path current_dir(runtime::session_ts::crat const& session_r) const;
  [[nodiscard]] bool accepts_images() const noexcept;
  [[nodiscard]] ava::core::Result<std::uint64_t> reserve_prompt();
  void rollback_prompt_reservation(std::uint64_t reservation) noexcept;
  [[nodiscard]] RequestResult prompt(AcpPromptContent content, std::stop_token stop_token, std::optional<std::uint64_t> reservation = std::nullopt,
                                     std::function<bool()> request_terminal_commit = {});
  void cancel() noexcept;
  [[nodiscard]] ava::core::VoidResult close();

  // Return a temporary Read Access Type instance for unlocked_session_.
  [[nodiscard]] runtime::session_ts::crat session_r() const { return unlocked_session_; }

 private:
  struct PermissionGrantKey
  {
    ava::permissions::Operation operation = ava::permissions::Operation::ReadFile;
    ava::agent::Mode mode = ava::agent::Mode::Build;
    std::string workspace;
    std::string target;
    std::string command;
    std::string command_recipe_key;
    std::string tool_name;

    friend bool operator==(PermissionGrantKey const&, PermissionGrantKey const&) = default;
    AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
  };

  struct PermissionGrant
  {
    PermissionGrantKey key;
    bool allow = false;
    AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
  };

  [[nodiscard]] ava::core::VoidResult send_text_update(std::string_view kind, std::string_view text, std::string_view message_id = {}) const;
  [[nodiscard]] RequestResult finish_prompt(std::uint64_t reservation, RequestResult result);
  [[nodiscard]] bool prompt_cancel_pending(std::uint64_t reservation) const noexcept;
  [[nodiscard]] ava::permissions::PermissionResolver permission_resolver(std::uint64_t reservation, std::stop_token stop_token);
  [[nodiscard]] ava::core::Result<ava::permissions::PermissionResolutionDecision> resolve_permission(ava::permissions::PermissionPrompt const& prompt,
                                                                                                     std::uint64_t reservation, std::stop_token stop_token);
  [[nodiscard]] PermissionGrantKey permission_grant_key(ava::permissions::PermissionPrompt const& prompt) const;

  runtime::session_ts unlocked_session_;
  AcpSessionOptions options_;
  std::string session_id_;
  mutable std::mutex mutex_;
  std::condition_variable idle_;
  bool active_prompt_ = false;
  std::uint64_t active_prompt_reservation_ = 0;
  std::uint64_t active_prompt_cancel_generation_ = 0;
  std::optional<ActiveRunGuard> active_run_guard_ = std::nullopt;
  std::string active_run_request_id_;
  std::uint64_t next_prompt_reservation_ = 0;
  std::uint64_t cancel_generation_ = 0;
  bool closing_ = false;
  std::vector<PermissionGrant> permission_grants_;
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

class AcpSessionRegistry
{
 public:
  explicit AcpSessionRegistry(AcpSessionOptions options);
  AcpSessionRegistry(AcpSessionRegistry const&) = delete;
  AcpSessionRegistry& operator=(AcpSessionRegistry const&) = delete;
  ~AcpSessionRegistry();

  [[nodiscard]] ava::core::Result<std::filesystem::path> resolve_cwd(std::string_view requested) const;
  [[nodiscard]] ava::core::Result<std::shared_ptr<AcpSessionHost>> create(std::filesystem::path const& cwd,
                                                                          std::shared_ptr<ava::mcp::McpConfig const> mcp_config);
  [[nodiscard]] ava::core::Result<std::shared_ptr<AcpSessionHost>> load(std::string_view session_id, std::filesystem::path const& cwd,
                                                                        std::shared_ptr<ava::mcp::McpConfig const> mcp_config);
  [[nodiscard]] std::weak_ptr<AcpSessionHost> find(std::string_view session_id) const;
  [[nodiscard]] ava::core::VoidResult close(std::string_view session_id);
  [[nodiscard]] ava::core::Result<std::string> list_json(std::optional<std::filesystem::path> const& cwd, std::optional<std::string> const& cursor,
                                                         ava::session::SessionCancelCallback cancel_requested = nullptr);
  void cancel(std::string_view session_id) noexcept;
  void shutdown() noexcept;

  [[nodiscard]] std::filesystem::path const& launch_root() const noexcept;
  [[nodiscard]] ava::config::XdgPaths const& paths() const noexcept;
  [[nodiscard]] bool default_model_accepts_images() const noexcept;

 private:
  [[nodiscard]] ava::core::VoidResult reserve_insertion(std::optional<std::string_view> session_id = std::nullopt);
  void release_insertion() noexcept;
  [[nodiscard]] ava::core::Result<std::shared_ptr<AcpSessionHost>> insert_reserved(runtime::session_ts&& unlocked_session);

  struct ListRecord
  {
    std::string session_id;
    std::string cwd;
    std::string title;
    std::string updated_at;
    AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
  };

  struct ListSnapshot
  {
    std::optional<std::string> cwd;
    std::vector<ListRecord> records;
    std::size_t offset = 0;
    std::size_t retained_bytes = 0;
    AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
  };

  AcpSessionOptions options_;
  std::optional<ava::core::Error> coordinator_startup_error_ = std::nullopt;
  mutable std::mutex mutex_;
  bool closing_ = false;
  std::size_t pending_insertions_ = 0;
  std::unordered_map<std::string, std::shared_ptr<AcpSessionHost>> hosts_;
  std::unordered_map<std::string, ListSnapshot> list_snapshots_;
  std::deque<std::string> list_snapshot_order_;
  std::size_t list_snapshot_bytes_ = 0;
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

[[nodiscard]] ava::core::Result<std::string_view> acp_stop_reason(ava::core::RuntimeTerminalOutcome outcome);

}  // namespace ava::app::acp

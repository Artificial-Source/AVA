#pragma once

#include "ava/debug/print_members_on.h"
#include "ava/core/AnchorSet.h"
#include "ava/core/NoLogString.h"
#include "ava/core/result.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>
#include <sys/types.h>

namespace ava::lsp {

using CancelCallback = std::function<bool()>;

struct Diagnostic
{
  int severity = 0;
  ava::core::NoLogString message;
  int line = 0;
  int column = 0;
  std::string code;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

struct Range
{
  int start_line = 0;
  int start_column = 0;
  int end_line = 0;
  int end_column = 0;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

struct Symbol
{
  ava::core::NoLogString name;
  int kind = 0;
  std::filesystem::path path;
  Range range;
  ava::core::NoLogString container;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

struct Location
{
  std::filesystem::path path;
  Range range;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

struct ExecutableIdentity
{
  // Logical approved discovery path. Identity is sealed by the remaining
  // descriptor metadata; no path canonicalization is performed.
  std::filesystem::path executable_path;
  std::uintmax_t owner_uid = 0;
  std::uintmax_t owner_gid = 0;
  std::uintmax_t mode = 0;
  std::uintmax_t link_count = 0;
  std::uintmax_t device = 0;
  std::uintmax_t inode = 0;
  std::uintmax_t size = 0;
  std::int64_t changed_seconds = 0;
  std::int64_t changed_nanoseconds = 0;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

struct ServerConfig
{
  std::vector<ava::core::NoLogString> argv;
  std::filesystem::path workspace_root;
  std::filesystem::path server_root = {};
  std::shared_ptr<ava::core::AnchorSet const> anchor_set = nullptr;
  std::filesystem::path process_cwd;
  std::chrono::milliseconds startup_timeout{10000};
  std::chrono::milliseconds request_timeout{3000};
  std::string language_id = "plaintext";
  std::optional<ExecutableIdentity> executable_identity = std::nullopt;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

class DiagnosticsProvider
{
 public:
  DiagnosticsProvider() = default;
  DiagnosticsProvider(DiagnosticsProvider const&) = delete;
  DiagnosticsProvider& operator=(DiagnosticsProvider const&) = delete;
  DiagnosticsProvider(DiagnosticsProvider&&) = delete;
  DiagnosticsProvider& operator=(DiagnosticsProvider&&) = delete;
  virtual ~DiagnosticsProvider() = default;

  [[nodiscard]] virtual ava::core::Result<std::vector<Diagnostic>> diagnostics(std::filesystem::path const& path,
                                                                               CancelCallback cancel_requested = nullptr) = 0;
  [[nodiscard]] virtual ava::core::Result<std::vector<Symbol>> document_symbols(std::filesystem::path const& path, CancelCallback cancel_requested = nullptr);
  [[nodiscard]] virtual ava::core::Result<std::vector<Symbol>> workspace_symbols(std::string_view query, CancelCallback cancel_requested = nullptr);
  [[nodiscard]] virtual ava::core::Result<std::vector<Location>> definitions(std::filesystem::path const& path, int line, int column,
                                                                             CancelCallback cancel_requested = nullptr);
  [[nodiscard]] virtual ava::core::Result<std::vector<Location>> references(std::filesystem::path const& path, int line, int column,
                                                                            CancelCallback cancel_requested = nullptr);
  virtual void set_permission_request_ids(std::shared_ptr<std::vector<std::string>> ids);

  AVA_DEBUG_PURE_VIRTUAL_PRINT_MEMBERS
};

// Responses use bounded Content-Length framing and strict JSON validation.
// Malformed framing stops the child; server request IDs retain their JSON type.
class SubprocessLspClient final : public DiagnosticsProvider
{
 public:
  explicit SubprocessLspClient(ServerConfig config);
  ~SubprocessLspClient() override;

  SubprocessLspClient(SubprocessLspClient const&) = delete;
  SubprocessLspClient& operator=(SubprocessLspClient const&) = delete;
  SubprocessLspClient(SubprocessLspClient&&) = delete;
  SubprocessLspClient& operator=(SubprocessLspClient&&) = delete;

  [[nodiscard]] static ava::core::Result<std::shared_ptr<SubprocessLspClient>> start(ServerConfig config, CancelCallback cancel_requested = nullptr);
  [[nodiscard]] ava::core::Result<std::vector<Diagnostic>> diagnostics(std::filesystem::path const& path, CancelCallback cancel_requested = nullptr) override;
  [[nodiscard]] ava::core::Result<std::vector<Symbol>> document_symbols(std::filesystem::path const& path, CancelCallback cancel_requested = nullptr) override;
  [[nodiscard]] ava::core::Result<std::vector<Symbol>> workspace_symbols(std::string_view query, CancelCallback cancel_requested = nullptr) override;
  [[nodiscard]] ava::core::Result<std::vector<Location>> definitions(std::filesystem::path const& path, int line, int column,
                                                                     CancelCallback cancel_requested = nullptr) override;
  [[nodiscard]] ava::core::Result<std::vector<Location>> references(std::filesystem::path const& path, int line, int column,
                                                                    CancelCallback cancel_requested = nullptr) override;
  [[nodiscard]] bool is_alive();

  AVA_DEBUG_PRINT_MEMBERS_ON

 private:
  [[nodiscard]] ava::core::VoidResult launch();
  [[nodiscard]] ava::core::VoidResult initialize(CancelCallback cancel_requested = nullptr);
  [[nodiscard]] ava::core::VoidResult send_notification(std::string_view method, std::string_view params_json, std::chrono::steady_clock::time_point deadline,
                                                        std::chrono::milliseconds timeout, std::string_view phase, CancelCallback cancel_requested = nullptr);
  [[nodiscard]] ava::core::VoidResult respond_to_server_request(std::string_view message, std::string_view id_json,
                                                                std::chrono::steady_clock::time_point deadline, std::chrono::milliseconds timeout,
                                                                std::string_view phase, CancelCallback cancel_requested = nullptr);
  [[nodiscard]] ava::core::VoidResult synchronize_document(std::filesystem::path const& path, std::chrono::steady_clock::time_point deadline,
                                                           CancelCallback cancel_requested = nullptr);
  [[nodiscard]] ava::core::VoidResult dispatch_notification(std::string_view message);
  [[nodiscard]] std::optional<std::vector<Diagnostic>> cached_diagnostics(std::filesystem::path const& path) const;
  [[nodiscard]] ava::core::Result<std::string> request_response(std::string_view method, std::string_view params_json,
                                                                std::chrono::steady_clock::time_point deadline, std::chrono::milliseconds timeout,
                                                                std::string_view phase, CancelCallback cancel_requested = nullptr);
  [[nodiscard]] ava::core::VoidResult write_message(std::string_view body, std::chrono::steady_clock::time_point deadline, std::chrono::milliseconds timeout,
                                                    std::string_view phase, std::string_view method, CancelCallback cancel_requested = nullptr);
  [[nodiscard]] ava::core::Result<std::string> read_message(std::chrono::steady_clock::time_point deadline, std::chrono::milliseconds timeout,
                                                            std::string_view phase, std::string_view method, CancelCallback cancel_requested = nullptr);
  [[nodiscard]] ava::core::VoidResult wait_for_readable(std::chrono::steady_clock::time_point deadline, std::chrono::milliseconds timeout,
                                                        std::string_view phase, std::string_view method, CancelCallback cancel_requested = nullptr);
  [[nodiscard]] ava::core::VoidResult wait_for_writable(std::chrono::steady_clock::time_point deadline, std::chrono::milliseconds timeout,
                                                        std::string_view phase, std::string_view method, CancelCallback cancel_requested = nullptr);
  [[nodiscard]] ava::core::VoidResult check_child_running();
  void close_fds() noexcept;
  void terminate_child() noexcept;

  ServerConfig config_;
  std::mutex operation_mutex_;
  pid_t pid_ = -1;
  // Set only after the parent verifies that this child owns a distinct group.
  pid_t owned_pgid_ = -1;
  int stdin_fd_ = -1;
  int stdout_fd_ = -1;
  int next_id_ = 1;
  ava::core::NoLogString read_buffer_;
  std::unordered_map<ava::core::NoLogString, ava::core::NoLogString> open_document_contents_;
  std::unordered_map<ava::core::NoLogString, int> open_document_versions_;
  std::unordered_map<ava::core::NoLogString, std::vector<Diagnostic>> diagnostics_cache_;
  std::unordered_map<ava::core::NoLogString, std::size_t> diagnostics_cache_bytes_;
  std::size_t diagnostics_cache_total_bytes_ = 0;
  bool supports_pull_diagnostics_ = false;
};

}  // namespace ava::lsp

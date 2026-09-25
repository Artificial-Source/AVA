#include "sys.h"
#include "ava/lsp/bounded_file_reader.h"
#include "ava/lsp/lsp_client.h"
#include "ava/lsp/lsp_client_internal.h"
#include "ava/core/json.h"

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ava::lsp {
namespace {

constexpr std::uintmax_t kMaxDocumentBytes = 512 * 1024;
constexpr std::size_t kMaxOpenDocuments = 64;

std::string command_label(std::vector<std::string> const& argv)
{
  std::string label;
  for (std::size_t index = 0; index < argv.size(); ++index)
  {
    if (index > 0)
      label += ' ';
    label += argv[index];
  }
  return label;
}

}  // namespace

namespace lsp_client_internal {

ava::core::Error lsp_error(ava::core::ErrorCategory category, std::string message, ServerConfig const& config, ava::core::ErrorCode code)
{
  auto error = ava::core::Error(category, std::move(message), code);
  error.with_context("command", command_label(config.argv));
  error.with_context("workspace", config.workspace_root.string());
  return error;
}

ava::core::Error errno_error(std::string message, ServerConfig const& config)
{
  auto error = lsp_error(ava::core::ErrorCategory::Io, std::move(message), config);
  error.with_context("cause", std::strerror(errno));
  return error;
}

bool is_canceled(CancelCallback const& cancel_requested)
{
  return cancel_requested && cancel_requested();
}

ava::core::Error canceled_error(std::string message, ServerConfig const& config)
{
  auto error = lsp_error(ava::core::ErrorCategory::Unknown, std::move(message), config, ava::core::ErrorCode::Canceled);
  error.with_context("canceled", "true");
  return error;
}

}  // namespace lsp_client_internal

using namespace lsp_client_internal;

namespace {

ava::core::Result<std::string> read_text_document(std::filesystem::path const& path, ServerConfig const& config, std::chrono::steady_clock::time_point deadline,
                                                  CancelCallback const& cancel_requested)
{
  auto content = read_bounded_lsp_file(BoundedFileReadOptions{
      .path = path,
      .workspace_root = config.workspace_root,
      .anchor_set = config.anchor_set,
      .max_bytes = kMaxDocumentBytes,
      .scope = BoundedFileReadScope::Workspace,
      .deadline = deadline,
      .cancel_requested = cancel_requested,
  });
  if (!content)
  {
    auto error = std::move(content.error());
    error.with_context("command", command_label(config.argv));
    error.with_context("workspace", config.workspace_root.string());
    return std::unexpected(std::move(error));
  }
  if (!*content)
  {
    auto error = lsp_error(ava::core::ErrorCategory::NotFound, "LSP document was not found", config);
    error.with_context("path", path.string());
    return std::unexpected(std::move(error));
  }
  return std::move(**content);
}

}  // namespace

ava::core::Result<std::vector<Symbol>> DiagnosticsProvider::document_symbols(std::filesystem::path const&, CancelCallback)
{
  return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Tool, "LSP document symbols provider is unavailable"));
}

ava::core::Result<std::vector<Symbol>> DiagnosticsProvider::workspace_symbols(std::string_view, CancelCallback)
{
  return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Tool, "LSP workspace symbols provider is unavailable"));
}

ava::core::Result<std::vector<Location>> DiagnosticsProvider::definitions(std::filesystem::path const&, int, int, CancelCallback)
{
  return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Tool, "LSP definitions provider is unavailable"));
}

ava::core::Result<std::vector<Location>> DiagnosticsProvider::references(std::filesystem::path const&, int, int, CancelCallback)
{
  return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Tool, "LSP references provider is unavailable"));
}

void DiagnosticsProvider::set_permission_request_ids(std::shared_ptr<std::vector<std::string>>)
{
}

SubprocessLspClient::SubprocessLspClient(ServerConfig config) : config_(std::move(config))
{
}

SubprocessLspClient::~SubprocessLspClient()
{
  terminate_child();
  close_fds();
}

ava::core::Result<std::shared_ptr<SubprocessLspClient>> SubprocessLspClient::start(ServerConfig config, CancelCallback cancel_requested)
{
  if (config.argv.empty() || config.argv.front().empty())
  {
    auto error = ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "LSP server command argv must not be empty");
    return std::unexpected(std::move(error));
  }
  auto const server_root = config.server_root.empty() ? config.workspace_root.lexically_normal() : config.server_root.lexically_normal();
  auto const process_cwd = config.process_cwd.empty() ? config.workspace_root.lexically_normal() : config.process_cwd.lexically_normal();
  if (!config.anchor_set || !config.workspace_root.is_absolute() || !server_root.is_absolute() ||
      !path_is_within(server_root, config.workspace_root.lexically_normal()) || !process_cwd.is_absolute())
  {
    auto error = ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "LSP logical roots and shared AnchorSet are invalid");
    return std::unexpected(std::move(error));
  }
  if (config.startup_timeout < std::chrono::milliseconds(100) || config.startup_timeout > std::chrono::seconds(30))
  {
    auto error = ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "LSP startup timeout is out of bounds");
    error.with_context("field", "startup_timeout");
    error.with_context("min_ms", "100");
    error.with_context("max_ms", "30000");
    return std::unexpected(std::move(error));
  }
  if (config.request_timeout < std::chrono::milliseconds(100) || config.request_timeout > std::chrono::seconds(30))
  {
    auto error = ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "LSP request timeout is out of bounds");
    error.with_context("field", "request_timeout");
    error.with_context("min_ms", "100");
    error.with_context("max_ms", "30000");
    return std::unexpected(std::move(error));
  }
  if (is_canceled(cancel_requested))
  {
    return std::unexpected(canceled_error("LSP startup canceled", config));
  }

  auto client = std::make_shared<SubprocessLspClient>(std::move(config));
  if (auto launched = client->launch(); !launched)
    return std::unexpected(std::move(launched.error()));
  if (is_canceled(cancel_requested))
  {
    client->terminate_child();
    return std::unexpected(canceled_error("LSP startup canceled", client->config_));
  }
  if (auto initialized = client->initialize(cancel_requested); !initialized)
  {
    return std::unexpected(std::move(initialized.error()));
  }
  return client;
}

ava::core::VoidResult SubprocessLspClient::initialize(CancelCallback cancel_requested)
{
  auto const deadline = std::chrono::steady_clock::now() + config_.startup_timeout;
  auto const root = config_.server_root.empty() ? config_.workspace_root : config_.server_root;
  auto const root_uri = file_uri(root, config_.workspace_root);
  std::string const params = "{\"processId\":null,\"rootUri\":" + json_string(root_uri) + ",\"capabilities\":{\"textDocument\":{\"diagnostic\":{}}}}";
  auto response = request_response("initialize", params, deadline, config_.startup_timeout, "startup", cancel_requested);
  if (!response)
    return std::unexpected(std::move(response.error()));
  auto capability = parse_pull_diagnostics_capability(*response, config_);
  if (!capability)
    return std::unexpected(std::move(capability.error()));
  supports_pull_diagnostics_ = *capability;
  return send_notification("initialized", "{}", deadline, config_.startup_timeout, "startup", cancel_requested);
}

ava::core::Result<std::vector<Diagnostic>> SubprocessLspClient::diagnostics(std::filesystem::path const& path, CancelCallback cancel_requested)
{
  std::lock_guard<std::mutex> lock(operation_mutex_);
  if (is_canceled(cancel_requested))
  {
    auto error = canceled_error("LSP diagnostics canceled", config_);
    error.with_context("path", path.string());
    return std::unexpected(std::move(error));
  }
  auto const deadline = std::chrono::steady_clock::now() + config_.request_timeout;
  if (auto opened = synchronize_document(path, deadline, cancel_requested); !opened)
    return std::unexpected(std::move(opened.error()));
  auto const uri = file_uri(path, config_.workspace_root);
  if (supports_pull_diagnostics_)
  {
    std::string const params = "{\"textDocument\":{\"uri\":" + json_string(uri) + "}}";
    auto response = request_response("textDocument/diagnostic", params, deadline, config_.request_timeout, "request", cancel_requested);
    if (!response)
      return std::unexpected(std::move(response.error()));
    return parse_diagnostics_response(*response, config_, path);
  }

  if (auto cached = cached_diagnostics(path))
    return std::move(*cached);
  while (true)
  {
    auto notification = read_message(deadline, config_.request_timeout, "request", "textDocument/publishDiagnostics", cancel_requested);
    if (!notification)
      return std::unexpected(std::move(notification.error()));
    auto const method = ava::core::json::string_field(*notification, "method");
    if (!method)
      return std::unexpected(lsp_error(ava::core::ErrorCategory::Tool, "LSP notification is malformed", config_));
    if (auto const id = ava::core::json::integer_field(*notification, "id"))
    {
      if (auto response = respond_to_server_request(*notification, *id, deadline, config_.request_timeout, "request", cancel_requested); !response)
        return std::unexpected(std::move(response.error()));
      continue;
    }
    if (auto dispatched = dispatch_notification(*notification); !dispatched)
      return std::unexpected(std::move(dispatched.error()));
    if (auto cached = cached_diagnostics(path))
      return std::move(*cached);
  }
}

ava::core::Result<std::vector<Symbol>> SubprocessLspClient::document_symbols(std::filesystem::path const& path, CancelCallback cancel_requested)
{
  std::lock_guard<std::mutex> lock(operation_mutex_);
  if (is_canceled(cancel_requested))
    return std::unexpected(canceled_error("LSP document symbols canceled", config_));
  auto const deadline = std::chrono::steady_clock::now() + config_.request_timeout;
  if (auto opened = synchronize_document(path, deadline, cancel_requested); !opened)
    return std::unexpected(std::move(opened.error()));
  auto const uri = file_uri(path, config_.workspace_root);
  std::string const params = "{\"textDocument\":{\"uri\":" + json_string(uri) + "}}";
  auto response = request_response("textDocument/documentSymbol", params, deadline, config_.request_timeout, "request", cancel_requested);
  if (!response)
    return std::unexpected(std::move(response.error()));
  return parse_document_symbols_response(*response, config_, path);
}

ava::core::Result<std::vector<Symbol>> SubprocessLspClient::workspace_symbols(std::string_view query, CancelCallback cancel_requested)
{
  std::lock_guard<std::mutex> lock(operation_mutex_);
  if (is_canceled(cancel_requested))
    return std::unexpected(canceled_error("LSP workspace symbols canceled", config_));
  auto const deadline = std::chrono::steady_clock::now() + config_.request_timeout;
  std::string const params = "{\"query\":" + json_string(query) + "}";
  auto response = request_response("workspace/symbol", params, deadline, config_.request_timeout, "request", cancel_requested);
  if (!response)
    return std::unexpected(std::move(response.error()));
  return parse_workspace_symbols_response(*response, config_);
}

ava::core::Result<std::vector<Location>> SubprocessLspClient::definitions(std::filesystem::path const& path, int line, int column,
                                                                          CancelCallback cancel_requested)
{
  std::lock_guard<std::mutex> lock(operation_mutex_);
  if (is_canceled(cancel_requested))
    return std::unexpected(canceled_error("LSP definition canceled", config_));
  if (line < 0 || column < 0)
  {
    return std::unexpected(lsp_error(ava::core::ErrorCategory::InvalidArgument, "LSP definition position is invalid", config_));
  }
  auto const deadline = std::chrono::steady_clock::now() + config_.request_timeout;
  if (auto opened = synchronize_document(path, deadline, cancel_requested); !opened)
    return std::unexpected(std::move(opened.error()));
  auto const uri = file_uri(path, config_.workspace_root);
  std::string const params =
      "{\"textDocument\":{\"uri\":" + json_string(uri) + "},\"position\":{\"line\":" + std::to_string(line) + ",\"character\":" + std::to_string(column) + "}}";
  auto response = request_response("textDocument/definition", params, deadline, config_.request_timeout, "request", cancel_requested);
  if (!response)
    return std::unexpected(std::move(response.error()));
  return parse_definition_response(*response, config_);
}

ava::core::Result<std::vector<Location>> SubprocessLspClient::references(std::filesystem::path const& path, int line, int column,
                                                                         CancelCallback cancel_requested)
{
  std::lock_guard<std::mutex> lock(operation_mutex_);
  if (is_canceled(cancel_requested))
    return std::unexpected(canceled_error("LSP references canceled", config_));
  if (line < 0 || column < 0)
  {
    return std::unexpected(lsp_error(ava::core::ErrorCategory::InvalidArgument, "LSP references position is invalid", config_));
  }
  auto const deadline = std::chrono::steady_clock::now() + config_.request_timeout;
  if (auto opened = synchronize_document(path, deadline, cancel_requested); !opened)
    return std::unexpected(std::move(opened.error()));
  auto const uri = file_uri(path, config_.workspace_root);
  std::string const params = "{\"textDocument\":{\"uri\":" + json_string(uri) + "},\"position\":{\"line\":" + std::to_string(line) +
                             ",\"character\":" + std::to_string(column) + "},\"context\":{\"includeDeclaration\":true}}";
  auto response = request_response("textDocument/references", params, deadline, config_.request_timeout, "request", cancel_requested);
  if (!response)
    return std::unexpected(std::move(response.error()));
  return parse_definition_response(*response, config_);
}

ava::core::VoidResult SubprocessLspClient::synchronize_document(std::filesystem::path const& path, std::chrono::steady_clock::time_point deadline,
                                                                CancelCallback cancel_requested)
{
  if (is_canceled(cancel_requested))
  {
    terminate_child();
    return std::unexpected(canceled_error("LSP document synchronization canceled", config_));
  }
  auto content = read_text_document(path, config_, deadline, cancel_requested);
  if (!content)
  {
    if (is_canceled(cancel_requested))
    {
      terminate_child();
      return std::unexpected(canceled_error("LSP document synchronization canceled", config_));
    }
    if (std::chrono::steady_clock::now() >= deadline)
    {
      auto error = lsp_error(ava::core::ErrorCategory::Tool, "timed out reading LSP document", config_);
      error.with_context("timeout_ms", std::to_string(config_.request_timeout.count()));
      error.with_context("phase", "request");
      error.with_context("method", "textDocument/didOpen");
      terminate_child();
      return std::unexpected(std::move(error));
    }
    return std::unexpected(std::move(content.error()));
  }
  if (is_canceled(cancel_requested))
  {
    terminate_child();
    return std::unexpected(canceled_error("LSP document synchronization canceled", config_));
  }
  if (std::chrono::steady_clock::now() >= deadline)
  {
    auto error = lsp_error(ava::core::ErrorCategory::Tool, "timed out reading LSP document", config_);
    error.with_context("timeout_ms", std::to_string(config_.request_timeout.count()));
    error.with_context("phase", "request");
    error.with_context("method", "textDocument/didOpen");
    terminate_child();
    return std::unexpected(std::move(error));
  }

  auto const uri = file_uri(path, config_.workspace_root);
  auto const existing = open_document_contents_.find(uri);
  if (existing != open_document_contents_.end() && existing->second == *content)
    return {};
  if (existing == open_document_contents_.end())
  {
    if (open_document_contents_.size() >= kMaxOpenDocuments)
      return std::unexpected(lsp_error(ava::core::ErrorCategory::Tool, "LSP open document count exceeds cap", config_));
    std::string const params = "{\"textDocument\":{\"uri\":" + json_string(uri) + ",\"languageId\":" + json_string(config_.language_id) +
                               ",\"version\":1,\"text\":" + json_string(*content) + "}}";
    auto sent = send_notification("textDocument/didOpen", params, deadline, config_.request_timeout, "request", cancel_requested);
    if (!sent)
      return sent;
    open_document_contents_.emplace(uri, std::move(*content));
    open_document_versions_.emplace(uri, 1);
    return {};
  }

  auto const version_it = open_document_versions_.find(uri);
  if (version_it == open_document_versions_.end() || version_it->second == std::numeric_limits<int>::max())
    return std::unexpected(lsp_error(ava::core::ErrorCategory::Tool, "LSP document version exceeds cap", config_));
  int const next_version = version_it->second + 1;
  std::string const params = "{\"textDocument\":{\"uri\":" + json_string(uri) + ",\"version\":" + std::to_string(next_version) +
                             "},\"contentChanges\":[{\"text\":" + json_string(*content) + "}]}";
  if (auto cached = diagnostics_cache_.find(uri); cached != diagnostics_cache_.end())
  {
    diagnostics_cache_total_bytes_ -= diagnostics_cache_bytes_[uri];
    diagnostics_cache_bytes_.erase(uri);
    diagnostics_cache_.erase(cached);
  }
  auto sent = send_notification("textDocument/didChange", params, deadline, config_.request_timeout, "request", cancel_requested);
  if (!sent)
    return sent;
  existing->second = std::move(*content);
  version_it->second = next_version;
  return {};
}

std::optional<std::vector<Diagnostic>> SubprocessLspClient::cached_diagnostics(std::filesystem::path const& path) const
{
  auto const cached = diagnostics_cache_.find(file_uri(path, config_.workspace_root));
  if (cached == diagnostics_cache_.end())
    return std::nullopt;
  return cached->second;
}

}  // namespace ava::lsp

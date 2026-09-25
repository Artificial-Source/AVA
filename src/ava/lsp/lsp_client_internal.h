#pragma once

#include "ava/lsp/lsp_client.h"

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>
#include <signal.h>
#include <sys/types.h>
#include "debug.h"

namespace ava::lsp::lsp_client_internal {

ava::core::Error lsp_error(ava::core::ErrorCategory category, std::string message, ServerConfig const& config,
                           ava::core::ErrorCode code = ava::core::ErrorCode::Unspecified);
ava::core::Error errno_error(std::string message, ServerConfig const& config);
bool is_canceled(CancelCallback const& cancel_requested);
ava::core::Error canceled_error(std::string message, ServerConfig const& config);

ssize_t read_retry(int fd, char* data, std::size_t size);
ssize_t write_retry(int fd, char const* data, std::size_t size);

bool path_is_within(std::filesystem::path const& candidate, std::filesystem::path const& root);
std::string file_uri(std::filesystem::path const& path, std::filesystem::path const& workspace_root);
std::string json_string(std::string_view value);
ava::core::Result<std::vector<Diagnostic>> parse_diagnostics_response(std::string_view response, ServerConfig const& config, std::filesystem::path const& path);
ava::core::Result<bool> parse_pull_diagnostics_capability(std::string_view response, ServerConfig const& config);
ava::core::Result<std::vector<Symbol>> parse_document_symbols_response(std::string_view response, ServerConfig const& config,
                                                                       std::filesystem::path const& path);
ava::core::Result<std::vector<Symbol>> parse_workspace_symbols_response(std::string_view response, ServerConfig const& config);
ava::core::Result<std::vector<Location>> parse_definition_response(std::string_view response, ServerConfig const& config);

}  // namespace ava::lsp::lsp_client_internal

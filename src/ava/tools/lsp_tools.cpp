#include "sys.h"
#include "ava/tools/lsp_tools.h"

#include <memory>
#include <utility>

namespace ava::tools {

namespace {

ava::core::VoidResult ensure_lsp_permission(ToolContext const& context, std::filesystem::path const& target_path,
                                            std::string_view command, std::string_view tool_name)
{
  return ensure_permission(context, ava::permissions::Operation::LspQuery, target_path, std::string(command),
                           std::string(tool_name), "LSP code intelligence requires permission");
}

ava::core::Result<std::shared_ptr<ava::lsp::DiagnosticsProvider>> lsp_provider(ToolContext const& context,
                                                                              std::filesystem::path const& path,
                                                                              std::string_view tool_name)
{
  std::shared_ptr<ava::lsp::DiagnosticsProvider> provider = context.lsp_diagnostics_provider;
  if (!provider) {
    auto error = ava::core::Error(ava::core::ErrorCategory::Tool, "LSP provider is unavailable");
    error.with_context("tool", std::string(tool_name));
    error.with_context("path", path.string());
    return std::unexpected(std::move(error));
  }
  return provider;
}

ava::core::VoidResult check_not_canceled(ToolContext const& context)
{
  if (context.cancel_requested && context.cancel_requested()) {
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Unknown, "tool canceled", ava::core::ErrorCode::Canceled));
  }
  return {};
}

}  // namespace

ava::core::Result<LspDiagnosticsResult> lsp_diagnostics(ToolContext const& context, std::filesystem::path const& path)
{
  if (auto canceled = check_not_canceled(context); !canceled) return std::unexpected(std::move(canceled.error()));
  if (auto permission = ensure_permission(context, ava::permissions::Operation::LspQuery, path, "", "lsp_diagnostics",
                                          "LSP diagnostics require permission");
      !permission) {
    return std::unexpected(std::move(permission.error()));
  }
  if (auto canceled = check_not_canceled(context); !canceled) return std::unexpected(std::move(canceled.error()));

  auto provider = lsp_provider(context, path, "lsp_diagnostics");
  if (!provider) return std::unexpected(std::move(provider.error()));

  auto diagnostics = (*provider)->diagnostics(path, context.cancel_requested);
  if (!diagnostics) return std::unexpected(std::move(diagnostics.error()));
  return LspDiagnosticsResult{.path = path, .diagnostics = std::move(*diagnostics)};
}

ava::core::Result<LspSymbolsResult> lsp_document_symbols(ToolContext const& context, std::filesystem::path const& path)
{
  if (auto canceled = check_not_canceled(context); !canceled) return std::unexpected(std::move(canceled.error()));
  if (auto permission = ensure_lsp_permission(context, path, "", "lsp_document_symbols"); !permission) {
    return std::unexpected(std::move(permission.error()));
  }
  if (auto canceled = check_not_canceled(context); !canceled) return std::unexpected(std::move(canceled.error()));
  auto provider = lsp_provider(context, path, "lsp_document_symbols");
  if (!provider) return std::unexpected(std::move(provider.error()));
  auto symbols = (*provider)->document_symbols(path, context.cancel_requested);
  if (!symbols) return std::unexpected(std::move(symbols.error()));
  return LspSymbolsResult{.path = path, .query = {}, .symbols = std::move(*symbols)};
}

ava::core::Result<LspSymbolsResult> lsp_workspace_symbols(ToolContext const& context, std::string_view query)
{
  if (auto canceled = check_not_canceled(context); !canceled) return std::unexpected(std::move(canceled.error()));
  if (auto permission = ensure_lsp_permission(context, context.workspace_dir, query, "lsp_workspace_symbols");
      !permission) {
    return std::unexpected(std::move(permission.error()));
  }
  if (auto canceled = check_not_canceled(context); !canceled) return std::unexpected(std::move(canceled.error()));
  auto provider = lsp_provider(context, context.workspace_dir, "lsp_workspace_symbols");
  if (!provider) return std::unexpected(std::move(provider.error()));
  auto symbols = (*provider)->workspace_symbols(query, context.cancel_requested);
  if (!symbols) return std::unexpected(std::move(symbols.error()));
  return LspSymbolsResult{.path = context.workspace_dir, .query = std::string(query), .symbols = std::move(*symbols)};
}

ava::core::Result<LspDefinitionResult> lsp_definition(ToolContext const& context, std::filesystem::path const& path,
                                                       int line, int column)
{
  if (auto canceled = check_not_canceled(context); !canceled) return std::unexpected(std::move(canceled.error()));
  if (line < 0 || column < 0) {
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "LSP definition position is invalid"));
  }
  if (auto permission = ensure_lsp_permission(context, path, "", "lsp_definition"); !permission) {
    return std::unexpected(std::move(permission.error()));
  }
  if (auto canceled = check_not_canceled(context); !canceled) return std::unexpected(std::move(canceled.error()));
  auto provider = lsp_provider(context, path, "lsp_definition");
  if (!provider) return std::unexpected(std::move(provider.error()));
  auto locations = (*provider)->definitions(path, line, column, context.cancel_requested);
  if (!locations) return std::unexpected(std::move(locations.error()));
  return LspDefinitionResult{.path = path, .line = line, .column = column, .locations = std::move(*locations)};
}

ava::core::Result<LspDefinitionResult> lsp_references(ToolContext const& context, std::filesystem::path const& path,
                                                       int line, int column)
{
  if (auto canceled = check_not_canceled(context); !canceled) return std::unexpected(std::move(canceled.error()));
  if (line < 0 || column < 0) {
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "LSP references position is invalid"));
  }
  if (auto permission = ensure_lsp_permission(context, path, "", "lsp_references"); !permission) {
    return std::unexpected(std::move(permission.error()));
  }
  if (auto canceled = check_not_canceled(context); !canceled) return std::unexpected(std::move(canceled.error()));
  auto provider = lsp_provider(context, path, "lsp_references");
  if (!provider) return std::unexpected(std::move(provider.error()));
  auto locations = (*provider)->references(path, line, column, context.cancel_requested);
  if (!locations) return std::unexpected(std::move(locations.error()));
  return LspDefinitionResult{.path = path, .line = line, .column = column, .locations = std::move(*locations)};
}

}  // namespace ava::tools

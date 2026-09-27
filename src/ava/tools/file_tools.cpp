#include "sys.h"
#include "ava/tools/diff_utils.h"
#include "ava/tools/edit_match.h"
#include "ava/tools/file_io.h"
#include "ava/tools/file_tools.h"
#include "ava/tools/file_tools_internal.h"
#include "ava/tools/mutation_queue.h"
#include "ava/tools/secure_workspace.h"
#include "ava/tools/tool_permission.h"
#include "ava/tools/tool_permission_internal.h"
#include "ava/permissions/permission_rules.h"
#include "ava/core/ids.h"
#include "ava/core/path.h"

#include <memory>
#include <optional>
#include <string_view>
#include <utility>

namespace ava::tools {

namespace {

constexpr std::size_t kMaxPermissionDiffBytes = 32 * 1024;

struct PermissionDiffPreview
{
  std::string text;
  bool truncated = false;
};

using detail::check_canceled;
using detail::is_canceled_error;
using detail::read_all_text;
using detail::read_head_text;
using detail::write_file_unlocked;
using tool_permission_internal::audit_event;
using tool_permission_internal::effective_tool_name;
using tool_permission_internal::record_permission_audit;

std::shared_ptr<MutationQueue> effective_mutation_queue(ToolContext const& context)
{
  if (context.mutation_queue)
    return context.mutation_queue;
  return default_mutation_queue();
}

using ava::core::normalized_absolute_path;

bool is_legacy_workspace_permission_rules_path(ToolContext const& context, std::filesystem::path const& path)
{
  if (context.workspace_dir.empty())
    return false;
  auto const protected_path = normalized_absolute_path(context.workspace_dir / ".ava" / "permission-rules.json");
  return normalized_absolute_path(path) == protected_path;
}

bool is_enforceable_permission_rules_path(ToolContext const& context, std::filesystem::path const& path)
{
  if (ava::permissions::is_registered_enforceable_permission_rules_file(path))
    return true;
  if (context.protected_permission_rule_store && ava::permissions::is_enforceable_permission_rules_file(*context.protected_permission_rule_store, path))
    return true;
  return false;
}

}  // namespace

namespace file_tools_internal {

ava::core::VoidResult reject_permission_rules_file_mutation(ToolContext const& context, std::filesystem::path const& path)
{
  if (!is_legacy_workspace_permission_rules_path(context, path) && !is_enforceable_permission_rules_path(context, path))
    return {};

  auto error = ava::core::Error(ava::core::ErrorCategory::PermissionDenied, "permission rule files cannot be modified by normal file tools");
  error.with_context("path", normalized_absolute_path(path).string());
  error.with_context("management", "use permission_rule_add or permission_rule_remove RPC commands");
  return std::unexpected(std::move(error));
}

}  // namespace file_tools_internal

namespace {

using file_tools_internal::reject_permission_rules_file_mutation;

ava::core::Result<bool> write_permission_diff_preview_read_allowed(ToolContext const& context, std::filesystem::path const& path)
{
  auto const decision = ava::permissions::decide(ava::permissions::PermissionRequest{
      .operation = ava::permissions::Operation::ReadFile,
      .mode = context.mode,
      .workspace_dir = context.workspace_dir,
      .target_path = path,
      .command = "",
  });
  if (decision.action != ava::permissions::PermissionAction::Allow)
    return false;
  if (!context.auto_allow_deny_preflight)
    return true;

  auto const permission_request_id = ava::core::make_id("permreq");
  auto const tool_name = effective_tool_name(context, ava::permissions::Operation::ReadFile, {});
  auto preflight = context.auto_allow_deny_preflight(ava::permissions::PermissionPrompt{
      .permission_request_id = permission_request_id,
      .tool_call_id = context.current_call_id,
      .operation = ava::permissions::Operation::ReadFile,
      .mode = context.mode,
      .workspace_dir = context.workspace_dir,
      .target_path = path,
      .command = "",
      .tool_name = tool_name,
      .reason = decision.reason,
      .risk = decision.risk,
  });
  if (preflight && (*preflight == ava::permissions::PermissionResolution::Allow || *preflight == ava::permissions::PermissionResolution::AllowSessionGrant))
  {
    return true;
  }

  auto denied = decision;
  denied.action = ava::permissions::PermissionAction::Deny;
  denied.risk = ava::permissions::PermissionRisk::Critical;
  denied.reason = preflight ? (preflight->reason.empty() ? std::string("operation denied by persistent policy") : preflight->reason)
                            : "persistent deny preflight failed: " + preflight.error().format();
  auto event = audit_event(context, permission_request_id, ava::permissions::Operation::ReadFile, tool_name, denied, path, "", std::nullopt);
  event.resolution = "deny";
  event.resolution_source = preflight && !preflight->resolution_source.empty() ? preflight->resolution_source : "persistent_rule_error";
  event.resolution_reason = denied.reason;
  if (preflight)
    event.rule_id = preflight->rule_id;
  if (auto audited = record_permission_audit(context, event); !audited)
    return std::unexpected(std::move(audited.error()));
  return false;
}

ava::core::Result<std::optional<PermissionDiffPreview>> write_permission_diff_preview(ToolContext const& context, std::filesystem::path const& path,
                                                                                      std::string_view content)
{
  if (auto canceled = check_canceled(context, "write_file_permission_preview", path); !canceled)
  {
    return std::unexpected(std::move(canceled.error()));
  }
  // Permission must precede every installed frontend call. A local preview
  // would also be misleading when the frontend owns the exact written bytes.
  if (context.exact_file_access && context.exact_file_access->supports_write_text_file())
    return std::optional<PermissionDiffPreview>{};
  bool exists = false;
  auto permission_target = path;
  if (context.secure_workspace)
  {
    auto resolved = context.secure_workspace->resolve(path, SecureWorkspaceResolveMode::AllowMissing);
    if (!resolved)
      return std::unexpected(std::move(resolved.error()));
    exists = resolved->exists;
    permission_target = std::move(resolved->absolute);
  }
  else
  {
    std::error_code exists_error;
    exists = std::filesystem::exists(path, exists_error);
    if (exists_error)
      return std::optional<PermissionDiffPreview>{};
  }

  std::string original;
  if (exists)
  {
    auto read_allowed = write_permission_diff_preview_read_allowed(context, permission_target);
    // An overwrite may proceed without a diff preview, but denied old bytes
    // must not be read or returned through that optional preview. This safety
    // check is deliberately preflight-only: preparing a write prompt must not
    // acquire separate frontend read authority.
    if (!read_allowed || !*read_allowed)
      return std::optional<PermissionDiffPreview>{};
    auto current = context.exact_file_access && !context.exact_file_access->supports_write_text_file()
                       ? detail::read_all_text_local_only(context, path, "write_file_permission_preview")
                       : read_all_text(context, path, "write_file_permission_preview");
    if (!current)
    {
      if (is_canceled_error(current.error()))
        return std::unexpected(std::move(current.error()));
      return std::optional<PermissionDiffPreview>{};
    }
    original = std::move(*current);
  }

  auto diff = unified_diff(original, content, path, path, kMaxPermissionDiffBytes);
  return std::optional<PermissionDiffPreview>{PermissionDiffPreview{.text = std::move(diff.text), .truncated = diff.truncated}};
}

}  // namespace

ava::core::Result<TextOutput> read_file(ToolContext const& context, std::filesystem::path const& path, ReadOptions options)
{
  if (auto canceled = check_canceled(context, "read_file", path); !canceled)
  {
    return std::unexpected(std::move(canceled.error()));
  }
  if (!options.permission_already_checked)
  {
    if (auto permission = ensure_permission(context, ava::permissions::Operation::ReadFile, path, "", "", "tool requires permission"); !permission)
    {
      return std::unexpected(permission.error());
    }
  }
  if (auto canceled = check_canceled(context, "read_file", path); !canceled)
  {
    return std::unexpected(std::move(canceled.error()));
  }
  if (!options.permission_already_checked)
  {
    if (auto started = announce_tool_execution_start(context); !started)
      return std::unexpected(std::move(started.error()));
  }

  return read_head_text(context, path, options);
}

ava::core::Result<FileMutationResult> write_file(ToolContext const& context, std::filesystem::path const& path, std::string_view content, WriteOptions options)
{
  if (auto canceled = check_canceled(context, "write_file", path); !canceled)
  {
    return std::unexpected(std::move(canceled.error()));
  }
  // Reject symlinks before any permission check or file read. This prevents
  // writing through a symlink to a protected target (e.g., a source file in
  // plan mode) without needing canonicalization to detect the target.
  // The check uses symlink_status (no path resolution), consistent with
  // read_file's symlink rejection.
  if (std::error_code link_error; std::filesystem::is_symlink(std::filesystem::symlink_status(path, link_error)))
  {
    auto error = ava::core::Error(ava::core::ErrorCategory::PermissionDenied, "file writes do not follow symlinks");
    error.with_context("operation", "write_file");
    error.with_context("path", path.string());
    return std::unexpected(std::move(error));
  }
  if (auto protected_file = reject_permission_rules_file_mutation(context, path); !protected_file)
  {
    return std::unexpected(std::move(protected_file.error()));
  }
  std::optional<PermissionDiffPreview> preview;
  if (!options.permission_already_checked)
  {
    auto preview_result = write_permission_diff_preview(context, path, content);
    if (!preview_result)
      return std::unexpected(std::move(preview_result.error()));
    preview = std::move(*preview_result);
    auto const diff_preview = preview ? std::string_view(preview->text) : std::string_view{};
    if (auto permission = ensure_permission(context, ava::permissions::Operation::EditFile, path, "", "", "tool requires permission", diff_preview,
                                            preview ? preview->truncated : false);
        !permission)
    {
      return std::unexpected(permission.error());
    }
  }
  if (auto canceled = check_canceled(context, "write_file", path); !canceled)
  {
    return std::unexpected(std::move(canceled.error()));
  }
  if (!options.permission_already_checked)
  {
    if (auto started = announce_tool_execution_start(context); !started)
      return std::unexpected(std::move(started.error()));
  }

  auto attach_preview = [&preview](ava::core::Result<FileMutationResult> written) -> ava::core::Result<FileMutationResult> {
    if (written && preview)
    {
      written->diff = std::move(preview->text);
      written->diff_truncated = preview->truncated;
    }
    return written;
  };

  if (options.mutation_already_locked)
    return attach_preview(write_file_unlocked(context, path, content));
  [[maybe_unused]] auto mutation_lock = effective_mutation_queue(context)->lock_path(path);
  return attach_preview(write_file_unlocked(context, path, content));
}

ava::core::Result<FileMutationResult> edit_file(ToolContext const& context, std::filesystem::path const& path, std::string_view old_text,
                                                std::string_view new_text)
{
  if (old_text.empty())
  {
    auto error = ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "old_text must not be empty");
    error.with_context("path", path.string());
    return std::unexpected(std::move(error));
  }
  if (context.exact_file_access && context.exact_file_access->supports_read_text_file() != context.exact_file_access->supports_write_text_file())
  {
    auto error = ava::core::Error(ava::core::ErrorCategory::PermissionDenied,
                                  "edit_file requires coherent read and write ownership; the exact-file capabilities are partial");
    error.with_context("path", path.string());
    return std::unexpected(std::move(error));
  }
  if (auto canceled = check_canceled(context, "edit_file", path); !canceled)
  {
    return std::unexpected(std::move(canceled.error()));
  }
  if (auto protected_file = reject_permission_rules_file_mutation(context, path); !protected_file)
  {
    return std::unexpected(std::move(protected_file.error()));
  }
  auto const read_decision = ava::permissions::decide(ava::permissions::PermissionRequest{
      .operation = ava::permissions::Operation::ReadFile,
      .mode = context.mode,
      .workspace_dir = context.workspace_dir,
      .target_path = path,
      .command = "",
  });
  if (read_decision.action == ava::permissions::PermissionAction::Deny)
  {
    if (auto permission = ensure_permission(context, ava::permissions::Operation::ReadFile, path, "", "", "tool requires permission"); !permission)
    {
      return std::unexpected(permission.error());
    }
  }
  auto const edit_decision = ava::permissions::decide(ava::permissions::PermissionRequest{
      .operation = ava::permissions::Operation::EditFile,
      .mode = context.mode,
      .workspace_dir = context.workspace_dir,
      .target_path = path,
      .command = "",
  });
  if (edit_decision.action == ava::permissions::PermissionAction::Deny)
  {
    if (auto permission = ensure_permission(context, ava::permissions::Operation::EditFile, path, "", "", "tool requires permission"); !permission)
    {
      return std::unexpected(permission.error());
    }
  }
  if (auto permission = ensure_permission(context, ava::permissions::Operation::ReadFile, path, "", "", "tool requires permission"); !permission)
  {
    return std::unexpected(permission.error());
  }
  if (auto canceled = check_canceled(context, "edit_file", path); !canceled)
  {
    return std::unexpected(std::move(canceled.error()));
  }

  [[maybe_unused]] auto mutation_lock = effective_mutation_queue(context)->lock_path(path);
  auto content = read_all_text(context, path, "edit_file");
  if (!content)
  {
    return std::unexpected(content.error());
  }
  if (auto canceled = check_canceled(context, "edit_file", path); !canceled)
  {
    return std::unexpected(std::move(canceled.error()));
  }

  auto match = find_unique_text_match(*content, old_text, path, "old_text was not found", "old_text is not unique");
  if (!match)
    return std::unexpected(match.error());

  auto const original = *content;
  content->replace(match->position, match->size, new_text);
  auto diff = unified_diff(original, *content, path, path, kMaxPermissionDiffBytes);
  if (auto permission = ensure_permission(context, ava::permissions::Operation::EditFile, path, "", "", "tool requires permission", diff.text, diff.truncated);
      !permission)
  {
    return std::unexpected(permission.error());
  }
  if (auto canceled = check_canceled(context, "edit_file", path); !canceled)
  {
    return std::unexpected(std::move(canceled.error()));
  }
  if (auto started = announce_tool_execution_start(context); !started)
    return std::unexpected(std::move(started.error()));
  auto written = write_file(context, path, *content, WriteOptions{.permission_already_checked = true, .mutation_already_locked = true});
  if (!written)
    return std::unexpected(written.error());

  written->diff = std::move(diff.text);
  written->diff_truncated = diff.truncated;
  written->line_endings = to_string(match->content_analysis.line_endings);
  written->had_utf8_bom = match->content_analysis.has_utf8_bom;
  return written;
}

}  // namespace ava::tools

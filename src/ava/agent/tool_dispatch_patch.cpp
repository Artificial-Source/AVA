#include "sys.h"
#include "ava/agent/tool_dispatch_common.h"
#include "ava/agent/tool_dispatch_patch.h"
#include "ava/tools/diff_utils.h"
#include "ava/tools/edit_match.h"
#include "ava/tools/file_io.h"
#include "ava/tools/file_tools.h"
#include "ava/tools/mutation_queue.h"
#include "ava/tools/secure_workspace.h"
#include "ava/tools/tool_permission.h"
#include "ava/core/ids.h"
#include "ava/core/json.h"

#include <algorithm>
#include <map>
#include <utility>
#include <vector>

namespace ava::agent {
namespace {

constexpr std::size_t kMaxMutationDiffBytes = 32 * 1024;

std::filesystem::path unique_patch_temp_path(std::filesystem::path const& target)
{
  auto const stem = target.filename().string() + ".ava-patch-";
  for (int attempt = 0; attempt < 8; ++attempt)
  {
    auto candidate = target.parent_path() / (stem + ava::core::make_id("tmp") + ".tmp");
    std::error_code exists_error;
    if (!std::filesystem::exists(candidate, exists_error) && !exists_error)
      return candidate;
  }
  return target.parent_path() / (stem + ava::core::make_id("tmp") + ".tmp");
}

struct StagedPatchWrite
{
  std::filesystem::path target;
  std::filesystem::path temp;
  std::size_t bytes_written = 0;
};

void cleanup_staged_patch_writes(std::vector<StagedPatchWrite> const& writes, std::size_t start_index = 0)
{
  for (std::size_t index = start_index; index < writes.size(); ++index)
  {
    ava::tools::remove_staged_file_best_effort(writes[index].temp);
  }
}

ava::core::VoidResult apply_existing_target_permissions_to_staged_file(std::filesystem::path const& target, std::filesystem::path const& temp)
{
  std::error_code status_error;
  auto const target_status = std::filesystem::status(target, status_error);
  if (status_error)
  {
    auto error = ava::core::Error(ava::core::ErrorCategory::Io, "failed to read target permissions for patch write");
    error.with_context("path", target.string());
    error.with_context("temp_path", temp.string());
    error.with_context("cause", status_error.message());
    return std::unexpected(std::move(error));
  }
  if (!std::filesystem::exists(target_status))
    return {};

  std::error_code permissions_error;
  std::filesystem::permissions(temp, target_status.permissions(), std::filesystem::perm_options::replace, permissions_error);
  if (permissions_error)
  {
    auto error = ava::core::Error(ava::core::ErrorCategory::Io, "failed to apply target permissions to patch write");
    error.with_context("path", target.string());
    error.with_context("temp_path", temp.string());
    error.with_context("cause", permissions_error.message());
    return std::unexpected(std::move(error));
  }
  return {};
}

ava::core::Result<std::vector<StagedPatchWrite>> stage_patch_writes(ava::tools::ToolContext const& context, std::vector<std::filesystem::path> const& paths,
                                                                    std::map<std::filesystem::path, std::string> const& final_contents)
{
  std::vector<StagedPatchWrite> staged;
  staged.reserve(paths.size());

  for (auto const& path : paths)
  {
    auto const temp = unique_patch_temp_path(path);
    auto const content = final_contents.find(path);
    if (content == final_contents.end())
    {
      auto error = ava::core::Error(ava::core::ErrorCategory::Unknown, "missing staged patch content");
      error.with_context("path", path.string());
      cleanup_staged_patch_writes(staged);
      return std::unexpected(std::move(error));
    }

    auto written = ava::tools::write_file(context, temp, content->second, ava::tools::WriteOptions{.permission_already_checked = true});
    if (!written)
    {
      ava::tools::remove_staged_file_best_effort(temp);
      cleanup_staged_patch_writes(staged);
      auto error = written.error();
      error.with_context("stage", "temporary_patch_write");
      error.with_context("target_path", path.string());
      error.with_context("temp_path", temp.string());
      return std::unexpected(std::move(error));
    }

    if (auto permissions = apply_existing_target_permissions_to_staged_file(path, temp); !permissions)
    {
      ava::tools::remove_staged_file_best_effort(temp);
      cleanup_staged_patch_writes(staged);
      auto error = permissions.error();
      error.with_context("stage", "temporary_patch_permissions");
      return std::unexpected(std::move(error));
    }

    staged.push_back(StagedPatchWrite{.target = path, .temp = temp, .bytes_written = written->bytes_written});
  }

  return staged;
}

ava::core::VoidResult commit_staged_patch_writes(std::vector<StagedPatchWrite> const& staged)
{
  for (std::size_t index = 0; index < staged.size(); ++index)
  {
    auto const& write = staged[index];
    if (auto committed = ava::tools::replace_file_with_staged_file(write.temp, write.target); !committed)
    {
      cleanup_staged_patch_writes(staged, index);
      auto error = committed.error();
      error.with_context("stage", "commit_staged_patch_write");
      error.with_context("atomicity", "all edits are staged before commit, but cross-file rename commit is not atomic");
      return std::unexpected(std::move(error));
    }
  }
  return {};
}

ava::core::Result<std::vector<ava::tools::SecureWorkspace::StagedWrite>> stage_secure_patch_writes(
    ava::tools::ToolContext const& context, std::vector<std::filesystem::path> const& paths, std::map<std::filesystem::path, std::string> const& final_contents)
{
  std::vector<ava::tools::SecureWorkspace::StagedWrite> staged;
  staged.reserve(paths.size());
  for (auto const& path : paths)
  {
    auto const content = final_contents.find(path);
    if (content == final_contents.end())
    {
      auto error = ava::core::Error(ava::core::ErrorCategory::Unknown, "missing staged patch content");
      error.with_context("path", path.string());
      return std::unexpected(std::move(error));
    }
    auto write = context.secure_workspace->stage_write(path, content->second, context.cancel_requested);
    if (!write)
    {
      auto error = std::move(write.error());
      error.with_context("stage", "temporary_patch_write");
      error.with_context("target_path", path.string());
      return std::unexpected(std::move(error));
    }
    staged.push_back(std::move(*write));
  }
  return staged;
}

std::string describe_patch_paths(std::vector<std::filesystem::path> const& paths)
{
  if (paths.empty())
    return "(none)";
  std::string result;
  for (auto const& path : paths)
  {
    if (!result.empty())
      result += ", ";
    result += path.generic_string();
  }
  return result;
}

ava::core::VoidResult commit_secure_patch_writes(std::vector<ava::tools::SecureWorkspace::StagedWrite>& staged)
{
  std::vector<std::filesystem::path> committed_paths;
  committed_paths.reserve(staged.size());
  for (auto& write : staged)
  {
    if (auto committed = write.commit(); !committed)
    {
      auto changed_paths = committed_paths;
      if (write.target_changed())
        changed_paths.push_back(write.path());
      auto error = std::move(committed.error());
      error.with_context("stage", "commit_staged_patch_write");
      error.with_context("atomicity", "all targets were staged before commit, but cross-file commit is not atomic and changed targets were not rolled back");
      error.with_context("already_committed_paths", describe_patch_paths(committed_paths));
      error.with_context("changed_paths", describe_patch_paths(changed_paths));
      error.with_context("commit_failed_path", write.path().generic_string());
      return std::unexpected(std::move(error));
    }
    committed_paths.push_back(write.path());
  }
  return {};
}

}  // namespace

ToolDispatchResult apply_patch_result(ava::tools::ToolContext const& context, ProviderToolCall const& call)
{
  if (context.exact_file_access && context.exact_file_access->supports_read_text_file() != context.exact_file_access->supports_write_text_file())
  {
    return tool_dispatch::simple_error_result(call, ava::core::ErrorCategory::PermissionDenied,
                                              "apply_patch requires coherent read and write ownership; the exact-file capabilities are partial");
  }

  auto const edits = ava::core::json::objects_in_array_field(call.arguments_json, "edits");
  if (edits.empty())
  {
    return tool_dispatch::simple_error_result(call, ava::core::ErrorCategory::InvalidArgument, "apply_patch requires a non-empty edits array");
  }
  if (edits.size() > 32)
  {
    return tool_dispatch::simple_error_result(call, ava::core::ErrorCategory::InvalidArgument, "apply_patch supports at most 32 edits per call");
  }

  struct PatchEdit
  {
    std::filesystem::path target;
    std::string old_text;
    std::string new_text;
  };

  struct PatchReplacement
  {
    std::size_t position = 0;
    std::size_t old_size = 0;
    std::string new_text;
  };

  std::vector<PatchEdit> parsed_edits;
  parsed_edits.reserve(edits.size());
  std::map<std::filesystem::path, std::filesystem::path> permission_targets;
  for (std::size_t index = 0; index < edits.size(); ++index)
  {
    if (auto canceled = tool_dispatch::check_canceled(context, call); !canceled)
    {
      return tool_dispatch::tool_error_result(call, canceled.error());
    }
    auto path = tool_dispatch::required_safe_string_arg(edits[index], "path", call.name);
    if (!path)
      return tool_dispatch::tool_error_result(call, path.error());
    auto old_text = tool_dispatch::required_text_arg(edits[index], "old_text", call.name);
    if (!old_text)
      return tool_dispatch::tool_error_result(call, old_text.error());
    if (old_text->empty())
    {
      auto error = ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "patch old_text must not be empty");
      error.with_context("path", *path);
      return tool_dispatch::tool_error_result(call, error);
    }
    auto new_text = tool_dispatch::required_text_arg(edits[index], "new_text", call.name);
    if (!new_text)
      return tool_dispatch::tool_error_result(call, new_text.error());

    auto const unresolved_target = tool_dispatch::workspace_path(context, *path);
    auto const target = context.secure_workspace ? unresolved_target.lexically_normal() : tool_dispatch::permission_dedupe_path(unresolved_target);
    permission_targets.emplace(target, target);
    parsed_edits.push_back(PatchEdit{.target = target, .old_text = std::move(*old_text), .new_text = std::move(*new_text)});
  }

  if (context.exact_file_access && context.exact_file_access->supports_read_text_file() && context.exact_file_access->supports_write_text_file() &&
      permission_targets.size() > 1)
  {
    auto error = ava::core::Error(ava::core::ErrorCategory::InvalidArgument,
                                  "transactional multi-file client apply_patch calls are unsupported; use one distinct target per call");
    error.with_context("distinct_target_count", std::to_string(permission_targets.size()));
    error.with_context("reason", "the ACP client filesystem has no transaction primitive");
    return tool_dispatch::tool_error_result(call, error);
  }

  if (auto canceled = tool_dispatch::check_canceled(context, call); !canceled)
  {
    return tool_dispatch::tool_error_result(call, canceled.error());
  }
  for (auto const& [dedupe_path, target] : permission_targets)
  {
    static_cast<void>(dedupe_path);
    if (auto permission =
            ava::tools::ensure_permission(context, ava::permissions::Operation::ReadFile, target, "", call.name, "patch read requires permission");
        !permission)
    {
      return tool_dispatch::tool_error_result(call, permission.error());
    }
  }

  std::vector<std::filesystem::path> lock_paths;
  lock_paths.reserve(permission_targets.size());
  for (auto const& [dedupe_path, target] : permission_targets)
  {
    static_cast<void>(dedupe_path);
    lock_paths.push_back(target);
  }
  auto patch_queue = context.mutation_queue ? context.mutation_queue : ava::tools::default_mutation_queue();
  [[maybe_unused]] auto mutation_lock = patch_queue->lock_paths(lock_paths);

  if (auto canceled = tool_dispatch::check_canceled(context, call); !canceled)
  {
    return tool_dispatch::tool_error_result(call, canceled.error());
  }
  std::map<std::filesystem::path, std::string> original_contents;
  std::map<std::filesystem::path, std::vector<PatchReplacement>> replacements_by_path;
  std::vector<std::filesystem::path> applied_paths;
  for (auto const& edit : parsed_edits)
  {
    if (auto canceled = tool_dispatch::check_canceled(context, call); !canceled)
    {
      return tool_dispatch::tool_error_result(call, canceled.error());
    }
    auto const& target = edit.target;
    if (!original_contents.contains(target))
    {
      auto content = ava::tools::detail::read_all_text(context, target, "apply_patch");
      if (!content)
        return tool_dispatch::tool_error_result(call, content.error());
      original_contents.emplace(target, std::move(*content));
      applied_paths.push_back(target);
    }

    auto const& content = original_contents[target];
    auto match = ava::tools::find_unique_text_match(content, edit.old_text, target, "patch old_text was not found", "patch old_text is not unique");
    if (!match)
      return tool_dispatch::tool_error_result(call, match.error());
    replacements_by_path[target].push_back(PatchReplacement{.position = match->position, .old_size = match->size, .new_text = edit.new_text});
  }

  std::map<std::filesystem::path, std::string> final_contents;
  for (auto const& target : applied_paths)
  {
    auto content = original_contents[target];
    auto& replacements = replacements_by_path[target];
    std::ranges::sort(replacements, {}, [](PatchReplacement const& replacement) { return replacement.position; });
    for (std::size_t index = 1; index < replacements.size(); ++index)
    {
      auto const previous_end = replacements[index - 1].position + replacements[index - 1].old_size;
      if (replacements[index].position < previous_end)
      {
        auto error = ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "patch edits overlap");
        error.with_context("path", target.string());
        return tool_dispatch::tool_error_result(call, error);
      }
    }
    for (auto replacement = replacements.rbegin(); replacement != replacements.rend(); ++replacement)
    {
      content.replace(replacement->position, replacement->old_size, replacement->new_text);
    }
    final_contents.emplace(target, std::move(content));
  }

  std::string diff_text;
  bool diff_truncated = false;
  std::map<std::filesystem::path, ava::tools::DiffPreview> permission_diffs;
  for (auto const& target : applied_paths)
  {
    permission_diffs.emplace(target, ava::tools::unified_diff(original_contents[target], final_contents[target], target, target, kMaxMutationDiffBytes));
  }
  for (auto const& target : applied_paths)
  {
    if (diff_text.size() >= kMaxMutationDiffBytes)
    {
      diff_truncated = true;
      break;
    }
    auto diff = ava::tools::unified_diff(original_contents[target], final_contents[target], target, target, kMaxMutationDiffBytes - diff_text.size());
    diff_text += std::move(diff.text);
    diff_truncated = diff_truncated || diff.truncated;
  }

  for (auto const& target : applied_paths)
  {
    if (auto canceled = tool_dispatch::check_canceled(context, call); !canceled)
    {
      return tool_dispatch::tool_error_result(call, canceled.error());
    }
    auto const diff = permission_diffs.find(target);
    auto const diff_preview = diff == permission_diffs.end() ? std::string_view{} : std::string_view(diff->second.text);
    auto const permission_diff_truncated = diff != permission_diffs.end() && diff->second.truncated;
    if (auto permission = ava::tools::ensure_permission(context, ava::permissions::Operation::EditFile, target, "", call.name, "patch edit requires permission",
                                                        diff_preview, permission_diff_truncated);
        !permission)
    {
      return tool_dispatch::tool_error_result(call, permission.error());
    }
  }

  if (auto canceled = tool_dispatch::check_canceled(context, call); !canceled)
  {
    return tool_dispatch::tool_error_result(call, canceled.error());
  }
  if (auto started = ava::tools::announce_tool_execution_start(context); !started)
    return tool_dispatch::tool_error_result(call, started.error());
  std::vector<StagedPatchWrite> completed_writes;
  completed_writes.reserve(applied_paths.size());
  if (context.exact_file_access && context.exact_file_access->supports_read_text_file() && context.exact_file_access->supports_write_text_file())
  {
    for (auto const& target : applied_paths)
    {
      auto written = ava::tools::write_file(context, target, final_contents[target],
                                            ava::tools::WriteOptions{.permission_already_checked = true, .mutation_already_locked = true});
      if (!written)
        return tool_dispatch::tool_error_result(call, written.error());
      completed_writes.push_back(StagedPatchWrite{.target = target, .temp = {}, .bytes_written = written->bytes_written});
    }
  }
  else if (context.secure_workspace)
  {
    auto staged = stage_secure_patch_writes(context, applied_paths, final_contents);
    if (!staged)
      return tool_dispatch::tool_error_result(call, staged.error());
    if (auto canceled = tool_dispatch::check_canceled(context, call); !canceled)
      return tool_dispatch::tool_error_result(call, canceled.error());
    if (auto committed = commit_secure_patch_writes(*staged); !committed)
      return tool_dispatch::tool_error_result(call, committed.error());
    for (auto const& write : *staged) completed_writes.push_back(StagedPatchWrite{.target = write.path(), .temp = {}, .bytes_written = write.bytes_written()});
  }
  else
  {
    auto staged = stage_patch_writes(context, applied_paths, final_contents);
    if (!staged)
      return tool_dispatch::tool_error_result(call, staged.error());
    if (auto canceled = tool_dispatch::check_canceled(context, call); !canceled)
    {
      cleanup_staged_patch_writes(*staged);
      return tool_dispatch::tool_error_result(call, canceled.error());
    }
    if (auto committed = commit_staged_patch_writes(*staged); !committed)
      return tool_dispatch::tool_error_result(call, committed.error());
    completed_writes = std::move(*staged);
  }

  std::string text = "{\"tool\":\"apply_patch\",\"ok\":true,\"edits\":[";
  for (std::size_t index = 0; index < completed_writes.size(); ++index)
  {
    auto const& write = completed_writes[index];

    if (index > 0)
      text += ',';
    text += "{\"path\":\"" + ava::core::json::escape(write.target.generic_string()) + "\",\"bytes_written\":" + std::to_string(write.bytes_written) + "}";
  }
  text += "]";
  tool_dispatch::append_changed_files_json(text, applied_paths);
  tool_dispatch::append_diff_json(text, diff_text, diff_truncated);
  text += "}";
  return ToolDispatchResult{.call_id = call.id, .name = call.name, .success = true, .result_text = std::move(text)};
}

}  // namespace ava::agent

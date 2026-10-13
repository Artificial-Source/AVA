#include "sys.h"
#include "runtime/RunOptions.h"
#include "runtime/Session.h"
#include "runtime_prompt_file_references.h"
#include "ava/debug/print_members_on.h"
#include "ava/agent/agent_loop_session.h"
#include "ava/tools/file_tools.h"
#include "ava/core/error.h"

#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ava::app {
namespace {

constexpr std::size_t kMaxPromptFileReferences = 5;
constexpr std::size_t kPromptReferenceMaxBytes = 32 * 1024;
constexpr std::size_t kPromptReferenceMaxLines = 300;

struct PromptFileReference
{
  std::string path;

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

bool is_reference_start(std::string_view text, std::size_t index)
{
  return text[index] == '@' && (index == 0 || std::isspace(static_cast<unsigned char>(text[index - 1])) != 0);
}

bool is_trailing_reference_punctuation(char ch)
{
  switch (ch)
  {
    case '.':
    case ',':
    case ';':
    case ':':
    case '!':
    case '?':
    case ')':
    case ']':
    case '}':
      return true;
    default:
      return false;
  }
}

std::vector<PromptFileReference> prompt_file_references(std::string_view text)
{
  std::vector<PromptFileReference> references;
  auto add_reference = [&references](std::string path) {
    if (path.empty())
      return;
    if (std::ranges::any_of(references, [&](PromptFileReference const& existing) { return existing.path == path; }))
      return;
    references.push_back(PromptFileReference{.path = std::move(path)});
  };
  for (std::size_t index = 0; index < text.size(); ++index)
  {
    if (!is_reference_start(text, index))
      continue;
    if (index + 1 < text.size() && text[index + 1] == '"')
    {
      auto end = index + 2;
      while (end < text.size() && text[end] != '"') ++end;
      add_reference(std::string(text.substr(index + 2, end - index - 2)));
      index = end;
      continue;
    }
    auto end = index + 1;
    while (end < text.size() && std::isspace(static_cast<unsigned char>(text[end])) == 0) ++end;
    auto token_end = end;
    while (token_end > index + 1 && is_trailing_reference_punctuation(text[token_end - 1])) --token_end;
    if (token_end <= index + 1)
      continue;
    add_reference(std::string(text.substr(index + 1, token_end - index - 1)));
  }
  return references;
}

ava::tools::ToolContext prompt_file_reference_context(runtime::session_ts& unlocked_session, runtime::RunOptions const& options)
{
  auto snapshot = [&] {
    SCOPED_CRITICAL_AREA_R(session_r, unlocked_session);
    runtime::ModelSelection_ts::crat model_selection_r = session_r->model_selection_r();
    return std::tuple{session_r->workspace_dir(),
                      session_r->store.session_path().parent_path() / "spill",
                      session_r->mode(),
                      session_r->anchor_set(),
                      session_r->ava_authority_roots_1(),
                      session_r->store.session_id(),
                      model_selection_r->model.provider_id,
                      model_selection_r->model.model_id,
                      session_r->current_dir()};
  }();
  auto& [workspace_dir, spill_dir, mode, anchor_set, authority_roots, session_id, provider_id, model_id, current_dir] = snapshot;
  return ava::tools::ToolContext{.workspace_dir = workspace_dir,
                                 .spill_dir = spill_dir,
                                 .mode = mode,
                                 .permission_resolver = options.permission_resolver,
                                 .permission_audit_sink = [&unlocked_session](ava::tools::PermissionAuditEvent const& event) -> ava::core::VoidResult {
                                   auto append_route = runtime::session_ts::rat(unlocked_session)->owner_append_route_1();
                                   return ava::agent::append_permission_decision(append_route, event);
                                 },
                                 .cancel_requested = options.cancel_requested,
                                 .permission_tool_name = "file_reference",
                                 .permission_actor = "user",
                                 .anchor_set = anchor_set,
                                 .ava_authority_roots = authority_roots,
                                 .exact_file_access = options.exact_file_access,
                                 .command_executor = options.command_executor,
                                 .session_id = session_id,
                                 .provider_id = provider_id,
                                 .model_id = model_id,
                                 .current_dir = current_dir};
}

}  // namespace

ava::core::Result<std::string> expand_prompt_file_references(runtime::session_ts& unlocked_session, std::string const& user_message, runtime::RunOptions const& options)
{
  auto references = prompt_file_references(user_message);
  if (references.empty())
    return user_message;
  if (references.size() > kMaxPromptFileReferences)
  {
    auto error = ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "too many @ file references");
    error.with_context("max_references", std::to_string(kMaxPromptFileReferences));
    error.with_context("reference_count", std::to_string(references.size()));
    return std::unexpected(std::move(error));
  }

  auto context = prompt_file_reference_context(unlocked_session, options);
  std::string expanded = user_message;
  expanded += "\n\nReferenced files:";
  for (auto const& reference : references)
  {
    auto read = ava::tools::read_file(context, context.current_dir / reference.path,
                                      ava::tools::ReadOptions{.max_bytes = kPromptReferenceMaxBytes, .offset_line = 1, .max_lines = kPromptReferenceMaxLines});
    if (!read)
    {
      auto error = read.error();
      error.with_context("file_reference", reference.path);
      return std::unexpected(std::move(error));
    }
    expanded += "\n\n--- ";
    expanded += reference.path;
    expanded += " ---\n";
    expanded += read->content;
    if (read->truncated)
    {
      expanded += "\n[reference truncated";
      if (read->next_offset_line > 0)
        expanded += "; next offset " + std::to_string(read->next_offset_line);
      if (read->byte_limited)
        expanded += "; byte cap reached";
      if (read->line_limited)
        expanded += "; line cap reached";
      expanded += "]";
    }
  }
  return expanded;
}

}  // namespace ava::app

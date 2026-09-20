#pragma once

#include "ava/debug/print_members_on.h"

#include <array>
#include <optional>
#include <span>
#include <string_view>

namespace ava::agent {

struct ToolMetadata
{
  std::string_view name;
  std::string_view description;
  std::string_view schema_json;
  std::string_view permission_category;
  std::string_view output_bound_summary;
  std::string_view execution_mode;
  std::string_view event_rendering_hint;
  // Reserved for future provider/model-specific description variants without changing the tool registry shape.
  std::optional<std::string_view> description_family;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

inline constexpr std::array<ToolMetadata, 20> kBuiltinToolMetadata{{
    ToolMetadata{
        .name = "read_file",
        .description = "Read a text file through AVA permission checks. Use offset and limit to continue "
                       "large files by line.",
        .schema_json =
            R"({"type":"function","name":"read_file","description":"Read a text file through AVA permission checks. Prefer this over shell commands for inspecting file contents. Use offset and limit to continue large files by line; use max_bytes only as a safety cap for unusually long lines or large ranges.","parameters":{"type":"object","properties":{"path":{"type":"string","description":"Workspace-relative path, or an absolute path that will require permission if outside the workspace."},"max_bytes":{"type":"integer","minimum":1,"maximum":524288,"description":"Safety cap for returned bytes. Defaults to 51200 and is capped at 524288."},"offset":{"type":"integer","minimum":1,"description":"1-based line number to start reading from. Defaults to 1."},"limit":{"type":"integer","minimum":1,"maximum":100000,"description":"Maximum number of lines to return from offset. Defaults to 200."}},"required":["path"]}})",
        .permission_category = "read",
        .output_bound_summary = "File content is bounded by a default 200-line window, max_bytes safety cap, and tool-level caps.",
        .execution_mode = "synchronous",
        .event_rendering_hint = "file_read",
        .description_family = std::string_view("filesystem")},
    ToolMetadata{
        .name = "list_directory",
        .description = "List readable entries in one directory through AVA permission checks.",
        .schema_json =
            R"({"type":"function","name":"list_directory","description":"List readable files and subdirectories in one directory. Use this to orient before glob/grep or edits; it returns names, type, and size only.","parameters":{"type":"object","properties":{"path":{"type":"string","description":"Directory to list. Defaults to the workspace root."},"max_entries":{"type":"integer","minimum":1,"maximum":5000,"description":"Maximum entries returned. Defaults to 500 and is capped at 5000."}}}})",
        .permission_category = "search",
        .output_bound_summary = "Directory entries are bounded by max_entries and omit paths denied by read policy.",
        .execution_mode = "synchronous",
        .event_rendering_hint = "file_search",
        .description_family = std::string_view("search")},
    ToolMetadata{
        .name = "write_file",
        .description = "Write a full file through AVA permission checks. Use for new files or intentional "
                       "full rewrites.",
        .schema_json =
            R"({"type":"function","name":"write_file","description":"Write a full file through AVA permission checks. Use for new files or intentional full rewrites; use edit_file or apply_patch for small changes to existing files. Denied for source files in plan mode.","parameters":{"type":"object","properties":{"path":{"type":"string"},"content":{"type":"string"}},"required":["path","content"]}})",
        .permission_category = "edit",
        .output_bound_summary = "Returns write status and byte count only.",
        .execution_mode = "synchronous",
        .event_rendering_hint = "file_write",
        .description_family = std::string_view("filesystem")},
    ToolMetadata{
        .name = "edit_file",
        .description = "Replace one exact unique text span in a file through AVA permission checks.",
        .schema_json =
            R"({"type":"function","name":"edit_file","description":"Replace one exact unique text span in a file through AVA permission checks. Keep old_text small but uniquely identifiable, preserve exact whitespace and line endings, and use apply_patch for multiple replacements.","parameters":{"type":"object","properties":{"path":{"type":"string"},"old_text":{"type":"string","minLength":1,"description":"Exact text to replace; it must occur exactly once."},"new_text":{"type":"string","description":"Replacement text."}},"required":["path","old_text","new_text"]}})",
        .permission_category = "edit",
        .output_bound_summary = "Returns edit status and byte count only.",
        .execution_mode = "synchronous",
        .event_rendering_hint = "file_edit",
        .description_family = std::string_view("filesystem")},
    ToolMetadata{
        .name = "glob",
        .description = "Find readable non-symlink workspace files by glob pattern.",
        .schema_json =
            R"({"type":"function","name":"glob","description":"Find readable non-symlink workspace files by glob pattern. Use this for file discovery when you know filename shapes; use grep for content search and list_directory for one directory.","parameters":{"type":"object","properties":{"pattern":{"type":"string","description":"Glob pattern such as **/*.cpp or src/**/*.h. Bracket character classes are not supported."},"max_results":{"type":"integer","minimum":1,"maximum":10000,"description":"Maximum paths returned. Defaults to 2000 and is capped at 10000."}},"required":["pattern"]}})",
        .permission_category = "search",
        .output_bound_summary = "Matched paths are bounded by max_results and tool-level caps.",
        .execution_mode = "synchronous",
        .event_rendering_hint = "file_search",
        .description_family = std::string_view("search")},
    ToolMetadata{
        .name = "grep",
        .description = "Search readable non-symlink workspace files for text or a regular expression.",
        .schema_json =
            R"({"type":"function","name":"grep","description":"Search readable non-symlink workspace files for text. Defaults to literal, case-sensitive matching; set literal=false for ECMAScript regex and case_insensitive=true when casing is uncertain. Use read_file on matches for surrounding context.","parameters":{"type":"object","properties":{"pattern":{"type":"string","description":"Literal text by default, or an ECMAScript regex when literal is false."},"include":{"type":"string","description":"Glob limiting searched files. Defaults to **/* and skips symlinked files."},"max_matches":{"type":"integer","minimum":1,"maximum":10000,"description":"Maximum matched lines returned. Defaults to 2000 and is capped at 10000."},"literal":{"type":"boolean","description":"When true, pattern is matched literally. Defaults to true."},"case_insensitive":{"type":"boolean","description":"When true, matching ignores ASCII case. Defaults to false."}},"required":["pattern"]}})",
        .permission_category = "search",
        .output_bound_summary = "Matched lines are bounded by max_matches and line truncation caps.",
        .execution_mode = "synchronous",
        .event_rendering_hint = "file_search",
        .description_family = std::string_view("search")},
    ToolMetadata{
        .name = "bash",
        .description = "Run a permissioned local argv-style command for builds, tests, and verification.",
        .schema_json =
            R"({"type":"function","name":"bash","description":"Run a permissioned local argv-style command for builds, tests, and verification. AVA does not run a shell here: avoid pipes, redirects, variables, subshells, and other shell metacharacters. Prefer read_file, list_directory, glob, and grep for inspection.","parameters":{"type":"object","properties":{"command":{"type":"string","description":"Command plus arguments, separated by spaces, with no shell metacharacters."},"timeout_ms":{"type":"integer","minimum":1,"maximum":120000,"description":"Timeout in milliseconds. Defaults to 30000 and is capped at 120000."},"max_lines":{"type":"integer","minimum":1,"maximum":100000,"description":"Maximum trailing output lines retained. Defaults to 200."},"limit":{"type":"integer","minimum":1,"maximum":100000,"description":"Alias for max_lines."},"max_bytes":{"type":"integer","minimum":1,"maximum":524288,"description":"Safety cap for retained output bytes. Defaults to 51200 and is capped at 524288."}},"required":["command"]}})",
        .permission_category = "bash",
        .output_bound_summary = "Command output retains a default 200-line tail plus a max_bytes safety cap.",
        .execution_mode = "synchronous_process",
        .event_rendering_hint = "command",
        .description_family = std::string_view("process")},
    ToolMetadata{.name = "webfetch",
                 .description = "Fetch bounded content from a known http or https URL after network permission approval.",
                 .schema_json =
                     R"({"type":"function","name":"webfetch","description":"Fetch bounded content from a known http or https URL after network permission approval. Use websearch first when you need to discover relevant URLs. Defaults to markdown output; use text or html when exact source format matters. Use offset and limit to continue long fetched content by line.","parameters":{"type":"object","properties":{"url":{"type":"string","description":"Fully formed http or https URL to fetch."},"format":{"type":"string","enum":["markdown","text","html"],"description":"Return format. Defaults to markdown."},"offset":{"type":"integer","minimum":1,"description":"1-based line number to start returning after conversion. Defaults to 1."},"limit":{"type":"integer","minimum":1,"maximum":100000,"description":"Maximum converted content lines to return. Defaults to 200."},"max_bytes":{"type":"integer","minimum":1,"maximum":5242880,"description":"Safety cap for returned bytes. Defaults to 1048576 and is capped at 5242880."},"timeout_ms":{"type":"integer","minimum":1000,"maximum":120000,"description":"Defaults to 30000 and is clamped to 1000-120000."}},"required":["url"]}})",
                 .permission_category = "network.fetch",
                 .output_bound_summary = "Response content is bounded by a default 200-line window and max_bytes safety cap.",
                 .execution_mode = "synchronous_network",
                 .event_rendering_hint = "network_fetch",
                 .description_family = std::string_view("network")},
    ToolMetadata{
        .name = "websearch",
        .description = "Search the web for current sources after network permission approval.",
        .schema_json =
            R"({"type":"function","name":"websearch","description":"Search the web for current sources after network permission approval. Use this for discovery when you do not already know the URL, then use webfetch on specific results for deeper reading.","parameters":{"type":"object","properties":{"query":{"type":"string","description":"Search query. Include the current year when looking for recent information."},"num_results":{"type":"integer","minimum":1,"maximum":10,"description":"Maximum results returned. Defaults to 8 and is capped at 10."},"context_max_chars":{"type":"integer","minimum":1,"maximum":30000,"description":"Maximum result text characters retained. Defaults to 10000 and is capped at 30000. The camelCase alias contextMaxCharacters is also accepted."},"timeout_ms":{"type":"integer","minimum":1000,"maximum":60000,"description":"Defaults to 25000 and is clamped to 1000-60000."}},"required":["query"]}})",
        .permission_category = "network.search",
        .output_bound_summary = "Search results are bounded by num_results and context_max_chars.",
        .execution_mode = "synchronous_network",
        .event_rendering_hint = "network_search",
        .description_family = std::string_view("network")},
    ToolMetadata{
        .name = "skill",
        .description = "Load a listed local or global SKILL.md instruction file into the conversation.",
        .schema_json =
            R"({"type":"function","name":"skill","description":"Load a listed local or global SKILL.md instruction file into the conversation. Use this when the task matches a skill shown in the available_skills system prompt section.","parameters":{"type":"object","properties":{"name":{"type":"string","description":"Skill name from available_skills."}},"required":["name"]}})",
        .permission_category = "skill",
        .output_bound_summary = "Skill content is bounded by the skill loader file-size cap and includes a sampled file list.",
        .execution_mode = "synchronous",
        .event_rendering_hint = "skill",
        .description_family = std::string_view("skills")},
    ToolMetadata{.name = "task",
                 .description = "Run a foreground or background subagent task in a child session. Use a subagent_type listed in available_subagents.",
                 .schema_json =
                     R"({"type":"function","name":"task","description":"Run a foreground or background subagent task in a child session. Use for complex codebase exploration or delegated implementation work. subagent_type must match a subagent listed in the available_subagents system prompt section. Prefer mode=foreground or mode=background; legacy background remains accepted.","parameters":{"type":"object","additionalProperties":false,"properties":{"description":{"type":"string","description":"Short 3-5 word task label."},"prompt":{"type":"string","description":"Complete instructions for the subagent. Include exactly what it should return."},"subagent_type":{"type":"string","description":"Subagent type to run, from available_subagents."},"task_id":{"type":"string","description":"Existing owned child session id to continue in foreground or background mode."},"command":{"type":"string","description":"Optional command or user action that triggered this task."},"max_tool_iterations":{"type":"integer","minimum":1,"maximum":1000,"description":"Tool-use round limit for this invocation. Overrides the selected agent definition."},"mode":{"type":"string","enum":["foreground","background"],"description":"Preferred execution mode."},"background":{"type":"boolean","description":"Legacy mode flag. When supplied with mode it must agree."}},"required":["description","prompt","subagent_type"]}})",
                 .permission_category = "task",
                 .output_bound_summary = "Subagent result text is bounded by the nested agent loop output limits.",
                 .execution_mode = "subagent",
                 .event_rendering_hint = "task",
                 .description_family = std::string_view("subagent")},
    ToolMetadata{
        .name = "job",
        .description = "List and control subagent jobs owned by the current parent session.",
        .schema_json =
            R"({"type":"function","name":"job","description":"List, inspect, wait for, retrieve, cancel, or steer subagent jobs owned by this parent session. result is available only after terminal completion. steer delivers a message at the child's next safe provider boundary.","parameters":{"type":"object","additionalProperties":false,"properties":{"action":{"type":"string","enum":["list","status","wait","result","cancel","steer"]},"job_id":{"type":"string","maxLength":96},"message":{"type":"string","maxLength":16384,"description":"Required only for steer."},"timeout_ms":{"type":"integer","minimum":1,"maximum":30000,"description":"Finite wait timeout. Defaults to 1000ms and is clamped to 30000ms."}},"required":["action"]}})",
        .permission_category = "job",
        .output_bound_summary = "Snapshots are schema-versioned, content-redacted, and bounded to 64 list entries and 16 KiB terminal summaries.",
        .execution_mode = "synchronous",
        .event_rendering_hint = "job",
        .description_family = std::string_view("subagent")},
    ToolMetadata{
        .name = "lsp_diagnostics",
        .description = "Query configured local language-server diagnostics for one workspace file.",
        .schema_json =
            R"({"type":"function","name":"lsp_diagnostics","description":"Query configured local language-server diagnostics for one workspace file.","parameters":{"type":"object","properties":{"path":{"type":"string","maxLength":4096}},"required":["path"]}})",
        .permission_category = "lsp.query",
        .output_bound_summary = "Returns structured diagnostics only; local server configuration stays hidden.",
        .execution_mode = "synchronous_process",
        .event_rendering_hint = "lsp_diagnostics",
        .description_family = std::string_view("lsp")},
    ToolMetadata{
        .name = "lsp_document_symbols",
        .description = "Query configured local language-server symbols for one workspace file.",
        .schema_json =
            R"({"type":"function","name":"lsp_document_symbols","description":"Query configured local language-server document symbols for one workspace file. Returns bounded symbol names, kinds, ranges, and containers; local server details stay hidden.","parameters":{"type":"object","properties":{"path":{"type":"string","maxLength":4096}},"required":["path"]}})",
        .permission_category = "lsp.query",
        .output_bound_summary = "Returns bounded structured document symbols only; local server configuration stays hidden.",
        .execution_mode = "synchronous_process",
        .event_rendering_hint = "lsp_symbols",
        .description_family = std::string_view("lsp")},
    ToolMetadata{
        .name = "lsp_workspace_symbols",
        .description = "Query configured local language-server workspace symbols by name.",
        .schema_json =
            R"({"type":"function","name":"lsp_workspace_symbols","description":"Query configured local language-server workspace symbols by name. Use a short query string; results are bounded and normalized to workspace-relative paths when possible.","parameters":{"type":"object","properties":{"query":{"type":"string","maxLength":1024}},"required":["query"]}})",
        .permission_category = "lsp.query",
        .output_bound_summary = "Returns bounded structured workspace symbols only; local server configuration stays hidden.",
        .execution_mode = "synchronous_process",
        .event_rendering_hint = "lsp_symbols",
        .description_family = std::string_view("lsp")},
    ToolMetadata{
        .name = "lsp_definition",
        .description = "Query configured local language-server definitions for one file position.",
        .schema_json =
            R"({"type":"function","name":"lsp_definition","description":"Query configured local language-server definitions for one workspace file position. Lines and columns are zero-based, matching LSP positions.","parameters":{"type":"object","properties":{"path":{"type":"string","maxLength":4096},"line":{"type":"integer","minimum":0},"column":{"type":"integer","minimum":0}},"required":["path","line","column"]}})",
        .permission_category = "lsp.query",
        .output_bound_summary = "Returns bounded definition locations only; local server configuration stays hidden.",
        .execution_mode = "synchronous_process",
        .event_rendering_hint = "lsp_definition",
        .description_family = std::string_view("lsp")},
    ToolMetadata{
        .name = "lsp_references",
        .description = "Query configured local language-server references for one file position.",
        .schema_json =
            R"({"type":"function","name":"lsp_references","description":"Query configured local language-server references for one workspace file position. Lines and columns are zero-based, matching LSP positions. Results are bounded and normalized to workspace-relative paths when possible.","parameters":{"type":"object","properties":{"path":{"type":"string","maxLength":4096},"line":{"type":"integer","minimum":0},"column":{"type":"integer","minimum":0}},"required":["path","line","column"]}})",
        .permission_category = "lsp.query",
        .output_bound_summary = "Returns bounded reference locations only; local server configuration stays hidden.",
        .execution_mode = "synchronous_process",
        .event_rendering_hint = "lsp_references",
        .description_family = std::string_view("lsp")},
    ToolMetadata{
        .name = "apply_patch",
        .description = "Apply up to 32 exact text replacements across files through AVA permission checks.",
        .schema_json =
            R"({"type":"function","name":"apply_patch","description":"Apply up to 32 exact text replacements across files through AVA permission checks. Use this for coordinated edits; each old_text must exist exactly once in its file and edits are validated before writes are committed.","parameters":{"type":"object","properties":{"edits":{"type":"array","minItems":1,"maxItems":32,"items":{"type":"object","properties":{"path":{"type":"string"},"old_text":{"type":"string","minLength":1,"description":"Exact text to replace; it must occur exactly once in the file."},"new_text":{"type":"string"}},"required":["path","old_text","new_text"]}}},"required":["edits"]}})",
        .permission_category = "edit",
        .output_bound_summary = "Applies at most 32 edits and returns per-file byte counts.",
        .execution_mode = "synchronous",
        .event_rendering_hint = "file_edit",
        .description_family = std::string_view("filesystem")},
    ToolMetadata{
        .name = "question",
        .description = "Ask the user a clarification question through AVA's backend question resolver.",
        .schema_json =
            R"({"type":"function","name":"question","description":"Ask the user a clarification question through AVA's backend question resolver.","parameters":{"type":"object","properties":{"header":{"type":"string"},"question":{"type":"string"},"options":{"type":"array","items":{"oneOf":[{"type":"string"},{"type":"object","properties":{"value":{"type":"string"},"label":{"type":"string"}}}]}},"multiple":{"type":"boolean"},"allow_multiple":{"type":"boolean"},"custom":{"type":"boolean"},"allow_custom":{"type":"boolean"}},"required":["question"]}})",
        .permission_category = "user",
        .output_bound_summary = "Returns the bounded user selection or custom answer text.",
        .execution_mode = "synchronous_user_interaction",
        .event_rendering_hint = "question",
        .description_family = std::string_view("interaction")},
    ToolMetadata{
        .name = "todowrite",
        .description =
            "Replace the conversation todo list with a full snapshot. Use for nontrivial multi-step work; keep one item in_progress while actively "
            "working; mark completed only after completion/verification; pass an empty list to clear.",
        .schema_json =
            R"({"type":"function","name":"todowrite","description":"Replace the conversation todo list with a full snapshot. Use for nontrivial multi-step work. Maintain exactly one in_progress item while actively working when practical; mark completed only after completion and verification; pass an empty todos array to clear the list.","parameters":{"type":"object","additionalProperties":false,"properties":{"todos":{"type":"array","maxItems":50,"items":{"type":"object","additionalProperties":false,"properties":{"id":{"type":"string","minLength":1,"maxLength":32,"description":"Stable ASCII semantic id matching [A-Za-z0-9_-]."},"content":{"type":"string","minLength":1,"maxLength":512,"description":"Human-readable todo text."},"status":{"type":"string","enum":["pending","in_progress","completed"]}},"required":["id","content","status"]}}},"required":["todos"]}})",
        .permission_category = "user",
        .output_bound_summary = "Returns a schema-versioned full-list snapshot with counts; at most 50 items.",
        .execution_mode = "synchronous",
        .event_rendering_hint = "todo",
        .description_family = std::string_view("interaction")},
}};

[[nodiscard]] inline constexpr std::span<ToolMetadata const> builtin_tool_metadata() noexcept
{
  return std::span<ToolMetadata const>(kBuiltinToolMetadata.data(), kBuiltinToolMetadata.size());
}

}  // namespace ava::agent

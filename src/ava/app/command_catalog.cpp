#include "sys.h"
#include "ava/app/command_catalog.h"

#include <algorithm>
#include <string>
#include <vector>

namespace ava::app {
namespace {

bool token_matches(std::string_view token, CommandCatalogEntry const& entry) noexcept
{
  if (token == entry.command)
    return true;
  return std::ranges::find(entry.aliases, token) != entry.aliases.end();
}

std::string_view command_token(std::string_view line) noexcept
{
  auto const end = line.find_first_of(" \t\r\n");
  return line.substr(0, end == std::string_view::npos ? line.size() : end);
}

}  // namespace

std::vector<CommandCatalogEntry> const& command_catalog()
{
  static auto const catalog = std::vector<CommandCatalogEntry>{
      CommandCatalogEntry{.command = "/help", .description = "Show commands and effective hotkeys", .category = "General"},
      CommandCatalogEntry{.command = "/hotkeys",
                          .aliases = {"/keybindings"},
                          .description = "Show, initialize, import, edit, reset, or validate effective TUI keybindings",
                          .category = "General"},
      CommandCatalogEntry{.command = "/settings", .description = "Open TUI settings", .category = "General"},
      CommandCatalogEntry{.command = "/sidebar", .description = "Open the current session overview", .category = "General"},
      CommandCatalogEntry{.command = "/overview", .description = "Toggle the path-free startup resources overview", .category = "General"},
      CommandCatalogEntry{
          .command = "/stash", .description = "Open, restore, or clear process-memory prompt drafts", .hint = "[pop|clear]", .category = "General"},
      CommandCatalogEntry{
          .command = "/search", .description = "Find literal text in currently rendered TUI transcript items", .hint = "[query]", .category = "General"},
      CommandCatalogEntry{
          .command = "/theme", .description = "Persist the TUI display theme", .hint = "[dark|light|plain|custom-name|reset]", .category = "General"},
      CommandCatalogEntry{.command = "/images", .description = "Persist TUI image preview visibility", .hint = "[on|off|reset]", .category = "General"},
      CommandCatalogEntry{
          .command = "/image-width", .description = "Persist TUI image preview width in cells", .hint = "<8..160>|reset", .category = "General"},
      CommandCatalogEntry{.command = "/cursor",
                          .description = "Persist TUI cursor style and blink",
                          .hint = "default|block|underline|bar [blink|steady]",
                          .category = "General"},
      CommandCatalogEntry{.command = "/mode", .description = "Toggle build/plan mode", .category = "General"},
      CommandCatalogEntry{.command = "/details",
                          .description = "Set or toggle Compact, Rich, or Expanded tool cards",
                          .hint = "[compact|rich|expanded]",
                          .category = "General"},
      CommandCatalogEntry{.command = "/tool",
                          .aliases = {"/tools"},
                          .description = "Show the latest or matching tool details in the TUI",
                          .hint = "[query]",
                          .category = "General"},
      CommandCatalogEntry{.command = "/diff", .description = "Show the latest or matching tool diff in the TUI", .hint = "[query]", .category = "General"},
      CommandCatalogEntry{.command = "/copy",
                          .description = "Copy the latest AVA message, a public user turn, or matching tool, diff, or permission details in the TUI",
                          .category = "General"},
      CommandCatalogEntry{.command = "/share",
                          .description = "Share a session export",
                          .category = "Deferred",
                          .enabled = false,
                          .disabled_reason = "cloud sharing is deferred; use /export html <path> for a local sanitized export"},
      CommandCatalogEntry{.command = "/changelog",
                          .description = "Show release notes",
                          .category = "Deferred",
                          .enabled = false,
                          .disabled_reason = "interactive changelog is deferred; see README.md and docs/product/mvp-baseline.md"},
      CommandCatalogEntry{.command = "/packages",
                          .aliases = {"/package"},
                          .description = "Install or manage resource packages",
                          .hint = "<list|install|remove|update|config>",
                          .category = "Deferred",
                          .enabled = false,
                          .disabled_reason = "package install/update is deferred pending local-source, provenance, trust, rollback, and compatibility policy"},
      CommandCatalogEntry{.command = "/thinking",
                          .description = "Toggle inline thinking visibility, or use /thinking details for the latest long thinking block",
                          .category = "General"},
      CommandCatalogEntry{
          .command = "/attach", .aliases = {"/image"}, .description = "Attach an image to the next TUI prompt", .hint = "<path>", .category = "Files"},
      CommandCatalogEntry{
          .command = "/connect", .aliases = {"/login"}, .description = "Store provider credentials or start OpenAI OAuth", .category = "General"},
      CommandCatalogEntry{.command = "/quit", .aliases = {"/exit"}, .description = "Exit", .category = "General"},
      CommandCatalogEntry{
          .command = "/sessions",
          .aliases = {"/tree"},
          .description = "Show, rename, label, archive, or unarchive sessions in this workspace",
          .hint = "[--archived] [query|id] | rename <id> <name|--clear> | labels <id> <label...|--clear> | archive <id> --confirm | unarchive <id>",
          .category = "Sessions"},
      CommandCatalogEntry{.command = "/jobs",
                          .description = "List, inspect, wait for, retrieve, cancel, or promote subagent jobs",
                          .hint = "[list|history|show|wait|result|cancel|promote] <id> [timeout_ms]",
                          .category = "Sessions"},
      CommandCatalogEntry{.command = "/fork", .description = "Fork the current session at its latest entry", .hint = "[name]", .category = "Sessions"},
      CommandCatalogEntry{.command = "/fork-from", .description = "Fork from a selected public user turn in the TUI", .category = "Sessions"},
      CommandCatalogEntry{.command = "/clone", .description = "Clone the full current session", .hint = "[name]", .category = "Sessions"},
      CommandCatalogEntry{.command = "/new", .aliases = {"/clear"}, .description = "Start a new session", .hint = "[name]", .category = "Sessions"},
      CommandCatalogEntry{.command = "/resume", .description = "Resume a session", .hint = "<id>", .category = "Sessions"},
      CommandCatalogEntry{
          .command = "/name", .aliases = {"/rename"}, .description = "Set or clear the current session name", .hint = "<name|--clear>", .category = "Sessions"},
      CommandCatalogEntry{.command = "/labels",
                          .aliases = {"/label"},
                          .description = "Set or clear current session labels",
                          .hint = "<label...|--clear>",
                          .category = "Sessions"},
      CommandCatalogEntry{
          .command = "/context", .description = "List prompt, context, command, skill, and plugin freshness", .hint = "[query|source]", .category = "Sessions"},
      CommandCatalogEntry{.command = "/stats", .aliases = {"/session"}, .description = "Show session counts, usage, and cost", .category = "Sessions"},
      CommandCatalogEntry{.command = "/status", .description = "Alias for /stats session status", .category = "Sessions"},
      CommandCatalogEntry{.command = "/compact", .description = "Generate and record a provider summary", .hint = "[instructions]", .category = "Sessions"},
      CommandCatalogEntry{.command = "/export",
                          .description = "Export this session as Markdown, HTML, or sanitized portable JSONL",
                          .hint = "[markdown|html|jsonl] [path]",
                          .category = "Sessions"},
      CommandCatalogEntry{.command = "/permissions",
                          .aliases = {"/permission-rules", "/perms"},
                          .description = "List rules and audit permission decisions",
                          .hint = "<list|audit|diagnose|explain|add|remove> ...",
                          .category = "Safety"},
      CommandCatalogEntry{.command = "/glob", .description = "List files matching a glob pattern", .hint = "<pattern>", .category = "Files"},
      CommandCatalogEntry{.command = "/find", .description = "Pi-style alias for /glob", .hint = "<pattern>", .category = "Files"},
      CommandCatalogEntry{.command = "/ls", .description = "List directory entries through the permissioned list tool", .hint = "[path]", .category = "Files"},
      CommandCatalogEntry{.command = "/grep", .description = "Search matching files for literal text", .hint = "<text> [glob]", .category = "Files"},
      CommandCatalogEntry{.command = "/read", .description = "Read a file through the permissioned read tool", .hint = "<path>", .category = "Files"},
      CommandCatalogEntry{.command = "/write", .description = "Write text through the permissioned write tool", .hint = "<path> <txt>", .category = "Files"},
      CommandCatalogEntry{.command = "/bash", .description = "Run a permissioned shell command", .hint = "<command>", .category = "Shell"},
      CommandCatalogEntry{.command = "/models",
                          .aliases = {"/model"},
                          .description = "Open the model selector or filter configured models",
                          .hint = "[query]",
                          .category = "Models"},
      CommandCatalogEntry{.command = "/providers", .description = "List provider capability and credential status", .hint = "[query]", .category = "Models"},
      CommandCatalogEntry{.command = "/scoped-models", .description = "Enable, disable, order, and save models for Ctrl+P cycling", .category = "Models"},
      CommandCatalogEntry{.command = "/plugins",
                          .description = "List, install, remove, inspect, enable, disable, and validate plugins",
                          .hint = "<list|install|remove|inspect|enable|disable|validate|failures|prompts|prompt|skills|skill> ...",
                          .category = "Plugins"},
      CommandCatalogEntry{
          .command = "/trust", .description = "Inspect or save project resource trust", .hint = "[status|project|deny|clear]", .category = "Plugins"},
      CommandCatalogEntry{.command = "/mcp",
                          .description = "List, inspect, discover, and restart MCP servers",
                          .hint = "<list|inspect|tools|restart> ...",
                          .category = "Plugins"},
      CommandCatalogEntry{
          .command = "/plugin", .description = "Run an enabled plugin command", .hint = "run <plugin_id> <command> [arguments_json]", .category = "Plugins"},
      CommandCatalogEntry{.command = "/import",
                          .description = "Import an AVA JSONL session archive into a new local session",
                          .hint = "<path.jsonl> --confirm",
                          .category = "Sessions"},
      CommandCatalogEntry{.command = "/reload",
                          .description = "Reload config domains; /reload all also refreshes workspace path completions",
                          .hint = "[all|theme|models|prompts|trust|compaction|keybindings|auth|permissions|lsp|mcp|plugins]",
                          .category = "General"},
      CommandCatalogEntry{.command = "/logout",
                          .description = "Clear provider login",
                          .category = "Deferred",
                          .enabled = false,
                          .disabled_reason = "provider logout is deferred; remove only the provider entry from auth.json or rerun /connect to replace it"},
  };
  return catalog;
}

CommandCatalogEntry const* find_command_catalog_entry(std::string_view line) noexcept
{
  if (!line.starts_with('/'))
    return nullptr;
  auto const token = command_token(line);
  for (auto const& entry : command_catalog())
  {
    if (token_matches(token, entry))
      return &entry;
  }
  return nullptr;
}

std::string normalize_command_line(std::string_view line, CommandCatalogEntry const& entry)
{
  auto const token = command_token(line);
  if (token == entry.command)
    return std::string(line);
  auto const rest = line.substr(token.size());
  return entry.command + std::string(rest);
}

}  // namespace ava::app

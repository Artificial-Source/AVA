#include "sys.h"
#include "ava/command/environment.h"
#include "ava/permissions/permission.h"
#include "ava/core/NoLogString.h"
#include "ava/core/ascii.h"
#include "ava/core/json.h"
#include "ava/core/mode.h"
#include "ava/core/open_beneath.h"
#include "ava/core/path.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <string>
#include <utility>
#include <vector>
#include <fcntl.h>
#include <unistd.h>

namespace ava::permissions {

PermissionResolutionDecision::PermissionResolutionDecision(PermissionResolution resolution_in) : resolution(resolution_in)
{
}

PermissionResolutionDecision::PermissionResolutionDecision(PermissionResolution resolution_in, std::string reason_in)
    : resolution(resolution_in), reason(std::move(reason_in))
{
}

std::optional<std::string> validated_permission_user_guidance(std::string_view value)
{
  if (value.empty() || value.size() > kMaxPermissionUserGuidanceBytes || ava::core::has_c0_or_del(value) || !ava::core::json::is_valid_utf8(value))
  {
    return std::nullopt;
  }
  return std::string(value);
}

std::string with_provider_user_guidance(std::string content, std::string_view guidance)
{
  auto validated = validated_permission_user_guidance(guidance);
  if (!validated)
    return content;

  if (ava::core::json::is_valid_object(content))
  {
    while (!content.empty() && (content.back() == ' ' || content.back() == '\t' || content.back() == '\n' || content.back() == '\r'))
      content.pop_back();
    if (content.size() >= 2 && content.back() == '}')
    {
      content.pop_back();
      while (content.size() > 1 && (content.back() == ' ' || content.back() == '\t' || content.back() == '\n' || content.back() == '\r'))
        content.pop_back();
      bool const empty_object = content.size() == 1;  // only remaining '{'
      if (!empty_object)
        content.push_back(',');
      content += "\"provider_user_guidance\":\"" + ava::core::json::escape(*validated) + "\"}";
      return content;
    }
  }

  if (!content.empty() && content.back() != '\n')
    content.push_back('\n');
  content += "[ava:provider_user_guidance] ";
  content += *validated;
  return content;
}

bool operator==(PermissionResolutionDecision const& decision, PermissionResolution resolution)
{
  return decision.resolution == resolution;
}

bool operator==(PermissionResolution resolution, PermissionResolutionDecision const& decision)
{
  return decision == resolution;
}

std::optional<PermissionAction> parse_permission_action(std::string_view value)
{
  if (value == "allow")
    return PermissionAction::Allow;
  if (value == "ask")
    return PermissionAction::Ask;
  if (value == "deny")
    return PermissionAction::Deny;
  return std::nullopt;
}

std::optional<Operation> parse_operation(std::string_view value)
{
  if (value == "read")
    return Operation::ReadFile;
  if (value == "search")
    return Operation::SearchFiles;
  if (value == "edit")
    return Operation::EditFile;
  if (value == "bash")
    return Operation::RunCommand;
  if (value == "network.fetch")
    return Operation::NetworkFetch;
  if (value == "network.search")
    return Operation::NetworkSearch;
  if (value == "lsp.server.launch")
    return Operation::LspServerLaunch;
  if (value == "lsp.query")
    return Operation::LspQuery;
  if (value == "skill")
    return Operation::SkillLoad;
  if (value == "task")
    return Operation::TaskRun;
  if (value == "plugin.execute")
    return Operation::PluginExecute;
  if (value == "plugin.tool.call")
    return Operation::PluginToolCall;
  if (value == "plugin.command.run")
    return Operation::PluginCommandRun;
  if (value == "plugin.ui.present")
    return Operation::PluginUiPresent;
  if (value == "plugin.event.observe")
    return Operation::PluginEventObserve;
  if (value == "mcp.server.launch")
    return Operation::McpServerLaunch;
  if (value == "mcp.server.connect")
    return Operation::McpServerConnect;
  if (value == "mcp.tool.call")
    return Operation::McpToolCall;
  if (value == "mcp.resource.read")
    return Operation::McpResourceRead;
  return std::nullopt;
}

namespace {

struct ParsedCommand
{
  bool ok = false;
  ava::core::NoLogString reason;
  std::vector<ava::core::NoLogString> argv;
};

std::string lowercase(std::string_view value)
{
  std::string result(value);
  std::ranges::transform(result, result.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
  return result;
}

bool contains_any(std::string_view value, std::vector<std::string_view> const& needles)
{
  return std::ranges::any_of(needles, [value](std::string_view needle) { return value.find(needle) != std::string_view::npos; });
}

bool equals_any(std::string_view value, std::vector<std::string_view> const& candidates)
{
  return std::ranges::any_of(candidates, [value](std::string_view candidate) { return value == candidate; });
}

bool is_shell_metacharacter(char ch)
{
  switch (ch)
  {
    case ';':
    case '&':
    case '|':
    case '<':
    case '>':
    case '`':
    case '$':
    case '(':
    case ')':
      return true;
    default:
      return false;
  }
}

ParsedCommand parse_command_argv(std::string_view command)
{
  ParsedCommand parsed;
  std::string current;
  char quote = '\0';
  bool escaping = false;

  for (char const ch : command)
  {
    auto const byte = static_cast<unsigned char>(ch);
    if (byte < 0x20 || byte == 0x7F)
    {
      parsed.reason = "command contains a forbidden control byte";
      return parsed;
    }
    if (escaping)
    {
      current.push_back(ch);
      escaping = false;
      continue;
    }
    if (ch == '\\')
    {
      escaping = true;
      continue;
    }
    if (quote != '\0')
    {
      if (ch == quote)
      {
        quote = '\0';
      }
      else
      {
        current.push_back(ch);
      }
      continue;
    }
    if (ch == '\'' || ch == '"')
    {
      quote = ch;
      continue;
    }
    if (is_shell_metacharacter(ch))
    {
      parsed.reason = "shell metacharacters are not supported by the command tool";
      return parsed;
    }
    if (std::isspace(byte) != 0)
    {
      if (!current.empty())
      {
        parsed.argv.push_back(current);
        current.clear();
      }
      continue;
    }
    current.push_back(ch);
  }

  if (escaping || quote != '\0')
  {
    parsed.reason = "unterminated command escape or quote";
    return parsed;
  }
  if (!current.empty())
    parsed.argv.push_back(current);
  if (parsed.argv.empty())
  {
    parsed.reason = "empty command";
    return parsed;
  }
  parsed.ok = true;
  return parsed;
}

std::vector<std::string> lowercase_argv(std::vector<std::string> const& argv)
{
  std::vector<std::string> result;
  result.reserve(argv.size());
  for (auto const& arg : argv)
    result.push_back(lowercase(arg));
  return result;
}

bool is_within_workspace(std::filesystem::path const& workspace_dir, std::filesystem::path const& target_path)
{
  if (target_path.empty())
    return true;

  auto const workspace = ava::core::normalized_absolute_path(workspace_dir);
  auto const target = ava::core::normalized_absolute_path(target_path);
  if (target == workspace)
    return true;

  auto relative = target.lexically_relative(workspace);
  if (relative.empty() || *relative.begin() == "..")
    return false;

  std::error_code link_error;
  if (std::filesystem::is_symlink(std::filesystem::symlink_status(target, link_error)))
  {
    auto destination = std::filesystem::read_symlink(target, link_error);
    if (link_error)
      return false;
    if (!destination.is_absolute())
      destination = target.parent_path() / destination;
    destination = destination.lexically_normal();
    if (destination == workspace)
      return true;
    auto const destination_relative = destination.lexically_relative(workspace);
    return !destination_relative.empty() && *destination_relative.begin() != "..";
  }

  int const workspace_fd = ::open(workspace.c_str(), O_PATH | O_DIRECTORY | O_CLOEXEC);
  if (workspace_fd < 0)
    return false;

  bool within = false;
  for (;;)
  {
    int const target_fd = ava::core::open_beneath(workspace_fd, relative, O_PATH | O_CLOEXEC);
    if (target_fd >= 0)
    {
      ::close(target_fd);
      within = true;
      break;
    }
    if (errno != ENOENT && errno != ENOTDIR)
      break;
    auto parent = relative.parent_path();
    if (parent.empty() || parent == relative)
    {
      within = true;
      break;
    }
    relative = std::move(parent);
  }
  ::close(workspace_fd);
  return within;
}

std::filesystem::path policy_path(std::filesystem::path const& path)
{
  return ava::core::normalized_absolute_path(path);
}

bool is_secret_path(std::filesystem::path const& path)
{
  bool previous_is_config = false;
  for (auto const& part : path.lexically_normal())
  {
    auto const component = lowercase(part.string());
    if (equals_any(component, {".ssh", ".aws", ".gnupg"}) || (previous_is_config && component == "gcloud"))
    {
      return true;
    }
    previous_is_config = component == ".config";
  }

  auto const filename = lowercase(path.filename().string());
  if (filename == ".env" || filename.starts_with(".env."))
  {
    return filename != ".env.example";
  }
  if (equals_any(filename, {".npmrc", ".netrc", ".pypirc", ".gem/credentials", ".dockerconfigjson", "credentials.json", "auth.json", "config.json"}))
  {
    auto const full = lowercase(path.lexically_normal().string());
    return filename != "config.json" || contains_any(full, {"/.docker/", "\\.docker\\"});
  }

  auto const full = lowercase(path.string());
  return contains_any(full, {"id_rsa", "id_ed25519", "id_ecdsa", "id_dsa", "/.ssh/", "\\.ssh\\", "/.aws/", "\\.aws\\", "/.gnupg/", "\\.gnupg\\", "credentials",
                             "secret", "token"});
}

bool is_markdown_path(std::filesystem::path const& path)
{
  return lowercase(path.extension().string()) == ".md";
}

bool is_planning_markdown(std::filesystem::path const& path)
{
  auto const normalized = lowercase(path.lexically_normal().string());
  return is_markdown_path(path) &&
         (normalized.find("docs/") != std::string::npos || normalized.find("plan") != std::string::npos || normalized.find("version") != std::string::npos);
}

bool is_safe_relative_path_arg(std::string_view value)
{
  if (value.empty() || value.starts_with("-"))
    return true;
  std::filesystem::path const path(value);
  if (path.is_absolute())
    return false;
  for (auto const& part : path.lexically_normal())
  {
    if (part == "..")
      return false;
  }
  return !is_secret_path(path);
}

bool is_path_carrying_option(std::string_view value)
{
  auto const lower = lowercase(value);
  if (equals_any(lower, {"-o", "--output", "--git-dir", "--work-tree", "--exec-path", "--config-env"}))
  {
    return true;
  }
  return lower.starts_with("--output=") || lower.starts_with("--git-dir=") || lower.starts_with("--work-tree=") || lower.starts_with("--exec-path=") ||
         lower.starts_with("--config-env=");
}

bool has_unsafe_path_arg(std::vector<std::string> const& argv, std::size_t start)
{
  for (std::size_t index = start; index < argv.size(); ++index)
  {
    if (is_path_carrying_option(argv[index]))
      return true;
    if (!is_safe_relative_path_arg(argv[index]))
      return true;
  }
  return false;
}

bool is_dangerous_cmake_arg(std::string_view value)
{
  return value == "-e" || value == "-p" || value == "--install" || value == "--open";
}

bool is_simple_number(std::string_view value)
{
  if (value.empty())
    return false;
  bool saw_digit = false;
  bool saw_dot = false;
  for (char const ch : value)
  {
    if (std::isdigit(static_cast<unsigned char>(ch)) != 0)
    {
      saw_digit = true;
    }
    else if (ch == '.' && !saw_dot)
    {
      saw_dot = true;
    }
    else
    {
      return false;
    }
  }
  return saw_digit;
}

PermissionDecision decision(PermissionAction action, std::string reason, PermissionRisk risk)
{
  return PermissionDecision{.action = action, .reason = std::move(reason), .risk = risk};
}

bool path_is_within(std::filesystem::path const& root, std::filesystem::path const& candidate)
{
  auto const relative = candidate.lexically_relative(root);
  auto const text = relative.generic_string();
  return !relative.empty() && relative != ".." && !text.starts_with("../");
}

struct RecipeArgument
{
  // `kind` and `value` are independently framed hash fields. Never encode the
  // kind into a value prefix: a caller-controlled literal must not collide with
  // a normalized workspace path such as "workspace:foo".
  std::string_view kind;
  ava::core::NoLogString value;
  ava::core::NoLogString display;
};

RecipeArgument recipe_argument(std::string_view value, std::filesystem::path const& workspace)
{
  auto const path = std::filesystem::path(value);
  if (!path.is_absolute())
    return RecipeArgument{.kind = "literal", .value = std::string(value), .display = std::string(value)};

  auto const normalized = path.lexically_normal();
  auto const normalized_workspace = workspace.lexically_normal();
  if (!path_is_within(normalized_workspace, normalized))
  {
    auto text = normalized.generic_string();
    return RecipeArgument{.kind = "literal", .value = text, .display = std::move(text)};
  }
  auto relative = normalized.lexically_relative(normalized_workspace).generic_string();
  if (relative == ".")
    relative.clear();
  return RecipeArgument{.kind = "workspace_path", .value = relative, .display = "workspace:" + (relative.empty() ? "." : relative)};
}

bool is_credential_bearing_long_option(std::string_view lower)
{
  // Matches the lowercased long option name, with or without a trailing
  // =value suffix.  Covers separate (--user value) and --option=value forms.
  static constexpr std::array<std::string_view, 27> kCredentialOptions{
      // curl/wget credential-bearing long options
      "--user",
      "--proxy-user",
      "--header",
      "--proxy-header",
      "--cookie",
      "--oauth2-bearer",
      "--aws-sigv4",
      "--cert",
      "--key",
      "--pass",
      "--cacert",
      "--form-string",
      // wget-specific credential options
      "--http-user",
      "--http-password",
      "--http-header",
      "--proxy-password",
      // general credential-like option names
      "--token",
      "--secret",
      "--password",
      "--api-key",
      "--api_key",
      "--apikey",
      "--authorization",
      "--auth",
      "--credential",
      "--bearer",
      "--signature",
  };
  auto const eq = lower.find('=');
  auto const name = eq != std::string_view::npos ? lower.substr(0, eq) : lower;
  return std::ranges::find(kCredentialOptions, name) != kCredentialOptions.end();
}

bool is_credential_bearing_short_option(std::string_view arg)
{
  // Short options are case-sensitive in curl/wget.  Check every character in
  // a short-option cluster to catch separate (-u value), concatenated
  // (-uvalue), and combined (-vu) forms conservatively.
  if (arg.size() < 2 || arg[0] != '-' || arg[1] == '-')
    return false;
  for (std::size_t index = 1; index < arg.size(); ++index)
  {
    switch (arg[index])
    {
      case 'u':  // curl --user
      case 'H':  // curl --header
      case 'b':  // curl --cookie
      case 'E':  // curl --cert
      case 'U':  // curl --proxy-user
        return true;
      default:
        break;
    }
  }
  return false;
}

bool contains_secret_like_argument(std::string_view value)
{
  auto const lower = lowercase(value);
  auto const scheme = lower.find("://");
  // URL userinfo and arbitrary query strings may carry credentials under
  // application-specific names that AVA cannot enumerate safely. Keep simple
  // endpoint URLs reusable, but make every queried URL one-shot rather than
  // persisting its text in a recipe display or rule.
  if (scheme != std::string::npos && (lower.find('@', scheme + 3) != std::string::npos || lower.find('?', scheme + 3) != std::string::npos))
    return true;
  // Credential-like key=value patterns cover inline assignments and
  // non-URL option values. False positives only narrow reusable authority.
  static constexpr std::array<std::string_view, 18> kCredentialPatterns{
      "token=", "secret=",     "password=", "passwd=",    "api_key=", "apikey=", "api-key=", "access_token=", "authorization=",
      "auth=",  "credential=", "bearer=",   "signature=", "key=",     "sig=",    "cookie=",  "session=",      "sessionid=",
  };
  return std::ranges::any_of(kCredentialPatterns, [lower](std::string_view pattern) { return lower.find(pattern) != std::string::npos; });
}

bool is_payload_bearing_long_option(std::string_view lower)
{
  // curl/wget request-body, form, config, and query options whose argument is
  // an arbitrary payload that may carry secrets. Stable recipe minting must
  // refuse these conservatively (favoring false positives) rather than parsing
  // bodies. Matches the lowercased long option name with or without =value.
  static constexpr std::array<std::string_view, 15> kPayloadOptions{
      // curl request-body and form options
      "--data",
      "--data-ascii",
      "--data-binary",
      "--data-raw",
      "--data-urlencode",
      "--json",
      "--form",
      "--form-string",
      "--url-query",
      "--config",
      // wget request-body options (--header is also credential-bearing but is
      // listed here so wget header payloads are refused too)
      "--post-data",
      "--post-file",
      "--body-data",
      "--body-file",
      "--header",
  };
  auto const eq = lower.find('=');
  auto const name = eq != std::string_view::npos ? lower.substr(0, eq) : lower;
  return std::ranges::find(kPayloadOptions, name) != kPayloadOptions.end();
}

bool is_payload_bearing_short_option(std::string_view arg)
{
  // Short-option clusters for curl payload/config flags: -d (--data),
  // -F (--form), -K (--config). Check every character to catch separate
  // (-d value), concatenated (-dvalue), and combined (-vd) forms.
  if (arg.size() < 2 || arg[0] != '-' || arg[1] == '-')
    return false;
  for (std::size_t index = 1; index < arg.size(); ++index)
  {
    switch (arg[index])
    {
      case 'd':  // curl --data
      case 'F':  // curl --form
      case 'K':  // curl --config
        return true;
      default:
        break;
    }
  }
  return false;
}

bool contains_secret_like_arguments(std::vector<std::string> const& argv)
{
  return std::ranges::any_of(argv, [](std::string const& argument) {
    auto const lower = lowercase(argument);
    return is_credential_bearing_long_option(lower) || is_credential_bearing_short_option(argument) || is_payload_bearing_long_option(lower) ||
           is_payload_bearing_short_option(argument) || contains_secret_like_argument(argument);
  });
}

struct StableRecipeIdentity
{
  std::string global_key;
  std::string workspace_key;
  std::string display;

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

std::optional<StableRecipeIdentity> stable_recipe_identity(ava::command::CommandPlan const& plan, CommandContainmentInfo const& containment)
{
  static constexpr std::string_view kPayloadVersion = "ava-command-recipe-payload-v1";
  auto const& classification = plan.classification();
  if (plan.execution_domain() != ava::command::CommandExecutionDomain::DirectArgv ||
      (classification.level != ava::command::CommandLevel::Standard && classification.level != ava::command::CommandLevel::Sensitive) ||
      !plan.resolved_executable())
    return std::nullopt;

  std::string recipe_name;
  std::vector<std::string> argv;
  if (classification.recipe)
  {
    recipe_name = std::string(ava::command::to_string(classification.recipe->recipe));
    argv = classification.recipe->canonical_argv;
  }
  else
  {
    // Sensitive classifications have no policy recipe descriptor today, but
    // only a fully direct, secret-free argv is eligible for an exact typed
    // recipe key. Unknown/critical/raw plans never reach this branch.
    recipe_name = "sensitive-" + std::string(ava::command::to_string(classification.family));
    argv = plan.argv();
    if (argv.empty())
      return std::nullopt;
    argv.front() = plan.resolved_executable()->executable.canonical_path.generic_string();
  }
  if (argv.empty() || contains_secret_like_arguments(argv))
    return std::nullopt;

  std::vector<RecipeArgument> normalized_argv;
  normalized_argv.reserve(argv.size());
  for (auto const& value : argv)
    normalized_argv.push_back(recipe_argument(value, plan.workspace()));

  auto append_payload = [&](ava::command::detail::Sha256Builder& hash, bool include_workspace) {
    hash.append_field(kPayloadVersion);
    hash.append_field(recipe_name);
    hash.append_field(ava::command::to_string(classification.family));
    hash.append_field(plan.resolved_executable()->executable.canonical_path.generic_string());
    hash.append_field(ava::command::to_string(plan.resolved_executable()->origin));
    hash.append_field(plan.environment_profile_id());
    hash.append_field(containment.profile_id.empty() ? "not-required" : containment.profile_id);
    hash.append_field(containment.network_allowed ? "network-allowed" : "network-blocked");
    hash.append_number(normalized_argv.size());
    for (auto const& value : normalized_argv)
    {
      hash.append_field(value.kind);
      hash.append_field(value.value);
    }
    if (include_workspace)
      hash.append_field(plan.workspace().lexically_normal().generic_string());
  };

  ava::command::detail::Sha256Builder global_hash;
  append_payload(global_hash, false);
  ava::command::detail::Sha256Builder workspace_hash;
  append_payload(workspace_hash, true);

  std::string display = recipe_name + " (" + std::string(ava::command::to_string(classification.family)) + ")";
  if (normalized_argv.size() > 1)
  {
    display += ":";
    for (std::size_t index = 1; index < normalized_argv.size(); ++index)
    {
      // Standard recipes retain their established concise display. Sensitive
      // direct argv includes each safe normalized argument and its domain, so
      // visually similar workspace paths and literals stay distinguishable.
      auto const& argument = normalized_argv[index];
      display += " " + (classification.recipe ? argument.display : std::string(argument.kind) + "=" + argument.display);
    }
  }
  return StableRecipeIdentity{.global_key = "sha256:ava-command-recipe-v1:" + global_hash.hex(),
                              .workspace_key = "sha256:ava-command-workspace-recipe-v1:" + workspace_hash.hex(),
                              .display = std::move(display)};
}

std::vector<ava::command::InteractiveScope> effective_scopes(CommandPermissionMetadata const& metadata)
{
  std::vector<ava::command::InteractiveScope> scopes{ava::command::InteractiveScope::Once};
  if (!metadata.global_recipe_key.empty() && !metadata.workspace_recipe_key.empty() && metadata.executor_identity_verified &&
      metadata.containment_status != CommandContainmentStatus::Unavailable &&
      metadata.containment_status != CommandContainmentStatus::UnverifiedDelegatedExecutor &&
      (metadata.level == ava::command::CommandLevel::Standard || metadata.level == ava::command::CommandLevel::Sensitive))
  {
    if (metadata.backend_maximum_scope == ava::command::InteractiveScope::Session ||
        metadata.backend_maximum_scope == ava::command::InteractiveScope::Workspace)
    {
      scopes.push_back(ava::command::InteractiveScope::Session);
    }
    if (metadata.backend_maximum_scope == ava::command::InteractiveScope::Workspace)
    {
      scopes.push_back(ava::command::InteractiveScope::Workspace);
    }
  }
  return scopes;
}

PermissionRisk default_allow_risk(Operation operation)
{
  switch (operation)
  {
    case Operation::EditFile:
    case Operation::RunCommand:
    case Operation::NetworkFetch:
    case Operation::NetworkSearch:
    case Operation::LspServerLaunch:
    case Operation::SkillLoad:
    case Operation::TaskRun:
    case Operation::PluginExecute:
    case Operation::PluginToolCall:
    case Operation::PluginCommandRun:
    case Operation::PluginUiPresent:
    case Operation::PluginEventObserve:
    case Operation::McpServerLaunch:
    case Operation::McpServerConnect:
    case Operation::McpToolCall:
    case Operation::McpResourceRead:
      return PermissionRisk::Medium;
    case Operation::ReadFile:
    case Operation::SearchFiles:
    case Operation::LspQuery:
      return PermissionRisk::Low;
  }
  return PermissionRisk::Medium;
}

}  // namespace

std::vector<ava::command::InteractiveScope> command_permission_effective_scopes(CommandPermissionMetadata const& metadata)
{
  return effective_scopes(metadata);
}

PermissionDecision decide(PermissionRequest const& request)
{
  auto const checked_path = policy_path(request.target_path);

  if ((request.operation == Operation::ReadFile || request.operation == Operation::EditFile || request.operation == Operation::LspQuery) &&
      is_secret_path(checked_path))
  {
    return decision(PermissionAction::Deny, "target looks like a secret file", PermissionRisk::Critical);
  }

  if (!is_within_workspace(request.workspace_dir, request.target_path))
  {
    return decision(PermissionAction::Ask, "target is outside the workspace", PermissionRisk::High);
  }

  if (request.operation == Operation::EditFile && request.mode == ava::core::Mode::Plan && !is_planning_markdown(checked_path))
  {
    return decision(PermissionAction::Deny, "plan mode can only edit planning markdown", PermissionRisk::High);
  }

  if (request.operation == Operation::RunCommand)
  {
    if (request.command_metadata)
      return decide(*request.command_metadata);
    return classify_command(request.command);
  }

  if (request.operation == Operation::NetworkFetch)
  {
    return decision(PermissionAction::Ask, "network fetch requires explicit approval", PermissionRisk::Medium);
  }

  if (request.operation == Operation::NetworkSearch)
  {
    return decision(PermissionAction::Ask, "network search requires explicit approval", PermissionRisk::Medium);
  }

  if (request.operation == Operation::LspServerLaunch)
  {
    return decision(PermissionAction::Ask, "LSP server launch requires explicit approval", PermissionRisk::High);
  }

  if (request.operation == Operation::SkillLoad)
  {
    return decision(PermissionAction::Ask, "skill loading requires explicit approval", PermissionRisk::Medium);
  }

  if (request.operation == Operation::TaskRun)
  {
    return decision(PermissionAction::Allow, "subagent task launch is allowed and audited", PermissionRisk::Medium);
  }

  if (request.operation == Operation::PluginExecute)
  {
    return decision(PermissionAction::Ask, "plugin subprocess execution requires explicit approval", PermissionRisk::High);
  }

  if (request.operation == Operation::PluginToolCall)
  {
    return decision(PermissionAction::Ask, "plugin tool calls require explicit approval", PermissionRisk::High);
  }

  if (request.operation == Operation::PluginCommandRun)
  {
    return decision(PermissionAction::Ask, "plugin commands require explicit approval", PermissionRisk::High);
  }

  if (request.operation == Operation::PluginUiPresent)
  {
    return decision(PermissionAction::Ask, "plugin UI presentation requires explicit approval", PermissionRisk::High);
  }

  if (request.operation == Operation::PluginEventObserve)
  {
    return decision(PermissionAction::Ask, "plugin event observation requires explicit approval", PermissionRisk::Medium);
  }

  if (request.operation == Operation::McpServerLaunch)
  {
    return decision(PermissionAction::Ask, "MCP server launch requires explicit approval", PermissionRisk::High);
  }

  if (request.operation == Operation::McpServerConnect)
  {
    return decision(PermissionAction::Ask, "MCP server connection requires explicit approval", PermissionRisk::High);
  }

  if (request.operation == Operation::McpToolCall)
  {
    return decision(PermissionAction::Ask, "MCP tool calls require explicit approval", PermissionRisk::High);
  }

  if (request.operation == Operation::McpResourceRead)
  {
    return decision(PermissionAction::Ask, "MCP resource reads require explicit approval", PermissionRisk::Medium);
  }

  return decision(PermissionAction::Allow, "allowed by default workspace policy", default_allow_risk(request.operation));
}

CommandPermissionMetadata command_permission_metadata(ava::command::CommandPlan const& plan, bool unverified_delegated_executor)
{
  return command_permission_metadata(plan, CommandContainmentInfo{}, unverified_delegated_executor);
}

CommandPermissionMetadata command_permission_metadata(ava::command::CommandPlan const& plan, CommandContainmentInfo const& containment,
                                                      bool unverified_delegated_executor)
{
  auto const& classification = plan.classification();
  CommandContainmentStatus containment_status = CommandContainmentStatus::NotRequired;
  if (classification.capabilities.requires_containment || classification.capabilities.executes_mutable_project_code ||
      classification.level == ava::command::CommandLevel::Sensitive)
  {
    containment_status = containment.available ? CommandContainmentStatus::Available : CommandContainmentStatus::Unavailable;
  }
  CommandPermissionMetadata metadata{
      .level = classification.level,
      .family = classification.family,
      .fingerprint = plan.fingerprint(),
      .execution_domain = plan.execution_domain(),
      .resolved_executable = plan.resolved_executable() ? plan.resolved_executable()->executable.requested_path : std::filesystem::path{},
      .executable_origin = plan.resolved_executable() ? plan.resolved_executable()->origin : ava::command::ExecutableOrigin::System,
      .cwd = plan.cwd(),
      .executes_mutable_project_code = classification.capabilities.executes_mutable_project_code,
      .containment_available = containment.available,
      .containment_status = containment_status,
      .containment_profile_id = containment.profile_id,
      .containment_network_allowed = containment.network_allowed,
      .backend_maximum_scope = classification.max_interactive_scope,
      .recipe_payload_version = {},
      .global_recipe_key = {},
      .workspace_recipe_key = {},
      .recipe_display = {},
      .effective_allowed_scopes = {},
      .environment_profile_id = plan.environment_profile_id(),
      .environment_digest = plan.environment_digest(),
      .executor_identity_verified = !unverified_delegated_executor};
  bool const containment_required = classification.capabilities.requires_containment || classification.capabilities.executes_mutable_project_code;
  if (!unverified_delegated_executor && metadata.containment_status == CommandContainmentStatus::Unavailable)
  {
    // Any command that requires containment, including otherwise non-mutable
    // Sensitive network commands, is an explicit one-shot Critical decision
    // when the containment contract cannot be prepared.
    metadata.level = ava::command::CommandLevel::Critical;
    metadata.backend_maximum_scope = ava::command::InteractiveScope::Once;
  }
  if (unverified_delegated_executor)
  {
    metadata.level = ava::command::CommandLevel::Critical;
    metadata.family = ava::command::CommandFamily::UnverifiedDelegatedExecutor;
    metadata.containment_available = false;
    metadata.containment_status = CommandContainmentStatus::UnverifiedDelegatedExecutor;
    metadata.backend_maximum_scope = ava::command::InteractiveScope::Once;
  }

  if (metadata.executor_identity_verified && metadata.level != ava::command::CommandLevel::Critical &&
      metadata.containment_status != CommandContainmentStatus::Unavailable &&
      (!containment_required || metadata.containment_status == CommandContainmentStatus::Available ||
       metadata.containment_status == CommandContainmentStatus::NotRequired))
  {
    if (auto identity = stable_recipe_identity(plan, containment))
    {
      metadata.recipe_payload_version = "ava-command-recipe-payload-v1";
      metadata.global_recipe_key = std::move(identity->global_key);
      metadata.workspace_recipe_key = std::move(identity->workspace_key);
      metadata.recipe_display = std::move(identity->display);
    }
  }
  metadata.effective_allowed_scopes = command_permission_effective_scopes(metadata);
  return metadata;
}

PermissionDecision decide(CommandPermissionMetadata const& metadata)
{
  if (!metadata.executor_identity_verified || metadata.containment_status == CommandContainmentStatus::UnverifiedDelegatedExecutor)
  {
    return decision(PermissionAction::Ask, "delegated command executor identity and environment are unverified", PermissionRisk::Critical);
  }
  if (metadata.containment_status == CommandContainmentStatus::Unavailable)
  {
    return decision(PermissionAction::Ask,
                    "sealed command requires containment, but containment is unavailable; explicit one-shot approval will run it uncontained",
                    PermissionRisk::Critical);
  }
  if (metadata.level == ava::command::CommandLevel::Standard && !metadata.executes_mutable_project_code)
  {
    return decision(PermissionAction::Allow, "sealed command is a standard inspection recipe", PermissionRisk::Low);
  }
  // A mutable command whose required containment plan is unavailable is
  // downgraded to Critical risk. It may run uncontained only after an explicit
  // one-shot approval, with metadata/audit stating unavailable/uncontained.
  // This preserves user authority: unavailable containment must not look like
  // ordinary Sensitive/Standard containment.
  bool const needs_containment = metadata.executes_mutable_project_code || metadata.containment_status == CommandContainmentStatus::Unavailable;
  if (needs_containment && !metadata.containment_available && metadata.containment_status != CommandContainmentStatus::NotRequired)
  {
    return decision(
        PermissionAction::Ask,
        "sealed command executes mutable project code; containment is unavailable and the command will run uncontained after explicit one-shot approval",
        PermissionRisk::Critical);
  }
  if (metadata.level == ava::command::CommandLevel::Standard)
  {
    if (metadata.containment_available)
      return decision(PermissionAction::Allow, "sealed command executes mutable project code under verified development containment", PermissionRisk::Medium);
    return decision(PermissionAction::Ask, "sealed command executes mutable project code; containment is unavailable", PermissionRisk::High);
  }
  if (metadata.level == ava::command::CommandLevel::Sensitive)
  {
    return decision(PermissionAction::Ask, "sealed command has sensitive capabilities", PermissionRisk::High);
  }
  return decision(PermissionAction::Ask, "sealed command is critical", PermissionRisk::Critical);
}

bool command_permission_allows_reusable_grant(CommandPermissionMetadata const& metadata) noexcept
{
  if (!metadata.executor_identity_verified || metadata.containment_status == CommandContainmentStatus::Unavailable ||
      metadata.containment_status == CommandContainmentStatus::UnverifiedDelegatedExecutor || metadata.level == ava::command::CommandLevel::Critical ||
      metadata.backend_maximum_scope == ava::command::InteractiveScope::Once || metadata.global_recipe_key.empty() || metadata.workspace_recipe_key.empty())
    return false;
  if ((metadata.executes_mutable_project_code || metadata.containment_status == CommandContainmentStatus::Available) &&
      metadata.containment_status != CommandContainmentStatus::Available)
    return false;
  return std::ranges::find(metadata.effective_allowed_scopes, ava::command::InteractiveScope::Session) != metadata.effective_allowed_scopes.end() ||
         std::ranges::find(metadata.effective_allowed_scopes, ava::command::InteractiveScope::Workspace) != metadata.effective_allowed_scopes.end();
}

bool command_prompt_allows_persistent_allow(PermissionPrompt const& prompt) noexcept
{
  if (!prompt.command_metadata)
    return true;
  auto const& metadata = *prompt.command_metadata;
  return command_permission_allows_reusable_grant(metadata) &&
         std::ranges::find(metadata.effective_allowed_scopes, ava::command::InteractiveScope::Workspace) != metadata.effective_allowed_scopes.end();
}

bool is_repository_controlled_build_or_test_command(std::string_view command)
{
  auto const parsed = parse_command_argv(command);
  if (!parsed.ok)
    return false;

  auto const argv = lowercase_argv(parsed.argv);
  auto const executable = lowercase(std::filesystem::path(argv.front()).filename().string());
  if (executable == "ctest")
    return true;
  return executable == "cmake" && std::ranges::any_of(argv, [](std::string const& arg) {
           return arg == "--build" || arg.starts_with("--build=") || arg == "--build-and-test" || arg == "--workflow";
         });
}

PermissionDecision classify_command(std::string_view command)
{
  auto const parsed = parse_command_argv(command);
  if (!parsed.ok)
  {
    return decision(PermissionAction::Deny, parsed.reason, PermissionRisk::High);
  }
  auto const argv = lowercase_argv(parsed.argv);
  auto const& executable = argv[0];
  auto const value = lowercase(command);

  if (contains_any(value, {"rm -rf", "rm -fr", "mkfs", ":(){", "chmod -r 777 /", "chown -r ", "> /dev/"}))
  {
    return decision(PermissionAction::Deny, "command matches a destructive pattern", PermissionRisk::Critical);
  }

  if (equals_any(executable,
                 {"bash", "sh", "zsh", "fish", "python", "python3", "node", "perl", "ruby", "php", "lua", "make", "ninja", "npm", "pnpm", "yarn", "bun"}))
  {
    return decision(PermissionAction::Deny, "command can execute arbitrary scripts", PermissionRisk::High);
  }

  if (contains_any(value, {"git push", "git reset --hard", "git clean", "npm publish", "pnpm publish", "yarn publish", "deploy", "terraform apply",
                           "kubectl delete", "sudo "}))
  {
    return decision(PermissionAction::Ask, "command can change external or destructive state", PermissionRisk::High);
  }

  if (executable == "git" && argv.size() >= 2 && (argv[1] == "status" || argv[1] == "diff" || argv[1] == "log"))
  {
    if (!has_unsafe_path_arg(parsed.argv, 2))
    {
      return decision(PermissionAction::Allow, "command is read-only or local verification", PermissionRisk::Low);
    }
    return decision(PermissionAction::Ask, "git command includes unsafe path or output options", PermissionRisk::Medium);
  }
  if (executable == "cmake")
  {
    for (auto const& arg : argv)
    {
      if (is_dangerous_cmake_arg(arg))
      {
        return decision(PermissionAction::Deny, "cmake script/file helpers are not allowed", PermissionRisk::High);
      }
    }
  }
  if (is_repository_controlled_build_or_test_command(command))
  {
    return decision(PermissionAction::Ask, "repository build or test execution requires explicit approval", PermissionRisk::High);
  }
  if (executable == "rg")
  {
    for (auto const& arg : argv)
    {
      if (arg == "--pre" || arg.starts_with("--pre="))
      {
        return decision(PermissionAction::Deny, "rg preprocessors can execute arbitrary commands", PermissionRisk::High);
      }
    }
    if (!has_unsafe_path_arg(parsed.argv, 1))
    {
      return decision(PermissionAction::Allow, "command is read-only or local verification", PermissionRisk::Low);
    }
  }
  if (executable == "ls" && !has_unsafe_path_arg(parsed.argv, 1))
  {
    return decision(PermissionAction::Allow, "command is read-only or local verification", PermissionRisk::Low);
  }
  if (executable == "pwd" && (argv.size() == 1 || (argv.size() == 2 && (argv[1] == "-p" || argv[1] == "-l"))))
  {
    return decision(PermissionAction::Allow, "command is read-only or local verification", PermissionRisk::Low);
  }
  if (executable == "sleep" && argv.size() == 2 && is_simple_number(argv[1]))
  {
    return decision(PermissionAction::Allow, "command is read-only or local verification", PermissionRisk::Low);
  }
  if (executable == "printenv" && argv.size() == 2 && (argv[1] == "pwd" || argv[1] == "path" || argv[1] == "home"))
  {
    return decision(PermissionAction::Allow, "command is read-only or local verification", PermissionRisk::Low);
  }

  return decision(PermissionAction::Ask, "command risk is unknown", PermissionRisk::Medium);
}

std::string to_string(PermissionAction action)
{
  switch (action)
  {
    case PermissionAction::Allow:
      return "allow";
    case PermissionAction::Ask:
      return "ask";
    case PermissionAction::Deny:
      return "deny";
  }
  return "deny";
}

std::string to_string(PermissionResolution resolution)
{
  switch (resolution)
  {
    case PermissionResolution::Allow:
    case PermissionResolution::AllowSessionGrant:
      return "allow";
    case PermissionResolution::Deny:
      return "deny";
    case PermissionResolution::Cancel:
      return "cancel";
  }
  return "deny";
}

std::string to_string(PermissionResolutionDecision const& decision)
{
  return to_string(decision.resolution);
}

std::string to_string(CommandContainmentStatus status)
{
  switch (status)
  {
    case CommandContainmentStatus::NotRequired:
      return "not_required";
    case CommandContainmentStatus::Unavailable:
      return "unavailable";
    case CommandContainmentStatus::Available:
      return "available";
    case CommandContainmentStatus::Active:
      return "active";
    case CommandContainmentStatus::UnverifiedDelegatedExecutor:
      return "unverified_delegated_executor";
  }
  return "unavailable";
}

std::string to_string(PermissionRisk risk)
{
  switch (risk)
  {
    case PermissionRisk::Low:
      return "low";
    case PermissionRisk::Medium:
      return "medium";
    case PermissionRisk::High:
      return "high";
    case PermissionRisk::Critical:
      return "critical";
  }
  return "high";
}

std::string to_string(Operation operation)
{
  switch (operation)
  {
    case Operation::ReadFile:
      return "read";
    case Operation::SearchFiles:
      return "search";
    case Operation::EditFile:
      return "edit";
    case Operation::RunCommand:
      return "bash";
    case Operation::NetworkFetch:
      return "network.fetch";
    case Operation::NetworkSearch:
      return "network.search";
    case Operation::LspServerLaunch:
      return "lsp.server.launch";
    case Operation::LspQuery:
      return "lsp.query";
    case Operation::SkillLoad:
      return "skill";
    case Operation::TaskRun:
      return "task";
    case Operation::PluginExecute:
      return "plugin.execute";
    case Operation::PluginToolCall:
      return "plugin.tool.call";
    case Operation::PluginCommandRun:
      return "plugin.command.run";
    case Operation::PluginUiPresent:
      return "plugin.ui.present";
    case Operation::PluginEventObserve:
      return "plugin.event.observe";
    case Operation::McpServerLaunch:
      return "mcp.server.launch";
    case Operation::McpServerConnect:
      return "mcp.server.connect";
    case Operation::McpToolCall:
      return "mcp.tool.call";
    case Operation::McpResourceRead:
      return "mcp.resource.read";
  }
  return "unknown";
}

}  // namespace ava::permissions

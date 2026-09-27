#include "sys.h"
#include "ava/command/environment.h"
#include "ava/lsp/bounded_file_reader.h"
#include "ava/lsp/configured_provider.h"
#include "ava/core/ids.h"
#include "ava/core/json.h"
#include "ava/core/process_args.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <mutex>
#include <optional>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace ava::lsp {
namespace {

constexpr std::uintmax_t kMaxLspConfigBytes = 64 * 1024;
constexpr std::size_t kMaxServers = 8;
constexpr std::size_t kMaxArgv = 32;
constexpr std::size_t kMaxArgBytes = 4096;
constexpr std::size_t kMaxIdBytes = 64;
constexpr long long kMinTimeoutMs = 100;
constexpr long long kMaxTimeoutMs = 30000;

struct ConfiguredServer
{
  std::string id;
  std::vector<std::string> argv;
  std::vector<std::string> file_extensions;
  std::string language_id = "plaintext";
  std::chrono::milliseconds startup_timeout{3000};
  std::chrono::milliseconds request_timeout{3000};
  bool project_scoped = false;
  bool builtin = false;
  std::filesystem::path source_path;
  std::optional<ExecutableIdentity> executable_identity = std::nullopt;
};

std::optional<std::string> workspace_relative_process_arg(std::vector<std::string> const& argv)
{
  auto const match = std::ranges::find_if(argv, [](std::string const& value) { return ava::core::is_workspace_relative_process_arg(value); });
  if (match == argv.end())
    return std::nullopt;
  return *match;
}

std::string argv_json(std::vector<std::string> const& argv)
{
  std::string value = "[";
  for (std::size_t index = 0; index < argv.size(); ++index)
  {
    if (index > 0)
      value += ',';
    value += '"';
    value += ava::core::json::escape(argv[index]);
    value += '"';
  }
  value += ']';
  return value;
}

std::string executable_identity_token(ExecutableIdentity const& identity)
{
  ava::command::detail::Sha256Builder hash;
  hash.append_field("ava-lsp-executable-identity-v1");
  hash.append_field(identity.executable_path.generic_string());
  hash.append_number(identity.owner_uid);
  hash.append_number(identity.owner_gid);
  hash.append_number(identity.mode);
  hash.append_number(identity.link_count);
  hash.append_number(identity.device);
  hash.append_number(identity.inode);
  hash.append_number(identity.size);
  hash.append_field(std::to_string(identity.changed_seconds));
  hash.append_field(std::to_string(identity.changed_nanoseconds));
  return "sha256:ava-lsp-executable-v1:" + hash.hex();
}

std::string permission_command(ConfiguredServer const& server)
{
  auto const argv = argv_json(server.argv);
  if (!server.builtin || !server.executable_identity)
    return argv;
  return "{\"argv\":" + argv + ",\"executable_identity\":\"" + executable_identity_token(*server.executable_identity) + "\"}";
}

ava::core::Error config_error(ava::core::ErrorCategory category, std::string message, std::filesystem::path const& path)
{
  auto error = ava::core::Error(category, std::move(message));
  error.with_context("path", path.string());
  return error;
}

bool has_control_byte(std::string_view value)
{
  return std::ranges::any_of(value, [](unsigned char ch) { return ch < 0x20 || ch == 0x7f; });
}

bool valid_server_id(std::string_view value)
{
  if (value.empty() || value.size() > kMaxIdBytes || has_control_byte(value))
    return false;
  return std::ranges::all_of(value, [](unsigned char ch) { return std::isalnum(ch) != 0 || ch == '_' || ch == '-' || ch == '.'; });
}

bool valid_extension(std::string_view value)
{
  if (value.empty() || value.size() > 32 || value.front() != '.' || has_control_byte(value))
    return false;
  return std::ranges::all_of(value.substr(1), [](unsigned char ch) { return std::isalnum(ch) != 0 || ch == '_' || ch == '-' || ch == '+'; });
}

bool valid_language_id(std::string_view value)
{
  if (value.empty() || value.size() > 64 || has_control_byte(value))
    return false;
  return std::ranges::all_of(value, [](unsigned char ch) { return std::isalnum(ch) != 0 || ch == '_' || ch == '-' || ch == '+' || ch == '#'; });
}

std::optional<char> field_first_char(std::string_view object, std::string_view key)
{
  auto const start = ava::core::json::field_value_start(object, key);
  if (!start || *start >= object.size())
    return std::nullopt;
  return object[*start];
}

bool field_is_present(std::string_view object, std::string_view key)
{
  return ava::core::json::field_value_start(object, key).has_value();
}

std::optional<std::vector<char>> array_value_start_chars(std::string_view array)
{
  if (array.size() < 2 || array.front() != '[' || array.back() != ']')
    return std::nullopt;
  std::vector<char> starts;
  bool in_string = false;
  bool escaped = false;
  int object_depth = 0;
  int array_depth = 0;
  bool expecting_value = true;
  for (std::size_t index = 1; index + 1 < array.size(); ++index)
  {
    auto const ch = array[index];
    if (in_string)
    {
      if (escaped)
      {
        escaped = false;
        continue;
      }
      if (ch == '\\')
      {
        escaped = true;
        continue;
      }
      if (ch == '"')
        in_string = false;
      continue;
    }
    if (object_depth == 0 && array_depth == 0 && std::isspace(static_cast<unsigned char>(ch)))
      continue;
    if (object_depth == 0 && array_depth == 0 && ch == ',')
    {
      expecting_value = true;
      continue;
    }
    if (expecting_value && object_depth == 0 && array_depth == 0)
    {
      starts.push_back(ch);
      expecting_value = false;
    }
    if (ch == '"')
    {
      in_string = true;
    }
    else if (ch == '{')
    {
      ++object_depth;
    }
    else if (ch == '}')
    {
      if (object_depth > 0)
        --object_depth;
    }
    else if (ch == '[')
    {
      ++array_depth;
    }
    else if (ch == ']')
    {
      if (array_depth > 0)
        --array_depth;
    }
  }
  return starts;
}

std::optional<std::size_t> json_string_end(std::string_view text, std::size_t start)
{
  if (start >= text.size() || text[start] != '"')
    return std::nullopt;
  bool escaped = false;
  for (std::size_t index = start + 1; index < text.size(); ++index)
  {
    auto const ch = text[index];
    if (escaped)
    {
      escaped = false;
      continue;
    }
    if (ch == '\\')
    {
      escaped = true;
      continue;
    }
    if (ch == '"')
      return index + 1;
  }
  return std::nullopt;
}

std::optional<std::size_t> json_balanced_end(std::string_view text, std::size_t start, char open, char close)
{
  if (start >= text.size() || text[start] != open)
    return std::nullopt;
  std::size_t depth = 0;
  for (std::size_t index = start; index < text.size(); ++index)
  {
    auto const ch = text[index];
    if (ch == '"')
    {
      auto const end = json_string_end(text, index);
      if (!end)
        return std::nullopt;
      index = *end - 1;
      continue;
    }
    if (ch == open)
      ++depth;
    if (ch == close)
    {
      if (depth == 0)
        return std::nullopt;
      --depth;
      if (depth == 0)
        return index + 1;
    }
  }
  return std::nullopt;
}

ava::core::Result<long long> strict_integer_field(std::string_view object, std::string_view key, long long fallback)
{
  auto const start = ava::core::json::field_value_start(object, key);
  if (!start)
    return fallback;
  auto index = *start;
  if (index >= object.size() || !std::isdigit(static_cast<unsigned char>(object[index])))
  {
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "integer field must be a JSON integer"));
  }
  long long value = 0;
  while (index < object.size() && std::isdigit(static_cast<unsigned char>(object[index])))
  {
    auto const digit = static_cast<long long>(object[index] - '0');
    if (value > (std::numeric_limits<long long>::max() - digit) / 10)
    {
      return std::unexpected(ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "integer field is too large"));
    }
    value = value * 10 + digit;
    ++index;
  }
  while (index < object.size() && std::isspace(static_cast<unsigned char>(object[index]))) ++index;
  if (index >= object.size() || (object[index] != ',' && object[index] != '}'))
  {
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "integer field must be a JSON integer"));
  }
  return value;
}

std::optional<std::vector<std::string>> strict_string_array_field(std::string_view object, std::string_view key)
{
  auto const start = ava::core::json::field_value_start(object, key);
  if (!start)
    return std::vector<std::string>{};
  if (*start >= object.size() || object[*start] != '[')
    return std::nullopt;
  auto const end = json_balanced_end(object, *start, '[', ']');
  if (!end)
    return std::nullopt;
  auto const starts = array_value_start_chars(object.substr(*start, *end - *start));
  if (!starts || std::ranges::any_of(*starts, [](char ch) { return ch != '"'; }))
    return std::nullopt;
  auto values = ava::core::json::strings_in_array_field(object, key);
  if (!values.empty())
    return values;
  auto const array = object.substr(*start, *end - *start);
  auto index = std::size_t{1};
  while (index + 1 < array.size() && std::isspace(static_cast<unsigned char>(array[index]))) ++index;
  if (index + 1 < array.size() && array[index] != ']')
    return std::nullopt;
  return std::vector<std::string>{};
}

std::optional<std::vector<std::string>> strict_object_array_field(std::string_view object, std::string_view key)
{
  auto const start = ava::core::json::field_value_start(object, key);
  if (!start)
    return std::vector<std::string>{};
  if (*start >= object.size() || object[*start] != '[')
    return std::nullopt;
  auto values = ava::core::json::objects_in_array_field(object, key);
  if (values.empty())
  {
    auto const end = json_balanced_end(object, *start, '[', ']');
    if (!end)
      return std::nullopt;
    auto const array = object.substr(*start, *end - *start);
    auto index = std::size_t{1};
    while (index + 1 < array.size() && std::isspace(static_cast<unsigned char>(array[index]))) ++index;
    if (index + 1 < array.size() && array[index] != ']')
      return std::nullopt;
  }
  else
  {
    auto const end = json_balanced_end(object, *start, '[', ']');
    if (!end)
      return std::nullopt;
    auto const starts = array_value_start_chars(object.substr(*start, *end - *start));
    if (!starts || starts->size() != values.size() || std::ranges::any_of(*starts, [](char ch) { return ch != '{'; }))
    {
      return std::nullopt;
    }
  }
  return values;
}

ava::core::Result<std::optional<std::string>> read_global_config_file(std::filesystem::path const& path,
                                                                      std::shared_ptr<ava::core::AnchorSet const> const& anchor_set)
{
  auto const read_config = [&](bool require_private_owner) {
    return read_bounded_lsp_file(BoundedFileReadOptions{
        .path = path,
        .workspace_root = {},
        .anchor_set = anchor_set,
        .max_bytes = kMaxLspConfigBytes,
        .scope = BoundedFileReadScope::External,
        .missing_ok = true,
        .require_private_owner = require_private_owner,
    });
  };

  // Preserve explicit-only global configuration compatibility. Built-in opt-in
  // carries installed executable authority, so any file that defines the field
  // is reread through the same AnchorSet with descriptor-bound owner/mode/link
  // requirements before its contents can be parsed.
  auto content = read_config(false);
  if (!content || !*content || !field_is_present(**content, "builtin_servers"))
    return content;
  return read_config(true);
}

ava::core::Result<std::optional<std::string>> read_project_config_file(std::filesystem::path const& path, std::filesystem::path const& workspace_root,
                                                                       std::shared_ptr<ava::core::AnchorSet const> const& anchor_set)
{
  return read_bounded_lsp_file(BoundedFileReadOptions{
      .path = path,
      .workspace_root = workspace_root,
      .anchor_set = anchor_set,
      .max_bytes = kMaxLspConfigBytes,
      .scope = BoundedFileReadScope::Workspace,
      .missing_ok = true,
  });
}

ava::core::Result<void> parse_config(std::filesystem::path const& path, std::string_view json, std::vector<ConfiguredServer>& servers,
                                     std::vector<std::string>& builtin_ids, bool project_scoped)
{
  if (json.empty())
    return {};
  if (!ava::core::json::is_valid_object(json))
  {
    return std::unexpected(config_error(ava::core::ErrorCategory::InvalidArgument, "LSP config must be a JSON object", path));
  }
  auto const version = strict_integer_field(json, "version", 1);
  if (!version)
  {
    return std::unexpected(config_error(ava::core::ErrorCategory::InvalidArgument, "LSP config version must be an integer", path));
  }
  if (*version != 1)
  {
    return std::unexpected(config_error(ava::core::ErrorCategory::InvalidArgument, "unsupported LSP config version", path));
  }

  if (field_is_present(json, "builtin_servers"))
  {
    if (project_scoped)
    {
      return std::unexpected(config_error(ava::core::ErrorCategory::InvalidArgument, "project LSP config must not define builtin_servers", path));
    }
    if (field_first_char(json, "builtin_servers") != std::optional<char>{'['})
    {
      return std::unexpected(config_error(ava::core::ErrorCategory::InvalidArgument, "LSP config builtin_servers must be an array", path));
    }
    auto requested = strict_string_array_field(json, "builtin_servers");
    if (!requested || requested->size() > builtin_server_recipes().size())
    {
      return std::unexpected(config_error(ava::core::ErrorCategory::InvalidArgument, "LSP config builtin_servers is invalid", path));
    }
    auto const recipes = builtin_server_recipes();
    for (auto const& id : *requested)
    {
      if (!valid_server_id(id) || std::ranges::none_of(recipes, [&](BuiltinServerRecipe const& recipe) { return recipe.id == id; }))
      {
        return std::unexpected(config_error(ava::core::ErrorCategory::InvalidArgument, "LSP config builtin server id is unknown", path));
      }
      if (std::ranges::find(builtin_ids, id) != builtin_ids.end())
      {
        return std::unexpected(config_error(ava::core::ErrorCategory::InvalidArgument, "LSP config builtin server id is duplicated", path));
      }
      builtin_ids.push_back(id);
    }
  }

  if (field_is_present(json, "servers") && field_first_char(json, "servers") != std::optional<char>{'['})
  {
    return std::unexpected(config_error(ava::core::ErrorCategory::InvalidArgument, "LSP config servers must be an array", path));
  }
  auto const server_objects = strict_object_array_field(json, "servers");
  if (!server_objects)
  {
    return std::unexpected(config_error(ava::core::ErrorCategory::InvalidArgument, "LSP config servers must contain only objects", path));
  }
  if (servers.size() + server_objects->size() > kMaxServers)
  {
    auto error = config_error(ava::core::ErrorCategory::InvalidArgument, "LSP config defines too many servers", path);
    error.with_context("max_servers", std::to_string(kMaxServers));
    return std::unexpected(std::move(error));
  }

  for (auto const& server_json : *server_objects)
  {
    auto id = ava::core::json::string_field(server_json, "id");
    if (!id || !valid_server_id(*id))
    {
      return std::unexpected(config_error(ava::core::ErrorCategory::InvalidArgument, "LSP server id is invalid", path));
    }
    auto argv = strict_string_array_field(server_json, "argv");
    if (!argv || argv->empty() || argv->size() > kMaxArgv)
    {
      return std::unexpected(config_error(ava::core::ErrorCategory::InvalidArgument, "LSP server argv is invalid", path));
    }
    for (auto const& arg : *argv)
    {
      if (arg.empty() || arg.size() > kMaxArgBytes || has_control_byte(arg))
      {
        return std::unexpected(config_error(ava::core::ErrorCategory::InvalidArgument, "LSP server argv contains an invalid argument", path));
      }
    }

    std::vector<std::string> extensions;
    if (field_is_present(server_json, "file_extensions") && field_first_char(server_json, "file_extensions") != std::optional<char>{'['})
    {
      return std::unexpected(config_error(ava::core::ErrorCategory::InvalidArgument, "LSP server file_extensions must be an array", path));
    }
    auto extensions_field = strict_string_array_field(server_json, "file_extensions");
    if (!extensions_field)
    {
      return std::unexpected(config_error(ava::core::ErrorCategory::InvalidArgument, "LSP server file_extensions must contain only strings", path));
    }
    for (auto extension : *extensions_field)
    {
      if (!valid_extension(extension))
      {
        return std::unexpected(config_error(ava::core::ErrorCategory::InvalidArgument, "LSP server file extension is invalid", path));
      }
      extensions.push_back(std::move(extension));
    }

    std::string language_id = "plaintext";
    if (field_is_present(server_json, "language_id"))
    {
      if (field_first_char(server_json, "language_id") != std::optional<char>{'"'})
      {
        return std::unexpected(config_error(ava::core::ErrorCategory::InvalidArgument, "LSP server language_id must be a string", path));
      }
      auto parsed_language_id = ava::core::json::string_field(server_json, "language_id");
      if (!parsed_language_id)
      {
        return std::unexpected(config_error(ava::core::ErrorCategory::InvalidArgument, "LSP server language_id must be a string", path));
      }
      language_id = std::move(*parsed_language_id);
    }
    if (!valid_language_id(language_id))
    {
      return std::unexpected(config_error(ava::core::ErrorCategory::InvalidArgument, "LSP server language_id is invalid", path));
    }

    auto const timeout_value = strict_integer_field(server_json, "timeout_ms", 3000);
    if (!timeout_value)
    {
      return std::unexpected(config_error(ava::core::ErrorCategory::InvalidArgument, "LSP server timeout_ms must be an integer", path));
    }
    if (*timeout_value < kMinTimeoutMs || *timeout_value > kMaxTimeoutMs)
    {
      return std::unexpected(config_error(ava::core::ErrorCategory::InvalidArgument, "LSP server timeout is outside supported limits", path));
    }

    auto const startup_timeout_value = strict_integer_field(server_json, "startup_timeout_ms", *timeout_value);
    if (!startup_timeout_value)
    {
      return std::unexpected(config_error(ava::core::ErrorCategory::InvalidArgument, "LSP server startup_timeout_ms must be an integer", path));
    }
    if (*startup_timeout_value < kMinTimeoutMs || *startup_timeout_value > kMaxTimeoutMs)
    {
      return std::unexpected(config_error(ava::core::ErrorCategory::InvalidArgument, "LSP server startup timeout is outside supported limits", path));
    }

    if (!project_scoped)
    {
      auto relative_arg = workspace_relative_process_arg(*argv);
      if (relative_arg)
      {
        auto error = config_error(ava::core::ErrorCategory::InvalidArgument, "global LSP server argv must not contain workspace-relative paths", path);
        error.with_context("server", *id);
        error.with_context("argument", *relative_arg);
        error.with_context("resolution", "move the server to project LSP config and trust the project, or use an absolute path/PATH command");
        return std::unexpected(std::move(error));
      }
    }

    if (std::ranges::any_of(servers, [&id](ConfiguredServer const& existing) { return existing.id == *id; }))
    {
      return std::unexpected(config_error(ava::core::ErrorCategory::InvalidArgument, "LSP server id is duplicated", path));
    }

    servers.push_back(ConfiguredServer{.id = std::move(*id),
                                       .argv = std::move(*argv),
                                       .file_extensions = std::move(extensions),
                                       .language_id = std::move(language_id),
                                       .startup_timeout = std::chrono::milliseconds(*startup_timeout_value),
                                       .request_timeout = std::chrono::milliseconds(*timeout_value),
                                       .project_scoped = project_scoped,
                                       .builtin = false,
                                       .source_path = path,
                                       .executable_identity = std::nullopt});
  }
  return {};
}

std::vector<BuiltinServerInspection> append_available_builtin_servers(std::vector<std::string> const& builtin_ids, ConfiguredLspProviderFiles const& files,
                                                                      std::vector<ConfiguredServer>& servers)
{
  auto inspections = inspect_builtin_servers(builtin_ids, files.workspace_root, files.anchor_set, files.builtin_discovery);
  auto const recipes = builtin_server_recipes();
  for (auto const& inspection : inspections)
  {
    if (inspection.status != BuiltinServerStatus::Available || !inspection.executable ||
        std::ranges::any_of(servers, [&](ConfiguredServer const& server) { return server.id == inspection.id; }))
    {
      continue;
    }
    auto const recipe = std::ranges::find_if(recipes, [&](BuiltinServerRecipe const& candidate) { return candidate.id == inspection.id; });
    if (recipe == recipes.end())
      continue;
    std::vector<std::string> argv{inspection.executable->executable_path.string()};
    argv.insert(argv.end(), recipe->arguments.begin(), recipe->arguments.end());
    servers.push_back(ConfiguredServer{
        .id = recipe->id,
        .argv = std::move(argv),
        .file_extensions = recipe->file_extensions,
        .language_id = recipe->language_id,
        .startup_timeout = std::chrono::milliseconds(10000),
        .request_timeout = std::chrono::milliseconds(3000),
        .project_scoped = false,
        .builtin = true,
        .source_path = files.global_config_file,
        .executable_identity = inspection.executable,
    });
  }
  return inspections;
}

bool path_matches(ConfiguredServer const& server, std::filesystem::path const& path)
{
  if (server.file_extensions.empty())
    return true;
  auto const extension = path.extension().string();
  return std::ranges::find(server.file_extensions, extension) != server.file_extensions.end();
}

class ConfiguredLspProvider final : public DiagnosticsProvider
{
 public:
  ConfiguredLspProvider(std::filesystem::path workspace_root, std::shared_ptr<ava::core::AnchorSet const> anchor_set, ava::core::Mode mode,
                        ava::permissions::PermissionResolver permission_resolver, std::vector<ConfiguredServer> servers)
      : workspace_root_(std::move(workspace_root)),
        anchor_set_(std::move(anchor_set)),
        mode_(mode),
        permission_resolver_(std::move(permission_resolver)),
        servers_(std::move(servers))
  {
  }

  [[nodiscard]] ava::core::Result<std::vector<Diagnostic>> diagnostics(std::filesystem::path const& path, CancelCallback cancel_requested = nullptr) override
  {
    auto client = client_for_path(path, cancel_requested);
    if (!client)
      return std::unexpected(std::move(client.error()));
    return (*client)->diagnostics(path, std::move(cancel_requested));
  }

  [[nodiscard]] ava::core::Result<std::vector<Symbol>> document_symbols(std::filesystem::path const& path, CancelCallback cancel_requested = nullptr) override
  {
    auto client = client_for_path(path, cancel_requested);
    if (!client)
      return std::unexpected(std::move(client.error()));
    return (*client)->document_symbols(path, std::move(cancel_requested));
  }

  [[nodiscard]] ava::core::Result<std::vector<Symbol>> workspace_symbols(std::string_view query, CancelCallback cancel_requested = nullptr) override
  {
    std::vector<Symbol> symbols;
    for (auto const& server : servers_)
    {
      auto client = start_client(server, workspace_root_, cancel_requested);
      if (!client)
        return std::unexpected(std::move(client.error()));
      auto server_symbols = (*client)->workspace_symbols(query, cancel_requested);
      if (!server_symbols)
        return std::unexpected(std::move(server_symbols.error()));
      symbols.insert(symbols.end(), server_symbols->begin(), server_symbols->end());
    }
    return symbols;
  }

  [[nodiscard]] ava::core::Result<std::vector<Location>> definitions(std::filesystem::path const& path, int line, int column,
                                                                     CancelCallback cancel_requested = nullptr) override
  {
    auto client = client_for_path(path, cancel_requested);
    if (!client)
      return std::unexpected(std::move(client.error()));
    return (*client)->definitions(path, line, column, std::move(cancel_requested));
  }

  [[nodiscard]] ava::core::Result<std::vector<Location>> references(std::filesystem::path const& path, int line, int column,
                                                                    CancelCallback cancel_requested = nullptr) override
  {
    auto client = client_for_path(path, cancel_requested);
    if (!client)
      return std::unexpected(std::move(client.error()));
    return (*client)->references(path, line, column, std::move(cancel_requested));
  }

  void set_permission_request_ids(std::shared_ptr<std::vector<std::string>> ids) override { permission_request_ids_ = std::move(ids); }

 private:
  [[nodiscard]] ava::core::Result<std::shared_ptr<SubprocessLspClient>> client_for_path(std::filesystem::path const& path,
                                                                                        CancelCallback const& cancel_requested)
  {
    auto const it = std::ranges::find_if(servers_, [&path](ConfiguredServer const& server) { return path_matches(server, path); });
    if (it == servers_.end())
    {
      auto error = ava::core::Error(ava::core::ErrorCategory::Tool, "no configured LSP server matches path");
      error.with_context("path", path.string());
      return std::unexpected(std::move(error));
    }
    auto const root = it->builtin ? select_builtin_server_root(it->id, path, workspace_root_, anchor_set_) : workspace_root_;
    return start_client(*it, root, cancel_requested);
  }

  [[nodiscard]] ava::core::Result<std::shared_ptr<SubprocessLspClient>> start_client(ConfiguredServer const& server, std::filesystem::path const& root,
                                                                                     CancelCallback const& cancel_requested)
  {
    std::lock_guard<std::mutex> lock(clients_mutex_);
    auto const cache_key = server.id + "\n" + root.lexically_normal().generic_string();
    if (auto cached = clients_.find(cache_key); cached != clients_.end())
    {
      if (cached->second && cached->second->is_alive())
        return cached->second;
      clients_.erase(cached);
    }
    auto launch = ensure_launch_permission(server);
    if (!launch)
      return std::unexpected(std::move(launch.error()));
    auto const process_cwd =
        server.builtin ? root : (server.project_scoped ? workspace_root_ : ava::core::safe_global_process_cwd(server.source_path, workspace_root_));
    auto client = SubprocessLspClient::start(ServerConfig{.argv = server.argv,
                                                          .workspace_root = workspace_root_,
                                                          .server_root = root,
                                                          .anchor_set = anchor_set_,
                                                          .process_cwd = process_cwd,
                                                          .startup_timeout = server.startup_timeout,
                                                          .request_timeout = server.request_timeout,
                                                          .language_id = server.language_id,
                                                          .executable_identity = server.executable_identity},
                                             cancel_requested);
    if (!client)
      return std::unexpected(std::move(client.error()));
    clients_.emplace(cache_key, *client);
    return *client;
  }

  [[nodiscard]] ava::core::VoidResult ensure_launch_permission(ConfiguredServer const& server) const
  {
    auto const decision = ava::permissions::decide(ava::permissions::PermissionRequest{
        .operation = ava::permissions::Operation::LspServerLaunch,
        .mode = mode_,
        .workspace_dir = workspace_root_,
        .target_path = workspace_root_,
        .command = permission_command(server),
    });
    if (decision.action == ava::permissions::PermissionAction::Allow)
      return {};
    if (decision.action == ava::permissions::PermissionAction::Deny)
    {
      auto error = ava::core::Error(ava::core::ErrorCategory::PermissionDenied, "LSP server launch denied by policy");
      error.with_context("server", server.id);
      return std::unexpected(std::move(error));
    }
    if (!permission_resolver_)
    {
      auto error = ava::core::Error(ava::core::ErrorCategory::PermissionDenied, "LSP server launch requires permission");
      error.with_context("server", server.id);
      return std::unexpected(std::move(error));
    }
    auto permission_request_id = ava::core::make_id("permreq");
    auto resolved = permission_resolver_(ava::permissions::PermissionPrompt{
        .permission_request_id = permission_request_id,
        .operation = ava::permissions::Operation::LspServerLaunch,
        .mode = mode_,
        .workspace_dir = workspace_root_,
        .target_path = workspace_root_,
        .command = permission_command(server),
        .tool_name = "lsp_server_launch",
        .reason = decision.reason,
        .risk = decision.risk,
    });
    if (permission_request_ids_ && std::ranges::find(*permission_request_ids_, permission_request_id) == permission_request_ids_->end())
    {
      permission_request_ids_->push_back(permission_request_id);
    }
    if (!resolved)
      return std::unexpected(std::move(resolved.error()));
    if (*resolved == ava::permissions::PermissionResolution::Allow || *resolved == ava::permissions::PermissionResolution::AllowSessionGrant)
    {
      return {};
    }
    if (*resolved == ava::permissions::PermissionResolution::Cancel)
    {
      auto error = ava::core::Error(ava::core::ErrorCategory::Unknown, "agent loop canceled", ava::core::ErrorCode::Canceled);
      error.with_context("server", server.id);
      error.with_context("permission_request_id", permission_request_id);
      return std::unexpected(std::move(error));
    }
    auto error = ava::core::Error(ava::core::ErrorCategory::PermissionDenied, "LSP server launch denied");
    error.with_context("server", server.id);
    return std::unexpected(std::move(error));
  }

  std::filesystem::path workspace_root_;
  std::shared_ptr<ava::core::AnchorSet const> anchor_set_;
  ava::core::Mode mode_ = ava::core::Mode::Build;
  ava::permissions::PermissionResolver permission_resolver_ = nullptr;
  std::vector<ConfiguredServer> servers_;
  std::mutex clients_mutex_;
  std::unordered_map<std::string, std::shared_ptr<SubprocessLspClient>> clients_;
  std::shared_ptr<std::vector<std::string>> permission_request_ids_ = nullptr;
};

ConfiguredLspConfigDiagnostic inspect_config_source(std::string scope, std::filesystem::path const& path, bool project_scoped,
                                                    std::filesystem::path const& workspace_root, std::shared_ptr<ava::core::AnchorSet const> const& anchor_set,
                                                    std::vector<ConfiguredServer>& servers, std::vector<std::string>& builtin_ids)
{
  ConfiguredLspConfigDiagnostic diagnostic{.scope = std::move(scope), .path = path};
  auto content = project_scoped ? read_project_config_file(path, workspace_root, anchor_set) : read_global_config_file(path, anchor_set);
  if (!content)
  {
    diagnostic.error = std::move(content.error());
    return diagnostic;
  }
  if (!*content)
    return diagnostic;

  diagnostic.exists = true;
  diagnostic.byte_count = (*content)->size();
  auto candidate_servers = servers;
  auto candidate_builtin_ids = builtin_ids;
  auto parsed = parse_config(path, **content, candidate_servers, candidate_builtin_ids, project_scoped);
  if (!parsed)
  {
    diagnostic.error = std::move(parsed.error());
    return diagnostic;
  }

  diagnostic.loaded = true;
  diagnostic.server_count = candidate_servers.size() - servers.size();
  servers = std::move(candidate_servers);
  builtin_ids = std::move(candidate_builtin_ids);
  return diagnostic;
}

}  // namespace

ava::core::Result<std::shared_ptr<DiagnosticsProvider>> make_configured_lsp_provider(ConfiguredLspProviderFiles const& files)
{
  std::vector<ConfiguredServer> servers;
  std::vector<std::string> builtin_ids;
  if (!files.global_config_file.empty())
  {
    auto content = read_global_config_file(files.global_config_file, files.anchor_set);
    if (!content)
      return std::unexpected(std::move(content.error()));
    if (*content)
    {
      auto parsed = parse_config(files.global_config_file, **content, servers, builtin_ids, false);
      if (!parsed)
        return std::unexpected(std::move(parsed.error()));
    }
  }
  if (!files.project_config_file.empty())
  {
    auto content = read_project_config_file(files.project_config_file, files.workspace_root, files.anchor_set);
    if (!content)
      return std::unexpected(std::move(content.error()));
    if (*content)
    {
      auto parsed = parse_config(files.project_config_file, **content, servers, builtin_ids, true);
      if (!parsed)
        return std::unexpected(std::move(parsed.error()));
    }
  }
  static_cast<void>(append_available_builtin_servers(builtin_ids, files, servers));
  if (servers.empty())
    return std::shared_ptr<DiagnosticsProvider>{};
  return std::make_shared<ConfiguredLspProvider>(files.workspace_root.lexically_normal(), files.anchor_set, files.mode, files.permission_resolver,
                                                 std::move(servers));
}

ConfiguredLspProviderInspection inspect_configured_lsp_provider(ConfiguredLspProviderFiles const& files)
{
  ConfiguredLspProviderInspection inspection;
  std::vector<ConfiguredServer> servers;
  std::vector<std::string> builtin_ids;
  if (!files.global_config_file.empty())
  {
    inspection.configs.push_back(
        inspect_config_source("global", files.global_config_file, false, files.workspace_root, files.anchor_set, servers, builtin_ids));
  }
  if (!files.project_config_file.empty())
  {
    inspection.configs.push_back(
        inspect_config_source("project", files.project_config_file, true, files.workspace_root, files.anchor_set, servers, builtin_ids));
  }
  inspection.builtin_servers = append_available_builtin_servers(builtin_ids, files, servers);
  inspection.server_count = servers.size();
  inspection.error_count = static_cast<std::size_t>(
      std::ranges::count_if(inspection.configs, [](ConfiguredLspConfigDiagnostic const& diagnostic) { return diagnostic.error.has_value(); }));
  return inspection;
}

}  // namespace ava::lsp

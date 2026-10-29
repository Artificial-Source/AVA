#include "sys.h"
#include "ava/command/discovery.h"
#include "ava/command/environment.h"
#include "ava/command/intent_internal.h"
#include "ava/core/ascii.h"
#include "ava/core/sha256.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <utility>

namespace ava::command::detail {
namespace {

ava::core::VoidResult validate_sealed_environment_root(PathMetadata const& root, std::string_view name, CommandLimits const& limits,
                                                       std::shared_ptr<ava::core::AnchorSet const> const& anchor_set)
{
  if (root.canonical_path.empty() || !root.canonical_path.is_absolute() || has_forbidden_path_byte(root.canonical_path) ||
      root.canonical_path.string().size() > limits.max_path_bytes)
  {
    return std::unexpected(command_error(ava::core::ErrorCategory::InvalidArgument,
                                         std::string(name) + " is not a bounded canonical synthetic environment path", "path", root.canonical_path.string()));
  }
  auto fresh = path_metadata_is_fresh(root, anchor_set);
  if (!fresh)
    return std::unexpected(std::move(fresh.error()));
  if (!*fresh)
  {
    return std::unexpected(
        command_error(ava::core::ErrorCategory::Io, std::string(name) + " changed during environment construction", "path", root.requested_path.string()));
  }
  return {};
}

ava::core::Result<std::string> join_path(std::vector<CommandPathEntry> const& entries, PathMetadata const& trusted_home, CommandLimits const& limits,
                                         std::shared_ptr<ava::core::AnchorSet const> const& anchor_set)
{
  std::string result;
  for (auto const& entry : entries)
  {
    // Environment construction is internal, but retain this validation so no
    // future caller can inject a second PATH entry through a colon-containing
    // CommandPathEntry.
    if (entry.directory.empty() || !entry.directory.is_absolute() || entry.directory.string().find(':') != std::string::npos ||
        has_forbidden_path_byte(entry.directory) || entry.metadata.canonical_path != entry.directory)
    {
      return std::unexpected(
          command_error(ava::core::ErrorCategory::InvalidArgument, "sealed PATH entry is not safely representable", "path", entry.directory.string()));
    }
    auto fresh = is_sealed_user_tool_path(entry.metadata.requested_path, trusted_home)
                     ? user_tool_path_metadata_is_fresh(entry.metadata, trusted_home, anchor_set)
                     : path_metadata_is_fresh(entry.metadata, anchor_set);
    if (!fresh)
      return std::unexpected(std::move(fresh.error()));
    if (!*fresh)
      return std::unexpected(
          command_error(ava::core::ErrorCategory::Io, "sealed PATH entry changed during environment construction", "path", entry.directory.string()));
    std::size_t const separator = result.empty() ? 0U : 1U;
    if (result.size() > limits.max_path_bytes || separator > limits.max_path_bytes - result.size() ||
        entry.directory.string().size() > limits.max_path_bytes - result.size() - separator)
    {
      return std::unexpected(command_error(ava::core::ErrorCategory::InvalidArgument, "derived PATH exceeds the bounded safe representation"));
    }
    if (separator != 0)
      result.push_back(':');
    result += entry.directory.string();
  }
  return result;
}

std::string environment_digest(std::string_view profile_id, std::vector<EnvironmentVariable> const& entries)
{
  Sha256Builder hash;
  hash.append_field("ava-command-environment-v3");
  hash.append_field(profile_id);
  hash.append_number(entries.size());
  for (auto const& entry : entries)
  {
    hash.append_field(entry.key);
    hash.append_field(entry.value);
  }
  return "sha256:ava-command-environment-v3:" + hash.hex();
}

}  // namespace

void Sha256Builder::append_bytes(std::string_view value)
{
  bytes_.insert(bytes_.end(), value.begin(), value.end());
}

void Sha256Builder::append_field(std::string_view value)
{
  std::array<char, sizeof(std::uint64_t)> length{};
  auto size = static_cast<std::uint64_t>(value.size());
  for (auto& byte : length)
  {
    byte = static_cast<char>(size & 0xffU);
    size >>= 8U;
  }
  append_bytes(std::string_view(length.data(), length.size()));
  append_bytes(value);
}

void Sha256Builder::append_number(std::uintmax_t value)
{
  append_field(std::to_string(value));
}

std::string Sha256Builder::hex() const
{
  return ava::core::hex_encode(ava::core::sha256(bytes_));
}

ava::core::Result<CommandEnvironment> EnvironmentFactory::make(CommandEnvironmentOptions const& options, std::vector<CommandPathEntry> const& path_entries,
                                                               std::filesystem::path const& logical_cwd, PathMetadata const& trusted_home,
                                                               SyntheticEnvironmentRoots roots, CommandLimits const& limits,
                                                               std::shared_ptr<ava::core::AnchorSet const> const& anchor_set,
                                                               CommandEnvironment::FactoryPasskey passkey)
{
  if (auto valid = validate_limits(limits); !valid)
    return std::unexpected(std::move(valid.error()));
  if (logical_cwd.empty() || !logical_cwd.is_absolute() || logical_cwd.string().size() > limits.max_path_bytes || has_forbidden_path_byte(logical_cwd))
    return std::unexpected(command_error(ava::core::ErrorCategory::InvalidArgument, "logical child PWD must be a bounded absolute path"));
  if (options.profile_id.empty() || options.profile_id.size() > 128 || ava::core::has_c0_or_del(options.profile_id) ||
      !std::ranges::all_of(options.profile_id, [](unsigned char ch) { return std::isalnum(ch) != 0 || ch == '-' || ch == '_' || ch == '.'; }))
  {
    return std::unexpected(command_error(ava::core::ErrorCategory::InvalidArgument, "environment profile id must be a bounded safe identifier"));
  }
  for (auto const* value : {&options.user, &options.logname})
  {
    if (value->empty() || value->size() > limits.max_argument_bytes || ava::core::has_c0_or_del(*value))
      return std::unexpected(command_error(ava::core::ErrorCategory::InvalidArgument, "USER and LOGNAME must be bounded safe values"));
  }
  auto trusted_home_fresh = trusted_home_metadata_is_fresh(trusted_home, anchor_set);
  if (!trusted_home_fresh)
    return std::unexpected(std::move(trusted_home_fresh.error()));
  if (!*trusted_home_fresh)
    return std::unexpected(command_error(ava::core::ErrorCategory::Io, "sealed trusted home changed during environment construction"));
  for (auto const& [name, root] : std::array<std::pair<std::string_view, PathMetadata const*>, 6>{{{"HOME", &roots.home},
                                                                                                   {"XDG_CONFIG_HOME", &roots.xdg_config_home},
                                                                                                   {"XDG_CACHE_HOME", &roots.xdg_cache_home},
                                                                                                   {"XDG_DATA_HOME", &roots.xdg_data_home},
                                                                                                   {"XDG_STATE_HOME", &roots.xdg_state_home},
                                                                                                   {"TMPDIR", &roots.tmpdir}}})
  {
    if (auto valid = validate_sealed_environment_root(*root, name, limits, anchor_set); !valid)
      return std::unexpected(std::move(valid.error()));
  }

  auto path = join_path(path_entries, trusted_home, limits, anchor_set);
  if (!path)
    return std::unexpected(std::move(path.error()));
  CommandEnvironment environment(std::move(passkey), std::move(roots));
  environment.profile_id_ = options.profile_id;
  environment.entries_ = {{"LANG", "C.UTF-8"},
                          {"LC_ALL", "C.UTF-8"},
                          {"LC_CTYPE", "C.UTF-8"},
                          {"TZ", "UTC"},
                          {"USER", options.user},
                          {"LOGNAME", options.logname},
                          {"PWD", logical_cwd.string()},
                          {"PATH", std::move(*path)},
                          {"HOME", environment.roots_.home.canonical_path.string()},
                          {"XDG_CONFIG_HOME", environment.roots_.xdg_config_home.canonical_path.string()},
                          {"XDG_CACHE_HOME", environment.roots_.xdg_cache_home.canonical_path.string()},
                          {"XDG_DATA_HOME", environment.roots_.xdg_data_home.canonical_path.string()},
                          {"XDG_STATE_HOME", environment.roots_.xdg_state_home.canonical_path.string()},
                          {"TMPDIR", environment.roots_.tmpdir.canonical_path.string()}};
  environment.digest_ = environment_digest(environment.profile_id_, environment.entries_);
  return environment;
}

ava::core::VoidResult validate_environment_matches_plan(CommandEnvironment const& environment, CommandPlan const& plan)
{
  if (environment.profile_id() != plan.environment_profile_id() || environment.digest() != plan.environment_digest() ||
      environment.roots_ != plan.synthetic_environment_roots_)
  {
    return std::unexpected(
        command_error(ava::core::ErrorCategory::PermissionDenied, "prepared command environment does not match the sealed plan environment digest"));
  }
  return {};
}

}  // namespace ava::command::detail

namespace ava::command {

CommandEnvironment::CommandEnvironment(FactoryPasskey, SyntheticEnvironmentRoots roots) : roots_(std::move(roots))
{
}

std::string const& CommandEnvironment::profile_id() const noexcept
{
  return profile_id_;
}

std::string const& CommandEnvironment::digest() const noexcept
{
  return digest_;
}

std::vector<EnvironmentVariable> const& CommandEnvironment::entries() const noexcept
{
  return entries_;
}

SyntheticEnvironmentRoots const& CommandEnvironment::synthetic_roots() const noexcept
{
  return roots_;
}

}  // namespace ava::command

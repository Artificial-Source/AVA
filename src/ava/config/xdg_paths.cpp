#include "sys.h"
#include "ava/config/xdg_paths.h"

#include <cstdlib>
#include <string_view>
#include <pwd.h>
#include <sys/types.h>
#include <unistd.h>

namespace ava::config {
namespace {

std::filesystem::path home_dir()
{
  char const* home = std::getenv("HOME");
  if (home != nullptr && !std::string_view(home).empty())
  {
    auto home_path = std::filesystem::path(home).lexically_normal();
    if (home_path.is_absolute())
      return home_path;
  }

  passwd const* entry = ::getpwuid(::getuid());
  if (entry != nullptr && entry->pw_dir != nullptr && !std::string_view(entry->pw_dir).empty())
  {
    auto passwd_home = std::filesystem::path(entry->pw_dir).lexically_normal();
    if (passwd_home.is_absolute())
      return passwd_home;
  }

  return std::filesystem::path("/nonexistent");
}

std::filesystem::path env_path_or(std::string_view name, std::filesystem::path const& fallback)
{
  std::string const key(name);
  char const* value = std::getenv(key.c_str());
  if (value == nullptr || std::string_view(value).empty())
    return fallback;
  auto path = std::filesystem::path(value).lexically_normal();
  if (!path.is_absolute())
    return fallback;
  return path;
}

}  // namespace

XdgPaths xdg_paths()
{
  auto const home = home_dir();
  auto const config_home = env_path_or("XDG_CONFIG_HOME", home / ".config");
  auto const state_home = env_path_or("XDG_STATE_HOME", home / ".local" / "state");
  auto const data_home = env_path_or("XDG_DATA_HOME", home / ".local" / "share");
  auto const ava_config = config_home / "ava";
  auto const ava_state = state_home / "ava";
  return XdgPaths{
      .config_home = config_home,
      .state_home = state_home,
      .data_home = data_home,
      .ava_config_dir = ava_config,
      .ava_state_dir = ava_state,
      .auth_file = ava_config / "auth.json",
      .compaction_file = ava_config / "compaction.json",
      .global_agents_file = ava_config / "AGENTS.md",
      .models_file = ava_config / "models.json",
      .providers_file = ava_config / "providers.json",
      .prompts_dir = ava_config / "prompts",
      .sessions_dir = ava_state / "sessions",
  };
}

std::filesystem::path legacy_ava_credentials_path()
{
  return home_dir() / ".ava" / "credentials.json";
}

std::filesystem::path legacy_compatible_auth_path()
{
  return xdg_paths().data_home / "opencode" / "auth.json";
}

}  // namespace ava::config

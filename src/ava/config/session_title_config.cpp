#include "sys.h"
#include "ava/config/session_title_config.h"
#include "ava/core/AnchorOpen.h"
#include "ava/core/ascii.h"
#include "ava/core/strict_json.h"

#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <set>
#include <string>
#include <system_error>
#include <utility>
#include <fcntl.h>
#include <nlohmann/json.hpp>
#include <sys/stat.h>
#include <unistd.h>

namespace ava::config {
namespace {

constexpr std::size_t kMaxSessionTitleConfigBytes = 16 * 1024;
constexpr std::size_t kMaxTargetIdBytes = 256;
using Json = nlohmann::json;

ava::core::Error config_error(std::string message, std::string_view field = {})
{
  auto error = ava::core::Error(ava::core::ErrorCategory::Configuration, std::move(message));
  if (!field.empty())
    error.with_context("field", std::string(field));
  return error;
}

ava::core::Result<std::string> read_config(std::filesystem::path const& path, ava::core::AnchorSet const& anchors)
{
  auto opened = ava::core::open_readable(anchors, path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
  if (!opened)
    return std::unexpected(std::move(opened.error()));

  struct stat status{};
  if (::fstat(opened->fd(), &status) != 0)
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Io, "failed to inspect session title config descriptor")
                               .with_context("path", path.string())
                               .with_context("cause", std::generic_category().message(errno)));
  if (!S_ISREG(status.st_mode) || status.st_uid != ::geteuid() || status.st_nlink != 1 || (status.st_mode & 0022) != 0)
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Configuration, "session title config must be an owner-controlled regular file")
                               .with_context("path", path.string()));
  if (status.st_size < 0 || static_cast<std::uintmax_t>(status.st_size) > kMaxSessionTitleConfigBytes)
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Configuration, "session title config is too large")
                               .with_context("path", path.string())
                               .with_context("max_bytes", std::to_string(kMaxSessionTitleConfigBytes)));

  std::string content;
  content.reserve(static_cast<std::size_t>(status.st_size));
  std::array<char, 4096> buffer{};
  for (;;)
  {
    auto const count = ::read(opened->fd(), buffer.data(), buffer.size());
    if (count == 0)
      break;
    if (count < 0)
    {
      if (errno == EINTR)
        continue;
      return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Io, "failed to read bounded session title config")
                                 .with_context("path", path.string())
                                 .with_context("cause", std::generic_category().message(errno)));
    }
    if (static_cast<std::size_t>(count) > kMaxSessionTitleConfigBytes - content.size())
      return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Configuration, "session title config is too large")
                                 .with_context("path", path.string())
                                 .with_context("max_bytes", std::to_string(kMaxSessionTitleConfigBytes)));
    content.append(buffer.data(), static_cast<std::size_t>(count));
  }
  return content;
}

ava::core::VoidResult validate_target(std::optional<std::string> const& value, std::string_view field)
{
  if (!value)
    return {};
  if (value->empty() || value->size() > kMaxTargetIdBytes || ava::core::has_c0_or_del(*value))
    return std::unexpected(config_error("session title target is invalid", field));
  return {};
}

}  // namespace

SessionTitleConfig default_session_title_config()
{
  return {};
}

ava::core::Result<SessionTitleConfig> parse_session_title_config(std::string_view content)
{
  if (content.size() > kMaxSessionTitleConfigBytes)
    return std::unexpected(config_error("session title config is too large"));
  auto const strict = ava::core::validate_strict_json(content, 8);
  if (strict != ava::core::StrictJsonStatus::Valid)
    return std::unexpected(config_error(strict == ava::core::StrictJsonStatus::DuplicateObjectKey ? "session title config contains a duplicate member"
                                                                                                  : "session title config is not valid bounded JSON"));

  Json root;
  try
  {
    root = Json::parse(content);
  }
  catch (...)
  {
    return std::unexpected(config_error("session title config is not valid JSON"));
  }
  if (!root.is_object())
    return std::unexpected(config_error("session title config must be an object"));
  static std::set<std::string> const allowed{"schema_version", "enabled", "provider", "model"};
  for (auto const& [key, value] : root.items())
  {
    (void)value;
    if (!allowed.contains(key))
      return std::unexpected(config_error("session title config contains an unsupported member", key));
  }
  if (!root.contains("schema_version") || !root["schema_version"].is_number_integer() || root["schema_version"].get<long long>() != 1)
    return std::unexpected(config_error("session title config requires schema_version 1", "schema_version"));

  SessionTitleConfig config;
  if (root.contains("enabled"))
  {
    if (!root["enabled"].is_boolean())
      return std::unexpected(config_error("session title enabled must be a boolean", "enabled"));
    config.enabled = root["enabled"].get<bool>();
  }
  auto read_string = [&](std::string_view key) -> ava::core::Result<std::optional<std::string>> {
    auto const name = std::string(key);
    if (!root.contains(name))
      return std::optional<std::string>{};
    if (!root[name].is_string())
      return std::unexpected(config_error("session title target must be a string", key));
    return std::optional<std::string>(root[name].get<std::string>());
  };
  auto provider = read_string("provider");
  if (!provider)
    return std::unexpected(std::move(provider.error()));
  auto model = read_string("model");
  if (!model)
    return std::unexpected(std::move(model.error()));
  config.provider_id = std::move(*provider);
  config.model_id = std::move(*model);
  if (auto valid = validate_target(config.provider_id, "provider"); !valid)
    return std::unexpected(std::move(valid.error()));
  if (auto valid = validate_target(config.model_id, "model"); !valid)
    return std::unexpected(std::move(valid.error()));
  if (config.provider_id && !config.model_id)
    return std::unexpected(config_error("session title provider requires an explicit model", "provider"));
  return config;
}

ava::core::Result<SessionTitleConfig> load_session_title_config(XdgPaths const& paths, ava::core::AnchorSet const& anchors)
{
  if (auto const* override_value = std::getenv("AVA_SESSION_TITLES"); override_value != nullptr)
  {
    auto const value = std::string_view(override_value);
    if (value == "off")
      return SessionTitleConfig{.enabled = false};
    if (value != "on")
      return std::unexpected(config_error("AVA_SESSION_TITLES must be 'on' or 'off'", "AVA_SESSION_TITLES"));
  }
  auto const path = paths.ava_config_dir / "session-titles.json";
  std::error_code exists_error;
  bool const exists = std::filesystem::exists(path, exists_error);
  if (exists_error)
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Io, "failed to inspect session title config").with_context("path", path.string()));
  if (!exists)
    return default_session_title_config();
  auto content = read_config(path, anchors);
  if (!content)
    return std::unexpected(std::move(content.error()));
  auto parsed = parse_session_title_config(*content);
  if (!parsed)
    parsed.error().with_context("path", path.string());
  return parsed;
}

}  // namespace ava::config

#include "sys.h"
#include "ava/config/provider_config_internal.h"
#include "ava/core/UniqueFd.h"

#include <array>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace ava::config {
namespace provider_config_detail {

// nullopt => path does not exist. empty string => present zero-byte file.
ava::core::Result<std::optional<std::string>> read_providers_file(std::filesystem::path const& path)
{
  int flags = O_RDONLY | O_CLOEXEC | O_NONBLOCK;
#ifdef O_NOFOLLOW
  flags |= O_NOFOLLOW;
#endif
  ava::core::UniqueFd fd(::open(path.c_str(), flags));
  if (fd.get() < 0)
  {
    if (errno == ENOENT)
      return std::optional<std::string>{};
    auto category = (errno == EACCES || errno == EPERM || errno == ELOOP) ? ava::core::ErrorCategory::PermissionDenied : ava::core::ErrorCategory::Io;
    auto error = path_error(category, "failed to open providers config", path, "open");
    error.with_context("cause", errno_message());
    return std::unexpected(std::move(error));
  }

  struct stat status{};
  if (::fstat(fd.get(), &status) != 0)
  {
    auto error = path_error(ava::core::ErrorCategory::Io, "failed to inspect providers config descriptor", path, "fstat");
    error.with_context("cause", errno_message());
    return std::unexpected(std::move(error));
  }
  if (!S_ISREG(status.st_mode))
    return std::unexpected(path_error(ava::core::ErrorCategory::Configuration, "providers config must be a regular file", path, "validate"));
  if (status.st_uid != ::geteuid())
    return std::unexpected(path_error(ava::core::ErrorCategory::PermissionDenied, "providers config must be owned by the effective user", path, "validate"));
  if (status.st_nlink != 1)
    return std::unexpected(path_error(ava::core::ErrorCategory::PermissionDenied, "providers config must not be hard-linked", path, "validate"));
  if ((status.st_mode & 0022) != 0)
    return std::unexpected(path_error(ava::core::ErrorCategory::PermissionDenied, "providers config must not be group- or world-writable", path, "validate"));
  if (status.st_size < 0 || static_cast<std::uintmax_t>(status.st_size) > kMaxUserProviderConfigBytes)
  {
    auto error = path_error(ava::core::ErrorCategory::Configuration, "providers config is too large", path, "validate");
    error.with_context("max_bytes", std::to_string(kMaxUserProviderConfigBytes));
    return std::unexpected(std::move(error));
  }

  std::string content;
  content.reserve(static_cast<std::size_t>(status.st_size));
  std::array<char, 4096> buffer{};
  for (;;)
  {
    auto const count = ::read(fd.get(), buffer.data(), buffer.size());
    if (count == 0)
      break;
    if (count < 0)
    {
      if (errno == EINTR)
        continue;
      auto error = path_error(ava::core::ErrorCategory::Io, "failed to read providers config", path, "read");
      error.with_context("cause", errno_message());
      return std::unexpected(std::move(error));
    }
    if (static_cast<std::size_t>(count) > kMaxUserProviderConfigBytes - content.size())
    {
      auto error = path_error(ava::core::ErrorCategory::Configuration, "providers config is too large", path, "read");
      error.with_context("max_bytes", std::to_string(kMaxUserProviderConfigBytes));
      return std::unexpected(std::move(error));
    }
    content.append(buffer.data(), static_cast<std::size_t>(count));
  }
  return std::optional<std::string>(std::move(content));
}

}  // namespace provider_config_detail

ava::core::Result<std::vector<UserProviderDefinition>> load_user_provider_definitions(XdgPaths const& paths)
{
  using provider_config_detail::config_error;
  using provider_config_detail::read_providers_file;

  auto const& path = paths.providers_file;
  if (path.empty())
    return std::unexpected(config_error("providers config path is not configured", "providers_file"));

  auto content = read_providers_file(path);
  if (!content)
    return std::unexpected(std::move(content.error()));
  if (!*content)
    return std::vector<UserProviderDefinition>{};

  auto parsed = parse_user_provider_definitions(**content);
  if (!parsed)
  {
    parsed.error().with_context("path", path.string());
    return std::unexpected(std::move(parsed.error()));
  }
  return parsed;
}

}  // namespace ava::config

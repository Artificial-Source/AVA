#include "sys.h"
#include "ava/context/markdown_resource.h"
#include "ava/core/UniqueFd.h"
#include "ava/core/string_utils.h"

#include <array>
#include <cerrno>
#include <cstring>
#include <utility>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace ava::context {
namespace {

ava::core::Error resource_io_error(std::string message, std::filesystem::path const& path, int error_number = 0)
{
  auto error = ava::core::Error(ava::core::ErrorCategory::Io, std::move(message));
  error.with_context("path", path.string());
  if (error_number != 0)
    error.with_context("cause", std::strerror(error_number));
  return error;
}

std::string describe_not_regular(std::string_view resource_description)
{
  return std::string(resource_description) + " is not a regular file";
}

std::string describe_too_large(std::string_view resource_description)
{
  return std::string(resource_description) + " is too large";
}

std::string describe_open_failed(std::string_view resource_description)
{
  return "failed to open " + std::string(resource_description);
}

std::string describe_read_failed(std::string_view resource_description)
{
  return "failed while reading " + std::string(resource_description);
}

}  // namespace

ava::core::Result<std::string> read_resource_file(std::filesystem::path const& path, ResourceFileReadOptions options)
{
  ava::core::UniqueFd file(::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK | O_NOFOLLOW));
  if (file.get() < 0)
  {
    if (errno == ELOOP || errno == EISDIR || errno == ENXIO || errno == EOPNOTSUPP)
      return std::unexpected(resource_io_error(describe_not_regular(options.resource_description), path, errno));
    return std::unexpected(resource_io_error(describe_open_failed(options.resource_description), path, errno));
  }

  struct stat status{};
  if (::fstat(file.get(), &status) != 0)
    return std::unexpected(resource_io_error(describe_open_failed(options.resource_description), path, errno));
  if (!S_ISREG(status.st_mode))
    return std::unexpected(resource_io_error(describe_not_regular(options.resource_description), path));
  if (status.st_size < 0 || static_cast<std::uintmax_t>(status.st_size) > options.max_bytes)
  {
    auto error = resource_io_error(describe_too_large(options.resource_description), path);
    error.with_context("max_bytes", std::to_string(options.max_bytes));
    return std::unexpected(std::move(error));
  }

  std::string content;
  content.reserve(static_cast<std::size_t>(status.st_size));
  std::array<char, 4096> buffer{};
  while (true)
  {
    auto const count = ::read(file.get(), buffer.data(), buffer.size());
    if (count == 0)
      break;
    if (count < 0)
    {
      if (errno == EINTR)
        continue;
      return std::unexpected(resource_io_error(describe_read_failed(options.resource_description), path, errno));
    }
    if (content.size() > options.max_bytes || static_cast<std::size_t>(count) > options.max_bytes - content.size())
    {
      auto error = resource_io_error(describe_too_large(options.resource_description), path);
      error.with_context("max_bytes", std::to_string(options.max_bytes));
      return std::unexpected(std::move(error));
    }
    content.append(buffer.data(), static_cast<std::size_t>(count));
  }
  return content;
}

ParsedMarkdown parse_markdown(std::string_view content)
{
  ParsedMarkdown parsed;
  if (!(content.starts_with("---\n") || content.starts_with("---\r\n")))
  {
    parsed.body = std::string(content);
    return parsed;
  }

  auto const body_start = content.starts_with("---\r\n") ? 5 : 4;
  auto const delimiter = content.find("\n---", body_start);
  if (delimiter == std::string_view::npos)
  {
    parsed.body = std::string(content);
    return parsed;
  }

  auto const frontmatter = content.substr(body_start, delimiter - body_start);
  std::size_t line_start = 0;
  while (line_start <= frontmatter.size())
  {
    auto const line_end = frontmatter.find('\n', line_start);
    auto line = frontmatter.substr(line_start, line_end == std::string_view::npos ? std::string_view::npos : line_end - line_start);
    if (!line.empty() && line.back() == '\r')
      line.remove_suffix(1);
    if (auto const colon = line.find(':'); colon != std::string_view::npos)
    {
      auto key = core::trim(line.substr(0, colon));
      auto value = core::strip_matching_quotes(core::trim_view(line.substr(colon + 1)));
      if (!key.empty())
        parsed.frontmatter[std::move(key)] = value;
    }
    if (line_end == std::string_view::npos)
      break;
    line_start = line_end + 1;
  }

  auto after = delimiter + 4;
  if (after < content.size() && content[after] == '\r')
    ++after;
  if (after < content.size() && content[after] == '\n')
    ++after;
  parsed.body = std::string(content.substr(after));
  return parsed;
}

std::string markdown_field(ParsedMarkdown const& markdown, std::string_view name)
{
  auto const it = markdown.frontmatter.find(std::string(name));
  if (it == markdown.frontmatter.end())
    return {};
  return it->second;
}

}  // namespace ava::context

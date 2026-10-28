#include "sys.h"
#include "ava/config/provider_config_internal.h"
#include "ava/core/ascii.h"
#include "ava/core/json.h"

#include <algorithm>
#include <array>
#include <string>
#include <string_view>
#include <arpa/inet.h>

namespace ava::config::provider_config_detail {

bool is_unreserved_hostname_char(char ch) noexcept
{
  auto const byte = static_cast<unsigned char>(ch);
  return (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') || (byte >= '0' && byte <= '9') || ch == '-' || ch == '.' || ch == '_';
}

bool is_valid_hostname(std::string_view host) noexcept
{
  if (host.empty() || host.size() > 253 || ava::core::has_c0_or_del(host) || host.front() == '.' || host.back() == '.')
    return false;
  if (host.find("..") != std::string_view::npos)
    return false;
  for (char const ch : host)
  {
    if (!is_unreserved_hostname_char(ch))
      return false;
  }
  return true;
}

bool is_valid_ipv4(std::string_view host) noexcept
{
  if (host.empty() || host.size() >= INET_ADDRSTRLEN || host.find('\0') != std::string_view::npos)
    return false;
  std::array<char, INET_ADDRSTRLEN> text{};
  std::ranges::copy(host, text.begin());
  in_addr address{};
  return ::inet_pton(AF_INET, text.data(), &address) == 1;
}

bool is_valid_ipv6_literal(std::string_view host) noexcept
{
  if (host.size() < 4 || host.front() != '[' || host.back() != ']')
    return false;
  auto const inner = host.substr(1, host.size() - 2);
  if (inner.size() >= INET6_ADDRSTRLEN || inner.find('\0') != std::string_view::npos)
    return false;
  std::array<char, INET6_ADDRSTRLEN> text{};
  std::ranges::copy(inner, text.begin());
  in6_addr address{};
  return ::inet_pton(AF_INET6, text.data(), &address) == 1;
}

bool is_localhost_host(std::string_view host) noexcept
{
  return host == "localhost" || host == "127.0.0.1" || host == "[::1]";
}

bool path_has_dot_or_empty_segment(std::string_view path) noexcept
{
  if (path.empty())
    return false;
  std::size_t begin = 0;
  while (begin <= path.size())
  {
    auto const end = path.find('/', begin);
    auto const segment = path.substr(begin, end == std::string_view::npos ? path.size() - begin : end - begin);
    // Absolute paths start with '/', producing an initial empty segment; that alone is fine.
    if (begin != 0 || !segment.empty())
    {
      if (segment.empty() || segment == "." || segment == "..")
        return true;
    }
    if (end == std::string_view::npos)
      break;
    begin = end + 1;
  }
  return false;
}

ava::core::VoidResult validate_request_path(std::string_view path, std::string_view field)
{
  if (path.empty() || path.size() > kMaxRequestPathBytes)
    return std::unexpected(config_error("provider request_path length is invalid", field));
  if (path.front() != '/')
    return std::unexpected(config_error("provider request_path must be an absolute path", field));
  if (ava::core::has_c0_or_del(path) || path.find('\\') != std::string_view::npos || path.find('?') != std::string_view::npos ||
      path.find('#') != std::string_view::npos)
    return std::unexpected(config_error("provider request_path contains forbidden characters", field));
  if (has_encoded_separator_or_dot_ambiguity(path))
    return std::unexpected(config_error("provider request_path contains ambiguous percent-encoding", field));
  if (path_has_dot_or_empty_segment(path))
    return std::unexpected(config_error("provider request_path contains empty or dot segments", field));
  if (!ava::core::json::is_valid_utf8(path))
    return std::unexpected(config_error("provider request_path must be valid UTF-8", field));
  return {};
}

ava::core::Result<ParsedBaseUrl> parse_and_validate_base_url(std::string_view raw)
{
  if (raw.empty() || raw.size() > kMaxBaseUrlBytes)
    return std::unexpected(config_error("provider base_url length is invalid", "base_url"));
  if (ava::core::has_c0_or_del(raw))
    return std::unexpected(config_error("provider base_url contains control characters", "base_url"));
  if (raw.find('\\') != std::string_view::npos)
    return std::unexpected(config_error("provider base_url must not contain backslashes", "base_url"));
  if (raw.find('?') != std::string_view::npos)
    return std::unexpected(config_error("provider base_url must not contain a query", "base_url"));
  if (raw.find('#') != std::string_view::npos)
    return std::unexpected(config_error("provider base_url must not contain a fragment", "base_url"));
  if (has_encoded_separator_or_dot_ambiguity(raw))
    return std::unexpected(config_error("provider base_url contains ambiguous percent-encoding", "base_url"));

  auto const scheme_end = raw.find("://");
  if (scheme_end == std::string_view::npos || scheme_end == 0)
    return std::unexpected(config_error("provider base_url must include an http or https scheme", "base_url"));
  auto const scheme = raw.substr(0, scheme_end);
  bool const is_https = scheme == "https";
  bool const is_http = scheme == "http";
  if (!is_https && !is_http)
    return std::unexpected(config_error("provider base_url scheme must be http or https", "base_url"));

  auto const rest = raw.substr(scheme_end + 3);
  if (rest.empty())
    return std::unexpected(config_error("provider base_url is missing a host", "base_url"));

  // Reject userinfo: any '@' in the authority section.
  auto const path_start_rel = rest.find('/');
  auto const authority = path_start_rel == std::string_view::npos ? rest : rest.substr(0, path_start_rel);
  auto const base_path = path_start_rel == std::string_view::npos ? std::string_view{} : rest.substr(path_start_rel);
  if (authority.empty())
    return std::unexpected(config_error("provider base_url is missing a host", "base_url"));
  if (authority.find('@') != std::string_view::npos)
    return std::unexpected(config_error("provider base_url must not contain userinfo", "base_url"));

  std::string_view host;
  std::string_view port;
  if (!authority.empty() && authority.front() == '[')
  {
    auto const close = authority.find(']');
    if (close == std::string_view::npos)
      return std::unexpected(config_error("provider base_url has an invalid IPv6 host", "base_url"));
    host = authority.substr(0, close + 1);
    if (close + 1 < authority.size())
    {
      if (authority[close + 1] != ':')
        return std::unexpected(config_error("provider base_url has an invalid host/port", "base_url"));
      port = authority.substr(close + 2);
    }
    if (!is_valid_ipv6_literal(host))
      return std::unexpected(config_error("provider base_url has an invalid IPv6 host", "base_url"));
  }
  else
  {
    auto const colon = authority.rfind(':');
    if (colon != std::string_view::npos)
    {
      host = authority.substr(0, colon);
      port = authority.substr(colon + 1);
    }
    else
    {
      host = authority;
    }
    if (host.empty())
      return std::unexpected(config_error("provider base_url is missing a host", "base_url"));
    // Host must not itself contain percent-encoding (authority identity).
    if (host.find('%') != std::string_view::npos)
      return std::unexpected(config_error("provider base_url host must not be percent-encoded", "base_url"));
    bool const looks_like_ipv4 = host.find_first_not_of("0123456789.") == std::string_view::npos && host.find('.') != std::string_view::npos;
    if (looks_like_ipv4)
    {
      if (!is_valid_ipv4(host))
        return std::unexpected(config_error("provider base_url has an invalid IPv4 host", "base_url"));
    }
    else if (!is_valid_hostname(host))
    {
      return std::unexpected(config_error("provider base_url has an invalid host", "base_url"));
    }
  }

  if (!port.empty())
  {
    if (port.size() > 5)
      return std::unexpected(config_error("provider base_url has an invalid port", "base_url"));
    unsigned long value = 0;
    for (char const ch : port)
    {
      if (ch < '0' || ch > '9')
        return std::unexpected(config_error("provider base_url has an invalid port", "base_url"));
      value = value * 10UL + static_cast<unsigned long>(ch - '0');
      if (value > 65535UL)
        return std::unexpected(config_error("provider base_url has an invalid port", "base_url"));
    }
    if (value == 0UL)
      return std::unexpected(config_error("provider base_url has an invalid port", "base_url"));
  }
  else if (authority.back() == ':')
  {
    return std::unexpected(config_error("provider base_url has an invalid port", "base_url"));
  }

  if (is_http && !is_localhost_host(host))
    return std::unexpected(config_error("provider base_url may use http only for localhost, 127.0.0.1, or [::1]", "base_url"));

  // Trim trailing slashes from any base path before segment checks and joining.
  // A base of scheme://host/ becomes an empty path contribution.
  std::string_view normalized_base_path = base_path;
  while (!normalized_base_path.empty() && normalized_base_path.back() == '/') normalized_base_path.remove_suffix(1);

  if (!normalized_base_path.empty())
  {
    if (auto valid = validate_request_path(normalized_base_path, "base_url"); !valid)
      return std::unexpected(std::move(valid.error()));
  }

  std::string canonical;
  canonical.reserve(raw.size());
  canonical.append(scheme);
  canonical.append("://");
  canonical.append(host);
  if (!port.empty())
  {
    canonical.push_back(':');
    canonical.append(port);
  }
  if (!normalized_base_path.empty())
    canonical.append(normalized_base_path);

  // Final trailing-slash trim of the entire base (defensive; path already trimmed).
  while (!canonical.empty() && canonical.back() == '/') canonical.pop_back();

  // Re-check the scheme://host minimum survived trimming.
  if (canonical.find("://") == std::string::npos)
    return std::unexpected(config_error("provider base_url is invalid after normalization", "base_url"));

  return ParsedBaseUrl{.canonical_base = std::move(canonical)};
}

std::string join_endpoint(std::string_view base_url, std::string_view request_path)
{
  std::string endpoint;
  endpoint.reserve(base_url.size() + request_path.size());
  endpoint.assign(base_url);
  // base_url is already trimmed of trailing slashes; request_path is absolute.
  endpoint.append(request_path);
  return endpoint;
}

}  // namespace ava::config::provider_config_detail

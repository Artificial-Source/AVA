#include "sys.h"
#include "ava/tools/webfetch_tool.h"
#include "ava/core/string_utils.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstring>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>

namespace ava::tools {
namespace {

constexpr std::size_t kMaxWebFetchBytes = 5 * 1024 * 1024;
constexpr int kMaxWebFetchTimeoutMs = 120000;

struct ValidatedUrl
{
  std::string url;
  std::string host;
  std::string port;
};

std::string lowercase(std::string_view value)
{
  std::string result(value);
  std::ranges::transform(result, result.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
  return result;
}

bool starts_with_case_insensitive(std::string_view value, std::string_view prefix)
{
  if (value.size() < prefix.size())
    return false;
  for (std::size_t index = 0; index < prefix.size(); ++index)
  {
    if (std::tolower(static_cast<unsigned char>(value[index])) != std::tolower(static_cast<unsigned char>(prefix[index])))
    {
      return false;
    }
  }
  return true;
}

bool all_decimal_digits(std::string_view value)
{
  return !value.empty() && std::ranges::all_of(value, [](unsigned char ch) { return std::isdigit(ch); });
}

bool hex_literal(std::string_view value)
{
  if (value.size() <= 2 || value[0] != '0' || (value[1] != 'x' && value[1] != 'X'))
    return false;
  return std::ranges::all_of(value.substr(2), [](unsigned char ch) { return std::isxdigit(ch); });
}

bool numeric_ipv4_literal_or_alias(std::string_view host)
{
  if (host.empty() || !std::isdigit(static_cast<unsigned char>(host.front())))
    return false;
  if (all_decimal_digits(host) || hex_literal(host))
    return true;
  if (host.find('.') == std::string_view::npos)
    return false;
  std::size_t part_start = 0;
  while (part_start <= host.size())
  {
    auto const dot = host.find('.', part_start);
    auto const part = host.substr(part_start, dot == std::string_view::npos ? std::string_view::npos : dot - part_start);
    if (!all_decimal_digits(part) && !hex_literal(part))
      return false;
    if (dot == std::string_view::npos)
      break;
    part_start = dot + 1;
  }
  return true;
}

bool private_or_non_global_ipv4(unsigned long address)
{
  auto const first = static_cast<unsigned char>((address >> 24) & 0xFF);
  auto const second = static_cast<unsigned char>((address >> 16) & 0xFF);
  auto const third = static_cast<unsigned char>((address >> 8) & 0xFF);
  return first == 0 || first == 10 || first == 127 || first >= 224 || (first == 100 && second >= 64 && second <= 127) || (first == 169 && second == 254) ||
         (first == 172 && second >= 16 && second <= 31) || (first == 192 && second == 168) || (first == 192 && second == 0 && third == 0) ||
         (first == 192 && second == 0 && third == 2) || (first == 198 && second >= 18 && second <= 19) || (first == 198 && second == 51 && third == 100) ||
         (first == 203 && second == 0 && third == 113);
}

bool private_or_non_global_ipv6(in6_addr const& address)
{
  auto const* bytes = address.s6_addr;
  bool const ipv4_mapped = std::ranges::all_of(std::span(bytes, 10), [](unsigned char byte) { return byte == 0; }) && bytes[10] == 0xFF && bytes[11] == 0xFF;
  bool const nat64_well_known = bytes[0] == 0x00 && bytes[1] == 0x64 && bytes[2] == 0xFF && bytes[3] == 0x9B &&
                                std::ranges::all_of(std::span(bytes + 4, 8), [](unsigned char byte) { return byte == 0; });
  bool const nat64_local_use = bytes[0] == 0x00 && bytes[1] == 0x64 && bytes[2] == 0xFF && bytes[3] == 0x9B && bytes[4] == 0x00 && bytes[5] == 0x01;
  bool const unspecified = std::ranges::all_of(std::span(bytes, 16), [](unsigned char byte) { return byte == 0; });
  bool const loopback = std::ranges::all_of(std::span(bytes, 15), [](unsigned char byte) { return byte == 0; }) && bytes[15] == 1;
  return ipv4_mapped || nat64_well_known || nat64_local_use || unspecified || loopback || bytes[0] == 0xFF || (bytes[0] & 0xFE) == 0xFC ||
         (bytes[0] == 0xFE && (bytes[1] & 0xC0) == 0x80) || (bytes[0] == 0xFE && (bytes[1] & 0xC0) == 0xC0) ||
         (bytes[0] == 0x20 && bytes[1] == 0x01 && bytes[2] == 0x0D && bytes[3] == 0xB8);
}

ava::core::Result<std::string> validate_resolved_host(std::string_view host)
{
  addrinfo hints{};
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_family = AF_UNSPEC;
  addrinfo* raw_results = nullptr;
  auto const query_host = std::string(host);
  int const rc = getaddrinfo(query_host.c_str(), nullptr, &hints, &raw_results);
  if (rc != 0)
  {
    auto error = ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "webfetch failed to resolve URL host");
    error.with_context("host", query_host);
    error.with_context("cause", gai_strerror(rc));
    return std::unexpected(std::move(error));
  }
  std::unique_ptr<addrinfo, decltype(&freeaddrinfo)> results(raw_results, freeaddrinfo);
  std::string first_global_address;
  for (auto const* item = results.get(); item != nullptr; item = item->ai_next)
  {
    if (item->ai_family == AF_INET)
    {
      auto address = reinterpret_cast<sockaddr_in const*>(item->ai_addr)->sin_addr;
      auto const host_order = ntohl(address.s_addr);
      if (private_or_non_global_ipv4(host_order))
      {
        auto error = ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "webfetch blocks private or non-global resolved hosts");
        error.with_context("host", query_host);
        return std::unexpected(std::move(error));
      }
      if (first_global_address.empty())
      {
        std::array<char, INET_ADDRSTRLEN> text{};
        if (inet_ntop(AF_INET, &address, text.data(), text.size()) != nullptr)
          first_global_address = text.data();
      }
    }
    else if (item->ai_family == AF_INET6)
    {
      auto const& address = reinterpret_cast<sockaddr_in6 const*>(item->ai_addr)->sin6_addr;
      if (private_or_non_global_ipv6(address))
      {
        auto error = ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "webfetch blocks private or non-global resolved hosts");
        error.with_context("host", query_host);
        return std::unexpected(std::move(error));
      }
      if (first_global_address.empty())
      {
        std::array<char, INET6_ADDRSTRLEN> text{};
        if (inet_ntop(AF_INET6, &address, text.data(), text.size()) != nullptr)
        {
          first_global_address = "[";
          first_global_address += text.data();
          first_global_address += "]";
        }
      }
    }
  }
  if (first_global_address.empty())
  {
    auto error = ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "webfetch host has no global address");
    error.with_context("host", query_host);
    return std::unexpected(std::move(error));
  }
  return first_global_address;
}

ava::core::Result<ValidatedUrl> validated_url(std::string_view url)
{
  if (url.empty() || url.size() > 4096)
  {
    auto error = ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "webfetch URL is empty or too long");
    error.with_context("max_bytes", "4096");
    return std::unexpected(std::move(error));
  }
  for (char const ch : url)
  {
    auto const byte = static_cast<unsigned char>(ch);
    if (byte < 0x20 || byte == 0x7F)
    {
      return std::unexpected(ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "webfetch URL contains a control byte"));
    }
  }

  bool const https = starts_with_case_insensitive(url, "https://");
  bool const http = starts_with_case_insensitive(url, "http://");
  if (!https && !http)
  {
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "webfetch only supports http and https URLs"));
  }
  auto const authority_start = https ? std::string_view("https://").size() : std::string_view("http://").size();
  auto const path_start = url.find_first_of("/?#", authority_start);
  auto authority = url.substr(authority_start, path_start == std::string_view::npos ? std::string_view::npos : path_start - authority_start);
  if (authority.empty())
  {
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "webfetch URL host is empty"));
  }
  if (authority.find('@') != std::string_view::npos)
  {
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "webfetch URL must not include userinfo"));
  }
  if (!authority.empty() && authority.front() == '[')
  {
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "webfetch does not allow IP literal hosts"));
  }
  std::string port = https ? "443" : "80";
  if (auto const colon = authority.rfind(':'); colon != std::string_view::npos)
  {
    port = std::string(authority.substr(colon + 1));
    if (port.empty() || !std::ranges::all_of(port, [](unsigned char ch) { return std::isdigit(ch); }))
    {
      return std::unexpected(ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "webfetch URL port is invalid"));
    }
    authority = authority.substr(0, colon);
  }

  auto const host = lowercase(authority);
  if (host == "localhost" || host == "localhost." || host.ends_with(".localhost") || host.ends_with(".local"))
  {
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "webfetch blocks local hostnames"));
  }
  if (numeric_ipv4_literal_or_alias(host))
  {
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "webfetch does not allow IP literal hosts"));
  }

  int octets[4] = {0, 0, 0, 0};
  std::size_t part_start = 0;
  bool ipv4 = true;
  for (int part = 0; part < 4; ++part)
  {
    auto const dot = part == 3 ? host.size() : host.find('.', part_start);
    if (dot == std::string::npos || dot == part_start)
    {
      ipv4 = false;
      break;
    }
    auto const text = std::string_view(host).substr(part_start, dot - part_start);
    int value = 0;
    auto const [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (ec != std::errc{} || ptr != text.data() + text.size() || value < 0 || value > 255)
    {
      ipv4 = false;
      break;
    }
    octets[part] = value;
    part_start = dot + 1;
  }
  if (ipv4 && part_start == host.size() + 1)
  {
    bool const private_or_local = octets[0] == 10 || octets[0] == 127 || octets[0] == 0 || (octets[0] == 169 && octets[1] == 254) ||
                                  (octets[0] == 172 && octets[1] >= 16 && octets[1] <= 31) || (octets[0] == 192 && octets[1] == 168);
    if (private_or_local)
    {
      return std::unexpected(ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "webfetch blocks private IP hosts"));
    }
  }

  return ValidatedUrl{.url = std::string(url), .host = host, .port = std::move(port)};
}

std::string header_value(std::map<std::string, std::string> const& headers, std::string_view name)
{
  auto const wanted = lowercase(name);
  for (auto const& [key, value] : headers)
  {
    if (lowercase(key) == wanted)
      return value;
  }
  return {};
}

bool looks_binary(std::string_view body, std::string_view content_type)
{
  auto const type = lowercase(content_type);
  if (!type.empty() &&
      !(type.starts_with("text/") || type.find("json") != std::string::npos || type.find("xml") != std::string::npos ||
        type.find("html") != std::string::npos || type.find("javascript") != std::string::npos || type.find("ecmascript") != std::string::npos))
  {
    return true;
  }
  return body.find('\0') != std::string_view::npos;
}

std::string accept_header(WebFetchFormat format)
{
  switch (format)
  {
    case WebFetchFormat::Markdown:
      return "text/markdown;q=1.0,text/x-markdown;q=0.9,text/plain;q=0.8,text/html;q=0.7,*/*;q=0.1";
    case WebFetchFormat::Text:
      return "text/plain;q=1.0,text/markdown;q=0.9,text/html;q=0.8,*/*;q=0.1";
    case WebFetchFormat::Html:
      return "text/html;q=1.0,application/xhtml+xml;q=0.9,text/plain;q=0.8,*/*;q=0.1";
  }
  return "text/html,text/plain,application/json,application/xml;q=0.9,*/*;q=0.1";
}

std::string html_entity_decode(std::string_view text)
{
  std::string out;
  out.reserve(text.size());
  for (std::size_t index = 0; index < text.size();)
  {
    if (text[index] != '&')
    {
      out.push_back(text[index++]);
      continue;
    }
    if (text.substr(index, 5) == "&amp;")
    {
      out.push_back('&');
      index += 5;
    }
    else if (text.substr(index, 4) == "&lt;")
    {
      out.push_back('<');
      index += 4;
    }
    else if (text.substr(index, 4) == "&gt;")
    {
      out.push_back('>');
      index += 4;
    }
    else if (text.substr(index, 6) == "&quot;")
    {
      out.push_back('"');
      index += 6;
    }
    else if (text.substr(index, 5) == "&#39;")
    {
      out.push_back('\'');
      index += 5;
    }
    else if (text.substr(index, 6) == "&nbsp;")
    {
      out.push_back(' ');
      index += 6;
    }
    else
    {
      out.push_back(text[index++]);
    }
  }
  return out;
}

bool tag_starts_with(std::string_view tag, std::string_view name)
{
  if (tag.size() < name.size())
    return false;
  for (std::size_t index = 0; index < name.size(); ++index)
  {
    if (std::tolower(static_cast<unsigned char>(tag[index])) != std::tolower(static_cast<unsigned char>(name[index])))
    {
      return false;
    }
  }
  return tag.size() == name.size() || std::isspace(static_cast<unsigned char>(tag[name.size()])) != 0 || tag[name.size()] == '>' || tag[name.size()] == '/';
}

std::string html_to_text(std::string_view html, bool markdown)
{
  std::string out;
  out.reserve(html.size());
  bool in_tag = false;
  bool skipping = false;
  std::string tag;
  for (std::size_t index = 0; index < html.size(); ++index)
  {
    char const ch = html[index];
    if (in_tag)
    {
      if (ch == '>')
      {
        auto const trimmed = lowercase(tag);
        bool const closing = !trimmed.empty() && trimmed.front() == '/';
        auto const name = closing ? std::string_view(trimmed).substr(1) : std::string_view(trimmed);
        if (!closing && (tag_starts_with(name, "script") || tag_starts_with(name, "style") || tag_starts_with(name, "noscript")))
        {
          skipping = true;
        }
        else if (closing && (tag_starts_with(name, "script") || tag_starts_with(name, "style") || tag_starts_with(name, "noscript")))
        {
          skipping = false;
        }
        else if (!skipping)
        {
          if (tag_starts_with(name, "br") || tag_starts_with(name, "p") || tag_starts_with(name, "div") || tag_starts_with(name, "li") ||
              tag_starts_with(name, "tr"))
          {
            out += '\n';
          }
          else if (markdown && !closing && (tag_starts_with(name, "h1") || tag_starts_with(name, "h2") || tag_starts_with(name, "h3")))
          {
            out += "\n\n";
            if (tag_starts_with(name, "h1"))
              out += "# ";
            if (tag_starts_with(name, "h2"))
              out += "## ";
            if (tag_starts_with(name, "h3"))
              out += "### ";
          }
        }
        tag.clear();
        in_tag = false;
        continue;
      }
      tag.push_back(ch);
      continue;
    }
    if (ch == '<')
    {
      in_tag = true;
      tag.clear();
      continue;
    }
    if (!skipping)
      out.push_back(ch);
  }

  auto decoded = html_entity_decode(out);
  std::string compact;
  compact.reserve(decoded.size());
  int newlines = 0;
  bool pending_space = false;
  for (char const ch : decoded)
  {
    if (ch == '\r')
      continue;
    if (ch == '\n')
    {
      pending_space = false;
      if (newlines < 2)
        compact.push_back('\n');
      ++newlines;
      continue;
    }
    if (std::isspace(static_cast<unsigned char>(ch)) != 0)
    {
      pending_space = true;
      continue;
    }
    if (pending_space && !compact.empty() && compact.back() != '\n')
      compact.push_back(' ');
    pending_space = false;
    newlines = 0;
    compact.push_back(ch);
  }
  return core::trim(std::string_view(compact));
}

bool is_html_content(std::string_view content_type)
{
  return lowercase(content_type).find("html") != std::string::npos;
}

std::size_t logical_line_count(std::string_view text)
{
  if (text.empty())
    return 0;
  auto const newline_count = static_cast<std::size_t>(std::ranges::count(text, '\n'));
  return text.back() == '\n' ? newline_count : newline_count + 1;
}

std::size_t byte_offset_for_line(std::string_view text, std::size_t line)
{
  if (line <= 1)
    return 0;
  std::size_t current_line = 1;
  for (std::size_t index = 0; index < text.size(); ++index)
  {
    if (text[index] != '\n')
      continue;
    ++current_line;
    if (current_line == line)
      return index + 1;
  }
  return text.size();
}

void trim_partial_final_line(std::string& text)
{
  auto const newline = text.find_last_of('\n');
  if (newline == std::string::npos || newline + 1 == text.size())
    return;
  text.resize(newline + 1);
}

void apply_line_window(WebFetchResult& result, std::string_view text, WebFetchOptions const& options, std::size_t max_bytes)
{
  result.total_bytes = text.size();
  result.total_lines = logical_line_count(text);
  result.start_line = options.offset_line == 0 ? 1 : options.offset_line;
  auto const end_line_exclusive = options.max_lines == 0 || result.start_line > std::numeric_limits<std::size_t>::max() - options.max_lines
                                      ? std::numeric_limits<std::size_t>::max()
                                      : result.start_line + options.max_lines;

  auto const start_offset = byte_offset_for_line(text, result.start_line);
  auto const end_offset = end_line_exclusive == std::numeric_limits<std::size_t>::max() ? text.size() : byte_offset_for_line(text, end_line_exclusive);
  auto const selected = text.substr(start_offset, end_offset - start_offset);
  result.content = std::string(selected.substr(0, std::min(max_bytes, selected.size())));
  if (result.content.size() < selected.size())
    trim_partial_final_line(result.content);

  result.output_bytes = result.content.size();
  result.output_lines = logical_line_count(result.content);
  result.end_line = result.output_lines > 0 ? result.start_line + result.output_lines - 1 : 0;
  result.byte_limited = result.output_bytes < selected.size();
  result.line_limited = options.max_lines > 0 && end_line_exclusive != std::numeric_limits<std::size_t>::max() && result.total_lines >= end_line_exclusive;
  result.truncated = result.byte_limited || result.line_limited;
  if (result.line_limited && !result.byte_limited && result.end_line > 0 && result.end_line < result.total_lines)
  {
    result.next_offset_line = result.end_line + 1;
  }
}

}  // namespace

ava::core::Result<WebFetchResult> webfetch(ToolContext const& context, std::string_view url, WebFetchOptions options)
{
  if (context.cancel_requested && context.cancel_requested())
  {
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Unknown, "tool canceled", ava::core::ErrorCode::Canceled));
  }
  auto safe_url = validated_url(url);
  if (!safe_url)
    return std::unexpected(std::move(safe_url.error()));

  if (auto permission =
          ensure_permission(context, ava::permissions::Operation::NetworkFetch, {}, safe_url->url, "webfetch", "network fetch requires permission");
      !permission)
  {
    return std::unexpected(std::move(permission.error()));
  }
  if (context.cancel_requested && context.cancel_requested())
  {
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Unknown, "tool canceled", ava::core::ErrorCode::Canceled));
  }

  auto const max_bytes = std::min(options.max_bytes == 0 ? std::size_t{1024 * 1024} : options.max_bytes, kMaxWebFetchBytes);
  auto const timeout_ms = std::clamp(options.timeout_ms <= 0 ? 30000 : options.timeout_ms, 1000, kMaxWebFetchTimeoutMs);

  std::vector<std::string> resolve_hosts;
  if (options.transport == nullptr)
  {
    auto resolved = validate_resolved_host(safe_url->host);
    if (!resolved)
      return std::unexpected(std::move(resolved.error()));
    resolve_hosts.push_back(safe_url->host + ":" + safe_url->port + ":" + *resolved);
  }

  std::unique_ptr<ava::http::Transport> owned_transport;
  ava::http::Transport* transport = options.transport;
  if (transport == nullptr)
  {
    if (!context.transport_factory)
    {
      return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Configuration, "webfetch transport process authority is unavailable"));
    }
    auto created = context.transport_factory();
    if (!created)
      return std::unexpected(std::move(created.error()));
    if (!*created)
      return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Configuration, "webfetch transport is unavailable"));
    owned_transport = std::move(*created);
    transport = owned_transport.get();
  }
  auto response = transport->send(ava::http::HttpRequest{.method = "GET",
                                                         .url = safe_url->url,
                                                         .headers = {{"Accept", accept_header(options.format)}, {"User-Agent", "AVA/1.0 webfetch"}},
                                                         .body = "",
                                                         .timeout_ms = timeout_ms,
                                                         .follow_redirects = false,
                                                         .include_response_headers = true,
                                                         .resolve_hosts = std::move(resolve_hosts)},
                                  context.cancel_requested);
  if (!response)
    return std::unexpected(std::move(response.error()));
  if (context.cancel_requested && context.cancel_requested())
  {
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Unknown, "tool canceled", ava::core::ErrorCode::Canceled));
  }
  if (response->status_code < 200 || response->status_code >= 300)
  {
    auto error = ava::core::Error(ava::core::ErrorCategory::Provider, "webfetch HTTP request failed");
    error.with_context("status_code", std::to_string(response->status_code));
    error.with_context("url", safe_url->url);
    return std::unexpected(std::move(error));
  }

  auto const content_type = header_value(response->headers, "content-type");
  if (looks_binary(response->body, content_type))
  {
    auto error = ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "webfetch response appears to be binary");
    if (!content_type.empty())
      error.with_context("content_type", content_type);
    return std::unexpected(std::move(error));
  }

  WebFetchResult result;
  result.url = safe_url->url;
  result.status_code = response->status_code;
  result.content_type = content_type;
  std::string converted = response->body;
  if (is_html_content(content_type))
  {
    if (options.format == WebFetchFormat::Markdown)
    {
      converted = html_to_text(response->body, true);
    }
    else if (options.format == WebFetchFormat::Text)
    {
      converted = html_to_text(response->body, false);
    }
  }
  apply_line_window(result, converted, options, max_bytes);
  return result;
}

}  // namespace ava::tools

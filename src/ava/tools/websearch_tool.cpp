#include "sys.h"
#include "ava/tools/websearch_tool.h"
#include "ava/core/error.h"
#include "ava/core/json.h"
#include "ava/core/string_utils.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <memory>
#include <string_view>

namespace ava::tools {
namespace {

constexpr std::size_t kMaxQueryBytes = 512;
constexpr std::size_t kMaxWebSearchResults = 10;
constexpr std::size_t kMaxWebSearchContextChars = 30000;
constexpr int kMaxWebSearchTimeoutMs = 60000;

ava::core::Result<std::string> validate_query(std::string_view query)
{
  auto trimmed = core::trim(query);
  if (trimmed.empty() || trimmed.size() > kMaxQueryBytes)
  {
    auto error = ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "websearch query is empty or too long");
    error.with_context("max_bytes", std::to_string(kMaxQueryBytes));
    return std::unexpected(std::move(error));
  }
  for (char const ch : trimmed)
  {
    auto const byte = static_cast<unsigned char>(ch);
    if (byte < 0x20 || byte == 0x7F)
    {
      return std::unexpected(ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "websearch query contains a control byte"));
    }
  }
  return trimmed;
}

bool unreserved(unsigned char ch)
{
  return std::isalnum(ch) != 0 || ch == '-' || ch == '_' || ch == '.' || ch == '~';
}

std::string url_encode(std::string_view value)
{
  constexpr std::array<char, 16> hex{'0', '1', '2', '3', '4', '5', '6', '7', '8', '9', 'A', 'B', 'C', 'D', 'E', 'F'};
  std::string out;
  out.reserve(value.size() * 3);
  for (unsigned char const ch : value)
  {
    if (unreserved(ch))
    {
      out.push_back(static_cast<char>(ch));
    }
    else if (ch == ' ')
    {
      out.push_back('+');
    }
    else
    {
      out.push_back('%');
      out.push_back(hex[(ch >> 4) & 0x0F]);
      out.push_back(hex[ch & 0x0F]);
    }
  }
  return out;
}

std::string title_from_url(std::string_view url)
{
  auto without_scheme = url;
  if (without_scheme.starts_with("https://"))
    without_scheme.remove_prefix(8);
  if (without_scheme.starts_with("http://"))
    without_scheme.remove_prefix(7);
  auto const slash = without_scheme.find('/');
  auto host = slash == std::string_view::npos ? without_scheme : without_scheme.substr(0, slash);
  return host.empty() ? std::string("result") : std::string(host);
}

void add_result(std::vector<WebSearchResultItem>& results, WebSearchResultItem item)
{
  if (item.url.empty() && item.snippet.empty())
    return;
  if (item.title.empty())
    item.title = title_from_url(item.url);
  auto duplicate = std::ranges::find_if(results, [&](WebSearchResultItem const& existing) { return !item.url.empty() && existing.url == item.url; });
  if (duplicate != results.end())
    return;
  results.push_back(std::move(item));
}

void collect_duckduckgo_topic(std::string_view object, std::vector<WebSearchResultItem>& results)
{
  auto const url = ava::core::json::string_field(object, "FirstURL").value_or("");
  auto const snippet = ava::core::json::string_field(object, "Text").value_or("");
  if (!url.empty() || !snippet.empty())
  {
    add_result(results, WebSearchResultItem{.title = title_from_url(url), .url = url, .snippet = snippet});
  }
  for (auto const& nested : ava::core::json::objects_in_array_field(object, "Topics"))
  {
    collect_duckduckgo_topic(nested, results);
  }
}

std::vector<WebSearchResultItem> parse_duckduckgo_results(std::string_view body)
{
  std::vector<WebSearchResultItem> results;
  auto const abstract = ava::core::json::string_field(body, "AbstractText").value_or("");
  auto const abstract_url = ava::core::json::string_field(body, "AbstractURL").value_or("");
  auto const heading = ava::core::json::string_field(body, "Heading").value_or("");
  if (!abstract.empty() || !abstract_url.empty())
  {
    add_result(results, WebSearchResultItem{.title = heading.empty() ? title_from_url(abstract_url) : heading, .url = abstract_url, .snippet = abstract});
  }
  for (auto const& result : ava::core::json::objects_in_array_field(body, "Results"))
  {
    collect_duckduckgo_topic(result, results);
  }
  for (auto const& topic : ava::core::json::objects_in_array_field(body, "RelatedTopics"))
  {
    collect_duckduckgo_topic(topic, results);
  }
  return results;
}

}  // namespace

ava::core::Result<WebSearchResult> websearch(ToolContext const& context, std::string_view query, WebSearchOptions options)
{
  if (context.cancel_requested && context.cancel_requested())
  {
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Unknown, "tool canceled", ava::core::ErrorCode::Canceled));
  }

  auto safe_query = validate_query(query);
  if (!safe_query)
    return std::unexpected(std::move(safe_query.error()));

  if (auto permission =
          ensure_permission(context, ava::permissions::Operation::NetworkSearch, {}, *safe_query, "websearch", "network search requires permission");
      !permission)
  {
    return std::unexpected(std::move(permission.error()));
  }
  if (context.cancel_requested && context.cancel_requested())
  {
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Unknown, "tool canceled", ava::core::ErrorCode::Canceled));
  }

  auto const max_results = std::min(options.max_results == 0 ? std::size_t{8} : options.max_results, kMaxWebSearchResults);
  auto const context_max_chars = std::min(options.context_max_chars == 0 ? std::size_t{10000} : options.context_max_chars, kMaxWebSearchContextChars);
  auto const timeout_ms = std::clamp(options.timeout_ms <= 0 ? 25000 : options.timeout_ms, 1000, kMaxWebSearchTimeoutMs);

  std::string const url = "https://api.duckduckgo.com/?q=" + url_encode(*safe_query) + "&format=json&no_html=1&skip_disambig=1&no_redirect=1";

  std::unique_ptr<ava::http::Transport> owned_transport;
  ava::http::Transport* transport = options.transport;
  if (transport == nullptr)
  {
    if (!context.transport_factory)
    {
      return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Configuration, "websearch transport process authority is unavailable"));
    }
    auto created = context.transport_factory();
    if (!created)
      return std::unexpected(std::move(created.error()));
    if (!*created)
      return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Configuration, "websearch transport is unavailable"));
    owned_transport = std::move(*created);
    transport = owned_transport.get();
  }
  auto response = transport->send(ava::http::HttpRequest{.method = "GET",
                                                         .url = url,
                                                         .headers = {{"Accept", "application/json"}, {"User-Agent", "AVA/1.0 websearch"}},
                                                         .body = "",
                                                         .timeout_ms = timeout_ms,
                                                         .follow_redirects = true,
                                                         .include_response_headers = true,
                                                         .resolve_hosts = {}},
                                  context.cancel_requested);
  if (!response)
    return std::unexpected(std::move(response.error()));
  if (response->status_code < 200 || response->status_code >= 300)
  {
    auto error = ava::core::Error(ava::core::ErrorCategory::Provider, "websearch HTTP request failed");
    error.with_context("status_code", std::to_string(response->status_code));
    return std::unexpected(std::move(error));
  }
  if (!ava::core::json::is_valid_object(response->body))
  {
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Provider, "websearch response was not valid JSON"));
  }

  auto parsed = parse_duckduckgo_results(response->body);
  WebSearchResult result;
  result.query = *safe_query;
  result.engine = "duckduckgo_instant_answer";
  result.total_results = parsed.size();
  result.truncated = parsed.size() > max_results;

  std::size_t used_chars = 0;
  for (auto& item : parsed)
  {
    if (result.results.size() >= max_results)
      break;
    auto const item_chars = item.title.size() + item.url.size() + item.snippet.size();
    if (used_chars > 0 && used_chars + item_chars > context_max_chars)
    {
      result.truncated = true;
      break;
    }
    used_chars += item_chars;
    result.results.push_back(std::move(item));
  }
  result.output_chars = used_chars;
  return result;
}

}  // namespace ava::tools

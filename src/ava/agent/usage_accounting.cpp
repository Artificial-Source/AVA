#include "sys.h"
#include "ava/agent/usage_accounting.h"
#include "ava/core/saturating.h"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <limits>
#include <sstream>
#include <string>

namespace ava::agent {
namespace {

std::string decimal_json(long double value)
{
  std::ostringstream out;
  out << std::setprecision(12) << value;
  return out.str();
}

void append_optional_integer_field(std::string& json, std::string_view key, std::optional<long long> const& value, bool& first)
{
  if (!value || *value < 0)
    return;
  if (!first)
    json += ',';
  first = false;
  json += '"';
  json += key;
  json += "\":";
  json += std::to_string(*value);
}

// Token counts are signed; the shared core saturating_add only accepts unsigned integers.
long long saturating_add(long long left, long long right) noexcept
{
  constexpr auto maximum = std::numeric_limits<long long>::max();
  constexpr auto minimum = std::numeric_limits<long long>::min();
  if (right > 0 && left > maximum - right)
    return maximum;
  if (right < 0 && left < minimum - right)
    return minimum;
  return left + right;
}

long long clamped_byte_count(std::size_t value) noexcept
{
  constexpr auto maximum = std::numeric_limits<long long>::max();
  return value > static_cast<std::size_t>(maximum) ? maximum : static_cast<long long>(value);
}

std::size_t output_estimate_bytes(ParsedAssistantTurn const& turn)
{
  std::size_t bytes = turn.text.size();
  for (auto const& reasoning : turn.reasoning_blocks)
  {
    bytes = ava::core::saturating_add(bytes, reasoning.text.size());
    bytes = ava::core::saturating_add(bytes, reasoning.signature.size());
    bytes = ava::core::saturating_add(bytes, reasoning.redacted_data.size());
    bytes = ava::core::saturating_add(bytes, reasoning.native_item_json.size());
  }
  for (auto const& call : turn.tool_calls)
  {
    bytes = ava::core::saturating_add(bytes, call.id.size());
    bytes = ava::core::saturating_add(bytes, call.name.size());
    bytes = ava::core::saturating_add(bytes, call.arguments_json.size());
  }
  return bytes;
}

void add_optional_usage_field(std::optional<long long>& total, std::optional<long long> const& value)
{
  if (!value || *value < 0)
    return;
  if (!total)
    total = 0;
  *total = saturating_add(*total, *value);
}

long double millionths(long long tokens, long double price_per_million)
{
  auto const token_millions = static_cast<long double>(tokens) / 1'000'000.0L;
  constexpr auto maximum = std::numeric_limits<long double>::max();
  if (token_millions > 0.0L && price_per_million > maximum / token_millions)
    return maximum;
  auto const value = token_millions * price_per_million;
  return std::isfinite(value) ? value : maximum;
}

void add_cost_component(long double& total, long double component) noexcept
{
  constexpr auto maximum = std::numeric_limits<long double>::max();
  if (!std::isfinite(component) || !std::isfinite(total) || (component > 0.0L && total > maximum - component))
    total = maximum;
  else
    total += component;
}

}  // namespace

std::optional<long double> usage_cost_usd(ava::config::ModelPricing const& pricing, ava::provider::TokenUsage const& usage)
{
  if (usage.estimated)
    return std::nullopt;

  long long const input_tokens = usage.input_tokens.value_or(0);
  long long const cache_read_tokens = usage.cache_read_tokens.value_or(0);
  long long const cache_write_tokens = usage.cache_write_tokens.value_or(0);
  if (cache_read_tokens > 0 && !pricing.cache_read_per_million)
    return std::nullopt;
  if (cache_write_tokens > 0 && !pricing.cache_write_per_million)
    return std::nullopt;
  long long regular_input_tokens = input_tokens;
  if (pricing.cache_read_per_million)
    regular_input_tokens -= std::min(regular_input_tokens, cache_read_tokens);
  if (pricing.cache_write_per_million)
    regular_input_tokens -= std::min(regular_input_tokens, cache_write_tokens);

  auto output_tokens = usage.output_tokens.or_else([&usage, input_tokens]() -> std::optional<long long> {
    if (!usage.total_tokens || *usage.total_tokens < input_tokens)
      return std::nullopt;
    return *usage.total_tokens - input_tokens;
  });
  long long const reasoning_tokens = usage.reasoning_tokens.value_or(0);
  long long regular_output_tokens = output_tokens.value_or(0);
  if (pricing.reasoning_per_million)
    regular_output_tokens -= std::min(regular_output_tokens, reasoning_tokens);

  if (regular_input_tokens > 0 && !pricing.input_per_million)
    return std::nullopt;
  if (regular_output_tokens > 0 && !pricing.output_per_million)
    return std::nullopt;

  long double total = 0.0L;
  bool has_billable_usage = false;
  if (regular_input_tokens > 0)
  {
    add_cost_component(total, millionths(regular_input_tokens, *pricing.input_per_million));
    has_billable_usage = true;
  }
  if (pricing.cache_read_per_million && cache_read_tokens > 0)
  {
    add_cost_component(total, millionths(cache_read_tokens, *pricing.cache_read_per_million));
    has_billable_usage = true;
  }
  if (pricing.cache_write_per_million && cache_write_tokens > 0)
  {
    add_cost_component(total, millionths(cache_write_tokens, *pricing.cache_write_per_million));
    has_billable_usage = true;
  }
  if (regular_output_tokens > 0)
  {
    add_cost_component(total, millionths(regular_output_tokens, *pricing.output_per_million));
    has_billable_usage = true;
  }
  if (pricing.reasoning_per_million && reasoning_tokens > 0)
  {
    add_cost_component(total, millionths(reasoning_tokens, *pricing.reasoning_per_million));
    has_billable_usage = true;
  }

  return has_billable_usage ? std::optional<long double>(total) : std::nullopt;
}

std::string usage_json(ava::provider::TokenUsage const& usage, std::optional<long double> const& cost_usd)
{
  std::string json = "{";
  bool first = true;
  append_optional_integer_field(json, "input_tokens", usage.input_tokens, first);
  append_optional_integer_field(json, "output_tokens", usage.output_tokens, first);
  append_optional_integer_field(json, "reasoning_tokens", usage.reasoning_tokens, first);
  append_optional_integer_field(json, "cache_read_tokens", usage.cache_read_tokens, first);
  append_optional_integer_field(json, "cache_write_tokens", usage.cache_write_tokens, first);
  append_optional_integer_field(json, "total_tokens", usage.total_tokens, first);
  append_optional_integer_field(json, "estimated_input_bytes", usage.estimated_input_bytes, first);
  append_optional_integer_field(json, "estimated_output_bytes", usage.estimated_output_bytes, first);
  append_optional_integer_field(json, "estimated_total_bytes", usage.estimated_total_bytes, first);
  if (!first)
    json += ',';
  json += "\"estimated\":";
  json += usage.estimated ? "true" : "false";
  json += ",\"source\":\"";
  json += usage.estimated ? "estimated" : "provider";
  json += '"';
  if (usage.estimated)
  {
    json += ",\"estimation_method\":\"byte_count\"";
  }
  if (cost_usd)
  {
    json += ",\"cost_usd\":";
    json += decimal_json(*cost_usd);
    json += ",\"cost_estimated\":";
    json += usage.estimated ? "true" : "false";
  }
  json += '}';
  return json;
}

ava::provider::TokenUsage with_total_tokens(ava::provider::TokenUsage usage)
{
  if (!usage.total_tokens && usage.input_tokens && usage.output_tokens)
  {
    usage.total_tokens = saturating_add(*usage.input_tokens, *usage.output_tokens);
  }
  return usage;
}

ava::provider::TokenUsage estimate_usage_from_turn(std::string_view request_body, ParsedAssistantTurn const& turn)
{
  auto const input_bytes = clamped_byte_count(request_body.size());
  auto const output_bytes = clamped_byte_count(output_estimate_bytes(turn));
  return ava::provider::TokenUsage{.input_tokens = std::nullopt,
                                   .output_tokens = std::nullopt,
                                   .reasoning_tokens = std::nullopt,
                                   .cache_read_tokens = std::nullopt,
                                   .cache_write_tokens = std::nullopt,
                                   .total_tokens = std::nullopt,
                                   .estimated_input_bytes = input_bytes,
                                   .estimated_output_bytes = output_bytes,
                                   .estimated_total_bytes = saturating_add(input_bytes, output_bytes),
                                   .estimated = true};
}

void accumulate_usage(std::optional<ava::provider::TokenUsage>& total, ava::provider::TokenUsage const& usage)
{
  if (!total)
    total = ava::provider::TokenUsage{};
  add_optional_usage_field(total->input_tokens, usage.input_tokens);
  add_optional_usage_field(total->output_tokens, usage.output_tokens);
  add_optional_usage_field(total->reasoning_tokens, usage.reasoning_tokens);
  add_optional_usage_field(total->cache_read_tokens, usage.cache_read_tokens);
  add_optional_usage_field(total->cache_write_tokens, usage.cache_write_tokens);
  add_optional_usage_field(total->total_tokens, usage.total_tokens);
  add_optional_usage_field(total->estimated_input_bytes, usage.estimated_input_bytes);
  add_optional_usage_field(total->estimated_output_bytes, usage.estimated_output_bytes);
  add_optional_usage_field(total->estimated_total_bytes, usage.estimated_total_bytes);
  total->estimated = total->estimated || usage.estimated;
}

}  // namespace ava::agent

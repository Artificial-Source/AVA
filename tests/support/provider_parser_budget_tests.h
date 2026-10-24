#pragma once

#include "tests/support/test_harness.h"
#include "ava/provider/provider.h"

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

namespace ava::tests {

// Exercise the public provider and incremental-parser seams, not parser internals.
template <typename BufferedText, typename StreamedText>
void test_provider_parser_budgets(ava::provider::Provider const& provider, BufferedText buffered_text, StreamedText streamed_text,
                                  std::vector<std::string> const& oversized_arrays, std::string_view empty_record)
{
  using namespace ava::provider;
  auto is_limit_error = [](auto const& result) {
    if (!result)
      return result.error().category() == ava::core::ErrorCategory::Provider && result.error().message().find("parser limit exceeded") != std::string::npos &&
             result.error().message().size() < 160;
    return !result->empty() && result->back().type == StreamEventType::Error &&
           result->back().error_message.find("parser limit exceeded") != std::string::npos && result->back().error_message.size() < 160 &&
           std::ranges::none_of(*result, [](auto const& event) { return event.type == StreamEventType::Done; });
  };
  std::string const oversized(kMaxProviderSseBufferedBytes + 1, 'x');
  expect(is_limit_error(provider.parse_response({.status_code = 200, .headers = {}, .body = buffered_text(oversized)}, false)),
         "buffered provider rejects oversized single record before expansion");
  for (bool const delimiter : {false, true})
  {
    auto parser = provider.create_stream_parser();
    auto const record = streamed_text(oversized) + (delimiter ? "\n\n" : "");
    auto const rejected = parser->append(record);
    auto const ignored = parser->append(streamed_text("AFTER_LIMIT_CANARY") + "\n\n");
    auto const finished = parser->finish();
    expect(is_limit_error(rejected) && ignored && ignored->empty() && finished && finished->empty(),
           "oversized SSE record is terminal with or without a delimiter");
  }
  {
    auto parser = provider.create_stream_parser();
    auto const record = streamed_text(oversized) + "\n\n";
    bool rejected = false;
    for (std::size_t offset = 0; offset < record.size(); offset += 1024)
    {
      auto const events = parser->append(std::string_view(record).substr(offset, 1024));
      rejected = rejected || is_limit_error(events);
    }
    auto const finished = parser->finish();
    expect(rejected && finished && finished->empty(), "fragmenting one oversized SSE line cannot evade the record byte limit");
  }
  {
    auto parser = provider.create_stream_parser();
    bool rejected = false;
    for (std::size_t index = 0; index < 257; ++index)
      rejected = is_limit_error(parser->append("data: " + std::string(1024, 'x') + "\n")) || rejected;
    auto const finished = parser->finish();
    expect(rejected && finished && finished->empty(), "many short data lines cannot evade the cumulative SSE record byte limit");
  }
  for (auto const& body : oversized_arrays)
  {
    expect(is_limit_error(provider.parse_response({.status_code = 200, .headers = {}, .body = body}, false)),
           "buffered provider rejects oversized protocol arrays");
    auto parser = provider.create_stream_parser();
    auto const rejected = parser->append("data: " + body + "\n\n");
    auto const ignored = parser->append(streamed_text("AFTER_ARRAY_CANARY") + "\n\n");
    auto const finished = parser->finish();
    expect(is_limit_error(rejected) && ignored && ignored->empty() && finished && finished->empty(),
           "streamed provider rejects oversized nested protocol arrays terminally");
  }
  auto parser = provider.create_stream_parser();
  std::string const fragment(4096, 'x');
  bool accepted_prefix = true;
  for (std::size_t index = 0; index < kMaxProviderSseBufferedBytes / fragment.size(); ++index)
  {
    auto const events = parser->append(streamed_text(fragment) + "\n\n");
    accepted_prefix = accepted_prefix && events && !is_limit_error(events);
  }
  auto const overflow = parser->append(streamed_text("x") + "\n\n");
  auto const ignored = parser->append(streamed_text("AFTER_FRAGMENT_CANARY") + "\n\n");
  auto const finished = parser->finish();
  expect(accepted_prefix && is_limit_error(overflow) && ignored && ignored->empty() && finished && finished->empty(),
         "cumulative small text fragments accept 256 KiB exactly then fail closed at one byte over");

  parser = provider.create_stream_parser();
  std::size_t emitted = 0;
  bool rejected = false;
  for (std::size_t index = 0; index <= kMaxProviderParserEvents; ++index)
  {
    auto const events = parser->append(std::string(empty_record) + "\n\n");
    if (!events)
      break;
    emitted += events->size();
    if (is_limit_error(events))
    {
      rejected = true;
      break;
    }
  }
  auto const after_records = parser->append(streamed_text("AFTER_RECORD_CANARY") + "\n\n");
  expect(rejected && emitted <= kMaxProviderParserEvents && after_records && after_records->empty(),
         "record and event counts are cumulative across append calls, including ignored records");

  parser = provider.create_stream_parser();
  emitted = 0;
  rejected = false;
  for (std::size_t index = 0; index < kMaxProviderParserEvents; ++index)
  {
    auto const events = parser->append(streamed_text("x") + "\n\n");
    if (!events)
      break;
    emitted += events->size();
    if (is_limit_error(events))
    {
      rejected = true;
      break;
    }
  }
  auto const final_events = parser->finish();
  expect(rejected && emitted == kMaxProviderParserEvents && final_events && final_events->empty(),
         "normalized event budget reserves its final slot for one terminal diagnostic across append calls");
}

template <typename Fragment>
void test_provider_tool_fragment_budget(ava::provider::Provider const& provider, std::string_view start, Fragment fragment, std::string_view done)
{
  using namespace ava::provider;
  auto parser = provider.create_stream_parser();
  auto const started = parser->append(start);
  bool accepted = started.has_value();
  bool rejected = false;
  for (std::size_t index = 0; index < 65; ++index)
  {
    auto const events = parser->append(fragment(std::string(4096, 'x')) + "\n\n");
    if (events && !events->empty() && events->back().type == StreamEventType::Error)
    {
      rejected = index >= 60 && events->back().error_message.find("parser limit exceeded") != std::string::npos;
      break;
    }
    accepted = accepted && events.has_value();
  }
  auto const ignored = parser->append(done);
  auto const finished = parser->finish();
  expect(accepted && rejected && ignored && ignored->empty() && finished && finished->empty(),
         "tool argument fragments share the cumulative byte budget and never gain a successful terminal after overflow");
}

inline std::string provider_budget_array(std::string_view item, std::size_t count = ava::provider::kMaxProviderParserArrayItems + 1)
{
  std::string result = "[";
  for (std::size_t index = 0; index < count; ++index)
  {
    if (index != 0)
      result += ',';
    result += item;
  }
  return result + ']';
}

}  // namespace ava::tests

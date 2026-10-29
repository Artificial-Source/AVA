#pragma once

#include "ava/provider/provider.h"
#include "ava/core/NoLogString.h"

#include <string>
#include <string_view>
#include <vector>

namespace ava::provider {

// One response owns these counters, regardless of transport chunk boundaries.
// Keep the existing Responses limits authoritative for every protocol family.
class ProviderParserBudget
{
 public:
  [[nodiscard]] bool record(std::string_view data) { return data.size() <= kMaxProviderSseBufferedBytes && ++records_ <= kMaxProviderParserEvents; }

  [[nodiscard]] bool fragment(std::string_view value)
  {
    if (value.size() > kMaxProviderSseBufferedBytes - fragment_bytes_)
      return false;
    fragment_bytes_ += value.size();
    return true;
  }

  [[nodiscard]] bool event(StreamEvent const& event)
  {
    // Leave one slot for the fixed terminal diagnostic, as Responses does.
    if (events_ >= kMaxProviderParserEvents - 1)
      return false;
    for (auto const* field : {&event.text, &event.reasoning_signature, &event.reasoning_redacted_data})
      if (!fragment(*field))
        return false;
    for (auto const* field : {&event.tool_call_id, &event.tool_name, &event.provider_item_id, &event.reasoning_format, &event.reasoning_native_item_json})
    {
      if (field->size() > kMaxProviderSseBufferedBytes - metadata_bytes_)
        return false;
      metadata_bytes_ += field->size();
    }
    ++events_;
    return true;
  }

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT

 private:
  std::size_t records_ = 0;
  std::size_t events_ = 0;
  std::size_t fragment_bytes_ = 0;
  std::size_t metadata_bytes_ = 0;
};

inline StreamEvent provider_parser_limit_event(std::string_view family)
{
  return StreamEvent{.type = StreamEventType::Error,
                     .text = "",
                     .tool_call_id = "",
                     .tool_name = "",
                     .error_message = std::string(family) + " response parser limit exceeded",
                     .usage = std::nullopt};
}

// Enforce the same output bounds on buffered responses before publication.
[[nodiscard]] inline ava::core::Result<std::vector<StreamEvent>> bounded_provider_events(std::vector<StreamEvent> events, std::string_view family)
{
  ProviderParserBudget budget;
  for (auto const& event : events)
    if (!budget.event(event))
      return std::vector<StreamEvent>{provider_parser_limit_event(family)};
  return events;
}

// SSE framing and budget enforcement are shared; family adapters only decode
// complete records. No record or fragment can grow by changing chunk boundaries.
class ProviderSseBuffer
{
 public:
  // Opaque fragments retained without an immediate event must be charged
  // before storage too. Their eventual event publication is checked again.
  [[nodiscard]] bool retain_fragment(std::string_view value) { return budget_.fragment(value); }

  template <typename Decode>
  std::vector<StreamEvent> append(std::string_view chunk, std::string_view family, Decode decode)
  {
    std::vector<StreamEvent> events;
    if (failed_)
      return events;
    if (chunk.size() > kMaxProviderSseBufferedBytes || pending_line_.size() > kMaxProviderSseBufferedBytes - chunk.size())
    {
      fail(events, family);
      return events;
    }
    pending_line_.append(chunk);
    std::size_t start = 0;
    auto search = scan_offset_;
    while (!failed_)
    {
      auto const newline = pending_line_.find('\n', search);
      if (newline == std::string::npos)
        break;
      line(events, std::string_view(pending_line_).substr(start, newline - start), family, decode);
      start = newline + 1;
      search = start;
    }
    if (failed_)
    {
      pending_line_.clear();
      data_.clear();
    }
    else if (start != 0)
      pending_line_.erase(0, start);
    scan_offset_ = pending_line_.size();
    return events;
  }

  template <typename Decode, typename Complete>
  std::vector<StreamEvent> finish(std::string_view family, Decode decode, Complete complete)
  {
    std::vector<StreamEvent> events;
    if (!failed_ && !pending_line_.empty())
      line(events, pending_line_, family, decode);
    if (!failed_ && !data_.empty())
      record(events, family, decode);
    if (!failed_)
    {
      auto const first = events.size();
      complete(events);
      account(events, first, family);
    }
    *this = ProviderSseBuffer{};
    return events;
  }

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT

 private:
  void fail(std::vector<StreamEvent>& events, std::string_view family)
  {
    failed_ = true;
    events.push_back(provider_parser_limit_event(family));
  }

  void account(std::vector<StreamEvent>& events, std::size_t first, std::string_view family)
  {
    for (auto index = first; index < events.size(); ++index)
    {
      if (!budget_.event(events[index]))
      {
        events.resize(index);
        fail(events, family);
        return;
      }
      // Preserve each protocol's ordinary error semantics. Budget failures
      // alone are latched here, including the smaller opaque-reasoning cap.
      auto const diagnostic = std::string_view(events[index].error_message);
      bool const budget_error = diagnostic.starts_with(family) &&
                                (diagnostic.substr(family.size()) == " response parser limit exceeded" || diagnostic.ends_with("exceeded byte limit"));
      if (events[index].type == StreamEventType::Error && budget_error)
      {
        events.resize(index + 1);
        failed_ = true;
        return;
      }
    }
  }

  template <typename Decode>
  void record(std::vector<StreamEvent>& events, std::string_view family, Decode decode)
  {
    if (!budget_.record(data_))
      fail(events, family);
    else
    {
      auto const first = events.size();
      decode(events, data_);
      account(events, first, family);
    }
    data_.clear();
  }

  template <typename Decode>
  void line(std::vector<StreamEvent>& events, std::string_view value, std::string_view family, Decode decode)
  {
    if (!value.empty() && value.back() == '\r')
      value.remove_suffix(1);
    if (value.empty())
    {
      if (!data_.empty())
        record(events, family, decode);
      return;
    }
    if (!value.starts_with("data:"))
      return;
    value.remove_prefix(5);
    if (!value.empty() && value.front() == ' ')
      value.remove_prefix(1);
    auto const separator = data_.empty() ? 0U : 1U;
    if (value.size() > kMaxProviderSseBufferedBytes - data_.size() || separator > kMaxProviderSseBufferedBytes - data_.size() - value.size())
    {
      fail(events, family);
      return;
    }
    if (separator != 0)
      data_ += '\n';
    data_.append(value);
  }

  ProviderParserBudget budget_;
  // Shared framing storage retains private provider content across transport chunks.
  ava::core::NoLogString pending_line_;
  ava::core::NoLogString data_;
  std::size_t scan_offset_ = 0;
  bool failed_ = false;
};

}  // namespace ava::provider

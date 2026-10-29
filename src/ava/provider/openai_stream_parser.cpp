#include "sys.h"
#include "ava/provider/openai_stream_parser.h"
#include "ava/provider/openai_stream_parser_internal.h"

#include <optional>
#include <string_view>
#include <vector>

namespace ava::provider {

using namespace openai_stream_parser_internal;

ava::core::Result<std::vector<StreamEvent>> OpenAIStreamParser::append(std::string_view chunk)
{
  return sse_.append(chunk, "OpenAI", [this](auto& events, std::string_view data) { append_event_for_data(events, data); });
}

void OpenAIStreamParser::reset_state() noexcept
{
  saw_content_ = false;
  saw_refusal_ = false;
  reasoning_open_ = false;
  reasoning_text_seen_ = false;
  active_reasoning_item_id_.clear();
  active_reasoning_output_index_ = std::nullopt;
  active_reasoning_text_.clear();
  completed_reasoning_item_ids_.clear();
  completed_reasoning_texts_.clear();
  function_calls_.clear();
  function_call_item_ids_by_logical_id_.clear();
  legacy_function_calls_.clear();
  message_items_.clear();
  documented_output_item_types_.clear();
  documented_output_item_added_ids_.clear();
  documented_output_item_ids_by_index_.clear();
  done_seen_ = false;
  error_seen_ = false;
}

ava::core::Result<std::vector<StreamEvent>> OpenAIStreamParser::finish()
{
  auto events = sse_.finish(
      "OpenAI", [this](auto& output, std::string_view data) { append_event_for_data(output, data); },
      [this](auto& output) {
        if (!done_seen_ && !error_seen_)
        {
          if (!reject_unended_documented_function_calls(output))
            static_cast<void>(reject_unended_documented_message_items(output));
        }
        append_finish_reasoning_if_open(output);
        if (saw_content_ && !done_seen_ && !error_seen_)
          append_stream_error(output, error_seen_, "OpenAI SSE stream ended before done marker");
      });
  reset_state();
  return events;
}

}  // namespace ava::provider

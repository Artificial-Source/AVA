#include "sys.h"
#include "ava/tui/TuiFrontEnd.h"
#include "ava/tui/composer_editor.h"
#include "ava/tui/composer_internal.h"
#include "ava/tui/runtime_transcript_internal.h"
#include "ava/tui/text.h"
#include "ava/tui/tool_cards.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iterator>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <unistd.h>

namespace ava::tui::runtime_transcript {
namespace {

constexpr std::string_view kCopyOptionPrefix = "copy:";
constexpr std::string_view kBase64Alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string title_case_ascii(std::string_view text)
{
  std::string output;
  output.reserve(text.size());
  bool at_word_start = true;
  for (char ch : text)
  {
    auto const byte = static_cast<unsigned char>(ch);
    if (byte == '_' || byte == '-')
    {
      output.push_back(' ');
      at_word_start = true;
      continue;
    }
    if (at_word_start && byte >= 'a' && byte <= 'z')
    {
      output.push_back(static_cast<char>(byte - ('a' - 'A')));
    }
    else
    {
      output.push_back(ch);
    }
    at_word_start = byte == ' ';
  }
  return output;
}

std::string turn_duration_label(std::chrono::steady_clock::duration duration)
{
  auto const milliseconds = std::max<std::int64_t>(0, std::chrono::duration_cast<std::chrono::milliseconds>(duration).count());
  if (milliseconds < 1000)
    return std::to_string(milliseconds) + "ms";

  auto const seconds = milliseconds / 1000;
  if (seconds < 10)
  {
    auto const tenths = (milliseconds % 1000) / 100;
    return std::to_string(seconds) + "." + std::to_string(tenths) + "s";
  }
  if (seconds < 60)
    return std::to_string((milliseconds + 500) / 1000) + "s";

  auto const minutes = seconds / 60;
  auto const remaining_seconds = seconds % 60;
  return std::to_string(minutes) + "m " + std::to_string(remaining_seconds) + "s";
}

bool transcript_item_has_visible_content(TranscriptItem const& item, bool thinking_visible)
{
  return !item.text.empty() || !text_empty(item.text_model) || (thinking_visible && (!item.thinking.empty() || !text_empty(item.thinking_model)));
}

bool write_all_to_stdout(std::string_view text)
{
  std::size_t offset = 0;
  while (offset < text.size())
  {
    auto const written = ::write(STDOUT_FILENO, text.data() + offset, text.size() - offset);
    if (written < 0)
    {
      if (errno == EINTR)
        continue;
      return false;
    }
    if (written == 0)
      return false;
    offset += static_cast<std::size_t>(written);
  }
  return true;
}

std::string tool_card_identity_key(ToolTimelineItem const& item)
{
  if (!item.call_id.empty())
    return "call\n" + item.call_id;
  if (!item.request_id.empty())
    return "request\n" + item.request_id;
  if (!item.correlation_id.empty())
    return "correlation\n" + item.correlation_id;
  return "fallback\n" + item.name + "\n" + item.argument_summary;
}

std::size_t truncate_transcript(std::vector<TranscriptItem>& transcript)
{
  if (transcript.size() <= kMaxTranscriptItems)
    return 0;
  auto const removed = transcript.size() - kMaxTranscriptItems;
  transcript.erase(transcript.begin(), transcript.begin() + static_cast<std::ptrdiff_t>(removed));
  return removed;
}

}  // namespace

std::string assistant_meta_for_snapshot(TuiFrontEnd* frontend, ComposerSnapshot const& snapshot, std::optional<std::chrono::steady_clock::duration> elapsed)
{
  auto model = frontend->model_selection().refresh_model();
  if (model.value.empty())
    return {};
  auto mode = title_case_ascii(snapshot.mode);
  if (mode.empty())
    mode = "AVA";
  auto meta = mode + " · " + model.value;
  if (elapsed)
    meta += " · " + turn_duration_label(*elapsed);
  return meta;
}

void apply_assistant_turn_meta(std::vector<TranscriptItem>& transcript, std::string const& meta, bool thinking_visible)
{
  TranscriptItem* final_visible_assistant = nullptr;
  for (auto& item : transcript)
  {
    if (item.tool || item.label != "ava")
      continue;
    item.meta.clear();
    if (transcript_item_has_visible_content(item, thinking_visible))
      final_visible_assistant = &item;
  }
  if (final_visible_assistant && !meta.empty())
    final_visible_assistant->meta = meta;
}

std::ptrdiff_t push_fallback_assistant_outputs(ComposerSnapshot& snapshot, std::vector<std::string> const& outputs, std::string const& meta)
{
  std::vector<TranscriptItem> items;
  items.reserve(outputs.size());
  for (auto const& output : outputs)
    items.push_back(TranscriptItem{.label = "ava", .text = output});
  apply_assistant_turn_meta(items, meta, snapshot.thinking_visible);
  std::ptrdiff_t item_index_shift = 0;
  for (auto& item : items)
    item_index_shift += push_transcript(snapshot, std::move(item));
  return item_index_shift;
}

std::string base64_encode(std::string_view text)
{
  std::string output;
  // encoded_size = ceil(n / 3) * 4 = ((n + 2) / 3) * 4 — guard both steps.
  if (text.size() <= std::numeric_limits<std::size_t>::max() - 2)
  {
    auto const groups = (text.size() + 2) / 3;
    if (groups <= std::numeric_limits<std::size_t>::max() / 4)
      output.reserve(groups * 4);
  }
  for (std::size_t index = 0; index < text.size(); index += 3)
  {
    auto const first = static_cast<unsigned char>(text[index]);
    auto const second = index + 1 < text.size() ? static_cast<unsigned char>(text[index + 1]) : 0;
    auto const third = index + 2 < text.size() ? static_cast<unsigned char>(text[index + 2]) : 0;
    auto const block = (static_cast<unsigned int>(first) << 16U) | (static_cast<unsigned int>(second) << 8U) | static_cast<unsigned int>(third);
    output.push_back(kBase64Alphabet[(block >> 18U) & 0x3FU]);
    output.push_back(kBase64Alphabet[(block >> 12U) & 0x3FU]);
    output.push_back(index + 1 < text.size() ? kBase64Alphabet[(block >> 6U) & 0x3FU] : '=');
    output.push_back(index + 2 < text.size() ? kBase64Alphabet[block & 0x3FU] : '=');
  }
  return output;
}

std::optional<std::string> try_build_osc52_clipboard_sequence(std::string_view text)
{
  if (text.empty() || text.size() > kMaxTerminalClipboardTextBytes)
    return std::nullopt;

  constexpr std::string_view kPrefix = "\x1b]52;c;";
  constexpr std::string_view kSuffix = "\x1b\\";
  auto const encoded = base64_encode(text);

  // Bound above keeps prefix + encoded + suffix well inside size_t.
  auto const total_size = kPrefix.size() + encoded.size() + kSuffix.size();
  std::string sequence;
  sequence.reserve(total_size);
  sequence.append(kPrefix);
  sequence.append(encoded);
  sequence.append(kSuffix);
  return sequence;
}

std::optional<std::string> try_build_osc52_clipboard_transport(std::string_view text, std::optional<std::string_view> tmux)
{
  auto raw_sequence = try_build_osc52_clipboard_sequence(text);
  if (!raw_sequence.has_value() || !tmux.has_value() || tmux->empty())
    return raw_sequence;

  constexpr std::string_view kTmuxPassthroughPrefix = "\x1bPtmux;";
  constexpr std::string_view kSequenceTerminator = "\x1b\\";
  std::string transport = *raw_sequence;
  transport.append(kTmuxPassthroughPrefix);
  for (char byte : *raw_sequence)
  {
    if (byte == '\x1b')
      transport.push_back('\x1b');
    transport.push_back(byte);
  }
  transport.append(kSequenceTerminator);
  return transport;
}

bool copy_text_to_terminal_clipboard(std::string_view text)
{
  auto const* tmux_value = std::getenv("TMUX");
  auto const tmux = tmux_value == nullptr ? std::nullopt : std::optional<std::string_view>(tmux_value);
  auto sequence = try_build_osc52_clipboard_transport(text, tmux);
  if (!sequence.has_value())
    return false;
  return write_all_to_stdout(*sequence);
}

std::optional<std::string_view> copy_text_from_answer(ava::agent::QuestionAnswer const& answer)
{
  for (auto const& option : answer.selected_options)
  {
    if (option.starts_with(kCopyOptionPrefix))
      return std::string_view(option).substr(kCopyOptionPrefix.size());
  }
  return std::nullopt;
}

std::optional<std::string> latest_ava_message_copy_text(std::vector<TranscriptItem> const& transcript)
{
  for (auto item = transcript.rbegin(); item != transcript.rend(); ++item)
  {
    if (item->label != "ava" && item->label != "assistant")
      continue;
    if (item->text.empty())
      continue;
    return item->text;
  }
  return std::nullopt;
}

LatestAssistantCopyResult copy_latest_assistant_message(std::vector<TranscriptItem> const& transcript,
                                                        std::function<bool(std::string_view)> const& terminal_writer)
{
  auto const text = latest_ava_message_copy_text(transcript);
  if (!text)
    return LatestAssistantCopyResult::NoMessage;
  if (text->size() > kMaxTerminalClipboardTextBytes)
    return LatestAssistantCopyResult::Oversize;
  return terminal_writer(*text) ? LatestAssistantCopyResult::RequestSent : LatestAssistantCopyResult::WriteFailure;
}

std::string latest_assistant_copy_status(LatestAssistantCopyResult result)
{
  switch (result)
  {
    case LatestAssistantCopyResult::RequestSent:
      return "last AVA message copy request sent";
    case LatestAssistantCopyResult::NoMessage:
      return "no AVA messages to copy";
    case LatestAssistantCopyResult::Oversize:
      return "latest AVA message exceeds 64 KiB clipboard limit";
    case LatestAssistantCopyResult::WriteFailure:
      return "clipboard copy failed";
  }
  return "clipboard copy failed";
}

std::vector<std::pair<std::string, bool>> capture_tool_detail_visibility(std::vector<TranscriptItem> const& transcript)
{
  std::vector<std::pair<std::string, bool>> overrides;
  for (auto const& item : transcript)
  {
    if (item.tool && item.tool->details_visible)
      overrides.emplace_back(tool_card_identity_key(*item.tool), *item.tool->details_visible);
  }
  return overrides;
}

void carry_tool_detail_visibility(std::vector<std::pair<std::string, bool>> const& overrides, std::vector<TranscriptItem>& transcript)
{
  std::vector<bool> used(overrides.size(), false);
  for (std::size_t item_index = transcript.size(); item_index > 0; --item_index)
  {
    auto& item = transcript[item_index - 1];
    if (!item.tool)
      continue;
    auto const key = tool_card_identity_key(*item.tool);
    for (std::size_t override_index = overrides.size(); override_index > 0; --override_index)
    {
      auto const index = override_index - 1;
      if (used[index] || overrides[index].first != key)
        continue;
      item.tool->details_visible = overrides[index].second;
      used[index] = true;
      break;
    }
  }
}

namespace {

bool item_hosts_thinking_content(TranscriptItem const& item)
{
  if (item.tool)
    return false;
  if (item.label == "thinking")
    return !item.text.empty() || !text_empty(item.text_model);
  if (item.label == "ava")
    return !item.thinking.empty() || !text_empty(item.thinking_model);
  return false;
}

}  // namespace

std::vector<std::pair<std::size_t, bool>> capture_thinking_expansion(std::vector<TranscriptItem> const& transcript)
{
  std::vector<std::pair<std::size_t, bool>> overrides;
  for (std::size_t index = 0; index < transcript.size(); ++index)
  {
    if (transcript[index].thinking_expanded && item_hosts_thinking_content(transcript[index]))
      overrides.emplace_back(index, true);
  }
  return overrides;
}

void carry_thinking_expansion(std::vector<std::pair<std::size_t, bool>> const& overrides, std::vector<TranscriptItem>& transcript,
                              std::ptrdiff_t item_index_shift)
{
  // Captured current-UI true flags are authoritative after apply_capped_transcript_snapshot.
  // Apply copies submitted_transcript, which can restore stale true flags from a prior turn;
  // clear destination expansion before remapping only the currently expanded indices.
  for (auto& item : transcript)
    item.thinking_expanded = false;
  for (auto const& [old_index, expanded] : overrides)
  {
    if (!expanded)
      continue;
    auto const shifted = static_cast<std::ptrdiff_t>(old_index) + item_index_shift;
    if (shifted < 0)
      continue;
    auto const new_index = static_cast<std::size_t>(shifted);
    if (new_index >= transcript.size())
      continue;
    auto& item = transcript[new_index];
    // Refuse stale index ownership onto a replacement non-thinking slot (for example a
    // newly projected tool/text tail that landed on a previously expanded index).
    if (!item_hosts_thinking_content(item))
      continue;
    item.thinking_expanded = true;
  }
}

std::string diff_transcript_text(std::string_view title, std::string_view diff)
{
  auto text = std::string(title);
  text += "\n\n```diff\n";
  text += diff;
  if (!diff.empty() && diff.back() != '\n')
    text += '\n';
  text += "```";
  return text;
}

std::string question_answer_audit_detail(ava::agent::QuestionAnswer const& answer)
{
  auto detail = std::string("question answered");
  if (!answer.selected_options.empty())
  {
    auto const first = std::string_view(answer.selected_options.front());
    detail += first.starts_with(kCopyOptionPrefix) ? ": copy" : ": " + answer.selected_options.front();
  }
  if (!answer.custom_text.empty())
    detail += answer.selected_options.empty() ? ": custom" : ", custom";
  return detail;
}

std::ptrdiff_t push_transcript(ComposerSnapshot& snapshot, TranscriptItem item)
{
  snapshot.transcript.push_back(std::move(item));
  auto const removed = truncate_transcript(snapshot.transcript);
  ++snapshot.transcript_generation;
  return -static_cast<std::ptrdiff_t>(removed);
}

void push_history(std::vector<std::string>& history, std::string input)
{
  static_cast<void>(push_composer_input_history(history, std::move(input)));
}

}  // namespace ava::tui::runtime_transcript

namespace ava::tui {

CappedTranscriptSnapshotUpdate apply_capped_transcript_snapshot(std::vector<TranscriptItem>& destination,
                                                                std::vector<TranscriptItem> const& submitted_transcript,
                                                                std::vector<TranscriptItem> turn_transcript, std::size_t previous_leading_evictions)
{
  destination = submitted_transcript;
  destination.insert(destination.end(), std::make_move_iterator(turn_transcript.begin()), std::make_move_iterator(turn_transcript.end()));
  auto const leading_evictions = destination.size() > kMaxTranscriptItems ? destination.size() - kMaxTranscriptItems : std::size_t{0};
  if (leading_evictions > 0)
    destination.erase(destination.begin(), destination.begin() + static_cast<std::ptrdiff_t>(leading_evictions));
  auto const item_index_shift = static_cast<std::ptrdiff_t>(previous_leading_evictions) - static_cast<std::ptrdiff_t>(leading_evictions);
  return CappedTranscriptSnapshotUpdate{.leading_evictions = leading_evictions, .item_index_shift = item_index_shift};
}

std::optional<std::size_t> toggle_latest_matching_tool_details(std::vector<TranscriptItem>& transcript, std::string_view query, ToolPresentation inherited)
{
  for (std::size_t index = transcript.size(); index > 0; --index)
  {
    auto& item = transcript[index - 1];
    if (!item.tool || !detail::tool_card_matches_copy_query(*item.tool, query))
      continue;
    item.tool->details_visible = detail::tool_card_presentation(*item.tool, inherited) != ToolPresentation::Expanded;
    return index - 1;
  }
  return std::nullopt;
}

bool transcript_item_thinking_is_boundable(TranscriptItem const& item, std::size_t width, bool thinking_visible)
{
  return detail::transcript_item_has_boundable_thinking(item, width, thinking_visible);
}

bool toggle_thinking_expansion_at(std::vector<TranscriptItem>& transcript, std::size_t item_index, std::size_t width, bool thinking_visible)
{
  if (item_index >= transcript.size())
    return false;
  auto& item = transcript[item_index];
  if (!detail::transcript_item_has_boundable_thinking(item, width, thinking_visible))
    return false;
  item.thinking_expanded = !item.thinking_expanded;
  return true;
}

std::optional<std::size_t> toggle_latest_boundable_thinking(std::vector<TranscriptItem>& transcript, std::size_t width, bool thinking_visible)
{
  for (std::size_t index = transcript.size(); index > 0; --index)
  {
    if (!toggle_thinking_expansion_at(transcript, index - 1, width, thinking_visible))
      continue;
    return index - 1;
  }
  return std::nullopt;
}

}  // namespace ava::tui

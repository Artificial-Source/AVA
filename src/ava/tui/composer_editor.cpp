// Applies TUI-thread draft edits, preserving cursor, undo and pasted-text boundaries.
#include "sys.h"
#include "ava/tui/composer_editor.h"
#include "ava/tui/composer_internal.h"

#include <algorithm>
#include <cctype>
#include <optional>
#include <utility>

namespace ava::tui {
namespace {

constexpr std::size_t kMaxUndoItems = 100;
constexpr std::size_t kMaxKillRingItems = 16;
constexpr std::size_t kMaxInputHistoryItems = 100;
constexpr std::size_t kPasteMarkerLineThreshold = 10;
constexpr std::size_t kPasteMarkerByteThreshold = 2000;

std::string trim_ascii_copy(std::string_view text)
{
  while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front())) != 0)
    text.remove_prefix(1);
  while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())) != 0)
    text.remove_suffix(1);
  return std::string(text);
}

bool is_ascii_space_at(std::string_view text, std::size_t cursor)
{
  if (cursor >= text.size())
    return false;
  auto const byte = static_cast<unsigned char>(text[cursor]);
  return (byte & 0x80U) == 0 && std::isspace(byte) != 0;
}

bool is_unicode_space_codepoint(char32_t codepoint)
{
  return codepoint == 0x00A0 || codepoint == 0x1680 || (codepoint >= 0x2000 && codepoint <= 0x200A) || codepoint == 0x2028 || codepoint == 0x2029 ||
         codepoint == 0x202F || codepoint == 0x205F || codepoint == 0x3000;
}

bool is_unicode_punctuation_codepoint(char32_t codepoint)
{
  return (codepoint >= 0x2000 && codepoint <= 0x206F) || (codepoint >= 0x2E00 && codepoint <= 0x2E7F) || (codepoint >= 0x3001 && codepoint <= 0x3003) ||
         (codepoint >= 0x3008 && codepoint <= 0x3020) || codepoint == 0x3030 || codepoint == 0x303D || (codepoint >= 0xFE10 && codepoint <= 0xFE19) ||
         (codepoint >= 0xFE30 && codepoint <= 0xFE4F) || (codepoint >= 0xFF01 && codepoint <= 0xFF0F) || (codepoint >= 0xFF1A && codepoint <= 0xFF20) ||
         (codepoint >= 0xFF3B && codepoint <= 0xFF40) || (codepoint >= 0xFF5B && codepoint <= 0xFF65);
}

bool is_wide_word_codepoint(char32_t codepoint)
{
  return (codepoint >= 0x2E80 && codepoint <= 0x2EFF) || (codepoint >= 0x3040 && codepoint <= 0x30FF) || (codepoint >= 0x31F0 && codepoint <= 0x31FF) ||
         (codepoint >= 0x3400 && codepoint <= 0x4DBF) || (codepoint >= 0x4E00 && codepoint <= 0x9FFF) || (codepoint >= 0xA960 && codepoint <= 0xA97F) ||
         (codepoint >= 0xAC00 && codepoint <= 0xD7A3) || (codepoint >= 0xF900 && codepoint <= 0xFAFF) || (codepoint >= 0xFF10 && codepoint <= 0xFF19) ||
         (codepoint >= 0xFF21 && codepoint <= 0xFF3A) || (codepoint >= 0xFF41 && codepoint <= 0xFF5A) || (codepoint >= 0x20000 && codepoint <= 0x3FFFD);
}

detail::WordSegmentClass word_segment_class_at_impl(std::string_view text, std::size_t cursor)
{
  using detail::WordSegmentClass;
  if (is_ascii_space_at(text, cursor))
    return WordSegmentClass::Space;
  if (cursor >= text.size())
    return WordSegmentClass::Space;
  auto const byte = static_cast<unsigned char>(text[cursor]);
  if ((byte & 0x80U) == 0)
  {
    if (std::isalnum(byte) != 0 || byte == '_')
      return WordSegmentClass::Word;
    return WordSegmentClass::Punctuation;
  }

  auto const length = detail::utf8_sequence_length(byte);
  char32_t codepoint = 0;
  if (!detail::decode_utf8_codepoint(text, cursor, length, codepoint))
    return WordSegmentClass::Word;
  if (is_unicode_space_codepoint(codepoint))
    return WordSegmentClass::Space;
  if (is_unicode_punctuation_codepoint(codepoint))
    return WordSegmentClass::Punctuation;
  if (is_wide_word_codepoint(codepoint))
    return WordSegmentClass::WideWord;
  return WordSegmentClass::Word;
}

// Shared with rendering via detail::terminal_text_cluster_bytes (compact rules, not full UAX #29).
std::size_t next_cluster_cursor(std::string_view text, std::size_t cursor)
{
  cursor = clamp_composer_draft_cursor(text, cursor);
  if (cursor >= text.size())
    return text.size();
  auto const bytes = detail::terminal_text_cluster_bytes(text, cursor);
  return cursor + std::max<std::size_t>(bytes, 1);
}

std::size_t previous_cluster_cursor(std::string_view text, std::size_t cursor)
{
  cursor = clamp_composer_draft_cursor(text, cursor);
  if (cursor == 0)
    return 0;

  // Walk cluster starts forward through the current line prefix so movement stays O(line).
  auto scan_from = std::size_t{0};
  auto const prev_break = text.rfind('\n', cursor - 1);
  if (prev_break != std::string_view::npos)
  {
    if (prev_break + 1 == cursor)
    {
      // Cursor is at the start of a line; the previous cluster is the newline itself.
      return prev_break;
    }
    scan_from = prev_break + 1;
  }

  auto previous = scan_from;
  for (auto index = scan_from; index < cursor;)
  {
    previous = index;
    auto const step = std::max<std::size_t>(detail::terminal_text_cluster_bytes(text, index), 1);
    if (index + step > cursor)
      return index;
    index += step;
  }
  return previous;
}

std::size_t line_start_cursor(std::string_view text, std::size_t cursor)
{
  cursor = clamp_composer_draft_cursor(text, cursor);
  if (cursor == 0)
    return 0;
  auto const line_break = text.rfind('\n', cursor - 1);
  return line_break == std::string_view::npos ? std::size_t{0} : line_break + 1;
}

std::size_t line_end_cursor(std::string_view text, std::size_t cursor)
{
  cursor = clamp_composer_draft_cursor(text, cursor);
  auto const line_break = text.find('\n', cursor);
  return line_break == std::string_view::npos ? text.size() : line_break;
}

std::size_t delete_to_line_end_cursor(std::string_view text, std::size_t cursor)
{
  auto const line_end = line_end_cursor(text, cursor);
  if (line_end == cursor && line_end < text.size() && text[line_end] == '\n')
    return line_end + 1;
  return line_end;
}

std::size_t line_column(std::string_view text, std::size_t cursor)
{
  auto const start = line_start_cursor(text, cursor);
  return cursor - start;
}

std::size_t previous_line_cursor(std::string_view text, std::size_t cursor, std::size_t target_column)
{
  cursor = clamp_composer_draft_cursor(text, cursor);
  auto const current_start = line_start_cursor(text, cursor);
  if (current_start == 0)
    return cursor;
  auto const previous_end = current_start - 1;
  auto const previous_start = line_start_cursor(text, previous_end);
  auto const target = previous_start + target_column;
  return std::min(target, previous_end);
}

std::size_t next_line_cursor(std::string_view text, std::size_t cursor, std::size_t target_column)
{
  cursor = clamp_composer_draft_cursor(text, cursor);
  auto const current_end = line_end_cursor(text, cursor);
  if (current_end >= text.size())
    return cursor;
  auto const next_start = current_end + 1;
  auto const next_end = line_end_cursor(text, next_start);
  auto const target = next_start + target_column;
  return std::min(target, next_end);
}

void clear_yank_tracking(ComposerDraftState& draft)
{
  draft.yank_start = std::string::npos;
  draft.yank_end = std::string::npos;
  draft.yank_ring_index = 0;
}

void clear_vertical_tracking(ComposerDraftState& draft)
{
  draft.vertical_column = std::string::npos;
}

void clear_typing_undo_coalesce(ComposerDraftState& draft)
{
  draft.coalesce_typing_undo = false;
}

void clear_kill_sequence(ComposerDraftState& draft)
{
  draft.kill_sequence = ComposerKillSequence::None;
}

void clear_nonvertical_tracking(ComposerDraftState& draft)
{
  clear_yank_tracking(draft);
  clear_vertical_tracking(draft);
  clear_typing_undo_coalesce(draft);
  clear_kill_sequence(draft);
}

bool insert_text_is_coalesceable_typing(std::string_view text)
{
  if (text.empty())
    return false;
  for (std::size_t index = 0; index < text.size();)
  {
    auto const byte = static_cast<unsigned char>(text[index]);
    if ((byte & 0x80U) == 0)
    {
      if (byte == '\n' || byte == '\r' || byte == '\t' || std::isspace(byte) != 0)
        return false;
      ++index;
      continue;
    }

    auto const length = detail::utf8_sequence_length(byte);
    char32_t codepoint = 0;
    if (!detail::decode_utf8_codepoint(text, index, length, codepoint))
    {
      // Malformed bytes remain ordinary typing; they do not force an undo boundary by themselves.
      ++index;
      continue;
    }
    if (is_unicode_space_codepoint(codepoint))
      return false;
    index += length;
  }
  return true;
}

ComposerDraftSnapshot current_snapshot(ComposerDraftState const& draft)
{
  auto const cursor = clamp_composer_draft_cursor_to_atomic_boundary(draft, draft.cursor);
  return ComposerDraftSnapshot{.text = draft.text, .cursor = cursor, .paste_entries = draft.paste_entries, .next_paste_id = draft.next_paste_id};
}

void push_snapshot(std::vector<ComposerDraftSnapshot>& stack, ComposerDraftSnapshot snapshot)
{
  if (!stack.empty() && stack.back().text == snapshot.text && stack.back().cursor == snapshot.cursor && stack.back().paste_entries == snapshot.paste_entries &&
      stack.back().next_paste_id == snapshot.next_paste_id)
    return;
  stack.push_back(std::move(snapshot));
  if (stack.size() > kMaxUndoItems)
  {
    stack.erase(stack.begin(), stack.begin() + static_cast<std::ptrdiff_t>(stack.size() - kMaxUndoItems));
  }
}

void push_undo(ComposerDraftState& draft)
{
  push_snapshot(draft.undo_stack, current_snapshot(draft));
}

void push_redo(ComposerDraftState& draft)
{
  push_snapshot(draft.redo_stack, current_snapshot(draft));
}

void record_undo(ComposerDraftState& draft)
{
  push_undo(draft);
  draft.redo_stack.clear();
  clear_yank_tracking(draft);
  clear_vertical_tracking(draft);
  clear_typing_undo_coalesce(draft);
  clear_kill_sequence(draft);
}

// Record an undo boundary without breaking an in-progress kill accumulation sequence.
void record_undo_preserving_kill_sequence(ComposerDraftState& draft)
{
  auto const kill_sequence = draft.kill_sequence;
  push_undo(draft);
  draft.redo_stack.clear();
  clear_yank_tracking(draft);
  clear_vertical_tracking(draft);
  clear_typing_undo_coalesce(draft);
  draft.kill_sequence = kill_sequence;
}

void remember_kill(ComposerDraftState& draft, std::string killed, ComposerKillSequence direction)
{
  if (killed.empty() || direction == ComposerKillSequence::None)
    return;

  if (draft.kill_sequence == direction && !draft.kill_ring.empty())
  {
    if (direction == ComposerKillSequence::Backward)
      draft.kill_ring.front().insert(0, killed);
    else
      draft.kill_ring.front().append(killed);
    draft.kill_buffer = draft.kill_ring.front();
  }
  else
  {
    draft.kill_buffer = std::move(killed);
    draft.kill_ring.insert(draft.kill_ring.begin(), draft.kill_buffer);
    if (draft.kill_ring.size() > kMaxKillRingItems)
      draft.kill_ring.resize(kMaxKillRingItems);
  }

  draft.kill_sequence = direction;
  clear_yank_tracking(draft);
  clear_vertical_tracking(draft);
  clear_typing_undo_coalesce(draft);
}

void ensure_kill_ring(ComposerDraftState& draft)
{
  if (!draft.kill_ring.empty() || draft.kill_buffer.empty())
    return;
  draft.kill_ring.push_back(draft.kill_buffer);
}

std::size_t input_line_count(std::string_view text)
{
  if (text.empty())
    return 0;
  return static_cast<std::size_t>(std::ranges::count(text, '\n')) + 1;
}

bool should_collapse_paste(std::string_view text)
{
  return input_line_count(text) > kPasteMarkerLineThreshold || text.size() > kPasteMarkerByteThreshold;
}

std::string paste_marker(std::size_t id, std::string_view text)
{
  auto const lines = input_line_count(text);
  if (lines > kPasteMarkerLineThreshold)
    return "[paste #" + std::to_string(id) + " +" + std::to_string(lines) + " lines]";
  return "[paste #" + std::to_string(id) + " " + std::to_string(text.size()) + " chars]";
}

struct PasteMarkerRange
{
  std::size_t start = 0;
  std::size_t end = 0;
};

bool active_paste_entry_matches(ComposerDraftState const& draft, ComposerPasteEntry const& entry)
{
  return entry.start != std::string::npos && !entry.marker.empty() && entry.start + entry.marker.size() <= draft.text.size() &&
         draft.text.compare(entry.start, entry.marker.size(), entry.marker) == 0;
}

std::optional<PasteMarkerRange> paste_marker_starting_at(ComposerDraftState const& draft, std::size_t cursor)
{
  cursor = clamp_composer_draft_cursor(draft.text, cursor);
  for (auto const& entry : draft.paste_entries)
  {
    if (entry.start == cursor && active_paste_entry_matches(draft, entry))
    {
      return PasteMarkerRange{.start = cursor, .end = cursor + entry.marker.size()};
    }
  }
  return std::nullopt;
}

std::optional<PasteMarkerRange> paste_marker_touching_left(ComposerDraftState const& draft, std::size_t cursor)
{
  cursor = clamp_composer_draft_cursor(draft.text, cursor);
  for (auto const& entry : draft.paste_entries)
  {
    if (!active_paste_entry_matches(draft, entry))
      continue;
    auto const end = entry.start + entry.marker.size();
    if (entry.start < cursor && cursor <= end)
      return PasteMarkerRange{.start = entry.start, .end = end};
  }
  return std::nullopt;
}

void shift_paste_markers_for_insert(ComposerDraftState& draft, std::size_t cursor, std::size_t length)
{
  for (auto& entry : draft.paste_entries)
  {
    if (!active_paste_entry_matches(draft, entry))
      continue;
    auto const end = entry.start + entry.marker.size();
    if (entry.start < cursor && cursor < end)
    {
      entry.start = std::string::npos;
      continue;
    }
    if (entry.start >= cursor)
      entry.start += length;
  }
  std::erase_if(draft.paste_entries, [](ComposerPasteEntry const& entry) { return entry.start == std::string::npos; });
}

void shift_paste_markers_for_erase(ComposerDraftState& draft, std::size_t start, std::size_t end)
{
  if (end <= start)
    return;
  auto const length = end - start;
  for (auto& entry : draft.paste_entries)
  {
    if (!active_paste_entry_matches(draft, entry))
      continue;
    auto const marker_end = entry.start + entry.marker.size();
    if (end <= entry.start)
    {
      entry.start -= length;
      continue;
    }
    if (start >= marker_end)
      continue;
    entry.start = std::string::npos;
  }
  std::erase_if(draft.paste_entries, [](ComposerPasteEntry const& entry) { return entry.start == std::string::npos; });
}

void shift_paste_markers_for_replace(ComposerDraftState& draft, std::size_t start, std::size_t end, std::size_t replacement_size)
{
  if (end < start)
    std::swap(start, end);
  auto const erased_size = end - start;
  for (auto& entry : draft.paste_entries)
  {
    if (!active_paste_entry_matches(draft, entry))
      continue;
    auto const marker_end = entry.start + entry.marker.size();
    if (end <= entry.start)
    {
      entry.start = entry.start - erased_size + replacement_size;
      continue;
    }
    if (start >= marker_end)
      continue;
    entry.start = std::string::npos;
  }
  std::erase_if(draft.paste_entries, [](ComposerPasteEntry const& entry) { return entry.start == std::string::npos; });
}

std::size_t previous_atomic_cursor_at(ComposerDraftState const& draft, std::size_t cursor)
{
  cursor = clamp_composer_draft_cursor(draft.text, cursor);
  if (auto marker = paste_marker_touching_left(draft, cursor))
    return marker->start;
  return previous_cluster_cursor(draft.text, cursor);
}

std::size_t next_atomic_cursor_at(ComposerDraftState const& draft, std::size_t cursor)
{
  cursor = clamp_composer_draft_cursor(draft.text, cursor);
  if (auto marker = paste_marker_starting_at(draft, cursor))
    return marker->end;
  if (auto marker = paste_marker_touching_left(draft, cursor); marker && cursor < marker->end)
    return marker->end;
  return next_cluster_cursor(draft.text, cursor);
}

std::size_t previous_atomic_input_cursor(ComposerDraftState const& draft)
{
  return previous_atomic_cursor_at(draft, draft.cursor);
}

std::size_t next_atomic_input_cursor(ComposerDraftState const& draft)
{
  return next_atomic_cursor_at(draft, draft.cursor);
}

// Word movement classifies only compact cluster starts (and paste markers), never ZWJ/mark interiors.
std::size_t previous_atomic_word_cursor(ComposerDraftState const& draft)
{
  auto cursor = clamp_composer_draft_cursor_to_atomic_boundary(draft, draft.cursor);
  while (cursor > 0)
  {
    auto const previous = previous_atomic_cursor_at(draft, cursor);
    if (word_segment_class_at_impl(draft.text, previous) != detail::WordSegmentClass::Space)
      break;
    cursor = previous;
  }
  if (cursor == 0)
    return 0;
  if (auto marker = paste_marker_touching_left(draft, cursor))
    return marker->start;
  auto const target_class = word_segment_class_at_impl(draft.text, previous_atomic_cursor_at(draft, cursor));
  while (cursor > 0)
  {
    if (auto marker = paste_marker_touching_left(draft, cursor))
      return marker->start;
    auto const previous = previous_atomic_cursor_at(draft, cursor);
    if (word_segment_class_at_impl(draft.text, previous) != target_class)
      break;
    cursor = previous;
  }
  return cursor;
}

std::size_t next_atomic_word_cursor(ComposerDraftState const& draft)
{
  auto cursor = clamp_composer_draft_cursor_to_atomic_boundary(draft, draft.cursor);
  if (auto marker = paste_marker_starting_at(draft, cursor))
    return marker->end;
  if (auto marker = paste_marker_touching_left(draft, cursor); marker && cursor < marker->end)
    return marker->end;
  while (cursor < draft.text.size() && word_segment_class_at_impl(draft.text, cursor) == detail::WordSegmentClass::Space)
  {
    cursor = next_atomic_cursor_at(draft, cursor);
  }
  if (auto marker = paste_marker_starting_at(draft, cursor))
    return marker->end;
  if (cursor >= draft.text.size())
    return cursor;
  auto const target_class = word_segment_class_at_impl(draft.text, cursor);
  while (cursor < draft.text.size() && word_segment_class_at_impl(draft.text, cursor) == target_class)
  {
    if (auto marker = paste_marker_starting_at(draft, cursor))
      return marker->end;
    if (auto marker = paste_marker_touching_left(draft, cursor); marker && cursor < marker->end)
      return marker->end;
    cursor = next_atomic_cursor_at(draft, cursor);
  }
  return cursor;
}

// TUI thread: remove an owned draft range and record undo/kill history before
// mutation. Return whether bytes changed; repair a UTF-8 join before returning.
bool erase_range(ComposerDraftState& draft, std::size_t start, std::size_t end, ComposerKillSequence kill_direction)
{
  start = clamp_composer_draft_cursor_to_atomic_boundary(draft, start);
  end = clamp_composer_draft_cursor_to_atomic_boundary(draft, end);
  if (end < start)
    std::swap(start, end);
  if (start == end)
    return false;
  auto killed = draft.text.substr(start, end - start);
  if (kill_direction == ComposerKillSequence::None)
  {
    record_undo(draft);
  }
  else
  {
    record_undo_preserving_kill_sequence(draft);
    remember_kill(draft, std::move(killed), kill_direction);
  }
  shift_paste_markers_for_erase(draft, start, end);
  draft.text.erase(start, end - start);
  // Removing a separator can join previously malformed bytes into one character.
  draft.cursor = clamp_composer_draft_cursor(draft.text, start);
  return true;
}

}  // namespace

detail::WordSegmentClass detail::word_segment_class_at(std::string_view text, std::size_t cursor)
{
  return word_segment_class_at_impl(text, cursor);
}

std::size_t clamp_composer_draft_cursor(std::string_view text, std::size_t cursor)
{
  cursor = std::min(cursor, text.size());
  if (cursor == 0 || cursor >= text.size())
    return cursor;
  if (!detail::is_utf8_continuation(static_cast<unsigned char>(text[cursor])))
    return cursor;

  // Only snap to a starter when this continuation belongs to a valid sequence covering `cursor`.
  // Orphan continuation bytes stay on their own byte boundary so malformed UTF-8 remains editable.
  auto start = cursor;
  while (start > 0 && detail::is_utf8_continuation(static_cast<unsigned char>(text[start - 1])))
  {
    --start;
  }
  if (start == 0)
    return cursor;

  auto const starter = start - 1;
  auto const expected_length = detail::utf8_sequence_length(static_cast<unsigned char>(text[starter]));
  if (expected_length <= 1)
    return cursor;

  char32_t codepoint = 0;
  if (!detail::decode_utf8_codepoint(text, starter, expected_length, codepoint))
    return cursor;
  if (starter < cursor && cursor < starter + expected_length)
    return starter;
  return cursor;
}

std::size_t clamp_composer_draft_cursor_to_atomic_boundary(ComposerDraftState const& draft, std::size_t cursor)
{
  cursor = clamp_composer_draft_cursor(draft.text, cursor);
  for (auto const& entry : draft.paste_entries)
  {
    if (!active_paste_entry_matches(draft, entry))
      continue;
    auto const end = entry.start + entry.marker.size();
    if (entry.start < cursor && cursor < end)
    {
      auto const left = cursor - entry.start;
      auto const right = end - cursor;
      return left <= right ? entry.start : end;
    }
  }

  if (cursor == 0 || cursor >= draft.text.size())
    return cursor;

  auto scan_from = std::size_t{0};
  if (cursor > 0)
  {
    auto const prev_break = draft.text.rfind('\n', cursor - 1);
    if (prev_break != std::string::npos)
      scan_from = prev_break + 1;
  }
  for (auto index = scan_from; index < cursor;)
  {
    auto const step = std::max<std::size_t>(detail::terminal_text_cluster_bytes(draft.text, index), 1);
    if (index + step > cursor)
    {
      auto const left = cursor - index;
      auto const right = index + step - cursor;
      return left <= right ? index : index + step;
    }
    index += step;
  }
  return cursor;
}

void reset_composer_draft(ComposerDraftState& draft, std::string text, std::size_t cursor)
{
  draft.text = std::move(text);
  draft.paste_entries.clear();
  draft.next_paste_id = 1;
  draft.cursor = cursor == std::string::npos ? draft.text.size() : clamp_composer_draft_cursor_to_atomic_boundary(draft, cursor);
  draft.undo_stack.clear();
  draft.redo_stack.clear();
  clear_nonvertical_tracking(draft);
  draft.coalesce_typing_undo = false;
  draft.kill_sequence = ComposerKillSequence::None;
}

bool replace_composer_draft(ComposerDraftState& draft, std::string text, std::size_t cursor)
{
  ComposerDraftState probe;
  probe.text = text;
  auto const next_cursor = cursor == std::string::npos ? text.size() : clamp_composer_draft_cursor_to_atomic_boundary(probe, cursor);
  if (draft.text == text && clamp_composer_draft_cursor_to_atomic_boundary(draft, draft.cursor) == next_cursor)
    return false;
  record_undo(draft);
  draft.text = std::move(text);
  draft.paste_entries.clear();
  draft.next_paste_id = 1;
  draft.cursor = next_cursor;
  return true;
}

// TUI thread: replace the selected byte range while retaining undo and paste
// ownership. Borrow replacement only for the call; return whether a replacement was performed.
bool replace_composer_draft_range(ComposerDraftState& draft, std::size_t start, std::size_t end, std::string_view replacement)
{
  start = clamp_composer_draft_cursor_to_atomic_boundary(draft, start);
  end = clamp_composer_draft_cursor_to_atomic_boundary(draft, end);
  if (end < start)
    std::swap(start, end);
  if (start == end && replacement.empty())
    return false;
  record_undo(draft);
  shift_paste_markers_for_replace(draft, start, end, replacement.size());
  draft.text.replace(start, end - start, replacement);
  // The replacement endpoint may now lie inside a character completed by the suffix.
  draft.cursor = clamp_composer_draft_cursor(draft.text, start + replacement.size());
  return true;
}

// TUI thread: insert call-scoped text into the owned draft, coalescing compatible
// typing undo entries. Return whether nonempty text was inserted with a valid cursor.
bool insert_composer_draft_text(ComposerDraftState& draft, std::string_view text)
{
  if (text.empty())
    return false;

  draft.cursor = clamp_composer_draft_cursor_to_atomic_boundary(draft, draft.cursor);
  auto const coalesce = draft.coalesce_typing_undo && insert_text_is_coalesceable_typing(text) && draft.kill_sequence == ComposerKillSequence::None;
  if (!coalesce)
    record_undo(draft);
  else
  {
    // Extend the current typing undo group; still discard any redo branch.
    draft.redo_stack.clear();
    clear_yank_tracking(draft);
    clear_vertical_tracking(draft);
    clear_kill_sequence(draft);
  }

  shift_paste_markers_for_insert(draft, draft.cursor, text.size());
  draft.text.insert(draft.cursor, text);
  // A starter inserted before an orphan continuation can complete one character.
  draft.cursor = clamp_composer_draft_cursor(draft.text, draft.cursor + text.size());
  draft.coalesce_typing_undo = insert_text_is_coalesceable_typing(text);
  return true;
}

bool replace_composer_backslash_before_cursor_with_newline(ComposerDraftState& draft)
{
  auto const cursor = clamp_composer_draft_cursor_to_atomic_boundary(draft, draft.cursor);
  if (cursor == 0 || draft.text[cursor - 1] != '\\')
    return false;
  return replace_composer_draft_range(draft, cursor - 1, cursor, "\n");
}

bool insert_composer_paste_text(ComposerDraftState& draft, std::string_view text)
{
  if (text.empty())
    return false;
  if (!should_collapse_paste(text))
  {
    // Pastes never join ordinary typing undo groups, even when small enough to stay literal.
    clear_typing_undo_coalesce(draft);
    auto const changed = insert_composer_draft_text(draft, text);
    clear_typing_undo_coalesce(draft);
    return changed;
  }

  record_undo(draft);
  auto const id = draft.next_paste_id++;
  auto marker = paste_marker(id, text);
  auto marker_text = marker;
  draft.cursor = clamp_composer_draft_cursor_to_atomic_boundary(draft, draft.cursor);
  auto const marker_start = draft.cursor;
  shift_paste_markers_for_insert(draft, marker_start, marker_text.size());
  draft.text.insert(draft.cursor, marker_text);
  draft.cursor += marker_text.size();
  draft.paste_entries.push_back(ComposerPasteEntry{.id = id, .marker = std::move(marker), .text = std::string(text), .start = marker_start});
  clear_typing_undo_coalesce(draft);
  return true;
}

bool jump_composer_draft_to_character(ComposerDraftState& draft, std::string_view character, bool forward)
{
  if (character.empty() || draft.text.empty())
    return false;

  clear_yank_tracking(draft);
  clear_nonvertical_tracking(draft);
  draft.cursor = clamp_composer_draft_cursor_to_atomic_boundary(draft, draft.cursor);

  if (forward)
  {
    auto const search_from = draft.cursor >= draft.text.size() ? draft.text.size() : next_atomic_cursor_at(draft, draft.cursor);
    auto const found = draft.text.find(character, search_from);
    if (found == std::string::npos)
      return false;
    draft.cursor = clamp_composer_draft_cursor_to_atomic_boundary(draft, found);
    return true;
  }

  if (draft.cursor == 0)
    return false;
  auto const search_before = previous_atomic_cursor_at(draft, draft.cursor);
  auto const found = draft.text.rfind(character, search_before);
  if (found == std::string::npos)
    return false;
  draft.cursor = clamp_composer_draft_cursor_to_atomic_boundary(draft, found);
  return true;
}

// TUI thread: apply one editor action to caller-owned state and its undo history.
// Report the action's edit result; tracking metadata can reset even on false.
bool apply_composer_draft_action(ComposerDraftState& draft, TuiAction action)
{
  // Every action starts on a compact cluster / paste-marker boundary. Malformed orphan bytes remain addressable.
  draft.cursor = clamp_composer_draft_cursor_to_atomic_boundary(draft, draft.cursor);
  switch (action)
  {
    case TuiAction::ClearInput:
      if (draft.text.empty())
        return false;
      {
        auto killed = draft.text;
        record_undo_preserving_kill_sequence(draft);
        remember_kill(draft, std::move(killed), ComposerKillSequence::Backward);
      }
      draft.text.clear();
      draft.cursor = 0;
      draft.paste_entries.clear();
      draft.next_paste_id = 1;
      return true;
    case TuiAction::DeleteBackward:
      // Ordinary backspace deletes one cluster and does not contribute to the kill ring.
      return erase_range(draft, previous_atomic_input_cursor(draft), draft.cursor, ComposerKillSequence::None);
    case TuiAction::DeleteForward:
      // Ordinary forward-delete deletes one cluster and does not contribute to the kill ring.
      return erase_range(draft, draft.cursor, next_atomic_input_cursor(draft), ComposerKillSequence::None);
    case TuiAction::DeleteWordBackward:
      return erase_range(draft, previous_atomic_word_cursor(draft), draft.cursor, ComposerKillSequence::Backward);
    case TuiAction::DeleteWordForward:
      return erase_range(draft, draft.cursor, next_atomic_word_cursor(draft), ComposerKillSequence::Forward);
    case TuiAction::DeleteToLineStart:
      return erase_range(draft, line_start_cursor(draft.text, draft.cursor), draft.cursor, ComposerKillSequence::Backward);
    case TuiAction::DeleteToLineEnd:
      return erase_range(draft, draft.cursor, delete_to_line_end_cursor(draft.text, draft.cursor), ComposerKillSequence::Forward);
    case TuiAction::CursorLeft:
      clear_nonvertical_tracking(draft);
      draft.cursor = previous_atomic_input_cursor(draft);
      return true;
    case TuiAction::CursorRight:
      clear_nonvertical_tracking(draft);
      draft.cursor = next_atomic_input_cursor(draft);
      return true;
    case TuiAction::CursorUp: {
      if (draft.vertical_column == std::string::npos)
        draft.vertical_column = line_column(draft.text, draft.cursor);
      // Keep logical-line byte-column targeting, then snap off codepoint/cluster interiors.
      auto const next = clamp_composer_draft_cursor_to_atomic_boundary(draft, previous_line_cursor(draft.text, draft.cursor, draft.vertical_column));
      if (next == draft.cursor)
        return false;
      clear_yank_tracking(draft);
      draft.cursor = next;
      return true;
    }
    case TuiAction::CursorDown: {
      if (draft.vertical_column == std::string::npos)
        draft.vertical_column = line_column(draft.text, draft.cursor);
      auto const next = clamp_composer_draft_cursor_to_atomic_boundary(draft, next_line_cursor(draft.text, draft.cursor, draft.vertical_column));
      if (next == draft.cursor)
        return false;
      clear_yank_tracking(draft);
      draft.cursor = next;
      return true;
    }
    case TuiAction::CursorLineStart:
      clear_nonvertical_tracking(draft);
      draft.cursor = line_start_cursor(draft.text, draft.cursor);
      return true;
    case TuiAction::CursorLineEnd:
      clear_nonvertical_tracking(draft);
      draft.cursor = line_end_cursor(draft.text, draft.cursor);
      return true;
    case TuiAction::CursorWordLeft:
      clear_nonvertical_tracking(draft);
      draft.cursor = previous_atomic_word_cursor(draft);
      return true;
    case TuiAction::CursorWordRight:
      clear_nonvertical_tracking(draft);
      draft.cursor = next_atomic_word_cursor(draft);
      return true;
    case TuiAction::JumpForward:
    case TuiAction::JumpBackward:
      return false;
    case TuiAction::Undo: {
      if (draft.undo_stack.empty())
        return false;
      push_redo(draft);
      auto previous = std::move(draft.undo_stack.back());
      draft.undo_stack.pop_back();
      draft.text = std::move(previous.text);
      draft.paste_entries = std::move(previous.paste_entries);
      draft.next_paste_id = previous.next_paste_id;
      draft.cursor = clamp_composer_draft_cursor_to_atomic_boundary(draft, previous.cursor);
      clear_nonvertical_tracking(draft);
      return true;
    }
    case TuiAction::Redo: {
      if (draft.redo_stack.empty())
        return false;
      push_undo(draft);
      auto next = std::move(draft.redo_stack.back());
      draft.redo_stack.pop_back();
      draft.text = std::move(next.text);
      draft.paste_entries = std::move(next.paste_entries);
      draft.next_paste_id = next.next_paste_id;
      draft.cursor = clamp_composer_draft_cursor_to_atomic_boundary(draft, next.cursor);
      clear_nonvertical_tracking(draft);
      return true;
    }
    case TuiAction::Yank: {
      ensure_kill_ring(draft);
      if (draft.kill_ring.empty() || draft.kill_ring.front().empty())
        return false;
      auto const yanked = draft.kill_ring.front();
      record_undo(draft);
      auto const start = draft.cursor;
      shift_paste_markers_for_insert(draft, draft.cursor, yanked.size());
      draft.text.insert(draft.cursor, yanked);
      draft.cursor += yanked.size();
      draft.kill_buffer = yanked;
      draft.yank_start = start;
      draft.yank_end = draft.cursor;
      draft.yank_ring_index = 0;
      // YankPop needs the raw inserted span, even if its endpoint lies inside a
      // newly completed character. Only the visible cursor is normalized.
      draft.cursor = clamp_composer_draft_cursor(draft.text, draft.cursor);
      return true;
    }
    case TuiAction::YankPop: {
      ensure_kill_ring(draft);
      if (draft.kill_ring.size() < 2 || draft.yank_start == std::string::npos || draft.yank_end == std::string::npos || draft.yank_start > draft.text.size() ||
          draft.yank_end > draft.text.size() || draft.yank_start > draft.yank_end)
      {
        return false;
      }
      auto const start = draft.yank_start;
      auto const end = draft.yank_end;
      auto const next_ring_index = (draft.yank_ring_index + 1) % draft.kill_ring.size();
      auto const replacement = draft.kill_ring[next_ring_index];
      record_undo(draft);
      shift_paste_markers_for_replace(draft, start, end, replacement.size());
      draft.text.replace(start, end - start, replacement);
      draft.cursor = start + replacement.size();
      draft.kill_buffer = replacement;
      draft.yank_start = start;
      draft.yank_end = draft.cursor;
      draft.yank_ring_index = next_ring_index;
      // Keep the raw replacement span for the next cycle before moving the cursor.
      draft.cursor = clamp_composer_draft_cursor(draft.text, draft.cursor);
      return true;
    }
    case TuiAction::Submit:
    case TuiAction::NewLine:
    case TuiAction::Cancel:
    case TuiAction::CopySelection:
    case TuiAction::CopyLatestAssistant:
    case TuiAction::PromptStash:
    case TuiAction::ExternalEditor:
    case TuiAction::Suspend:
    case TuiAction::ClipboardPasteImage:
    case TuiAction::HistoryPrev:
    case TuiAction::HistoryNext:
    case TuiAction::PalettePrev:
    case TuiAction::PaletteNext:
    case TuiAction::SelectPrev:
    case TuiAction::SelectNext:
    case TuiAction::SelectPageUp:
    case TuiAction::SelectPageDown:
    case TuiAction::SelectConfirm:
    case TuiAction::SelectCancel:
    case TuiAction::AutocompleteAccept:
    case TuiAction::PromptAllow:
    case TuiAction::PromptDeny:
    case TuiAction::DetailsToggle:
    case TuiAction::PageUp:
    case TuiAction::PageDown:
    case TuiAction::TranscriptHalfPageUp:
    case TuiAction::TranscriptHalfPageDown:
    case TuiAction::ModeToggle:
    case TuiAction::Interrupt:
    case TuiAction::Exit:
    case TuiAction::VariantCycle:
    case TuiAction::ReasoningSelect:
    case TuiAction::ThinkingToggle:
    case TuiAction::ModelSelect:
    case TuiAction::ModelCycleForward:
    case TuiAction::ModelCycleBackward:
    case TuiAction::ModelsSave:
    case TuiAction::ModelsEnableAll:
    case TuiAction::ModelsClearAll:
    case TuiAction::ModelsToggleProvider:
    case TuiAction::ModelsReorderUp:
    case TuiAction::ModelsReorderDown:
    case TuiAction::MessageFollowUp:
    case TuiAction::MessageDequeue:
    case TuiAction::MessagePrev:
    case TuiAction::MessageNext:
    case TuiAction::JumpToBottom:
    case TuiAction::SessionNew:
    case TuiAction::SessionTree:
    case TuiAction::SessionFork:
    case TuiAction::SessionResume:
    case TuiAction::SessionTogglePath:
    case TuiAction::SessionToggleSort:
    case TuiAction::SessionToggleNamedFilter:
    case TuiAction::SessionRename:
    case TuiAction::SessionArchive:
    case TuiAction::SessionArchiveNoninvasive:
    case TuiAction::SessionSummarizeParent:
    case TuiAction::TreeFoldOrUp:
    case TuiAction::TreeUnfoldOrDown:
    case TuiAction::TreeEditLabel:
    case TuiAction::TreeToggleLabelTimestamp:
    case TuiAction::TreeFilterLabeledOnly:
    case TuiAction::TreeFilterAll:
    case TuiAction::OverviewToggle:
      return false;
  }
  return false;
}

bool push_composer_input_history(std::vector<std::string>& history, std::string input)
{
  input = trim_ascii_copy(input);
  if (input.empty())
    return false;
  if (!history.empty() && history.back() == input)
    return false;
  history.push_back(std::move(input));
  if (history.size() > kMaxInputHistoryItems)
  {
    history.erase(history.begin(), history.begin() + static_cast<std::ptrdiff_t>(history.size() - kMaxInputHistoryItems));
  }
  return true;
}

void clear_composer_input_history_browse(std::optional<std::size_t>& history_index, std::string& draft_input)
{
  history_index.reset();
  draft_input.clear();
}

bool browse_composer_input_history(ComposerDraftState& draft, std::vector<std::string> const& history, std::optional<std::size_t>& history_index,
                                   std::string& draft_input, bool previous)
{
  if (history.empty())
    return false;

  if (previous)
  {
    if (!history_index)
    {
      draft_input = draft.text;
      history_index = history.size() - 1;
    }
    else if (*history_index > 0)
    {
      --(*history_index);
    }
    else
    {
      return false;
    }
    return replace_composer_draft(draft, history[*history_index], 0);
  }

  if (!history_index)
    return false;

  if (*history_index + 1 >= history.size())
  {
    auto restored = std::move(draft_input);
    clear_composer_input_history_browse(history_index, draft_input);
    return replace_composer_draft(draft, std::move(restored));
  }

  ++(*history_index);
  return replace_composer_draft(draft, history[*history_index]);
}

std::string normalize_composer_paste_text(std::string_view text)
{
  std::string output;
  output.reserve(text.size());
  for (std::size_t index = 0; index < text.size(); ++index)
  {
    auto const byte = static_cast<unsigned char>(text[index]);
    if (byte == '\r')
    {
      if (index + 1 < text.size() && text[index + 1] == '\n')
        ++index;
      output.push_back('\n');
      continue;
    }
    if (byte == '\n' || byte == '\t' || byte >= 0x20)
      output.push_back(static_cast<char>(byte));
  }
  return output;
}

std::string expanded_composer_draft_text(ComposerDraftState const& draft)
{
  if (draft.text.empty() || draft.paste_entries.empty())
    return draft.text;

  std::string output;
  output.reserve(draft.text.size());
  for (std::size_t index = 0; index < draft.text.size();)
  {
    auto const found = std::ranges::find_if(draft.paste_entries,
                                            [&](ComposerPasteEntry const& entry) { return entry.start == index && active_paste_entry_matches(draft, entry); });
    if (found != draft.paste_entries.end())
    {
      output += found->text;
      index += found->marker.size();
      continue;
    }
    output.push_back(draft.text[index]);
    ++index;
  }
  return output;
}

}  // namespace ava::tui

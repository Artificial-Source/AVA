#pragma once

#include "ava/tui/keybindings.h"
#include "ava/core/NoLogString.h"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ava::tui {

struct ComposerPasteEntry
{
  std::size_t id = 0;
  std::string marker;
  ava::core::NoLogString text;
  std::size_t start = std::string::npos;

  friend bool operator==(ComposerPasteEntry const&, ComposerPasteEntry const&) = default;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

struct ComposerDraftSnapshot
{
  ava::core::NoLogString text;
  std::size_t cursor = 0;
  std::vector<ComposerPasteEntry> paste_entries;
  std::size_t next_paste_id = 1;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

// Direction of the active consecutive kill sequence for kill-ring accumulation.
enum class ComposerKillSequence
{
  None,
  Backward,
  Forward,
};

struct ComposerDraftState
{
  ava::core::NoLogString text;
  std::size_t cursor = 0;
  ava::core::NoLogString kill_buffer;
  std::vector<ComposerDraftSnapshot> undo_stack;
  std::vector<ComposerDraftSnapshot> redo_stack;
  std::vector<ava::core::NoLogString> kill_ring;
  std::size_t yank_start = std::string::npos;
  std::size_t yank_end = std::string::npos;
  std::size_t yank_ring_index = 0;
  std::vector<ComposerPasteEntry> paste_entries;
  std::size_t next_paste_id = 1;
  std::size_t vertical_column = std::string::npos;
  // When true, the next contiguous ordinary non-whitespace insert extends the current undo group.
  bool coalesce_typing_undo = false;
  ComposerKillSequence kill_sequence = ComposerKillSequence::None;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

[[nodiscard]] std::size_t clamp_composer_draft_cursor(std::string_view text, std::size_t cursor);
[[nodiscard]] std::size_t clamp_composer_draft_cursor_to_atomic_boundary(ComposerDraftState const& draft, std::size_t cursor);
void reset_composer_draft(ComposerDraftState& draft, std::string text = {}, std::size_t cursor = std::string::npos);
bool replace_composer_draft(ComposerDraftState& draft, std::string text, std::size_t cursor = std::string::npos);
bool replace_composer_draft_range(ComposerDraftState& draft, std::size_t start, std::size_t end, std::string_view replacement);
bool insert_composer_draft_text(ComposerDraftState& draft, std::string_view text);
bool replace_composer_backslash_before_cursor_with_newline(ComposerDraftState& draft);
bool insert_composer_paste_text(ComposerDraftState& draft, std::string_view text);
bool jump_composer_draft_to_character(ComposerDraftState& draft, std::string_view character, bool forward);
bool apply_composer_draft_action(ComposerDraftState& draft, TuiAction action);
bool push_composer_input_history(std::vector<std::string>& history, std::string input);
void clear_composer_input_history_browse(std::optional<std::size_t>& history_index, std::string& draft_input);
bool browse_composer_input_history(ComposerDraftState& draft, std::vector<std::string> const& history, std::optional<std::size_t>& history_index,
                                   std::string& draft_input, bool previous);
[[nodiscard]] std::string normalize_composer_paste_text(std::string_view text);
[[nodiscard]] std::string expanded_composer_draft_text(ComposerDraftState const& draft);

}  // namespace ava::tui

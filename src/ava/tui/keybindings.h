#pragma once

#include "ava/tui/terminal.h"
#include "ava/core/result.h"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ava::tui {

enum class TuiAction
{
  Submit,
  NewLine,
  Cancel,
  ClearInput,
  CopySelection,
  CopyLatestAssistant,
  PromptStash,
  ExternalEditor,
  Suspend,
  ClipboardPasteImage,
  DeleteBackward,
  DeleteForward,
  HistoryPrev,
  HistoryNext,
  PalettePrev,
  PaletteNext,
  SelectPrev,
  SelectNext,
  SelectPageUp,
  SelectPageDown,
  SelectConfirm,
  SelectCancel,
  CursorLeft,
  CursorRight,
  CursorUp,
  CursorDown,
  CursorLineStart,
  CursorLineEnd,
  CursorWordLeft,
  CursorWordRight,
  JumpForward,
  JumpBackward,
  DeleteWordBackward,
  DeleteWordForward,
  DeleteToLineStart,
  DeleteToLineEnd,
  Undo,
  Redo,
  Yank,
  YankPop,
  AutocompleteAccept,
  PromptAllow,
  PromptDeny,
  DetailsToggle,
  PageUp,
  PageDown,
  TranscriptHalfPageUp,
  TranscriptHalfPageDown,
  ModeToggle,
  Interrupt,
  Exit,
  VariantCycle,
  ReasoningSelect,
  ThinkingToggle,
  ModelSelect,
  ModelCycleForward,
  ModelCycleBackward,
  ModelsSave,
  ModelsEnableAll,
  ModelsClearAll,
  ModelsToggleProvider,
  ModelsReorderUp,
  ModelsReorderDown,
  MessageFollowUp,
  MessageDequeue,
  MessagePrev,
  MessageNext,
  JumpToBottom,
  SessionNew,
  SessionTree,
  SessionFork,
  SessionResume,
  SessionTogglePath,
  SessionToggleSort,
  SessionToggleNamedFilter,
  SessionRename,
  SessionArchive,
  SessionArchiveNoninvasive,
  SessionSummarizeParent,
  TreeFoldOrUp,
  TreeUnfoldOrDown,
  TreeEditLabel,
  TreeToggleLabelTimestamp,
  TreeFilterLabeledOnly,
  TreeFilterAll,
  // Process-local startup overview toggle. Intentionally unbound by default;
  // public config id is app.overview.toggle. /overview remains always available.
  OverviewToggle,
};

struct TuiKeyBindings
{
  std::vector<std::pair<TuiAction, std::vector<terminal::Key>>> bindings;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

struct TuiKeyBindingHelpItem
{
  // Human-facing primary label for help/hotkeys/settings surfaces.
  std::string label;
  // Canonical machine action id for config JSON and /keybindings set|reset.
  std::string action;
  std::string description;
  std::string keys;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

[[nodiscard]] TuiKeyBindings default_key_bindings();
// Returns the first configured action for a key. Runtime dispatch should prefer
// key_matches_action() when shared keys need context-specific handling.
[[nodiscard]] std::optional<TuiAction> action_for_key(TuiKeyBindings const& bindings, terminal::Key key);
[[nodiscard]] bool key_matches_action(TuiKeyBindings const& bindings, TuiAction action, terminal::Key key);
[[nodiscard]] std::optional<terminal::Key> parse_key_name(std::string_view text);
[[nodiscard]] std::string key_display(terminal::Key key);
[[nodiscard]] std::string action_name(TuiAction action);
// Concise human primary label for help/hotkeys/settings. Canonical ids stay on action_name().
[[nodiscard]] std::string action_label(TuiAction action);
[[nodiscard]] std::optional<TuiAction> key_binding_action_from_name(std::string_view name);
[[nodiscard]] std::string key_binding_config_action_id(TuiAction action);
[[nodiscard]] std::string action_description(TuiAction action);
[[nodiscard]] std::string keys_display(TuiKeyBindings const& bindings, TuiAction action);
[[nodiscard]] std::vector<TuiKeyBindingHelpItem> key_binding_help_items(TuiKeyBindings const& bindings);
[[nodiscard]] std::string default_key_bindings_config_json();
[[nodiscard]] ava::core::Result<TuiKeyBindings> parse_key_bindings_json(std::string_view json);
[[nodiscard]] ava::core::Result<TuiKeyBindings> parse_key_bindings_json(std::string_view json, TuiKeyBindings base);
[[nodiscard]] ava::core::Result<TuiKeyBindings> load_key_bindings(std::filesystem::path const& keybinds_file);

}  // namespace ava::tui

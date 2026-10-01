#include "sys.h"
#include "ava/tui/keybindings.h"
#include "ava/core/error.h"
#include "ava/core/string_utils.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <iterator>
#include <string_view>
#include <utility>

namespace ava::tui {
namespace {

constexpr std::array kActions = {TuiAction::Submit,
                                 TuiAction::NewLine,
                                 TuiAction::Cancel,
                                 TuiAction::ClearInput,
                                 TuiAction::CopySelection,
                                 TuiAction::CopyLatestAssistant,
                                 TuiAction::PromptStash,
                                 TuiAction::ExternalEditor,
                                 TuiAction::Suspend,
                                 TuiAction::ClipboardPasteImage,
                                 TuiAction::DeleteBackward,
                                 TuiAction::DeleteForward,
                                 TuiAction::HistoryPrev,
                                 TuiAction::HistoryNext,
                                 TuiAction::PalettePrev,
                                 TuiAction::PaletteNext,
                                 TuiAction::SelectPrev,
                                 TuiAction::SelectNext,
                                 TuiAction::SelectPageUp,
                                 TuiAction::SelectPageDown,
                                 TuiAction::SelectConfirm,
                                 TuiAction::SelectCancel,
                                 TuiAction::CursorLeft,
                                 TuiAction::CursorRight,
                                 TuiAction::CursorUp,
                                 TuiAction::CursorDown,
                                 TuiAction::CursorLineStart,
                                 TuiAction::CursorLineEnd,
                                 TuiAction::CursorWordLeft,
                                 TuiAction::CursorWordRight,
                                 TuiAction::JumpForward,
                                 TuiAction::JumpBackward,
                                 TuiAction::DeleteWordBackward,
                                 TuiAction::DeleteWordForward,
                                 TuiAction::DeleteToLineStart,
                                 TuiAction::DeleteToLineEnd,
                                 TuiAction::Undo,
                                 TuiAction::Redo,
                                 TuiAction::Yank,
                                 TuiAction::YankPop,
                                 TuiAction::AutocompleteAccept,
                                 TuiAction::PromptAllow,
                                 TuiAction::PromptDeny,
                                 TuiAction::DetailsToggle,
                                 TuiAction::PageUp,
                                 TuiAction::PageDown,
                                 TuiAction::TranscriptHalfPageUp,
                                 TuiAction::TranscriptHalfPageDown,
                                 TuiAction::ModeToggle,
                                 TuiAction::Interrupt,
                                 TuiAction::Exit,
                                 TuiAction::VariantCycle,
                                 TuiAction::ReasoningSelect,
                                 TuiAction::ThinkingToggle,
                                 TuiAction::ModelSelect,
                                 TuiAction::ModelCycleForward,
                                 TuiAction::ModelCycleBackward,
                                 TuiAction::ModelsSave,
                                 TuiAction::ModelsEnableAll,
                                 TuiAction::ModelsClearAll,
                                 TuiAction::ModelsToggleProvider,
                                 TuiAction::ModelsReorderUp,
                                 TuiAction::ModelsReorderDown,
                                 TuiAction::MessageFollowUp,
                                 TuiAction::MessageDequeue,
                                 TuiAction::MessagePrev,
                                 TuiAction::MessageNext,
                                 TuiAction::JumpToBottom,
                                 TuiAction::SessionNew,
                                 TuiAction::SessionTree,
                                 TuiAction::SessionFork,
                                 TuiAction::SessionResume,
                                 TuiAction::SessionTogglePath,
                                 TuiAction::SessionToggleSort,
                                 TuiAction::SessionToggleNamedFilter,
                                 TuiAction::SessionRename,
                                 TuiAction::SessionArchive,
                                 TuiAction::SessionArchiveNoninvasive,
                                 TuiAction::SessionSummarizeParent,
                                 TuiAction::TreeFoldOrUp,
                                 TuiAction::TreeUnfoldOrDown,
                                 TuiAction::TreeEditLabel,
                                 TuiAction::TreeToggleLabelTimestamp,
                                 TuiAction::TreeFilterLabeledOnly,
                                 TuiAction::TreeFilterAll,
                                 TuiAction::OverviewToggle};

struct ActionAlias
{
  std::string_view name;
  TuiAction action;
  int precedence;
};

struct ActionNameResolution
{
  TuiAction action;
  int precedence;
};

struct ParsedKeyBindingEntry
{
  TuiAction action;
  std::vector<std::string> values;
  int precedence = 0;
};

constexpr std::array kActionAliases = {
    ActionAlias{"tui.editor.cursorUp", TuiAction::CursorUp, 2},
    ActionAlias{"cursorUp", TuiAction::CursorUp, 1},
    ActionAlias{"tui.editor.cursorDown", TuiAction::CursorDown, 2},
    ActionAlias{"cursorDown", TuiAction::CursorDown, 1},
    ActionAlias{"tui.editor.cursorLeft", TuiAction::CursorLeft, 2},
    ActionAlias{"cursorLeft", TuiAction::CursorLeft, 1},
    ActionAlias{"tui.editor.cursorRight", TuiAction::CursorRight, 2},
    ActionAlias{"cursorRight", TuiAction::CursorRight, 1},
    ActionAlias{"tui.editor.cursorWordLeft", TuiAction::CursorWordLeft, 2},
    ActionAlias{"cursorWordLeft", TuiAction::CursorWordLeft, 1},
    ActionAlias{"tui.editor.cursorWordRight", TuiAction::CursorWordRight, 2},
    ActionAlias{"cursorWordRight", TuiAction::CursorWordRight, 1},
    ActionAlias{"tui.editor.cursorLineStart", TuiAction::CursorLineStart, 2},
    ActionAlias{"cursorLineStart", TuiAction::CursorLineStart, 1},
    ActionAlias{"tui.editor.cursorLineEnd", TuiAction::CursorLineEnd, 2},
    ActionAlias{"cursorLineEnd", TuiAction::CursorLineEnd, 1},
    ActionAlias{"tui.editor.jumpForward", TuiAction::JumpForward, 2},
    ActionAlias{"jumpForward", TuiAction::JumpForward, 1},
    ActionAlias{"tui.editor.jumpBackward", TuiAction::JumpBackward, 2},
    ActionAlias{"jumpBackward", TuiAction::JumpBackward, 1},
    ActionAlias{"tui.editor.pageUp", TuiAction::PageUp, 2},
    ActionAlias{"pageUp", TuiAction::PageUp, 1},
    ActionAlias{"tui.editor.pageDown", TuiAction::PageDown, 2},
    ActionAlias{"pageDown", TuiAction::PageDown, 1},
    ActionAlias{"tui.editor.deleteCharBackward", TuiAction::DeleteBackward, 2},
    ActionAlias{"deleteCharBackward", TuiAction::DeleteBackward, 1},
    ActionAlias{"tui.editor.deleteCharForward", TuiAction::DeleteForward, 2},
    ActionAlias{"deleteCharForward", TuiAction::DeleteForward, 1},
    ActionAlias{"tui.editor.deleteWordBackward", TuiAction::DeleteWordBackward, 2},
    ActionAlias{"deleteWordBackward", TuiAction::DeleteWordBackward, 1},
    ActionAlias{"tui.editor.deleteWordForward", TuiAction::DeleteWordForward, 2},
    ActionAlias{"deleteWordForward", TuiAction::DeleteWordForward, 1},
    ActionAlias{"tui.editor.deleteToLineStart", TuiAction::DeleteToLineStart, 2},
    ActionAlias{"deleteToLineStart", TuiAction::DeleteToLineStart, 1},
    ActionAlias{"tui.editor.deleteToLineEnd", TuiAction::DeleteToLineEnd, 2},
    ActionAlias{"deleteToLineEnd", TuiAction::DeleteToLineEnd, 1},
    ActionAlias{"tui.editor.yank", TuiAction::Yank, 2},
    ActionAlias{"tui.editor.yankPop", TuiAction::YankPop, 2},
    ActionAlias{"yankPop", TuiAction::YankPop, 1},
    ActionAlias{"tui.editor.undo", TuiAction::Undo, 2},
    ActionAlias{"tui.editor.redo", TuiAction::Redo, 2},
    ActionAlias{"tui.input.newLine", TuiAction::NewLine, 2},
    ActionAlias{"newLine", TuiAction::NewLine, 1},
    ActionAlias{"tui.input.submit", TuiAction::Submit, 2},
    ActionAlias{"tui.input.copy", TuiAction::CopySelection, 2},
    ActionAlias{"copySelection", TuiAction::CopySelection, 1},
    ActionAlias{"app.transcript.copyLatestAssistant", TuiAction::CopyLatestAssistant, 2},
    ActionAlias{"copyLatestAssistant", TuiAction::CopyLatestAssistant, 1},
    ActionAlias{"app.prompt.stash", TuiAction::PromptStash, 2},
    ActionAlias{"promptStash", TuiAction::PromptStash, 1},
    ActionAlias{"app.transcript.halfPageUp", TuiAction::TranscriptHalfPageUp, 2},
    ActionAlias{"transcriptHalfPageUp", TuiAction::TranscriptHalfPageUp, 1},
    ActionAlias{"halfPageUp", TuiAction::TranscriptHalfPageUp, 1},
    ActionAlias{"app.transcript.halfPageDown", TuiAction::TranscriptHalfPageDown, 2},
    ActionAlias{"transcriptHalfPageDown", TuiAction::TranscriptHalfPageDown, 1},
    ActionAlias{"halfPageDown", TuiAction::TranscriptHalfPageDown, 1},
    ActionAlias{"tui.input.tab", TuiAction::AutocompleteAccept, 2},
    ActionAlias{"tui.select.up", TuiAction::SelectPrev, 2},
    ActionAlias{"selectUp", TuiAction::SelectPrev, 1},
    ActionAlias{"tui.select.down", TuiAction::SelectNext, 2},
    ActionAlias{"selectDown", TuiAction::SelectNext, 1},
    ActionAlias{"tui.select.pageUp", TuiAction::SelectPageUp, 2},
    ActionAlias{"selectPageUp", TuiAction::SelectPageUp, 1},
    ActionAlias{"tui.select.pageDown", TuiAction::SelectPageDown, 2},
    ActionAlias{"selectPageDown", TuiAction::SelectPageDown, 1},
    ActionAlias{"tui.select.confirm", TuiAction::SelectConfirm, 2},
    ActionAlias{"selectConfirm", TuiAction::SelectConfirm, 1},
    ActionAlias{"tui.select.cancel", TuiAction::SelectCancel, 2},
    ActionAlias{"selectCancel", TuiAction::SelectCancel, 1},
    ActionAlias{"app.clear", TuiAction::ClearInput, 2},
    ActionAlias{"clear", TuiAction::ClearInput, 1},
    ActionAlias{"app.editor.external", TuiAction::ExternalEditor, 2},
    ActionAlias{"externalEditor", TuiAction::ExternalEditor, 1},
    ActionAlias{"app.suspend", TuiAction::Suspend, 2},
    ActionAlias{"suspend", TuiAction::Suspend, 1},
    ActionAlias{"app.clipboard.pasteImage", TuiAction::ClipboardPasteImage, 2},
    ActionAlias{"pasteImage", TuiAction::ClipboardPasteImage, 1},
    ActionAlias{"app.interrupt", TuiAction::Interrupt, 2},
    ActionAlias{"app.exit", TuiAction::Exit, 2},
    ActionAlias{"app.tools.expand", TuiAction::DetailsToggle, 2},
    ActionAlias{"expandTools", TuiAction::DetailsToggle, 1},
    ActionAlias{"app.model.select", TuiAction::ModelSelect, 2},
    ActionAlias{"modelSelect", TuiAction::ModelSelect, 1},
    ActionAlias{"app.model.cycleForward", TuiAction::ModelCycleForward, 2},
    ActionAlias{"modelCycleForward", TuiAction::ModelCycleForward, 1},
    ActionAlias{"app.model.cycleBackward", TuiAction::ModelCycleBackward, 2},
    ActionAlias{"modelCycleBackward", TuiAction::ModelCycleBackward, 1},
    ActionAlias{"app.models.save", TuiAction::ModelsSave, 2},
    ActionAlias{"modelsSave", TuiAction::ModelsSave, 1},
    ActionAlias{"app.models.enableAll", TuiAction::ModelsEnableAll, 2},
    ActionAlias{"modelsEnableAll", TuiAction::ModelsEnableAll, 1},
    ActionAlias{"app.models.clearAll", TuiAction::ModelsClearAll, 2},
    ActionAlias{"modelsClearAll", TuiAction::ModelsClearAll, 1},
    ActionAlias{"app.models.toggleProvider", TuiAction::ModelsToggleProvider, 2},
    ActionAlias{"modelsToggleProvider", TuiAction::ModelsToggleProvider, 1},
    ActionAlias{"app.models.reorderUp", TuiAction::ModelsReorderUp, 2},
    ActionAlias{"modelsReorderUp", TuiAction::ModelsReorderUp, 1},
    ActionAlias{"app.models.reorderDown", TuiAction::ModelsReorderDown, 2},
    ActionAlias{"modelsReorderDown", TuiAction::ModelsReorderDown, 1},
    ActionAlias{"app.thinking.cycle", TuiAction::VariantCycle, 2},
    ActionAlias{"thinkingCycle", TuiAction::VariantCycle, 1},
    ActionAlias{"app.thinking.select", TuiAction::ReasoningSelect, 2},
    ActionAlias{"thinkingSelect", TuiAction::ReasoningSelect, 1},
    ActionAlias{"app.thinking.toggle", TuiAction::ThinkingToggle, 2},
    ActionAlias{"toggleThinking", TuiAction::ThinkingToggle, 1},
    ActionAlias{"app.message.followUp", TuiAction::MessageFollowUp, 2},
    ActionAlias{"followUp", TuiAction::MessageFollowUp, 1},
    ActionAlias{"app.message.dequeue", TuiAction::MessageDequeue, 2},
    ActionAlias{"messageDequeue", TuiAction::MessageDequeue, 1},
    ActionAlias{"app.session.new", TuiAction::SessionNew, 2},
    ActionAlias{"sessionNew", TuiAction::SessionNew, 1},
    ActionAlias{"app.session.tree", TuiAction::SessionTree, 2},
    ActionAlias{"sessionTree", TuiAction::SessionTree, 1},
    ActionAlias{"app.session.fork", TuiAction::SessionFork, 2},
    ActionAlias{"sessionFork", TuiAction::SessionFork, 1},
    ActionAlias{"app.session.resume", TuiAction::SessionResume, 2},
    ActionAlias{"sessionResume", TuiAction::SessionResume, 1},
    ActionAlias{"app.session.togglePath", TuiAction::SessionTogglePath, 2},
    ActionAlias{"sessionTogglePath", TuiAction::SessionTogglePath, 1},
    ActionAlias{"app.session.toggleSort", TuiAction::SessionToggleSort, 2},
    ActionAlias{"sessionToggleSort", TuiAction::SessionToggleSort, 1},
    ActionAlias{"app.session.toggleNamedFilter", TuiAction::SessionToggleNamedFilter, 2},
    ActionAlias{"sessionToggleNamedFilter", TuiAction::SessionToggleNamedFilter, 1},
    ActionAlias{"app.session.rename", TuiAction::SessionRename, 2},
    ActionAlias{"sessionRename", TuiAction::SessionRename, 1},
    ActionAlias{"app.session.delete", TuiAction::SessionArchive, 2},
    ActionAlias{"sessionDelete", TuiAction::SessionArchive, 1},
    ActionAlias{"app.session.deleteNoninvasive", TuiAction::SessionArchiveNoninvasive, 2},
    ActionAlias{"sessionDeleteNoninvasive", TuiAction::SessionArchiveNoninvasive, 1},
    ActionAlias{"app.sessions.summarizeParent", TuiAction::SessionSummarizeParent, 2},
    ActionAlias{"app.session.summarizeParent", TuiAction::SessionSummarizeParent, 1},
    ActionAlias{"sessionSummarizeParent", TuiAction::SessionSummarizeParent, 1},
    ActionAlias{"app.tree.foldOrUp", TuiAction::TreeFoldOrUp, 2},
    ActionAlias{"treeFoldOrUp", TuiAction::TreeFoldOrUp, 1},
    ActionAlias{"app.tree.unfoldOrDown", TuiAction::TreeUnfoldOrDown, 2},
    ActionAlias{"treeUnfoldOrDown", TuiAction::TreeUnfoldOrDown, 1},
    ActionAlias{"app.tree.editLabel", TuiAction::TreeEditLabel, 2},
    ActionAlias{"treeEditLabel", TuiAction::TreeEditLabel, 1},
    ActionAlias{"app.tree.toggleLabelTimestamp", TuiAction::TreeToggleLabelTimestamp, 2},
    ActionAlias{"treeToggleLabelTimestamp", TuiAction::TreeToggleLabelTimestamp, 1},
    ActionAlias{"app.tree.filter.labeledOnly", TuiAction::TreeFilterLabeledOnly, 2},
    ActionAlias{"treeFilterLabeledOnly", TuiAction::TreeFilterLabeledOnly, 1},
    ActionAlias{"app.tree.filter.all", TuiAction::TreeFilterAll, 2},
    ActionAlias{"treeFilterAll", TuiAction::TreeFilterAll, 1},
    ActionAlias{"app.overview.toggle", TuiAction::OverviewToggle, 2},
    ActionAlias{"overviewToggle", TuiAction::OverviewToggle, 1},
};

std::string normalize_token(std::string_view text)
{
  std::string result;
  result.reserve(text.size());
  for (char const ch : text)
  {
    if (std::isspace(static_cast<unsigned char>(ch)) != 0 || ch == '-' || ch == '_')
      continue;
    result.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
  }
  return result;
}

std::string normalize_action_id(std::string_view text)
{
  std::string result;
  result.reserve(text.size());
  for (char const ch : text)
  {
    if (std::isspace(static_cast<unsigned char>(ch)) != 0 || ch == '-' || ch == '_' || ch == '.')
      continue;
    result.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
  }
  return result;
}

int hex_value(char ch)
{
  if (ch >= '0' && ch <= '9')
    return ch - '0';
  if (ch >= 'a' && ch <= 'f')
    return ch - 'a' + 10;
  if (ch >= 'A' && ch <= 'F')
    return ch - 'A' + 10;
  return -1;
}

std::optional<int> parse_hex_code_unit(std::string_view text, std::size_t hex_start)
{
  if (hex_start + 3 >= text.size())
    return std::nullopt;
  int const a = hex_value(text[hex_start]);
  int const b = hex_value(text[hex_start + 1]);
  int const c = hex_value(text[hex_start + 2]);
  int const d = hex_value(text[hex_start + 3]);
  if (a < 0 || b < 0 || c < 0 || d < 0)
    return std::nullopt;
  return (a << 12) | (b << 8) | (c << 4) | d;
}

void append_utf8(std::string& out, int codepoint)
{
  if (codepoint <= 0x7F)
  {
    out.push_back(static_cast<char>(codepoint));
  }
  else if (codepoint <= 0x7FF)
  {
    out.push_back(static_cast<char>(0xC0 | ((codepoint >> 6) & 0x1F)));
    out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
  }
  else if (codepoint <= 0xFFFF)
  {
    out.push_back(static_cast<char>(0xE0 | ((codepoint >> 12) & 0x0F)));
    out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
  }
  else
  {
    out.push_back(static_cast<char>(0xF0 | ((codepoint >> 18) & 0x07)));
    out.push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
  }
}

std::vector<std::string> split_comma_list(std::string_view text)
{
  std::vector<std::string> parts;
  std::size_t start = 0;
  while (start <= text.size())
  {
    auto const comma = text.find(',', start);
    auto const end = comma == std::string_view::npos ? text.size() : comma;
    auto part = core::trim(text.substr(start, end - start));
    if (!part.empty())
      parts.push_back(std::move(part));
    if (comma == std::string_view::npos)
      break;
    start = comma + 1;
  }
  return parts;
}

ava::core::Result<std::vector<terminal::Key>> parse_key_list(std::string_view text)
{
  std::vector<terminal::Key> keys;
  for (auto const& part : split_comma_list(text))
  {
    auto const key = parse_key_name(part);
    if (!key)
    {
      auto error = ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "unknown TUI key binding");
      error.with_context("key", part);
      return std::unexpected(std::move(error));
    }
    keys.push_back(*key);
  }
  if (keys.empty())
  {
    auto error = ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "empty TUI key binding");
    error.with_context("value", std::string(text));
    return std::unexpected(std::move(error));
  }
  return keys;
}

ava::core::Result<std::vector<terminal::Key>> parse_key_values(std::vector<std::string> const& values)
{
  std::vector<terminal::Key> keys;
  for (auto const& value : values)
  {
    auto parsed = parse_key_list(value);
    if (!parsed)
      return std::unexpected(std::move(parsed.error()));
    keys.insert(keys.end(), parsed->begin(), parsed->end());
  }
  if (keys.empty())
  {
    auto error = ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "empty TUI key binding");
    return std::unexpected(std::move(error));
  }
  return keys;
}

ava::core::Error keybinds_error(std::string message)
{
  return ava::core::Error(ava::core::ErrorCategory::InvalidArgument, std::move(message));
}

void skip_json_whitespace(std::string_view json, std::size_t& offset)
{
  while (offset < json.size() && std::isspace(static_cast<unsigned char>(json[offset])) != 0) ++offset;
}

ava::core::Result<std::string> parse_json_string(std::string_view json, std::size_t& offset, std::string_view expected_message)
{
  if (offset >= json.size() || json[offset] != '"')
  {
    return std::unexpected(keybinds_error(std::string(expected_message)));
  }
  ++offset;
  std::string key;
  while (offset < json.size())
  {
    auto const ch = json[offset++];
    if (ch == '"')
      return key;
    if (static_cast<unsigned char>(ch) < 0x20)
    {
      return std::unexpected(keybinds_error("invalid control character in JSON object key"));
    }
    if (ch != '\\')
    {
      key.push_back(ch);
      continue;
    }
    if (offset >= json.size())
      return std::unexpected(keybinds_error("unterminated JSON escape"));
    auto const escape = json[offset++];
    if (escape == 'u')
    {
      auto const code_unit = parse_hex_code_unit(json, offset);
      if (!code_unit)
        return std::unexpected(keybinds_error("invalid JSON unicode escape"));
      append_utf8(key, *code_unit);
      offset += 4;
      continue;
    }
    switch (escape)
    {
      case '"':
        key.push_back('"');
        break;
      case '\\':
        key.push_back('\\');
        break;
      case '/':
        key.push_back('/');
        break;
      case 'n':
        key.push_back('\n');
        break;
      case 'r':
        key.push_back('\r');
        break;
      case 't':
        key.push_back('\t');
        break;
      case 'b':
        key.push_back('\b');
        break;
      case 'f':
        key.push_back('\f');
        break;
      default:
        return std::unexpected(keybinds_error("invalid JSON object key escape"));
    }
  }
  return std::unexpected(keybinds_error("unterminated JSON object key"));
}

ava::core::Result<std::string> parse_json_key(std::string_view json, std::size_t& offset)
{
  return parse_json_string(json, offset, "expected JSON object key");
}

std::optional<ActionNameResolution> action_from_name(std::string_view name)
{
  auto const normalized = normalize_action_id(name);
  for (auto const action : kActions)
  {
    if (normalize_action_id(action_name(action)) == normalized)
      return ActionNameResolution{.action = action, .precedence = 2};
  }
  for (auto const alias : kActionAliases)
  {
    if (normalize_action_id(alias.name) == normalized)
      return ActionNameResolution{.action = alias.action, .precedence = alias.precedence};
  }
  return std::nullopt;
}

ava::core::Result<std::vector<std::string>> parse_key_binding_value(std::string_view json, std::size_t& offset, std::string_view action)
{
  if (offset >= json.size())
  {
    auto error = keybinds_error("missing TUI keybinding value");
    error.with_context("action", std::string(action));
    return std::unexpected(std::move(error));
  }

  if (json[offset] == '"')
  {
    auto value = parse_json_string(json, offset, "TUI keybinding values must be strings or arrays of strings");
    if (!value)
      return std::unexpected(std::move(value.error()));
    return std::vector<std::string>{std::move(*value)};
  }

  if (json[offset] != '[')
  {
    auto error = keybinds_error("TUI keybinding values must be strings or arrays of strings");
    error.with_context("action", std::string(action));
    return std::unexpected(std::move(error));
  }

  ++offset;
  std::vector<std::string> values;
  skip_json_whitespace(json, offset);
  if (offset < json.size() && json[offset] == ']')
  {
    ++offset;
    auto error = keybinds_error("empty TUI key binding");
    error.with_context("action", std::string(action));
    return std::unexpected(std::move(error));
  }

  while (true)
  {
    skip_json_whitespace(json, offset);
    auto value = parse_json_string(json, offset, "TUI keybinding array entries must be strings");
    if (!value)
    {
      value.error().with_context("action", std::string(action));
      return std::unexpected(std::move(value.error()));
    }
    if (core::trim(*value).empty())
    {
      auto error = keybinds_error("empty TUI key binding");
      error.with_context("action", std::string(action));
      return std::unexpected(std::move(error));
    }
    values.push_back(std::move(*value));
    skip_json_whitespace(json, offset);
    if (offset >= json.size())
      return std::unexpected(keybinds_error("unterminated keybinding array"));
    if (json[offset] == ',')
    {
      ++offset;
      continue;
    }
    if (json[offset] == ']')
    {
      ++offset;
      return values;
    }
    return std::unexpected(keybinds_error("expected ',' or ']' in keybinding array"));
  }
}

ava::core::Result<std::vector<ParsedKeyBindingEntry>> parse_key_binding_entries(std::string_view json)
{
  std::vector<ParsedKeyBindingEntry> entries;
  std::size_t offset = 0;
  skip_json_whitespace(json, offset);
  if (offset >= json.size() || json[offset] != '{')
  {
    return std::unexpected(keybinds_error("keybinds.json must be a JSON object"));
  }
  ++offset;
  skip_json_whitespace(json, offset);
  if (offset < json.size() && json[offset] == '}')
  {
    ++offset;
    skip_json_whitespace(json, offset);
    if (offset != json.size())
      return std::unexpected(keybinds_error("unexpected content after keybinds object"));
    return entries;
  }

  while (true)
  {
    skip_json_whitespace(json, offset);
    auto key = parse_json_key(json, offset);
    if (!key)
      return std::unexpected(std::move(key.error()));
    auto const action = action_from_name(*key);
    if (!action)
    {
      auto error = keybinds_error("unknown TUI keybinding action");
      error.with_context("action", *key);
      return std::unexpected(std::move(error));
    }
    skip_json_whitespace(json, offset);
    if (offset >= json.size() || json[offset] != ':')
    {
      return std::unexpected(keybinds_error("expected ':' after keybinds action"));
    }
    ++offset;
    skip_json_whitespace(json, offset);
    auto value = parse_key_binding_value(json, offset, *key);
    if (!value)
      return std::unexpected(std::move(value.error()));
    auto existing = std::ranges::find_if(entries, [&](auto const& entry) { return entry.action == action->action; });
    if (existing == entries.end())
    {
      entries.push_back(ParsedKeyBindingEntry{.action = action->action, .values = std::move(*value), .precedence = action->precedence});
    }
    else if (action->precedence >= existing->precedence)
    {
      existing->values = std::move(*value);
      existing->precedence = action->precedence;
    }
    skip_json_whitespace(json, offset);
    if (offset >= json.size())
      return std::unexpected(keybinds_error("unterminated keybinds object"));
    if (json[offset] == ',')
    {
      ++offset;
      continue;
    }
    if (json[offset] == '}')
    {
      ++offset;
      skip_json_whitespace(json, offset);
      if (offset != json.size())
        return std::unexpected(keybinds_error("unexpected content after keybinds object"));
      return entries;
    }
    return std::unexpected(keybinds_error("expected ',' or '}' in keybinds object"));
  }
}

std::optional<std::vector<terminal::Key>*> keys_for_action(TuiKeyBindings& bindings, TuiAction action)
{
  for (auto& [candidate, keys] : bindings.bindings)
  {
    if (candidate == action)
      return &keys;
  }
  return std::nullopt;
}

bool is_select_action(TuiAction action)
{
  switch (action)
  {
    case TuiAction::SelectPrev:
    case TuiAction::SelectNext:
    case TuiAction::SelectPageUp:
    case TuiAction::SelectPageDown:
    case TuiAction::SelectConfirm:
    case TuiAction::SelectCancel:
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
    case TuiAction::ModelsSave:
    case TuiAction::ModelsEnableAll:
    case TuiAction::ModelsClearAll:
    case TuiAction::ModelsToggleProvider:
    case TuiAction::ModelsReorderUp:
    case TuiAction::ModelsReorderDown:
      return true;
    case TuiAction::Submit:
    case TuiAction::NewLine:
    case TuiAction::Cancel:
    case TuiAction::ClearInput:
    case TuiAction::CopySelection:
    case TuiAction::CopyLatestAssistant:
    case TuiAction::PromptStash:
    case TuiAction::ExternalEditor:
    case TuiAction::Suspend:
    case TuiAction::ClipboardPasteImage:
    case TuiAction::DeleteBackward:
    case TuiAction::DeleteForward:
    case TuiAction::HistoryPrev:
    case TuiAction::HistoryNext:
    case TuiAction::PalettePrev:
    case TuiAction::PaletteNext:
    case TuiAction::CursorLeft:
    case TuiAction::CursorRight:
    case TuiAction::CursorUp:
    case TuiAction::CursorDown:
    case TuiAction::CursorLineStart:
    case TuiAction::CursorLineEnd:
    case TuiAction::CursorWordLeft:
    case TuiAction::CursorWordRight:
    case TuiAction::JumpForward:
    case TuiAction::JumpBackward:
    case TuiAction::DeleteWordBackward:
    case TuiAction::DeleteWordForward:
    case TuiAction::DeleteToLineStart:
    case TuiAction::DeleteToLineEnd:
    case TuiAction::Undo:
    case TuiAction::Redo:
    case TuiAction::Yank:
    case TuiAction::YankPop:
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
    case TuiAction::MessageFollowUp:
    case TuiAction::MessageDequeue:
    case TuiAction::MessagePrev:
    case TuiAction::MessageNext:
    case TuiAction::JumpToBottom:
    case TuiAction::SessionNew:
    case TuiAction::SessionTree:
    case TuiAction::SessionFork:
    case TuiAction::SessionResume:
    case TuiAction::OverviewToggle:
      return false;
  }
  return false;
}

bool actions_can_share_key(TuiAction lhs, TuiAction rhs)
{
  if (lhs == rhs || is_select_action(lhs) != is_select_action(rhs))
    return true;
  auto const is_ctrl_c_semantic = [](TuiAction action) {
    return action == TuiAction::ClearInput || action == TuiAction::CopySelection || action == TuiAction::Interrupt;
  };
  return is_ctrl_c_semantic(lhs) && is_ctrl_c_semantic(rhs);
}

void remove_keys_from_other_actions(TuiKeyBindings& bindings, TuiAction action, std::vector<terminal::Key> const& keys_to_remove)
{
  for (auto& [candidate, keys] : bindings.bindings)
  {
    if (actions_can_share_key(action, candidate))
      continue;
    std::erase_if(keys, [&](terminal::Key key) { return std::ranges::find(keys_to_remove, key) != keys_to_remove.end(); });
  }
}

ava::core::VoidResult validate_user_key_conflicts(std::vector<std::pair<TuiAction, std::vector<terminal::Key>>> const& overrides)
{
  std::vector<std::pair<terminal::Key, TuiAction>> claims;
  for (auto const& [action, keys] : overrides)
  {
    for (auto const key : keys)
    {
      auto const claimed = std::ranges::find_if(claims, [key](auto const& claim) { return claim.first == key; });
      if (claimed == claims.end())
      {
        claims.push_back({key, action});
        continue;
      }
      if (claimed->second == action)
        continue;
      if (actions_can_share_key(claimed->second, action))
        continue;

      auto error = keybinds_error("conflicting TUI keybinding");
      error.with_context("key", key_display(key))
          .with_context("action", action_name(action))
          .with_context("conflicts_with", action_name(claimed->second))
          .with_context("fix", "assign each configured key to only one action");
      return std::unexpected(std::move(error));
    }
  }
  return {};
}

std::string_view config_action_id(TuiAction action)
{
  switch (action)
  {
    case TuiAction::Submit:
      return "tui.input.submit";
    case TuiAction::NewLine:
      return "tui.input.newLine";
    case TuiAction::Cancel:
      return "cancel";
    case TuiAction::ClearInput:
      return "app.clear";
    case TuiAction::CopySelection:
      return "tui.input.copy";
    case TuiAction::CopyLatestAssistant:
      return "app.transcript.copyLatestAssistant";
    case TuiAction::PromptStash:
      return "app.prompt.stash";
    case TuiAction::ExternalEditor:
      return "app.editor.external";
    case TuiAction::Suspend:
      return "app.suspend";
    case TuiAction::ClipboardPasteImage:
      return "app.clipboard.pasteImage";
    case TuiAction::DeleteBackward:
      return "tui.editor.deleteCharBackward";
    case TuiAction::DeleteForward:
      return "tui.editor.deleteCharForward";
    case TuiAction::HistoryPrev:
      return "history_prev";
    case TuiAction::HistoryNext:
      return "history_next";
    case TuiAction::PalettePrev:
      return "palette_prev";
    case TuiAction::PaletteNext:
      return "palette_next";
    case TuiAction::SelectPrev:
      return "tui.select.up";
    case TuiAction::SelectNext:
      return "tui.select.down";
    case TuiAction::SelectPageUp:
      return "tui.select.pageUp";
    case TuiAction::SelectPageDown:
      return "tui.select.pageDown";
    case TuiAction::SelectConfirm:
      return "tui.select.confirm";
    case TuiAction::SelectCancel:
      return "tui.select.cancel";
    case TuiAction::CursorLeft:
      return "tui.editor.cursorLeft";
    case TuiAction::CursorRight:
      return "tui.editor.cursorRight";
    case TuiAction::CursorUp:
      return "tui.editor.cursorUp";
    case TuiAction::CursorDown:
      return "tui.editor.cursorDown";
    case TuiAction::CursorLineStart:
      return "tui.editor.cursorLineStart";
    case TuiAction::CursorLineEnd:
      return "tui.editor.cursorLineEnd";
    case TuiAction::CursorWordLeft:
      return "tui.editor.cursorWordLeft";
    case TuiAction::CursorWordRight:
      return "tui.editor.cursorWordRight";
    case TuiAction::JumpForward:
      return "tui.editor.jumpForward";
    case TuiAction::JumpBackward:
      return "tui.editor.jumpBackward";
    case TuiAction::DeleteWordBackward:
      return "tui.editor.deleteWordBackward";
    case TuiAction::DeleteWordForward:
      return "tui.editor.deleteWordForward";
    case TuiAction::DeleteToLineStart:
      return "tui.editor.deleteToLineStart";
    case TuiAction::DeleteToLineEnd:
      return "tui.editor.deleteToLineEnd";
    case TuiAction::Undo:
      return "tui.editor.undo";
    case TuiAction::Redo:
      return "tui.editor.redo";
    case TuiAction::Yank:
      return "tui.editor.yank";
    case TuiAction::YankPop:
      return "tui.editor.yankPop";
    case TuiAction::AutocompleteAccept:
      return "tui.input.tab";
    case TuiAction::PromptAllow:
      return "prompt_allow";
    case TuiAction::PromptDeny:
      return "prompt_deny";
    case TuiAction::DetailsToggle:
      return "app.tools.expand";
    case TuiAction::PageUp:
      return "tui.editor.pageUp";
    case TuiAction::PageDown:
      return "tui.editor.pageDown";
    case TuiAction::TranscriptHalfPageUp:
      return "app.transcript.halfPageUp";
    case TuiAction::TranscriptHalfPageDown:
      return "app.transcript.halfPageDown";
    case TuiAction::ModeToggle:
      return "mode_toggle";
    case TuiAction::Interrupt:
      return "app.interrupt";
    case TuiAction::Exit:
      return "app.exit";
    case TuiAction::VariantCycle:
      return "app.thinking.cycle";
    case TuiAction::ReasoningSelect:
      return "app.thinking.select";
    case TuiAction::ThinkingToggle:
      return "app.thinking.toggle";
    case TuiAction::ModelSelect:
      return "app.model.select";
    case TuiAction::ModelCycleForward:
      return "app.model.cycleForward";
    case TuiAction::ModelCycleBackward:
      return "app.model.cycleBackward";
    case TuiAction::ModelsSave:
      return "app.models.save";
    case TuiAction::ModelsEnableAll:
      return "app.models.enableAll";
    case TuiAction::ModelsClearAll:
      return "app.models.clearAll";
    case TuiAction::ModelsToggleProvider:
      return "app.models.toggleProvider";
    case TuiAction::ModelsReorderUp:
      return "app.models.reorderUp";
    case TuiAction::ModelsReorderDown:
      return "app.models.reorderDown";
    case TuiAction::MessageFollowUp:
      return "app.message.followUp";
    case TuiAction::MessageDequeue:
      return "app.message.dequeue";
    case TuiAction::MessagePrev:
      return "message_prev";
    case TuiAction::MessageNext:
      return "message_next";
    case TuiAction::JumpToBottom:
      return "jump_to_bottom";
    case TuiAction::SessionNew:
      return "app.session.new";
    case TuiAction::SessionTree:
      return "app.session.tree";
    case TuiAction::SessionFork:
      return "app.session.fork";
    case TuiAction::SessionResume:
      return "app.session.resume";
    case TuiAction::SessionTogglePath:
      return "app.session.togglePath";
    case TuiAction::SessionToggleSort:
      return "app.session.toggleSort";
    case TuiAction::SessionToggleNamedFilter:
      return "app.session.toggleNamedFilter";
    case TuiAction::SessionRename:
      return "app.session.rename";
    case TuiAction::SessionArchive:
      return "app.session.delete";
    case TuiAction::SessionArchiveNoninvasive:
      return "app.session.deleteNoninvasive";
    case TuiAction::SessionSummarizeParent:
      return "app.sessions.summarizeParent";
    case TuiAction::TreeFoldOrUp:
      return "app.tree.foldOrUp";
    case TuiAction::TreeUnfoldOrDown:
      return "app.tree.unfoldOrDown";
    case TuiAction::TreeEditLabel:
      return "app.tree.editLabel";
    case TuiAction::TreeToggleLabelTimestamp:
      return "app.tree.toggleLabelTimestamp";
    case TuiAction::TreeFilterLabeledOnly:
      return "app.tree.filter.labeledOnly";
    case TuiAction::TreeFilterAll:
      return "app.tree.filter.all";
    case TuiAction::OverviewToggle:
      return "app.overview.toggle";
  }
  return "unknown";
}

void append_json_string(std::string& output, std::string_view text)
{
  output.push_back('"');
  for (char const ch : text)
  {
    switch (ch)
    {
      case '"':
        output += "\\\"";
        break;
      case '\\':
        output += "\\\\";
        break;
      case '\n':
        output += "\\n";
        break;
      case '\r':
        output += "\\r";
        break;
      case '\t':
        output += "\\t";
        break;
      default:
        output.push_back(ch);
        break;
    }
  }
  output.push_back('"');
}

bool has_same_context_default_key_conflict(TuiKeyBindings const& bindings, TuiAction action, std::vector<terminal::Key> const& keys)
{
  auto const session_precedence_over_models = [](TuiAction current, TuiAction candidate) {
    return (current == TuiAction::SessionTogglePath && candidate == TuiAction::ModelsToggleProvider) ||
           (current == TuiAction::SessionToggleSort && candidate == TuiAction::ModelsSave);
  };
  for (auto const key : keys)
  {
    for (auto const& [candidate, candidate_keys] : bindings.bindings)
    {
      if (candidate == action || actions_can_share_key(action, candidate) || session_precedence_over_models(action, candidate))
        continue;
      if (std::ranges::find(candidate_keys, key) != candidate_keys.end())
        return true;
    }
  }
  return false;
}

}  // namespace

TuiKeyBindings default_key_bindings()
{
  using Key = terminal::Key;
  return TuiKeyBindings{.bindings = {{TuiAction::Submit, {Key::Enter}},
                                     {TuiAction::NewLine, {Key::ShiftEnter, Key::CtrlEnter}},
                                     {TuiAction::Cancel, {Key::Escape}},
                                     {TuiAction::ClearInput, {Key::CtrlC}},
                                     {TuiAction::CopySelection, {Key::CtrlC}},
                                     {TuiAction::CopyLatestAssistant, {Key::F5}},
                                     {TuiAction::PromptStash, {Key::F6}},
                                     {TuiAction::ExternalEditor, {Key::CtrlG}},
                                     {TuiAction::Suspend, {Key::CtrlZ}},
                                     {TuiAction::ClipboardPasteImage, {Key::CtrlV}},
                                     {TuiAction::DeleteBackward, {Key::Backspace, Key::ShiftBackspace, Key::CtrlH}},
                                     {TuiAction::DeleteForward, {Key::Delete, Key::ShiftDelete, Key::CtrlD}},
                                     {TuiAction::HistoryPrev, {}},
                                     {TuiAction::HistoryNext, {}},
                                     {TuiAction::PalettePrev, {Key::ArrowUp}},
                                     {TuiAction::PaletteNext, {Key::ArrowDown}},
                                     {TuiAction::SelectPrev, {Key::ArrowUp}},
                                     {TuiAction::SelectNext, {Key::ArrowDown}},
                                     {TuiAction::SelectPageUp, {Key::PageUp}},
                                     {TuiAction::SelectPageDown, {Key::PageDown}},
                                     {TuiAction::SelectConfirm, {Key::Enter}},
                                     {TuiAction::SelectCancel, {Key::Escape, Key::CtrlC}},
                                     {TuiAction::CursorLeft, {Key::ArrowLeft, Key::CtrlB}},
                                     {TuiAction::CursorRight, {Key::ArrowRight, Key::CtrlF}},
                                     {TuiAction::CursorUp, {}},
                                     {TuiAction::CursorDown, {}},
                                     {TuiAction::CursorLineStart, {Key::Home, Key::CtrlA}},
                                     {TuiAction::CursorLineEnd, {Key::End, Key::CtrlE}},
                                     {TuiAction::CursorWordLeft, {Key::CtrlArrowLeft, Key::AltArrowLeft, Key::AltB}},
                                     {TuiAction::CursorWordRight, {Key::CtrlArrowRight, Key::AltArrowRight, Key::AltF}},
                                     {TuiAction::JumpForward, {Key::CtrlRightBracket}},
                                     {TuiAction::JumpBackward, {Key::CtrlAltRightBracket}},
                                     {TuiAction::DeleteWordBackward, {Key::CtrlW, Key::AltBackspace}},
                                     {TuiAction::DeleteWordForward, {Key::AltD, Key::AltDelete}},
                                     {TuiAction::DeleteToLineStart, {Key::CtrlU}},
                                     {TuiAction::DeleteToLineEnd, {Key::CtrlK}},
                                     {TuiAction::Undo, {Key::CtrlMinus}},
                                     {TuiAction::Redo, {Key::CtrlR}},
                                     {TuiAction::Yank, {Key::CtrlY}},
                                     {TuiAction::YankPop, {Key::AltY}},
                                     {TuiAction::AutocompleteAccept, {Key::Tab}},
                                     {TuiAction::PromptAllow, {}},
                                     {TuiAction::PromptDeny, {}},
                                     {TuiAction::DetailsToggle, {Key::CtrlO}},
                                     {TuiAction::PageUp, {Key::PageUp}},
                                     {TuiAction::PageDown, {Key::PageDown}},
                                     {TuiAction::TranscriptHalfPageUp, {}},
                                     {TuiAction::TranscriptHalfPageDown, {}},
                                     {TuiAction::ModeToggle, {Key::Tab}},
                                     {TuiAction::Interrupt, {Key::CtrlC}},
                                     {TuiAction::Exit, {Key::CtrlD}},
                                     {TuiAction::VariantCycle, {Key::ShiftTab}},
                                     {TuiAction::ReasoningSelect, {Key::CtrlT}},
                                     {TuiAction::ThinkingToggle, {}},
                                     {TuiAction::ModelSelect, {Key::CtrlL}},
                                     {TuiAction::ModelCycleForward, {Key::CtrlP}},
                                     {TuiAction::ModelCycleBackward, {Key::CtrlShiftP}},
                                     {TuiAction::MessageFollowUp, {Key::AltEnter}},
                                     {TuiAction::MessageDequeue, {Key::AltArrowUp}},
                                     {TuiAction::MessagePrev, {Key::AltK}},
                                     {TuiAction::MessageNext, {Key::AltJ}},
                                     {TuiAction::JumpToBottom, {Key::CtrlEnd}},
                                     {TuiAction::SessionNew, {}},
                                     {TuiAction::SessionTree, {}},
                                     {TuiAction::SessionFork, {}},
                                     {TuiAction::SessionResume, {}},
                                     {TuiAction::SessionTogglePath, {Key::CtrlP}},
                                     {TuiAction::SessionToggleSort, {Key::CtrlS, Key::CtrlT}},
                                     {TuiAction::SessionToggleNamedFilter, {Key::CtrlN}},
                                     {TuiAction::SessionRename, {Key::CtrlR}},
                                     {TuiAction::SessionArchive, {Key::CtrlD}},
                                     {TuiAction::SessionArchiveNoninvasive, {Key::CtrlBackspace}},
                                     {TuiAction::SessionSummarizeParent, {}},
                                     {TuiAction::TreeFoldOrUp, {Key::CtrlArrowLeft, Key::AltArrowLeft}},
                                     {TuiAction::TreeUnfoldOrDown, {Key::CtrlArrowRight, Key::AltArrowRight}},
                                     {TuiAction::TreeEditLabel, {Key::ShiftL}},
                                     {TuiAction::TreeToggleLabelTimestamp, {Key::ShiftT}},
                                     {TuiAction::TreeFilterLabeledOnly, {}},
                                     {TuiAction::TreeFilterAll, {}},
                                     {TuiAction::OverviewToggle, {}},
                                     {TuiAction::ModelsSave, {Key::CtrlS}},
                                     {TuiAction::ModelsEnableAll, {Key::CtrlA}},
                                     {TuiAction::ModelsClearAll, {Key::CtrlX}},
                                     {TuiAction::ModelsToggleProvider, {Key::CtrlP}},
                                     {TuiAction::ModelsReorderUp, {Key::AltArrowUp}},
                                     {TuiAction::ModelsReorderDown, {Key::AltArrowDown}}}};
}

std::optional<TuiAction> action_for_key(TuiKeyBindings const& bindings, terminal::Key key)
{
  for (auto const& [action, keys] : bindings.bindings)
  {
    if (std::ranges::find(keys, key) != keys.end())
      return action;
  }
  return std::nullopt;
}

bool key_matches_action(TuiKeyBindings const& bindings, TuiAction action, terminal::Key key)
{
  for (auto const& [candidate, keys] : bindings.bindings)
  {
    if (candidate == action && std::ranges::find(keys, key) != keys.end())
      return true;
  }
  return false;
}

std::optional<terminal::Key> parse_key_name(std::string_view text)
{
  using Key = terminal::Key;
  auto const normalized = normalize_token(text);
  if (normalized == "enter" || normalized == "return")
    return Key::Enter;
  if (normalized == "shift+enter" || normalized == "ctrl+j" || normalized == "ctrlj")
    return Key::ShiftEnter;
  if (normalized == "ctrl+enter" || normalized == "ctrlenter")
    return Key::CtrlEnter;
  if (normalized == "alt+enter" || normalized == "altenter" || normalized == "meta+enter" || normalized == "metaenter")
    return Key::AltEnter;
  if (normalized == "backspace" || normalized == "bs")
    return Key::Backspace;
  if (normalized == "shift+backspace" || normalized == "shiftbackspace" || normalized == "shift+bs" || normalized == "shiftbs")
    return Key::ShiftBackspace;
  if (normalized == "ctrl+backspace" || normalized == "ctrlbackspace" || normalized == "ctrl+bs" || normalized == "ctrlbs" ||
      normalized == "control+backspace" || normalized == "controlbackspace")
    return Key::CtrlBackspace;
  if (normalized == "delete" || normalized == "del")
    return Key::Delete;
  if (normalized == "shift+delete" || normalized == "shiftdelete" || normalized == "shift+del" || normalized == "shiftdel")
    return Key::ShiftDelete;
  if (normalized == "insert" || normalized == "ins")
    return Key::Insert;
  if (normalized == "clear")
    return Key::Clear;
  if (normalized == "tab")
    return Key::Tab;
  if (normalized == "space")
    return Key::Space;
  if (normalized == "ctrl+space" || normalized == "ctrlspace" || normalized == "control+space" || normalized == "controlspace")
    return Key::CtrlSpace;
  if (normalized == "ctrl+0" || normalized == "ctrl0" || normalized == "control+0" || normalized == "control0")
    return Key::Ctrl0;
  if (normalized == "ctrl+1" || normalized == "ctrl1" || normalized == "control+1" || normalized == "control1")
    return Key::Ctrl1;
  if (normalized == "ctrl+2" || normalized == "ctrl2" || normalized == "control+2" || normalized == "control2")
    return Key::Ctrl2;
  if (normalized == "ctrl+3" || normalized == "ctrl3" || normalized == "control+3" || normalized == "control3")
    return Key::Ctrl3;
  if (normalized == "ctrl+4" || normalized == "ctrl4" || normalized == "control+4" || normalized == "control4")
    return Key::Ctrl4;
  if (normalized == "ctrl+5" || normalized == "ctrl5" || normalized == "control+5" || normalized == "control5")
    return Key::Ctrl5;
  if (normalized == "ctrl+6" || normalized == "ctrl6" || normalized == "control+6" || normalized == "control6")
    return Key::Ctrl6;
  if (normalized == "ctrl+7" || normalized == "ctrl7" || normalized == "control+7" || normalized == "control7")
    return Key::Ctrl7;
  if (normalized == "ctrl+8" || normalized == "ctrl8" || normalized == "control+8" || normalized == "control8")
    return Key::Ctrl8;
  if (normalized == "ctrl+9" || normalized == "ctrl9" || normalized == "control+9" || normalized == "control9")
    return Key::Ctrl9;
  if (normalized == "shift+tab" || normalized == "shifttab" || normalized == "backtab")
    return Key::ShiftTab;
  if (normalized == "shift+l" || normalized == "shiftl")
    return Key::ShiftL;
  if (normalized == "shift+t" || normalized == "shiftt")
    return Key::ShiftT;
  if (normalized == "esc" || normalized == "escape")
    return Key::Escape;
  if (normalized == "arrowup" || normalized == "up")
    return Key::ArrowUp;
  if (normalized == "arrowdown" || normalized == "down")
    return Key::ArrowDown;
  if (normalized == "arrowleft" || normalized == "left")
    return Key::ArrowLeft;
  if (normalized == "arrowright" || normalized == "right")
    return Key::ArrowRight;
  if (normalized == "shift+arrowup" || normalized == "shiftarrowup" || normalized == "shift+up" || normalized == "shiftup")
    return Key::ShiftArrowUp;
  if (normalized == "shift+arrowdown" || normalized == "shiftarrowdown" || normalized == "shift+down" || normalized == "shiftdown")
    return Key::ShiftArrowDown;
  if (normalized == "shift+arrowleft" || normalized == "shiftarrowleft" || normalized == "shift+left" || normalized == "shiftleft")
    return Key::ShiftArrowLeft;
  if (normalized == "shift+arrowright" || normalized == "shiftarrowright" || normalized == "shift+right" || normalized == "shiftright")
    return Key::ShiftArrowRight;
  if (normalized == "shift+ctrl+arrowleft" || normalized == "ctrl+shift+arrowleft" || normalized == "shiftctrlarrowleft" ||
      normalized == "ctrlshiftarrowleft" || normalized == "shift+ctrl+left" || normalized == "ctrl+shift+left" || normalized == "shiftctrlleft" ||
      normalized == "ctrlshiftleft")
    return Key::ShiftCtrlArrowLeft;
  if (normalized == "shift+ctrl+arrowright" || normalized == "ctrl+shift+arrowright" || normalized == "shiftctrlarrowright" ||
      normalized == "ctrlshiftarrowright" || normalized == "shift+ctrl+right" || normalized == "ctrl+shift+right" || normalized == "shiftctrlright" ||
      normalized == "ctrlshiftright")
    return Key::ShiftCtrlArrowRight;
  if (normalized == "shift+alt+arrowleft" || normalized == "alt+shift+arrowleft" || normalized == "shiftaltarrowleft" || normalized == "altshiftarrowleft" ||
      normalized == "shift+alt+left" || normalized == "alt+shift+left" || normalized == "shiftaltleft" || normalized == "altshiftleft" ||
      normalized == "shift+meta+arrowleft" || normalized == "meta+shift+arrowleft" || normalized == "shiftmetaarrowleft" ||
      normalized == "metashiftarrowleft" || normalized == "shift+meta+left" || normalized == "meta+shift+left" || normalized == "shiftmetaleft" ||
      normalized == "metashiftleft")
    return Key::ShiftAltArrowLeft;
  if (normalized == "shift+alt+arrowright" || normalized == "alt+shift+arrowright" || normalized == "shiftaltarrowright" ||
      normalized == "altshiftarrowright" || normalized == "shift+alt+right" || normalized == "alt+shift+right" || normalized == "shiftaltright" ||
      normalized == "altshiftright" || normalized == "shift+meta+arrowright" || normalized == "meta+shift+arrowright" || normalized == "shiftmetaarrowright" ||
      normalized == "metashiftarrowright" || normalized == "shift+meta+right" || normalized == "meta+shift+right" || normalized == "shiftmetaright" ||
      normalized == "metashiftright")
    return Key::ShiftAltArrowRight;
  if (normalized == "ctrl+arrowleft" || normalized == "ctrlarrowleft" || normalized == "ctrl+left" || normalized == "ctrlleft")
    return Key::CtrlArrowLeft;
  if (normalized == "ctrl+arrowright" || normalized == "ctrlarrowright" || normalized == "ctrl+right" || normalized == "ctrlright")
    return Key::CtrlArrowRight;
  if (normalized == "alt+arrowleft" || normalized == "altarrowleft" || normalized == "alt+left" || normalized == "altleft" || normalized == "meta+arrowleft" ||
      normalized == "metaarrowleft" || normalized == "meta+left" || normalized == "metaleft")
    return Key::AltArrowLeft;
  if (normalized == "alt+arrowright" || normalized == "altarrowright" || normalized == "alt+right" || normalized == "altright" ||
      normalized == "meta+arrowright" || normalized == "metaarrowright" || normalized == "meta+right" || normalized == "metaright")
    return Key::AltArrowRight;
  if (normalized == "alt+arrowup" || normalized == "altarrowup" || normalized == "alt+up" || normalized == "altup" || normalized == "meta+arrowup" ||
      normalized == "metaarrowup" || normalized == "meta+up" || normalized == "metaup")
    return Key::AltArrowUp;
  if (normalized == "alt+arrowdown" || normalized == "altarrowdown" || normalized == "alt+down" || normalized == "altdown" || normalized == "meta+arrowdown" ||
      normalized == "metaarrowdown" || normalized == "meta+down" || normalized == "metadown")
    return Key::AltArrowDown;
  if (normalized == "pageup" || normalized == "pgup")
    return Key::PageUp;
  if (normalized == "pagedown" || normalized == "pgdown")
    return Key::PageDown;
  if (normalized == "home")
    return Key::Home;
  if (normalized == "end")
    return Key::End;
  if (normalized == "ctrl+home" || normalized == "ctrlhome" || normalized == "control+home" || normalized == "controlhome")
    return Key::CtrlHome;
  if (normalized == "ctrl+end" || normalized == "ctrlend" || normalized == "control+end" || normalized == "controlend")
    return Key::CtrlEnd;
  if (normalized == "shift+home" || normalized == "shifthome")
    return Key::ShiftHome;
  if (normalized == "shift+end" || normalized == "shiftend")
    return Key::ShiftEnd;
  if (normalized == "shift+ctrl+home" || normalized == "ctrl+shift+home" || normalized == "shiftctrlhome" || normalized == "ctrlshifthome" ||
      normalized == "shift+control+home" || normalized == "control+shift+home" || normalized == "shiftcontrolhome" || normalized == "controlshifthome")
    return Key::ShiftCtrlHome;
  if (normalized == "shift+ctrl+end" || normalized == "ctrl+shift+end" || normalized == "shiftctrlend" || normalized == "ctrlshiftend" ||
      normalized == "shift+control+end" || normalized == "control+shift+end" || normalized == "shiftcontrolend" || normalized == "controlshiftend")
    return Key::ShiftCtrlEnd;
  if (normalized == "ctrl+a" || normalized == "ctrla")
    return Key::CtrlA;
  if (normalized == "ctrl+b" || normalized == "ctrlb")
    return Key::CtrlB;
  if (normalized == "ctrl+c" || normalized == "ctrlc")
    return Key::CtrlC;
  if (normalized == "ctrl+d" || normalized == "ctrld")
    return Key::CtrlD;
  if (normalized == "ctrl+e" || normalized == "ctrle")
    return Key::CtrlE;
  if (normalized == "ctrl+f" || normalized == "ctrlf")
    return Key::CtrlF;
  if (normalized == "ctrl+g" || normalized == "ctrlg")
    return Key::CtrlG;
  if (normalized == "ctrl+h" || normalized == "ctrlh")
    return Key::CtrlH;
  if (normalized == "ctrl+k" || normalized == "ctrlk")
    return Key::CtrlK;
  if (normalized == "ctrl+l" || normalized == "ctrll")
    return Key::CtrlL;
  if (normalized == "ctrl+" || normalized == "ctrl+minus" || normalized == "ctrlminus" || normalized == "ctrlhyphen")
    return Key::CtrlMinus;
  if (normalized == "ctrl+/" || normalized == "ctrl/" || normalized == "ctrl+slash" || normalized == "ctrlslash")
    return Key::CtrlSlash;
  if (normalized == "ctrl+n" || normalized == "ctrln")
    return Key::CtrlN;
  if (normalized == "ctrl+o" || normalized == "ctrlo")
    return Key::CtrlO;
  if (normalized == "ctrl+p" || normalized == "ctrlp")
    return Key::CtrlP;
  if (normalized == "shift+ctrl+p" || normalized == "ctrl+shift+p" || normalized == "shiftctrlp" || normalized == "ctrlshiftp")
    return Key::CtrlShiftP;
  if (normalized == "ctrl+r" || normalized == "ctrlr")
    return Key::CtrlR;
  if (normalized == "ctrl+]" || normalized == "ctrl]" || normalized == "ctrl+rightbracket" || normalized == "ctrlrightbracket")
    return Key::CtrlRightBracket;
  if (normalized == "ctrl+s" || normalized == "ctrls")
    return Key::CtrlS;
  if (normalized == "ctrl+t" || normalized == "ctrlt")
    return Key::CtrlT;
  if (normalized == "ctrl+u" || normalized == "ctrlu")
    return Key::CtrlU;
  if (normalized == "ctrl+v" || normalized == "ctrlv")
    return Key::CtrlV;
  if (normalized == "ctrl+w" || normalized == "ctrlw")
    return Key::CtrlW;
  if (normalized == "ctrl+x" || normalized == "ctrlx")
    return Key::CtrlX;
  if (normalized == "ctrl+y" || normalized == "ctrly")
    return Key::CtrlY;
  if (normalized == "ctrl+z" || normalized == "ctrlz")
    return Key::CtrlZ;
  if (normalized == "f1")
    return Key::F1;
  if (normalized == "f2")
    return Key::F2;
  if (normalized == "f3")
    return Key::F3;
  if (normalized == "f4")
    return Key::F4;
  if (normalized == "f5")
    return Key::F5;
  if (normalized == "f6")
    return Key::F6;
  if (normalized == "f7")
    return Key::F7;
  if (normalized == "f8")
    return Key::F8;
  if (normalized == "f9")
    return Key::F9;
  if (normalized == "f10")
    return Key::F10;
  if (normalized == "f11")
    return Key::F11;
  if (normalized == "f12")
    return Key::F12;
  if (normalized == "alt+backspace" || normalized == "altbackspace" || normalized == "meta+backspace" || normalized == "metabackspace")
    return Key::AltBackspace;
  if (normalized == "alt+b" || normalized == "altb" || normalized == "meta+b" || normalized == "metab")
    return Key::AltB;
  if (normalized == "alt+d" || normalized == "altd" || normalized == "meta+d" || normalized == "metad")
    return Key::AltD;
  if (normalized == "alt+delete" || normalized == "altdelete" || normalized == "alt+del" || normalized == "altdel" || normalized == "meta+delete" ||
      normalized == "metadelete" || normalized == "meta+del" || normalized == "metadel")
    return Key::AltDelete;
  if (normalized == "alt+f" || normalized == "altf" || normalized == "meta+f" || normalized == "metaf")
    return Key::AltF;
  if (normalized == "alt+h" || normalized == "alth" || normalized == "meta+h" || normalized == "metah")
    return Key::AltH;
  if (normalized == "alt+j" || normalized == "altj" || normalized == "meta+j" || normalized == "metaj")
    return Key::AltJ;
  if (normalized == "alt+k" || normalized == "altk" || normalized == "meta+k" || normalized == "metak")
    return Key::AltK;
  if (normalized == "alt+l" || normalized == "altl" || normalized == "meta+l" || normalized == "metal")
    return Key::AltL;
  if (normalized == "alt+w" || normalized == "altw" || normalized == "meta+w" || normalized == "metaw")
    return Key::AltW;
  if (normalized == "ctrl+alt+]" || normalized == "ctrlalt]" || normalized == "alt+ctrl+]" || normalized == "altctrl]" ||
      normalized == "ctrl+alt+rightbracket" || normalized == "ctrlaltrightbracket" || normalized == "alt+ctrl+rightbracket" ||
      normalized == "altctrlrightbracket")
    return Key::CtrlAltRightBracket;
  if (normalized == "alt+y" || normalized == "alty" || normalized == "meta+y" || normalized == "metay")
    return Key::AltY;
  return std::nullopt;
}

std::string key_display(terminal::Key key)
{
  using Key = terminal::Key;
  switch (key)
  {
    case Key::Enter:
      return "Enter";
    case Key::Backspace:
      return "Backspace";
    case Key::ShiftBackspace:
      return "Shift+Backspace";
    case Key::CtrlBackspace:
      return "Ctrl+Backspace";
    case Key::Delete:
      return "Delete";
    case Key::ShiftDelete:
      return "Shift+Delete";
    case Key::Insert:
      return "Insert";
    case Key::Clear:
      return "Clear";
    case Key::Tab:
      return "Tab";
    case Key::Space:
      return "Space";
    case Key::CtrlSpace:
      return "Ctrl+Space";
    case Key::Ctrl0:
      return "Ctrl+0";
    case Key::Ctrl1:
      return "Ctrl+1";
    case Key::Ctrl2:
      return "Ctrl+2";
    case Key::Ctrl3:
      return "Ctrl+3";
    case Key::Ctrl4:
      return "Ctrl+4";
    case Key::Ctrl5:
      return "Ctrl+5";
    case Key::Ctrl6:
      return "Ctrl+6";
    case Key::Ctrl7:
      return "Ctrl+7";
    case Key::Ctrl8:
      return "Ctrl+8";
    case Key::Ctrl9:
      return "Ctrl+9";
    case Key::ShiftTab:
      return "Shift+Tab";
    case Key::ShiftL:
      return "Shift+L";
    case Key::ShiftT:
      return "Shift+T";
    case Key::Escape:
      return "Esc";
    case Key::ArrowUp:
      return "Up";
    case Key::ArrowDown:
      return "Down";
    case Key::ArrowLeft:
      return "Left";
    case Key::ArrowRight:
      return "Right";
    case Key::ShiftArrowUp:
      return "Shift+Up";
    case Key::ShiftArrowDown:
      return "Shift+Down";
    case Key::ShiftArrowLeft:
      return "Shift+Left";
    case Key::ShiftArrowRight:
      return "Shift+Right";
    case Key::ShiftCtrlArrowLeft:
      return "Shift+Ctrl+Left";
    case Key::ShiftCtrlArrowRight:
      return "Shift+Ctrl+Right";
    case Key::ShiftAltArrowLeft:
      return "Shift+Alt+Left";
    case Key::ShiftAltArrowRight:
      return "Shift+Alt+Right";
    case Key::CtrlArrowLeft:
      return "Ctrl+Left";
    case Key::CtrlArrowRight:
      return "Ctrl+Right";
    case Key::AltArrowUp:
      return "Alt+Up";
    case Key::AltArrowDown:
      return "Alt+Down";
    case Key::AltArrowLeft:
      return "Alt+Left";
    case Key::AltArrowRight:
      return "Alt+Right";
    case Key::PageUp:
      return "PageUp";
    case Key::PageDown:
      return "PageDown";
    case Key::Home:
      return "Home";
    case Key::End:
      return "End";
    case Key::CtrlHome:
      return "Ctrl+Home";
    case Key::CtrlEnd:
      return "Ctrl+End";
    case Key::ShiftHome:
      return "Shift+Home";
    case Key::ShiftEnd:
      return "Shift+End";
    case Key::ShiftCtrlHome:
      return "Shift+Ctrl+Home";
    case Key::ShiftCtrlEnd:
      return "Shift+Ctrl+End";
    case Key::ShiftEnter:
      return "Shift+Enter";
    case Key::CtrlEnter:
      return "Ctrl+Enter";
    case Key::AltEnter:
      return "Alt+Enter";
    case Key::CtrlA:
      return "Ctrl+A";
    case Key::CtrlB:
      return "Ctrl+B";
    case Key::CtrlC:
      return "Ctrl+C";
    case Key::CtrlD:
      return "Ctrl+D";
    case Key::CtrlE:
      return "Ctrl+E";
    case Key::CtrlF:
      return "Ctrl+F";
    case Key::CtrlG:
      return "Ctrl+G";
    case Key::CtrlH:
      return "Ctrl+H";
    case Key::CtrlK:
      return "Ctrl+K";
    case Key::CtrlL:
      return "Ctrl+L";
    case Key::CtrlMinus:
      return "Ctrl+-";
    case Key::CtrlSlash:
      return "Ctrl+/";
    case Key::CtrlN:
      return "Ctrl+N";
    case Key::CtrlO:
      return "Ctrl+O";
    case Key::CtrlP:
      return "Ctrl+P";
    case Key::CtrlShiftP:
      return "Shift+Ctrl+P";
    case Key::CtrlQ:
      return "Ctrl+Q";
    case Key::CtrlR:
      return "Ctrl+R";
    case Key::CtrlRightBracket:
      return "Ctrl+]";
    case Key::CtrlS:
      return "Ctrl+S";
    case Key::CtrlT:
      return "Ctrl+T";
    case Key::CtrlU:
      return "Ctrl+U";
    case Key::CtrlV:
      return "Ctrl+V";
    case Key::CtrlW:
      return "Ctrl+W";
    case Key::CtrlX:
      return "Ctrl+X";
    case Key::CtrlY:
      return "Ctrl+Y";
    case Key::CtrlZ:
      return "Ctrl+Z";
    case Key::F1:
      return "F1";
    case Key::F2:
      return "F2";
    case Key::F3:
      return "F3";
    case Key::F4:
      return "F4";
    case Key::F5:
      return "F5";
    case Key::F6:
      return "F6";
    case Key::F7:
      return "F7";
    case Key::F8:
      return "F8";
    case Key::F9:
      return "F9";
    case Key::F10:
      return "F10";
    case Key::F11:
      return "F11";
    case Key::F12:
      return "F12";
    case Key::AltBackspace:
      return "Alt+Backspace";
    case Key::AltB:
      return "Alt+B";
    case Key::AltD:
      return "Alt+D";
    case Key::AltDelete:
      return "Alt+Delete";
    case Key::AltF:
      return "Alt+F";
    case Key::AltH:
      return "Alt+H";
    case Key::AltJ:
      return "Alt+J";
    case Key::AltK:
      return "Alt+K";
    case Key::AltL:
      return "Alt+L";
    case Key::AltW:
      return "Alt+W";
    case Key::CtrlAltRightBracket:
      return "Ctrl+Alt+]";
    case Key::AltY:
      return "Alt+Y";
    case Key::MouseWheelUp:
      return "MouseWheelUp";
    case Key::MouseWheelDown:
      return "MouseWheelDown";
    case Key::MouseLeftPress:
      return "MouseLeftPress";
    case Key::MouseLeftClick:
      return "MouseLeftClick";
    case Key::MouseLeftDrag:
      return "MouseLeftDrag";
    case Key::MouseLeftRelease:
      return "MouseLeftRelease";
    case Key::MousePointerCancel:
      return "MousePointerCancel";
    case Key::Character:
    case Key::Unknown:
    case Key::Resize:
    case Key::Mouse:
    case Key::WideCharacter:
      return "";
  }
  return "";
}

std::string action_name(TuiAction action)
{
  switch (action)
  {
    case TuiAction::Submit:
      return "submit";
    case TuiAction::NewLine:
      return "new_line";
    case TuiAction::Cancel:
      return "cancel";
    case TuiAction::ClearInput:
      return "clear_input";
    case TuiAction::CopySelection:
      return "copy_selection";
    case TuiAction::CopyLatestAssistant:
      return "copy_latest_assistant";
    case TuiAction::PromptStash:
      return "prompt_stash";
    case TuiAction::ExternalEditor:
      return "external_editor";
    case TuiAction::Suspend:
      return "suspend";
    case TuiAction::ClipboardPasteImage:
      return "clipboard_paste_image";
    case TuiAction::DeleteBackward:
      return "delete_backward";
    case TuiAction::DeleteForward:
      return "delete_forward";
    case TuiAction::HistoryPrev:
      return "history_prev";
    case TuiAction::HistoryNext:
      return "history_next";
    case TuiAction::PalettePrev:
      return "palette_prev";
    case TuiAction::PaletteNext:
      return "palette_next";
    case TuiAction::SelectPrev:
      return "select_prev";
    case TuiAction::SelectNext:
      return "select_next";
    case TuiAction::SelectPageUp:
      return "select_page_up";
    case TuiAction::SelectPageDown:
      return "select_page_down";
    case TuiAction::SelectConfirm:
      return "select_confirm";
    case TuiAction::SelectCancel:
      return "select_cancel";
    case TuiAction::CursorLeft:
      return "cursor_left";
    case TuiAction::CursorRight:
      return "cursor_right";
    case TuiAction::CursorUp:
      return "cursor_up";
    case TuiAction::CursorDown:
      return "cursor_down";
    case TuiAction::CursorLineStart:
      return "cursor_line_start";
    case TuiAction::CursorLineEnd:
      return "cursor_line_end";
    case TuiAction::CursorWordLeft:
      return "cursor_word_left";
    case TuiAction::CursorWordRight:
      return "cursor_word_right";
    case TuiAction::JumpForward:
      return "jump_forward";
    case TuiAction::JumpBackward:
      return "jump_backward";
    case TuiAction::DeleteWordBackward:
      return "delete_word_backward";
    case TuiAction::DeleteWordForward:
      return "delete_word_forward";
    case TuiAction::DeleteToLineStart:
      return "delete_to_line_start";
    case TuiAction::DeleteToLineEnd:
      return "delete_to_line_end";
    case TuiAction::Undo:
      return "undo";
    case TuiAction::Redo:
      return "redo";
    case TuiAction::Yank:
      return "yank";
    case TuiAction::YankPop:
      return "yank_pop";
    case TuiAction::AutocompleteAccept:
      return "autocomplete_accept";
    case TuiAction::PromptAllow:
      return "prompt_allow";
    case TuiAction::PromptDeny:
      return "prompt_deny";
    case TuiAction::DetailsToggle:
      return "details_toggle";
    case TuiAction::PageUp:
      return "page_up";
    case TuiAction::PageDown:
      return "page_down";
    case TuiAction::TranscriptHalfPageUp:
      return "transcript_half_page_up";
    case TuiAction::TranscriptHalfPageDown:
      return "transcript_half_page_down";
    case TuiAction::ModeToggle:
      return "mode_toggle";
    case TuiAction::Interrupt:
      return "interrupt";
    case TuiAction::Exit:
      return "exit";
    case TuiAction::VariantCycle:
      return "variant_cycle";
    case TuiAction::ReasoningSelect:
      return "reasoning_select";
    case TuiAction::ThinkingToggle:
      return "thinking_toggle";
    case TuiAction::ModelSelect:
      return "model_select";
    case TuiAction::ModelCycleForward:
      return "model_cycle_forward";
    case TuiAction::ModelCycleBackward:
      return "model_cycle_backward";
    case TuiAction::ModelsSave:
      return "models_save";
    case TuiAction::ModelsEnableAll:
      return "models_enable_all";
    case TuiAction::ModelsClearAll:
      return "models_clear_all";
    case TuiAction::ModelsToggleProvider:
      return "models_toggle_provider";
    case TuiAction::ModelsReorderUp:
      return "models_reorder_up";
    case TuiAction::ModelsReorderDown:
      return "models_reorder_down";
    case TuiAction::MessageFollowUp:
      return "message_follow_up";
    case TuiAction::MessageDequeue:
      return "message_dequeue";
    case TuiAction::MessagePrev:
      return "message_prev";
    case TuiAction::MessageNext:
      return "message_next";
    case TuiAction::JumpToBottom:
      return "jump_to_bottom";
    case TuiAction::SessionNew:
      return "session_new";
    case TuiAction::SessionTree:
      return "session_tree";
    case TuiAction::SessionFork:
      return "session_fork";
    case TuiAction::SessionResume:
      return "session_resume";
    case TuiAction::SessionTogglePath:
      return "session_toggle_path";
    case TuiAction::SessionToggleSort:
      return "session_toggle_sort";
    case TuiAction::SessionToggleNamedFilter:
      return "session_toggle_named_filter";
    case TuiAction::SessionRename:
      return "session_rename";
    case TuiAction::SessionArchive:
      return "session_archive";
    case TuiAction::SessionArchiveNoninvasive:
      return "session_archive_noninvasive";
    case TuiAction::SessionSummarizeParent:
      return "session_summarize_parent";
    case TuiAction::TreeFoldOrUp:
      return "tree_fold_or_up";
    case TuiAction::TreeUnfoldOrDown:
      return "tree_unfold_or_down";
    case TuiAction::TreeEditLabel:
      return "tree_edit_label";
    case TuiAction::TreeToggleLabelTimestamp:
      return "tree_toggle_label_timestamp";
    case TuiAction::TreeFilterLabeledOnly:
      return "tree_filter_labeled_only";
    case TuiAction::TreeFilterAll:
      return "tree_filter_all";
    case TuiAction::OverviewToggle:
      return "overview_toggle";
  }
  return "unknown";
}

std::string action_label(TuiAction action)
{
  switch (action)
  {
    case TuiAction::Submit:
      return "Submit";
    case TuiAction::NewLine:
      return "Insert newline";
    case TuiAction::Cancel:
      return "Cancel";
    case TuiAction::ClearInput:
      return "Clear input";
    case TuiAction::CopySelection:
      return "Copy selection";
    case TuiAction::CopyLatestAssistant:
      return "Copy latest assistant message";
    case TuiAction::PromptStash:
      return "Stash or restore prompt";
    case TuiAction::ExternalEditor:
      return "Open external editor";
    case TuiAction::Suspend:
      return "Suspend";
    case TuiAction::ClipboardPasteImage:
      return "Paste clipboard image";
    case TuiAction::DeleteBackward:
      return "Delete backward";
    case TuiAction::DeleteForward:
      return "Delete forward";
    case TuiAction::HistoryPrev:
      return "Previous history";
    case TuiAction::HistoryNext:
      return "Next history";
    case TuiAction::PalettePrev:
      return "Previous palette item";
    case TuiAction::PaletteNext:
      return "Next palette item";
    case TuiAction::SelectPrev:
      return "Previous item";
    case TuiAction::SelectNext:
      return "Next item";
    case TuiAction::SelectPageUp:
      return "Select page up";
    case TuiAction::SelectPageDown:
      return "Select page down";
    case TuiAction::SelectConfirm:
      return "Confirm selection";
    case TuiAction::SelectCancel:
      return "Cancel selection";
    case TuiAction::CursorLeft:
      return "Move cursor left";
    case TuiAction::CursorRight:
      return "Move cursor right";
    case TuiAction::CursorUp:
      return "Move cursor up";
    case TuiAction::CursorDown:
      return "Move cursor down";
    case TuiAction::CursorLineStart:
      return "Move to line start";
    case TuiAction::CursorLineEnd:
      return "Move to line end";
    case TuiAction::CursorWordLeft:
      return "Move word left";
    case TuiAction::CursorWordRight:
      return "Move word right";
    case TuiAction::JumpForward:
      return "Jump forward";
    case TuiAction::JumpBackward:
      return "Jump backward";
    case TuiAction::DeleteWordBackward:
      return "Delete word backward";
    case TuiAction::DeleteWordForward:
      return "Delete word forward";
    case TuiAction::DeleteToLineStart:
      return "Delete to line start";
    case TuiAction::DeleteToLineEnd:
      return "Delete to line end";
    case TuiAction::Undo:
      return "Undo";
    case TuiAction::Redo:
      return "Redo";
    case TuiAction::Yank:
      return "Yank";
    case TuiAction::YankPop:
      return "Yank pop";
    case TuiAction::AutocompleteAccept:
      return "Accept autocomplete";
    case TuiAction::PromptAllow:
      return "Allow prompt";
    case TuiAction::PromptDeny:
      return "Deny prompt";
    case TuiAction::DetailsToggle:
      return "Toggle details";
    case TuiAction::PageUp:
      return "Page up";
    case TuiAction::PageDown:
      return "Page down";
    case TuiAction::TranscriptHalfPageUp:
      return "Transcript half-page up";
    case TuiAction::TranscriptHalfPageDown:
      return "Transcript half-page down";
    case TuiAction::ModeToggle:
      return "Toggle build/plan mode";
    case TuiAction::Interrupt:
      return "Interrupt";
    case TuiAction::Exit:
      return "Exit";
    case TuiAction::VariantCycle:
      return "Cycle variant";
    case TuiAction::ReasoningSelect:
      return "Select thinking mode";
    case TuiAction::ThinkingToggle:
      return "Toggle thinking";
    case TuiAction::ModelSelect:
      return "Select model";
    case TuiAction::ModelCycleForward:
      return "Next model";
    case TuiAction::ModelCycleBackward:
      return "Previous model";
    case TuiAction::ModelsSave:
      return "Save models";
    case TuiAction::ModelsEnableAll:
      return "Enable all models";
    case TuiAction::ModelsClearAll:
      return "Clear all models";
    case TuiAction::ModelsToggleProvider:
      return "Toggle provider";
    case TuiAction::ModelsReorderUp:
      return "Reorder model up";
    case TuiAction::ModelsReorderDown:
      return "Reorder model down";
    case TuiAction::MessageFollowUp:
      return "Queue follow-up";
    case TuiAction::MessageDequeue:
      return "Dequeue message";
    case TuiAction::MessagePrev:
      return "Previous user turn";
    case TuiAction::MessageNext:
      return "Next user turn";
    case TuiAction::JumpToBottom:
      return "Jump to live tail";
    case TuiAction::SessionNew:
      return "New session";
    case TuiAction::SessionTree:
      return "Session tree";
    case TuiAction::SessionFork:
      return "Fork session";
    case TuiAction::SessionResume:
      return "Resume session";
    case TuiAction::SessionTogglePath:
      return "Toggle session path";
    case TuiAction::SessionToggleSort:
      return "Toggle session sort";
    case TuiAction::SessionToggleNamedFilter:
      return "Toggle named filter";
    case TuiAction::SessionRename:
      return "Rename session";
    case TuiAction::SessionArchive:
      return "Archive session";
    case TuiAction::SessionArchiveNoninvasive:
      return "Archive/restore (empty search)";
    case TuiAction::SessionSummarizeParent:
      return "Summarize abandoned parent";
    case TuiAction::TreeFoldOrUp:
      return "Go to parent session";
    case TuiAction::TreeUnfoldOrDown:
      return "Go to child session";
    case TuiAction::TreeEditLabel:
      return "Edit tree label";
    case TuiAction::TreeToggleLabelTimestamp:
      return "Toggle label timestamp";
    case TuiAction::TreeFilterLabeledOnly:
      return "Filter labeled only";
    case TuiAction::TreeFilterAll:
      return "Toggle archived sessions";
    case TuiAction::OverviewToggle:
      return "Toggle startup overview";
  }
  return "Unknown action";
}

std::optional<TuiAction> key_binding_action_from_name(std::string_view name)
{
  auto const resolved = action_from_name(name);
  if (!resolved)
    return std::nullopt;
  return resolved->action;
}

std::string key_binding_config_action_id(TuiAction action)
{
  return std::string(config_action_id(action));
}

std::string action_description(TuiAction action)
{
  switch (action)
  {
    case TuiAction::Submit:
      return "Submit input or select the highlighted slash command";
    case TuiAction::NewLine:
      return "Insert a newline in the composer";
    case TuiAction::Cancel:
      return "Dismiss slash suggestions or clear composer input";
    case TuiAction::ClearInput:
      return "Clear the current composer input";
    case TuiAction::CopySelection:
      return "Copy the selected composer or transcript text";
    case TuiAction::CopyLatestAssistant:
      return "Copy the latest assistant message without changing the composer";
    case TuiAction::PromptStash:
      return "Stash a nonempty prompt or open the process-memory prompt stash";
    case TuiAction::ExternalEditor:
      return "Open the current draft in $VISUAL or $EDITOR";
    case TuiAction::Suspend:
      return "Suspend AVA to the background until resumed by the shell";
    case TuiAction::ClipboardPasteImage:
      return "Paste an image from the system clipboard as a pending attachment";
    case TuiAction::DeleteBackward:
      return "Delete the previous character";
    case TuiAction::DeleteForward:
      return "Delete the character after the cursor";
    case TuiAction::HistoryPrev:
      return "Recall the previous input history item, or scroll up when history is empty";
    case TuiAction::HistoryNext:
      return "Recall the next input history item, or scroll down when not browsing history";
    case TuiAction::PalettePrev:
      return "Move to the previous slash palette item";
    case TuiAction::PaletteNext:
      return "Move to the next slash palette item";
    case TuiAction::SelectPrev:
      return "Move to the previous select-list item";
    case TuiAction::SelectNext:
      return "Move to the next select-list item";
    case TuiAction::SelectPageUp:
      return "Page up in a select-list modal";
    case TuiAction::SelectPageDown:
      return "Page down in a select-list modal";
    case TuiAction::SelectConfirm:
      return "Confirm the highlighted select-list item";
    case TuiAction::SelectCancel:
      return "Cancel the active select-list modal";
    case TuiAction::CursorLeft:
      return "Move the input cursor left";
    case TuiAction::CursorRight:
      return "Move the input cursor right";
    case TuiAction::CursorUp:
      return "Move the input cursor up within a multiline draft";
    case TuiAction::CursorDown:
      return "Move the input cursor down within a multiline draft";
    case TuiAction::CursorLineStart:
      return "Move the input cursor to the start of the current line";
    case TuiAction::CursorLineEnd:
      return "Move the input cursor to the end of the current line";
    case TuiAction::CursorWordLeft:
      return "Move the input cursor to the previous word";
    case TuiAction::CursorWordRight:
      return "Move the input cursor to the next word";
    case TuiAction::JumpForward:
      return "Jump forward to the next typed character";
    case TuiAction::JumpBackward:
      return "Jump backward to the next typed character";
    case TuiAction::DeleteWordBackward:
      return "Delete the word before the cursor";
    case TuiAction::DeleteWordForward:
      return "Delete the word after the cursor";
    case TuiAction::DeleteToLineStart:
      return "Delete from the cursor to the start of the current line";
    case TuiAction::DeleteToLineEnd:
      return "Delete from the cursor to the end of the current line";
    case TuiAction::Undo:
      return "Undo the last composer edit";
    case TuiAction::Redo:
      return "Redo the last undone composer edit";
    case TuiAction::Yank:
      return "Paste the last killed composer text";
    case TuiAction::YankPop:
      return "Replace the last yank with the next kill-ring entry";
    case TuiAction::AutocompleteAccept:
      return "Accept the highlighted slash command suggestion";
    case TuiAction::PromptAllow:
      return "Allow the active permission prompt when prompt keybind support exists";
    case TuiAction::PromptDeny:
      return "Deny or cancel the active prompt";
    case TuiAction::DetailsToggle:
      return "Toggle details for the focused prompt or tool when available";
    case TuiAction::PageUp:
      return "Scroll the transcript up by half a page";
    case TuiAction::PageDown:
      return "Scroll the transcript down by half a page";
    case TuiAction::TranscriptHalfPageUp:
      return "Scroll the transcript up by half the visible transcript rows";
    case TuiAction::TranscriptHalfPageDown:
      return "Scroll the transcript down by half the visible transcript rows";
    case TuiAction::ModeToggle:
      return "Toggle build/plan mode when the slash palette is not active";
    case TuiAction::Interrupt:
      return "Clear a non-empty composer draft, otherwise exit the current TUI loop";
    case TuiAction::Exit:
      return "Exit the TUI when the composer is empty";
    case TuiAction::VariantCycle:
      return "Cycle model reasoning choices when backend support exists";
    case TuiAction::ReasoningSelect:
      return "Choose a policy-supported thinking mode between turns";
    case TuiAction::ThinkingToggle:
      return "Toggle thinking block visibility without changing provider reasoning mode";
    case TuiAction::ModelSelect:
      return "Open the model selector when available";
    case TuiAction::ModelCycleForward:
      return "Cycle to the next configured model";
    case TuiAction::ModelCycleBackward:
      return "Cycle to the previous configured model when terminal input reports Shift+Ctrl+P";
    case TuiAction::ModelsSave:
      return "Save the scoped model-cycle selection when persistent model settings support exists";
    case TuiAction::ModelsEnableAll:
      return "Enable all visible models in the scoped model-cycle selector";
    case TuiAction::ModelsClearAll:
      return "Clear all visible models in the scoped model-cycle selector";
    case TuiAction::ModelsToggleProvider:
      return "Toggle the highlighted provider in the scoped model-cycle selector";
    case TuiAction::ModelsReorderUp:
      return "Move the highlighted enabled model earlier in the scoped cycle order";
    case TuiAction::ModelsReorderDown:
      return "Move the highlighted enabled model later in the scoped cycle order";
    case TuiAction::MessageFollowUp:
      return "Queue a follow-up message during an active run, or submit normally when idle";
    case TuiAction::MessageDequeue:
      return "Restore the latest queued active-run message to the composer";
    case TuiAction::MessagePrev:
      return "Jump to the previous retained user turn";
    case TuiAction::MessageNext:
      return "Jump to the next retained user turn, then return to the live tail";
    case TuiAction::JumpToBottom:
      return "Return the transcript to the live tail";
    case TuiAction::SessionNew:
      return "Start a new session through the existing /new workflow";
    case TuiAction::SessionTree:
      return "Open the session tree selector";
    case TuiAction::SessionFork:
      return "Fork the current session through the existing /fork workflow";
    case TuiAction::SessionResume:
      return "Open the session resume selector";
    case TuiAction::SessionTogglePath:
      return "Toggle path display in the session selector";
    case TuiAction::SessionToggleSort:
      return "Cycle sort mode in the session selector";
    case TuiAction::SessionToggleNamedFilter:
      return "Toggle named-session filtering in the session selector";
    case TuiAction::SessionRename:
      return "Draft a rename command for the highlighted session";
    case TuiAction::SessionArchive:
      return "Archive or restore the highlighted session after confirmation";
    case TuiAction::SessionArchiveNoninvasive:
      return "Archive or restore the highlighted session when the selector search is empty";
    case TuiAction::SessionSummarizeParent:
      return "Prepare a summary for abandoned work in the current session's direct parent from the session selector";
    case TuiAction::TreeFoldOrUp:
      return "Navigate to the highlighted session's parent branch in the selector";
    case TuiAction::TreeUnfoldOrDown:
      return "Navigate to the highlighted session's first child branch in the selector";
    case TuiAction::TreeEditLabel:
      return "Draft a labels command for the highlighted session";
    case TuiAction::TreeToggleLabelTimestamp:
      return "Toggle label timestamps in the session tree selector";
    case TuiAction::TreeFilterLabeledOnly:
      return "Toggle named/labeled session filtering in the session selector";
    case TuiAction::TreeFilterAll:
      return "Toggle archived-session visibility in the session selector";
    case TuiAction::OverviewToggle:
      return "Open or close the path-free startup overview";
  }
  return "Unknown action";
}

std::string keys_display(TuiKeyBindings const& bindings, TuiAction action)
{
  for (auto const& [candidate, keys] : bindings.bindings)
  {
    if (candidate != action)
      continue;
    std::string text;
    for (auto const key : keys)
    {
      auto display = key_display(key);
      if (display.empty())
        continue;
      if (!text.empty())
        text += ", ";
      text += display;
    }
    return text;
  }
  return "";
}

std::vector<TuiKeyBindingHelpItem> key_binding_help_items(TuiKeyBindings const& bindings)
{
  std::vector<TuiKeyBindingHelpItem> items;
  items.reserve(bindings.bindings.size());
  for (auto const& [action, keys] : bindings.bindings)
  {
    static_cast<void>(keys);
    auto keys_text = keys_display(bindings, action);
    if (keys_text.empty())
      continue;
    items.push_back(TuiKeyBindingHelpItem{
        .label = action_label(action), .action = action_name(action), .description = action_description(action), .keys = std::move(keys_text)});
  }
  return items;
}

std::string default_key_bindings_config_json()
{
  auto const defaults = default_key_bindings();
  std::string output = "{\n";
  bool first_action = true;
  for (auto const& [action, keys] : defaults.bindings)
  {
    if (has_same_context_default_key_conflict(defaults, action, keys))
      continue;

    std::vector<std::string> displays;
    displays.reserve(keys.size());
    for (auto const key : keys)
    {
      auto display = key_display(key);
      if (!display.empty())
        displays.push_back(std::move(display));
    }
    if (displays.empty())
      continue;

    if (!first_action)
      output += ",\n";
    first_action = false;
    output += "  ";
    append_json_string(output, config_action_id(action));
    output += ": [";
    for (std::size_t index = 0; index < displays.size(); ++index)
    {
      if (index > 0)
        output += ", ";
      append_json_string(output, displays[index]);
    }
    output += "]";
  }
  output += "\n}\n";
  return output;
}

ava::core::Result<TuiKeyBindings> parse_key_bindings_json(std::string_view json)
{
  return parse_key_bindings_json(json, default_key_bindings());
}

ava::core::Result<TuiKeyBindings> parse_key_bindings_json(std::string_view json, TuiKeyBindings base)
{
  auto entries = parse_key_binding_entries(json);
  if (!entries)
    return std::unexpected(std::move(entries.error()));

  std::vector<std::pair<TuiAction, std::vector<terminal::Key>>> overrides;
  overrides.reserve(entries->size());
  for (auto const& entry : *entries)
  {
    auto keys = parse_key_values(entry.values);
    if (!keys)
      return std::unexpected(std::move(keys.error()));
    overrides.push_back({entry.action, std::move(*keys)});
  }

  if (auto conflicts = validate_user_key_conflicts(overrides); !conflicts)
    return std::unexpected(std::move(conflicts.error()));

  for (auto& [action, keys] : overrides)
  {
    remove_keys_from_other_actions(base, action, keys);
    auto target = keys_for_action(base, action);
    if (!target)
    {
      base.bindings.push_back({action, std::move(keys)});
    }
    else
    {
      **target = std::move(keys);
    }
  }

  return base;
}

ava::core::Result<TuiKeyBindings> load_key_bindings(std::filesystem::path const& keybinds_file)
{
  std::error_code exists_error;
  if (!std::filesystem::exists(keybinds_file, exists_error))
    return default_key_bindings();
  if (exists_error)
  {
    auto error = ava::core::Error(ava::core::ErrorCategory::Io, "failed to inspect TUI keybinds file");
    error.with_context("path", keybinds_file.string()).with_context("cause", exists_error.message());
    return std::unexpected(std::move(error));
  }

  std::ifstream input(keybinds_file, std::ios::binary);
  if (!input)
  {
    auto error = ava::core::Error(ava::core::ErrorCategory::Io, "failed to read TUI keybinds file");
    error.with_context("path", keybinds_file.string());
    return std::unexpected(std::move(error));
  }
  std::string const content((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
  auto parsed = parse_key_bindings_json(content);
  if (!parsed)
    parsed.error().with_context("path", keybinds_file.string());
  return parsed;
}

}  // namespace ava::tui

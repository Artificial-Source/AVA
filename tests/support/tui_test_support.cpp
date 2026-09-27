#include "sys.h"
#include "tests/support/tui_test_support.h"
#include "ava/core/Application.h"

#include <algorithm>
#include <utility>

namespace tui_test_support {

// Make the test's newterm output stream available to the application Context used by the composer renderer.
ScopedComposerScreenOutput::ScopedComposerScreenOutput(FILE* output)
{
  ava::core::Application::instance().terminal_context().bind_external_screen_output_for_test(output);
}

// Release the borrowed stream before its owning test fixture closes it.
ScopedComposerScreenOutput::~ScopedComposerScreenOutput()
{
  ava::core::Application::instance().terminal_context().bind_external_screen_output_for_test(nullptr);
}

ScopedTerminalCapabilityProfile::ScopedTerminalCapabilityProfile(std::string term_program_value)
    : term("TERM", "xterm-256color"),
      term_program("TERM_PROGRAM", std::move(term_program_value)),
      terminal_emulator("TERMINAL_EMULATOR", ""),
      tmux("TMUX", ""),
      kitty_window_id("KITTY_WINDOW_ID", ""),
      ghostty_resources_dir("GHOSTTY_RESOURCES_DIR", ""),
      wezterm_pane("WEZTERM_PANE", ""),
      warp_session_id("WARP_SESSION_ID", ""),
      warp_terminal_session_uuid("WARP_TERMINAL_SESSION_UUID", ""),
      iterm_session_id("ITERM_SESSION_ID", ""),
      wt_session("WT_SESSION", ""),
      tmux_hyperlinks("AVA_TUI_TMUX_HYPERLINKS", "")
{
}

std::vector<ava::tui::SlashCommandItem> const& standard_slash_commands()
{
  static std::vector<ava::tui::SlashCommandItem> const commands = {
      ava::tui::SlashCommandItem{.command = "/help", .description = "Show help", .category = "General"},
      ava::tui::SlashCommandItem{.command = "/grep", .description = "Search files", .hint = "<text> [glob]", .category = "Files"},
      ava::tui::SlashCommandItem{.command = "/glob", .description = "List matching files", .hint = "<pattern>", .category = "Files"},
      ava::tui::SlashCommandItem{.command = "/quit", .description = "Exit", .category = "General"}};
  return commands;
}

ava::tui::SlashCommandItem const* find_slash_command_item(std::vector<ava::tui::SlashCommandItem> const& items, std::string_view command)
{
  auto const found = std::ranges::find_if(items, [command](auto const& item) { return item.command == command; });
  return found == items.end() ? nullptr : &*found;
}

bool slash_argument_completion_matches(ava::tui::SlashCommandArgumentCompletion const& completion, std::size_t argument_index, std::string_view value,
                                       std::vector<std::string> const& required_previous_args)
{
  return completion.argument_index == argument_index && completion.value == value && completion.required_previous_args == required_previous_args;
}

bool has_slash_argument_completion(ava::tui::SlashCommandItem const* item, std::size_t argument_index, std::string_view value,
                                   std::vector<std::string> const& required_previous_args)
{
  return item != nullptr && std::ranges::any_of(item->argument_completions, [&](auto const& completion) {
           return slash_argument_completion_matches(completion, argument_index, value, required_previous_args);
         });
}

std::string_view tree_action_key_bindings_json() noexcept
{
  return "{\"app.tree.foldOrUp\":\"Ctrl+O\","
         "\"app.tree.unfoldOrDown\":\"Ctrl+Y\","
         "\"app.tree.editLabel\":\"Shift+L\","
         "\"app.tree.toggleLabelTimestamp\":\"Shift+T\","
         "\"app.tree.filter.labeledOnly\":\"Ctrl+Space\","
         "\"app.tree.filter.all\":\"Ctrl+/\"}";
}

std::vector<std::string> plain_lines(std::vector<std::string> lines)
{
  for (auto& line : lines) line = strip_sgr(line);
  return lines;
}

std::string join_plain_lines(std::vector<std::string> const& lines)
{
  std::string text;
  for (auto const& line : lines) text += line + '\n';
  return text;
}

std::string join_visible_lines(std::vector<std::string> const& lines)
{
  std::string text;
  for (auto const& line : lines) text += strip_sgr(line) + '\n';
  return text;
}

std::size_t count_occurrences(std::string_view text, std::string_view needle)
{
  std::size_t count = 0;
  for (auto offset = text.find(needle); offset != std::string_view::npos; offset = text.find(needle, offset + needle.size())) ++count;
  return count;
}

}  // namespace tui_test_support

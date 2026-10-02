#pragma once

#include "tests/support/test_harness.h"
#include "ava/tui/composer.h"

#include <cstddef>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

namespace tui_test_support {

struct ScopedTerminalCapabilityProfile
{
  explicit ScopedTerminalCapabilityProfile(std::string term_program_value);

  ScopedEnvVar term;
  ScopedEnvVar term_program;
  ScopedEnvVar terminal_emulator;
  ScopedEnvVar tmux;
  ScopedEnvVar kitty_window_id;
  ScopedEnvVar ghostty_resources_dir;
  ScopedEnvVar wezterm_pane;
  ScopedEnvVar warp_session_id;
  ScopedEnvVar warp_terminal_session_uuid;
  ScopedEnvVar iterm_session_id;
  ScopedEnvVar wt_session;
  ScopedEnvVar tmux_hyperlinks;
};

std::vector<ava::tui::SlashCommandItem> const& standard_slash_commands();
ava::tui::SlashCommandItem const* find_slash_command_item(std::vector<ava::tui::SlashCommandItem> const& items, std::string_view command);
bool slash_argument_completion_matches(ava::tui::SlashCommandArgumentCompletion const& completion, std::size_t argument_index, std::string_view value,
                                       std::vector<std::string> const& required_previous_args = {});
bool has_slash_argument_completion(ava::tui::SlashCommandItem const* item, std::size_t argument_index, std::string_view value,
                                   std::vector<std::string> const& required_previous_args = {});
std::string_view tree_action_key_bindings_json() noexcept;
std::vector<std::string> plain_lines(std::vector<std::string> lines);
std::string join_plain_lines(std::vector<std::string> const& lines);
std::string join_visible_lines(std::vector<std::string> const& lines);
std::size_t count_occurrences(std::string_view text, std::string_view needle);

}  // namespace tui_test_support

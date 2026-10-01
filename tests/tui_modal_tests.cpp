#include "sys.h"
#include "tests/support/test_harness.h"
#include "tests/support/tui_test_support.h"
#include "ava/tui/composer.h"
#include "ava/tui/composer_internal.h"
#include "ava/tui/keybindings.h"
#include "ava/tui/runtime.h"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using tui_test_support::ScopedTerminalCapabilityProfile;

void run_tui_modal_tests_part_1()
{
  auto single_question = ava::tui::QuestionPromptView{
      .header = "Choose",
      .question = "Pick one",
      .options = {ava::tui::QuestionPromptOptionView{.value = "alpha", .label = "Alpha"}, ava::tui::QuestionPromptOptionView{.value = "beta", .label = "Beta"}},
      .multiple = false,
      .allow_custom = true,
      .selected_option_index = 0,
      .custom_text = ""};
  auto question_input = ava::tui::handle_question_prompt_input(single_question, ava::tui::InputEvent{.key = ava::tui::terminal::Key::Character, .character = '2'});
  expect(
      question_input.action == ava::tui::QuestionPromptInputAction::Resolve && question_input.selected_option_index == 1 && question_input.options[1].selected,
      "question prompt numeric shortcut selects and resolves a single-select option");
  question_input = ava::tui::handle_question_prompt_input(single_question, ava::tui::InputEvent{.key = ava::tui::terminal::Key::Character, .character = 'x'});
  expect(question_input.action == ava::tui::QuestionPromptInputAction::Redraw && question_input.custom_text == "x" &&
             std::ranges::none_of(question_input.options, [](ava::tui::QuestionPromptOptionView const& option) { return option.selected; }),
         "question prompt custom text edits clear single-select option state");
  single_question.custom_text = question_input.custom_text;
  question_input = ava::tui::handle_question_prompt_input(single_question, ava::tui::InputEvent{.key = ava::tui::terminal::Key::Character, .character = ' '});
  expect(question_input.action == ava::tui::QuestionPromptInputAction::Redraw && question_input.custom_text == "x ",
         "question prompt custom text can include spaces after typing starts");
  single_question.custom_text = question_input.custom_text;
  question_input = ava::tui::handle_question_prompt_input(single_question, ava::tui::InputEvent{.key = ava::tui::terminal::Key::Space});
  expect(question_input.action == ava::tui::QuestionPromptInputAction::Redraw && question_input.custom_text == "x  ",
         "question prompt semantic Space appends to existing custom text");
  single_question.custom_text = question_input.custom_text;
  question_input = ava::tui::handle_question_prompt_input(
      single_question, ava::tui::InputEvent{.key = ava::tui::terminal::Key::Character, .character = static_cast<char>(0xC3), .text = "\xC3\xA9"});
  expect(question_input.action == ava::tui::QuestionPromptInputAction::Redraw && question_input.custom_text == std::string("x  ") + "\xC3\xA9",
         "question prompt custom text preserves utf-8 input");
  single_question.custom_text = "x";
  question_input = ava::tui::handle_question_prompt_input(single_question, ava::tui::InputEvent{.key = ava::tui::terminal::Key::Backspace});
  expect(question_input.action == ava::tui::QuestionPromptInputAction::Redraw && question_input.custom_text.empty(),
         "question prompt backspace edits custom text");
  auto secret_question = ava::tui::QuestionPromptView{.header = "Connect",
                                                      .question = "Paste API key",
                                                      .options = {},
                                                      .multiple = false,
                                                      .allow_custom = true,
                                                      .secret = true,
                                                      .selected_option_index = 0,
                                                      .custom_text = ""};
  question_input = ava::tui::handle_question_prompt_input(secret_question, ava::tui::InputEvent{.key = ava::tui::terminal::Key::Enter});
  expect(question_input.action == ava::tui::QuestionPromptInputAction::Redraw && question_input.custom_text.empty(),
         "question prompt enter keeps an empty single custom answer open");

  auto copy_question = ava::tui::QuestionPromptView{.header = "Connect",
                                                    .question = "Open URL",
                                                    .options = {ava::tui::QuestionPromptOptionView{.value = "done", .label = "Done"},
                                                                ava::tui::QuestionPromptOptionView{.value = "copy:https://auth.openai.com", .label = "C Copy"}},
                                                    .multiple = false,
                                                    .allow_custom = false,
                                                    .selected_option_index = 0,
                                                    .custom_text = ""};
  question_input = ava::tui::handle_question_prompt_input(copy_question, ava::tui::InputEvent{.key = ava::tui::terminal::Key::Character, .character = 'c'});
  expect(question_input.action == ava::tui::QuestionPromptInputAction::Copy && question_input.copy_text == "https://auth.openai.com" &&
             std::ranges::none_of(question_input.options, [](ava::tui::QuestionPromptOptionView const& option) { return option.selected; }),
         "question prompt copy shortcut copies without resolving a single-select option");
  question_input = ava::tui::handle_question_prompt_input(copy_question, ava::tui::InputEvent{.key = ava::tui::terminal::Key::Enter});
  expect(question_input.action == ava::tui::QuestionPromptInputAction::Resolve && question_input.options[0].selected,
         "question prompt enter confirms the selected non-copy option");

  auto multi_question = ava::tui::QuestionPromptView{
      .header = "Choose",
      .question = "Pick many",
      .options = {ava::tui::QuestionPromptOptionView{.value = "read", .label = "Read"}, ava::tui::QuestionPromptOptionView{.value = "grep", .label = "Grep"}},
      .multiple = true,
      .allow_custom = true,
      .selected_option_index = 0,
      .custom_text = ""};
  question_input = ava::tui::handle_question_prompt_input(multi_question, ava::tui::InputEvent{.key = ava::tui::terminal::Key::Character, .character = ' '});
  expect(question_input.action == ava::tui::QuestionPromptInputAction::Redraw && question_input.options[0].selected,
         "question prompt space toggles selected multi-select option");
  multi_question.options = question_input.options;
  question_input = ava::tui::handle_question_prompt_input(multi_question, ava::tui::InputEvent{.key = ava::tui::terminal::Key::Space});
  expect(question_input.action == ava::tui::QuestionPromptInputAction::Redraw && !question_input.options[0].selected,
         "question prompt semantic Space toggles selected multi-select option");
  multi_question.options = question_input.options;
  question_input = ava::tui::handle_question_prompt_input(multi_question, ava::tui::InputEvent{.key = ava::tui::terminal::Key::Character, .character = '2'});
  expect(question_input.action == ava::tui::QuestionPromptInputAction::Redraw && !question_input.options[0].selected && question_input.options[1].selected,
         "question prompt numeric shortcut toggles multi-select options without resolving");
  multi_question.options = question_input.options;
  question_input = ava::tui::handle_question_prompt_input(multi_question, ava::tui::InputEvent{.key = ava::tui::terminal::Key::Enter});
  expect(question_input.action == ava::tui::QuestionPromptInputAction::Resolve && !question_input.options[0].selected && question_input.options[1].selected,
         "question prompt enter resolves current multi-select choices");
  question_input = ava::tui::handle_question_prompt_input(multi_question, ava::tui::InputEvent{.key = ava::tui::terminal::Key::Escape});
  expect(question_input.action == ava::tui::QuestionPromptInputAction::Cancel, "question prompt escape cancels safely");
  auto empty_multi_question = multi_question;
  for (auto& option : empty_multi_question.options) option.selected = false;
  empty_multi_question.custom_text.clear();
  auto empty_multi_answer = ava::tui::question_answer_from_prompt_view(empty_multi_question);
  expect(empty_multi_answer && empty_multi_answer->selected_options.empty() && empty_multi_answer->custom_text.empty(),
         "question prompt accepts an empty multi-select answer");
}
void run_tui_modal_tests_part_2()
{
  auto const& slash_commands = tui_test_support::standard_slash_commands();
  std::vector<ava::tui::SlashCommandItem> const disabled_slash_commands = {
      ava::tui::SlashCommandItem{.command = "/models",
                                 .description = "Select model",
                                 .category = "Planned",
                                 .aliases = {"/model"},
                                 .key_display = "Ctrl+M",
                                 .enabled = false,
                                 .disabled_reason = "model switching is not implemented"},
      ava::tui::SlashCommandItem{.command = "/mode", .description = "Toggle mode", .category = "General"}};
  auto const alias_matches = ava::tui::filter_slash_commands("/model", disabled_slash_commands);
  expect(alias_matches.size() == 1 && alias_matches.front().command == "/models", "tui slash palette filters aliases as well as primary command names");
  expect(ava::tui::slash_palette_visible("/model", disabled_slash_commands), "tui slash palette keeps disabled exact alias matches visible");
  auto const disabled_reason = ava::tui::slash_command_selection_disabled_reason("/model", disabled_slash_commands, 0);
  expect(disabled_reason && disabled_reason->find("not implemented") != std::string::npos, "tui slash selection exposes disabled command explanations");

  auto const disabled_palette = ava::tui::render_composer(ava::tui::ComposerSnapshot{.mode = "build",
                                                                                     .provider = "openai",
                                                                                     .model = "gpt-5.5",
                                                                                     .session_id = "session_test",
                                                                                     .input = "/model",
                                                                                     .status = "ready",
                                                                                     .transcript = {},
                                                                                     .slash_commands = disabled_slash_commands,
                                                                                     .selected_slash_command_index = 0,
                                                                                     .width = 140,
                                                                                     .height = 10});
  expect(std::ranges::any_of(disabled_palette,
                             [](std::string const& line) {
                               auto const visible = strip_sgr(line);
                               return visible.find("/models (/model)") != std::string::npos && visible.find("Ctrl+M") != std::string::npos &&
                                      visible.find("disabled: model switching is not implemented") != std::string::npos;
                             }),
         "tui slash palette renders aliases, key displays, and disabled reasons");

  auto const palette = ava::tui::render_composer(ava::tui::ComposerSnapshot{.mode = "build",
                                                                            .provider = "openai",
                                                                            .model = "gpt-5.5",
                                                                            .session_id = "session_test",
                                                                            .input = "/g",
                                                                            .status = "ready",
                                                                            .transcript = {},
                                                                            .slash_commands = slash_commands,
                                                                            .selected_slash_command_index = 1,
                                                                            .width = 80,
                                                                            .height = 12});
  expect(std::ranges::any_of(palette, [](std::string const& line) { return line.find("commands matching /g") == std::string::npos; }) &&
             std::ranges::any_of(palette,
                                 [](std::string const& line) {
                                   return line.find("/grep") != std::string::npos && line.find("Files") != std::string::npos &&
                                          line.find("Search files") != std::string::npos;
                                 }) &&
             std::ranges::any_of(
                 palette,
                 [](std::string const& line) { return line.find("/glob") != std::string::npos && line.find("List matching files") != std::string::npos; }) &&
             std::ranges::any_of(palette,
                                 [](std::string const& line) {
                                   auto const visible = strip_sgr(line);
                                   return visible.find("› /glob") != std::string::npos && visible.find("selected") == std::string::npos &&
                                          visible.find("(2/2)") == std::string::npos;
                                 }) &&
             std::ranges::any_of(palette, [](std::string const& line) { return line.find("\x1b[1m/glob") != std::string::npos; }) &&
             std::ranges::any_of(palette,
                                 [](std::string const& line) {
                                   return line.find("/glob") != std::string::npos && line.find("\x1b[48;2;26;31;46m") != std::string::npos;
                                 }) &&
             std::ranges::any_of(palette,
                                 [](std::string const& line) {
                                   return strip_sgr(line).starts_with("│  /g") && line.find("\x1b[49m") != std::string::npos &&
                                          line.find("\x1b[48;2;26;31;46m") == std::string::npos;
                                 }) &&
             std::ranges::none_of(palette, [](std::string const& line) { return line.find("\x1b[7m") != std::string::npos; }) &&
             std::ranges::none_of(palette, [](std::string const& line) { return line.find("/help") != std::string::npos; }),
         "tui renders filtered slash-command palette with elevated composer surface while the draft dock stays on the screen background");
  auto const suppressed_palette = ava::tui::render_composer(ava::tui::ComposerSnapshot{.mode = "build",
                                                                                       .provider = "openai",
                                                                                       .model = "gpt-5.5",
                                                                                       .session_id = "session_test",
                                                                                       .input = "/g",
                                                                                       .status = "ready",
                                                                                       .transcript = {},
                                                                                       .slash_commands = slash_commands,
                                                                                       .slash_palette_suppressed = true,
                                                                                       .width = 80,
                                                                                       .height = 12});
  expect(std::ranges::none_of(suppressed_palette, [](std::string const& line) { return strip_sgr(line).find("/grep") != std::string::npos; }) &&
             std::ranges::any_of(suppressed_palette, [](std::string const& line) { return strip_sgr(line).find("│  /g") != std::string::npos; }),
         "tui can dismiss slash autocomplete without clearing the draft input");
  auto const clicked_palette_index = ava::tui::slash_palette_selection_for_screen_position(ava::tui::ComposerSnapshot{.mode = "build",
                                                                                                                      .provider = "openai",
                                                                                                                      .model = "gpt-5.5",
                                                                                                                      .session_id = "session_test",
                                                                                                                      .input = "/g",
                                                                                                                      .status = "ready",
                                                                                                                      .transcript = {},
                                                                                                                      .slash_commands = slash_commands,
                                                                                                                      .selected_slash_command_index = 1,
                                                                                                                      .width = 80,
                                                                                                                      .height = 12},
                                                                                           10, 1);
  expect(clicked_palette_index && *clicked_palette_index == 1, "tui maps slash palette screen rows back to selectable commands for clicks");
  auto const suppressed_clicked_palette_index =
      ava::tui::slash_palette_selection_for_screen_position(ava::tui::ComposerSnapshot{.mode = "build",
                                                                                       .provider = "openai",
                                                                                       .model = "gpt-5.5",
                                                                                       .session_id = "session_test",
                                                                                       .input = "/g",
                                                                                       .status = "ready",
                                                                                       .transcript = {},
                                                                                       .slash_commands = slash_commands,
                                                                                       .selected_slash_command_index = 1,
                                                                                       .slash_palette_suppressed = true,
                                                                                       .width = 80,
                                                                                       .height = 12},
                                                            9, 1);
  expect(!suppressed_clicked_palette_index, "tui ignores slash palette click mapping after autocomplete is dismissed");
  auto const blocked_question_palette_index = ava::tui::slash_palette_selection_for_screen_position(
      ava::tui::ComposerSnapshot{.mode = "build",
                                 .provider = "openai",
                                 .model = "gpt-5.5",
                                 .session_id = "session_test",
                                 .input = "/g",
                                 .status = "ready",
                                 .transcript = {},
                                 .slash_commands = slash_commands,
                                 .question_prompt = ava::tui::QuestionPromptView{.header = "Question",
                                                                                 .question = "Choose",
                                                                                 .options = {},
                                                                                 .multiple = false,
                                                                                 .allow_custom = false,
                                                                                 .selected_option_index = 0,
                                                                                 .custom_text = ""},
                                 .selected_slash_command_index = 1,
                                 .width = 80,
                                 .height = 12},
      9, 1);
  expect(!blocked_question_palette_index, "tui ignores slash palette click mapping while a question prompt is active");

  std::vector<ava::tui::SlashCommandItem> many_slash_commands;
  for (int index = 0; index < 8; ++index)
  {
    many_slash_commands.push_back(ava::tui::SlashCommandItem{.command = "/item" + std::to_string(index), .description = "Command " + std::to_string(index)});
  }
  auto const tiny_palette = ava::tui::render_composer(ava::tui::ComposerSnapshot{.mode = "build",
                                                                                 .provider = "openai",
                                                                                 .model = "gpt-5.5",
                                                                                 .session_id = "session_test",
                                                                                 .input = "/",
                                                                                 .status = "ready",
                                                                                 .transcript = {},
                                                                                 .slash_commands = many_slash_commands,
                                                                                 .selected_slash_command_index = 6,
                                                                                 .width = 80,
                                                                                 .height = 8});
  expect(std::ranges::any_of(tiny_palette, [](std::string const& line) { return strip_sgr(line).find("› /item6") != std::string::npos; }) &&
             std::ranges::none_of(tiny_palette, [](std::string const& line) { return line.find("/item0") != std::string::npos; }),
         "tui keeps selected slash palette item visible when height is tight");
  auto const first_scrolled_palette_click =
      ava::tui::slash_palette_selection_for_screen_position(ava::tui::ComposerSnapshot{.mode = "build",
                                                                                       .provider = "openai",
                                                                                       .model = "gpt-5.5",
                                                                                       .session_id = "session_test",
                                                                                       .input = "/",
                                                                                       .status = "ready",
                                                                                       .transcript = {},
                                                                                       .slash_commands = many_slash_commands,
                                                                                       .selected_slash_command_index = 6,
                                                                                       .width = 80,
                                                                                       .height = 8},
                                                            1, 1);
  auto const selected_scrolled_palette_click =
      ava::tui::slash_palette_selection_for_screen_position(ava::tui::ComposerSnapshot{.mode = "build",
                                                                                       .provider = "openai",
                                                                                       .model = "gpt-5.5",
                                                                                       .session_id = "session_test",
                                                                                       .input = "/",
                                                                                       .status = "ready",
                                                                                       .transcript = {},
                                                                                       .slash_commands = many_slash_commands,
                                                                                       .selected_slash_command_index = 6,
                                                                                       .width = 80,
                                                                                       .height = 8},
                                                            6, 1);
  auto const outside_scrolled_palette_click =
      ava::tui::slash_palette_selection_for_screen_position(ava::tui::ComposerSnapshot{.mode = "build",
                                                                                       .provider = "openai",
                                                                                       .model = "gpt-5.5",
                                                                                       .session_id = "session_test",
                                                                                       .input = "/",
                                                                                       .status = "ready",
                                                                                       .transcript = {},
                                                                                       .slash_commands = many_slash_commands,
                                                                                       .selected_slash_command_index = 6,
                                                                                       .width = 80,
                                                                                       .height = 8},
                                                            7, 1);
  expect(first_scrolled_palette_click && *first_scrolled_palette_click == 1 && selected_scrolled_palette_click && *selected_scrolled_palette_click == 6 &&
             !outside_scrolled_palette_click,
         "tui maps slash palette click rows through a scrolled visible window and ignores outside rows");

  auto const starved_palette =
      ava::tui::render_composer(ava::tui::ComposerSnapshot{.mode = "build",
                                                           .provider = "openai",
                                                           .model = "gpt-5.5",
                                                           .session_id = "session_test",
                                                           .input = "/",
                                                           .status = "ready",
                                                           .transcript = {ava::tui::TranscriptItem{.label = "ava", .text = "must not leak"}},
                                                           .slash_commands = many_slash_commands,
                                                           .selected_slash_command_index = 4,
                                                           .width = 40,
                                                           .height = 8});
  expect(starved_palette.size() == 8 &&
             std::ranges::any_of(starved_palette, [](std::string const& line) { return strip_sgr(line).find("› /item4") != std::string::npos; }) &&
             std::ranges::none_of(starved_palette, [](std::string const& line) { return strip_sgr(line).find("must not leak") != std::string::npos; }),
         "tui keeps the bottom composer fixed when the slash palette exhausts transcript height");

  auto const no_match_palette = ava::tui::render_composer(ava::tui::ComposerSnapshot{.mode = "build",
                                                                                     .provider = "openai",
                                                                                     .model = "gpt-5.5",
                                                                                     .session_id = "session_test",
                                                                                     .input = "/zz",
                                                                                     .status = "ready",
                                                                                     .transcript = {},
                                                                                     .slash_commands = slash_commands,
                                                                                     .width = 80,
                                                                                     .height = 12});
  expect(std::ranges::any_of(no_match_palette, [](std::string const& line) { return strip_sgr(line).find("no commands match /zz") != std::string::npos; }),
         "tui slash-command palette renders deterministic empty state");
}
void run_tui_modal_tests_part_3()
{
  auto const question_frame = ava::tui::render_composer(ava::tui::ComposerSnapshot{
      .mode = "build",
      .provider = "openai",
      .model = "gpt-5.5",
      .session_id = "session_test",
      .input = "do not focus composer",
      .status = "question required",
      .transcript = {},
      .question_prompt =
          ava::tui::QuestionPromptView{.header = "Choose tools",
                                       .question = "Pick the tools to run",
                                       .options = {ava::tui::QuestionPromptOptionView{.value = "read", .label = "Read files"},
                                                   ava::tui::QuestionPromptOptionView{.value = "grep", .label = "Search text", .selected = true}},
                                       .multiple = true,
                                       .allow_custom = true,
                                       .selected_option_index = 1,
                                       .custom_text = "explain"},
      .width = 64,
      .height = 12});
  expect(
      question_frame.size() == 12 &&
          std::ranges::any_of(question_frame, [](std::string const& line) { return strip_sgr(line).find("│  do not focus composer") != std::string::npos; }) &&
          std::ranges::any_of(question_frame, [](std::string const& line) { return strip_sgr(line).find("? Choose tools") != std::string::npos; }) &&
          std::ranges::any_of(question_frame, [](std::string const& line) { return strip_sgr(line).find("› 2. ✓ Search text") != std::string::npos; }) &&
          std::ranges::any_of(question_frame, [](std::string const& line) { return strip_sgr(line).find("Custom: explain") != std::string::npos; }) &&
          std::ranges::any_of(question_frame, [](std::string const& line) { return line.find(ava::tui::detail::kSgrQuestionBg) != std::string::npos; }) &&
          std::ranges::none_of(question_frame, [](std::string const& line) { return line.find("\x1b[7m") != std::string::npos; }),
      "tui composer frame renders multi-select questions on a distinct surface without reverse video");

  auto wrapped_question_snapshot = ava::tui::ComposerSnapshot{
      .mode = "build",
      .provider = "openai",
      .model = "gpt-5.5",
      .session_id = "session_test",
      .input = "",
      .status = "question required",
      .transcript = {},
      .question_prompt = ava::tui::QuestionPromptView{.header = "Decision",
                                                      .question = "Choose whether this intentionally long question should wrap across multiple bounded rows",
                                                      .options = {ava::tui::QuestionPromptOptionView{.value = "yes", .label = "Yes"},
                                                                  ava::tui::QuestionPromptOptionView{.value = "no", .label = "No"}},
                                                      .selected_option_index = 1,
                                                      .custom_text = {}},
      .width = 40,
      .height = 10};
  auto const wrapped_question_frame = ava::tui::render_composer(wrapped_question_snapshot);
  expect(std::ranges::any_of(wrapped_question_frame, [](std::string const& line) { return strip_sgr(line).find("intentionall") != std::string::npos; }) &&
             std::ranges::any_of(wrapped_question_frame,
                                 [](std::string const& line) { return strip_sgr(line).find("question should wrap acro") != std::string::npos; }) &&
             std::ranges::any_of(wrapped_question_frame, [](std::string const& line) { return strip_sgr(line).find("› 2. No") != std::string::npos; }) &&
             std::ranges::any_of(wrapped_question_frame, [](std::string const& line) { return strip_sgr(line).find("Esc") != std::string::npos; }),
         "tui docked questions wrap within a bounded budget while preserving the selected option and cancel action");

  auto single_click_snapshot = ava::tui::ComposerSnapshot{
      .mode = "build",
      .provider = "openai",
      .model = "gpt-5.5",
      .session_id = "session_test",
      .input = "",
      .status = "question required",
      .transcript = {},
      .question_prompt = ava::tui::QuestionPromptView{.header = "Pick one",
                                                      .question = "Choose an option",
                                                      .options = {ava::tui::QuestionPromptOptionView{.value = "alpha", .label = "Alpha"},
                                                                  ava::tui::QuestionPromptOptionView{.value = "beta", .label = "Beta"}},
                                                      .allow_custom = true,
                                                      .selected_option_index = 0,
                                                      .custom_text = {}},
      .width = 80,
      .height = 24};
  auto const single_click_frame = ava::tui::render_composer(single_click_snapshot);
  auto const single_beta_row = static_cast<std::size_t>(
      std::ranges::find_if(single_click_frame, [](std::string const& line) { return strip_sgr(line).find("2. Beta") != std::string::npos; }) -
      single_click_frame.begin());
  auto const single_click = ava::tui::question_option_for_screen_position(single_click_snapshot, single_beta_row + 1, 4);
  expect(single_click && *single_click == 1, "question dock hit testing maps a visible single-select option at 80x24");
  auto centered_dock_click_snapshot = single_click_snapshot;
  centered_dock_click_snapshot.width = 160;
  auto const centered_dock_click_frame = ava::tui::render_composer(centered_dock_click_snapshot);
  auto const centered_dock_beta_row = static_cast<std::size_t>(
      std::ranges::find_if(centered_dock_click_frame, [](std::string const& line) { return strip_sgr(line).find("2. Beta") != std::string::npos; }) -
      centered_dock_click_frame.begin());
  expect(ava::tui::question_option_for_screen_position(centered_dock_click_snapshot, centered_dock_beta_row + 1, 21) == 1 &&
             ava::tui::question_option_for_screen_position(centered_dock_click_snapshot, centered_dock_beta_row + 1, 140) == 1 &&
             !ava::tui::question_option_for_screen_position(centered_dock_click_snapshot, centered_dock_beta_row + 1, 20) &&
             !ava::tui::question_option_for_screen_position(centered_dock_click_snapshot, centered_dock_beta_row + 1, 141),
         "question dock uses centered content geometry and rejects both exact gutters");
  auto clicked_single_prompt = *single_click_snapshot.question_prompt;
  clicked_single_prompt.options[0].selected = true;
  clicked_single_prompt.custom_text = "typed custom answer";
  auto const clicked_single = ava::tui::activate_question_option(clicked_single_prompt, *single_click);
  expect(clicked_single.action == ava::tui::QuestionPromptInputAction::Resolve && clicked_single.selected_option_index == 1 &&
             !clicked_single.options[0].selected && clicked_single.options[1].selected && clicked_single.custom_text.empty(),
         "question dock mouse activation authoritatively selects a single option over existing custom text");
  expect(!ava::tui::question_option_for_screen_position(single_click_snapshot, 1, 1) &&
             !ava::tui::question_option_for_screen_position(single_click_snapshot, single_beta_row + 1, 81),
         "question dock hit testing rejects transcript and outside columns");
  auto retained_drawer_dock = single_click_snapshot;
  retained_drawer_dock.sidebar = ava::tui::SidebarSnapshot{.session_id = "session_test"};
  retained_drawer_dock.sidebar_drawer_visible = true;
  auto const retained_drawer_dock_frame = ava::tui::render_composer(retained_drawer_dock);
  auto const retained_drawer_beta_row = static_cast<std::size_t>(
      std::ranges::find_if(retained_drawer_dock_frame, [](std::string const& line) { return strip_sgr(line).find("2. Beta") != std::string::npos; }) -
      retained_drawer_dock_frame.begin());
  expect(ava::tui::question_option_for_screen_position(retained_drawer_dock, retained_drawer_beta_row + 1, 4) == 1 &&
             !ava::tui::question_option_for_screen_position(retained_drawer_dock, 1, 4) &&
             !ava::tui::question_option_for_screen_position(retained_drawer_dock, retained_drawer_beta_row + 1, retained_drawer_dock.width + 1),
         "question dock hit testing supersedes a retained drawer flag while rejecting outside rows and columns");

  auto custom_cursor_snapshot = single_click_snapshot;
  custom_cursor_snapshot.question_prompt->custom_text = "custom answer";
  auto const custom_cursor_frame = ava::tui::render_composer(custom_cursor_snapshot);
  expect(
      std::ranges::any_of(custom_cursor_frame, [](std::string const& line) { return strip_sgr(line).find("› Custom: custom answer") != std::string::npos; }) &&
          std::ranges::none_of(custom_cursor_frame,
                               [](std::string const& line) {
                                 auto const text = strip_sgr(line);
                                 return text.find("›") != std::string::npos &&
                                        (text.find("1. Alpha") != std::string::npos || text.find("2. Beta") != std::string::npos);
                               }),
      "nonempty single-select custom text owns the question cursor marker");

  auto multi_click_snapshot = single_click_snapshot;
  multi_click_snapshot.question_prompt->multiple = true;
  multi_click_snapshot.question_prompt->allow_custom = true;
  multi_click_snapshot.question_prompt->selected_option_index = 0;
  multi_click_snapshot.question_prompt->custom_text = "keep these notes";
  multi_click_snapshot.width = 100;
  multi_click_snapshot.height = 12;
  auto const multi_click_frame = ava::tui::render_composer(multi_click_snapshot);
  auto const multi_beta_row = static_cast<std::size_t>(
      std::ranges::find_if(multi_click_frame, [](std::string const& line) { return strip_sgr(line).find("2. · Beta") != std::string::npos; }) -
      multi_click_frame.begin());
  auto const multi_click = ava::tui::question_option_for_screen_position(multi_click_snapshot, multi_beta_row + 1, 4);
  auto const clicked_multi = ava::tui::activate_question_option(*multi_click_snapshot.question_prompt, multi_click.value_or(0));
  expect(multi_click && *multi_click == 1 && clicked_multi.action == ava::tui::QuestionPromptInputAction::Redraw && clicked_multi.options[1].selected &&
             clicked_multi.custom_text == "keep these notes",
         "question dock hit testing stays visible at 100x12 and mouse activation toggles multi-select while preserving custom text");

  auto copy_click_snapshot = single_click_snapshot;
  copy_click_snapshot.question_prompt->options = {ava::tui::QuestionPromptOptionView{.value = "copy:https://example.test/path?q=exact", .label = "Copy URL"}};
  copy_click_snapshot.question_prompt->custom_text = "unrelated custom text";
  auto const copy_click_frame = ava::tui::render_composer(copy_click_snapshot);
  auto const copy_row = static_cast<std::size_t>(
      std::ranges::find_if(copy_click_frame, [](std::string const& line) { return strip_sgr(line).find("1. Copy URL") != std::string::npos; }) -
      copy_click_frame.begin());
  auto const copy_click = ava::tui::question_option_for_screen_position(copy_click_snapshot, copy_row + 1, 4);
  auto const clicked_copy = ava::tui::activate_question_option(*copy_click_snapshot.question_prompt, copy_click.value_or(1));
  auto const invalid_click = ava::tui::activate_question_option(*copy_click_snapshot.question_prompt, 9);
  expect(copy_click && *copy_click == 0 && clicked_copy.action == ava::tui::QuestionPromptInputAction::Copy &&
             clicked_copy.copy_text == "https://example.test/path?q=exact" && clicked_copy.custom_text == "unrelated custom text" &&
             invalid_click.action == ava::tui::QuestionPromptInputAction::None && invalid_click.selected_option_index == 0 &&
             invalid_click.options.size() == 1 && !invalid_click.options.front().selected &&
             invalid_click.custom_text == copy_click_snapshot.question_prompt->custom_text,
         "question option click mapping copies the exact payload despite custom text and rejects an invalid option index");

  std::vector<ava::tui::QuestionPromptOptionView> long_question_options;
  for (std::size_t index = 0; index < 12; ++index)
  {
    long_question_options.push_back(ava::tui::QuestionPromptOptionView{.value = "option-" + std::to_string(index), .label = "Option " + std::to_string(index)});
  }
  auto question_navigation_prompt = ava::tui::QuestionPromptView{
      .header = "Choose one", .question = "Pick an option", .options = long_question_options, .selected_option_index = 0, .custom_text = {}};
  auto question_navigation = ava::tui::handle_question_prompt_input(question_navigation_prompt, ava::tui::InputEvent{.key = ava::tui::terminal::Key::ArrowDown});
  expect(question_navigation.action == ava::tui::QuestionPromptInputAction::Redraw && question_navigation.selected_option_index == 1,
         "question modal down arrow advances the selected option");
  question_navigation_prompt.selected_option_index = question_navigation.selected_option_index;
  question_navigation = ava::tui::handle_question_prompt_input(question_navigation_prompt, ava::tui::InputEvent{.key = ava::tui::terminal::Key::PageDown});
  expect(question_navigation.action == ava::tui::QuestionPromptInputAction::Redraw && question_navigation.selected_option_index == 6,
         "question modal PageDown advances by a viewport-sized page");
  question_navigation_prompt.selected_option_index = question_navigation.selected_option_index;
  question_navigation = ava::tui::handle_question_prompt_input(question_navigation_prompt, ava::tui::InputEvent{.key = ava::tui::terminal::Key::End});
  expect(question_navigation.action == ava::tui::QuestionPromptInputAction::Redraw && question_navigation.selected_option_index == 11,
         "question modal End moves to the final option");
  question_navigation_prompt.selected_option_index = question_navigation.selected_option_index;
  question_navigation = ava::tui::handle_question_prompt_input(question_navigation_prompt, ava::tui::InputEvent{.key = ava::tui::terminal::Key::MouseWheelUp});
  expect(question_navigation.action == ava::tui::QuestionPromptInputAction::Redraw && question_navigation.selected_option_index == 10,
         "question modal mouse wheel uses the same selection path as arrow navigation");

  auto const long_question_dock = ava::tui::render_composer(ava::tui::ComposerSnapshot{
      .mode = "build",
      .provider = "openai",
      .model = "gpt-5.5",
      .session_id = "session_test",
      .input = "",
      .status = "question required",
      .transcript = {},
      .question_prompt =
          ava::tui::QuestionPromptView{
              .header = "Choose one", .question = "Pick an option", .options = long_question_options, .selected_option_index = 9, .custom_text = {}},
      .width = 64,
      .height = 12});
  expect(std::ranges::any_of(long_question_dock, [](std::string const& line) { return strip_sgr(line).find("10. Option 9") != std::string::npos; }),
         "question dock scrolls its option window to keep the selected row visible");
  bool every_question_dock_selection_visible = true;
  for (std::size_t index = 0; index < long_question_options.size(); ++index)
  {
    auto dock_snapshot = ava::tui::ComposerSnapshot{};
    dock_snapshot.mode = "build";
    dock_snapshot.provider = "openai";
    dock_snapshot.model = "gpt-5.5";
    dock_snapshot.session_id = "session_test";
    dock_snapshot.question_prompt = question_navigation_prompt;
    dock_snapshot.question_prompt->selected_option_index = index;
    dock_snapshot.width = 64;
    dock_snapshot.height = 12;
    auto const frame = ava::tui::render_composer(dock_snapshot);
    auto const wanted = std::to_string(index + 1) + ". Option " + std::to_string(index);
    every_question_dock_selection_visible &=
        std::ranges::any_of(frame, [&](std::string const& line) { return strip_sgr(line).find(wanted) != std::string::npos; });
  }
  expect(every_question_dock_selection_visible, "question dock viewport follows every selected option beyond the initial screen");
  for (std::size_t height = 8; height <= 11; ++height)
  {
    auto compact_dock = ava::tui::ComposerSnapshot{};
    compact_dock.mode = "build";
    compact_dock.provider = "openai";
    compact_dock.model = "gpt-5.5";
    compact_dock.session_id = "session_test";
    compact_dock.input = "draft one\ndraft two\ndraft three\ndraft four\ndraft five\ndraft six";
    compact_dock.status = "question required";
    compact_dock.question_prompt = question_navigation_prompt;
    compact_dock.question_prompt->selected_option_index = 9;
    compact_dock.width = 64;
    compact_dock.height = height;
    auto const frame = ava::tui::render_composer(compact_dock);
    expect(std::ranges::any_of(frame, [](std::string const& line) { return strip_sgr(line).find("10. Option 9") != std::string::npos; }),
           "minimum-height question dock keeps the selected option visible above a wrapped composer draft");
  }

  auto const secret_question_frame =
      ava::tui::render_composer(ava::tui::ComposerSnapshot{.mode = "build",
                                                           .provider = "openai",
                                                           .model = "gpt-5.5",
                                                           .session_id = "session_test",
                                                           .input = "",
                                                           .status = "question required",
                                                           .transcript = {},
                                                           .question_prompt = ava::tui::QuestionPromptView{.header = "Connect",
                                                                                                           .question = "Paste API key",
                                                                                                           .options = {},
                                                                                                           .multiple = false,
                                                                                                           .allow_custom = true,
                                                                                                           .secret = true,
                                                                                                           .custom_text = "sk-visible-secret"},
                                                           .width = 64,
                                                           .height = 9});
  expect(std::ranges::none_of(secret_question_frame, [](std::string const& line) { return strip_sgr(line).find("sk-visible-secret") != std::string::npos; }) &&
             std::ranges::any_of(secret_question_frame,
                                 [](std::string const& line) { return strip_sgr(line).find("Custom: *****************") != std::string::npos; }),
         "tui question dock masks secret custom input");

  auto const modal_question_frame = ava::tui::render_composer(ava::tui::ComposerSnapshot{
      .mode = "build",
      .provider = "",
      .model = "gpt-5.5",
      .session_id = "session_test",
      .input = "composer stays behind modal",
      .status = "question required",
      .transcript = {ava::tui::TranscriptItem{.label = "ava", .text = "background transcript"}},
      .question_prompt = ava::tui::QuestionPromptView{.header = "Connect a provider",
                                                      .question = "Select provider",
                                                      .options = {ava::tui::QuestionPromptOptionView{.value = "openai", .label = "OpenAI"},
                                                                  ava::tui::QuestionPromptOptionView{.value = "anthropic", .label = "Anthropic"}},
                                                      .multiple = false,
                                                      .allow_custom = true,
                                                      .secret = false,
                                                      .modal = true,
                                                      .searchable = true,
                                                      .selected_option_index = 1,
                                                      .custom_text = "anth"},
      .width = 80,
      .height = 16});
  expect(
      modal_question_frame.size() == 16 &&
          std::ranges::any_of(modal_question_frame, [](std::string const& line) { return strip_sgr(line).find("Connect a provider") != std::string::npos; }) &&
          std::ranges::any_of(modal_question_frame, [](std::string const& line) { return strip_sgr(line).find("Search: anth") != std::string::npos; }) &&
          std::ranges::any_of(modal_question_frame, [](std::string const& line) { return strip_sgr(line).find("Anthropic") != std::string::npos; }) &&
          std::ranges::none_of(modal_question_frame, [](std::string const& line) { return strip_sgr(line).find("OpenAI") != std::string::npos; }),
      "tui renders searchable provider questions as centered filtered modals");
  auto searchable_click_prompt = ava::tui::QuestionPromptView{.header = "Connect a provider",
                                                              .question = "Select provider",
                                                              .options = {ava::tui::QuestionPromptOptionView{.value = "openai", .label = "OpenAI"},
                                                                          ava::tui::QuestionPromptOptionView{.value = "anthropic", .label = "Anthropic"}},
                                                              .allow_custom = true,
                                                              .searchable = true,
                                                              .selected_option_index = 0,
                                                              .custom_text = "anth"};
  auto const searchable_click = ava::tui::activate_question_option(searchable_click_prompt, 1);
  expect(searchable_click.action == ava::tui::QuestionPromptInputAction::Resolve && searchable_click.selected_option_index == 1 &&
             searchable_click.options[1].selected && searchable_click.custom_text.empty(),
         "searchable question mouse activation selects the clicked visible option instead of resolving its query as custom text");

  auto modal_click_snapshot = ava::tui::ComposerSnapshot{
      .mode = "build",
      .provider = "openai",
      .model = "gpt-5.5",
      .session_id = "session_test",
      .input = "",
      .status = "question required",
      .transcript = {},
      .question_prompt = ava::tui::QuestionPromptView{.header = "Pick one",
                                                      .question = "Choose an option",
                                                      .options = {ava::tui::QuestionPromptOptionView{.value = "alpha", .label = "Alpha"},
                                                                  ava::tui::QuestionPromptOptionView{.value = "beta", .label = "Beta"}},
                                                      .modal = true,
                                                      .selected_option_index = 1,
                                                      .custom_text = {}},
      .width = 80,
      .height = 24};
  auto const modal_click_frame = ava::tui::render_composer(modal_click_snapshot);
  auto const modal_beta_row = static_cast<std::size_t>(
      std::ranges::find_if(modal_click_frame, [](std::string const& line) { return strip_sgr(line).find("› 2. Beta") != std::string::npos; }) -
      modal_click_frame.begin());
  auto const modal_click = ava::tui::question_option_for_screen_position(modal_click_snapshot, modal_beta_row + 1, 20);
  expect(modal_click && *modal_click == 1 && !ava::tui::question_option_for_screen_position(modal_click_snapshot, 1, 20) &&
             !ava::tui::question_option_for_screen_position(modal_click_snapshot, modal_click_snapshot.height, 20) &&
             std::ranges::none_of(modal_click_frame, [](std::string const& line) { return line.find("\x1b[7m") != std::string::npos; }),
         "question modal hit testing maps only its shared option rows and uses no reverse-video selection bar");
  auto retained_drawer_modal = modal_click_snapshot;
  retained_drawer_modal.sidebar = ava::tui::SidebarSnapshot{.session_id = "session_test"};
  retained_drawer_modal.sidebar_drawer_visible = true;
  auto const retained_drawer_modal_frame = ava::tui::render_composer(retained_drawer_modal);
  auto const retained_drawer_modal_beta_row = static_cast<std::size_t>(
      std::ranges::find_if(retained_drawer_modal_frame, [](std::string const& line) { return strip_sgr(line).find("› 2. Beta") != std::string::npos; }) -
      retained_drawer_modal_frame.begin());
  expect(ava::tui::question_option_for_screen_position(retained_drawer_modal, retained_drawer_modal_beta_row + 1, 20) == 1 &&
             !ava::tui::question_option_for_screen_position(retained_drawer_modal, 1, 20),
         "question modal hit testing supersedes a retained drawer flag while rejecting rows outside the modal");

  auto rail_modal_click = modal_click_snapshot;
  rail_modal_click.width = 176;
  rail_modal_click.sidebar = ava::tui::SidebarSnapshot{
      .activity = {ava::tui::SidebarActivityItem{.id = "active", .label = "rail-must-not-render", .status = ava::tui::ToolTimelineStatus::Running}},
      .session_id = "session_test",
      .mode = "build",
      .provider = "openai",
      .model = "gpt-5.5"};
  auto const rail_modal_frame = ava::tui::render_composer(rail_modal_click);
  auto const rail_modal_beta_row = static_cast<std::size_t>(
      std::ranges::find_if(rail_modal_frame, [](std::string const& line) { return strip_sgr(line).find("› 2. Beta") != std::string::npos; }) -
      rail_modal_frame.begin());
  auto modal_without_rail = rail_modal_click;
  modal_without_rail.sidebar = std::nullopt;
  expect(ava::tui::composer_canvas_layout(rail_modal_click).content_width == 120 && ava::tui::composer_canvas_layout(rail_modal_click).left == 28 &&
             rail_modal_frame == ava::tui::render_composer(modal_without_rail) &&
             ava::tui::question_option_for_screen_position(rail_modal_click, rail_modal_beta_row + 1, 53) == 1 &&
             ava::tui::question_option_for_screen_position(rail_modal_click, rail_modal_beta_row + 1, 124) == 1 &&
             ava::tui::question_option_for_screen_position(modal_without_rail, rail_modal_beta_row + 1, 60) == 1 &&
             !ava::tui::question_option_for_screen_position(rail_modal_click, rail_modal_beta_row + 1, 28) &&
             !ava::tui::question_option_for_screen_position(rail_modal_click, rail_modal_beta_row + 1, 149) &&
             std::ranges::none_of(rail_modal_frame, [](std::string const& line) { return strip_sgr(line).find("rail-must-not-render") != std::string::npos; }),
         "question modal suppresses the rail, centers inside the shared canvas, and maps physical mouse columns exactly once");

  auto depth_modal_snapshot = rail_modal_click;
  depth_modal_snapshot.transcript = {
      ava::tui::TranscriptItem{.label = "ava", .text = "**Backdrop context** [docs](https://example.test/modal-backdrop) remains visible"}};
  auto depth_base_snapshot = depth_modal_snapshot;
  depth_base_snapshot.question_prompt = std::nullopt;
  depth_base_snapshot.sidebar = std::nullopt;
  {
    ScopedEnvVar no_color_guard("NO_COLOR", "");
    ScopedTerminalCapabilityProfile hyperlink_terminal("vscode");
    auto const depth_base_frame = ava::tui::render_composer(depth_base_snapshot);
    auto const depth_modal_frame = ava::tui::render_composer(depth_modal_snapshot);
    expect(!depth_base_frame.empty() && depth_modal_frame.size() == depth_base_frame.size() &&
               depth_base_frame.front().find("\x1b]8;;https://example.test/modal-backdrop") != std::string::npos &&
               depth_modal_frame.front().find("Backdrop context") != std::string::npos &&
               depth_modal_frame.front().find("\x1b[38;2;88;96;112m") != std::string::npos && depth_modal_frame.front().find("\x1b]8;") == std::string::npos &&
               depth_modal_frame.front().find("\x1b[1m") == std::string::npos &&
               visible_columns(depth_modal_frame.front()) == visible_columns(depth_base_frame.front()),
           "colored modal backdrop keeps safe visible context at the same cell width while replacing SGR and OSC styling with the thinking tone");
  }
  {
    ScopedEnvVar no_color_guard("NO_COLOR", "1");
    auto const plain_base_frame = ava::tui::render_composer(depth_base_snapshot);
    auto const plain_modal_frame = ava::tui::render_composer(depth_modal_snapshot);
    expect(!plain_base_frame.empty() && plain_modal_frame.front() == plain_base_frame.front() &&
               plain_modal_frame.front().find("Backdrop context") != std::string::npos &&
               std::ranges::none_of(plain_modal_frame, [](std::string const& line) { return line.find('\x1b') != std::string::npos; }),
           "plain modal backdrop preserves the underlying visible text deterministically without terminal styling");
  }

  auto long_modal_prompt = ava::tui::QuestionPromptView{.header = "Choose a provider",
                                                        .question = "Select provider",
                                                        .options = long_question_options,
                                                        .modal = true,
                                                        .searchable = true,
                                                        .selected_option_index = 9,
                                                        .custom_text = {}};
  auto const long_modal_frame = ava::tui::render_composer(ava::tui::ComposerSnapshot{.mode = "build",
                                                                                     .provider = "openai",
                                                                                     .model = "gpt-5.5",
                                                                                     .session_id = "session_test",
                                                                                     .input = "",
                                                                                     .status = "question required",
                                                                                     .transcript = {},
                                                                                     .question_prompt = long_modal_prompt,
                                                                                     .width = 64,
                                                                                     .height = 12});
  expect(std::ranges::any_of(long_modal_frame, [](std::string const& line) { return strip_sgr(line).find("Option 9") != std::string::npos; }),
         "question modal scrolls its option window to keep the selected row visible");
  bool every_question_modal_selection_visible = true;
  for (std::size_t index = 0; index < long_question_options.size(); ++index)
  {
    auto modal_snapshot = ava::tui::ComposerSnapshot{};
    modal_snapshot.mode = "build";
    modal_snapshot.provider = "openai";
    modal_snapshot.model = "gpt-5.5";
    modal_snapshot.session_id = "session_test";
    modal_snapshot.question_prompt = long_modal_prompt;
    modal_snapshot.question_prompt->selected_option_index = index;
    modal_snapshot.width = 64;
    modal_snapshot.height = 12;
    auto const frame = ava::tui::render_composer(modal_snapshot);
    auto const wanted = "Option " + std::to_string(index);
    every_question_modal_selection_visible &=
        std::ranges::any_of(frame, [&](std::string const& line) { return strip_sgr(line).find(wanted) != std::string::npos; });
  }
  expect(every_question_modal_selection_visible, "question modal viewport follows every selected option beyond the initial screen");
  for (std::size_t height = 8; height <= 12; ++height)
  {
    for (std::size_t selected = 0; selected < long_question_options.size(); ++selected)
    {
      auto compact_modal = ava::tui::ComposerSnapshot{};
      compact_modal.mode = "build";
      compact_modal.provider = "openai";
      compact_modal.model = "gpt-5.5";
      compact_modal.session_id = "session_test";
      compact_modal.question_prompt = long_modal_prompt;
      compact_modal.question_prompt->selected_option_index = selected;
      compact_modal.width = 64;
      compact_modal.height = height;
      auto const frame = ava::tui::render_composer(compact_modal);
      auto const selected_label = "Option " + std::to_string(selected);
      expect(std::ranges::any_of(frame,
                                 [&](std::string const& line) {
                                   auto const text = strip_sgr(line);
                                   return text.find("›") != std::string::npos && text.find(selected_label) != std::string::npos;
                                 }),
             "integrated question modal keeps every selected option visible at compact terminal heights");
    }
  }

  auto const oauth_modal_frame = ava::tui::render_composer(ava::tui::ComposerSnapshot{
      .mode = "build",
      .provider = "openai",
      .model = "gpt-5.5",
      .session_id = "session_test",
      .input = "",
      .status = "question required",
      .transcript = {},
      .question_prompt = ava::tui::QuestionPromptView{.header = "ChatGPT Pro/Plus (browser)",
                                                      .question = "https://auth.openai.com/oauth/authorize?client_id="
                                                                  "app_EMoamEEZ73f0CkXaXp7hrann&redirect_uri=http%3A%2F%2Flocalhost%3A1455"
                                                                  "%2Fauth%2Fcallback&code_challenge=longchallengevalue&state=state\n"
                                                                  "\nComplete authorization in your browser. This window will close automatically."
                                                                  "\n\nWaiting for authorization...",
                                                      .options = {ava::tui::QuestionPromptOptionView{.value = "copy:https://auth.openai.com/oauth/authorize",
                                                                                                     .label = "C Copy"}},
                                                      .multiple = false,
                                                      .allow_custom = false,
                                                      .secret = false,
                                                      .modal = true,
                                                      .searchable = false,
                                                      .selected_option_index = 0,
                                                      .custom_text = ""},
      .width = 80,
      .height = 20});
  expect(std::ranges::any_of(oauth_modal_frame, [](std::string const& line) { return strip_sgr(line).find("https://auth.openai.com") != std::string::npos; }) &&
             std::ranges::any_of(oauth_modal_frame, [](std::string const& line) { return strip_sgr(line).find("C Copy") != std::string::npos; }) &&
             std::ranges::any_of(oauth_modal_frame,
                                 [](std::string const& line) { return strip_sgr(line).find("Waiting for authorization") != std::string::npos; }) &&
             std::ranges::any_of(oauth_modal_frame, [](std::string const& line) { return strip_sgr(line).find("C copy") != std::string::npos; }) &&
             std::ranges::none_of(oauth_modal_frame, [](std::string const& line) { return strip_sgr(line).find("Enter confirm") != std::string::npos; }),
         "tui renders OpenAI OAuth modal with opencode-style waiting and C copy shortcut");

  auto sidebar_modal_snapshot = ava::tui::ComposerSnapshot{
      .mode = "build",
      .provider = "openai",
      .model = "gpt-5.5",
      .session_id = "session_test",
      .input = "",
      .status = "question required",
      .transcript = {},
      .question_prompt = ava::tui::QuestionPromptView{.header = "Connect OpenAI",
                                                      .question = "Choose login method",
                                                      .options = {ava::tui::QuestionPromptOptionView{.value = "api_key", .label = "A OpenAI API key"}},
                                                      .multiple = false,
                                                      .allow_custom = false,
                                                      .secret = false,
                                                      .modal = true,
                                                      .searchable = false,
                                                      .selected_option_index = 0,
                                                      .custom_text = ""},
      .width = 176,
      .height = 22,
      .sidebar = ava::tui::SidebarSnapshot{
          .activity = {ava::tui::SidebarActivityItem{.id = "connect", .label = "rail-connect", .status = ava::tui::ToolTimelineStatus::Running}},
          .session_id = "session_test",
          .mode = "build",
          .provider = "openai",
          .model = "gpt-5.5",
          .workspace = "/workspace",
          .git_branch = "develop",
          .version = "0.32",
          .context_source_count = 1}};
  auto const sidebar_modal_frame = ava::tui::render_composer(sidebar_modal_snapshot);
  expect(ava::tui::composer_canvas_layout(sidebar_modal_snapshot).content_width == 120 && ava::tui::composer_canvas_layout(sidebar_modal_snapshot).left == 28 &&
             std::ranges::any_of(sidebar_modal_frame, [](std::string const& line) { return strip_sgr(line).find("? Connect OpenAI") == 56; }),
         "tui modal centers inside the width-limited canvas when an automatic rail snapshot exists");
  auto const wide_question_modal = ava::tui::detail::render_question_modal(*sidebar_modal_snapshot.question_prompt, 80, 14);
  auto const narrow_question_modal = ava::tui::detail::render_question_modal(*sidebar_modal_snapshot.question_prompt, 55, 12);
  auto const wide_question_title = strip_sgr(wide_question_modal[1]);
  auto const narrow_question_title = strip_sgr(narrow_question_modal[1]);
  expect(wide_question_title.starts_with("    ? Connect OpenAI") && wide_question_title.ends_with(std::string(4, ' ')) &&
             narrow_question_title.starts_with("  ? Connect OpenAI") && !narrow_question_title.starts_with("    ") &&
             narrow_question_title.ends_with(std::string(2, ' ')),
         "centered question modals share the complete responsive horizontal inset without changing their existing vertical padding");

  auto custom_secret_prompt = *sidebar_modal_snapshot.question_prompt;
  custom_secret_prompt.options.clear();
  custom_secret_prompt.allow_custom = true;
  custom_secret_prompt.secret = true;
  auto visible_custom_line = [](std::vector<std::string> const& rows) {
    for (auto const& row : rows)
    {
      auto visible = strip_sgr(row);
      if (visible.find("Custom:") != std::string::npos)
        return visible;
    }
    return std::string{};
  };
  auto const wide_empty_custom = visible_custom_line(ava::tui::detail::render_question_modal(custom_secret_prompt, 80, 14));
  auto const narrow_empty_custom = visible_custom_line(ava::tui::detail::render_question_modal(custom_secret_prompt, 55, 12));
  custom_secret_prompt.custom_text = "sk-must-stay-hidden";
  auto const wide_entered_custom = visible_custom_line(ava::tui::detail::render_question_modal(custom_secret_prompt, 80, 14));
  auto const narrow_entered_custom = visible_custom_line(ava::tui::detail::render_question_modal(custom_secret_prompt, 55, 12));
  expect(wide_empty_custom.starts_with("      Custom: paste secret") && wide_empty_custom.ends_with(std::string(4, ' ')) &&
             wide_entered_custom.starts_with("    › Custom: *******************") && wide_entered_custom.ends_with(std::string(4, ' ')) &&
             narrow_empty_custom.starts_with("    Custom: paste secret") && narrow_empty_custom.ends_with(std::string(2, ' ')) &&
             narrow_entered_custom.starts_with("  › Custom: *******************") && narrow_entered_custom.ends_with(std::string(2, ' ')) &&
             wide_entered_custom.find("sk-must-stay-hidden") == std::string::npos && narrow_entered_custom.find("sk-must-stay-hidden") == std::string::npos,
         "centered custom-answer rows retain complete responsive modal insets before and after secret entry without exposing the secret");
  expect(std::ranges::none_of(sidebar_modal_frame,
                              [](std::string const& line) {
                                auto const visible = strip_sgr(line);
                                return visible.find("rail-connect") != std::string::npos || visible.find("Modified Files") != std::string::npos ||
                                       visible.find("Session") != std::string::npos || visible.find("Context") != std::string::npos;
                              }),
         "tui modal is authoritative over automatic rail content");

  auto searchable_question = ava::tui::QuestionPromptView{.header = "Connect a provider",
                                                          .question = "Select provider",
                                                          .options = {ava::tui::QuestionPromptOptionView{.value = "openai", .label = "OpenAI"},
                                                                      ava::tui::QuestionPromptOptionView{.value = "anthropic", .label = "Anthropic"}},
                                                          .multiple = false,
                                                          .allow_custom = true,
                                                          .secret = false,
                                                          .modal = true,
                                                          .searchable = true,
                                                          .selected_option_index = 0,
                                                          .custom_text = ""};
  auto searchable_input = ava::tui::handle_question_prompt_input(searchable_question, ava::tui::InputEvent{.key = ava::tui::terminal::Key::Character, .character = 'h'});
  expect(searchable_input.action == ava::tui::QuestionPromptInputAction::Redraw && searchable_input.custom_text == "h" &&
             searchable_input.selected_option_index == 1,
         "searchable question typing filters and moves selection to the first match");
  searchable_question.custom_text = "anth";
  searchable_question.selected_option_index = 1;
  searchable_input = ava::tui::handle_question_prompt_input(searchable_question, ava::tui::InputEvent{.key = ava::tui::terminal::Key::Enter});
  expect(searchable_input.action == ava::tui::QuestionPromptInputAction::Resolve && searchable_input.options[1].selected,
         "searchable question enter selects the matched provider option");
  searchable_question.custom_text = "custom-provider";
  searchable_question.selected_option_index = 0;
  searchable_input = ava::tui::handle_question_prompt_input(searchable_question, ava::tui::InputEvent{.key = ava::tui::terminal::Key::Enter});
  auto const custom_search_answer =
      ava::tui::question_answer_from_prompt_view(ava::tui::QuestionPromptView{.header = searchable_question.header,
                                                                              .question = searchable_question.question,
                                                                              .options = searchable_input.options,
                                                                              .multiple = searchable_question.multiple,
                                                                              .allow_custom = searchable_question.allow_custom,
                                                                              .secret = searchable_question.secret,
                                                                              .modal = searchable_question.modal,
                                                                              .searchable = searchable_question.searchable,
                                                                              .selected_option_index = searchable_input.selected_option_index,
                                                                              .custom_text = searchable_input.custom_text});
  expect(custom_search_answer && custom_search_answer->selected_options.empty() && custom_search_answer->custom_text == "custom-provider",
         "searchable question enter resolves custom provider ids when no option matches");

  auto selector = ava::tui::SelectListView{.title = "Pick model",
                                           .subtitle = std::string("Choose a provider/model pair ") + "\x1b[31m" + "unsafe",
                                           .items = {ava::tui::SelectListItemView{.value = "openai/gpt-5.5",
                                                                                  .label = "GPT-5.5",
                                                                                  .description = "openai/gpt-5.5",
                                                                                  .group = "openai",
                                                                                  .detail = "tools yes · reasoning yes",
                                                                                  .badge = "reasoning",
                                                                                  .current = true,
                                                                                  .enabled = true,
                                                                                  .disabled_reason = {}},
                                                     ava::tui::SelectListItemView{.value = "anthropic/claude-sonnet-4-5",
                                                                                  .label = "Claude Sonnet 4.5",
                                                                                  .description = "anthropic/claude-sonnet-4-5",
                                                                                  .group = "anthropic",
                                                                                  .detail = "tools yes · reasoning no",
                                                                                  .badge = {},
                                                                                  .current = false,
                                                                                  .enabled = true,
                                                                                  .disabled_reason = {}},
                                                     ava::tui::SelectListItemView{.value = "ghost/model",
                                                                                  .label = "Ghost Model",
                                                                                  .description = "ghost/model",
                                                                                  .group = "ghost",
                                                                                  .detail = "tools ?",
                                                                                  .badge = {},
                                                                                  .current = false,
                                                                                  .enabled = false,
                                                                                  .disabled_reason = "provider unavailable"}},
                                           .selected_item_index = 0,
                                           .query = {},
                                           .placeholder = "Search models",
                                           .empty_text = "No matches",
                                           .footer_hint = "Enter choose · Esc cancel"};
  auto scrolling_selector = ava::tui::SelectListView{.title = "Pick model",
                                                     .subtitle = "Current openai/gpt-5.6-sol · selection is validated by the backend before session mutation",
                                                     .items = {},
                                                     .selected_item_index = 0,
                                                     .query = {},
                                                     .placeholder = "Search models",
                                                     .empty_text = "No matches",
                                                     .footer_hint = {}};
  for (std::size_t index = 0; index < 14; ++index)
  {
    scrolling_selector.items.push_back(ava::tui::SelectListItemView{.value = "model-" + std::to_string(index),
                                                                    .label = "Model " + std::to_string(index),
                                                                    .description = {},
                                                                    .group = "provider-" + std::to_string(index / 3),
                                                                    .detail = {},
                                                                    .badge = {},
                                                                    .current = false,
                                                                    .enabled = true,
                                                                    .disabled_reason = {}});
  }
  scrolling_selector.selected_item_index = 10;
  auto const scrolling_selector_frame = ava::tui::render_composer(ava::tui::ComposerSnapshot{.mode = "build",
                                                                                             .provider = "openai",
                                                                                             .model = "gpt-5.5",
                                                                                             .session_id = "session_test",
                                                                                             .input = "",
                                                                                             .status = "selecting",
                                                                                             .transcript = {},
                                                                                             .select_list = scrolling_selector,
                                                                                             .width = 64,
                                                                                             .height = 16});
  expect(std::ranges::any_of(scrolling_selector_frame, [](std::string const& line) { return strip_sgr(line).find("Model 10") != std::string::npos; }),
         "grouped select-list modals scroll far enough to keep the selected row visible");
  expect(std::ranges::none_of(scrolling_selector_frame, [](std::string const& line) { return line.find("\x1b[7m") != std::string::npos; }),
         "shared selectors use an accent marker and bold label without reverse-video bars");
  auto const scrolling_selected_row =
      std::ranges::find_if(scrolling_selector_frame, [](std::string const& line) { return strip_sgr(line).find("Model 10") != std::string::npos; });
  auto scrolling_hit_snapshot = ava::tui::ComposerSnapshot{};
  scrolling_hit_snapshot.mode = "build";
  scrolling_hit_snapshot.provider = "openai";
  scrolling_hit_snapshot.model = "gpt-5.5";
  scrolling_hit_snapshot.session_id = "session_test";
  scrolling_hit_snapshot.select_list = scrolling_selector;
  scrolling_hit_snapshot.width = 64;
  scrolling_hit_snapshot.height = 16;
  auto const scrolling_hit = scrolling_selected_row == scrolling_selector_frame.end()
                                 ? std::optional<std::size_t>{}
                                 : ava::tui::select_list_selection_for_screen_position(
                                       scrolling_hit_snapshot, static_cast<std::size_t>(scrolling_selected_row - scrolling_selector_frame.begin()) + 1, 8);
  expect(scrolling_hit && *scrolling_hit == 10, "grouped select-list modal hit testing shares the rendered scrolling window");
  bool every_select_list_selection_visible = true;
  for (std::size_t index = 0; index < scrolling_selector.items.size(); ++index)
  {
    auto selected_view = scrolling_selector;
    selected_view.selected_item_index = index;
    auto selected_snapshot = ava::tui::ComposerSnapshot{};
    selected_snapshot.mode = "build";
    selected_snapshot.provider = "openai";
    selected_snapshot.model = "gpt-5.5";
    selected_snapshot.session_id = "session_test";
    selected_snapshot.select_list = std::move(selected_view);
    selected_snapshot.width = 64;
    selected_snapshot.height = 16;
    auto const frame = ava::tui::render_composer(selected_snapshot);
    auto const wanted = "Model " + std::to_string(index);
    every_select_list_selection_visible &=
        std::ranges::any_of(frame, [&](std::string const& line) { return strip_sgr(line).find(wanted) != std::string::npos; });
  }
  expect(every_select_list_selection_visible, "grouped select-list viewport follows every selected item beyond the initial screen");
  for (std::size_t height = 8; height <= 12; ++height)
  {
    for (std::size_t selected = 0; selected < scrolling_selector.items.size(); ++selected)
    {
      scrolling_selector.selected_item_index = selected;
      auto compact_snapshot = ava::tui::ComposerSnapshot{};
      compact_snapshot.mode = "build";
      compact_snapshot.provider = "openai";
      compact_snapshot.model = "gpt-5.5";
      compact_snapshot.session_id = "session_test";
      compact_snapshot.select_list = scrolling_selector;
      compact_snapshot.width = 64;
      compact_snapshot.height = height;
      auto const frame = ava::tui::render_composer(compact_snapshot);
      auto const selected_label = "Model " + std::to_string(selected);
      auto const selected_line = std::ranges::find_if(frame, [&](std::string const& line) {
        auto const text = strip_sgr(line);
        return text.find("›") != std::string::npos && text.find(selected_label) != std::string::npos;
      });
      expect(selected_line != frame.end(), "integrated select-list modal keeps every selected item visible at compact terminal heights");
      if (selected_line != frame.end())
      {
        auto const hit = ava::tui::select_list_selection_for_screen_position(compact_snapshot, static_cast<std::size_t>(selected_line - frame.begin()) + 1, 8);
        expect(hit && *hit == selected, "integrated select-list modal hit testing matches every compact rendered selection");
      }
    }
  }
  for (std::size_t selected = 0; selected < scrolling_selector.items.size(); ++selected)
  {
    scrolling_selector.selected_item_index = selected;
    auto const compact_lines = ava::tui::detail::render_select_list_modal(scrolling_selector, 64, 8);
    auto const selected_label = "Model " + std::to_string(selected);
    auto const selected_line = std::ranges::find_if(compact_lines, [&](std::string const& line) {
      auto const text = strip_sgr(line);
      return text.find("›") != std::string::npos && text.find(selected_label) != std::string::npos;
    });
    expect(selected_line != compact_lines.end(), "minimum-height select-list modal reserves a visible row for every selection despite a wrapped subtitle");
    if (selected_line != compact_lines.end())
    {
      auto const hit =
          ava::tui::detail::select_list_item_for_modal_row(scrolling_selector, static_cast<std::size_t>(selected_line - compact_lines.begin()), 64, 8);
      expect(hit && *hit == selected, "minimum-height select-list modal hit testing matches every rendered selection");
    }
  }

  auto boundary_selector = ava::tui::SelectListView{
      .title = "Group boundary",
      .subtitle = {},
      .items = {ava::tui::SelectListItemView{
                    .value = "a-0", .label = "A zero", .description = {}, .group = "provider-a", .detail = {}, .badge = {}, .disabled_reason = {}},
                ava::tui::SelectListItemView{
                    .value = "a-1", .label = "A one", .description = {}, .group = "provider-a", .detail = {}, .badge = {}, .disabled_reason = {}},
                ava::tui::SelectListItemView{
                    .value = "b-0", .label = "B zero", .description = {}, .group = "provider-b", .detail = {}, .badge = {}, .disabled_reason = {}}},
      .selected_item_index = 2,
      .query = {},
      .placeholder = "Search",
      .empty_text = "No matches",
      .footer_hint = {}};
  auto const boundary_lines = ava::tui::detail::render_select_list_modal(boundary_selector, 64, 10);
  auto const boundary_selected = std::ranges::find_if(boundary_lines, [](std::string const& line) {
    auto const text = strip_sgr(line);
    return text.find("›") != std::string::npos && text.find("B zero") != std::string::npos;
  });
  expect(boundary_selected != boundary_lines.end() && boundary_selected != boundary_lines.begin() &&
             strip_sgr(*(boundary_selected - 1)).find("provider-b") != std::string::npos,
         "grouped select-list modal never renders the first item of a new group beneath the previous group's header");
  if (boundary_selected != boundary_lines.end())
  {
    auto const hit =
        ava::tui::detail::select_list_item_for_modal_row(boundary_selector, static_cast<std::size_t>(boundary_selected - boundary_lines.begin()), 64, 10);
    expect(hit && *hit == 2, "group-boundary select-list hit testing matches the rendered selected item");
  }

  selector.query = "sonnet";
  auto selector_matches = ava::tui::filter_select_list_items(selector);
  expect(selector_matches.size() == 1 && selector_matches.front() == 1,
         "select-list fuzzy filter matches provider/model labels and preserves original row ids");
  auto selector_input = ava::tui::handle_select_list_input(selector, ava::tui::InputEvent{.key = ava::tui::terminal::Key::Character, .character = 'g'});
  expect(selector_input.action == ava::tui::SelectListInputAction::Redraw && selector_input.query == "sonnetg" && selector_input.selected_item_index == 0,
         "select-list typing updates the search query and clamps to empty-match selection safely");
  selector_input = ava::tui::handle_select_list_input(selector, ava::tui::InputEvent{.key = ava::tui::terminal::Key::Space});
  expect(selector_input.action == ava::tui::SelectListInputAction::Redraw && selector_input.query == "sonnet ",
         "select-list semantic Space remains searchable text instead of losing spaces in modal queries");
  auto const selector_keybinds = ava::tui::parse_key_bindings_json(
      "{\"tui.select.confirm\":[\"Enter\",\"Space\"],\"tui.select.cancel\":[\"Escape\",\"Ctrl+W\"],"
      "\"tui.select.up\":\"Ctrl+P\",\"tui.select.down\":\"Ctrl+N\","
      "\"tui.select.pageUp\":\"Ctrl+O\",\"tui.select.pageDown\":\"Ctrl+Y\"}");
  expect(static_cast<bool>(selector_keybinds), "select-list custom keybind fixture parses");
  auto const selector_bindings = selector_keybinds ? *selector_keybinds : ava::tui::default_key_bindings();
  auto bound_selector = selector;
  bound_selector.query.clear();
  bound_selector.selected_item_index = 0;
  selector_input = ava::tui::handle_select_list_input(bound_selector, ava::tui::InputEvent{.key = ava::tui::terminal::Key::CtrlN}, selector_bindings);
  expect(selector_input.action == ava::tui::SelectListInputAction::Redraw && selector_input.selected_item_index == 1,
         "select-list uses configured down binding before raw Ctrl+N modal actions");
  bound_selector.selected_item_index = 1;
  selector_input = ava::tui::handle_select_list_input(bound_selector, ava::tui::InputEvent{.key = ava::tui::terminal::Key::CtrlP}, selector_bindings);
  expect(selector_input.action == ava::tui::SelectListInputAction::Redraw && selector_input.selected_item_index == 0,
         "select-list uses configured up binding before raw Ctrl+P modal actions");
  bound_selector.selected_item_index = 0;
  selector_input = ava::tui::handle_select_list_input(bound_selector, ava::tui::InputEvent{.key = ava::tui::terminal::Key::Space}, selector_bindings);
  expect(selector_input.action == ava::tui::SelectListInputAction::Resolve && selector_input.selected_item_index == 0,
         "select-list configured Space confirms instead of appending query text");
  selector_input = ava::tui::handle_select_list_input(bound_selector, ava::tui::InputEvent{.key = ava::tui::terminal::Key::CtrlW}, selector_bindings);
  expect(selector_input.action == ava::tui::SelectListInputAction::Cancel,
         "select-list uses configured cancel binding before raw composer delete-word actions");
  auto const session_selector_keybinds = ava::tui::parse_key_bindings_json(
      "{\"app.session.togglePath\":\"Ctrl+O\",\"app.session.toggleSort\":\"Ctrl+Y\","
      "\"app.session.toggleNamedFilter\":\"Ctrl+U\",\"app.session.rename\":\"Ctrl+K\","
      "\"app.session.delete\":\"Alt+D\"}");
  expect(static_cast<bool>(session_selector_keybinds), "session selector custom keybind fixture parses");
  auto const session_selector_bindings = session_selector_keybinds ? *session_selector_keybinds : ava::tui::default_key_bindings();
  auto session_bound_selector = selector;
  session_bound_selector.query = "custom";
  selector_input = ava::tui::handle_select_list_input(session_bound_selector, ava::tui::InputEvent{.key = ava::tui::terminal::Key::CtrlO}, session_selector_bindings);
  expect(selector_input.action == ava::tui::SelectListInputAction::TogglePathDisplay && selector_input.query == session_bound_selector.query,
         "select-list routes custom app.session.togglePath binding before raw composer details actions");
  selector_input = ava::tui::handle_select_list_input(session_bound_selector, ava::tui::InputEvent{.key = ava::tui::terminal::Key::CtrlY}, session_selector_bindings);
  expect(selector_input.action == ava::tui::SelectListInputAction::CycleSort && selector_input.query == session_bound_selector.query,
         "select-list routes custom app.session.toggleSort binding before raw composer yank actions");
  selector_input = ava::tui::handle_select_list_input(session_bound_selector, ava::tui::InputEvent{.key = ava::tui::terminal::Key::CtrlU}, session_selector_bindings);
  expect(selector_input.action == ava::tui::SelectListInputAction::ToggleNamedFilter && selector_input.query == session_bound_selector.query,
         "select-list routes custom app.session.toggleNamedFilter binding before raw composer line deletion");
  selector_input = ava::tui::handle_select_list_input(session_bound_selector, ava::tui::InputEvent{.key = ava::tui::terminal::Key::CtrlK}, session_selector_bindings);
  expect(selector_input.action == ava::tui::SelectListInputAction::Rename && selector_input.query == session_bound_selector.query,
         "select-list routes custom app.session.rename binding before raw composer line-end deletion");
  selector_input = ava::tui::handle_select_list_input(session_bound_selector, ava::tui::InputEvent{.key = ava::tui::terminal::Key::AltD}, session_selector_bindings);
  expect(selector_input.action == ava::tui::SelectListInputAction::Archive && selector_input.query == session_bound_selector.query,
         "select-list routes custom app.session.delete binding before raw composer word deletion");
  auto const tree_selector_keybinds = ava::tui::parse_key_bindings_json(tui_test_support::tree_action_key_bindings_json());
  expect(static_cast<bool>(tree_selector_keybinds), "tree selector custom keybind fixture parses");
  auto const tree_selector_bindings = tree_selector_keybinds ? *tree_selector_keybinds : ava::tui::default_key_bindings();
  selector_input = ava::tui::handle_select_list_input(session_bound_selector, ava::tui::InputEvent{.key = ava::tui::terminal::Key::CtrlO}, tree_selector_bindings);
  expect(selector_input.action == ava::tui::SelectListInputAction::BranchParent && selector_input.query == session_bound_selector.query,
         "select-list routes custom app.tree.foldOrUp binding before raw composer details actions");
  selector_input = ava::tui::handle_select_list_input(session_bound_selector, ava::tui::InputEvent{.key = ava::tui::terminal::Key::CtrlY}, tree_selector_bindings);
  expect(selector_input.action == ava::tui::SelectListInputAction::BranchChild && selector_input.query == session_bound_selector.query,
         "select-list routes custom app.tree.unfoldOrDown binding before raw composer yank actions");
  selector_input = ava::tui::handle_select_list_input(
      session_bound_selector, ava::tui::InputEvent{.key = ava::tui::terminal::Key::Character, .character = 'L', .text = "L"}, tree_selector_bindings);
  expect(selector_input.action == ava::tui::SelectListInputAction::Label && selector_input.query == session_bound_selector.query,
         "select-list routes Pi app.tree.editLabel Shift+L binding to the existing label draft action");
  selector_input = ava::tui::handle_select_list_input(
      session_bound_selector, ava::tui::InputEvent{.key = ava::tui::terminal::Key::Character, .character = 'l', .text = "l"}, tree_selector_bindings);
  expect(selector_input.action == ava::tui::SelectListInputAction::Redraw && selector_input.query == session_bound_selector.query + "l",
         "select-list keeps ordinary lowercase l as searchable text when Shift+L is bound");
  selector_input = ava::tui::handle_select_list_input(
      session_bound_selector, ava::tui::InputEvent{.key = ava::tui::terminal::Key::Character, .character = 'T', .text = "T"}, tree_selector_bindings);
  expect(selector_input.action == ava::tui::SelectListInputAction::ToggleLabelTimestamp && selector_input.query == session_bound_selector.query,
         "select-list routes Pi app.tree.toggleLabelTimestamp Shift+T binding to the label-time toggle");
  selector_input = ava::tui::handle_select_list_input(session_bound_selector, ava::tui::InputEvent{.key = ava::tui::terminal::Key::CtrlSpace}, tree_selector_bindings);
  expect(selector_input.action == ava::tui::SelectListInputAction::ToggleNamedFilter && selector_input.query == session_bound_selector.query,
         "select-list routes Pi app.tree.filter.labeledOnly to the existing named/labeled filter");
  selector_input = ava::tui::handle_select_list_input(session_bound_selector, ava::tui::InputEvent{.key = ava::tui::terminal::Key::CtrlSlash}, tree_selector_bindings);
  expect(selector_input.action == ava::tui::SelectListInputAction::ToggleArchivedFilter && selector_input.query == session_bound_selector.query,
         "select-list routes Pi app.tree.filter.all to the existing archived visibility filter");
  selector_input = ava::tui::handle_select_list_input(
      session_bound_selector, ava::tui::InputEvent{.key = ava::tui::terminal::Key::Character, .character = 't', .text = "t"}, tree_selector_bindings);
  expect(selector_input.action == ava::tui::SelectListInputAction::Redraw && selector_input.query == session_bound_selector.query + "t",
         "select-list keeps ordinary lowercase t as searchable text when Shift+T is bound");
  selector.query = "ghost";
  selector.selected_item_index = 2;
  selector_input = ava::tui::handle_select_list_input(selector, ava::tui::InputEvent{.key = ava::tui::terminal::Key::Enter});
  expect(selector_input.action == ava::tui::SelectListInputAction::Redraw, "select-list enter refuses disabled model/provider rows without resolving");
  selector.query = "claude";
  selector.selected_item_index = 0;
  selector_input = ava::tui::handle_select_list_input(selector, ava::tui::InputEvent{.key = ava::tui::terminal::Key::Enter});
  expect(selector_input.action == ava::tui::SelectListInputAction::Resolve && selector_input.selected_item_index == 1,
         "select-list enter resolves the highlighted enabled fuzzy match");
  selector_input = ava::tui::handle_select_list_input(selector, ava::tui::InputEvent{.key = ava::tui::terminal::Key::Escape});
  expect(selector_input.action == ava::tui::SelectListInputAction::Cancel, "select-list escape cancels the modal safely");
  selector_input = ava::tui::handle_select_list_input(selector, ava::tui::InputEvent{.key = ava::tui::terminal::Key::MouseLeftClick});
  expect(selector_input.action == ava::tui::SelectListInputAction::None && selector_input.query == selector.query,
         "select-list raw mouse clicks are resolved by rendered modal hit-testing instead of triggering keyboard-only actions");
  selector_input = ava::tui::handle_select_list_input(selector, ava::tui::InputEvent{.key = ava::tui::terminal::Key::CtrlT});
  expect(selector_input.action == ava::tui::SelectListInputAction::CycleSort && selector_input.query == selector.query,
         "select-list ctrl+t exposes a modal sort-cycle action without clearing search state");
  selector_input = ava::tui::handle_select_list_input(selector, ava::tui::InputEvent{.key = ava::tui::terminal::Key::CtrlS});
  expect(selector_input.action == ava::tui::SelectListInputAction::CycleSort && selector_input.query == selector.query,
         "select-list ctrl+s exposes the Pi-compatible modal sort-cycle action without clearing search state");
  selector_input = ava::tui::handle_select_list_input(selector, ava::tui::InputEvent{.key = ava::tui::terminal::Key::CtrlN});
  expect(selector_input.action == ava::tui::SelectListInputAction::ToggleNamedFilter && selector_input.query == selector.query,
         "select-list ctrl+n exposes a modal named-session filter action without clearing search state");
  selector_input = ava::tui::handle_select_list_input(selector, ava::tui::InputEvent{.key = ava::tui::terminal::Key::CtrlP});
  expect(selector_input.action == ava::tui::SelectListInputAction::TogglePathDisplay && selector_input.query == selector.query,
         "select-list ctrl+p exposes a modal path-display toggle without clearing search state");
  selector_input = ava::tui::handle_select_list_input(selector, ava::tui::InputEvent{.key = ava::tui::terminal::Key::CtrlA});
  expect(selector_input.action == ava::tui::SelectListInputAction::ToggleArchivedFilter && selector_input.query == selector.query,
         "select-list ctrl+a exposes a modal archived-session filter toggle without clearing search state");
  selector_input = ava::tui::handle_select_list_input(selector, ava::tui::InputEvent{.key = ava::tui::terminal::Key::CtrlX});
  expect(selector_input.action == ava::tui::SelectListInputAction::ModelsClearAll && selector_input.query == selector.query,
         "select-list ctrl+x exposes a scoped-model clear action without clearing search state");
  selector_input = ava::tui::handle_select_list_input(selector, ava::tui::InputEvent{.key = ava::tui::terminal::Key::CtrlR});
  expect(selector_input.action == ava::tui::SelectListInputAction::Rename && selector_input.query == selector.query,
         "select-list ctrl+r exposes a modal rename action without clearing search state");
  selector_input = ava::tui::handle_select_list_input(selector, ava::tui::InputEvent{.key = ava::tui::terminal::Key::CtrlL});
  expect(selector_input.action == ava::tui::SelectListInputAction::Label && selector_input.query == selector.query,
         "select-list ctrl+l exposes a modal label action without clearing search state");
  selector_input = ava::tui::handle_select_list_input(selector, ava::tui::InputEvent{.key = ava::tui::terminal::Key::CtrlD});
  expect(selector_input.action == ava::tui::SelectListInputAction::Archive && selector_input.query == selector.query,
         "select-list ctrl+d exposes a modal archive action without clearing search state");
  selector_input = ava::tui::handle_select_list_input(selector, ava::tui::InputEvent{.key = ava::tui::terminal::Key::CtrlBackspace});
  expect(selector_input.action == ava::tui::SelectListInputAction::None && selector_input.query == selector.query,
         "select-list ctrl+backspace is non-destructive while the search query is non-empty");
  auto empty_query_selector = selector;
  empty_query_selector.query.clear();
  selector_input = ava::tui::handle_select_list_input(empty_query_selector, ava::tui::InputEvent{.key = ava::tui::terminal::Key::CtrlBackspace});
  expect(selector_input.action == ava::tui::SelectListInputAction::ArchiveNoninvasive && selector_input.query.empty(),
         "select-list ctrl+backspace archives only when the selector search query is empty");
  selector_input = ava::tui::handle_select_list_input(selector, ava::tui::InputEvent{.key = ava::tui::terminal::Key::AltArrowLeft});
  expect(selector_input.action == ava::tui::SelectListInputAction::BranchParent && selector_input.query == selector.query,
         "select-list alt-left exposes a modal parent-branch action without clearing search state");
  selector_input = ava::tui::handle_select_list_input(selector, ava::tui::InputEvent{.key = ava::tui::terminal::Key::CtrlArrowLeft});
  expect(selector_input.action == ava::tui::SelectListInputAction::BranchParent && selector_input.query == selector.query,
         "select-list ctrl-left exposes a modal parent-branch action without clearing search state");
  selector_input = ava::tui::handle_select_list_input(selector, ava::tui::InputEvent{.key = ava::tui::terminal::Key::AltArrowRight});
  expect(selector_input.action == ava::tui::SelectListInputAction::BranchChild && selector_input.query == selector.query,
         "select-list alt-right exposes a modal child-branch action without clearing search state");
  selector_input = ava::tui::handle_select_list_input(selector, ava::tui::InputEvent{.key = ava::tui::terminal::Key::CtrlArrowRight});
  expect(selector_input.action == ava::tui::SelectListInputAction::BranchChild && selector_input.query == selector.query,
         "select-list ctrl-right exposes a modal child-branch action without clearing search state");
  ava::tui::SelectListView paged_selector{.title = "Paged",
                                          .subtitle = {},
                                          .items = {},
                                          .selected_item_index = 0,
                                          .query = {},
                                          .placeholder = "Search rows",
                                          .empty_text = "No rows",
                                          .footer_hint = {}};
  for (int index = 0; index < 8; ++index)
  {
    paged_selector.items.push_back(ava::tui::SelectListItemView{.value = "row_" + std::to_string(index),
                                                                .label = "Row " + std::to_string(index),
                                                                .description = "Page navigation row",
                                                                .group = "Rows",
                                                                .detail = {},
                                                                .badge = {},
                                                                .current = false,
                                                                .enabled = true,
                                                                .disabled_reason = {}});
  }
  selector_input = ava::tui::handle_select_list_input(paged_selector, ava::tui::InputEvent{.key = ava::tui::terminal::Key::PageDown});
  expect(selector_input.action == ava::tui::SelectListInputAction::Redraw && selector_input.selected_item_index == 5,
         "select-list PageDown jumps five visible rows");
  auto bound_paged_selector = paged_selector;
  selector_input = ava::tui::handle_select_list_input(bound_paged_selector, ava::tui::InputEvent{.key = ava::tui::terminal::Key::CtrlY}, selector_bindings);
  expect(selector_input.action == ava::tui::SelectListInputAction::Redraw && selector_input.selected_item_index == 5,
         "select-list uses configured page-down binding before raw Ctrl+Y composer yank actions");
  bound_paged_selector.selected_item_index = 5;
  selector_input = ava::tui::handle_select_list_input(bound_paged_selector, ava::tui::InputEvent{.key = ava::tui::terminal::Key::CtrlO}, selector_bindings);
  expect(selector_input.action == ava::tui::SelectListInputAction::Redraw && selector_input.selected_item_index == 0,
         "select-list uses configured page-up binding before raw Ctrl+O details actions");
  paged_selector.selected_item_index = 5;
  selector_input = ava::tui::handle_select_list_input(paged_selector, ava::tui::InputEvent{.key = ava::tui::terminal::Key::PageDown});
  expect(selector_input.action == ava::tui::SelectListInputAction::Redraw && selector_input.selected_item_index == 7,
         "select-list PageDown clamps at the last matching row instead of wrapping");
  paged_selector.selected_item_index = 7;
  selector_input = ava::tui::handle_select_list_input(paged_selector, ava::tui::InputEvent{.key = ava::tui::terminal::Key::PageUp});
  expect(selector_input.action == ava::tui::SelectListInputAction::Redraw && selector_input.selected_item_index == 2,
         "select-list PageUp jumps five visible rows");
  paged_selector.selected_item_index = 0;
  selector_input = ava::tui::handle_select_list_input(paged_selector, ava::tui::InputEvent{.key = ava::tui::terminal::Key::PageUp});
  expect(selector_input.action == ava::tui::SelectListInputAction::Redraw && selector_input.selected_item_index == 0,
         "select-list PageUp clamps at the first matching row instead of wrapping");
  selector_input = ava::tui::handle_select_list_input(selector, ava::tui::InputEvent{.key = ava::tui::terminal::Key::CtrlEnter});
  expect(selector_input.action == ava::tui::SelectListInputAction::None && selector_input.query == selector.query,
         "select-list ctrl+enter remains a composer-only newline alias and does not trigger modal actions");
  selector_input = ava::tui::handle_select_list_input(selector, ava::tui::InputEvent{.key = ava::tui::terminal::Key::AltEnter});
  expect(selector_input.action == ava::tui::SelectListInputAction::None && selector_input.query == selector.query,
         "select-list alt+enter remains a composer-only follow-up/submit action and does not trigger modal actions");
  selector_input = ava::tui::handle_select_list_input(selector, ava::tui::InputEvent{.key = ava::tui::terminal::Key::ShiftTab});
  expect(selector_input.action == ava::tui::SelectListInputAction::None && selector_input.query == selector.query,
         "select-list shift+tab remains an app-level reasoning shortcut and does not trigger modal actions");
  selector_input = ava::tui::handle_select_list_input(selector, ava::tui::InputEvent{.key = ava::tui::terminal::Key::AltArrowLeft});
  expect(selector_input.action == ava::tui::SelectListInputAction::BranchParent && selector_input.query == selector.query,
         "select-list alt+left triggers parent-branch navigation in modal context");
  selector_input = ava::tui::handle_select_list_input(selector, ava::tui::InputEvent{.key = ava::tui::terminal::Key::AltArrowRight});
  expect(selector_input.action == ava::tui::SelectListInputAction::BranchChild && selector_input.query == selector.query,
         "select-list alt+right triggers child-branch navigation in modal context");
  selector_input = ava::tui::handle_select_list_input(selector, ava::tui::InputEvent{.key = ava::tui::terminal::Key::AltArrowUp});
  expect(selector_input.action == ava::tui::SelectListInputAction::ModelsReorderUp && selector_input.query == selector.query,
         "select-list alt+up exposes a scoped-model reorder-up action without clearing search state");
  selector_input = ava::tui::handle_select_list_input(selector, ava::tui::InputEvent{.key = ava::tui::terminal::Key::AltArrowDown});
  expect(selector_input.action == ava::tui::SelectListInputAction::ModelsReorderDown && selector_input.query == selector.query,
         "select-list alt+down exposes a scoped-model reorder-down action without clearing search state");

  auto const selector_frame =
      ava::tui::render_composer(ava::tui::ComposerSnapshot{.mode = "build",
                                                           .provider = "openai",
                                                           .model = "gpt-5.5",
                                                           .session_id = "session_test",
                                                           .input = "composer behind selector",
                                                           .status = "selecting model",
                                                           .transcript = {ava::tui::TranscriptItem{.label = "ava", .text = "background transcript"}},
                                                           .select_list = selector,
                                                           .width = 88,
                                                           .height = 18});
  auto click_selector = selector;
  click_selector.query.clear();
  click_selector.selected_item_index = 0;
  auto click_selector_snapshot = ava::tui::ComposerSnapshot{.mode = "build",
                                                            .provider = "openai",
                                                            .model = "gpt-5.5",
                                                            .session_id = "session_test",
                                                            .input = "composer behind selector",
                                                            .status = "selector opened",
                                                            .transcript = {},
                                                            .select_list = click_selector,
                                                            .width = 92,
                                                            .height = 18};
  auto const click_selector_frame = ava::tui::render_composer(click_selector_snapshot);
  auto const click_row =
      std::ranges::find_if(click_selector_frame, [](std::string const& line) { return strip_sgr(line).find("Claude Sonnet 4.5") != std::string::npos; }) -
      click_selector_frame.begin() + 1;
  auto const clicked_selector = ava::tui::select_list_selection_for_screen_position(click_selector_snapshot, click_row, 12);
  expect(clicked_selector && *clicked_selector == 1, "select-list hit-test maps the exact rendered modal item row back to its original item index");
  auto rail_selector_snapshot = click_selector_snapshot;
  rail_selector_snapshot.width = 176;
  rail_selector_snapshot.sidebar = ava::tui::SidebarSnapshot{
      .modified_files = {ava::tui::SidebarModifiedFile{.path = "rail-must-not-render.cpp"}}, .mode = "build", .provider = "openai", .model = "gpt-5.5"};
  auto const rail_selector_frame = ava::tui::render_composer(rail_selector_snapshot);
  auto const rail_selector_row =
      std::ranges::find_if(rail_selector_frame, [](std::string const& line) { return strip_sgr(line).find("Claude Sonnet 4.5") != std::string::npos; }) -
      rail_selector_frame.begin() + 1;
  auto selector_without_rail = rail_selector_snapshot;
  selector_without_rail.sidebar = std::nullopt;
  expect(ava::tui::composer_canvas_layout(rail_selector_snapshot).content_width == 120 && ava::tui::composer_canvas_layout(rail_selector_snapshot).left == 28 &&
             rail_selector_frame == ava::tui::render_composer(selector_without_rail) &&
             ava::tui::select_list_selection_for_screen_position(rail_selector_snapshot, rail_selector_row, 53) == 1 &&
             ava::tui::select_list_selection_for_screen_position(rail_selector_snapshot, rail_selector_row, 124) == 1 &&
             ava::tui::select_list_selection_for_screen_position(selector_without_rail, rail_selector_row, 60) == 1 &&
             !ava::tui::select_list_selection_for_screen_position(rail_selector_snapshot, rail_selector_row, 28) &&
             !ava::tui::select_list_selection_for_screen_position(rail_selector_snapshot, rail_selector_row, 149) &&
             std::ranges::none_of(rail_selector_frame,
                                  [](std::string const& line) { return strip_sgr(line).find("rail-must-not-render.cpp") != std::string::npos; }),
         "select-list modal suppresses an actionable rail and maps centered physical mouse geometry exactly once");
  auto const outside_selector = ava::tui::select_list_selection_for_screen_position(ava::tui::ComposerSnapshot{.mode = "build",
                                                                                                               .provider = "openai",
                                                                                                               .model = "gpt-5.5",
                                                                                                               .session_id = "session_test",
                                                                                                               .input = "composer behind selector",
                                                                                                               .status = "selector opened",
                                                                                                               .transcript = {},
                                                                                                               .select_list = click_selector,
                                                                                                               .width = 92,
                                                                                                               .height = 18},
                                                                                    12, 1);
  expect(!outside_selector, "select-list hit-test ignores clicks outside the modal horizontal bounds");
  expect(selector_frame.size() == 18 &&
             std::ranges::any_of(selector_frame, [](std::string const& line) { return strip_sgr(line).find("Pick model") != std::string::npos; }) &&
             std::ranges::any_of(selector_frame, [](std::string const& line) { return strip_sgr(line).find("filter  claude") != std::string::npos; }) &&
             std::ranges::any_of(selector_frame, [](std::string const& line) { return strip_sgr(line).find("Claude Sonnet 4.5") != std::string::npos; }),
         "tui renders the shared selector title, quiet filter, and one-row filtered item");
  expect(std::ranges::none_of(selector_frame, [](std::string const& line) { return strip_sgr(line).find("GPT-5.5  current") != std::string::npos; }) &&
             std::ranges::none_of(selector_frame, [](std::string const& line) { return line.find("\x1b[7m") != std::string::npos; }),
         "tui shared selector uses textual current state and no reverse-video bar");
  expect(std::ranges::any_of(selector_frame,
                             [](std::string const& line) {
                               auto const visible = strip_sgr(line);
                               return visible.find("↑↓ navigate") != std::string::npos && visible.find("Enter select") != std::string::npos &&
                                      visible.find("Esc close") != std::string::npos;
                             }),
         "tui shared selector renders one width-prioritized footer row");
}

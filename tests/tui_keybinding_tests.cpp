#include "sys.h"
#include "tests/support/test_harness.h"
#include "tests/support/tui_test_support.h"
#include "ava/tui/keybindings.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>

void run_tui_keybinding_tests()
{
  auto const key_bindings = ava::tui::parse_key_bindings_json(
      "{\"submit\":\"Ctrl+T, Enter\",\"new_line\":\"Shift+Enter, Ctrl+Enter\",\"message_follow_up\":\"Alt+Enter\","
      "\"delete_to_line_start\":\"Ctrl+U\","
      "\"autocomplete_accept\":\"Tab\",\"variant_cycle\":\"Ctrl+D, Shift+Tab\",\"undo\":\"Ctrl+-\","
      "\"suspend\":\"Ctrl+Z\",\"clipboard_paste_image\":\"Ctrl+V\",\"redo\":\"Ctrl+R\",\"delete_forward\":\"Delete\","
      "\"cursor_word_left\":\"Ctrl+Left, Alt+Left, Alt+B\",\"cursor_word_right\":\"Ctrl+Right, Alt+Right, Alt+F\","
      "\"jump_forward\":\"Ctrl+]\",\"jump_backward\":\"Ctrl+Alt+]\","
      "\"delete_word_backward\":\"Ctrl+W, Alt+Backspace\",\"delete_word_forward\":\"Alt+D, Alt+Delete\","
      "\"details_toggle\":\"Ctrl+O\",\"model_select\":\"Ctrl+L\",\"model_cycle_forward\":\"Ctrl+P\","
      "\"model_cycle_backward\":\"Shift+Ctrl+P\",\"app.models.save\":\"Ctrl+S\","
      "\"app.models.enableAll\":\"Ctrl+A\",\"app.models.clearAll\":\"Ctrl+X\","
      "\"app.models.toggleProvider\":\"Ctrl+P\",\"app.models.reorderUp\":\"Alt+Up\","
      "\"app.models.reorderDown\":\"Alt+Down\",\"message_dequeue\":\"Alt+Up\",\"yank_pop\":\"Alt+Y\"}");
  auto const parsed_ctrl_s = ava::tui::parse_key_name("Ctrl+S");
  auto const parsed_ctrl_l = ava::tui::parse_key_name("Ctrl+L");
  auto const parsed_ctrl_shift_p = ava::tui::parse_key_name("Shift+Ctrl+P");
  auto const parsed_ctrl_g = ava::tui::parse_key_name("Ctrl+G");
  auto const parsed_alt_up = ava::tui::parse_key_name("Alt+Up");
  auto const parsed_alt_down = ava::tui::parse_key_name("Alt+Down");
  auto const parsed_ctrl_x = ava::tui::parse_key_name("Ctrl+X");
  auto const parsed_ctrl_enter = ava::tui::parse_key_name("Ctrl+Enter");
  auto const parsed_alt_enter = ava::tui::parse_key_name("Alt+Enter");
  auto const parsed_space = ava::tui::parse_key_name("Space");
  auto const parsed_ctrl_space = ava::tui::parse_key_name("Ctrl+Space");
  auto const parsed_ctrl_1 = ava::tui::parse_key_name("Ctrl+1");
  auto const parsed_ctrl_9 = ava::tui::parse_key_name("Ctrl+9");
  auto const parsed_shift_tab = ava::tui::parse_key_name("Shift+Tab");
  auto const parsed_shift_l = ava::tui::parse_key_name("Shift+L");
  auto const parsed_shift_t = ava::tui::parse_key_name("Shift+T");
  auto const parsed_ctrl_minus = ava::tui::parse_key_name("Ctrl+-");
  auto const parsed_ctrl_slash = ava::tui::parse_key_name("Ctrl+/");
  auto const parsed_ctrl_o = ava::tui::parse_key_name("Ctrl+O");
  auto const parsed_ctrl_v = ava::tui::parse_key_name("Ctrl+V");
  auto const parsed_ctrl_right_bracket = ava::tui::parse_key_name("Ctrl+]");
  auto const parsed_ctrl_alt_right_bracket = ava::tui::parse_key_name("Ctrl+Alt+]");
  auto const parsed_shift_up = ava::tui::parse_key_name("Shift+Up");
  auto const parsed_shift_down = ava::tui::parse_key_name("Shift+Down");
  auto const parsed_shift_left = ava::tui::parse_key_name("Shift+Left");
  auto const parsed_shift_right = ava::tui::parse_key_name("Shift+Right");
  auto const parsed_shift_ctrl_left = ava::tui::parse_key_name("Shift+Ctrl+Left");
  auto const parsed_ctrl_shift_right = ava::tui::parse_key_name("Ctrl+Shift+Right");
  auto const parsed_shift_alt_left = ava::tui::parse_key_name("Shift+Alt+Left");
  auto const parsed_alt_shift_right = ava::tui::parse_key_name("Alt+Shift+Right");
  auto const parsed_alt_left = ava::tui::parse_key_name("Alt+Left");
  auto const parsed_alt_right = ava::tui::parse_key_name("Alt+Right");
  auto const parsed_shift_backspace = ava::tui::parse_key_name("Shift+Backspace");
  auto const parsed_ctrl_backspace = ava::tui::parse_key_name("Ctrl+Backspace");
  auto const parsed_shift_delete = ava::tui::parse_key_name("Shift+Delete");
  auto const parsed_insert = ava::tui::parse_key_name("Insert");
  auto const parsed_clear = ava::tui::parse_key_name("Clear");
  auto const parsed_alt_backspace = ava::tui::parse_key_name("Alt+Backspace");
  auto const parsed_alt_d = ava::tui::parse_key_name("Alt+D");
  auto const parsed_alt_delete = ava::tui::parse_key_name("Alt+Delete");
  auto const parsed_alt_h = ava::tui::parse_key_name("Alt+H");
  auto const parsed_alt_j = ava::tui::parse_key_name("Alt+J");
  auto const parsed_alt_k = ava::tui::parse_key_name("Alt+K");
  auto const parsed_alt_l = ava::tui::parse_key_name("Alt+L");
  auto const parsed_alt_w = ava::tui::parse_key_name("Alt+W");
  auto const parsed_home = ava::tui::parse_key_name("Home");
  auto const parsed_end = ava::tui::parse_key_name("End");
  auto const parsed_ctrl_home = ava::tui::parse_key_name("Ctrl+Home");
  auto const parsed_ctrl_end = ava::tui::parse_key_name("Ctrl+End");
  auto const parsed_shift_home = ava::tui::parse_key_name("Shift+Home");
  auto const parsed_shift_end = ava::tui::parse_key_name("Shift+End");
  auto const parsed_shift_ctrl_home = ava::tui::parse_key_name("Shift+Ctrl+Home");
  auto const parsed_ctrl_shift_end = ava::tui::parse_key_name("Ctrl+Shift+End");
  auto const parsed_f2 = ava::tui::parse_key_name("F2");
  auto const parsed_f12 = ava::tui::parse_key_name("F12");
  expect(
      key_bindings && ava::tui::action_for_key(*key_bindings, ava::tui::terminal::Key::CtrlT) == ava::tui::TuiAction::Submit &&
          ava::tui::action_for_key(*key_bindings, ava::tui::terminal::Key::CtrlD) == ava::tui::TuiAction::VariantCycle &&
          ava::tui::key_matches_action(*key_bindings, ava::tui::TuiAction::VariantCycle, ava::tui::terminal::Key::ShiftTab) &&
          ava::tui::key_matches_action(*key_bindings, ava::tui::TuiAction::CursorWordLeft, ava::tui::terminal::Key::CtrlArrowLeft) &&
          ava::tui::key_matches_action(*key_bindings, ava::tui::TuiAction::CursorWordLeft, ava::tui::terminal::Key::AltArrowLeft) &&
          ava::tui::key_matches_action(*key_bindings, ava::tui::TuiAction::CursorWordLeft, ava::tui::terminal::Key::AltB) &&
          ava::tui::key_matches_action(*key_bindings, ava::tui::TuiAction::CursorWordRight, ava::tui::terminal::Key::CtrlArrowRight) &&
          ava::tui::key_matches_action(*key_bindings, ava::tui::TuiAction::CursorWordRight, ava::tui::terminal::Key::AltArrowRight) &&
          ava::tui::key_matches_action(*key_bindings, ava::tui::TuiAction::CursorWordRight, ava::tui::terminal::Key::AltF) &&
          ava::tui::key_matches_action(*key_bindings, ava::tui::TuiAction::JumpForward, ava::tui::terminal::Key::CtrlRightBracket) &&
          ava::tui::key_matches_action(*key_bindings, ava::tui::TuiAction::JumpBackward, ava::tui::terminal::Key::CtrlAltRightBracket) &&
          ava::tui::key_matches_action(*key_bindings, ava::tui::TuiAction::DeleteForward, ava::tui::terminal::Key::Delete) &&
          ava::tui::key_matches_action(*key_bindings, ava::tui::TuiAction::DeleteWordBackward, ava::tui::terminal::Key::AltBackspace) &&
          ava::tui::key_matches_action(*key_bindings, ava::tui::TuiAction::DeleteWordForward, ava::tui::terminal::Key::AltD) &&
          ava::tui::key_matches_action(*key_bindings, ava::tui::TuiAction::DeleteWordForward, ava::tui::terminal::Key::AltDelete) &&
          ava::tui::key_matches_action(*key_bindings, ava::tui::TuiAction::NewLine, ava::tui::terminal::Key::CtrlEnter) &&
          !ava::tui::key_matches_action(*key_bindings, ava::tui::TuiAction::NewLine, ava::tui::terminal::Key::AltEnter) &&
          ava::tui::key_matches_action(*key_bindings, ava::tui::TuiAction::MessageFollowUp, ava::tui::terminal::Key::AltEnter) &&
          ava::tui::key_matches_action(*key_bindings, ava::tui::TuiAction::Undo, ava::tui::terminal::Key::CtrlMinus) &&
          ava::tui::key_matches_action(*key_bindings, ava::tui::TuiAction::Suspend, ava::tui::terminal::Key::CtrlZ) &&
          ava::tui::key_matches_action(*key_bindings, ava::tui::TuiAction::ClipboardPasteImage, ava::tui::terminal::Key::CtrlV) &&
          ava::tui::key_matches_action(*key_bindings, ava::tui::TuiAction::Redo, ava::tui::terminal::Key::CtrlR) &&
          ava::tui::key_matches_action(*key_bindings, ava::tui::TuiAction::DetailsToggle, ava::tui::terminal::Key::CtrlO) &&
          ava::tui::key_matches_action(*key_bindings, ava::tui::TuiAction::ModelSelect, ava::tui::terminal::Key::CtrlL) &&
          ava::tui::key_matches_action(*key_bindings, ava::tui::TuiAction::ModelCycleForward, ava::tui::terminal::Key::CtrlP) &&
          ava::tui::key_matches_action(*key_bindings, ava::tui::TuiAction::ModelCycleBackward, ava::tui::terminal::Key::CtrlShiftP) &&
          ava::tui::key_matches_action(*key_bindings, ava::tui::TuiAction::ModelsSave, ava::tui::terminal::Key::CtrlS) &&
          ava::tui::key_matches_action(*key_bindings, ava::tui::TuiAction::ModelsEnableAll, ava::tui::terminal::Key::CtrlA) &&
          ava::tui::key_matches_action(*key_bindings, ava::tui::TuiAction::ModelsClearAll, ava::tui::terminal::Key::CtrlX) &&
          ava::tui::key_matches_action(*key_bindings, ava::tui::TuiAction::ModelsToggleProvider, ava::tui::terminal::Key::CtrlP) &&
          ava::tui::key_matches_action(*key_bindings, ava::tui::TuiAction::ModelsReorderUp, ava::tui::terminal::Key::AltArrowUp) &&
          ava::tui::key_matches_action(*key_bindings, ava::tui::TuiAction::ModelsReorderDown, ava::tui::terminal::Key::AltArrowDown) &&
          ava::tui::key_matches_action(*key_bindings, ava::tui::TuiAction::MessageDequeue, ava::tui::terminal::Key::AltArrowUp) && parsed_ctrl_s &&
          ava::tui::key_display(*parsed_ctrl_s) == "Ctrl+S" && parsed_ctrl_l && ava::tui::key_display(*parsed_ctrl_l) == "Ctrl+L" && parsed_ctrl_shift_p &&
          ava::tui::key_display(*parsed_ctrl_shift_p) == "Shift+Ctrl+P" && parsed_ctrl_g && ava::tui::key_display(*parsed_ctrl_g) == "Ctrl+G" &&
          parsed_alt_up && ava::tui::key_display(*parsed_alt_up) == "Alt+Up" && parsed_alt_down && ava::tui::key_display(*parsed_alt_down) == "Alt+Down" &&
          parsed_ctrl_x && ava::tui::key_display(*parsed_ctrl_x) == "Ctrl+X" && parsed_ctrl_enter &&
          ava::tui::key_display(*parsed_ctrl_enter) == "Ctrl+Enter" && parsed_alt_enter && ava::tui::key_display(*parsed_alt_enter) == "Alt+Enter" &&
          parsed_space && ava::tui::key_display(*parsed_space) == "Space" && parsed_ctrl_space && ava::tui::key_display(*parsed_ctrl_space) == "Ctrl+Space" &&
          parsed_ctrl_1 && ava::tui::key_display(*parsed_ctrl_1) == "Ctrl+1" && parsed_ctrl_9 && ava::tui::key_display(*parsed_ctrl_9) == "Ctrl+9" &&
          parsed_shift_tab && ava::tui::key_display(*parsed_shift_tab) == "Shift+Tab" && parsed_shift_l &&
          ava::tui::key_display(*parsed_shift_l) == "Shift+L" && parsed_shift_t && ava::tui::key_display(*parsed_shift_t) == "Shift+T" && parsed_ctrl_minus &&
          ava::tui::key_display(*parsed_ctrl_minus) == "Ctrl+-" && parsed_ctrl_slash && ava::tui::key_display(*parsed_ctrl_slash) == "Ctrl+/" &&
          parsed_ctrl_o && ava::tui::key_display(*parsed_ctrl_o) == "Ctrl+O" && parsed_ctrl_v && ava::tui::key_display(*parsed_ctrl_v) == "Ctrl+V" &&
          parsed_ctrl_right_bracket && ava::tui::key_display(*parsed_ctrl_right_bracket) == "Ctrl+]" && parsed_ctrl_alt_right_bracket &&
          ava::tui::key_display(*parsed_ctrl_alt_right_bracket) == "Ctrl+Alt+]" && parsed_shift_up && ava::tui::key_display(*parsed_shift_up) == "Shift+Up" &&
          parsed_shift_down && ava::tui::key_display(*parsed_shift_down) == "Shift+Down" && parsed_shift_left &&
          ava::tui::key_display(*parsed_shift_left) == "Shift+Left" && parsed_shift_right && ava::tui::key_display(*parsed_shift_right) == "Shift+Right" &&
          parsed_shift_ctrl_left && ava::tui::key_display(*parsed_shift_ctrl_left) == "Shift+Ctrl+Left" && parsed_ctrl_shift_right &&
          ava::tui::key_display(*parsed_ctrl_shift_right) == "Shift+Ctrl+Right" && parsed_shift_alt_left &&
          ava::tui::key_display(*parsed_shift_alt_left) == "Shift+Alt+Left" && parsed_alt_shift_right &&
          ava::tui::key_display(*parsed_alt_shift_right) == "Shift+Alt+Right" && parsed_alt_left && ava::tui::key_display(*parsed_alt_left) == "Alt+Left" &&
          parsed_alt_right && ava::tui::key_display(*parsed_alt_right) == "Alt+Right" && parsed_shift_backspace &&
          ava::tui::key_display(*parsed_shift_backspace) == "Shift+Backspace" && parsed_ctrl_backspace &&
          ava::tui::key_display(*parsed_ctrl_backspace) == "Ctrl+Backspace" && parsed_shift_delete &&
          ava::tui::key_display(*parsed_shift_delete) == "Shift+Delete" && parsed_insert && ava::tui::key_display(*parsed_insert) == "Insert" && parsed_clear &&
          ava::tui::key_display(*parsed_clear) == "Clear" && parsed_alt_backspace && ava::tui::key_display(*parsed_alt_backspace) == "Alt+Backspace" &&
          parsed_alt_d && ava::tui::key_display(*parsed_alt_d) == "Alt+D" && parsed_alt_delete && ava::tui::key_display(*parsed_alt_delete) == "Alt+Delete" &&
          parsed_alt_h && ava::tui::key_display(*parsed_alt_h) == "Alt+H" && parsed_alt_j && ava::tui::key_display(*parsed_alt_j) == "Alt+J" && parsed_alt_k &&
          ava::tui::key_display(*parsed_alt_k) == "Alt+K" && parsed_alt_l && ava::tui::key_display(*parsed_alt_l) == "Alt+L" && parsed_alt_w &&
          ava::tui::key_display(*parsed_alt_w) == "Alt+W" && parsed_home && ava::tui::key_display(*parsed_home) == "Home" && parsed_end &&
          ava::tui::key_display(*parsed_end) == "End" && parsed_ctrl_home && ava::tui::key_display(*parsed_ctrl_home) == "Ctrl+Home" && parsed_ctrl_end &&
          ava::tui::key_display(*parsed_ctrl_end) == "Ctrl+End" && parsed_shift_home && ava::tui::key_display(*parsed_shift_home) == "Shift+Home" &&
          parsed_shift_end && ava::tui::key_display(*parsed_shift_end) == "Shift+End" && parsed_shift_ctrl_home &&
          ava::tui::key_display(*parsed_shift_ctrl_home) == "Shift+Ctrl+Home" && parsed_ctrl_shift_end &&
          ava::tui::key_display(*parsed_ctrl_shift_end) == "Shift+Ctrl+End" && parsed_f2 && ava::tui::key_display(*parsed_f2) == "F2" && parsed_f12 &&
          ava::tui::key_display(*parsed_f12) == "F12" && ava::tui::parse_key_name("Ctrl+H") == ava::tui::terminal::Key::CtrlH &&
          ava::tui::key_display(ava::tui::terminal::Key::CtrlH) == "Ctrl+H" &&
          ava::tui::key_matches_action(*key_bindings, ava::tui::TuiAction::YankPop, ava::tui::terminal::Key::AltY) &&
          ava::tui::key_matches_action(*key_bindings, ava::tui::TuiAction::DeleteToLineStart, ava::tui::terminal::Key::CtrlU) &&
          ava::tui::key_matches_action(*key_bindings, ava::tui::TuiAction::AutocompleteAccept, ava::tui::terminal::Key::Tab) &&
          ava::tui::keys_display(*key_bindings, ava::tui::TuiAction::CursorWordRight).find("Ctrl+Right") != std::string::npos &&
          ava::tui::keys_display(*key_bindings, ava::tui::TuiAction::Submit).find("Ctrl+T") != std::string::npos,
      "tui keybind parser maps configured keys to semantic actions and display text");
  auto const function_key_bindings = ava::tui::parse_key_bindings_json("{\"tui.editor.cursorLineEnd\":\"F2\"}");
  expect(function_key_bindings && ava::tui::key_matches_action(*function_key_bindings, ava::tui::TuiAction::CursorLineEnd, ava::tui::terminal::Key::F2),
         "tui keybind parser accepts Pi-style function keys for configurable actions");
  auto const special_key_bindings = ava::tui::parse_key_bindings_json("{\"tui.editor.cursorLineStart\":\"Insert\",\"tui.editor.cursorLineEnd\":\"Clear\"}");
  expect(special_key_bindings && ava::tui::key_matches_action(*special_key_bindings, ava::tui::TuiAction::CursorLineStart, ava::tui::terminal::Key::Insert) &&
             ava::tui::key_matches_action(*special_key_bindings, ava::tui::TuiAction::CursorLineEnd, ava::tui::terminal::Key::Clear),
         "tui keybind parser accepts Pi-style Insert and Clear special keys");
  auto const ctrl_digit_bindings = ava::tui::parse_key_bindings_json("{\"tui.editor.cursorLineEnd\":\"Ctrl+1\"}");
  expect(ctrl_digit_bindings && ava::tui::key_matches_action(*ctrl_digit_bindings, ava::tui::TuiAction::CursorLineEnd, ava::tui::terminal::Key::Ctrl1),
         "tui keybind parser accepts Pi-style Ctrl+digit special keys");
  auto const prompt_navigation_bindings = ava::tui::parse_key_bindings_json(
      "{\"app.transcript.copyLatestAssistant\":\"F7\",\"promptStash\":\"F8\","
      "\"transcriptHalfPageUp\":\"F9\",\"halfPageDown\":\"F10\"}");
  expect(prompt_navigation_bindings && ava::tui::key_matches_action(*prompt_navigation_bindings, ava::tui::TuiAction::CopyLatestAssistant, ava::tui::terminal::Key::F7) &&
             ava::tui::key_matches_action(*prompt_navigation_bindings, ava::tui::TuiAction::PromptStash, ava::tui::terminal::Key::F8) &&
             ava::tui::key_matches_action(*prompt_navigation_bindings, ava::tui::TuiAction::TranscriptHalfPageUp, ava::tui::terminal::Key::F9) &&
             ava::tui::key_matches_action(*prompt_navigation_bindings, ava::tui::TuiAction::TranscriptHalfPageDown, ava::tui::terminal::Key::F10) &&
             ava::tui::key_binding_config_action_id(ava::tui::TuiAction::CopyLatestAssistant) == "app.transcript.copyLatestAssistant" &&
             ava::tui::key_binding_config_action_id(ava::tui::TuiAction::PromptStash) == "app.prompt.stash" &&
             ava::tui::key_binding_config_action_id(ava::tui::TuiAction::TranscriptHalfPageUp) == "app.transcript.halfPageUp" &&
             ava::tui::key_binding_config_action_id(ava::tui::TuiAction::TranscriptHalfPageDown) == "app.transcript.halfPageDown",
         "tui keybind parser accepts canonical and supported aliases for copy, prompt stash, and transcript half-page actions");
  auto const default_bindings = ava::tui::default_key_bindings();
  expect(!ava::tui::key_matches_action(default_bindings, ava::tui::TuiAction::HistoryPrev, ava::tui::terminal::Key::ArrowUp) &&
             !ava::tui::key_matches_action(default_bindings, ava::tui::TuiAction::HistoryNext, ava::tui::terminal::Key::ArrowDown) &&
             ava::tui::key_matches_action(default_bindings, ava::tui::TuiAction::NewLine, ava::tui::terminal::Key::ShiftEnter) &&
             ava::tui::key_matches_action(default_bindings, ava::tui::TuiAction::NewLine, ava::tui::terminal::Key::CtrlEnter) &&
             !ava::tui::key_matches_action(default_bindings, ava::tui::TuiAction::NewLine, ava::tui::terminal::Key::AltEnter) &&
             ava::tui::key_matches_action(default_bindings, ava::tui::TuiAction::MessageFollowUp, ava::tui::terminal::Key::AltEnter) &&
             ava::tui::key_matches_action(default_bindings, ava::tui::TuiAction::PalettePrev, ava::tui::terminal::Key::ArrowUp) &&
             ava::tui::key_matches_action(default_bindings, ava::tui::TuiAction::SelectPrev, ava::tui::terminal::Key::ArrowUp) &&
             ava::tui::key_matches_action(default_bindings, ava::tui::TuiAction::SelectNext, ava::tui::terminal::Key::ArrowDown) &&
             ava::tui::key_matches_action(default_bindings, ava::tui::TuiAction::SelectPageUp, ava::tui::terminal::Key::PageUp) &&
             ava::tui::key_matches_action(default_bindings, ava::tui::TuiAction::SelectPageDown, ava::tui::terminal::Key::PageDown) &&
             ava::tui::key_matches_action(default_bindings, ava::tui::TuiAction::SelectConfirm, ava::tui::terminal::Key::Enter) &&
             ava::tui::key_matches_action(default_bindings, ava::tui::TuiAction::SelectCancel, ava::tui::terminal::Key::Escape) &&
             ava::tui::key_matches_action(default_bindings, ava::tui::TuiAction::SelectCancel, ava::tui::terminal::Key::CtrlC) &&
             ava::tui::key_matches_action(default_bindings, ava::tui::TuiAction::SessionTogglePath, ava::tui::terminal::Key::CtrlP) &&
             ava::tui::key_matches_action(default_bindings, ava::tui::TuiAction::SessionToggleSort, ava::tui::terminal::Key::CtrlS) &&
             ava::tui::key_matches_action(default_bindings, ava::tui::TuiAction::SessionToggleSort, ava::tui::terminal::Key::CtrlT) &&
             ava::tui::key_matches_action(default_bindings, ava::tui::TuiAction::SessionToggleNamedFilter, ava::tui::terminal::Key::CtrlN) &&
             ava::tui::key_matches_action(default_bindings, ava::tui::TuiAction::SessionRename, ava::tui::terminal::Key::CtrlR) &&
             ava::tui::key_matches_action(default_bindings, ava::tui::TuiAction::SessionArchive, ava::tui::terminal::Key::CtrlD) &&
             ava::tui::key_matches_action(default_bindings, ava::tui::TuiAction::SessionArchiveNoninvasive, ava::tui::terminal::Key::CtrlBackspace) &&
             ava::tui::key_matches_action(default_bindings, ava::tui::TuiAction::TreeFoldOrUp, ava::tui::terminal::Key::CtrlArrowLeft) &&
             ava::tui::key_matches_action(default_bindings, ava::tui::TuiAction::TreeFoldOrUp, ava::tui::terminal::Key::AltArrowLeft) &&
             ava::tui::key_matches_action(default_bindings, ava::tui::TuiAction::TreeUnfoldOrDown, ava::tui::terminal::Key::CtrlArrowRight) &&
             ava::tui::key_matches_action(default_bindings, ava::tui::TuiAction::TreeUnfoldOrDown, ava::tui::terminal::Key::AltArrowRight) &&
             ava::tui::key_matches_action(default_bindings, ava::tui::TuiAction::TreeEditLabel, ava::tui::terminal::Key::ShiftL) &&
             ava::tui::key_matches_action(default_bindings, ava::tui::TuiAction::TreeToggleLabelTimestamp, ava::tui::terminal::Key::ShiftT) &&
             !ava::tui::key_matches_action(default_bindings, ava::tui::TuiAction::CursorUp, ava::tui::terminal::Key::ArrowUp) &&
             !ava::tui::key_matches_action(default_bindings, ava::tui::TuiAction::CursorDown, ava::tui::terminal::Key::ArrowDown) &&
             !ava::tui::key_matches_action(default_bindings, ava::tui::TuiAction::HistoryPrev, ava::tui::terminal::Key::AltK) &&
             !ava::tui::key_matches_action(default_bindings, ava::tui::TuiAction::HistoryNext, ava::tui::terminal::Key::AltJ) &&
             !ava::tui::key_matches_action(default_bindings, ava::tui::TuiAction::CursorUp, ava::tui::terminal::Key::AltK) &&
             !ava::tui::key_matches_action(default_bindings, ava::tui::TuiAction::CursorDown, ava::tui::terminal::Key::AltJ) &&
             ava::tui::key_matches_action(default_bindings, ava::tui::TuiAction::MessagePrev, ava::tui::terminal::Key::AltK) &&
             ava::tui::key_matches_action(default_bindings, ava::tui::TuiAction::MessageNext, ava::tui::terminal::Key::AltJ) &&
             ava::tui::key_matches_action(default_bindings, ava::tui::TuiAction::JumpToBottom, ava::tui::terminal::Key::CtrlEnd) &&
             !ava::tui::key_matches_action(default_bindings, ava::tui::TuiAction::SelectPrev, ava::tui::terminal::Key::AltK) &&
             !ava::tui::key_matches_action(default_bindings, ava::tui::TuiAction::SelectNext, ava::tui::terminal::Key::AltJ) &&
             !ava::tui::key_matches_action(default_bindings, ava::tui::TuiAction::SelectConfirm, ava::tui::terminal::Key::CtrlEnd) &&
             ava::tui::key_matches_action(default_bindings, ava::tui::TuiAction::CursorWordLeft, ava::tui::terminal::Key::CtrlArrowLeft) &&
             ava::tui::key_matches_action(default_bindings, ava::tui::TuiAction::CursorWordLeft, ava::tui::terminal::Key::AltArrowLeft) &&
             ava::tui::key_matches_action(default_bindings, ava::tui::TuiAction::CursorWordLeft, ava::tui::terminal::Key::AltB) &&
             ava::tui::key_matches_action(default_bindings, ava::tui::TuiAction::CursorWordRight, ava::tui::terminal::Key::CtrlArrowRight) &&
             ava::tui::key_matches_action(default_bindings, ava::tui::TuiAction::CursorWordRight, ava::tui::terminal::Key::AltArrowRight) &&
             ava::tui::key_matches_action(default_bindings, ava::tui::TuiAction::CursorWordRight, ava::tui::terminal::Key::AltF) &&
             ava::tui::key_matches_action(default_bindings, ava::tui::TuiAction::JumpForward, ava::tui::terminal::Key::CtrlRightBracket) &&
             ava::tui::key_matches_action(default_bindings, ava::tui::TuiAction::JumpBackward, ava::tui::terminal::Key::CtrlAltRightBracket) &&
             ava::tui::key_matches_action(default_bindings, ava::tui::TuiAction::CursorLineStart, ava::tui::terminal::Key::Home) &&
             ava::tui::key_matches_action(default_bindings, ava::tui::TuiAction::CursorLineEnd, ava::tui::terminal::Key::End) &&
             ava::tui::key_matches_action(default_bindings, ava::tui::TuiAction::DeleteForward, ava::tui::terminal::Key::Delete) &&
             ava::tui::key_matches_action(default_bindings, ava::tui::TuiAction::DeleteForward, ava::tui::terminal::Key::ShiftDelete) &&
             ava::tui::key_matches_action(default_bindings, ava::tui::TuiAction::DeleteForward, ava::tui::terminal::Key::CtrlD) &&
             ava::tui::key_matches_action(default_bindings, ava::tui::TuiAction::DeleteBackward, ava::tui::terminal::Key::ShiftBackspace) &&
             ava::tui::key_matches_action(default_bindings, ava::tui::TuiAction::DeleteBackward, ava::tui::terminal::Key::CtrlH) &&
             ava::tui::key_matches_action(default_bindings, ava::tui::TuiAction::Exit, ava::tui::terminal::Key::CtrlD) &&
             ava::tui::key_matches_action(default_bindings, ava::tui::TuiAction::DeleteWordBackward, ava::tui::terminal::Key::AltBackspace) &&
             ava::tui::key_matches_action(default_bindings, ava::tui::TuiAction::DeleteWordForward, ava::tui::terminal::Key::AltD) &&
             ava::tui::key_matches_action(default_bindings, ava::tui::TuiAction::DeleteWordForward, ava::tui::terminal::Key::AltDelete) &&
             ava::tui::key_matches_action(default_bindings, ava::tui::TuiAction::ModeToggle, ava::tui::terminal::Key::Tab) &&
             ava::tui::key_matches_action(default_bindings, ava::tui::TuiAction::AutocompleteAccept, ava::tui::terminal::Key::Tab) &&
             !ava::tui::key_matches_action(default_bindings, ava::tui::TuiAction::Undo, ava::tui::terminal::Key::CtrlZ) &&
             ava::tui::key_matches_action(default_bindings, ava::tui::TuiAction::Undo, ava::tui::terminal::Key::CtrlMinus) &&
             ava::tui::key_matches_action(default_bindings, ava::tui::TuiAction::Suspend, ava::tui::terminal::Key::CtrlZ) &&
             ava::tui::key_matches_action(default_bindings, ava::tui::TuiAction::ClipboardPasteImage, ava::tui::terminal::Key::CtrlV) &&
             ava::tui::key_matches_action(default_bindings, ava::tui::TuiAction::Redo, ava::tui::terminal::Key::CtrlR) &&
             ava::tui::key_matches_action(default_bindings, ava::tui::TuiAction::Yank, ava::tui::terminal::Key::CtrlY) &&
             ava::tui::key_matches_action(default_bindings, ava::tui::TuiAction::YankPop, ava::tui::terminal::Key::AltY) &&
             ava::tui::key_matches_action(default_bindings, ava::tui::TuiAction::DetailsToggle, ava::tui::terminal::Key::CtrlO) &&
             ava::tui::key_matches_action(default_bindings, ava::tui::TuiAction::CopySelection, ava::tui::terminal::Key::CtrlC) &&
             ava::tui::key_matches_action(default_bindings, ava::tui::TuiAction::ClearInput, ava::tui::terminal::Key::CtrlC) &&
             ava::tui::key_matches_action(default_bindings, ava::tui::TuiAction::CopyLatestAssistant, ava::tui::terminal::Key::F5) &&
             ava::tui::key_matches_action(default_bindings, ava::tui::TuiAction::PromptStash, ava::tui::terminal::Key::F6) &&
             ava::tui::keys_display(default_bindings, ava::tui::TuiAction::TranscriptHalfPageUp).empty() &&
             ava::tui::keys_display(default_bindings, ava::tui::TuiAction::TranscriptHalfPageDown).empty() &&
             !ava::tui::key_matches_action(default_bindings, ava::tui::TuiAction::TranscriptHalfPageUp, ava::tui::terminal::Key::CtrlU) &&
             !ava::tui::key_matches_action(default_bindings, ava::tui::TuiAction::TranscriptHalfPageDown, ava::tui::terminal::Key::CtrlD) &&
             ava::tui::key_matches_action(default_bindings, ava::tui::TuiAction::ExternalEditor, ava::tui::terminal::Key::CtrlG) &&
             ava::tui::key_matches_action(default_bindings, ava::tui::TuiAction::VariantCycle, ava::tui::terminal::Key::ShiftTab) &&
             !ava::tui::key_matches_action(default_bindings, ava::tui::TuiAction::VariantCycle, ava::tui::terminal::Key::CtrlT) &&
             ava::tui::key_matches_action(default_bindings, ava::tui::TuiAction::ReasoningSelect, ava::tui::terminal::Key::CtrlT) &&
             !ava::tui::key_matches_action(default_bindings, ava::tui::TuiAction::ThinkingToggle, ava::tui::terminal::Key::CtrlT) &&
             ava::tui::key_matches_action(default_bindings, ava::tui::TuiAction::ModelSelect, ava::tui::terminal::Key::CtrlL) &&
             ava::tui::key_matches_action(default_bindings, ava::tui::TuiAction::ModelCycleForward, ava::tui::terminal::Key::CtrlP) &&
             ava::tui::key_matches_action(default_bindings, ava::tui::TuiAction::ModelCycleBackward, ava::tui::terminal::Key::CtrlShiftP) &&
             ava::tui::key_matches_action(default_bindings, ava::tui::TuiAction::ModelsSave, ava::tui::terminal::Key::CtrlS) &&
             ava::tui::key_matches_action(default_bindings, ava::tui::TuiAction::ModelsEnableAll, ava::tui::terminal::Key::CtrlA) &&
             ava::tui::key_matches_action(default_bindings, ava::tui::TuiAction::ModelsClearAll, ava::tui::terminal::Key::CtrlX) &&
             ava::tui::key_matches_action(default_bindings, ava::tui::TuiAction::ModelsToggleProvider, ava::tui::terminal::Key::CtrlP) &&
             ava::tui::key_matches_action(default_bindings, ava::tui::TuiAction::ModelsReorderUp, ava::tui::terminal::Key::AltArrowUp) &&
             ava::tui::key_matches_action(default_bindings, ava::tui::TuiAction::ModelsReorderDown, ava::tui::terminal::Key::AltArrowDown) &&
             ava::tui::key_matches_action(default_bindings, ava::tui::TuiAction::MessageDequeue, ava::tui::terminal::Key::AltArrowUp) &&
             ava::tui::key_matches_action(default_bindings, ava::tui::TuiAction::Interrupt, ava::tui::terminal::Key::CtrlC),
         "tui default keybinds preserve context-specific semantic actions for shared keys");
  auto const navigation_key_bindings =
      ava::tui::parse_key_bindings_json("{\"message_prev\":\"PageUp\",\"message_next\":\"PageDown\",\"jump_to_bottom\":\"Ctrl+T\"}");
  expect(
      navigation_key_bindings && ava::tui::key_matches_action(*navigation_key_bindings, ava::tui::TuiAction::MessagePrev, ava::tui::terminal::Key::PageUp) &&
          ava::tui::key_matches_action(*navigation_key_bindings, ava::tui::TuiAction::MessageNext, ava::tui::terminal::Key::PageDown) &&
          ava::tui::key_matches_action(*navigation_key_bindings, ava::tui::TuiAction::JumpToBottom, ava::tui::terminal::Key::CtrlT) &&
          ava::tui::action_name(ava::tui::TuiAction::CursorUp) == "cursor_up" && ava::tui::action_name(ava::tui::TuiAction::CursorDown) == "cursor_down" &&
          ava::tui::action_name(ava::tui::TuiAction::JumpForward) == "jump_forward" &&
          ava::tui::action_name(ava::tui::TuiAction::JumpBackward) == "jump_backward" &&
          ava::tui::action_name(ava::tui::TuiAction::DeleteWordForward) == "delete_word_forward" &&
          ava::tui::action_name(ava::tui::TuiAction::SelectPrev) == "select_prev" && ava::tui::action_name(ava::tui::TuiAction::SelectNext) == "select_next" &&
          ava::tui::action_name(ava::tui::TuiAction::SelectPageUp) == "select_page_up" &&
          ava::tui::action_name(ava::tui::TuiAction::SelectPageDown) == "select_page_down" &&
          ava::tui::action_name(ava::tui::TuiAction::SelectConfirm) == "select_confirm" &&
          ava::tui::action_name(ava::tui::TuiAction::SelectCancel) == "select_cancel" &&
          ava::tui::action_name(ava::tui::TuiAction::CopySelection) == "copy_selection" &&
          ava::tui::action_name(ava::tui::TuiAction::CopyLatestAssistant) == "copy_latest_assistant" &&
          ava::tui::action_name(ava::tui::TuiAction::PromptStash) == "prompt_stash" &&
          ava::tui::action_name(ava::tui::TuiAction::TranscriptHalfPageUp) == "transcript_half_page_up" &&
          ava::tui::action_name(ava::tui::TuiAction::TranscriptHalfPageDown) == "transcript_half_page_down" &&
          ava::tui::action_name(ava::tui::TuiAction::ExternalEditor) == "external_editor" && ava::tui::action_name(ava::tui::TuiAction::Suspend) == "suspend" &&
          ava::tui::action_name(ava::tui::TuiAction::ClipboardPasteImage) == "clipboard_paste_image" &&
          ava::tui::action_name(ava::tui::TuiAction::ModelSelect) == "model_select" &&
          ava::tui::action_name(ava::tui::TuiAction::ModelCycleForward) == "model_cycle_forward" &&
          ava::tui::action_name(ava::tui::TuiAction::ModelCycleBackward) == "model_cycle_backward" &&
          ava::tui::action_name(ava::tui::TuiAction::ModelsSave) == "models_save" &&
          ava::tui::action_name(ava::tui::TuiAction::ModelsEnableAll) == "models_enable_all" &&
          ava::tui::action_name(ava::tui::TuiAction::ModelsClearAll) == "models_clear_all" &&
          ava::tui::action_name(ava::tui::TuiAction::ModelsToggleProvider) == "models_toggle_provider" &&
          ava::tui::action_name(ava::tui::TuiAction::ModelsReorderUp) == "models_reorder_up" &&
          ava::tui::action_name(ava::tui::TuiAction::ModelsReorderDown) == "models_reorder_down" &&
          ava::tui::action_name(ava::tui::TuiAction::ReasoningSelect) == "reasoning_select" &&
          ava::tui::action_name(ava::tui::TuiAction::ThinkingToggle) == "thinking_toggle" &&
          ava::tui::action_name(ava::tui::TuiAction::MessageFollowUp) == "message_follow_up" &&
          ava::tui::action_name(ava::tui::TuiAction::MessageDequeue) == "message_dequeue" &&
          ava::tui::action_name(ava::tui::TuiAction::MessagePrev) == "message_prev" &&
          ava::tui::action_name(ava::tui::TuiAction::MessageNext) == "message_next" &&
          ava::tui::action_name(ava::tui::TuiAction::JumpToBottom) == "jump_to_bottom" &&
          ava::tui::action_name(ava::tui::TuiAction::SessionNew) == "session_new" &&
          ava::tui::action_name(ava::tui::TuiAction::SessionTree) == "session_tree" &&
          ava::tui::action_name(ava::tui::TuiAction::SessionFork) == "session_fork" &&
          ava::tui::action_name(ava::tui::TuiAction::SessionResume) == "session_resume" &&
          ava::tui::action_name(ava::tui::TuiAction::SessionTogglePath) == "session_toggle_path" &&
          ava::tui::action_name(ava::tui::TuiAction::SessionToggleSort) == "session_toggle_sort" &&
          ava::tui::action_name(ava::tui::TuiAction::SessionToggleNamedFilter) == "session_toggle_named_filter" &&
          ava::tui::action_name(ava::tui::TuiAction::SessionRename) == "session_rename" &&
          ava::tui::action_name(ava::tui::TuiAction::SessionArchive) == "session_archive" &&
          ava::tui::action_name(ava::tui::TuiAction::SessionArchiveNoninvasive) == "session_archive_noninvasive" &&
          ava::tui::action_name(ava::tui::TuiAction::TreeFoldOrUp) == "tree_fold_or_up" &&
          ava::tui::action_name(ava::tui::TuiAction::TreeUnfoldOrDown) == "tree_unfold_or_down" &&
          ava::tui::action_name(ava::tui::TuiAction::TreeEditLabel) == "tree_edit_label" &&
          ava::tui::action_name(ava::tui::TuiAction::TreeToggleLabelTimestamp) == "tree_toggle_label_timestamp",
      "tui keybind parser exposes semantic action names for editor and navigation actions");
  auto const help_items = ava::tui::key_binding_help_items(default_bindings);
  expect(
      std::ranges::any_of(help_items,
                          [](ava::tui::TuiKeyBindingHelpItem const& item) {
                            return item.action == "variant_cycle" && item.keys.find("Shift+Tab") != std::string::npos &&
                                   item.keys.find("Ctrl+T") == std::string::npos;
                          }) &&
          std::ranges::any_of(
              help_items,
              [](ava::tui::TuiKeyBindingHelpItem const& item) { return item.action == "reasoning_select" && item.keys.find("Ctrl+T") != std::string::npos; }) &&
          std::ranges::none_of(
              help_items,
              [](ava::tui::TuiKeyBindingHelpItem const& item) { return item.action == "thinking_toggle" && item.keys.find("Ctrl+T") != std::string::npos; }) &&
          std::ranges::any_of(
              help_items,
              [](ava::tui::TuiKeyBindingHelpItem const& item) { return item.action == "model_select" && item.keys.find("Ctrl+L") != std::string::npos; }) &&
          std::ranges::any_of(
              help_items,
              [](ava::tui::TuiKeyBindingHelpItem const& item) { return item.action == "copy_selection" && item.keys.find("Ctrl+C") != std::string::npos; }) &&
          std::ranges::any_of(
              help_items,
              [](ava::tui::TuiKeyBindingHelpItem const& item) { return item.action == "clear_input" && item.keys.find("Ctrl+C") != std::string::npos; }) &&
          std::ranges::any_of(help_items,
                              [](ava::tui::TuiKeyBindingHelpItem const& item) {
                                return item.action == "copy_latest_assistant" && item.keys.find("F5") != std::string::npos;
                              }) &&
          std::ranges::any_of(
              help_items,
              [](ava::tui::TuiKeyBindingHelpItem const& item) { return item.action == "prompt_stash" && item.keys.find("F6") != std::string::npos; }) &&
          std::ranges::none_of(help_items, [](ava::tui::TuiKeyBindingHelpItem const& item) { return item.action == "transcript_half_page_up"; }) &&
          std::ranges::none_of(help_items, [](ava::tui::TuiKeyBindingHelpItem const& item) { return item.action == "transcript_half_page_down"; }) &&
          std::ranges::any_of(
              help_items,
              [](ava::tui::TuiKeyBindingHelpItem const& item) { return item.action == "external_editor" && item.keys.find("Ctrl+G") != std::string::npos; }) &&
          std::ranges::any_of(
              help_items,
              [](ava::tui::TuiKeyBindingHelpItem const& item) { return item.action == "suspend" && item.keys.find("Ctrl+Z") != std::string::npos; }) &&
          std::ranges::any_of(help_items,
                              [](ava::tui::TuiKeyBindingHelpItem const& item) {
                                return item.action == "clipboard_paste_image" && item.keys.find("Ctrl+V") != std::string::npos;
                              }) &&
          std::ranges::any_of(help_items,
                              [](ava::tui::TuiKeyBindingHelpItem const& item) {
                                return item.action == "model_cycle_forward" && item.keys.find("Ctrl+P") != std::string::npos;
                              }) &&
          std::ranges::any_of(help_items,
                              [](ava::tui::TuiKeyBindingHelpItem const& item) {
                                return item.action == "model_cycle_backward" && item.keys.find("Shift+Ctrl+P") != std::string::npos;
                              }) &&
          std::ranges::any_of(
              help_items,
              [](ava::tui::TuiKeyBindingHelpItem const& item) { return item.action == "models_clear_all" && item.keys.find("Ctrl+X") != std::string::npos; }) &&
          std::ranges::any_of(help_items,
                              [](ava::tui::TuiKeyBindingHelpItem const& item) {
                                return item.action == "message_follow_up" && item.keys.find("Alt+Enter") != std::string::npos;
                              }) &&
          std::ranges::any_of(
              help_items,
              [](ava::tui::TuiKeyBindingHelpItem const& item) { return item.action == "message_dequeue" && item.keys.find("Alt+Up") != std::string::npos; }) &&
          std::ranges::any_of(
              help_items,
              [](ava::tui::TuiKeyBindingHelpItem const& item) { return item.action == "message_prev" && item.keys.find("Alt+K") != std::string::npos; }) &&
          std::ranges::any_of(
              help_items,
              [](ava::tui::TuiKeyBindingHelpItem const& item) { return item.action == "message_next" && item.keys.find("Alt+J") != std::string::npos; }) &&
          std::ranges::any_of(
              help_items,
              [](ava::tui::TuiKeyBindingHelpItem const& item) { return item.action == "jump_to_bottom" && item.keys.find("Ctrl+End") != std::string::npos; }) &&
          std::ranges::none_of(help_items, [](ava::tui::TuiKeyBindingHelpItem const& item) { return item.action == "history_prev"; }) &&
          std::ranges::none_of(help_items, [](ava::tui::TuiKeyBindingHelpItem const& item) { return item.action == "history_next"; }) &&
          std::ranges::none_of(help_items, [](ava::tui::TuiKeyBindingHelpItem const& item) { return item.action == "cursor_up"; }) &&
          std::ranges::none_of(help_items, [](ava::tui::TuiKeyBindingHelpItem const& item) { return item.action == "cursor_down"; }) &&
          std::ranges::any_of(help_items,
                              [](ava::tui::TuiKeyBindingHelpItem const& item) {
                                return item.action == "session_toggle_path" && item.keys.find("Ctrl+P") != std::string::npos;
                              }) &&
          std::ranges::any_of(help_items,
                              [](ava::tui::TuiKeyBindingHelpItem const& item) {
                                return item.action == "session_toggle_sort" && item.keys.find("Ctrl+S") != std::string::npos &&
                                       item.keys.find("Ctrl+T") != std::string::npos;
                              }) &&
          std::ranges::any_of(
              help_items,
              [](ava::tui::TuiKeyBindingHelpItem const& item) { return item.action == "session_archive" && item.keys.find("Ctrl+D") != std::string::npos; }) &&
          std::ranges::any_of(help_items,
                              [](ava::tui::TuiKeyBindingHelpItem const& item) {
                                return item.action == "tree_fold_or_up" && item.keys.find("Ctrl+Left") != std::string::npos &&
                                       item.keys.find("Alt+Left") != std::string::npos;
                              }) &&
          std::ranges::any_of(help_items,
                              [](ava::tui::TuiKeyBindingHelpItem const& item) {
                                return item.action == "tree_unfold_or_down" && item.keys.find("Ctrl+Right") != std::string::npos &&
                                       item.keys.find("Alt+Right") != std::string::npos;
                              }) &&
          std::ranges::any_of(help_items,
                              [](ava::tui::TuiKeyBindingHelpItem const& item) {
                                return item.action == "delete_to_line_start" && item.keys.find("Ctrl+U") != std::string::npos;
                              }) &&
          std::ranges::any_of(
              help_items,
              [](ava::tui::TuiKeyBindingHelpItem const& item) { return item.action == "jump_forward" && item.keys.find("Ctrl+]") != std::string::npos; }) &&
          std::ranges::any_of(help_items,
                              [](ava::tui::TuiKeyBindingHelpItem const& item) {
                                return item.action == "jump_backward" && item.keys.find("Ctrl+Alt+]") != std::string::npos;
                              }) &&
          std::ranges::any_of(help_items,
                              [](ava::tui::TuiKeyBindingHelpItem const& item) {
                                return item.action == "delete_forward" && item.keys.find("Shift+Delete") != std::string::npos &&
                                       item.keys.find("Ctrl+D") != std::string::npos;
                              }) &&
          std::ranges::any_of(help_items,
                              [](ava::tui::TuiKeyBindingHelpItem const& item) {
                                return item.action == "delete_backward" && item.keys.find("Shift+Backspace") != std::string::npos;
                              }) &&
          std::ranges::any_of(help_items,
                              [](ava::tui::TuiKeyBindingHelpItem const& item) {
                                return item.action == "delete_word_backward" && item.keys.find("Alt+Backspace") != std::string::npos;
                              }) &&
          std::ranges::any_of(help_items,
                              [](ava::tui::TuiKeyBindingHelpItem const& item) {
                                return item.action == "delete_word_forward" && item.keys.find("Alt+D") != std::string::npos;
                              }) &&
          std::ranges::any_of(help_items,
                              [](ava::tui::TuiKeyBindingHelpItem const& item) {
                                return item.action == "delete_word_forward" && item.keys.find("Alt+Delete") != std::string::npos;
                              }) &&
          std::ranges::any_of(
              help_items, [](ava::tui::TuiKeyBindingHelpItem const& item) { return item.action == "undo" && item.keys.find("Ctrl+Z") == std::string::npos; }) &&
          std::ranges::any_of(
              help_items, [](ava::tui::TuiKeyBindingHelpItem const& item) { return item.action == "undo" && item.keys.find("Ctrl+-") != std::string::npos; }) &&
          std::ranges::any_of(
              help_items, [](ava::tui::TuiKeyBindingHelpItem const& item) { return item.action == "redo" && item.keys.find("Ctrl+R") != std::string::npos; }) &&
          std::ranges::any_of(
              help_items, [](ava::tui::TuiKeyBindingHelpItem const& item) { return item.action == "yank" && item.keys.find("Ctrl+Y") != std::string::npos; }) &&
          std::ranges::any_of(
              help_items,
              [](ava::tui::TuiKeyBindingHelpItem const& item) { return item.action == "yank_pop" && item.keys.find("Alt+Y") != std::string::npos; }) &&
          std::ranges::any_of(help_items,
                              [](ava::tui::TuiKeyBindingHelpItem const& item) {
                                return item.action == "session_archive_noninvasive" && item.keys.find("Ctrl+Backspace") != std::string::npos;
                              }),
      "tui keybind help lists concrete semantic action names and effective keys");
  expect(
      std::ranges::all_of(help_items,
                          [](ava::tui::TuiKeyBindingHelpItem const& item) {
                            return !item.label.empty() && item.label.size() <= 40 && item.label.find('_') == std::string::npos && !item.action.empty() &&
                                   item.action.find(' ') == std::string::npos;
                          }) &&
          std::ranges::any_of(help_items,
                              [](ava::tui::TuiKeyBindingHelpItem const& item) {
                                return item.action == "jump_to_bottom" && item.label == "Jump to live tail" && item.keys.find("Ctrl+End") != std::string::npos;
                              }) &&
          std::ranges::any_of(help_items,
                              [](ava::tui::TuiKeyBindingHelpItem const& item) {
                                return item.action == "message_prev" && item.label == "Previous user turn" && item.keys.find("Alt+K") != std::string::npos;
                              }) &&
          std::ranges::any_of(help_items,
                              [](ava::tui::TuiKeyBindingHelpItem const& item) {
                                return item.action == "message_next" && item.label == "Next user turn" && item.keys.find("Alt+J") != std::string::npos;
                              }) &&
          std::ranges::any_of(
              help_items, [](ava::tui::TuiKeyBindingHelpItem const& item) { return item.action == "mode_toggle" && item.label == "Toggle build/plan mode"; }),
      "tui keybind help items carry human primary labels beside stable machine action ids");
  {
    constexpr ava::tui::TuiAction kAllActions[] = {
        ava::tui::TuiAction::Submit,
        ava::tui::TuiAction::NewLine,
        ava::tui::TuiAction::Cancel,
        ava::tui::TuiAction::ClearInput,
        ava::tui::TuiAction::CopySelection,
        ava::tui::TuiAction::CopyLatestAssistant,
        ava::tui::TuiAction::PromptStash,
        ava::tui::TuiAction::ExternalEditor,
        ava::tui::TuiAction::Suspend,
        ava::tui::TuiAction::ClipboardPasteImage,
        ava::tui::TuiAction::DeleteBackward,
        ava::tui::TuiAction::DeleteForward,
        ava::tui::TuiAction::HistoryPrev,
        ava::tui::TuiAction::HistoryNext,
        ava::tui::TuiAction::PalettePrev,
        ava::tui::TuiAction::PaletteNext,
        ava::tui::TuiAction::SelectPrev,
        ava::tui::TuiAction::SelectNext,
        ava::tui::TuiAction::SelectPageUp,
        ava::tui::TuiAction::SelectPageDown,
        ava::tui::TuiAction::SelectConfirm,
        ava::tui::TuiAction::SelectCancel,
        ava::tui::TuiAction::CursorLeft,
        ava::tui::TuiAction::CursorRight,
        ava::tui::TuiAction::CursorUp,
        ava::tui::TuiAction::CursorDown,
        ava::tui::TuiAction::CursorLineStart,
        ava::tui::TuiAction::CursorLineEnd,
        ava::tui::TuiAction::CursorWordLeft,
        ava::tui::TuiAction::CursorWordRight,
        ava::tui::TuiAction::JumpForward,
        ava::tui::TuiAction::JumpBackward,
        ava::tui::TuiAction::DeleteWordBackward,
        ava::tui::TuiAction::DeleteWordForward,
        ava::tui::TuiAction::DeleteToLineStart,
        ava::tui::TuiAction::DeleteToLineEnd,
        ava::tui::TuiAction::Undo,
        ava::tui::TuiAction::Redo,
        ava::tui::TuiAction::Yank,
        ava::tui::TuiAction::YankPop,
        ava::tui::TuiAction::AutocompleteAccept,
        ava::tui::TuiAction::PromptAllow,
        ava::tui::TuiAction::PromptDeny,
        ava::tui::TuiAction::DetailsToggle,
        ava::tui::TuiAction::PageUp,
        ava::tui::TuiAction::PageDown,
        ava::tui::TuiAction::TranscriptHalfPageUp,
        ava::tui::TuiAction::TranscriptHalfPageDown,
        ava::tui::TuiAction::ModeToggle,
        ava::tui::TuiAction::Interrupt,
        ava::tui::TuiAction::Exit,
        ava::tui::TuiAction::VariantCycle,
        ava::tui::TuiAction::ReasoningSelect,
        ava::tui::TuiAction::ThinkingToggle,
        ava::tui::TuiAction::ModelSelect,
        ava::tui::TuiAction::ModelCycleForward,
        ava::tui::TuiAction::ModelCycleBackward,
        ava::tui::TuiAction::ModelsSave,
        ava::tui::TuiAction::ModelsEnableAll,
        ava::tui::TuiAction::ModelsClearAll,
        ava::tui::TuiAction::ModelsToggleProvider,
        ava::tui::TuiAction::ModelsReorderUp,
        ava::tui::TuiAction::ModelsReorderDown,
        ava::tui::TuiAction::MessageFollowUp,
        ava::tui::TuiAction::MessageDequeue,
        ava::tui::TuiAction::MessagePrev,
        ava::tui::TuiAction::MessageNext,
        ava::tui::TuiAction::JumpToBottom,
        ava::tui::TuiAction::SessionNew,
        ava::tui::TuiAction::SessionTree,
        ava::tui::TuiAction::SessionFork,
        ava::tui::TuiAction::SessionResume,
        ava::tui::TuiAction::SessionTogglePath,
        ava::tui::TuiAction::SessionToggleSort,
        ava::tui::TuiAction::SessionToggleNamedFilter,
        ava::tui::TuiAction::SessionRename,
        ava::tui::TuiAction::SessionArchive,
        ava::tui::TuiAction::SessionArchiveNoninvasive,
        ava::tui::TuiAction::TreeFoldOrUp,
        ava::tui::TuiAction::TreeUnfoldOrDown,
        ava::tui::TuiAction::TreeEditLabel,
        ava::tui::TuiAction::TreeToggleLabelTimestamp,
        ava::tui::TuiAction::TreeFilterLabeledOnly,
        ava::tui::TuiAction::TreeFilterAll,
    };
    bool labels_ok = true;
    for (auto const action : kAllActions)
    {
      auto const label = ava::tui::action_label(action);
      auto const name = ava::tui::action_name(action);
      if (label.empty() || label.size() > 40 || label.find('_') != std::string::npos || name.empty() || name == "unknown" ||
          name.find(' ') != std::string::npos)
      {
        labels_ok = false;
        break;
      }
    }
    expect(labels_ok && ava::tui::action_label(ava::tui::TuiAction::CursorLeft) == "Move cursor left" &&
               ava::tui::action_label(ava::tui::TuiAction::ModelCycleForward) == "Next model" &&
               ava::tui::action_name(ava::tui::TuiAction::JumpToBottom) == "jump_to_bottom" &&
               ava::tui::action_label(ava::tui::TuiAction::TreeFoldOrUp) == "Go to parent session" &&
               ava::tui::action_label(ava::tui::TuiAction::TreeUnfoldOrDown) == "Go to child session" &&
               ava::tui::action_label(ava::tui::TuiAction::SessionArchiveNoninvasive) == "Archive/restore (empty search)" &&
               ava::tui::action_label(ava::tui::TuiAction::TreeFilterAll) == "Toggle archived sessions",
           "tui action_label covers every TuiAction with nonempty bounded human text while action_name stays machine-stable");
  }
  expect(!ava::tui::parse_key_bindings_json("{\"submit\":\"Hyper+Enter\"}"), "tui keybind parser rejects unknown key names");
  expect(!ava::tui::parse_key_bindings_json("{\"submt\":\"Enter\"}"), "tui keybind parser rejects unknown action names");
  auto const escaped_action_keybinds = ava::tui::parse_key_bindings_json("{\"\\u0073\\u0075\\u0062\\u006d\\u0069\\u0074\":\"Ctrl+T\"}");
  expect(escaped_action_keybinds && ava::tui::key_matches_action(*escaped_action_keybinds, ava::tui::TuiAction::Submit, ava::tui::terminal::Key::CtrlT),
         "tui keybind parser accepts JSON unicode escapes in action names");
  auto const space_action_keybinds = ava::tui::parse_key_bindings_json("{\"tui.input.submit\":\"Space\"}");
  expect(space_action_keybinds && ava::tui::key_matches_action(*space_action_keybinds, ava::tui::TuiAction::Submit, ava::tui::terminal::Key::Space),
         "tui keybind parser accepts Pi-style Space key names for semantic bindings");
  auto const ctrl_space_action_keybinds = ava::tui::parse_key_bindings_json("{\"tui.input.submit\":\"Ctrl+Space\"}");
  expect(ctrl_space_action_keybinds && ava::tui::key_matches_action(*ctrl_space_action_keybinds, ava::tui::TuiAction::Submit, ava::tui::terminal::Key::CtrlSpace),
         "tui keybind parser accepts Pi-style Ctrl+Space key names for semantic bindings");
  auto const ctrl_slash_action_keybinds = ava::tui::parse_key_bindings_json("{\"tui.input.submit\":\"Ctrl+/\"}");
  expect(ctrl_slash_action_keybinds && ava::tui::key_matches_action(*ctrl_slash_action_keybinds, ava::tui::TuiAction::Submit, ava::tui::terminal::Key::CtrlSlash),
         "tui keybind parser accepts Pi-style Ctrl+/ key names for semantic bindings");
  auto const select_action_keybinds = ava::tui::parse_key_bindings_json(
      "{\"tui.select.confirm\":[\"Enter\",\"Space\"],\"tui.select.cancel\":[\"Escape\",\"Ctrl+W\"],"
      "\"tui.select.up\":\"Ctrl+P\",\"tui.select.down\":\"Ctrl+N\","
      "\"tui.select.pageUp\":\"Ctrl+O\",\"tui.select.pageDown\":\"Ctrl+Y\"}");
  expect(select_action_keybinds && ava::tui::key_matches_action(*select_action_keybinds, ava::tui::TuiAction::SelectConfirm, ava::tui::terminal::Key::Enter) &&
             ava::tui::key_matches_action(*select_action_keybinds, ava::tui::TuiAction::SelectConfirm, ava::tui::terminal::Key::Space) &&
             ava::tui::key_matches_action(*select_action_keybinds, ava::tui::TuiAction::Submit, ava::tui::terminal::Key::Enter) &&
             !ava::tui::key_matches_action(*select_action_keybinds, ava::tui::TuiAction::Submit, ava::tui::terminal::Key::Space) &&
             ava::tui::key_matches_action(*select_action_keybinds, ava::tui::TuiAction::SelectCancel, ava::tui::terminal::Key::Escape) &&
             ava::tui::key_matches_action(*select_action_keybinds, ava::tui::TuiAction::SelectCancel, ava::tui::terminal::Key::CtrlW) &&
             ava::tui::key_matches_action(*select_action_keybinds, ava::tui::TuiAction::Cancel, ava::tui::terminal::Key::Escape) &&
             ava::tui::key_matches_action(*select_action_keybinds, ava::tui::TuiAction::DeleteWordBackward, ava::tui::terminal::Key::CtrlW) &&
             ava::tui::key_matches_action(*select_action_keybinds, ava::tui::TuiAction::SelectPrev, ava::tui::terminal::Key::CtrlP) &&
             ava::tui::key_matches_action(*select_action_keybinds, ava::tui::TuiAction::SelectNext, ava::tui::terminal::Key::CtrlN) &&
             ava::tui::key_matches_action(*select_action_keybinds, ava::tui::TuiAction::SelectPageUp, ava::tui::terminal::Key::CtrlO) &&
             ava::tui::key_matches_action(*select_action_keybinds, ava::tui::TuiAction::SelectPageDown, ava::tui::terminal::Key::CtrlY),
         "tui keybind parser maps Pi select action ids to context-specific select-list actions");
  expect(!ava::tui::parse_key_bindings_json("{\"submit\":123}"), "tui keybind parser rejects non-string action values");
  auto const array_action_keybinds = ava::tui::parse_key_bindings_json("{\"cursor_line_start\":[\"Home\",\"Ctrl+A\"],\"message_dequeue\":[\"Alt+Up\"]}");
  expect(array_action_keybinds && ava::tui::key_matches_action(*array_action_keybinds, ava::tui::TuiAction::CursorLineStart, ava::tui::terminal::Key::Home) &&
             ava::tui::key_matches_action(*array_action_keybinds, ava::tui::TuiAction::CursorLineStart, ava::tui::terminal::Key::CtrlA) &&
             ava::tui::key_matches_action(*array_action_keybinds, ava::tui::TuiAction::MessageDequeue, ava::tui::terminal::Key::AltArrowUp),
         "tui keybind parser accepts Pi-style arrays of key names");
  auto const namespaced_action_keybinds = ava::tui::parse_key_bindings_json(
      "{\"tui.editor.cursorLineStart\":[\"Home\",\"Ctrl+A\"],\"app.message.dequeue\":[\"Alt+Up\"],"
      "\"app.message.followUp\":\"Alt+Enter\","
      "\"app.clear\":\"Ctrl+C\","
      "\"app.editor.external\":\"Ctrl+G\","
      "\"app.suspend\":\"Ctrl+Z\","
      "\"app.clipboard.pasteImage\":\"Ctrl+V\","
      "\"app.tree.editLabel\":\"Shift+L\","
      "\"app.tools.expand\":\"Ctrl+O\",\"tui.editor.deleteCharBackward\":[\"Backspace\",\"Ctrl+H\"]}");
  expect(namespaced_action_keybinds && ava::tui::key_matches_action(*namespaced_action_keybinds, ava::tui::TuiAction::CursorLineStart, ava::tui::terminal::Key::Home) &&
             ava::tui::key_matches_action(*namespaced_action_keybinds, ava::tui::TuiAction::CursorLineStart, ava::tui::terminal::Key::CtrlA) &&
             ava::tui::key_matches_action(*namespaced_action_keybinds, ava::tui::TuiAction::MessageDequeue, ava::tui::terminal::Key::AltArrowUp) &&
             ava::tui::key_matches_action(*namespaced_action_keybinds, ava::tui::TuiAction::MessageFollowUp, ava::tui::terminal::Key::AltEnter) &&
             ava::tui::key_matches_action(*namespaced_action_keybinds, ava::tui::TuiAction::ClearInput, ava::tui::terminal::Key::CtrlC) &&
             ava::tui::key_matches_action(*namespaced_action_keybinds, ava::tui::TuiAction::ExternalEditor, ava::tui::terminal::Key::CtrlG) &&
             ava::tui::key_matches_action(*namespaced_action_keybinds, ava::tui::TuiAction::Suspend, ava::tui::terminal::Key::CtrlZ) &&
             ava::tui::key_matches_action(*namespaced_action_keybinds, ava::tui::TuiAction::ClipboardPasteImage, ava::tui::terminal::Key::CtrlV) &&
             ava::tui::key_matches_action(*namespaced_action_keybinds, ava::tui::TuiAction::DetailsToggle, ava::tui::terminal::Key::CtrlO) &&
             ava::tui::key_matches_action(*namespaced_action_keybinds, ava::tui::TuiAction::TreeEditLabel, ava::tui::terminal::Key::ShiftL) &&
             ava::tui::key_matches_action(*namespaced_action_keybinds, ava::tui::TuiAction::DeleteBackward, ava::tui::terminal::Key::CtrlH),
         "tui keybind parser accepts matching Pi namespaced action ids and Ctrl+H");
  auto const vim_action_keybinds = ava::tui::parse_key_bindings_json(
      "{\"tui.editor.cursorLeft\":[\"Left\",\"Alt+H\"],\"tui.editor.cursorDown\":[\"Down\",\"Alt+J\"],"
      "\"tui.editor.cursorUp\":[\"Up\",\"Alt+K\"],\"tui.editor.cursorRight\":[\"Right\",\"Alt+L\"],"
      "\"tui.editor.cursorWordRight\":[\"Alt+Right\",\"Alt+W\"]}");
  expect(vim_action_keybinds && ava::tui::key_matches_action(*vim_action_keybinds, ava::tui::TuiAction::CursorLeft, ava::tui::terminal::Key::AltH) &&
             ava::tui::key_matches_action(*vim_action_keybinds, ava::tui::TuiAction::CursorDown, ava::tui::terminal::Key::AltJ) &&
             ava::tui::key_matches_action(*vim_action_keybinds, ava::tui::TuiAction::CursorUp, ava::tui::terminal::Key::AltK) &&
             ava::tui::key_matches_action(*vim_action_keybinds, ava::tui::TuiAction::CursorRight, ava::tui::terminal::Key::AltL) &&
             ava::tui::key_matches_action(*vim_action_keybinds, ava::tui::TuiAction::CursorWordRight, ava::tui::terminal::Key::AltW),
         "tui keybind parser accepts Pi Vim-style Alt+H/J/K/L and Alt+W cursor aliases");
  auto const legacy_action_keybinds = ava::tui::parse_key_bindings_json(
      "{\"cursorLineEnd\":[\"End\",\"Ctrl+E\"],\"expandTools\":\"Ctrl+O\","
      "\"toggleThinking\":\"Ctrl+T\",\"followUp\":\"Alt+Enter\"}");
  expect(legacy_action_keybinds && ava::tui::key_matches_action(*legacy_action_keybinds, ava::tui::TuiAction::CursorLineEnd, ava::tui::terminal::Key::End) &&
             ava::tui::key_matches_action(*legacy_action_keybinds, ava::tui::TuiAction::CursorLineEnd, ava::tui::terminal::Key::CtrlE) &&
             ava::tui::key_matches_action(*legacy_action_keybinds, ava::tui::TuiAction::DetailsToggle, ava::tui::terminal::Key::CtrlO) &&
             ava::tui::key_matches_action(*legacy_action_keybinds, ava::tui::TuiAction::ThinkingToggle, ava::tui::terminal::Key::CtrlT) &&
             ava::tui::key_matches_action(*legacy_action_keybinds, ava::tui::TuiAction::MessageFollowUp, ava::tui::terminal::Key::AltEnter),
         "tui keybind parser accepts legacy camelCase action aliases where AVA has matching semantics");
  auto const thinking_toggle_keybinds = ava::tui::parse_key_bindings_json("{\"app.thinking.toggle\":\"Ctrl+T\"}");
  expect(thinking_toggle_keybinds && ava::tui::key_matches_action(*thinking_toggle_keybinds, ava::tui::TuiAction::ThinkingToggle, ava::tui::terminal::Key::CtrlT) &&
             !ava::tui::key_matches_action(*thinking_toggle_keybinds, ava::tui::TuiAction::ReasoningSelect, ava::tui::terminal::Key::CtrlT) &&
             !ava::tui::key_matches_action(*thinking_toggle_keybinds, ava::tui::TuiAction::VariantCycle, ava::tui::terminal::Key::CtrlT),
         "tui keybind parser retains the configurable Pi thinking visibility toggle action id");
  auto const reasoning_select_keybinds = ava::tui::parse_key_bindings_json("{\"app.thinking.select\":\"Ctrl+T\"}");
  expect(reasoning_select_keybinds && ava::tui::key_matches_action(*reasoning_select_keybinds, ava::tui::TuiAction::ReasoningSelect, ava::tui::terminal::Key::CtrlT) &&
             !ava::tui::key_matches_action(*reasoning_select_keybinds, ava::tui::TuiAction::ThinkingToggle, ava::tui::terminal::Key::CtrlT),
         "tui keybind parser accepts the thinking-mode selector action id");
  expect(!ava::tui::parse_key_bindings_json("{\"reasoning_select\":\"Ctrl+T\",\"thinking_toggle\":\"Ctrl+T\"}"),
         "tui keybind parser preserves duplicate conflict detection across thinking selector and visibility actions");
  auto const namespaced_over_legacy_keybinds =
      ava::tui::parse_key_bindings_json("{\"app.tools.expand\":\"Ctrl+O\",\"expandTools\":\"Ctrl+T\",\"variant_cycle\":\"Ctrl+T\"}");
  expect(namespaced_over_legacy_keybinds &&
             ava::tui::key_matches_action(*namespaced_over_legacy_keybinds, ava::tui::TuiAction::DetailsToggle, ava::tui::terminal::Key::CtrlO) &&
             !ava::tui::key_matches_action(*namespaced_over_legacy_keybinds, ava::tui::TuiAction::DetailsToggle, ava::tui::terminal::Key::CtrlT) &&
             ava::tui::key_matches_action(*namespaced_over_legacy_keybinds, ava::tui::TuiAction::VariantCycle, ava::tui::terminal::Key::CtrlT),
         "tui keybind parser gives namespaced action ids precedence over legacy aliases before conflict checks");
  auto const later_current_alias_keybinds = ava::tui::parse_key_bindings_json("{\"tui.input.submit\":\"Ctrl+T\",\"submit\":\"Ctrl+D\"}");
  expect(later_current_alias_keybinds && ava::tui::key_matches_action(*later_current_alias_keybinds, ava::tui::TuiAction::Submit, ava::tui::terminal::Key::CtrlD) &&
             !ava::tui::key_matches_action(*later_current_alias_keybinds, ava::tui::TuiAction::Submit, ava::tui::terminal::Key::CtrlT),
         "tui keybind parser lets the later current-form alias win for the same effective action");
  auto const copy_action_keybinds = ava::tui::parse_key_bindings_json("{\"tui.input.copy\":\"Ctrl+C\"}");
  expect(copy_action_keybinds && ava::tui::key_matches_action(*copy_action_keybinds, ava::tui::TuiAction::CopySelection, ava::tui::terminal::Key::CtrlC) &&
             ava::tui::key_matches_action(*copy_action_keybinds, ava::tui::TuiAction::ClearInput, ava::tui::terminal::Key::CtrlC) &&
             ava::tui::key_matches_action(*copy_action_keybinds, ava::tui::TuiAction::Interrupt, ava::tui::terminal::Key::CtrlC),
         "tui keybind parser accepts Pi copy action ids while preserving Ctrl+C clear and interrupt fallbacks");
  auto const session_delete_noninvasive_keybinds = ava::tui::parse_key_bindings_json("{\"app.session.deleteNoninvasive\":\"Ctrl+Backspace\"}");
  expect(session_delete_noninvasive_keybinds &&
             ava::tui::key_matches_action(*session_delete_noninvasive_keybinds, ava::tui::TuiAction::SessionArchiveNoninvasive, ava::tui::terminal::Key::CtrlBackspace),
         "tui keybind parser accepts Pi session non-invasive delete action id");
  auto const branch_summary_keybinds = ava::tui::parse_key_bindings_json("{\"app.sessions.summarizeParent\":\"F8\"}");
  auto const branch_summary_alias_keybinds = ava::tui::parse_key_bindings_json("{\"sessionSummarizeParent\":\"F9\"}");
  auto const branch_summary_help =
      branch_summary_keybinds ? ava::tui::key_binding_help_items(*branch_summary_keybinds) : std::vector<ava::tui::TuiKeyBindingHelpItem>{};
  auto const branch_summary_help_item = std::ranges::find_if(branch_summary_help, [](auto const& item) { return item.action == "session_summarize_parent"; });
  expect(branch_summary_keybinds && branch_summary_alias_keybinds &&
             ava::tui::key_matches_action(*branch_summary_keybinds, ava::tui::TuiAction::SessionSummarizeParent, ava::tui::terminal::Key::F8) &&
             ava::tui::key_matches_action(*branch_summary_alias_keybinds, ava::tui::TuiAction::SessionSummarizeParent, ava::tui::terminal::Key::F9) &&
             ava::tui::key_binding_action_from_name("app.session.summarizeParent") == ava::tui::TuiAction::SessionSummarizeParent &&
             ava::tui::key_binding_config_action_id(ava::tui::TuiAction::SessionSummarizeParent) == "app.sessions.summarizeParent" &&
             branch_summary_help_item != branch_summary_help.end() && branch_summary_help_item->label == "Summarize abandoned parent" &&
             branch_summary_help_item->keys == "F8" && branch_summary_help_item->description.find("session selector") != std::string::npos,
         "parent-summary keybinding supports its canonical id, narrow aliases, semantic help, and configured dispatch");
  auto const session_action_keybinds = ava::tui::parse_key_bindings_json(
      "{\"app.session.new\":\"Alt+H\",\"app.session.tree\":\"Alt+J\","
      "\"app.session.fork\":\"Alt+K\",\"app.session.resume\":\"Alt+L\","
      "\"app.session.togglePath\":\"Ctrl+O\","
      "\"app.session.toggleSort\":\"Ctrl+Y\","
      "\"app.session.toggleNamedFilter\":\"Ctrl+U\","
      "\"app.session.rename\":\"Ctrl+K\","
      "\"app.session.delete\":\"Alt+D\"}");
  expect(session_action_keybinds && ava::tui::key_matches_action(*session_action_keybinds, ava::tui::TuiAction::SessionNew, ava::tui::terminal::Key::AltH) &&
             ava::tui::key_matches_action(*session_action_keybinds, ava::tui::TuiAction::SessionTree, ava::tui::terminal::Key::AltJ) &&
             ava::tui::key_matches_action(*session_action_keybinds, ava::tui::TuiAction::SessionFork, ava::tui::terminal::Key::AltK) &&
             ava::tui::key_matches_action(*session_action_keybinds, ava::tui::TuiAction::SessionResume, ava::tui::terminal::Key::AltL) &&
             ava::tui::key_matches_action(*session_action_keybinds, ava::tui::TuiAction::SessionTogglePath, ava::tui::terminal::Key::CtrlO) &&
             ava::tui::key_matches_action(*session_action_keybinds, ava::tui::TuiAction::SessionToggleSort, ava::tui::terminal::Key::CtrlY) &&
             ava::tui::key_matches_action(*session_action_keybinds, ava::tui::TuiAction::SessionToggleNamedFilter, ava::tui::terminal::Key::CtrlU) &&
             ava::tui::key_matches_action(*session_action_keybinds, ava::tui::TuiAction::SessionRename, ava::tui::terminal::Key::CtrlK) &&
             ava::tui::key_matches_action(*session_action_keybinds, ava::tui::TuiAction::SessionArchive, ava::tui::terminal::Key::AltD),
         "tui keybind parser accepts Pi session action ids");
  auto const tree_action_keybinds = ava::tui::parse_key_bindings_json(tui_test_support::tree_action_key_bindings_json());
  expect(tree_action_keybinds && ava::tui::key_matches_action(*tree_action_keybinds, ava::tui::TuiAction::TreeFoldOrUp, ava::tui::terminal::Key::CtrlO) &&
             ava::tui::key_matches_action(*tree_action_keybinds, ava::tui::TuiAction::TreeUnfoldOrDown, ava::tui::terminal::Key::CtrlY) &&
             ava::tui::key_matches_action(*tree_action_keybinds, ava::tui::TuiAction::TreeEditLabel, ava::tui::terminal::Key::ShiftL) &&
             ava::tui::key_matches_action(*tree_action_keybinds, ava::tui::TuiAction::TreeToggleLabelTimestamp, ava::tui::terminal::Key::ShiftT) &&
             ava::tui::key_matches_action(*tree_action_keybinds, ava::tui::TuiAction::TreeFilterLabeledOnly, ava::tui::terminal::Key::CtrlSpace) &&
             ava::tui::key_matches_action(*tree_action_keybinds, ava::tui::TuiAction::TreeFilterAll, ava::tui::terminal::Key::CtrlSlash),
         "tui keybind parser accepts Pi tree branch navigation, label, and equivalent filter action ids");
  expect(!ava::tui::parse_key_bindings_json("{\"app.tree.filter.noTools\":\"Ctrl+T\"}"),
         "tui keybind parser rejects Pi tree filter ids without an AVA session-selector equivalent");
  expect(!ava::tui::parse_key_bindings_json("{\"submit\":[]}"), "tui keybind parser rejects empty keybinding arrays");
  expect(!ava::tui::parse_key_bindings_json("{\"submit\":[\"Enter\",123]}"), "tui keybind parser rejects non-string keybinding array entries");
  auto const conflicting_keybinds = ava::tui::parse_key_bindings_json("{\"model_cycle_forward\":\"Ctrl+P\",\"model_cycle_backward\":\"Ctrl+P\"}");
  auto const conflicting_keybinds_error = conflicting_keybinds ? std::string() : conflicting_keybinds.error().format();
  expect(!conflicting_keybinds && conflicting_keybinds_error.find("conflicting TUI keybinding") != std::string::npos &&
             conflicting_keybinds_error.find("Ctrl+P") != std::string::npos && conflicting_keybinds_error.find("model_cycle_forward") != std::string::npos &&
             conflicting_keybinds_error.find("model_cycle_backward") != std::string::npos,
         "tui keybind parser rejects user-configured conflicts with actionable key and action context");
  auto const conflicting_select_keybinds = ava::tui::parse_key_bindings_json("{\"tui.select.confirm\":\"Space\",\"tui.select.cancel\":\"Space\"}");
  auto const conflicting_select_keybinds_error = conflicting_select_keybinds ? std::string() : conflicting_select_keybinds.error().format();
  expect(!conflicting_select_keybinds && conflicting_select_keybinds_error.find("conflicting TUI keybinding") != std::string::npos &&
             conflicting_select_keybinds_error.find("Space") != std::string::npos &&
             conflicting_select_keybinds_error.find("select_confirm") != std::string::npos &&
             conflicting_select_keybinds_error.find("select_cancel") != std::string::npos,
         "tui keybind parser rejects conflicts within the select-list keybinding context");
  auto const shadowing_keybinds = ava::tui::parse_key_bindings_json("{\"submit\":\"Ctrl+D\"}");
  expect(shadowing_keybinds && ava::tui::key_matches_action(*shadowing_keybinds, ava::tui::TuiAction::Submit, ava::tui::terminal::Key::CtrlD) &&
             !ava::tui::key_matches_action(*shadowing_keybinds, ava::tui::TuiAction::DeleteForward, ava::tui::terminal::Key::CtrlD) &&
             !ava::tui::key_matches_action(*shadowing_keybinds, ava::tui::TuiAction::Exit, ava::tui::terminal::Key::CtrlD),
         "tui keybind parser lets custom bindings shadow shared default keys without flagging conflicts");

  auto const default_config_json = ava::tui::default_key_bindings_config_json();
  auto const default_config_keybinds = ava::tui::parse_key_bindings_json(default_config_json);
  expect(ava::tui::keys_display(ava::tui::default_key_bindings(), ava::tui::TuiAction::SessionSummarizeParent).empty() &&
             default_config_json.find("app.sessions.summarizeParent") == std::string::npos,
         "parent-summary action is default-unbound and generated default config does not invent a binding");
  expect(
      default_config_json.find("\"tui.editor.cursorLeft\"") != std::string::npos && default_config_json.find("\"app.clear\"") != std::string::npos &&
          default_config_json.find("\"tui.input.copy\"") != std::string::npos && default_config_json.find("\"app.editor.external\"") != std::string::npos &&
          default_config_json.find("\"app.suspend\"") != std::string::npos && default_config_json.find("\"app.clipboard.pasteImage\"") != std::string::npos &&
          default_config_json.find("\"app.session.togglePath\"") != std::string::npos &&
          default_config_json.find("\"app.session.toggleSort\"") != std::string::npos &&
          default_config_json.find("\"app.session.toggleNamedFilter\"") != std::string::npos &&
          default_config_json.find("\"app.session.rename\"") != std::string::npos && default_config_json.find("\"app.session.delete\"") != std::string::npos &&
          default_config_json.find("\"app.session.deleteNoninvasive\"") != std::string::npos &&
          default_config_json.find("\"app.tree.foldOrUp\"") != std::string::npos &&
          default_config_json.find("\"app.tree.unfoldOrDown\"") != std::string::npos &&
          default_config_json.find("\"app.tree.editLabel\"") != std::string::npos &&
          default_config_json.find("\"app.tree.toggleLabelTimestamp\"") != std::string::npos &&
          default_config_json.find("\"app.tree.filter.labeledOnly\"") == std::string::npos &&
          default_config_json.find("\"app.tree.filter.all\"") == std::string::npos &&
          default_config_json.find("\"app.models.clearAll\"") != std::string::npos &&
          default_config_json.find("\"app.thinking.select\"") != std::string::npos &&
          default_config_json.find("\"app.thinking.toggle\"") == std::string::npos &&
          default_config_json.find("\"app.message.followUp\"") != std::string::npos &&
          default_config_json.find("\"app.model.cycleForward\"") != std::string::npos && default_config_json.find("\"message_prev\"") != std::string::npos &&
          default_config_json.find("\"message_next\"") != std::string::npos && default_config_json.find("\"jump_to_bottom\"") != std::string::npos &&
          default_config_json.find("\"history_prev\"") == std::string::npos && default_config_json.find("cursor_up") == std::string::npos &&
          default_config_json.find("cursor_down") == std::string::npos && default_config_json.find("\"mode_toggle\"") == std::string::npos &&
          default_config_keybinds && ava::tui::key_matches_action(*default_config_keybinds, ava::tui::TuiAction::CursorLeft, ava::tui::terminal::Key::CtrlB) &&
          ava::tui::key_matches_action(*default_config_keybinds, ava::tui::TuiAction::CopySelection, ava::tui::terminal::Key::CtrlC) &&
          ava::tui::key_matches_action(*default_config_keybinds, ava::tui::TuiAction::ClearInput, ava::tui::terminal::Key::CtrlC) &&
          ava::tui::key_matches_action(*default_config_keybinds, ava::tui::TuiAction::ExternalEditor, ava::tui::terminal::Key::CtrlG) &&
          ava::tui::key_matches_action(*default_config_keybinds, ava::tui::TuiAction::Suspend, ava::tui::terminal::Key::CtrlZ) &&
          ava::tui::key_matches_action(*default_config_keybinds, ava::tui::TuiAction::ClipboardPasteImage, ava::tui::terminal::Key::CtrlV) &&
          ava::tui::key_matches_action(*default_config_keybinds, ava::tui::TuiAction::Interrupt, ava::tui::terminal::Key::CtrlC) &&
          ava::tui::key_matches_action(*default_config_keybinds, ava::tui::TuiAction::SessionTogglePath, ava::tui::terminal::Key::CtrlP) &&
          ava::tui::key_matches_action(*default_config_keybinds, ava::tui::TuiAction::SessionToggleSort, ava::tui::terminal::Key::CtrlS) &&
          ava::tui::key_matches_action(*default_config_keybinds, ava::tui::TuiAction::SessionToggleNamedFilter, ava::tui::terminal::Key::CtrlN) &&
          ava::tui::key_matches_action(*default_config_keybinds, ava::tui::TuiAction::SessionRename, ava::tui::terminal::Key::CtrlR) &&
          ava::tui::key_matches_action(*default_config_keybinds, ava::tui::TuiAction::SessionArchive, ava::tui::terminal::Key::CtrlD) &&
          ava::tui::key_matches_action(*default_config_keybinds, ava::tui::TuiAction::SessionArchiveNoninvasive, ava::tui::terminal::Key::CtrlBackspace) &&
          ava::tui::key_matches_action(*default_config_keybinds, ava::tui::TuiAction::TreeFoldOrUp, ava::tui::terminal::Key::CtrlArrowLeft) &&
          ava::tui::key_matches_action(*default_config_keybinds, ava::tui::TuiAction::TreeUnfoldOrDown, ava::tui::terminal::Key::CtrlArrowRight) &&
          ava::tui::key_matches_action(*default_config_keybinds, ava::tui::TuiAction::TreeEditLabel, ava::tui::terminal::Key::ShiftL) &&
          ava::tui::key_matches_action(*default_config_keybinds, ava::tui::TuiAction::TreeToggleLabelTimestamp, ava::tui::terminal::Key::ShiftT) &&
          !ava::tui::key_matches_action(*default_config_keybinds, ava::tui::TuiAction::TreeFilterLabeledOnly, ava::tui::terminal::Key::CtrlN) &&
          !ava::tui::key_matches_action(*default_config_keybinds, ava::tui::TuiAction::TreeFilterAll, ava::tui::terminal::Key::CtrlA) &&
          ava::tui::key_matches_action(*default_config_keybinds, ava::tui::TuiAction::ModelsClearAll, ava::tui::terminal::Key::CtrlX) &&
          ava::tui::key_matches_action(*default_config_keybinds, ava::tui::TuiAction::ReasoningSelect, ava::tui::terminal::Key::CtrlT) &&
          !ava::tui::key_matches_action(*default_config_keybinds, ava::tui::TuiAction::ThinkingToggle, ava::tui::terminal::Key::CtrlT) &&
          ava::tui::key_matches_action(*default_config_keybinds, ava::tui::TuiAction::MessageFollowUp, ava::tui::terminal::Key::AltEnter) &&
          ava::tui::key_matches_action(*default_config_keybinds, ava::tui::TuiAction::MessagePrev, ava::tui::terminal::Key::AltK) &&
          ava::tui::key_matches_action(*default_config_keybinds, ava::tui::TuiAction::MessageNext, ava::tui::terminal::Key::AltJ) &&
          ava::tui::key_matches_action(*default_config_keybinds, ava::tui::TuiAction::JumpToBottom, ava::tui::terminal::Key::CtrlEnd) &&
          !ava::tui::key_matches_action(*default_config_keybinds, ava::tui::TuiAction::HistoryPrev, ava::tui::terminal::Key::AltK) &&
          !ava::tui::key_matches_action(*default_config_keybinds, ava::tui::TuiAction::CursorUp, ava::tui::terminal::Key::AltK) &&
          !ava::tui::key_matches_action(*default_config_keybinds, ava::tui::TuiAction::SelectPrev, ava::tui::terminal::Key::AltK) &&
          ava::tui::key_matches_action(*default_config_keybinds, ava::tui::TuiAction::Submit, ava::tui::terminal::Key::Enter),
      "tui keybind default config template uses Pi-style ids and remains valid with intentional shared defaults");

  auto const root = create_empty_root("tui-keybinds");
  auto const keybind_root = root / "tui-keybinds";
  std::filesystem::remove_all(keybind_root);
  std::filesystem::create_directories(keybind_root);
  auto const keybinds_file = keybind_root / "keybinds.json";
  auto const missing_keybinds = ava::tui::load_key_bindings(keybinds_file);
  expect(missing_keybinds && ava::tui::key_matches_action(*missing_keybinds, ava::tui::TuiAction::Submit, ava::tui::terminal::Key::Enter),
         "tui keybind file loader falls back to defaults when the file is missing");
  {
    std::ofstream output(keybinds_file);
    output << "{\"submit\":\"Ctrl+T\",\"variant_cycle\":[\"Shift+Tab\",\"Ctrl+D\"]}";
  }
  auto const loaded_keybinds = ava::tui::load_key_bindings(keybinds_file);
  expect(loaded_keybinds && ava::tui::key_matches_action(*loaded_keybinds, ava::tui::TuiAction::Submit, ava::tui::terminal::Key::CtrlT) &&
             ava::tui::key_matches_action(*loaded_keybinds, ava::tui::TuiAction::VariantCycle, ava::tui::terminal::Key::ShiftTab) &&
             ava::tui::key_matches_action(*loaded_keybinds, ava::tui::TuiAction::VariantCycle, ava::tui::terminal::Key::CtrlD),
         "tui keybind file loader reads valid configured string and array bindings");
  {
    std::ofstream output(keybinds_file);
    output << "{\"submit\":\"Enter\"";
  }
  expect(!ava::tui::load_key_bindings(keybinds_file), "tui keybind file loader rejects malformed JSON");
  {
    std::ofstream output(keybinds_file);
    output << "{\"submt\":\"Enter\"}";
  }
  expect(!ava::tui::load_key_bindings(keybinds_file), "tui keybind file loader rejects unknown actions");
  {
    std::ofstream output(keybinds_file);
    output << "{\"submit\":\"Ctrl+P\",\"model_cycle_forward\":\"Ctrl+P\"}";
  }
  auto const conflicting_loaded_keybinds = ava::tui::load_key_bindings(keybinds_file);
  auto const conflicting_loaded_keybinds_error = conflicting_loaded_keybinds ? std::string() : conflicting_loaded_keybinds.error().format();
  expect(!conflicting_loaded_keybinds && conflicting_loaded_keybinds_error.find("conflicting TUI keybinding") != std::string::npos &&
             conflicting_loaded_keybinds_error.find("Ctrl+P") != std::string::npos &&
             conflicting_loaded_keybinds_error.find(keybinds_file.string()) != std::string::npos,
         "tui keybind file loader reports configured conflicts with file path context");
}

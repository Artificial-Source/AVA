#include "sys.h"
#include "support/test_harness.h"
#include "support/tui_test_support.h"
#include "ava/tui/composer.h"
#include "ava/tui/composer_editor.h"
#include "ava/tui/composer_internal.h"
#include "ava/tui/keybindings.h"
#include "ava/tui/runtime_input_internal.h"
#include "ava/tui/runtime_internal.h"
#include "ava/tui/terminal.h"
#include "ava/tui/terminal/Context.h"
#include "ava/tui/terminal_image.h"
#include "ava/tui/text_wrap.h"
#include "ava/tui/theme.h"

#include <algorithm>
#include <chrono>
#include <clocale>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <curses.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

namespace {
void test_tui_terminal_image_support()
{
  auto const unknown = ava::tui::detect_terminal_image_capabilities(ava::tui::TerminalEnvironment{});
  expect(unknown.images == ava::tui::TerminalImageProtocol::None && !unknown.true_color && !unknown.hyperlinks &&
             unknown.detail.find("unknown terminal") != std::string::npos,
         "terminal image detection keeps unknown terminals on the textual fallback path");

  auto const tmux_ghostty = ava::tui::detect_terminal_image_capabilities(
      ava::tui::TerminalEnvironment{.term_program = "ghostty", .term = "tmux-256color", .color_term = "truecolor", .tmux = true}, true);
  expect(tmux_ghostty.images == ava::tui::TerminalImageProtocol::None && tmux_ghostty.true_color && tmux_ghostty.hyperlinks && tmux_ghostty.badge == "tmux" &&
             tmux_ghostty.detail.find("tmux") != std::string::npos,
         "terminal image detection disables image protocols under tmux while preserving explicit truecolor and forwarded hyperlinks");

  auto const tmux_default = ava::tui::detect_terminal_image_capabilities(
      ava::tui::TerminalEnvironment{.term_program = "ghostty", .term = "tmux-256color", .color_term = "truecolor", .tmux = true});
  expect(tmux_default.images == ava::tui::TerminalImageProtocol::None && tmux_default.true_color && !tmux_default.hyperlinks &&
             tmux_default.detail.find("AVA_TUI_TMUX_HYPERLINKS") != std::string::npos,
         "terminal image detection keeps tmux OSC 8 disabled unless forwarding is explicitly advertised");

  {
    ScopedEnvVar term("TERM", "tmux-256color");
    ScopedEnvVar term_program("TERM_PROGRAM", "ghostty");
    ScopedEnvVar terminal_emulator("TERMINAL_EMULATOR", "");
    ScopedEnvVar color_term("COLORTERM", "truecolor");
    ScopedEnvVar tmux("TMUX", "/tmp/tmux-1000/default,123,0");
    ScopedEnvVar tmux_hyperlinks("AVA_TUI_TMUX_HYPERLINKS", "1");
    auto const tmux_env = ava::tui::current_terminal_environment();
    auto const tmux_env_caps = ava::tui::detect_terminal_image_capabilities(tmux_env);
    expect(tmux_env.tmux && tmux_env.tmux_forwards_hyperlinks && tmux_env_caps.images == ava::tui::TerminalImageProtocol::None && tmux_env_caps.hyperlinks &&
               tmux_env_caps.detail.find("explicit tmux forwarding") != std::string::npos,
           "terminal image detection honors the explicit tmux hyperlink forwarding environment hint at runtime");
  }

  auto const kitty = ava::tui::detect_terminal_image_capabilities(ava::tui::TerminalEnvironment{.term_program = "kitty", .term = "xterm-kitty"});
  auto const ghostty = ava::tui::detect_terminal_image_capabilities(ava::tui::TerminalEnvironment{.term_program = "ghostty"});
  auto const wezterm = ava::tui::detect_terminal_image_capabilities(ava::tui::TerminalEnvironment{.term_program = "wezterm"});
  auto const warp = ava::tui::detect_terminal_image_capabilities(ava::tui::TerminalEnvironment{.term_program = "warpterminal"});
  expect(kitty.images == ava::tui::TerminalImageProtocol::Kitty && ghostty.images == ava::tui::TerminalImageProtocol::Kitty &&
             wezterm.images == ava::tui::TerminalImageProtocol::Kitty && warp.images == ava::tui::TerminalImageProtocol::Kitty && kitty.true_color &&
             kitty.hyperlinks,
         "terminal image detection enables Kitty-compatible graphics for Kitty, Ghostty, WezTerm, and Warp outside multiplexers");

  auto const iterm = ava::tui::detect_terminal_image_capabilities(ava::tui::TerminalEnvironment{.term_program = "iterm.app"});
  auto const windows_terminal = ava::tui::detect_terminal_image_capabilities(ava::tui::TerminalEnvironment{.term = "xterm-256color", .wt_session = true});
  expect(iterm.images == ava::tui::TerminalImageProtocol::Iterm2 && iterm.true_color && iterm.hyperlinks &&
             windows_terminal.images == ava::tui::TerminalImageProtocol::None && windows_terminal.true_color && windows_terminal.hyperlinks,
         "terminal image detection distinguishes iTerm2 inline images from hyperlink-only terminals");

  auto const kitty_sequence = ava::tui::encode_kitty_image("AAAA", ava::tui::KittyImageOptions{.columns = 2, .rows = 2, .image_id = 42, .move_cursor = false});
  auto const long_kitty_sequence = ava::tui::encode_kitty_image(std::string(5000, 'A'));
  expect(kitty_sequence.starts_with("\x1b_Ga=T,f=100,q=2,C=1,c=2,r=2,i=42;AAAA") && kitty_sequence.ends_with("\x1b\\") &&
             long_kitty_sequence.find(",m=1;") != std::string::npos && long_kitty_sequence.find("\x1b_Gm=0;") != std::string::npos &&
             ava::tui::delete_kitty_image(42) == "\x1b_Ga=d,d=I,i=42,q=2\x1b\\" && ava::tui::delete_all_kitty_images() == "\x1b_Ga=d,d=A,q=2\x1b\\",
         "terminal image helpers build Kitty graphics and cleanup sequences without terminal-side cursor movement when requested");

  auto const iterm_sequence = ava::tui::encode_iterm2_image(
      "BBBB", ava::tui::Iterm2ImageOptions{.width = "12", .height = "auto", .name = "screen.png", .preserve_aspect_ratio = false});
  expect(iterm_sequence.starts_with("\x1b]1337;File=inline=1;width=12;height=auto;name=c2NyZWVuLnBuZw==;preserveAspectRatio=0:BBBB") &&
             iterm_sequence.ends_with("\a"),
         "terminal image helpers build iTerm2 inline image sequences with encoded names");

  expect(ava::tui::terminal_line_contains_image_sequence("plain text") == false &&
             ava::tui::terminal_line_contains_image_sequence(std::string("x") + "\x1b_Gpayload\x1b\\") &&
             ava::tui::terminal_line_contains_image_sequence(std::string("x") + "\x1b]1337;File=inline=1:AAAA\a"),
         "terminal image line detection recognizes Kitty and iTerm2 escape sequences without false positives on plain paths");

  auto const cells = ava::tui::calculate_image_cell_size(ava::tui::ImageDimensions{.width_px = 20, .height_px = 100}, 10, 5,
                                                         ava::tui::TerminalCellDimensions{.width_px = 10, .height_px = 10});
  expect(cells.columns == 1 && cells.rows == 5, "terminal image sizing preserves aspect ratio within cell bounds");

  auto const fallback_cell_size = ava::tui::calculate_image_cell_size(ava::tui::ImageDimensions{.width_px = 90, .height_px = 90}, 10);
  auto const square_cell_size = ava::tui::calculate_image_cell_size(ava::tui::ImageDimensions{.width_px = 90, .height_px = 90}, 10, std::nullopt,
                                                                    ava::tui::TerminalCellDimensions{.width_px = 9, .height_px = 9});
  expect(fallback_cell_size.columns == 10 && fallback_cell_size.rows == 5 && square_cell_size.columns == 10 && square_cell_size.rows == 10,
         "terminal image sizing documents the 9x18 fallback cell constraint and honors explicit cell dimensions when supplied");

  auto const portrait = ava::tui::calculate_image_cell_size(ava::tui::ImageDimensions{.width_px = 100, .height_px = 400}, 40, 20,
                                                            ava::tui::TerminalCellDimensions{.width_px = 10, .height_px = 20});
  auto const landscape = ava::tui::calculate_image_cell_size(ava::tui::ImageDimensions{.width_px = 400, .height_px = 100}, 40, 20,
                                                             ava::tui::TerminalCellDimensions{.width_px = 10, .height_px = 20});
  auto const tiny = ava::tui::calculate_image_cell_size(ava::tui::ImageDimensions{.width_px = 1, .height_px = 1}, 40);
  auto const extreme = ava::tui::calculate_image_cell_size(ava::tui::ImageDimensions{.width_px = 10000, .height_px = 10}, 40, 10,
                                                           ava::tui::TerminalCellDimensions{.width_px = 10, .height_px = 20});
  expect(portrait.columns <= 40 && portrait.rows <= 20 && landscape.columns <= 40 && landscape.rows <= 20 && tiny.columns >= 1 && tiny.rows >= 1 &&
             extreme.columns <= 40 && extreme.rows <= 10,
         "image aspect sizing remains stable for portrait, landscape, tiny, and extreme dimensions");

  std::string png(24, '\0');
  png[0] = static_cast<char>(0x89);
  png[1] = 'P';
  png[2] = 'N';
  png[3] = 'G';
  png[4] = '\r';
  png[5] = '\n';
  png[6] = static_cast<char>(0x1A);
  png[7] = '\n';
  png[11] = 13;
  png[12] = 'I';
  png[13] = 'H';
  png[14] = 'D';
  png[15] = 'R';
  png[16] = 0;
  png[17] = 0;
  png[18] = 1;
  png[19] = 44;
  png[20] = 0;
  png[21] = 0;
  png[22] = 0;
  png[23] = static_cast<char>(200);
  auto const dimensions = ava::tui::image_dimensions_from_bytes(png, "image/png");
  expect(dimensions && dimensions->width_px == 300 && dimensions->height_px == 200,
         "terminal image dimension parser reads PNG dimensions for future preview row sizing");
}
}  // namespace

void run_tui_terminal_image_tests()
{
  test_tui_terminal_image_support();
}

void run_tui_terminal_input_tests_part_1()
{
  expect(ava::tui::terminal_escape_delay_ms() == 100,
         "terminal escape delay is deliberately tuned low enough for responsive Esc while buffering split CSI input");
  auto const osc8_width_sample = std::string("a\x1b]8;;https://example.test\x1b\\b\x1b]8;;\x1b\\");
  expect(ava::tui::detail::terminal_text_columns(osc8_width_sample) == 2 &&
             ava::tui::detail::fit_line_preserving_sgr(std::string("\x1b]8;;https://example.test\x1b\\abcdef\x1b]8;;\x1b\\"), 4).find("\x1b]8;;\x1b\\...") !=
                 std::string::npos,
         "tui width helpers treat OSC 8 hyperlinks as zero-width and close truncated links before ellipses");
  auto const underlined_wrap = ava::tui::detail::wrap_ansi_text(std::string(ava::tui::detail::kSgrUnderline) + "abcdef", 3);
  expect(underlined_wrap.size() == 2 && underlined_wrap[0] == std::string(ava::tui::detail::kSgrUnderline) + "abc" + std::string(ava::tui::detail::kSgrReset) &&
             underlined_wrap[1] == std::string(ava::tui::detail::kSgrUnderline) + "def" + std::string(ava::tui::detail::kSgrReset),
         "tui ANSI wrapping closes active underline at synthetic line breaks and reopens it on continuations");
  auto const reset_before_wrap =
      ava::tui::detail::wrap_ansi_text(std::string(ava::tui::detail::kSgrUnderline) + "abc" + std::string(ava::tui::detail::kSgrReset) + "def", 3);
  expect(reset_before_wrap.size() == 2 && reset_before_wrap[1] == "def", "tui ANSI wrapping honors explicit SGR resets before continuing with plain text");
  auto const background_sgr = std::string("\x1b[48;2;1;2;3m");
  auto const background_wrap = ava::tui::detail::wrap_ansi_text(background_sgr + "abcd" + std::string(ava::tui::detail::kSgrReset), 2);
  expect(background_wrap.size() == 2 && background_wrap[0] == background_sgr + "ab" + std::string(ava::tui::detail::kSgrReset) &&
             background_wrap[1] == background_sgr + "cd" + std::string(ava::tui::detail::kSgrReset),
         "tui ANSI wrapping preserves truecolor backgrounds across wrapped rows when the background remains active");
  auto const long_word_wrap = ava::tui::detail::wrap_ansi_text("abcdef", 2);
  expect(long_word_wrap == std::vector<std::string>({"ab", "cd", "ef"}), "tui ANSI wrapping hard-wraps long unbroken words deterministically");
  auto const cjk_ansi_wrap = ava::tui::detail::wrap_ansi_text(std::string("a") + "\xE7\x95\x8C" + "\xE7\x95\x8C" + "b", 3);
  expect(cjk_ansi_wrap.size() == 2 && cjk_ansi_wrap[0] == std::string("a") + "\xE7\x95\x8C" && cjk_ansi_wrap[1] == std::string("\xE7\x95\x8C") + "b" &&
             ava::tui::detail::terminal_text_columns(cjk_ansi_wrap[0]) == 3 && ava::tui::detail::terminal_text_columns(cjk_ansi_wrap[1]) == 3,
         "tui ANSI wrapping uses AVA display-width accounting for CJK cells");
  auto const newline_wrap = ava::tui::detail::wrap_ansi_text("ab\r\ncd\nef", 2);
  expect(newline_wrap == std::vector<std::string>({"ab", "cd", "ef"}), "tui ANSI wrapping treats CRLF and LF as explicit line breaks");
  expect(ava::tui::detail::wrap_ansi_text("", 4) == std::vector<std::string>({""}), "tui ANSI wrapping returns one empty row for empty input");
  expect(ava::tui::detail::wrap_ansi_text("ab", 0) == std::vector<std::string>({"a", "b"}), "tui ANSI wrapping clamps zero width to one column");
  auto const osc_open = std::string("\x1b]8;;https://example.test\x1b\\");
  auto const osc_close = std::string("\x1b]8;;\x1b\\");
  auto const osc_wrap = ava::tui::detail::wrap_ansi_text(osc_open + "abcd" + osc_close, 2);
  expect(osc_wrap.size() == 2 && osc_wrap[0] == osc_open + "ab" + osc_close && osc_wrap[1] == osc_open + "cd" + osc_close,
         "tui ANSI wrapping closes and reopens OSC 8 hyperlinks at synthetic line breaks");
  auto const malformed_color_wrap = ava::tui::detail::wrap_ansi_text(std::string("\x1b[38;2m") + "ab", 1);
  expect(malformed_color_wrap == std::vector<std::string>({std::string("\x1b[38;2m") + "a", "b"}),
         "tui ANSI wrapping preserves malformed extended color bytes without leaking dim state");
  expect(ava::tui::detail::ncurses_color_role_for_sgr(ava::tui::detail::kSgrMuted) == ava::tui::detail::NcursesColorRole::Muted &&
             ava::tui::detail::ncurses_color_role_for_sgr(ava::tui::detail::kSgrThinking) == ava::tui::detail::NcursesColorRole::Muted &&
             ava::tui::detail::ncurses_color_role_for_sgr(ava::tui::detail::kSgrTextDimmed) == ava::tui::detail::NcursesColorRole::Muted &&
             ava::tui::detail::ncurses_color_role_for_sgr(ava::tui::detail::kSgrAccent) == ava::tui::detail::NcursesColorRole::Accent,
         "tui ncurses SGR roles classify AVA muted and thinking colors before the blue accent");
  auto const sanitized_wrap = ava::tui::detail::wrap_transcript_text(std::string(ava::tui::detail::kSgrUnderline) + "ab", 3);
  expect(!sanitized_wrap.empty() && sanitized_wrap[0].find('\x1b') == std::string::npos,
         "production transcript wrapping still sanitizes raw escape sequences before wrapping");
  auto const kitty_push_sequence = ava::tui::terminal_kitty_keyboard_push_sequence();
  expect(kitty_push_sequence == std::string_view("\x1b[>7u") &&
             ava::tui::terminal_kitty_keyboard_query_sequence() == std::string_view("\x1b[>7u\x1b[?u\x1b[c") &&
             ava::tui::terminal_kitty_keyboard_pop_sequence() == std::string_view("\x1b[<u"),
         "normal terminal sessions request all supported Kitty key-reporting flags, query support, and restore the stack on exit");
  constexpr int kKittyDisambiguateEscapeCodes = 1;
  constexpr int kKittyReportEventTypes = 2;
  constexpr int kKittyReportAlternateKeys = 4;
  constexpr int kHealthyKittyFlags = kKittyDisambiguateEscapeCodes | kKittyReportEventTypes | kKittyReportAlternateKeys;
  expect(kHealthyKittyFlags == 7 && kitty_push_sequence == "\x1b[>7u", "normal Kitty sequence maps all supported key-reporting flags");
  expect(ava::tui::terminal_modify_other_keys_enable_sequence() == std::string_view("\x1b[>4;2m") &&
             ava::tui::terminal_modify_other_keys_disable_sequence() == std::string_view("\x1b[>4;0m"),
         "terminal session has xterm modifyOtherKeys fallback enable and disable sequences");
  auto const kitty_flags = ava::tui::terminal_kitty_keyboard_flags_response("[?5u");
  auto const kitty_zero_flags = ava::tui::terminal_kitty_keyboard_flags_response("[?0u");
  using KeyboardAction = ava::tui::KeyboardProtocolResponseAction;
  expect(kitty_flags && *kitty_flags == 5 && kitty_zero_flags && *kitty_zero_flags == 0 && !ava::tui::terminal_kitty_keyboard_flags_response("[?u") &&
             !ava::tui::terminal_kitty_keyboard_flags_response("[?5c") && ava::tui::terminal_device_attributes_response("[?1c") &&
             ava::tui::terminal_device_attributes_response("[?62;4;52c") && !ava::tui::terminal_device_attributes_response("[?c") &&
             !ava::tui::terminal_device_attributes_response("[?62;4;52u") &&
             ava::tui::terminal_keyboard_protocol_response_action("[?0u", false, false) == KeyboardAction::EnableModifyOtherKeys &&
             ava::tui::terminal_keyboard_protocol_response_action("[?62;4;52c", false, false) == KeyboardAction::EnableModifyOtherKeys &&
             ava::tui::terminal_keyboard_protocol_response_action("[?62;4;52c", true, false) == KeyboardAction::None &&
             ava::tui::terminal_keyboard_protocol_response_action("[?5u", false, true) == KeyboardAction::DisableModifyOtherKeys &&
             ava::tui::terminal_keyboard_protocol_response_action("[?5u", false, false) == KeyboardAction::None,
         "terminal keyboard negotiation parser recognizes Kitty flag and device-attribute replies and derives fallback actions");
  expect(
      ava::tui::terminal_escape_sequence_key("[27;2;13~") == ava::tui::Key::ShiftEnter &&
          ava::tui::terminal_escape_sequence_key("[13;2u") == ava::tui::Key::ShiftEnter &&
          ava::tui::terminal_escape_sequence_key("[13;2~") == ava::tui::Key::ShiftEnter &&
          ava::tui::terminal_escape_sequence_key("[27;5;13~") == ava::tui::Key::CtrlEnter &&
          ava::tui::terminal_escape_sequence_key("[13;5u") == ava::tui::Key::CtrlEnter &&
          ava::tui::terminal_escape_sequence_key("[13;5~") == ava::tui::Key::CtrlEnter &&
          ava::tui::terminal_escape_sequence_key("[27;3;13~") == ava::tui::Key::AltEnter &&
          ava::tui::terminal_escape_sequence_key("[13;3u") == ava::tui::Key::AltEnter &&
          ava::tui::terminal_escape_sequence_key("[13;3~") == ava::tui::Key::AltEnter &&
          ava::tui::terminal_escape_sequence_key("\r") == ava::tui::Key::AltEnter && ava::tui::terminal_escape_sequence_complete("\r") &&
          ava::tui::terminal_escape_sequence_key("[Z") == ava::tui::Key::ShiftTab &&
          ava::tui::terminal_escape_sequence_key("[1;6P") == ava::tui::Key::CtrlShiftP &&
          ava::tui::terminal_escape_sequence_key("[80;6u") == ava::tui::Key::CtrlShiftP &&
          ava::tui::terminal_escape_sequence_key("[1079::112;6u") == ava::tui::Key::CtrlShiftP &&
          ava::tui::terminal_escape_sequence_key("[103;5u") == ava::tui::Key::CtrlG && ava::tui::terminal_escape_sequence_key("[3~") == ava::tui::Key::Delete &&
          ava::tui::terminal_escape_sequence_key("[3$") == ava::tui::Key::ShiftDelete && ava::tui::terminal_escape_sequence_complete("[3$") &&
          ava::tui::terminal_escape_sequence_key("[3;2~") == ava::tui::Key::ShiftDelete &&
          ava::tui::terminal_escape_sequence_key("[2~") == ava::tui::Key::Insert && ava::tui::terminal_escape_sequence_key("[5~") == ava::tui::Key::PageUp &&
          ava::tui::terminal_escape_sequence_key("[6~") == ava::tui::Key::PageDown &&
          ava::tui::terminal_escape_sequence_key("[5;2~") == ava::tui::Key::PageUp &&
          ava::tui::terminal_escape_sequence_key("[6;2~") == ava::tui::Key::PageDown && ava::tui::terminal_escape_sequence_key("OP") == ava::tui::Key::F1 &&
          ava::tui::terminal_escape_sequence_key("OQ") == ava::tui::Key::F2 && ava::tui::terminal_escape_sequence_key("[11~") == ava::tui::Key::F1 &&
          ava::tui::terminal_escape_sequence_key("[12~") == ava::tui::Key::F2 && ava::tui::terminal_escape_sequence_key("[24~") == ava::tui::Key::F12 &&
          ava::tui::terminal_escape_sequence_key("[H") == ava::tui::Key::Home && ava::tui::terminal_escape_sequence_key("OH") == ava::tui::Key::Home &&
          ava::tui::terminal_escape_sequence_key("[1~") == ava::tui::Key::Home && ava::tui::terminal_escape_sequence_key("[7~") == ava::tui::Key::Home &&
          ava::tui::terminal_escape_sequence_key("[1;5H") == ava::tui::Key::CtrlHome &&
          ava::tui::terminal_escape_sequence_key("[7;5~") == ava::tui::Key::CtrlHome &&
          ava::tui::terminal_escape_sequence_key("[1;6H") == ava::tui::Key::ShiftCtrlHome &&
          ava::tui::terminal_escape_sequence_key("[7;6~") == ava::tui::Key::ShiftCtrlHome &&
          ava::tui::terminal_escape_sequence_key("[1;2H") == ava::tui::Key::ShiftHome &&
          ava::tui::terminal_escape_sequence_key("[2H") == ava::tui::Key::ShiftHome &&
          ava::tui::terminal_escape_sequence_key("[7$") == ava::tui::Key::ShiftHome &&
          ava::tui::terminal_escape_sequence_key("[7;2~") == ava::tui::Key::ShiftHome && ava::tui::terminal_escape_sequence_key("[F") == ava::tui::Key::End &&
          ava::tui::terminal_escape_sequence_key("OF") == ava::tui::Key::End && ava::tui::terminal_escape_sequence_key("[4~") == ava::tui::Key::End &&
          ava::tui::terminal_escape_sequence_key("[8~") == ava::tui::Key::End && ava::tui::terminal_escape_sequence_key("[1;5F") == ava::tui::Key::CtrlEnd &&
          ava::tui::terminal_escape_sequence_key("[8;5~") == ava::tui::Key::CtrlEnd &&
          ava::tui::terminal_escape_sequence_key("[1;6F") == ava::tui::Key::ShiftCtrlEnd &&
          ava::tui::terminal_escape_sequence_key("[8;6~") == ava::tui::Key::ShiftCtrlEnd &&
          ava::tui::terminal_escape_sequence_key("[1;2F") == ava::tui::Key::ShiftEnd &&
          ava::tui::terminal_escape_sequence_key("[2F") == ava::tui::Key::ShiftEnd &&
          ava::tui::terminal_escape_sequence_key("[8$") == ava::tui::Key::ShiftEnd &&
          ava::tui::terminal_escape_sequence_key("[8;2~") == ava::tui::Key::ShiftEnd &&
          ava::tui::terminal_escape_sequence_key("[45;5u") == ava::tui::Key::CtrlMinus &&
          ava::tui::terminal_escape_sequence_key(std::string_view("\x1f", 1)) == ava::tui::Key::CtrlMinus &&
          ava::tui::terminal_escape_sequence_key(std::string_view("\x1d", 1)) == ava::tui::Key::CtrlAltRightBracket &&
          ava::tui::terminal_escape_sequence_key("[3;3~") == ava::tui::Key::AltDelete &&
          ava::tui::terminal_escape_sequence_key("[1;5D") == ava::tui::Key::CtrlArrowLeft &&
          ava::tui::terminal_escape_sequence_key("[1;5C") == ava::tui::Key::CtrlArrowRight &&
          ava::tui::terminal_escape_sequence_key("[5D") == ava::tui::Key::CtrlArrowLeft &&
          ava::tui::terminal_escape_sequence_key("[5C") == ava::tui::Key::CtrlArrowRight &&
          ava::tui::terminal_escape_sequence_key("[1;2D") == ava::tui::Key::ShiftArrowLeft &&
          ava::tui::terminal_escape_sequence_key("[1;2C") == ava::tui::Key::ShiftArrowRight &&
          ava::tui::terminal_escape_sequence_key("[2D") == ava::tui::Key::ShiftArrowLeft &&
          ava::tui::terminal_escape_sequence_key("[2C") == ava::tui::Key::ShiftArrowRight &&
          ava::tui::terminal_escape_sequence_key("[d") == ava::tui::Key::ShiftArrowLeft &&
          ava::tui::terminal_escape_sequence_key("[c") == ava::tui::Key::ShiftArrowRight &&
          ava::tui::terminal_escape_sequence_key("[1;2A") == ava::tui::Key::ShiftArrowUp &&
          ava::tui::terminal_escape_sequence_key("[1;2B") == ava::tui::Key::ShiftArrowDown &&
          ava::tui::terminal_escape_sequence_key("[2A") == ava::tui::Key::ShiftArrowUp &&
          ava::tui::terminal_escape_sequence_key("[2B") == ava::tui::Key::ShiftArrowDown &&
          ava::tui::terminal_escape_sequence_key("[a") == ava::tui::Key::ShiftArrowUp &&
          ava::tui::terminal_escape_sequence_key("[b") == ava::tui::Key::ShiftArrowDown &&
          ava::tui::terminal_escape_sequence_key("[1;6D") == ava::tui::Key::ShiftCtrlArrowLeft &&
          ava::tui::terminal_escape_sequence_key("[1;6C") == ava::tui::Key::ShiftCtrlArrowRight &&
          ava::tui::terminal_escape_sequence_key("[6D") == ava::tui::Key::ShiftCtrlArrowLeft &&
          ava::tui::terminal_escape_sequence_key("[6C") == ava::tui::Key::ShiftCtrlArrowRight &&
          ava::tui::terminal_escape_sequence_key("[1;4D") == ava::tui::Key::ShiftAltArrowLeft &&
          ava::tui::terminal_escape_sequence_key("[1;4C") == ava::tui::Key::ShiftAltArrowRight &&
          ava::tui::terminal_escape_sequence_key("[4D") == ava::tui::Key::ShiftAltArrowLeft &&
          ava::tui::terminal_escape_sequence_key("[4C") == ava::tui::Key::ShiftAltArrowRight &&
          ava::tui::terminal_escape_sequence_key("[1;3A") == ava::tui::Key::AltArrowUp &&
          ava::tui::terminal_escape_sequence_key("[3A") == ava::tui::Key::AltArrowUp &&
          ava::tui::terminal_escape_sequence_key("[1;3B") == ava::tui::Key::AltArrowDown &&
          ava::tui::terminal_escape_sequence_key("[3B") == ava::tui::Key::AltArrowDown &&
          ava::tui::terminal_escape_sequence_key("[1;3D") == ava::tui::Key::AltArrowLeft &&
          ava::tui::terminal_escape_sequence_key("[1;3C") == ava::tui::Key::AltArrowRight &&
          ava::tui::terminal_escape_sequence_key("[3D") == ava::tui::Key::AltArrowLeft &&
          ava::tui::terminal_escape_sequence_key("[3C") == ava::tui::Key::AltArrowRight &&
          ava::tui::terminal_escape_sequence_key("[A") == ava::tui::Key::ArrowUp && ava::tui::terminal_escape_sequence_key("[B") == ava::tui::Key::ArrowDown &&
          ava::tui::terminal_escape_sequence_key("[C") == ava::tui::Key::ArrowRight &&
          ava::tui::terminal_escape_sequence_key("[D") == ava::tui::Key::ArrowLeft && ava::tui::terminal_escape_sequence_key("OA") == ava::tui::Key::ArrowUp &&
          ava::tui::terminal_escape_sequence_key("OB") == ava::tui::Key::ArrowDown &&
          ava::tui::terminal_escape_sequence_key("OC") == ava::tui::Key::ArrowRight &&
          ava::tui::terminal_escape_sequence_key("OD") == ava::tui::Key::ArrowLeft &&
          ava::tui::terminal_escape_sequence_key("[1;1A") == ava::tui::Key::ArrowUp &&
          ava::tui::terminal_escape_sequence_key("[1;1B") == ava::tui::Key::ArrowDown &&
          ava::tui::terminal_escape_sequence_key("[1;1C") == ava::tui::Key::ArrowRight &&
          ava::tui::terminal_escape_sequence_key("[1;1D") == ava::tui::Key::ArrowLeft &&
          ava::tui::terminal_escape_sequence_key("[57417u") == ava::tui::Key::ArrowLeft &&
          ava::tui::terminal_escape_sequence_key("[57418u") == ava::tui::Key::ArrowRight &&
          ava::tui::terminal_escape_sequence_key("[57419u") == ava::tui::Key::ArrowUp &&
          ava::tui::terminal_escape_sequence_key("[57420u") == ava::tui::Key::ArrowDown &&
          ava::tui::terminal_escape_sequence_key("[57419;2u") == ava::tui::Key::ShiftArrowUp &&
          ava::tui::terminal_escape_sequence_key("[57420;2u") == ava::tui::Key::ShiftArrowDown &&
          ava::tui::terminal_escape_sequence_key("[57417;2u") == ava::tui::Key::ShiftArrowLeft &&
          ava::tui::terminal_escape_sequence_key("[57418;2u") == ava::tui::Key::ShiftArrowRight &&
          ava::tui::terminal_escape_sequence_key("[57417;5u") == ava::tui::Key::CtrlArrowLeft &&
          ava::tui::terminal_escape_sequence_key("[57418;5u") == ava::tui::Key::CtrlArrowRight &&
          ava::tui::terminal_escape_sequence_key("[57417;3u") == ava::tui::Key::AltArrowLeft &&
          ava::tui::terminal_escape_sequence_key("[57418;3u") == ava::tui::Key::AltArrowRight &&
          ava::tui::terminal_escape_sequence_key("[57417;6u") == ava::tui::Key::ShiftCtrlArrowLeft &&
          ava::tui::terminal_escape_sequence_key("[57418;6u") == ava::tui::Key::ShiftCtrlArrowRight &&
          ava::tui::terminal_escape_sequence_key("[57417;4u") == ava::tui::Key::ShiftAltArrowLeft &&
          ava::tui::terminal_escape_sequence_key("[57418;4u") == ava::tui::Key::ShiftAltArrowRight &&
          ava::tui::terminal_escape_sequence_key("[57419;3u") == ava::tui::Key::AltArrowUp &&
          ava::tui::terminal_escape_sequence_key("[57420;3u") == ava::tui::Key::AltArrowDown &&
          ava::tui::terminal_escape_sequence_key("[57421u") == ava::tui::Key::PageUp &&
          ava::tui::terminal_escape_sequence_key("[57422u") == ava::tui::Key::PageDown &&
          ava::tui::terminal_escape_sequence_key("[57423u") == ava::tui::Key::Home && ava::tui::terminal_escape_sequence_key("[57424u") == ava::tui::Key::End &&
          ava::tui::terminal_escape_sequence_key("[57423;5u") == ava::tui::Key::CtrlHome &&
          ava::tui::terminal_escape_sequence_key("[57424;5u") == ava::tui::Key::CtrlEnd &&
          ava::tui::terminal_escape_sequence_key("[57423;6u") == ava::tui::Key::ShiftCtrlHome &&
          ava::tui::terminal_escape_sequence_key("[57424;6u") == ava::tui::Key::ShiftCtrlEnd &&
          ava::tui::terminal_escape_sequence_key("[57423;2u") == ava::tui::Key::ShiftHome &&
          ava::tui::terminal_escape_sequence_key("[57424;2u") == ava::tui::Key::ShiftEnd &&
          ava::tui::terminal_escape_sequence_key("[57426u") == ava::tui::Key::Delete &&
          ava::tui::terminal_escape_sequence_key("[57426;2u") == ava::tui::Key::ShiftDelete &&
          ava::tui::terminal_escape_sequence_key("[57426;3u") == ava::tui::Key::AltDelete &&
          ava::tui::terminal_escape_sequence_key("[127;2u") == ava::tui::Key::ShiftBackspace &&
          ava::tui::terminal_escape_sequence_key("[127;5u") == ava::tui::Key::CtrlBackspace &&
          ava::tui::terminal_escape_sequence_key("[99;5u") == ava::tui::Key::CtrlC &&
          ava::tui::terminal_escape_sequence_key("[120;5u") == ava::tui::Key::CtrlX &&
          ava::tui::terminal_escape_sequence_key("[1089::99;5u") == ava::tui::Key::CtrlC &&
          ava::tui::terminal_escape_sequence_key("[99;5:2u") == ava::tui::Key::CtrlC &&
          ava::tui::terminal_escape_sequence_key("[99;5:3u") == ava::tui::Key::Unknown &&
          ava::tui::terminal_escape_sequence_key("[100;3u") == ava::tui::Key::AltD &&
          ava::tui::terminal_escape_sequence_key("[27;5;99~") == ava::tui::Key::CtrlC &&
          ava::tui::terminal_escape_sequence_key("[27;5;100~") == ava::tui::Key::CtrlD &&
          ava::tui::terminal_escape_sequence_key("[27;5;45~") == ava::tui::Key::CtrlMinus &&
          ava::tui::terminal_escape_sequence_key("[27;6;80~") == ava::tui::Key::CtrlShiftP &&
          ava::tui::terminal_escape_sequence_key("[27;3;100~") == ava::tui::Key::AltD &&
          ava::tui::terminal_escape_sequence_key("[27;2;127~") == ava::tui::Key::ShiftBackspace &&
          ava::tui::terminal_escape_sequence_key("[27;5;127~") == ava::tui::Key::CtrlBackspace &&
          ava::tui::terminal_escape_sequence_key("[27;3;127~") == ava::tui::Key::AltBackspace &&
          ava::tui::terminal_escape_sequence_key("[27;1;127~") == ava::tui::Key::Backspace &&
          ava::tui::terminal_escape_sequence_key("[27;1;27~") == ava::tui::Key::Escape &&
          ava::tui::terminal_escape_sequence_key(std::string_view("\x00", 1)) == ava::tui::Key::CtrlSpace &&
          ava::tui::terminal_escape_sequence_key("[32;5u") == ava::tui::Key::CtrlSpace &&
          ava::tui::terminal_escape_sequence_key("[27;5;32~") == ava::tui::Key::CtrlSpace &&
          ava::tui::terminal_escape_sequence_key("[48;5u") == ava::tui::Key::Ctrl0 &&
          ava::tui::terminal_escape_sequence_key("[49;5u") == ava::tui::Key::Ctrl1 &&
          ava::tui::terminal_escape_sequence_key("[57;5u") == ava::tui::Key::Ctrl9 &&
          ava::tui::terminal_escape_sequence_key("[27;5;49~") == ava::tui::Key::Ctrl1 &&
          ava::tui::terminal_escape_sequence_key("[47;5u") == ava::tui::Key::CtrlSlash &&
          ava::tui::terminal_escape_sequence_key("[47::91;5u") == ava::tui::Key::CtrlSlash &&
          ava::tui::terminal_escape_sequence_key("[27;5;47~") == ava::tui::Key::CtrlSlash &&
          ava::tui::terminal_escape_sequence_key("[27;2;9~") == ava::tui::Key::ShiftTab &&
          ava::tui::terminal_escape_sequence_key("[<64;12;9M") == ava::tui::Key::MouseWheelUp &&
          ava::tui::terminal_escape_sequence_key("[<65;12;9M") == ava::tui::Key::MouseWheelDown &&
          ava::tui::terminal_escape_sequence_key("[<68;12;9M") == ava::tui::Key::MouseWheelUp &&
          ava::tui::terminal_escape_sequence_key("[<69;12;9M") == ava::tui::Key::MouseWheelDown &&
          ava::tui::terminal_escape_sequence_key("[<0;12;9M") == ava::tui::Key::MouseLeftPress &&
          ava::tui::terminal_escape_sequence_key("[<32;12;9M") == ava::tui::Key::MouseLeftDrag &&
          ava::tui::terminal_escape_sequence_key("[<64;12;9m") == ava::tui::Key::Unknown &&
          ava::tui::terminal_escape_sequence_key("[<65;12;9m") == ava::tui::Key::Unknown &&
          ava::tui::terminal_escape_sequence_key("[<0;12;9m") == ava::tui::Key::MouseLeftRelease &&
          ava::tui::terminal_escape_sequence_key(std::string_view("\x7f", 1)) == ava::tui::Key::AltBackspace &&
          ava::tui::terminal_escape_sequence_key(std::string_view("\b", 1)) == ava::tui::Key::AltBackspace &&
          ava::tui::terminal_escape_sequence_key("b") == ava::tui::Key::AltB && ava::tui::terminal_escape_sequence_key("d") == ava::tui::Key::AltD &&
          ava::tui::terminal_escape_sequence_key("f") == ava::tui::Key::AltF && ava::tui::terminal_escape_sequence_key("h") == ava::tui::Key::AltH &&
          ava::tui::terminal_escape_sequence_key("j") == ava::tui::Key::AltJ && ava::tui::terminal_escape_sequence_key("k") == ava::tui::Key::AltK &&
          ava::tui::terminal_escape_sequence_key("l") == ava::tui::Key::AltL && ava::tui::terminal_escape_sequence_key("w") == ava::tui::Key::AltW &&
          ava::tui::terminal_escape_sequence_key("y") == ava::tui::Key::AltY && ava::tui::terminal_escape_sequence_key("[13;2") == ava::tui::Key::Unknown &&
          ava::tui::terminal_escape_sequence_key("[13;5") == ava::tui::Key::Unknown &&
          ava::tui::terminal_escape_sequence_key("[27;2;13") == ava::tui::Key::Unknown &&
          ava::tui::terminal_escape_sequence_key("[999999999999999999999;2u") == ava::tui::Key::Unknown &&
          ava::tui::terminal_escape_sequence_key("[200~") == ava::tui::Key::Unknown,
      "terminal escape parser maps complete modified Enter CSI forms without treating partial keys or paste markers as "
      "text");
  expect(ava::tui::terminal_escape_sequence_key("[1;1:1A") == ava::tui::Key::ArrowUp &&
             ava::tui::terminal_escape_sequence_key("[1;1:1B") == ava::tui::Key::ArrowDown &&
             ava::tui::terminal_escape_sequence_key("[1;1:2C") == ava::tui::Key::ArrowRight &&
             ava::tui::terminal_escape_sequence_key("[1;1:2D") == ava::tui::Key::ArrowLeft &&
             ava::tui::terminal_escape_sequence_key("[1;2:1A") == ava::tui::Key::ShiftArrowUp &&
             ava::tui::terminal_escape_sequence_key("[1;3:2B") == ava::tui::Key::AltArrowDown &&
             ava::tui::terminal_escape_sequence_key("[1;5:1D") == ava::tui::Key::CtrlArrowLeft &&
             ava::tui::terminal_escape_sequence_key("[1;6:2C") == ava::tui::Key::ShiftCtrlArrowRight &&
             ava::tui::terminal_escape_sequence_key("[1;129:1B") == ava::tui::Key::ArrowDown &&
             ava::tui::terminal_escape_sequence_key("[1;1:3A") == ava::tui::Key::Unknown &&
             ava::tui::terminal_escape_sequence_key("[1;2:3B") == ava::tui::Key::Unknown && ava::tui::terminal_escape_sequence_should_discard("[1;1:3A"),
         "terminal escape parser decodes Kitty special-CSI arrow press and repeat events with modifiers while consuming releases");
  expect(
      ava::tui::terminal_escape_sequence_key("[1;1:1H") == ava::tui::Key::Home && ava::tui::terminal_escape_sequence_key("[1;1:2F") == ava::tui::Key::End &&
          ava::tui::terminal_escape_sequence_key("[1;5:1H") == ava::tui::Key::CtrlHome &&
          ava::tui::terminal_escape_sequence_key("[1;6:2F") == ava::tui::Key::ShiftCtrlEnd &&
          ava::tui::terminal_escape_sequence_key("[1;1:3H") == ava::tui::Key::Unknown &&
          ava::tui::terminal_escape_sequence_key("[7;1:1~") == ava::tui::Key::Home && ava::tui::terminal_escape_sequence_key("[8;1:2~") == ava::tui::Key::End &&
          ava::tui::terminal_escape_sequence_key("[5;1:1~") == ava::tui::Key::PageUp &&
          ava::tui::terminal_escape_sequence_key("[6;1:2~") == ava::tui::Key::PageDown &&
          ava::tui::terminal_escape_sequence_key("[2;1:1~") == ava::tui::Key::Insert &&
          ava::tui::terminal_escape_sequence_key("[3;2:2~") == ava::tui::Key::ShiftDelete &&
          ava::tui::terminal_escape_sequence_key("[5;1:3~") == ava::tui::Key::Unknown &&
          ava::tui::terminal_escape_sequence_key("[1;1:1P") == ava::tui::Key::F1 && ava::tui::terminal_escape_sequence_key("[1;1:2S") == ava::tui::Key::F4 &&
          ava::tui::terminal_escape_sequence_key("[15;1:1~") == ava::tui::Key::F5 && ava::tui::terminal_escape_sequence_key("[24;1:2~") == ava::tui::Key::F12 &&
          ava::tui::terminal_escape_sequence_key("[1;1:3P") == ava::tui::Key::Unknown &&
          ava::tui::terminal_escape_sequence_key("[15;1:3~") == ava::tui::Key::Unknown && ava::tui::terminal_escape_sequence_should_discard("[5;1:3~"),
      "terminal escape parser applies Kitty event suffixes to Home, End, tilde special keys, and function keys");
  expect(ava::tui::terminal_escape_sequence_key("[1;1:A") == ava::tui::Key::Unknown &&
             ava::tui::terminal_escape_sequence_key("[1;1:0A") == ava::tui::Key::Unknown &&
             ava::tui::terminal_escape_sequence_key("[1;1:4A") == ava::tui::Key::Unknown &&
             ava::tui::terminal_escape_sequence_key("[1;:1A") == ava::tui::Key::Unknown &&
             ava::tui::terminal_escape_sequence_key("[1;0:1A") == ava::tui::Key::Unknown &&
             ava::tui::terminal_escape_sequence_key("[1:1A") == ava::tui::Key::Unknown &&
             ava::tui::terminal_escape_sequence_key("[1;1:1xA") == ava::tui::Key::Unknown &&
             ava::tui::terminal_escape_sequence_key("[1;1:1;2A") == ava::tui::Key::Unknown &&
             ava::tui::terminal_escape_sequence_key("[5;1:~") == ava::tui::Key::Unknown &&
             ava::tui::terminal_escape_sequence_key("[5;1:0~") == ava::tui::Key::Unknown &&
             ava::tui::terminal_escape_sequence_key("[5;1:9~") == ava::tui::Key::Unknown &&
             ava::tui::terminal_escape_sequence_key("[1;1:Q") == ava::tui::Key::Unknown &&
             ava::tui::terminal_escape_sequence_key("[1;1:4Q") == ava::tui::Key::Unknown &&
             ava::tui::terminal_escape_sequence_key("[1;1:1xQ") == ava::tui::Key::Unknown,
         "terminal escape parser fails closed on malformed Kitty special-CSI modifiers, event types, and trailing fields");
  expect(ava::tui::terminal_escape_sequence_key("[A") == ava::tui::Key::ArrowUp && ava::tui::terminal_escape_sequence_key("OB") == ava::tui::Key::ArrowDown &&
             ava::tui::terminal_escape_sequence_key("[1;5D") == ava::tui::Key::CtrlArrowLeft &&
             ava::tui::terminal_escape_sequence_key("[H") == ava::tui::Key::Home && ava::tui::terminal_escape_sequence_key("OF") == ava::tui::Key::End &&
             ava::tui::terminal_escape_sequence_key("[5~") == ava::tui::Key::PageUp && ava::tui::terminal_escape_sequence_key("[24~") == ava::tui::Key::F12 &&
             ava::tui::terminal_escape_sequence_key("[57420;1:2u") == ava::tui::Key::ArrowDown &&
             ava::tui::terminal_escape_sequence_key("[1;129B") == ava::tui::Key::ArrowDown,
         "terminal escape parser retains legacy CSI, SS3, modified, CSI-u, and tmux-compatible lock-modifier forms");
  expect(ava::tui::terminal_escape_sequence_key("[1;129B") == ava::tui::Key::ArrowDown &&
             ava::tui::terminal_escape_sequence_key("[1;193A") == ava::tui::Key::ArrowUp &&
             ava::tui::terminal_escape_sequence_key("[1;130A") == ava::tui::Key::ShiftArrowUp &&
             ava::tui::terminal_escape_sequence_key("[1;133D") == ava::tui::Key::CtrlArrowLeft &&
             ava::tui::terminal_escape_sequence_key("[1;17B") == ava::tui::Key::Unknown &&
             ava::tui::terminal_escape_sequence_key("[57420;129u") == ava::tui::Key::ArrowDown &&
             ava::tui::terminal_escape_sequence_key("[5;129~") == ava::tui::Key::PageUp &&
             ava::tui::terminal_escape_sequence_key("[6;129~") == ava::tui::Key::PageDown &&
             ava::tui::terminal_escape_sequence_key("[1;129H") == ava::tui::Key::Home &&
             ava::tui::terminal_escape_sequence_key("[1;129F") == ava::tui::Key::End &&
             ava::tui::terminal_escape_sequence_key("[1;130H") == ava::tui::Key::ShiftHome &&
             ava::tui::terminal_escape_sequence_key("[1;133F") == ava::tui::Key::CtrlEnd &&
             ava::tui::terminal_escape_sequence_key("[3;129~") == ava::tui::Key::Delete &&
             ava::tui::terminal_escape_sequence_key("[3;130~") == ava::tui::Key::ShiftDelete &&
             ava::tui::terminal_escape_sequence_key("[3;131~") == ava::tui::Key::AltDelete,
         "terminal escape parser ignores Ghostty Kitty lock modifiers for physical navigation keys");
  auto const kitty_keypad_one = ava::tui::terminal_escape_sequence_event("[57400u");
  auto const kitty_keypad_plus = ava::tui::terminal_escape_sequence_event("[57413u");
  auto const kitty_shifted_letter = ava::tui::terminal_escape_sequence_event("[97:65:97;2u");
  auto const kitty_repeat_letter = ava::tui::terminal_escape_sequence_event("[97;1:2u");
  auto const kitty_release_letter = ava::tui::terminal_escape_sequence_event("[97;1:3u");
  auto const modify_plain_letter = ava::tui::terminal_escape_sequence_event("[27;1;120~");
  auto const modify_shifted_letter = ava::tui::terminal_escape_sequence_event("[27;2;69~");
  expect(kitty_keypad_one.key == ava::tui::Key::Character && kitty_keypad_one.text == "1" && kitty_keypad_plus.key == ava::tui::Key::Character &&
             kitty_keypad_plus.text == "+" && kitty_shifted_letter.key == ava::tui::Key::Character && kitty_shifted_letter.text == "A" &&
             kitty_repeat_letter.key == ava::tui::Key::Character && kitty_repeat_letter.text == "a" && kitty_release_letter.key == ava::tui::Key::Unknown &&
             modify_plain_letter.key == ava::tui::Key::Character && modify_plain_letter.text == "x" && modify_shifted_letter.key == ava::tui::Key::Character &&
             modify_shifted_letter.text == "E",
         "terminal escape parser decodes Kitty CSI-u and xterm modifyOtherKeys printable reports while ignoring releases");
  auto const legacy_mouse_sequence = [](unsigned int button, unsigned int column, unsigned int row) {
    std::string sequence = "[M";
    sequence.push_back(static_cast<char>(button + 32U));
    sequence.push_back(static_cast<char>(column + 32U));
    sequence.push_back(static_cast<char>(row + 32U));
    return sequence;
  };
  ava::tui::terminal_reset_mouse_tracking();
  auto const sgr_hover = ava::tui::terminal_escape_sequence_event("[<32;12;9M");
  auto const sgr_press = ava::tui::terminal_escape_sequence_event("[<0;12;9M");
  auto const sgr_drag = ava::tui::terminal_escape_sequence_event("[<32;14;10M");
  auto const sgr_release = ava::tui::terminal_escape_sequence_event("[<0;14;10m");
  auto const sgr_post_release_hover = ava::tui::terminal_escape_sequence_event("[<32;15;10M");
  auto const sgr_shift_press = ava::tui::terminal_escape_sequence_event("[<4;12;9M");
  auto const sgr_shift_motion = ava::tui::terminal_escape_sequence_event("[<36;14;10M");
  auto const sgr_wheel = ava::tui::terminal_escape_sequence_event("[<65;21;7M");
  auto const sgr_shift_wheel = ava::tui::terminal_escape_sequence_event("[<69;22;8M");
  ava::tui::terminal_reset_mouse_tracking();
  auto const legacy_hover = ava::tui::terminal_escape_sequence_event(legacy_mouse_sequence(32, 12, 9));
  auto const legacy_press = ava::tui::terminal_escape_sequence_event(legacy_mouse_sequence(0, 12, 9));
  auto const legacy_drag = ava::tui::terminal_escape_sequence_event(legacy_mouse_sequence(32, 14, 10));
  auto const legacy_release = ava::tui::terminal_escape_sequence_event(legacy_mouse_sequence(3, 14, 10));
  auto const legacy_post_release_hover = ava::tui::terminal_escape_sequence_event(legacy_mouse_sequence(32, 15, 10));
  auto const legacy_wheel = ava::tui::terminal_escape_sequence_event(legacy_mouse_sequence(64, 21, 7));
  expect(sgr_hover.key == ava::tui::Key::Unknown && sgr_press.key == ava::tui::Key::MouseLeftPress && sgr_press.mouse_column == 12 &&
             sgr_press.mouse_row == 9 && sgr_drag.key == ava::tui::Key::MouseLeftDrag && sgr_drag.mouse_column == 14 && sgr_drag.mouse_row == 10 &&
             sgr_release.key == ava::tui::Key::MouseLeftRelease && sgr_release.mouse_column == 14 && sgr_release.mouse_row == 10 &&
             sgr_post_release_hover.key == ava::tui::Key::Unknown && sgr_shift_press.key == ava::tui::Key::MousePointerCancel &&
             sgr_shift_press.mouse_column == 12 && sgr_shift_press.mouse_row == 9 && sgr_shift_motion.key == ava::tui::Key::MousePointerCancel &&
             sgr_wheel.key == ava::tui::Key::MouseWheelDown && sgr_wheel.mouse_column == 21 && sgr_wheel.mouse_row == 7 &&
             sgr_shift_wheel.key == ava::tui::Key::MouseWheelDown && sgr_shift_wheel.mouse_column == 22 && sgr_shift_wheel.mouse_row == 8 &&
             legacy_hover.key == ava::tui::Key::Unknown && legacy_press.key == ava::tui::Key::MouseLeftPress &&
             legacy_drag.key == ava::tui::Key::MouseLeftDrag && legacy_release.key == ava::tui::Key::MouseLeftRelease &&
             legacy_post_release_hover.key == ava::tui::Key::Unknown && legacy_wheel.key == ava::tui::Key::MouseWheelUp && legacy_wheel.mouse_column == 21 &&
             legacy_wheel.mouse_row == 7,
         "terminal mouse protocols preserve real press-drag-release lifecycles, ignore hover, cancel on Shift, and preserve wheels");
#ifdef NCURSES_MOUSE_VERSION
  ava::tui::terminal_reset_mouse_tracking();
  auto const ncurses_hover = ava::tui::terminal_ncurses_mouse_event(REPORT_MOUSE_POSITION, 3, 4);
  auto const ncurses_press = ava::tui::terminal_ncurses_mouse_event(BUTTON1_PRESSED, 3, 4);
  auto const ncurses_drag = ava::tui::terminal_ncurses_mouse_event(REPORT_MOUSE_POSITION, 5, 6);
  auto const ncurses_release = ava::tui::terminal_ncurses_mouse_event(BUTTON1_RELEASED, 5, 6);
  auto const ncurses_after_release = ava::tui::terminal_ncurses_mouse_event(REPORT_MOUSE_POSITION, 7, 8);
  auto const ncurses_click = ava::tui::terminal_ncurses_mouse_event(BUTTON1_CLICKED, 9, 10);
#ifdef BUTTON_SHIFT
  auto const ncurses_shift_press = ava::tui::terminal_ncurses_mouse_event(BUTTON1_PRESSED | BUTTON_SHIFT, 3, 4);
  auto const ncurses_shift_ok = ncurses_shift_press.key == ava::tui::Key::MousePointerCancel;
#else
  auto const ncurses_shift_ok = true;
#endif
  expect(ncurses_hover.key == ava::tui::Key::Unknown && ncurses_press.key == ava::tui::Key::MouseLeftPress &&
             ncurses_drag.key == ava::tui::Key::MouseLeftDrag && ncurses_release.key == ava::tui::Key::MouseLeftRelease &&
             ncurses_after_release.key == ava::tui::Key::Unknown && ncurses_click.key == ava::tui::Key::MouseLeftClick && ncurses_shift_ok,
         "ncurses mouse reports preserve owned left-button lifecycle, click fallback, and Shift pointer cancel");
#endif
  expect(ava::tui::terminal_escape_sequence_complete("[13;2u") && ava::tui::terminal_escape_sequence_complete("[?25l") &&
             ava::tui::terminal_escape_sequence_complete("[45;5u") && ava::tui::terminal_escape_sequence_complete("[3;3~") &&
             ava::tui::terminal_escape_sequence_complete("[<0;12;9M") && ava::tui::terminal_escape_sequence_complete(legacy_mouse_sequence(0, 12, 9)) &&
             ava::tui::terminal_escape_sequence_complete(std::string_view("\x00", 1)) &&
             ava::tui::terminal_escape_sequence_complete(std::string_view("\x1f", 1)) &&
             ava::tui::terminal_escape_sequence_complete(std::string_view("\x1d", 1)) &&
             ava::tui::terminal_escape_sequence_complete(std::string_view("\x7f", 1)) &&
             ava::tui::terminal_escape_sequence_complete(std::string_view("\b", 1)) &&
             ava::tui::terminal_escape_sequence_complete(std::string("]0;AVA") + "\a") &&
             ava::tui::terminal_escape_sequence_complete(std::string("]52;c;AAAA") + "\x1b\\") &&
             ava::tui::terminal_escape_sequence_complete(std::string("P1;2|payload") + "\x1b\\") && !ava::tui::terminal_escape_sequence_complete("[13;2") &&
             !ava::tui::terminal_escape_sequence_complete("[200") && !ava::tui::terminal_escape_sequence_complete("[M ") &&
             !ava::tui::terminal_escape_sequence_complete("]0;AVA") && !ava::tui::terminal_escape_sequence_complete(std::string("P1;2|payload")),
         "terminal escape parser buffers CSI, OSC, and DCS sequences until a real terminator is present");
  expect(ava::tui::terminal_escape_sequence_should_discard("[?25l") && ava::tui::terminal_escape_sequence_should_discard("[?5u") &&
             ava::tui::terminal_escape_sequence_should_discard("[?62;4;52c") &&
             ava::tui::terminal_escape_sequence_should_discard(std::string("]0;AVA") + "\a") &&
             ava::tui::terminal_escape_sequence_should_discard(std::string("]52;c;AAAA") + "\x1b\\") &&
             ava::tui::terminal_escape_sequence_should_discard(std::string("P1;2|payload") + "\x1b\\") &&
             ava::tui::terminal_escape_sequence_should_discard("[<64;12;9m") && ava::tui::terminal_escape_sequence_should_discard("[97;1:3u") &&
             !ava::tui::terminal_escape_sequence_should_discard("[13;2u") && !ava::tui::terminal_escape_sequence_should_discard("[200~") &&
             !ava::tui::terminal_escape_sequence_should_discard("[13;2"),
         "terminal escape parser discards completed terminal controls while preserving AVA-owned key and paste markers");
}
void run_tui_terminal_input_tests_part_2()
{
  std::string utf8_input = std::string("ab") + "\xC3\xA9";
  ava::tui::erase_last_utf8_codepoint(utf8_input);
  expect(utf8_input == "ab", "tui backspace erases a complete utf-8 codepoint");
  std::string orphan_continuation = std::string("a") + std::string("\x80", 1);
  ava::tui::erase_last_utf8_codepoint(orphan_continuation);
  expect(orphan_continuation == "a", "tui backspace erases only a trailing orphan utf-8 continuation byte");
  std::string orphan_after_utf8 = std::string("a") + "\xC3\xA9" + std::string("\x80", 1);
  ava::tui::erase_last_utf8_codepoint(orphan_after_utf8);
  expect(orphan_after_utf8 == std::string("a") + "\xC3\xA9", "tui backspace preserves the preceding utf-8 codepoint when erasing an orphan continuation byte");
  std::string incomplete_starter = std::string("a") + std::string("\xE2", 1);
  ava::tui::erase_last_utf8_codepoint(incomplete_starter);
  expect(incomplete_starter == "a", "tui backspace erases an incomplete trailing utf-8 starter byte");
  std::string incomplete_starter_with_continuation = std::string("a") + std::string("\xF0\x9F", 2);
  ava::tui::erase_last_utf8_codepoint(incomplete_starter_with_continuation);
  expect(incomplete_starter_with_continuation == std::string("a") + std::string("\xF0", 1),
         "tui backspace erases only one byte from an incomplete trailing utf-8 sequence");

  ava::tui::ComposerDraftState draft;
  expect(ava::tui::insert_composer_draft_text(draft, std::string("a") + "\xC3\xA9") && draft.text == std::string("a") + "\xC3\xA9" &&
             draft.cursor == draft.text.size(),
         "tui draft editor inserts utf-8 text at the cursor");
  expect(ava::tui::apply_composer_draft_action(draft, ava::tui::TuiAction::DeleteBackward) && draft.text == "a",
         "tui draft editor deletes a complete utf-8 codepoint before the cursor");
  expect(ava::tui::apply_composer_draft_action(draft, ava::tui::TuiAction::Undo) && draft.text == std::string("a") + "\xC3\xA9",
         "tui draft editor undo restores the previous edit");
  expect(ava::tui::apply_composer_draft_action(draft, ava::tui::TuiAction::Redo) && draft.text == "a", "tui draft editor redo reapplies the undone edit");
  expect(ava::tui::apply_composer_draft_action(draft, ava::tui::TuiAction::Undo) && draft.text == std::string("a") + "\xC3\xA9",
         "tui draft editor can undo again after redo");
  ava::tui::reset_composer_draft(draft, "alpha beta", std::string("alpha ").size());
  expect(ava::tui::replace_composer_draft_range(draft, std::string("alpha ").size(), draft.text.size(), "TWO") && draft.text == "alpha TWO" &&
             draft.cursor == std::string("alpha TWO").size(),
         "tui draft editor replaces selected byte ranges and moves the cursor after the replacement");
  expect(ava::tui::apply_composer_draft_action(draft, ava::tui::TuiAction::Undo) && draft.text == "alpha beta" && draft.cursor == std::string("alpha ").size(),
         "tui draft editor range replacement participates in undo history");
  ava::tui::reset_composer_draft(draft, std::string("a") + "\xC3\xA9");
  expect(ava::tui::apply_composer_draft_action(draft, ava::tui::TuiAction::CursorLeft) && draft.cursor == 1,
         "tui draft editor moves left by full utf-8 codepoint boundaries");
  ava::tui::reset_composer_draft(draft, std::string("a") + "\xC3\xA9" + "b", 1);
  expect(ava::tui::apply_composer_draft_action(draft, ava::tui::TuiAction::DeleteForward) && draft.text == "ab" && draft.cursor == 1,
         "tui draft editor forward-delete removes a full utf-8 codepoint after the cursor");
  ava::tui::reset_composer_draft(draft, std::string("a") + "\xC3\xA9", 1);
  expect(ava::tui::apply_composer_draft_action(draft, ava::tui::TuiAction::DeleteBackward) && draft.text == std::string("\xC3\xA9"),
         "tui draft editor preserves the utf-8 codepoint after deleting preceding ascii text");
  ava::tui::reset_composer_draft(draft, "one two three");
  expect(ava::tui::apply_composer_draft_action(draft, ava::tui::TuiAction::CursorWordLeft) && draft.cursor == 8,
         "tui draft editor moves to the previous word start");
  expect(ava::tui::apply_composer_draft_action(draft, ava::tui::TuiAction::DeleteWordBackward) && draft.text == "one three" && draft.kill_buffer == "two ",
         "tui draft editor deletes the previous word into the kill buffer");
  ava::tui::reset_composer_draft(draft, "one two three", 3);
  expect(ava::tui::apply_composer_draft_action(draft, ava::tui::TuiAction::DeleteWordForward) && draft.text == "one three" && draft.cursor == 3 &&
             draft.kill_buffer == " two",
         "tui draft editor deletes the next word into the kill buffer");
  expect(ava::tui::apply_composer_draft_action(draft, ava::tui::TuiAction::Undo) && draft.text == "one two three" && draft.cursor == 3,
         "tui draft editor undo restores forward word deletion");
  ava::tui::reset_composer_draft(draft, "path/to/file", 0);
  expect(ava::tui::apply_composer_draft_action(draft, ava::tui::TuiAction::CursorWordRight) && draft.cursor == 4 &&
             ava::tui::apply_composer_draft_action(draft, ava::tui::TuiAction::CursorWordRight) && draft.cursor == 5 &&
             ava::tui::apply_composer_draft_action(draft, ava::tui::TuiAction::CursorWordRight) && draft.cursor == 7 &&
             ava::tui::apply_composer_draft_action(draft, ava::tui::TuiAction::CursorWordRight) && draft.cursor == 8 &&
             ava::tui::apply_composer_draft_action(draft, ava::tui::TuiAction::CursorWordRight) && draft.cursor == draft.text.size(),
         "tui draft editor word-right stops at path punctuation boundaries");
  expect(ava::tui::apply_composer_draft_action(draft, ava::tui::TuiAction::CursorWordLeft) && draft.cursor == 8 &&
             ava::tui::apply_composer_draft_action(draft, ava::tui::TuiAction::CursorWordLeft) && draft.cursor == 7 &&
             ava::tui::apply_composer_draft_action(draft, ava::tui::TuiAction::CursorWordLeft) && draft.cursor == 5 &&
             ava::tui::apply_composer_draft_action(draft, ava::tui::TuiAction::CursorWordLeft) && draft.cursor == 4 &&
             ava::tui::apply_composer_draft_action(draft, ava::tui::TuiAction::CursorWordLeft) && draft.cursor == 0,
         "tui draft editor word-left stops at path punctuation boundaries");
  ava::tui::reset_composer_draft(draft, "foo...bar", 0);
  expect(ava::tui::apply_composer_draft_action(draft, ava::tui::TuiAction::CursorWordRight) && draft.cursor == 3 &&
             ava::tui::apply_composer_draft_action(draft, ava::tui::TuiAction::CursorWordRight) && draft.cursor == 6 &&
             ava::tui::apply_composer_draft_action(draft, ava::tui::TuiAction::CursorWordRight) && draft.cursor == draft.text.size(),
         "tui draft editor word-right treats punctuation runs as one segment");
  expect(ava::tui::apply_composer_draft_action(draft, ava::tui::TuiAction::CursorWordLeft) && draft.cursor == 6 &&
             ava::tui::apply_composer_draft_action(draft, ava::tui::TuiAction::CursorWordLeft) && draft.cursor == 3 &&
             ava::tui::apply_composer_draft_action(draft, ava::tui::TuiAction::CursorWordLeft) && draft.cursor == 0,
         "tui draft editor word-left treats punctuation runs as one segment");
  ava::tui::reset_composer_draft(draft, "path/to/file", 7);
  expect(ava::tui::apply_composer_draft_action(draft, ava::tui::TuiAction::DeleteWordBackward) && draft.text == "path//file" && draft.cursor == 5 &&
             draft.kill_buffer == "to",
         "tui draft editor word-backspace deletes only the preceding path segment");
  ava::tui::reset_composer_draft(draft, "path/to/file", 4);
  expect(ava::tui::apply_composer_draft_action(draft, ava::tui::TuiAction::DeleteWordForward) && draft.text == "pathto/file" && draft.cursor == 4 &&
             draft.kill_buffer == "/",
         "tui draft editor forward word deletion can delete a punctuation segment independently");
  auto const cjk_hello = std::string("\xE4\xBD\xA0\xE5\xA5\xBD");
  auto const fullwidth_comma = std::string("\xEF\xBC\x8C");
  auto const cjk_world = std::string("\xE4\xB8\x96\xE7\x95\x8C");
  auto const cjk_sentence = cjk_hello + fullwidth_comma + cjk_world;
  auto const cjk_after_hello = cjk_hello.size();
  auto const cjk_after_comma = cjk_after_hello + fullwidth_comma.size();
  ava::tui::reset_composer_draft(draft, cjk_sentence);
  expect(ava::tui::apply_composer_draft_action(draft, ava::tui::TuiAction::CursorWordLeft) && draft.cursor == cjk_after_comma,
         "tui draft editor word-left stops after fullwidth punctuation before a CJK word");
  expect(ava::tui::apply_composer_draft_action(draft, ava::tui::TuiAction::CursorWordLeft) && draft.cursor == cjk_after_hello,
         "tui draft editor word-left treats fullwidth punctuation as its own segment");
  expect(ava::tui::apply_composer_draft_action(draft, ava::tui::TuiAction::CursorWordLeft) && draft.cursor == 0,
         "tui draft editor word-left moves over the preceding CJK word");
  expect(ava::tui::apply_composer_draft_action(draft, ava::tui::TuiAction::CursorWordRight) && draft.cursor == cjk_after_hello,
         "tui draft editor word-right moves over the first CJK word");
  expect(ava::tui::apply_composer_draft_action(draft, ava::tui::TuiAction::CursorWordRight) && draft.cursor == cjk_after_comma,
         "tui draft editor word-right stops after fullwidth punctuation");
  expect(ava::tui::apply_composer_draft_action(draft, ava::tui::TuiAction::CursorWordRight) && draft.cursor == cjk_sentence.size(),
         "tui draft editor word-right moves over the final CJK word");
  auto const mixed_cjk = std::string("hello") + cjk_hello + fullwidth_comma + "world" + cjk_world;
  auto const mixed_after_ascii_hello = std::string("hello").size();
  auto const mixed_after_cjk_hello = mixed_after_ascii_hello + cjk_hello.size();
  auto const mixed_after_comma = mixed_after_cjk_hello + fullwidth_comma.size();
  auto const mixed_after_ascii_world = mixed_after_comma + std::string("world").size();
  ava::tui::reset_composer_draft(draft, mixed_cjk);
  expect(ava::tui::apply_composer_draft_action(draft, ava::tui::TuiAction::CursorWordLeft) && draft.cursor == mixed_after_ascii_world,
         "tui draft editor word-left separates adjacent ASCII and CJK words");
  expect(ava::tui::apply_composer_draft_action(draft, ava::tui::TuiAction::CursorWordLeft) && draft.cursor == mixed_after_comma,
         "tui draft editor word-left moves over ASCII after fullwidth punctuation");
  expect(ava::tui::apply_composer_draft_action(draft, ava::tui::TuiAction::CursorWordLeft) && draft.cursor == mixed_after_cjk_hello,
         "tui draft editor word-left stops on fullwidth punctuation in mixed text");
  expect(ava::tui::apply_composer_draft_action(draft, ava::tui::TuiAction::CursorWordLeft) && draft.cursor == mixed_after_ascii_hello,
         "tui draft editor word-left moves over CJK before ASCII");
  expect(ava::tui::apply_composer_draft_action(draft, ava::tui::TuiAction::CursorWordLeft) && draft.cursor == 0,
         "tui draft editor word-left moves over leading ASCII before CJK");
  expect(ava::tui::apply_composer_draft_action(draft, ava::tui::TuiAction::CursorWordRight) && draft.cursor == mixed_after_ascii_hello &&
             ava::tui::apply_composer_draft_action(draft, ava::tui::TuiAction::CursorWordRight) && draft.cursor == mixed_after_cjk_hello &&
             ava::tui::apply_composer_draft_action(draft, ava::tui::TuiAction::CursorWordRight) && draft.cursor == mixed_after_comma &&
             ava::tui::apply_composer_draft_action(draft, ava::tui::TuiAction::CursorWordRight) && draft.cursor == mixed_after_ascii_world &&
             ava::tui::apply_composer_draft_action(draft, ava::tui::TuiAction::CursorWordRight) && draft.cursor == mixed_cjk.size(),
         "tui draft editor word-right steps through mixed ASCII, CJK, and fullwidth punctuation segments");
  ava::tui::reset_composer_draft(draft, cjk_sentence, cjk_after_comma);
  expect(ava::tui::apply_composer_draft_action(draft, ava::tui::TuiAction::DeleteWordBackward) && draft.text == cjk_hello + cjk_world &&
             draft.cursor == cjk_after_hello && draft.kill_buffer == fullwidth_comma,
         "tui draft editor word-backspace deletes fullwidth punctuation as its own segment");
  ava::tui::reset_composer_draft(draft, "first\nsecond line");
  expect(ava::tui::apply_composer_draft_action(draft, ava::tui::TuiAction::CursorLineStart) && draft.cursor == 6,
         "tui draft editor moves to the start of the current line");
  expect(ava::tui::apply_composer_draft_action(draft, ava::tui::TuiAction::CursorLineEnd) && draft.cursor == draft.text.size(),
         "tui draft editor moves to the end of the current line");
  expect(ava::tui::apply_composer_draft_action(draft, ava::tui::TuiAction::DeleteToLineStart) && draft.text == "first\n" && draft.kill_buffer == "second line",
         "tui draft editor deletes from cursor to line start into the kill buffer");
  expect(ava::tui::apply_composer_draft_action(draft, ava::tui::TuiAction::Yank) && draft.text == "first\nsecond line",
         "tui draft editor yanks the last killed line text");
  expect(ava::tui::apply_composer_draft_action(draft, ava::tui::TuiAction::Undo) && draft.text == "first\n", "tui draft editor undo removes the yanked text");
  ava::tui::reset_composer_draft(draft, "first\nsecond line", 6);
  expect(ava::tui::apply_composer_draft_action(draft, ava::tui::TuiAction::DeleteToLineEnd) && draft.text == "first\n" && draft.kill_buffer == "second line",
         "tui draft editor deletes from cursor to line end into the kill buffer");
  ava::tui::reset_composer_draft(draft, "join\nline", 4);
  expect(ava::tui::apply_composer_draft_action(draft, ava::tui::TuiAction::DeleteToLineEnd) && draft.text == "joinline" && draft.cursor == 4 &&
             draft.kill_buffer == "\n",
         "tui draft editor joins the next line when deleting at line end");
  expect(ava::tui::apply_composer_draft_action(draft, ava::tui::TuiAction::DeleteToLineEnd) && draft.text == "join" && draft.cursor == 4 &&
             draft.kill_buffer == "\nline",
         "tui draft editor accumulates consecutive forward kills into one kill-ring entry");
  ava::tui::reset_composer_draft(draft, "last", 4);
  expect(!ava::tui::apply_composer_draft_action(draft, ava::tui::TuiAction::DeleteToLineEnd) && draft.text == "last" && draft.cursor == 4,
         "tui draft editor leaves final line end unchanged when there is nothing to delete");
  ava::tui::reset_composer_draft(draft, "abc\ndef\nghij", 6);
  expect(ava::tui::apply_composer_draft_action(draft, ava::tui::TuiAction::CursorUp) && draft.cursor == 2,
         "tui draft editor moves vertically to the previous line at the same column");
  expect(!ava::tui::apply_composer_draft_action(draft, ava::tui::TuiAction::CursorUp) && draft.cursor == 2,
         "tui draft editor leaves cursor unchanged when vertical movement is already at the first line");
  expect(ava::tui::apply_composer_draft_action(draft, ava::tui::TuiAction::CursorDown) && draft.cursor == 6 &&
             ava::tui::apply_composer_draft_action(draft, ava::tui::TuiAction::CursorDown) && draft.cursor == 10,
         "tui draft editor moves vertically to following lines at the same column");
  ava::tui::reset_composer_draft(draft, "abcdef\nx\nabcdef", 5);
  expect(ava::tui::apply_composer_draft_action(draft, ava::tui::TuiAction::CursorDown) && draft.cursor == 8 &&
             ava::tui::apply_composer_draft_action(draft, ava::tui::TuiAction::CursorDown) && draft.cursor == 14,
         "tui draft editor preserves sticky column across shorter lines");
  ava::tui::reset_composer_draft(draft, "abcdef\nx\nabcdef", 5);
  expect(ava::tui::apply_composer_draft_action(draft, ava::tui::TuiAction::CursorDown) && draft.cursor == 8 &&
             ava::tui::apply_composer_draft_action(draft, ava::tui::TuiAction::CursorLeft) && draft.cursor == 7 &&
             ava::tui::apply_composer_draft_action(draft, ava::tui::TuiAction::CursorDown) && draft.cursor == 9,
         "tui draft editor resets sticky column after horizontal movement");
  ava::tui::reset_composer_draft(draft, "hello world", 0);
  expect(ava::tui::jump_composer_draft_to_character(draft, "o", true) && draft.cursor == 4,
         "tui draft editor jumps forward to the first matching character after the cursor");
  expect(ava::tui::jump_composer_draft_to_character(draft, "o", true) && draft.cursor == 7,
         "tui draft editor jumps forward to the next matching character after the cursor");
  expect(!ava::tui::jump_composer_draft_to_character(draft, "z", true) && draft.cursor == 7,
         "tui draft editor leaves the cursor unchanged when a forward jump target is missing");
  expect(ava::tui::jump_composer_draft_to_character(draft, "h", false) && draft.cursor == 0, "tui draft editor jumps backward across the current draft");
  ava::tui::reset_composer_draft(draft, "abc\ndef\nghi", 0);
  expect(ava::tui::jump_composer_draft_to_character(draft, "g", true) && draft.cursor == 8, "tui draft editor jumps forward across lines");
  expect(ava::tui::jump_composer_draft_to_character(draft, "d", false) && draft.cursor == 4, "tui draft editor jumps backward across lines");
  ava::tui::reset_composer_draft(draft, "Hello World", 0);
  expect(!ava::tui::jump_composer_draft_to_character(draft, "h", true) && draft.cursor == 0 && ava::tui::jump_composer_draft_to_character(draft, "W", true) &&
             draft.cursor == 6,
         "tui draft editor character jumps are case-sensitive");
  ava::tui::reset_composer_draft(draft, "pending message");
  expect(ava::tui::apply_composer_draft_action(draft, ava::tui::TuiAction::ClearInput) && draft.text.empty() && draft.cursor == 0 &&
             draft.kill_buffer == "pending message",
         "tui draft editor clears a non-empty composer draft into the kill buffer");
  expect(ava::tui::apply_composer_draft_action(draft, ava::tui::TuiAction::Undo) && draft.text == "pending message",
         "tui draft editor undo restores a cleared composer draft");
  ava::tui::ComposerDraftState backslash_newline_draft;
  expect(ava::tui::insert_composer_draft_text(backslash_newline_draft, "\\") && backslash_newline_draft.text == "\\",
         "tui draft editor inserts backslash visibly before Enter workaround");
  expect(ava::tui::replace_composer_backslash_before_cursor_with_newline(backslash_newline_draft) && backslash_newline_draft.text == "\n" &&
             backslash_newline_draft.cursor == 1,
         "tui draft editor converts a standalone backslash before cursor into a newline");
  expect(ava::tui::apply_composer_draft_action(backslash_newline_draft, ava::tui::TuiAction::Undo) && backslash_newline_draft.text == "\\",
         "tui draft editor can undo the backslash-enter newline workaround");
  ava::tui::reset_composer_draft(backslash_newline_draft, "\\\\\\");
  expect(ava::tui::replace_composer_backslash_before_cursor_with_newline(backslash_newline_draft) && backslash_newline_draft.text == "\\\\\n",
         "tui draft editor removes only the backslash immediately before cursor for Enter newline workaround");
  ava::tui::reset_composer_draft(backslash_newline_draft, "\\x");
  expect(!ava::tui::replace_composer_backslash_before_cursor_with_newline(backslash_newline_draft) && backslash_newline_draft.text == "\\x",
         "tui draft editor leaves ordinary backslash text alone when backslash is not before cursor");
  ava::tui::reset_composer_draft(backslash_newline_draft, "\\x", 1);
  expect(ava::tui::replace_composer_backslash_before_cursor_with_newline(backslash_newline_draft) && backslash_newline_draft.text == "\nx",
         "tui draft editor applies the backslash-enter newline workaround at the current cursor");
  ava::tui::ComposerDraftState ring_draft;
  ava::tui::reset_composer_draft(ring_draft, "alpha beta gamma");
  expect(ava::tui::apply_composer_draft_action(ring_draft, ava::tui::TuiAction::DeleteWordBackward) && ring_draft.text == "alpha beta " &&
             ring_draft.kill_buffer == "gamma",
         "tui draft editor records killed text in a ring");
  expect(ava::tui::apply_composer_draft_action(ring_draft, ava::tui::TuiAction::DeleteWordBackward) && ring_draft.text == "alpha " &&
             ring_draft.kill_buffer == "beta gamma" && ring_draft.kill_ring.size() == 1 && ring_draft.kill_ring.front() == "beta gamma",
         "tui draft editor prepends consecutive backward kills into one kill-ring entry");
  expect(ava::tui::apply_composer_draft_action(ring_draft, ava::tui::TuiAction::Yank) && ring_draft.text == "alpha beta gamma",
         "tui draft editor yanks the accumulated kill-ring entry");
  ava::tui::reset_composer_draft(ring_draft, "one two three");
  ring_draft.kill_ring.clear();
  ring_draft.kill_buffer.clear();
  ring_draft.kill_sequence = ava::tui::ComposerKillSequence::None;
  expect(ava::tui::apply_composer_draft_action(ring_draft, ava::tui::TuiAction::DeleteWordBackward) && ring_draft.kill_buffer == "three" &&
             ring_draft.kill_ring.size() == 1,
         "tui draft editor seeds a fresh kill-ring entry after reset");
  expect(ava::tui::apply_composer_draft_action(ring_draft, ava::tui::TuiAction::CursorLeft) &&
             ava::tui::apply_composer_draft_action(ring_draft, ava::tui::TuiAction::DeleteWordBackward) && ring_draft.kill_buffer == "two" &&
             ring_draft.kill_ring.size() == 2 && ring_draft.kill_ring.front() == "two" && ring_draft.kill_ring[1] == "three",
         "tui draft editor breaks kill accumulation after cursor movement");
  expect(ava::tui::apply_composer_draft_action(ring_draft, ava::tui::TuiAction::Yank) && ring_draft.kill_buffer == "two",
         "tui draft editor yanks the newest kill-ring entry after a broken sequence");
  expect(ava::tui::apply_composer_draft_action(ring_draft, ava::tui::TuiAction::YankPop) && ring_draft.kill_buffer == "three",
         "tui draft editor yank-pop swaps the previous yank with the next kill-ring entry");
  expect(ava::tui::apply_composer_draft_action(ring_draft, ava::tui::TuiAction::YankPop) && ring_draft.kill_buffer == "two",
         "tui draft editor yank-pop cycles through the kill ring");

  std::vector<std::string> input_history;
  expect(!ava::tui::push_composer_input_history(input_history, "   \t  "), "tui input history ignores empty and whitespace-only submissions");
  expect(ava::tui::push_composer_input_history(input_history, " first prompt ") && input_history.back() == "first prompt",
         "tui input history trims submitted prompts before storing them");
  expect(!ava::tui::push_composer_input_history(input_history, "first prompt"), "tui input history skips consecutive duplicates");
  expect(ava::tui::push_composer_input_history(input_history, "second prompt") && ava::tui::push_composer_input_history(input_history, "first prompt"),
         "tui input history keeps non-consecutive duplicates");

  ava::tui::ComposerDraftState history_draft;
  ava::tui::reset_composer_draft(history_draft, "draft text", 5);
  std::optional<std::size_t> history_index;
  std::string draft_input;
  expect(ava::tui::browse_composer_input_history(history_draft, input_history, history_index, draft_input, true) && history_draft.text == "first prompt" &&
             history_draft.cursor == 0 && history_index && *history_index == 2,
         "tui input history recalls the newest item with the cursor at the start on previous");
  expect(ava::tui::browse_composer_input_history(history_draft, input_history, history_index, draft_input, true) && history_draft.text == "second prompt" &&
             history_draft.cursor == 0 && history_index && *history_index == 1,
         "tui input history walks backward through older items");
  expect(ava::tui::browse_composer_input_history(history_draft, input_history, history_index, draft_input, false) && history_draft.text == "first prompt" &&
             history_draft.cursor == std::string("first prompt").size() && history_index && *history_index == 2,
         "tui input history walks forward toward newer items with the cursor at the end");
  expect(ava::tui::browse_composer_input_history(history_draft, input_history, history_index, draft_input, false) && history_draft.text == "draft text" &&
             history_draft.cursor == std::string("draft text").size() && !history_index,
         "tui input history restores the in-progress draft after the newest item");
  expect(!ava::tui::browse_composer_input_history(history_draft, input_history, history_index, draft_input, false),
         "tui input history next does nothing when not browsing");
  static_cast<void>(ava::tui::browse_composer_input_history(history_draft, input_history, history_index, draft_input, true));
  ava::tui::clear_composer_input_history_browse(history_index, draft_input);
  expect(!history_index && draft_input.empty(), "tui input history browsing state clears when the draft is edited");

  std::vector<std::string> capped_history;
  for (int index = 0; index < 105; ++index)
    static_cast<void>(ava::tui::push_composer_input_history(capped_history, "item " + std::to_string(index)));
  expect(capped_history.size() == 100 && capped_history.front() == "item 5" && capped_history.back() == "item 104",
         "tui input history keeps the newest 100 entries");

  expect(ava::tui::normalize_composer_paste_text("one\r\ntwo\rthree") == "one\ntwo\nthree",
         "tui paste normalizes crlf and lone carriage returns into newlines");
  expect(ava::tui::normalize_composer_paste_text(std::string("a\x01\tb\n", 5) + "\xC3\xA9") == std::string("a\tb\n", 4) + "\xC3\xA9",
         "tui paste strips controls while preserving tabs, newlines, and utf-8 bytes");
  ava::tui::ComposerDraftState paste_draft;
  expect(ava::tui::insert_composer_paste_text(paste_draft, "one\ntwo") && paste_draft.text == "one\ntwo" &&
             ava::tui::expanded_composer_draft_text(paste_draft) == "one\ntwo",
         "tui small bracketed paste remains literal in the draft and submitted text");
  ava::tui::reset_composer_draft(paste_draft);
  auto const large_paste = std::string("line01\nline02\nline03\nline04\nline05\nline06\nline07\nline08\nline09\nline10\nline11");
  expect(ava::tui::insert_composer_paste_text(paste_draft, large_paste) && paste_draft.text == "[paste #1 +11 lines]" &&
             paste_draft.cursor == paste_draft.text.size() && ava::tui::expanded_composer_draft_text(paste_draft) == large_paste,
         "tui large bracketed paste collapses to a marker while preserving exact submitted text");
  auto make_marker_draft = [&]() {
    ava::tui::ComposerDraftState marker_draft;
    static_cast<void>(ava::tui::insert_composer_draft_text(marker_draft, "A"));
    static_cast<void>(ava::tui::insert_composer_paste_text(marker_draft, large_paste));
    static_cast<void>(ava::tui::insert_composer_draft_text(marker_draft, "B"));
    return marker_draft;
  };
  auto marker_draft = make_marker_draft();
  auto const marker_size = marker_draft.text.size() - 2;
  marker_draft.cursor = 0;
  expect(ava::tui::apply_composer_draft_action(marker_draft, ava::tui::TuiAction::CursorRight) && marker_draft.cursor == 1 &&
             ava::tui::apply_composer_draft_action(marker_draft, ava::tui::TuiAction::CursorRight) && marker_draft.cursor == 1 + marker_size &&
             ava::tui::apply_composer_draft_action(marker_draft, ava::tui::TuiAction::CursorRight) && marker_draft.cursor == marker_draft.text.size(),
         "tui draft editor treats recorded paste markers as one unit for right-arrow movement");
  expect(ava::tui::apply_composer_draft_action(marker_draft, ava::tui::TuiAction::CursorLeft) && marker_draft.cursor == 1 + marker_size &&
             ava::tui::apply_composer_draft_action(marker_draft, ava::tui::TuiAction::CursorLeft) && marker_draft.cursor == 1,
         "tui draft editor treats recorded paste markers as one unit for left-arrow movement");
  auto const marker_start = std::size_t{1};
  auto const marker_end = marker_start + marker_size;
  expect(ava::tui::clamp_composer_draft_cursor_to_atomic_boundary(marker_draft, marker_start + 1) == marker_start &&
             ava::tui::clamp_composer_draft_cursor_to_atomic_boundary(marker_draft, marker_end - 1) == marker_end &&
             ava::tui::clamp_composer_draft_cursor_to_atomic_boundary(marker_draft, marker_start) == marker_start &&
             ava::tui::clamp_composer_draft_cursor_to_atomic_boundary(marker_draft, marker_end) == marker_end,
         "tui draft editor snaps cursor positions inside recorded paste markers to atomic boundaries");
  auto word_marker_draft = [&]() {
    ava::tui::ComposerDraftState marker_word_draft;
    static_cast<void>(ava::tui::insert_composer_draft_text(marker_word_draft, "X "));
    static_cast<void>(ava::tui::insert_composer_paste_text(marker_word_draft, large_paste));
    static_cast<void>(ava::tui::insert_composer_draft_text(marker_word_draft, " Y"));
    return marker_word_draft;
  }();
  auto const word_marker_size = word_marker_draft.text.size() - 4;
  word_marker_draft.cursor = 0;
  expect(ava::tui::apply_composer_draft_action(word_marker_draft, ava::tui::TuiAction::CursorWordRight) && word_marker_draft.cursor == 1 &&
             ava::tui::apply_composer_draft_action(word_marker_draft, ava::tui::TuiAction::CursorWordRight) &&
             word_marker_draft.cursor == 2 + word_marker_size &&
             ava::tui::apply_composer_draft_action(word_marker_draft, ava::tui::TuiAction::CursorWordRight) &&
             word_marker_draft.cursor == word_marker_draft.text.size(),
         "tui draft editor treats recorded paste markers as one unit for word-right movement");
  expect(ava::tui::apply_composer_draft_action(word_marker_draft, ava::tui::TuiAction::CursorWordLeft) && word_marker_draft.cursor == 3 + word_marker_size &&
             ava::tui::apply_composer_draft_action(word_marker_draft, ava::tui::TuiAction::CursorWordLeft) && word_marker_draft.cursor == 2,
         "tui draft editor treats recorded paste markers as one unit for word-left movement");
  word_marker_draft.cursor = 2 + word_marker_size;
  expect(ava::tui::apply_composer_draft_action(word_marker_draft, ava::tui::TuiAction::DeleteWordBackward) && word_marker_draft.text == "X  Y" &&
             word_marker_draft.cursor == 2 && ava::tui::expanded_composer_draft_text(word_marker_draft) == "X  Y",
         "tui draft editor deletes a recorded paste marker as one unit with word-backspace");
  auto forward_word_marker_draft = [&]() {
    ava::tui::ComposerDraftState marker_word_draft;
    static_cast<void>(ava::tui::insert_composer_draft_text(marker_word_draft, "X "));
    static_cast<void>(ava::tui::insert_composer_paste_text(marker_word_draft, large_paste));
    static_cast<void>(ava::tui::insert_composer_draft_text(marker_word_draft, " Y"));
    return marker_word_draft;
  }();
  forward_word_marker_draft.cursor = 2;
  expect(ava::tui::apply_composer_draft_action(forward_word_marker_draft, ava::tui::TuiAction::DeleteWordForward) && forward_word_marker_draft.text == "X  Y" &&
             forward_word_marker_draft.cursor == 2 && ava::tui::expanded_composer_draft_text(forward_word_marker_draft) == "X  Y",
         "tui draft editor deletes a recorded paste marker as one unit with forward word deletion");
  auto delete_marker_draft = make_marker_draft();
  delete_marker_draft.cursor = delete_marker_draft.text.size() - 1;
  expect(ava::tui::apply_composer_draft_action(delete_marker_draft, ava::tui::TuiAction::DeleteBackward) && delete_marker_draft.text == "AB" &&
             delete_marker_draft.cursor == 1 && ava::tui::expanded_composer_draft_text(delete_marker_draft) == "AB",
         "tui draft editor deletes a recorded paste marker as one unit with backspace");
  auto forward_delete_marker_draft = make_marker_draft();
  forward_delete_marker_draft.cursor = 1;
  expect(ava::tui::apply_composer_draft_action(forward_delete_marker_draft, ava::tui::TuiAction::DeleteForward) && forward_delete_marker_draft.text == "AB" &&
             forward_delete_marker_draft.cursor == 1 && ava::tui::expanded_composer_draft_text(forward_delete_marker_draft) == "AB",
         "tui draft editor deletes a recorded paste marker as one unit with forward delete");
  expect(ava::tui::apply_composer_draft_action(paste_draft, ava::tui::TuiAction::Undo) && paste_draft.text.empty() &&
             ava::tui::apply_composer_draft_action(paste_draft, ava::tui::TuiAction::Redo) &&
             ava::tui::expanded_composer_draft_text(paste_draft) == large_paste,
         "tui large paste markers survive undo and redo expansion");
  ava::tui::reset_composer_draft(paste_draft, "[paste #1 +11 lines]");
  expect(ava::tui::expanded_composer_draft_text(paste_draft) == "[paste #1 +11 lines]",
         "tui manually typed paste-marker text is not expanded without a recorded paste entry");
  expect(ava::tui::apply_composer_draft_action(paste_draft, ava::tui::TuiAction::CursorLineStart) &&
             ava::tui::apply_composer_draft_action(paste_draft, ava::tui::TuiAction::CursorRight) && paste_draft.cursor == 1,
         "tui manually typed paste-marker text is not treated as an atomic marker");
  expect(ava::tui::clamp_composer_draft_cursor_to_atomic_boundary(paste_draft, 3) == 3,
         "tui manually typed paste-marker text does not receive recorded-marker cursor snapping");
  auto duplicate_marker_draft = make_marker_draft();
  auto const duplicate_marker_text = duplicate_marker_draft.text.substr(1, marker_size);
  duplicate_marker_draft.cursor = duplicate_marker_draft.text.size();
  static_cast<void>(ava::tui::insert_composer_draft_text(duplicate_marker_draft, duplicate_marker_text));
  auto const duplicate_marker_start = duplicate_marker_draft.text.size() - duplicate_marker_text.size();
  duplicate_marker_draft.cursor = duplicate_marker_start;
  expect(ava::tui::apply_composer_draft_action(duplicate_marker_draft, ava::tui::TuiAction::CursorRight) &&
             duplicate_marker_draft.cursor == duplicate_marker_start + 1 &&
             ava::tui::expanded_composer_draft_text(duplicate_marker_draft) == std::string("A") + large_paste + "B" + duplicate_marker_text,
         "tui duplicate manually typed paste-marker text remains literal beside a recorded marker");
  duplicate_marker_draft.cursor = duplicate_marker_start;
  expect(ava::tui::apply_composer_draft_action(duplicate_marker_draft, ava::tui::TuiAction::CursorWordRight) &&
             duplicate_marker_draft.cursor == duplicate_marker_start + 1,
         "tui duplicate manually typed paste-marker text is punctuation-aware but not atomic for word movement");
  ava::tui::reset_composer_draft(paste_draft);
  auto const long_single_line = std::string(2001, 'x');
  expect(ava::tui::insert_composer_paste_text(paste_draft, long_single_line) && paste_draft.text == "[paste #1 2001 chars]" &&
             ava::tui::expanded_composer_draft_text(paste_draft) == long_single_line,
         "tui large single-line paste collapses by byte count and expands for submit");

  // Wave A editor fidelity: cluster-aware editing, typing undo groups, kill accumulation.
  auto const combining_acute = std::string("\xCC\x81");
  auto const e_combining = std::string("e") + combining_acute;
  auto const regional_c = std::string("\xF0\x9F\x87\xA8");
  auto const regional_n = std::string("\xF0\x9F\x87\xB3");
  auto const flag_cn = regional_c + regional_n;
  auto const thumbs_up = std::string("\xF0\x9F\x91\x8D");
  auto const light_skin_tone = std::string("\xF0\x9F\x8F\xBB");
  auto const skin_tone_thumbs = thumbs_up + light_skin_tone;
  auto const man = std::string("\xF0\x9F\x91\xA8");
  auto const woman = std::string("\xF0\x9F\x91\xA9");
  auto const girl = std::string("\xF0\x9F\x91\xA7");
  auto const zwj = std::string("\xE2\x80\x8D");
  auto const family_zwj = man + zwj + woman + zwj + girl;
  auto const malformed_bytes = std::string("a") + std::string("\x80\xFF", 2) + "b";

  expect(ava::tui::detail::terminal_text_cluster_bytes(e_combining, 0) == e_combining.size() &&
             ava::tui::detail::terminal_text_cluster_bytes(flag_cn, 0) == flag_cn.size() &&
             ava::tui::detail::terminal_text_cluster_bytes(skin_tone_thumbs, 0) == skin_tone_thumbs.size() &&
             ava::tui::detail::terminal_text_cluster_bytes(family_zwj, 0) == family_zwj.size() &&
             ava::tui::detail::terminal_text_cluster_bytes(malformed_bytes, 1) == 1,
         "tui shared cluster helper covers combining marks, flags, skin tones, ZWJ families, and malformed bytes");

  ava::tui::ComposerDraftState cluster_draft;
  ava::tui::reset_composer_draft(cluster_draft, std::string("x") + e_combining + "y");
  expect(ava::tui::apply_composer_draft_action(cluster_draft, ava::tui::TuiAction::CursorLeft) && cluster_draft.cursor == 1 + e_combining.size() &&
             ava::tui::apply_composer_draft_action(cluster_draft, ava::tui::TuiAction::CursorLeft) && cluster_draft.cursor == 1 &&
             ava::tui::apply_composer_draft_action(cluster_draft, ava::tui::TuiAction::CursorLeft) && cluster_draft.cursor == 0,
         "tui draft editor moves left over base+combining clusters atomically");
  expect(ava::tui::apply_composer_draft_action(cluster_draft, ava::tui::TuiAction::CursorRight) && cluster_draft.cursor == 1 &&
             ava::tui::apply_composer_draft_action(cluster_draft, ava::tui::TuiAction::CursorRight) && cluster_draft.cursor == 1 + e_combining.size() &&
             ava::tui::apply_composer_draft_action(cluster_draft, ava::tui::TuiAction::CursorRight) && cluster_draft.cursor == cluster_draft.text.size(),
         "tui draft editor moves right over base+combining clusters atomically");
  ava::tui::reset_composer_draft(cluster_draft, std::string("x") + e_combining + "y");
  expect(ava::tui::apply_composer_draft_action(cluster_draft, ava::tui::TuiAction::DeleteBackward) && cluster_draft.text == std::string("x") + e_combining &&
             ava::tui::apply_composer_draft_action(cluster_draft, ava::tui::TuiAction::DeleteBackward) && cluster_draft.text == "x" &&
             cluster_draft.kill_buffer.empty(),
         "tui draft editor backspaces base+combining clusters without updating the kill ring");
  ava::tui::reset_composer_draft(cluster_draft, std::string("x") + e_combining + "y", 1);
  expect(ava::tui::apply_composer_draft_action(cluster_draft, ava::tui::TuiAction::DeleteForward) && cluster_draft.text == "xy" && cluster_draft.cursor == 1,
         "tui draft editor forward-deletes base+combining clusters atomically");

  ava::tui::reset_composer_draft(cluster_draft, std::string("a") + flag_cn + "b");
  expect(ava::tui::apply_composer_draft_action(cluster_draft, ava::tui::TuiAction::CursorLeft) && cluster_draft.cursor == 1 + flag_cn.size() &&
             ava::tui::apply_composer_draft_action(cluster_draft, ava::tui::TuiAction::CursorLeft) && cluster_draft.cursor == 1 &&
             ava::tui::apply_composer_draft_action(cluster_draft, ava::tui::TuiAction::DeleteForward) && cluster_draft.text == "ab",
         "tui draft editor treats regional-indicator flag pairs as one atomic cluster");

  ava::tui::reset_composer_draft(cluster_draft, std::string("a") + skin_tone_thumbs + "b");
  expect(ava::tui::apply_composer_draft_action(cluster_draft, ava::tui::TuiAction::CursorLeft) && cluster_draft.cursor == 1 + skin_tone_thumbs.size() &&
             ava::tui::apply_composer_draft_action(cluster_draft, ava::tui::TuiAction::DeleteBackward) && cluster_draft.text == "ab",
         "tui draft editor treats emoji skin-tone modifier sequences as one atomic cluster");

  ava::tui::reset_composer_draft(cluster_draft, std::string("a") + family_zwj + "b");
  expect(ava::tui::apply_composer_draft_action(cluster_draft, ava::tui::TuiAction::CursorLeft) && cluster_draft.cursor == 1 + family_zwj.size() &&
             ava::tui::apply_composer_draft_action(cluster_draft, ava::tui::TuiAction::DeleteBackward) && cluster_draft.text == "ab",
         "tui draft editor treats family ZWJ emoji sequences as one atomic cluster");

  ava::tui::reset_composer_draft(cluster_draft, malformed_bytes);
  expect(cluster_draft.text.size() == 4 && cluster_draft.cursor == 4, "tui draft editor loads malformed utf-8 draft at end");
  expect(ava::tui::apply_composer_draft_action(cluster_draft, ava::tui::TuiAction::DeleteBackward) &&
             cluster_draft.text == std::string("a") + std::string("\x80\xFF", 2) && cluster_draft.cursor == 3,
         "tui draft editor backspaces trailing ascii after malformed utf-8");
  expect(ava::tui::apply_composer_draft_action(cluster_draft, ava::tui::TuiAction::DeleteBackward) &&
             cluster_draft.text == std::string("a") + std::string("\x80", 1) && cluster_draft.cursor == 2,
         "tui draft editor backspaces the 0xFF malformed byte");
  expect(ava::tui::apply_composer_draft_action(cluster_draft, ava::tui::TuiAction::DeleteBackward) && cluster_draft.text == "a" && cluster_draft.cursor == 1,
         "tui draft editor backspaces the 0x80 malformed byte");
  expect(ava::tui::apply_composer_draft_action(cluster_draft, ava::tui::TuiAction::CursorLeft) && cluster_draft.cursor == 0 &&
             ava::tui::apply_composer_draft_action(cluster_draft, ava::tui::TuiAction::DeleteForward) && cluster_draft.text.empty(),
         "tui draft editor forward-deletes the remaining ascii after malformed cleanup");
  ava::tui::reset_composer_draft(cluster_draft, malformed_bytes, 1);
  expect(cluster_draft.cursor == 1, "tui draft editor keeps the cursor on orphan utf-8 continuation bytes");
  expect(ava::tui::apply_composer_draft_action(cluster_draft, ava::tui::TuiAction::DeleteForward) &&
             cluster_draft.text == std::string("a") + std::string("\xFF", 1) + "b" && cluster_draft.cursor == 1,
         "tui draft editor forward-deletes malformed utf-8 one byte at a time");
  expect(ava::tui::apply_composer_draft_action(cluster_draft, ava::tui::TuiAction::CursorLeft) && cluster_draft.cursor == 0 &&
             ava::tui::apply_composer_draft_action(cluster_draft, ava::tui::TuiAction::CursorRight) && cluster_draft.cursor == 1 &&
             ava::tui::apply_composer_draft_action(cluster_draft, ava::tui::TuiAction::CursorRight) && cluster_draft.cursor == 2,
         "tui draft editor moves across malformed utf-8 one byte at a time");

  auto const large_paste_for_atomic = std::string("line01\nline02\nline03\nline04\nline05\nline06\nline07\nline08\nline09\nline10\nline11");
  ava::tui::ComposerDraftState paste_atomic_draft;
  expect(ava::tui::insert_composer_draft_text(paste_atomic_draft, "A") && ava::tui::insert_composer_paste_text(paste_atomic_draft, large_paste_for_atomic) &&
             ava::tui::insert_composer_draft_text(paste_atomic_draft, "B"),
         "tui draft editor builds a paste-marker draft for atomicity checks");
  auto const paste_marker_size = paste_atomic_draft.text.size() - 2;
  paste_atomic_draft.cursor = paste_atomic_draft.text.size();
  expect(ava::tui::apply_composer_draft_action(paste_atomic_draft, ava::tui::TuiAction::DeleteBackward) &&
             paste_atomic_draft.text.size() == 1 + paste_marker_size &&
             ava::tui::apply_composer_draft_action(paste_atomic_draft, ava::tui::TuiAction::DeleteBackward) && paste_atomic_draft.text == "A",
         "tui draft editor deletes recorded paste markers as one atomic unit");

  ava::tui::ComposerDraftState undo_group_draft;
  expect(ava::tui::insert_composer_draft_text(undo_group_draft, "h") && ava::tui::insert_composer_draft_text(undo_group_draft, "i") &&
             ava::tui::insert_composer_draft_text(undo_group_draft, "!") && undo_group_draft.text == "hi!" && undo_group_draft.undo_stack.size() == 1,
         "tui draft editor coalesces contiguous ordinary typing into one undo group");
  expect(ava::tui::insert_composer_draft_text(undo_group_draft, " ") && undo_group_draft.text == "hi! " && undo_group_draft.undo_stack.size() == 2,
         "tui draft editor breaks typing undo groups at whitespace");
  expect(ava::tui::insert_composer_draft_text(undo_group_draft, "y") && ava::tui::insert_composer_draft_text(undo_group_draft, "o") &&
             undo_group_draft.text == "hi! yo" && undo_group_draft.undo_stack.size() == 3,
         "tui draft editor starts a new coalesced word-run undo group after whitespace");
  expect(ava::tui::apply_composer_draft_action(undo_group_draft, ava::tui::TuiAction::Undo) && undo_group_draft.text == "hi! " &&
             ava::tui::apply_composer_draft_action(undo_group_draft, ava::tui::TuiAction::Undo) && undo_group_draft.text == "hi!" &&
             ava::tui::apply_composer_draft_action(undo_group_draft, ava::tui::TuiAction::Undo) && undo_group_draft.text.empty(),
         "tui draft editor undoes coalesced word runs and whitespace boundaries as separate groups");
  expect(ava::tui::apply_composer_draft_action(undo_group_draft, ava::tui::TuiAction::Redo) && undo_group_draft.text == "hi!" &&
             ava::tui::apply_composer_draft_action(undo_group_draft, ava::tui::TuiAction::Redo) && undo_group_draft.text == "hi! " &&
             ava::tui::apply_composer_draft_action(undo_group_draft, ava::tui::TuiAction::Redo) && undo_group_draft.text == "hi! yo",
         "tui draft editor redo reapplies coalesced typing groups exactly");

  ava::tui::ComposerDraftState newline_undo_draft;
  expect(ava::tui::insert_composer_draft_text(newline_undo_draft, "ab") && ava::tui::insert_composer_draft_text(newline_undo_draft, "\n") &&
             ava::tui::insert_composer_draft_text(newline_undo_draft, "cd") && newline_undo_draft.undo_stack.size() == 3,
         "tui draft editor breaks typing undo groups at newlines");

  ava::tui::ComposerDraftState cursor_break_draft;
  expect(ava::tui::insert_composer_draft_text(cursor_break_draft, "ab") && cursor_break_draft.undo_stack.size() == 1,
         "tui draft editor records one undo group for an initial word run");
  expect(ava::tui::apply_composer_draft_action(cursor_break_draft, ava::tui::TuiAction::CursorLeft) &&
             ava::tui::insert_composer_draft_text(cursor_break_draft, "X") && cursor_break_draft.text == "aXb" && cursor_break_draft.undo_stack.size() == 2,
         "tui draft editor breaks typing undo groups after cursor movement");
  expect(ava::tui::apply_composer_draft_action(cursor_break_draft, ava::tui::TuiAction::Undo) && cursor_break_draft.text == "ab" &&
             ava::tui::apply_composer_draft_action(cursor_break_draft, ava::tui::TuiAction::Undo) && cursor_break_draft.text.empty(),
         "tui draft editor undoes post-cursor-break inserts separately from the earlier word run");

  ava::tui::ComposerDraftState undo_cap_draft;
  for (int index = 0; index < 120; ++index)
  {
    static_cast<void>(ava::tui::insert_composer_draft_text(undo_cap_draft, "x"));
    static_cast<void>(ava::tui::insert_composer_draft_text(undo_cap_draft, " "));
  }
  expect(undo_cap_draft.undo_stack.size() == 100, "tui draft editor caps undo history at 100 snapshots");

  ava::tui::ComposerDraftState forward_kill_draft;
  ava::tui::reset_composer_draft(forward_kill_draft, "alpha beta gamma", 0);
  expect(ava::tui::apply_composer_draft_action(forward_kill_draft, ava::tui::TuiAction::DeleteWordForward) && forward_kill_draft.kill_buffer == "alpha" &&
             ava::tui::apply_composer_draft_action(forward_kill_draft, ava::tui::TuiAction::DeleteWordForward) &&
             forward_kill_draft.kill_buffer == "alpha beta" && forward_kill_draft.kill_ring.size() == 1,
         "tui draft editor appends consecutive forward kills into one kill-ring entry");

  ava::tui::ComposerDraftState kill_cap_ring;
  for (int index = 0; index < 20; ++index)
  {
    auto piece = "w" + std::to_string(index);
    kill_cap_ring.text = piece;
    kill_cap_ring.cursor = piece.size();
    kill_cap_ring.kill_sequence = ava::tui::ComposerKillSequence::None;
    static_cast<void>(ava::tui::apply_composer_draft_action(kill_cap_ring, ava::tui::TuiAction::DeleteWordBackward));
    // Break the kill sequence so each word becomes its own ring entry.
    static_cast<void>(ava::tui::apply_composer_draft_action(kill_cap_ring, ava::tui::TuiAction::CursorLeft));
  }
  expect(kill_cap_ring.kill_ring.size() == 16 && kill_cap_ring.kill_ring.front() == "w19" && kill_cap_ring.kill_ring.back() == "w4",
         "tui draft editor caps the kill ring at 16 entries");

  // Cluster-invariant word movement/deletion and vertical snap regressions.
  auto const nbsp = std::string("\xC2\xA0");
  auto const ideographic_space = std::string("\xE3\x80\x80");
  ava::tui::ComposerDraftState word_cluster_draft;
  ava::tui::reset_composer_draft(word_cluster_draft, std::string("pre ") + family_zwj + " post");
  expect(ava::tui::apply_composer_draft_action(word_cluster_draft, ava::tui::TuiAction::CursorWordLeft) &&
             word_cluster_draft.cursor == std::string("pre ").size() + family_zwj.size() + 1 &&
             ava::tui::apply_composer_draft_action(word_cluster_draft, ava::tui::TuiAction::CursorWordLeft) &&
             word_cluster_draft.cursor == std::string("pre ").size() &&
             ava::tui::clamp_composer_draft_cursor_to_atomic_boundary(word_cluster_draft, word_cluster_draft.cursor) == word_cluster_draft.cursor,
         "tui draft editor word-left treats a ZWJ family cluster as one word unit");
  expect(ava::tui::apply_composer_draft_action(word_cluster_draft, ava::tui::TuiAction::CursorWordRight) &&
             word_cluster_draft.cursor == std::string("pre ").size() + family_zwj.size() &&
             ava::tui::clamp_composer_draft_cursor_to_atomic_boundary(word_cluster_draft, word_cluster_draft.cursor) == word_cluster_draft.cursor,
         "tui draft editor word-right advances over a whole ZWJ family cluster");
  ava::tui::reset_composer_draft(word_cluster_draft, std::string("pre ") + family_zwj + " post");
  expect(ava::tui::apply_composer_draft_action(word_cluster_draft, ava::tui::TuiAction::DeleteWordBackward) &&
             word_cluster_draft.text == std::string("pre ") + family_zwj + " " && word_cluster_draft.kill_buffer == "post" &&
             word_cluster_draft.text.find(family_zwj) != std::string::npos,
         "tui draft editor word-backspace deletes the trailing word without splitting a preceding ZWJ family");
  // Break the kill sequence so the next word-delete is observed independently.
  expect(ava::tui::apply_composer_draft_action(word_cluster_draft, ava::tui::TuiAction::CursorLineStart) &&
             ava::tui::apply_composer_draft_action(word_cluster_draft, ava::tui::TuiAction::CursorLineEnd),
         "tui draft editor can reposition after the first word-delete");
  expect(ava::tui::apply_composer_draft_action(word_cluster_draft, ava::tui::TuiAction::DeleteWordBackward) && word_cluster_draft.text == "pre " &&
             word_cluster_draft.kill_buffer == family_zwj + " " && word_cluster_draft.text.find(zwj) == std::string::npos &&
             word_cluster_draft.kill_buffer.find(man) != std::string::npos && word_cluster_draft.kill_buffer.find(woman) != std::string::npos &&
             word_cluster_draft.kill_buffer.find(girl) != std::string::npos,
         "tui draft editor word-backspace removes a ZWJ family cluster intact with no orphan joiners");
  ava::tui::reset_composer_draft(word_cluster_draft, std::string("pre ") + family_zwj + " post", std::string("pre ").size());
  expect(ava::tui::apply_composer_draft_action(word_cluster_draft, ava::tui::TuiAction::DeleteWordForward) && word_cluster_draft.text == "pre  post" &&
             word_cluster_draft.kill_buffer == family_zwj && word_cluster_draft.kill_buffer.find(zwj) != std::string::npos &&
             word_cluster_draft.text.find(zwj) == std::string::npos,
         "tui draft editor forward word-delete removes a ZWJ family cluster as one unit");

  ava::tui::reset_composer_draft(word_cluster_draft, std::string("aa") + e_combining + "bb");
  expect(ava::tui::apply_composer_draft_action(word_cluster_draft, ava::tui::TuiAction::CursorWordLeft) && word_cluster_draft.cursor == 0 &&
             ava::tui::apply_composer_draft_action(word_cluster_draft, ava::tui::TuiAction::CursorWordRight) &&
             word_cluster_draft.cursor == word_cluster_draft.text.size(),
         "tui draft editor word movement keeps base+combining clusters inside one word run");
  ava::tui::reset_composer_draft(word_cluster_draft, std::string("x ") + e_combining + " y");
  expect(ava::tui::apply_composer_draft_action(word_cluster_draft, ava::tui::TuiAction::CursorWordLeft) &&
             word_cluster_draft.cursor == std::string("x ").size() + e_combining.size() + 1 &&
             ava::tui::apply_composer_draft_action(word_cluster_draft, ava::tui::TuiAction::CursorWordLeft) &&
             word_cluster_draft.cursor == std::string("x ").size() &&
             ava::tui::apply_composer_draft_action(word_cluster_draft, ava::tui::TuiAction::DeleteWordForward) && word_cluster_draft.text == "x  y" &&
             word_cluster_draft.kill_buffer == e_combining && word_cluster_draft.kill_buffer.find(combining_acute) != std::string::npos,
         "tui draft editor word-delete removes base+combining as one unit with no orphan mark");

  // Uneven logical lines: sticky byte column intersects a multibyte codepoint and a cluster.
  auto const cjk = std::string("\xE7\x95\x8C");
  auto const vertical_text = std::string("abcdef") + "\n" + cjk + e_combining + family_zwj;
  ava::tui::ComposerDraftState vertical_draft;
  ava::tui::reset_composer_draft(vertical_draft, vertical_text, 4);  // column 4 on "abcdef"
  expect(ava::tui::apply_composer_draft_action(vertical_draft, ava::tui::TuiAction::CursorDown), "tui draft editor can move down onto an uneven unicode line");
  expect(ava::tui::clamp_composer_draft_cursor_to_atomic_boundary(vertical_draft, vertical_draft.cursor) == vertical_draft.cursor,
         "tui draft editor vertical movement never leaves the cursor inside a compact cluster");
  auto const down_cursor = vertical_draft.cursor;
  // Target byte column 4 lands inside/after the leading CJK cell on the second line; snap must be a cluster edge.
  expect(down_cursor == 0 + std::string("abcdef\n").size() || down_cursor == std::string("abcdef\n").size() + cjk.size() ||
             down_cursor == std::string("abcdef\n").size() + cjk.size() + e_combining.size() || down_cursor == vertical_text.size(),
         "tui draft editor snaps vertical targets to whole CJK/combining/ZWJ cluster boundaries");
  expect(ava::tui::apply_composer_draft_action(vertical_draft, ava::tui::TuiAction::CursorUp) && vertical_draft.cursor == 4,
         "tui draft editor preserves sticky byte-column policy when returning to the previous logical line");

  // Shift-selection simulation: anchor + CursorDown must yield whole-cluster bounds (no partial substrings).
  ava::tui::reset_composer_draft(vertical_draft, vertical_text, 1);
  auto const selection_anchor = vertical_draft.cursor;
  expect(ava::tui::apply_composer_draft_action(vertical_draft, ava::tui::TuiAction::CursorDown),
         "tui draft editor extends vertically for selection simulation");
  auto const selection_start = std::min(selection_anchor, vertical_draft.cursor);
  auto const selection_end = std::max(selection_anchor, vertical_draft.cursor);
  expect(ava::tui::clamp_composer_draft_cursor_to_atomic_boundary(vertical_draft, selection_start) == selection_start &&
             ava::tui::clamp_composer_draft_cursor_to_atomic_boundary(vertical_draft, selection_end) == selection_end,
         "tui draft editor vertical selection bounds stay on compact cluster boundaries");
  {
    std::size_t walk = selection_start;
    bool selection_cluster_aligned = selection_start < selection_end;
    while (walk < selection_end)
    {
      auto const step = std::max<std::size_t>(ava::tui::detail::terminal_text_cluster_bytes(vertical_draft.text, walk), 1);
      if (walk + step > selection_end)
      {
        selection_cluster_aligned = false;
        break;
      }
      walk += step;
    }
    expect(selection_cluster_aligned && walk == selection_end, "tui draft editor vertical selection spans whole compact clusters only");
    auto const selected = vertical_draft.text.substr(selection_start, selection_end - selection_start);
    expect(selected.find(zwj) == std::string::npos || selected.find(family_zwj) != std::string::npos,
           "tui draft editor vertical selection never keeps a ZWJ without its full family cluster");
  }

  // reset/replace with an interior cluster offset must snap before any edit observes the cursor.
  ava::tui::reset_composer_draft(vertical_draft, std::string("x") + family_zwj + "y", 1 + 3);
  expect(ava::tui::clamp_composer_draft_cursor_to_atomic_boundary(vertical_draft, vertical_draft.cursor) == vertical_draft.cursor &&
             (vertical_draft.cursor == 1 || vertical_draft.cursor == 1 + family_zwj.size()),
         "tui draft editor reset snaps arbitrary offsets out of ZWJ cluster interiors");
  expect(ava::tui::replace_composer_draft(vertical_draft, std::string("x") + e_combining + "y", 2) &&
             ava::tui::clamp_composer_draft_cursor_to_atomic_boundary(vertical_draft, vertical_draft.cursor) == vertical_draft.cursor &&
             (vertical_draft.cursor == 1 || vertical_draft.cursor == 1 + e_combining.size()),
         "tui draft editor replace snaps arbitrary offsets out of base+combining interiors");

  // Range replace / selection-style deletion must snap stale interior bounds and never emit partial clusters.
  ava::tui::ComposerDraftState range_draft;
  ava::tui::reset_composer_draft(range_draft, std::string("a") + family_zwj + "b");
  expect(ava::tui::replace_composer_draft_range(range_draft, 1 + 2, 1 + family_zwj.size(), "") && range_draft.text == "ab" &&
             range_draft.text.find(zwj) == std::string::npos && range_draft.cursor == 1,
         "tui draft editor range replace snaps an interior start through the ZWJ cluster end without splitting");
  ava::tui::reset_composer_draft(range_draft, std::string("a") + e_combining + "b");
  expect(ava::tui::replace_composer_draft_range(range_draft, 2, 1 + e_combining.size(), "X") && range_draft.text == "aXb" &&
             range_draft.text.find(combining_acute) == std::string::npos,
         "tui draft editor range replace snaps a mark-interior start so selection deletion keeps base+combining atomic");

  // Unicode space codepoints break typing undo coalescing like ASCII whitespace.
  ava::tui::ComposerDraftState unicode_space_undo;
  expect(ava::tui::insert_composer_draft_text(unicode_space_undo, "ab") && unicode_space_undo.undo_stack.size() == 1,
         "tui draft editor records one undo group before a unicode space boundary");
  expect(ava::tui::insert_composer_draft_text(unicode_space_undo, nbsp) && unicode_space_undo.undo_stack.size() == 2,
         "tui draft editor breaks typing undo groups on NBSP");
  expect(ava::tui::insert_composer_draft_text(unicode_space_undo, "cd") && unicode_space_undo.undo_stack.size() == 3,
         "tui draft editor starts a new typing undo group after NBSP");
  expect(ava::tui::insert_composer_draft_text(unicode_space_undo, ideographic_space) && unicode_space_undo.undo_stack.size() == 4 &&
             ava::tui::insert_composer_draft_text(unicode_space_undo, "ef") && unicode_space_undo.undo_stack.size() == 5,
         "tui draft editor breaks typing undo groups on ideographic space");
  expect(ava::tui::apply_composer_draft_action(unicode_space_undo, ava::tui::TuiAction::Undo) &&
             unicode_space_undo.text == std::string("ab") + nbsp + "cd" + ideographic_space &&
             ava::tui::apply_composer_draft_action(unicode_space_undo, ava::tui::TuiAction::Redo) &&
             unicode_space_undo.text == std::string("ab") + nbsp + "cd" + ideographic_space + "ef",
         "tui draft editor undo/redo across unicode space boundaries remains exact");

  // Mid-action stale interior cursor is corrected before word deletion.
  ava::tui::ComposerDraftState stale_cursor_draft;
  ava::tui::reset_composer_draft(stale_cursor_draft, std::string("a") + family_zwj + "b");
  stale_cursor_draft.cursor = 1 + 4;  // deliberately inside the family cluster
  expect(ava::tui::apply_composer_draft_action(stale_cursor_draft, ava::tui::TuiAction::DeleteWordForward) && stale_cursor_draft.text == "a" &&
             stale_cursor_draft.text.find(zwj) == std::string::npos && stale_cursor_draft.kill_buffer.find(zwj) != std::string::npos &&
             ava::tui::clamp_composer_draft_cursor_to_atomic_boundary(stale_cursor_draft, stale_cursor_draft.cursor) == stale_cursor_draft.cursor,
         "tui draft editor corrects interior cluster cursors before word deletion and leaves no orphan ZWJ");

  auto const split_empty = ava::tui::split_lines("");
  expect(split_empty.size() == 1 && split_empty.front().empty(), "tui split keeps empty input as one line");
  auto const split_trailing = ava::tui::split_lines("a\n");
  expect(split_trailing.size() == 2 && split_trailing[0] == "a" && split_trailing[1].empty(), "tui split preserves trailing empty line");
  auto const split_crlf = ava::tui::split_lines("a\r\nb\rc");
  expect(split_crlf.size() == 3 && split_crlf[0] == "a" && split_crlf[1] == "b" && split_crlf[2] == "c",
         "tui split treats crlf and carriage-return output as line breaks");
}
namespace {
struct VirtualTerminalProfile
{
  std::string name;
  std::string term;
  std::string term_program = {};
  std::string colorterm = {};
  std::string tmux = {};
  std::string tmux_pane = {};
  std::string ssh_tty = {};
  std::string kitty_window_id = {};
  std::string wezterm_exec = {};
  bool no_color = false;
};

struct VirtualTerminalResult
{
  bool screen_created = false;
  bool size_reported = false;
  bool base_drawn = false;
  bool modal_drawn = false;
  bool cursor_restored_after_modal = false;
  bool cached_row_draw_preserves_unchanged_lower_row = false;
  bool graphic_overlay_cache_stable = false;
  bool processing_footer_updates_stable = false;
  bool processing_footer_output_is_quiet = false;
  bool processing_footer_output_is_plain = false;
  bool cursor_forced_visible_for_teardown = false;
  bool checked_builtin_dark_default_screen_bg = false;
  bool builtin_dark_stdscr_background_is_default = false;
};

std::optional<std::string> ncurses_screen_row(std::size_t row)
{
  if (row >= static_cast<std::size_t>(LINES > 0 ? LINES : 0))
    return std::nullopt;
  std::string text;
  text.reserve(static_cast<std::size_t>(COLS > 0 ? COLS : 0));
  for (int column = 0; column < COLS; ++column)
  {
    auto const cell = mvwinch(stdscr, static_cast<int>(row), column);
    if (cell == static_cast<chtype>(ERR))
      return std::nullopt;
    text.push_back(static_cast<char>(cell & A_CHARTEXT));
  }
  return text;
}

VirtualTerminalResult exercise_virtual_terminal_profile(VirtualTerminalProfile const& profile)
{
  ScopedEnvVar term_guard("TERM", profile.term);
  ScopedEnvVar term_program_guard("TERM_PROGRAM", profile.term_program);
  ScopedEnvVar colorterm_guard("COLORTERM", profile.colorterm);
  ScopedEnvVar tmux_guard("TMUX", profile.tmux);
  ScopedEnvVar tmux_pane_guard("TMUX_PANE", profile.tmux_pane);
  ScopedEnvVar ssh_tty_guard("SSH_TTY", profile.ssh_tty);
  ScopedEnvVar kitty_window_guard("KITTY_WINDOW_ID", profile.kitty_window_id);
  ScopedEnvVar wezterm_exec_guard("WEZTERM_EXECUTABLE", profile.wezterm_exec);
  ScopedEnvVar no_color_guard("NO_COLOR", profile.no_color ? "1" : "");

  VirtualTerminalResult result;
  ScopedTmpFile input;
  ScopedTmpFile output;

  SCREEN* screen = newterm(nullptr, output.get(), input.get());
  result.screen_created = screen != nullptr;
  if (screen)
  {
    static_cast<void>(set_term(screen));
    // Match production color setup before drawing so default-background pairs resolve.
    if (has_colors())
    {
      static_cast<void>(start_color());
      static_cast<void>(use_default_colors());
    }
    int rows = 0;
    int columns = 0;
    getmaxyx(stdscr, rows, columns);
    result.size_reported = rows > 0 && columns > 0;
    expect(result.size_reported, "ncurses smoke test reports a usable virtual screen size for " + profile.name);
    static_cast<void>(resizeterm(14, 160));
    auto const ime_sensitive_input = std::string("a") + "\xE7\x95\x8C" + "e" + "\xCC\x81";
    auto const snapshot =
        ava::tui::ComposerSnapshot{.mode = "build",
                                   .provider = "openai",
                                   .model = "gpt-5.5",
                                   .session_id = "session_term",
                                   .input = ime_sensitive_input,
                                   .status = "ready",
                                   .transcript = {ava::tui::TranscriptItem{.label = "ava", .text = "virtual terminal draw keeps \xE7\x95\x8C bounded"}},
                                   .width = 160,
                                   .height = 14,
                                   .input_cursor = ime_sensitive_input.size()};
    auto const canvas = ava::tui::composer_canvas_layout(snapshot);
    auto const expected_column = canvas.left + ava::tui::detail::input_cursor_column(snapshot, canvas.content_width);
    result.base_drawn = ava::tui::draw_screen(snapshot);
    // initialize_color_pairs caches statically across SCREENs; assert default screen bg once on the dark baseline.
    if (!profile.no_color && profile.name == "xterm baseline" && has_colors())
    {
      result.checked_builtin_dark_default_screen_bg = true;
      short foreground = 0;
      short background = 0;
      auto const bkgd_cell = getbkgd(stdscr);
      auto const pair = static_cast<short>(PAIR_NUMBER(bkgd_cell));
      result.builtin_dark_stdscr_background_is_default = pair_content(pair, &foreground, &background) == OK && background == -1;
    }

    auto modal_snapshot = snapshot;
    modal_snapshot.select_list = ava::tui::SelectListView{.title = "Terminal profile",
                                                          .subtitle = profile.name,
                                                          .items = {ava::tui::SelectListItemView{.value = profile.term,
                                                                                                 .label = profile.term,
                                                                                                 .description = profile.name,
                                                                                                 .group = "terminal",
                                                                                                 .detail = "virtual newterm smoke",
                                                                                                 .badge = {},
                                                                                                 .current = true,
                                                                                                 .enabled = true,
                                                                                                 .disabled_reason = {}}},
                                                          .selected_item_index = 0,
                                                          .query = {},
                                                          .placeholder = "Search profiles",
                                                          .empty_text = "No profiles",
                                                          .footer_hint = "Esc closes"};
    result.modal_drawn = ava::tui::draw_screen(modal_snapshot);

    auto const restored_drawn = ava::tui::draw_screen(snapshot);
    int cursor_y = 0;
    int cursor_x = 0;
    getyx(stdscr, cursor_y, cursor_x);
    result.cursor_restored_after_modal = restored_drawn && cursor_x == static_cast<int>(expected_column - 1) && cursor_y >= 0 && cursor_y < LINES;

    auto exercise_cached_row_draw = [&]() {
      static_cast<void>(resizeterm(14, 80));
      auto cached_snapshot = snapshot;
      cached_snapshot.width = 80;
      cached_snapshot.height = 14;
      cached_snapshot.transcript.clear();
      cached_snapshot.input = "UPPER-CACHE-ROW\nLOWER-CACHE-ROW";
      cached_snapshot.input_cursor = cached_snapshot.input.size();
      ava::tui::detail::CompletionMatchCache completion_cache;
      ava::tui::detail::TranscriptLayoutCache transcript_cache;
      ava::tui::detail::ScreenRowCache screen_cache;
      if (!ava::tui::detail::draw_screen_cached(cached_snapshot, completion_cache, cached_snapshot.file_references_generation, transcript_cache,
                                                cached_snapshot.transcript_generation, screen_cache))
      {
        return false;
      }

      auto const initial_surfaces = screen_cache.surfaces;
      auto const upper = std::ranges::find_if(initial_surfaces, [](std::string const& line) { return line.find("UPPER-CACHE-ROW") != std::string::npos; });
      auto const lower = std::ranges::find_if(initial_surfaces, [](std::string const& line) { return line.find("LOWER-CACHE-ROW") != std::string::npos; });
      if (upper == initial_surfaces.end() || lower == initial_surfaces.end())
        return false;
      auto const upper_row = static_cast<std::size_t>(std::distance(initial_surfaces.begin(), upper));
      auto const lower_row = static_cast<std::size_t>(std::distance(initial_surfaces.begin(), lower));
      auto const initial_lower_screen_row = ncurses_screen_row(lower_row);
      if (lower_row != upper_row + 1 || !initial_lower_screen_row || initial_lower_screen_row->find("LOWER-CACHE-ROW") == std::string::npos)
        return false;

      cached_snapshot.input.replace(0, std::string_view("UPPER-CACHE-ROW").size(), "EDITD-CACHE-ROW");
      if (!ava::tui::detail::draw_screen_cached(cached_snapshot, completion_cache, cached_snapshot.file_references_generation, transcript_cache,
                                                cached_snapshot.transcript_generation, screen_cache))
      {
        return false;
      }
      auto const changed_rows = ava::tui::detail::changed_screen_rows(initial_surfaces, screen_cache.surfaces, {}, false);
      auto const retained_lower_screen_row = ncurses_screen_row(lower_row);
      return changed_rows == std::vector<std::size_t>{upper_row} && retained_lower_screen_row &&
             retained_lower_screen_row->find("LOWER-CACHE-ROW") != std::string::npos;
    };
    result.cached_row_draw_preserves_unchanged_lower_row = exercise_cached_row_draw();

    auto exercise_graphic_overlay_cache = [&]() {
      if (profile.no_color)
        return true;
      static_cast<void>(resizeterm(14, 80));
      auto graphic_snapshot = snapshot;
      graphic_snapshot.width = 80;
      graphic_snapshot.height = 14;
      graphic_snapshot.pending_attachments = {ava::tui::PendingAttachmentItem{
          .label = "cached-preview.png",
          .detail = "(image/png) persistent graphic cache regression",
          .preview = ava::tui::PendingAttachmentItem::Preview{.protocol = ava::tui::TerminalImageProtocol::Kitty,
                                                              .base64_data = std::make_shared<std::string const>("GRAPHIC_CACHE_FIRST_PAYLOAD"),
                                                              .dimensions = ava::tui::ImageDimensions{.width_px = 20, .height_px = 20},
                                                              .image_id = 424242}}};
      ava::tui::detail::CompletionMatchCache completion_cache;
      ava::tui::detail::TranscriptLayoutCache transcript_cache;
      ava::tui::detail::ScreenRowCache screen_cache;
      auto const saved_stdout = dup(STDOUT_FILENO);
      if (saved_stdout < 0 || std::fflush(stdout) != 0 || dup2(fileno(output.get()), STDOUT_FILENO) < 0)
      {
        if (saved_stdout >= 0)
          static_cast<void>(close(saved_stdout));
        return false;
      }
      auto restore_stdout = [&]() {
        static_cast<void>(std::fflush(stdout));
        static_cast<void>(dup2(saved_stdout, STDOUT_FILENO));
        static_cast<void>(close(saved_stdout));
      };
      auto draw_and_capture = [&]() -> std::optional<std::string> {
        if (std::fflush(stdout) != 0 || std::fflush(output.get()) != 0 || std::fseek(output.get(), 0, SEEK_END) != 0)
          return std::nullopt;
        auto const before = std::ftell(output.get());
        if (before < 0 ||
            !ava::tui::detail::draw_screen_cached(graphic_snapshot, completion_cache, graphic_snapshot.file_references_generation, transcript_cache,
                                                  graphic_snapshot.transcript_generation, screen_cache) ||
            std::fflush(stdout) != 0 || std::fflush(output.get()) != 0)
        {
          return std::nullopt;
        }
        auto const after = std::ftell(output.get());
        if (after < before || std::fseek(output.get(), before, SEEK_SET) != 0)
          return std::nullopt;
        std::string captured(static_cast<std::size_t>(after - before), '\0');
        if (!captured.empty() && std::fread(captured.data(), 1, captured.size(), output.get()) != captured.size())
          return std::nullopt;
        if (std::fseek(output.get(), 0, SEEK_END) != 0)
          return std::nullopt;
        return captured;
      };

      auto const first = draw_and_capture();
      auto const identical = draw_and_capture();
      graphic_snapshot.pending_attachments.front().preview->base64_data = std::make_shared<std::string const>("GRAPHIC_CACHE_CHANGED_PAYLOAD");
      auto const changed = draw_and_capture();
      screen_cache.valid = false;
      auto const invalidated = draw_and_capture();
      graphic_snapshot.pending_attachments.clear();
      auto const removed = draw_and_capture();
      restore_stdout();
      auto const deletion = ava::tui::delete_kitty_image(424242);
      return first && identical && changed && invalidated && removed && first->find("GRAPHIC_CACHE_FIRST_PAYLOAD") != std::string::npos &&
             identical->find("GRAPHIC_CACHE_FIRST_PAYLOAD") == std::string::npos && changed->find("GRAPHIC_CACHE_CHANGED_PAYLOAD") != std::string::npos &&
             invalidated->find("GRAPHIC_CACHE_CHANGED_PAYLOAD") != std::string::npos && removed->find(deletion) != std::string::npos;
    };
    result.graphic_overlay_cache_stable = exercise_graphic_overlay_cache();

    auto exercise_processing_footer = [&](ava::tui::ComposerSnapshot footer_snapshot) {
      static_cast<void>(resizeterm(static_cast<int>(footer_snapshot.height), static_cast<int>(footer_snapshot.width)));
      footer_snapshot.processing = true;
      footer_snapshot.input = "cursor";
      footer_snapshot.input_cursor = footer_snapshot.input.size();
      footer_snapshot.pending_attachments = {ava::tui::PendingAttachmentItem{
          .label = "footer-preview.png",
          .detail = "(image/png) footer-only graphic regression",
          .preview = ava::tui::PendingAttachmentItem::Preview{.protocol = ava::tui::TerminalImageProtocol::Kitty,
                                                              .base64_data = std::make_shared<std::string const>("FOOTER_ONLY_KITTY_MARKER"),
                                                              .dimensions = ava::tui::ImageDimensions{.width_px = 20, .height_px = 20},
                                                              .image_id = 31337}}};
      auto const saved_stdout = dup(STDOUT_FILENO);
      if (saved_stdout < 0 || std::fflush(stdout) != 0 || dup2(fileno(output.get()), STDOUT_FILENO) < 0)
      {
        if (saved_stdout >= 0)
          static_cast<void>(close(saved_stdout));
        return false;
      }
      auto restore_stdout = [&]() {
        static_cast<void>(std::fflush(stdout));
        static_cast<void>(dup2(saved_stdout, STDOUT_FILENO));
        static_cast<void>(close(saved_stdout));
      };
      if (!ava::tui::draw_screen(footer_snapshot))
      {
        restore_stdout();
        return false;
      }
      static_cast<void>(std::fflush(output.get()));
      auto const output_before_footer = std::ftell(output.get());
      auto const footer_canvas = ava::tui::composer_canvas_layout(footer_snapshot);
      auto const expected_footer_column = footer_canvas.left + ava::tui::detail::input_cursor_column(footer_snapshot, footer_canvas.content_width);
      ava::tui::detail::CompletionMatchCache completion_cache;
      ava::tui::detail::TranscriptLayoutCache transcript_cache;
      ava::tui::detail::ScreenRowCache screen_cache;
      for (std::size_t frame = 0; frame != 4; ++frame)
      {
        footer_snapshot.spinner_frame = frame;
        if (!ava::tui::detail::draw_processing_footer_cached(footer_snapshot, completion_cache, footer_snapshot.file_references_generation, transcript_cache,
                                                             footer_snapshot.transcript_generation, screen_cache))
        {
          restore_stdout();
          return false;
        }
        int footer_cursor_y = 0;
        int footer_cursor_x = 0;
        getyx(stdscr, footer_cursor_y, footer_cursor_x);
        if (footer_cursor_x != static_cast<int>(expected_footer_column - 1) || footer_cursor_y < 0 || footer_cursor_y >= LINES)
        {
          restore_stdout();
          return false;
        }
      }
      static_cast<void>(std::fflush(output.get()));
      auto const output_after_footer = std::ftell(output.get());
      std::string footer_output;
      bool footer_output_read = false;
      if (output_before_footer >= 0 && output_after_footer >= output_before_footer && std::fseek(output.get(), output_before_footer, SEEK_SET) == 0)
      {
        footer_output.resize(static_cast<std::size_t>(output_after_footer - output_before_footer));
        footer_output_read = std::fread(footer_output.data(), 1, footer_output.size(), output.get()) == footer_output.size();
        static_cast<void>(std::fseek(output.get(), 0, SEEK_END));
      }
      restore_stdout();
      auto const footer_output_is_quiet = footer_output_read && footer_output.find("\x1b[?25l") == std::string::npos &&
                                          footer_output.find("\x1b[?25h") == std::string::npos && footer_output.find("\x1b[2J") == std::string::npos &&
                                          footer_output.find("FOOTER_ONLY_KITTY_MARKER") == std::string::npos;
      auto const footer_output_is_plain = !profile.no_color || (footer_output_read && footer_output.find("\x1b[1m") == std::string::npos);
      result.processing_footer_output_is_quiet = result.processing_footer_output_is_quiet && footer_output_is_quiet;
      result.processing_footer_output_is_plain = result.processing_footer_output_is_plain && footer_output_is_plain;
      return output_before_footer >= 0 && output_after_footer >= output_before_footer && footer_output_is_quiet && footer_output_is_plain;
    };
    auto ordinary_footer_snapshot = snapshot;
    ordinary_footer_snapshot.width = 80;
    ordinary_footer_snapshot.height = 14;
    auto centered_footer_snapshot = snapshot;
    centered_footer_snapshot.width = 160;
    centered_footer_snapshot.height = 14;
    auto rail_footer_snapshot = centered_footer_snapshot;
    rail_footer_snapshot.sidebar = ava::tui::SidebarSnapshot{.activity = {ava::tui::SidebarActivityItem{.id = "running", .label = "run", .detail = "active"}}};
    auto narrow_footer_snapshot = ordinary_footer_snapshot;
    narrow_footer_snapshot.width = 20;
    narrow_footer_snapshot.height = 8;
    result.processing_footer_output_is_quiet = true;
    result.processing_footer_output_is_plain = true;
    result.processing_footer_updates_stable = exercise_processing_footer(ordinary_footer_snapshot) && exercise_processing_footer(centered_footer_snapshot) &&
                                              exercise_processing_footer(rail_footer_snapshot) && exercise_processing_footer(narrow_footer_snapshot);
    result.cursor_forced_visible_for_teardown = ava::tui::detail::force_terminal_cursor_visible();
    static_cast<void>(endwin());
    delscreen(screen);
  }

  return result;
}

void test_ncurses_newterm_smoke_without_real_tty()
{
  char const* previous_locale_value = std::setlocale(LC_ALL, nullptr);
  std::string const previous_locale = previous_locale_value == nullptr ? "C" : previous_locale_value;
  static_cast<void>(std::setlocale(LC_ALL, ""));

  std::vector<VirtualTerminalProfile> const profiles = {
      {.name = "xterm baseline", .term = "xterm-256color"},
      {.name = "xterm NO_COLOR", .term = "xterm-256color", .no_color = true},
      {.name = "screen/tmux terminfo", .term = "screen-256color"},
      {.name = "tmux-like environment",
       .term = "xterm-256color",
       .term_program = "tmux",
       .colorterm = "truecolor",
       .tmux = "/tmp/tmux-1000/default,123,0",
       .tmux_pane = "%1"},
      {.name = "kitty-like environment", .term = "xterm-256color", .term_program = "kitty", .colorterm = "truecolor", .kitty_window_id = "1"},
      {.name = "wezterm over ssh-like environment",
       .term = "xterm-256color",
       .term_program = "WezTerm",
       .colorterm = "truecolor",
       .ssh_tty = "/dev/pts/99",
       .wezterm_exec = "/usr/bin/wezterm"}};

  std::size_t exercised = 0;
  bool checked_default_screen_bg = false;
  for (auto const& profile : profiles)
  {
    auto const result = exercise_virtual_terminal_profile(profile);
    if (result.screen_created)
      ++exercised;
    expect(result.screen_created, "ncurses smoke test creates a screen without a real terminal for " + profile.name);
    expect(result.base_drawn && result.modal_drawn && result.cursor_restored_after_modal && result.cached_row_draw_preserves_unchanged_lower_row &&
               result.graphic_overlay_cache_stable && result.processing_footer_updates_stable && result.processing_footer_output_is_quiet &&
               result.processing_footer_output_is_plain && result.cursor_forced_visible_for_teardown &&
               (!result.checked_builtin_dark_default_screen_bg || result.builtin_dark_stdscr_background_is_default),
           "ncurses smoke test draws base/modal frames, preserves unchanged rows, suppresses identical graphic payloads while retransmitting changes and "
           "invalidations and deleting removed Kitty images, updates processing footers without terminal clears, cursor toggles, graphics, or NO_COLOR bold "
           "styling, forces the cursor visible for teardown, and keeps the built-in dark stdscr background pair at terminal default (-1) for " +
               profile.name);
    if (result.checked_builtin_dark_default_screen_bg)
      checked_default_screen_bg = true;
  }
  expect(checked_default_screen_bg,
         "ncurses smoke test verifies built-in dark stdscr background uses terminal default color on one supported baseline profile");
  expect(exercised == profiles.size(),
         "ncurses smoke test covers xterm and screen terminfo plus tmux, kitty, wezterm, and ssh-like environment "
         "variables");
  static_cast<void>(std::setlocale(LC_ALL, previous_locale.c_str()));
}
}  // namespace

void run_tui_terminal_virtual_smoke_tests()
{
  test_ncurses_newterm_smoke_without_real_tty();
}

namespace {

std::string osc11_response(std::string_view payload, bool use_st = false)
{
  std::string sequence = "]11;";
  sequence.append(payload);
  if (use_st)
  {
    sequence.push_back('\x1b');
    sequence.push_back('\\');
  }
  else
  {
    sequence.push_back('\a');
  }
  return sequence;
}

bool color_eq(std::optional<ava::tui::TerminalBackgroundColor> const& color, int red, int green, int blue)
{
  return color && color->red == red && color->green == green && color->blue == blue;
}

void push_bytes_to_curses(std::string_view bytes)
{
  for (auto it = bytes.rbegin(); it != bytes.rend(); ++it)
  {
    auto const value = static_cast<unsigned char>(*it);
    expect(unget_wch(static_cast<wchar_t>(value)) != ERR, "osc11 tests can unget terminal bytes into ncurses");
  }
}

void test_osc11_background_parser()
{
  expect(ava::tui::terminal_background_query_sequence() == std::string_view("\x1b]11;?\x1b\\"),
         "terminal background probe emits OSC 11 query terminated with ST");

  expect(color_eq(ava::tui::terminal_osc11_background_response(osc11_response("rgb:f/f/f")), 255, 255, 255) &&
             color_eq(ava::tui::terminal_osc11_background_response(osc11_response("rgb:ff/ff/ff")), 255, 255, 255) &&
             color_eq(ava::tui::terminal_osc11_background_response(osc11_response("rgb:fff/fff/fff")), 255, 255, 255) &&
             color_eq(ava::tui::terminal_osc11_background_response(osc11_response("rgb:ffff/ffff/ffff")), 255, 255, 255) &&
             color_eq(ava::tui::terminal_osc11_background_response(osc11_response("rgb:0000/0000/0000")), 0, 0, 0) &&
             color_eq(ava::tui::terminal_osc11_background_response(osc11_response("rgb:8080/8080/8080")), 128, 128, 128) &&
             color_eq(ava::tui::terminal_osc11_background_response(osc11_response("rgb:Ab/Cd/Ef")), 171, 205, 239) &&
             color_eq(ava::tui::terminal_osc11_background_response(osc11_response("rgba:ff/ff/ff/80")), 255, 255, 255) &&
             color_eq(ava::tui::terminal_osc11_background_response(osc11_response("rgba:ffff/0000/0000/8000")), 255, 0, 0) &&
             color_eq(ava::tui::terminal_osc11_background_response(osc11_response("#ffffff")), 255, 255, 255) &&
             color_eq(ava::tui::terminal_osc11_background_response(osc11_response("#000000")), 0, 0, 0) &&
             color_eq(ava::tui::terminal_osc11_background_response(osc11_response("#FFFFFFFFFFFF", true)), 255, 255, 255) &&
             color_eq(ava::tui::terminal_osc11_background_response(osc11_response("#8080ffff0000")), 128, 255, 0),
         "OSC 11 parser accepts rgb/rgba/# forms across hex widths and case with BEL or ST");

  auto const light = ava::tui::terminal_osc11_background_response(osc11_response("rgb:eeee/eeee/eeee"));
  auto const dark = ava::tui::terminal_osc11_background_response(osc11_response("#202020"));
  expect(light && dark && (((light->red * 299) + (light->green * 587) + (light->blue * 114)) / 1000) >= 180 &&
             (((dark->red * 299) + (dark->green * 587) + (dark->blue * 114)) / 1000) < 180,
         "OSC 11 parser preserves channel values that classify above and below luminance 180");

  std::string oversized = "]11;rgb:";
  oversized.append(300, 'f');
  oversized.push_back('\a');
  expect(!ava::tui::terminal_osc11_background_response("]11;rgb:ff/ff\a") && !ava::tui::terminal_osc11_background_response("]11;rgb:ff/ff/ff") &&
             !ava::tui::terminal_osc11_background_response("]12;rgb:ff/ff/ff\a") && !ava::tui::terminal_osc11_background_response("]11;rgb:ff/fff/ff\a") &&
             !ava::tui::terminal_osc11_background_response("]11;rgb:ff/ff/fg\a") && !ava::tui::terminal_osc11_background_response("]11;rgba:ff/ff/ff/zz\a") &&
             !ava::tui::terminal_osc11_background_response("]11;#fff\a") && !ava::tui::terminal_osc11_background_response("]11;#gg0000\a") &&
             !ava::tui::terminal_osc11_background_response(oversized) && !ava::tui::terminal_osc11_background_response("]11;rgb:ff/ff/ff/ff\a"),
         "OSC 11 parser rejects malformed, oversized, wrong-OSC, missing-terminator, unequal-width, and bad-alpha replies");
}

void test_osc11_theme_precedence_and_reset()
{
  ScopedEnvVar no_color_guard("NO_COLOR", "");
  ScopedEnvVar theme_env_guard("AVA_TUI_THEME", "");
  ScopedEnvVar colorfgbg_guard("COLORFGBG", "0;15");
  ava::tui::set_tui_config_theme(std::nullopt);
  ava::tui::reset_detected_terminal_background_appearance();

  expect(ava::tui::tui_theme_needs_terminal_background_probe(), "automatic theme selection requests an OSC 11 terminal-background probe");

  ava::tui::set_detected_terminal_background_appearance(ava::tui::TerminalBackgroundAppearance::Dark);
  auto active = ava::tui::active_tui_theme();
  expect(active.kind == ava::tui::TuiThemeKind::Dark && active.badge == "OSC 11" && active.detail == "terminal background appears dark from OSC 11" &&
             active.revision == "osc11-dark",
         "detected OSC 11 dark appearance beats disagreeing COLORFGBG light inference without exposing raw color data");

  {
    ScopedEnvVar explicit_light("AVA_TUI_THEME", "light");
    expect(!ava::tui::tui_theme_needs_terminal_background_probe(), "explicit AVA_TUI_THEME disables the OSC 11 probe");
    active = ava::tui::active_tui_theme();
    expect(active.kind == ava::tui::TuiThemeKind::Light && active.badge == "AVA_TUI_THEME",
           "explicit AVA_TUI_THEME light wins over disagreeing OSC 11 detection");
  }

  {
    ScopedEnvVar explicit_dark("AVA_TUI_THEME", "dark");
    ava::tui::set_detected_terminal_background_appearance(ava::tui::TerminalBackgroundAppearance::Light);
    active = ava::tui::active_tui_theme();
    expect(active.kind == ava::tui::TuiThemeKind::Dark && active.badge == "AVA_TUI_THEME",
           "explicit AVA_TUI_THEME dark wins over disagreeing OSC 11 light detection");
  }

  {
    ScopedEnvVar explicit_plain("AVA_TUI_THEME", "plain");
    active = ava::tui::active_tui_theme();
    expect(active.kind == ava::tui::TuiThemeKind::Plain && active.badge == "AVA_TUI_THEME", "explicit plain theme wins over OSC 11 detection");
  }

  {
    ScopedEnvVar no_color_on("NO_COLOR", "1");
    expect(!ava::tui::tui_theme_needs_terminal_background_probe(), "NO_COLOR disables the OSC 11 probe");
    active = ava::tui::active_tui_theme();
    expect(active.kind == ava::tui::TuiThemeKind::Plain && active.badge == "NO_COLOR", "NO_COLOR still outranks OSC 11 detection");
  }

  ava::tui::set_tui_config_theme("light");
  ava::tui::set_detected_terminal_background_appearance(ava::tui::TerminalBackgroundAppearance::Dark);
  expect(!ava::tui::tui_theme_needs_terminal_background_probe(), "configured display.json theme disables the OSC 11 probe");
  active = ava::tui::active_tui_theme();
  expect(active.kind == ava::tui::TuiThemeKind::Light && active.badge == "display.json", "configured light theme wins over disagreeing OSC 11 detection");

  ava::tui::TuiCustomTheme custom{
      .name = "sunrise",
      .path = "/tmp/sunrise.json",
      .palette = ava::tui::TuiThemePalette{.text = -1, .muted = 242, .success = 34, .warning = 220, .error = 196, .accent = 39, .screen_bg = 255},
      .revision = "custom-rev"};
  ava::tui::set_tui_config_theme("sunrise", custom);
  ava::tui::set_detected_terminal_background_appearance(ava::tui::TerminalBackgroundAppearance::Dark);
  expect(!ava::tui::tui_theme_needs_terminal_background_probe(), "configured custom theme disables the OSC 11 probe");
  active = ava::tui::active_tui_theme();
  expect(active.kind == ava::tui::TuiThemeKind::Custom && active.name == "sunrise" && active.badge == "display.json",
         "configured custom theme wins over OSC 11 detection");

  ava::tui::set_tui_config_theme(std::nullopt);
  {
    ScopedEnvVar unknown_theme("AVA_TUI_THEME", "auto");
    expect(ava::tui::tui_theme_needs_terminal_background_probe(), "unknown/auto AVA_TUI_THEME keeps automatic probe semantics");
    ava::tui::set_detected_terminal_background_appearance(ava::tui::TerminalBackgroundAppearance::Light);
    active = ava::tui::active_tui_theme();
    expect(active.kind == ava::tui::TuiThemeKind::Light && active.badge == "OSC 11", "unknown/auto AVA_TUI_THEME falls through to OSC 11 detection");
  }

  ava::tui::reset_detected_terminal_background_appearance();
  expect(!ava::tui::detected_terminal_background_appearance(), "reset clears session-scoped OSC 11 detection");
  active = ava::tui::active_tui_theme();
  expect(active.kind == ava::tui::TuiThemeKind::Light && active.badge == "COLORFGBG", "after OSC 11 reset, COLORFGBG inference remains the next fallback");

  {
    ScopedEnvVar clear_colorfgbg("COLORFGBG", "");
    active = ava::tui::active_tui_theme();
    expect(active.kind == ava::tui::TuiThemeKind::Dark && active.badge == "built-in", "without OSC 11 or COLORFGBG the built-in dark fallback is unchanged");
  }

  ava::tui::set_tui_config_theme(std::nullopt);
  ava::tui::reset_detected_terminal_background_appearance();
}

void test_startup_input_fifo_order_and_cap()
{
  ava::tui::runtime_input::clear_startup_input_queue();
  expect(ava::tui::runtime_input::startup_input_queue_size() == 0, "startup input queue begins empty");

  auto key = [](ava::tui::Key k) {
    return ava::tui::runtime_input::RuntimeInput{.event = ava::tui::InputEvent{.key = k, .character = '\0', .text = {}, .mouse_column = 0, .mouse_row = 0},
                                                 .text = {},
                                                 .bracketed_paste = false,
                                                 .resize = false};
  };
  auto character = [](char ch) {
    return ava::tui::runtime_input::RuntimeInput{
        .event = ava::tui::InputEvent{.key = ava::tui::Key::Character, .character = ch, .text = std::string(1, ch), .mouse_column = 0, .mouse_row = 0},
        .text = std::string(1, ch),
        .bracketed_paste = false,
        .resize = false};
  };
  auto paste = ava::tui::runtime_input::RuntimeInput{
      .event = ava::tui::InputEvent{.key = ava::tui::Key::Character, .character = 'p', .text = "pasted", .mouse_column = 0, .mouse_row = 0},
      .text = "pasted",
      .bracketed_paste = true,
      .resize = false};
  auto resize = ava::tui::runtime_input::RuntimeInput{
      .event = ava::tui::InputEvent{.key = ava::tui::Key::Unknown, .character = '\0', .text = {}, .mouse_column = 0, .mouse_row = 0},
      .text = {},
      .bracketed_paste = false,
      .resize = true};

  expect(ava::tui::runtime_input::enqueue_startup_input(character('a')) && ava::tui::runtime_input::enqueue_startup_input(resize) &&
             ava::tui::runtime_input::enqueue_startup_input(key(ava::tui::Key::Enter)) && ava::tui::runtime_input::enqueue_startup_input(std::move(paste)) &&
             ava::tui::runtime_input::startup_input_queue_size() == 4,
         "startup input queue preserves insertion order for key, resize, and complete paste events");

  auto first = ava::tui::runtime_input::poll_curses_input();
  auto second = ava::tui::runtime_input::read_curses_input_with_timeout(std::chrono::milliseconds(0));
  expect(first && first->event.key == ava::tui::Key::Character && first->text == "a" && second && second->resize &&
             ava::tui::runtime_input::startup_input_queue_size() == 2,
         "poll and timeout reads drain the startup FIFO before touching ncurses");

  // Blocking drain of remaining queued events does not require a live curses screen.
  auto third = ava::tui::runtime_input::read_curses_input();
  auto fourth = ava::tui::runtime_input::read_curses_input();
  expect(
      third.event.key == ava::tui::Key::Enter && fourth.bracketed_paste && fourth.text == "pasted" && ava::tui::runtime_input::startup_input_queue_size() == 0,
      "blocking reads drain remaining startup FIFO events in order");

  for (std::size_t index = 0; index < 64; ++index)
  {
    expect(ava::tui::runtime_input::enqueue_startup_input(character(static_cast<char>('0' + (index % 10)))), "startup input queue accepts up to 64 events");
  }
  expect(!ava::tui::runtime_input::enqueue_startup_input(character('x')) && ava::tui::runtime_input::startup_input_queue_size() == 64,
         "startup input queue rejects events beyond the 64-event cap");
  ava::tui::runtime_input::clear_startup_input_queue();
  expect(ava::tui::runtime_input::startup_input_queue_size() == 0, "clear empties the startup input queue between sessions");
}

void test_osc11_handler_arming_and_virtual_probe()
{
  ScopedEnvVar no_color_guard("NO_COLOR", "");
  ScopedEnvVar theme_env_guard("AVA_TUI_THEME", "");
  ScopedEnvVar colorfgbg_guard("COLORFGBG", "15;0");
  ScopedEnvVar tmux_guard("TMUX", "");
  ScopedEnvVar term_guard("TERM", "xterm-256color");
  ava::tui::set_tui_config_theme(std::nullopt);
  ava::tui::reset_detected_terminal_background_appearance();
  ava::tui::runtime_input::clear_startup_input_queue();
  ava::tui::disarm_terminal_background_response_handler();

  auto const light_reply = osc11_response("rgb:ffff/ffff/ffff");
  expect(!ava::tui::terminal_background_response_handle(light_reply) && !ava::tui::detected_terminal_background_appearance(),
         "disarmed OSC 11 replies never mutate theme state");

  ava::tui::arm_terminal_background_response_handler();
  expect(ava::tui::terminal_background_response_handler_armed(), "OSC 11 response handler can be armed");
  expect(!ava::tui::terminal_background_response_handle("]12;rgb:ff/ff/ff\a") && !ava::tui::detected_terminal_background_appearance(),
         "armed handler ignores non-OSC-11 control replies without mutation");
  expect(!ava::tui::terminal_background_response_handle("]11;rgb:ff/ff\a") && !ava::tui::detected_terminal_background_appearance(),
         "armed handler ignores malformed OSC 11 payloads without mutation");
  expect(ava::tui::terminal_background_response_handle(light_reply) &&
             ava::tui::detected_terminal_background_appearance() == ava::tui::TerminalBackgroundAppearance::Light,
         "armed handler accepts a valid light OSC 11 reply");
  ava::tui::disarm_terminal_background_response_handler();
  ava::tui::reset_detected_terminal_background_appearance();

  char const* previous_locale_value = std::setlocale(LC_ALL, nullptr);
  std::string const previous_locale = previous_locale_value == nullptr ? "C" : previous_locale_value;
  static_cast<void>(std::setlocale(LC_ALL, ""));

  ScopedTmpFile input;
  ScopedTmpFile output;

  SCREEN* screen = newterm(nullptr, output.get(), input.get());
  expect(screen != nullptr, "OSC 11 virtual probe creates an ncurses screen");
  if (!screen)
  {
    static_cast<void>(std::setlocale(LC_ALL, previous_locale.c_str()));
    return;
  }

  static_cast<void>(set_term(screen));
  static_cast<void>(raw());
  static_cast<void>(noecho());
  static_cast<void>(keypad(stdscr, TRUE));
  if (has_colors())
  {
    static_cast<void>(start_color());
    static_cast<void>(use_default_colors());
  }

  // OSC 11 light reply (chunk-assembled by the ordinary escape reader), a Kitty
  // keyboard flags reply, then a plain typed key. Bytes are ungot in reverse so
  // the probe observes them in this order.
  std::string feed;
  feed.push_back('\x1b');
  feed += light_reply;
  feed.push_back('\x1b');
  feed += "[?5u";
  feed.push_back('z');
  push_bytes_to_curses(feed);

  ava::tui::arm_terminal_background_response_handler();
  ava::tui::runtime_input::drain_startup_probe_input(std::chrono::milliseconds(50));
  ava::tui::disarm_terminal_background_response_handler();

  auto const active = ava::tui::active_tui_theme();
  expect(ava::tui::detected_terminal_background_appearance() == ava::tui::TerminalBackgroundAppearance::Light && active.kind == ava::tui::TuiThemeKind::Light &&
             active.badge == "OSC 11" && active.detail.find("OSC 11") != std::string::npos && active.detail.find("ffff") == std::string::npos,
         "virtual probe applies OSC 11 light theme without exposing raw reply bytes");

  auto const queued = ava::tui::runtime_input::poll_curses_input();
  expect(queued && queued->event.key == ava::tui::Key::Character && queued->text == "z" && ava::tui::runtime_input::startup_input_queue_size() == 0,
         "virtual probe preserves a key typed during OSC 11 drain and discards Kitty protocol replies");

  // Ensure no raw OSC payload leaked into the queued event text.
  expect(!queued->text.contains("]11") && !queued->text.contains("rgb:"), "startup FIFO never surfaces raw OSC 11 reply contents");

  auto const snapshot = ava::tui::ComposerSnapshot{.mode = "build",
                                                   .provider = "openai",
                                                   .model = "gpt-5.5",
                                                   .session_id = "session_osc11",
                                                   .input = queued->text,
                                                   .status = "ready",
                                                   .transcript = {ava::tui::TranscriptItem{.label = "ava", .text = "osc11 contrast"}},
                                                   .width = 80,
                                                   .height = 14,
                                                   .input_cursor = queued->text.size()};
  expect(ava::tui::draw_screen(snapshot), "first rendered frame after OSC 11 detection draws successfully");
  if (has_colors())
  {
    short foreground = 0;
    short background = 0;
    auto const bkgd_cell = getbkgd(stdscr);
    auto const pair = static_cast<short>(PAIR_NUMBER(bkgd_cell));
    expect(pair_content(pair, &foreground, &background) == OK && background == -1,
           "OSC 11 light selection keeps the ordinary screen canvas on terminal-default background");
  }

  ava::tui::runtime_input::clear_startup_input_queue();
  ava::tui::reset_detected_terminal_background_appearance();
  static_cast<void>(endwin());
  delscreen(screen);
  static_cast<void>(std::setlocale(LC_ALL, previous_locale.c_str()));
}

void test_osc11_tmux_skip_semantics()
{
  ScopedEnvVar no_color_guard("NO_COLOR", "");
  ScopedEnvVar theme_env_guard("AVA_TUI_THEME", "");
  ScopedEnvVar colorfgbg_guard("COLORFGBG", "");
  ava::tui::set_tui_config_theme(std::nullopt);
  ava::tui::reset_detected_terminal_background_appearance();

  expect(ava::tui::tui_theme_needs_terminal_background_probe(), "probe remains desired when theme selection is automatic");

  // Pure environment gate: TMUX / TERM=tmux* suppress; direct xterm and absent/empty allow.
  expect(!ava::tui::terminal_background_probe_environment_allows_query("/tmp/tmux-1000/default,1,0", "xterm-256color"),
         "non-empty TMUX suppresses the OSC 11 query");
  expect(!ava::tui::terminal_background_probe_environment_allows_query(std::nullopt, "tmux-256color"), "TERM=tmux* suppresses the OSC 11 query");
  expect(!ava::tui::terminal_background_probe_environment_allows_query(std::string_view{}, "tmux"),
         "empty TMUX with TERM=tmux still suppresses via TERM prefix");
  expect(ava::tui::terminal_background_probe_environment_allows_query(std::nullopt, "xterm-256color"), "direct xterm TERM allows the OSC 11 query");
  expect(ava::tui::terminal_background_probe_environment_allows_query(std::string_view{}, "xterm-256color"),
         "empty TMUX is intentional allow with direct TERM");
  expect(ava::tui::terminal_background_probe_environment_allows_query(std::nullopt, std::nullopt), "absent TMUX and TERM intentionally allow the OSC 11 query");
  expect(ava::tui::terminal_background_probe_environment_allows_query(std::string_view{}, std::string_view{}),
         "empty TMUX and TERM intentionally allow the OSC 11 query");

  {
    ScopedEnvVar tmux_guard("TMUX", "/tmp/tmux-1000/default,1,0");
    ScopedEnvVar term_guard("TERM", "xterm-256color");
    // Environment skip is enforced by the pure gate; the probe-need predicate stays
    // about theme automation only. Document the split here so tmux smoke stays aligned.
    expect(ava::tui::tui_theme_needs_terminal_background_probe(), "TMUX does not change automatic theme selection; runtime skips the query separately");
  }

  {
    ScopedEnvVar tmux_guard("TMUX", "");
    ScopedEnvVar term_guard("TERM", "tmux-256color");
    expect(ava::tui::tui_theme_needs_terminal_background_probe(), "TERM=tmux* still leaves theme selection automatic while runtime skips the OSC 11 query");
  }

  ava::tui::set_detected_terminal_background_appearance(ava::tui::TerminalBackgroundAppearance::Light);
  auto active = ava::tui::active_tui_theme();
  expect(active.badge == "OSC 11", "prior detection remains visible until session reset");
  ava::tui::reset_detected_terminal_background_appearance();
  active = ava::tui::active_tui_theme();
  expect(active.badge == "built-in", "session reset clears OSC 11 detection so the next session cannot inherit it");
}

std::string read_tmpfile_bytes(FILE* file)
{
  expect(file != nullptr && std::fseek(file, 0, SEEK_SET) == 0, "query writer tests can rewind the temporary stream");
  std::string bytes;
  char buffer[64];
  while (true)
  {
    auto const n = std::fread(buffer, 1, sizeof(buffer), file);
    if (n == 0)
      break;
    bytes.append(buffer, n);
  }
  return bytes;
}

void test_osc11_query_writer_and_environment_gate_seam()
{
  auto const expected_query = std::string(ava::tui::terminal_background_query_sequence());
  expect(expected_query == "\x1b]11;?\x1b\\", "query writer seam uses the ST-terminated OSC 11 probe bytes");

  ScopedTmpFile allowed;

  expect(ava::tui::write_terminal_background_query(allowed.get()), "query writer reports success after writing the OSC 11 probe");
  expect(read_tmpfile_bytes(allowed.get()) == expected_query, "query writer emits exact OSC 11 query bytes");
  expect(!ava::tui::write_terminal_background_query(nullptr), "query writer fails closed on a null stream");

  ScopedTmpFile skipped_tmux;
  ScopedTmpFile skipped_term;
  ScopedTmpFile allowed_emit;

  expect(!ava::tui::emit_terminal_background_query_if_environment_allows("/tmp/tmux-1000/default,1,0", "xterm-256color", skipped_tmux.get()) &&
             read_tmpfile_bytes(skipped_tmux.get()).empty(),
         "emit seam writes nothing when TMUX suppresses the query");
  expect(!ava::tui::emit_terminal_background_query_if_environment_allows(std::nullopt, "tmux-256color", skipped_term.get()) &&
             read_tmpfile_bytes(skipped_term.get()).empty(),
         "emit seam writes nothing when TERM=tmux* suppresses the query");
  expect(ava::tui::emit_terminal_background_query_if_environment_allows(std::nullopt, "xterm-256color", allowed_emit.get()) &&
             read_tmpfile_bytes(allowed_emit.get()) == expected_query,
         "emit seam writes exact OSC 11 bytes for a direct xterm environment");
}

struct VirtualOsc11Screen
{
  SCREEN* screen = nullptr;
  ScopedTmpFile input;
  ScopedTmpFile output;
  std::string previous_locale;

  explicit operator bool() const { return screen != nullptr; }
};

VirtualOsc11Screen enter_virtual_osc11_screen(char const* purpose)
{
  VirtualOsc11Screen state;
  char const* previous_locale_value = std::setlocale(LC_ALL, nullptr);
  state.previous_locale = previous_locale_value == nullptr ? "C" : previous_locale_value;
  static_cast<void>(std::setlocale(LC_ALL, ""));

  state.screen = newterm(nullptr, state.output.get(), state.input.get());
  expect(state.screen != nullptr, std::string(purpose) + " creates an ncurses screen");
  if (!state.screen)
  {
    static_cast<void>(std::setlocale(LC_ALL, state.previous_locale.c_str()));
    return state;
  }

  static_cast<void>(set_term(state.screen));
  static_cast<void>(raw());
  static_cast<void>(noecho());
  static_cast<void>(keypad(stdscr, TRUE));
  if (has_colors())
  {
    static_cast<void>(start_color());
    static_cast<void>(use_default_colors());
  }
  return state;
}

void leave_virtual_osc11_screen(VirtualOsc11Screen& state)
{
  if (state.screen)
  {
    static_cast<void>(endwin());
    delscreen(state.screen);
    state.screen = nullptr;
  }
  static_cast<void>(std::setlocale(LC_ALL, state.previous_locale.c_str()));
}

void test_osc11_reply_inside_startup_bracketed_paste(bool use_st)
{
  ScopedEnvVar no_color_guard("NO_COLOR", "");
  ScopedEnvVar theme_env_guard("AVA_TUI_THEME", "");
  ScopedEnvVar colorfgbg_guard("COLORFGBG", "15;0");
  ScopedEnvVar tmux_guard("TMUX", "");
  ScopedEnvVar term_guard("TERM", "xterm-256color");
  ava::tui::set_tui_config_theme(std::nullopt);
  ava::tui::reset_detected_terminal_background_appearance();
  ava::tui::runtime_input::clear_startup_input_queue();
  ava::tui::disarm_terminal_background_response_handler();

  auto screen = enter_virtual_osc11_screen(use_st ? "OSC 11 ST paste interleave" : "OSC 11 BEL paste interleave");
  if (!screen)
    return;

  auto const light_reply = osc11_response("rgb:ffff/ffff/ffff", use_st);
  std::string feed;
  feed.push_back('\x1b');
  feed += "[200~";
  feed += "hello";
  feed.push_back('\x1b');
  feed += light_reply;
  feed += "world";
  feed.push_back('\x1b');
  feed += "[201~";
  push_bytes_to_curses(feed);

  ava::tui::arm_terminal_background_response_handler();
  ava::tui::runtime_input::drain_startup_probe_input(std::chrono::milliseconds(50));
  ava::tui::disarm_terminal_background_response_handler();

  expect(ava::tui::detected_terminal_background_appearance() == ava::tui::TerminalBackgroundAppearance::Light,
         use_st ? "ST OSC 11 reply inside startup paste sets light appearance" : "BEL OSC 11 reply inside startup paste sets light appearance");

  auto const queued = ava::tui::runtime_input::poll_curses_input();
  expect(queued && queued->bracketed_paste && queued->text == "helloworld" && ava::tui::runtime_input::startup_input_queue_size() == 0,
         use_st ? "ST OSC 11 reply is stripped from one complete startup paste with contiguous user text"
                : "BEL OSC 11 reply is stripped from one complete startup paste with contiguous user text");
  expect(queued && !queued->text.contains("]11") && !queued->text.contains("rgb:") && !queued->text.contains("ffff"),
         "startup paste never surfaces raw OSC 11 reply contents");

  ava::tui::runtime_input::clear_startup_input_queue();
  ava::tui::reset_detected_terminal_background_appearance();
  leave_virtual_osc11_screen(screen);
}

void test_non_owned_escape_inside_startup_bracketed_paste_is_preserved()
{
  ScopedEnvVar no_color_guard("NO_COLOR", "");
  ScopedEnvVar theme_env_guard("AVA_TUI_THEME", "");
  ScopedEnvVar colorfgbg_guard("COLORFGBG", "15;0");
  ScopedEnvVar tmux_guard("TMUX", "");
  ScopedEnvVar term_guard("TERM", "xterm-256color");
  ava::tui::set_tui_config_theme(std::nullopt);
  ava::tui::reset_detected_terminal_background_appearance();
  ava::tui::runtime_input::clear_startup_input_queue();
  ava::tui::disarm_terminal_background_response_handler();

  auto screen = enter_virtual_osc11_screen("non-owned paste escape");
  if (!screen)
    return;

  // Armed handler must not silently eat non-OSC-11 escape content inside paste.
  std::string feed;
  feed.push_back('\x1b');
  feed += "[200~";
  feed += "ab";
  feed.push_back('\x1b');
  feed += "]12;rgb:ff/ff/ff";
  feed.push_back('\a');
  feed += "cd";
  feed.push_back('\x1b');
  feed += "[201~";
  push_bytes_to_curses(feed);

  ava::tui::arm_terminal_background_response_handler();
  ava::tui::runtime_input::drain_startup_probe_input(std::chrono::milliseconds(50));
  ava::tui::disarm_terminal_background_response_handler();

  expect(!ava::tui::detected_terminal_background_appearance(), "non-OSC-11 escape inside startup paste does not mutate appearance");

  auto const queued = ava::tui::runtime_input::poll_curses_input();
  // ESC/BEL are stripped by paste normalization; printable OSC body remains so the
  // sequence is not silently discarded under protocol ownership.
  expect(queued && queued->bracketed_paste && queued->text == "ab]12;rgb:ff/ff/ffcd" && ava::tui::runtime_input::startup_input_queue_size() == 0,
         "malformed/non-owned escape content inside startup paste is preserved under ordinary normalization");

  ava::tui::runtime_input::clear_startup_input_queue();
  ava::tui::reset_detected_terminal_background_appearance();
  leave_virtual_osc11_screen(screen);
}

void test_disarmed_and_malformed_osc11_inside_startup_bracketed_paste_preserve_semantics()
{
  ScopedEnvVar no_color_guard("NO_COLOR", "");
  ScopedEnvVar theme_env_guard("AVA_TUI_THEME", "");
  ScopedEnvVar colorfgbg_guard("COLORFGBG", "15;0");
  ScopedEnvVar tmux_guard("TMUX", "");
  ScopedEnvVar term_guard("TERM", "xterm-256color");
  ava::tui::set_tui_config_theme(std::nullopt);

  {
    ava::tui::reset_detected_terminal_background_appearance();
    ava::tui::runtime_input::clear_startup_input_queue();
    ava::tui::disarm_terminal_background_response_handler();

    auto screen = enter_virtual_osc11_screen("disarmed OSC 11 paste");
    if (!screen)
      return;

    // Valid OSC 11 while disarmed is not protocol-owned: paste keeps ordinary semantics.
    auto const light_reply = osc11_response("rgb:ffff/ffff/ffff");
    std::string feed;
    feed.push_back('\x1b');
    feed += "[200~";
    feed += "pre";
    feed.push_back('\x1b');
    feed += light_reply;
    feed += "post";
    feed.push_back('\x1b');
    feed += "[201~";
    push_bytes_to_curses(feed);

    ava::tui::runtime_input::drain_startup_probe_input(std::chrono::milliseconds(50));

    expect(!ava::tui::detected_terminal_background_appearance(), "disarmed OSC 11 reply inside startup paste does not mutate appearance");
    auto const queued = ava::tui::runtime_input::poll_curses_input();
    expect(queued && queued->bracketed_paste && queued->text == "pre]11;rgb:ffff/ffff/ffffpost" && ava::tui::runtime_input::startup_input_queue_size() == 0,
           "disarmed valid OSC 11 inside paste is preserved under ordinary normalization");

    ava::tui::runtime_input::clear_startup_input_queue();
    leave_virtual_osc11_screen(screen);
  }

  {
    ava::tui::reset_detected_terminal_background_appearance();
    ava::tui::runtime_input::clear_startup_input_queue();
    ava::tui::disarm_terminal_background_response_handler();

    auto screen = enter_virtual_osc11_screen("malformed OSC 11 paste");
    if (!screen)
      return;

    // Armed handler still must not claim malformed OSC 11 payloads.
    std::string feed;
    feed.push_back('\x1b');
    feed += "[200~";
    feed += "x";
    feed.push_back('\x1b');
    feed += "]11;rgb:ff/ff";
    feed.push_back('\a');
    feed += "y";
    feed.push_back('\x1b');
    feed += "[201~";
    push_bytes_to_curses(feed);

    ava::tui::arm_terminal_background_response_handler();
    ava::tui::runtime_input::drain_startup_probe_input(std::chrono::milliseconds(50));
    ava::tui::disarm_terminal_background_response_handler();

    expect(!ava::tui::detected_terminal_background_appearance(), "malformed OSC 11 inside startup paste does not mutate appearance");
    auto const queued = ava::tui::runtime_input::poll_curses_input();
    expect(queued && queued->bracketed_paste && queued->text == "x]11;rgb:ff/ffy" && ava::tui::runtime_input::startup_input_queue_size() == 0,
           "malformed OSC 11 inside paste is preserved under ordinary normalization");

    // A second complete paste after the probe drain still works: bracketed paste stays enabled.
    std::string second;
    second.push_back('\x1b');
    second += "[200~";
    second += "again";
    second.push_back('\x1b');
    second += "[201~";
    push_bytes_to_curses(second);
    auto const follow_up = ava::tui::runtime_input::read_curses_input_with_timeout(std::chrono::milliseconds(50));
    expect(follow_up && follow_up->bracketed_paste && follow_up->text == "again", "bracketed paste remains enabled after OSC 11 paste interleave handling");

    ava::tui::runtime_input::clear_startup_input_queue();
    ava::tui::reset_detected_terminal_background_appearance();
    leave_virtual_osc11_screen(screen);
  }
}

struct SequenceCapture
{
  std::vector<std::string> sequences;
};

SequenceCapture* g_sequence_capture = nullptr;

void capture_terminal_sequence(std::string_view sequence)
{
  if (g_sequence_capture != nullptr)
    g_sequence_capture->sequences.emplace_back(sequence);
}

int g_flushinp_calls = 0;
int g_tcflush_calls = 0;
int g_tcflush_last_fd = -1;
int g_tcflush_last_selector = -1;
std::vector<char> g_flush_order;

void capture_flushinp() noexcept
{
  ++g_flushinp_calls;
  g_flush_order.push_back('i');
}

int capture_tcflush(int fd, int queue_selector) noexcept
{
  ++g_tcflush_calls;
  g_tcflush_last_fd = fd;
  g_tcflush_last_selector = queue_selector;
  g_flush_order.push_back('t');
  return 0;
}

void reset_lifecycle_test_seams()
{
  ava::tui::detail::reset_terminal_sequence_writer_for_test();
  ava::tui::detail::reset_terminal_input_flush_hooks_for_test();
  ava::tui::detail::reset_terminal_protocol_ownership_for_test();
  g_sequence_capture = nullptr;
  g_flushinp_calls = 0;
  g_tcflush_calls = 0;
  g_tcflush_last_fd = -1;
  g_tcflush_last_selector = -1;
  g_flush_order.clear();
}

std::size_t count_sequence(std::vector<std::string> const& sequences, std::string_view needle)
{
  return static_cast<std::size_t>(std::count(sequences.begin(), sequences.end(), std::string(needle)));
}

bool sequences_contain(std::vector<std::string> const& sequences, std::string_view needle)
{
  return std::find(sequences.begin(), sequences.end(), std::string(needle)) != sequences.end();
}

void test_alacritty_da2_version_gate_and_lifecycle()
{
  expect(ava::tui::terminal_alacritty_da2_probe_environment_allows_query(std::nullopt, "xterm-256color", "Alacritty") &&
             !ava::tui::terminal_alacritty_da2_probe_environment_allows_query(std::nullopt, "xterm-256color", "ghostty") &&
             !ava::tui::terminal_alacritty_da2_probe_environment_allows_query("/tmp/tmux", "xterm-256color", "Alacritty") &&
             !ava::tui::terminal_alacritty_da2_probe_environment_allows_query(std::nullopt, "tmux-256color", "Alacritty") &&
             !ava::tui::terminal_alacritty_da2_probe_environment_allows_query(std::nullopt, "screen-256color", "Alacritty"),
         "only a positively identified direct Alacritty is eligible for the DA2 query");

  auto const version_2401 = ava::tui::terminal_alacritty_da2_version("[>0;2401;1c");
  auto const version_2402 = ava::tui::terminal_alacritty_da2_version("[>0;2402;1c");
  expect(version_2401 && *version_2401 == 2401 && version_2402 && *version_2402 == 2402 && ava::tui::terminal_alacritty_da2_version("[>0;1;1c") == 1 &&
             ava::tui::terminal_alacritty_da2_version("[>0;999999;1c") == 999999,
         "strict Alacritty DA2 parsing preserves the exact 2401/2402 boundary and accepted version range");
  for (auto const malformed : {std::string_view("[>0;0;1c"), std::string_view("[>0;1000000;1c"), std::string_view("[>1;2402;1c"),
                               std::string_view("[>0;2402;0c"), std::string_view("[>0;2402;1;2c"), std::string_view("[>0;24x2;1c"),
                               std::string_view("[>0;2402;1C"), std::string_view("[?0;2402;1c"), std::string_view("[>0;2402;1cjunk")})
  {
    expect(!ava::tui::terminal_alacritty_da2_version(malformed), "malformed or non-Alacritty DA2 reply is rejected: " + std::string(malformed));
  }

  constexpr std::string_view fragmented_reply = "[>0;2402;1c";
  for (std::size_t length = 1; length < fragmented_reply.size(); ++length)
  {
    expect(!ava::tui::terminal_escape_sequence_complete(fragmented_reply.substr(0, length)),
           "fragmented DA2 reply remains incomplete until its strict final byte");
  }
  expect(ava::tui::terminal_escape_sequence_complete(fragmented_reply), "the complete fragmented DA2 reply becomes one bounded CSI sequence");

  // Every environment uses disambiguation-only flags without an Alacritty event-type probe.
  for (auto const& environment : {std::pair{std::string(""), std::string("xterm-256color")}, std::pair{std::string("ghostty"), std::string("xterm-256color")},
                                  std::pair{std::string("Alacritty"), std::string("tmux-256color")}})
  {
    ScopedEnvVar term_program_guard("TERM_PROGRAM", environment.first);
    ScopedEnvVar term_guard("TERM", environment.second);
    ScopedEnvVar tmux_guard("TMUX", "");
    reset_lifecycle_test_seams();
    SequenceCapture capture;
    g_sequence_capture = &capture;
    ava::tui::detail::set_terminal_sequence_writer_for_test(&capture_terminal_sequence);
    ava::tui::arm_owned_terminal_protocols_on_enter();
    auto const ownership = ava::tui::terminal_protocol_ownership();
    expect(ownership.kitty_keyboard_active_flags == 1 && ownership.kitty_keyboard_desired_flags == 1 && !ownership.alacritty_da2_probe_armed &&
               count_sequence(capture.sequences, "\x1b[>1u\x1b[?u\x1b[c") == 1 &&
               !sequences_contain(capture.sequences, ava::tui::terminal_alacritty_da2_query_sequence()) &&
               !ava::tui::terminal_keyboard_protocol_handle_response("[>0;2402;1c"),
           "Kitty sessions use disambiguation-only flags and do not need or trust unsolicited DA2 replies");
    ava::tui::restore_owned_terminal_protocols();
    reset_lifecycle_test_seams();
  }

  // Direct Alacritty also keeps disambiguation-only flags because AVA no longer requests event types.
  {
    ScopedEnvVar term_program_guard("TERM_PROGRAM", "Alacritty");
    ScopedEnvVar term_guard("TERM", "xterm-256color");
    ScopedEnvVar tmux_guard("TMUX", "");
    reset_lifecycle_test_seams();
    SequenceCapture capture;
    g_sequence_capture = &capture;
    ava::tui::detail::set_terminal_sequence_writer_for_test(&capture_terminal_sequence);
    ava::tui::arm_owned_terminal_protocols_on_enter();
    auto ownership = ava::tui::terminal_protocol_ownership();
    expect(ownership.kitty_keyboard_active_flags == 1 && ownership.kitty_keyboard_desired_flags == 1 && !ownership.alacritty_da2_probe_armed &&
               sequences_contain(capture.sequences, "\x1b[>1u\x1b[?u\x1b[c") &&
               !sequences_contain(capture.sequences, ava::tui::terminal_alacritty_da2_query_sequence()),
           "direct Alacritty uses flags 1 without issuing an irrelevant event-type compatibility query");
    ava::tui::release_owned_terminal_protocols();
    ava::tui::rearm_owned_terminal_protocols();
    ownership = ava::tui::terminal_protocol_ownership();
    expect(ownership.kitty_keyboard_active_flags == 1 && ownership.kitty_keyboard_desired_flags == 1 && !ownership.alacritty_da2_probe_armed &&
               count_sequence(capture.sequences, "\x1b[>1u") == 1 && !ava::tui::terminal_keyboard_protocol_handle_response("[>0;2402;1c"),
           "flags 1 survive handoff and rearm without accepting an unsolicited DA2 upgrade");
    ava::tui::restore_owned_terminal_protocols();
    reset_lifecycle_test_seams();
  }
}

void test_terminal_protocol_lifecycle_kitty_supported_path()
{
  ScopedEnvVar term_program_guard("TERM_PROGRAM", "ghostty");
  ScopedEnvVar term_guard("TERM", "xterm-256color");
  ScopedEnvVar tmux_guard("TMUX", "");
  reset_lifecycle_test_seams();
  SequenceCapture capture;
  g_sequence_capture = &capture;
  ava::tui::detail::set_terminal_sequence_writer_for_test(&capture_terminal_sequence);

  ava::tui::arm_owned_terminal_protocols_on_enter();
  expect(ava::tui::terminal_keyboard_protocol_handle_response("[?5u"), "lifecycle test injects Kitty flags>0 reply");
  auto ownership = ava::tui::terminal_protocol_ownership();
  expect(ownership.kitty_keyboard_supported && ownership.keyboard_protocol_kitty_response_seen && !ownership.modify_other_keys_desired &&
             !ownership.modify_other_keys_enabled,
         "Kitty-supported negotiation leaves modifyOtherKeys off");

  // Force-enable then ensure a later positive Kitty reply disables and forgets desire.
  expect(ava::tui::terminal_keyboard_protocol_handle_response("[?0u") && ava::tui::terminal_protocol_ownership().modify_other_keys_desired,
         "flags=0 can enable modifyOtherKeys mid-session");
  expect(ava::tui::terminal_keyboard_protocol_handle_response("[?5u") && !ava::tui::terminal_protocol_ownership().modify_other_keys_desired &&
             !ava::tui::terminal_protocol_ownership().modify_other_keys_enabled,
         "later Kitty support disables modifyOtherKeys and clears desire");

  ava::tui::release_owned_terminal_protocols();
  ava::tui::rearm_owned_terminal_protocols();
  ownership = ava::tui::terminal_protocol_ownership();
  expect(ownership.kitty_keyboard_pushed && !ownership.modify_other_keys_enabled &&
             count_sequence(capture.sequences, ava::tui::terminal_modify_other_keys_enable_sequence()) == 1,
         "Kitty-supported resume re-pushes keyboard protocol without re-enabling modifyOtherKeys");
  expect(!sequences_contain(capture.sequences, ava::tui::terminal_background_query_sequence()), "Kitty-supported handoff/resume never emits OSC 11");

  ava::tui::restore_owned_terminal_protocols();
  reset_lifecycle_test_seams();
}

void test_terminal_input_flush_ordering_seam()
{
  reset_lifecycle_test_seams();
  ava::tui::detail::set_terminal_input_flush_hooks_for_test(&capture_flushinp, &capture_tcflush);

  ava::tui::discard_pending_terminal_input();
  expect(g_flushinp_calls == 1 && g_tcflush_calls == 1 && g_tcflush_last_fd == STDIN_FILENO && g_tcflush_last_selector == TCIFLUSH &&
             g_flush_order.size() == 2 && g_flush_order[0] == 'i' && g_flush_order[1] == 't',
         "final input flush calls flushinp then tcflush(TCIFLUSH) exactly once each");

  ava::tui::discard_pending_terminal_input();
  expect(g_flushinp_calls == 2 && g_tcflush_calls == 2 && g_flush_order == std::vector<char>({'i', 't', 'i', 't'}),
         "input flush remains ordered and fail-soft across repeated restore-safe calls");

  reset_lifecycle_test_seams();
}

bool write_all_fd(int fd, std::string_view bytes)
{
  auto const* cursor = bytes.data();
  auto remaining = bytes.size();
  while (remaining != 0)
  {
    auto const written = ::write(fd, cursor, remaining);
    if (written < 0)
      return false;
    cursor += written;
    remaining -= static_cast<std::size_t>(written);
  }
  return true;
}

bool looks_like_leaked_sgr_mouse_payload(ava::tui::runtime_input::RuntimeInput const& input)
{
  if (input.event.key != ava::tui::Key::Character && input.event.key != ava::tui::Key::Space)
    return false;
  auto const& text = input.text.empty() ? input.event.text : input.text;
  if (text.empty())
    return false;
  // Residual SGR bodies after a bare KEY_MOUSE (ESC[<) look like "0;10;5M" / "65;20;8M".
  return text.find_first_of("0123456789;Mm") != std::string::npos;
}

std::optional<ava::tui::runtime_input::RuntimeInput> read_direct_mouse_event(std::string_view label)
{
  auto input = ava::tui::runtime_input::read_curses_input_with_timeout(std::chrono::milliseconds(100));
  expect(input.has_value(), std::string("direct-terminal mouse PTY produced an event for ") + std::string(label));
  return input;
}

void expect_no_residual_mouse_payload(std::string_view label)
{
  auto residual = ava::tui::runtime_input::poll_curses_input();
  while (residual)
  {
    expect(!looks_like_leaked_sgr_mouse_payload(*residual), std::string("direct-terminal mouse path must not leak SGR payload characters after ") +
                                                                std::string(label) + " (got text '" +
                                                                (residual->text.empty() ? residual->event.text : residual->text) + "')");
    // Unknown/empty after KEY_MOUSE getmouse failure is also a leak symptom; allow only true absence.
    expect(false, std::string("direct-terminal mouse path left unexpected residual input after ") + std::string(label));
    residual = ava::tui::runtime_input::poll_curses_input();
  }
}

// Same-size geometry refresh must not inject KEY_RESIZE. Plugin surface fit checks
// refresh repeatedly; unconditional resizeterm floods the input queue on some hosts.
void test_same_size_geometry_refresh_does_not_inject_key_resize()
{
  char const* previous_locale_value = std::setlocale(LC_ALL, nullptr);
  std::string const previous_locale = previous_locale_value == nullptr ? "C" : previous_locale_value;
  static_cast<void>(std::setlocale(LC_ALL, ""));

  ScopedEnvVar term_guard("TERM", "xterm-256color");
  ScopedEnvVar tmux_guard("TMUX", "");

  int master_fd = ::posix_openpt(O_RDWR | O_NOCTTY);
  expect(master_fd >= 0, "same-size geometry PTY can open a master");
  if (master_fd < 0)
  {
    static_cast<void>(std::setlocale(LC_ALL, previous_locale.c_str()));
    return;
  }
  if (::grantpt(master_fd) != 0 || ::unlockpt(master_fd) != 0)
  {
    expect(false, "same-size geometry PTY can grant/unlock the slave");
    static_cast<void>(::close(master_fd));
    static_cast<void>(std::setlocale(LC_ALL, previous_locale.c_str()));
    return;
  }
  char* slave_name = ::ptsname(master_fd);
  expect(slave_name != nullptr, "same-size geometry PTY exposes a slave name");
  if (slave_name == nullptr)
  {
    static_cast<void>(::close(master_fd));
    static_cast<void>(std::setlocale(LC_ALL, previous_locale.c_str()));
    return;
  }
  int slave_fd = ::open(slave_name, O_RDWR | O_NOCTTY);
  expect(slave_fd >= 0, "same-size geometry PTY can open the slave");
  if (slave_fd < 0)
  {
    static_cast<void>(::close(master_fd));
    static_cast<void>(std::setlocale(LC_ALL, previous_locale.c_str()));
    return;
  }

  winsize size{};
  size.ws_row = 24;
  size.ws_col = 80;
  static_cast<void>(::ioctl(slave_fd, TIOCSWINSZ, &size));

  // Point STDOUT at the PTY so refresh_terminal_geometry_from_kernel's TIOCGWINSZ
  // observes the controlled geometry rather than the test runner's terminal.
  int saved_stdout = ::dup(STDOUT_FILENO);
  expect(saved_stdout >= 0, "same-size geometry PTY can duplicate stdout");
  if (saved_stdout < 0)
  {
    static_cast<void>(::close(slave_fd));
    static_cast<void>(::close(master_fd));
    static_cast<void>(std::setlocale(LC_ALL, previous_locale.c_str()));
    return;
  }
  if (::dup2(slave_fd, STDOUT_FILENO) < 0)
  {
    expect(false, "same-size geometry PTY can redirect stdout onto the slave");
    static_cast<void>(::close(saved_stdout));
    static_cast<void>(::close(slave_fd));
    static_cast<void>(::close(master_fd));
    static_cast<void>(std::setlocale(LC_ALL, previous_locale.c_str()));
    return;
  }

  int output_fd = ::dup(slave_fd);
  FILE* input = ::fdopen(slave_fd, "r+");
  FILE* output = output_fd >= 0 ? ::fdopen(output_fd, "w") : nullptr;
  expect(input != nullptr && output != nullptr, "same-size geometry PTY can fdopen slave streams");
  if (!input || !output)
  {
    if (input)
      static_cast<void>(std::fclose(input));
    else
      static_cast<void>(::close(slave_fd));
    if (output)
      static_cast<void>(std::fclose(output));
    else if (output_fd >= 0)
      static_cast<void>(::close(output_fd));
    static_cast<void>(::dup2(saved_stdout, STDOUT_FILENO));
    static_cast<void>(::close(saved_stdout));
    static_cast<void>(::close(master_fd));
    static_cast<void>(std::setlocale(LC_ALL, previous_locale.c_str()));
    return;
  }

  SCREEN* screen = newterm(nullptr, output, input);
  expect(screen != nullptr, "same-size geometry PTY creates an ncurses screen");
  if (!screen)
  {
    static_cast<void>(std::fclose(input));
    static_cast<void>(std::fclose(output));
    static_cast<void>(::dup2(saved_stdout, STDOUT_FILENO));
    static_cast<void>(::close(saved_stdout));
    static_cast<void>(::close(master_fd));
    static_cast<void>(std::setlocale(LC_ALL, previous_locale.c_str()));
    return;
  }

  static_cast<void>(set_term(screen));
  static_cast<void>(raw());
  static_cast<void>(noecho());
  static_cast<void>(keypad(stdscr, TRUE));
  static_cast<void>(wtimeout(stdscr, 0));
  // Align ncurses geometry with the controlled PTY size once (real resize path).
  static_cast<void>(resizeterm(static_cast<int>(size.ws_row), static_cast<int>(size.ws_col)));
  flushinp();

  auto drain_resize_events = []() -> int {
    int resize_count = 0;
    for (int i = 0; i < 64; ++i)
    {
      auto input_event = ava::tui::runtime_input::poll_curses_input();
      if (!input_event)
        break;
      if (input_event->resize)
        ++resize_count;
    }
    return resize_count;
  };
  static_cast<void>(drain_resize_events());

  expect(LINES == static_cast<int>(size.ws_row) && COLS == static_cast<int>(size.ws_col), "same-size geometry baseline matches the controlled PTY winsize");

  for (int i = 0; i < 8; ++i)
    ava::tui::terminal::Context::refresh_geometry_from_kernel();

  auto const same_size_resizes = drain_resize_events();
  expect(same_size_resizes == 0, "same-size repeated geometry refresh must not inject KEY_RESIZE (got " + std::to_string(same_size_resizes) + ")");
  expect(LINES == static_cast<int>(size.ws_row) && COLS == static_cast<int>(size.ws_col), "same-size geometry refresh leaves LINES/COLS unchanged");

  // Real resize still applies: kernel size change must update ncurses geometry.
  winsize grown = size;
  grown.ws_row = 30;
  grown.ws_col = 100;
  static_cast<void>(::ioctl(STDOUT_FILENO, TIOCSWINSZ, &grown));
  ava::tui::terminal::Context::refresh_geometry_from_kernel();
  expect(LINES == static_cast<int>(grown.ws_row) && COLS == static_cast<int>(grown.ws_col), "real kernel resize still updates ncurses geometry via resizeterm");
  // Consuming any KEY_RESIZE from the real path is fine; just drain so teardown is clean.
  static_cast<void>(drain_resize_events());

  static_cast<void>(endwin());
  delscreen(screen);
  static_cast<void>(std::fclose(input));
  static_cast<void>(std::fclose(output));
  static_cast<void>(::dup2(saved_stdout, STDOUT_FILENO));
  static_cast<void>(::close(saved_stdout));
  static_cast<void>(::close(master_fd));
  ava::tui::runtime_input::clear_startup_input_queue();
  static_cast<void>(std::setlocale(LC_ALL, previous_locale.c_str()));
}

// Deterministic openpty/newterm regression for direct terminfo where kmous=ESC[<.
// Virtual unget_wch tests cannot reproduce ncurses KEY_MOUSE matching + getmouse.
void test_direct_terminal_ncurses_mouse_sgr_no_composer_leak()
{
#ifdef NCURSES_MOUSE_VERSION
  char const* previous_locale_value = std::setlocale(LC_ALL, nullptr);
  std::string const previous_locale = previous_locale_value == nullptr ? "C" : previous_locale_value;
  static_cast<void>(std::setlocale(LC_ALL, ""));

  // Prefer Ghostty terminfo when present; otherwise the confirmed xterm-256color kmous=ESC[< path.
  char const* term_name = "xterm-256color";
  if (::access("/usr/share/terminfo/x/xterm-ghostty", R_OK) == 0)
    term_name = "xterm-ghostty";

  ScopedEnvVar term_guard("TERM", term_name);
  ScopedEnvVar tmux_guard("TMUX", "");
  ScopedEnvVar term_program_guard("TERM_PROGRAM", "");

  ava::tui::terminal_reset_mouse_tracking();
  ava::tui::runtime_input::clear_startup_input_queue();

  int master_fd = ::posix_openpt(O_RDWR | O_NOCTTY);
  expect(master_fd >= 0, "direct-terminal mouse PTY can open a master");
  if (master_fd < 0)
  {
    static_cast<void>(std::setlocale(LC_ALL, previous_locale.c_str()));
    return;
  }
  if (::grantpt(master_fd) != 0 || ::unlockpt(master_fd) != 0)
  {
    expect(false, "direct-terminal mouse PTY can grant/unlock the slave");
    static_cast<void>(::close(master_fd));
    static_cast<void>(std::setlocale(LC_ALL, previous_locale.c_str()));
    return;
  }
  char* slave_name = ::ptsname(master_fd);
  expect(slave_name != nullptr, "direct-terminal mouse PTY exposes a slave name");
  if (slave_name == nullptr)
  {
    static_cast<void>(::close(master_fd));
    static_cast<void>(std::setlocale(LC_ALL, previous_locale.c_str()));
    return;
  }
  int slave_fd = ::open(slave_name, O_RDWR | O_NOCTTY);
  expect(slave_fd >= 0, "direct-terminal mouse PTY can open the slave");
  if (slave_fd < 0)
  {
    static_cast<void>(::close(master_fd));
    static_cast<void>(std::setlocale(LC_ALL, previous_locale.c_str()));
    return;
  }

  winsize size{};
  size.ws_row = 24;
  size.ws_col = 80;
  static_cast<void>(::ioctl(slave_fd, TIOCSWINSZ, &size));

  int output_fd = ::dup(slave_fd);
  FILE* input = ::fdopen(slave_fd, "r+");
  FILE* output = output_fd >= 0 ? ::fdopen(output_fd, "w") : nullptr;
  expect(input != nullptr && output != nullptr, "direct-terminal mouse PTY can fdopen slave streams");
  if (!input || !output)
  {
    if (input)
      static_cast<void>(std::fclose(input));
    else
      static_cast<void>(::close(slave_fd));
    if (output)
      static_cast<void>(std::fclose(output));
    else if (output_fd >= 0)
      static_cast<void>(::close(output_fd));
    static_cast<void>(::close(master_fd));
    static_cast<void>(std::setlocale(LC_ALL, previous_locale.c_str()));
    return;
  }

  {
    ava::tui::terminal::Context terminal_context(output, input);
    static_cast<void>(keypad(stdscr, TRUE));
    static_cast<void>(wtimeout(stdscr, 100));

    char const* kmous = tigetstr("kmous");
    bool const kmous_is_sgr_prefix = kmous != nullptr && kmous != reinterpret_cast<char*>(-1) && std::string_view(kmous) == "\x1b[<";
    expect(kmous_is_sgr_prefix, std::string("direct-terminal mouse regression requires kmous=ESC[< under TERM=") + term_name);

    expect(has_mouse(), "terminal Context mouse mode initializes the ncurses mouse driver");

    auto feed = [&](std::string_view label, std::string_view sequence) {
      expect(write_all_fd(master_fd, sequence), std::string("direct-terminal mouse PTY can write ") + std::string(label));
    };

    auto expect_mouse = [&](std::string_view label, ava::tui::Key key, std::size_t column, std::size_t row) {
      auto input_event = read_direct_mouse_event(label);
      if (!input_event)
        return;
      expect(input_event->event.key == key && input_event->event.mouse_column == column && input_event->event.mouse_row == row && input_event->text.empty() &&
                 input_event->event.text.empty() && !input_event->bracketed_paste,
             std::string("direct-terminal mouse delivers ") + std::string(label) + " without residual text");
      expect_no_residual_mouse_payload(label);
    };

    // Feed serially: ncurses mouse FIFO is shallow; batching drops intermediate reports.
    feed("press", "\x1b[<0;10;5M");
    expect_mouse("left press", ava::tui::Key::MouseLeftPress, 10, 5);

    feed("drag", "\x1b[<32;12;6M");
    expect_mouse("left drag", ava::tui::Key::MouseLeftDrag, 12, 6);

    feed("release", "\x1b[<0;12;6m");
    expect_mouse("left release", ava::tui::Key::MouseLeftRelease, 12, 6);

    feed("wheel up", "\x1b[<64;20;8M");
    expect_mouse("wheel up", ava::tui::Key::MouseWheelUp, 20, 8);

    feed("wheel down", "\x1b[<65;20;8M");
    expect_mouse("wheel down", ava::tui::Key::MouseWheelDown, 20, 8);

    feed("shift press", "\x1b[<4;15;9M");
    expect_mouse("shift press cancel", ava::tui::Key::MousePointerCancel, 15, 9);

    // Ordinary text must still reach the composer path after mouse traffic.
    feed("plain z", "z");
    auto plain = read_direct_mouse_event("plain z");
    if (plain)
    {
      expect(plain->event.key == ava::tui::Key::Character && plain->text == "z",
             "direct-terminal mouse mode preserves ordinary character input after SGR traffic");
    }
    expect_no_residual_mouse_payload("plain z");

    // Raw SGR parser (tmux/multiplexer path) remains authoritative for full sequences.
    ava::tui::terminal_reset_mouse_tracking();
    auto const raw_press = ava::tui::terminal_escape_sequence_event("[<0;11;7M");
    auto const raw_drag = ava::tui::terminal_escape_sequence_event("[<32;13;8M");
    auto const raw_release = ava::tui::terminal_escape_sequence_event("[<0;13;8m");
    auto const raw_wheel_up = ava::tui::terminal_escape_sequence_event("[<64;21;9M");
    auto const raw_wheel_down = ava::tui::terminal_escape_sequence_event("[<65;21;9M");
    auto const raw_shift = ava::tui::terminal_escape_sequence_event("[<4;11;7M");
    expect(raw_press.key == ava::tui::Key::MouseLeftPress && raw_drag.key == ava::tui::Key::MouseLeftDrag &&
               raw_release.key == ava::tui::Key::MouseLeftRelease && raw_wheel_up.key == ava::tui::Key::MouseWheelUp &&
               raw_wheel_down.key == ava::tui::Key::MouseWheelDown && raw_shift.key == ava::tui::Key::MousePointerCancel,
           "raw SGR mouse parser (tmux/multiplexer path) still classifies press/drag/release/wheel/shift");
  }

  static_cast<void>(std::fclose(input));
  static_cast<void>(std::fclose(output));
  static_cast<void>(::close(master_fd));
  ava::tui::runtime_input::clear_startup_input_queue();
  ava::tui::terminal_reset_mouse_tracking();
  static_cast<void>(std::setlocale(LC_ALL, previous_locale.c_str()));
#else
  expect(true, "ncurses mouse support unavailable; direct-terminal mouse PTY regression skipped");
#endif
}

}  // namespace

void run_tui_terminal_osc11_theme_tests()
{
  test_osc11_background_parser();
  test_osc11_theme_precedence_and_reset();
  test_startup_input_fifo_order_and_cap();
  test_osc11_handler_arming_and_virtual_probe();
  test_osc11_tmux_skip_semantics();
  test_osc11_query_writer_and_environment_gate_seam();
  test_osc11_reply_inside_startup_bracketed_paste(false);
  test_osc11_reply_inside_startup_bracketed_paste(true);
  test_non_owned_escape_inside_startup_bracketed_paste_is_preserved();
  test_disarmed_and_malformed_osc11_inside_startup_bracketed_paste_preserve_semantics();
}

void run_tui_terminal_lifecycle_protocol_tests()
{
  test_alacritty_da2_version_gate_and_lifecycle();
  test_terminal_protocol_lifecycle_kitty_supported_path();
  test_terminal_input_flush_ordering_seam();
  test_same_size_geometry_refresh_does_not_inject_key_resize();
  test_direct_terminal_ncurses_mouse_sgr_no_composer_leak();
}

#include "sys.h"
#include "ava/tui/terminal.h"

#include <curses.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#if !defined(NCURSES_WIDECHAR) || NCURSES_WIDECHAR != 1
#error "AVA requires ncursesw with wide-character support."
#endif

#include "ava/tui/theme.h"
#include "ava/core/error.h"

#include <cctype>
#include <clocale>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <optional>
#include <string_view>
#include <utility>

namespace ava::tui {
namespace {

constexpr int kTerminalEscapeDelayMs = 100;
constexpr std::size_t kOsc11ResponseMaxBytes = 256;
constexpr int kKittyKeyboardHealthyFlags = 7;
constexpr int kKittyKeyboardBrokenAlacrittyFlags = 5;
constexpr int kBrokenAlacrittyMaxPackedVersion = 2401;
constexpr std::string_view kKittyKeyboardPushSequence = "\x1b[>7u";
constexpr std::string_view kKittyKeyboardBrokenAlacrittyPushSequence = "\x1b[>5u";
constexpr std::string_view kKittyKeyboardQuerySequence = "\x1b[>7u\x1b[?u\x1b[c";
constexpr std::string_view kKittyKeyboardBrokenAlacrittyQuerySequence = "\x1b[>5u\x1b[?u\x1b[c";
constexpr std::string_view kKittyKeyboardPopSequence = "\x1b[<u";
constexpr std::string_view kAlacrittyDa2QuerySequence = "\x1b[>c";
constexpr std::string_view kCursorStyleResetSequence = "\x1b[0 q";
constexpr std::string_view kCursorBlinkingBlockSequence = "\x1b[1 q";
constexpr std::string_view kCursorSteadyBlockSequence = "\x1b[2 q";
constexpr std::string_view kCursorBlinkingUnderlineSequence = "\x1b[3 q";
constexpr std::string_view kCursorSteadyUnderlineSequence = "\x1b[4 q";
constexpr std::string_view kCursorBlinkingBarSequence = "\x1b[5 q";
constexpr std::string_view kCursorSteadyBarSequence = "\x1b[6 q";
constexpr std::string_view kModifyOtherKeysEnableSequence = "\x1b[>4;2m";
constexpr std::string_view kModifyOtherKeysDisableSequence = "\x1b[>4;0m";
constexpr std::string_view kTerminalBackgroundQuerySequence = "\x1b]11;?\x1b\\";

bool g_keyboard_protocol_kitty_response_seen = false;
bool g_keyboard_protocol_kitty_supported = false;
int g_kitty_keyboard_active_flags = 0;
int g_kitty_keyboard_desired_flags = kKittyKeyboardHealthyFlags;
bool g_alacritty_da2_probe_armed = false;
bool g_modify_other_keys_enabled = false;
bool g_modify_other_keys_desired = false;
bool g_bracketed_paste_enabled = false;
bool g_mouse_enabled = false;
bool g_left_mouse_down = false;
bool g_terminal_background_response_handler_armed = false;
TerminalCursorSettings g_terminal_cursor_settings{};
bool g_terminal_cursor_style_forced = false;
detail::TerminalSequenceWriter g_terminal_sequence_writer = nullptr;
detail::TerminalFlushinpHook g_terminal_flushinp_hook = nullptr;
detail::TerminalTcflushHook g_terminal_tcflush_hook = nullptr;

InputEvent key_event(Key key)
{
  return InputEvent{.key = key, .character = '\0', .text = {}, .mouse_column = 0, .mouse_row = 0};
}

constexpr std::string_view kBracketedPasteEnableSequence = "\x1b[?2004h";
constexpr std::string_view kBracketedPasteDisableSequence = "\x1b[?2004l";
constexpr std::string_view kMouseEnableSequence = "\x1b[?1003l\x1b[?1000h\x1b[?1002h\x1b[?1006h";
constexpr std::string_view kMouseDisableSequence = "\x1b[?1003l\x1b[?1006l\x1b[?1002l\x1b[?1000l";

void write_terminal_sequence(std::string_view sequence)
{
  if (sequence.empty())
    return;
  if (g_terminal_sequence_writer != nullptr)
  {
    g_terminal_sequence_writer(sequence);
    return;
  }
  static_cast<void>(std::fwrite(sequence.data(), 1, sequence.size(), stdout));
  static_cast<void>(std::fflush(stdout));
}

void reset_ncurses_left_mouse_button_state() noexcept
{
#ifdef NCURSES_MOUSE_VERSION
  // Only touch ncurses mouse state when a screen is active. Pure sequence tests
  // still track ownership without requiring newterm.
  //
  // Do not gate on has_mouse(): ncurses reports has_mouse() only after a nonzero
  // mousemask() initializes the mouse driver. AVA still emits portable
  // 1000/1002/1006 enables. On direct xterm/Ghostty terminfo (kmous=ESC[<),
  // ncurses consumes ESC[< as KEY_MOUSE; without mousemask, getmouse() fails and
  // SGR payloads such as "65;68;12M" leak into the composer as ordinary text.
  if (stdscr == nullptr)
    return;
  if (g_mouse_enabled)
  {
    mmask_t previous_mask = 0;
    // Re-arm the owned mask so any incomplete button-down state is dropped at the
    // protocol boundary (Shift cancel, disable/rearm, suspend/editor handoff).
    mmask_t const mask = BUTTON1_PRESSED | BUTTON1_RELEASED | BUTTON1_CLICKED | REPORT_MOUSE_POSITION | BUTTON4_PRESSED | BUTTON5_PRESSED;
    static_cast<void>(mousemask(0, &previous_mask));
    static_cast<void>(mousemask(mask, &previous_mask));
    static_cast<void>(mouseinterval(0));
  }
  else
  {
    static_cast<void>(mousemask(0, nullptr));
  }
#endif
}

void apply_mouse_enabled(bool enabled) noexcept
{
  if (g_mouse_enabled == enabled)
    return;
  // A protocol boundary invalidates any incomplete press/drag lifecycle. This
  // also prevents a motion report after suspend/resume from becoming a drag.
  g_left_mouse_down = false;
  // Publish the target enablement before resetting ncurses so re-arm uses the new mask.
  g_mouse_enabled = enabled;
  reset_ncurses_left_mouse_button_state();
  // Keep the portable xterm SGR enable/disable boundary even when ncurses owns
  // KEY_MOUSE decoding. Multiplexers (tmux kmous=ESC[M) still deliver full SGR
  // reports to AVA's raw escape parser; direct terminfo needs mousemask above.
  write_terminal_sequence(enabled ? kMouseEnableSequence : kMouseDisableSequence);
}

void set_bracketed_paste(bool enabled)
{
  if (g_bracketed_paste_enabled == enabled)
    return;
  write_terminal_sequence(enabled ? kBracketedPasteEnableSequence : kBracketedPasteDisableSequence);
  g_bracketed_paste_enabled = enabled;
}

std::string_view kitty_keyboard_push_sequence_for_flags(int flags)
{
  return flags == kKittyKeyboardBrokenAlacrittyFlags ? kKittyKeyboardBrokenAlacrittyPushSequence : kKittyKeyboardPushSequence;
}

void push_kitty_keyboard_protocol(bool include_query)
{
  // Never grow the Kitty keyboard stack: one AVA-owned push at a time.
  if (g_kitty_keyboard_active_flags != 0)
    return;
  if (include_query)
  {
    write_terminal_sequence(g_kitty_keyboard_desired_flags == kKittyKeyboardBrokenAlacrittyFlags ? kKittyKeyboardBrokenAlacrittyQuerySequence
                                                                                                 : kKittyKeyboardQuerySequence);
  }
  else
  {
    write_terminal_sequence(kitty_keyboard_push_sequence_for_flags(g_kitty_keyboard_desired_flags));
  }
  g_kitty_keyboard_active_flags = g_kitty_keyboard_desired_flags;
  if (include_query && g_alacritty_da2_probe_armed)
    write_terminal_sequence(kAlacrittyDa2QuerySequence);
}

void pop_kitty_keyboard_protocol()
{
  if (g_kitty_keyboard_active_flags == 0)
    return;
  write_terminal_sequence(kKittyKeyboardPopSequence);
  g_kitty_keyboard_active_flags = 0;
}

void reset_keyboard_protocol_negotiation()
{
  g_keyboard_protocol_kitty_response_seen = false;
  g_keyboard_protocol_kitty_supported = false;
  g_kitty_keyboard_desired_flags = kKittyKeyboardHealthyFlags;
  g_alacritty_da2_probe_armed = false;
  g_modify_other_keys_desired = false;
  g_modify_other_keys_enabled = false;
}

void enable_modify_other_keys_fallback()
{
  if (g_modify_other_keys_enabled)
    return;
  write_terminal_sequence(kModifyOtherKeysEnableSequence);
  g_modify_other_keys_enabled = true;
  g_modify_other_keys_desired = true;
}

void disable_modify_other_keys_fallback()
{
  if (!g_modify_other_keys_enabled)
    return;
  write_terminal_sequence(kModifyOtherKeysDisableSequence);
  g_modify_other_keys_enabled = false;
}

void apply_keyboard_protocol_response_action(KeyboardProtocolResponseAction action)
{
  switch (action)
  {
    case KeyboardProtocolResponseAction::EnableModifyOtherKeys:
      enable_modify_other_keys_fallback();
      break;
    case KeyboardProtocolResponseAction::DisableModifyOtherKeys:
      disable_modify_other_keys_fallback();
      // Kitty won negotiation: do not re-enable modifyOtherKeys on handoff resume.
      g_modify_other_keys_desired = false;
      break;
    case KeyboardProtocolResponseAction::None:
      break;
  }
}

bool is_utf8_continuation(unsigned char byte)
{
  return (byte & 0xC0U) == 0x80U;
}

std::size_t utf8_sequence_length(unsigned char byte)
{
  if ((byte & 0x80U) == 0)
    return 1;
  if (byte >= 0xC2U && byte <= 0xDFU)
    return 2;
  if ((byte & 0xF0U) == 0xE0U)
    return 3;
  if (byte >= 0xF0U && byte <= 0xF4U)
    return 4;
  return 0;
}

std::optional<int> parse_unsigned_int(std::string_view text, std::size_t& index)
{
  if (index >= text.size() || text[index] < '0' || text[index] > '9')
    return std::nullopt;
  int value = 0;
  while (index < text.size() && text[index] >= '0' && text[index] <= '9')
  {
    auto const digit = text[index] - '0';
    if (value > (std::numeric_limits<int>::max() - digit) / 10)
      return std::nullopt;
    value = (value * 10) + digit;
    ++index;
  }
  return value;
}

bool consume_char(std::string_view text, std::size_t& index, char expected)
{
  if (index >= text.size() || text[index] != expected)
    return false;
  ++index;
  return true;
}

struct KittyCsiUSequence
{
  int codepoint = 0;
  std::optional<int> shifted_key = std::nullopt;
  std::optional<int> base_layout_key = std::nullopt;
  int modifiers = 0;
  int event_type = 1;
};

struct ModifyOtherKeysSequence
{
  int codepoint = 0;
  int modifiers = 0;
};

struct SpecialCsiSequence
{
  int first_parameter = 0;
  int modifier_value = 1;
  int event_type = 1;
  bool has_explicit_modifier = false;
  bool has_event_type = false;
};

std::optional<SpecialCsiSequence> parse_special_csi_sequence(std::string_view sequence)
{
  if (!sequence.starts_with('[') || sequence.size() < 3)
    return std::nullopt;

  auto index = std::size_t{1};
  auto const first_parameter = parse_unsigned_int(sequence, index);
  if (!first_parameter)
    return std::nullopt;

  auto modifier_value = 1;
  auto event_type = 1;
  auto has_explicit_modifier = false;
  auto has_event_type = false;
  if (index < sequence.size() && sequence[index] == ';')
  {
    ++index;
    auto const parsed_modifier = parse_unsigned_int(sequence, index);
    if (!parsed_modifier || *parsed_modifier <= 0)
      return std::nullopt;
    modifier_value = *parsed_modifier;
    has_explicit_modifier = true;

    if (index < sequence.size() && sequence[index] == ':')
    {
      ++index;
      auto const parsed_event_type = parse_unsigned_int(sequence, index);
      if (!parsed_event_type || *parsed_event_type < 1 || *parsed_event_type > 3)
        return std::nullopt;
      event_type = *parsed_event_type;
      has_event_type = true;
    }
  }

  if (index + 1 != sequence.size())
    return std::nullopt;
  return SpecialCsiSequence{.first_parameter = *first_parameter,
                            .modifier_value = modifier_value,
                            .event_type = event_type,
                            .has_explicit_modifier = has_explicit_modifier,
                            .has_event_type = has_event_type};
}

std::optional<int> parse_optional_unsigned_int(std::string_view text, std::size_t& index)
{
  if (index >= text.size() || text[index] < '0' || text[index] > '9')
    return std::nullopt;
  return parse_unsigned_int(text, index);
}

std::optional<KittyCsiUSequence> parse_kitty_csi_u_sequence(std::string_view sequence)
{
  if (!sequence.starts_with('[') || sequence.size() < 3 || sequence.back() != 'u')
    return std::nullopt;

  auto index = std::size_t{1};
  auto const codepoint = parse_unsigned_int(sequence, index);
  if (!codepoint)
    return std::nullopt;

  auto shifted_key = std::optional<int>{};
  auto base_layout_key = std::optional<int>{};
  if (index < sequence.size() && sequence[index] == ':')
  {
    ++index;
    shifted_key = parse_optional_unsigned_int(sequence, index);
    if (index < sequence.size() && sequence[index] == ':')
    {
      ++index;
      base_layout_key = parse_optional_unsigned_int(sequence, index);
      if (!base_layout_key)
        return std::nullopt;
    }
  }

  auto modifier_value = 1;
  auto event_type = 1;
  if (index < sequence.size() && sequence[index] == ';')
  {
    ++index;
    auto const parsed_modifier = parse_unsigned_int(sequence, index);
    if (!parsed_modifier || *parsed_modifier <= 0)
      return std::nullopt;
    modifier_value = *parsed_modifier;

    if (index < sequence.size() && sequence[index] == ':')
    {
      ++index;
      auto const parsed_event_type = parse_unsigned_int(sequence, index);
      if (!parsed_event_type || *parsed_event_type <= 0)
        return std::nullopt;
      event_type = *parsed_event_type;
    }
  }

  if (index + 1 != sequence.size() || sequence[index] != 'u')
    return std::nullopt;

  return KittyCsiUSequence{
      .codepoint = *codepoint, .shifted_key = shifted_key, .base_layout_key = base_layout_key, .modifiers = modifier_value - 1, .event_type = event_type};
}

constexpr int kKittyModifierShift = 1;
constexpr int kKittyModifierAlt = 2;
constexpr int kKittyModifierCtrl = 4;
constexpr int kKittyModifierSuper = 8;
constexpr int kKittyLockModifiers = 64 + 128;

int effective_kitty_modifiers(int modifiers)
{
  return modifiers & ~kKittyLockModifiers;
}

Key cursor_key_from_direction_and_modifiers(int direction, int modifiers)
{
  auto const effective_modifiers = effective_kitty_modifiers(modifiers);
  if ((effective_modifiers & ~(kKittyModifierShift | kKittyModifierAlt | kKittyModifierCtrl)) != 0)
    return Key::Unknown;

  auto const has_shift = (effective_modifiers & kKittyModifierShift) != 0;
  auto const has_alt = (effective_modifiers & kKittyModifierAlt) != 0;
  auto const has_ctrl = (effective_modifiers & kKittyModifierCtrl) != 0;
  switch (direction)
  {
    case -4:
      if (has_ctrl && has_shift && !has_alt)
        return Key::ShiftCtrlArrowLeft;
      if (has_alt && has_shift && !has_ctrl)
        return Key::ShiftAltArrowLeft;
      if (has_ctrl && !has_alt && !has_shift)
        return Key::CtrlArrowLeft;
      if (has_alt && !has_ctrl && !has_shift)
        return Key::AltArrowLeft;
      if (has_shift && !has_ctrl && !has_alt)
        return Key::ShiftArrowLeft;
      return effective_modifiers == 0 ? Key::ArrowLeft : Key::Unknown;
    case -3:
      if (has_ctrl && has_shift && !has_alt)
        return Key::ShiftCtrlArrowRight;
      if (has_alt && has_shift && !has_ctrl)
        return Key::ShiftAltArrowRight;
      if (has_ctrl && !has_alt && !has_shift)
        return Key::CtrlArrowRight;
      if (has_alt && !has_ctrl && !has_shift)
        return Key::AltArrowRight;
      if (has_shift && !has_ctrl && !has_alt)
        return Key::ShiftArrowRight;
      return effective_modifiers == 0 ? Key::ArrowRight : Key::Unknown;
    case -1:
      if (has_shift && !has_ctrl && !has_alt)
        return Key::ShiftArrowUp;
      if (has_alt && !has_ctrl && !has_shift)
        return Key::AltArrowUp;
      return effective_modifiers == 0 ? Key::ArrowUp : Key::Unknown;
    case -2:
      if (has_shift && !has_ctrl && !has_alt)
        return Key::ShiftArrowDown;
      if (has_alt && !has_ctrl && !has_shift)
        return Key::AltArrowDown;
      return effective_modifiers == 0 ? Key::ArrowDown : Key::Unknown;
    default:
      return Key::Unknown;
  }
}

Key home_end_key_from_modifiers(bool home, int modifiers)
{
  auto const effective_modifiers = effective_kitty_modifiers(modifiers);
  if ((effective_modifiers & ~(kKittyModifierShift | kKittyModifierCtrl)) != 0)
    return Key::Unknown;
  auto const has_shift = (effective_modifiers & kKittyModifierShift) != 0;
  auto const has_ctrl = (effective_modifiers & kKittyModifierCtrl) != 0;
  if (has_shift && has_ctrl)
    return home ? Key::ShiftCtrlHome : Key::ShiftCtrlEnd;
  if (has_ctrl)
    return home ? Key::CtrlHome : Key::CtrlEnd;
  if (has_shift)
    return home ? Key::ShiftHome : Key::ShiftEnd;
  return home ? Key::Home : Key::End;
}

Key csi_home_end_key(std::string_view sequence)
{
  if (sequence == "OH")
    return Key::Home;
  if (sequence == "OF")
    return Key::End;
  if (!sequence.starts_with('[') || sequence.size() < 2 || (sequence.back() != 'H' && sequence.back() != 'F'))
    return Key::Unknown;
  auto const home = sequence.back() == 'H';
  if (sequence.size() == 2)
    return home_end_key_from_modifiers(home, 0);

  auto const parsed = parse_special_csi_sequence(sequence);
  if (!parsed || (parsed->has_explicit_modifier && parsed->first_parameter != 1) || parsed->event_type == 3)
    return Key::Unknown;
  auto const modifier_value = parsed->has_explicit_modifier ? parsed->modifier_value : parsed->first_parameter;
  if (modifier_value <= 0)
    return Key::Unknown;
  return home_end_key_from_modifiers(home, modifier_value - 1);
}

Key csi_cursor_key(std::string_view sequence)
{
  if (sequence.size() == 2 && sequence[0] == 'O')
  {
    switch (sequence[1])
    {
      case 'A':
        return Key::ArrowUp;
      case 'B':
        return Key::ArrowDown;
      case 'C':
        return Key::ArrowRight;
      case 'D':
        return Key::ArrowLeft;
      default:
        return Key::Unknown;
    }
  }
  if (!sequence.starts_with('[') || sequence.size() < 2)
    return Key::Unknown;

  auto const final = sequence.back();
  auto direction = 0;
  switch (final)
  {
    case 'A':
      direction = -1;
      break;
    case 'B':
      direction = -2;
      break;
    case 'C':
      direction = -3;
      break;
    case 'D':
      direction = -4;
      break;
    default:
      return Key::Unknown;
  }
  if (sequence.size() == 2)
    return cursor_key_from_direction_and_modifiers(direction, 0);

  auto const parsed = parse_special_csi_sequence(sequence);
  if (!parsed || (parsed->has_explicit_modifier && parsed->first_parameter != 1) || parsed->event_type == 3)
    return Key::Unknown;
  auto const modifier_value = parsed->has_explicit_modifier ? parsed->modifier_value : parsed->first_parameter;
  if (modifier_value <= 0)
    return Key::Unknown;
  return cursor_key_from_direction_and_modifiers(direction, modifier_value - 1);
}

int normalize_kitty_functional_codepoint(int codepoint)
{
  switch (codepoint)
  {
    case 57399:
      return '0';
    case 57400:
      return '1';
    case 57401:
      return '2';
    case 57402:
      return '3';
    case 57403:
      return '4';
    case 57404:
      return '5';
    case 57405:
      return '6';
    case 57406:
      return '7';
    case 57407:
      return '8';
    case 57408:
      return '9';
    case 57409:
      return '.';
    case 57410:
      return '/';
    case 57411:
      return '*';
    case 57412:
      return '-';
    case 57413:
      return '+';
    case 57415:
      return '=';
    case 57416:
      return ',';
    case 57417:
      return -4;
    case 57418:
      return -3;
    case 57419:
      return -1;
    case 57420:
      return -2;
    case 57421:
      return -12;
    case 57422:
      return -13;
    case 57423:
      return -14;
    case 57424:
      return -15;
    case 57425:
      return -11;
    case 57426:
      return -10;
    default:
      return codepoint;
  }
}

bool is_ascii_letter_or_symbol(int codepoint)
{
  if ((codepoint >= 'a' && codepoint <= 'z') || (codepoint >= 'A' && codepoint <= 'Z') || (codepoint >= '0' && codepoint <= '9'))
    return true;
  switch (codepoint)
  {
    case '`':
    case '-':
    case '=':
    case '[':
    case ']':
    case '\\':
    case ';':
    case '\'':
    case ',':
    case '.':
    case '/':
    case '!':
    case '@':
    case '#':
    case '$':
    case '%':
    case '^':
    case '&':
    case '*':
    case '(':
    case ')':
    case '_':
    case '+':
    case '|':
    case '~':
    case '{':
    case '}':
    case ':':
    case '<':
    case '>':
    case '?':
      return true;
    default:
      return false;
  }
}

int lowercase_ascii_letter(int codepoint)
{
  if (codepoint >= 'A' && codepoint <= 'Z')
    return codepoint + ('a' - 'A');
  return codepoint;
}

int kitty_key_identity_codepoint(KittyCsiUSequence const& parsed)
{
  auto const normalized = lowercase_ascii_letter(normalize_kitty_functional_codepoint(parsed.codepoint));
  if (is_ascii_letter_or_symbol(normalized))
    return normalized;
  if (parsed.base_layout_key)
    return lowercase_ascii_letter(normalize_kitty_functional_codepoint(*parsed.base_layout_key));
  return normalized;
}

int text_identity_codepoint(int codepoint)
{
  return lowercase_ascii_letter(normalize_kitty_functional_codepoint(codepoint));
}

Key control_key_from_codepoint(int codepoint)
{
  switch (codepoint)
  {
    case '0':
      return Key::Ctrl0;
    case '1':
      return Key::Ctrl1;
    case '2':
      return Key::Ctrl2;
    case '3':
      return Key::Ctrl3;
    case '4':
      return Key::Ctrl4;
    case '5':
      return Key::Ctrl5;
    case '6':
      return Key::Ctrl6;
    case '7':
      return Key::Ctrl7;
    case '8':
      return Key::Ctrl8;
    case '9':
      return Key::Ctrl9;
    case 'a':
      return Key::CtrlA;
    case 'b':
      return Key::CtrlB;
    case 'c':
      return Key::CtrlC;
    case 'd':
      return Key::CtrlD;
    case 'e':
      return Key::CtrlE;
    case 'f':
      return Key::CtrlF;
    case 'g':
      return Key::CtrlG;
    case 'h':
      return Key::CtrlH;
    case 'k':
      return Key::CtrlK;
    case 'l':
      return Key::CtrlL;
    case 'n':
      return Key::CtrlN;
    case 'o':
      return Key::CtrlO;
    case 'p':
      return Key::CtrlP;
    case 'r':
      return Key::CtrlR;
    case 's':
      return Key::CtrlS;
    case 't':
      return Key::CtrlT;
    case 'u':
      return Key::CtrlU;
    case 'v':
      return Key::CtrlV;
    case 'w':
      return Key::CtrlW;
    case 'x':
      return Key::CtrlX;
    case 'y':
      return Key::CtrlY;
    case 'z':
      return Key::CtrlZ;
    case ']':
      return Key::CtrlRightBracket;
    case '-':
      return Key::CtrlMinus;
    case '/':
      return Key::CtrlSlash;
    default:
      return Key::Unknown;
  }
}

Key alt_key_from_codepoint(int codepoint)
{
  switch (codepoint)
  {
    case 'b':
      return Key::AltB;
    case 'd':
      return Key::AltD;
    case 'f':
      return Key::AltF;
    case 'h':
      return Key::AltH;
    case 'j':
      return Key::AltJ;
    case 'k':
      return Key::AltK;
    case 'l':
      return Key::AltL;
    case 'w':
      return Key::AltW;
    case 'y':
      return Key::AltY;
    default:
      return Key::Unknown;
  }
}

Key key_from_codepoint_and_modifiers(int codepoint, int modifiers)
{
  auto const effective_modifiers = effective_kitty_modifiers(modifiers);
  if ((effective_modifiers & kKittyModifierSuper) != 0)
    return Key::Unknown;

  auto const normalized = normalize_kitty_functional_codepoint(codepoint);
  auto const has_shift = (effective_modifiers & kKittyModifierShift) != 0;
  auto const has_alt = (effective_modifiers & kKittyModifierAlt) != 0;
  auto const has_ctrl = (effective_modifiers & kKittyModifierCtrl) != 0;
  auto const identity = text_identity_codepoint(normalized);

  if (has_ctrl && has_shift && !has_alt && identity == 'p')
    return Key::CtrlShiftP;
  if (has_ctrl && has_alt && !has_shift && identity == ']')
    return Key::CtrlAltRightBracket;
  if (is_ascii_letter_or_symbol(identity))
  {
    if (has_ctrl && !has_alt && !has_shift)
      return control_key_from_codepoint(identity);
    if (has_alt && !has_ctrl && !has_shift)
      return alt_key_from_codepoint(identity);
  }

  switch (normalized)
  {
    case 13:
      if (has_shift && !has_ctrl && !has_alt)
        return Key::ShiftEnter;
      if (has_ctrl && !has_shift && !has_alt)
        return Key::CtrlEnter;
      if (has_alt && !has_shift && !has_ctrl)
        return Key::AltEnter;
      return effective_modifiers == 0 ? Key::Enter : Key::Unknown;
    case 9:
      if (has_shift && !has_ctrl && !has_alt)
        return Key::ShiftTab;
      return effective_modifiers == 0 ? Key::Tab : Key::Unknown;
    case 27:
      return effective_modifiers == 0 ? Key::Escape : Key::Unknown;
    case 32:
      if (has_ctrl && !has_shift && !has_alt)
        return Key::CtrlSpace;
      return effective_modifiers == 0 ? Key::Space : Key::Unknown;
    case 127:
      if (has_shift && !has_ctrl && !has_alt)
        return Key::ShiftBackspace;
      if (has_ctrl && !has_shift && !has_alt)
        return Key::CtrlBackspace;
      if (has_alt && !has_ctrl && !has_shift)
        return Key::AltBackspace;
      return effective_modifiers == 0 ? Key::Backspace : Key::Unknown;
    case -4:
    case -3:
    case -2:
    case -1:
      return cursor_key_from_direction_and_modifiers(normalized, effective_modifiers);
    case -10:
      if (has_shift && !has_ctrl && !has_alt)
        return Key::ShiftDelete;
      if (has_alt && !has_ctrl && !has_shift)
        return Key::AltDelete;
      return !has_alt ? Key::Delete : Key::Unknown;
    case -12:
      return Key::PageUp;
    case -13:
      return Key::PageDown;
    case -14:
      if (has_ctrl && has_shift && !has_alt)
        return Key::ShiftCtrlHome;
      if (has_ctrl && !has_shift && !has_alt)
        return Key::CtrlHome;
      if (has_shift && !has_ctrl && !has_alt)
        return Key::ShiftHome;
      return Key::Home;
    case -15:
      if (has_ctrl && has_shift && !has_alt)
        return Key::ShiftCtrlEnd;
      if (has_ctrl && !has_shift && !has_alt)
        return Key::CtrlEnd;
      if (has_shift && !has_ctrl && !has_alt)
        return Key::ShiftEnd;
      return Key::End;
    default:
      return Key::Unknown;
  }
}

Key key_from_kitty_csi_u_sequence(std::string_view sequence)
{
  auto const parsed = parse_kitty_csi_u_sequence(sequence);
  if (!parsed)
    return Key::Unknown;

  auto const modifiers = effective_kitty_modifiers(parsed->modifiers);
  if (parsed->event_type == 3)
    return Key::Unknown;
  auto const identity = kitty_key_identity_codepoint(*parsed);
  if (identity != text_identity_codepoint(parsed->codepoint))
    return key_from_codepoint_and_modifiers(identity, modifiers);
  return key_from_codepoint_and_modifiers(parsed->codepoint, modifiers);
}

void append_utf8_codepoint(std::string& text, int codepoint)
{
  if (codepoint < 0 || codepoint > 0x10FFFF || (codepoint >= 0xD800 && codepoint <= 0xDFFF))
    return;
  if (codepoint <= 0x7F)
  {
    text.push_back(static_cast<char>(codepoint));
  }
  else if (codepoint <= 0x7FF)
  {
    text.push_back(static_cast<char>(0xC0 | ((codepoint >> 6) & 0x1F)));
    text.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
  }
  else if (codepoint <= 0xFFFF)
  {
    text.push_back(static_cast<char>(0xE0 | ((codepoint >> 12) & 0x0F)));
    text.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
    text.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
  }
  else
  {
    text.push_back(static_cast<char>(0xF0 | ((codepoint >> 18) & 0x07)));
    text.push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F)));
    text.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
    text.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
  }
}

std::optional<InputEvent> kitty_csi_u_printable_event(std::string_view sequence)
{
  auto const parsed = parse_kitty_csi_u_sequence(sequence);
  if (!parsed)
    return std::nullopt;

  auto const modifiers = effective_kitty_modifiers(parsed->modifiers);
  if (parsed->event_type == 3)
    return std::nullopt;
  if ((modifiers & ~(kKittyModifierShift | kKittyLockModifiers)) != 0)
    return std::nullopt;

  auto codepoint = parsed->codepoint;
  if ((modifiers & kKittyModifierShift) != 0 && parsed->shifted_key)
    codepoint = *parsed->shifted_key;
  codepoint = normalize_kitty_functional_codepoint(codepoint);
  if (codepoint < 32)
    return std::nullopt;

  std::string text;
  append_utf8_codepoint(text, codepoint);
  if (text.empty())
    return std::nullopt;
  return InputEvent{.key = Key::Character, .character = text.size() == 1 ? text.front() : '\0', .text = std::move(text), .mouse_column = 0, .mouse_row = 0};
}

std::optional<ModifyOtherKeysSequence> parse_modify_other_keys_sequence(std::string_view sequence)
{
  if (!sequence.starts_with('[') || sequence.size() < 8 || sequence.back() != '~')
    return std::nullopt;

  auto index = std::size_t{1};
  auto const escape_code = parse_unsigned_int(sequence, index);
  if (!escape_code || *escape_code != 27 || !consume_char(sequence, index, ';'))
    return std::nullopt;

  auto const modifier_value = parse_unsigned_int(sequence, index);
  if (!modifier_value || *modifier_value <= 0 || !consume_char(sequence, index, ';'))
    return std::nullopt;

  auto const codepoint = parse_unsigned_int(sequence, index);
  if (!codepoint || index + 1 != sequence.size() || sequence[index] != '~')
    return std::nullopt;

  return ModifyOtherKeysSequence{.codepoint = *codepoint, .modifiers = *modifier_value - 1};
}

Key key_from_modify_other_keys_sequence(std::string_view sequence)
{
  auto const parsed = parse_modify_other_keys_sequence(sequence);
  if (!parsed)
    return Key::Unknown;
  return key_from_codepoint_and_modifiers(parsed->codepoint, parsed->modifiers);
}

Key function_key_from_number(int number)
{
  switch (number)
  {
    case 11:
      return Key::F1;
    case 12:
      return Key::F2;
    case 13:
      return Key::F3;
    case 14:
      return Key::F4;
    case 15:
      return Key::F5;
    case 17:
      return Key::F6;
    case 18:
      return Key::F7;
    case 19:
      return Key::F8;
    case 20:
      return Key::F9;
    case 21:
      return Key::F10;
    case 23:
      return Key::F11;
    case 24:
      return Key::F12;
    default:
      return Key::Unknown;
  }
}

Key function_key_from_sequence(std::string_view sequence)
{
  if (sequence == "OP")
    return Key::F1;
  if (sequence == "OQ")
    return Key::F2;
  if (sequence == "OR")
    return Key::F3;
  if (sequence == "OS")
    return Key::F4;

  if (sequence.starts_with('[') && sequence.size() >= 2 && sequence.back() >= 'P' && sequence.back() <= 'S')
  {
    auto const parsed = parse_special_csi_sequence(sequence);
    if (!parsed || !parsed->has_explicit_modifier || !parsed->has_event_type || parsed->first_parameter != 1 || parsed->event_type == 3 ||
        effective_kitty_modifiers(parsed->modifier_value - 1) != 0)
      return Key::Unknown;
    switch (sequence.back())
    {
      case 'P':
        return Key::F1;
      case 'Q':
        return Key::F2;
      case 'R':
        return Key::F3;
      case 'S':
        return Key::F4;
      default:
        return Key::Unknown;
    }
  }

  if (sequence.starts_with('[') && sequence.ends_with('~'))
  {
    auto const parsed = parse_special_csi_sequence(sequence);
    if (parsed && parsed->has_explicit_modifier && parsed->has_event_type)
    {
      if (parsed->event_type == 3 || effective_kitty_modifiers(parsed->modifier_value - 1) != 0)
        return Key::Unknown;
      return function_key_from_number(parsed->first_parameter);
    }
  }

  if (sequence.size() >= 4 && sequence.starts_with('[') && sequence.ends_with('~'))
  {
    auto index = std::size_t{1};
    auto const number = parse_unsigned_int(sequence, index);
    if (number && index + 1 == sequence.size())
      return function_key_from_number(*number);
  }
  return Key::Unknown;
}

std::optional<InputEvent> modify_other_keys_printable_event(std::string_view sequence)
{
  auto const parsed = parse_modify_other_keys_sequence(sequence);
  if (!parsed)
    return std::nullopt;

  auto const modifiers = effective_kitty_modifiers(parsed->modifiers);
  if ((modifiers & ~(kKittyModifierShift | kKittyLockModifiers)) != 0)
    return std::nullopt;
  auto const codepoint = normalize_kitty_functional_codepoint(parsed->codepoint);
  if (codepoint < 32)
    return std::nullopt;

  std::string text;
  append_utf8_codepoint(text, codepoint);
  if (text.empty())
    return std::nullopt;
  return InputEvent{.key = Key::Character, .character = text.size() == 1 ? text.front() : '\0', .text = std::move(text), .mouse_column = 0, .mouse_row = 0};
}

bool is_modified_enter_csi_u(std::string_view sequence, int expected_modifiers)
{
  if (!sequence.starts_with('['))
    return false;
  auto index = std::size_t{1};
  auto const codepoint = parse_unsigned_int(sequence, index);
  if (!codepoint || *codepoint != 13)
    return false;
  if (!consume_char(sequence, index, ';'))
    return false;
  auto const modifiers = parse_unsigned_int(sequence, index);
  if (!modifiers || *modifiers != expected_modifiers)
    return false;
  return index + 1 == sequence.size() && (sequence[index] == 'u' || sequence[index] == '~');
}

bool is_shift_enter_csi_u(std::string_view sequence)
{
  return is_modified_enter_csi_u(sequence, 2);
}

bool is_ctrl_enter_csi_u(std::string_view sequence)
{
  return is_modified_enter_csi_u(sequence, 5);
}

bool is_alt_enter_csi_u(std::string_view sequence)
{
  return is_modified_enter_csi_u(sequence, 3);
}

bool is_ctrl_minus_csi_u(std::string_view sequence)
{
  if (!sequence.starts_with('['))
    return false;
  auto index = std::size_t{1};
  auto const codepoint = parse_unsigned_int(sequence, index);
  if (!codepoint || *codepoint != 45)
    return false;
  if (!consume_char(sequence, index, ';'))
    return false;
  auto const modifiers = parse_unsigned_int(sequence, index);
  if (!modifiers || *modifiers != 5)
    return false;
  return index + 1 == sequence.size() && (sequence[index] == 'u' || sequence[index] == '~');
}

bool is_ctrl_shift_p_csi_u(std::string_view sequence)
{
  if (!sequence.starts_with('['))
    return false;
  auto index = std::size_t{1};
  auto const codepoint = parse_unsigned_int(sequence, index);
  if (!codepoint || (*codepoint != 'P' && *codepoint != 'p'))
    return false;
  if (!consume_char(sequence, index, ';'))
    return false;
  auto const modifiers = parse_unsigned_int(sequence, index);
  if (!modifiers || *modifiers != 6)
    return false;
  return index + 1 == sequence.size() && (sequence[index] == 'u' || sequence[index] == '~');
}

bool is_alt_delete_sequence(std::string_view sequence)
{
  if (!sequence.starts_with('['))
    return false;
  auto index = std::size_t{1};
  auto const codepoint = parse_unsigned_int(sequence, index);
  if (!codepoint || *codepoint != 3)
    return false;
  if (!consume_char(sequence, index, ';'))
    return false;
  auto const modifiers = parse_unsigned_int(sequence, index);
  if (!modifiers || *modifiers != 3)
    return false;
  return index + 1 == sequence.size() && sequence[index] == '~';
}

bool is_legacy_modified_enter_sequence(std::string_view sequence, int expected_modifiers)
{
  if (!sequence.starts_with('['))
    return false;
  auto index = std::size_t{1};
  auto const escape_code = parse_unsigned_int(sequence, index);
  if (!escape_code || *escape_code != 27)
    return false;
  if (!consume_char(sequence, index, ';'))
    return false;
  auto const modifiers = parse_unsigned_int(sequence, index);
  if (!modifiers || *modifiers != expected_modifiers)
    return false;
  if (!consume_char(sequence, index, ';'))
    return false;
  auto const key_code = parse_unsigned_int(sequence, index);
  return key_code && *key_code == 13 && index + 1 == sequence.size() && sequence[index] == '~';
}

bool is_legacy_shift_enter_sequence(std::string_view sequence)
{
  return is_legacy_modified_enter_sequence(sequence, 2);
}

bool is_legacy_ctrl_enter_sequence(std::string_view sequence)
{
  return is_legacy_modified_enter_sequence(sequence, 5);
}

bool is_legacy_alt_enter_sequence(std::string_view sequence)
{
  return is_legacy_modified_enter_sequence(sequence, 3);
}

Key page_key_from_csi_tilde(std::string_view sequence)
{
  if (!sequence.starts_with('[') || sequence.empty() || sequence.back() != '~')
    return Key::Unknown;

  auto const parsed = parse_special_csi_sequence(sequence);
  if (!parsed || parsed->event_type == 3)
    return Key::Unknown;

  auto effective_modifiers = 0;
  if (parsed->has_explicit_modifier)
  {
    effective_modifiers = effective_kitty_modifiers(parsed->modifier_value - 1);
    if ((effective_modifiers & ~(kKittyModifierShift | kKittyModifierAlt | kKittyModifierCtrl)) != 0)
      return Key::Unknown;
  }

  if (parsed->first_parameter == 1 || parsed->first_parameter == 7)
    return home_end_key_from_modifiers(true, effective_modifiers);
  if (parsed->first_parameter == 4 || parsed->first_parameter == 8)
    return home_end_key_from_modifiers(false, effective_modifiers);
  if (parsed->first_parameter == 3)
  {
    if (effective_modifiers == kKittyModifierShift)
      return Key::ShiftDelete;
    if (effective_modifiers == kKittyModifierAlt)
      return Key::AltDelete;
    return effective_modifiers == 0 ? Key::Delete : Key::Unknown;
  }
  if (parsed->first_parameter == 2)
    return Key::Insert;
  if (parsed->first_parameter == 5)
    return Key::PageUp;
  if (parsed->first_parameter == 6)
    return Key::PageDown;
  return Key::Unknown;
}

InputEvent normalized_left_mouse_event(bool shift, bool wheel_up, bool wheel_down, bool press, bool release, bool motion, bool clicked, std::size_t column,
                                       std::size_t row)
{
  auto event = key_event(Key::Unknown);
  if (wheel_up || wheel_down)
  {
    // Preserve transcript scrolling even when a terminal reports Shift with a
    // wheel gesture; Shift ownership only suppresses button selection.
    event.key = wheel_up ? Key::MouseWheelUp : Key::MouseWheelDown;
  }
  else if (shift)
  {
    // Shift belongs to terminal-native selection. Cancel AVA pointer ownership so a
    // later unmodified hover/release cannot extend or toggle an armed interaction.
    g_left_mouse_down = false;
    reset_ncurses_left_mouse_button_state();
    event.key = Key::MousePointerCancel;
  }
  else if (clicked)
  {
    // ncurses CLICKED is a complete press/release fallback.
    g_left_mouse_down = false;
    event.key = Key::MouseLeftClick;
  }
  else if (release)
  {
    if (g_left_mouse_down)
      event.key = Key::MouseLeftRelease;
    g_left_mouse_down = false;
  }
  else if (motion && g_left_mouse_down)
  {
    event.key = Key::MouseLeftDrag;
  }
  else if (press)
  {
    g_left_mouse_down = true;
    event.key = Key::MouseLeftPress;
  }
  if (event.key != Key::Unknown)
  {
    event.mouse_column = column;
    event.mouse_row = row;
  }
  return event;
}

std::optional<InputEvent> sgr_mouse_event(std::string_view sequence)
{
  if (!sequence.starts_with("[<") || sequence.size() < 7)
    return std::nullopt;
  auto index = std::size_t{2};
  auto const button = parse_unsigned_int(sequence, index);
  if (!button || !consume_char(sequence, index, ';'))
    return std::nullopt;
  auto const column = parse_unsigned_int(sequence, index);
  if (!column || !consume_char(sequence, index, ';'))
    return std::nullopt;
  auto const row = parse_unsigned_int(sequence, index);
  if (!row || index + 1 != sequence.size() || (sequence[index] != 'M' && sequence[index] != 'm'))
    return std::nullopt;
  auto const button_code = *button;
  auto const final = sequence[index];
  auto const is_motion = (button_code & 32) != 0;
  auto const is_wheel = (button_code & 64) != 0;
  auto const has_shift = (button_code & 4) != 0;
  auto const base_button = button_code & 3;
  return normalized_left_mouse_event(has_shift, is_wheel && final == 'M' && (button_code & 1) == 0, is_wheel && final == 'M' && (button_code & 1) != 0,
                                     final == 'M' && !is_motion && !is_wheel && base_button == 0, final == 'm' && !is_wheel && base_button == 0,
                                     final == 'M' && is_motion && !is_wheel && base_button == 0, false, static_cast<std::size_t>(*column),
                                     static_cast<std::size_t>(*row));
}

Key sgr_mouse_key(std::string_view sequence)
{
  if (auto event = sgr_mouse_event(sequence))
    return event->key;
  return Key::Unknown;
}

bool is_legacy_mouse_sequence(std::string_view sequence)
{
  return sequence.starts_with("[M") && sequence.size() >= 5;
}

std::optional<InputEvent> legacy_mouse_event(std::string_view sequence)
{
  if (!is_legacy_mouse_sequence(sequence))
    return std::nullopt;
  auto const button_byte = static_cast<unsigned char>(sequence[2]);
  auto const column_byte = static_cast<unsigned char>(sequence[3]);
  auto const row_byte = static_cast<unsigned char>(sequence[4]);
  if (button_byte < 32 || column_byte < 32 || row_byte < 32)
    return key_event(Key::Unknown);
  auto const button = static_cast<unsigned int>(button_byte - 32U);
  auto const is_motion = (button & 32U) != 0;
  auto const is_wheel = (button & 64U) != 0;
  auto const has_shift = (button & 4U) != 0;
  auto const base_button = button & 3U;
  return normalized_left_mouse_event(has_shift, is_wheel && (button & 1U) == 0, is_wheel && (button & 1U) != 0, !is_motion && !is_wheel && base_button == 0U,
                                     !is_wheel && base_button == 3U, is_motion && !is_wheel && base_button == 0U, false,
                                     static_cast<std::size_t>(column_byte - 32U), static_cast<std::size_t>(row_byte - 32U));
}

Key legacy_mouse_key(std::string_view sequence)
{
  if (auto event = legacy_mouse_event(sequence))
    return event->key;
  return Key::Unknown;
}

bool is_csi_final_byte(unsigned char byte)
{
  return byte >= 0x40U && byte <= 0x7EU;
}

bool is_control_string_intro(char ch)
{
  return ch == ']' || ch == 'P' || ch == '^' || ch == '_' || ch == 'X';
}

bool ends_with_string_terminator(std::string_view sequence)
{
  return sequence.size() >= 2 && sequence[sequence.size() - 2] == '\x1b' && sequence.back() == '\\';
}

bool is_control_string_complete(std::string_view sequence)
{
  if (sequence.empty() || !is_control_string_intro(sequence.front()))
    return false;
  if (ends_with_string_terminator(sequence))
    return true;
  return sequence.front() == ']' && sequence.find('\a') != std::string_view::npos;
}

std::optional<int> hex_digit_value(char ch)
{
  if (ch >= '0' && ch <= '9')
    return ch - '0';
  if (ch >= 'a' && ch <= 'f')
    return 10 + (ch - 'a');
  if (ch >= 'A' && ch <= 'F')
    return 10 + (ch - 'A');
  return std::nullopt;
}

std::optional<int> parse_hex_channel(std::string_view text)
{
  if (text.empty() || text.size() > 4)
    return std::nullopt;

  unsigned value = 0;
  for (char const ch : text)
  {
    auto const digit = hex_digit_value(ch);
    if (!digit)
      return std::nullopt;
    value = (value << 4U) | static_cast<unsigned>(*digit);
  }

  auto const max_value = (1U << (static_cast<unsigned>(text.size()) * 4U)) - 1U;
  if (max_value == 0)
    return std::nullopt;
  return static_cast<int>((value * 255U) / max_value);
}

std::optional<TerminalBackgroundColor> parse_rgb_slash_color(std::string_view payload, bool allow_alpha)
{
  auto const prefix = allow_alpha ? std::string_view("rgba:") : std::string_view("rgb:");
  if (!payload.starts_with(prefix))
    return std::nullopt;
  payload.remove_prefix(prefix.size());

  auto next_component = [&](bool last) -> std::optional<std::string_view> {
    if (payload.empty())
      return std::nullopt;
    if (last)
    {
      auto const component = payload;
      payload = {};
      return component;
    }
    auto const separator = payload.find('/');
    if (separator == std::string_view::npos)
      return std::nullopt;
    auto const component = payload.substr(0, separator);
    payload.remove_prefix(separator + 1);
    return component;
  };

  auto const red_text = next_component(false);
  auto const green_text = next_component(false);
  auto const blue_text = next_component(!allow_alpha);
  if (!red_text || !green_text || !blue_text)
    return std::nullopt;
  if (red_text->size() != green_text->size() || green_text->size() != blue_text->size())
    return std::nullopt;

  auto const red = parse_hex_channel(*red_text);
  auto const green = parse_hex_channel(*green_text);
  auto const blue = parse_hex_channel(*blue_text);
  if (!red || !green || !blue)
    return std::nullopt;

  if (allow_alpha)
  {
    auto const alpha_text = next_component(true);
    if (!alpha_text || alpha_text->size() != red_text->size() || !parse_hex_channel(*alpha_text))
      return std::nullopt;
  }

  if (!payload.empty())
    return std::nullopt;

  return TerminalBackgroundColor{.red = *red, .green = *green, .blue = *blue};
}

std::optional<TerminalBackgroundColor> parse_hash_color(std::string_view payload)
{
  if (!payload.starts_with('#') || (payload.size() != 7 && payload.size() != 13))
    return std::nullopt;

  auto const width = payload.size() == 7 ? std::size_t{2} : std::size_t{4};
  auto const red = parse_hex_channel(payload.substr(1, width));
  auto const green = parse_hex_channel(payload.substr(1 + width, width));
  auto const blue = parse_hex_channel(payload.substr(1 + (2 * width), width));
  if (!red || !green || !blue)
    return std::nullopt;
  return TerminalBackgroundColor{.red = *red, .green = *green, .blue = *blue};
}

std::optional<std::string_view> osc11_payload(std::string_view sequence)
{
  if (sequence.size() > kOsc11ResponseMaxBytes || !sequence.starts_with("]11;"))
    return std::nullopt;

  if (sequence.back() == '\a')
    return sequence.substr(4, sequence.size() - 5);
  if (ends_with_string_terminator(sequence))
    return sequence.substr(4, sequence.size() - 6);
  return std::nullopt;
}

int rgb_luminance(TerminalBackgroundColor const& color)
{
  return ((color.red * 299) + (color.green * 587) + (color.blue * 114)) / 1000;
}

}  // namespace

bool detail::force_terminal_cursor_visible() noexcept
{
  static_cast<void>(curs_set(0));
  return curs_set(1) != ERR;
}

void detail::set_terminal_sequence_writer_for_test(TerminalSequenceWriter writer) noexcept
{
  g_terminal_sequence_writer = writer;
}

void detail::reset_terminal_sequence_writer_for_test() noexcept
{
  g_terminal_sequence_writer = nullptr;
}

void detail::set_terminal_input_flush_hooks_for_test(TerminalFlushinpHook flushinp_hook, TerminalTcflushHook tcflush_hook) noexcept
{
  g_terminal_flushinp_hook = flushinp_hook;
  g_terminal_tcflush_hook = tcflush_hook;
}

void detail::reset_terminal_input_flush_hooks_for_test() noexcept
{
  g_terminal_flushinp_hook = nullptr;
  g_terminal_tcflush_hook = nullptr;
}

void detail::reset_terminal_protocol_ownership_for_test() noexcept
{
  g_keyboard_protocol_kitty_response_seen = false;
  g_keyboard_protocol_kitty_supported = false;
  g_kitty_keyboard_active_flags = 0;
  g_kitty_keyboard_desired_flags = kKittyKeyboardHealthyFlags;
  g_alacritty_da2_probe_armed = false;
  g_modify_other_keys_enabled = false;
  g_modify_other_keys_desired = false;
  g_bracketed_paste_enabled = false;
  g_mouse_enabled = false;
  g_left_mouse_down = false;
  g_terminal_cursor_settings = {};
  g_terminal_cursor_style_forced = false;
}

void erase_last_utf8_codepoint(std::string& text)
{
  if (text.empty())
    return;
  if (!is_utf8_continuation(static_cast<unsigned char>(text.back())))
  {
    text.pop_back();
    return;
  }

  auto start = text.size();
  while (start > 0 && is_utf8_continuation(static_cast<unsigned char>(text[start - 1])))
  {
    --start;
  }
  if (start == 0)
  {
    text.pop_back();
    return;
  }

  auto const expected_length = utf8_sequence_length(static_cast<unsigned char>(text[start - 1]));
  auto const actual_length = text.size() - (start - 1);
  if (expected_length > 1 && expected_length == actual_length)
  {
    text.erase(start - 1);
  }
  else
  {
    text.pop_back();
  }
}

int terminal_escape_delay_ms()
{
  return kTerminalEscapeDelayMs;
}

std::string_view terminal_kitty_keyboard_push_sequence()
{
  return kKittyKeyboardPushSequence;
}

std::string_view terminal_kitty_keyboard_query_sequence()
{
  return kKittyKeyboardQuerySequence;
}

std::string_view terminal_kitty_keyboard_pop_sequence()
{
  return kKittyKeyboardPopSequence;
}

std::string_view terminal_alacritty_da2_query_sequence()
{
  return kAlacrittyDa2QuerySequence;
}

std::optional<int> terminal_alacritty_da2_version(std::string_view sequence)
{
  if (!sequence.starts_with("[>0;") || sequence.size() < 8 || sequence.back() != 'c')
    return std::nullopt;
  auto index = std::size_t{4};
  auto const version = parse_unsigned_int(sequence, index);
  if (!version || *version < 1 || *version > 999999 || !consume_char(sequence, index, ';'))
    return std::nullopt;
  auto const final_parameter = parse_unsigned_int(sequence, index);
  if (!final_parameter || *final_parameter != 1 || index + 1 != sequence.size() || sequence[index] != 'c')
    return std::nullopt;
  return version;
}

bool terminal_alacritty_da2_probe_environment_allows_query(std::optional<std::string_view> tmux, std::optional<std::string_view> term,
                                                           std::optional<std::string_view> term_program)
{
  if (tmux && !tmux->empty())
    return false;
  if (term && (term->starts_with("tmux") || term->starts_with("screen")))
    return false;
  if (!term_program)
    return false;
  constexpr std::string_view expected = "alacritty";
  if (term_program->size() != expected.size())
    return false;
  for (std::size_t index = 0; index < expected.size(); ++index)
  {
    if (static_cast<char>(std::tolower(static_cast<unsigned char>((*term_program)[index]))) != expected[index])
      return false;
  }
  return true;
}

std::string_view terminal_modify_other_keys_enable_sequence()
{
  return kModifyOtherKeysEnableSequence;
}

std::string_view terminal_modify_other_keys_disable_sequence()
{
  return kModifyOtherKeysDisableSequence;
}

std::string_view terminal_bracketed_paste_enable_sequence()
{
  return kBracketedPasteEnableSequence;
}

std::string_view terminal_bracketed_paste_disable_sequence()
{
  return kBracketedPasteDisableSequence;
}

std::string_view terminal_mouse_enable_sequence()
{
  return kMouseEnableSequence;
}

std::string_view terminal_mouse_disable_sequence()
{
  return kMouseDisableSequence;
}

TerminalProtocolOwnership terminal_protocol_ownership() noexcept
{
  return TerminalProtocolOwnership{
      .kitty_keyboard_pushed = g_kitty_keyboard_active_flags != 0,
      .kitty_keyboard_active_flags = g_kitty_keyboard_active_flags,
      .kitty_keyboard_desired_flags = g_kitty_keyboard_desired_flags,
      .alacritty_da2_probe_armed = g_alacritty_da2_probe_armed,
      .kitty_keyboard_supported = g_keyboard_protocol_kitty_supported,
      .keyboard_protocol_kitty_response_seen = g_keyboard_protocol_kitty_response_seen,
      .modify_other_keys_enabled = g_modify_other_keys_enabled,
      .modify_other_keys_desired = g_modify_other_keys_desired,
      .bracketed_paste_enabled = g_bracketed_paste_enabled,
      .mouse_enabled = g_mouse_enabled,
  };
}

void arm_owned_terminal_protocols_on_enter() noexcept
{
  // Fresh session: healthy/unknown terminals start with all supported key-reporting
  // flags. A positively identified direct Alacritty starts conservatively without
  // event types and upgrades only after its asynchronous strict DA2 reply proves
  // the fixed version.
  reset_keyboard_protocol_negotiation();
  auto const environment = [](char const* name) -> std::optional<std::string_view> {
    auto const* value = std::getenv(name);
    return value == nullptr ? std::nullopt : std::optional<std::string_view>{value};
  };
  g_alacritty_da2_probe_armed = terminal_alacritty_da2_probe_environment_allows_query(environment("TMUX"), environment("TERM"), environment("TERM_PROGRAM"));
  if (g_alacritty_da2_probe_armed)
    g_kitty_keyboard_desired_flags = kKittyKeyboardBrokenAlacrittyFlags;
  apply_mouse_enabled(true);
  set_bracketed_paste(true);
  push_kitty_keyboard_protocol(/*include_query=*/true);
}

void release_owned_terminal_protocols() noexcept
{
  // Handoff path: disable what AVA currently owns without forgetting negotiated
  // keyboard preferences (modifyOtherKeys desired / kitty supported).
  disable_modify_other_keys_fallback();
  pop_kitty_keyboard_protocol();
  set_bracketed_paste(false);
  apply_mouse_enabled(false);
  if (g_terminal_cursor_style_forced)
  {
    write_terminal_sequence(kCursorStyleResetSequence);
    g_terminal_cursor_style_forced = false;
  }
}

void rearm_owned_terminal_protocols() noexcept
{
  // Resume path after reset_prog_mode. Never re-probes OSC 11. Re-pushes Kitty
  // without query/DA so the stack cannot grow and negotiation is not restarted.
  apply_mouse_enabled(true);
  set_bracketed_paste(true);
  push_kitty_keyboard_protocol(/*include_query=*/false);
  if (g_modify_other_keys_desired)
    enable_modify_other_keys_fallback();
  apply_terminal_cursor_settings(g_terminal_cursor_settings);
}

void restore_owned_terminal_protocols() noexcept
{
  release_owned_terminal_protocols();
  reset_keyboard_protocol_negotiation();
  g_terminal_cursor_settings = {};
}

std::string_view terminal_cursor_style_sequence(TerminalCursorSettings settings) noexcept
{
  switch (settings.style)
  {
    case TerminalCursorStyle::Default:
      return {};
    case TerminalCursorStyle::Block:
      return settings.blink ? kCursorBlinkingBlockSequence : kCursorSteadyBlockSequence;
    case TerminalCursorStyle::Underline:
      return settings.blink ? kCursorBlinkingUnderlineSequence : kCursorSteadyUnderlineSequence;
    case TerminalCursorStyle::Bar:
      return settings.blink ? kCursorBlinkingBarSequence : kCursorSteadyBarSequence;
  }
  return {};
}

std::string_view terminal_cursor_style_reset_sequence() noexcept
{
  return kCursorStyleResetSequence;
}

void apply_terminal_cursor_settings(TerminalCursorSettings settings) noexcept
{
  auto const already_applied = g_terminal_cursor_style_forced && g_terminal_cursor_settings == settings;
  g_terminal_cursor_settings = settings;
  auto const sequence = terminal_cursor_style_sequence(settings);
  if (sequence.empty())
  {
    if (g_terminal_cursor_style_forced)
    {
      write_terminal_sequence(kCursorStyleResetSequence);
      g_terminal_cursor_style_forced = false;
    }
    return;
  }
  if (already_applied)
    return;
  write_terminal_sequence(sequence);
  g_terminal_cursor_style_forced = true;
}

TerminalCursorSettings terminal_cursor_settings() noexcept
{
  return g_terminal_cursor_settings;
}

bool terminal_cursor_style_forced() noexcept
{
  return g_terminal_cursor_style_forced;
}

void refresh_terminal_geometry_from_kernel() noexcept
{
  winsize size{};
  if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &size) != 0)
    return;
  if (size.ws_row <= 0 || size.ws_col <= 0)
    return;
  // Fail-soft when curses is not initialized (unit tests without a TTY, partial enter).
  if (stdscr == nullptr)
    return;
  // Same-size resizeterm injects KEY_RESIZE on some hosts. Plugin surface fit checks
  // refresh geometry repeatedly; flooding the input queue starves real Down/Enter.
  auto const rows = static_cast<int>(size.ws_row);
  auto const cols = static_cast<int>(size.ws_col);
  if (is_term_resized(rows, cols) == FALSE)
    return;
  static_cast<void>(resizeterm(rows, cols));
}

void discard_pending_terminal_input() noexcept
{
  // Ordering: drain the curses input queue first while the screen is still the
  // active consumer, then discard any remaining kernel-side unread bytes. Both
  // steps are nonblocking, fail-soft, and free of sleeps or read loops.
  if (g_terminal_flushinp_hook != nullptr)
    g_terminal_flushinp_hook();
  else
    flushinp();

  if (g_terminal_tcflush_hook != nullptr)
    static_cast<void>(g_terminal_tcflush_hook(STDIN_FILENO, TCIFLUSH));
  else
    static_cast<void>(tcflush(STDIN_FILENO, TCIFLUSH));
}

std::optional<int> terminal_kitty_keyboard_flags_response(std::string_view sequence)
{
  if (!sequence.starts_with("[?") || sequence.size() < 4 || sequence.back() != 'u')
    return std::nullopt;

  auto index = std::size_t{2};
  auto const flags = parse_unsigned_int(sequence, index);
  if (!flags || index + 1 != sequence.size() || sequence[index] != 'u')
    return std::nullopt;
  return flags;
}

bool terminal_device_attributes_response(std::string_view sequence)
{
  if (!sequence.starts_with("[?") || sequence.size() < 4 || sequence.back() != 'c')
    return false;

  auto index = std::size_t{2};
  if (!parse_unsigned_int(sequence, index))
    return false;
  while (index + 1 < sequence.size())
  {
    if (!consume_char(sequence, index, ';'))
      return false;
    if (!parse_unsigned_int(sequence, index))
      return false;
  }
  return index + 1 == sequence.size() && sequence[index] == 'c';
}

KeyboardProtocolResponseAction terminal_keyboard_protocol_response_action(std::string_view sequence, bool kitty_response_seen, bool modify_other_keys_enabled)
{
  if (auto const flags = terminal_kitty_keyboard_flags_response(sequence))
  {
    if (*flags > 0)
      return modify_other_keys_enabled ? KeyboardProtocolResponseAction::DisableModifyOtherKeys : KeyboardProtocolResponseAction::None;
    return modify_other_keys_enabled ? KeyboardProtocolResponseAction::None : KeyboardProtocolResponseAction::EnableModifyOtherKeys;
  }

  if (terminal_device_attributes_response(sequence))
  {
    if (!kitty_response_seen && !modify_other_keys_enabled)
      return KeyboardProtocolResponseAction::EnableModifyOtherKeys;
  }

  return KeyboardProtocolResponseAction::None;
}

bool terminal_keyboard_protocol_handle_response(std::string_view sequence)
{
  if (auto const version = terminal_alacritty_da2_version(sequence))
  {
    if (!g_alacritty_da2_probe_armed)
      return false;
    g_alacritty_da2_probe_armed = false;
    g_kitty_keyboard_desired_flags = *version > kBrokenAlacrittyMaxPackedVersion ? kKittyKeyboardHealthyFlags : kKittyKeyboardBrokenAlacrittyFlags;
    // Replace AVA's one active stack layer rather than stacking another push.
    if (g_kitty_keyboard_active_flags != 0 && g_kitty_keyboard_active_flags != g_kitty_keyboard_desired_flags)
    {
      pop_kitty_keyboard_protocol();
      push_kitty_keyboard_protocol(/*include_query=*/false);
    }
    return true;
  }

  if (auto const flags = terminal_kitty_keyboard_flags_response(sequence))
  {
    auto const action = terminal_keyboard_protocol_response_action(sequence, g_keyboard_protocol_kitty_response_seen, g_modify_other_keys_enabled);
    g_keyboard_protocol_kitty_response_seen = true;
    g_keyboard_protocol_kitty_supported = *flags > 0;
    apply_keyboard_protocol_response_action(action);
    return true;
  }

  if (terminal_device_attributes_response(sequence))
  {
    apply_keyboard_protocol_response_action(
        terminal_keyboard_protocol_response_action(sequence, g_keyboard_protocol_kitty_response_seen, g_modify_other_keys_enabled));
    return true;
  }

  return false;
}

std::string_view terminal_background_query_sequence()
{
  return kTerminalBackgroundQuerySequence;
}

bool terminal_background_probe_environment_allows_query(std::optional<std::string_view> tmux, std::optional<std::string_view> term)
{
  if (tmux && !tmux->empty())
    return false;
  if (term && term->starts_with("tmux"))
    return false;
  return true;
}

bool write_terminal_background_query(FILE* out)
{
  if (out == nullptr)
    return false;

  auto const query = terminal_background_query_sequence();
  if (std::fwrite(query.data(), 1, query.size(), out) != query.size())
    return false;
  return std::fflush(out) == 0;
}

bool emit_terminal_background_query_if_environment_allows(std::optional<std::string_view> tmux, std::optional<std::string_view> term, FILE* out)
{
  if (!terminal_background_probe_environment_allows_query(tmux, term))
    return false;
  return write_terminal_background_query(out);
}

std::optional<TerminalBackgroundColor> terminal_osc11_background_response(std::string_view sequence)
{
  auto const payload = osc11_payload(sequence);
  if (!payload)
    return std::nullopt;

  if (auto color = parse_rgb_slash_color(*payload, false))
    return color;
  if (auto color = parse_rgb_slash_color(*payload, true))
    return color;
  return parse_hash_color(*payload);
}

void arm_terminal_background_response_handler()
{
  g_terminal_background_response_handler_armed = true;
}

void disarm_terminal_background_response_handler()
{
  g_terminal_background_response_handler_armed = false;
}

bool terminal_background_response_handler_armed()
{
  return g_terminal_background_response_handler_armed;
}

bool terminal_background_response_handle(std::string_view sequence)
{
  if (!g_terminal_background_response_handler_armed)
    return false;

  auto const color = terminal_osc11_background_response(sequence);
  if (!color)
    return false;

  auto const appearance = rgb_luminance(*color) >= 180 ? TerminalBackgroundAppearance::Light : TerminalBackgroundAppearance::Dark;
  set_detected_terminal_background_appearance(appearance);
  return true;
}

InputEvent terminal_ncurses_mouse_event(std::uint64_t button_state, std::size_t column, std::size_t row)
{
#ifdef NCURSES_MOUSE_VERSION
  auto const matches = [button_state](mmask_t mask) { return (static_cast<mmask_t>(button_state) & mask) != 0; };
#ifdef BUTTON_SHIFT
  auto const shift = matches(BUTTON_SHIFT);
#else
  auto const shift = false;
#endif
#ifdef BUTTON5_PRESSED
  auto const wheel_down = matches(BUTTON5_PRESSED);
#else
  auto const wheel_down = false;
#endif
  return normalized_left_mouse_event(shift, matches(BUTTON4_PRESSED), wheel_down, matches(BUTTON1_PRESSED), matches(BUTTON1_RELEASED),
                                     matches(REPORT_MOUSE_POSITION), matches(BUTTON1_CLICKED), column, row);
#else
  static_cast<void>(button_state);
  static_cast<void>(column);
  static_cast<void>(row);
  return key_event(Key::Unknown);
#endif
}

void terminal_reset_mouse_tracking() noexcept
{
  g_left_mouse_down = false;
  reset_ncurses_left_mouse_button_state();
}

InputEvent terminal_escape_sequence_event(std::string_view sequence)
{
  if (auto event = sgr_mouse_event(sequence))
    return *event;
  if (auto event = legacy_mouse_event(sequence))
    return *event;
  auto const key = terminal_escape_sequence_key(sequence);
  if (key != Key::Unknown)
    return key_event(key);
  if (auto event = kitty_csi_u_printable_event(sequence))
    return *event;
  if (auto event = modify_other_keys_printable_event(sequence))
    return *event;
  return key_event(Key::Unknown);
}

Key terminal_escape_sequence_key(std::string_view sequence)
{
  if (sequence == "\x7f" || sequence == "\b")
    return Key::AltBackspace;
  if (sequence == "\x1d")
    return Key::CtrlAltRightBracket;
  if (sequence == "\x1f" || is_ctrl_minus_csi_u(sequence))
    return Key::CtrlMinus;
  if (sequence == std::string_view("\x00", 1))
    return Key::CtrlSpace;
  if (sequence == "\r" || sequence == "\n")
    return Key::AltEnter;
  if (sequence == "[Z")
    return Key::ShiftTab;
  if (sequence == "[1;6P" || is_ctrl_shift_p_csi_u(sequence))
    return Key::CtrlShiftP;
  if (sequence == "[3$")
    return Key::ShiftDelete;
  if (is_alt_delete_sequence(sequence))
    return Key::AltDelete;
  if (sequence == "b" || sequence == "B")
    return Key::AltB;
  if (sequence == "d" || sequence == "D")
    return Key::AltD;
  if (sequence == "f" || sequence == "F")
    return Key::AltF;
  if (sequence == "h" || sequence == "H")
    return Key::AltH;
  if (sequence == "j" || sequence == "J")
    return Key::AltJ;
  if (sequence == "k" || sequence == "K")
    return Key::AltK;
  if (sequence == "l" || sequence == "L")
    return Key::AltL;
  if (sequence == "w" || sequence == "W")
    return Key::AltW;
  if (sequence == "y" || sequence == "Y")
    return Key::AltY;
  if (auto const key = csi_cursor_key(sequence); key != Key::Unknown)
    return key;
  if (sequence == "[d")
    return Key::ShiftArrowLeft;
  if (sequence == "[c")
    return Key::ShiftArrowRight;
  if (sequence == "[a")
    return Key::ShiftArrowUp;
  if (sequence == "[b")
    return Key::ShiftArrowDown;
  if (auto const key = csi_home_end_key(sequence); key != Key::Unknown)
    return key;
  if (sequence == "[7$")
    return Key::ShiftHome;
  if (sequence == "[8$")
    return Key::ShiftEnd;
  if (is_legacy_shift_enter_sequence(sequence) || is_shift_enter_csi_u(sequence))
    return Key::ShiftEnter;
  if (is_legacy_ctrl_enter_sequence(sequence) || is_ctrl_enter_csi_u(sequence))
    return Key::CtrlEnter;
  if (is_legacy_alt_enter_sequence(sequence) || is_alt_enter_csi_u(sequence))
    return Key::AltEnter;
  if (auto const key = function_key_from_sequence(sequence); key != Key::Unknown)
    return key;
  if (auto const key = key_from_kitty_csi_u_sequence(sequence); key != Key::Unknown)
    return key;
  if (auto const key = key_from_modify_other_keys_sequence(sequence); key != Key::Unknown)
    return key;
  if (auto const key = page_key_from_csi_tilde(sequence); key != Key::Unknown)
    return key;
  if (auto const key = sgr_mouse_key(sequence); key != Key::Unknown)
    return key;
  if (auto const key = legacy_mouse_key(sequence); key != Key::Unknown)
    return key;
  return Key::Unknown;
}

bool terminal_escape_sequence_complete(std::string_view sequence)
{
  if (sequence.empty())
    return false;
  if (is_control_string_complete(sequence))
    return true;
  if (is_control_string_intro(sequence.front()))
    return false;

  if (sequence.starts_with('['))
  {
    if (sequence.size() <= 1)
      return false;
    if (sequence.starts_with("[M"))
      return is_legacy_mouse_sequence(sequence);
    if (sequence == "[3$" || sequence == "[7$" || sequence == "[8$")
      return true;
    return is_csi_final_byte(static_cast<unsigned char>(sequence.back()));
  }

  if (sequence.starts_with('O'))
  {
    if (sequence.size() <= 1)
      return false;
    return is_csi_final_byte(static_cast<unsigned char>(sequence.back()));
  }

  if (sequence.starts_with('#'))
  {
    if (sequence.size() <= 1)
      return false;
    return is_csi_final_byte(static_cast<unsigned char>(sequence.back()));
  }

  if (sequence.size() == 1)
  {
    auto const byte = static_cast<unsigned char>(sequence.front());
    if (byte == 0x00U || byte == 0x08U || byte == 0x0AU || byte == 0x0DU || byte == 0x1DU || byte == 0x1FU || byte == 0x7FU)
      return true;
    return byte >= 0x30U && byte <= 0x7EU;
  }

  return is_csi_final_byte(static_cast<unsigned char>(sequence.back()));
}

bool terminal_escape_sequence_should_discard(std::string_view sequence)
{
  if (!terminal_escape_sequence_complete(sequence))
    return false;
  if (sequence == "[200~")
    return false;
  return terminal_escape_sequence_key(sequence) == Key::Unknown;
}

bool terminal_is_tty()
{
  return isatty(STDIN_FILENO) != 0 && isatty(STDOUT_FILENO) != 0;
}

}  // namespace ava::tui

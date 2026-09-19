#include "sys.h"
#include "support/terminal_test_support.h"
#include "support/test_harness.h"
#include "terminal/ColorPalette.h"
#include "terminal/Context.h"

#include <array>
#include <cstdio>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace terminal = ava::tui::terminal;

namespace ava::tui::terminal {

// Expose ColorPalette's pure OSC 4 seams to focused tests without widening the production API.
struct ColorPaletteTestAccess
{
  static std::string query(int color_index) { return ColorPalette::osc4_query(color_index); }
  static std::string queries(int first_color_index, int number_of_colors) { return ColorPalette::osc4_queries(first_color_index, number_of_colors); }
  static std::string set_color(int color_index, Color color) { return ColorPalette::osc4_set_color(color_index, color); }
  static std::optional<std::pair<int, Color>> parse_response(std::string_view response) { return ColorPalette::parse_osc4_response(response); }
  static std::optional<Color> probe(Context& context, int color_index) { return ColorPalette::probe_color(context, color_index); }
  static std::vector<Color> probe(Context& context, int first_color_index, int number_of_colors)
  {
    return ColorPalette::probe_colors(context, first_color_index, number_of_colors);
  }
};

} // namespace ava::tui::terminal

namespace {

// Read all bytes emitted to `file`, rewinding it first and leaving it at end-of-file.
std::string read_all(FILE* file);

// Count non-overlapping occurrences of `needle` in `haystack`, returning zero for an empty needle.
std::size_t count_occurrences(std::string_view haystack, std::string_view needle);

// Verify known CIELAB endpoints and exact 8-bit sRGB round trips through the forward and inverse conversions.
void test_srgb_cielab_round_trip()
{
  expect(terminal::ColorPalette::lab_to_rgb({0.0, 0.0, 0.0}).as_int() == 0x000000, "CIELAB black must convert to sRGB black");
  expect(terminal::ColorPalette::lab_to_rgb({100.0, 0.0, 0.0}).as_int() == 0xffffff, "CIELAB D65 white must convert to sRGB white");
  expect(terminal::ColorPalette::lab_to_rgb({53.2371155954293, 80.0882453236802, 67.1996262211358}).as_int() == 0xff0000,
         "the reference CIELAB red must convert to sRGB red");

  constexpr std::array<int, 11> colors = {0x000000, 0xffffff, 0xff0000, 0x00ff00, 0x0000ff, 0x808080, 0x301838, 0xa8e050, 0x102850, 0xe8c8f0, 0x50d8a8};
  for (int const packed_rgb : colors)
  {
    terminal::Color const original{static_cast<uint32_t>(packed_rgb)};
    terminal::Color const round_trip = terminal::ColorPalette::lab_to_rgb(terminal::ColorPalette::rgb_to_lab(original));
    expect(round_trip.as_int() == original.as_int(),
           "sRGB color " + std::to_string(packed_rgb) + " must survive an RGB-to-CIELAB-to-RGB round trip, got " + std::to_string(round_trip.as_int()));
  }
}

// Verify exact OSC 4 query framing and strict parsing of live palette replies with common channel widths and terminators.
void test_osc4_palette_protocol()
{
  using Access = terminal::ColorPaletteTestAccess;
  expect(Access::query(224) == std::string("\x1b]4;224;?\x1b\\"),
         "an indexed OSC 4 palette query must use the passed index, the OSC introducer, and an ST terminator");
  expect(Access::queries(224, 3) == std::string("\x1b]4;224;?\x1b\\\x1b]4;225;?\x1b\\\x1b]4;226;?\x1b\\"),
         "a batched OSC 4 palette query must emit consecutive independently terminated indices in one string");
  expect(Access::set_color(7, terminal::Color{0x12abef}) == std::string("\x1b]4;7;rgb:12/ab/ef\x1b\\"),
         "an indexed OSC 4 palette assignment must encode each sRGB channel and terminate with ST");

  auto const red = Access::parse_response("\x1b]4;7;rgb:ffff/0000/0000\x1b\\");
  expect(red && red->first == 7 && red->second.as_int() == 0xff0000, "OSC 4 must parse a four-digit red response terminated by ST");
  auto const green = Access::parse_response("\x1b]4;3;rgb:00/ff/00\a");
  expect(green && green->first == 3 && green->second.as_int() == 0x00ff00, "OSC 4 must parse a two-digit green response terminated by BEL");
  auto const truncated = Access::parse_response("\x1b]4;4;rgb:00ff/0100/01ff\a");
  expect(truncated && truncated->second.as_int() == 0x000101,
         "four-digit OSC 4 channels must discard the low byte so each consecutive 256-value range maps to one sRGB byte");
  auto const mixed_widths = Access::parse_response("\x1b]4;5;rgb:f/abc/12\a");
  expect(mixed_widths && mixed_widths->second.as_int() == 0xffab12,
         "short OSC 4 channels must expand one hex digit and retain the most significant two digits of longer values");
  expect(!Access::parse_response("\x1b[4;3;rgb:00/ff/00\x1b\\"), "a CSI sequence must not be accepted as an OSC 4 palette response");

  expect(!Access::parse_response("\x1b]4;0;rgb:ffff/0000/0000"), "an unterminated OSC 4 response must fail instead of returning a partial color");
}

// Verify mutable-prefix detection, exact-color reuse, programming of free entries, and destruction-time restoration.
void test_mutable_palette_probe()
{
  ScopedTmpFile input;
  ScopedTmpFile output;

  // This will be read by the terminal::Context constructor.
  write_OSC4_reply(input.get(), 16);
  write_KeyboardInputMode_reply(input.get(), SupportedMode::KittyProtocol);

  // Prepare mock replies for this tests probes.
  std::string replies;
  for (int index = 0; index != 16; ++index)
    replies += "\x1b]4;" + std::to_string(index) + ";rgb:10/20/30\x1b\\";
  replies += "\x1b]4;7;rgb:ef/df/cf\x1b\\";  // The complemented test color was accepted.
  replies += "\x1b]4;15;rgb:10/20/30\x1b\\"; // The second assignment was ignored.
  expect(std::fwrite(replies.data(), 1, replies.size(), input.get()) == replies.size(), "all simulated OSC 4 replies must be written");
  std::rewind(input.get());

  {
    ScopedEnvVar term_guard("TERM", "xterm-16color");
    terminal::Context terminal_context(output.get(), input.get());
    std::unique_ptr<terminal::ColorPalette> const palette = terminal::ColorPalette::create(terminal_context);
    expect(palette != nullptr, "a complete OSC 4 exchange must create a live ColorPalette");
    if (palette)
    {
      expect(palette->last_mutable_palette_index() == 8, "the largest successful power-of-two mutable prefix must be retained");
      expect(palette->nearest_indexed_color(terminal::Color{0x102030}) == 0, "an exact live palette match must be reused without programming an entry");
      expect(palette->nearest_indexed_color(terminal::Color{0x123456}) == 7, "the highest free mutable entry must be programmed for a new color");
      expect(palette->nearest_indexed_color(terminal::Color{0x123456}) == 7, "a programmed exact match must be reused without consuming another entry");
      expect(palette->nearest_indexed_color(terminal::Color{0x654321}) == 6, "later colors must not overwrite mutable indices already returned to callers");
      constexpr std::array<int, 5> additional_colors = {0x204060, 0x305070, 0x406080, 0x507090, 0x6080a0};
      for (std::size_t offset = 0; offset != additional_colors.size(); ++offset)
      {
        int const index = palette->nearest_indexed_color(terminal::Color{static_cast<std::uint32_t>(additional_colors[offset])});
        expect(index == 5 - static_cast<int>(offset), "each remaining mutable entry must be allocated once from high to low");
      }
      expect(palette->nearest_indexed_color(terminal::Color{0xabcdef}) >= 0, "an exhausted mutable palette must fall back to a current nearest entry");
    }
  }

  std::fflush(output.get());
  std::string const emitted = read_all(output.get());
  expect(emitted.find("\x1b]4;7;rgb:ef/df/cf\x1b\\\x1b]4;7;?\x1b\\\x1b]4;7;rgb:10/20/30\x1b\\") != std::string::npos,
         "a successful mutability probe must assign, verify, and restore the boundary color");
  expect(emitted.find("\x1b]4;15;rgb:ef/df/cf\x1b\\\x1b]4;15;?\x1b\\\x1b]4;15;rgb:10/20/30\x1b\\") != std::string::npos,
         "a rejected mutability probe must still restore the boundary color defensively");
  expect(count_occurrences(emitted, "\x1b]4;7;rgb:12/34/56\x1b\\") == 1, "reusing a programmed exact color must not emit another palette assignment");
  expect(emitted.find("\x1b]4;6;rgb:65/43/21\x1b\\") != std::string::npos, "a second distinct color must use another mutable entry");
  expect(count_occurrences(emitted, "\x1b]4;7;rgb:10/20/30\x1b\\") == 2,
         "the boundary entry must be restored once after probing and once after its application-time assignment");
  expect(count_occurrences(emitted, "\x1b]4;6;rgb:10/20/30\x1b\\") == 1, "destruction must restore every other application-programmed entry");
  expect(emitted.find("\x1b]4;0;rgb:") == std::string::npos, "an exact mutable entry already returned to a caller must never be overwritten");
  expect(emitted.find("rgb:ab/cd/ef") == std::string::npos, "palette exhaustion must use a nearest entry instead of overwriting a reserved one");
}

// Read all bytes emitted to `file`, rewinding it first and leaving it at end-of-file.
//
// The caller must flush any writers before calling this helper. Embedded terminal control bytes are retained.
std::string read_all(FILE* file)
{
  std::rewind(file);
  std::string result;
  std::array<char, 1024> buffer;
  while (std::size_t const count = std::fread(buffer.data(), 1, buffer.size(), file))
    result.append(buffer.data(), count);
  return result;
}

// Count non-overlapping occurrences without allocating copies of either input.
std::size_t count_occurrences(std::string_view haystack, std::string_view needle)
{
  if (needle.empty())
    return 0;

  std::size_t count = 0;
  for (std::size_t position = 0; (position = haystack.find(needle, position)) != std::string_view::npos; position += needle.size())
    ++count;
  return count;
}

// Verify that xterm-compatible indexed colors use the standardized color cube instead of ncurses' synthetic palette report.
//
// Runs ncurses against temporary files with TERM=xterm-256color and inspects the SGR sequences emitted for a rendered cell. The
// requested green and blue must map to their nearby cube colors rather than basic yellow and black or unmodified grayscale slots.
void test_xterm_indexed_colors_use_standard_palette()
{
  ScopedTmpFile input;
  ScopedTmpFile output;

  // This will be read by the terminal::Context constructor.
  write_OSC4_reply(input.get(), 256);
  write_KeyboardInputMode_reply(input.get(), SupportedMode::KittyProtocol);
  std::rewind(input.get());

  {
    ScopedEnvVar term_guard("TERM", "xterm-256color");
    terminal::Context terminal_context(output.get(), input.get());
    expect(!terminal::ColorPaletteTestAccess::probe(terminal_context, 224), "a single-index probe with no OSC 4 reply must return no Color");
    expect(terminal::ColorPaletteTestAccess::probe(terminal_context, 224, 3).empty(), "a batched probe with no OSC 4 replies must return no colors");
    expect(!terminal::ColorPalette::create(terminal_context), "an input stream with no OSC 4 replies must produce no live ColorPalette");
    terminal::ColorPair const pair = terminal_context.create_color_pair({0xa8e050}, {0x102850});
    terminal::BasicWindow window({2, 2}, {0, 0});
    window.attr_set(terminal::Rendition{pair});
    window.addstr("X");
    window.refresh();
  }

  std::fflush(output.get());
  std::string const emitted = read_all(output.get());
  expect(emitted.find("\x1b]4;224;?\x1b\\") != std::string::npos, "the single-index probe must emit an OSC 4 query for its passed index");
  expect(emitted.find("38;5;149") != std::string::npos, "xterm-256color must map green 0xa8e050 to nearby green cube entry 149");
  expect(emitted.find("48;5;17") != std::string::npos, "xterm-256color must map blue 0x102850 to nearby blue cube entry 17");
  expect(emitted.find("rgb:a8/e0/50") == std::string::npos && emitted.find("rgb:10/28/50") == std::string::npos,
         "indexed-color rendering must not reprogram a fixed palette with either requested application color");
}

} // namespace

void run_terminal_color_tests()
{
  test_srgb_cielab_round_trip();
  test_osc4_palette_protocol();
  test_mutable_palette_probe();
  test_xterm_indexed_colors_use_standard_palette();
}

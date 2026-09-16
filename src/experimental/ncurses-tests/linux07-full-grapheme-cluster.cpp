#include "sys.h"
#include "Application.h"
#include "terminal/Attributes.h"
#include "terminal/ComplexChar.h"
#include "terminal/GraphemeCluster.h"
#include "terminal/Context.h"
#include "terminal/BasicWindow.h"

#include <array>
#include <cstdlib>
#include <iostream>

namespace terminal = ava::tui::terminal;

namespace {

using Storage = terminal::GraphemeCluster::Storage;

// Report a deterministic failure message for this standalone executable test.
// Returning false lets main keep the assertions compact while still ending the
// ncurses session normally if a terminal-backed round trip fails.
bool require(bool condition, char const* message)
{
  if (!condition)
    std::cerr << "linux07-full-grapheme-cluster: " << message << '\n';
  return condition;
}

// Compare the complete fixed-capacity grapheme storage, including the full-cluster
// edge case where no in-place L'\0' terminator exists.
bool same_storage(terminal::GraphemeCluster const& grapheme_cluster, Storage const& expected)
{
  return grapheme_cluster.storage() == expected;
}

} // namespace

int main()
{
  Application application("linux07-full-grapheme-cluster");
  terminal::Context& terminal_context = application.terminal_context();
  terminal_context.initialize();

  Storage const expected_full_cluster = {L'a', L'\u0301', L'\u0302', L'\u0303', L'\u0308'};

  terminal::GraphemeCluster const full_cluster{L"a\u0301\u0302\u0303\u0308"};
  if (!require(full_cluster.length() == terminal::GraphemeCluster::capacity, "full cluster should use every storage slot") ||
      !require(!full_cluster.is_zero_terminated(), "full cluster should not have an in-place terminator") ||
      !require(same_storage(full_cluster, expected_full_cluster), "full cluster storage should match the input grapheme"))
    return EXIT_FAILURE;

  terminal::ComplexChar const source{full_cluster, terminal::Attributes{terminal::Attribute::bold}};
  if (!require(same_storage(source.cell_character(), expected_full_cluster), "ComplexChar should preserve a full GraphemeCluster"))
    return EXIT_FAILURE;

  bool round_trip_ok = false;
  {
    // Exercise the public conversion path: BasicWindow::set_background converts the
    // ComplexChar to ncurses cchar_t with setcchar, and get_background converts
    // it back with getcchar.  This specifically guards the CCHARW_MAX edge case
    // where setcchar needs a temporary terminator and getcchar writes one extra
    // wchar_t beyond ncurses' fixed cchar_t payload.
    terminal::ColorPair const green_colorpair = terminal_context.create_color_pair({}, {0x008800});
    terminal_context.stdscr().set_background({green_colorpair});
    terminal::Dimension screen_size = terminal_context.size();
    terminal::Position offset{screen_size.height() / 4, screen_size.width() / 4};
    terminal::BasicWindow window{screen_size / 2, offset};
    window.set_background(source);
    terminal::Margin margin{.top = 1, .bottom = 1, .left = 1, .right = 1};
    window.set_border({margin, green_colorpair});
    terminal::ComplexChar const round_trip = window.get_background();
    round_trip_ok =
        require(same_storage(round_trip.cell_character(), expected_full_cluster), "BasicWindow background round-trip should preserve full cluster storage") &&
        require(round_trip.cell_character().length() == terminal::GraphemeCluster::capacity,
                "round-tripped full cluster should still use every storage slot") &&
        require(!round_trip.cell_character().is_zero_terminated(), "round-tripped full cluster should still be unterminated in-place") &&
        require((round_trip.rendition().attributes().mask() & static_cast<terminal::Attributes::attr_t>(terminal::Attribute::bold)) != 0,
                "BasicWindow background round-trip should preserve bold attribute");

    window.move({1, 1});
    terminal_context.stdscr().refresh();
    window.refresh();
    terminal_context.get_wch();
  }

  if (!round_trip_ok)
    return EXIT_FAILURE;

  std::cout << "Success\n";

  return EXIT_SUCCESS;
}

#include "terminal/Context.h"
#include "terminal/Window.h"

namespace terminal = ava::tui::terminal;

/// Display a four-row, terminal-wide input area over a terminal painted dark gray with its default foreground.
///
/// The input area applies its own backgrounds, draws a cyan left rail, refreshes its owned window after stdscr, and blocks for one key.
/// The terminal must be at least five rows and six columns so the positioned window and its margin leave a writable interior.
int main()
{
  terminal::Context terminal_context;
  terminal::Rendition const terminal_background(terminal_context.create_color_pair({}, {0x0a0a0a}));
  terminal_context.stdscr().set_background(terminal_background);
  terminal_context.stdscr().refresh();

  terminal::Rendition const normal_rendition(terminal_context.create_color_pair({0xeeeeee}, {0x1e1e1e}));
  terminal::ColorPair const border_colorpair = terminal_context.create_color_pair({0x1e1e1e}, {0x0a0a0a});
  terminal::ColorPair const border_lhs_colorpair = terminal_context.create_color_pair({0x56b6c2}, {0x0a0a0a});
  terminal::Box const left_border{L"┃██"
                                  L"┃ █"
                                  L"╹▀▀"};
  terminal::Dimension const size{5, terminal_context.cols() - 4};
  terminal::Position const top_left{terminal_context.rows() - size.height() - 2, 2};
  terminal::Window input_area(size, top_left,
                              {terminal::Margin{.top = 1, .bottom = 2, .left = 3, .right = 2}, border_colorpair, border_lhs_colorpair, left_border});
  input_area.outer_window().set_background(normal_rendition);
  input_area.set_background(normal_rendition);
  input_area.draw_border();
  input_area.refresh();

  [[maybe_unused]] int const key = terminal_context.get_wch();
}

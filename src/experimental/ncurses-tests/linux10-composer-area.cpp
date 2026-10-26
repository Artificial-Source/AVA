#include "sys.h"
#include "Application.h"
#include "ComposerArea.h"
#include "lorem_ipsum_paragraphs.h"
#include "terminal/Context.h"
#include "terminal/WindowPad.h"

namespace terminal = ava::tui::terminal;

/// Display a four-row, terminal-wide composer area over a terminal painted dark gray with its default foreground.
///
/// The composer area applies its own backgrounds, draws a cyan left rail, refreshes its owned window after stdscr, and blocks for one key.
/// The terminal must be at least five rows and six columns so the positioned window and its margin leave a writable interior.
int main()
{
  Application application("linux10-composer-area");
  application.initialize_terminal_context();

  constexpr terminal::Color tui_background = 0x0a0a0a;
  constexpr terminal::Color composer_foreground = 0xeeeeee;
  constexpr terminal::Color composer_background = 0x1e1e1e;
  constexpr terminal::Color info_color = 0x56b6c2;              // This is used for the accent bar below.

  terminal::Context& terminal_context = application.terminal_context();
  terminal_context.apply_cursor_settings({terminal::CursorStyle::Block, true});
  terminal_context.stdscr().set_background(terminal_context.create_color_pair({}, tui_background));

  // The colors used for the left-most column of the border.
  terminal::ColorPair const accent_bar_color_pair = terminal_context.create_color_pair(info_color, tui_background);

  constexpr terminal::Margin const screen_margin{.top = 0, .bottom = 2, .left = 2, .right = 2};
  terminal::columns_t const composer_area_width = terminal_context.cols() - screen_margin.width();
  terminal::Position const bottom_left{terminal_context.rows() - screen_margin.bottom, screen_margin.left};

  ava::tui::ComposerArea composer_area(composer_area_width, bottom_left, composer_foreground, composer_background, tui_background);
  composer_area.set_accent_bar_color_pair(accent_bar_color_pair);

  constexpr int total_paragraph_count = 2;
  for (int paragraph_number = 0; paragraph_number < total_paragraph_count; ++paragraph_number)
    composer_area.append(make_lorem_ipsum_paragraph(terminal_context, paragraph_number));

  // Generate the graphemes content of the pad, determine the required window height and create the window.
  composer_area.generate(true, true);

  // A test string for the bottom of the margin area.
  std::string const agent_label = "Coder";
  std::string const agent_model_separator = " · ";
  std::string const model_label = "GPT-6.1 Sol";
  std::string const provider_label = "OpenAI";

  // Write the background color to the virtual screen (erase it).
  terminal_context.stdscr().wnoutrefresh();
  // Show the contents of `pad` in the pad viewport of `composer_area`.
  composer_area.pnoutrefresh(terminal::ScrollPosition::end);

  // Publish the virtual screen to the physical screen and wait for a key press.
  terminal_context.doupdate();
  terminal_context.get_wch();

  // Change the accent bar color.
  constexpr terminal::Color color = 0x880000;
  terminal::ColorPair const accent_bar_color_pair2 = terminal_context.create_color_pair(color, tui_background);
  composer_area.set_accent_bar_color_pair(accent_bar_color_pair2);
  composer_area.pnoutrefresh(terminal::ScrollPosition::end);

  // Publish the virtual screen to the physical screen and wait for a key press.
  terminal_context.doupdate();
  terminal_context.get_wch();
}

#include "sys.h"
#include "lorem_ipsum_paragraphs.h"
#include "terminal/Context.h"
#include "terminal/Window.h"
#include "terminal/Pad.h"

class Application : public ava::core::Application
{
 public:
  Application() : ava::core::Application(true) { }
  [[nodiscard]] std::string_view application_name() const noexcept override { return "linux10-composer-area"; }

  // Can't print ava::core::Application.
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

namespace terminal = ava::tui::terminal;

/// Display a four-row, terminal-wide composer area over a terminal painted dark gray with its default foreground.
///
/// The composer area applies its own backgrounds, draws a cyan left rail, refreshes its owned window after stdscr, and blocks for one key.
/// The terminal must be at least five rows and six columns so the positioned window and its margin leave a writable interior.
int main()
{
  [[maybe_unused]] Application application;

  terminal::Context terminal_context;
  terminal::Rendition const terminal_background(terminal_context.create_color_pair({}, {0x0a0a0a}));
  terminal_context.stdscr().set_background(terminal_background);

  terminal::Rendition const normal_rendition(terminal_context.create_color_pair({0xeeeeee}, {0x1e1e1e}));
  terminal::ColorPair const border_colorpair = terminal_context.create_color_pair({0x1e1e1e}, {0x0a0a0a});
  terminal::ColorPair const border_lhs_colorpair = terminal_context.create_color_pair({0x56b6c2}, {0x0a0a0a});
  terminal::Box const left_border{L"┃██"
                                  L"┃ █"
                                  L"╹▀▀"};
  uint32_t const composer_area_height = 5;
  uint32_t const composer_area_width = terminal_context.cols() - 4;
  terminal::Dimension const size{composer_area_height, composer_area_width};
  terminal::Position const top_left{terminal_context.rows() - size.height() - 2, 2};
  terminal::Window composer_area(size, top_left,
                              {terminal::Margin{.top = 1, .bottom = 2, .left = 3, .right = 2}, border_colorpair, border_lhs_colorpair, left_border});
  composer_area.outer_window().set_background(normal_rendition);
  composer_area.set_background(normal_rendition);
  composer_area.draw_border();

  terminal::Pad pad;
  int const total_paragraph_count = 2;
  for (int paragraph_number = 0; paragraph_number < total_paragraph_count; ++paragraph_number)
    pad.append(make_lorem_ipsum_paragraph(terminal_context, paragraph_number));

  // Generate the content of the pad for a window with the given width.
  pad.generate(composer_area_width);

  composer_area.move({0, 0});
  terminal_context.stdscr().refresh();
  composer_area.refresh();
  terminal_context.get_wch();
}

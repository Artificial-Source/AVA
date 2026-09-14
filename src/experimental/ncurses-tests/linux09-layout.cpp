#include "sys.h"
#include "Application.h"
#include "terminal/Context.h"
#include "terminal/HorizontalLayout.h"
#include "terminal/Pad.h"
#include "terminal/Paragraph.h"
#include "terminal/Spacer.h"

#include "debug.h"

namespace terminal = ava::tui::terminal;

struct TestPad : public terminal::Pad
{
  void generate_grapheme_surface(terminal::columns_t columns, bool blank_line_between_block_rows)
  {
    terminal::Pad::generate_grapheme_surface(columns, blank_line_between_block_rows);
  }
};

int main()
{
  Application application("linux09-layout");

  terminal::Context terminal_context;
  terminal::HorizontalLayout horizontal_layout1;

  {
    terminal::Rendition const rendition(terminal_context.create_color_pair(0xeeddcc, 0x335500));
    auto exit_text = terminal::TextSpan::create(u8"Exit the app", rendition, {.priority = 1});
    auto spacer = terminal::Spacer::create();
    terminal::Rendition const rendition2(terminal_context.create_color_pair(0xeeddcc, 0x006655));
    auto shortcuts = terminal::Paragraph::create(rendition2, {.priority = 2, .alignment = terminal::HorizontalAlignment::right});
    auto shortcuts_text = terminal::TextSpan::create(u8"ctrl+c, ctrl+d, ctrl+x q");
    shortcuts->append(std::move(shortcuts_text));
    shortcuts->initialize_cached_natural_width();

    horizontal_layout1.append(std::move(exit_text));
    horizontal_layout1.append(std::move(spacer));
    horizontal_layout1.append(std::move(shortcuts));

    horizontal_layout1.set_width(17);
    Dout(dc::notice, "horizontal_layout1 = " << horizontal_layout1);
  }

  TestPad pad;
  pad.append(std::move(horizontal_layout1));
  pad.generate_grapheme_surface(47, false);
  pad.generate();
  pad.prefresh({0, 0}, {5, 5}, pad.dimension());

  //... allow resizing with keyboard

  [[maybe_unused]] int wch = terminal_context.get_wch();
}

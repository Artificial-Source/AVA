#include "sys.h"
#include "Application.h"
#include "lorem_ipsum_paragraphs.h"
#include "terminal/Context.h"
#include "terminal/Pad.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace terminal = ava::tui::terminal;

constexpr uint32_t pad_line_width = 120;
constexpr int paragraph_repetitions = 20;

int main()
{
  Application application("linux08-pad");

  namespace terminal = ava::tui::terminal;
  terminal::Context terminal_context;
  terminal::BasicWindow const& stdscr = terminal_context.stdscr();

  // Fill the Pad's with the lorem ipsum paragraphs, cycling through them (and through the
  // paragraph default renditions) to get enough content to scroll through.
  std::array<terminal::Pad, 3> pads;
  int const total_paragraph_count = paragraph_repetitions * static_cast<int>(lorem_ipsum_paragraphs.size());
  for (int paragraph_number = 0; paragraph_number < total_paragraph_count; ++paragraph_number)
    for (int p = 0; p < pads.size(); ++p)
      pads[p].append(make_lorem_ipsum_paragraph(terminal_context, paragraph_number));

  // Wrap the content at pad_line_width cells and create the ncurses pad from it.
  for (int p = 0; p < pads.size(); ++p)
    pads[p].generate(pad_line_width);

  uint32_t const pad_view_height = 17;
  terminal::Position const top_left_first_pad_view{1, 5};
  std::array<terminal::Position, 3> pad_view_pos;
  for (int p = 0; p < pads.size(); ++p)
  {
    terminal::Margin const pad_view_offset{static_cast<uint8_t>(p * pad_view_height), 0};
    pad_view_pos[p] = top_left_first_pad_view + pad_view_offset;
  }
  terminal::Dimension const view_size{pad_view_height, std::min(pad_line_width, stdscr.getmaxyx().width() - top_left_first_pad_view.col())};

  int const max_first_row = static_cast<int>(pads[0].dimension().height() - pads.size() * view_size.height());
  int first_row = 0;
  for (;;)
  {
    for (int p = 0; p < pads.size(); ++p)
      pads[p].prefresh({static_cast<uint32_t>(first_row + p * pad_view_height), 0}, pad_view_pos[p], view_size);

    bool saw_esc = false;
    bool saw_bracket_open = false;
    int count = 0;
    for (;;)
    {
      wint_t key;
      // Input is screen-global, but reading it through stdscr can implicitly refresh
      // stdscr over the pad. Reading through the pad retains ncurses key decoding;
      // pads are deliberately not refreshed as a side effect of input operations.
      pads[0].basic_window().get_wch(key);

      if (key == 'q')
        return EXIT_SUCCESS;

      bool saw_scroll_up, saw_scroll_down;
      if (saw_esc && key == '[')
        saw_bracket_open = true;
      else
      {
        saw_scroll_up = false;
        saw_scroll_down = false;
        if (saw_bracket_open && (key == 'A' || key == 'B'))
        {
          saw_scroll_up = key == 'A';
          saw_scroll_down = key == 'B';
        }
        saw_bracket_open = false;
      }

      if (saw_scroll_up || saw_scroll_down)
      {
        if ((++count % 5) == 0)
        {
          first_row = std::clamp(first_row + (saw_scroll_down ? 1 : -1), 0, max_first_row);
          break;
        }
      }

      saw_esc = key == 27;
    }
  }
}

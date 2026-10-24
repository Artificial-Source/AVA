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

struct TestPad : public terminal::Pad
{
  void generate_grapheme_surface(terminal::columns_t columns, bool blank_line_between_block_rows)
  {
    terminal::Pad::generate_grapheme_surface(columns, blank_line_between_block_rows);
  }
};

class SGRDecoder
{
 private:
  std::array<char, 16> buf_;
  int index_ = 0;

 public:
  enum Result {
    decoding, discarded, WheelUp, WheelDown
  };

  Result feed(wint_t key)
  {
    if (index_ == 0)
    {
      if (key != 27)
        return discarded;
    }
    else if (key == 77)
    {
      if (index_ >= 5)
      {
        index_ = 0;
        if (buf_[4] == 52)
          return WheelUp;
        if (buf_[4] == 53)
          return WheelDown;
      }
      index_ = 0;
      return discarded;
    }
    // Store key.
    buf_[index_++] = key;
    // Overflow protection.
    if (index_ == buf_.size())
      return discard();
    // Still incomplete.
    return decoding;
  }

 private:
  Result discard()
  {
    index_ = 0;
    return discarded;
  }
};

int main()
{
  Application application("linux08-pad");
  application.initialize_terminal_context();

  namespace terminal = ava::tui::terminal;
  terminal::BasicWindow const& stdscr = application.terminal_context().stdscr();

  // Fill the Pad's with the lorem ipsum paragraphs, cycling through them (and through the
  // paragraph default renditions) to get enough content to scroll through.
  std::array<TestPad, 3> pads;
  int const total_paragraph_count = paragraph_repetitions * static_cast<int>(lorem_ipsum_paragraphs.size());
  for (int paragraph_number = 0; paragraph_number < total_paragraph_count; ++paragraph_number)
    for (int p = 0; p < pads.size(); ++p)
      pads[p].append(make_lorem_ipsum_paragraph(application.terminal_context(), paragraph_number));

  for (int p = 0; p < pads.size(); ++p)
    pads[p].generate_grapheme_surface(pad_line_width, true);

  // Wrap the content at pad_line_width cells and create the ncurses pad from it.
  for (int p = 0; p < pads.size(); ++p)
    pads[p].build();

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
  SGRDecoder decoder;
  for (;;)
  {
    for (int p = 0; p < pads.size(); ++p)
      pads[p].prefresh({static_cast<uint32_t>(first_row + p * pad_view_height), 0}, pad_view_pos[p], view_size);

    int count = 0;
    for (;;)
    {
      wint_t key;
      // Input is screen-global, but reading it through stdscr can implicitly refresh
      // stdscr over the pad. Reading through the pad retains ncurses key decoding;
      // pads are deliberately not refreshed as a side effect of input operations.
      pads[0].basic_window().get_wch(key);

      Dout(dc::notice, "key = " << key);

      if (key == 'q')
        return EXIT_SUCCESS;

      SGRDecoder::Result result = decoder.feed(key);
      if (result == SGRDecoder::WheelUp || result == SGRDecoder::WheelDown)
      {
        first_row = std::clamp(first_row + (result == SGRDecoder::WheelDown ? 1 : -1), 0, max_first_row);
        break;
      }
    }
  }
}

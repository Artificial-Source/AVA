#include "sys.h"
#include "Application.h"
#include "terminal/Attributes.h"
#include "terminal/Context.h"

#include <iostream>
#include <string>

namespace terminal = ava::tui::terminal;

void add_text(std::string& text, char const* attr_str)
{
  if (!text.empty())
    text += "|";
  text += attr_str;
}

int main()
{
  Application application("linux04-attributes");
  terminal::Context& terminal_context = application.terminal_context();
  terminal_context.initialize();

  wint_t wch;
  {
    terminal::BasicWindow& window = terminal_context.stdscr();
    window.move({10, 0});

    // A_STANDOUT, A_UNDERLINE, A_BOLD, A_BLINK
    for (int standout = 0; standout <= 1; ++standout)
      for (int underline = 0; underline <= 1; ++underline)
        for (int bold = 0; bold <= 1; ++bold)
          for (int blink = 0; blink <= 1; ++blink)
          {
            terminal::Attributes attr;
            std::string text;
            using Attribute = terminal::Attribute;
            if (standout)
            {
              attr |= Attribute::standout;
              add_text(text, "A_STANDOUT");
            }
            if (underline)
            {
              attr |= Attribute::underline;
              add_text(text, "A_UNDERLINE");
            }
            if (bold)
            {
              attr |= Attribute::bold;
              add_text(text, "A_BOLD");
            }
            if (blink)
            {
              attr |= Attribute::blink;
              add_text(text, "A_BLINK");
            }
            window.attr_set(terminal::Rendition{{}, attr});
            // A_NORMAL == 0, so it can clearly not be combined with other attributes in an OR-ed list.
            if (attr == Attribute::normal)
              text = "A_NORMAL";
            text += "\n";
            window.addstr(text.c_str());
          }

    window.refresh();
    terminal_context.get_wch();
  }
}

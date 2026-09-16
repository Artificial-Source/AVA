#include "sys.h"
#include "Application.h"
#include "terminal/Attributes.h"
#include "terminal/Context.h"

#include <iostream>
#include <string>
#include <curses.h>

void add_text(std::string& text, char const* attr_str)
{
  if (!text.empty())
    text += "|";
  text += attr_str;
}

int main()
{
  Application application("linux04-attributes");
  ava::tui::terminal::Context& terminal_context = application.terminal_context();
  terminal_context.initialize();

  wint_t wch;
  {
    move(10, 0);

    // A_STANDOUT, A_UNDERLINE, A_BOLD, A_BLINK
    for (int standout = 0; standout <= 1; ++standout)
      for (int underline = 0; underline <= 1; ++underline)
        for (int bold = 0; bold <= 1; ++bold)
          for (int blink = 0; blink <= 1; ++blink)
          {
            uint32_t attr = 0;
            std::string text;
            if (standout)
            {
              attr |= A_STANDOUT;
              add_text(text, "A_STANDOUT");
            }
            if (underline)
            {
              attr |= A_UNDERLINE;
              add_text(text, "A_UNDERLINE");
            }
            if (bold)
            {
              attr |= A_BOLD;
              add_text(text, "A_BOLD");
            }
            if (blink)
            {
              attr |= A_BLINK;
              add_text(text, "A_BLINK");
            }
            attr_set(attr, 0, nullptr);
            // A_NORMAL == 0, so it can clearly not be combined with other attributes in an OR-ed list.
            if (attr == A_NORMAL)
              text = "A_NORMAL";
            text += "\n";
            addstr(text.c_str());
          }

    refresh();
    get_wch(&wch);
  }
}

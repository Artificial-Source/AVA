#include "sys.h"
#include "terminal/Context.h"
#include "terminal/HorizontalLayout.h"
#include "terminal/Pad.h"
#include "terminal/Paragraph.h"
#include "terminal/Spacer.h"

#include "debug.h"

class Application : public ava::core::Application
{
 public:
  [[nodiscard]] std::string_view application_name() const noexcept override { return "linux09-layout"; }

  // Can't print ava::core::Application.
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

namespace terminal = ava::tui::terminal;

int main()
{
  Debug(NAMESPACE_DEBUG::init());
  Debug(libcw_do.always_flush_on());

#ifdef CWDEBUG
  std::mutex log_mutex;
  std::ofstream log("debug.out");
  Debug(libcw_do.set_ostream(&log, &log_mutex));
#endif

  [[maybe_unused]] Application application;

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

  terminal::Pad pad;
  pad.append(std::move(horizontal_layout1));
  pad.generate(17, false);
  pad.prefresh({0, 0}, {5, 5}, pad.dimension());

  //... allow resizing with keyboard

  [[maybe_unused]] int wch = terminal_context.get_wch();
}

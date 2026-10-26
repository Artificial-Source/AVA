#include "sys.h"
#include "ComposerArea.h"

namespace ava::tui {
namespace {

// Create a new color pair from a fore- and background color.
terminal::ColorPair create_color_pair(terminal::Color foreground, terminal::Color background)
{
  return core::Application::instance().terminal_context().create_color_pair(foreground, background);
}

} // namespace

ComposerAreaColorPairs::ComposerAreaColorPairs(terminal::Color foreground, terminal::Color background, terminal::Color tui_background)
    : border_color_pair_(create_color_pair(background, tui_background)), rendition_(create_color_pair(foreground, background))
{
}

void ComposerArea::set_accent_bar_color_pair(terminal::ColorPair accent_bar_color_pair)
{
  change_lhs_border_color_pair(accent_bar_color_pair);
}

ComposerArea::ComposerArea(terminal::columns_t width, terminal::Position bottom_left, terminal::Color foreground, terminal::Color background,
                           terminal::Color tui_background)
    : ComposerAreaColorPairs(foreground, background, tui_background),
      terminal::WindowPad(width, bottom_left, rendition_,
                          terminal::Border{composer_margin, border_color_pair_, create_color_pair(default_accent_bar_color, tui_background), composer_box})
{
}

} // namespace ava::tui

#pragma once

#include "terminal/Box.h"
#include "terminal/Color.h"
#include "terminal/WindowPad.h"

#include <atomic>

namespace ava::tui {

class ComposerAreaColorPairs
{
 protected:
  // The colors used for the remaining box characters of the border.
  terminal::ColorPair const border_color_pair_;
  // The rendition used for the margin and the pad viewport.
  terminal::Rendition const rendition_;

 public:
  ComposerAreaColorPairs(terminal::Color foreground, terminal::Color background, terminal::Color tui_background);

  AVA_DEBUG_PRINT_MEMBERS_ON
};

class ComposerArea : public ComposerAreaColorPairs, public terminal::WindowPad
{
 public:
  static constexpr terminal::Box composer_box{
      L"┃██"
      L"┃ █"
      L"╹▀▀"};
  //                                accent bar--^
  //                                             ^^-- top, right and bottom.

  // The margin used by the composer:
  //
  //  ╳  123 (left)                      (right) 12  ╳
  //  ╳  ╾─╼                                     ╾╼  ╳
  //  ╳  ┃█████████████████████████████████████████]1╳ (top)    █ : composer_box characters
  //  ╳  ┃░░·← 0,0▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒░█  ╳          ░ : additional composer_margin area
  //  ╳╾╼┃░░▒▒▒▒▒▒▒▒▒▒▒pad viewport▒▒▒▒▒▒▒▒▒▒▒▒▒▒░█╾╼╳          ▒ : pad viewport area
  //  ╳12┃░░▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒░█12╳          ╳ : terminal edge
  //  ╳  ┃░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░█  ╳
  //  ╳  ┃░░Coder░·░GPT-5.6░Sol░OpenAI░░░░░░░░░░░░█⎤2╳ (bottom)
  //  ╳  ╹▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀⎦1╳
  //  ╳  /project/path          2 (screen margin)    ╳
  //  ╳  ↑ accent bar           1                    ╳
  //  ╳╳╳╳╳╳╳╳╳╳╳╳╳╳╳╳╳╳╳╳╳╳╳╳╳╳╳╳╳╳╳╳╳╳╳╳╳╳╳╳╳╳╳╳╳╳╳╳

  static constexpr terminal::Margin const screen_margin{.top = 0, .bottom = 2, .left = 2, .right = 2};
  static constexpr terminal::Margin const composer_margin{.top = 1, .bottom = 3, .left = 3, .right = 2};
  static constexpr terminal::Color default_accent_bar_color = 0x477ab8;

 public:
  ComposerArea(terminal::columns_t width, terminal::Position bottom_left, terminal::Color foreground, terminal::Color background,
               terminal::Color tui_background);

  void set_accent_bar_color_pair(terminal::ColorPair accent_bar_color_pair);

  AVA_DEBUG_PRINT_MEMBERS_ON
};

} // namespace ava::tui

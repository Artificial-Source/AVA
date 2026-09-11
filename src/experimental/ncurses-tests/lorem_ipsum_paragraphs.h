#pragma once

#include "ava/tui/terminal/Context.h"
#include "ava/tui/terminal/Paragraph.h"

#include <array>
#include <memory>
#include <string>

extern std::array<char const*, 10> lorem_ipsum_paragraphs;
std::unique_ptr<ava::tui::terminal::Paragraph> make_paragraph(std::string_view text, ava::tui::terminal::Rendition paragraph_rendition,
                                                              std::array<ava::tui::terminal::ColorPair, 4> const& phrase_colors);
std::unique_ptr<ava::tui::terminal::Paragraph> make_lorem_ipsum_paragraph(ava::tui::terminal::Context& terminal_context, int paragraph_number);

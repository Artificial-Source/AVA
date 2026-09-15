#include "sys.h"
#include "terminal/BasicWindow.h"
#include "terminal/ColorPair.h"
#include "terminal/Context.h"
#include "terminal/GraphemeBlockRow.h"
#include "terminal/HorizontalLayout.h"
#include "terminal/Pad.h"
#include "terminal/Paragraph.h"
#include "terminal/Spacer.h"
#include "terminal/TextSpan.h"
#include "tests/support/test_harness.h"

#include <clocale>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

namespace terminal = ava::tui::terminal;

namespace {

// Verify the assigned width of `item` against `expected_width`, identifying failures with `name`.
void expect_assigned_width(terminal::LayoutItem const& item, uint32_t expected_width, std::string_view name)
{
  terminal::columns_t const actual_width = item.assigned_width().columns();
  expect(actual_width == expected_width,
         std::string{name} + " must be assigned " + std::to_string(expected_width) + " columns, got " + std::to_string(actual_width));
}

// Verify the first wide character stored at `pos` in `window`, identifying failures with `description`.
void expect_character(terminal::BasicWindow const& window, terminal::Position pos, wchar_t expected_character, std::string_view description)
{
  terminal::ComplexChar cell;
  window.in_wch(pos, cell);
  expect(cell.cell_character().data()[0] == expected_character, std::string{description});
}

// Verify priority negotiation among an exit label, greedy spacer, and right-aligned shortcut paragraph.
void test_status_line_layout()
{
  auto exit_text = terminal::TextSpan::create(u8"Exit the app", {.priority = 1});
  terminal::TextSpan const* exit_text_ptr = exit_text.get();
  auto spacer = terminal::Spacer::create();
  terminal::Spacer const* spacer_ptr = spacer.get();
  auto shortcuts = terminal::Paragraph::create({.priority = 2, .alignment = terminal::HorizontalAlignment::right});
  terminal::Paragraph const* shortcuts_ptr = shortcuts.get();
  shortcuts->append(terminal::TextSpan::create(u8"ctrl+c, ctrl+d, ctrl+x q"));
  shortcuts->initialize_cached_natural_width();

  terminal::HorizontalLayout horizontal_layout;
  horizontal_layout.append(std::move(exit_text));
  horizontal_layout.append(std::move(spacer));
  horizontal_layout.append(std::move(shortcuts));
  horizontal_layout.set_width(17);

  expect_assigned_width(*exit_text_ptr, 3, "exit label");
  expect_assigned_width(*spacer_ptr, 1, "status-line spacer");
  expect_assigned_width(*shortcuts_ptr, 13, "shortcut paragraph");
}

// Verify that mixed priorities reach their natural or intermediate widths in the documented boundary example.
void test_priority_boundary_layout()
{
  auto item0 = terminal::TextSpan::create(u8"1234567890123", {.priority = 5, .minimum_width = 4});
  terminal::TextSpan const* item0_ptr = item0.get();
  auto item1 = terminal::TextSpan::create(u8"12345678901234", {.priority = 2, .minimum_width = 8});
  terminal::TextSpan const* item1_ptr = item1.get();
  auto item2 = terminal::TextSpan::create(u8"123456789012", {.priority = 2, .minimum_width = 5});
  terminal::TextSpan const* item2_ptr = item2.get();
  auto item3 = terminal::Spacer::create({.minimum_width = 3});
  terminal::Spacer const* item3_ptr = item3.get();

  terminal::HorizontalLayout horizontal_layout;
  horizontal_layout.append(std::move(item3));
  horizontal_layout.append(std::move(item0));
  horizontal_layout.append(std::move(item2));
  horizontal_layout.append(std::move(item1));
  horizontal_layout.set_width(32);

  expect_assigned_width(*item3_ptr, 3, "boundary item 3");
  expect_assigned_width(*item0_ptr, 13, "boundary item 0");
  expect_assigned_width(*item2_ptr, 7, "boundary item 2");
  expect_assigned_width(*item1_ptr, 9, "boundary item 1");
}

// Verify weighted distribution when one item reaches its natural width and the others remain flexible.
void test_weighted_layout()
{
  auto item_a = terminal::TextSpan::create(u8"12345678901234567890", {.weight = 1.0f, .minimum_width = 5});
  terminal::TextSpan const* item_a_ptr = item_a.get();
  auto item_b = terminal::TextSpan::create(u8"1234567890123456789012345678901", {.weight = 1.5f, .minimum_width = 7});
  terminal::TextSpan const* item_b_ptr = item_b.get();
  auto item_c = terminal::TextSpan::create(u8"12345678901234567", {.weight = 2.0f, .minimum_width = 11});
  terminal::TextSpan const* item_c_ptr = item_c.get();

  terminal::HorizontalLayout horizontal_layout;
  horizontal_layout.append(std::move(item_a));
  horizontal_layout.append(std::move(item_b));
  horizontal_layout.append(std::move(item_c));
  horizontal_layout.set_width(43);

  expect_assigned_width(*item_a_ptr, 11, "weighted item A");
  expect_assigned_width(*item_b_ptr, 15, "weighted item B");
  expect_assigned_width(*item_c_ptr, 17, "weighted item C");
}

// Verify layouts are not constrained by the former fixed 16-item scratch arrays.
void test_large_item_count_layout()
{
  constexpr std::size_t item_count = 20;
  terminal::HorizontalLayout horizontal_layout;
  std::vector<terminal::TextSpan const*> items;
  items.reserve(item_count);
  for (std::size_t index = 0; index < item_count; ++index)
  {
    auto item = terminal::TextSpan::create(u8"x", {.minimum_width = 1});
    items.push_back(item.get());
    horizontal_layout.append(std::move(item));
  }

  horizontal_layout.set_width(item_count);
  for (std::size_t index = 0; index < items.size(); ++index)
    expect_assigned_width(*items[index], 1, "large-count item " + std::to_string(index));
}

struct TestPad : public terminal::Pad
{
  void generate(terminal::columns_t columns, bool blank_line_between_block_rows)
  {
    generate_grapheme_surface(columns, blank_line_between_block_rows);
    build();
  }
};

// Verify that TextSpan truncation counts terminal columns rather than wide characters when rendering mixed-width text.
//
// Runs ncurses against temporary files and inspects both left- and right-aligned output. The rocket cannot fit in the
// one column remaining after the six-column prefix, so the left-aligned TextSpan gets a trailing filler space. For a
// right-aligned Paragraph, leading filler moves that prefix to the right edge and all retained trailing spaces are clipped.
void test_mixed_width_text_span_rendering()
{
  FILE* input = std::tmpfile();
  FILE* output = std::tmpfile();
  if (!input || !output)
  {
    if (input)
      static_cast<void>(std::fclose(input));
    if (output)
      static_cast<void>(std::fclose(output));
    expect(false, "tmpfile must be available for mixed-width TextSpan rendering");
    return;
  }

  ScopedEnvVar term_guard("TERM", "xterm-256color");
  terminal::Context terminal_context(output, input);

  bool const color_support = terminal_context.has_colors();
  expect(color_support, "TERM=xterm-256color must provide colors for the terminal::Pad test");

  {
    //   index i    priority   minimum width   natural width
    //   0          5          4               13
    //   1          2          8               14
    //   2          2          5               12
    //   3          0          3               unlimited
    auto item0 = terminal::TextSpan::create(u8"abcdefghijklm", {.priority = 5, .minimum_width = 4});
    auto item1 = terminal::TextSpan::create(u8"12345678901234", {.priority = 2, .minimum_width = 8});
    auto item2 = terminal::TextSpan::create(u8"αβ😀γδ🚀εζηθ", {.priority = 2, .minimum_width = 5});
    auto item3 = terminal::Spacer::create({.minimum_width = 3});

    terminal::HorizontalLayout horizontal_layout;
    horizontal_layout.append(std::move(item3));
    horizontal_layout.append(std::move(item0));
    horizontal_layout.append(std::move(item2));
    horizontal_layout.append(std::move(item1));
    TestPad pad;
    pad.append(std::move(horizontal_layout));
    pad.generate(32, false);
    expect(pad.content_rows() == 1 && pad.dimension().width() == 32, "a one-row HorizontalLayout assignment must generate a 1 x 32 pad");
    std::wstring expected_output = L"   abcdefghijklmαβ😀γδ 123456789";
    terminal::columns_t col = 0;
    for (size_t i = 0; i != expected_output.size(); ++i)
    {
      std::ostringstream oss;
      oss << "left-aligned mixed-width output must put expected_output[" << i << "] in column " << col;
      expect_character(pad.basic_window(), {0, col}, expected_output[i], oss.str());
      col += (expected_output[i] == L'😀') ? 2 : 1;
    }
  }

  {
    //                      A         B         C
    //   minimum width |    5         7        11
    //   natural width |   20        31        17
    //          weight |  1.0       1.5       2.0
    auto item_A = terminal::TextSpan::create(u8"12345678901234567890", {.weight = 1, .minimum_width = 5});
    auto item_B = terminal::TextSpan::create(u8"1234567890123456789012345678901", {.weight = 1.5, .minimum_width = 7});
    auto item_C = terminal::TextSpan::create(u8"12345678901234567", {.weight = 2, .minimum_width = 11});
    auto ptr_A = item_A.get();
    auto ptr_B = item_B.get();
    auto ptr_C = item_C.get();

    terminal::HorizontalLayout horizontal_layout;
    horizontal_layout.append(std::move(item_A));
    horizontal_layout.append(std::move(item_B));
    horizontal_layout.append(std::move(item_C));
    horizontal_layout.set_width(43);

    // Expected output: "1234567890112345678901234512345678901234567".
    expect(ptr_A->assigned_width().columns() == 11, "the first item should be assigned a width of 11");
    expect(ptr_B->assigned_width().columns() == 15, "the second item should be assigned a width of 15");
    expect(ptr_C->assigned_width().columns() == 17, "the third item should be assigned a width of 17");
  }

  {
    auto paragraph = terminal::Paragraph::create({.alignment = terminal::HorizontalAlignment::right});
    paragraph->append(terminal::TextSpan::create(u8"αβ😀  🚀εζηθ"));
    terminal::GraphemeBlock rows = paragraph->create_grapheme_block(7);
    expect(!rows.empty(), "right-aligned mixed-width Paragraph must produce at least one GraphemeSpan");
    if (!rows.empty())
    {
      expect(rows.front().columns() == 6 && rows.front().columns_excluding_trailing_whitespace() == 4,
             "the first mixed-width GraphemeSpan must retain six source columns while excluding its two trailing spaces");
      expect(!rows.front().grapheme_runs().empty() && rows.front().grapheme_runs().front().str() == L"αβ😀  ",
             "right-alignment rendering must not remove the source GraphemeRun's trailing spaces");

      terminal::BasicWindow pad = terminal::BasicWindow::newpad({1, 9});
      rows.front().write_to(pad, terminal::Rendition{terminal::ColorPair{}}, true);
      terminal::Position const cursor = pad.getyx();
      expect(cursor.row() == 0 && cursor.col() == 7, "right-aligned mixed-width output must advance by exactly seven columns");
      expect_character(pad, {0, 0}, L' ', "right-aligned mixed-width output must begin with filler space");
      expect_character(pad, {0, 1}, L' ', "right-aligned mixed-width output must begin with filler spaces");
      expect_character(pad, {0, 2}, L' ', "right-aligned mixed-width output must begin with three filler spaces");
      expect_character(pad, {0, 3}, L'α', "right-aligned mixed-width output must put alpha in column 1");
      expect_character(pad, {0, 4}, L'β', "right-aligned mixed-width output must put beta in column 2");
      expect_character(pad, {0, 5}, L'😀', "right-aligned mixed-width output must put the two-column emoji in column 3");
    }
  }

  {
    terminal::HorizontalLayout horizontal_layout;
    horizontal_layout.append(terminal::TextSpan::create(u8"ab  ", {.alignment = terminal::HorizontalAlignment::right}));
    TestPad pad;
    pad.append(std::move(horizontal_layout));
    pad.generate(6, false);

    expect_character(pad.basic_window(), {0, 0}, L' ', "a right-aligned standalone TextSpan must begin with filler space");
    expect_character(pad.basic_window(), {0, 1}, L' ', "a right-aligned standalone TextSpan must use all leading filler space");
    expect_character(pad.basic_window(), {0, 2}, L'a', "a right-aligned standalone TextSpan must align its retained content");
    expect_character(pad.basic_window(), {0, 4}, L' ', "a right-aligned standalone TextSpan must retain source trailing space");
    expect_character(pad.basic_window(), {0, 5}, L' ', "a right-aligned standalone TextSpan must retain all source trailing space");
  }

  {
    terminal::HorizontalLayout horizontal_layout;
    horizontal_layout.append(terminal::TextSpan::create(u8"🚀", {.minimum_width = 1}));
    TestPad pad;
    pad.append(std::move(horizontal_layout));
    pad.generate(1, false);

    expect_character(pad.basic_window(), {0, 0}, L' ', "a standalone TextSpan whose first cluster does not fit must render as empty space");
  }

  {
    std::u8string invalid_utf8{static_cast<char8_t>(0xff)};
    auto paragraph = terminal::Paragraph::create({.minimum_width = 1});
    paragraph->append(terminal::TextSpan::create(invalid_utf8));
    paragraph->initialize_cached_natural_width();

    TestPad pad;
    pad.append(std::move(paragraph));
    pad.generate(3, false);

    expect(pad.content_rows() == 1 && pad.dimension().width() == 3, "a Paragraph containing only an invalid TextSpan must remain a renderable blank row");
    expect_character(pad.basic_window(), {0, 0}, L' ', "an invalid TextSpan in a Paragraph must render as empty space");
  }

  {
    auto wrapping = terminal::Paragraph::create({.minimum_width = 2});
    wrapping->append(terminal::TextSpan::create(u8"aa aa"));
    wrapping->initialize_cached_natural_width();

    terminal::HorizontalLayout first_row;
    first_row.append(std::move(wrapping));
    first_row.append(terminal::TextSpan::create(u8"B", {.minimum_width = 1}));
    first_row.append(terminal::Spacer::create({.minimum_width = 1}));

    auto ending = terminal::Paragraph::create({.minimum_width = 1});
    ending->append(terminal::TextSpan::create(u8"end"));
    ending->initialize_cached_natural_width();
    terminal::HorizontalLayout second_row;
    second_row.append(std::move(ending));

    TestPad pad;
    pad.append(std::move(first_row));
    pad.append(std::move(second_row));
    pad.generate(5, true);

    expect(pad.content_rows() == 4 && pad.dimension().width() == 5,
           "two HorizontalLayout block rows must include their tallest blocks and one separating blank row");
    expect_character(pad.basic_window(), {0, 0}, L'a', "the first block row must contain the first wrapped paragraph row");
    expect_character(pad.basic_window(), {0, 3}, L'B', "an adjacent one-row item must be written on the first surface row");
    expect_character(pad.basic_window(), {1, 0}, L'a', "the taller block must continue on the next surface row");
    expect_character(pad.basic_window(), {1, 3}, L' ', "space must fill below a shorter adjacent block");
    expect_character(pad.basic_window(), {2, 0}, L' ', "Pad must leave a blank row between GraphemeBlockRows");
    expect_character(pad.basic_window(), {3, 0}, L'e', "the second block row must follow the separating blank row");
  }

  {
    terminal::HorizontalLayout narrower;
    narrower.append(terminal::TextSpan::create(u8"a", {.minimum_width = 1}));
    narrower.append(terminal::TextSpan::create(u8"b", {.minimum_width = 1}));
    terminal::HorizontalLayout wider;
    wider.append(terminal::Spacer::create({.minimum_width = 9}));

    TestPad pad;
    pad.append(std::move(narrower));
    pad.append(std::move(wider));
    pad.generate(2, false);
    expect(pad.content_rows() == 2 && pad.dimension().width() == 9,
           "Pad must size itself to the widest block row, got " + std::to_string(pad.content_rows()) + " x " + std::to_string(pad.dimension().width()));
  }

  {
    terminal::Rendition const short_rendition{terminal::ColorPair{}, terminal::Attribute::bold};
    terminal::Rendition const tall_rendition{terminal::ColorPair{}, terminal::Attribute::underline};
    auto short_paragraph = terminal::Paragraph::create(short_rendition, {.minimum_width = 1});
    short_paragraph->append(terminal::TextSpan::create(u8"x"));
    short_paragraph->initialize_cached_natural_width();
    auto tall_paragraph = terminal::Paragraph::create(tall_rendition, {.minimum_width = 1});
    tall_paragraph->append(terminal::TextSpan::create(u8"y y"));
    tall_paragraph->initialize_cached_natural_width();

    terminal::HorizontalLayout horizontal_layout;
    horizontal_layout.append(std::move(short_paragraph));
    horizontal_layout.append(std::move(tall_paragraph));
    terminal::GraphemeBlockRow block_row = horizontal_layout.create_grapheme_block_row(3);
    expect(block_row.blocks()[0].size() == 1 && block_row.blocks()[1].size() == 2,
           "GraphemeBlockRow must not extend a shorter GraphemeBlock with synthetic rows");
    TestPad pad;
    pad.append(std::move(horizontal_layout));
    pad.generate(3, false);

    terminal::ComplexChar cell;
    pad.basic_window().in_wch({1, 0}, cell);
    expect(cell.cell_character().data()[0] == L' ' && cell.rendition().attributes().mask() == terminal::Attributes{}.mask(),
           "empty space below a short Paragraph must use the BasicWindow default rendition");
  }

  static_cast<void>(std::fclose(input));
  static_cast<void>(std::fclose(output));
}

} // namespace

// Run deterministic HorizontalLayout width negotiation and terminal-backed mixed-width rendering coverage.
void run_terminal_horizontal_layout_tests()
{
  test_status_line_layout();
  test_priority_boundary_layout();
  test_weighted_layout();
  test_large_item_count_layout();
  test_mixed_width_text_span_rendering();
}

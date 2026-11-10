#include "sys.h"
#include "support/TuiEventLoopFixture.h"
#include "tests/support/test_harness.h"
#include "tests/support/tui_test_support.h"
#include "ava/tui/Composer.h"
#include "ava/tui/runtime_render_internal.h"
#include "ava/tui/runtime_state_internal.h"
#include "ava/tui/runtime_transcript_internal.h"
#include "ava/tui/runtime_transcript_selection_internal.h"
#include "ava/tui/theme.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <unistd.h>

namespace {

ava::tui::detail::TranscriptLayout selection_layout()
{
  return ava::tui::detail::TranscriptLayout{.lines = {"leading spacer", "\x1b[1mheading\x1b[0m",
                                                      "ab\xE4\xBD\xA0"
                                                      "c",
                                                      "e\xCC\x81x", "", "same"},
                                            .block_boundaries = {1, 4, 6},
                                            .message_starts = {1, 5},
                                            .content_starts = {2, 5},
                                            .message_item_indices = {2, 5}};
}

ava::tui::detail::TranscriptLayoutCache selection_cache(ava::tui::detail::TranscriptLayout layout, std::size_t generation = 1)
{
  ava::tui::detail::TranscriptLayoutCache cache;
  cache.transcript_generation = generation;
  cache.width = 40;
  cache.valid = true;
  cache.layout = std::move(layout);
  return cache;
}

ava::tui::InputEvent mouse_event(ava::tui::terminal::Key key, std::size_t row, std::size_t column)
{
  return ava::tui::InputEvent{.key = key, .mouse_column = column, .mouse_row = row};
}

ava::tui::TranscriptItem private_launch_selection_task()
{
  ava::tui::ToolTimelineItem tool{
      .status = ava::tui::ToolTimelineStatus::Success,
      .name = "task",
      .argument_summary = "arguments provided",
      .result_summary = "ok",
      .arguments_json = R"({"description":"ordinary selectable header","subagent_type":"explore","mode":"foreground"})",
      .result_json = R"({"tool":"task","ok":true,"subagent_type":"explore","description":"ordinary selectable header","state":"completed","tool_calls":1})",
      .call_id = "call_private_launch_selection",
      .lifecycle = ava::tui::ToolLifecycleState::Complete};
  tool.subagent_launch_display = ava::agent::SubagentLaunchDisplay::normalized("PRIVATE MODEL SELECTION TOKEN", std::string_view("private-selection-thinking"));
  return ava::tui::TranscriptItem{.tool = std::move(tool)};
}

// Publish fixture mutations through the same generation boundary as streamed output.
// The mutable value is a test-owned copy, never an alias to live presentation storage.
template <typename Change>
void change_transcript(ava::tui::RuntimePresentationState& presentation, Change&& change)
{
  auto transcript = presentation.screen().transcript;
  change(transcript);
  presentation.publish_transcript(std::move(transcript));
}

// Route fixture mouse input through the real selection owner with a caller-controlled clock and viewport shift.
template <typename ToggleTool, typename ToggleThinking>
ava::tui::TranscriptSelectionMouseResult handle(
    ava::tui::RuntimeTranscriptSelectionState& state, ava::tui::InputEvent const& event, ava::tui::RuntimePresentationState& presentation,
    ava::tui::detail::TranscriptLayoutCache const& cache, ava::tui::Composer* draft, std::size_t& scroll, ToggleTool const& toggle_tool,
    ToggleThinking const& toggle_thinking, std::ptrdiff_t frozen_to_live_shift = 0,
    ava::tui::RuntimeTranscriptSelectionState::Clock::time_point now = ava::tui::RuntimeTranscriptSelectionState::Clock::now())
{
  return state.handle_mouse(event, presentation, cache, draft, scroll, frozen_to_live_shift, toggle_tool, toggle_thinking, now);
}

}  // namespace

// Run pointer selection and copy regressions against published transcript generations.
void run_tui_transcript_selection_tests()
{
  tui_test_support::TuiFrontEndFixture frontend;
  using ava::tui::TranscriptSelectionEndpoint;
  using ava::tui::TranscriptSelectionRange;

  auto const layout = selection_layout();
  auto const heading = ava::tui::endpoint_for_absolute_line(layout, 1, 3);
  auto const wide_left = ava::tui::endpoint_for_absolute_line(layout, 2, 3);
  auto const wide_right = ava::tui::snap_display_column(
      "ab\xE4\xBD\xA0"
      "c",
      3, true);
  expect(!ava::tui::endpoint_for_absolute_line(layout, 0, 0) && !ava::tui::endpoint_for_absolute_line(layout, 4, 0) && heading && heading->item_index == 2 &&
             heading->line_offset == 0 && wide_left && wide_left->display_column == 2 && wide_right == 4,
         "transcript selection owns headings and bodies, rejects inter-item spacers, and snaps wide cells atomically");

  auto const styled_plain = ava::tui::transcript_selection_plain_row("\x1b[31mred\x1b[0m\x1b]8;;https://invalid.example\x1b\\link\x1b]8;;\x1b\\\x01");
  expect(styled_plain == "redlink", "transcript selection strips SGR, OSC, and controls from copied rows");

  auto const extracted = ava::tui::extract_transcript_selection_text(
      layout,
      TranscriptSelectionRange{.anchor = TranscriptSelectionEndpoint{.item_index = 2, .line_offset = 1, .display_column = 1},
                               .focus = TranscriptSelectionEndpoint{.item_index = 2, .line_offset = 2, .display_column = 1}},
      64 * 1024);
  expect(!extracted.oversize &&
             extracted.text ==
                 "b\xE4\xBD\xA0"
                 "c\ne\xCC\x81" &&
             extracted.examined_rows == 2,
         "transcript selection extracts stripped rendered rows with soft-wrap newlines and whole grapheme clusters");

  auto private_snapshot = ava::tui::ComposerSnapshot{};
  private_snapshot.transcript = {private_launch_selection_task(), ava::tui::TranscriptItem{.label = "ava", .text = "ordinary selectable following content"}};
  auto const private_layout = ava::tui::detail::render_transcript_layout(private_snapshot.transcript, 32, ava::tui::ToolPresentation::Compact, true, true);
  auto const first_private = std::ranges::find(private_layout.presentation_private_rows, true);
  auto const private_count = static_cast<std::size_t>(std::ranges::count(private_layout.presentation_private_rows, true));
  auto const following_position = std::ranges::find(private_layout.message_item_indices, std::size_t{1});
  auto const following_layout_position = static_cast<std::size_t>(following_position - private_layout.message_item_indices.begin());
  auto const following_line = private_layout.message_starts[following_layout_position];
  auto const header_line = private_layout.message_starts.front();
  auto const header_plain = ava::tui::transcript_selection_plain_row(private_layout.lines[header_line]);
  auto const following_plain = ava::tui::transcript_selection_plain_row(private_layout.lines[following_line]);
  auto const private_span = ava::tui::extract_transcript_selection_text(
      private_layout,
      TranscriptSelectionRange{
          .anchor = *ava::tui::endpoint_for_absolute_line(private_layout, header_line, 0),
          .focus = *ava::tui::endpoint_for_absolute_line(private_layout, following_line, ava::tui::transcript_selection_plain_columns(following_plain))},
      64 * 1024);
  auto const private_partial = ava::tui::extract_transcript_selection_text(
      private_layout,
      TranscriptSelectionRange{
          .anchor = *ava::tui::endpoint_for_absolute_line(private_layout,
                                                          static_cast<std::size_t>(first_private - private_layout.presentation_private_rows.begin()), 5),
          .focus = *ava::tui::endpoint_for_absolute_line(private_layout, following_line, ava::tui::transcript_selection_plain_columns(following_plain))},
      64 * 1024);
  auto private_visible_compact = tui_test_support::join_visible_lines(private_layout.lines);
  std::erase_if(private_visible_compact, [](unsigned char ch) { return ch == ' ' || ch == '\n'; });
  expect(private_layout.presentation_private_rows.size() == private_layout.lines.size() && private_count >= 2 &&
             private_span.text == header_plain + "\n" + following_plain && private_partial.text == following_plain &&
             private_span.text.find("PRIVATE MODEL SELECTION TOKEN") == std::string::npos &&
             private_span.text.find("private-selection-thinking") == std::string::npos &&
             private_visible_compact.find("PRIVATEMODELSELECTIONTOKEN") != std::string::npos,
         "rendered transcript selection traverses wrapped private launch rows but excludes their full and partial bytes without blank lines or accidental "
         "joining while ordinary header and following content stay selectable");

  auto long_layout = ava::tui::detail::TranscriptLayout{
      .lines = {std::string(64 * 1024 + 1, 'x')}, .block_boundaries = {0, 1}, .message_starts = {0}, .content_starts = {0}, .message_item_indices = {0}};
  auto const oversized = ava::tui::extract_transcript_selection_text(
      long_layout,
      TranscriptSelectionRange{.anchor = TranscriptSelectionEndpoint{},
                               .focus = TranscriptSelectionEndpoint{.item_index = 0, .line_offset = 0, .display_column = 64 * 1024 + 1}},
      64 * 1024);
  expect(oversized.oversize && oversized.text.empty() && oversized.examined_rows == 1,
         "transcript selection observes the 64KiB+1 boundary without retaining a truncated clipboard payload");

  auto const highlighted = ava::tui::apply_transcript_selection_highlight("\x1b[31mred\x1b[0m", 1, 3, false);
  auto const no_color = ava::tui::apply_transcript_selection_highlight("\x1b[31mred\x1b[0m", 1, 3, true);
  expect(highlighted.find(ava::tui::detail::kReverseVideo) != std::string::npos && highlighted.find(ava::tui::detail::kReverseVideoOff) != std::string::npos &&
             highlighted.find("\x1b[0m") != std::string::npos && no_color == "red" && no_color.find('\x1b') == std::string::npos,
         "transcript selection highlight uses style-preserving reverse-off and emits no escapes in plain output");

  // Shared Unicode segment classification drives rendered word selection across
  // punctuation, whitespace, wide cells, and combining clusters.
  auto const granular_layout = ava::tui::detail::TranscriptLayout{.lines = {"alpha,  beta \xE4\xBD\xA0\xE4\xBD\xA0 e\xCC\x81x", "gamma delta"},
                                                                  .block_boundaries = {0, 2},
                                                                  .message_starts = {0},
                                                                  .content_starts = {0},
                                                                  .message_item_indices = {0}};
  auto const alpha_unit = ava::tui::transcript_word_selection_unit(granular_layout, *ava::tui::endpoint_for_absolute_line(granular_layout, 0, 2));
  auto const punctuation_unit = ava::tui::transcript_word_selection_unit(granular_layout, *ava::tui::endpoint_for_absolute_line(granular_layout, 0, 5));
  auto const whitespace_unit = ava::tui::transcript_word_selection_unit(granular_layout, *ava::tui::endpoint_for_absolute_line(granular_layout, 0, 6));
  auto const wide_unit = ava::tui::transcript_word_selection_unit(granular_layout, *ava::tui::endpoint_for_absolute_line(granular_layout, 0, 14));
  auto const combining_unit = ava::tui::transcript_word_selection_unit(granular_layout, *ava::tui::endpoint_for_absolute_line(granular_layout, 0, 18));
  expect(alpha_unit && alpha_unit->start.display_column == 0 && alpha_unit->end.display_column == 5 && punctuation_unit &&
             punctuation_unit->start.display_column == 5 && punctuation_unit->end.display_column == 6 && whitespace_unit &&
             whitespace_unit->start.display_column == 6 && whitespace_unit->end.display_column == 8 && wide_unit && wide_unit->start.display_column == 13 &&
             wide_unit->end.display_column == 17 && combining_unit && combining_unit->start.display_column == 18 && combining_unit->end.display_column == 20,
         "rendered word units reuse the Unicode composer segment classifier and preserve punctuation, whitespace, wide, and combining cluster boundaries");

  {
    using SelectionClock = ava::tui::RuntimeTranscriptSelectionState::Clock;
    auto const started = SelectionClock::time_point{};
    ava::tui::RuntimePresentationState click_snapshot_presentation(frontend.frontend(), {});
    auto const& click_snapshot = click_snapshot_presentation.screen();
    click_snapshot_presentation.publish_dimensions(40, click_snapshot.height);
    click_snapshot_presentation.publish_dimensions(click_snapshot.width, 10);
    click_snapshot_presentation.publish_transcript({ava::tui::TranscriptItem{.label = "ava", .text = "stable source"}});
    auto click_cache = selection_cache(granular_layout, 100);
    ava::tui::RuntimeTranscriptSelectionState click_state;
    ava::tui::Composer click_draft;
    click_draft.replace_draft("draft stays", 4);
    std::size_t click_scroll = 0;
    auto never = [](std::size_t) { return false; };
    auto click = [&](std::size_t row, std::size_t column, std::chrono::milliseconds at) {
      static_cast<void>(handle(click_state, mouse_event(ava::tui::terminal::Key::MouseLeftPress, row, column), click_snapshot_presentation, click_cache,
                               &click_draft, click_scroll, never, never, 0, started + at));
      static_cast<void>(handle(click_state, mouse_event(ava::tui::terminal::Key::MouseLeftRelease, row, column), click_snapshot_presentation, click_cache,
                               &click_draft, click_scroll, never, never, 0, started + at + std::chrono::milliseconds(1)));
    };

    click(1, 2, std::chrono::milliseconds(0));
    auto const single_empty = click_state.empty();
    click(1, 2, std::chrono::milliseconds(100));
    auto const double_range = click_state.range();
    auto const double_text = double_range ? ava::tui::extract_transcript_selection_text(granular_layout, *double_range, 1024).text : std::string{};
    click(1, 2, std::chrono::milliseconds(200));
    auto const triple_range = click_state.range();
    auto const triple_text = triple_range ? ava::tui::extract_transcript_selection_text(granular_layout, *triple_range, 1024).text : std::string{};
    click(1, 2, std::chrono::milliseconds(300));
    auto const fourth_resets = click_state.empty();
    click(1, 2, std::chrono::milliseconds(900));
    auto const timeout_resets = click_state.empty();
    click(1, 10, std::chrono::milliseconds(1000));
    auto const different_word_resets = click_state.empty();
    expect(single_empty && double_text == "alpha" && triple_text == granular_layout.lines[0] && fourth_resets && timeout_resets && different_word_resets &&
               click_draft.draft().text == "draft stays" && click_draft.draft().cursor == 4,
           "500 ms click chains select word on click two, visual row on click three, reset on click four, timeout, or a different word, and never edit the "
           "composer draft");

    // A pointer cancel invalidates the click chain, while a proven cap shift remaps it.
    click_state.clear();
    click(1, 2, std::chrono::milliseconds(1100));
    static_cast<void>(handle(click_state, mouse_event(ava::tui::terminal::Key::MousePointerCancel, 1, 2), click_snapshot_presentation, click_cache,
                             &click_draft, click_scroll, never, never, 0, started + std::chrono::milliseconds(1150)));
    click(1, 2, std::chrono::milliseconds(1200));
    auto const cancel_failed_closed = click_state.empty();

    click_state.clear();
    change_transcript(click_snapshot_presentation, [&](auto& transcript) { transcript.resize(3, click_snapshot.transcript[0]); });
    click(1, 2, std::chrono::milliseconds(1300));
    auto shifted_click_layout = granular_layout;
    shifted_click_layout.message_item_indices = {2};
    click_state.apply_item_index_shift(2, shifted_click_layout);
    click_cache = selection_cache(shifted_click_layout, 101);
    click(1, 2, std::chrono::milliseconds(1400));
    auto const cap_range = click_state.range();
    auto const cap_text = cap_range ? ava::tui::extract_transcript_selection_text(shifted_click_layout, *cap_range, 1024).text : std::string{};

    click_state.clear();
    click_cache = selection_cache(granular_layout, 102);
    change_transcript(click_snapshot_presentation, [&](auto& transcript) { transcript[0].text = "before"; });
    click(1, 2, std::chrono::milliseconds(1500));
    change_transcript(click_snapshot_presentation, [&](auto& transcript) { transcript[0].text = "replacement"; });
    click_cache.transcript_generation = 103;
    click(1, 2, std::chrono::milliseconds(1600));
    expect(cancel_failed_closed && cap_text == "alpha" && click_state.empty(),
           "pointer cancel and source-authority replacement fail closed while a proven transcript-cap shift remaps the pending click chain");
  }

  {
    using SelectionClock = ava::tui::RuntimeTranscriptSelectionState::Clock;
    auto const started = SelectionClock::time_point{};
    ava::tui::RuntimePresentationState drag_snapshot_presentation(frontend.frontend(), {});
    auto const& drag_snapshot = drag_snapshot_presentation.screen();
    drag_snapshot_presentation.publish_dimensions(40, drag_snapshot.height);
    drag_snapshot_presentation.publish_dimensions(drag_snapshot.width, 10);
    drag_snapshot_presentation.publish_transcript({ava::tui::TranscriptItem{.label = "ava", .text = "stable source"}});
    auto const drag_cache = selection_cache(granular_layout, 110);
    ava::tui::RuntimeTranscriptSelectionState drag_state;
    ava::tui::Composer drag_draft;
    std::size_t drag_scroll = 0;
    auto never = [](std::size_t) { return false; };
    auto send = [&](ava::tui::terminal::Key key, std::size_t row, std::size_t column, int milliseconds) {
      return handle(drag_state, mouse_event(key, row, column), drag_snapshot_presentation, drag_cache, &drag_draft, drag_scroll, never, never, 0,
                    started + std::chrono::milliseconds(milliseconds));
    };

    // Double-click beta, then extend by complete words into the second visual row.
    static_cast<void>(send(ava::tui::terminal::Key::MouseLeftPress, 1, 10, 0));
    static_cast<void>(send(ava::tui::terminal::Key::MouseLeftRelease, 1, 10, 1));
    static_cast<void>(send(ava::tui::terminal::Key::MouseLeftPress, 1, 10, 100));
    static_cast<void>(send(ava::tui::terminal::Key::MouseLeftDrag, 2, 8, 110));
    static_cast<void>(send(ava::tui::terminal::Key::MouseLeftRelease, 2, 8, 111));
    auto const forward_range = drag_state.range();
    auto const forward_text = forward_range ? ava::tui::extract_transcript_selection_text(granular_layout, *forward_range, 1024).text : std::string{};

    // The same granular drag in reverse includes complete target and anchor words.
    drag_state.clear();
    static_cast<void>(send(ava::tui::terminal::Key::MouseLeftPress, 2, 8, 200));
    static_cast<void>(send(ava::tui::terminal::Key::MouseLeftRelease, 2, 8, 201));
    static_cast<void>(send(ava::tui::terminal::Key::MouseLeftPress, 2, 8, 300));
    static_cast<void>(send(ava::tui::terminal::Key::MouseLeftDrag, 1, 2, 310));
    static_cast<void>(send(ava::tui::terminal::Key::MouseLeftRelease, 1, 2, 311));
    auto const backward_range = drag_state.range();
    auto const backward_text = backward_range ? ava::tui::extract_transcript_selection_text(granular_layout, *backward_range, 1024).text : std::string{};

    // Triple-click a row, then extend by complete rendered visual rows.
    drag_state.clear();
    static_cast<void>(send(ava::tui::terminal::Key::MouseLeftPress, 1, 2, 400));
    static_cast<void>(send(ava::tui::terminal::Key::MouseLeftRelease, 1, 2, 401));
    static_cast<void>(send(ava::tui::terminal::Key::MouseLeftPress, 1, 2, 500));
    static_cast<void>(send(ava::tui::terminal::Key::MouseLeftRelease, 1, 2, 501));
    static_cast<void>(send(ava::tui::terminal::Key::MouseLeftPress, 1, 2, 600));
    static_cast<void>(send(ava::tui::terminal::Key::MouseLeftDrag, 2, 2, 610));
    static_cast<void>(send(ava::tui::terminal::Key::MouseLeftRelease, 2, 2, 611));
    auto const line_range = drag_state.range();
    auto const line_text = line_range ? ava::tui::extract_transcript_selection_text(granular_layout, *line_range, 1024).text : std::string{};

    expect(forward_text == "beta \xE4\xBD\xA0\xE4\xBD\xA0 e\xCC\x81x\ngamma delta" &&
               backward_text == granular_layout.lines[0] + "\n" + granular_layout.lines[1] &&
               line_text == granular_layout.lines[0] + "\n" + granular_layout.lines[1],
           "word and line granular drags extend by complete rendered units in both directions across wrapped visual rows");
  }

  {
    auto const top = ava::tui::detail::transcript_position_indicator_geometry(100, 10, 90);
    auto const middle = ava::tui::detail::transcript_position_indicator_geometry(100, 10, 45);
    auto const bottom = ava::tui::detail::transcript_position_indicator_geometry(100, 10, 0);
    auto const proportional = ava::tui::detail::transcript_position_indicator_geometry(20, 10, 5);
    auto const fits = ava::tui::detail::transcript_position_indicator_geometry(10, 10, 0);
    auto const cjk = std::string("abcdef\xE4\xBD\xA0");
    auto const combining = std::string("abcdefg") + "e\xCC\x81";
    auto const zwj = std::string("abcdef\xF0\x9F\x91\xA9\xE2\x80\x8D\xF0\x9F\x92\xBB");
    auto const accent = std::string("\x1b[38;2;77;158;246m");
    auto const reset = std::string("\x1b[0m");
    auto const link_open = std::string("\x1b]8;;https://example.test\x1b\\");
    auto const link_close = std::string("\x1b]8;;\x1b\\");
    auto const reverse = std::string(ava::tui::detail::kReverseVideo);
    auto const reverse_off = std::string(ava::tui::detail::kReverseVideoOff);
    std::vector<std::string> indicator_source{
        "12345678", "abcdefgZ",      cjk, combining, zwj, accent + "abcdefgZ" + reset, link_open + "abcdefgZ" + link_close, reverse + "abcdefgZ" + reverse_off,
        "row",      "tail unchanged"};
    auto indicator_lines = indicator_source;
    auto const thumb = ava::tui::detail::TranscriptPositionIndicatorGeometry{.thumb_start = 1, .thumb_length = 8, .visible = true};
    ava::tui::detail::apply_transcript_position_indicator_overlay(indicator_lines, 8, thumb, false);
    auto const styled_content_survives =
        indicator_lines[1] == "abcdefg" + reverse + "Z" + reverse_off && indicator_lines[2] == std::string("abcdef") + reverse + "\xE4\xBD\xA0" + reverse_off &&
        indicator_lines[3] == std::string("abcdefg") + reverse + "e\xCC\x81" + reverse_off &&
        indicator_lines[4] == std::string("abcdef") + reverse + "\xF0\x9F\x91\xA9\xE2\x80\x8D\xF0\x9F\x92\xBB" + reverse_off &&
        indicator_lines[5] == accent + "abcdefg" + reverse + "Z" + reverse_off + reset &&
        indicator_lines[6] == link_open + "abcdefg" + reverse + "Z" + reverse_off + link_close &&
        indicator_lines[7] == reverse + "abcdefg" + reverse_off + "Z" + reverse + reverse_off && indicator_lines[8] == "row    " + reverse + " " + reverse_off;
    auto only_thumb_rows_styled = indicator_lines[0] == indicator_source[0] && indicator_lines[9] == indicator_source[9];
    auto exact_thumb_widths = true;
    for (std::size_t row = 1; row <= 8; ++row)
    {
      only_thumb_rows_styled = only_thumb_rows_styled && indicator_lines[row] != indicator_source[row];
      exact_thumb_widths = exact_thumb_widths && ava::tui::detail::terminal_text_columns(indicator_lines[row]) == 8;
    }
    auto const no_destructive_glyphs = std::ranges::none_of(indicator_lines, [](std::string const& line) {
      return line.find("│") != std::string::npos || line.find("█") != std::string::npos || line.find('|') != std::string::npos ||
             line.find('#') != std::string::npos;
    });
    auto plain_lines = indicator_source;
    ava::tui::detail::apply_transcript_position_indicator_overlay(plain_lines, 8, thumb, true);
    auto const plain_unchanged = plain_lines == indicator_source;

    using IndicatorClock = ava::tui::TranscriptPositionIndicatorState::Clock;
    ava::tui::TranscriptPositionIndicatorState indicator;
    auto const started = IndicatorClock::time_point{};
    indicator.show(started);
    auto const expiry = indicator.expires_at();
    auto const waits_one_second = expiry && *expiry - started == ava::tui::TranscriptPositionIndicatorState::kVisibleDuration;
    auto const early_visible = !indicator.expire_if_due(started + std::chrono::milliseconds(999)) && indicator.visible();
    auto const expired = indicator.expire_if_due(started + std::chrono::milliseconds(1000)) && !indicator.visible();
    ava::tui::RuntimePresentationState streaming_snapshot_presentation(frontend.frontend(), {});
    ava::tui::Composer streaming_draft;
    tui_test_support::TuiEventLoopFixture event_loop;
    ava::tui::RuntimeRenderer streaming_renderer(event_loop.timer_registry(), frontend.frontend(), streaming_snapshot_presentation, streaming_draft);
    change_transcript(streaming_snapshot_presentation, [&](auto& transcript) {
      transcript.push_back(ava::tui::TranscriptItem{.label = "ava", .text = "streaming", .stream_id = "live", .append_only_stream = true});
    });
    streaming_snapshot_presentation.invalidate_transcript_layout();
    auto const streaming_stays_hidden = !streaming_snapshot_presentation.screen().transcript_position_indicator_visible;
    expect(top.visible && top.thumb_start == 0 && middle.visible && middle.thumb_start > top.thumb_start && middle.thumb_start < bottom.thumb_start &&
               bottom.visible && bottom.thumb_start + bottom.thumb_length == 10 && proportional.thumb_length == 5 && !fits.visible && styled_content_survives &&
               only_thumb_rows_styled && exact_thumb_widths && no_destructive_glyphs && plain_unchanged && waits_one_second && early_visible && expired &&
               streaming_stays_hidden,
           "the paint-only transcript indicator derives proportional geometry, styles only complete right-edge thumb cells without replacing ASCII, wide, "
           "combining, ZWJ, SGR, or OSC8 content, leaves plain output unchanged, expires after one second, and stays hidden for live-tail streaming alone");
  }

  ava::tui::RuntimePresentationState snapshot_presentation(frontend.frontend(), {});
  auto const& snapshot = snapshot_presentation.screen();
  snapshot_presentation.publish_dimensions(40, snapshot.height);
  snapshot_presentation.publish_dimensions(snapshot.width, 10);
  change_transcript(snapshot_presentation, [&](auto& transcript) { transcript.resize(1); });
  change_transcript(snapshot_presentation, [&](auto& transcript) { transcript[0].text = "body"; });
  change_transcript(snapshot_presentation, [&](auto& transcript) { transcript[0].tool.emplace(); });
  auto header_layout = ava::tui::detail::TranscriptLayout{
      .lines = {"tool header", "body text"}, .block_boundaries = {0, 2}, .message_starts = {0}, .content_starts = {0}, .message_item_indices = {0}};
  auto cache = selection_cache(header_layout);
  ava::tui::RuntimeTranscriptSelectionState state;
  ava::tui::Composer draft;
  draft.replace_draft("draft", 0);
  draft.extend_selection_to(3);
  std::size_t scroll = 0;
  std::size_t toggles = 0;
  std::size_t last_toggled = std::string::npos;
  auto toggle_tool = [&](std::size_t item) {
    ++toggles;
    last_toggled = item;
    return true;
  };
  auto never_toggle = [](std::size_t) { return false; };

  static_cast<void>(
      handle(state, mouse_event(ava::tui::terminal::Key::MouseLeftPress, 1, 2), snapshot_presentation, cache, &draft, scroll, toggle_tool, never_toggle));
  static_cast<void>(
      handle(state, mouse_event(ava::tui::terminal::Key::MouseLeftRelease, 1, 2), snapshot_presentation, cache, &draft, scroll, toggle_tool, never_toggle));
  expect(toggles == 1 && last_toggled == 0 && state.empty(), "transcript header press/release without movement preserves click-to-toggle fallback");

  // Rapid action clicks on a tool/thinking header must continue toggling. They
  // are not eligible for body double/triple-click word or line selection.
  static_cast<void>(
      handle(state, mouse_event(ava::tui::terminal::Key::MouseLeftPress, 1, 2), snapshot_presentation, cache, &draft, scroll, toggle_tool, never_toggle));
  static_cast<void>(
      handle(state, mouse_event(ava::tui::terminal::Key::MouseLeftRelease, 1, 2), snapshot_presentation, cache, &draft, scroll, toggle_tool, never_toggle));
  static_cast<void>(
      handle(state, mouse_event(ava::tui::terminal::Key::MouseLeftClick, 1, 2), snapshot_presentation, cache, &draft, scroll, toggle_tool, never_toggle));
  static_cast<void>(
      handle(state, mouse_event(ava::tui::terminal::Key::MouseLeftClick, 1, 2), snapshot_presentation, cache, &draft, scroll, toggle_tool, never_toggle));
  expect(toggles == 4 && last_toggled == 0 && state.empty(),
         "rapid press/release and complete-click header actions keep toggling instead of entering the body multi-click selection chain");

  static_cast<void>(
      handle(state, mouse_event(ava::tui::terminal::Key::MouseLeftPress, 1, 2), snapshot_presentation, cache, &draft, scroll, toggle_tool, never_toggle));
  static_cast<void>(
      handle(state, mouse_event(ava::tui::terminal::Key::MouseLeftRelease, 2, 5), snapshot_presentation, cache, &draft, scroll, toggle_tool, never_toggle));
  auto header_drag = state.range();
  expect(toggles == 4 && header_drag && header_drag->anchor.item_index == 0 && header_drag->anchor.line_offset == 0 && header_drag->focus.line_offset == 1 &&
             !draft.selection_bounds(),
         "transcript header drag anchors at the original press endpoint, does not toggle, and clears draft selection");

  state.clear();
  static_cast<void>(
      handle(state, mouse_event(ava::tui::terminal::Key::MouseLeftDrag, 2, 5), snapshot_presentation, cache, &draft, scroll, toggle_tool, never_toggle));
  expect(state.empty(), "transcript hover or drag without an owned press never starts selection");

  static_cast<void>(
      handle(state, mouse_event(ava::tui::terminal::Key::MouseLeftPress, 2, 2), snapshot_presentation, cache, &draft, scroll, toggle_tool, never_toggle));
  static_cast<void>(
      handle(state, mouse_event(ava::tui::terminal::Key::MouseLeftDrag, 2, 6), snapshot_presentation, cache, &draft, scroll, toggle_tool, never_toggle));
  static_cast<void>(
      handle(state, mouse_event(ava::tui::terminal::Key::MouseLeftRelease, 2, 6), snapshot_presentation, cache, &draft, scroll, toggle_tool, never_toggle));
  expect(state.range().has_value() && !state.dragging(), "transcript body drag remains selected after release");
  snapshot_presentation.publish_status("selection copy request sent");
  auto const copied_frame = ava::tui::render_composer(frontend.frontend(), snapshot);
  expect(std::ranges::any_of(copied_frame, [](std::string const& line) { return strip_sgr(line).find("selection copy request sent") != std::string::npos; }),
         "successful transcript copy status is visible in the terminal frame");

  // SEL-004: allowlisted clipboard failure and oversize statuses render in-frame.
  snapshot_presentation.publish_status("clipboard copy failed");
  auto const fail_frame = ava::tui::render_composer(frontend.frontend(), snapshot);
  expect(std::ranges::any_of(fail_frame, [](std::string const& line) { return strip_sgr(line).find("clipboard copy failed") != std::string::npos; }),
         "clipboard copy failed status is visible in the terminal frame");
  snapshot_presentation.publish_status("selection too large to copy");
  auto const oversize_frame = ava::tui::render_composer(frontend.frontend(), snapshot);
  expect(std::ranges::any_of(oversize_frame, [](std::string const& line) { return strip_sgr(line).find("selection too large to copy") != std::string::npos; }),
         "selection too large to copy status is visible in the terminal frame");

  auto scrolling_layout = ava::tui::detail::TranscriptLayout{};
  for (std::size_t line = 0; line < 20; ++line)
    scrolling_layout.lines.push_back("line " + std::to_string(line));
  scrolling_layout.block_boundaries = {0, 20};
  scrolling_layout.message_starts = {0};
  scrolling_layout.content_starts = {0};
  scrolling_layout.message_item_indices = {0};
  auto scrolling_cache = selection_cache(scrolling_layout, snapshot.transcript_generation);
  ava::tui::RuntimeTranscriptSelectionState autoscroll_state;
  snapshot_presentation.publish_status({});
  scroll = 0;
  using SelectionClock = ava::tui::RuntimeTranscriptSelectionState::Clock;
  auto const held_started = SelectionClock::time_point{};
  static_cast<void>(handle(autoscroll_state, mouse_event(ava::tui::terminal::Key::MouseLeftPress, 2, 2), snapshot_presentation, scrolling_cache, &draft, scroll,
                           never_toggle, never_toggle, 0, held_started));
  static_cast<void>(handle(autoscroll_state, mouse_event(ava::tui::terminal::Key::MouseLeftDrag, 1, 2), snapshot_presentation, scrolling_cache, &draft, scroll,
                           never_toggle, never_toggle, 0, held_started));
  auto const held_deadline = autoscroll_state.edge_autoscroll_deadline();
  auto const early_tick =
      autoscroll_state.tick_edge_autoscroll(snapshot_presentation, scrolling_cache, scroll, 0, held_started + std::chrono::milliseconds(49));
  auto const due_tick = autoscroll_state.tick_edge_autoscroll(snapshot_presentation, scrolling_cache, scroll, 0, held_started + std::chrono::milliseconds(50));
  expect(scroll == 2 && autoscroll_state.range() && held_deadline &&
             *held_deadline - held_started == ava::tui::RuntimeTranscriptSelectionState::kEdgeAutoscrollInterval &&
             early_tick == ava::tui::TranscriptSelectionMouseResult::Ignored && due_tick == ava::tui::TranscriptSelectionMouseResult::HandledNeedsRender,
         "transcript selection motion autoscrolls immediately and held-edge selection repeats at a deterministic 50 ms renderer cadence without a thread");

  // No collapsed overview chrome reserves leading rows: the first screen row is an
  // ordinary transcript row for presses and drag autoscroll alike.
  {
    auto top_row_layout = ava::tui::detail::TranscriptLayout{};
    for (std::size_t line = 0; line < 40; ++line)
      top_row_layout.lines.push_back("top row line " + std::to_string(line));
    top_row_layout.block_boundaries = {0, 40};
    top_row_layout.message_starts = {0};
    top_row_layout.content_starts = {0};
    top_row_layout.message_item_indices = {0};
    ava::tui::RuntimePresentationState top_row_snapshot_presentation(frontend.frontend(), {});
    auto const& top_row_snapshot = top_row_snapshot_presentation.screen();
    top_row_snapshot_presentation.publish_transcript(snapshot.transcript);
    top_row_snapshot_presentation.publish_status(snapshot.status);
    top_row_snapshot_presentation.publish_dimensions(80, top_row_snapshot.height);
    top_row_snapshot_presentation.publish_dimensions(top_row_snapshot.width, 24);
    top_row_snapshot_presentation.initialize_startup_overview(*frontend.frontend(), ava::tui::StartupOverviewSnapshot{
                                                                                        .mode = "build",
                                                                                        .compact_line = "build · /overview",
                                                                                        .detail_line = "detail",
                                                                                    });
    top_row_snapshot_presentation.publish_transcript_scroll(0);
    auto top_row_cache = selection_cache(top_row_layout, top_row_snapshot.transcript_generation);
    ava::tui::RuntimeTranscriptSelectionState top_row_drag;
    std::size_t top_row_scroll = 0;
    // A press on the first screen row starts transcript selection; no dead chrome zone remains.
    static_cast<void>(handle(top_row_drag, mouse_event(ava::tui::terminal::Key::MouseLeftPress, 1, 2), top_row_snapshot_presentation, top_row_cache, &draft,
                             top_row_scroll, never_toggle, never_toggle));
    expect(!top_row_drag.empty(), "first screen row is transcript, not reserved overview chrome");
    // Dragging back to the first row still autoscrolls at the upper edge.
    static_cast<void>(handle(top_row_drag, mouse_event(ava::tui::terminal::Key::MouseLeftPress, 3, 2), top_row_snapshot_presentation, top_row_cache, &draft,
                             top_row_scroll, never_toggle, never_toggle));
    static_cast<void>(handle(top_row_drag, mouse_event(ava::tui::terminal::Key::MouseLeftDrag, 1, 2), top_row_snapshot_presentation, top_row_cache, &draft,
                             top_row_scroll, never_toggle, never_toggle));
    expect(top_row_scroll == 1 && top_row_drag.range(),
           "transcript selection drag to the first row autoscrolls at the upper edge while retaining selection authority");
  }

  auto remapped_layout = header_layout;
  remapped_layout.lines = {"tool header"};
  remapped_layout.block_boundaries = {0, 1};
  state.rebind_authority(remapped_layout, 2, 40, ava::tui::ToolPresentation::Rich, true, false);
  expect(state.range() && state.range()->anchor.line_offset == 0 && state.range()->focus.line_offset == 0,
         "transcript selection clamps content-relative endpoints after resize or expansion reflow");

  auto shifted_layout = remapped_layout;
  shifted_layout.message_item_indices = {2};
  state.apply_item_index_shift(2, shifted_layout);
  expect(state.range() && state.range()->anchor.item_index == 2 && state.range()->focus.item_index == 2,
         "transcript selection remaps endpoints through positive transcript-cap item shifts");
  shifted_layout.message_item_indices.clear();
  shifted_layout.message_starts.clear();
  shifted_layout.content_starts.clear();
  shifted_layout.block_boundaries.clear();
  state.remap_or_clear(shifted_layout);
  expect(state.empty(), "transcript selection clears when selected content is suppressed or replaced");

  auto duplicate_layout = ava::tui::detail::TranscriptLayout{
      .lines = {"same", "same"}, .block_boundaries = {0, 1, 2}, .message_starts = {0, 1}, .content_starts = {0, 1}, .message_item_indices = {0, 1}};
  auto duplicate_cache = selection_cache(duplicate_layout, 3);
  change_transcript(snapshot_presentation, [&](auto& transcript) { transcript.resize(2); });
  change_transcript(snapshot_presentation, [&](auto& transcript) { transcript[0].tool.reset(); });
  change_transcript(snapshot_presentation, [&](auto& transcript) { transcript[1].text = "same"; });
  change_transcript(snapshot_presentation, [&](auto& transcript) { transcript[1].stream_id = "stable-stream"; });
  change_transcript(snapshot_presentation, [&](auto& transcript) { transcript[1].append_only_stream = true; });
  static_cast<void>(handle(state, mouse_event(ava::tui::terminal::Key::MouseLeftPress, 2, 2), snapshot_presentation, duplicate_cache, &draft, scroll,
                           never_toggle, never_toggle));
  static_cast<void>(handle(state, mouse_event(ava::tui::terminal::Key::MouseLeftDrag, 2, 5), snapshot_presentation, duplicate_cache, &draft, scroll,
                           never_toggle, never_toggle));
  static_cast<void>(handle(state, mouse_event(ava::tui::terminal::Key::MouseLeftRelease, 2, 5), snapshot_presentation, duplicate_cache, &draft, scroll,
                           never_toggle, never_toggle));
  expect(state.range() && state.range()->anchor.item_index == 1 && state.range()->focus.item_index == 1,
         "transcript selection ownership is item-indexed and independent of duplicate rendered text");

  change_transcript(snapshot_presentation, [&](auto& transcript) { transcript[1].text = "same appended"; });
  auto streamed_layout = duplicate_layout;
  streamed_layout.lines[1] = "same appended";
  auto streamed_cache = selection_cache(streamed_layout, 4);
  expect(state.ensure_authority(streamed_cache, &snapshot) && state.range() && state.range()->anchor.item_index == 1,
         "transcript selection survives a proven append-only stream update");
  change_transcript(snapshot_presentation, [&](auto& transcript) { transcript[1].append_only_stream = false; });
  change_transcript(snapshot_presentation, [&](auto& transcript) { transcript[1].text = "replacement"; });
  auto replacement_layout = duplicate_layout;
  replacement_layout.lines[1] = "replacement";
  auto replacement_cache = selection_cache(replacement_layout, 5);
  expect(state.ensure_authority(replacement_cache, &snapshot) && state.empty(), "transcript selection clears when selected source content is replaced");

  ava::tui::detail::TranscriptLayoutCache invalid_cache;
  expect(!state.ensure_authority(invalid_cache) && state.empty(), "transcript selection fails closed instead of rebuilding an invalid layout authority");

  // SEL-001: frozen detached layout + deferred leading-eviction shift maps header toggle to the
  // same live source item (tool and thinking), and fails closed when the source is gone.
  {
    ava::tui::RuntimePresentationState frozen_snapshot_presentation(frontend.frontend(), {});
    auto const& frozen_snapshot = frozen_snapshot_presentation.screen();
    frozen_snapshot_presentation.publish_dimensions(40, frozen_snapshot.height);
    frozen_snapshot_presentation.publish_dimensions(frozen_snapshot.width, 10);
    frozen_snapshot_presentation.publish_thinking_visibility(true);
    change_transcript(frozen_snapshot_presentation, [&](auto& transcript) { transcript.resize(3); });
    change_transcript(frozen_snapshot_presentation, [&](auto& transcript) { transcript[0].text = "old leading"; });
    change_transcript(frozen_snapshot_presentation, [&](auto& transcript) { transcript[1].text = "body"; });
    change_transcript(frozen_snapshot_presentation, [&](auto& transcript) { transcript[1].tool.emplace(); });
    change_transcript(frozen_snapshot_presentation, [&](auto& transcript) { transcript[1].tool->name = "bash"; });
    change_transcript(frozen_snapshot_presentation, [&](auto& transcript) { transcript[1].tool->call_id = "call-tool"; });
    std::string long_think;
    for (int line = 0; line < 20; ++line)
    {
      long_think += "thinking line ";
      long_think += std::to_string(line);
      long_think.push_back('\n');
    }
    change_transcript(frozen_snapshot_presentation, [&](auto& transcript) { transcript[2].label = "thinking"; });
    change_transcript(frozen_snapshot_presentation, [&](auto& transcript) { transcript[2].text = long_think; });
    change_transcript(frozen_snapshot_presentation, [&](auto& transcript) { transcript[2].thinking_expanded = false; });

    // Live transcript after leading eviction: former indices 1/2 are now 0/1.
    change_transcript(frozen_snapshot_presentation, [&](auto& transcript) { transcript.erase(transcript.begin()); });

    auto frozen_tool_layout = ava::tui::detail::TranscriptLayout{.lines = {"tool header", "body text"},
                                                                 .block_boundaries = {0, 2},
                                                                 .message_starts = {0},
                                                                 .content_starts = {0},
                                                                 // Frozen authority still names pre-shift item 1.
                                                                 .message_item_indices = {1}};
    auto frozen_tool_cache = selection_cache(frozen_tool_layout, 10);
    ava::tui::RuntimeTranscriptSelectionState frozen_state;
    ava::tui::Composer frozen_draft;
    std::size_t frozen_scroll = 0;
    std::size_t tool_toggle_index = std::string::npos;
    std::size_t tool_toggles = 0;
    auto toggle_live_tool = [&](std::size_t item) {
      ++tool_toggles;
      tool_toggle_index = item;
      return item < frozen_snapshot.transcript.size() && frozen_snapshot.transcript[item].tool.has_value();
    };

    static_cast<void>(handle(frozen_state, mouse_event(ava::tui::terminal::Key::MouseLeftPress, 1, 2), frozen_snapshot_presentation, frozen_tool_cache,
                             &frozen_draft, frozen_scroll, toggle_live_tool, never_toggle, /*frozen_to_live_shift=*/-1));
    expect(frozen_state.dragging(), "frozen tool header press arms without forcing a live layout rebuild");
    static_cast<void>(handle(frozen_state, mouse_event(ava::tui::terminal::Key::MouseLeftRelease, 1, 2), frozen_snapshot_presentation, frozen_tool_cache,
                             &frozen_draft, frozen_scroll, toggle_live_tool, never_toggle, /*frozen_to_live_shift=*/-1));
    expect(tool_toggles == 1 && tool_toggle_index == 0 && !frozen_state.dragging(),
           "frozen detached tool header release maps through deferred item_index_shift onto the same live source");

    // Body drag under the same frozen authority keeps frozen indices (no live rebuild).
    tool_toggles = 0;
    static_cast<void>(handle(frozen_state, mouse_event(ava::tui::terminal::Key::MouseLeftPress, 2, 2), frozen_snapshot_presentation, frozen_tool_cache,
                             &frozen_draft, frozen_scroll, toggle_live_tool, never_toggle, /*frozen_to_live_shift=*/-1));
    static_cast<void>(handle(frozen_state, mouse_event(ava::tui::terminal::Key::MouseLeftDrag, 2, 6), frozen_snapshot_presentation, frozen_tool_cache,
                             &frozen_draft, frozen_scroll, toggle_live_tool, never_toggle, /*frozen_to_live_shift=*/-1));
    static_cast<void>(handle(frozen_state, mouse_event(ava::tui::terminal::Key::MouseLeftRelease, 2, 6), frozen_snapshot_presentation, frozen_tool_cache,
                             &frozen_draft, frozen_scroll, toggle_live_tool, never_toggle, /*frozen_to_live_shift=*/-1));
    expect(tool_toggles == 0 && frozen_state.range() && frozen_state.range()->anchor.item_index == 1 && frozen_state.range()->focus.item_index == 1,
           "frozen body selection retains frozen item indices and does not force live rebuild or toggle");
    frozen_state.clear();

    // Thinking header under freeze + shift.
    auto frozen_think_layout = ava::tui::detail::TranscriptLayout{
        .lines = {"thinking header", "preview"}, .block_boundaries = {0, 2}, .message_starts = {0}, .content_starts = {0}, .message_item_indices = {2}};
    auto frozen_think_cache = selection_cache(frozen_think_layout, 11);
    std::size_t think_toggle_index = std::string::npos;
    std::size_t think_toggles = 0;
    auto toggle_live_think = [&](std::size_t item) {
      ++think_toggles;
      think_toggle_index = item;
      return item < frozen_snapshot.transcript.size() && !frozen_snapshot.transcript[item].tool.has_value();
    };
    static_cast<void>(handle(frozen_state, mouse_event(ava::tui::terminal::Key::MouseLeftPress, 1, 2), frozen_snapshot_presentation, frozen_think_cache,
                             &frozen_draft, frozen_scroll, never_toggle, toggle_live_think, /*frozen_to_live_shift=*/-1));
    static_cast<void>(handle(frozen_state, mouse_event(ava::tui::terminal::Key::MouseLeftRelease, 1, 2), frozen_snapshot_presentation, frozen_think_cache,
                             &frozen_draft, frozen_scroll, never_toggle, toggle_live_think, /*frozen_to_live_shift=*/-1));
    expect(think_toggles == 1 && think_toggle_index == 1,
           "frozen detached thinking header release maps through deferred item_index_shift onto the same live source");

    // Source replacement after arm fails closed (same mapped index, incompatible identity).
    tool_toggles = 0;
    tool_toggle_index = std::string::npos;
    static_cast<void>(handle(frozen_state, mouse_event(ava::tui::terminal::Key::MouseLeftPress, 1, 2), frozen_snapshot_presentation, frozen_tool_cache,
                             &frozen_draft, frozen_scroll, toggle_live_tool, never_toggle, /*frozen_to_live_shift=*/-1));
    expect(frozen_state.dragging(), "tool header arms before source replacement");
    change_transcript(frozen_snapshot_presentation, [&](auto& transcript) { transcript[0] = ava::tui::TranscriptItem{.label = "ava", .text = "replaced"}; });
    static_cast<void>(handle(frozen_state, mouse_event(ava::tui::terminal::Key::MouseLeftRelease, 1, 2), frozen_snapshot_presentation, frozen_tool_cache,
                             &frozen_draft, frozen_scroll, toggle_live_tool, never_toggle, /*frozen_to_live_shift=*/-1));
    expect(tool_toggles == 0 && !frozen_state.dragging(), "header toggle fails closed when the mapped live source was replaced");

    // Leading-eviction of the frozen index itself fails closed on toggle mapping.
    expect(!ava::tui::shift_transcript_selection_item_index(0, -1), "frozen item 0 cannot survive a leading-eviction item_index_shift of -1");

    // Direct click path also maps through the deferred shift.
    change_transcript(frozen_snapshot_presentation, [&](auto& transcript) { transcript[0] = ava::tui::TranscriptItem{.text = "body"}; });
    change_transcript(frozen_snapshot_presentation, [&](auto& transcript) { transcript[0].tool.emplace(); });
    change_transcript(frozen_snapshot_presentation, [&](auto& transcript) { transcript[0].tool->name = "bash"; });
    change_transcript(frozen_snapshot_presentation, [&](auto& transcript) { transcript[0].tool->call_id = "call-tool"; });
    tool_toggles = 0;
    tool_toggle_index = std::string::npos;
    static_cast<void>(handle(frozen_state, mouse_event(ava::tui::terminal::Key::MouseLeftClick, 1, 2), frozen_snapshot_presentation, frozen_tool_cache,
                             &frozen_draft, frozen_scroll, toggle_live_tool, never_toggle, /*frozen_to_live_shift=*/-1));
    expect(tool_toggles == 1 && tool_toggle_index == 0, "frozen tool header click maps through deferred item_index_shift");
  }

  // SEL-002: cancel pointer interaction ends Selecting/HeaderArmed without discarding a committed range.
  {
    ava::tui::RuntimePresentationState cancel_snapshot_presentation(frontend.frontend(), {});
    auto const& cancel_snapshot = cancel_snapshot_presentation.screen();
    cancel_snapshot_presentation.publish_dimensions(40, cancel_snapshot.height);
    cancel_snapshot_presentation.publish_dimensions(cancel_snapshot.width, 10);
    change_transcript(cancel_snapshot_presentation, [&](auto& transcript) { transcript.resize(1); });
    change_transcript(cancel_snapshot_presentation, [&](auto& transcript) { transcript[0].text = "body"; });
    change_transcript(cancel_snapshot_presentation, [&](auto& transcript) { transcript[0].tool.emplace(); });
    auto cancel_layout = ava::tui::detail::TranscriptLayout{
        .lines = {"tool header", "body text"}, .block_boundaries = {0, 2}, .message_starts = {0}, .content_starts = {0}, .message_item_indices = {0}};
    auto cancel_cache = selection_cache(cancel_layout, 20);
    ava::tui::RuntimeTranscriptSelectionState cancel_state;
    ava::tui::Composer cancel_draft;
    std::size_t cancel_scroll = 0;
    std::size_t cancel_toggles = 0;
    auto toggle_cancel = [&](std::size_t) {
      ++cancel_toggles;
      return true;
    };

    static_cast<void>(handle(cancel_state, mouse_event(ava::tui::terminal::Key::MouseLeftPress, 1, 2), cancel_snapshot_presentation, cancel_cache,
                             &cancel_draft, cancel_scroll, toggle_cancel, never_toggle));
    expect(cancel_state.dragging(), "header press arms before cancel");
    auto const cancel_armed = handle(cancel_state, mouse_event(ava::tui::terminal::Key::MousePointerCancel, 1, 2), cancel_snapshot_presentation, cancel_cache,
                                     &cancel_draft, cancel_scroll, toggle_cancel, never_toggle);
    expect(cancel_armed == ava::tui::TranscriptSelectionMouseResult::HandledNeedsRender && !cancel_state.dragging() && cancel_toggles == 0,
           "MousePointerCancel clears HeaderArmed without toggling");
    static_cast<void>(handle(cancel_state, mouse_event(ava::tui::terminal::Key::MouseLeftRelease, 1, 2), cancel_snapshot_presentation, cancel_cache,
                             &cancel_draft, cancel_scroll, toggle_cancel, never_toggle));
    expect(cancel_toggles == 0, "release after pointer cancel cannot toggle an armed header");

    static_cast<void>(handle(cancel_state, mouse_event(ava::tui::terminal::Key::MouseLeftPress, 2, 2), cancel_snapshot_presentation, cancel_cache,
                             &cancel_draft, cancel_scroll, toggle_cancel, never_toggle));
    static_cast<void>(handle(cancel_state, mouse_event(ava::tui::terminal::Key::MouseLeftDrag, 2, 6), cancel_snapshot_presentation, cancel_cache, &cancel_draft,
                             cancel_scroll, toggle_cancel, never_toggle));
    expect(cancel_state.dragging() && cancel_state.range(), "body drag establishes an in-flight selection");
    auto const focus_before = cancel_state.range()->focus.display_column;
    cancel_state.cancel_pointer_interaction();
    expect(!cancel_state.dragging() && cancel_state.range() && cancel_state.range()->focus.display_column == focus_before,
           "cancel_pointer_interaction ends Selecting while preserving the range");
    // Production terminals cannot emit Drag after cancel without a new press (g_left_mouse_down
    // is cleared). Release/hover alone must not toggle or mutate the preserved range.
    static_cast<void>(handle(cancel_state, mouse_event(ava::tui::terminal::Key::MouseLeftRelease, 2, 10), cancel_snapshot_presentation, cancel_cache,
                             &cancel_draft, cancel_scroll, toggle_cancel, never_toggle));
    expect(!cancel_state.dragging() && cancel_toggles == 0 && cancel_state.range() && cancel_state.range()->focus.display_column == focus_before,
           "release after cancel cannot toggle or extend a previously cancelled interaction");
    static_cast<void>(handle(cancel_state, mouse_event(ava::tui::terminal::Key::MousePointerCancel, 2, 10), cancel_snapshot_presentation, cancel_cache,
                             &cancel_draft, cancel_scroll, toggle_cancel, never_toggle));
    expect(cancel_state.range() && cancel_state.range()->focus.display_column == focus_before, "repeated pointer cancel preserves the committed range");
  }

  // SEL-003: endpoint hit-test uses direct upper_bound on const message_starts (large layout).
  {
    constexpr std::size_t kItems = 4096;
    ava::tui::detail::TranscriptLayout large;
    large.lines.reserve(kItems);
    large.message_starts.reserve(kItems);
    large.content_starts.reserve(kItems);
    large.message_item_indices.reserve(kItems);
    large.block_boundaries.reserve(kItems + 1);
    large.block_boundaries.push_back(0);
    for (std::size_t index = 0; index < kItems; ++index)
    {
      large.lines.push_back("row-" + std::to_string(index));
      large.message_starts.push_back(index);
      large.content_starts.push_back(index);
      large.message_item_indices.push_back(index);
      large.block_boundaries.push_back(index + 1);
    }
    auto const mid = ava::tui::endpoint_for_absolute_line(large, kItems / 2, 1);
    auto const last = ava::tui::endpoint_for_absolute_line(large, kItems - 1, 0);
    expect(mid && mid->item_index == kItems / 2 && mid->line_offset == 0 && last && last->item_index == kItems - 1,
           "endpoint hit-test upper_bounds directly on large const message_starts without copying");
    expect(ava::tui::shift_transcript_selection_item_index(5, -1) == std::size_t{4} && !ava::tui::shift_transcript_selection_item_index(0, -1) &&
               ava::tui::shift_transcript_selection_item_index(2, 3) == std::size_t{5},
           "selection item index shift maps forward and fails closed on leading eviction");
  }

  // SEL-004 production path: oversize copy_selection sets the allowlisted status string.
  // Establish a multi-row selection on short rows (stable geometry), then grow those rows
  // under the same item ownership so middle full rows push the extract past 64 KiB.
  {
    ava::tui::RuntimePresentationState copy_snapshot_presentation(frontend.frontend(), {});
    auto const& copy_snapshot = copy_snapshot_presentation.screen();
    copy_snapshot_presentation.publish_dimensions(40, copy_snapshot.height);
    copy_snapshot_presentation.publish_dimensions(copy_snapshot.width, 10);
    change_transcript(copy_snapshot_presentation, [&](auto& transcript) { transcript.resize(1); });
    change_transcript(copy_snapshot_presentation, [&](auto& transcript) { transcript[0].text = "body"; });
    constexpr std::size_t kRows = 5;
    ava::tui::detail::TranscriptLayout copy_layout;
    copy_layout.lines.assign(kRows, "short-row");
    copy_layout.block_boundaries = {0, kRows};
    copy_layout.message_starts = {0};
    copy_layout.content_starts = {0};
    copy_layout.message_item_indices = {0};
    auto copy_cache = selection_cache(copy_layout, 30);
    ava::tui::RuntimeTranscriptSelectionState copy_state;
    ava::tui::Composer copy_draft;
    std::size_t copy_scroll = 0;
    static_cast<void>(handle(copy_state, mouse_event(ava::tui::terminal::Key::MouseLeftPress, 1, 2), copy_snapshot_presentation, copy_cache, &copy_draft,
                             copy_scroll, never_toggle, never_toggle));
    static_cast<void>(handle(copy_state, mouse_event(ava::tui::terminal::Key::MouseLeftDrag, 5, 8), copy_snapshot_presentation, copy_cache, &copy_draft,
                             copy_scroll, never_toggle, never_toggle));
    static_cast<void>(handle(copy_state, mouse_event(ava::tui::terminal::Key::MouseLeftRelease, 5, 8), copy_snapshot_presentation, copy_cache, &copy_draft,
                             copy_scroll, never_toggle, never_toggle));
    expect(copy_state.range().has_value(), "multi-row selection range is established before oversize growth");
    copy_cache.layout.lines.assign(kRows, std::string(20 * 1024, 'x'));
    copy_cache.transcript_generation = 31;
    expect(copy_state.ensure_authority(copy_cache, &copy_snapshot) && copy_state.range().has_value(),
           "selection survives same-item layout payload growth used for oversize copy");
    expect(!copy_state.copy_selection(copy_snapshot_presentation, copy_cache) && copy_snapshot.status == "selection too large to copy",
           "copy_selection reports selection too large to copy for oversize payloads");
    auto const copy_frame = ava::tui::render_composer(frontend.frontend(), copy_snapshot);
    expect(std::ranges::any_of(copy_frame, [](std::string const& line) { return strip_sgr(line).find("selection too large to copy") != std::string::npos; }),
           "oversize copy failure status is visible in the terminal frame");
  }

  // Production copy path over a frozen detached authority: geometry may cross the
  // private row, but the OSC52 payload must be built only from ordinary rows.
  {
    ScopedEnvVar tmux("TMUX", "");
    ava::tui::RuntimePresentationState copy_snapshot_presentation(frontend.frontend(), {});
    auto const& copy_snapshot = copy_snapshot_presentation.screen();
    copy_snapshot_presentation.publish_dimensions(40, copy_snapshot.height);
    copy_snapshot_presentation.publish_dimensions(copy_snapshot.width, 10);
    copy_snapshot_presentation.publish_transcript({private_launch_selection_task()});
    auto frozen_private_layout = ava::tui::detail::TranscriptLayout{.lines = {"ordinary header", "PRIVATE FROZEN LAUNCH", "ordinary following"},
                                                                    .presentation_private_rows = {false, true, false},
                                                                    .block_boundaries = {0, 3},
                                                                    .message_starts = {0},
                                                                    .content_starts = {0},
                                                                    .message_item_indices = {1}};
    auto frozen_private_cache = selection_cache(std::move(frozen_private_layout), 40);
    ava::tui::RuntimeTranscriptSelectionState copy_state;
    ava::tui::Composer copy_draft;
    std::size_t copy_scroll = 0;
    static_cast<void>(handle(copy_state, mouse_event(ava::tui::terminal::Key::MouseLeftPress, 1, 2), copy_snapshot_presentation, frozen_private_cache,
                             &copy_draft, copy_scroll, never_toggle, never_toggle, /*frozen_to_live_shift=*/-1));
    static_cast<void>(handle(copy_state, mouse_event(ava::tui::terminal::Key::MouseLeftDrag, 3, 20), copy_snapshot_presentation, frozen_private_cache,
                             &copy_draft, copy_scroll, never_toggle, never_toggle, /*frozen_to_live_shift=*/-1));
    static_cast<void>(handle(copy_state, mouse_event(ava::tui::terminal::Key::MouseLeftRelease, 3, 20), copy_snapshot_presentation, frozen_private_cache,
                             &copy_draft, copy_scroll, never_toggle, never_toggle, /*frozen_to_live_shift=*/-1));
    auto const copy_range = copy_state.range();
    auto const expected_text =
        copy_range ? ava::tui::extract_transcript_selection_text(frozen_private_cache.layout, *copy_range, 64 * 1024).text : std::string{};
    auto const expected_sequence = ava::tui::runtime_transcript::try_build_osc52_clipboard_sequence(expected_text);

    ScopedTmpFile output;
    auto const saved_stdout = dup(STDOUT_FILENO);
    auto redirected = saved_stdout >= 0 && std::fflush(stdout) == 0 && dup2(fileno(output.get()), STDOUT_FILENO) >= 0;
    auto copied = false;
    if (redirected)
      copied = copy_state.copy_selection(copy_snapshot_presentation, frozen_private_cache);
    if (saved_stdout >= 0)
    {
      static_cast<void>(std::fflush(stdout));
      static_cast<void>(dup2(saved_stdout, STDOUT_FILENO));
      static_cast<void>(close(saved_stdout));
    }
    std::string captured;
    if (std::fflush(output.get()) == 0 && std::fseek(output.get(), 0, SEEK_END) == 0)
    {
      auto const size = std::ftell(output.get());
      if (size >= 0 && std::fseek(output.get(), 0, SEEK_SET) == 0)
      {
        captured.resize(static_cast<std::size_t>(size));
        if (!captured.empty() && std::fread(captured.data(), 1, captured.size(), output.get()) != captured.size())
          captured.clear();
      }
    }

    expect(copy_range && expected_sequence && expected_text.find("rdinary header") != std::string::npos &&
               expected_text.find("ordinary following") != std::string::npos && expected_text.find("PRIVATE FROZEN LAUNCH") == std::string::npos && copied &&
               copy_snapshot.status == "selection copy request sent" && captured == *expected_sequence,
           "frozen detached selection copy emits an OSC52 payload containing ordinary cross-row text but no private launch bytes");
  }
}

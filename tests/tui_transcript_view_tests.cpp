// Direct TUI-thread TranscriptView regressions using public transitions and test-owned layout values.
#include "sys.h"
#include "tests/support/test_harness.h"
#include "tests/support/tui_test_support.h"
#include "ava/tui/Composer.h"
#include "ava/tui/TranscriptView.h"
#include "ava/tui/runtime_state_internal.h"

#include <chrono>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace {

// Own every borrowed dependency on the test thread; reverse destruction retires
// the view before its Composer, projection, and registered offline frontend.
struct TranscriptViewFixture final
{
  // The common runner owns the initialized terminal; this fixture starts no loop or worker.
  TranscriptViewFixture() : presentation(frontend.frontend(), {}), view(presentation, composer) { presentation.publish_dimensions(40, 24); }

  // Own registered backend data until the projection and view have been destroyed.
  tui_test_support::TuiFrontEndFixture frontend;
  // Own the TUI-thread screen; tests mutate it only through production publications.
  ava::tui::RuntimePresentationState presentation;
  // Own the editor for the full lifetime of the view's borrowed reference.
  ava::tui::Composer composer;
  // Own the viewport and selection under test; all calls stay on this test thread.
  ava::tui::TranscriptView view;

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

// Build independent message identities on the test thread, returning owned source values.
std::vector<ava::tui::TranscriptItem> transcript_items(std::size_t count)
{
  std::vector<ava::tui::TranscriptItem> items;
  for (std::size_t index = 0; index < count; ++index)
    items.push_back({.label = "ava", .text = "message " + std::to_string(index)});
  return items;
}

// Copy a two-row layout per source item on the test thread; the cache borrows no screen data.
ava::tui::detail::TranscriptLayoutCache layout_for(ava::tui::ComposerSnapshot const& screen)
{
  ava::tui::detail::TranscriptLayoutCache cache;
  cache.transcript_generation = screen.transcript_generation;
  cache.width = screen.width;
  cache.valid = true;
  for (std::size_t index = 0; index < screen.transcript.size(); ++index)
  {
    auto const start = cache.layout.lines.size();
    cache.layout.block_boundaries.push_back(start);
    cache.layout.message_starts.push_back(start);
    cache.layout.content_starts.push_back(start);
    cache.layout.message_item_indices.push_back(index);
    cache.layout.lines.push_back(screen.transcript[index].text);
    cache.layout.lines.push_back("continuation");
  }
  cache.layout.block_boundaries.push_back(cache.layout.lines.size());
  return cache;
}

// Plain messages have no expandable headers; this test-thread callback retains nothing.
bool never_toggle(std::size_t)
{
  return false;
}

// Deliver one report on the test thread; layout is borrowed only for this public API call.
ava::tui::TranscriptSelectionMouseResult send_mouse(TranscriptViewFixture& fixture, ava::tui::detail::TranscriptLayoutCache const& layout,
                                                    ava::tui::terminal::Key key, std::size_t row, std::size_t column)
{
  return fixture.view.handle_mouse({.key = key, .mouse_column = column, .mouse_row = row}, layout, never_toggle, never_toggle);
}

// Commit a test-thread range away from viewport edges; fixture and cache are borrowed only for this call.
bool select_row(TranscriptViewFixture& fixture, ava::tui::detail::TranscriptLayoutCache const& layout, std::size_t row)
{
  using ava::tui::TranscriptSelectionMouseResult;
  using ava::tui::terminal::Key;
  auto const press = send_mouse(fixture, layout, Key::MouseLeftPress, row, 2);
  auto const drag = send_mouse(fixture, layout, Key::MouseLeftDrag, row, 6);
  auto const release = send_mouse(fixture, layout, Key::MouseLeftRelease, row, 6);
  fixture.view.publish_selection(layout, ava::tui::TranscriptSelectionProjection::Live);
  return ava::tests::require(press == TranscriptSelectionMouseResult::HandledNeedsRender && drag == TranscriptSelectionMouseResult::HandledNeedsRender &&
                                 release == TranscriptSelectionMouseResult::HandledNeedsRender && fixture.view.has_selection(),
                             "transcript view fixture commits a range through public pointer input");
}

// Queue detached output on the test thread; the production token retains no layout borrow.
void detach_with_output(TranscriptViewFixture& fixture)
{
  fixture.view.scroll_up(3, 8);
  fixture.view.complete_transcript_update(ava::tui::TranscriptUpdate(true, {.valid = true, .content_relative = true, .item_index = 0}), 0, 2);
}

// Verify delayed badge publication, frozen sidebar ownership, and the three tail transitions on one test thread.
void test_detached_chrome_and_tail_transitions()
{
  TranscriptViewFixture fixture;
  auto const& screen = fixture.presentation.screen();
  fixture.presentation.publish_activity_item({.id = "responding", .label = "before detachment"});
  fixture.view.begin_frame();
  detach_with_output(fixture);
  fixture.presentation.publish_activity_item({.id = "responding", .label = "new live activity"});
  fixture.view.freeze_sidebar();
  expect(fixture.view.scroll_offset() == 3 && fixture.view.has_deferred_update() && screen.transcript_scroll_offset == 3 &&
             screen.transcript_new_output_count == 0,
         "detached output updates scroll immediately while its badge waits for a frame");

  fixture.view.begin_frame();
  expect(screen.transcript_new_output_count == 2 && screen.sidebar && screen.sidebar->activity.size() == 1 &&
             screen.sidebar->activity.front().label == "before detachment" && fixture.presentation.sidebar().activity.size() == 1 &&
             fixture.presentation.sidebar().activity.front().label == "new live activity",
         "detached frames publish the badge and first captured sidebar despite newer live activity");

  fixture.view.request_live_tail();
  expect(fixture.view.scroll_offset() == 0 && fixture.view.has_deferred_update() && screen.transcript_new_output_count == 2 && screen.sidebar &&
             !screen.sidebar->activity.empty() && screen.sidebar->activity.front().label == "before detachment",
         "request_live_tail retains detached chrome and deferred work until the frame boundary");
  // Re-enter history before painting: retained state must still be observable.
  fixture.view.scroll_up(3, 8);
  fixture.view.begin_frame();
  expect(screen.transcript_new_output_count == 2 && screen.sidebar && !screen.sidebar->activity.empty() &&
             screen.sidebar->activity.front().label == "before detachment" && fixture.view.has_deferred_update(),
         "an unpainted live-tail request retains its frozen sidebar and output count");
  fixture.view.request_live_tail();
  fixture.view.begin_frame();
  expect(!fixture.view.has_deferred_update() && screen.transcript_scroll_offset == 0 && screen.transcript_new_output_count == 0 && screen.sidebar &&
             screen.sidebar->activity.size() == 1 && screen.sidebar->activity.front().label == "new live activity",
         "begin_frame commits a live-tail request and publishes the current sidebar");
  fixture.view.scroll_up(3, 8);
  fixture.view.begin_frame();
  expect(screen.transcript_new_output_count == 0 && screen.sidebar && !screen.sidebar->activity.empty() &&
             screen.sidebar->activity.front().label == "new live activity",
         "a later detachment cannot revive count or sidebar retired by the live-tail frame");

  fixture.view.complete_transcript_update(ava::tui::TranscriptUpdate(true, {.valid = true, .content_relative = true, .item_index = 0}), 0, 2);
  fixture.presentation.publish_activity_item({.id = "responding", .label = "after local command"});
  fixture.view.reset_for_local_command();
  expect(fixture.view.scroll_offset() == 0 && fixture.view.has_deferred_update(),
         "local-command reset clears detached count and sidebar immediately but retains layout work");
  // A zero-offset frame would clear stale state itself and hide a broken reset.
  fixture.view.scroll_up(3, 8);
  fixture.view.begin_frame();
  expect(screen.transcript_new_output_count == 0 && screen.sidebar && !screen.sidebar->activity.empty() &&
             screen.sidebar->activity.front().label == "after local command" && fixture.view.has_deferred_update(),
         "re-entering history proves local-command count and sidebar cleanup before a live-tail frame");

  fixture.view.return_to_live_tail();
  detach_with_output(fixture);
  fixture.presentation.publish_activity_item({.id = "responding", .label = "after explicit return"});
  fixture.view.return_to_live_tail();
  fixture.view.publish_chrome();
  expect(fixture.view.scroll_offset() == 0 && !fixture.view.has_deferred_update() && screen.transcript_scroll_offset == 0 &&
             screen.transcript_new_output_count == 0,
         "explicit return_to_live_tail also discards the pending anchor before chrome publication");
  fixture.view.scroll_up(3, 8);
  fixture.view.begin_frame();
  expect(screen.transcript_new_output_count == 0 && screen.sidebar && !screen.sidebar->activity.empty() &&
             screen.sidebar->activity.front().label == "after explicit return" && !fixture.view.has_deferred_update(),
         "explicit return retires internal count and sidebar before another detached frame");
}

// Synchronize two leading evictions against one retained anchor and committed selection on the test thread.
void test_first_deferred_anchor_and_accumulated_shifts()
{
  TranscriptViewFixture fixture;
  auto const& screen = fixture.presentation.screen();
  auto items = transcript_items(5);
  fixture.presentation.publish_transcript(items);
  auto const original_layout = layout_for(screen);
  if (!select_row(fixture, original_layout, 7))
    return;
  fixture.view.scroll_up(3, 8);

  items.erase(items.begin());
  fixture.presentation.publish_transcript(items);
  fixture.view.complete_transcript_update(ava::tui::TranscriptUpdate(true, {.valid = true, .content_relative = true, .item_index = 3, .line_offset = 0}), -1,
                                          1);
  items.erase(items.begin());
  fixture.presentation.publish_transcript(std::move(items));
  fixture.view.complete_transcript_update(ava::tui::TranscriptUpdate(true, {.valid = true, .content_relative = true, .item_index = 4, .line_offset = 1}), -1,
                                          2);
  fixture.view.publish_selection(original_layout, ava::tui::TranscriptSelectionProjection::Frozen);
  expect(fixture.view.has_deferred_update() && screen.transcript_new_output_count == 0 && screen.transcript_selection_anchor_item == 3,
         "deferred output keeps selection bound to its painted item indices until synchronization");

  auto const current_layout = layout_for(screen);
  fixture.view.synchronize_deferred(current_layout, 4);
  fixture.view.publish_selection(current_layout, ava::tui::TranscriptSelectionProjection::Live);
  expect(!fixture.view.has_deferred_update() && fixture.view.scroll_offset() == 2 && screen.transcript_scroll_offset == 2 &&
             screen.transcript_new_output_count == 3 && fixture.view.has_selection() && screen.transcript_selection_anchor_item == 1 &&
             screen.transcript_selection_focus_item == 1 && screen.transcript_selection_anchor_column == 1 && screen.transcript_selection_focus_column == 5,
         "synchronization preserves the first anchor and applies both eviction shifts to viewport and selection");
  fixture.view.synchronize_deferred(current_layout, 4);
  fixture.view.publish_selection(current_layout, ava::tui::TranscriptSelectionProjection::Live);
  expect(fixture.view.scroll_offset() == 2 && screen.transcript_selection_anchor_item == 1,
         "synchronizing a retired deferred update cannot apply its eviction shift twice");
}

// Exercise projection policies and pointer cancellation without borrowing mutable selection authority.
void test_selection_projection_and_pointer_cancel()
{
  TranscriptViewFixture fixture;
  auto const& screen = fixture.presentation.screen();
  auto items = transcript_items(3);
  fixture.presentation.publish_transcript(items);
  auto const original_layout = layout_for(screen);
  if (!select_row(fixture, original_layout, 5))
    return;

  fixture.composer.begin_pointer_selection(0, true);
  if (!ava::tests::require(fixture.composer.mouse_selecting(), "composer fixture arms a competing pointer interaction"))
    return;
  auto const canceled = send_mouse(fixture, original_layout, ava::tui::terminal::Key::MousePointerCancel, 0, 0);
  expect(canceled == ava::tui::TranscriptSelectionMouseResult::HandledNeedsRender && !fixture.composer.mouse_selecting() && fixture.view.has_selection() &&
             screen.transcript_selection_anchor_item == 2 && screen.transcript_selection_focus_item == 2 && screen.transcript_selection_anchor_column == 1 &&
             screen.transcript_selection_focus_column == 5,
         "pointer cancellation retires the competing drag while preserving the committed transcript range");

  items.erase(items.begin());
  fixture.presentation.publish_transcript(std::move(items));
  fixture.view.complete_transcript_update(ava::tui::TranscriptUpdate(false, {}), -1, 1);
  fixture.view.publish_selection(original_layout, ava::tui::TranscriptSelectionProjection::Frozen);
  expect(screen.transcript_selection_anchor_item == 2 && screen.transcript_selection_focus_item == 2,
         "Frozen selection publication retains painted indices and leaves live eviction debt pending");
  auto const current_layout = layout_for(screen);
  fixture.view.publish_selection(current_layout, ava::tui::TranscriptSelectionProjection::Live);
  expect(fixture.view.has_selection() && screen.transcript_selection_anchor_item == 1 && screen.transcript_selection_focus_item == 1,
         "Live selection publication applies pending eviction debt against current source authority");

  fixture.view.publish_selection(current_layout, ava::tui::TranscriptSelectionProjection::Suppressed);
  expect(!fixture.view.has_selection() && screen.transcript_selection_anchor_item == std::string::npos &&
             screen.transcript_selection_focus_item == std::string::npos && !fixture.view.autoscroll_deadline(),
         "Suppressed publication clears selection authority and its projected highlight");
}

// Advance only the view's supplied monotonic clock, then retire all old-session state on the same thread.
void test_indicator_renewal_expiry_and_session_reset()
{
  TranscriptViewFixture fixture;
  auto const& screen = fixture.presentation.screen();
  auto const started = ava::tui::TranscriptView::Clock::time_point{};
  fixture.view.show_position_indicator(started);
  auto const first_deadline = fixture.view.indicator_deadline();
  expect(first_deadline && *first_deadline == started + std::chrono::seconds(1) && screen.transcript_position_indicator_visible,
         "show_position_indicator publishes visibility and a one-second semantic deadline");
  fixture.view.show_position_indicator(started + std::chrono::milliseconds(500));
  auto const renewed_deadline = fixture.view.indicator_deadline();
  expect(renewed_deadline && *renewed_deadline == started + std::chrono::milliseconds(1500) &&
             !fixture.view.expire_position_indicator(started + std::chrono::seconds(1)) && screen.transcript_position_indicator_visible,
         "renewal prevents the earlier indicator deadline from hiding the current indicator");
  expect(fixture.view.expire_position_indicator(started + std::chrono::milliseconds(1500)) && !fixture.view.indicator_deadline() &&
             !screen.transcript_position_indicator_visible && !fixture.view.expire_position_indicator(started + std::chrono::seconds(2)),
         "indicator expiry changes publication once at the renewed deadline");

  fixture.presentation.publish_transcript(transcript_items(3));
  auto const layout = layout_for(screen);
  if (!select_row(fixture, layout, 3))
    return;
  fixture.presentation.publish_activity_item({.id = "reset", .label = "before reset"});
  detach_with_output(fixture);
  fixture.presentation.publish_activity_item({.id = "reset", .label = "after reset"});
  fixture.view.show_position_indicator(started + std::chrono::seconds(3));
  fixture.view.begin_frame();
  expect(screen.transcript_new_output_count == 2 && screen.sidebar && !screen.sidebar->activity.empty() &&
             screen.sidebar->activity.front().label == "before reset",
         "session reset fixture publishes old detached count and sidebar before retirement");
  fixture.view.reset_for_session_transition();
  expect(!fixture.view.has_selection() && !fixture.view.has_deferred_update() && fixture.view.scroll_offset() == 0 && !fixture.view.indicator_deadline() &&
             !fixture.view.autoscroll_deadline() && screen.transcript_selection_anchor_item == std::string::npos &&
             screen.transcript_selection_focus_item == std::string::npos && screen.transcript_scroll_offset == 0 && screen.transcript_new_output_count == 0 &&
             !screen.transcript_position_indicator_visible,
         "session transition retires old viewport, selection, detached chrome, and indicator state together");
  // Re-detach before paint so begin_frame cannot repair a reset that left old state behind.
  fixture.view.scroll_up(3, 8);
  fixture.view.begin_frame();
  expect(
      screen.transcript_new_output_count == 0 && screen.sidebar && !screen.sidebar->activity.empty() && screen.sidebar->activity.front().label == "after reset",
      "session reset prevents old count or sidebar from reappearing on the next detachment");
}

}  // namespace

// Run direct owner cases on the runner's initialized terminal thread; every fixture dies before the next case.
void run_tui_transcript_view_tests()
{
  test_detached_chrome_and_tail_transitions();
  test_first_deferred_anchor_and_accumulated_shifts();
  test_selection_projection_and_pointer_cancel();
  test_indicator_renewal_expiry_and_session_reset();
}

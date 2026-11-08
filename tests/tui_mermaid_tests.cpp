#include "sys.h"
#include "support/TuiEventLoopFixture.h"
#include "tests/support/test_harness.h"
#include "tests/support/tui_test_support.h"
#include "ava/tui/Composer.h"
#include "ava/tui/SelectorHost.h"
#include "ava/tui/TuiEventLoop.h"
#include "ava/tui/TuiFrontEnd.h"
#include "ava/tui/composer.h"
#include "ava/tui/composer_internal.h"
#include "ava/tui/mermaid_projection.h"
#include "ava/tui/runtime_mermaid_internal.h"
#include "ava/tui/runtime_navigation_internal.h"
#include "ava/tui/runtime_render_internal.h"
#include "ava/tui/runtime_state_internal.h"
#include "ava/tui/runtime_transcript_search_internal.h"
#include "ava/tui/runtime_transcript_selection_internal.h"
#include "ava/core/UniqueFd.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <ranges>
#include <string>
#include <system_error>
#include <utility>
#include <vector>
#include <unistd.h>

namespace {

using ava::tui::detail::MermaidAcceptedPresentation;
using ava::tui::detail::MermaidTranscriptProjection;

std::shared_ptr<MermaidTranscriptProjection const> accepted_projection(ava::tui::TranscriptItem const& item, std::size_t item_index, std::uint64_t epoch,
                                                                       std::string text)
{
  auto blocks = ava::tui::detail::scan_completed_assistant_mermaid_fences(item);
  if (blocks.empty())
    return {};
  auto const& block = blocks.front();
  return std::make_shared<MermaidTranscriptProjection const>(epoch, std::vector<MermaidAcceptedPresentation>{{.config_epoch = epoch,
                                                                                                              .item_identity = 17,
                                                                                                              .block_identity = 23,
                                                                                                              .item_index = item_index,
                                                                                                              .block_index = block.block_index,
                                                                                                              .fence_start = block.fence_start,
                                                                                                              .fence_end = block.fence_end,
                                                                                                              .source = block.source,
                                                                                                              .text = std::move(text)}});
}

void test_mermaid_fence_scanner_matrix()
{
  auto completed = ava::tui::TranscriptItem{.label = "ava", .text = "before\n```  MeRmAiD extra-token\ngraph TD\nA-->B\n``` trailing\nafter"};
  auto blocks = ava::tui::detail::scan_completed_assistant_mermaid_fences(completed);
  expect(blocks.size() == 1 && blocks.front().block_index == 0 && blocks.front().source == "graph TD\nA-->B\n" &&
             completed.text.substr(blocks.front().fence_start, blocks.front().fence_end - blocks.front().fence_start) ==
                 "```  MeRmAiD extra-token\ngraph TD\nA-->B\n``` trailing\n",
         "scanner accepts a completed case-insensitive first Mermaid info token and AVA-compatible closing marker variants");

  std::vector<ava::tui::TranscriptItem> excluded{
      {.label = "ava", .text = "```mermaid-extra\ngraph TD\n```"},
      {.label = "ava", .text = "inline ```mermaid graph TD```"},
      {.label = "ava", .text = "```mermaid\ngraph TD"},
      {.label = "you", .text = "```mermaid\ngraph TD\n```"},
      {.label = "tool", .text = "```mermaid\ngraph TD\n```"},
      {.label = "thinking", .text = "```mermaid\ngraph TD\n```"},
      {.label = "ava", .text = "~~~mermaid\ngraph TD\n~~~"},
      {.label = "ava", .text = "```cpp\n```mermaid\n```\n```"},
  };
  excluded.push_back(ava::tui::TranscriptItem{.label = "ava", .text = "```mermaid\n" + std::string(64 * 1024 + 1, 'x') + "\n```"});
  auto live = completed;
  live.stream_id = "live";
  live.append_only_stream = true;
  excluded.push_back(std::move(live));
  auto const all_excluded =
      std::ranges::all_of(excluded, [](auto const& item) { return ava::tui::detail::scan_completed_assistant_mermaid_fences(item).empty(); });
  expect(all_excluded, "scanner excludes suffix, inline, unclosed, user/tool/thinking, tilde, nested, oversized, and append-only streaming sources");

  auto stream_identity_only = completed;
  stream_identity_only.stream_id = "still-live";
  auto carriage_returns = ava::tui::TranscriptItem{.label = "ava", .text = "```MERMAID\rgraph LR\rA-->B\r```\r"};
  auto carriage_return_blocks = ava::tui::detail::scan_completed_assistant_mermaid_fences(carriage_returns);
  auto exact_limit = ava::tui::TranscriptItem{.label = "ava", .text = "```mermaid\n" + std::string(64 * 1024 - 1, 'x') + "\n```"};
  expect(ava::tui::detail::scan_completed_assistant_mermaid_fences(stream_identity_only).empty() && carriage_return_blocks.size() == 1 &&
             carriage_return_blocks.front().source == "graph LR\rA-->B\r" && ava::tui::detail::scan_completed_assistant_mermaid_fences(exact_limit).size() == 1,
         "scanner rejects retained live stream identities while accepting CR line endings and an exact 64-KiB source");
}

void test_projection_rendering_is_nonrecursive_and_shared_by_geometry()
{
  ava::tui::TranscriptItem item{.label = "ava", .text = "intro\n```mermaid\nSOURCE_ONLY-->B\n```\noutro"};
  std::vector<ava::tui::TranscriptItem> transcript{item};
  auto const ordinary = ava::tui::detail::render_transcript_layout(transcript, 48);
  auto projection = accepted_projection(item, 0, 7, "**HELPER_LITERAL**\n```not-a-fence\nplain helper row");
  auto const projected = ava::tui::detail::render_transcript_layout(transcript, 48, ava::tui::ToolPresentation::Rich, true, false, projection.get());
  auto const projected_plain = tui_test_support::join_plain_lines(projected.lines);
  expect(projected_plain.find("HELPER_LITERAL") != std::string::npos && projected_plain.find("**HELPER_LITERAL**") != std::string::npos &&
             projected_plain.find("```not-a-fence") != std::string::npos && projected_plain.find("SOURCE_ONLY") == std::string::npos &&
             projected_plain.find("```mermaid") == std::string::npos,
         "accepted helper text replaces only the fence and renders as literal preformatted text without recursive Markdown or ANSI parsing");

  ava::tui::detail::TranscriptLayoutCache cache;
  ava::tui::detail::refresh_transcript_layout_cache(cache, transcript, 11, 48, ava::tui::ToolPresentation::Rich, true, false, projection.get());
  auto const tail = ava::tui::detail::render_transcript_tail_lines(transcript, 48, 100, ava::tui::ToolPresentation::Rich, true, false, projection.get());
  auto const starts = ava::tui::detail::transcript_message_start_lines(transcript, 48, ava::tui::ToolPresentation::Rich, true, false, projection.get());
  ava::tui::ComposerSnapshot search_snapshot;
  search_snapshot.transcript = transcript;
  search_snapshot.mermaid_enabled = true;
  search_snapshot.mermaid_config_epoch = 7;
  search_snapshot.mermaid_projection = projection;
  auto helper_matches = ava::tui::detail::build_transcript_search_matches(search_snapshot, projected, "helper_literal");
  auto source_matches = ava::tui::detail::build_transcript_search_matches(search_snapshot, projected, "source_only");
  auto helper_line = std::ranges::find_if(projected.lines, [](std::string const& line) { return line.find("HELPER_LITERAL") != std::string::npos; });
  auto endpoint = helper_line == projected.lines.end()
                      ? std::optional<ava::tui::TranscriptSelectionEndpoint>{}
                      : ava::tui::endpoint_for_absolute_line(projected, static_cast<std::size_t>(helper_line - projected.lines.begin()), 2);
  auto selection_unit = endpoint ? ava::tui::transcript_line_selection_unit(projected, *endpoint) : std::optional<ava::tui::TranscriptSelectionUnit>{};
  auto selection = selection_unit
                       ? ava::tui::extract_transcript_selection_text(projected, {.anchor = selection_unit->start, .focus = selection_unit->end}, 1024)
                       : ava::tui::TranscriptSelectionExtractResult{};
  auto highlighted = projected.lines;
  if (selection_unit)
    ava::tui::apply_transcript_selection_overlay(highlighted, projected, {.anchor = selection_unit->start, .focus = selection_unit->end}, 0, false);
  expect(cache.layout.lines == projected.lines && tail == projected.lines && starts == projected.message_starts && helper_matches.size() == 1 &&
             source_matches.empty() && endpoint && endpoint->item_index == 0 && selection.text.find("HELPER_LITERAL") != std::string::npos &&
             highlighted != projected.lines,
         "full, tail, cache, message starts, search, and selection extraction/highlight/hit geometry share the projection-aware layout authority");

  auto active_transcript = transcript;
  active_transcript.push_back({.label = "ava", .text = "live stream row", .stream_id = "active-stream", .append_only_stream = true});
  ava::tui::detail::TranscriptTailRenderCache tail_cache;
  static_cast<void>(ava::tui::detail::render_transcript_tail_lines_cached(tail_cache, active_transcript, 20, 48, 100));
  auto projected_active_tail = ava::tui::detail::render_transcript_tail_lines_cached(tail_cache, active_transcript, 21, 48, 100,
                                                                                     ava::tui::ToolPresentation::Rich, true, false, projection.get());
  auto const projected_active_plain = tui_test_support::join_plain_lines(projected_active_tail);
  expect(projected_active_plain.find("HELPER_LITERAL") != std::string::npos && projected_active_plain.find("SOURCE_ONLY") == std::string::npos,
         "projection publication invalidates the append-only tail cache even while a later assistant item is actively streaming");

  auto block = ava::tui::detail::scan_completed_assistant_mermaid_fences(item).front();
  auto stale = std::make_shared<MermaidTranscriptProjection const>(7, std::vector<MermaidAcceptedPresentation>{{.config_epoch = 6,
                                                                                                                .item_identity = 29,
                                                                                                                .block_identity = 31,
                                                                                                                .item_index = 0,
                                                                                                                .block_index = block.block_index,
                                                                                                                .fence_start = block.fence_start,
                                                                                                                .fence_end = block.fence_end,
                                                                                                                .source = block.source,
                                                                                                                .text = "STALE_HELPER"}});
  auto const stale_layout = ava::tui::detail::render_transcript_layout(transcript, 48, ava::tui::ToolPresentation::Rich, true, false, stale.get());
  expect(ordinary.lines == stale_layout.lines,
         "missing, stale, pending, failed, or evicted projection lookup preserves ordinary fenced-code rendering byte-for-byte");
}

// Test-thread fake backend storage; copied bridge callbacks share ownership until they retire.
struct FakeBridgeState
{
  // Test-thread admission history, retained for request identity assertions.
  std::vector<ava::tui::TuiMermaidRenderRequest> requests;
  // Test-thread result queue transferred by the fake drain callback.
  std::vector<ava::tui::TuiMermaidRenderCompletion> completions;
  // Test-thread cancellation identities recorded without running a backend worker.
  std::vector<std::pair<std::uint64_t, std::uint64_t>> cancellations;
  // Test thread sets admission pressure before asking the controller to enqueue.
  bool queue_full = false;
};

ava::tui::TuiMermaidRenderBridge fake_bridge(std::shared_ptr<FakeBridgeState> state, std::uint64_t epoch = 1)
{
  return ava::tui::TuiMermaidRenderBridge{
      .config_epoch = epoch,
      .enabled = true,
      .enqueue =
          [state](ava::tui::TuiMermaidRenderRequest request) {
            if (state->queue_full)
              return ava::tui::TuiMermaidEnqueueResult::QueueFull;
            state->requests.push_back(std::move(request));
            return ava::tui::TuiMermaidEnqueueResult::Accepted;
          },
      .cancel =
          [state](std::uint64_t identity, std::uint64_t config_epoch) {
            state->cancellations.emplace_back(identity, config_epoch);
            return true;
          },
      .drain =
          [state] {
            auto completions = std::move(state->completions);
            state->completions.clear();
            return completions;
          },
  };
}

// Protect diagram identity reconciliation so stale results never replace current content.
void test_runtime_projection_reconciliation_and_stale_fail_closed()
{
  auto state = std::make_shared<FakeBridgeState>();
  tui_test_support::TuiFrontEndFixture frontend;
  ava::tui::RuntimePresentationState presentation(frontend.frontend(), {});
  auto const& snapshot = presentation.screen();
  ava::tui::Composer composer;
  tui_test_support::TuiEventLoopFixture event_loop;
  ava::tui::RuntimeRenderer renderer(event_loop.loop(), frontend.frontend(), presentation, composer);
  ava::tui::RuntimeNavigationController navigation(*frontend.frontend(), presentation, composer, renderer);
  ava::tui::SelectorHost selectors(presentation);
  ava::tui::TranscriptSearchController search(presentation, renderer, navigation, selectors);
  ava::tui::RuntimeMermaidPresentationController controller(presentation, renderer, search, fake_bridge(state));
  presentation.finish_session_transition("session-a");
  auto display = frontend.frontend()->display_settings_snapshot();
  display.mermaid_enabled = true;
  display.mermaid_config_epoch = 1;
  presentation.apply_local_display_settings(display);
  presentation.publish_transcript({{.label = "ava", .text = "```mermaid\ngraph TD; A-->B\n```"}});
  auto const generation = snapshot.transcript_generation;
  auto pending = controller.service();
  expect(pending == ava::tui::MermaidServiceOutcome::Unchanged && state->requests.size() == 1 && snapshot.transcript_generation == generation &&
             !snapshot.mermaid_projection,
         "reconciliation enqueues eligible finalized blocks without changing fallback geometry while pending");

  auto const request = state->requests.front();
  state->completions.push_back({.identity = request.identity, .config_epoch = 1, .accepted = true, .text = "rendered A to B"});
  auto accepted = controller.service();
  auto const projected = snapshot.mermaid_projection;
  expect(accepted == ava::tui::MermaidServiceOutcome::Changed && snapshot.transcript_generation == generation + 1 && projected &&
             projected->accepted().size() == 1 && projected->accepted().front().item_index == 0,
         "accepted completion publishes one bounded presentation result and advances transcript geometry generation");

  auto fallback_before = ava::tui::detail::render_transcript_layout(snapshot.transcript, 60);
  display.mermaid_config_epoch = 2;
  presentation.apply_local_display_settings(display);
  state->completions.push_back({.identity = request.identity, .config_epoch = 1, .accepted = true, .text = "stale result"});
  auto reconfigured = controller.service();
  auto fallback_after = ava::tui::detail::render_transcript_layout(snapshot.transcript, 60, ava::tui::ToolPresentation::Rich, true, false,
                                                                   ava::tui::detail::active_mermaid_projection(snapshot));
  expect(reconfigured == ava::tui::MermaidServiceOutcome::Changed && !snapshot.mermaid_projection && fallback_after.lines == fallback_before.lines &&
             state->requests.size() == 2,
         "configuration epoch changes remove old projections, ignore stale completions, and immediately requeue under the new epoch");

  auto streaming = snapshot.transcript;
  streaming.front().append_only_stream = true;
  streaming.front().stream_id = "live";
  presentation.publish_transcript(std::move(streaming));
  auto const request_count = state->requests.size();
  static_cast<void>(controller.service());
  expect(state->requests.size() == request_count && !state->cancellations.empty(),
         "an item becoming append-only cancels its prior identity and is never enqueued or projected while streaming");

  controller.shutdown();
}

// Protect bounded diagram projection and placement against a detached transcript anchor.
void test_runtime_projection_bounds_cap_and_detached_anchor_geometry()
{
  auto state = std::make_shared<FakeBridgeState>();
  tui_test_support::TuiFrontEndFixture frontend;
  ava::tui::RuntimePresentationState presentation(frontend.frontend(), {});
  auto const& snapshot = presentation.screen();
  ava::tui::Composer composer;
  tui_test_support::TuiEventLoopFixture event_loop;
  ava::tui::RuntimeRenderer renderer(event_loop.loop(), frontend.frontend(), presentation, composer);
  ava::tui::RuntimeNavigationController navigation(*frontend.frontend(), presentation, composer, renderer);
  ava::tui::SelectorHost selectors(presentation);
  ava::tui::TranscriptSearchController search(presentation, renderer, navigation, selectors);
  ava::tui::RuntimeMermaidPresentationController controller(presentation, renderer, search, fake_bridge(state));
  presentation.finish_session_transition("bounded");
  auto display = frontend.frontend()->display_settings_snapshot();
  display.mermaid_enabled = true;
  display.mermaid_config_epoch = 1;
  presentation.apply_local_display_settings(display);
  for (std::size_t index = 0; index < 40; ++index)
    static_cast<void>(presentation.append_transcript({.label = "ava", .text = "```mermaid\ngraph TD; A" + std::to_string(index) + "-->B\n```"}));
  static_cast<void>(controller.service());
  expect(state->requests.size() == 32, "TUI reconciliation never exceeds the coordinator's 32 undrained identity bound");

  auto const evicted_request = state->requests.front();
  auto retained = snapshot.transcript;
  retained.erase(retained.begin());
  presentation.publish_transcript(std::move(retained));
  static_cast<void>(controller.service());
  state->completions.push_back({.identity = evicted_request.identity, .config_epoch = 1, .accepted = true, .text = "must stay stale after leading eviction"});
  static_cast<void>(controller.service());
  expect(
      state->requests.size() == 33 && !state->cancellations.empty() && state->cancellations.front().first == evicted_request.identity &&
          !snapshot.mermaid_projection,
      "leading transcript eviction preserves retained identities, cancels the evicted pending block, fills one freed slot, and ignores its stale completion");

  auto anchor_item = ava::tui::TranscriptItem{.label = "ava", .text = "before\n```mermaid\nsource\n```\nafter"};
  std::vector<ava::tui::TranscriptItem> transcript;
  for (std::size_t index = 0; index < 12; ++index)
    transcript.push_back({.label = "ava", .text = "older " + std::to_string(index)});
  auto const anchor_index = transcript.size();
  transcript.push_back(anchor_item);
  for (std::size_t index = 0; index < 12; ++index)
    transcript.push_back({.label = "ava", .text = "newer " + std::to_string(index)});
  auto const ordinary = ava::tui::detail::render_transcript_layout(transcript, 32);
  auto message = std::ranges::find(ordinary.message_item_indices, anchor_index);
  auto const position = static_cast<std::size_t>(message - ordinary.message_item_indices.begin());
  auto const old_max = ordinary.lines.size() - 6;
  auto const old_scroll = old_max - ordinary.content_starts[position];
  auto const anchor = ava::tui::detail::capture_transcript_viewport_anchor(ordinary, old_max, old_scroll);
  auto projection = accepted_projection(anchor_item, anchor_index, 1, "row 1\nrow 2\nrow 3\nrow 4\nrow 5\nrow 6\nrow 7");
  auto const projected = ava::tui::detail::render_transcript_layout(transcript, 32, ava::tui::ToolPresentation::Rich, true, false, projection.get());
  auto const new_max = projected.lines.size() - 6;
  auto const restored_scroll = ava::tui::detail::restore_transcript_viewport_anchor(anchor, projected, new_max, 0);
  auto const visible_start = new_max - restored_scroll;
  auto projected_message = std::ranges::find(projected.message_item_indices, anchor_index);
  auto const projected_position = static_cast<std::size_t>(projected_message - projected.message_item_indices.begin());
  expect(anchor.valid && visible_start == projected.content_starts[projected_position],
         "detached viewport anchor restoration preserves the authoritative item/content row when a projection changes geometry");

  controller.shutdown();
}

// The fake holds callback copies independently of the controller so late-notification lifetime is observable.
struct B5BridgeState
{
  // Requests admitted by the fake backend.
  std::vector<ava::tui::TuiMermaidRenderRequest> requests;
  // Published results waiting for an optional successful drain.
  std::vector<ava::tui::TuiMermaidRenderCompletion> completions;
  // Registered producer notification, cleared by controller shutdown.
  std::function<void()> notify;
  // Independently configurable enqueue outcome distinguishes capacity from contention.
  ava::tui::TuiMermaidEnqueueResult enqueue_result = ava::tui::TuiMermaidEnqueueResult::Accepted;
  // Contention leaves completions untouched and returns nullopt.
  bool drain_busy = false;
  // Attempt counts detect bridge work in callbacks and premature retries.
  unsigned enqueues = 0;
  // Test-thread drain attempts distinguish notified work from periodic polling.
  unsigned drains = 0;

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

// Shared ownership keeps fake backend observations alive until all copied bridge callbacks retire.
ava::tui::TuiMermaidRenderBridge b5_bridge(std::shared_ptr<B5BridgeState> state)
{
  ava::tui::TuiMermaidRenderBridge bridge;
  bridge.config_epoch = 1;
  bridge.enabled = true;
  bridge.enqueue = [state](ava::tui::TuiMermaidRenderRequest request) {
    ++state->enqueues;
    if (state->enqueue_result == ava::tui::TuiMermaidEnqueueResult::Accepted)
      state->requests.push_back(std::move(request));
    return state->enqueue_result;
  };
  bridge.cancel = [](std::uint64_t, std::uint64_t) { return true; };
  bridge.drain = [state]() -> std::optional<std::vector<ava::tui::TuiMermaidRenderCompletion>> {
    ++state->drains;
    if (state->drain_busy)
      return std::nullopt;
    return std::exchange(state->completions, {});
  };
  bridge.set_completion_notifier = [state](std::function<void()> notify) {
    state->notify = std::move(notify);
    if (state->notify && !state->completions.empty())
      state->notify();
  };
  return bridge;
}

// A real pipe supports the loop while fixture-owned descriptors outlive its waiter.
std::pair<ava::core::UniqueFd, ava::core::UniqueFd> b5_loop_pipe()
{
  std::array<int, 2> descriptors{-1, -1};
  if (::pipe(descriptors.data()) != 0)
    throw std::system_error(errno, std::generic_category(), "create Mermaid test input pipe");
  return {ava::core::UniqueFd(descriptors[0]), ava::core::UniqueFd(descriptors[1])};
}

// Declaration order retires controller/renderer timer handles before the loop and its descriptors.
struct B5AttachedMermaidFixture
{
  // Backend state is shared with neutral bridge callbacks until controller shutdown.
  std::shared_ptr<B5BridgeState> state = std::make_shared<B5BridgeState>();
  // Readiness descriptors belong to this fixture.
  std::pair<ava::core::UniqueFd, ava::core::UniqueFd> descriptors = b5_loop_pipe();
  // Event loop outlives every attached presentation owner.
  tui_test_support::TuiEventLoopFixture event_loop{descriptors.first.get()};
  // Timer registry of event_loop.
  ava::tui::TuiEventLoop& loop = event_loop.loop();
  // Frontend owns registered session data for all following presentation objects.
  tui_test_support::TuiFrontEndFixture frontend;
  // Presentation owns the snapshot borrowed by the remaining members.
  ava::tui::RuntimePresentationState presentation{frontend.frontend(), {}};
  // Composer is borrowed by renderer and navigation.
  ava::tui::Composer composer;
  // Renderer batches invalidation while attached to the loop.
  ava::tui::RuntimeRenderer renderer{loop, frontend.frontend(), presentation, composer};
  // Navigation shares the stable presentation and renderer owners.
  ava::tui::RuntimeNavigationController navigation{*frontend.frontend(), presentation, composer, renderer};
  // Selectors precede the search owner that borrows them.
  ava::tui::SelectorHost selectors{presentation};
  // Search consumes projection mutations without owning the controller.
  ava::tui::TranscriptSearchController search{presentation, renderer, navigation, selectors};
  // Controller is destroyed first and unregisters its producer notification.
  ava::tui::RuntimeMermaidPresentationController controller{presentation, renderer, search, b5_bridge(state)};

  // Renderer is already registered; attach Mermaid and install one eligible block.
  B5AttachedMermaidFixture()
  {
    controller.attach_loop(loop);
    presentation.finish_session_transition("b5-attached");
    auto display = frontend.frontend()->display_settings_snapshot();
    display.mermaid_enabled = true;
    display.mermaid_config_epoch = 1;
    presentation.apply_local_display_settings(display);
    presentation.publish_transcript({{.label = "ava", .text = "```mermaid\ngraph TD; A-->B\n```"}});
  }

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

// A completion is consumed on notification without transcript mutation or a pending-work polling timer.
void test_attached_mermaid_completion_without_transcript_change()
{
  B5AttachedMermaidFixture fixture;
  auto const& snapshot = fixture.presentation.screen();
  auto const generation = snapshot.transcript_generation;
  auto const first = fixture.controller.service_if_changed();
  if (!ava::tests::require(
          first == ava::tui::MermaidServiceOutcome::Unchanged && fixture.state->requests.size() == 1 && static_cast<bool>(fixture.state->notify),
          "attached controller admits a request and registers a completion notifier"))
    return;
  auto const drains = fixture.state->drains;
  expect(!fixture.loop.has_due_work(std::chrono::steady_clock::now() + std::chrono::hours(1)), "outstanding accepted work arms no 16 ms discovery timer");
  fixture.loop.service_due(std::chrono::steady_clock::now() + std::chrono::hours(1));
  static_cast<void>(fixture.controller.service_if_changed());
  expect(fixture.state->drains == drains, "unchanged pending work performs no periodic drain");
  auto const request = fixture.state->requests.front();
  fixture.state->completions.push_back({.identity = request.identity, .config_epoch = 1, .accepted = true, .text = "notified result"});
  fixture.state->notify();
  expect(snapshot.transcript_generation == generation && fixture.state->drains == drains,
         "producer notification only publishes readiness and does not execute bridge work");
  fixture.event_loop.wait(tui_test_support::TuiEventLoopFixture::Wait::ReadyOnly);
  auto const result = fixture.controller.service_if_changed();
  expect(result == ava::tui::MermaidServiceOutcome::Changed && snapshot.transcript_generation == generation + 1 && snapshot.mermaid_projection &&
             snapshot.mermaid_projection->accepted().size() == 1 && snapshot.mermaid_projection->accepted().front().text == "notified result",
         "unchanged transcript consumes a notified completion and publishes its projection");
}

// Queue capacity by itself must wait for notification rather than arm a contention retry timer.
void test_attached_mermaid_queue_full_does_not_poll()
{
  B5AttachedMermaidFixture fixture;
  fixture.state->enqueue_result = ava::tui::TuiMermaidEnqueueResult::QueueFull;
  static_cast<void>(fixture.controller.service_if_changed());
  auto const attempts = fixture.state->enqueues;
  expect(attempts == 1 && !fixture.loop.has_due_work(std::chrono::steady_clock::now() + std::chrono::hours(1)),
         "QueueFull is distinct from Busy and has no retry timer");
  fixture.loop.service_due(std::chrono::steady_clock::now() + std::chrono::hours(1));
  static_cast<void>(fixture.controller.service_if_changed());
  expect(fixture.state->enqueues == attempts, "unchanged full-queue work is not retried by loop polling");
}

// Busy enqueue and Busy drain share 100 then 250 ms backoff, unaffected by unrelated service calls.
void test_attached_mermaid_busy_backoff(bool busy_drain)
{
  using namespace std::chrono_literals;
  using Clock = std::chrono::steady_clock;
  B5AttachedMermaidFixture fixture;
  fixture.state->drain_busy = busy_drain;
  fixture.state->enqueue_result = busy_drain ? ava::tui::TuiMermaidEnqueueResult::Accepted : ava::tui::TuiMermaidEnqueueResult::Busy;
  auto const before_first = Clock::now();
  static_cast<void>(fixture.controller.service_if_changed());
  auto const after_first = Clock::now();
  auto const first_attempts = busy_drain ? fixture.state->drains : fixture.state->enqueues;
  expect(first_attempts > 0 && !fixture.loop.has_due_work(before_first + 99ms) && fixture.loop.has_due_work(after_first + 100ms),
         "first Busy result arms a 100 ms retry");

  // Force an unrelated transcript observation; no source changes and no readiness notification justify another bridge attempt.
  fixture.presentation.invalidate_transcript_layout();
  static_cast<void>(fixture.controller.service_if_changed());
  static_cast<void>(fixture.controller.service());
  expect((busy_drain ? fixture.state->drains : fixture.state->enqueues) == first_attempts && !fixture.loop.has_due_work(before_first + 99ms),
         "unrelated changed and unconditional service calls do not shorten Busy backoff");

  fixture.loop.service_due(after_first + 100ms);
  expect((busy_drain ? fixture.state->drains : fixture.state->enqueues) == first_attempts, "due retry callback sets readiness without executing bridge work");
  auto const before_second = Clock::now();
  static_cast<void>(fixture.controller.service_if_changed());
  auto const after_second = Clock::now();
  auto const second_attempts = busy_drain ? fixture.state->drains : fixture.state->enqueues;
  expect(second_attempts > first_attempts && !fixture.loop.has_due_work(before_second + 249ms) && fixture.loop.has_due_work(after_second + 250ms),
         "repeated Busy result backs off to 250 ms");
  static_cast<void>(fixture.controller.service());
  expect((busy_drain ? fixture.state->drains : fixture.state->enqueues) == second_attempts, "repeated service does not bypass the longer retry");

  fixture.state->drain_busy = false;
  fixture.state->enqueue_result = ava::tui::TuiMermaidEnqueueResult::Accepted;
  fixture.loop.service_due(after_second + 250ms);
  static_cast<void>(fixture.controller.service_if_changed());
  expect(!fixture.loop.has_due_work(Clock::now() + 1h), "successful empty drain and accepted pending work cancel contention retries");
}

// Shutdown unregisters and cancels Busy retry; copied notifications remain safe after controller and loop destruction.
void test_attached_mermaid_late_notification_lifetime()
{
  std::function<void()> late_notification;
  {
    B5AttachedMermaidFixture fixture;
    fixture.state->enqueue_result = ava::tui::TuiMermaidEnqueueResult::Busy;
    static_cast<void>(fixture.controller.service_if_changed());
    late_notification = fixture.state->notify;
    if (!ava::tests::require(static_cast<bool>(late_notification), "attached controller provides a copyable notification"))
      return;
    fixture.controller.shutdown();
    auto const drains = fixture.state->drains;
    auto const enqueues = fixture.state->enqueues;
    late_notification();
    fixture.loop.service_due(std::chrono::steady_clock::now() + std::chrono::hours(1));
    auto const result = fixture.controller.service_if_changed();
    expect(!fixture.state->notify && !fixture.loop.has_due_work(std::chrono::steady_clock::now() + std::chrono::hours(1)) &&
               result == ava::tui::MermaidServiceOutcome::Unchanged && fixture.state->drains == drains && fixture.state->enqueues == enqueues,
           "shutdown unregisters and cancels retry while late notifications perform no bridge work");
  }
  late_notification();
  late_notification = {};
  expect(true, "retained notification safely retires after the controller and event loop are destroyed");
}

}  // namespace

// Run diagram projection and notification/backoff regressions through the TUI owner.
void run_tui_mermaid_tests()
{
  test_attached_mermaid_completion_without_transcript_change();
  test_attached_mermaid_queue_full_does_not_poll();
  test_attached_mermaid_busy_backoff(false);
  test_attached_mermaid_busy_backoff(true);
  test_attached_mermaid_late_notification_lifetime();
  test_mermaid_fence_scanner_matrix();
  test_projection_rendering_is_nonrecursive_and_shared_by_geometry();
  test_runtime_projection_reconciliation_and_stale_fail_closed();
  test_runtime_projection_bounds_cap_and_detached_anchor_geometry();
}

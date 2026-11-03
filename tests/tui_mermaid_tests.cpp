#include "sys.h"
#include "tests/support/test_harness.h"
#include "tests/support/tui_test_support.h"
#include "ava/tui/Composer.h"
#include "ava/tui/SelectorHost.h"
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

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <ranges>
#include <string>
#include <utility>
#include <vector>

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

struct FakeBridgeState
{
  std::vector<ava::tui::TuiMermaidRenderRequest> requests;
  std::vector<ava::tui::TuiMermaidRenderCompletion> completions;
  std::vector<std::pair<std::uint64_t, std::uint64_t>> cancellations;
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

void test_runtime_projection_reconciliation_and_stale_fail_closed()
{
  auto state = std::make_shared<FakeBridgeState>();
  tui_test_support::TuiFrontEndFixture frontend;
  ava::tui::RuntimePresentationState presentation(frontend.frontend(), {});
  auto& snapshot = presentation.screen();
  ava::tui::Composer composer;
  ava::tui::RuntimeRenderer renderer(frontend.frontend(), snapshot, presentation.sidebar(), composer);
  ava::tui::RuntimeNavigationController navigation(*frontend.frontend(), snapshot, composer, renderer);
  ava::tui::SelectorHost selectors(snapshot);
  ava::tui::TranscriptSearchController search(presentation, renderer, navigation, selectors);
  ava::tui::RuntimeMermaidPresentationController controller(snapshot, renderer, search, fake_bridge(state));
  snapshot.session_id = "session-a";
  snapshot.mermaid_enabled = true;
  snapshot.mermaid_config_epoch = 1;
  snapshot.transcript = {{.label = "ava", .text = "```mermaid\ngraph TD; A-->B\n```"}};
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
  snapshot.mermaid_config_epoch = 2;
  state->completions.push_back({.identity = request.identity, .config_epoch = 1, .accepted = true, .text = "stale result"});
  auto reconfigured = controller.service();
  auto fallback_after = ava::tui::detail::render_transcript_layout(snapshot.transcript, 60, ava::tui::ToolPresentation::Rich, true, false,
                                                                   ava::tui::detail::active_mermaid_projection(snapshot));
  expect(reconfigured == ava::tui::MermaidServiceOutcome::Changed && !snapshot.mermaid_projection && fallback_after.lines == fallback_before.lines &&
             state->requests.size() == 2,
         "configuration epoch changes remove old projections, ignore stale completions, and immediately requeue under the new epoch");

  snapshot.transcript.front().append_only_stream = true;
  snapshot.transcript.front().stream_id = "live";
  ++snapshot.transcript_generation;
  auto const request_count = state->requests.size();
  static_cast<void>(controller.service());
  expect(state->requests.size() == request_count && !state->cancellations.empty(),
         "an item becoming append-only cancels its prior identity and is never enqueued or projected while streaming");

  controller.shutdown();
}

void test_runtime_projection_bounds_cap_and_detached_anchor_geometry()
{
  auto state = std::make_shared<FakeBridgeState>();
  tui_test_support::TuiFrontEndFixture frontend;
  ava::tui::RuntimePresentationState presentation(frontend.frontend(), {});
  auto& snapshot = presentation.screen();
  ava::tui::Composer composer;
  ava::tui::RuntimeRenderer renderer(frontend.frontend(), snapshot, presentation.sidebar(), composer);
  ava::tui::RuntimeNavigationController navigation(*frontend.frontend(), snapshot, composer, renderer);
  ava::tui::SelectorHost selectors(snapshot);
  ava::tui::TranscriptSearchController search(presentation, renderer, navigation, selectors);
  ava::tui::RuntimeMermaidPresentationController controller(snapshot, renderer, search, fake_bridge(state));
  snapshot.session_id = "bounded";
  snapshot.mermaid_enabled = true;
  snapshot.mermaid_config_epoch = 1;
  for (std::size_t index = 0; index < 40; ++index)
    snapshot.transcript.push_back({.label = "ava", .text = "```mermaid\ngraph TD; A" + std::to_string(index) + "-->B\n```"});
  static_cast<void>(controller.service());
  expect(state->requests.size() == 32, "TUI reconciliation never exceeds the coordinator's 32 undrained identity bound");

  auto const evicted_request = state->requests.front();
  snapshot.transcript.erase(snapshot.transcript.begin());
  ++snapshot.transcript_generation;
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

}  // namespace

void run_tui_mermaid_tests()
{
  test_mermaid_fence_scanner_matrix();
  test_projection_rendering_is_nonrecursive_and_shared_by_geometry();
  test_runtime_projection_reconciliation_and_stale_fail_closed();
  test_runtime_projection_bounds_cap_and_detached_anchor_geometry();
}

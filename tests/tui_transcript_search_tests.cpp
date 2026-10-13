#include "sys.h"
#include "tests/support/test_harness.h"
#include "tests/support/tui_test_support.h"
#include "ava/tui/composer.h"
#include "ava/tui/composer_internal.h"
#include "ava/tui/runtime_transcript_internal.h"
#include "ava/tui/runtime_transcript_search_internal.h"
#include "ava/tui/tool_cards.h"

#include <algorithm>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

ava::tui::TranscriptItem private_launch_task_item()
{
  ava::tui::ToolTimelineItem tool{
      .status = ava::tui::ToolTimelineStatus::Success,
      .name = "task",
      .argument_summary = "arguments provided",
      .result_summary = "ok",
      .arguments_json = R"({"description":"ordinary visible audit","subagent_type":"explore","mode":"foreground"})",
      .result_json =
          R"({"tool":"task","ok":true,"subagent_type":"explore","description":"ordinary visible audit","state":"completed","tool_calls":2,"task_result":"ordinary visible result"})",
      .call_id = "call_private_launch_search",
      .lifecycle = ava::tui::ToolLifecycleState::Complete};
  tool.subagent_launch_display = ava::agent::SubagentLaunchDisplay::normalized("PRIVATE MODEL SEARCH TOKEN", std::string_view("private-thinking-token"));
  return ava::tui::TranscriptItem{.tool = std::move(tool)};
}

void test_transcript_search_literal_and_sanitized_rendered_scope()
{
  ava::tui::ComposerSnapshot snapshot;
  snapshot.transcript = {
      ava::tui::TranscriptItem{.label = "you", .text = "unused source"},
      ava::tui::TranscriptItem{.label = "ava", .text = "unused duplicate"},
      ava::tui::TranscriptItem{.tool = ava::tui::ToolTimelineItem{.status = ava::tui::ToolTimelineStatus::Success, .name = "read"}},
  };
  ava::tui::detail::TranscriptLayout layout{
      .lines = {"\x1b[31mALPHA first\x1b[0m", "", "\x1b]8;;https://example.invalid\x1b\\alpha duplicate\x1b]8;;\x1b\\",
                std::string("tool result ") + char{1} + " Äpfel"},
      .message_starts = {0, 2, 3},
      .content_starts = {0, 2, 3},
      .message_item_indices = {0, 1, 2},
  };

  auto alpha = ava::tui::detail::build_transcript_search_matches(snapshot, layout, "alpha");
  auto upper_non_ascii = ava::tui::detail::build_transcript_search_matches(snapshot, layout, "Ä");
  auto lower_non_ascii = ava::tui::detail::build_transcript_search_matches(snapshot, layout, "ä");
  auto all = ava::tui::detail::build_transcript_search_matches(snapshot, layout, "");
  expect(alpha.size() == 2 && alpha[0].item_index == 0 && alpha[1].item_index == 1 && alpha[0].identity == "user" && alpha[1].identity == "assistant" &&
             alpha[0].detail == "ALPHA first" && alpha[1].detail == "alpha duplicate" &&
             ava::tui::detail::build_transcript_search_matches(snapshot, layout, "example.invalid").empty(),
         "transcript search is ASCII-case-insensitive, strips SGR/OSC, and retains duplicate rendered items in chronological order");
  expect(upper_non_ascii.size() == 1 && upper_non_ascii[0].item_index == 2 && lower_non_ascii.empty(),
         "transcript search keeps non-ASCII UTF-8 matching byte-exact");
  expect(all.size() == 3 && all[2].identity == "tool · read" && all[2].detail == "tool result ? Äpfel",
         "empty transcript search lists every rendered block with sanitized safe identities and first nonblank details");
}

void test_transcript_search_uses_current_rendered_tool_and_thinking_presentation()
{
  ava::tui::ComposerSnapshot snapshot;
  snapshot.tool_presentation = ava::tui::ToolPresentation::Rich;
  snapshot.thinking_visible = false;
  snapshot.transcript = {
      ava::tui::TranscriptItem{.label = "ava", .text = "visible assistant answer", .thinking = "SECRET HIDDEN THINKING"},
      ava::tui::TranscriptItem{.tool = ava::tui::ToolTimelineItem{.status = ava::tui::ToolTimelineStatus::Success,
                                                                  .name = "custom_tool",
                                                                  .argument_summary = "needle=alpha",
                                                                  .result_summary = "done",
                                                                  .result_json = R"({"output":"rendered rich result alpha"})",
                                                                  .lifecycle = ava::tui::ToolLifecycleState::Complete}},
  };
  auto rich_layout = ava::tui::detail::render_transcript_layout(snapshot.transcript, 80, snapshot.tool_presentation, snapshot.thinking_visible, false);
  auto hidden = ava::tui::detail::build_transcript_search_matches(snapshot, rich_layout, "SECRET");
  auto rich = ava::tui::detail::build_transcript_search_matches(snapshot, rich_layout, "rich result");

  snapshot.tool_presentation = ava::tui::ToolPresentation::Compact;
  auto compact_layout = ava::tui::detail::render_transcript_layout(snapshot.transcript, 80, snapshot.tool_presentation, snapshot.thinking_visible, false);
  auto compact = ava::tui::detail::build_transcript_search_matches(snapshot, compact_layout, "rich result");
  expect(hidden.empty() && rich.size() == 1 && rich.front().item_index == 1 && compact.empty(),
         "transcript search includes only current rendered thinking and Rich/Compact tool-card content");

  snapshot.thinking_visible = true;
  auto thinking_layout = ava::tui::detail::render_transcript_layout(snapshot.transcript, 80, snapshot.tool_presentation, snapshot.thinking_visible, false);
  auto visible_thinking = ava::tui::detail::build_transcript_search_matches(snapshot, thinking_layout, "secret hidden thinking");
  expect(visible_thinking.size() == 1 && visible_thinking.front().item_index == 0,
         "transcript search includes thinking only when the current transcript layout renders it");
}

void test_transcript_search_is_stable_across_render_widths()
{
  constexpr std::string_view kLongPath = "/workspace/src/components/transcript_search_boundary_stability.cpp";
  constexpr std::string_view kToolToken = "/workspace/generated/very_long_unspaced_tool_card_path/abcdefghijklmnopqrstuvwxyz0123456789/result.json";
  ava::tui::ComposerSnapshot snapshot;
  snapshot.transcript = {
      ava::tui::TranscriptItem{.label = "ava", .text = "prefix alpha beta suffix " + std::string(kLongPath) + " exact Äpfel"},
      ava::tui::TranscriptItem{.tool = ava::tui::ToolTimelineItem{.status = ava::tui::ToolTimelineStatus::Success,
                                                                  .name = "custom_tool",
                                                                  .argument_summary = "inspect",
                                                                  .result_summary = "done",
                                                                  .result_json = "{\"output\":\"" + std::string(kToolToken) + "\"}",
                                                                  .lifecycle = ava::tui::ToolLifecycleState::Complete}},
  };
  for (auto const presentation : {ava::tui::ToolPresentation::Rich, ava::tui::ToolPresentation::Expanded})
  {
    auto const wide_layout = ava::tui::detail::render_transcript_layout(snapshot.transcript, 120, presentation, snapshot.thinking_visible, false);
    auto const narrow_layout = ava::tui::detail::render_transcript_layout(snapshot.transcript, 40, presentation, snapshot.thinking_visible, false);
    auto const wide_phrase = ava::tui::detail::build_transcript_search_matches(snapshot, wide_layout, "alpha beta");
    auto const narrow_phrase = ava::tui::detail::build_transcript_search_matches(snapshot, narrow_layout, "alpha beta");
    auto const wide_path = ava::tui::detail::build_transcript_search_matches(snapshot, wide_layout, kLongPath);
    auto const narrow_path = ava::tui::detail::build_transcript_search_matches(snapshot, narrow_layout, kLongPath);
    auto const wide_tool_token = ava::tui::detail::build_transcript_search_matches(snapshot, wide_layout, kToolToken);
    auto const narrow_tool_token = ava::tui::detail::build_transcript_search_matches(snapshot, narrow_layout, kToolToken);
    auto const wide_non_ascii = ava::tui::detail::build_transcript_search_matches(snapshot, wide_layout, "Äpfel");
    auto const narrow_non_ascii = ava::tui::detail::build_transcript_search_matches(snapshot, narrow_layout, "Äpfel");
    auto const folded_non_ascii = ava::tui::detail::build_transcript_search_matches(snapshot, narrow_layout, "äpfel");
    expect(narrow_layout.lines.size() > wide_layout.lines.size() && wide_phrase.size() == 1 && narrow_phrase.size() == 1 && wide_path.size() == 1 &&
               narrow_path.size() == 1 && wide_tool_token.size() == 1 && narrow_tool_token.size() == 1 && wide_tool_token.front().item_index == 1 &&
               narrow_tool_token.front().item_index == 1 && wide_non_ascii.size() == 1 && narrow_non_ascii.size() == 1 && folded_non_ascii.empty() &&
               !narrow_path.front().detail.empty() && narrow_path.front().detail.size() <= ava::tui::detail::kMaxTranscriptSearchDetailBytes,
           "transcript search preserves prose and actual Rich/Expanded tool-card hard-wrapped unspaced content across wide and narrow renders: presentation=" +
               std::string(ava::tui::to_string(presentation)) + " wide-lines=" + std::to_string(wide_layout.lines.size()) +
               " narrow-lines=" + std::to_string(narrow_layout.lines.size()) + " wide-tool=" + std::to_string(wide_tool_token.size()) +
               " narrow-tool=" + std::to_string(narrow_tool_token.size()));
  }
}

void test_transcript_search_projection_match_and_modal_updates_are_suffix_bounded()
{
  ava::tui::ComposerSnapshot snapshot;
  ava::tui::detail::TranscriptLayout layout;
  snapshot.transcript.reserve(ava::tui::kMaxTranscriptItems);
  layout.lines.reserve(ava::tui::kMaxTranscriptItems);
  layout.message_starts.reserve(ava::tui::kMaxTranscriptItems);
  layout.content_starts.reserve(ava::tui::kMaxTranscriptItems);
  layout.message_item_indices.reserve(ava::tui::kMaxTranscriptItems);
  for (std::size_t index = 0; index < ava::tui::kMaxTranscriptItems; ++index)
  {
    auto text = "retained item [" + std::to_string(index) + "]";
    snapshot.transcript.push_back(ava::tui::TranscriptItem{.label = "ava", .text = text});
    layout.lines.push_back(text);
    layout.message_starts.push_back(index);
    layout.content_starts.push_back(index);
    layout.message_item_indices.push_back(index);
  }

  ava::tui::detail::TranscriptSearchProjectionCache cache;
  static_cast<void>(cache.update_query("retained item"));
  auto initial_update = cache.rebuild_all(snapshot, layout);
  ava::tui::SelectListView view;
  std::size_t modal_row_build_count = 0;
  ava::tui::detail::update_transcript_search_select_list_rows(view, cache.matches(), cache.query(), initial_update.first_changed_match_row,
                                                              modal_row_build_count);
  auto const initial_builds = cache.projection_build_count();
  auto const initial_layout_visits = cache.layout_position_visit_count();
  auto const initial_scans = cache.match_projection_evaluation_count();
  auto const initial_realignments = cache.match_entry_realign_count();
  auto const initial_splices = cache.match_entry_splice_count();
  auto const initial_modal_rows = modal_row_build_count;

  snapshot.transcript.back().text = "retained item tail replacement";
  auto tail_update = cache.refresh_after_transcript_mutation(snapshot, 80, snapshot.tool_presentation, false, false, 0, ava::tui::kMaxTranscriptItems - 1);
  ava::tui::detail::update_transcript_search_select_list_rows(view, cache.matches(), cache.query(), tail_update.first_changed_match_row, modal_row_build_count);
  auto const tail_builds = cache.projection_build_count();
  auto const tail_layout_visits = cache.layout_position_visit_count();
  auto const tail_scans = cache.match_projection_evaluation_count();
  auto const tail_realignments = cache.match_entry_realign_count();
  auto const tail_splices = cache.match_entry_splice_count();
  auto const tail_modal_rows = modal_row_build_count;
  auto const query_builds = cache.projection_build_count();
  auto query_update = cache.update_query("RETAINED ITEM");
  ava::tui::detail::update_transcript_search_select_list_rows(view, cache.matches(), cache.query(), query_update.first_changed_match_row,
                                                              modal_row_build_count);
  auto const query_scans = cache.match_projection_evaluation_count();
  auto const same_query_update = cache.update_query("RETAINED ITEM");
  auto const same_query_scans = cache.match_projection_evaluation_count();

  snapshot.transcript.back().text = "retained item tail replacement twice";
  auto const before_repeated_layout_visits = cache.layout_position_visit_count();
  auto const before_repeated_scans = cache.match_projection_evaluation_count();
  auto const before_repeated_realignments = cache.match_entry_realign_count();
  auto const before_repeated_splices = cache.match_entry_splice_count();
  auto const before_repeated_modal_rows = modal_row_build_count;
  auto repeated_tail_update =
      cache.refresh_after_transcript_mutation(snapshot, 80, snapshot.tool_presentation, false, false, 0, ava::tui::kMaxTranscriptItems - 1);
  ava::tui::detail::update_transcript_search_select_list_rows(view, cache.matches(), cache.query(), repeated_tail_update.first_changed_match_row,
                                                              modal_row_build_count);
  auto const repeated_layout_visits = cache.layout_position_visit_count();
  auto const repeated_scans = cache.match_projection_evaluation_count();
  auto const repeated_realignments = cache.match_entry_realign_count();
  auto const repeated_splices = cache.match_entry_splice_count();
  auto const repeated_modal_rows = modal_row_build_count;
  auto const empty_update = cache.update_query("");
  ava::tui::detail::update_transcript_search_select_list_rows(view, cache.matches(), cache.query(), empty_update.first_changed_match_row,
                                                              modal_row_build_count);
  snapshot.transcript.back().text = "empty query incremental tail";
  auto empty_tail_update =
      cache.refresh_after_transcript_mutation(snapshot, 80, snapshot.tool_presentation, false, false, 0, ava::tui::kMaxTranscriptItems - 1);
  ava::tui::detail::update_transcript_search_select_list_rows(view, cache.matches(), cache.query(), empty_tail_update.first_changed_match_row,
                                                              modal_row_build_count);

  expect(initial_builds == ava::tui::kMaxTranscriptItems && initial_layout_visits == ava::tui::kMaxTranscriptItems &&
             initial_scans == ava::tui::kMaxTranscriptItems && cache.matches().size() == 1000 && tail_update.first_changed_match_row == 999 &&
             tail_builds == initial_builds + 1 && tail_layout_visits == initial_layout_visits + 1 && tail_scans == initial_scans + 1 &&
             tail_realignments == initial_realignments && tail_splices == initial_splices + 1 && tail_modal_rows == initial_modal_rows + 1 &&
             query_builds == initial_builds + 1 && query_update.first_changed_match_row == 0 &&
             query_scans == initial_scans + 1 + ava::tui::kMaxTranscriptItems && same_query_update.first_changed_match_row == 1000 &&
             same_query_scans == query_scans && repeated_tail_update.first_changed_match_row == 999 &&
             repeated_layout_visits == before_repeated_layout_visits + 1 && repeated_scans == before_repeated_scans + 1 &&
             repeated_realignments == before_repeated_realignments && repeated_splices == before_repeated_splices + 1 &&
             repeated_modal_rows == before_repeated_modal_rows + 1 && empty_tail_update.first_changed_match_row == 999 &&
             cache.authoritative_mutation_item_render_count() == 3 && cache.projection_build_count() == initial_builds + 3 &&
             cache.match_projection_evaluation_count() == initial_scans + 3 + (2 * ava::tui::kMaxTranscriptItems) && modal_row_build_count == 3003,
         "1,000 retained matching rows stay untouched while repeated shift-zero tail updates add exactly one authoritative item render, projection build, "
         "layout visit, match evaluation, match splice, and modal row with zero retained-match realignments");
}

ava::tui::TranscriptItem context_tool(std::string name, std::string result)
{
  return ava::tui::TranscriptItem{.tool = ava::tui::ToolTimelineItem{.status = ava::tui::ToolTimelineStatus::Success,
                                                                     .name = std::move(name),
                                                                     .argument_summary = "path=src/context.cpp",
                                                                     .result_summary = std::move(result),
                                                                     .lifecycle = ava::tui::ToolLifecycleState::Complete}};
}

void test_transcript_search_roomy_direct_item_render_matches_full_layout_projection()
{
  ava::tui::ComposerSnapshot snapshot;
  snapshot.tool_presentation = ava::tui::ToolPresentation::Rich;
  snapshot.thinking_visible = false;
  snapshot.transcript = {
      context_tool("read_file", "first context result"),
      context_tool("grep", "second context result with a long wrapped /workspace/source/component/transcript_search_projection.cpp path"),
      ava::tui::TranscriptItem{.tool = ava::tui::ToolTimelineItem{.status = ava::tui::ToolTimelineStatus::Success,
                                                                  .name = "custom_tool",
                                                                  .argument_summary = "inspect representative content",
                                                                  .result_summary = "DUPLICATE TOOL RESULT",
                                                                  .result_json = R"({"output":"long wrapped tool-card detail for direct projection parity"})",
                                                                  .lifecycle = ava::tui::ToolLifecycleState::Complete}},
      ava::tui::TranscriptItem{.label = "ava", .text = "DUPLICATE TOOL RESULT"},
      ava::tui::TranscriptItem{.label = "ava", .text = "visible answer", .thinking = "hidden reasoning"},
      ava::tui::TranscriptItem{.label = "you", .text = "roomy user turn"},
      ava::tui::TranscriptItem{.label = "thinking", .text = "hidden standalone thinking"},
  };
  constexpr auto width = std::size_t{48};
  auto const layout = ava::tui::detail::render_transcript_layout(snapshot.transcript, width, snapshot.tool_presentation, snapshot.thinking_visible, false);

  auto roomy_gap_present = false;
  bool blocks_match = layout.block_boundaries.size() == layout.message_starts.size();
  for (std::size_t item_index = 0; item_index < snapshot.transcript.size(); ++item_index)
  {
    auto const direct = ava::tui::detail::render_transcript_search_item_lines(snapshot.transcript, item_index, width, snapshot.tool_presentation,
                                                                              snapshot.thinking_visible, false);
    auto const position = std::ranges::find(layout.message_item_indices, item_index);
    if (position == layout.message_item_indices.end())
    {
      blocks_match = blocks_match && direct.empty();
      continue;
    }
    auto const position_index = static_cast<std::size_t>(position - layout.message_item_indices.begin());
    auto const start = layout.message_starts[position_index];
    auto const end = position_index + 1 < layout.block_boundaries.size() ? layout.block_boundaries[position_index + 1] : layout.lines.size();
    roomy_gap_present = roomy_gap_present || layout.block_boundaries[position_index] < start;
    blocks_match = blocks_match && direct == std::vector<std::string>(layout.lines.begin() + static_cast<std::ptrdiff_t>(start),
                                                                      layout.lines.begin() + static_cast<std::ptrdiff_t>(end));
  }

  ava::tui::detail::TranscriptSearchProjectionCache cache;
  static_cast<void>(cache.update_query(""));
  static_cast<void>(cache.rebuild_all(snapshot, layout));
  auto const full_matches = cache.matches();
  static_cast<void>(cache.refresh_after_transcript_mutation(snapshot, width, snapshot.tool_presentation, snapshot.thinking_visible, false, 0, 0));
  auto const& direct_matches = cache.matches();
  auto projections_match = full_matches.size() == direct_matches.size();
  for (std::size_t index = 0; projections_match && index < full_matches.size(); ++index)
  {
    projections_match = full_matches[index].item_index == direct_matches[index].item_index && full_matches[index].identity == direct_matches[index].identity &&
                        full_matches[index].detail == direct_matches[index].detail;
  }
  expect(roomy_gap_present && blocks_match && projections_match && cache.authoritative_mutation_item_render_count() == snapshot.transcript.size(),
         "roomy direct mutation rendering exactly matches full-layout search blocks and projections while excluding decorative inter-group spacing for context "
         "headings, wrapped rich tool cards, suppressed duplicate results, visible assistant prose, and hidden thinking items");
}

void test_transcript_search_trailing_space_literal_survives_direct_mutation()
{
  ava::tui::ComposerSnapshot snapshot;
  snapshot.transcript = {
      ava::tui::TranscriptItem{.label = "ava", .text = "literal trailing result\n\ncontinued inside item"},
      ava::tui::TranscriptItem{.label = "you", .text = "next visual group"},
  };
  constexpr auto width = std::size_t{80};
  auto const layout = ava::tui::detail::render_transcript_layout(snapshot.transcript, width, snapshot.tool_presentation, true, false);
  ava::tui::detail::TranscriptSearchProjectionCache cache;
  static_cast<void>(cache.update_query("result "));
  static_cast<void>(cache.rebuild_all(snapshot, layout));
  auto const full_matches = cache.matches();
  auto const blank_row_matches = ava::tui::detail::build_transcript_search_matches(snapshot, layout, "result  continued");

  snapshot.transcript.front().text = "literal trailing result\n\ncontinued inside mutated item";
  static_cast<void>(cache.refresh_after_transcript_mutation(snapshot, width, snapshot.tool_presentation, true, false, 0, 0));
  auto const direct_matches = cache.matches();

  expect(layout.block_boundaries.size() == 2 && layout.block_boundaries[1] < layout.message_starts[1] && full_matches.size() == 1 &&
             full_matches.front().item_index == 0 && blank_row_matches.size() == 1 && blank_row_matches.front().item_index == 0 && direct_matches.size() == 1 &&
             direct_matches.front().item_index == 0,
         "a literal query ending in a space matches an actual blank row inside an item before and after direct mutation, while decorative roomy spacing before "
         "the next visual group belongs to neither projection");
}

void test_transcript_search_rebuilds_negative_shift_render_boundaries()
{
  ava::tui::ComposerSnapshot snapshot;
  snapshot.tool_presentation = ava::tui::ToolPresentation::Compact;
  snapshot.transcript = {context_tool("read_file", "DUPLICATE ASSISTANT RESULT"),
                         ava::tui::TranscriptItem{.label = "ava", .text = "DUPLICATE ASSISTANT RESULT"}};
  auto layout = ava::tui::detail::render_transcript_layout(snapshot.transcript, 80, snapshot.tool_presentation, false, false);
  ava::tui::detail::TranscriptSearchProjectionCache cache;
  static_cast<void>(cache.update_query("duplicate assistant result"));
  static_cast<void>(cache.rebuild_all(snapshot, layout));
  auto const initial_builds = cache.projection_build_count();
  auto const initial_scans = cache.match_projection_evaluation_count();
  auto initial_matches = cache.matches();

  snapshot.transcript.erase(snapshot.transcript.begin());
  auto update = cache.refresh_after_transcript_mutation(snapshot, 80, snapshot.tool_presentation, false, false, -1, snapshot.transcript.size());
  auto visible_matches = cache.matches();
  expect(initial_matches.size() == 1 && initial_matches.front().identity == "tool · read_file" && visible_matches.size() == 1 &&
             visible_matches.front().item_index == 0 && visible_matches.front().identity == "assistant" && update.first_changed_match_row == 0 &&
             cache.projection_build_count() == initial_builds + 1 && cache.match_projection_evaluation_count() == initial_scans + 1,
         "evicting a tool rebuilds and rescans the new leading assistant so an adjacency-suppressed duplicate becomes visible and searchable");

  snapshot.transcript = {context_tool("read_file", "first context"), context_tool("grep", "second context")};
  layout = ava::tui::detail::render_transcript_layout(snapshot.transcript, 80, snapshot.tool_presentation, false, false);
  static_cast<void>(cache.update_query("2 tools"));
  static_cast<void>(cache.rebuild_all(snapshot, layout));
  auto const pair_builds = cache.projection_build_count();
  auto const pair_scans = cache.match_projection_evaluation_count();
  snapshot.transcript.push_back(context_tool("glob", "third context"));
  static_cast<void>(cache.refresh_after_transcript_mutation(snapshot, 80, snapshot.tool_presentation, false, false, 0, 2));
  expect(cache.matches().empty() && cache.projection_build_count() == pair_builds + 2 && cache.match_projection_evaluation_count() == pair_scans + 2,
         "a context-tool tail append rebuilds and filters only the changed suffix plus the earlier group-heading owner");

  snapshot.transcript = {context_tool("read_file", "first context"), context_tool("grep", "second context")};
  layout = ava::tui::detail::render_transcript_layout(snapshot.transcript, 80, snapshot.tool_presentation, false, false);
  static_cast<void>(cache.update_query("context gathering"));
  static_cast<void>(cache.rebuild_all(snapshot, layout));
  auto const grouped_builds = cache.projection_build_count();
  auto const grouped_scans = cache.match_projection_evaluation_count();
  auto grouped = cache.matches();
  snapshot.transcript.erase(snapshot.transcript.begin());
  static_cast<void>(cache.refresh_after_transcript_mutation(snapshot, 80, snapshot.tool_presentation, false, false, -1, snapshot.transcript.size()));
  expect(grouped.size() == 1 && grouped.front().item_index == 0 && cache.matches().empty() && cache.projection_build_count() == grouped_builds + 1 &&
             cache.match_projection_evaluation_count() == grouped_scans + 1,
         "negative cap shift rebuilds the actual new leading context tool and removes its stale synthetic group-heading match");
}

void test_transcript_search_rebuilds_positive_shift_join_boundary()
{
  ava::tui::ComposerSnapshot snapshot;
  snapshot.tool_presentation = ava::tui::ToolPresentation::Compact;
  snapshot.transcript = {ava::tui::TranscriptItem{.label = "ava", .text = "RESTORED PREFIX DUPLICATE"}};
  auto layout = ava::tui::detail::render_transcript_layout(snapshot.transcript, 80, snapshot.tool_presentation, false, false);
  ava::tui::detail::TranscriptSearchProjectionCache cache;
  static_cast<void>(cache.update_query("restored prefix duplicate"));
  static_cast<void>(cache.rebuild_all(snapshot, layout));
  auto const initial_builds = cache.projection_build_count();
  auto const initial_scans = cache.match_projection_evaluation_count();

  snapshot.transcript.insert(snapshot.transcript.begin(), context_tool("read_file", "RESTORED PREFIX DUPLICATE"));
  static_cast<void>(cache.refresh_after_transcript_mutation(snapshot, 80, snapshot.tool_presentation, false, false, 1, snapshot.transcript.size()));
  expect(cache.matches().size() == 1 && cache.matches().front().item_index == 0 && cache.matches().front().identity == "tool · read_file" &&
             cache.projection_build_count() == initial_builds + 1 && cache.match_projection_evaluation_count() == initial_scans + 2,
         "positive restored-prefix shift filters both the restored projection and first retained join item, removing the newly suppressed assistant match");
}

void test_transcript_search_rebuilds_positive_shift_shortened_context_heading()
{
  ava::tui::ComposerSnapshot snapshot;
  snapshot.tool_presentation = ava::tui::ToolPresentation::Compact;
  snapshot.transcript = {ava::tui::TranscriptItem{.label = "you", .text = "unrelated retained item"}, context_tool("read_file", "first context"),
                         context_tool("grep", "second context"), context_tool("glob", "third context"), context_tool("list_directory", "fourth context")};
  auto layout = ava::tui::detail::render_transcript_layout(snapshot.transcript, 80, snapshot.tool_presentation, false, false);
  ava::tui::detail::TranscriptSearchProjectionCache cache;
  static_cast<void>(cache.update_query("tools"));
  static_cast<void>(cache.rebuild_all(snapshot, layout));
  auto const initial_matches = cache.matches();
  auto const initial_builds = cache.projection_build_count();
  auto const initial_layout_visits = cache.layout_position_visit_count();
  auto const initial_scans = cache.match_projection_evaluation_count();
  auto const initial_realignments = cache.match_entry_realign_count();
  auto const initial_splices = cache.match_entry_splice_count();

  snapshot.transcript = {ava::tui::TranscriptItem{.label = "ava", .text = "restored prefix item"}, snapshot.transcript[0], snapshot.transcript[1],
                         snapshot.transcript[2]};
  auto const update = cache.refresh_after_transcript_mutation(snapshot, 80, snapshot.tool_presentation, false, false, 1, snapshot.transcript.size());
  auto const& shortened_matches = cache.matches();
  expect(
      initial_matches.size() == 1 && initial_matches.front().item_index == 1 && initial_matches.front().detail.find("4 tools") != std::string::npos &&
          shortened_matches.size() == 1 && shortened_matches.front().item_index == 2 && shortened_matches.front().detail.find("2 tools") != std::string::npos &&
          shortened_matches.front().detail.find("4 tools") == std::string::npos && update.first_changed_match_row == 0 &&
          cache.projection_build_count() == initial_builds + 3 && cache.layout_position_visit_count() == initial_layout_visits + 3 &&
          cache.match_projection_evaluation_count() == initial_scans + 3 && cache.match_entry_realign_count() == initial_realignments + 1 &&
          cache.match_entry_splice_count() == initial_splices + 1,
      "positive prefix restoration plus tail shrink rebuilds only the restored prefix, retained join, and non-join shortened context heading owner, replacing "
      "the stale four-tool search match with the authoritative two-tool heading");
}

void test_transcript_search_realigns_context_metadata_before_later_tail_update()
{
  ava::tui::ComposerSnapshot snapshot;
  snapshot.tool_presentation = ava::tui::ToolPresentation::Compact;
  snapshot.transcript = {context_tool("read_file", "first context"), context_tool("grep", "second context")};
  auto layout = ava::tui::detail::render_transcript_layout(snapshot.transcript, 80, snapshot.tool_presentation, false, false);
  ava::tui::detail::TranscriptSearchProjectionCache cache;
  static_cast<void>(cache.update_query("tools"));
  static_cast<void>(cache.rebuild_all(snapshot, layout));

  snapshot.transcript.insert(snapshot.transcript.begin(), context_tool("glob", "restored context"));
  static_cast<void>(cache.refresh_after_transcript_mutation(snapshot, 80, snapshot.tool_presentation, false, false, 1, snapshot.transcript.size()));
  auto const restored_matches = cache.matches();
  auto const before_tail_builds = cache.projection_build_count();
  auto const before_tail_layout_visits = cache.layout_position_visit_count();
  auto const before_tail_evaluations = cache.match_projection_evaluation_count();
  auto const before_tail_realignments = cache.match_entry_realign_count();
  auto const before_tail_splices = cache.match_entry_splice_count();

  snapshot.transcript.push_back(context_tool("list_directory", "appended context"));
  auto const tail_update = cache.refresh_after_transcript_mutation(snapshot, 80, snapshot.tool_presentation, false, false, 0, 3);
  auto const& appended_matches = cache.matches();
  expect(restored_matches.size() == 1 && restored_matches.front().item_index == 0 && restored_matches.front().detail.find("3 tools") != std::string::npos &&
             appended_matches.size() == 1 && appended_matches.front().item_index == 0 && appended_matches.front().detail.find("4 tools") != std::string::npos &&
             appended_matches.front().detail.find("3 tools") == std::string::npos && tail_update.first_changed_match_row == 0 &&
             cache.projection_build_count() == before_tail_builds + 2 && cache.layout_position_visit_count() == before_tail_layout_visits + 2 &&
             cache.match_projection_evaluation_count() == before_tail_evaluations + 2 && cache.match_entry_realign_count() == before_tail_realignments &&
             cache.match_entry_splice_count() == before_tail_splices + 1,
         "a restored context tool realigns retained run metadata so a later shift-zero append invalidates the true heading owner, produces the authoritative "
         "four-tool match, and touches only the owner and changed tail");
}

void test_transcript_search_bounded_thinking_hidden_tail_and_generation()
{
  auto long_thinking = std::string{};
  for (std::size_t index = 1; index <= 24; ++index)
  {
    long_thinking += "unique-hidden-tail-token-" + std::to_string(index);
    if (index < 24)
      long_thinking.push_back('\n');
  }
  ava::tui::ComposerSnapshot snapshot{
      .mode = "build",
      //.provider = "openai",
      //.model = "gpt-5.5",
      .session_id = "session_thinking_search",
      .input = "",
      .status = "ready",
      .transcript = {ava::tui::TranscriptItem{.label = "thinking", .text = long_thinking},
                     ava::tui::TranscriptItem{.label = "ava", .text = "assistant after thinking"}},
      .width = 80,
      .height = 40,
      .thinking_visible = true,
  };

  auto collapsed_layout = ava::tui::detail::render_transcript_layout(snapshot.transcript, 80, snapshot.tool_presentation, snapshot.thinking_visible, false);
  auto hidden_matches = ava::tui::detail::build_transcript_search_matches(snapshot, collapsed_layout, "unique-hidden-tail-token-24");
  auto visible_matches = ava::tui::detail::build_transcript_search_matches(snapshot, collapsed_layout, "unique-hidden-tail-token-1");
  expect(hidden_matches.empty() && !visible_matches.empty(),
         "transcript search misses the collapsed thinking hidden tail and still hits currently rendered thinking rows");

  snapshot.transcript[0].thinking_expanded = true;
  ++snapshot.transcript_generation;
  auto expanded_layout = ava::tui::detail::render_transcript_layout(snapshot.transcript, 80, snapshot.tool_presentation, snapshot.thinking_visible, false);
  auto expanded_matches = ava::tui::detail::build_transcript_search_matches(snapshot, expanded_layout, "unique-hidden-tail-token-24");
  expect(expanded_matches.size() == 1 && expanded_matches.front().item_index == 0,
         "transcript search hits the previously hidden thinking tail after explicit expand");

  ava::tui::detail::TranscriptSearchProjectionCache cache;
  static_cast<void>(cache.update_query("unique-hidden-tail-token-24"));
  static_cast<void>(cache.refresh_after_transcript_mutation(snapshot, 80, snapshot.tool_presentation, true, false, 0, 0));
  expect(cache.matches().size() == 1 && cache.matches().front().item_index == 0, "search projection cache indexes expanded thinking tails after rebuild");
  auto const before_builds = cache.authoritative_mutation_item_render_count();
  // Positive index shift: a retained prefix item is inserted ahead of the expanded thinking block.
  snapshot.transcript.insert(snapshot.transcript.begin(), ava::tui::TranscriptItem{.label = "you", .text = "prefix"});
  ++snapshot.transcript_generation;
  auto const update = cache.refresh_after_transcript_mutation(snapshot, 80, snapshot.tool_presentation, true, false, 1, snapshot.transcript.size());
  expect(cache.authoritative_mutation_item_render_count() > before_builds &&
             std::ranges::any_of(cache.matches(), [](auto const& match) { return match.item_index == 1; }),
         "thinking search cache realigns match item indices across a positive cap/index shift");
  static_cast<void>(update);
}

void test_transcript_search_excludes_private_launch_rows_from_authoritative_projections()
{
  ava::tui::ComposerSnapshot snapshot;
  snapshot.tool_presentation = ava::tui::ToolPresentation::Expanded;
  snapshot.transcript = {private_launch_task_item(), ava::tui::TranscriptItem{.label = "ava", .text = "ordinary following assistant content"}};
  constexpr auto width = std::size_t{32};
  auto const layout = ava::tui::detail::render_transcript_layout(snapshot.transcript, width, snapshot.tool_presentation, snapshot.thinking_visible, false);
  auto visible = tui_test_support::join_visible_lines(layout.lines);
  std::erase_if(visible, [](unsigned char ch) { return ch == ' ' || ch == '\n'; });
  auto const private_rows = static_cast<std::size_t>(std::ranges::count(layout.presentation_private_rows, true));

  ava::tui::detail::TranscriptSearchProjectionCache cache;
  static_cast<void>(cache.update_query("PRIVATE MODEL SEARCH TOKEN"));
  static_cast<void>(cache.rebuild_all(snapshot, layout));
  auto const model_matches = cache.matches();
  static_cast<void>(cache.update_query("private-thinking-token"));
  auto const thinking_matches = cache.matches();
  static_cast<void>(cache.update_query("Explore"));
  auto const ordinary_matches = cache.matches();
  static_cast<void>(cache.update_query("ordinary following assistant"));
  auto const following_matches = cache.matches();

  snapshot.transcript[0].tool->subagent_launch_display =
      ava::agent::SubagentLaunchDisplay::normalized("PRIVATE MUTATED MODEL TOKEN", std::string_view("mutated-private-thinking"));
  static_cast<void>(cache.update_query("PRIVATE MUTATED MODEL TOKEN"));
  static_cast<void>(cache.refresh_after_transcript_mutation(snapshot, width, snapshot.tool_presentation, snapshot.thinking_visible, false, 0, 0));
  auto const mutated_matches = cache.matches();

  expect(visible.find("PRIVATEMODELSEARCHTOKEN") != std::string::npos && private_rows >= 2 && layout.presentation_private_rows.size() == layout.lines.size() &&
             model_matches.empty() && thinking_matches.empty() && ordinary_matches.size() == 1 && ordinary_matches.front().item_index == 0 &&
             ordinary_matches.front().detail.find("PRIVATE MODEL SEARCH TOKEN") == std::string::npos && following_matches.size() == 1 &&
             following_matches.front().item_index == 1 && mutated_matches.empty(),
         "authoritative transcript search cache keeps wrapped task launch model/reasoning visible but omits every private row from initial and changed-suffix "
         "projections, details, and matches while retaining ordinary card and following text");
}

void test_transcript_search_details_and_queries_are_bounded()
{
  ava::tui::ComposerSnapshot snapshot;
  snapshot.transcript = {ava::tui::TranscriptItem{.label = "ava", .text = "unused"}};
  ava::tui::detail::TranscriptLayout layout{
      .lines = {std::string(400, 'x') + " MATCH"}, .message_starts = {0}, .content_starts = {0}, .message_item_indices = {0}};
  auto matches = ava::tui::detail::build_transcript_search_matches(snapshot, layout, "match");
  std::string max_query(ava::tui::detail::kMaxTranscriptSearchQueryBytes - 1, 'a');
  max_query.push_back('b');
  auto repetitive_candidate = std::string(16 * 1024, 'a') + "b";
  std::string too_long(ava::tui::detail::kMaxTranscriptSearchQueryBytes + 1, 'q');
  expect(matches.size() == 1 && matches.front().detail.size() <= ava::tui::detail::kMaxTranscriptSearchDetailBytes &&
             ava::tui::detail::transcript_search_literal_match(repetitive_candidate, max_query) && !ava::tui::detail::transcript_search_query_valid(too_long) &&
             !ava::tui::detail::transcript_search_query_valid("line\nbreak") && ava::tui::detail::transcript_search_query_valid("spaces are literal"),
         "transcript search bounds rendered details, handles a maximum-length repetitive literal, and rejects oversized or control-bearing modal queries");
}

}  // namespace

void run_tui_transcript_search_tests()
{
  test_transcript_search_literal_and_sanitized_rendered_scope();
  test_transcript_search_uses_current_rendered_tool_and_thinking_presentation();
  test_transcript_search_is_stable_across_render_widths();
  test_transcript_search_projection_match_and_modal_updates_are_suffix_bounded();
  test_transcript_search_roomy_direct_item_render_matches_full_layout_projection();
  test_transcript_search_trailing_space_literal_survives_direct_mutation();
  test_transcript_search_rebuilds_negative_shift_render_boundaries();
  test_transcript_search_rebuilds_positive_shift_join_boundary();
  test_transcript_search_rebuilds_positive_shift_shortened_context_heading();
  test_transcript_search_realigns_context_metadata_before_later_tail_update();
  test_transcript_search_bounded_thinking_hidden_tail_and_generation();
  test_transcript_search_excludes_private_launch_rows_from_authoritative_projections();
  test_transcript_search_details_and_queries_are_bounded();
}

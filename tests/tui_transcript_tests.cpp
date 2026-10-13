#include "sys.h"
#include "tests/support/test_harness.h"
#include "tests/support/tui_test_support.h"
#include "ava/tui/composer.h"
#include "ava/tui/composer_internal.h"
#include "ava/tui/runtime_internal.h"
#include "ava/tui/runtime_transcript_internal.h"
#include "ava/tui/tool_cards.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <iostream>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

void run_tui_transcript_tests_part_1()
{
  tui_test_support::TuiFrontEndFixture frontend;
  auto const multiline_input =
      ava::tui::render_composer(frontend.frontend(), ava::tui::ComposerSnapshot{.mode = "build",
                                                                                //.provider = "openai",
                                                                                //.model = "gpt-5.5",
                                                                                .session_id = "session_test",
                                                                                .input = "first\nsecond",
                                                                                .status = "ready",
                                                                                .transcript = {},
                                                                                .width = 50,
                                                                                .height = 8,
                                                                                .tool_presentation = ava::tui::ToolPresentation::Compact});
  expect(multiline_input.size() == 8 && strip_sgr(multiline_input[5]).starts_with("│  first") && strip_sgr(multiline_input[6]).starts_with("│  second") &&
             strip_sgr(multiline_input[7]).starts_with("│  GPT-5.5") && multiline_input[4].find("\x1b[48;2;26;31;46m") == std::string::npos &&
             std::ranges::none_of(multiline_input, [](std::string const& line) { return strip_sgr(line).find("❯") != std::string::npos; }),
         "tui renders multiline input plus one footer with the same three-column prefix and no surface padding");
  auto const empty_composer_height =
      ava::tui::render_composer(frontend.frontend(), ava::tui::ComposerSnapshot{.mode = "build",
                                                                                //.provider = "openai",
                                                                                //.model = "gpt-5.5",
                                                                                .session_id = "session_test",
                                                                                .input = "",
                                                                                .status = "ready",
                                                                                .transcript = {},
                                                                                .width = 50,
                                                                                .height = 12,
                                                                                .tool_presentation = ava::tui::ToolPresentation::Compact});
  auto const grown_composer_height =
      ava::tui::render_composer(frontend.frontend(), ava::tui::ComposerSnapshot{.mode = "build",
                                                                                //.provider = "openai",
                                                                                //.model = "gpt-5.5",
                                                                                .session_id = "session_test",
                                                                                .input = "one\ntwo\nthree\nfour\nfive",
                                                                                .status = "ready",
                                                                                .transcript = {},
                                                                                .width = 50,
                                                                                .height = 12,
                                                                                .tool_presentation = ava::tui::ToolPresentation::Compact});
  auto const dock_gutter_rows = [](std::vector<std::string> const& rendered) {
    return static_cast<std::size_t>(std::ranges::count_if(rendered, [](std::string const& line) {
      return strip_sgr(line).starts_with("│  ") && line.find("\x1b[49m") != std::string::npos && line.find("\x1b[48;2;26;31;46m") == std::string::npos;
    }));
  };
  expect(dock_gutter_rows(grown_composer_height) > dock_gutter_rows(empty_composer_height) &&
             std::ranges::any_of(grown_composer_height, [](std::string const& line) { return strip_sgr(line).find("│  five") != std::string::npos; }) &&
             std::ranges::none_of(grown_composer_height, [](std::string const& line) { return line.find("\x1b[48;2;26;31;46m") != std::string::npos; }),
         "tui composer grows with multiline input on the screen background and keeps the latest line visible");
  auto const tall_draft =
      ava::tui::render_composer(frontend.frontend(), ava::tui::ComposerSnapshot{.mode = "build",
                                                                                //.provider = "openai",
                                                                                //.model = "gpt-5.5",
                                                                                .session_id = "session_test",
                                                                                .input = "one\ntwo\nthree\nfour\nfive\nsix\nseven\neight\nnine",
                                                                                .status = "ready",
                                                                                .transcript = {},
                                                                                .width = 70,
                                                                                .height = 12,
                                                                                .tool_presentation = ava::tui::ToolPresentation::Compact});
  expect(std::ranges::none_of(tall_draft, [](std::string const& line) { return strip_sgr(line).find("draft +") != std::string::npos; }) &&
             std::ranges::any_of(tall_draft, [](std::string const& line) { return strip_sgr(line).find("│  nine") != std::string::npos; }) &&
             std::ranges::none_of(tall_draft, [](std::string const& line) { return strip_sgr(line).find("│  one") != std::string::npos; }),
         "tui composer hides draft overflow text while keeping the latest draft line visible");
  auto const scrolled_draft =
      ava::tui::render_composer(frontend.frontend(), ava::tui::ComposerSnapshot{.mode = "build",
                                                                                //.provider = "openai",
                                                                                //.model = "gpt-5.5",
                                                                                .session_id = "session_test",
                                                                                .input = "one\ntwo\nthree\nfour\nfive\nsix\nseven\neight\nnine",
                                                                                .status = "ready",
                                                                                .transcript = {},
                                                                                .width = 70,
                                                                                .height = 12,
                                                                                .input_cursor = std::string::npos,
                                                                                .sidebar = std::nullopt,
                                                                                .draft_scroll_offset = 2});
  expect(std::ranges::any_of(scrolled_draft, [](std::string const& line) { return strip_sgr(line).find("│  one") != std::string::npos; }) &&
             std::ranges::none_of(scrolled_draft, [](std::string const& line) { return strip_sgr(line).find("│  nine") != std::string::npos; }) &&
             std::ranges::none_of(scrolled_draft, [](std::string const& line) { return strip_sgr(line).find("draft +") != std::string::npos; }),
         "tui composer draft scroll offset shows older draft lines without footer overflow text");

  std::vector<ava::tui::TranscriptItem> many_items;
  for (int index = 0; index < 20; ++index)
  {
    many_items.push_back(ava::tui::TranscriptItem{.label = "line", .text = "item " + std::to_string(index)});
  }
  auto const scrolled = ava::tui::render_composer(frontend.frontend(), ava::tui::ComposerSnapshot{.mode = "build",
                                                                                                  //.provider = "openai",
                                                                                                  //.model = "gpt-5.5",
                                                                                                  .session_id = "session_test",
                                                                                                  .input = "",
                                                                                                  .status = "ready",
                                                                                                  .transcript = many_items,
                                                                                                  .width = 40,
                                                                                                  .height = 12,
                                                                                                  .tool_presentation = ava::tui::ToolPresentation::Compact});
  expect(std::ranges::any_of(scrolled, [](std::string const& line) { return line.find("item 19") != std::string::npos; }) &&
             std::ranges::none_of(scrolled, [](std::string const& line) { return line.find("lines hidden") != std::string::npos; }) &&
             std::ranges::none_of(scrolled, [](std::string const& line) { return line.find("item 0") != std::string::npos; }),
         "tui transcript viewport keeps newest lines without hidden-line banners");

  auto parity_120_snapshot = ava::tui::ComposerSnapshot{.mode = "build",
                                                        //.provider = "openai",
                                                        //.model = "gpt-5.5",
                                                        .session_id = "session_canvas_scroll_parity",
                                                        .input = "",
                                                        .status = "ready",
                                                        .transcript = many_items,
                                                        .width = 120,
                                                        .height = 12};
  parity_120_snapshot.transcript.push_back(ava::tui::TranscriptItem{.label = "ava", .text = std::string(260, 'w')});
  auto parity_160_snapshot = parity_120_snapshot;
  parity_160_snapshot.width = 160;
  auto const parity_120_max = ava::tui::composer_max_transcript_scroll_offset(parity_120_snapshot, parity_120_snapshot.width, parity_120_snapshot.height);
  auto const parity_160_max = ava::tui::composer_max_transcript_scroll_offset(parity_160_snapshot, parity_160_snapshot.width, parity_160_snapshot.height);
  auto const parity_120_frame = ava::tui::render_composer(frontend.frontend(), parity_120_snapshot);
  auto const parity_160_frame = ava::tui::render_composer(frontend.frontend(), parity_160_snapshot);
  auto centered_frame_matches = parity_120_frame.size() == parity_160_frame.size();
  for (std::size_t index = 0; centered_frame_matches && index < parity_120_frame.size(); ++index)
  {
    auto ordinary = strip_sgr(parity_120_frame[index]);
    ordinary.append(120 - std::min<std::size_t>(120, visible_columns(ordinary)), ' ');
    auto const wide = strip_sgr(parity_160_frame[index]);
    centered_frame_matches = visible_columns(wide) == 160 && wide.size() >= 40 && wide.substr(20, wide.size() - 40) == ordinary;
  }
  expect(parity_160_max == parity_120_max && centered_frame_matches,
         "tui 160-column centered transcript preserves exact 120-column wrapping, viewport, and max-scroll geometry");

  auto const scrolled_up = ava::tui::render_composer(frontend.frontend(), ava::tui::ComposerSnapshot{.mode = "build",
                                                                                                     //.provider = "openai",
                                                                                                     //.model = "gpt-5.5",
                                                                                                     .session_id = "session_test",
                                                                                                     .input = "",
                                                                                                     .status = "ready",
                                                                                                     .transcript = many_items,
                                                                                                     .selected_slash_command_index = 0,
                                                                                                     .transcript_scroll_offset = 4,
                                                                                                     .width = 80,
                                                                                                     .height = 12,
                                                                                                     .tool_presentation = ava::tui::ToolPresentation::Compact});
  expect(std::ranges::none_of(scrolled_up, [](std::string const& line) { return line.find("lines hidden") != std::string::npos; }) &&
             std::ranges::any_of(scrolled_up, [](std::string const& line) { return line.find("item 15") != std::string::npos; }) &&
             std::ranges::none_of(scrolled_up, [](std::string const& line) { return line.find("item 19") != std::string::npos; }),
         "tui transcript viewport supports an explicit scroll offset");

  auto const detached_scroll =
      ava::tui::render_composer(frontend.frontend(), ava::tui::ComposerSnapshot{.mode = "build",
                                                                                //.provider = "openai",
                                                                                //.model = "gpt-5.5",
                                                                                .session_id = "session_test",
                                                                                .input = "",
                                                                                .status = "ready",
                                                                                .transcript = many_items,
                                                                                .transcript_scroll_offset = 4,
                                                                                .transcript_new_output_count = 3,
                                                                                .width = 80,
                                                                                .height = 12,
                                                                                .tool_presentation = ava::tui::ToolPresentation::Compact});
  expect(std::ranges::none_of(detached_scroll,
                              [](std::string const& line) {
                                auto const visible = strip_sgr(line);
                                return visible.find("scrollback detached") != std::string::npos || visible.find("updates below") != std::string::npos ||
                                       visible.find("jump_to_bottom") != std::string::npos;
                              }) &&
             std::ranges::any_of(detached_scroll, [](std::string const& line) { return strip_sgr(line).find("item 15") != std::string::npos; }) &&
             std::ranges::none_of(detached_scroll, [](std::string const& line) { return strip_sgr(line).find("item 19") != std::string::npos; }),
         "tui detached transcript preserves its scroll position and output accounting without rendering a banner or consuming the first row");

  auto docked_scrollback_snapshot = ava::tui::ComposerSnapshot{
      .mode = "build",
      //.provider = "openai",
      //.model = "gpt-5.5",
      .session_id = "session_docked_scrollback",
      .input = "",
      .status = "invalid_argument: first alert row\nsecond alert row\nthird alert row\nfourth alert row",
      .transcript = {ava::tui::TranscriptItem{.label = "status", .text = "OLDEST-ROW-HIDDEN-BY-INDICATOR\nOLDEST-ROW-REACHABLE"}},
      .width = 72,
      .height = 12,
      .queued_messages = {ava::tui::QueuedMessageItem{.id = "q1", .kind = "follow-up", .text = "first queued task"},
                          ava::tui::QueuedMessageItem{.id = "q2", .kind = "steer", .text = "second queued task"}},
      .pending_attachments = {ava::tui::PendingAttachmentItem{
          .label = "preview.png",
          .detail = "(image/png, 4 bytes) preview kitty",
          .preview = ava::tui::PendingAttachmentItem::Preview{.protocol = ava::tui::TerminalImageProtocol::Kitty,
                                                              .base64_data = std::make_shared<std::string const>("AAAA"),
                                                              .dimensions = ava::tui::ImageDimensions{.width_px = 200, .height_px = 10},
                                                              .image_id = 77}}}};
  for (int index = 1; index < 20; ++index)
  {
    docked_scrollback_snapshot.transcript.push_back(ava::tui::TranscriptItem{.label = "status", .text = "TRANSCRIPT-ROW-" + std::to_string(index)});
  }
  auto const docked_scrollback_full = ava::tui::detail::render_transcript_lines(docked_scrollback_snapshot.transcript, 72, false, true);
  auto const old_local_transcript_height =
      docked_scrollback_snapshot.height -
      ava::tui::detail::composer_block_line_count(docked_scrollback_snapshot, docked_scrollback_snapshot.height, docked_scrollback_snapshot.width);
  auto const old_local_max = docked_scrollback_full.size() - old_local_transcript_height;
  auto const shared_docked_max =
      ava::tui::composer_max_transcript_scroll_offset(docked_scrollback_snapshot, docked_scrollback_snapshot.width, docked_scrollback_snapshot.height);
  docked_scrollback_snapshot.transcript_scroll_offset = shared_docked_max;
  auto const maximally_scrolled_docked_frame = ava::tui::render_composer(frontend.frontend(), docked_scrollback_snapshot);
  expect(shared_docked_max == docked_scrollback_full.size() - 2 && shared_docked_max > old_local_max &&
             std::ranges::any_of(maximally_scrolled_docked_frame,
                                 [](std::string const& line) { return strip_sgr(line).find("OLDEST-ROW-REACHABLE") != std::string::npos; }) &&
             std::ranges::any_of(maximally_scrolled_docked_frame,
                                 [](std::string const& line) { return strip_sgr(line).find("third alert row ...") != std::string::npos; }) &&
             std::ranges::any_of(maximally_scrolled_docked_frame,
                                 [](std::string const& line) { return strip_sgr(line).find("second queued task") != std::string::npos; }) &&
             std::ranges::any_of(maximally_scrolled_docked_frame,
                                 [](std::string const& line) { return strip_sgr(line).find("attached image preview.png") != std::string::npos; }),
         "tui dock-aware shared transcript scroll limit reflects two visible transcript rows and reaches the oldest transcript while a multiline alert, "
         "queue, and attachment preview remain docked");

  auto diff_prompt_scrollback_snapshot = docked_scrollback_snapshot;
  diff_prompt_scrollback_snapshot.status = "permission required";
  diff_prompt_scrollback_snapshot.height = 18;
  diff_prompt_scrollback_snapshot.transcript_scroll_offset = 0;
  diff_prompt_scrollback_snapshot.queued_messages.clear();
  diff_prompt_scrollback_snapshot.pending_attachments.clear();
  diff_prompt_scrollback_snapshot.permission_prompt = ava::tui::PermissionPromptView{
      .tool_name = "edit",
      .operation = "edit",
      .target = "src/ava/tui/composer.cpp",
      .reason = "review a bounded diff",
      .diff_preview =
          "--- old\n+++ new\n@@ -1,8 +1,8 @@\n-old one\n+new one\n-old two\n+new two\n-old three\n+new three\n-old four\n+new four\n-old five\n+new five\n"};
  auto const diff_prompt_max = ava::tui::composer_max_transcript_scroll_offset(diff_prompt_scrollback_snapshot, diff_prompt_scrollback_snapshot.width,
                                                                               diff_prompt_scrollback_snapshot.height);
  diff_prompt_scrollback_snapshot.transcript_scroll_offset = diff_prompt_max;
  auto const maximally_scrolled_diff_prompt_frame = ava::tui::render_composer(frontend.frontend(), diff_prompt_scrollback_snapshot);
  expect(diff_prompt_max == docked_scrollback_full.size() - 4 &&
             std::ranges::any_of(maximally_scrolled_diff_prompt_frame,
                                 [](std::string const& line) { return strip_sgr(line).find("OLDEST-ROW-REACHABLE") != std::string::npos; }) &&
             std::ranges::any_of(maximally_scrolled_diff_prompt_frame,
                                 [](std::string const& line) { return strip_sgr(line).find("Permission required") != std::string::npos; }) &&
             std::ranges::any_of(maximally_scrolled_diff_prompt_frame,
                                 [](std::string const& line) { return strip_sgr(line).find("diff lines hidden") != std::string::npos; }),
         "tui shared transcript scroll limit includes the twelve-row diff permission dock and reduced composer reservation");

  auto const wrapped_transcript = ava::tui::render_composer(
      frontend.frontend(), ava::tui::ComposerSnapshot{.mode = "build",
                                                      //.provider = "openai",
                                                      //.model = "gpt-5.5",
                                                      .session_id = "session_test",
                                                      .input = "",
                                                      .status = "ready",
                                                      .transcript = {ava::tui::TranscriptItem{.label = "ava",
                                                                                              .text = "abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyz"
                                                                                                      "abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyz"
                                                                                                      "abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyz"
                                                                                                      "abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyz"
                                                                                                      "abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyz"}},
                                                      .selected_slash_command_index = 0,
                                                      .transcript_scroll_offset = 1,
                                                      .width = 60,
                                                      .height = 8,
                                                      .tool_presentation = ava::tui::ToolPresentation::Compact});
  auto const wrapped_latest = ava::tui::render_composer(
      frontend.frontend(), ava::tui::ComposerSnapshot{.mode = "build",
                                                      //.provider = "openai",
                                                      //.model = "gpt-5.5",
                                                      .session_id = "session_test",
                                                      .input = "",
                                                      .status = "ready",
                                                      .transcript = {ava::tui::TranscriptItem{.label = "ava",
                                                                                              .text = "abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyz"
                                                                                                      "abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyz"
                                                                                                      "abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyz"
                                                                                                      "abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyz"
                                                                                                      "abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyz"}},
                                                      .selected_slash_command_index = 0,
                                                      .transcript_scroll_offset = 0,
                                                      .width = 60,
                                                      .height = 8,
                                                      .tool_presentation = ava::tui::ToolPresentation::Compact});
  expect(std::ranges::none_of(wrapped_transcript, [](std::string const& line) { return strip_sgr(line).find("lines hidden") != std::string::npos; }) &&
             std::ranges::none_of(wrapped_latest, [](std::string const& line) { return strip_sgr(line).find("scrollback detached") != std::string::npos; }),
         "tui transcript viewport wraps long transcript text without synthetic hidden-line or detached banners");

  std::vector<ava::tui::TranscriptItem> mixed_items;
  for (int index = 0; index < 12; ++index)
  {
    mixed_items.push_back(ava::tui::TranscriptItem{.label = "line", .text = "old " + std::to_string(index)});
  }
  mixed_items.push_back(ava::tui::TranscriptItem{
      .tool = ava::tui::ToolTimelineItem{
          .status = ava::tui::ToolTimelineStatus::Success, .name = "grep", .argument_summary = "needle", .result_summary = "2 matches"}});
  mixed_items.push_back(ava::tui::TranscriptItem{.label = "ava", .text = "done"});
  auto const mixed_scrolled =
      ava::tui::render_composer(frontend.frontend(), ava::tui::ComposerSnapshot{.mode = "build",
                                                                                //.provider = "openai",
                                                                                //.model = "gpt-5.5",
                                                                                .session_id = "session_test",
                                                                                .input = "",
                                                                                .status = "ready",
                                                                                .transcript = mixed_items,
                                                                                .width = 60,
                                                                                .height = 12,
                                                                                .tool_presentation = ava::tui::ToolPresentation::Compact});
  auto const mixed_visible = tui_test_support::join_visible_lines(mixed_scrolled);
  expect(mixed_visible.find("lines hidden") == std::string::npos && mixed_visible.find("+ grep") != std::string::npos &&
             mixed_visible.find("2 matches") != std::string::npos && mixed_visible.find("done") != std::string::npos &&
             mixed_visible.find("AVA") == std::string::npos && mixed_visible.find("old 0") == std::string::npos,
         "tui transcript viewport scrolls mixed text and tool-card lines together without hidden-line banners");

  auto const multiline =
      ava::tui::render_composer(frontend.frontend(), ava::tui::ComposerSnapshot{.mode = "build",
                                                                                //.provider = "openai",
                                                                                //.model = "gpt-5.5",
                                                                                .session_id = "session_test",
                                                                                .input = "",
                                                                                .status = "ready",
                                                                                .transcript = {ava::tui::TranscriptItem{.label = "ava", .text = "one\ntwo"}},
                                                                                .width = 80,
                                                                                .height = 14,
                                                                                .tool_presentation = ava::tui::ToolPresentation::Compact});
  expect(std::ranges::any_of(multiline,
                             [](std::string const& line) {
                               auto visible = strip_sgr(line);
                               return visible.find("one") != std::string::npos;
                             }) &&
             std::ranges::any_of(multiline,
                                 [](std::string const& line) {
                                   auto visible = strip_sgr(line);
                                   return visible.find("two") != std::string::npos;
                                 }),
         "tui renders multiline assistant transcript content inside the message block");
}
namespace {
void test_tui_large_render_performance_budget()
{
  tui_test_support::TuiFrontEndFixture frontend;
  std::vector<ava::tui::TranscriptItem> transcript;
  transcript.reserve(900);
  for (int index = 0; index < 300; ++index)
  {
    transcript.push_back(ava::tui::TranscriptItem{
        .label = "you",
        .text = "performance input " + std::to_string(index) + " with CJK \xE7\x95\x8C combining e\xCC\x81 and a very-long-token-that-must-wrap-safely"});
    transcript.push_back(
        ava::tui::TranscriptItem{.label = "ava",
                                 .text = "performance answer " + std::to_string(index) + " keeps rendered rows bounded while the transcript is large",
                                 .meta = "Build · GPT-5.5",
                                 .thinking = "reasoning summary " + std::to_string(index)});
    transcript.push_back(ava::tui::TranscriptItem{.tool = ava::tui::ToolTimelineItem{.status = ava::tui::ToolTimelineStatus::Success,
                                                                                     .name = "read_file",
                                                                                     .argument_summary = "path=src/ava/tui/composer.cpp",
                                                                                     .result_summary = "read 1024 bytes",
                                                                                     .call_id = "perf_" + std::to_string(index),
                                                                                     .lifecycle = ava::tui::ToolLifecycleState::Complete}});
  }

  auto const start = std::chrono::steady_clock::now();
  std::vector<std::string> frame;
  for (int pass = 0; pass < 4; ++pass)
  {
    frame = ava::tui::render_composer(frontend.frontend(), ava::tui::ComposerSnapshot{.mode = "build",
                                                                                      //.provider = "openai",
                                                                                      //.model = "gpt-5.5",
                                                                                      .session_id = "session_perf",
                                                                                      .input = "draft \xE7\x95\x8C",
                                                                                      .status = "ready",
                                                                                      .processing = true,
                                                                                      .transcript = transcript,
                                                                                      .transcript_scroll_offset = static_cast<std::size_t>(pass * 20),
                                                                                      .width = 120,
                                                                                      .height = 36,
                                                                                      .input_cursor = std::string::npos,
                                                                                      .tool_presentation = ava::tui::ToolPresentation::Expanded,
                                                                                      .thinking_visible = true});
  }
  auto const elapsed = std::chrono::steady_clock::now() - start;
  std::size_t max_columns = 0;
  std::string widest_line;
  for (auto const& line : frame)
  {
    auto const columns = visible_columns(line);
    if (columns > max_columns)
    {
      max_columns = columns;
      widest_line = strip_sgr(line);
    }
  }
  if (max_columns > 120)
  {
    std::cerr << "tui large render widest line has " << max_columns << " columns: " << widest_line << '\n';
  }
  expect(frame.size() == 36, "tui large render performance frame keeps the requested height");
  expect(std::ranges::all_of(frame, [](std::string const& line) { return line.find('\n') == std::string::npos && visible_columns(line) <= 120; }),
         "tui large render performance frame keeps every rendered line inside the requested width");
  expect(elapsed < std::chrono::seconds(5), "tui large render performance budget catches pathological redraw slowdowns without a real terminal");
}
void test_tui_f2_transcript_hierarchy_and_tool_shell()
{
  auto const& joined = tui_test_support::join_plain_lines;
  auto const& occurrences = tui_test_support::count_occurrences;
  using tui_test_support::plain_lines;
  auto const successful_tool = [](std::string name, std::string args, std::string result) {
    return ava::tui::TranscriptItem{.tool = ava::tui::ToolTimelineItem{.status = ava::tui::ToolTimelineStatus::Success,
                                                                       .name = std::move(name),
                                                                       .argument_summary = std::move(args),
                                                                       .result_summary = std::move(result),
                                                                       .lifecycle = ava::tui::ToolLifecycleState::Complete}};
  };

  auto const mixed_transcript =
      std::vector<ava::tui::TranscriptItem>{ava::tui::TranscriptItem{.label = "you", .text = "first user turn"},
                                            ava::tui::TranscriptItem{.label = "ava", .text = "first assistant answer", .thinking = "checked the request"},
                                            successful_tool("read_file", "path=src/main.cpp", "read 12 lines"),
                                            successful_tool("grep", "pattern=TODO", "3 matches"),
                                            ava::tui::TranscriptItem{.label = "ava", .text = "assistant continuation"},
                                            ava::tui::TranscriptItem{.label = "error", .text = "provider failed"},
                                            ava::tui::TranscriptItem{.label = "status", .text = "retry payload exactly"},
                                            ava::tui::TranscriptItem{.label = "queue", .text = ""},
                                            ava::tui::TranscriptItem{.label = "audit", .text = ""},
                                            ava::tui::TranscriptItem{.label = "", .text = ""},
                                            ava::tui::TranscriptItem{.label = "you", .text = "second user turn"},
                                            ava::tui::TranscriptItem{.label = "thinking", .text = "second flow reasoning"},
                                            successful_tool("write", "path=notes.txt", "wrote 27 bytes"),
                                            ava::tui::TranscriptItem{.label = "ava", .text = "second assistant answer"}};
  auto const mixed = plain_lines(ava::tui::detail::render_transcript_lines(mixed_transcript, 96, false, true));
  auto const mixed_text = joined(mixed);
  auto const blank_count = std::ranges::count(mixed, std::string{});
  auto const line_index = [&mixed](std::string_view needle) {
    auto const found = std::ranges::find_if(mixed, [needle](std::string const& line) { return line.find(needle) != std::string::npos; });
    return found == mixed.end() ? mixed.size() : static_cast<std::size_t>(found - mixed.begin());
  };
  auto const first_assistant = line_index("Thinking: checked the request");
  auto const first_answer = line_index("first assistant answer");
  auto const context_heading = line_index("context gathering");
  auto const first_tool = line_index("read_file");
  auto const second_tool = line_index("grep");
  auto const continuation = line_index("assistant continuation");
  auto const error = line_index("provider failed");
  auto const first_system = line_index("retry payload exactly");
  auto const queue_fallback = line_index("· queue");
  auto const audit_fallback = line_index("· audit");
  auto const event_fallback = line_index("· event");
  auto const second_user = line_index("second user turn");
  auto const second_flow = line_index("second flow reasoning");
  expect(blank_count == 10 && !mixed.empty() && !mixed.back().empty() && first_assistant > 0 && mixed[first_assistant - 1].empty() &&
             first_answer > first_assistant && mixed[first_answer - 1].empty() && context_heading > first_answer && mixed[context_heading - 1].empty() &&
             first_tool > context_heading && !mixed[first_tool - 1].empty() && second_tool > first_tool && !mixed[second_tool - 1].empty() &&
             continuation > second_tool && mixed[continuation - 1].empty() && error > 0 && mixed[error - 1].empty() && first_system > 0 &&
             mixed[first_system - 1].empty() && queue_fallback > first_system && !mixed[queue_fallback - 1].empty() && audit_fallback > queue_fallback &&
             !mixed[audit_fallback - 1].empty() && event_fallback > audit_fallback && !mixed[event_fallback - 1].empty() && second_user > 0 &&
             mixed[second_user - 1].empty() && second_flow > 0 && mixed[second_flow - 1].empty(),
         "tui F2 roomy transcript uses one blank between prose, reasoning, tool runs, and ownership groups, keeps consecutive tools compact, and adds no "
         "trailing blank");
  expect(mixed_text.find("Thinking: checked the request") != std::string::npos && mixed_text.find("!") != std::string::npos &&
             mixed_text.find("retry payload exactly") != std::string::npos && mixed_text.find("· queue") != std::string::npos &&
             mixed_text.find("· audit") != std::string::npos && mixed_text.find("· event") != std::string::npos &&
             mixed_text.find("first user turn") != std::string::npos && mixed_text.find("first assistant answer") != std::string::npos,
         "tui F2 transcript preserves ownership markers and truthful generic text with explicit label/event fallbacks");

  auto const compact_width = plain_lines(ava::tui::detail::render_transcript_lines(mixed_transcript, 43, false, true));
  auto const compact_height = plain_lines(ava::tui::detail::render_transcript_lines(mixed_transcript, 100, false, true, true));
  expect(std::ranges::none_of(compact_width, [](std::string const& line) { return line.empty(); }),
         "tui F2 transcript removes inter-group spacing below 44 columns");
  expect(std::ranges::none_of(compact_height, [](std::string const& line) { return line.empty(); }),
         "tui F2 explicit short-height spacing mode removes inter-group blanks at otherwise roomy widths");

  auto const visible_assistant_flow = std::vector<ava::tui::TranscriptItem>{ava::tui::TranscriptItem{.label = "ava", .text = "before quiet poll"},
                                                                            ava::tui::TranscriptItem{.label = "ava", .text = "after quiet poll"}};
  auto quiet_poll_flow = visible_assistant_flow;
  quiet_poll_flow.insert(
      quiet_poll_flow.begin() + 1,
      ava::tui::TranscriptItem{
          .tool = ava::tui::ToolTimelineItem{.status = ava::tui::ToolTimelineStatus::Success,
                                             .name = "job",
                                             .arguments_json = R"({"action":"wait","job_id":"job_visible_spacing"})",
                                             .result_json = R"({"schema_version":1,"job_id":"job_visible_spacing","state":"running","timed_out":true})",
                                             .lifecycle = ava::tui::ToolLifecycleState::Complete}});
  auto const visible_assistant_lines = ava::tui::detail::render_transcript_lines(visible_assistant_flow, 96, false, true);
  auto const quiet_poll_lines = ava::tui::detail::render_transcript_lines(quiet_poll_flow, 96, false, true);
  auto const visible_assistant_starts = ava::tui::detail::transcript_message_start_lines(visible_assistant_flow, 96, false, true);
  auto const quiet_poll_starts = ava::tui::detail::transcript_message_start_lines(quiet_poll_flow, 96, false, true);
  auto const quiet_poll_tail = ava::tui::detail::render_transcript_tail_lines(quiet_poll_flow, 96, quiet_poll_lines.size() + 3, false, true);
  expect(quiet_poll_lines == visible_assistant_lines && quiet_poll_starts == visible_assistant_starts && quiet_poll_tail == visible_assistant_lines,
         "tui quiet job polling items are invisible to roomy spacing, message starts, and bounded tail layout");

  auto const verify_hidden_thinking_spacing = [&](ava::tui::TranscriptItem first, std::string const& label) {
    auto const transcript = std::vector<ava::tui::TranscriptItem>{std::move(first), ava::tui::TranscriptItem{.label = "thinking", .text = "hidden reasoning"},
                                                                  ava::tui::TranscriptItem{.label = "ava", .text = "visible assistant"}};
    auto const full = ava::tui::detail::render_transcript_lines(transcript, 96, false, false);
    auto const plain_full = plain_lines(full);
    auto const assistant = std::ranges::find_if(plain_full, [](std::string const& line) { return line.find("visible assistant") != std::string::npos; });
    auto const assistant_line = assistant == plain_full.end() ? plain_full.size() : static_cast<std::size_t>(assistant - plain_full.begin());
    auto const starts = ava::tui::detail::transcript_message_start_lines(transcript, 96, false, false);
    expect(assistant_line > 0 && plain_full[assistant_line - 1].empty() && starts.size() == 2 && starts.back() == assistant_line,
           "tui F2 hidden thinking retains the logical group boundary after a visible " + label + " item");
    for (auto const budget : {std::size_t{1}, full.size() - 1, full.size()})
    {
      auto const tail = ava::tui::detail::render_transcript_tail_lines(transcript, 96, budget, false, false);
      auto const suffix_start = full.size() - budget;
      expect(tail == std::vector<std::string>(full.begin() + static_cast<std::ptrdiff_t>(suffix_start), full.end()),
             "tui F2 hidden thinking full/tail parity after a visible " + label + " item at budget " + std::to_string(budget));
    }
  };
  verify_hidden_thinking_spacing(ava::tui::TranscriptItem{.label = "you", .text = "visible user"}, "user");
  verify_hidden_thinking_spacing(ava::tui::TranscriptItem{.label = "status", .text = "visible system"}, "system");
  verify_hidden_thinking_spacing(ava::tui::TranscriptItem{.label = "error", .text = "visible error"}, "error");

  auto const leading_hidden_transcript =
      std::vector<ava::tui::TranscriptItem>{ava::tui::TranscriptItem{.label = "thinking", .text = "hidden leading reasoning"},
                                            ava::tui::TranscriptItem{.label = "ava", .text = "first visible assistant"}};
  auto const leading_hidden_full = ava::tui::detail::render_transcript_lines(leading_hidden_transcript, 96, false, false);
  auto const leading_hidden_tail = ava::tui::detail::render_transcript_tail_lines(leading_hidden_transcript, 96, leading_hidden_full.size() + 3, false, false);
  auto const leading_hidden_starts = ava::tui::detail::transcript_message_start_lines(leading_hidden_transcript, 96, false, false);
  expect(!leading_hidden_full.empty() && !strip_sgr(leading_hidden_full.front()).empty() && leading_hidden_tail == leading_hidden_full &&
             leading_hidden_starts == std::vector<std::size_t>{0},
         "tui F2 leading hidden entries leave full, bounded tail, and message starts free of a leading blank");

  auto generic_tool = ava::tui::ToolTimelineItem{
      .status = ava::tui::ToolTimelineStatus::Success,
      .name = "write",
      .argument_summary = "path=src/main.cpp",
      .result_summary = "wrote 27 bytes",
      .lifecycle = ava::tui::ToolLifecycleState::Complete,
      .permissions = {ava::tui::ToolPermissionAuditItem{.permission_request_id = "permreq_write", .decision = "allow", .reason = "workspace edit"}},
      .diff = "--- src/main.cpp\n+++ src/main.cpp\n-old\n+new",
      .changed_paths = {"src/main.cpp"}};
  auto const collapsed_tool = plain_lines(ava::tui::detail::render_tool_card(generic_tool, 100, false));
  auto const collapsed_tool_text = joined(collapsed_tool);
  auto const expanded_tool = plain_lines(ava::tui::detail::render_tool_card(generic_tool, 100, true));
  auto const expanded_tool_text = joined(expanded_tool);
  auto const generic_copy = ava::tui::detail::tool_card_copy_text(generic_tool);
  expect(collapsed_tool.size() == 1 && collapsed_tool.front().find("permission allow") == std::string::npos &&
             collapsed_tool.front().find("complete") == std::string::npos && collapsed_tool.front().find("src/main.cpp") != std::string::npos &&
             occurrences(collapsed_tool_text, "wrote 27 bytes") == 1 && collapsed_tool.front().find("src/main.cpp · wrote 27 bytes") != std::string::npos,
         "tui F5 compact mutation cards prefer the changed path and result without lifecycle or routine permission receipts");
  expect(expanded_tool.size() > 1 && !expanded_tool[1].empty() && expanded_tool.front().find("src/main.cpp · wrote 27 bytes") != std::string::npos &&
             occurrences(expanded_tool_text, "wrote 27 bytes") == 1 && expanded_tool_text.find("result: wrote 27 bytes") == std::string::npos &&
             expanded_tool_text.find("src/main.cpp") != std::string::npos && expanded_tool_text.find("permission:") == std::string::npos &&
             expanded_tool_text.find("changed: src/main.cpp") != std::string::npos && expanded_tool_text.find("diff src/main.cpp:") != std::string::npos &&
             generic_copy.find("result: wrote 27 bytes") != std::string::npos && generic_copy.find("diff:") != std::string::npos,
         "tui F5 expanded mutation content keeps its human call, changed path, diff, and safe copy payload");

  auto aliased_write = ava::tui::ToolTimelineItem{.status = ava::tui::ToolTimelineStatus::Success,
                                                  .name = "write",
                                                  .argument_summary = "path=src/main.cpp",
                                                  .result_summary = "wrote 27 bytes to /workspace/src/main.cpp; report /tmp/unrelated.txt",
                                                  .result_json = R"({"output":"updated /workspace/src/main.cpp\nreport /tmp/unrelated.txt"})",
                                                  .lifecycle = ava::tui::ToolLifecycleState::Complete,
                                                  .diff = "--- /workspace/src/main.cpp\n+++ /workspace/src/main.cpp\n-old\n+new",
                                                  .changed_paths = {"/workspace/src/main.cpp"}};
  auto const aliased_write_visible = joined(plain_lines(ava::tui::detail::render_tool_card(aliased_write, 200, true)));
  auto const aliased_write_copy = ava::tui::detail::tool_card_copy_text(aliased_write);
  expect(aliased_write_visible.find("/workspace/src/main.cpp") == std::string::npos &&
             aliased_write_visible.find("changed: src/main.cpp") != std::string::npos &&
             aliased_write_visible.find("result: wrote 27 bytes to src/main.cpp; report /tmp/unrelated.txt") != std::string::npos &&
             aliased_write_visible.find("updated src/main.cpp") != std::string::npos && aliased_write_visible.find("--- src/main.cpp") != std::string::npos &&
             aliased_write_visible.find("+++ src/main.cpp") != std::string::npos && aliased_write_visible.find("/tmp/unrelated.txt") != std::string::npos,
         "tui expanded write cards project proven absolute changed-path aliases in visible changed, result, output, and diff rows only");
  expect(aliased_write.result_summary == "wrote 27 bytes to /workspace/src/main.cpp; report /tmp/unrelated.txt" &&
             aliased_write.result_json == R"({"output":"updated /workspace/src/main.cpp\nreport /tmp/unrelated.txt"})" &&
             aliased_write.diff == "--- /workspace/src/main.cpp\n+++ /workspace/src/main.cpp\n-old\n+new" &&
             aliased_write.changed_paths == std::vector<std::string>{"/workspace/src/main.cpp"} &&
             aliased_write_copy.find("changed: /workspace/src/main.cpp") != std::string::npos,
         "tui write-card display path aliases leave canonical and copy payloads intact");

  auto duplicate_tool = generic_tool;
  duplicate_tool.changed_paths.clear();
  auto duplicate_transcript = std::vector<ava::tui::TranscriptItem>{ava::tui::TranscriptItem{.tool = duplicate_tool},
                                                                    ava::tui::TranscriptItem{.label = "ava", .text = "wrote 27 bytes   "}};
  for (auto const details_visible : {false, true})
  {
    auto const duplicate_lines = plain_lines(ava::tui::detail::render_transcript_lines(duplicate_transcript, 100, details_visible, true));
    auto const duplicate_text = joined(duplicate_lines);
    expect(occurrences(duplicate_text, "wrote 27 bytes") == 1 && duplicate_text.find("permission: allow") == std::string::npos &&
               ava::tui::detail::tool_card_copy_text(duplicate_tool).find("result: wrote 27 bytes") != std::string::npos,
           std::string("tui F2 exact adjacent tool-result suppression keeps one card result and the full copy payload when details are ") +
               (details_visible ? "expanded" : "collapsed"));
  }

  auto permission_duplicate = std::vector<ava::tui::TranscriptItem>{
      ava::tui::TranscriptItem{.tool = generic_tool},
      ava::tui::TranscriptItem{.label = "ava", .text = "permission allow · reason workspace edit", .meta = "must not attach to the tool row"}};
  auto permission_near_match = permission_duplicate;
  permission_near_match.back().text += " after review";
  auto const permission_duplicate_text = joined(plain_lines(ava::tui::detail::render_transcript_lines(permission_duplicate, 100, false, true)));
  auto const permission_near_match_text = joined(plain_lines(ava::tui::detail::render_transcript_lines(permission_near_match, 100, false, true)));
  expect(occurrences(permission_duplicate_text, "permission allow") == 1 && permission_duplicate_text.find("must not attach") != std::string::npos &&
             occurrences(permission_near_match_text, "permission allow") == 1 && permission_near_match_text.find("after review") != std::string::npos,
         "tui F2 routine permission receipts are not synthesized by cards and explicit assistant text remains intact");

  auto non_adjacent = duplicate_transcript;
  non_adjacent.insert(non_adjacent.begin() + 1, ava::tui::TranscriptItem{.label = "status", .text = "historical receipt"});
  auto near_match = duplicate_transcript;
  near_match.back().text = "Wrote 27 bytes";
  auto prefix_match = duplicate_transcript;
  prefix_match.back().text = "wrote 27 bytes and refreshed the index";
  auto const non_adjacent_text = joined(plain_lines(ava::tui::detail::render_transcript_lines(non_adjacent, 100, false, true)));
  auto const near_match_text = joined(plain_lines(ava::tui::detail::render_transcript_lines(near_match, 100, false, true)));
  auto const prefix_match_text = joined(plain_lines(ava::tui::detail::render_transcript_lines(prefix_match, 100, false, true)));
  expect(occurrences(non_adjacent_text, "wrote 27 bytes") == 2 && occurrences(near_match_text, "wrote 27 bytes") == 1 &&
             occurrences(near_match_text, "Wrote 27 bytes") == 1 && occurrences(prefix_match_text, "wrote 27 bytes") == 2,
         "tui F2 duplicate suppression is immediate, case-sensitive, exact, and never a broad prefix heuristic");

  auto unicode_tool = generic_tool;
  unicode_tool.permissions.clear();
  unicode_tool.diff.clear();
  unicode_tool.changed_paths.clear();
  unicode_tool.result_summary = std::string("界🙂 ") + std::string(180, 'x');
  auto const unicode_shell = ava::tui::detail::render_tool_card(unicode_tool, 44, false);
  auto long_name_tool = unicode_tool;
  long_name_tool.name = "long_named_provider_tool_with_an_extreme_identifier";
  auto const long_name_shell = ava::tui::detail::render_tool_card(long_name_tool, 44, false);
  expect(unicode_shell.size() == 1 && strip_sgr(unicode_shell.front()).find("+ write") != std::string::npos &&
             std::ranges::all_of(unicode_shell, [](std::string const& line) { return visible_columns(line) <= 44; }),
         "tui F2 generic tool shell cell-fits long Unicode primary summaries without adding a result row");
  expect(long_name_shell.size() == 1 && strip_sgr(long_name_shell.front()).find("+ long_") != std::string::npos &&
             strip_sgr(long_name_shell.front()).find("complete") == std::string::npos && visible_columns(long_name_shell.front()) <= 44,
         "tui F5 fitted tool shell preserves status and a fitted tool identity without redundant lifecycle text");

  auto const expanded_long_result = plain_lines(ava::tui::detail::render_tool_card(unicode_tool, 44, true));
  auto const expanded_long_result_text = joined(expanded_long_result);
  auto long_argument_tool = ava::tui::ToolTimelineItem{.status = ava::tui::ToolTimelineStatus::Running,
                                                       .name = "long_named_provider_tool_with_an_extreme_identifier",
                                                       .argument_summary = "argument-with-a-deliberately-long-value-that-cannot-fit-the-shell",
                                                       .lifecycle = ava::tui::ToolLifecycleState::ExecutionStarted};
  auto const expanded_long_argument = plain_lines(ava::tui::detail::render_tool_card(long_argument_tool, 44, true));
  auto const expanded_long_argument_text = joined(expanded_long_argument);
  expect(expanded_long_result.size() > 1 && expanded_long_result_text.find("result: ") != std::string::npos &&
             expanded_long_result_text.find("界") != std::string::npos &&
             ava::tui::detail::tool_card_copy_text(unicode_tool).find("result: ") != std::string::npos && expanded_long_argument.size() > 1 &&
             expanded_long_argument_text.find("argument-with-a-deliberately") != std::string::npos &&
             expanded_long_argument_text.find("args:") == std::string::npos,
         "tui F2 expanded details retain clipped results and wrapped human calls without raw argument labels");

  auto trailing_control_tool = generic_tool;
  trailing_control_tool.result_summary = "trailing receipt\r\n\f\v";
  auto trailing_control_transcript = std::vector<ava::tui::TranscriptItem>{ava::tui::TranscriptItem{.tool = trailing_control_tool},
                                                                           ava::tui::TranscriptItem{.label = "ava", .text = "trailing receipt\r\n\f\v"}};
  auto const trailing_control_text = joined(plain_lines(ava::tui::detail::render_transcript_lines(trailing_control_transcript, 100, false, true)));
  auto text_assistant = ava::tui::TranscriptItem{.label = "ava", .text_model = ava::tui::text_from_plain("Text receipt\r\n")};
  auto text_assistant_tool = generic_tool;
  text_assistant_tool.result_summary = "Text receipt\r\n";
  auto const text_assistant_transcript =
      std::vector<ava::tui::TranscriptItem>{ava::tui::TranscriptItem{.tool = text_assistant_tool}, std::move(text_assistant)};
  auto const text_assistant_text = joined(plain_lines(ava::tui::detail::render_transcript_lines(text_assistant_transcript, 100, false, true)));
  auto middle_control = trailing_control_transcript;
  middle_control[0].tool->result_summary =
      "middle\x01"
      "control";
  middle_control[1].text =
      "middle\x02"
      "control";
  auto const middle_control_text = joined(plain_lines(ava::tui::detail::render_transcript_lines(middle_control, 100, false, true)));
  expect(
      !trailing_control_text.empty() && trailing_control_text.starts_with("  │ + write") &&
          trailing_control_text.find("permission allow") == std::string::npos && !text_assistant_text.empty() &&
          text_assistant_text.starts_with("  │ + write") && text_assistant_text.find("permission allow") == std::string::npos &&
          occurrences(middle_control_text, "middle?control") == 1,
      "tui F5 adjacent suppression trims raw and sanitized trailing ASCII whitespace while matching sanitized display identity and preserving structured Text");
}

void test_tui_detached_transcript_anchor_survives_capped_eviction()
{
  constexpr auto width = std::size_t{42};
  constexpr auto viewport_height = std::size_t{4};
  auto context_tool = [](std::string name, std::string call_id) {
    return ava::tui::TranscriptItem{.tool = ava::tui::ToolTimelineItem{.status = ava::tui::ToolTimelineStatus::Success,
                                                                       .name = std::move(name),
                                                                       .argument_summary = "path=src/context.cpp",
                                                                       .result_summary = "read context row",
                                                                       .call_id = std::move(call_id),
                                                                       .lifecycle = ava::tui::ToolLifecycleState::Complete}};
  };
  std::vector<ava::tui::TranscriptItem> submitted;
  submitted.reserve(ava::tui::kMaxTranscriptItems);
  submitted.push_back(context_tool("read_file", "context_first"));
  submitted.push_back(context_tool("grep", "context_anchor"));
  submitted.push_back(context_tool("glob", "context_retained"));
  for (std::size_t index = submitted.size(); index < ava::tui::kMaxTranscriptItems; ++index)
  {
    if (index % 11 == 0)
      submitted.push_back(ava::tui::TranscriptItem{.label = "ava", .thinking = "hidden reasoning row " + std::to_string(index)});
    else
      submitted.push_back(ava::tui::TranscriptItem{
          .label = "you",
          .text = index % 3 == 0 ? "mixed tall row " + std::to_string(index) + " wraps across several terminal columns for anchor coverage"
                                 : "mixed short row " + std::to_string(index)});
  }

  auto layout = ava::tui::detail::render_transcript_layout(submitted, width, true, false, true);
  auto anchor_message = std::ranges::find(layout.message_item_indices, std::size_t{1});
  expect(anchor_message != layout.message_item_indices.end() && layout.content_starts.size() == layout.message_starts.size(),
         "production-cap detached fixture maps synthetic context prefixes separately from item content");
  if (anchor_message == layout.message_item_indices.end())
    return;
  auto const anchor_position = static_cast<std::size_t>(std::distance(layout.message_item_indices.begin(), anchor_message));
  auto const anchored_line_index = layout.content_starts[anchor_position];
  auto const anchored_line = layout.lines[anchored_line_index];
  auto old_max_scroll = layout.lines.size() > viewport_height ? layout.lines.size() - viewport_height : std::size_t{0};
  auto scroll_offset = old_max_scroll - std::min(old_max_scroll, anchored_line_index);
  auto anchor = ava::tui::detail::capture_transcript_viewport_anchor(layout, old_max_scroll, scroll_offset);

  std::vector<ava::tui::TranscriptItem> turn{ava::tui::TranscriptItem{.label = "ava", .text = "first live event"}};
  std::vector<ava::tui::TranscriptItem> capped;
  std::size_t prior_evictions = 0;
  auto first = ava::tui::apply_capped_transcript_snapshot(capped, submitted, turn, prior_evictions);
  prior_evictions = first.leading_evictions;
  auto next = ava::tui::detail::render_transcript_layout(capped, width, true, false, true);
  auto next_max_scroll = next.lines.size() > viewport_height ? next.lines.size() - viewport_height : std::size_t{0};
  scroll_offset = ava::tui::detail::restore_transcript_viewport_anchor(anchor, next, next_max_scroll, first.item_index_shift);
  auto visible_start = next_max_scroll - std::min(scroll_offset, next_max_scroll);
  expect(first.leading_evictions == 1 && first.item_index_shift == -1 && visible_start < next.lines.size() && next.lines[visible_start] == anchored_line &&
             next.lines.front().find("context gathering") != std::string::npos,
         "first production-cap eviction keeps the same context-tool content row when the retained tool gains a synthetic group heading: evictions=" +
             std::to_string(first.leading_evictions) + " shift=" + std::to_string(first.item_index_shift) + " visible=" + std::to_string(visible_start) +
             " anchored='" + anchored_line + "' actual='" + (visible_start < next.lines.size() ? next.lines[visible_start] : "missing") + "' first='" +
             (next.lines.empty() ? "missing" : next.lines.front()) + "'");

  auto const synthetic_anchor = ava::tui::detail::capture_transcript_viewport_anchor(next, next_max_scroll, next_max_scroll);
  auto without_group = capped;
  without_group[1].tool->name = "write";
  auto const without_group_layout = ava::tui::detail::render_transcript_layout(without_group, width, true, false, true);
  auto const without_group_max_scroll =
      without_group_layout.lines.size() > viewport_height ? without_group_layout.lines.size() - viewport_height : std::size_t{0};
  auto const synthetic_restore = ava::tui::detail::restore_transcript_viewport_anchor(synthetic_anchor, without_group_layout, without_group_max_scroll, 0);
  expect(synthetic_anchor.valid && !synthetic_anchor.content_relative && synthetic_restore == without_group_max_scroll,
         "an anchored synthetic context heading that disappears falls back deterministically to the oldest retained row");

  anchor = ava::tui::detail::capture_transcript_viewport_anchor(next, next_max_scroll, scroll_offset);
  turn.front().text += " with a taller updated body that changes wrapping";
  auto subsequent = ava::tui::apply_capped_transcript_snapshot(capped, submitted, turn, prior_evictions);
  prior_evictions = subsequent.leading_evictions;
  next = ava::tui::detail::render_transcript_layout(capped, width, true, false, true);
  next_max_scroll = next.lines.size() > viewport_height ? next.lines.size() - viewport_height : std::size_t{0};
  scroll_offset = ava::tui::detail::restore_transcript_viewport_anchor(anchor, next, next_max_scroll, subsequent.item_index_shift);
  visible_start = next_max_scroll - std::min(scroll_offset, next_max_scroll);
  expect(
      subsequent.leading_evictions == 1 && subsequent.item_index_shift == 0 && visible_start < next.lines.size() && next.lines[visible_start] == anchored_line,
      "subsequent same-cap event snapshot applies zero additional index shift while preserving the content anchor");

  anchor = ava::tui::detail::capture_transcript_viewport_anchor(next, next_max_scroll, scroll_offset);
  for (std::size_t event = 1; event <= 4; ++event)
  {
    turn.push_back(ava::tui::TranscriptItem{.label = "ava", .text = "additional live item " + std::to_string(event)});
    auto update = ava::tui::apply_capped_transcript_snapshot(capped, submitted, turn, prior_evictions);
    prior_evictions = update.leading_evictions;
    next = ava::tui::detail::render_transcript_layout(capped, width, true, false, true);
    next_max_scroll = next.lines.size() > viewport_height ? next.lines.size() - viewport_height : std::size_t{0};
    scroll_offset = ava::tui::detail::restore_transcript_viewport_anchor(anchor, next, next_max_scroll, update.item_index_shift);
    visible_start = next_max_scroll - std::min(scroll_offset, next_max_scroll);
    expect(update.item_index_shift == -1 && visible_start == 0,
           "repeated production-cap leading evictions use runtime shift arithmetic and retain oldest-row fallback after anchor eviction");
  }
}

void test_tui_detached_append_defers_layout_until_anchor_recovery()
{
  constexpr auto width = std::size_t{48};
  constexpr auto viewport_height = std::size_t{5};
  std::vector<ava::tui::TranscriptItem> submitted;
  submitted.reserve(ava::tui::kMaxTranscriptItems);
  for (std::size_t index = 0; index < ava::tui::kMaxTranscriptItems; ++index)
    submitted.push_back(ava::tui::TranscriptItem{.label = "ava", .text = "stable detached row " + std::to_string(index)});

  ava::tui::detail::TranscriptLayoutCache cache;
  ava::tui::detail::refresh_transcript_layout_cache(cache, submitted, 1, width, false, true, false);
  auto const initial_builds = cache.layout_build_count;
  auto const anchor_item_index = std::size_t{12};
  auto const anchor_position = std::ranges::find(cache.layout.message_item_indices, anchor_item_index);
  expect(anchor_position != cache.layout.message_item_indices.end(), "detached deferred-layout fixture exposes the selected transcript anchor");
  if (anchor_position == cache.layout.message_item_indices.end())
    return;
  auto const content_position = static_cast<std::size_t>(std::distance(cache.layout.message_item_indices.begin(), anchor_position));
  auto const anchor_line_index = cache.layout.content_starts[content_position];
  auto const anchor_line = cache.layout.lines[anchor_line_index];
  auto const old_max_scroll = ava::tui::detail::cached_transcript_max_scroll_offset(cache, viewport_height);
  auto const old_scroll_offset = old_max_scroll - std::min(old_max_scroll, anchor_line_index);
  auto const anchor = ava::tui::detail::capture_transcript_viewport_anchor(cache.layout, old_max_scroll, old_scroll_offset);

  std::vector<ava::tui::TranscriptItem> turn;
  std::vector<ava::tui::TranscriptItem> projected = submitted;
  std::size_t prior_evictions = 0;
  std::ptrdiff_t cumulative_shift = 0;
  for (std::size_t update_index = 0; update_index < 5; ++update_index)
  {
    turn.push_back(ava::tui::TranscriptItem{.label = "ava", .text = "append below frozen viewport " + std::to_string(update_index)});
    auto update = ava::tui::apply_capped_transcript_snapshot(projected, submitted, turn, prior_evictions);
    prior_evictions = update.leading_evictions;
    cumulative_shift += update.item_index_shift;
    expect(cache.layout_build_count == initial_builds && cache.transcript_generation == 1,
           "detached append-only updates leave the frozen transcript layout untouched until navigation");
  }

  ava::tui::detail::refresh_transcript_layout_cache(cache, projected, 6, width, false, true, false);
  auto const new_max_scroll = ava::tui::detail::cached_transcript_max_scroll_offset(cache, viewport_height);
  auto const recovered_scroll = ava::tui::detail::restore_transcript_viewport_anchor(anchor, cache.layout, new_max_scroll, cumulative_shift);
  auto const recovered_start = new_max_scroll - std::min(new_max_scroll, recovered_scroll);
  expect(prior_evictions == 5 && cumulative_shift == -5 && cache.layout_build_count == initial_builds + 1 && recovered_start < cache.layout.lines.size() &&
             cache.layout.lines[recovered_start] == anchor_line,
         "detached navigation performs one deferred rebuild and exactly recovers its anchor after repeated leading cap evictions");
}

void test_tui_streaming_tail_cache_is_bounded_and_matches_full_renderer()
{
  auto run_case = [](std::string name, std::string source, std::string append, std::size_t width, std::size_t height, bool compact_spacing) {
    std::vector<ava::tui::TranscriptItem> transcript{
        ava::tui::TranscriptItem{.label = "you", .text = "grouping prefix for " + name},
        ava::tui::TranscriptItem{.tool = ava::tui::ToolTimelineItem{.status = ava::tui::ToolTimelineStatus::Success,
                                                                    .name = "read_file",
                                                                    .argument_summary = "path=src/cache.cpp",
                                                                    .result_summary = "read 12 lines",
                                                                    .call_id = "stream_context",
                                                                    .lifecycle = ava::tui::ToolLifecycleState::Complete}},
        ava::tui::TranscriptItem{
            .label = "ava", .text = std::move(source), .meta = "Build · GPT-5.5", .stream_id = "stream_" + name, .append_only_stream = true}};
    ava::tui::detail::TranscriptTailRenderCache cache;
    auto assert_parity = [&](std::size_t generation) {
      auto const cached = ava::tui::detail::render_transcript_tail_lines_cached(cache, transcript, generation, width, height, true, true, compact_spacing);
      auto const full = ava::tui::detail::render_transcript_lines(transcript, width, true, true, compact_spacing);
      auto const suffix_start = full.size() > height ? full.size() - height : std::size_t{0};
      auto const expected = std::vector<std::string>(full.begin() + static_cast<std::ptrdiff_t>(suffix_start), full.end());
      expect(cached == expected, "streaming tail cache preserves full-render parity for " + name);
    };

    assert_parity(1);
    auto const full_work = cache.full_source_bytes;
    for (std::size_t update = 0; update < 4; ++update)
    {
      transcript.back().text += append;
      assert_parity(update + 2);
    }
    auto const incremental_work = cache.incremental_source_bytes;
    expect(cache.incremental_updates == 4 && cache.full_source_bytes == full_work && cache.source_budget_bytes > 0 &&
               cache.retained_source_bytes <= cache.source_budget_bytes && incremental_work <= cache.source_budget_bytes * 4,
           "streaming tail cache bounds unchanged-prefix update work and retained allocation by viewport plus carry for " + name);
  };

  std::string markdown;
  for (int index = 0; index < 800; ++index)
    markdown += index % 9 == 0 ? "## Cached heading\n" : "paragraph with **bold** and `inline code` content\n";
  std::string code = "```cpp\n";
  for (int index = 0; index < 900; ++index)
    code += "auto cached_value_" + std::to_string(index) + " = value + 1;\n";
  std::string bullets;
  for (int index = 0; index < 900; ++index)
    bullets += "- bounded bullet item " + std::to_string(index) + " with wrapping words\n";
  std::string numbered;
  for (int index = 0; index < 900; ++index)
    numbered += "1. normalized bounded numbered item with wrapping words\n";
  std::string blockquote = "> bounded quote begins\n";
  for (int index = 0; index < 900; ++index)
    blockquote += "lazy quoted continuation with wrapping words " + std::to_string(index) + "\n";
  std::string long_token(18000, 'x');

  run_case("markdown-roomy", std::move(markdown), "paragraph appended with **new markup**\n", 73, 11, false);
  run_case("open-code-compact", std::move(code), "auto appended_code = cached_value;\n", 64, 7, true);
  run_case("normalized-numbered", std::move(numbered), "1. newly appended normalized item\n", 58, 8, false);
  run_case("lazy-blockquote", std::move(blockquote), "new lazy quoted continuation\n", 67, 6, true);
  {
    ScopedEnvVar no_color_guard("NO_COLOR", "1");
    run_case("plain-bullets", std::move(bullets), "- newly appended bullet with enough words to wrap\n", 38, 4, false);
    run_case("plain-long-token", std::move(long_token), "yyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyy", 51, 9, true);
  }

  auto run_small_appends = [](std::string name, bool thinking, bool no_color) {
    std::optional<ScopedEnvVar> no_color_guard;
    if (no_color)
      no_color_guard.emplace("NO_COLOR", "1");
    std::vector<ava::tui::TranscriptItem> transcript{ava::tui::TranscriptItem{.label = "you", .text = "small append grouping prefix"},
                                                     ava::tui::TranscriptItem{.label = "ava",
                                                                              .text = thinking ? std::string{} : std::string("short start "),
                                                                              .meta = "Build · GPT-5.5",
                                                                              .thinking = thinking ? std::string("short thought ") : std::string{},
                                                                              .stream_id = "small_" + name,
                                                                              .append_only_stream = true}};
    ava::tui::detail::TranscriptTailRenderCache cache;
    auto assert_parity = [&](std::size_t generation) {
      auto const cached = ava::tui::detail::render_transcript_tail_lines_cached(cache, transcript, generation, 47, 7, true, true, false);
      auto const full = ava::tui::detail::render_transcript_tail_lines(transcript, 47, 7, true, true, false);
      expect(cached == full, "small append incremental cache preserves exact full-render parity for " + name);
    };
    assert_parity(1);
    auto const full_work = cache.full_source_bytes;
    std::size_t appended_bytes = 0;
    for (std::size_t update = 0; update < 180; ++update)
    {
      std::string chunk;
      switch (update % 12)
      {
        case 0:
          chunk = "wide 界 ";
          break;
        case 1:
          chunk = "\x1b[31mred\x1b[0m ";
          break;
        case 2:
          chunk = "long-logical-line-word ";
          break;
        case 3:
          chunk = "\n## Heading\n";
          break;
        case 4:
          chunk = "- bullet with **bold** text\n";
          break;
        case 5:
          chunk = "1. numbered `code` text\n";
          break;
        case 6:
          chunk = "> quoted row\n";
          break;
        case 7:
          chunk = "lazy quote continuation\n";
          break;
        case 8:
          chunk = "```cpp\n";
          break;
        case 9:
          chunk = "auto value = 1;\n";
          break;
        case 10:
          chunk = "```\n";
          break;
        default:
          chunk = "plain tail words ";
          break;
      }
      appended_bytes += chunk.size();
      if (thinking)
        transcript.back().thinking += chunk;
      else
        transcript.back().text += chunk;
      assert_parity(update + 2);
    }
    expect(cache.incremental_updates == 180 && cache.full_source_bytes == full_work && cache.incremental_source_bytes == appended_bytes &&
               cache.max_carry_source_bytes <= cache.source_budget_bytes && cache.carry_source_bytes <= cache.source_budget_bytes * cache.incremental_updates,
           "many short-to-medium appends process each new byte once plus bounded viewport/current-line carry for " + name);
  };
  run_small_appends("styled-assistant", false, false);
  run_small_appends("plain-thinking", true, true);

  auto tool = ava::tui::TranscriptItem{.tool = ava::tui::ToolTimelineItem{.status = ava::tui::ToolTimelineStatus::Running,
                                                                          .name = "streaming_tool",
                                                                          .argument_summary = std::string(90000, 'a'),
                                                                          .call_id = "tool_stream",
                                                                          .lifecycle = ava::tui::ToolLifecycleState::ArgumentsStreaming},
                                       .stream_id = "tool_stream",
                                       .append_only_stream = true};
  std::vector<ava::tui::TranscriptItem> tool_transcript{std::move(tool)};
  ava::tui::detail::TranscriptTailRenderCache tool_cache;
  auto const initial_tool_tail = ava::tui::detail::render_transcript_tail_lines_cached(tool_cache, tool_transcript, 1, 60, 5, false, true, false);
  auto const full_tool_work = tool_cache.full_source_bytes;
  bool tool_parity = initial_tool_tail == ava::tui::detail::render_transcript_tail_lines(tool_transcript, 60, 5, false, true, false);
  for (std::size_t update = 0; update < 4; ++update)
  {
    tool_transcript.back().tool->argument_summary += "append-only-tool-arguments";
    auto const cached = ava::tui::detail::render_transcript_tail_lines_cached(tool_cache, tool_transcript, update + 2, 60, 5, false, true, false);
    tool_parity = tool_parity && cached == ava::tui::detail::render_transcript_tail_lines(tool_transcript, 60, 5, false, true, false);
  }
  expect(tool_parity && tool_cache.incremental_updates == 4 && tool_cache.full_source_bytes == full_tool_work && tool_cache.incremental_source_bytes < 256,
         "collapsed append-only tool items reuse their stable fitted prefix with bounded update work and full-render parity");
}

void test_tui_transcript_tail_renderer_matches_full_visible_window()
{
  std::vector<ava::tui::TranscriptItem> transcript;
  for (int index = 0; index < 48; ++index)
  {
    transcript.push_back(ava::tui::TranscriptItem{
        .label = "you", .text = "tail renderer input " + std::to_string(index) + " with enough text to wrap over multiple terminal columns"});
    transcript.push_back(
        ava::tui::TranscriptItem{.label = "ava",
                                 .text = "tail renderer answer " + std::to_string(index) + " keeps viewport rows equivalent to the full renderer",
                                 .meta = "Build · GPT-5.5",
                                 .thinking = index % 3 == 0 ? "thinking " + std::to_string(index) : std::string{}});
    if (index % 4 == 0)
    {
      transcript.push_back(ava::tui::TranscriptItem{.tool = ava::tui::ToolTimelineItem{.status = ava::tui::ToolTimelineStatus::Success,
                                                                                       .name = "read_file",
                                                                                       .argument_summary = "path=src/main.cpp",
                                                                                       .result_summary = "read 12 lines",
                                                                                       .call_id = "tail_read_" + std::to_string(index),
                                                                                       .lifecycle = ava::tui::ToolLifecycleState::Complete}});
      transcript.push_back(ava::tui::TranscriptItem{.tool = ava::tui::ToolTimelineItem{.status = ava::tui::ToolTimelineStatus::Success,
                                                                                       .name = "grep",
                                                                                       .argument_summary = "pattern=tail",
                                                                                       .result_summary = "2 matches",
                                                                                       .call_id = "tail_grep_" + std::to_string(index),
                                                                                       .lifecycle = ava::tui::ToolLifecycleState::Complete}});
    }
  }

  constexpr auto width = std::size_t{92};
  constexpr auto height = std::size_t{13};
  for (auto const compact_spacing : {false, true})
  {
    auto const full = ava::tui::detail::render_transcript_lines(transcript, width, true, true, compact_spacing);
    auto const starts = ava::tui::detail::transcript_message_start_lines(transcript, width, true, true, compact_spacing);
    expect(starts.size() == transcript.size() && !starts.empty() && starts.front() == 0 && std::ranges::is_sorted(starts) &&
               std::ranges::all_of(starts, [&full](std::size_t start) { return start < full.size() && !strip_sgr(full[start]).empty(); }),
           std::string("tui transcript message starts share ") + (compact_spacing ? "compact" : "roomy") + " logical-group segmentation with full rendering");
    for (auto const budget : {std::size_t{1}, std::size_t{2}, height, height + 1, std::size_t{31}, std::size_t{100}})
    {
      auto const tail = ava::tui::detail::render_transcript_tail_lines(transcript, width, budget, true, true, compact_spacing);
      auto const suffix_start = full.size() > budget ? full.size() - budget : std::size_t{0};
      auto const expected = std::vector<std::string>(full.begin() + static_cast<std::ptrdiff_t>(suffix_start), full.end());
      expect(tail == expected, std::string("tui bounded transcript tail matches the ") + (compact_spacing ? "compact" : "roomy") +
                                   " full-render suffix at budget " + std::to_string(budget));
    }
    for (auto const offset : {std::size_t{0}, std::size_t{1}, std::size_t{7}, std::size_t{32}, std::size_t{96}})
    {
      auto const expected = ava::tui::detail::visible_transcript_lines(full, width, height, offset);
      auto const tail_budget = height + offset;
      auto const tail = ava::tui::detail::render_transcript_tail_lines(transcript, width, tail_budget, true, true, compact_spacing);
      auto const actual = ava::tui::detail::visible_transcript_lines(tail, width, height, offset);
      expect(actual == expected, std::string("tui bounded transcript tail renderer matches ") + (compact_spacing ? "compact" : "roomy") +
                                     " full-render visible rows at scroll offset " + std::to_string(offset));
    }
  }
}
void test_tui_completion_and_transcript_layout_caches()
{
  tui_test_support::TuiFrontEndFixture frontend;
  std::vector<ava::tui::FileReferenceItem> references;
  references.reserve(2000);
  for (std::size_t index = 0; index < 2000; ++index)
  {
    references.push_back(ava::tui::FileReferenceItem{.value = "src/generated/path-" + std::to_string(index) + (index % 17 == 0 ? "/" : ".cpp"),
                                                     .description = "synthetic workspace reference " + std::to_string(index),
                                                     .directory = index % 17 == 0,
                                                     .enabled = index != 1999,
                                                     .disabled_reason = index == 1999 ? "outside workspace" : std::string{}});
  }

  auto completion_snapshot = ava::tui::ComposerSnapshot{.mode = "build",
                                                        //.provider = "openai",
                                                        //.model = "gpt-5.5",
                                                        .session_id = "session_cache",
                                                        .input = "inspect @path-",
                                                        .status = "ready",
                                                        .transcript = {},
                                                        .file_references = references,
                                                        .selected_slash_command_index = 0,
                                                        .width = 96,
                                                        .height = 12,
                                                        .input_cursor = std::string("inspect @path-").size()};
  ava::tui::detail::CompletionMatchCache completion_cache;
  ava::tui::detail::refresh_completion_match_cache(completion_cache, completion_snapshot, 1);
  auto selected = std::size_t{0};
  auto const initial_builds = completion_cache.ranking_build_count;
  auto const initial_formats = completion_cache.formatted_candidate_count;
  for (std::size_t pass = 0; pass < 100; ++pass)
  {
    selected = ava::tui::detail::next_completion_selection(completion_cache, selected);
    completion_snapshot.selected_slash_command_index = selected;
    ava::tui::detail::refresh_completion_match_cache(completion_cache, completion_snapshot, 1);
    auto const reason = ava::tui::detail::completion_selection_disabled_reason(completion_cache, completion_snapshot.file_references, selected);
    auto const selection = ava::tui::detail::completion_selection_text(completion_cache, completion_snapshot, selected);
    auto const frame = ava::tui::detail::render_composer_frame_cached(frontend.frontend(), completion_snapshot, completion_cache, 1, nullptr, 0);
    expect(!selection.text.empty() && !reason && frame.lines.size() == completion_snapshot.height,
           "completion cache keeps enabled acceptance semantics while selection and rendering share one ranking");
  }
  expect(completion_cache.ranking_build_count == initial_builds && completion_cache.formatted_candidate_count - initial_formats <= 800,
         "2,000-reference completion navigation ranks once and formats at most the visible eight rows per frame");

  ++completion_snapshot.input_cursor;
  ava::tui::detail::refresh_completion_match_cache(completion_cache, completion_snapshot, 1);
  auto expected_builds = initial_builds + 1;
  expect(completion_cache.ranking_build_count == expected_builds, "completion cursor mutation rebuilds ranking exactly once");
  completion_snapshot.input += "1";
  completion_snapshot.input_cursor = completion_snapshot.input.size();
  ava::tui::detail::refresh_completion_match_cache(completion_cache, completion_snapshot, 1);
  expect(completion_cache.ranking_build_count == ++expected_builds, "completion input mutation rebuilds ranking exactly once");
  completion_snapshot.path_completion_force_active = true;
  ava::tui::detail::refresh_completion_match_cache(completion_cache, completion_snapshot, 1);
  expect(completion_cache.ranking_build_count == ++expected_builds, "completion force mutation rebuilds ranking exactly once");
  completion_snapshot.input = "inspect path-1999";
  completion_snapshot.input_cursor = completion_snapshot.input.size();
  ava::tui::detail::refresh_completion_match_cache(completion_cache, completion_snapshot, 1);
  expect(completion_cache.ranking_build_count == ++expected_builds, "completion active-surface mutation rebuilds ranking exactly once");
  references.back().description = "source revision changed";
  completion_snapshot.file_references = references;
  ava::tui::detail::refresh_completion_match_cache(completion_cache, completion_snapshot, 2);
  auto const disabled_reason = ava::tui::detail::completion_selection_disabled_reason(completion_cache, completion_snapshot.file_references, 0);
  expect(completion_cache.ranking_build_count == ++expected_builds && disabled_reason == "outside workspace",
         "completion source revision rebuilds once and preserves disabled acceptance status");

  std::vector<ava::tui::TranscriptItem> transcript;
  transcript.reserve(1000);
  for (std::size_t index = 0; index < 1000; ++index)
  {
    if (index % 5 == 0)
    {
      transcript.push_back(ava::tui::TranscriptItem{.tool = ava::tui::ToolTimelineItem{.status = ava::tui::ToolTimelineStatus::Success,
                                                                                       .name = "read",
                                                                                       .argument_summary = "path=src/generated/" + std::to_string(index),
                                                                                       .result_summary = "lines=8",
                                                                                       .call_id = "cache_" + std::to_string(index),
                                                                                       .lifecycle = ava::tui::ToolLifecycleState::Complete}});
    }
    else
    {
      transcript.push_back(ava::tui::TranscriptItem{.label = index % 2 == 0 ? "you" : "ava",
                                                    .text = "cached transcript item " + std::to_string(index) + " with **markdown** and wrapping text",
                                                    .thinking = index % 11 == 0 ? "bounded thinking " + std::to_string(index) : std::string{}});
    }
  }

  ava::tui::detail::TranscriptLayoutCache transcript_cache;
  ava::tui::detail::refresh_transcript_layout_cache(transcript_cache, transcript, 1, 80, false, true, false);
  auto const first_layout_builds = transcript_cache.layout_build_count;
  auto const legacy_lines = ava::tui::detail::render_transcript_lines(transcript, 80, false, true, false);
  for (std::size_t pass = 0; pass < 100; ++pass)
  {
    ava::tui::detail::refresh_transcript_layout_cache(transcript_cache, transcript, 1, 80, false, true, false);
    auto const height = std::size_t{19};
    auto const max_offset = ava::tui::detail::cached_transcript_max_scroll_offset(transcript_cache, height);
    auto const offset = std::min(max_offset, pass * 97);
    auto const cached = ava::tui::detail::cached_visible_transcript_lines(transcript_cache, height, offset);
    auto const legacy = ava::tui::detail::visible_transcript_lines(legacy_lines, 80, height, offset);
    expect(cached == legacy, "detached transcript cache viewport exactly matches the full renderer");
  }
  expect(transcript_cache.layout_build_count == first_layout_builds && transcript_cache.visible_slice_count == 100,
         "1,000-item transcript detach builds once while 100 scroll moves only slice the cached layout");
  auto const oldest_offset = ava::tui::detail::cached_transcript_max_scroll_offset(transcript_cache, 19);
  expect(ava::tui::detail::cached_visible_transcript_lines(transcript_cache, 19, oldest_offset) ==
             ava::tui::detail::visible_transcript_lines(legacy_lines, 80, 19, oldest_offset),
         "detached transcript scrolling reaches the oldest viewport with full-renderer parity");

  transcript.push_back(ava::tui::TranscriptItem{.label = "ava", .text = "new detached output"});
  ava::tui::detail::refresh_transcript_layout_cache(transcript_cache, transcript, 2, 80, false, true, false);
  auto expected_layout_builds = first_layout_builds + 1;
  expect(transcript_cache.layout_build_count == expected_layout_builds, "transcript generation mutation rebuilds detached layout exactly once");
  ava::tui::detail::refresh_transcript_layout_cache(transcript_cache, transcript, 2, 79, false, true, false);
  expect(transcript_cache.layout_build_count == ++expected_layout_builds, "transcript width mutation rebuilds detached layout exactly once");
  ava::tui::detail::refresh_transcript_layout_cache(transcript_cache, transcript, 2, 79, true, true, false);
  expect(transcript_cache.layout_build_count == ++expected_layout_builds, "transcript presentation mutation rebuilds detached layout exactly once");
  ava::tui::detail::refresh_transcript_layout_cache(transcript_cache, transcript, 2, 79, ava::tui::ToolPresentation::Rich, true, false);
  expect(transcript_cache.layout_build_count == ++expected_layout_builds, "transcript cache keys distinguish Rich from both Compact and Expanded presentation");
  ava::tui::detail::refresh_transcript_layout_cache(transcript_cache, transcript, 2, 79, ava::tui::ToolPresentation::Rich, false, false);
  expect(transcript_cache.layout_build_count == ++expected_layout_builds, "transcript thinking mutation rebuilds detached layout exactly once");
}

void test_tui_very_long_transcript_performance_budget()
{
  tui_test_support::TuiFrontEndFixture frontend;
  std::vector<ava::tui::TranscriptItem> transcript;
  transcript.reserve(1400);
  for (int index = 0; index < 600; ++index)
  {
    transcript.push_back(ava::tui::TranscriptItem{
        .label = "you",
        .text = "long transcript input " + std::to_string(index) + " asks for a bounded terminal redraw with CJK \xE7\x95\x8C and a long-token-that-wraps"});
    transcript.push_back(ava::tui::TranscriptItem{
        .label = "ava",
        .text = "long transcript answer " + std::to_string(index) + " renders markdown like **bold**, `code`, and - bullet-shaped text without overflowing",
        .meta = "Build · GPT-5.5",
        .thinking = index % 4 == 0 ? "checked viewport stress path " + std::to_string(index) : std::string{}});
    if (index % 5 == 0)
    {
      transcript.push_back(ava::tui::TranscriptItem{.tool = ava::tui::ToolTimelineItem{.status = ava::tui::ToolTimelineStatus::Success,
                                                                                       .name = "bash",
                                                                                       .argument_summary = "command=ctest --test-dir build",
                                                                                       .result_summary = "exit=0 lines=42 hidden=120",
                                                                                       .call_id = "long_perf_" + std::to_string(index),
                                                                                       .lifecycle = ava::tui::ToolLifecycleState::Complete,
                                                                                       .truncated = true,
                                                                                       .output_lines = 42,
                                                                                       .total_lines = 162,
                                                                                       .omitted_lines = 120}});
    }
  }
  expect(transcript.size() > 900, "tui very long transcript stress extends beyond the existing large-render scale");

  auto const start = std::chrono::steady_clock::now();
  std::vector<std::string> frame;
  std::vector<std::string> visible_frames;
  for (int pass = 0; pass < 3; ++pass)
  {
    frame = ava::tui::render_composer(frontend.frontend(), ava::tui::ComposerSnapshot{.mode = "build",
                                                                                      //.provider = "openai",
                                                                                      //.model = "gpt-5.5",
                                                                                      .session_id = "session_long_perf",
                                                                                      .input = "draft stays responsive while scrolling a very long transcript",
                                                                                      .status = "ready",
                                                                                      .processing = true,
                                                                                      .transcript = transcript,
                                                                                      .transcript_scroll_offset = static_cast<std::size_t>(pass * 240),
                                                                                      .width = 96,
                                                                                      .height = 30,
                                                                                      .input_cursor = std::string::npos,
                                                                                      .tool_presentation = ava::tui::ToolPresentation::Expanded,
                                                                                      .thinking_visible = true});
    visible_frames.push_back(tui_test_support::join_visible_lines(frame));
  }
  auto const elapsed = std::chrono::steady_clock::now() - start;
  expect(frame.size() == 30, "tui very long transcript frame keeps the requested height");
  expect(std::ranges::all_of(frame, [](std::string const& line) { return line.find('\n') == std::string::npos && visible_columns(line) <= 96; }),
         "tui very long transcript frame keeps every rendered line inside the requested width");
  expect(visible_frames.size() == 3 && visible_frames[0] != visible_frames[1] && visible_frames[1] != visible_frames[2],
         "tui very long transcript stress validates that scroll offsets change visible transcript content");
  expect(elapsed < std::chrono::seconds(20), "tui very long transcript performance budget keeps full redraw viable for real-world scrollback scale");
}
void test_tui_osc52_clipboard_sequence_bounds()
{
  using ava::tui::runtime_transcript::base64_encode;
  using ava::tui::runtime_transcript::kMaxTerminalClipboardTextBytes;
  using ava::tui::runtime_transcript::try_build_osc52_clipboard_sequence;
  using ava::tui::runtime_transcript::try_build_osc52_clipboard_transport;

  constexpr std::string_view kPrefix = "\x1b]52;c;";
  constexpr std::string_view kSuffix = "\x1b\\";

  auto const assert_valid_sequence = [&](std::string_view raw, std::string const& sequence, std::string_view label) {
    expect(sequence.starts_with(kPrefix) && sequence.ends_with(kSuffix),
           std::string("OSC 52 sequence keeps exact plain prefix and ST terminator for ") + std::string(label));
    auto const payload = sequence.substr(kPrefix.size(), sequence.size() - kPrefix.size() - kSuffix.size());
    expect(payload == base64_encode(raw), std::string("OSC 52 payload is the base64 encoding of source for ") + std::string(label));
    expect(payload.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/=") == std::string::npos,
           std::string("OSC 52 payload stays within the base64 alphabet for ") + std::string(label));
    // Source controls must not appear raw inside the base64 body (only in the fixed OSC framing).
    expect(payload.find('\x1b') == std::string::npos && payload.find('\0') == std::string::npos && payload.find('\a') == std::string::npos,
           std::string("OSC 52 payload contains no raw source control bytes for ") + std::string(label));
  };

  expect(kMaxTerminalClipboardTextBytes == 65'536, "terminal clipboard ceiling is the documented 64 KiB bound");
  expect(!try_build_osc52_clipboard_sequence("").has_value(), "empty clipboard text builds no OSC 52 sequence");

  auto const one_byte = try_build_osc52_clipboard_sequence("a");
  expect(one_byte.has_value() && *one_byte == std::string(kPrefix) + "YQ==" + std::string(kSuffix),
         "one-byte clipboard text builds the exact OSC 52 ST sequence");
  assert_valid_sequence("a", *one_byte, "one-byte");

  std::string const raw_one_byte = "\x1b]52;c;YQ==\x1b\\";
  std::string const wrapped_one_byte = "\x1bPtmux;\x1b\x1b]52;c;YQ==\x1b\x1b\\\x1b\\";
  auto const direct_missing_tmux = try_build_osc52_clipboard_transport("a", std::nullopt);
  auto const direct_empty_tmux = try_build_osc52_clipboard_transport("a", std::string_view{});
  expect(direct_missing_tmux == raw_one_byte && direct_empty_tmux == raw_one_byte, "missing or empty TMUX routes the exact existing raw OSC 52 bytes directly");

  auto const tmux_transport = try_build_osc52_clipboard_transport("a", "hostile;value\x1b-not-output");
  expect(tmux_transport == raw_one_byte + wrapped_one_byte,
         "nonempty TMUX routes the raw OSC 52 copy followed by one exact tmux passthrough with every inner ESC doubled");
  auto const first_wrapper = tmux_transport ? tmux_transport->find("\x1bPtmux;") : std::string::npos;
  expect(first_wrapper != std::string::npos && tmux_transport->find("\x1bPtmux;", first_wrapper + 1) == std::string::npos &&
             tmux_transport->find("hostile") == std::string::npos,
         "OSC 52 tmux routing adds one wrapper layer without interpolating TMUX content or recursively wrapping");

  constexpr std::string_view kUnicode = "界π";
  auto const unicode = try_build_osc52_clipboard_sequence(kUnicode);
  expect(unicode.has_value(), "Unicode clipboard text builds an OSC 52 sequence");
  assert_valid_sequence(kUnicode, *unicode, "unicode");

  std::string const controls("pre\x1b\0\amid", 8);
  auto const control_seq = try_build_osc52_clipboard_sequence(controls);
  expect(control_seq.has_value(), "embedded ESC/NUL/BEL clipboard text builds an OSC 52 sequence");
  assert_valid_sequence(controls, *control_seq, "embedded-controls");
  // Full sequence framing uses ESC only at the known OSC/ST edges, never from the source body.
  expect(control_seq->find('\0') == std::string::npos && control_seq->find('\a') == std::string::npos,
         "accepted OSC 52 sequence never carries raw NUL or BEL from source text");

  std::string const exact(kMaxTerminalClipboardTextBytes, 'x');
  auto const exact_seq = try_build_osc52_clipboard_sequence(exact);
  expect(exact_seq.has_value(), "exactly 64 KiB clipboard text is accepted");
  assert_valid_sequence(exact, *exact_seq, "exact-64KiB");
  expect(try_build_osc52_clipboard_transport(exact, "tmux").has_value(), "exactly 65,536 source bytes are accepted by tmux OSC 52 routing");

  std::string const oversized(kMaxTerminalClipboardTextBytes + 1, 'y');
  expect(!try_build_osc52_clipboard_sequence(oversized).has_value(), "65,537-byte clipboard text is rejected without building a sequence");
  expect(!try_build_osc52_clipboard_transport(oversized, "tmux").has_value(), "65,537 source bytes are rejected by tmux OSC 52 routing without truncation");
}

std::string long_thinking_body(std::size_t lines)
{
  std::string body;
  for (std::size_t index = 1; index <= lines; ++index)
  {
    body += "thinking-row-" + std::to_string(index);
    if (index < lines)
      body.push_back('\n');
  }
  return body;
}

void test_tui_bounded_thinking_disclosure_render_and_toggle()
{
  tui_test_support::TuiFrontEndFixture frontend;
  using tui_test_support::plain_lines;
  auto const long_body = long_thinking_body(20);
  auto const short_body = long_thinking_body(3);

  auto long_item = ava::tui::TranscriptItem{.label = "ava", .text = "assistant answer", .meta = "Build · GPT-5.5", .thinking = long_body};
  auto short_item = ava::tui::TranscriptItem{.label = "ava", .text = "short answer", .thinking = short_body};
  auto legacy_item = ava::tui::TranscriptItem{.label = "thinking", .text = long_body};
  auto live_item = ava::tui::TranscriptItem{.label = "ava", .thinking = long_body, .stream_id = "stream-1", .append_only_stream = true};

  auto const width = std::size_t{80};
  // The thinking block alone is capped at 12 rows; assistant text/meta follow.
  auto thinking_only = ava::tui::TranscriptItem{.label = "thinking", .text = long_body};
  auto const bounded = ava::tui::detail::render_transcript_layout({thinking_only}, width, ava::tui::ToolPresentation::Rich, true, false);
  auto const bounded_plain = plain_lines(bounded.lines);
  auto const hidden_footer = std::ranges::find_if(bounded_plain, [](std::string const& line) { return line.find("lines hidden") != std::string::npos; });
  expect(bounded.lines.size() == ava::tui::detail::kThinkingBoundedMaxRows && hidden_footer != bounded_plain.end(),
         "completed long thinking defaults to exactly 12 rendered rows including the hidden-count footer");
  auto const footer_plain = *hidden_footer;
  auto const ellipsis = footer_plain.find("… ");
  expect(ellipsis != std::string::npos, "bounded thinking footer uses the … N lines hidden marker");
  auto const hidden_n = std::stoul(footer_plain.substr(ellipsis + 4));
  // Full unexpanded count via expand.
  thinking_only.thinking_expanded = true;
  auto const expanded = ava::tui::detail::render_transcript_layout({thinking_only}, width, ava::tui::ToolPresentation::Rich, true, false);
  expect(expanded.lines.size() == hidden_n + ava::tui::detail::kThinkingBoundedContentRows &&
             std::ranges::none_of(plain_lines(expanded.lines), [](std::string const& line) { return line.find("lines hidden") != std::string::npos; }),
         "explicit expand shows the full thinking line vector and restores no footer; N matches full-minus-11");

  thinking_only.thinking_expanded = false;
  auto const collapsed_again = ava::tui::detail::render_transcript_layout({thinking_only}, width, ava::tui::ToolPresentation::Rich, true, false);
  expect(collapsed_again.lines.size() == ava::tui::detail::kThinkingBoundedMaxRows, "collapse restores the bounded twelve-row preview");

  auto const short_layout = ava::tui::detail::render_transcript_layout({short_item}, width, ava::tui::ToolPresentation::Rich, true, false);
  expect(std::ranges::none_of(plain_lines(short_layout.lines), [](std::string const& line) { return line.find("lines hidden") != std::string::npos; }) &&
             ava::tui::transcript_item_thinking_is_boundable(short_item, width, true) == false,
         "short thinking renders fully with no chrome and is not boundable");

  auto const live_layout = ava::tui::detail::render_transcript_layout({live_item}, width, ava::tui::ToolPresentation::Rich, true, false);
  expect(live_layout.lines.size() > ava::tui::detail::kThinkingBoundedMaxRows &&
             std::ranges::none_of(plain_lines(live_layout.lines), [](std::string const& line) { return line.find("lines hidden") != std::string::npos; }) &&
             !ava::tui::transcript_item_thinking_is_boundable(live_item, width, true),
         "live append-only pending reasoning stays fully expanded and is never auto-collapsed");

  auto const legacy_layout = ava::tui::detail::render_transcript_layout({legacy_item}, width, ava::tui::ToolPresentation::Rich, true, false);
  expect(legacy_layout.lines.size() == ava::tui::detail::kThinkingBoundedMaxRows, "legacy label=thinking long blocks share the same bounded disclosure");

  auto const ava_layout = ava::tui::detail::render_transcript_layout({long_item}, width, ava::tui::ToolPresentation::Rich, true, false);
  auto const ava_plain = plain_lines(ava_layout.lines);
  expect(std::ranges::any_of(ava_plain, [](std::string const& line) { return line.find("assistant answer") != std::string::npos; }) &&
             std::ranges::any_of(ava_plain, [](std::string const& line) { return line.find("Build · GPT-5.5") != std::string::npos; }) &&
             std::ranges::any_of(ava_plain, [](std::string const& line) { return line.find("lines hidden") != std::string::npos; }),
         "assistant text and final-only meta remain after bounded thinking");

  long_item.thinking_expanded = false;
  auto hidden_global = ava::tui::detail::render_transcript_layout({long_item}, width, ava::tui::ToolPresentation::Rich, false, false);
  expect(hidden_global.lines.size() > 0 &&
             std::ranges::none_of(plain_lines(hidden_global.lines), [](std::string const& line) { return line.find("Thinking:") != std::string::npos; }) &&
             std::ranges::any_of(plain_lines(hidden_global.lines), [](std::string const& line) { return line.find("assistant answer") != std::string::npos; }),
         "global thinking_visible=false hides thinking while assistant text remains");

  // Width/resize recalculation: wider width may wrap less; N is always full-at-width minus 11.
  auto narrow = ava::tui::TranscriptItem{.label = "thinking", .text = long_thinking_body(15)};
  auto const narrow_layout = ava::tui::detail::render_transcript_layout({narrow}, 40, ava::tui::ToolPresentation::Rich, true, false);
  narrow.thinking_expanded = true;
  auto const narrow_full = ava::tui::detail::render_transcript_layout({narrow}, 40, ava::tui::ToolPresentation::Rich, true, false);
  narrow.thinking_expanded = false;
  auto const narrow_plain = plain_lines(narrow_layout.lines);
  auto const narrow_footer = std::ranges::find_if(narrow_plain, [](std::string const& line) { return line.find("lines hidden") != std::string::npos; });
  expect(narrow_layout.lines.size() == ava::tui::detail::kThinkingBoundedMaxRows && narrow_footer != narrow_plain.end() &&
             narrow_full.lines.size() > ava::tui::detail::kThinkingBoundedMaxRows,
         "resize/width recalculates the bounded thinking footer from the current rendered line vector");

  // NO_COLOR / plain: composer frame strips SGR while keeping the truthful footer text.
  {
    ScopedEnvVar no_color("NO_COLOR", "1");
    auto plain_snapshot = ava::tui::ComposerSnapshot{
        .mode = "build",
        //.provider = "openai",
        //.model = "gpt-5.5",
        .session_id = "session_thinking_plain",
        .input = "",
        .status = "ready",
        .transcript = {ava::tui::TranscriptItem{.label = "thinking", .text = long_body}},
        .width = 80,
        .height = 24,
        .thinking_visible = true,
    };
    auto const plain_frame = ava::tui::render_composer(frontend.frontend(), plain_snapshot);
    auto const has_footer = std::ranges::any_of(plain_frame, [](std::string const& line) { return line.find("lines hidden") != std::string::npos; });
    auto const has_sgr = std::ranges::any_of(plain_frame, [](std::string const& line) { return line.find("\x1b[") != std::string::npos; });
    expect(has_footer && !has_sgr, "NO_COLOR keeps a plain truthful thinking hidden footer without SGR styling");
  }

  // Tools remain unchanged by thinking disclosure.
  auto tool_item = ava::tui::TranscriptItem{.tool = ava::tui::ToolTimelineItem{.status = ava::tui::ToolTimelineStatus::Success,
                                                                               .name = "read_file",
                                                                               .result_summary = "ok",
                                                                               .call_id = "tool-1",
                                                                               .lifecycle = ava::tui::ToolLifecycleState::Complete}};
  auto const with_tool = ava::tui::detail::render_transcript_layout({thinking_only, tool_item}, width, ava::tui::ToolPresentation::Rich, true, false);
  expect(std::ranges::any_of(plain_lines(with_tool.lines), [](std::string const& line) { return line.find("read_file") != std::string::npos; }),
         "tool cards remain rendered unchanged alongside bounded thinking");

  // Duplicate identical blocks expand independently.
  auto first = ava::tui::TranscriptItem{.label = "thinking", .text = long_body};
  auto second = ava::tui::TranscriptItem{.label = "thinking", .text = long_body};
  first.thinking_expanded = true;
  auto dup = std::vector<ava::tui::TranscriptItem>{first, second};
  auto const dup_layout = ava::tui::detail::render_transcript_layout(dup, width, ava::tui::ToolPresentation::Rich, true, false);
  expect(dup_layout.lines.size() == expanded.lines.size() + ava::tui::detail::kThinkingBoundedMaxRows,
         "duplicate identical thinking blocks keep independent expansion state");
  expect(ava::tui::toggle_thinking_expansion_at(dup, 1, width, true) && dup[1].thinking_expanded && dup[0].thinking_expanded,
         "toggle on the second duplicate does not clear the first block's expansion");

  // Carry across active-run rebuild with shift-zero and positive-cap eviction.
  {
    std::vector<ava::tui::TranscriptItem> submitted;
    for (std::size_t index = 0; index < 5; ++index)
      submitted.push_back(ava::tui::TranscriptItem{.label = "you", .text = "seed " + std::to_string(index)});
    std::vector<ava::tui::TranscriptItem> turn{
        ava::tui::TranscriptItem{.label = "thinking", .text = long_body, .thinking_expanded = true},
        ava::tui::TranscriptItem{.label = "ava", .text = "after reasoning"},
        ava::tui::TranscriptItem{.tool = ava::tui::ToolTimelineItem{.status = ava::tui::ToolTimelineStatus::Success,
                                                                    .name = "bash",
                                                                    .result_summary = "done",
                                                                    .call_id = "bash-1",
                                                                    .lifecycle = ava::tui::ToolLifecycleState::Complete}},
    };
    // Simulate UI snapshot after user expanded the thinking item at index 5.
    std::vector<ava::tui::TranscriptItem> ui = submitted;
    ui.insert(ui.end(), turn.begin(), turn.end());
    ui[5].thinking_expanded = true;
    auto overrides = ava::tui::runtime_transcript::capture_thinking_expansion(ui);
    std::vector<ava::tui::TranscriptItem> rebuilt;
    // Fresh turn projection without expansion flags, then tools/text updates after completed reasoning.
    std::vector<ava::tui::TranscriptItem> fresh_turn{
        ava::tui::TranscriptItem{.label = "thinking", .text = long_body},
        ava::tui::TranscriptItem{.label = "ava", .text = "after reasoning"},
        ava::tui::TranscriptItem{.tool = ava::tui::ToolTimelineItem{.status = ava::tui::ToolTimelineStatus::Success,
                                                                    .name = "bash",
                                                                    .result_summary = "done",
                                                                    .call_id = "bash-1",
                                                                    .lifecycle = ava::tui::ToolLifecycleState::Complete}},
        ava::tui::TranscriptItem{.label = "ava", .text = "final text"},
    };
    auto const zero_shift = ava::tui::apply_capped_transcript_snapshot(rebuilt, submitted, fresh_turn, 0);
    ava::tui::runtime_transcript::carry_thinking_expansion(overrides, rebuilt, zero_shift.item_index_shift);
    expect(zero_shift.item_index_shift == 0 && zero_shift.leading_evictions == 0 && rebuilt[5].thinking_expanded && !rebuilt[6].thinking_expanded &&
               rebuilt[5].label == "thinking",
           "shift-zero active-run rebuild carries expansion by exact source index through completed-reasoning + tool/text updates");

    // Positive cap eviction: fill past kMaxTranscriptItems so leading rows drop.
    submitted.clear();
    submitted.reserve(ava::tui::kMaxTranscriptItems);
    for (std::size_t index = 0; index < ava::tui::kMaxTranscriptItems - 2; ++index)
      submitted.push_back(ava::tui::TranscriptItem{.label = "you", .text = "pad " + std::to_string(index)});
    // UI currently shows submitted + one expanded thinking + one plain thinking (identical content).
    ui = submitted;
    ui.push_back(ava::tui::TranscriptItem{.label = "thinking", .text = long_body, .thinking_expanded = true});
    ui.push_back(ava::tui::TranscriptItem{.label = "thinking", .text = long_body});
    auto const expanded_index = ui.size() - 2;
    overrides = ava::tui::runtime_transcript::capture_thinking_expansion(ui);
    expect(overrides.size() == 1 && overrides.front().first == expanded_index, "capture records only expanded thinking indices");

    // Rebuild with two extra leading pads in submitted so three leading items are evicted relative to a prior 0-eviction baseline?
    // previous_leading_evictions=0, destination size = (kMax-2) + 3 turn items = kMax+1 => 1 eviction, shift=-1.
    std::vector<ava::tui::TranscriptItem> turn_cap{
        ava::tui::TranscriptItem{.label = "thinking", .text = long_body},
        ava::tui::TranscriptItem{.label = "thinking", .text = long_body},
        ava::tui::TranscriptItem{.label = "ava", .text = "tail"},
    };
    auto const cap_update = ava::tui::apply_capped_transcript_snapshot(rebuilt, submitted, turn_cap, 0);
    ava::tui::runtime_transcript::carry_thinking_expansion(overrides, rebuilt, cap_update.item_index_shift);
    auto const new_expanded = static_cast<std::size_t>(static_cast<std::ptrdiff_t>(expanded_index) + cap_update.item_index_shift);
    expect(cap_update.leading_evictions == 1 && cap_update.item_index_shift == -1 && new_expanded < rebuilt.size() && rebuilt[new_expanded].thinking_expanded &&
               rebuilt[new_expanded].label == "thinking" && !rebuilt[new_expanded + 1].thinking_expanded,
           "positive cap eviction remaps expansion by item_index_shift and keeps duplicate blocks independent");

    // Stale override must not apply onto a replacement non-thinking tail slot.
    overrides = {{rebuilt.size() - 1, true}};
    auto before = rebuilt.back().label;
    ava::tui::runtime_transcript::carry_thinking_expansion(overrides, rebuilt, 0);
    expect(before == "ava" && !rebuilt.back().thinking_expanded, "stale expansion override does not attach onto a replacement non-thinking tail item");

    // BT-001: submitted-prefix stale true flags must not resurrect a current-UI collapse.
    // Start with a submitted thinking item expanded (as at run start), collapse it in the
    // current UI, keep a different current expanded item, then production apply/capture/carry.
    {
      std::vector<ava::tui::TranscriptItem> submitted_prefix{
          ava::tui::TranscriptItem{.label = "you", .text = "seed"},
          ava::tui::TranscriptItem{.label = "thinking", .text = long_body, .thinking_expanded = true},  // expanded at run start
          ava::tui::TranscriptItem{.label = "ava", .text = "prior answer"},
      };
      // Current UI: user collapsed the submitted-prefix thinking and expanded a later turn item.
      std::vector<ava::tui::TranscriptItem> current_ui = submitted_prefix;
      current_ui[1].thinking_expanded = false;
      current_ui.push_back(ava::tui::TranscriptItem{.label = "thinking", .text = long_body, .thinking_expanded = true});
      current_ui.push_back(ava::tui::TranscriptItem{.label = "ava", .text = "live tail"});
      auto const current_overrides = ava::tui::runtime_transcript::capture_thinking_expansion(current_ui);
      expect(current_overrides.size() == 1 && current_overrides.front().first == 3 && current_overrides.front().second,
             "capture records only the currently expanded thinking index, not the collapsed submitted-prefix item");

      // Shift zero: apply restores submitted true flags; carry must clear them and reapply current set.
      std::vector<ava::tui::TranscriptItem> fresh_turn{
          ava::tui::TranscriptItem{.label = "thinking", .text = long_body},
          ava::tui::TranscriptItem{.label = "ava", .text = "live tail"},
      };
      std::vector<ava::tui::TranscriptItem> destination;
      auto const zero = ava::tui::apply_capped_transcript_snapshot(destination, submitted_prefix, fresh_turn, 0);
      expect(zero.item_index_shift == 0 && destination[1].thinking_expanded,
             "apply_capped_transcript_snapshot restores submitted-prefix thinking_expanded=true before carry");
      ava::tui::runtime_transcript::carry_thinking_expansion(current_overrides, destination, zero.item_index_shift);
      expect(zero.item_index_shift == 0 && !destination[1].thinking_expanded && destination[1].label == "thinking" && destination[3].thinking_expanded &&
                 destination[3].label == "thinking",
             "shift-zero carry keeps a current-UI collapse collapsed and a current expansion expanded");

      // Positive cap eviction / nonzero shift: same collapse authority after index remap.
      submitted_prefix.clear();
      submitted_prefix.reserve(ava::tui::kMaxTranscriptItems);
      for (std::size_t index = 0; index < ava::tui::kMaxTranscriptItems - 4; ++index)
        submitted_prefix.push_back(ava::tui::TranscriptItem{.label = "you", .text = "pad " + std::to_string(index)});
      auto const collapsed_submitted_index = submitted_prefix.size();
      submitted_prefix.push_back(ava::tui::TranscriptItem{.label = "thinking", .text = long_body, .thinking_expanded = true});
      submitted_prefix.push_back(ava::tui::TranscriptItem{.label = "ava", .text = "prior answer"});
      current_ui = submitted_prefix;
      current_ui[collapsed_submitted_index].thinking_expanded = false;
      auto const current_expanded_index = current_ui.size();
      current_ui.push_back(ava::tui::TranscriptItem{.label = "thinking", .text = long_body, .thinking_expanded = true});
      current_ui.push_back(ava::tui::TranscriptItem{.label = "thinking", .text = long_body});  // duplicate stays collapsed
      current_ui.push_back(ava::tui::TranscriptItem{.label = "ava", .text = "tail"});
      auto const cap_overrides = ava::tui::runtime_transcript::capture_thinking_expansion(current_ui);
      expect(cap_overrides.size() == 1 && cap_overrides.front().first == current_expanded_index,
             "cap-path capture still records only the current expanded index");
      // destination size = (kMax-2 submitted) + 3 turn = kMax+1 => 1 leading eviction, shift=-1.
      std::vector<ava::tui::TranscriptItem> turn_cap{
          ava::tui::TranscriptItem{.label = "thinking", .text = long_body},
          ava::tui::TranscriptItem{.label = "thinking", .text = long_body},
          ava::tui::TranscriptItem{.label = "ava", .text = "tail"},
      };
      auto const cap = ava::tui::apply_capped_transcript_snapshot(destination, submitted_prefix, turn_cap, 0);
      ava::tui::runtime_transcript::carry_thinking_expansion(cap_overrides, destination, cap.item_index_shift);
      auto const new_collapsed = static_cast<std::size_t>(static_cast<std::ptrdiff_t>(collapsed_submitted_index) + cap.item_index_shift);
      auto const new_expanded = static_cast<std::size_t>(static_cast<std::ptrdiff_t>(current_expanded_index) + cap.item_index_shift);
      expect(cap.leading_evictions == 1 && cap.item_index_shift == -1 && new_collapsed < destination.size() && new_expanded < destination.size() &&
                 !destination[new_collapsed].thinking_expanded && destination[new_collapsed].label == "thinking" &&
                 destination[new_expanded].thinking_expanded && destination[new_expanded].label == "thinking" &&
                 !destination[new_expanded + 1].thinking_expanded,
             "positive cap shift carry keeps submitted-prefix collapse collapsed, current expansion expanded, and duplicates independent");
    }
  }

  // Idle /thinking details mutates only presentation state, even at the transcript cap.
  {
    ava::tui::ComposerSnapshot idle_snapshot;
    idle_snapshot.mode = "build";
    //    idle_snapshot.provider = "openai";
    //    idle_snapshot.model = "gpt-5.5";
    idle_snapshot.session_id = "session_thinking_details_cap";
    idle_snapshot.status = "ready";
    idle_snapshot.width = 100;
    idle_snapshot.height = 40;
    idle_snapshot.thinking_visible = true;
    idle_snapshot.transcript.reserve(ava::tui::kMaxTranscriptItems);
    for (std::size_t index = 0; index < ava::tui::kMaxTranscriptItems - 1; ++index)
      idle_snapshot.transcript.push_back(ava::tui::TranscriptItem{.label = "you", .text = "pad " + std::to_string(index)});
    idle_snapshot.transcript.push_back(ava::tui::TranscriptItem{.label = "thinking", .text = long_body});
    expect(idle_snapshot.transcript.size() == ava::tui::kMaxTranscriptItems && !idle_snapshot.transcript.back().thinking_expanded,
           "idle /thinking details fixture starts at the transcript cap with collapsed latest thinking");

    auto const width = ava::tui::composer_main_width(idle_snapshot);
    auto const item_index = ava::tui::toggle_latest_boundable_thinking(idle_snapshot.transcript, width, idle_snapshot.thinking_visible);
    expect(item_index && *item_index == ava::tui::kMaxTranscriptItems - 1 && idle_snapshot.transcript[*item_index].thinking_expanded,
           "toggle expands the latest boundable thinking at the pre-push index");
    idle_snapshot.status = idle_snapshot.transcript[*item_index].thinking_expanded ? "latest thinking details expanded" : "latest thinking details collapsed";
    expect(
        idle_snapshot.transcript.size() == ava::tui::kMaxTranscriptItems && idle_snapshot.transcript.back().label == "thinking" &&
            idle_snapshot.transcript.back().thinking_expanded && idle_snapshot.status == "latest thinking details expanded" &&
            std::ranges::none_of(idle_snapshot.transcript,
                                 [](ava::tui::TranscriptItem const& item) { return item.label == "cmd" || item.text.starts_with("latest thinking details"); }),
        "idle /thinking details leaves the capped transcript unchanged apart from the requested presentation toggle");
  }

  // Hit-test: Thinking header toggles only boundable completed items; tools still win first.
  {
    auto hit_snapshot = ava::tui::ComposerSnapshot{
        .mode = "build",
        //.provider = "openai",
        //.model = "gpt-5.5",
        .session_id = "session_thinking_hit",
        .input = "draft",
        .status = "ready",
        .transcript = {ava::tui::TranscriptItem{.label = "ava", .text = "intro"}, ava::tui::TranscriptItem{.label = "thinking", .text = long_body},
                       ava::tui::TranscriptItem{.tool = ava::tui::ToolTimelineItem{.status = ava::tui::ToolTimelineStatus::Success,
                                                                                   .name = "glob",
                                                                                   .result_summary = "1 file",
                                                                                   .call_id = "glob_hit",
                                                                                   .lifecycle = ava::tui::ToolLifecycleState::Complete}},
                       ava::tui::TranscriptItem{.label = "ava", .text = "tail answer", .thinking = long_body}},
        .width = 100,
        .height = 40,
    };
    auto const frame = plain_lines(ava::tui::render_composer(frontend.frontend(), hit_snapshot));
    auto const thinking_line = std::ranges::find_if(frame, [](std::string const& line) { return line.find("Thinking:") != std::string::npos; });
    auto const tool_line = std::ranges::find_if(frame, [](std::string const& line) { return line.find("+ glob") != std::string::npos; });
    expect(thinking_line != frame.end() && tool_line != frame.end(), "thinking hit-test fixture renders Thinking header and tool card");
    auto const thinking_row = static_cast<std::size_t>(thinking_line - frame.begin()) + 1;
    auto const tool_row = static_cast<std::size_t>(tool_line - frame.begin()) + 1;
    auto const canvas = ava::tui::composer_canvas_layout(hit_snapshot);
    auto const col = canvas.left + 8;
    expect(ava::tui::detail::transcript_thinking_header_for_screen_position(hit_snapshot, thinking_row, col) == 1,
           "thinking header hit-test maps the boundable completed Thinking: row");
    expect(ava::tui::detail::transcript_tool_card_header_for_screen_position(hit_snapshot, tool_row, col) == 2, "tool header hit-test still wins on tool rows");
    expect(!ava::tui::detail::transcript_thinking_header_for_screen_position(hit_snapshot, tool_row, col), "thinking hit-test ignores tool header rows");
    // Live pending is not boundable.
    hit_snapshot.transcript[1].stream_id = "live";
    hit_snapshot.transcript[1].append_only_stream = true;
    expect(!ava::tui::detail::transcript_thinking_header_for_screen_position(hit_snapshot, thinking_row, col),
           "live pending thinking header is not a toggle target");
    hit_snapshot.transcript[1].stream_id.clear();
    hit_snapshot.transcript[1].append_only_stream = false;

    // Detached viewport anchor: scroll so the thinking header is visible mid-history.
    hit_snapshot.transcript.insert(hit_snapshot.transcript.begin(), 25, ava::tui::TranscriptItem{.label = "you", .text = "older line"});
    hit_snapshot.transcript.insert(hit_snapshot.transcript.end(), 25, ava::tui::TranscriptItem{.label = "ava", .text = "newer line"});
    auto const layout = ava::tui::detail::render_transcript_layout(hit_snapshot.transcript, ava::tui::composer_main_width(hit_snapshot), true, true, false);
    auto const thinking_msg = std::ranges::find(layout.message_item_indices, std::size_t{26});
    expect(thinking_msg != layout.message_item_indices.end(), "detached fixture retains the shifted thinking item index");
    auto const position = static_cast<std::size_t>(thinking_msg - layout.message_item_indices.begin());
    auto const max_scroll = ava::tui::composer_max_transcript_scroll_offset(hit_snapshot, hit_snapshot.width, hit_snapshot.height);
    auto const desired_start = layout.content_starts[position];
    hit_snapshot.transcript_scroll_offset = max_scroll - std::min(max_scroll, desired_start);
    expect(ava::tui::detail::transcript_thinking_header_for_screen_position(hit_snapshot, 1, col) == 26,
           "detached transcript thinking hit-test maps the anchored visible Thinking header");

    // Generation: toggle bumps caller-owned generation; layout identity follows thinking_expanded.
    auto generation = hit_snapshot.transcript_generation;
    expect(ava::tui::toggle_thinking_expansion_at(hit_snapshot.transcript, 26, ava::tui::composer_main_width(hit_snapshot), true),
           "toggle_thinking_expansion_at expands the detached boundable item");
    ++generation;
    hit_snapshot.transcript_generation = generation;
    auto const expanded_detached =
        ava::tui::detail::render_transcript_layout(hit_snapshot.transcript, ava::tui::composer_main_width(hit_snapshot), true, true, false);
    expect(hit_snapshot.transcript[26].thinking_expanded && expanded_detached.lines.size() > layout.lines.size(),
           "thinking expansion invalidates prior bounded layout geometry for the same generation bump");
  }
}
}  // namespace

void run_tui_large_render_performance_tests()
{
  test_tui_large_render_performance_budget();
}

void run_tui_transcript_hierarchy_tests()
{
  test_tui_osc52_clipboard_sequence_bounds();
  test_tui_f2_transcript_hierarchy_and_tool_shell();
  test_tui_bounded_thinking_disclosure_render_and_toggle();
  test_tui_detached_transcript_anchor_survives_capped_eviction();
  test_tui_detached_append_defers_layout_until_anchor_recovery();
  test_tui_streaming_tail_cache_is_bounded_and_matches_full_renderer();
  test_tui_transcript_tail_renderer_matches_full_visible_window();
}

void run_tui_transcript_cache_tests()
{
  test_tui_completion_and_transcript_layout_caches();
  test_tui_very_long_transcript_performance_budget();
}

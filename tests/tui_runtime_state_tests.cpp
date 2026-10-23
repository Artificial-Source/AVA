#include "sys.h"
#include "tests/support/test_harness.h"
#include "tests/support/tui_test_support.h"
#include "ava/event/EventEnvelope.h"
#include "ava/event/EventEnvelopeContext.h"
#include "ava/event/RuntimeEvent.h"
#include "ava/tui/TuiFrontEnd.h"
#include "ava/tui/composer.h"
#include "ava/tui/event_state.h"
#include "ava/tui/runtime.h"
#include "ava/tui/runtime_internal.h"
#include "ava/tui/runtime_state_internal.h"
#include "ava/tui/runtime_transcript_internal.h"
#include "ava/tui/tool_cards.h"
#include "ava/core/error.h"
#include "ava/core/result.h"

#include <algorithm>
#include <array>
#include <condition_variable>
#include <cstddef>
#include <expected>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {
void apply_session_start(ava::tui::TuiEventState& state, ava::core::Mode mode, std::string provider, std::string model)
{
  ava::tui::apply_runtime_event(
      state,
      ava::event::RuntimeEvent{{}, ava::event::SessionStartEvent{.payload = {.mode = mode, .provider = std::move(provider), .model = std::move(model)}}});
}

[[nodiscard]] ava::event::RuntimeEvent user_message_event(ava::event::MessagePayload payload)
{
  return {{}, ava::event::UserMessageEvent{.payload = std::move(payload)}};
}

[[nodiscard]] ava::event::RuntimeEvent assistant_message_event(ava::event::MessagePayload payload)
{
  return {{}, ava::event::AssistantMessageEvent{.payload = std::move(payload)}};
}

[[nodiscard]] ava::event::RuntimeEvent message_update_event(ava::event::MessagePayload payload)
{
  return {{}, ava::event::MessageUpdateEvent{.payload = std::move(payload)}};
}

[[nodiscard]] ava::event::RuntimeEvent message_end_event(ava::event::MessagePayload payload = {})
{
  return {{}, ava::event::MessageEndEvent{.payload = std::move(payload)}};
}

[[nodiscard]] ava::event::RuntimeEvent reasoning_start_event(ava::event::ReasoningPayload payload)
{
  return {{}, ava::event::ReasoningStartEvent{.payload = std::move(payload)}};
}

[[nodiscard]] ava::event::RuntimeEvent reasoning_delta_event(ava::event::ReasoningPayload payload)
{
  return {{}, ava::event::ReasoningDeltaEvent{.payload = std::move(payload)}};
}

[[nodiscard]] ava::event::RuntimeEvent reasoning_end_event(ava::event::ReasoningPayload payload = {})
{
  return {{}, ava::event::ReasoningEndEvent{.payload = std::move(payload)}};
}

[[nodiscard]] ava::event::RuntimeEvent provider_event(ava::event::ProviderPayload payload)
{
  return {{}, ava::event::ProviderEvent{.payload = std::move(payload)}};
}

[[nodiscard]] ava::event::RuntimeEvent tool_start_event(ava::event::ToolPayload payload)
{
  return {{}, ava::event::ToolStartEvent{.payload = std::move(payload)}};
}

[[nodiscard]] ava::event::RuntimeEvent tool_progress_event(ava::event::ToolPayload payload)
{
  return {{}, ava::event::ToolProgressEvent{.payload = std::move(payload)}};
}

[[nodiscard]] ava::event::RuntimeEvent tool_result_event(ava::event::ToolPayload payload)
{
  return {{}, ava::event::ToolResultEvent{.payload = std::move(payload)}};
}

[[nodiscard]] ava::event::RuntimeEvent compaction_start_event(ava::event::CompactionPayload payload)
{
  return {{}, ava::event::CompactionStartEvent{.payload = std::move(payload)}};
}

[[nodiscard]] ava::event::RuntimeEvent compaction_end_event(ava::event::CompactionPayload payload)
{
  return {{}, ava::event::CompactionEndEvent{.payload = std::move(payload)}};
}

[[nodiscard]] ava::event::RuntimeEvent retry_event(ava::event::RetryPayload payload, ava::event::RetryDiagnostics diagnostics = {})
{
  return {{}, ava::event::RetryEvent{.payload = std::move(payload), .diagnostics = std::move(diagnostics)}};
}

[[nodiscard]] ava::event::RuntimeEvent retry_tick_event(ava::event::RetryPayload payload, ava::event::RetryDiagnostics diagnostics = {})
{
  return {{}, ava::event::RetryTickEvent{.payload = std::move(payload), .diagnostics = std::move(diagnostics)}};
}

[[nodiscard]] bool visible_contains(std::vector<std::string> const& lines, std::string_view needle)
{
  return std::ranges::any_of(lines, [&](std::string const& line) { return strip_sgr(line).find(needle) != std::string::npos; });
}

[[nodiscard]] std::vector<std::string> render_processing_with_event_activity(ava::tui::TuiEventState const& state)
{
  tui_test_support::TuiFrontEndFixture frontend;
  return ava::tui::render_composer(frontend.frontend(), ava::tui::ComposerSnapshot{.mode = "build",
                                                                                   //.provider = "openai",
                                                                                   //.model = "gpt-5.5",
                                                                                   .session_id = "session_test",
                                                                                   .input = "",
                                                                                   .status = "thinking...",
                                                                                   .processing = true,
                                                                                   .active_run_hint = ava::tui::ActiveRunHint{.interrupt = "Esc"},
                                                                                   .transcript = {},
                                                                                   .width = 88,
                                                                                   .height = 12,
                                                                                   .sidebar = ava::tui::SidebarSnapshot{.activity = state.activity}});
}

[[nodiscard]] ava::event::RetryPayload sample_retry_payload(std::size_t remaining_ms = 0)
{
  auto payload = ava::event::RetryPayload{};
  payload.trigger = "rate_limit";
  payload.reason = "rate_limit";
  payload.attempt = 2;
  payload.max_attempts = 5;
  payload.delay_ms = 1000;
  payload.remaining_ms = remaining_ms;
  return payload;
}

[[nodiscard]] ava::tui::SidebarActivityItem const* find_retry_activity(ava::tui::TuiEventState const& state)
{
  auto existing = std::ranges::find_if(state.activity, [](ava::tui::SidebarActivityItem const& activity) { return activity.label == "retry"; });
  return existing == state.activity.end() ? nullptr : &*existing;
}

[[nodiscard]] ava::tui::SidebarActivityItem const* find_retry_activity_by_id(ava::tui::TuiEventState const& state, std::string_view id)
{
  auto existing = std::ranges::find_if(state.activity, [&](ava::tui::SidebarActivityItem const& activity) { return activity.id == id; });
  return existing == state.activity.end() ? nullptr : &*existing;
}

[[nodiscard]] std::size_t count_running_retry_activities(ava::tui::TuiEventState const& state)
{
  return static_cast<std::size_t>(std::ranges::count_if(state.activity, [](ava::tui::SidebarActivityItem const& activity) {
    return activity.label == "retry" && activity.status == ava::tui::ToolTimelineStatus::Running;
  }));
}

[[nodiscard]] std::size_t count_retry_activities(ava::tui::TuiEventState const& state)
{
  return static_cast<std::size_t>(
      std::ranges::count_if(state.activity, [](ava::tui::SidebarActivityItem const& activity) { return activity.label == "retry"; }));
}

[[nodiscard]] ava::event::RetryPayload retry_payload_with_reason(std::string reason, std::string trigger, std::size_t attempt, std::size_t delay_ms,
                                                                 std::size_t remaining_ms = 0)
{
  auto payload = ava::event::RetryPayload{};
  payload.reason = std::move(reason);
  payload.trigger = std::move(trigger);
  payload.attempt = attempt;
  payload.max_attempts = 5;
  payload.delay_ms = delay_ms;
  payload.remaining_ms = remaining_ms;
  return payload;
}

[[nodiscard]] ava::event::RuntimeEvent cancellation_event(ava::event::CancellationPayload payload)
{
  return {{}, ava::event::CancellationEvent{.payload = std::move(payload)}};
}

[[nodiscard]] ava::event::RuntimeEvent error_event(ava::event::ErrorPayload payload)
{
  return {{}, ava::event::ErrorEvent{.payload = std::move(payload)}};
}

[[nodiscard]] ava::event::RuntimeEvent completion_event(ava::event::CompletionPayload payload)
{
  return {{}, ava::event::CompletionEvent{.payload = std::move(payload)}};
}

void test_tui_event_state_reduces_runtime_events()
{
  tui_test_support::TuiFrontEndFixture frontend;
  ava::tui::TuiEventState state;
  apply_session_start(state, ava::core::Mode::Build, "openai", "gpt-5.5");

  auto user_payload = ava::event::MessagePayload{};
  user_payload.text = "hello";
  ava::tui::apply_runtime_event(state, user_message_event(user_payload));
  expect(state.run_status == ava::tui::TuiEventRunStatus::Running && state.transcript.size() == 1 && state.transcript[0].label == "you" &&
             state.transcript[0].text == "hello" && ava::tui::to_plain_text(state.transcript[0].text_model) == "hello",
         "tui event state records user messages as completed transcript items");

  auto delta_payload = ava::event::MessagePayload{};
  delta_payload.text = "hel";
  ava::tui::apply_runtime_event(state, message_update_event(delta_payload));
  delta_payload.text = "lo";
  ava::tui::apply_runtime_event(state, message_update_event(delta_payload));
  auto streaming_snapshot = ava::tui::event_state_transcript_snapshot(state);
  auto active_streaming_snapshot = ava::tui::event_state_transcript_snapshot(state, ava::tui::PendingTextProjection::Unparsed);
  expect(
      state.pending_assistant_text == "hello" && streaming_snapshot.size() == 2 && streaming_snapshot[1].label == "ava" &&
          streaming_snapshot[1].text == "hello" && streaming_snapshot[1].meta == "Build · GPT-5.5" &&
          ava::tui::to_plain_text(streaming_snapshot[1].text_model) == "hello" && active_streaming_snapshot.size() == 2 &&
          ava::tui::text_empty(active_streaming_snapshot[1].text_model) && active_streaming_snapshot[1].text == "hello",
      "default event snapshots fully model pending text while active streaming projection leaves cumulative text unparsed for the incremental tail renderer");

  ava::tui::apply_runtime_event(state, message_end_event());
  expect(state.run_status == ava::tui::TuiEventRunStatus::Completed && state.pending_assistant_text.empty() && state.transcript.size() == 2 &&
             state.transcript[1].label == "ava" && state.transcript[1].text == "hello" && state.transcript[1].meta == "Build · GPT-5.5" &&
             ava::tui::to_plain_text(state.transcript[1].text_model) == "hello",
         "tui event state commits assistant deltas on message end");
  expect(!state.activity.empty() && state.activity.back().id == "responding" && state.activity.back().status == ava::tui::ToolTimelineStatus::Success &&
             state.activity.back().detail == "assistant responded",
         "tui event state settles responding activity when assistant streaming ends");

  ava::tui::TuiEventState non_gpt_state;
  apply_session_start(non_gpt_state, ava::core::Mode::Build, "anthropic", "claude-sonnet-4-5");
  auto non_gpt_delta_payload = ava::event::MessagePayload{};
  non_gpt_delta_payload.text = "hi";
  ava::tui::apply_runtime_event(non_gpt_state, message_update_event(non_gpt_delta_payload));
  auto const non_gpt_snapshot = ava::tui::event_state_transcript_snapshot(non_gpt_state);
  expect(non_gpt_snapshot.size() == 1 && non_gpt_snapshot[0].meta == "Build · Claude Sonnet 4.5",
         "tui event state uses centralized model profile display labels for non-GPT assistant metadata");

  auto projected_turn = std::vector<ava::tui::TranscriptItem>{
      ava::tui::TranscriptItem{.label = "ava", .text = "first segment", .meta = "stale first metadata"},
      ava::tui::TranscriptItem{.tool = ava::tui::ToolTimelineItem{.status = ava::tui::ToolTimelineStatus::Success, .name = "read"}},
      ava::tui::TranscriptItem{.label = "ava", .meta = "stale reasoning metadata", .thinking = "reasoning segment"},
      ava::tui::TranscriptItem{.label = "ava", .text = "final segment"},
      ava::tui::TranscriptItem{.label = "ava", .meta = "stale hidden metadata", .thinking = "hidden final reasoning"},
      ava::tui::TranscriptItem{.label = "ava", .meta = "stale empty metadata"}};
  ava::tui::runtime_transcript::apply_assistant_turn_meta(projected_turn, "Build · GPT-5.5 · 1.2s", false);
  expect(projected_turn.size() == 6 && projected_turn[0].meta.empty() && projected_turn[2].meta.empty() && projected_turn[3].meta == "Build · GPT-5.5 · 1.2s" &&
             projected_turn[4].meta.empty() && projected_turn[5].meta.empty() && projected_turn[0].text == "first segment" &&
             projected_turn[2].thinking == "reasoning segment" && projected_turn[3].text == "final segment" &&
             projected_turn[4].thinking == "hidden final reasoning" && projected_turn[1].tool,
         "tui active-turn projection clears intermediate metadata and attaches one footer to the final visible assistant segment without reordering output");

  ava::tui::ComposerSnapshot fallback_snapshot;
  fallback_snapshot.thinking_visible = false;
  fallback_snapshot.transcript.resize(ava::tui::kMaxTranscriptItems - 3, ava::tui::TranscriptItem{.label = "old", .text = "retained"});
  fallback_snapshot.transcript.push_back(
      ava::tui::TranscriptItem{.tool = ava::tui::ToolTimelineItem{.status = ava::tui::ToolTimelineStatus::Success, .name = "read"}});
  ava::tui::runtime_transcript::push_fallback_assistant_outputs(fallback_snapshot, {"first output", "", "final output"}, "Build · GPT-5.5 · 2.0s");
  auto const metadata_count = std::ranges::count_if(fallback_snapshot.transcript, [](auto const& item) { return !item.meta.empty(); });
  auto const fallback_tail = fallback_snapshot.transcript.end() - 4;
  expect(fallback_snapshot.transcript.size() == ava::tui::kMaxTranscriptItems && fallback_snapshot.transcript_generation == 3 && fallback_tail->tool &&
             (fallback_tail + 1)->text == "first output" && (fallback_tail + 1)->meta.empty() && (fallback_tail + 2)->text.empty() &&
             (fallback_tail + 2)->meta.empty() && (fallback_tail + 3)->text == "final output" && (fallback_tail + 3)->meta == "Build · GPT-5.5 · 2.0s" &&
             metadata_count == 1,
         "no-event fallback wiring keeps tool context before capped multi-output items and gives metadata only to the final visible assistant output");

  ava::tui::ComposerSnapshot trailing_empty_fallback;
  ava::tui::runtime_transcript::push_fallback_assistant_outputs(trailing_empty_fallback, {"visible output", ""}, "Build · GPT-5.5");
  expect(trailing_empty_fallback.transcript.size() == 2 && trailing_empty_fallback.transcript[0].meta == "Build · GPT-5.5" &&
             trailing_empty_fallback.transcript[1].meta.empty(),
         "no-event fallback normalization does not move the footer from the final visible output to an empty trailing item");

  ava::tui::ComposerSnapshot capped_fallback;
  capped_fallback.transcript.resize(ava::tui::kMaxTranscriptItems, ava::tui::TranscriptItem{.label = "old", .text = "old item"});
  auto const fallback_shift = ava::tui::runtime_transcript::push_fallback_assistant_outputs(capped_fallback, {"one", "two", "three"}, "Build · GPT-5.5");
  expect(fallback_shift == -3 && capped_fallback.transcript.size() == ava::tui::kMaxTranscriptItems &&
             capped_fallback.transcript[capped_fallback.transcript.size() - 3].text == "one" && capped_fallback.transcript.back().text == "three",
         "multi-output no-event fallback reports the exact accumulated leading item-index shift at the production cap");

  ava::tui::ComposerSnapshot over_cap_push;
  over_cap_push.transcript.resize(ava::tui::kMaxTranscriptItems + 4, ava::tui::TranscriptItem{.label = "old", .text = "old item"});
  auto const over_cap_shift = ava::tui::runtime_transcript::push_transcript(over_cap_push, ava::tui::TranscriptItem{.label = "ava", .text = "new item"});
  expect(over_cap_shift == -5 && over_cap_push.transcript.size() == ava::tui::kMaxTranscriptItems && over_cap_push.transcript.back().text == "new item",
         "direct transcript pushes return the exact negative truncation shift even when the input snapshot starts above the cap");

  auto assistant_final_payload = ava::event::MessagePayload{};
  assistant_final_payload.text = "hello";
  ava::tui::apply_runtime_event(state, assistant_message_event(assistant_final_payload));
  expect(state.transcript.size() == 2 && state.transcript[1].text == "hello", "tui event state avoids duplicating matching streamed assistant final events");

  assistant_final_payload.text = "hello\n";
  ava::tui::apply_runtime_event(state, assistant_message_event(assistant_final_payload));
  expect(state.transcript.size() == 2 && state.transcript[1].text == "hello",
         "tui event state treats trailing whitespace-only final changes as duplicate streamed assistant events");

  ava::tui::TuiEventState reasoning_state;
  apply_session_start(reasoning_state, ava::core::Mode::Build, "openai", "gpt-5.5");
  auto reasoning_start_payload = ava::event::ReasoningPayload{};
  reasoning_start_payload.reasoning_format = "summary";
  ava::tui::apply_runtime_event(reasoning_state, reasoning_start_event(reasoning_start_payload));
  auto reasoning_delta_payload = ava::event::ReasoningPayload{};
  reasoning_delta_payload.text = "checking";
  ava::tui::apply_runtime_event(reasoning_state, reasoning_delta_event(reasoning_delta_payload));
  reasoning_delta_payload.text = " options";
  ava::tui::apply_runtime_event(reasoning_state, reasoning_delta_event(reasoning_delta_payload));
  auto reasoning_snapshot = ava::tui::event_state_transcript_snapshot(reasoning_state);
  expect(reasoning_state.pending_reasoning_text == "checking options" && reasoning_snapshot.size() == 1 && reasoning_snapshot[0].label == "ava" &&
             reasoning_snapshot[0].thinking == "checking options" && reasoning_snapshot[0].text.empty() &&
             ava::tui::to_plain_text(reasoning_snapshot[0].thinking_model) == "checking options",
         "tui event state exposes pending reasoning as part of the assistant turn");
  ava::tui::apply_runtime_event(reasoning_state, reasoning_end_event());
  expect(reasoning_state.pending_reasoning_text == "checking options" && reasoning_state.transcript.empty() && reasoning_state.activity.size() == 1 &&
             reasoning_state.activity[0].label == "reasoning" && reasoning_state.activity[0].status == ava::tui::ToolTimelineStatus::Success,
         "tui event state keeps completed reasoning attached to the pending assistant turn");

  auto reasoning_answer_payload = ava::event::MessagePayload{};
  reasoning_answer_payload.text = "answer";
  ava::tui::apply_runtime_event(reasoning_state, message_update_event(reasoning_answer_payload));
  ava::tui::apply_runtime_event(reasoning_state, message_end_event());
  expect(reasoning_state.pending_reasoning_text.empty() && reasoning_state.transcript.size() == 1 && reasoning_state.transcript[0].label == "ava" &&
             reasoning_state.transcript[0].text == "answer" && reasoning_state.transcript[0].thinking == "checking options" &&
             ava::tui::to_plain_text(reasoning_state.transcript[0].text_model) == "answer" &&
             ava::tui::to_plain_text(reasoning_state.transcript[0].thinking_model) == "checking options",
         "tui event state commits reasoning and answer as one assistant transcript item");

  auto const thinking_render = ava::tui::render_composer(frontend.frontend(), ava::tui::ComposerSnapshot{.mode = "build",
                                                                                                         //.provider = "openai",
                                                                                                         //.model = "gpt-5.5",
                                                                                                         .session_id = "session_test",
                                                                                                         .input = "",
                                                                                                         .status = "ready",
                                                                                                         .transcript = reasoning_state.transcript,
                                                                                                         .width = 60,
                                                                                                         .height = 10});
  expect(std::ranges::any_of(thinking_render, [](std::string const& line) { return strip_sgr(line).find("Thinking: checking options") != std::string::npos; }),
         "tui renders reasoning content as an inline thinking transcript block with a stable prefix");
  expect(std::ranges::any_of(thinking_render,
                             [](std::string const& line) {
                               return strip_sgr(line).find("Thinking:") != std::string::npos && line.find("\x1b[38;2;88;96;112m") != std::string::npos &&
                                      line.find("\x1b[3m") != std::string::npos && line.find("\x1b[48;2;18;23;34m") == std::string::npos;
                             }),
         "tui renders accessible labeled reasoning as muted italic text on the ordinary screen background");
  expect(std::ranges::none_of(thinking_render,
                              [](std::string const& line) {
                                auto const visible = strip_sgr(line);
                                return visible.find("╭─ AVA") != std::string::npos || visible.find("AVA:") != std::string::npos ||
                                       visible.find("╭─ You") != std::string::npos || visible.find("You:") != std::string::npos;
                              }),
         "tui transcript role headers stay hidden for compact chat rendering");
  expect(std::ranges::none_of(thinking_render, [](std::string const& line) { return strip_sgr(line).find("╭─ Thinking") != std::string::npos; }),
         "tui thinking transcript block avoids the normal boxed message header");
  auto const hidden_thinking_render = ava::tui::render_composer(frontend.frontend(), ava::tui::ComposerSnapshot{.mode = "build",
                                                                                                                //.provider = "openai",
                                                                                                                //.model = "gpt-5.5",
                                                                                                                .session_id = "session_test",
                                                                                                                .input = "",
                                                                                                                .status = "ready",
                                                                                                                .transcript = reasoning_state.transcript,
                                                                                                                .width = 60,
                                                                                                                .height = 10,
                                                                                                                .thinking_visible = false});
  expect(std::ranges::none_of(hidden_thinking_render,
                              [](std::string const& line) { return strip_sgr(line).find("Thinking: checking options") != std::string::npos; }) &&
             std::ranges::any_of(hidden_thinking_render, [](std::string const& line) { return strip_sgr(line).find("answer") != std::string::npos; }),
         "tui thinking visibility hides inline thinking blocks without hiding assistant text");

  ava::tui::TuiEventState redacted_reasoning_state;
  auto redacted_reasoning_payload = ava::event::ReasoningPayload{};
  redacted_reasoning_payload.text = "provider-private-secret";
  redacted_reasoning_payload.reasoning_redacted = true;
  ava::tui::apply_runtime_event(redacted_reasoning_state, reasoning_delta_event(redacted_reasoning_payload));
  auto redacted_snapshot = ava::tui::event_state_transcript_snapshot(redacted_reasoning_state);
  auto const redacted_render = ava::tui::render_composer(frontend.frontend(), ava::tui::ComposerSnapshot{.mode = "build",
                                                                                                         //.provider = "openai",
                                                                                                         //.model = "gpt-5.5",
                                                                                                         .session_id = "session_test",
                                                                                                         .input = "",
                                                                                                         .status = "ready",
                                                                                                         .transcript = redacted_snapshot,
                                                                                                         .width = 60,
                                                                                                         .height = 10});
  expect(
      redacted_snapshot.size() == 1 && redacted_snapshot[0].thinking == "[reasoning redacted]" &&
          std::ranges::none_of(redacted_render, [](std::string const& line) { return strip_sgr(line).find("provider-private-secret") != std::string::npos; }),
      "tui event state never renders text from redacted reasoning deltas");

  ava::tui::TuiEventState audit_state;
  auto permission_audit_payload = ava::event::ProviderPayload{};
  permission_audit_payload.text = "permission requested: bash pwd";
  permission_audit_payload.status = "tui:permission_request";
  ava::tui::apply_runtime_event(audit_state, provider_event(permission_audit_payload));
  auto question_audit_payload = ava::event::ProviderPayload{};
  question_audit_payload.text = "question answered: yes";
  question_audit_payload.status = "tui:question_answer";
  ava::tui::apply_runtime_event(audit_state, provider_event(question_audit_payload));
  expect(audit_state.transcript.empty() && audit_state.activity.size() == 2 && audit_state.activity[0].label == "permission" &&
             audit_state.activity[1].label == "question",
         "tui keeps routine permission and question receipts in internal activity rather than the ordinary transcript");

  ava::tui::TuiEventState live_permission_state;
  auto live_permission_tool_start_payload = ava::event::ToolPayload{};
  live_permission_tool_start_payload.text = "git push origin main";
  live_permission_tool_start_payload.call_id = "call_live_permission";
  live_permission_tool_start_payload.tool = "bash";
  ava::tui::apply_runtime_event(live_permission_state, tool_start_event(live_permission_tool_start_payload));
  auto live_permission_request_payload = ava::event::ProviderPayload{};
  live_permission_request_payload.text = "git push origin main";
  live_permission_request_payload.tool = "bash";
  live_permission_request_payload.status = "tui:permission_request";
  live_permission_request_payload.reason = "changes remote state";
  live_permission_request_payload.permission_request_ids = {"perm_live_permission"};
  ava::tui::apply_runtime_event(live_permission_state, provider_event(live_permission_request_payload));
  expect(live_permission_state.pending_tools.size() == 1 && live_permission_state.pending_tools[0].item.permissions.size() == 1 &&
             live_permission_state.pending_tools[0].item.permissions[0].permission_request_id == "perm_live_permission" &&
             live_permission_state.pending_tools[0].item.permissions[0].decision.empty() &&
             strip_sgr(ava::tui::detail::render_tool_card(live_permission_state.pending_tools[0].item, 100, false).front()).find("permission") ==
                 std::string::npos,
         "tui event state attaches the permission request internally without painting it on the running tool");
  auto live_permission_allow_payload = live_permission_request_payload;
  live_permission_allow_payload.status = "tui:permission_allow";
  live_permission_allow_payload.tool.clear();
  live_permission_allow_payload.text = "permission allowed";
  ava::tui::apply_runtime_event(live_permission_state, provider_event(live_permission_allow_payload));
  expect(live_permission_state.pending_tools[0].item.permissions.size() == 1 &&
             live_permission_state.pending_tools[0].item.permissions[0].decision == "allow" &&
             strip_sgr(ava::tui::detail::render_tool_card(live_permission_state.pending_tools[0].item, 100, false).front()).find("permission allow") ==
                 std::string::npos,
         "tui merges a permission resolution internally without painting a routine allow receipt");
  auto live_permission_result_payload = ava::event::ToolPayload{};
  live_permission_result_payload.text = "pushed";
  live_permission_result_payload.call_id = live_permission_tool_start_payload.call_id;
  live_permission_result_payload.tool = live_permission_tool_start_payload.tool;
  ava::tui::apply_runtime_event(live_permission_state, tool_result_event(live_permission_result_payload));
  expect(live_permission_state.pending_tools.empty() && !live_permission_state.transcript.empty() && live_permission_state.transcript.back().tool &&
             live_permission_state.transcript.back().tool->permissions.size() == 1 &&
             live_permission_state.transcript.back().tool->permissions[0].decision == "allow",
         "tui event state carries the correlated permission audit into the later settled tool result");

  auto verify_settled_provider_permission = [&](std::string resolution_detail, std::string expected_decision, std::string expected_label) {
    ava::tui::TuiEventState provider_permission_state;
    auto tool_start_payload = live_permission_tool_start_payload;
    tool_start_payload.call_id = "call_provider_permission_" + expected_decision + resolution_detail;
    ava::tui::apply_runtime_event(provider_permission_state, tool_start_event(tool_start_payload));
    auto request_payload = live_permission_request_payload;
    request_payload.permission_request_ids = {"perm_provider_permission"};
    ava::tui::apply_runtime_event(provider_permission_state, provider_event(request_payload));
    auto resolution_payload = request_payload;
    resolution_payload.status = "tui:permission_allow";
    resolution_payload.text = "permission allowed";
    resolution_payload.error_details = std::move(resolution_detail);
    ava::tui::apply_runtime_event(provider_permission_state, provider_event(resolution_payload));
    auto result_payload = live_permission_result_payload;
    result_payload.call_id = tool_start_payload.call_id;
    ava::tui::apply_runtime_event(provider_permission_state, tool_result_event(result_payload));
    auto const* item = provider_permission_state.transcript.empty() || !provider_permission_state.transcript.back().tool
                           ? nullptr
                           : &*provider_permission_state.transcript.back().tool;
    auto const rendered = item ? tui_test_support::join_visible_lines(ava::tui::detail::render_tool_card(*item, 100, true)) : std::string{};
    auto const copied = item ? ava::tui::detail::tool_card_copy_text(*item) : std::string{};
    expect(item && item->permissions.size() == 1 && item->permissions[0].decision == expected_decision && rendered.find("permission:") == std::string::npos &&
               copied.find("permission:") == std::string::npos && rendered.find("perm_provider_permission") == std::string::npos &&
               copied.find("perm_provider_permission") == std::string::npos,
           "tui provider permission audit preserves " + expected_label + " internally without exposing a routine receipt in card or copy text");
  };
  verify_settled_provider_permission("selected allow", "allow", "allow");
  verify_settled_provider_permission("selected allow session", "allow_session", "allow session");
  verify_settled_provider_permission("reused tui session grant", "allow_session", "allow session");
  verify_settled_provider_permission("selected allow and remember", "allow_remember", "allow always");

  ava::tui::TuiEventState ambiguous_permission_state;
  auto ambiguous_start_payload = live_permission_tool_start_payload;
  ambiguous_start_payload.tool = "read_file";
  ambiguous_start_payload.call_id = "call_ambiguous_one";
  ava::tui::apply_runtime_event(ambiguous_permission_state, tool_start_event(ambiguous_start_payload));
  ambiguous_start_payload.call_id = "call_ambiguous_two";
  ava::tui::apply_runtime_event(ambiguous_permission_state, tool_start_event(ambiguous_start_payload));
  auto ambiguous_request_payload = live_permission_request_payload;
  ambiguous_request_payload.tool = "read_file";
  ambiguous_request_payload.permission_request_ids = {"perm_ambiguous"};
  ava::tui::apply_runtime_event(ambiguous_permission_state, provider_event(ambiguous_request_payload));
  expect(ambiguous_permission_state.permission_audits.size() == 1 &&
             std::ranges::all_of(ambiguous_permission_state.pending_tools,
                                 [](ava::tui::PendingToolItem const& pending) { return pending.item.permissions.empty(); }),
         "tui event state does not guess between multiple running tools with the same permission tool name");
  auto ambiguous_first_result_payload = live_permission_result_payload;
  ambiguous_first_result_payload.call_id = "call_ambiguous_one";
  ambiguous_first_result_payload.tool = "read_file";
  ava::tui::apply_runtime_event(ambiguous_permission_state, tool_result_event(ambiguous_first_result_payload));
  auto ambiguous_reply_payload = ambiguous_request_payload;
  ambiguous_reply_payload.status = "tui:permission_allow";
  ambiguous_reply_payload.text = "permission allowed";
  ambiguous_reply_payload.error_details = "selected allow";
  ava::tui::apply_runtime_event(ambiguous_permission_state, provider_event(ambiguous_reply_payload));
  expect(ambiguous_permission_state.pending_tools.size() == 1 && ambiguous_permission_state.pending_tools[0].call_id == "call_ambiguous_two" &&
             ambiguous_permission_state.pending_tools[0].item.permissions.empty(),
         "tui permission audit merge never reuses unique-name fallback after an initially ambiguous request");
  auto ambiguous_second_result_payload = ambiguous_first_result_payload;
  ambiguous_second_result_payload.call_id = "call_ambiguous_two";
  ava::tui::apply_runtime_event(ambiguous_permission_state, tool_result_event(ambiguous_second_result_payload));
  expect(!ambiguous_permission_state.transcript.empty() && ambiguous_permission_state.transcript.back().tool &&
             ambiguous_permission_state.transcript.back().tool->permissions.empty(),
         "tui initially ambiguous permission audit remains unattached when the surviving same-name tool settles");

  ava::tui::TuiEventState exact_permission_state;
  auto exact_first_payload = ambiguous_start_payload;
  exact_first_payload.call_id = "call_exact_one";
  exact_first_payload.permission_request_ids.clear();
  ava::tui::apply_runtime_event(exact_permission_state, tool_start_event(exact_first_payload));
  auto exact_second_payload = exact_first_payload;
  exact_second_payload.call_id = "call_exact_two";
  exact_second_payload.permission_request_ids = {"perm_exact"};
  ava::tui::apply_runtime_event(exact_permission_state, tool_start_event(exact_second_payload));
  auto exact_request_payload = ambiguous_request_payload;
  exact_request_payload.permission_request_ids = {"perm_exact"};
  ava::tui::apply_runtime_event(exact_permission_state, provider_event(exact_request_payload));
  expect(exact_permission_state.pending_tools[0].item.permissions.empty() && exact_permission_state.pending_tools[1].item.permissions.size() == 1,
         "tui event state prefers an exact permission-id match when same-name pending tools are ambiguous");
  auto question_with_permission_shape_payload = ambiguous_request_payload;
  question_with_permission_shape_payload.status = "tui:question_request";
  question_with_permission_shape_payload.permission_request_ids = {"perm_must_not_attach"};
  auto const permission_audit_count = ambiguous_permission_state.permission_audits.size();
  ava::tui::apply_runtime_event(ambiguous_permission_state, provider_event(question_with_permission_shape_payload));
  expect(ambiguous_permission_state.permission_audits.size() == permission_audit_count &&
             std::ranges::all_of(ambiguous_permission_state.pending_tools,
                                 [](ava::tui::PendingToolItem const& pending) { return pending.item.permissions.empty(); }),
         "tui question audit events never create or attach permission state");

  ava::tui::TuiEventState reused_state;
  ava::tui::apply_runtime_event(reused_state, user_message_event(user_payload));
  delta_payload.text = "streamed";
  ava::tui::apply_runtime_event(reused_state, message_update_event(delta_payload));
  ava::tui::apply_runtime_event(reused_state, message_end_event());
  auto next_user_payload = ava::event::MessagePayload{};
  next_user_payload.text = "next";
  ava::tui::apply_runtime_event(reused_state, user_message_event(next_user_payload));
  assistant_final_payload.text = "fresh final";
  ava::tui::apply_runtime_event(reused_state, assistant_message_event(assistant_final_payload));
  expect(reused_state.transcript.size() == 4 && reused_state.transcript[1].text == "streamed" && reused_state.transcript.back().text == "fresh final",
         "tui event state clears streaming index before a reused-state next turn");

  ava::tui::TuiEventState final_state;
  assistant_final_payload.text = "direct final";
  ava::tui::apply_runtime_event(final_state, assistant_message_event(assistant_final_payload));
  expect(final_state.run_status == ava::tui::TuiEventRunStatus::Completed && final_state.transcript.size() == 1 && final_state.transcript[0].label == "ava" &&
             final_state.transcript[0].text == "direct final",
         "tui event state records assistant final events without streaming deltas");
  assistant_final_payload.text = "Use `ava` and **bold**";
  ava::tui::apply_runtime_event(final_state, assistant_message_event(assistant_final_payload));
  expect(final_state.transcript.size() == 2 && ava::tui::to_plain_text(final_state.transcript.back().text_model) == "Use ava and bold",
         "tui event state stores assistant Markdown as frontend-owned semantic Text");

  ava::tui::TuiEventState provider_state;
  auto provider_start_payload = ava::event::ProviderPayload{};
  provider_start_payload.text = R"({"path": "README.md"})";
  provider_start_payload.call_id = "provider_call_1";
  provider_start_payload.tool = "read_file";
  provider_start_payload.status = "tool_call_start";
  ava::tui::apply_runtime_event(provider_state, provider_event(provider_start_payload));
  auto provider_snapshot = ava::tui::event_state_transcript_snapshot(provider_state);
  auto const provider_activity_id = provider_state.activity.empty() ? std::string{} : provider_state.activity[0].id;
  expect(provider_state.activity.size() == 1 && !provider_activity_id.empty() && provider_state.activity[0].label == "read_file" &&
             provider_state.activity[0].detail == "provider is preparing tool call" &&
             provider_state.activity[0].status == ava::tui::ToolTimelineStatus::Running && provider_state.transcript.empty() &&
             provider_state.pending_tools.size() == 1 && provider_snapshot.size() == 1 && provider_snapshot.back().tool &&
             provider_snapshot.back().tool->lifecycle == ava::tui::ToolLifecycleState::ProviderAnnounced,
         "tui event state shows provider tool-call starts as pending announced tool cards");

  auto provider_delta_payload = provider_start_payload;
  provider_delta_payload.status = "tool_call_delta";
  provider_delta_payload.tool.clear();
  provider_delta_payload.text = R"({"path": "README.md", "partial": true})";
  ava::tui::apply_runtime_event(provider_state, provider_event(provider_delta_payload));
  provider_snapshot = ava::tui::event_state_transcript_snapshot(provider_state);
  expect(provider_state.activity.size() == 1 && provider_state.activity[0].id == provider_activity_id && provider_state.activity[0].label == "read_file" &&
             provider_state.activity[0].detail == "streaming tool arguments" && provider_state.activity[0].status == ava::tui::ToolTimelineStatus::Running &&
             provider_state.transcript.empty() && provider_state.pending_tools.size() == 1 &&
             provider_state.pending_tools[0].item.lifecycle == ava::tui::ToolLifecycleState::ArgumentsStreaming &&
             provider_state.pending_tools[0].item.argument_summary.find("\"partial\": true") != std::string::npos && provider_snapshot.size() == 1 &&
             provider_snapshot.back().tool,
         "tui event state keeps provider tool-call deltas on the pending tool card and preserves labels by call id");

  auto provider_end_payload = provider_delta_payload;
  provider_end_payload.status = "tool_call_end";
  provider_end_payload.text = R"({"path": "README.md", "complete": true})";
  ava::tui::apply_runtime_event(provider_state, provider_event(provider_end_payload));
  provider_snapshot = ava::tui::event_state_transcript_snapshot(provider_state);
  expect(provider_state.activity.size() == 1 && provider_state.activity[0].id == provider_activity_id && provider_state.activity[0].label == "read_file" &&
             provider_state.activity[0].detail == "tool call ready" && provider_state.activity[0].status == ava::tui::ToolTimelineStatus::Success &&
             provider_state.transcript.empty() && provider_state.pending_tools.size() == 1 &&
             provider_state.pending_tools[0].item.lifecycle == ava::tui::ToolLifecycleState::ArgumentsComplete && provider_snapshot.size() == 1 &&
             provider_snapshot.back().tool,
         "tui event state marks provider tool-call arguments complete without settling completed transcript history");

  auto provider_execution_start_payload = ava::event::ToolPayload{};
  provider_execution_start_payload.text = "path=README.md";
  provider_execution_start_payload.call_id = "provider_call_1";
  provider_execution_start_payload.tool = "read_file";
  ava::tui::apply_runtime_event(provider_state, tool_start_event(provider_execution_start_payload));
  expect(provider_state.pending_tools.size() == 1 && provider_state.pending_tools[0].item.lifecycle == ava::tui::ToolLifecycleState::ExecutionStarted &&
             provider_state.pending_tools[0].item.argument_summary == "path=README.md",
         "tui event state advances an announced provider tool card into execution by call id");

  auto provider_execution_progress_payload = ava::event::ToolPayload{};
  provider_execution_progress_payload.text = "reading file";
  provider_execution_progress_payload.call_id = "provider_call_1";
  provider_execution_progress_payload.tool = "read_file";
  ava::tui::apply_runtime_event(provider_state, tool_progress_event(provider_execution_progress_payload));
  expect(provider_state.pending_tools.size() == 1 && provider_state.pending_tools[0].item.lifecycle == ava::tui::ToolLifecycleState::Progress &&
             provider_state.pending_tools[0].item.result_summary == "reading file",
         "tui event state records partial tool progress on the pending card");

  auto provider_execution_result_payload = ava::event::ToolPayload{};
  provider_execution_result_payload.text = "read lines 1-10/10";
  provider_execution_result_payload.call_id = "provider_call_1";
  provider_execution_result_payload.tool = "read_file";
  provider_execution_result_payload.status = "success";
  ava::tui::apply_runtime_event(provider_state, tool_result_event(provider_execution_result_payload));
  expect(provider_state.pending_tools.empty() && !provider_state.transcript.empty() && provider_state.transcript.back().tool &&
             provider_state.transcript.back().tool->lifecycle == ava::tui::ToolLifecycleState::Complete &&
             provider_state.transcript.back().tool->argument_summary == "path=README.md",
         "tui event state settles completed tools into immutable transcript history");

  ava::tui::TuiEventState provider_without_id_state;
  auto provider_without_id_payload = ava::event::ProviderPayload{};
  provider_without_id_payload.tool = "grep";
  provider_without_id_payload.status = "tool_call_start";
  ava::tui::apply_runtime_event(provider_without_id_state, provider_event(provider_without_id_payload));
  auto const provider_without_id_activity_id = provider_without_id_state.activity.empty() ? std::string{} : provider_without_id_state.activity[0].id;
  provider_without_id_payload.status = "tool_call_delta";
  ava::tui::apply_runtime_event(provider_without_id_state, provider_event(provider_without_id_payload));
  provider_without_id_payload.status = "tool_call_end";
  ava::tui::apply_runtime_event(provider_without_id_state, provider_event(provider_without_id_payload));
  expect(provider_without_id_state.activity.size() == 1 && !provider_without_id_activity_id.empty() &&
             provider_without_id_state.activity[0].id == provider_without_id_activity_id && provider_without_id_state.activity[0].label == "grep" &&
             provider_without_id_state.activity[0].detail == "tool call ready" &&
             provider_without_id_state.activity[0].status == ava::tui::ToolTimelineStatus::Success && provider_without_id_state.pending_tools.size() == 1 &&
             provider_without_id_state.pending_tools[0].item.lifecycle == ava::tui::ToolLifecycleState::ArgumentsComplete,
         "tui event state coalesces provider tool-call activity and pending cards when provider events omit call ids");

  auto tool_start_payload = ava::event::ToolPayload{};
  tool_start_payload.text = "pwd";
  tool_start_payload.call_id = "call_1";
  tool_start_payload.tool = "bash";
  tool_start_payload.args_json = "{\"command\":\"pwd\"}";
  ava::tui::apply_runtime_event(state, tool_start_event(tool_start_payload));
  expect(state.pending_tools.size() == 1 && state.pending_tools[0].call_id == "call_1" &&
             state.pending_tools[0].item.status == ava::tui::ToolTimelineStatus::Running && state.pending_tools[0].item.name == "bash" &&
             state.pending_tools[0].item.argument_summary == "pwd" && state.pending_tools[0].item.arguments_json == "{\"command\":\"pwd\"}",
         "tui event state tracks started tools by call id");

  auto tool_progress_payload = ava::event::ToolPayload{};
  tool_progress_payload.text = "running pwd";
  tool_progress_payload.call_id = "call_1";
  tool_progress_payload.tool = "bash";
  tool_progress_payload.result_json = "{\"partial\":true}";
  ava::tui::apply_runtime_event(state, tool_progress_event(tool_progress_payload));
  auto tool_snapshot = ava::tui::event_state_transcript_snapshot(state);
  expect(state.pending_tools.size() == 1 && state.pending_tools[0].item.result_summary == "running pwd" &&
             state.pending_tools[0].item.result_json == "{\"partial\":true}" && !tool_snapshot.empty() && tool_snapshot.back().tool &&
             tool_snapshot.back().tool->status == ava::tui::ToolTimelineStatus::Running,
         "tui event state updates pending tool progress and includes it in snapshots");

  auto tool_result_payload = ava::event::ToolPayload{};
  tool_result_payload.text = "ok";
  tool_result_payload.call_id = "call_1";
  tool_result_payload.tool = "bash";
  tool_result_payload.result_json = "{\"ok\":true}";
  tool_result_payload.status = "success";
  ava::tui::apply_runtime_event(state, tool_result_event(tool_result_payload));
  expect(state.pending_tools.empty() && !state.transcript.empty() && state.transcript.back().tool &&
             state.transcript.back().tool->status == ava::tui::ToolTimelineStatus::Success && state.transcript.back().tool->argument_summary == "pwd" &&
             state.transcript.back().tool->result_summary == "ok" && state.transcript.back().tool->arguments_json == "{\"command\":\"pwd\"}" &&
             state.transcript.back().tool->result_json == "{\"ok\":true}",
         "tui event state moves successful tool results into completed transcript items");

  auto write_start_payload = ava::event::ToolPayload{};
  write_start_payload.text = "path=src/main.cpp, content=12 bytes";
  write_start_payload.call_id = "call_write";
  write_start_payload.tool = "write_file";
  ava::tui::apply_runtime_event(state, tool_start_event(write_start_payload));
  auto write_result_payload = ava::event::ToolPayload{};
  write_result_payload.text = "wrote 12 bytes";
  write_result_payload.call_id = "call_write";
  write_result_payload.tool = "write_file";
  write_result_payload.status = "success";
  ava::tui::apply_runtime_event(state, tool_result_event(write_result_payload));
  expect(!state.activity.empty() && state.activity.back().label == "write_file" && state.activity.back().status == ava::tui::ToolTimelineStatus::Success &&
             state.modified_files.size() == 1 && state.modified_files[0].path == "src/main.cpp",
         "tui event state feeds sidebar activity and modified-file summaries from successful mutating tools");

  auto semantic_write_payload = ava::event::ToolPayload{};
  semantic_write_payload.text = "edited file";
  semantic_write_payload.call_id = "call_semantic_write";
  semantic_write_payload.tool = "edit_file";
  semantic_write_payload.status = "success";
  semantic_write_payload.changed_paths = {"src/semantic.cpp"};
  ava::tui::apply_runtime_event(state, tool_result_event(semantic_write_payload));
  expect(std::ranges::any_of(state.modified_files, [](ava::tui::SidebarModifiedFile const& file) { return file.path == "src/semantic.cpp"; }),
         "tui event state prefers semantic changed paths over parsing mutating tool summaries");

  auto tool_error_payload = ava::event::ToolPayload{};
  tool_error_payload.text = "denied";
  tool_error_payload.call_id = "call_2";
  tool_error_payload.tool = "read";
  tool_error_payload.status = "error";
  ava::tui::apply_runtime_event(state, tool_result_event(tool_error_payload));
  expect(state.transcript.back().tool && state.transcript.back().tool->status == ava::tui::ToolTimelineStatus::Error &&
             state.transcript.back().tool->lifecycle == ava::tui::ToolLifecycleState::Error && state.transcript.back().tool->result_summary == "denied",
         "tui event state records errored tool results as error tool cards");

  auto tool_canceled_start_payload = ava::event::ToolPayload{};
  tool_canceled_start_payload.text = "sleep 30";
  tool_canceled_start_payload.call_id = "call_canceled";
  tool_canceled_start_payload.tool = "bash";
  tool_canceled_start_payload.args_json = "{\"command\":\"sleep 30\"}";
  ava::tui::apply_runtime_event(state, tool_start_event(tool_canceled_start_payload));
  auto tool_canceled_payload = ava::event::ToolPayload{};
  tool_canceled_payload.text = "stopped by user";
  tool_canceled_payload.call_id = "call_canceled";
  tool_canceled_payload.tool = "bash";
  tool_canceled_payload.result_json = "{\"tool\":\"bash\",\"canceled\":true}";
  tool_canceled_payload.status = "canceled";
  ava::tui::apply_runtime_event(state, tool_result_event(tool_canceled_payload));
  expect(state.transcript.back().tool && state.transcript.back().tool->status == ava::tui::ToolTimelineStatus::Canceled &&
             state.transcript.back().tool->lifecycle == ava::tui::ToolLifecycleState::Canceled &&
             state.transcript.back().tool->argument_summary == "sleep 30" && state.transcript.back().tool->result_summary == "stopped by user" &&
             state.activity.back().status == ava::tui::ToolTimelineStatus::Canceled,
         "tui event state records canceled tool results as canceled tool cards");

  ava::tui::TuiEventState permission_tool_state;
  ava::event::EventEnvelope permission_tool_requested{.schema_version = 1,
                                                      .event_id = "event_permission_tool_request",
                                                      .timestamp = "2026-04-30T00:00:00Z",
                                                      .session_id = "session_test",
                                                      .run_id = "run_permission_tool",
                                                      .turn_id = "turn_permission_tool",
                                                      .message_id = std::nullopt,
                                                      .request_id = "permission_1",
                                                      .correlation_id = "permission_1",
                                                      .name = "permission_requested",
                                                      .payload_json =
                                                          "{\"resolver_request_id\":\"permission_1\","
                                                          "\"permission_request_id\":\"permreq_push\","
                                                          "\"operation\":\"bash\",\"mode\":\"build\","
                                                          "\"target_path\":\"\",\"command\":\"git push origin main\","
                                                          "\"tool_name\":\"bash\",\"risk\":\"high\","
                                                          "\"reason\":\"command can change external state\"}"};
  ava::tui::apply_control_event_envelope(permission_tool_state, permission_tool_requested);
  ava::event::EventEnvelope permission_tool_replied{.schema_version = 1,
                                                    .event_id = "event_permission_tool_reply",
                                                    .timestamp = "2026-04-30T00:00:01Z",
                                                    .session_id = "session_test",
                                                    .run_id = "run_permission_tool",
                                                    .turn_id = "turn_permission_tool",
                                                    .message_id = std::nullopt,
                                                    .request_id = "permission_1",
                                                    .correlation_id = "permission_1",
                                                    .name = "permission_replied",
                                                    .payload_json =
                                                        "{\"resolver_request_id\":\"permission_1\","
                                                        "\"decision\":\"deny\",\"reason\":\"selected deny\"}"};
  ava::tui::apply_control_event_envelope(permission_tool_state, permission_tool_replied);
  auto permission_tool_payload = ava::event::ToolPayload{};
  permission_tool_payload.text = "permission denied";
  permission_tool_payload.call_id = "call_permission_tool";
  permission_tool_payload.tool = "bash";
  permission_tool_payload.args_json = R"({"command":"git push origin main"})";
  permission_tool_payload.result_json = R"({"tool":"bash","exit_code":126})";
  permission_tool_payload.status = "error";
  permission_tool_payload.permission_request_ids = {"permreq_push"};
  auto permission_tool_result = ava::event::RuntimeEvent{{.timestamp = "2026-04-30T00:00:02Z", .session_id = "session_test"},
                                                         ava::event::ToolResultEvent{.payload = std::move(permission_tool_payload)}};
  auto permission_tool_context = ava::event::EventEnvelopeContext{};
  permission_tool_context.run_id = "run_permission_tool";
  permission_tool_context.turn_id = "turn_permission_tool";
  permission_tool_context.message_id = "message_permission_tool";
  permission_tool_context.request_id = "request_tool";
  permission_tool_context.correlation_id = "call_permission_tool";
  ava::tui::apply_runtime_event(permission_tool_state, permission_tool_result, permission_tool_context);
  auto permission_tool_snapshot = ava::tui::event_state_transcript_snapshot(permission_tool_state);
  auto const permission_tool_render = ava::tui::render_composer(frontend.frontend(), ava::tui::ComposerSnapshot{.mode = "build",
                                                                                                                //.provider = "openai",
                                                                                                                //.model = "gpt-5.5",
                                                                                                                .session_id = "session_test",
                                                                                                                .input = "",
                                                                                                                .status = "ready",
                                                                                                                .transcript = permission_tool_snapshot,
                                                                                                                .width = 90,
                                                                                                                .height = 18});
  expect(permission_tool_snapshot.size() == 1 && permission_tool_snapshot.back().tool && permission_tool_snapshot.back().tool->permissions.size() == 1 &&
             permission_tool_snapshot.back().tool->permissions[0].permission_request_id == "permreq_push" &&
             permission_tool_snapshot.back().tool->permissions[0].decision == "deny" && permission_tool_snapshot.back().tool->permissions[0].risk == "high" &&
             std::ranges::none_of(permission_tool_render,
                                  [](std::string const& line) {
                                    auto const visible = strip_sgr(line);
                                    return visible.find("permreq_push") != std::string::npos || visible.find("permission_1") != std::string::npos;
                                  }),
         "tui EventEnvelope reducer retains linked permission metadata internally while ordinary cards hide resolver identities");

  ava::tui::TuiEventState canonical_session_permission_state;
  auto canonical_permission_requested = permission_tool_requested;
  canonical_permission_requested.event_id = "event_canonical_session_request";
  canonical_permission_requested.request_id = "permission_canonical_session";
  canonical_permission_requested.correlation_id = "permission_canonical_session";
  canonical_permission_requested.payload_json =
      "{\"resolver_request_id\":\"permission_canonical_session\",\"permission_request_id\":\"permreq_canonical_session\","
      "\"tool_name\":\"bash\",\"reason\":\"needs approval\"}";
  ava::tui::apply_control_event_envelope(canonical_session_permission_state, canonical_permission_requested);
  auto canonical_permission_replied = permission_tool_replied;
  canonical_permission_replied.event_id = "event_canonical_session_reply";
  canonical_permission_replied.request_id = "permission_canonical_session";
  canonical_permission_replied.correlation_id = "permission_canonical_session";
  canonical_permission_replied.payload_json =
      "{\"resolver_request_id\":\"permission_canonical_session\",\"decision\":\"allow_session\",\"reason\":\"rpc session grant\"}";
  ava::tui::apply_control_event_envelope(canonical_session_permission_state, canonical_permission_replied);
  auto canonical_permission_payload = ava::event::ToolPayload{};
  canonical_permission_payload.text = "ran";
  canonical_permission_payload.call_id = "call_canonical_session_tool";
  canonical_permission_payload.tool = "bash";
  canonical_permission_payload.status = "success";
  canonical_permission_payload.permission_request_ids = {"permreq_canonical_session"};
  auto canonical_permission_result = ava::event::RuntimeEvent{{.timestamp = "2026-04-30T00:00:02Z", .session_id = "session_test"},
                                                              ava::event::ToolResultEvent{.payload = std::move(canonical_permission_payload)}};
  auto canonical_permission_context = ava::event::EventEnvelopeContext{};
  canonical_permission_context.request_id = "request_canonical_session_tool";
  canonical_permission_context.correlation_id = "call_canonical_session_tool";
  ava::tui::apply_runtime_event(canonical_session_permission_state, canonical_permission_result, canonical_permission_context);
  auto const* canonical_session_item = canonical_session_permission_state.transcript.empty() || !canonical_session_permission_state.transcript.back().tool
                                           ? nullptr
                                           : &*canonical_session_permission_state.transcript.back().tool;
  auto const canonical_session_render =
      canonical_session_item ? tui_test_support::join_visible_lines(ava::tui::detail::render_tool_card(*canonical_session_item, 100, true)) : std::string{};
  auto const canonical_session_copy = canonical_session_item ? ava::tui::detail::tool_card_copy_text(*canonical_session_item) : std::string{};
  expect(canonical_session_item && canonical_session_item->permissions.size() == 1 && canonical_session_item->permissions[0].decision == "allow_session" &&
             canonical_session_render.find("permission:") == std::string::npos && canonical_session_copy.find("permission:") == std::string::npos &&
             canonical_session_render.find("permreq_canonical_session") == std::string::npos &&
             canonical_session_copy.find("permreq_canonical_session") == std::string::npos,
         "tui canonical permission replies remain internal to settled card and copy presentation");

  ava::tui::TuiEventState correlated_tool_state;
  auto correlated_provider_payload = ava::event::ProviderPayload{};
  correlated_provider_payload.text = R"({"pattern":)";
  correlated_provider_payload.call_id.clear();
  correlated_provider_payload.tool = "grep";
  correlated_provider_payload.status = "tool_call_delta";
  auto correlated_provider_delta = ava::event::RuntimeEvent{{.timestamp = "2026-04-30T00:00:00Z", .session_id = "session_test"},
                                                            ava::event::ProviderEvent{.payload = std::move(correlated_provider_payload)}};
  auto correlated_context = ava::event::EventEnvelopeContext{};
  correlated_context.run_id = "run_tool";
  correlated_context.turn_id = "turn_tool";
  correlated_context.message_id = "message_tool";
  correlated_context.request_id = "request_tool";
  correlated_context.correlation_id = "corr_tool";
  ava::tui::apply_runtime_event(correlated_tool_state, correlated_provider_delta, correlated_context);
  expect(correlated_tool_state.pending_tools.size() == 1 && correlated_tool_state.pending_tools[0].call_id == "corr_tool" &&
             correlated_tool_state.pending_tools[0].item.call_id == "corr_tool" && correlated_tool_state.activity.size() == 1 &&
             correlated_tool_state.activity[0].id == "corr_tool",
         "tui typed reducer derives empty provider tool-call ids from correlation context");
  auto correlated_progress_payload = ava::event::ToolPayload{};
  correlated_progress_payload.text = "scanned 10 files";
  correlated_progress_payload.tool = "grep";
  correlated_progress_payload.status = "running";
  correlated_progress_payload.call_id.clear();
  auto correlated_progress = ava::event::RuntimeEvent{{.timestamp = "2026-04-30T00:00:01Z", .session_id = "session_test"},
                                                      ava::event::ToolProgressEvent{.payload = std::move(correlated_progress_payload)}};
  ava::tui::apply_runtime_event(correlated_tool_state, correlated_progress, correlated_context);
  expect(correlated_tool_state.pending_tools.size() == 1 && correlated_tool_state.pending_tools[0].call_id == "corr_tool" &&
             correlated_tool_state.pending_tools[0].request_id == "request_tool" && correlated_tool_state.pending_tools[0].correlation_id == "corr_tool" &&
             correlated_tool_state.pending_tools[0].item.result_summary == "scanned 10 files" && correlated_tool_state.activity.size() == 1 &&
             correlated_tool_state.activity[0].id == "corr_tool",
         "tui typed reducer updates pending tools by runtime request and correlation context");

  auto correlated_result_payload = ava::event::ToolPayload{};
  correlated_result_payload.text = "2 matches";
  correlated_result_payload.tool = "grep";
  correlated_result_payload.args_json = R"({"pattern":"needle"})";
  correlated_result_payload.result_json = R"({"ok":true,"matches":2})";
  correlated_result_payload.status = "success";
  correlated_result_payload.call_id.clear();
  correlated_result_payload.diff = "--- note.txt\n+++ note.txt\n-old\n+new";
  correlated_result_payload.changed_paths = {"logs/output.txt"};
  correlated_result_payload.spill_path = "/tmp/ava-spill/grep.txt";
  correlated_result_payload.diff_truncated = true;
  correlated_result_payload.truncated = true;
  correlated_result_payload.output_bytes = 256;
  correlated_result_payload.total_bytes = 1024;
  correlated_result_payload.output_lines = 4;
  correlated_result_payload.total_lines = 20;
  correlated_result_payload.start_line = 5;
  correlated_result_payload.end_line = 8;
  correlated_result_payload.next_offset_line = 9;
  correlated_result_payload.omitted_bytes = 768;
  correlated_result_payload.omitted_lines = 12;
  correlated_result_payload.visible_matches = 2;
  correlated_result_payload.total_matches = 8;
  auto correlated_result = ava::event::RuntimeEvent{{.timestamp = "2026-04-30T00:00:02Z", .session_id = "session_test"},
                                                    ava::event::ToolResultEvent{.payload = std::move(correlated_result_payload)}};
  ava::tui::apply_runtime_event(correlated_tool_state, correlated_result, correlated_context);
  expect(correlated_tool_state.pending_tools.empty() && correlated_tool_state.transcript.size() == 1 && correlated_tool_state.transcript[0].tool &&
             correlated_tool_state.transcript[0].tool->call_id == "corr_tool" &&
             correlated_tool_state.transcript[0].tool->lifecycle == ava::tui::ToolLifecycleState::Complete &&
             correlated_tool_state.transcript[0].tool->truncated && correlated_tool_state.transcript[0].tool->arguments_json == "{\"pattern\":\"needle\"}" &&
             correlated_tool_state.transcript[0].tool->result_json == "{\"ok\":true,\"matches\":2}" &&
             correlated_tool_state.transcript[0].tool->changed_paths.size() == 1 &&
             correlated_tool_state.transcript[0].tool->changed_paths[0] == "logs/output.txt" &&
             correlated_tool_state.transcript[0].tool->spill_path == "/tmp/ava-spill/grep.txt" &&
             correlated_tool_state.transcript[0].tool->omitted_bytes == 768 && correlated_tool_state.transcript[0].tool->omitted_lines == 12 &&
             correlated_tool_state.transcript[0].tool->next_offset_line == 9 && correlated_tool_state.transcript[0].tool->total_matches == 8 &&
             correlated_tool_state.activity.size() == 1 && correlated_tool_state.activity[0].id == "corr_tool",
         "tui typed reducer settles completed tools with canonical truncation, spill, diff, and accounting metadata");

  auto second_provider_payload = ava::event::ProviderPayload{};
  second_provider_payload.text = R"({"path":)";
  second_provider_payload.call_id.clear();
  second_provider_payload.tool = "read_file";
  second_provider_payload.status = "tool_call_start";
  auto second_provider = ava::event::RuntimeEvent{{.timestamp = "2026-04-30T00:00:03Z", .session_id = "session_test"},
                                                  ava::event::ProviderEvent{.payload = std::move(second_provider_payload)}};
  auto second_context = ava::event::EventEnvelopeContext{};
  second_context.run_id = "run_tool_2";
  second_context.turn_id = "turn_tool_2";
  second_context.message_id = "message_tool_2";
  second_context.request_id = "request_tool_2";
  second_context.correlation_id = "corr_tool_2";
  ava::tui::apply_runtime_event(correlated_tool_state, second_provider, second_context);
  auto second_result_payload = ava::event::ToolPayload{};
  second_result_payload.text = "read ok";
  second_result_payload.tool = "read_file";
  second_result_payload.status = "success";
  second_result_payload.call_id.clear();
  auto second_result = ava::event::RuntimeEvent{{.timestamp = "2026-04-30T00:00:04Z", .session_id = "session_test"},
                                                ava::event::ToolResultEvent{.payload = std::move(second_result_payload)}};
  ava::tui::apply_runtime_event(correlated_tool_state, second_result, second_context);
  expect(correlated_tool_state.pending_tools.empty() && correlated_tool_state.transcript.size() == 2 && correlated_tool_state.transcript[0].tool &&
             correlated_tool_state.transcript[0].tool->call_id == "corr_tool" && correlated_tool_state.transcript[1].tool &&
             correlated_tool_state.transcript[1].tool->call_id == "corr_tool_2" &&
             std::ranges::count_if(correlated_tool_state.activity, [](ava::tui::SidebarActivityItem const& activity) { return activity.id == "corr_tool"; }) ==
                 1 &&
             std::ranges::count_if(correlated_tool_state.activity,
                                   [](ava::tui::SidebarActivityItem const& activity) { return activity.id == "corr_tool_2"; }) == 1,
         "tui typed reducer keeps independent empty-id tool cards separated by correlation context");

  ava::tui::TuiEventState shared_context_distinct_ids_state;
  auto shared_context = ava::event::EventEnvelopeContext{};
  shared_context.run_id = "run_shared";
  shared_context.turn_id = "turn_shared";
  shared_context.message_id = "message_shared";
  shared_context.request_id = "request_shared";
  shared_context.correlation_id = "corr_shared";
  auto shared_provider_a_payload = ava::event::ProviderPayload{};
  shared_provider_a_payload.call_id = "call_shared_a";
  shared_provider_a_payload.tool = "read_file";
  shared_provider_a_payload.status = "tool_call_start";
  auto shared_provider_a = ava::event::RuntimeEvent{{.timestamp = "2026-04-30T00:00:05Z", .session_id = "session_test"},
                                                    ava::event::ProviderEvent{.payload = std::move(shared_provider_a_payload)}};
  ava::tui::apply_runtime_event(shared_context_distinct_ids_state, shared_provider_a, shared_context);
  auto shared_provider_b_payload = ava::event::ProviderPayload{};
  shared_provider_b_payload.call_id = "call_shared_b";
  shared_provider_b_payload.tool = "grep";
  shared_provider_b_payload.status = "tool_call_start";
  auto shared_provider_b = ava::event::RuntimeEvent{{.timestamp = "2026-04-30T00:00:06Z", .session_id = "session_test"},
                                                    ava::event::ProviderEvent{.payload = std::move(shared_provider_b_payload)}};
  ava::tui::apply_runtime_event(shared_context_distinct_ids_state, shared_provider_b, shared_context);
  expect(shared_context_distinct_ids_state.pending_tools.size() == 2 &&
             std::ranges::count_if(shared_context_distinct_ids_state.pending_tools,
                                   [](ava::tui::PendingToolItem const& tool) { return tool.call_id == "call_shared_a"; }) == 1 &&
             std::ranges::count_if(shared_context_distinct_ids_state.pending_tools,
                                   [](ava::tui::PendingToolItem const& tool) { return tool.call_id == "call_shared_b"; }) == 1 &&
             std::ranges::count_if(shared_context_distinct_ids_state.activity,
                                   [](ava::tui::SidebarActivityItem const& activity) { return activity.id == "call_shared_a"; }) == 1 &&
             std::ranges::count_if(shared_context_distinct_ids_state.activity,
                                   [](ava::tui::SidebarActivityItem const& activity) { return activity.id == "call_shared_b"; }) == 1,
         "tui typed reducer keeps distinct provider call ids independent under shared request and correlation");

  auto shared_result_a_payload = ava::event::ToolPayload{};
  shared_result_a_payload.call_id = "call_shared_a";
  shared_result_a_payload.tool = "read_file";
  shared_result_a_payload.status = "success";
  shared_result_a_payload.text = "read a";
  auto shared_result_a = ava::event::RuntimeEvent{{.timestamp = "2026-04-30T00:00:07Z", .session_id = "session_test"},
                                                  ava::event::ToolResultEvent{.payload = std::move(shared_result_a_payload)}};
  ava::tui::apply_runtime_event(shared_context_distinct_ids_state, shared_result_a, shared_context);
  auto shared_result_b_payload = ava::event::ToolPayload{};
  shared_result_b_payload.call_id = "call_shared_b";
  shared_result_b_payload.tool = "grep";
  shared_result_b_payload.status = "success";
  shared_result_b_payload.text = "grep b";
  auto shared_result_b = ava::event::RuntimeEvent{{.timestamp = "2026-04-30T00:00:08Z", .session_id = "session_test"},
                                                  ava::event::ToolResultEvent{.payload = std::move(shared_result_b_payload)}};
  ava::tui::apply_runtime_event(shared_context_distinct_ids_state, shared_result_b, shared_context);
  expect(shared_context_distinct_ids_state.pending_tools.empty() && shared_context_distinct_ids_state.transcript.size() == 2 &&
             shared_context_distinct_ids_state.transcript[0].tool && shared_context_distinct_ids_state.transcript[0].tool->call_id == "call_shared_a" &&
             shared_context_distinct_ids_state.transcript[0].tool->name == "read_file" &&
             shared_context_distinct_ids_state.transcript[0].tool->result_summary == "read a" && shared_context_distinct_ids_state.transcript[1].tool &&
             shared_context_distinct_ids_state.transcript[1].tool->call_id == "call_shared_b" &&
             shared_context_distinct_ids_state.transcript[1].tool->name == "grep" &&
             shared_context_distinct_ids_state.transcript[1].tool->result_summary == "grep b",
         "tui typed reducer settles each shared-context provider tool by exact call id");

  ava::tui::TuiEventState call_id_provenance_state;
  auto fallback_context = ava::event::EventEnvelopeContext{};
  fallback_context.request_id = "request_fallback";
  fallback_context.correlation_id = "call_collision";
  auto fallback_payload = ava::event::ProviderPayload{};
  fallback_payload.tool = "read_file";
  fallback_payload.status = "tool_call_start";
  ava::tui::apply_runtime_event(call_id_provenance_state,
                                ava::event::RuntimeEvent{{.timestamp = "2026-04-30T00:00:09Z", .session_id = "session_test"},
                                                         ava::event::ProviderEvent{.payload = std::move(fallback_payload)}},
                                fallback_context);
  auto authoritative_payload = ava::event::ProviderPayload{};
  authoritative_payload.call_id = "call_collision";
  authoritative_payload.tool = "grep";
  authoritative_payload.status = "tool_call_start";
  ava::tui::apply_runtime_event(call_id_provenance_state,
                                ava::event::RuntimeEvent{{.timestamp = "2026-04-30T00:00:10Z", .session_id = "session_test"},
                                                         ava::event::ProviderEvent{.payload = std::move(authoritative_payload)}},
                                fallback_context);
  expect(call_id_provenance_state.pending_tools.size() == 2 && call_id_provenance_state.pending_tools[0].backend_call_id.empty() &&
             call_id_provenance_state.pending_tools[0].item.name == "read_file" &&
             call_id_provenance_state.pending_tools[1].backend_call_id == "call_collision" && call_id_provenance_state.pending_tools[1].item.name == "grep",
         "tui typed reducer does not treat a correlation fallback as an authoritative backend call id");
  auto authoritative_result_payload = ava::event::ToolPayload{};
  authoritative_result_payload.call_id = "call_collision";
  authoritative_result_payload.tool = "grep";
  authoritative_result_payload.status = "success";
  authoritative_result_payload.text = "grep collision";
  ava::tui::apply_runtime_event(call_id_provenance_state,
                                ava::event::RuntimeEvent{{.timestamp = "2026-04-30T00:00:11Z", .session_id = "session_test"},
                                                         ava::event::ToolResultEvent{.payload = std::move(authoritative_result_payload)}},
                                fallback_context);
  expect(call_id_provenance_state.pending_tools.size() == 1 && call_id_provenance_state.pending_tools[0].backend_call_id.empty() &&
             call_id_provenance_state.pending_tools[0].item.name == "read_file" && call_id_provenance_state.transcript.size() == 1 &&
             call_id_provenance_state.transcript[0].tool && call_id_provenance_state.transcript[0].tool->name == "grep" &&
             call_id_provenance_state.transcript[0].tool->result_summary == "grep collision",
         "tui typed reducer settles a raw call id without mutating a byte-identical correlation fallback card");

  ava::tui::TuiEventState session_start_identity_state;
  apply_session_start(session_start_identity_state, ava::core::Mode::Build, "openai", "gpt-5.5");
  expect(session_start_identity_state.current_mode == ava::core::Mode::Build && session_start_identity_state.current_provider_id == "openai" &&
             session_start_identity_state.current_model_id == "gpt-5.5",
         "tui session start records nonempty provider and model identity");
  apply_session_start(session_start_identity_state, ava::core::Mode::Plan, "", "");
  expect(session_start_identity_state.current_mode == ava::core::Mode::Plan && session_start_identity_state.current_provider_id == "openai" &&
             session_start_identity_state.current_model_id == "gpt-5.5",
         "tui session start updates mode while preserving nonempty provider and model identity");

  auto error_payload = ava::event::ErrorPayload{};
  error_payload.error_message = "provider failed";
  error_payload.error_details = "Provider: provider failed";
  ava::tui::apply_runtime_event(state, error_event(error_payload));
  expect(state.run_status == ava::tui::TuiEventRunStatus::Error && state.error_text == "provider failed" &&
             state.error_details == "Provider: provider failed" && state.transcript.back().label == "error" &&
             state.transcript.back().text == "provider failed",
         "tui event state records runtime errors and exposes error transcript text");

  ava::tui::TuiEventState streaming_error_state;
  auto streaming_delta_payload = ava::event::MessagePayload{};
  streaming_delta_payload.text = "partial answer";
  ava::tui::apply_runtime_event(streaming_error_state, message_update_event(streaming_delta_payload));
  auto streaming_error_payload = ava::event::ErrorPayload{};
  streaming_error_payload.error_message = "provider: curl transport failed";
  streaming_error_payload.error_details = "provider: curl transport failed\noutput: event: response.created\ndata: {...}";
  ava::tui::apply_runtime_event(streaming_error_state, error_event(streaming_error_payload));
  expect(streaming_error_state.transcript.size() == 2 && streaming_error_state.transcript[0].label == "ava" &&
             streaming_error_state.transcript[0].text == "partial answer" && streaming_error_state.transcript[1].label == "error" &&
             streaming_error_state.transcript[1].text == "provider: curl transport failed" &&
             streaming_error_state.transcript[1].text.find("output: event:") == std::string::npos,
         "tui event state commits partial assistant text before compact provider error messages");
  auto const collapsed_streaming_error =
      ava::tui::render_composer(frontend.frontend(), ava::tui::ComposerSnapshot{.mode = "build",
                                                                                //.provider = "openai",
                                                                                //.model = "gpt-5.5",
                                                                                .session_id = "session_test",
                                                                                .input = "",
                                                                                .status = "ready",
                                                                                .transcript = ava::tui::event_state_transcript_snapshot(streaming_error_state),
                                                                                .width = 88,
                                                                                .height = 12,
                                                                                .tool_presentation = ava::tui::ToolPresentation::Compact});
  auto const expanded_streaming_error =
      ava::tui::render_composer(frontend.frontend(), ava::tui::ComposerSnapshot{.mode = "build",
                                                                                //.provider = "openai",
                                                                                //.model = "gpt-5.5",
                                                                                .session_id = "session_test",
                                                                                .input = "",
                                                                                .status = "ready",
                                                                                .transcript = ava::tui::event_state_transcript_snapshot(streaming_error_state),
                                                                                .width = 88,
                                                                                .height = 16,
                                                                                .tool_presentation = ava::tui::ToolPresentation::Expanded});
  expect(std::ranges::any_of(collapsed_streaming_error, [](std::string const& line) { return strip_sgr(line).find("partial answer") != std::string::npos; }) &&
             std::ranges::any_of(collapsed_streaming_error,
                                 [](std::string const& line) { return strip_sgr(line).find("! provider: curl transport failed") != std::string::npos; }) &&
             std::ranges::any_of(collapsed_streaming_error,
                                 [](std::string const& line) { return strip_sgr(line).find("details hidden · /details") != std::string::npos; }) &&
             std::ranges::none_of(collapsed_streaming_error,
                                  [](std::string const& line) { return strip_sgr(line).find("output: event:") != std::string::npos; }) &&
             std::ranges::any_of(expanded_streaming_error,
                                 [](std::string const& line) { return strip_sgr(line).find("output: event: response.created") != std::string::npos; }),
         "tui renders concise collapsed errors while preserving partial assistant text and details-on-demand");

  ava::tui::TuiEventState canceled_state;
  auto canceled_payload = ava::event::ErrorPayload{};
  canceled_payload.error_message = "agent loop canceled";
  canceled_payload.error_details = "Unknown: agent loop canceled";
  ava::tui::apply_runtime_event(canceled_state, error_event(canceled_payload));
  expect(canceled_state.run_status == ava::tui::TuiEventRunStatus::Canceled && canceled_state.error_text == "stopped by user" &&
             canceled_state.transcript.size() == 1 && canceled_state.transcript[0].label == "ava" &&
             canceled_state.transcript[0].text == "stopped by user. Submit a new prompt to continue." && !canceled_state.activity.empty() &&
             canceled_state.activity.back().label == "stopped" && canceled_state.activity.back().status == ava::tui::ToolTimelineStatus::Canceled &&
             canceled_state.activity.back().detail.find("submit a new prompt to continue") != std::string::npos,
         "tui event state presents cooperative cancellation with continuation guidance");

  ava::tui::TuiEventState explicit_canceled_state;
  auto explicit_canceled_payload = ava::event::CancellationPayload{};
  explicit_canceled_payload.text = "stopped by user";
  explicit_canceled_payload.reason = "cancel_requested";
  ava::tui::apply_runtime_event(explicit_canceled_state, cancellation_event(explicit_canceled_payload));
  expect(explicit_canceled_state.run_status == ava::tui::TuiEventRunStatus::Canceled && explicit_canceled_state.transcript.size() == 1 &&
             explicit_canceled_state.transcript[0].text == "stopped by user. Submit a new prompt to continue." &&
             explicit_canceled_state.activity.back().status == ava::tui::ToolTimelineStatus::Canceled &&
             explicit_canceled_state.activity.back().detail.find("cancel_requested") != std::string::npos &&
             explicit_canceled_state.activity.back().detail.find("submit a new prompt to continue") != std::string::npos,
         "tui event state accepts explicit backend canceled lifecycle events");

  ava::tui::TuiEventState lifecycle_state;
  auto compaction_start_payload = ava::event::CompactionPayload{};
  compaction_start_payload.trigger = "auto";
  compaction_start_payload.estimated_tokens = 9000;
  compaction_start_payload.threshold_tokens = 8000;
  ava::tui::apply_runtime_event(lifecycle_state, compaction_start_event(compaction_start_payload));
  expect(lifecycle_state.transcript.empty() && lifecycle_state.activity.size() == 1 && lifecycle_state.activity[0].label == "compaction" &&
             lifecycle_state.activity[0].detail.find("tokens~9000/8000") != std::string::npos,
         "tui event state keeps compaction starts in status activity without inventing transcript content");
  auto retry_payload = ava::event::RetryPayload{};
  retry_payload.trigger = "context_overflow";
  retry_payload.reason = "context_overflow";
  retry_payload.attempt = 1;
  retry_payload.max_attempts = 1;
  retry_payload.delay_ms = 250;
  auto retry_diagnostics = ava::event::RetryDiagnostics{};
  retry_diagnostics.estimated_tokens = 9000;
  retry_diagnostics.threshold_tokens = 8000;
  retry_diagnostics.snapshot_entries = 3;
  retry_diagnostics.current_entries = 4;
  retry_diagnostics.summary_bytes = 321;
  ava::tui::apply_runtime_event(lifecycle_state, retry_event(retry_payload, retry_diagnostics));
  auto retry_tick_payload = ava::event::RetryPayload{};
  retry_tick_payload.trigger = "context_overflow";
  retry_tick_payload.reason = "context_overflow";
  retry_tick_payload.attempt = 1;
  retry_tick_payload.max_attempts = 1;
  retry_tick_payload.delay_ms = 250;
  retry_tick_payload.remaining_ms = 125;
  ava::tui::apply_runtime_event(lifecycle_state, retry_tick_event(retry_tick_payload));
  auto retry_tick_update_payload = retry_tick_payload;
  retry_tick_update_payload.remaining_ms = 25;
  ava::tui::apply_runtime_event(lifecycle_state, retry_tick_event(retry_tick_update_payload));
  expect(std::ranges::any_of(lifecycle_state.activity,
                             [](ava::tui::SidebarActivityItem const& activity) {
                               return activity.label == "retry" && activity.status == ava::tui::ToolTimelineStatus::Running &&
                                      activity.detail.find("remaining=25ms") != std::string::npos;
                             }),
         "tui event state keeps running retry countdown activity while remaining_ms is positive");
  auto compaction_end_payload = ava::event::CompactionPayload{};
  compaction_end_payload.trigger = "context_overflow";
  compaction_end_payload.attempt = 1;
  compaction_end_payload.max_attempts = 2;
  compaction_end_payload.summary_bytes = 1234;
  ava::tui::apply_runtime_event(lifecycle_state, compaction_end_event(compaction_end_payload));
  expect(lifecycle_state.transcript.size() == 3 && lifecycle_state.transcript[0].label == "audit" &&
             lifecycle_state.transcript[0].text.find("retrying after context_overflow") != std::string::npos &&
             lifecycle_state.transcript[0].text.find("attempt 1/1") != std::string::npos &&
             lifecycle_state.transcript[0].text.find("delay=250ms") != std::string::npos &&
             lifecycle_state.transcript[0].text.find("tokens~9000/8000") != std::string::npos &&
             lifecycle_state.transcript[0].text.find("entries=3/4") != std::string::npos &&
             lifecycle_state.transcript[0].text.find("summary=321 bytes") != std::string::npos && lifecycle_state.transcript[1].label == "audit" &&
             lifecycle_state.transcript[1].text.find("retry countdown after context_overflow") != std::string::npos &&
             lifecycle_state.transcript[1].text.find("remaining=25ms") != std::string::npos &&
             lifecycle_state.transcript[1].text.find("remaining=125ms") == std::string::npos && lifecycle_state.transcript[2].label == "compaction" &&
             lifecycle_state.transcript[2].text.find("compaction completed") != std::string::npos &&
             lifecycle_state.transcript[2].text.find("attempt 1/2") != std::string::npos &&
             lifecycle_state.transcript[2].text.find("summary=1234 bytes") != std::string::npos &&
             std::ranges::any_of(lifecycle_state.activity,
                                 [](ava::tui::SidebarActivityItem const& activity) {
                                   return activity.label == "retry" && activity.status == ava::tui::ToolTimelineStatus::Success &&
                                          activity.detail == "retry completed";
                                 }),
         "tui event state renders backend retry, retry countdown, and compaction markers with backend-provided detail");
  auto const compaction_render =
      ava::tui::render_composer(frontend.frontend(), ava::tui::ComposerSnapshot{.mode = "build",
                                                                                //.provider = "openai",
                                                                                //.model = "gpt-5.5",
                                                                                .session_id = "session_test",
                                                                                .input = "",
                                                                                .status = "ready",
                                                                                .transcript = ava::tui::event_state_transcript_snapshot(lifecycle_state),
                                                                                .width = 88,
                                                                                .height = 12});
  expect(std::ranges::any_of(compaction_render,
                             [](std::string const& line) {
                               auto const visible = strip_sgr(line);
                               return visible.find("compact compaction completed") != std::string::npos &&
                                      visible.find("summary=1234 bytes") != std::string::npos;
                             }),
         "tui renders compaction lifecycle entries as dedicated long-session status cards");

  ava::tui::TuiEventState done_state;
  delta_payload.text = "done text";
  ava::tui::apply_runtime_event(done_state, message_update_event(delta_payload));
  auto done_payload = ava::event::CompletionPayload{};
  done_payload.stop_reason = "stop";
  done_payload.provider_iterations = 2;
  done_payload.tool_calls = 1;
  ava::tui::apply_runtime_event(done_state, completion_event(done_payload));
  expect(done_state.run_status == ava::tui::TuiEventRunStatus::Done && done_state.stop_reason == "stop" && done_state.provider_iterations == 2 &&
             done_state.tool_calls == 1 && done_state.pending_assistant_text.empty() && done_state.transcript.size() == 1 &&
             done_state.transcript[0].text == "done text",
         "tui event state records done metadata and commits pending assistant text");

  std::vector<ava::event::RuntimeEvent> live_events;
  {
    auto parity_session_payload = ava::event::SessionPayload{};
    parity_session_payload.mode = ava::core::Mode::Plan;
    parity_session_payload.provider = "openai";
    parity_session_payload.model = "gpt-5.5";
    live_events.emplace_back(ava::event::RuntimeEventMetadata{}, ava::event::SessionStartEvent{.payload = std::move(parity_session_payload)});
  }
  {
    auto payload = ava::event::MessagePayload{};
    payload.text = "inspect";
    live_events.emplace_back(user_message_event(std::move(payload)));
  }
  {
    auto payload = ava::event::RetryPayload{};
    payload.trigger = "context_overflow";
    payload.reason = "context_overflow";
    payload.attempt = 1;
    payload.max_attempts = 1;
    live_events.emplace_back(retry_event(std::move(payload)));
  }
  {
    auto payload = ava::event::RetryPayload{};
    payload.trigger = "context_overflow";
    payload.reason = "context_overflow";
    payload.attempt = 1;
    payload.max_attempts = 1;
    payload.delay_ms = 250;
    payload.remaining_ms = 0;
    live_events.emplace_back(retry_tick_event(std::move(payload)));
  }
  {
    auto payload = ava::event::CompactionPayload{};
    payload.trigger = "context_overflow";
    payload.attempt = 1;
    payload.max_attempts = 2;
    live_events.emplace_back(compaction_start_event(std::move(payload)));
  }
  {
    auto payload = ava::event::CompactionPayload{};
    payload.trigger = "context_overflow";
    payload.attempt = 1;
    payload.max_attempts = 2;
    payload.summary_bytes = 512;
    live_events.emplace_back(compaction_end_event(std::move(payload)));
  }
  {
    auto payload = ava::event::ReasoningPayload{};
    payload.text = "checking";
    live_events.emplace_back(reasoning_delta_event(std::move(payload)));
  }
  {
    auto payload = ava::event::MessagePayload{};
    payload.text = "answer";
    live_events.emplace_back(message_update_event(std::move(payload)));
  }
  live_events.emplace_back(message_end_event());
  {
    auto payload = ava::event::ToolPayload{};
    payload.text = "path=README.md";
    payload.call_id = "call_parity";
    payload.tool = "read_file";
    live_events.emplace_back(tool_start_event(std::move(payload)));
  }
  {
    auto payload = ava::event::ToolPayload{};
    payload.text = "reading";
    payload.call_id = "call_parity";
    payload.tool = "read_file";
    payload.status = "running";
    live_events.emplace_back(tool_progress_event(std::move(payload)));
  }
  {
    auto payload = ava::event::ToolPayload{};
    payload.text = "12 bytes";
    payload.call_id = "call_parity";
    payload.tool = "read_file";
    payload.status = "success";
    live_events.emplace_back(tool_result_event(std::move(payload)));
  }
  {
    auto payload = ava::event::ProviderPayload{};
    payload.text = "question answered: yes";
    payload.status = "tui:question_answer";
    live_events.emplace_back(provider_event(std::move(payload)));
  }
  {
    auto payload = ava::event::CompletionPayload{};
    payload.stop_reason = "stop";
    payload.provider_iterations = 1;
    payload.tool_calls = 1;
    live_events.emplace_back(completion_event(std::move(payload)));
  }

  ava::tui::TuiEventState live_state;
  ava::tui::TuiEventState context_state;
  ava::event::EventEnvelopeContext parity_context;
  parity_context.run_id = "run_1";
  parity_context.turn_id = "turn_1";
  parity_context.message_id = "message_1";
  parity_context.request_id = "request_1";
  parity_context.correlation_id = "correlation_1";
  for (auto const& event : live_events)
  {
    ava::tui::apply_runtime_event(live_state, event);
    ava::tui::apply_runtime_event(context_state, event, parity_context);
  }
  auto const live_render = tui_test_support::plain_lines(
      ava::tui::render_composer(frontend.frontend(), ava::tui::ComposerSnapshot{.mode = "build",
                                                                                //.provider = "openai",
                                                                                //.model = "gpt-5.5",
                                                                                .session_id = "session_test",
                                                                                .input = "",
                                                                                .status = "ready",
                                                                                .transcript = ava::tui::event_state_transcript_snapshot(live_state),
                                                                                .width = 72,
                                                                                .height = 20}));
  auto const context_render = tui_test_support::plain_lines(
      ava::tui::render_composer(frontend.frontend(), ava::tui::ComposerSnapshot{.mode = "build",
                                                                                //.provider = "openai",
                                                                                //.model = "gpt-5.5",
                                                                                .session_id = "session_test",
                                                                                .input = "",
                                                                                .status = "ready",
                                                                                .transcript = ava::tui::event_state_transcript_snapshot(context_state),
                                                                                .width = 72,
                                                                                .height = 20}));
  expect(live_render == context_render && context_state.active_run_id == "run_1" && context_state.active_turn_id == "turn_1" &&
             context_state.active_message_id == "message_1" && context_state.active_request_id == "request_1" &&
             context_state.active_correlation_id == "correlation_1" && context_state.current_mode == ava::core::Mode::Plan &&
             context_state.current_provider_id == "openai" && context_state.current_model_id == "gpt-5.5" && context_state.transcript.back().tool &&
             context_state.transcript.front().label == "you",
         "tui typed direct and context reduction render the same story, track active ids, and inherit session mode/provider/model");

  ava::tui::TuiEventState resolver_state;
  ava::event::EventEnvelope permission_requested{.schema_version = 1,
                                                 .event_id = "event_permission",
                                                 .timestamp = "2026-04-30T00:00:00Z",
                                                 .session_id = "session_test",
                                                 .run_id = "run_prompt",
                                                 .turn_id = "turn_prompt",
                                                 .message_id = std::nullopt,
                                                 .request_id = "request_prompt",
                                                 .correlation_id = "request_prompt",
                                                 .name = "permission_requested",
                                                 .payload_json =
                                                     "{\"resolver_request_id\":\"permission_1\","
                                                     "\"operation\":\"shell.run\",\"mode\":\"build\","
                                                     "\"target_path\":\"\",\"command\":\"pwd\","
                                                     "\"tool_name\":\"bash\",\"reason\":\"needs approval\"}"};
  ava::tui::apply_control_event_envelope(resolver_state, permission_requested);
  expect(resolver_state.transcript.empty() && !resolver_state.activity.empty() && resolver_state.activity[0].label == "permission" &&
             resolver_state.activity[0].detail.find("permission requested: bash pwd") != std::string::npos && resolver_state.active_run_id == "run_prompt",
         "tui EventEnvelope reducer keeps shared permission requests in internal activity without transcript receipts");

  ava::event::EventEnvelope permission_replied{.schema_version = 1,
                                               .event_id = "event_permission_reply",
                                               .timestamp = "2026-04-30T00:00:00Z",
                                               .session_id = "session_test",
                                               .run_id = "run_prompt",
                                               .turn_id = "turn_prompt",
                                               .message_id = std::nullopt,
                                               .request_id = "request_prompt",
                                               .correlation_id = "request_prompt",
                                               .name = "permission_replied",
                                               .payload_json =
                                                   "{\"resolver_request_id\":\"permission_1\","
                                                   "\"decision\":\"deny\"}"};
  ava::tui::apply_control_event_envelope(resolver_state, permission_replied);
  expect(resolver_state.transcript.empty() &&
             std::ranges::any_of(resolver_state.activity,
                                 [](ava::tui::SidebarActivityItem const& item) { return item.label == "permission" && item.detail == "permission replied"; }),
         "tui EventEnvelope reducer keeps shared permission replies in internal activity without transcript receipts");

  ava::event::EventEnvelope question_requested{.schema_version = 1,
                                               .event_id = "event_question",
                                               .timestamp = "2026-04-30T00:00:01Z",
                                               .session_id = "session_test",
                                               .run_id = "run_prompt",
                                               .turn_id = "turn_prompt",
                                               .message_id = std::nullopt,
                                               .request_id = "request_question",
                                               .correlation_id = "request_question",
                                               .name = "question_requested",
                                               .payload_json = "{\"question\":\"Pick an option\"}"};
  ava::tui::apply_control_event_envelope(resolver_state, question_requested);
  expect(resolver_state.transcript.empty() && std::ranges::any_of(resolver_state.activity,
                                                                  [](ava::tui::SidebarActivityItem const& item) {
                                                                    return item.label == "question" && item.detail.find("Pick an option") != std::string::npos;
                                                                  }),
         "tui EventEnvelope reducer keeps shared question requests in internal activity without transcript receipts");

  ava::event::EventEnvelope question_replied{.schema_version = 1,
                                             .event_id = "event_question_reply",
                                             .timestamp = "2026-04-30T00:00:01Z",
                                             .session_id = "session_test",
                                             .run_id = "run_prompt",
                                             .turn_id = "turn_prompt",
                                             .message_id = std::nullopt,
                                             .request_id = "request_question",
                                             .correlation_id = "request_question",
                                             .name = "question_replied",
                                             .payload_json =
                                                 "{\"resolver_request_id\":\"question_1\","
                                                 "\"answer\":\"custom ok\"}"};
  ava::tui::apply_control_event_envelope(resolver_state, question_replied);
  expect(resolver_state.transcript.empty() &&
             std::ranges::any_of(resolver_state.activity,
                                 [](ava::tui::SidebarActivityItem const& item) { return item.label == "question" && item.detail == "question replied"; }),
         "tui EventEnvelope reducer keeps shared question replies in internal activity without transcript receipts");

  ava::event::EventEnvelope steer_queued{.schema_version = 1,
                                         .event_id = "event_steer",
                                         .timestamp = "2026-04-30T00:00:02Z",
                                         .session_id = "session_test",
                                         .run_id = "run_prompt",
                                         .turn_id = "turn_prompt",
                                         .message_id = std::nullopt,
                                         .request_id = "request_steer",
                                         .correlation_id = "request_steer",
                                         .name = "steer_queued",
                                         .payload_json = "{\"message\":\"Use smaller patch groups\"}"};
  ava::tui::apply_control_event_envelope(resolver_state, steer_queued);
  expect(resolver_state.transcript.size() == 1 && resolver_state.transcript.back().label == "audit" &&
             resolver_state.transcript.back().text.find("steer queued") != std::string::npos &&
             resolver_state.transcript.back().text.find("Use smaller patch groups") != std::string::npos && resolver_state.queued_messages.size() == 1 &&
             resolver_state.queued_messages.back().kind == "steer" &&
             std::ranges::any_of(resolver_state.activity,
                                 [](ava::tui::SidebarActivityItem const& item) {
                                   return item.label == "steer" && item.detail.find("Use smaller patch groups") != std::string::npos &&
                                          item.status == ava::tui::ToolTimelineStatus::Running;
                                 }),
         "tui EventEnvelope reducer surfaces backend queued steer events in transcript and sidebar activity");

  ava::event::EventEnvelope follow_up_queued{.schema_version = 1,
                                             .event_id = "event_follow_queue",
                                             .timestamp = "2026-04-30T00:00:02Z",
                                             .session_id = "session_test",
                                             .run_id = "run_prompt",
                                             .turn_id = "turn_prompt",
                                             .message_id = std::nullopt,
                                             .request_id = "request_follow",
                                             .correlation_id = "request_steer",
                                             .name = "follow_up_queued",
                                             .payload_json = "{\"message\":\"Continue after tests\"}"};
  ava::tui::apply_control_event_envelope(resolver_state, follow_up_queued);
  expect(resolver_state.transcript.size() == 2 && resolver_state.transcript.back().text.find("follow-up queued") != std::string::npos &&
             resolver_state.queued_messages.size() == 2 && resolver_state.queued_messages.back().kind == "follow-up" &&
             std::ranges::any_of(resolver_state.activity,
                                 [](ava::tui::SidebarActivityItem const& item) {
                                   return item.label == "follow-up" && item.status == ava::tui::ToolTimelineStatus::Running &&
                                          item.detail.find("Continue after tests") != std::string::npos;
                                 }),
         "tui EventEnvelope reducer records backend queued follow-up events");

  ava::event::EventEnvelope follow_up_started{.schema_version = 1,
                                              .event_id = "event_follow_start",
                                              .timestamp = "2026-04-30T00:00:02Z",
                                              .session_id = "session_test",
                                              .run_id = "run_prompt",
                                              .turn_id = "turn_prompt",
                                              .message_id = std::nullopt,
                                              .request_id = "request_follow",
                                              .correlation_id = "request_follow",
                                              .name = "follow_up_started",
                                              .payload_json = "{\"message\":\"Continue after tests\"}"};
  ava::tui::apply_control_event_envelope(resolver_state, follow_up_started);
  expect(resolver_state.transcript.size() == 3 && resolver_state.transcript.back().text.find("follow-up started") != std::string::npos &&
             resolver_state.queued_messages.size() == 1 &&
             std::ranges::any_of(resolver_state.activity,
                                 [](ava::tui::SidebarActivityItem const& item) {
                                   return item.label == "follow-up" && item.status == ava::tui::ToolTimelineStatus::Running &&
                                          item.detail.find("follow-up started") != std::string::npos;
                                 }),
         "tui EventEnvelope reducer records backend follow-up start events");

  ava::event::EventEnvelope follow_up_skipped{.schema_version = 1,
                                              .event_id = "event_follow_skip",
                                              .timestamp = "2026-04-30T00:00:02Z",
                                              .session_id = "session_test",
                                              .run_id = "run_prompt",
                                              .turn_id = "turn_prompt",
                                              .message_id = std::nullopt,
                                              .request_id = "request_follow",
                                              .correlation_id = "request_follow",
                                              .name = "follow_up_skipped",
                                              .payload_json =
                                                  "{\"message\":\"Continue after tests\","
                                                  "\"reason\":\"canceled\","
                                                  "\"message_truncated\":true,"
                                                  "\"message_bytes\":4096}"};
  ava::tui::apply_control_event_envelope(resolver_state, follow_up_skipped);
  expect(resolver_state.transcript.size() == 4 &&
             resolver_state.transcript.back().text.find("follow-up skipped: run stopped before delivery; submit it again to continue") != std::string::npos &&
             resolver_state.transcript.back().text.find("message truncated from 4096 bytes") != std::string::npos &&
             std::ranges::any_of(resolver_state.activity,
                                 [](ava::tui::SidebarActivityItem const& item) {
                                   return item.label == "follow-up" && item.status == ava::tui::ToolTimelineStatus::Error &&
                                          item.detail.find("Continue after tests") != std::string::npos;
                                 }),
         "tui EventEnvelope reducer records skipped follow-up events with continuation guidance and truncation metadata");

  ava::tui::TuiEventState steering_skip_state;
  ava::event::EventEnvelope steer_skipped{.schema_version = 1,
                                          .event_id = "event_steer_skip",
                                          .timestamp = "2026-04-30T00:00:02Z",
                                          .session_id = "session_test",
                                          .run_id = "run_prompt",
                                          .turn_id = "turn_prompt",
                                          .message_id = std::nullopt,
                                          .request_id = "request_steer",
                                          .correlation_id = "request_steer",
                                          .name = "steer_skipped",
                                          .payload_json =
                                              "{\"message\":\"Use smaller patch groups\","
                                              "\"reason\":\"run_completed_before_safe_point\"}"};
  ava::tui::apply_control_event_envelope(steering_skip_state, steer_skipped);
  expect(steering_skip_state.transcript.size() == 1 &&
             steering_skip_state.transcript.back().text.find("steer skipped: run finished before the next safe steering point") != std::string::npos &&
             steering_skip_state.transcript.back().text.find("Use smaller patch groups") != std::string::npos,
         "tui EventEnvelope reducer explains skipped steering when the turn finishes before a safe point");

  ava::event::EventEnvelope cancel_requested{.schema_version = 1,
                                             .event_id = "event_cancel",
                                             .timestamp = "2026-04-30T00:00:03Z",
                                             .session_id = "session_test",
                                             .run_id = "run_prompt",
                                             .turn_id = "turn_prompt",
                                             .message_id = std::nullopt,
                                             .request_id = "cancel_request",
                                             .correlation_id = "request_prompt",
                                             .name = "cancel_requested",
                                             .payload_json =
                                                 "{\"active_run\":true,\"cleared_steer\":1,"
                                                 "\"cleared_follow_up\":2,\"active_request_id\":\"request_prompt\"}"};
  ava::tui::apply_control_event_envelope(resolver_state, cancel_requested);
  expect(resolver_state.transcript.back().label == "audit" &&
             resolver_state.transcript.back().text.find("cancel requested for active run") != std::string::npos &&
             resolver_state.transcript.back().text.find("steer=1 follow-up=2") != std::string::npos && resolver_state.activity.back().label == "cancel",
         "tui EventEnvelope reducer surfaces backend cancel requests without pretending the run has finished");
}
void test_tui_transcript_projection_and_cap_parity()
{
  ava::tui::TuiEventState state;
  state.transcript.push_back(ava::tui::TranscriptItem{.label = "ava", .text = "completed assistant"});
  state.transcript.push_back(ava::tui::TranscriptItem{.tool = ava::tui::ToolTimelineItem{.status = ava::tui::ToolTimelineStatus::Success,
                                                                                         .name = "read_file",
                                                                                         .result_summary = "completed tool",
                                                                                         .call_id = "completed_call",
                                                                                         .lifecycle = ava::tui::ToolLifecycleState::Complete}});
  state.pending_assistant_text = "streaming assistant";
  state.pending_reasoning_text = "streaming reasoning";
  state.pending_assistant_meta = "Build · GPT-5.5";
  state.pending_tools.push_back(ava::tui::PendingToolItem{.call_id = "pending_call",
                                                          .backend_call_id = "pending_call",
                                                          .request_id = {},
                                                          .correlation_id = {},
                                                          .item = ava::tui::ToolTimelineItem{.status = ava::tui::ToolTimelineStatus::Running,
                                                                                             .name = "bash",
                                                                                             .result_summary = "ordered after assistant",
                                                                                             .call_id = "pending_call",
                                                                                             .lifecycle = ava::tui::ToolLifecycleState::Progress,
                                                                                             .details_visible = true},
                                                          .append_only_stream = true});
  auto projected = ava::tui::event_state_transcript_snapshot(state);

  std::vector<ava::tui::TranscriptItem> submitted;
  submitted.reserve(999);
  for (std::size_t index = 0; index < 999; ++index)
    submitted.push_back(ava::tui::TranscriptItem{.label = "you", .text = "submitted " + std::to_string(index)});
  std::vector<ava::tui::TranscriptItem> destination;
  auto const update = ava::tui::apply_capped_transcript_snapshot(destination, submitted, std::move(projected), 0);
  expect(update.leading_evictions == 3 && update.item_index_shift == -3 && destination.size() == ava::tui::kMaxTranscriptItems &&
             destination.front().text == "submitted 3" && destination[996].text == "completed assistant" && destination[997].tool &&
             destination[997].tool->name == "read_file" && destination[998].text == "streaming assistant" &&
             destination[998].thinking == "streaming reasoning" && destination[998].meta == "Build · GPT-5.5" && destination[999].tool &&
             destination[999].tool->name == "bash" && destination[999].tool->details_visible && destination[999].append_only_stream,
         "moved event-state projection preserves the 1,000-item cap, completed/streaming assistant-tool order, metadata, and tool detail visibility");
}

void test_tui_active_nonblocking_command_lane()
{
  ava::tui::TuiActiveRunQueues queues;
  std::size_t callback_calls = 0;
  std::size_t queue_calls = 0;
  queues.run_nonblocking_command = [&callback_calls](std::string const& submitted) -> std::optional<std::vector<std::string>> {
    if (!submitted.starts_with("/jobs"))
      return std::nullopt;
    ++callback_calls;
    return std::vector<std::string>{"promoted without modal"};
  };
  queues.queue_follow_up = [&queue_calls](std::string) -> ava::core::VoidResult {
    ++queue_calls;
    return {};
  };

  auto disabled_snapshot = ava::tui::ComposerSnapshot{};
  disabled_snapshot.input = "/jobs blocked";
  disabled_snapshot.input_cursor = disabled_snapshot.input.size();
  disabled_snapshot.path_completion_force_active = true;
  disabled_snapshot.file_references = {
      ava::tui::FileReferenceItem{.value = "blocked", .description = {}, .enabled = false, .disabled_reason = "outside workspace"}};
  auto const original_input = disabled_snapshot.input;
  auto const original_cursor = disabled_snapshot.input_cursor;
  auto const blocked = ava::tui::dispatch_tui_active_nonblocking_command_gated(disabled_snapshot, queues, disabled_snapshot.input);
  auto const blocked_callback_calls = callback_calls;
  auto const blocked_queue_calls = queue_calls;

  auto enabled_snapshot = disabled_snapshot;
  enabled_snapshot.file_references.front().enabled = true;
  auto const handled = ava::tui::dispatch_tui_active_nonblocking_command_gated(enabled_snapshot, queues, "/jobs promote job_1");
  auto const unrecognized = ava::tui::dispatch_tui_active_nonblocking_command_gated(enabled_snapshot, queues, "/models");
  ava::tui::TuiActiveRunQueues unavailable;
  auto const missing = ava::tui::dispatch_tui_active_nonblocking_command_gated(enabled_snapshot, unavailable, "/jobs");

  expect(blocked.kind == ava::tui::TuiActiveNonblockingCommandDispatchKind::Blocked && blocked.status == "path disabled: outside workspace" &&
             blocked.output.empty() && blocked_callback_calls == 0 && blocked_queue_calls == 0 && callback_calls == 1 && queue_calls == 0 &&
             disabled_snapshot.input == original_input && disabled_snapshot.input_cursor == original_cursor &&
             handled.kind == ava::tui::TuiActiveNonblockingCommandDispatchKind::Handled &&
             handled.output == std::vector<std::string>{"promoted without modal"} &&
             unrecognized.kind == ava::tui::TuiActiveNonblockingCommandDispatchKind::Unrecognized &&
             missing.kind == ava::tui::TuiActiveNonblockingCommandDispatchKind::Unrecognized,
         "tui active route seam blocks a disabled forced path before callback or queue mutation, while enabled and unrecognized commands retain dispatch "
         "classification");
}

void test_tui_selector_authority_dispatch_paints_before_callback()
{
  ava::tui::ComposerSnapshot snapshot;
  snapshot.mode = "build";
  //  snapshot.provider = "openai";
  //  snapshot.model = "gpt-5.5";
  snapshot.session_id = "session_current";
  snapshot.input = "draft survives";
  snapshot.status = "selecting";
  snapshot.select_list = ava::tui::SelectListView{.title = "Select model",
                                                  .subtitle = {},
                                                  .items = {ava::tui::SelectListItemView{.value = "openai/gpt-5.5",
                                                                                         .label = "GPT-5.5",
                                                                                         .description = {},
                                                                                         .group = "Models",
                                                                                         .detail = {},
                                                                                         .badge = {},
                                                                                         .current = false,
                                                                                         .enabled = true,
                                                                                         .disabled_reason = {}}},
                                                  .selected_item_index = 0,
                                                  .query = {},
                                                  .placeholder = "Search",
                                                  .empty_text = "No matches",
                                                  .footer_hint = "Esc cancel"};
  snapshot.input_cursor = 5;
  auto const original_input = snapshot.input;
  auto const original_cursor = snapshot.input_cursor;
  std::mutex mutex;
  std::condition_variable ready;
  std::condition_variable release;
  bool callback_started = false;
  bool callback_released = false;
  bool dispatch_succeeded = false;
  std::vector<std::string> events;

  std::thread dispatch([&]() {
    auto result = ava::tui::dispatch_tui_selector_authority(
        snapshot, "switching model…",
        [&]() {
          events.push_back("render");
          return true;
        },
        [&]() -> ava::core::Result<std::string> {
          std::unique_lock lock(mutex);
          events.push_back("callback");
          callback_started = true;
          ready.notify_one();
          release.wait(lock, [&]() { return callback_released; });
          return "model already selected";
        });
    dispatch_succeeded = result.has_value();
  });
  {
    std::unique_lock lock(mutex);
    ready.wait(lock, [&]() { return callback_started; });
    expect(events == std::vector<std::string>({"render", "callback"}) && !snapshot.select_list && snapshot.status == "switching model…",
           "selector authority dispatch clears and paints truthful pending status before a blocking callback starts");
    callback_released = true;
    release.notify_one();
  }
  dispatch.join();
  expect(dispatch_succeeded && snapshot.input == original_input && snapshot.input_cursor == original_cursor,
         "successful selector authority dispatch preserves the underlying draft and cursor until authoritative state applies");

  snapshot.select_list = ava::tui::SelectListView{};
  snapshot.select_list->title = "Select session";
  auto failed = ava::tui::dispatch_tui_selector_authority(
      snapshot, "opening session…", []() { return true; },
      []() -> ava::core::Result<std::string> { return std::unexpected(ava::core::Error(ava::core::ErrorCategory::NotFound, "session unavailable")); });
  expect(!failed && !snapshot.select_list && snapshot.status.find("session unavailable") != std::string::npos && snapshot.input == original_input &&
             snapshot.input_cursor == original_cursor,
         "failed selector authority dispatch keeps modal cleared, reports authority failure, and retains draft and cursor");
}

void test_tui_mixed_runtime_event_queue_preserves_order_and_context()
{
  ava::tui::RuntimeEventQueue queue;
  auto first_payload_value = ava::event::MessagePayload{};
  first_payload_value.text = "typed {not serialized}";
  auto first =
      ava::event::RuntimeEvent{{.timestamp = "first", .session_id = "session"}, ava::event::MessageUpdateEvent{.payload = std::move(first_payload_value)}};
  auto second_payload_value = ava::event::ToolPayload{};
  second_payload_value.text = "progress";
  second_payload_value.call_id = "call";
  second_payload_value.tool = "bash";
  auto second =
      ava::event::RuntimeEvent{{.timestamp = "second", .session_id = "session"}, ava::event::ToolProgressEvent{.payload = std::move(second_payload_value)}};
  auto first_context = ava::event::EventEnvelopeContext{};
  first_context.run_id = "run_1";
  first_context.turn_id = "turn_1";
  first_context.message_id = "message_1";
  first_context.request_id = "request_1";
  first_context.correlation_id = "correlation_1";
  auto second_context = ava::event::EventEnvelopeContext{};
  second_context.run_id = "run_2";
  second_context.turn_id = "turn_2";
  second_context.message_id = "message_2";
  second_context.request_id = "request_2";
  second_context.correlation_id = "correlation_2";
  expect(queue.enqueue(first, first_context).has_value(), "mixed TUI event queue accepts a typed runtime event");
  auto control_sink = queue.envelope_sink();
  auto control_event = ava::event::EventEnvelope{};
  control_event.schema_version = 1;
  control_event.event_id = "control";
  control_event.timestamp = "between";
  control_event.session_id = "session";
  control_event.request_id = "permission";
  control_event.correlation_id = "permission";
  control_event.name = "permission_replied";
  control_event.payload_json = R"({"decision":"allow"})";
  expect(control_sink(control_event).has_value(), "mixed TUI event queue accepts a control envelope");
  expect(queue.enqueue(second, second_context).has_value(), "mixed TUI event queue accepts a second typed runtime event");

  auto drained = queue.drain();
  auto const* first_queued = drained.size() == 3 ? std::get_if<ava::tui::QueuedRuntimeEvent>(&drained[0]) : nullptr;
  auto const* control = drained.size() == 3 ? std::get_if<ava::event::EventEnvelope>(&drained[1]) : nullptr;
  auto const* second_queued = drained.size() == 3 ? std::get_if<ava::tui::QueuedRuntimeEvent>(&drained[2]) : nullptr;
  auto const* first_payload = first_queued ? std::get_if<ava::event::MessageUpdateEvent>(&first_queued->event.payload()) : nullptr;
  auto const* second_payload = second_queued ? std::get_if<ava::event::ToolProgressEvent>(&second_queued->event.payload()) : nullptr;
  expect(queue.received_any() && queue.drain().empty() && first_payload && first_payload->payload.text == "typed {not serialized}" && control &&
             control->event_id == "control" && second_payload && second_payload->payload.call_id == "call" && first_queued->context.run_id == "run_1" &&
             first_queued->context.turn_id == "turn_1" && first_queued->context.message_id == "message_1" && first_queued->context.request_id == "request_1" &&
             first_queued->context.correlation_id == "correlation_1" && second_queued->context.run_id == "run_2" &&
             second_queued->context.turn_id == "turn_2" && second_queued->context.message_id == "message_2" &&
             second_queued->context.request_id == "request_2" && second_queued->context.correlation_id == "correlation_2",
         "mixed TUI event queue drains typed runtime records and controls in arrival order with exact un-serialized context");
}

void test_tui_private_subagent_launch_queue_and_exact_reducer()
{
  auto task_event = [](bool result = false) {
    ava::event::ToolPayload payload;
    payload.call_id = "duplicate-call";
    payload.tool = "task";
    payload.args_json = R"({"description":"live task","subagent_type":"general"})";
    if (result)
    {
      payload.status = "success";
      payload.result_json = R"({"tool":"task","ok":true,"description":"live task","state":"completed"})";
      return ava::event::RuntimeEvent{{}, ava::event::ToolResultEvent{.payload = std::move(payload)}};
    }
    return ava::event::RuntimeEvent{{}, ava::event::ToolStartEvent{.payload = std::move(payload)}};
  };
  auto context = ava::event::EventEnvelopeContext{};
  context.request_id = "request-new";
  context.correlation_id = "correlation-new";
  auto notification =
      ava::agent::SubagentLaunchNotification{.tool_call_id = "duplicate-call",
                                             .request_id = "request-new",
                                             .correlation_id = "correlation-new",
                                             .display = ava::agent::SubagentLaunchDisplay::normalized("GPT-5.6 Terra", std::string_view("high"))};

  ava::tui::RuntimeEventQueue queue;
  expect(queue.enqueue(task_event(), context).has_value(), "private launch queue accepts the public task Running event");
  queue.subagent_launch_sink()(notification);
  expect(queue.enqueue(task_event(true), context).has_value(), "private launch queue accepts the later public task result");
  auto drained = queue.drain();
  expect(drained.size() == 3 && std::holds_alternative<ava::tui::QueuedRuntimeEvent>(drained[0]) &&
             std::holds_alternative<ava::agent::SubagentLaunchNotification>(drained[1]) && std::holds_alternative<ava::tui::QueuedRuntimeEvent>(drained[2]) &&
             queue.received_any(),
         "private launch queue preserves Running, launch metadata, and result order");

  ava::tui::TuiEventState state;
  for (auto const& queued : drained)
  {
    std::visit(
        [&](auto const& value) {
          using Value = std::remove_cvref_t<decltype(value)>;
          if constexpr (std::same_as<Value, ava::tui::QueuedRuntimeEvent>)
            ava::tui::apply_runtime_event(state, value.event, value.context);
          else if constexpr (std::same_as<Value, ava::agent::SubagentLaunchNotification>)
            ava::tui::apply_subagent_launch_notification(state, value);
        },
        queued);
  }
  expect(state.pending_tools.empty() && state.transcript.size() == 1 && state.transcript.front().tool &&
             state.transcript.front().tool->subagent_launch_display &&
             state.transcript.front().tool->subagent_launch_display->model_display_name() == "GPT-5.6 Terra" &&
             state.transcript.front().tool->subagent_launch_display->reasoning_label() == "high",
         "one drain batch associates exact private launch metadata and preserves it through task settlement");

  ava::tui::TuiEventState historical;
  ava::tui::apply_runtime_event(historical, task_event(), context);
  ava::tui::apply_runtime_event(historical, task_event(true), context);
  expect(historical.transcript.size() == 1 && historical.transcript.front().tool && !historical.transcript.front().tool->subagent_launch_display,
         "historical task replay without a private notification has no launch metadata");

  auto make_pending = [](std::string request, std::string correlation, std::string name = "task") {
    return ava::tui::PendingToolItem{.call_id = "display-fallback",
                                     .backend_call_id = "duplicate-call",
                                     .request_id = std::move(request),
                                     .correlation_id = std::move(correlation),
                                     .item = ava::tui::ToolTimelineItem{.status = ava::tui::ToolTimelineStatus::Running, .name = std::move(name)}};
  };
  ava::tui::TuiEventState duplicates;
  duplicates.pending_tools.push_back(make_pending("request-old", "correlation-old"));
  duplicates.pending_tools.push_back(make_pending("request-new", "correlation-new"));
  ava::tui::apply_subagent_launch_notification(duplicates, notification);
  expect(!duplicates.pending_tools[0].item.subagent_launch_display && duplicates.pending_tools[1].item.subagent_launch_display,
         "duplicate backend call ids associate only through the exact request and correlation context");

  auto exact_rejection = [&](ava::agent::SubagentLaunchNotification rejected) {
    ava::tui::TuiEventState candidate;
    candidate.pending_tools.push_back(make_pending("request-new", "correlation-new"));
    ava::tui::apply_subagent_launch_notification(candidate, rejected);
    return candidate.pending_tools.front().item.subagent_launch_display.has_value();
  };
  auto wrong_request = notification;
  wrong_request.request_id = "request-wrong";
  auto wrong_correlation = notification;
  wrong_correlation.correlation_id = "correlation-wrong";
  auto empty_request = notification;
  empty_request.request_id.clear();
  auto empty_correlation = notification;
  empty_correlation.correlation_id.clear();
  auto wrong_call = notification;
  wrong_call.tool_call_id = "call-wrong";
  expect(!exact_rejection(wrong_request) && !exact_rejection(wrong_correlation) && !exact_rejection(empty_request) && !exact_rejection(empty_correlation) &&
             !exact_rejection(wrong_call),
         "private launch reducer rejects wrong or missing request, correlation, and backend call identities without fallback");

  ava::tui::TuiEventState non_task;
  non_task.pending_tools.push_back(make_pending("request-new", "correlation-new", "job"));
  ava::tui::apply_subagent_launch_notification(non_task, notification);
  ava::tui::TuiEventState ambiguous;
  ambiguous.pending_tools.push_back(make_pending("request-new", "correlation-new"));
  ambiguous.pending_tools.push_back(make_pending("request-new", "correlation-new"));
  ava::tui::apply_subagent_launch_notification(ambiguous, notification);
  expect(!non_task.pending_tools.front().item.subagent_launch_display &&
             std::ranges::none_of(ambiguous.pending_tools, [](auto const& pending) { return pending.item.subagent_launch_display.has_value(); }),
         "private launch reducer rejects non-task and ambiguous exact candidates");

  ava::tui::TuiEventState unmatched;
  ava::tui::apply_subagent_launch_notification(unmatched, notification);
  unmatched.pending_tools.push_back(make_pending("request-new", "correlation-new"));
  expect(!unmatched.pending_tools.front().item.subagent_launch_display, "unmatched private launch metadata is discarded rather than cached");

  ava::tui::RuntimeEventQueue teardown_queue;
  teardown_queue.subagent_launch_sink()(notification);
  teardown_queue.discard();
  expect(teardown_queue.drain().empty() && !teardown_queue.received_any(),
         "active-run or session teardown discards queued private launch metadata with ordinary events");
}

void test_tui_typed_runtime_inherits_session_identity_fields()
{
  ava::tui::TuiEventState state;
  auto context = ava::event::EventEnvelopeContext{};
  context.run_id = "run_typed";
  context.turn_id = "turn_typed";
  context.message_id = "message_typed";
  auto session_payload = ava::event::SessionPayload{};
  session_payload.mode = ava::core::Mode::Plan;
  session_payload.provider = "openai";
  session_payload.model = "model-x";
  ava::tui::apply_runtime_event(
      state, ava::event::RuntimeEvent{{.timestamp = {}, .session_id = "session_test"}, ava::event::SessionStartEvent{.payload = std::move(session_payload)}},
      context);
  auto update_payload = ava::event::MessagePayload{};
  update_payload.text = "inherited context";
  ava::tui::apply_runtime_event(
      state, ava::event::RuntimeEvent{{.timestamp = {}, .session_id = "session_test"}, ava::event::MessageUpdateEvent{.payload = std::move(update_payload)}},
      context);
  ava::tui::apply_runtime_event(
      state, ava::event::RuntimeEvent{{.timestamp = {}, .session_id = "session_test"}, ava::event::MessageEndEvent{.payload = ava::event::MessagePayload{}}},
      context);

  expect(state.current_mode == ava::core::Mode::Plan && state.current_provider_id == "openai" && state.current_model_id == "model-x" &&
             state.active_run_id == "run_typed" && state.active_turn_id == "turn_typed" && state.active_message_id == "message_typed" &&
             state.transcript.size() == 1 && state.transcript.front().meta.starts_with("Plan"),
         "TUI typed runtime reduction inherits Plan/openai/model-x from session_start while later payloads leave identity unchanged");
}

}  // namespace

void test_tui_retry_activity_settles_and_restores_contextual_hint()
{
  auto assert_retry_chrome = [](ava::tui::TuiEventState const& state, std::string_view expected_chrome, std::string const& case_name) {
    auto const lines = render_processing_with_event_activity(state);
    expect(visible_contains(lines, expected_chrome) && visible_contains(lines, "Esc stop") && !visible_contains(lines, "type a follow-up"), case_name);
  };
  auto assert_normal_hint = [](ava::tui::TuiEventState const& state, std::string const& case_name) {
    auto const lines = render_processing_with_event_activity(state);
    expect(visible_contains(lines, "Esc stop · type a follow-up") && !visible_contains(lines, "retry attempt") && !visible_contains(lines, "retry 0ms") &&
               !visible_contains(lines, "retry countdown"),
           case_name);
  };

  ava::tui::TuiEventState running_state;
  ava::tui::apply_runtime_event(running_state, retry_event(sample_retry_payload()));
  auto const* running_retry = find_retry_activity(running_state);
  expect(running_retry && running_retry->status == ava::tui::ToolTimelineStatus::Running &&
             running_retry->detail.find("retrying after rate_limit") != std::string::npos && count_running_retry_activities(running_state) == 1 &&
             running_state.transcript.size() == 1 && running_state.transcript[0].label == "audit",
         "tui event state keeps running retry activity visible after RetryEvent");
  assert_retry_chrome(running_state, "Esc stop · retry attempt 2/5", "tui event-state-to-render surfaces running retry chrome from typed RetryEvent activity");

  ava::tui::TuiEventState countdown_state = running_state;
  ava::tui::apply_runtime_event(countdown_state, retry_tick_event(sample_retry_payload(1500)));
  auto const* countdown_retry = find_retry_activity(countdown_state);
  expect(countdown_retry && countdown_retry->status == ava::tui::ToolTimelineStatus::Running &&
             countdown_retry->detail.find("remaining=1500ms") != std::string::npos && count_running_retry_activities(countdown_state) == 1 &&
             countdown_state.transcript.size() == 2 && countdown_state.transcript[1].text.find("remaining=1500ms") != std::string::npos,
         "tui event state updates running retry countdown activity and preserves audit transcript text");
  assert_retry_chrome(countdown_state, "Esc stop · retry 1500ms",
                      "tui event-state-to-render surfaces running retry countdown chrome from typed RetryTick activity");

  ava::tui::apply_runtime_event(countdown_state, retry_tick_event(sample_retry_payload(0)));
  auto const* zero_retry = find_retry_activity(countdown_state);
  expect(zero_retry && zero_retry->status == ava::tui::ToolTimelineStatus::Success && zero_retry->detail == "retry completed" &&
             count_running_retry_activities(countdown_state) == 0 && countdown_state.transcript.size() == 2 &&
             countdown_state.transcript[1].text.find("remaining=0ms") != std::string::npos &&
             countdown_state.transcript[0].text.find("retrying after rate_limit") != std::string::npos,
         "tui event state settles retry activity on remaining_ms==0 while preserving retry transcript audits");
  assert_normal_hint(countdown_state, "tui event-state-to-render drops retry chrome after remaining_ms==0 and restores the normal contextual hint");

  auto settle_with_progress = [&](auto apply_progress, std::string_view progress_name) {
    ava::tui::TuiEventState state;
    state.activity.push_back(
        ava::tui::SidebarActivityItem{.id = "unrelated:read", .label = "read_file", .detail = "already done", .status = ava::tui::ToolTimelineStatus::Success});
    ava::tui::apply_runtime_event(state, retry_event(sample_retry_payload()));
    ava::tui::apply_runtime_event(state, retry_tick_event(sample_retry_payload(900)));
    auto const before_lines = render_processing_with_event_activity(state);
    expect(visible_contains(before_lines, "retry 900ms"), std::string("precondition: running retry chrome visible before ") + std::string(progress_name));
    apply_progress(state);
    auto const* retry = find_retry_activity(state);
    auto const unrelated = std::ranges::find_if(state.activity, [](ava::tui::SidebarActivityItem const& activity) { return activity.id == "unrelated:read"; });
    expect(retry && retry->status == ava::tui::ToolTimelineStatus::Success && retry->detail == "retry completed" &&
               count_running_retry_activities(state) == 0 && unrelated != state.activity.end() && unrelated->label == "read_file" &&
               unrelated->detail == "already done" && unrelated->status == ava::tui::ToolTimelineStatus::Success,
           std::string("tui event state settles running retry on ") + std::string(progress_name) + " without changing unrelated activity");
    assert_normal_hint(state, std::string("tui event-state-to-render restores normal contextual hint after ") + std::string(progress_name));
  };

  settle_with_progress(
      [](ava::tui::TuiEventState& state) {
        auto payload = ava::event::MessagePayload{};
        payload.text = "hello";
        ava::tui::apply_runtime_event(state, message_update_event(std::move(payload)));
      },
      "assistant message progress");

  settle_with_progress(
      [](ava::tui::TuiEventState& state) {
        auto payload = ava::event::ProviderPayload{};
        payload.status = "tool_call_start";
        payload.tool = "read_file";
        payload.call_id = "provider_retry_settle";
        ava::tui::apply_runtime_event(state, provider_event(std::move(payload)));
      },
      "provider progress");

  settle_with_progress(
      [](ava::tui::TuiEventState& state) {
        auto payload = ava::event::ToolPayload{};
        payload.call_id = "call_retry_settle";
        payload.tool = "read_file";
        payload.text = "path=README.md";
        ava::tui::apply_runtime_event(state, tool_start_event(std::move(payload)));
      },
      "tool progress");

  settle_with_progress(
      [](ava::tui::TuiEventState& state) {
        auto payload = ava::event::CompletionPayload{};
        payload.stop_reason = "stop";
        ava::tui::apply_runtime_event(state, completion_event(std::move(payload)));
      },
      "completion progress");

  ava::tui::TuiEventState canceled_state;
  ava::tui::apply_runtime_event(canceled_state, retry_event(sample_retry_payload()));
  ava::tui::apply_runtime_event(canceled_state, retry_tick_event(sample_retry_payload(500)));
  ava::tui::apply_runtime_event(canceled_state, cancellation_event(ava::event::CancellationPayload{}));
  auto const* canceled_retry = find_retry_activity(canceled_state);
  expect(canceled_retry && canceled_retry->status == ava::tui::ToolTimelineStatus::Canceled && canceled_retry->detail == "retry canceled" &&
             canceled_state.run_status == ava::tui::TuiEventRunStatus::Canceled,
         "tui event state settles running retry as Canceled on cancellation");
  assert_normal_hint(canceled_state, "tui event-state-to-render removes retry chrome after cancellation settlement");

  ava::tui::TuiEventState error_state;
  ava::tui::apply_runtime_event(error_state, retry_event(sample_retry_payload()));
  ava::tui::apply_runtime_event(error_state, retry_tick_event(sample_retry_payload(500)));
  auto error_payload = ava::event::ErrorPayload{};
  error_payload.error_message = "provider unavailable";
  error_payload.text = "provider unavailable";
  ava::tui::apply_runtime_event(error_state, error_event(std::move(error_payload)));
  auto const* failed_retry = find_retry_activity(error_state);
  expect(failed_retry && failed_retry->status == ava::tui::ToolTimelineStatus::Error && failed_retry->detail == "retry failed" &&
             error_state.run_status == ava::tui::TuiEventRunStatus::Error,
         "tui event state settles running retry as Error on terminal non-cancel failure");
  assert_normal_hint(error_state, "tui event-state-to-render removes retry chrome after error settlement");

  ava::tui::TuiEventState reactivate_state;
  ava::tui::apply_runtime_event(reactivate_state, retry_event(sample_retry_payload()));
  ava::tui::apply_runtime_event(reactivate_state, retry_tick_event(sample_retry_payload(0)));
  auto const* settled = find_retry_activity(reactivate_state);
  expect(settled && settled->status == ava::tui::ToolTimelineStatus::Success && count_running_retry_activities(reactivate_state) == 0,
         "precondition: zero remaining settles retry before later reactivation");
  auto second_retry = sample_retry_payload();
  second_retry.attempt = 3;
  ava::tui::apply_runtime_event(reactivate_state, retry_event(std::move(second_retry)));
  auto const* reactivated = find_retry_activity(reactivate_state);
  expect(reactivated && reactivated->status == ava::tui::ToolTimelineStatus::Running && reactivated->detail.find("attempt 3/5") != std::string::npos &&
             count_running_retry_activities(reactivate_state) == 1 && reactivate_state.transcript.size() >= 2 &&
             std::ranges::count_if(reactivate_state.transcript, [](ava::tui::TranscriptItem const& item) { return item.label == "audit"; }) >= 2,
         "tui event state lets a later RetryEvent reactivate settled retry activity while preserving prior transcript audits");
  assert_retry_chrome(reactivate_state, "Esc stop · retry attempt 3/5",
                      "tui event-state-to-render restores retry chrome after a later typed RetryEvent reactivation");

  // Reason/trigger identity changes must settle the prior Running row before the new retry owns chrome.
  ava::tui::TuiEventState reason_change_state;
  reason_change_state.activity.push_back(
      ava::tui::SidebarActivityItem{.id = "unrelated:write", .label = "write_file", .detail = "kept", .status = ava::tui::ToolTimelineStatus::Success});
  ava::tui::apply_runtime_event(reason_change_state, retry_event(retry_payload_with_reason("rate_limit", "rate_limit", 1, 1000, 1000)));
  expect(count_running_retry_activities(reason_change_state) == 1 && count_retry_activities(reason_change_state) == 1,
         "precondition: first reason/trigger retry is the sole Running retry row");
  ava::tui::apply_runtime_event(reason_change_state, retry_event(retry_payload_with_reason("server_error", "provider_transport", 2, 2000, 2000)));
  auto const* superseded_rate_limit = find_retry_activity_by_id(reason_change_state, "retry:rate_limit");
  auto const* active_server_error = find_retry_activity_by_id(reason_change_state, "retry:server_error");
  auto const unrelated_write =
      std::ranges::find_if(reason_change_state.activity, [](ava::tui::SidebarActivityItem const& activity) { return activity.id == "unrelated:write"; });
  expect(superseded_rate_limit && superseded_rate_limit->status == ava::tui::ToolTimelineStatus::Success &&
             superseded_rate_limit->detail == "retry superseded" && active_server_error &&
             active_server_error->status == ava::tui::ToolTimelineStatus::Running && active_server_error->detail.find("server_error") != std::string::npos &&
             count_running_retry_activities(reason_change_state) == 1 && count_retry_activities(reason_change_state) == 2 &&
             reason_change_state.transcript.size() == 2 && reason_change_state.transcript[0].text.find("rate_limit") != std::string::npos &&
             reason_change_state.transcript[1].text.find("server_error") != std::string::npos && unrelated_write != reason_change_state.activity.end() &&
             unrelated_write->detail == "kept" && unrelated_write->status == ava::tui::ToolTimelineStatus::Success,
         "tui event state settles prior reason/trigger retry as superseded and keeps exactly one Running retry plus audit history");
  assert_retry_chrome(reason_change_state, "Esc stop · retry attempt 2/5",
                      "tui event-state-to-render surfaces only the current reason/trigger retry chrome after identity change");

  // A/B then tick0(B) must not leave A Running even if ids differ.
  ava::tui::TuiEventState ab_tick0_state;
  ava::tui::apply_runtime_event(ab_tick0_state, retry_event(retry_payload_with_reason("rate_limit", "rate_limit", 1, 1000, 1000)));
  ava::tui::apply_runtime_event(ab_tick0_state, retry_event(retry_payload_with_reason("timeout", "provider_transport", 2, 500, 500)));
  ava::tui::apply_runtime_event(ab_tick0_state, retry_tick_event(retry_payload_with_reason("timeout", "provider_transport", 2, 500, 250)));
  expect(count_running_retry_activities(ab_tick0_state) == 1 && find_retry_activity_by_id(ab_tick0_state, "retry:timeout") &&
             find_retry_activity_by_id(ab_tick0_state, "retry:timeout")->status == ava::tui::ToolTimelineStatus::Running,
         "precondition: positive remaining tick keeps only the current retry Running");
  assert_retry_chrome(ab_tick0_state, "Esc stop · retry 250ms",
                      "tui event-state-to-render keeps positive remaining countdown chrome on the active retry identity");
  ava::tui::apply_runtime_event(ab_tick0_state, retry_tick_event(retry_payload_with_reason("timeout", "provider_transport", 2, 500, 0)));
  auto const* settled_a = find_retry_activity_by_id(ab_tick0_state, "retry:rate_limit");
  auto const* settled_b = find_retry_activity_by_id(ab_tick0_state, "retry:timeout");
  expect(count_running_retry_activities(ab_tick0_state) == 0 && settled_a && settled_a->status == ava::tui::ToolTimelineStatus::Success &&
             settled_a->detail == "retry superseded" && settled_b && settled_b->status == ava::tui::ToolTimelineStatus::Success &&
             settled_b->detail == "retry completed" && ab_tick0_state.transcript.size() == 3 && ab_tick0_state.transcript[0].label == "audit" &&
             ab_tick0_state.transcript[0].text.find("rate_limit") != std::string::npos && ab_tick0_state.transcript[1].label == "audit" &&
             ab_tick0_state.transcript[1].text.find("timeout") != std::string::npos && ab_tick0_state.transcript[2].label == "audit" &&
             ab_tick0_state.transcript[2].text.find("remaining=0ms") != std::string::npos,
         "tui event state settles every Running retry on remaining_ms==0 so RetryEvent(A)/RetryEvent(B)/RetryTick(B,0) leaves no stale Running chrome");
  assert_normal_hint(ab_tick0_state, "tui event-state-to-render drops retry chrome after A/B/tick0 ownership settlement");

  // Zero-delay RetryEvent announces an immediate attempt (no ticks). Keep current chrome visible, then settle on progress.
  ava::tui::TuiEventState zero_delay_state;
  zero_delay_state.activity.push_back(
      ava::tui::SidebarActivityItem{.id = "unrelated:search", .label = "search", .detail = "idle", .status = ava::tui::ToolTimelineStatus::Success});
  auto zero_delay = retry_payload_with_reason("rate_limit", "provider_transport", 2, 0, 0);
  ava::tui::apply_runtime_event(zero_delay_state, retry_event(zero_delay));
  auto const* zero_delay_retry = find_retry_activity(zero_delay_state);
  expect(zero_delay_retry && zero_delay_retry->status == ava::tui::ToolTimelineStatus::Running && count_running_retry_activities(zero_delay_state) == 1 &&
             zero_delay_state.transcript.size() == 1 && zero_delay_state.transcript[0].label == "audit",
         "tui event state keeps zero-delay RetryEvent as the sole Running retry so immediate-attempt chrome can still surface");
  assert_retry_chrome(zero_delay_state, "Esc stop · retry attempt 2/5",
                      "tui event-state-to-render surfaces zero-delay RetryEvent chrome without requiring countdown ticks");
  auto zero_progress = ava::event::MessagePayload{};
  zero_progress.text = "recovered";
  ava::tui::apply_runtime_event(zero_delay_state, message_update_event(std::move(zero_progress)));
  auto const* zero_settled = find_retry_activity(zero_delay_state);
  auto const unrelated_search =
      std::ranges::find_if(zero_delay_state.activity, [](ava::tui::SidebarActivityItem const& activity) { return activity.id == "unrelated:search"; });
  expect(zero_settled && zero_settled->status == ava::tui::ToolTimelineStatus::Success && zero_settled->detail == "retry completed" &&
             count_running_retry_activities(zero_delay_state) == 0 && unrelated_search != zero_delay_state.activity.end() &&
             unrelated_search->detail == "idle" && zero_delay_state.transcript[0].label == "audit",
         "tui event state settles zero-delay retry on later progress without dropping audit or unrelated activity");
  assert_normal_hint(zero_delay_state, "tui event-state-to-render restores normal contextual hint after zero-delay retry progress settlement");

  // Later reactivation after multi-identity settlement must restore exactly one Running retry.
  ava::tui::TuiEventState multi_reactivate_state = ab_tick0_state;
  expect(count_running_retry_activities(multi_reactivate_state) == 0, "precondition: multi-identity A/B/tick0 state has no Running retry before reactivation");
  ava::tui::apply_runtime_event(multi_reactivate_state, retry_event(retry_payload_with_reason("rate_limit", "rate_limit", 4, 750, 750)));
  auto const* multi_reactivated = find_retry_activity_by_id(multi_reactivate_state, "retry:rate_limit");
  expect(multi_reactivated && multi_reactivated->status == ava::tui::ToolTimelineStatus::Running &&
             multi_reactivated->detail.find("attempt 4/5") != std::string::npos && count_running_retry_activities(multi_reactivate_state) == 1 &&
             std::ranges::count_if(multi_reactivate_state.transcript, [](ava::tui::TranscriptItem const& item) { return item.label == "audit"; }) >= 3,
         "tui event state reactivates a later retry cleanly after multi-identity settlement with exactly one Running retry row");
  assert_retry_chrome(multi_reactivate_state, "Esc stop · retry attempt 4/5",
                      "tui event-state-to-render restores retry chrome after later reactivation following A/B/tick0 settlement");
}

void test_tui_todowrite_event_state_and_snapshot_hydration()
{
  tui_test_support::TuiFrontEndFixture frontend;
  auto const success_json =
      R"({"schema_version":1,"tool":"todowrite","ok":true,"todos":[{"id":"a","content":"First","status":"completed"},{"id":"b","content":"Second","status":"in_progress"},{"id":"c","content":"Third","status":"pending"}],"counts":{"total":3,"pending":1,"in_progress":1,"completed":1}})";
  auto make_todo_payload = [](std::string text, std::string call_id, std::string result_json, std::string status) {
    ava::event::ToolPayload payload;
    payload.text = std::move(text);
    payload.call_id = std::move(call_id);
    payload.tool = "todowrite";
    payload.result_json = std::move(result_json);
    payload.status = std::move(status);
    return payload;
  };

  ava::tui::TuiEventState state;
  ava::tui::apply_runtime_event(state, tool_result_event(make_todo_payload("1/3 completed", "call_todo_1", success_json, "completed")));
  expect(state.todos.size() == 3 && state.todos[0].id == "a" && state.todos[0].status == ava::tui::TodoStatus::Completed &&
             state.todos[1].status == ava::tui::TodoStatus::InProgress && state.todos[2].status == ava::tui::TodoStatus::Pending,
         "successful todowrite ToolResult replaces event-state todos");

  ava::tui::apply_runtime_event(
      state, tool_result_event(
                 make_todo_payload("failed", "call_todo_fail", R"({"schema_version":1,"tool":"todowrite","ok":false,"error":{"message":"bad"}})", "error")));
  expect(state.todos.size() == 3, "failed todowrite ToolResult leaves existing todos unchanged");

  ava::tui::apply_runtime_event(state, tool_result_event(make_todo_payload("ok", "call_todo_bad", R"({"ok":true,"todos":[})", "completed")));
  expect(state.todos.size() == 3, "malformed successful todowrite ToolResult is ignored");

  ava::tui::apply_runtime_event(
      state,
      tool_result_event(make_todo_payload(
          "todos cleared", "call_todo_clear",
          R"({"schema_version":1,"tool":"todowrite","ok":true,"todos":[],"counts":{"total":0,"pending":0,"in_progress":0,"completed":0}})", "completed")));
  expect(state.todos.empty(), "empty successful todowrite snapshot clears event-state todos");

  ava::tui::TuiRuntimeOptions options;
  options.initial_todos = {ava::tui::TodoItem{.id = "seed", .content = "Hydrated", .status = ava::tui::TodoStatus::Pending}};
  ava::tui::RuntimePresentationState presentation(frontend.frontend(), options);
  expect(presentation.sidebar.todos.size() == 1 && presentation.sidebar.todos.front().id == "seed", "runtime options hydrate sidebar todos at startup");
  int todo_pulls = 0;
  options.read_committed_todos = [&]() {
    ++todo_pulls;
    return std::vector<ava::tui::TodoItem>{{.id = "switched", .content = "After switch", .status = ava::tui::TodoStatus::InProgress}};
  };
  presentation.refresh_after_backend_action(options);
  expect(todo_pulls == 1 && presentation.sidebar.todos.size() == 1 && presentation.sidebar.todos.front().id == "switched",
         "boundary refresh replaces todos from committed session history");
}

void test_frontend_data_changed_routes_each_canonical()
{
  tui_test_support::TuiFrontEndFixture fixture;
  auto& frontend = *fixture.frontend();
  auto consume = [&]() {
    // One public display accessor per canonical domain; consume all independently
    // pending fields as well so later writes cannot inherit initialization flags.
    std::array<bool, 7> changed{
        frontend.agent_selection().agent_refresh().changed,       frontend.directories().workspace_root_refresh().changed,
        frontend.git_branch().git_branch_refresh().changed,       frontend.model_selection().model_refresh().changed,
        frontend.prompt_metadata().mode_refresh().changed,        frontend.session_stats().token_count_refresh().changed,
        frontend.session_identity().session_id_refresh().changed,
    };
    static_cast<void>(frontend.agent_selection().provenance_refresh());
    static_cast<void>(frontend.agent_selection().tool_preset_refresh());
    static_cast<void>(frontend.directories().current_directory_refresh());
    static_cast<void>(frontend.model_selection().provider_refresh());
    static_cast<void>(frontend.model_selection().reasoning_refresh());
    static_cast<void>(frontend.model_selection().context_window_tokens_refresh());
    static_cast<void>(frontend.prompt_metadata().context_source_count_refresh());
    static_cast<void>(frontend.session_stats().contex_percentage_refresh());
    static_cast<void>(frontend.session_identity().session_path_refresh());
    return changed;
  };
  auto const initial = consume();
  expect(std::ranges::all_of(initial, [](bool changed) { return changed; }), "registration initializes all seven canonical display projections");
  expect(consume() == std::array<bool, 7>{}, "initial display changes are consumed independently");
  for (std::size_t domain = 0; domain < 7; ++domain)
  {
    switch (domain)
    {
      case 0:
        fixture.set_selected_agent_metadata("route-agent", ava::agent::AgentDefinitionProvenance::Project, ava::agent::AgentToolPreset::ReadOnly);
        break;
      case 1:
        fixture.set_directories("/route/workspace", "/route/workspace/current");
        break;
      case 2:
        fixture.set_git_branch("route-branch");
        break;
      case 3:
        fixture.set_model({.display_name = "Route model"});
        break;
      case 4:
        fixture.set_prompt_metadata(ava::agent::Mode::Plan, 3);
        break;
      case 5:
        fixture.set_active_token_estimates(42'000, 100);
        break;
      case 6:
        fixture.set_session_identity_metadata("route-session");
        break;
    }
    std::array<bool, 7> expected{};
    expected[domain] = true;
    expect(consume() == expected, "canonical write changes only its corresponding display projection, domain " + std::to_string(domain));
    expect(consume() == std::array<bool, 7>{}, "each canonical display notification is consumed once, domain " + std::to_string(domain));
  }
}

void test_runtime_initialization_preserves_local_and_retained_inputs()
{
  tui_test_support::TuiFrontEndFixture frontend;
  ava::tui::TuiRuntimeOptions options;
  options.app_version = "boundary-test-version";
  options.initial_status = "keybinding load warning";
  options.initial_transcript = {{.label = "ava", .text = "onboarding instructions"}};
  ava::tui::RuntimePresentationState presentation(frontend.frontend(), options);
  expect(presentation.sidebar.version == "boundary-test-version" && presentation.snapshot.status == "keybinding load warning" &&
             presentation.snapshot.transcript.size() == 1 && presentation.snapshot.transcript.front().text == "onboarding instructions",
         "immutable version, startup diagnostics and retained onboarding content survive initialization without backend-state transport");
  frontend.set_prompt_metadata(ava::agent::Mode::Plan, 2);
  presentation.refresh_prompt_metadata();
  expect(presentation.snapshot.mode == "plan" && presentation.sidebar.mode == "plan",
         "canonical mode refresh updates composer and sidebar together without a returned current-mode label");
}

void run_tui_runtime_event_state_tests()
{
  test_frontend_data_changed_routes_each_canonical();
  test_runtime_initialization_preserves_local_and_retained_inputs();
  test_tui_event_state_reduces_runtime_events();
  test_tui_retry_activity_settles_and_restores_contextual_hint();
  test_tui_transcript_projection_and_cap_parity();
  test_tui_mixed_runtime_event_queue_preserves_order_and_context();
  test_tui_private_subagent_launch_queue_and_exact_reducer();
  test_tui_typed_runtime_inherits_session_identity_fields();
  test_tui_todowrite_event_state_and_snapshot_hydration();
}

void run_tui_runtime_dispatch_tests()
{
  test_tui_active_nonblocking_command_lane();
  test_tui_selector_authority_dispatch_paints_before_callback();
}

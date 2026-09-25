#include "sys.h"
#include "ava/event/events.h"
#include "ava/app/command_format.h"
#include "ava/app/command_sessions.h"
#include "ava/app/runtime.h"
#include "ava/app/runtime/Session.h"
#include "ava/session/compaction.h"
#include "ava/session/record.h"

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace ava::app {
namespace {

ava::event::RuntimeEventMetadata command_event_metadata(runtime::session_ts const& unlocked_session)
{
  auto const session_id = runtime::session_ts::crat(unlocked_session)->store.session_id();
  return ava::event::RuntimeEventMetadata{
      .timestamp = ava::session::now_timestamp(),
      .session_id = session_id,
  };
}

bool command_canceled(CommandRequest const& request)
{
  return request.cancel_requested && request.cancel_requested();
}

ava::core::Error command_canceled_error()
{
  return ava::core::Error(ava::core::ErrorCategory::Unknown, "agent loop canceled", ava::core::ErrorCode::Canceled);
}

}  // namespace

ava::core::Result<CommandResult> run_compact_command(runtime::session_ts& unlocked_session, CommandRequest const& request)
{
  CommandResult result;
  result.handled = true;
  auto fail_compaction = [&](ava::core::Error error) -> ava::core::Result<CommandResult> {
    if (request.propagate_compaction_errors)
      return std::unexpected(std::move(error));
    add_output(result, error.format());
    return result;
  };
  auto const instructions = command_argument(request.command, "/compact");
  if (!request.compaction_summary_generator)
  {
    return fail_compaction(ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "/compact requires provider-backed summary generation"));
  }
  auto const paths = runtime::session_ts::rat(unlocked_session)->paths();
  auto loaded_config = ava::session::load_compaction_config(paths);
  if (!loaded_config)
  {
    return fail_compaction(std::move(loaded_config.error()));
  }
  auto config = resolve_compaction_config(unlocked_session, std::move(*loaded_config));
  if (!config)
  {
    return fail_compaction(std::move(config.error()));
  }
  ava::core::Result<ava::session::SessionReadAuthority> read_authority =
      std::unexpected(ava::core::Error(ava::core::ErrorCategory::Unknown, "session read authority was not captured"));
  std::shared_ptr<SessionRunController> run_controller;
  std::optional<long long> context_window_tokens;
  {
    SCOPED_CRITICAL_AREA_R(session_r, unlocked_session);
    read_authority = session_r->read_authority_1();
    run_controller = session_r->run_controller();
    context_window_tokens = session_r->model().context_window_tokens;
  }
  if (!read_authority)
    return fail_compaction(std::move(read_authority.error()));
  if (!run_controller)
  {
    return fail_compaction(ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "runtime session controller is unavailable"));
  }

  constexpr std::size_t max_compaction_attempts = 2;
  std::size_t last_snapshot_entries = 0;
  std::size_t last_current_entries = 0;
  for (std::size_t attempt = 0; attempt < max_compaction_attempts; ++attempt)
  {
    if (command_canceled(request))
      return fail_compaction(command_canceled_error());
    ava::core::Result<std::vector<ava::session::SessionEntry>> entries = read_authority->load();
    if (!entries)
    {
      return fail_compaction(std::move(entries.error()));
    }
    auto prepared = prepare_compaction_context(*entries, *config);
    if (!prepared)
      return fail_compaction(std::move(prepared.error()));
    ava::event::CompactionPayload start_payload{
        .provider = config->provider_id,
        .model = config->model_id,
        .status = "started",
        .trigger = "manual",
        .reason = "manual",
        .attempt = attempt + 1,
        .max_attempts = max_compaction_attempts,
        .estimated_tokens = prepared->estimated_tokens,
        .threshold_tokens = ava::session::effective_auto_threshold_tokens(*config, context_window_tokens),
        .retained_tokens = prepared->retained_tokens,
    };
    if (auto emitted = ava::event::emit_event(
            request.event_sink,
            ava::event::RuntimeEvent{command_event_metadata(unlocked_session), ava::event::CompactionStartEvent{.payload = std::move(start_payload)}});
        !emitted)
    {
      return fail_compaction(std::move(emitted.error()));
    }
    auto summary = request.compaction_summary_generator(prepared->active_entries, *config, instructions, prepared->estimated_tokens);
    if (!summary)
    {
      return fail_compaction(std::move(summary.error()));
    }
    if (command_canceled(request))
      return fail_compaction(command_canceled_error());
    if (summary->empty())
    {
      return fail_compaction(ava::core::Error(ava::core::ErrorCategory::Provider, "compaction summary generation returned an empty summary"));
    }
    if (summary->size() > config->max_summary_bytes)
    {
      auto error = ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "generated compaction summary is too large");
      error.with_context("max_summary_bytes", std::to_string(config->max_summary_bytes));
      error.with_context("summary_bytes", std::to_string(summary->size()));
      return fail_compaction(std::move(error));
    }

    auto entry = ava::session::make_manual_compaction_entry(
        ava::session::ManualCompactionRequest{.summary = *summary,
                                              .instructions = instructions,
                                              .config = *config,
                                              .estimated_tokens = prepared->estimated_tokens,
                                              .threshold_tokens = ava::session::effective_auto_threshold_tokens(*config, context_window_tokens),
                                              .retained_tokens = prepared->retained_tokens,
                                              .trigger = "manual",
                                              .recent_context = prepared->recent_context,
                                              .recent_context_omitted = prepared->recent_context_omitted});
    if (!entry)
      return fail_compaction(std::move(entry.error()));

    auto const snapshot_entries = entries->size();
    auto appended = run_controller->append_compaction_if_snapshot_matches(std::move(*entry), std::move(*entries), request.cancel_requested);
    if (!appended)
      return fail_compaction(std::move(appended.error()));
    bool const snapshot_stale = *appended == ava::session::SessionCompactionAppendResult::SnapshotMismatch;
    if (!snapshot_stale)
    {
      ava::event::CompactionPayload end_payload{
          .provider = config->provider_id,
          .model = config->model_id,
          .status = "completed",
          .trigger = "manual",
          .reason = "manual",
          .attempt = attempt + 1,
          .max_attempts = max_compaction_attempts,
          .estimated_tokens = prepared->estimated_tokens,
          .threshold_tokens = ava::session::effective_auto_threshold_tokens(*config, context_window_tokens),
          .retained_tokens = prepared->retained_tokens,
          .post_compaction_tokens = ava::session::estimate_tokens(*summary) + ava::session::estimate_tokens(instructions) + prepared->retained_tokens,
          .summary_bytes = summary->size(),
      };
      if (auto emitted = ava::event::emit_event(
              request.event_sink,
              ava::event::RuntimeEvent{command_event_metadata(unlocked_session), ava::event::CompactionEndEvent{.payload = std::move(end_payload)}});
          !emitted)
      {
        return fail_compaction(std::move(emitted.error()));
      }
      add_output(result, "compaction summary recorded");
      return result;
    }
    last_snapshot_entries = snapshot_entries;
    auto current_entries = read_authority->load();
    if (!current_entries)
      return fail_compaction(std::move(current_entries.error()));
    last_current_entries = current_entries->size();
    if (attempt + 1 < max_compaction_attempts)
    {
      ava::event::RetryPayload retry_payload;
      retry_payload.status = "started";
      retry_payload.trigger = "manual";
      retry_payload.reason = "stale_compaction_snapshot";
      retry_payload.attempt = attempt + 2;
      retry_payload.max_attempts = max_compaction_attempts;
      ava::event::RetryDiagnostics retry_diagnostics;
      retry_diagnostics.snapshot_entries = last_snapshot_entries;
      retry_diagnostics.current_entries = last_current_entries;
      if (auto emitted = ava::event::emit_event(
              request.event_sink,
              ava::event::RuntimeEvent{command_event_metadata(unlocked_session),
                                       ava::event::RetryEvent{.payload = std::move(retry_payload), .diagnostics = std::move(retry_diagnostics)}});
          !emitted)
      {
        return fail_compaction(std::move(emitted.error()));
      }
    }
  }
  return fail_compaction(stale_compaction_snapshot_error("manual", last_snapshot_entries, last_current_entries));
}

}  // namespace ava::app

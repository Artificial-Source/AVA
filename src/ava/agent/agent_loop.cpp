#include "sys.h"
#include "ava/http/transport.h"
#include "ava/agent/agent_loop.h"
#include "ava/agent/agent_turn_executor_internal.h"

#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace ava::agent {
namespace {

struct AgentTraceScope
{
  ava::session::SessionStore& store;
  std::shared_ptr<ava::observability::RunObservation> observation;
  ava::observability::TraceContext const& context;
  ava::observability::TraceOutcome outcome = ava::observability::TraceOutcome::Failed;

  std::uint64_t attachment_generation = 0;

  AgentTraceScope(ava::session::SessionStore& store_in, std::shared_ptr<ava::observability::RunObservation> observation_in,
                  ava::observability::TraceContext const& context_in, std::uint64_t generation) noexcept
      : store(store_in), observation(std::move(observation_in)), context(context_in), attachment_generation(generation)
  {
  }
  ~AgentTraceScope() noexcept
  {
    if (observation)
      observation->emit(ava::observability::TraceEventType::AgentRunTerminal, context, [this](auto& event) {
        event.phase = ava::observability::TracePhase::Run;
        event.outcome = outcome;
      });
    // Do not let stale background copies clear a newer attachment.
    store.clear_run_observation(attachment_generation);
  }
};

bool is_terminal_canceled_error(ava::core::Error const& error)
{
  return error.code() == ava::core::ErrorCode::Canceled;
}

ava::observability::TraceOutcome terminal_outcome(ava::core::Error const& error)
{
  if (is_terminal_canceled_error(error))
    return ava::observability::TraceOutcome::Canceled;
  switch (error.category())
  {
    case ava::core::ErrorCategory::Provider:
      return ava::observability::TraceOutcome::ProviderError;
    case ava::core::ErrorCategory::Tool:
      return ava::observability::TraceOutcome::ToolError;
    case ava::core::ErrorCategory::Session:
    case ava::core::ErrorCategory::Io:
      return ava::observability::TraceOutcome::SessionError;
    default:
      return ava::observability::TraceOutcome::Failed;
  }
}

}  // namespace

AgentLoop::AgentLoop(AgentLoopOptions options) : options_(std::move(options))
{
  auto [roots, over_limit] = detail::bounded_deduplicated_authority_roots(std::move(options_.tool_execution.ava_authority_roots));
  options_.tool_execution.ava_authority_roots = std::move(roots);
  ava_authority_roots_over_limit_ = over_limit;
}

std::string to_string(ToolTimelineStatus status)
{
  switch (status)
  {
    case ToolTimelineStatus::Running:
      return "running";
    case ToolTimelineStatus::Success:
      return "success";
    case ToolTimelineStatus::Canceled:
      return "canceled";
    case ToolTimelineStatus::Error:
      return "error";
  }
  return "unknown";
}

ava::core::Result<AgentLoopResult> AgentLoop::run_turn(std::string const& user_message, ava::session::SessionStore& store,
                                                       ava::provider::Provider const& provider, ava::http::Transport& transport)
{
  return run_turn(user_message, {}, store, provider, transport);
}

ava::core::Result<AgentLoopResult> AgentLoop::run_turn(std::string const& user_message, std::vector<ava::session::ImageAttachmentRef> const& image_attachments,
                                                       ava::session::SessionStore& store, ava::provider::Provider const& provider,
                                                       ava::http::Transport& transport)
{
  if (ava_authority_roots_over_limit_)
  {
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "AgentLoop received more than 64 distinct AVA authority roots"));
  }
  if (!options_.append_entry || !options_.append_batch)
  {
    return std::unexpected(
        ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "AgentLoop requires append and batch authority routes before producing records"));
  }
  if (!options_.session_read_authority)
  {
    return std::unexpected(
        ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "AgentLoop requires a lifetime-safe session read authority before reading history"));
  }
  ava::observability::TraceContext trace_context;
  std::optional<AgentTraceScope> trace_scope;
  if (options_.observation && options_.observation->enabled())
  {
    try
    {
      trace_context = options_.trace_context;
      trace_context.session_id = trace_context.session_id.empty() ? store.session_id() : trace_context.session_id;
      trace_context.provider_id = trace_context.provider_id.empty() ? options_.model.provider_id : trace_context.provider_id;
      // Empty/failed IDs are still isolated at RunObservation; do not let them
      // prevent the authoritative run.
      if (trace_context.run_id.empty())
        trace_context.run_id = options_.observation->next_id("run");
      if (trace_context.turn_id.empty())
        trace_context.turn_id = options_.observation->next_id("turn");
      auto const attachment_generation = store.set_run_observation(options_.observation, trace_context);
      // A failed observer attachment must not orphan the lifecycle: generation
      // zero simply makes scope cleanup a no-op after it emits the terminal.
      trace_scope.emplace(store, options_.observation, trace_context, attachment_generation);
      options_.observation->emit(ava::observability::TraceEventType::AgentRunStart, trace_context,
                                 [](auto& event) { event.phase = ava::observability::TracePhase::Run; });
    }
    catch (...)
    {
      options_.observation->account_external_failure();
    }
  }
  auto result = run_turn_impl(user_message, image_attachments, store, provider, transport, trace_context);
  if (trace_scope)
    trace_scope->outcome = result ? ava::observability::TraceOutcome::Completed : terminal_outcome(result.error());
  return result;
}

ava::core::Result<AgentLoopResult> AgentLoop::run_turn_impl(std::string const& user_message,
                                                            std::vector<ava::session::ImageAttachmentRef> const& image_attachments,
                                                            ava::session::SessionStore& store, ava::provider::Provider const& provider,
                                                            ava::http::Transport& transport, ava::observability::TraceContext const& trace_context)
{
  detail::AgentTurnExecutor executor(options_, user_message, image_attachments, store, provider, transport, trace_context);
  return executor.run();
}

}  // namespace ava::agent

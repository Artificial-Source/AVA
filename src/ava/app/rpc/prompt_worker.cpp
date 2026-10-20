#include "sys.h"
#include "output.h"
#include "prompt_worker.h"
#include "resolvers.h"
#include "run_state.h"
#include "serialization.h"
#include "session_operators.h"
#include "ava/event/events.h"
#include "ava/app/runtime.h"
#include "ava/app/runtime/Session.h"
#include "ava/provider/catalog.h"
#include "ava/core/thread.h"

#include <optional>
#include <utility>
#include <vector>

namespace ava::app::rpc {

ava::core::JoinThread make_rpc_prompt_worker(RpcPromptWorkerOptions options)
{
  DoutEntering(dc::rpc, "make_rpc_prompt_worker(options)");

  return ava::core::JoinThread::create("rpc_prompt", [options = std::move(options)](std::stop_token stop_token) mutable {
    runtime::session_ts& unlocked_session = options.unlocked_session;

    auto publish_terminal = [&](std::string const& request_id, ava::core::Result<std::string> result, bool allow_follow_up,
                                std::string_view terminal_reason) -> ava::core::Result<RpcFollowUpTransition> {
      RpcFollowUpTransition transition;
      if (allow_follow_up)
      {
        begin_prompt_terminal_publication(options.run_state);
      }
      else
      {
        transition.kind = RpcFollowUpTransitionKind::Deactivated;
        transition.cleared = begin_terminal_publication(options.run_state);
      }

      auto fail_publication = [&](ava::core::Error error) -> ava::core::Result<RpcFollowUpTransition> {
        complete_terminal_publication(options.run_state, request_id);
        return std::unexpected(std::move(error));
      };

      ava::core::VoidResult written;
      if (result)
        written = write_success(options.output, request_id, *result);
      else
        written = write_error(options.output, request_id, result.error());
      if (!written)
        return fail_publication(std::move(written.error()));

      if (allow_follow_up)
        transition = transition_after_prompt_terminal_response(options.run_state);

      if (transition.kind == RpcFollowUpTransitionKind::Activated)
      {
        auto started_event = *transition.follow_up;
        started_event.correlation_id = started_event.request_id;
        if (auto started = write_queue_event(options.output, unlocked_session, "follow_up_started", started_event);
            !started)
          return fail_publication(std::move(started.error()));
      }
      else
      {
        auto const reason = transition.kind == RpcFollowUpTransitionKind::Skipped ? std::string_view("canceled") : terminal_reason;
        if (auto skipped = write_skipped_queue_events(options.output, unlocked_session, transition.cleared, reason);
            !skipped)
          return fail_publication(std::move(skipped.error()));
        if (auto follow_up_errors = write_follow_up_errors(options.output, options.run_state, transition.cleared.follow_up_messages, reason); !follow_up_errors)
          return fail_publication(std::move(follow_up_errors.error()));
      }

      complete_terminal_publication(options.run_state, request_id);
      return transition;
    };

    auto abandon_after_output_failure = [&](ava::core::Error error) {
      record_async_error(options.run_state, std::move(error));
      static_cast<void>(deactivate_and_clear_queued_messages(options.run_state));
    };

    CRITICAL_AREA_BEGIN_R(session);
    auto const prompt_provider_id = session_r->model_selection_r()->model.provider_id;
    auto prompt_catalog = session_r->provider_catalog();
    CRITICAL_AREA_END_R(session);
    auto prompt_options = ensure_prompt_runtime_options(options.paths, prompt_provider_id, std::move(options.runtime_options), options.auth_transport, "prompt",
                                                        std::move(prompt_catalog));
    if (!prompt_options)
    {
      auto published = publish_terminal(options.request_id, std::unexpected(std::move(prompt_options.error())), false, "prompt_start_failed");
      if (!published)
        abandon_after_output_failure(std::move(published.error()));
      return;
    }
    auto policy_permission_resolver = prompt_options->permission_resolver;

    auto run_one_prompt = [&](std::string const& request_id, std::string const& message,
                              std::vector<ava::session::ImageAttachmentRef> image_attachments) -> ava::core::Result<std::string> {
      set_active_request_id(options.run_state, request_id);
      prompt_options->request_id = request_id;
      prompt_options->image_attachments = std::move(image_attachments);
      prompt_options->cancel_requested = [&run_state = options.run_state, stop_token] { return stop_token.stop_requested() || cancel_requested(run_state); };

      // These callbacks run synchronously inside run_prompt's write critical area and retain the Session reference only until run_prompt returns.
      prompt_options->permission_resolver = make_rpc_permission_resolver(options.pending_state, options.output, options.run_state, unlocked_session,
                                                                         policy_permission_resolver, request_id);
      prompt_options->question_resolver =
          make_rpc_question_resolver(options.pending_state, options.output, options.run_state, unlocked_session, request_id);

      // This callback runs inside run_prompt's write critical area, so retain an owning snapshot instead of relocking or capturing Session storage.
      auto const session_id = runtime::session_ts::rat(unlocked_session)->store.options_r()->session_id;
      prompt_options->take_steering_messages = [&options, session_id, request_id]() -> ava::core::Result<std::vector<std::string>> {
        auto queued = take_queued_steering_messages(options.run_state, request_id);
        std::vector<std::string> messages;
        messages.reserve(queued.size());
        for (auto const& item : queued)
        {
          auto envelope = resolver_event_envelope("steer_applied", item.request_id, item.correlation_id, session_id, queued_message_payload_json(item.message));
          if (auto written = Output::write_record(options.output, ava::event::serialize_event_envelope_jsonl(envelope)); !written)
            return std::unexpected(std::move(written.error()));
          messages.push_back(item.message);
        }
        return messages;
      };

      ava::event::EventBus event_bus;
      subscribe_event_envelope_writer(event_bus, options.output);
      prompt_options->event_sink = ava::event::make_runtime_event_bus_adapter(event_bus, rpc_event_context(request_id));

      auto selected_provider = provider_for_session_model(unlocked_session, options.injected_provider_id, options.injected_provider);
      if (!selected_provider)
        return std::unexpected(std::move(selected_provider.error()));
      auto prompt_provider = std::move(*selected_provider);

      auto result = run_prompt(unlocked_session, message, prompt_provider.get(), options.transport, *prompt_options);

      ClearedRpcQueues skipped_steering;
      skipped_steering.steering_messages = clear_queued_steering_messages(options.run_state);
      if (auto written = write_skipped_queue_events(options.output, unlocked_session, skipped_steering, "run_completed_before_safe_point"); !written)
      {
        return std::unexpected(std::move(written.error()));
      }
      if (!result)
        return std::unexpected(std::move(result.error()));
      return prompt_result_json(session_id_snapshot(unlocked_session), *result);
    };

    std::string request_id = options.request_id;
    auto prompt_run = run_one_prompt(request_id, options.message, std::move(options.image_attachments));
    while (true)
    {
      auto published = publish_terminal(request_id, std::move(prompt_run), true, "run_completed_before_safe_point");
      if (!published)
      {
        abandon_after_output_failure(std::move(published.error()));
        return;
      }
      if (published->kind != RpcFollowUpTransitionKind::Activated)
        return;

      auto follow_up = std::move(*published->follow_up);
      request_id = follow_up.request_id;
      prompt_run = run_one_prompt(request_id, follow_up.message, {});
    }
  });
}

}  // namespace ava::app::rpc

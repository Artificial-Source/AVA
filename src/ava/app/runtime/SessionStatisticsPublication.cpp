#include "sys.h"
#include "ava/app/runtime/Session.h"
#include "ava/app/runtime/SessionStatisticsPublication.h"
#include "ava/session/Stats.h"
#include "ava/session/compaction.h"

#include <optional>
#include <tuple>
#include <utility>

namespace ava::app::runtime {
namespace {

struct StatisticsInputs
{
  std::shared_ptr<SessionStatisticsPublicationState> publication;
  SessionStatisticsPublicationIdentity identity;
  ava::core::Result<ava::session::SessionReadAuthority> read_authority;

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

// Snapshot the exact Session destination, history authority, prompt, and
// controller mutation identity used by one computation attempt. The returned
// values own everything needed after the Session read lock is released.
StatisticsInputs capture_statistics_inputs(session_ts const& unlocked_session)
{
  SCOPED_CRITICAL_AREA_CR(session_r, unlocked_session);
  auto controller = session_r->run_controller();
  auto const mutation_identity = controller ? controller->statistics_mutation_identity() : SessionStatisticsMutationIdentity{};
  return StatisticsInputs{.publication = session_r->session_statistics_publication_state(),
                          .identity = SessionStatisticsPublicationIdentity{.session_id = session_r->store.options_session_id_copy(),
                                                                           .append_target = session_r->append_target(),
                                                                           .run_controller = std::move(controller),
                                                                           .mutation_identity = mutation_identity,
                                                                           .system_prompt = session_r->system_prompt()},
                          .read_authority = session_r->read_authority_1()};
}

// Build the bounded retry error returned when every computed result lost its
// identity race to a newer Session or append mutation.
ava::core::Error stale_statistics_error()
{
  auto error = ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "session statistics computation remained stale");
  error.with_context("recovery", "retry after the current session mutation completes");
  return error;
}

}  // namespace

ava::core::VoidResult refresh_session_statistics(session_ts& unlocked_session)
{
  DoutEntering(dc::notice, "refresh_session_statistics()");

  AVA_ASSERT_SESSION_UNLOCKED(unlocked_session, "calling refresh_session_statistics");

  auto first_inputs = capture_statistics_inputs(unlocked_session);
  if (!first_inputs.publication)
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "session statistics publisher is unavailable"));

  auto publication = first_inputs.publication;
  std::unique_lock work_lock(publication->mutex_);
  constexpr std::size_t kMaximumStaleRetries = 3;
  for (std::size_t attempt = 0; attempt < kMaximumStaleRetries; ++attempt)
  {
    auto inputs = capture_statistics_inputs(unlocked_session);
    if (inputs.publication != publication)
      return {};
    if ((inputs.identity.mutation_identity.append_order_generation & 1U) == 0 && publication->published_identity_ &&
        *publication->published_identity_ == inputs.identity)
      return {};
    if (!inputs.identity.run_controller)
      return std::unexpected(ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "runtime session controller is unavailable"));
    if (inputs.identity.mutation_identity.persistence_failure_latched)
      return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Io, "session statistics are unavailable during persistence recovery"));
    if (!inputs.read_authority)
      return std::unexpected(std::move(inputs.read_authority.error()));

    ++publication->work_count_;
    auto entries = inputs.read_authority->load();
    if (!entries)
      return std::unexpected(std::move(entries.error()));
    auto updated_stats = ava::session::compute_session_stats(*entries);
    if (!updated_stats)
      return std::unexpected(std::move(updated_stats.error()));
    auto active_context_tokens = ava::session::estimate_active_context_tokens(*entries);
    if (!active_context_tokens)
      return std::unexpected(std::move(active_context_tokens.error()));
    updated_stats->active_context_tokens = *active_context_tokens;
    updated_stats->system_prompt_tokens = ava::session::estimate_tokens(inputs.identity.system_prompt);

    {
      SCOPED_CRITICAL_AREA_W(session_w, unlocked_session);

      if (session_w->session_statistics_publication_state() != publication || session_w->store.options_r()->session_id != inputs.identity.session_id ||
          session_w->append_target() != inputs.identity.append_target || session_w->run_controller() != inputs.identity.run_controller ||
          session_w->system_prompt() != inputs.identity.system_prompt)
        return {};

      bool const published = inputs.identity.run_controller->publish_if_statistics_mutation_current(inputs.identity.mutation_identity, [&] {
        if (*session_w->session_stats_r() != *updated_stats)
          *session_w->session_stats_w() = std::move(*updated_stats);
        publication->published_identity_ = inputs.identity;
      });

      if (published)
        return {};
    }
  }
  return std::unexpected(stale_statistics_error());
}

std::size_t session_statistics_refresh_work_count(session_ts const& unlocked_session)
{
  auto publication = session_ts::crat(unlocked_session)->session_statistics_publication_state();
  if (!publication)
    return 0;
  std::lock_guard lock(publication->mutex_);
  return publication->work_count_;
}

}  // namespace ava::app::runtime

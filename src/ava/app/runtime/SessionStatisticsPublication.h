#pragma once

#include "ava/debug/print_members_on.h"
#include "ava/app/runtime/session_ts.h"
#include "ava/app/session_run_controller.h"
#include "ava/core/result.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

namespace ava::session {
class SessionAppendTarget;
}

namespace ava::app::runtime {

// Exact backend mutation identity for one successfully published statistics value.
// The prompt text is retained only for equality validation and is never observable or logged.
struct SessionStatisticsPublicationIdentity
{
  std::string session_id;
  std::shared_ptr<ava::session::SessionAppendTarget> append_target;
  std::shared_ptr<SessionRunController> run_controller;
  SessionStatisticsMutationIdentity mutation_identity;
  std::string system_prompt;

  // Compare durable statistics inputs. append_order_generation is deliberately
  // excluded: it validates stale work while in flight, but completed conditional
  // no-ops must still coalesce when the committed generation did not advance.
  bool operator==(SessionStatisticsPublicationIdentity const& other) const
  {
    return session_id == other.session_id && append_target == other.append_target && run_controller == other.run_controller &&
           mutation_identity.committed_append_generation == other.mutation_identity.committed_append_generation &&
           mutation_identity.persistence_invalidation_generation == other.mutation_identity.persistence_invalidation_generation &&
           system_prompt == other.system_prompt;
  }

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

// Session-owned synchronization for coalescing statistics work. It is replaced
// with the Session value and is never captured by controller append routes.
class SessionStatisticsPublicationState final
{
 public:
  SessionStatisticsPublicationState() = default;
  SessionStatisticsPublicationState(SessionStatisticsPublicationState const&) = delete;
  SessionStatisticsPublicationState& operator=(SessionStatisticsPublicationState const&) = delete;

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT

 private:
  std::mutex mutex_;
  std::optional<SessionStatisticsPublicationIdentity> published_identity_;
  std::size_t work_count_ = 0;

  friend ava::core::VoidResult refresh_session_statistics(session_ts& unlocked_session);
  friend std::size_t session_statistics_refresh_work_count(session_ts const& unlocked_session);
};

// Recompute and publish canonical statistics for unlocked_session when its
// append generation or system prompt changed. Costly reads and estimation run
// without the Session lock; stale work is rejected at the controller boundary.
[[nodiscard]] ava::core::VoidResult refresh_session_statistics(session_ts& unlocked_session);

// Return the number of non-coalesced computation attempts for unlocked_session.
// This is narrow backend instrumentation used to verify mutation-boundary work.
[[nodiscard]] std::size_t session_statistics_refresh_work_count(session_ts const& unlocked_session);

}  // namespace ava::app::runtime

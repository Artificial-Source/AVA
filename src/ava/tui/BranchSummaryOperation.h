#pragma once

#include "TuiTimer.h"
#include "runtime_state_internal.h"

namespace ava::tui {

class RuntimeRenderer;
class SelectorHost;
class TuiEventLoop;
namespace terminal {
class Context;
}

enum class BranchSummaryOutcome
{
  Inactive,
  Pending,
  Settled,
  ExitReady,
  RenderFailed
};

enum class BranchSummaryCancelMode
{
  ContinueSession,
  ExitAfterSettlement
};

class BranchSummaryOperation final
{
 public:
  // Frontend and session presentation/selector/renderer/context outlive this
  // TUI-thread-owned operation. Backend execution remains frontend-owned.
  BranchSummaryOperation(TuiFrontEnd& frontend, RuntimePresentationState& presentation, SelectorHost& selectors, RuntimeRenderer& renderer,
                         terminal::Context& terminal_context);
  BranchSummaryOperation(BranchSummaryOperation const&) = delete;
  BranchSummaryOperation& operator=(BranchSummaryOperation const&) = delete;

  // The attached loop outlives this stable operation and its registration.
  // Attach the renderer first so discovery requests only batch invalidation.
  void attach_loop(TuiEventLoop& loop);
  [[nodiscard]] std::optional<BranchSummaryOutcome> take_service_outcome();
  [[nodiscard]] bool active() const noexcept;
  [[nodiscard]] BranchSummaryOutcome start(SelectListView session_view, std::size_t selected_index, std::string selected_value);
  [[nodiscard]] BranchSummaryOutcome poll();
  [[nodiscard]] BranchSummaryOutcome cancel(BranchSummaryCancelMode mode);
  [[nodiscard]] BranchSummaryOutcome handle_input(InputEvent const& input);

  // Retains a session catalog checkpoint plus arbitrary presentation/renderer
  // aggregates; generated printing must not expose those message/path contents.
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT

 private:
  void restore_selector(SelectListView view);
  void present_status(std::string status);
  [[nodiscard]] BranchSummaryOutcome settle(TuiBranchSummarySnapshot const& operation);
  [[nodiscard]] BranchSummaryOutcome consume(TuiBranchSummarySnapshot const& operation);
  [[nodiscard]] BranchSummaryOutcome confirm();
  void discover_progress(TuiTimer::Clock::time_point now);

  TuiFrontEnd& frontend_;
  RuntimePresentationState& presentation_state_;
  SelectorHost& selectors_;
  RuntimeRenderer& renderer_;
  terminal::Context& terminal_context_;
  bool active_ = false;
  bool exit_requested_ = false;
  std::uint64_t generation_ = 0;
  TuiBranchSummaryPhase phase_ = TuiBranchSummaryPhase::Idle;
  SelectListView prior_session_view_;
  std::size_t prior_selected_index_ = 0;
  ava::core::NoLogString selected_source_value_;
  std::optional<BranchSummaryOutcome> service_outcome_;
  TuiTimer discovery_timer_;
};

} // namespace ava::tui

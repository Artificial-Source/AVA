// Publishes accepted Mermaid results on the TUI thread while application workers retain process ownership.
#include "sys.h"
#include "TuiEventLoop.h"
#include "TuiWakeup.h"
#include "ava/tui/runtime_mermaid_internal.h"
#include "ava/tui/runtime_render_internal.h"
#include "ava/tui/runtime_state_internal.h"
#include "ava/tui/runtime_transcript_search_internal.h"

#include <algorithm>
#include <limits>
#include <memory>
#include <ranges>
#include <utility>
#include "debug.h"

namespace ava::tui {
namespace {

// Temporary TUI-thread value used to compare accepted geometry before/after reconciliation; owns only indices
// and stable IDs, with no bridge/layout borrows.
struct AcceptedIdentity
{
  // Current TUI-thread transcript item index used to choose earliest affected search item.
  std::size_t item_index = 0;
  // Fence order within the item; changes can alter which source region is replaced.
  std::size_t block_index = 0;
  // Stable accepted source identity; equality detects retained results without comparing diagram output text.
  std::uint64_t block_identity = 0;

  // Anonymous-namespace types cannot have generated out-of-line member printers.
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

template <typename Records>
// TUI-thread pure projection of retained accepted records; returned values survive subsequent record mutation.
std::vector<AcceptedIdentity> accepted_identities(Records const& records)
{
  std::vector<AcceptedIdentity> accepted;
  for (auto const& record : records)
  {
    if (record.accepted_text)
    {
      accepted.push_back(AcceptedIdentity{.item_index = record.item_index, .block_index = record.block.block_index, .block_identity = record.block_identity});
    }
  }
  return accepted;
}

// TUI-thread pure comparison of accepted identities/order; geometry changes invalidate the corresponding
// presentation.
bool same_accepted(std::vector<AcceptedIdentity> const& left, std::vector<AcceptedIdentity> const& right)
{
  return left.size() == right.size() && std::ranges::equal(left, right, [](AcceptedIdentity const& lhs, AcceptedIdentity const& rhs) {
           return lhs.item_index == rhs.item_index && lhs.block_index == rhs.block_index && lhs.block_identity == rhs.block_identity;
         });
}

// TUI-thread conservative lower bound across before/after accepted entries; search may rebuild earlier items to
// avoid missing moved output.
std::size_t earliest_difference(std::vector<AcceptedIdentity> const& before, std::vector<AcceptedIdentity> const& after)
{
  auto earliest = std::numeric_limits<std::size_t>::max();
  for (auto const& entry : before)
    earliest = std::min(earliest, entry.item_index);
  for (auto const& entry : after)
    earliest = std::min(earliest, entry.item_index);
  return earliest == std::numeric_limits<std::size_t>::max() ? std::size_t{0} : earliest;
}

template <typename Record>
// TUI-thread source/offset equality allowing records to survive transcript index movement without resubmitting
// helper work.
bool same_block(Record const& record, detail::MermaidFenceBlock const& block)
{
  return record.block.block_index == block.block_index && record.block.fence_start == block.fence_start && record.block.fence_end == block.fence_end &&
         record.block.source == block.source;
}

}  // namespace

RuntimeMermaidPresentationController::RuntimeMermaidPresentationController(RuntimePresentationState& presentation, RuntimeRenderer& renderer,
                                                                           TranscriptSearchController& transcript_search, TuiMermaidRenderBridge bridge)
    : presentation_(presentation),
      renderer_(renderer),
      transcript_search_(transcript_search),
      bridge_(std::move(bridge)),
      config_epoch_(bridge_.config_epoch),
      enabled_(bridge_.enabled)
{
}

RuntimeMermaidPresentationController::~RuntimeMermaidPresentationController()
{
  shutdown();
}

void RuntimeMermaidPresentationController::attach_loop(TuiEventLoop& loop)
{
  // Supply set_completion_notifier before attaching any nonempty bridge, even
  // while disabled; otherwise accepted work can finish without waking its owner.
  ASSERT((!bridge_.enqueue && !bridge_.cancel && !bridge_.drain) || bridge_.set_completion_notifier);
  retry_timer_ = loop.member_timer(*this, &RuntimeMermaidPresentationController::mark_retry_due);
  if (bridge_.set_completion_notifier)
  {
    bridge_.set_completion_notifier([ready = completion_ready_, wakeup = loop.wakeup()]() noexcept {
      // Publish readiness before waking. Only shared/weak values cross threads;
      // a copied callback cannot reach a retired controller or terminal object.
      ready->store(true, std::memory_order_release);
      wakeup.notify();
    });
  }
  synchronize_retry_timer();
}

void RuntimeMermaidPresentationController::synchronize_retry_timer()
{
  if (shutdown_ || !bridge_busy_)
  {
    retry_timer_.cancel();
    retry_at_.reset();
    retry_delay_ = std::chrono::milliseconds(100);
  }
  else if (!retry_at_)
  {
    retry_at_ = TuiTimer::Clock::now() + retry_delay_;
    retry_timer_.arm_at(*retry_at_);
    retry_delay_ = std::chrono::milliseconds(250);
  }
}

void RuntimeMermaidPresentationController::mark_retry_due(TuiTimer::Clock::time_point)
{
  retry_at_.reset();
  retry_due_ = true;
}

MermaidServiceOutcome RuntimeMermaidPresentationController::service_if_changed()
{
  auto const completion_ready = completion_ready_->exchange(false, std::memory_order_acq_rel);
  auto const retry_due = std::exchange(retry_due_, false);
  if (shutdown_ || (!completion_ready && !retry_due && observed_snapshot_ && observed_transcript_generation_ == presentation_.screen().transcript_generation &&
                    session_id_ == presentation_.screen().session_id && config_epoch_ == presentation_.screen().mermaid_config_epoch &&
                    enabled_ == presentation_.screen().mermaid_enabled))
    return MermaidServiceOutcome::Unchanged;
  return service();
}

std::uint64_t RuntimeMermaidPresentationController::next_identity() noexcept
{
  auto identity = next_identity_++;
  if (identity == 0)
  {
    identity = next_identity_++;
    if (identity == 0)
      identity = 1;
  }
  return identity;
}

void RuntimeMermaidPresentationController::cancel_record(Record const& record) noexcept
{
  if (!record.pending || !bridge_.cancel)
    return;
  try
  {
    static_cast<void>(bridge_.cancel(record.request_identity, config_epoch_));
  }
  catch (...)
  {
  }
}

void RuntimeMermaidPresentationController::cancel_all() noexcept
{
  for (auto const& record : records_)
    cancel_record(record);
  records_.clear();
}

void RuntimeMermaidPresentationController::reconcile(ComposerSnapshot const& snapshot)
{
  // Temporary TUI-thread owned scanned fence awaiting reconciliation. Source/offsets are moved into retained
  // records before candidates are destroyed.
  struct Candidate
  {
    // Current TUI-thread index of the transcript item containing this candidate fence.
    std::size_t item_index = 0;
    // Owned completed fence/source from scan; reconciliation transfers it into a new record when no matching
    // record survives.
    detail::MermaidFenceBlock block;

    AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
  };
  // Scan completed fences across the full transcript, then trim the candidate set before reconciling records.
  // This limits retained records, not peak scan allocation; incomplete streaming fences remain ordinary text.
  std::vector<Candidate> candidates;
  for (std::size_t item_index = 0; item_index < snapshot.transcript.size(); ++item_index)
  {
    auto blocks = detail::scan_completed_assistant_mermaid_fences(snapshot.transcript[item_index]);
    for (auto& block : blocks)
      candidates.push_back(Candidate{.item_index = item_index, .block = std::move(block)});
  }
  if (candidates.size() > detail::kMaxMermaidPresentationEntries)
  {
    candidates.erase(candidates.begin(), candidates.begin() + static_cast<std::ptrdiff_t>(candidates.size() - detail::kMaxMermaidPresentationEntries));
  }

  // Match stable source/offset identity before allocating requests so item movement does not resubmit work or
  // lose accepted output.
  std::vector<Record> next;
  next.reserve(candidates.size());
  std::vector<bool> retained(records_.size(), false);
  std::size_t previous_item_index = std::numeric_limits<std::size_t>::max();
  std::uint64_t item_identity = 0;
  for (auto& candidate : candidates)
  {
    auto existing = std::size_t{0};
    for (; existing < records_.size(); ++existing)
    {
      if (!retained[existing] && same_block(records_[existing], candidate.block))
        break;
    }
    if (existing < records_.size())
    {
      retained[existing] = true;
      auto retained_record = std::move(records_[existing]);
      retained_record.item_index = candidate.item_index;
      next.push_back(std::move(retained_record));
      continue;
    }

    if (candidate.item_index != previous_item_index)
    {
      previous_item_index = candidate.item_index;
      item_identity = next_identity();
      auto prior_same_item = std::ranges::find_if(next, [&](Record const& record) { return record.item_index == candidate.item_index; });
      if (prior_same_item != next.end())
        item_identity = prior_same_item->item_identity;
    }
    auto const block_identity = next_identity();
    next.push_back(Record{.item_identity = item_identity,
                          .block_identity = block_identity,
                          .request_identity = block_identity,
                          .item_index = candidate.item_index,
                          .block = std::move(candidate.block)});
  }
  // Cancel removed records after matching all survivors; outstanding identity bookkeeping remains until their
  // results drain.
  for (std::size_t index = 0; index < records_.size(); ++index)
  {
    if (!retained[index])
      cancel_record(records_[index]);
  }
  records_ = std::move(next);
}

void RuntimeMermaidPresentationController::enqueue_waiting() noexcept
{
  if (!enabled_ || !bridge_.enqueue)
    return;
  auto pending = static_cast<std::size_t>(std::ranges::count_if(records_, &Record::pending));
  for (auto& record : records_)
  {
    if (pending >= 32 || record.submitted || record.failed || record.accepted_text)
      continue;
    try
    {
      // Allocate bookkeeping before submission: an accepted backend request must
      // remain discoverable even if its presentation record is later removed.
      outstanding_requests_.reserve(outstanding_requests_.size() + 1);
      auto const result =
          bridge_.enqueue(TuiMermaidRenderRequest{.identity = record.request_identity, .config_epoch = config_epoch_, .source = record.block.source});
      if (result == TuiMermaidEnqueueResult::Accepted)
      {
        record.submitted = true;
        record.pending = true;
        outstanding_requests_.emplace_back(config_epoch_, record.request_identity);
        ++pending;
      }
      else if (result == TuiMermaidEnqueueResult::Busy)
      {
        bridge_busy_ = true;
        return;
      }
      else if (result == TuiMermaidEnqueueResult::QueueFull)
      {
        // A completion releases capacity and supplies the next notification.
        return;
      }
      else if (result == TuiMermaidEnqueueResult::Rejected)
      {
        record.submitted = true;
        record.failed = true;
      }
    }
    catch (...)
    {
      record.submitted = true;
      record.failed = true;
    }
  }
}

void RuntimeMermaidPresentationController::drain_completions() noexcept
{
  if (!bridge_.drain)
    return;
  std::vector<TuiMermaidRenderCompletion> completions;
  try
  {
    auto drained = bridge_.drain();
    if (!drained)
    {
      bridge_busy_ = true;
      return;
    }
    completions = std::move(*drained);
  }
  catch (...)
  {
    return;
  }
  // Release bridge-slot bookkeeping even for stale/removed records. Only current-epoch pending records may
  // acquire bounded output.
  for (auto& completion : completions)
  {
    std::erase(outstanding_requests_, std::pair{completion.config_epoch, completion.identity});
    if (completion.config_epoch != config_epoch_)
      continue;
    auto record = std::ranges::find(records_, completion.identity, &Record::request_identity);
    if (record == records_.end() || !record->pending)
      continue;
    record->pending = false;
    if (!completion.accepted || completion.text.empty() || completion.text.size() > detail::kMaxMermaidPresentationOutputBytes)
    {
      record->failed = true;
      continue;
    }
    record->accepted_text = std::move(completion.text);
  }
}

void RuntimeMermaidPresentationController::enforce_result_byte_bound() noexcept
{
  std::size_t bytes = 0;
  for (auto record = records_.rbegin(); record != records_.rend(); ++record)
  {
    if (!record->accepted_text)
      continue;
    if (record->accepted_text->size() > detail::kMaxMermaidPresentationBytes - bytes)
    {
      record->accepted_text.reset();
      record->failed = true;
      continue;
    }
    bytes += record->accepted_text->size();
  }
}

std::vector<detail::MermaidAcceptedPresentation> RuntimeMermaidPresentationController::accepted_presentations() const
{
  std::vector<detail::MermaidAcceptedPresentation> accepted;
  accepted.reserve(records_.size());
  for (auto const& record : records_)
  {
    if (!record.accepted_text)
      continue;
    accepted.push_back(detail::MermaidAcceptedPresentation{.config_epoch = config_epoch_,
                                                           .item_identity = record.item_identity,
                                                           .block_identity = record.block_identity,
                                                           .item_index = record.item_index,
                                                           .block_index = record.block.block_index,
                                                           .fence_start = record.block.fence_start,
                                                           .fence_end = record.block.fence_end,
                                                           .source = record.block.source,
                                                           .text = *record.accepted_text});
  }
  return accepted;
}

MermaidServiceOutcome RuntimeMermaidPresentationController::service()
{
  auto const update = renderer_.begin_transcript_update();
  auto const changed_item = service_projection();
  synchronize_retry_timer();
  if (!changed_item)
    return MermaidServiceOutcome::Unchanged;
  transcript_search_.refresh_after_transcript_mutation(0, *changed_item);
  if (update.detached())
  {
    renderer_.complete_transcript_update(update, 0, 0);
    renderer_.synchronize_detached_transcript_layout();
  }
  return renderer_.request_render(FrameRenderKind::Full) ? MermaidServiceOutcome::Changed : MermaidServiceOutcome::RenderFailed;
}

std::optional<std::size_t> RuntimeMermaidPresentationController::service_projection()
{
  auto const& snapshot = presentation_.screen();
  if (shutdown_)
    return std::nullopt;
  auto const before = accepted_identities(records_);

  // Retire prior session/configuration records before discovering new fences; same-epoch canceled completions
  // still require draining.
  auto const session_changed = session_id_ != snapshot.session_id;
  auto const configuration_changed = config_epoch_ != snapshot.mermaid_config_epoch || enabled_ != snapshot.mermaid_enabled;
  if (session_changed || configuration_changed)
  {
    cancel_all();
    // Application reconfiguration drops the backend's prior epoch jobs and
    // results. Same-epoch session changes still need to drain canceled work.
    if (config_epoch_ != snapshot.mermaid_config_epoch)
      outstanding_requests_.clear();
    session_id_ = snapshot.session_id;
    config_epoch_ = snapshot.mermaid_config_epoch;
    enabled_ = snapshot.mermaid_enabled;
    observed_transcript_generation_ = 0;
  }

  if (!enabled_ || config_epoch_ == 0)
  {
    cancel_all();
  }
  else if (observed_transcript_generation_ != snapshot.transcript_generation || records_.empty())
  {
    reconcile(snapshot);
  }

  // Keep Busy backoff even when unrelated input changes the transcript. On an
  // eligible attempt, drain canceled/old-session results before submitting more
  // work. Disabled presentation must also release those coordinator slots.
  if (!retry_at_ || TuiTimer::Clock::now() >= *retry_at_)
  {
    retry_at_.reset();
    bridge_busy_ = false;
    drain_completions();
    if (!bridge_busy_ && enabled_ && config_epoch_ != 0)
    {
      enqueue_waiting();
      // Enqueue may publish cache/fallback results synchronously.
      if (!bridge_busy_)
        drain_completions();
      enforce_result_byte_bound();
    }
  }

  // Compare accepted identity sets only after submission/drain and byte-budget enforcement, then publish
  // geometry before notifying search of affected items.
  auto const after = accepted_identities(records_);
  auto const visual_changed = !same_accepted(before, after) || (snapshot.mermaid_projection && after.empty());
  if (visual_changed)
  {
    auto accepted = accepted_presentations();
    // Immutable snapshot copies share these bounded accepted results; helper
    // process ownership remains with the external bridge.
    presentation_.publish_mermaid(accepted.empty() ? std::shared_ptr<detail::MermaidTranscriptProjection const>{}
                                                   : std::make_shared<detail::MermaidTranscriptProjection const>(config_epoch_, std::move(accepted)));
  }
  observed_transcript_generation_ = snapshot.transcript_generation;
  observed_snapshot_ = true;
  return visual_changed ? std::optional{earliest_difference(before, after)} : std::nullopt;
}

void RuntimeMermaidPresentationController::shutdown() noexcept
{
  if (shutdown_)
    return;
  shutdown_ = true;
  // Retire TUI retry callbacks first. Unregistration prevents future stored callbacks, but independently copied
  // shared/weak notifications may safely finish later.
  retry_timer_.cancel();
  retry_at_.reset();
  if (bridge_.set_completion_notifier)
  {
    try
    {
      bridge_.set_completion_notifier({});
    }
    catch (...)
    {
      // A retained callback still owns only its atomic and a weak wake capability.
      try
      {
        Dout(dc::warning, "Mermaid notification retirement failed");
      }
      catch (...)
      { /* Diagnostics must not escape noexcept teardown. */
      }
    }
  }
  // Cancellation can still create application completions; controller teardown releases local records and
  // callback ownership after notification is retired.
  cancel_all();
  outstanding_requests_.clear();
  bridge_ = {};
}

}  // namespace ava::tui

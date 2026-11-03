#include "sys.h"
#include "ava/tui/runtime_mermaid_internal.h"
#include "ava/tui/runtime_render_internal.h"
#include "ava/tui/runtime_transcript_search_internal.h"

#include <algorithm>
#include <limits>
#include <memory>
#include <ranges>
#include <utility>

namespace ava::tui {
namespace {

struct AcceptedIdentity
{
  std::size_t item_index = 0;
  std::size_t block_index = 0;
  std::uint64_t block_identity = 0;

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

template <typename Records>
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

bool same_accepted(std::vector<AcceptedIdentity> const& left, std::vector<AcceptedIdentity> const& right)
{
  return left.size() == right.size() && std::ranges::equal(left, right, [](AcceptedIdentity const& lhs, AcceptedIdentity const& rhs) {
           return lhs.item_index == rhs.item_index && lhs.block_index == rhs.block_index && lhs.block_identity == rhs.block_identity;
         });
}

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
bool same_block(Record const& record, detail::MermaidFenceBlock const& block)
{
  return record.block.block_index == block.block_index && record.block.fence_start == block.fence_start && record.block.fence_end == block.fence_end &&
         record.block.source == block.source;
}

}  // namespace

RuntimeMermaidPresentationController::RuntimeMermaidPresentationController(ComposerSnapshot& snapshot, RuntimeRenderer& renderer,
                                                                           TranscriptSearchController& transcript_search, TuiMermaidRenderBridge bridge)
    : snapshot_(snapshot),
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
  struct Candidate
  {
    std::size_t item_index = 0;
    detail::MermaidFenceBlock block;

    AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
  };
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
      auto const result =
          bridge_.enqueue(TuiMermaidRenderRequest{.identity = record.request_identity, .config_epoch = config_epoch_, .source = record.block.source});
      if (result == TuiMermaidEnqueueResult::Accepted)
      {
        record.submitted = true;
        record.pending = true;
        ++pending;
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
    completions = bridge_.drain();
  }
  catch (...)
  {
    return;
  }
  for (auto& completion : completions)
  {
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
  auto old_anchor = detail::TranscriptViewportAnchor{};
  auto const preserve_viewport = renderer_.transcript().scroll_offset() > 0;
  if (preserve_viewport && !renderer_.transcript().has_deferred_update())
    old_anchor = renderer_.capture_transcript_viewport();
  auto const changed_item = service_projection();
  if (!changed_item)
    return MermaidServiceOutcome::Unchanged;
  transcript_search_.refresh_after_transcript_mutation(0, *changed_item);
  if (preserve_viewport)
  {
    renderer_.transcript().defer_update(old_anchor, 0);
    renderer_.synchronize_detached_transcript_layout();
  }
  return renderer_.request_render(FrameRenderKind::Full) ? MermaidServiceOutcome::Changed : MermaidServiceOutcome::RenderFailed;
}

std::optional<std::size_t> RuntimeMermaidPresentationController::service_projection()
{
  auto& snapshot = snapshot_;
  if (shutdown_)
    return std::nullopt;
  auto const before = accepted_identities(records_);

  auto const session_changed = session_id_ != snapshot.session_id;
  auto const configuration_changed = config_epoch_ != snapshot.mermaid_config_epoch || enabled_ != snapshot.mermaid_enabled;
  if (session_changed || configuration_changed)
  {
    cancel_all();
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

  if (enabled_ && config_epoch_ != 0)
  {
    enqueue_waiting();
    drain_completions();
    enforce_result_byte_bound();
  }

  auto const after = accepted_identities(records_);
  auto const visual_changed = !same_accepted(before, after) || (snapshot.mermaid_projection && after.empty());
  if (visual_changed)
  {
    auto accepted = accepted_presentations();
    // Immutable snapshot copies share these bounded accepted results; helper
    // process ownership remains with the external bridge.
    snapshot.mermaid_projection = accepted.empty() ? std::shared_ptr<detail::MermaidTranscriptProjection const>{}
                                                   : std::make_shared<detail::MermaidTranscriptProjection const>(config_epoch_, std::move(accepted));
    ++snapshot.transcript_generation;
  }
  observed_transcript_generation_ = snapshot.transcript_generation;
  return visual_changed ? std::optional{earliest_difference(before, after)} : std::nullopt;
}

void RuntimeMermaidPresentationController::shutdown() noexcept
{
  if (shutdown_)
    return;
  shutdown_ = true;
  cancel_all();
  bridge_ = {};
}

}  // namespace ava::tui

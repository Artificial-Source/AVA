// Serializes Mermaid configuration/lifetime while exposing non-I/O admission, drainage, and notification callbacks.
#include "sys.h"
#include "ava/app/tui/mermaid_tui_bridge.h"

#include <limits>
#include <type_traits>
#include <utility>

namespace ava::app {
namespace {

// Application-thread pure adapter: copy validated display settings into coordinator configuration with caller-
// owned epoch.
MermaidRenderConfiguration coordinator_config(MermaidDisplaySettings const& settings, std::uint64_t epoch)
{
  return mermaid_render_configuration_from_display_settings(settings, epoch);
}

// Pure comparison while callers hold bridge settings stable; unchanged enable/argv avoids invalidating cached
// results.
bool same_effective_settings(MermaidDisplaySettings const& left, MermaidDisplaySettings const& right)
{
  return left.enabled == right.enabled && left.argv == right.argv;
}

}  // namespace

std::shared_ptr<MermaidTuiBridge> MermaidTuiBridge::create(MermaidDisplaySettings const& settings)
{
  constexpr auto kInitialEpoch = std::uint64_t{1};
  auto coordinator = MermaidRenderCoordinator::create(coordinator_config(settings, kInitialEpoch));
  if (!coordinator)
    throw std::move(coordinator.error());
  return std::shared_ptr<MermaidTuiBridge>(new MermaidTuiBridge{std::move(*coordinator), settings, kInitialEpoch});
}

MermaidTuiBridge::MermaidTuiBridge(std::unique_ptr<MermaidRenderCoordinator> coordinator, MermaidDisplaySettings settings, std::uint64_t epoch)
    : coordinator_(std::move(coordinator)), settings_(std::move(settings)), epoch_(epoch)
{
}

MermaidTuiBridge::~MermaidTuiBridge()
{
  shutdown();
}

ava::core::VoidResult MermaidTuiBridge::apply(MermaidDisplaySettings const& settings)
{
  // Reconfiguration never invokes a notifier, so this lock can keep settings
  // and epoch publication atomic. Prepare the fallible copy before changing the
  // coordinator; committing the prepared settings below only moves owned values.
  std::lock_guard lock(mutex_);
  if (shutdown_ || !coordinator_)
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Unknown, "Mermaid TUI bridge is shut down"));
  if (same_effective_settings(settings_, settings))
    return {};
  if (epoch_ == std::numeric_limits<std::uint64_t>::max())
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Unknown, "Mermaid configuration epoch is exhausted"));
  auto prepared_settings = settings;
  static_assert(std::is_nothrow_move_assignable_v<MermaidDisplaySettings>);
  auto const next_epoch = epoch_ + 1;
  auto reconfigured = coordinator_->reconfigure(coordinator_config(settings, next_epoch));
  if (!reconfigured)
    return std::unexpected(reconfigured.error());
  settings_ = std::move(prepared_settings);
  epoch_ = next_epoch;
  return {};
}

MermaidTuiPresentationConfiguration MermaidTuiBridge::presentation_configuration() const noexcept
{
  std::lock_guard lock(mutex_);
  return MermaidTuiPresentationConfiguration{.epoch = epoch_, .enabled = !shutdown_ && settings_.enabled};
}

ava::tui::TuiMermaidRenderBridge MermaidTuiBridge::tui_bridge()
{
  // Each exported callback retains shared adapter ownership so application coordinator lifetime does not depend
  // on the TUI controller remaining alive.
  auto self = shared_from_this();
  auto const configuration = presentation_configuration();
  return ava::tui::TuiMermaidRenderBridge{
      .config_epoch = configuration.epoch,
      .enabled = configuration.enabled,
      .enqueue = [self](ava::tui::TuiMermaidRenderRequest request) { return self->enqueue(std::move(request)); },
      .cancel = [self](std::uint64_t identity, std::uint64_t config_epoch) { return self->cancel(identity, config_epoch); },
      .drain = [self] { return self->drain(); },
      .set_completion_notifier = [self](std::function<void()> notifier) { self->set_completion_notifier(std::move(notifier)); },
  };
}

ava::tui::TuiMermaidEnqueueResult MermaidTuiBridge::enqueue(ava::tui::TuiMermaidRenderRequest request) noexcept
{
  // Expose bridge contention as Busy. The coordinator is never replaced, and
  // this callback's shared bridge owner keeps it alive after releasing the lock.
  std::unique_lock lock(mutex_, std::try_to_lock);
  if (!lock.owns_lock())
    return ava::tui::TuiMermaidEnqueueResult::Busy;
  if (shutdown_ || !coordinator_ || request.config_epoch != epoch_ || !settings_.enabled)
    return ava::tui::TuiMermaidEnqueueResult::Rejected;
  auto* coordinator = coordinator_.get();
  lock.unlock();
  auto result = MermaidEnqueueResult::StaleEpoch;
  try
  {
    result =
        coordinator->enqueue(MermaidRenderRequest{.identity = request.identity, .config_epoch = request.config_epoch, .source = std::move(request.source)});
  }
  catch (...)
  {
    return ava::tui::TuiMermaidEnqueueResult::Rejected;
  }
  switch (result)
  {
    case MermaidEnqueueResult::Queued:
    case MermaidEnqueueResult::AttachedToExisting:
    case MermaidEnqueueResult::CompletedFromCache:
    case MermaidEnqueueResult::CompletedFallback:
      return ava::tui::TuiMermaidEnqueueResult::Accepted;
    case MermaidEnqueueResult::QueueFull:
      return ava::tui::TuiMermaidEnqueueResult::QueueFull;
    case MermaidEnqueueResult::StaleEpoch:
      return ava::tui::TuiMermaidEnqueueResult::Rejected;
  }
  return ava::tui::TuiMermaidEnqueueResult::Rejected;
}

bool MermaidTuiBridge::cancel(std::uint64_t identity, std::uint64_t config_epoch) noexcept
{
  std::unique_lock lock(mutex_, std::try_to_lock);
  if (!lock.owns_lock() || shutdown_ || !coordinator_ || config_epoch != epoch_)
    return false;
  // Cancellation can publish synchronously. Release bridge serialization before
  // its notifier runs; the coordinator rechecks the epoch under its own mutex.
  auto* coordinator = coordinator_.get();
  lock.unlock();
  try
  {
    return coordinator->cancel(identity, config_epoch);
  }
  catch (...)
  {
    return false;
  }
}

std::optional<std::vector<ava::tui::TuiMermaidRenderCompletion>> MermaidTuiBridge::drain() noexcept
{
  try
  {
    // Retry contention or allocation failure without consuming results. Reserve
    // the coordinator's entire bounded capacity before taking any completion;
    // otherwise allocation after removal could lose an admitted identity.
    std::unique_lock lock(mutex_, std::try_to_lock);
    if (!lock.owns_lock())
      return std::nullopt;
    if (shutdown_ || !coordinator_)
      return std::vector<ava::tui::TuiMermaidRenderCompletion>{};
    std::vector<ava::tui::TuiMermaidRenderCompletion> projected;
    projected.reserve(kMaxMermaidQueuedRequests);
    auto completions = coordinator_->take_completions();
    // String ownership moves into reserved slots. Once results leave the queue,
    // this conversion and the returned vector's move must not allocate or throw.
    static_assert(std::is_nothrow_move_constructible_v<ava::tui::TuiMermaidRenderCompletion>);
    for (auto& completion : completions)
    {
      auto const accepted = completion.outcome == MermaidRenderOutcome::Accepted;
      projected.push_back(ava::tui::TuiMermaidRenderCompletion{.identity = completion.identity,
                                                               .config_epoch = completion.config_epoch,
                                                               .accepted = accepted,
                                                               .text = accepted ? std::move(completion.text) : std::string{}});
    }
    return projected;
  }
  catch (...)
  {
    return std::nullopt;
  }
}

void MermaidTuiBridge::set_completion_notifier(std::function<void()> notifier)
{
  // Registration may immediately notify an existing backlog. The stable borrow
  // survives unlock; coordinator registration rejects a concurrent shutdown.
  MermaidRenderCoordinator* coordinator;
  {
    std::lock_guard lock(mutex_);
    if (shutdown_ || !coordinator_)
      return;
    coordinator = coordinator_.get();
  }
  coordinator->set_completion_notifier(std::move(notifier));
}

void MermaidTuiBridge::shutdown() noexcept
{
  MermaidRenderCoordinator* coordinator;
  {
    std::lock_guard lock(mutex_);
    if (shutdown_)
      return;
    shutdown_ = true;
    coordinator = coordinator_.get();
  }
  // Close admission before joining, but release the bridge mutex first: a
  // finishing worker notifier may query this bridge before it can return.
  if (coordinator)
    coordinator->shutdown();
}

}  // namespace ava::app

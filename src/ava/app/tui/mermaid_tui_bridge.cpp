#include "sys.h"
#include "ava/app/tui/mermaid_tui_bridge.h"

#include <limits>
#include <utility>

namespace ava::app {
namespace {

MermaidRenderConfiguration coordinator_config(MermaidDisplaySettings const& settings, std::uint64_t epoch)
{
  return mermaid_render_configuration_from_display_settings(settings, epoch);
}

bool same_effective_settings(MermaidDisplaySettings const& left, MermaidDisplaySettings const& right)
{
  return left.enabled == right.enabled && left.argv == right.argv;
}

}  // namespace

ava::core::Result<std::shared_ptr<MermaidTuiBridge>> MermaidTuiBridge::create(MermaidDisplaySettings const& settings)
{
  constexpr auto kInitialEpoch = std::uint64_t{1};
  auto coordinator = MermaidRenderCoordinator::create(coordinator_config(settings, kInitialEpoch));
  if (!coordinator)
    return std::unexpected(coordinator.error());
  return std::shared_ptr<MermaidTuiBridge>(new MermaidTuiBridge(std::move(*coordinator), settings, kInitialEpoch));
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
  std::lock_guard lock(mutex_);
  if (shutdown_ || !coordinator_)
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Unknown, "Mermaid TUI bridge is shut down"));
  if (same_effective_settings(settings_, settings))
    return {};
  if (epoch_ == std::numeric_limits<std::uint64_t>::max())
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Unknown, "Mermaid configuration epoch is exhausted"));
  auto const next_epoch = epoch_ + 1;
  auto reconfigured = coordinator_->reconfigure(coordinator_config(settings, next_epoch));
  if (!reconfigured)
    return std::unexpected(reconfigured.error());
  settings_ = settings;
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
  auto self = shared_from_this();
  auto const configuration = presentation_configuration();
  return ava::tui::TuiMermaidRenderBridge{
      .config_epoch = configuration.epoch,
      .enabled = configuration.enabled,
      .enqueue = [self](ava::tui::TuiMermaidRenderRequest request) { return self->enqueue(std::move(request)); },
      .cancel = [self](std::uint64_t identity, std::uint64_t config_epoch) { return self->cancel(identity, config_epoch); },
      .drain = [self] { return self->drain(); },
  };
}

ava::tui::TuiMermaidEnqueueResult MermaidTuiBridge::enqueue(ava::tui::TuiMermaidRenderRequest request) noexcept
{
  std::unique_lock lock(mutex_, std::try_to_lock);
  if (!lock.owns_lock())
    return ava::tui::TuiMermaidEnqueueResult::QueueFull;
  if (shutdown_ || !coordinator_ || request.config_epoch != epoch_ || !settings_.enabled)
    return ava::tui::TuiMermaidEnqueueResult::Rejected;
  auto result = MermaidEnqueueResult::StaleEpoch;
  try
  {
    result =
        coordinator_->enqueue(MermaidRenderRequest{.identity = request.identity, .config_epoch = request.config_epoch, .source = std::move(request.source)});
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
  try
  {
    return coordinator_->cancel(identity, config_epoch);
  }
  catch (...)
  {
    return false;
  }
}

std::vector<ava::tui::TuiMermaidRenderCompletion> MermaidTuiBridge::drain() noexcept
{
  std::unique_lock lock(mutex_, std::try_to_lock);
  if (!lock.owns_lock() || shutdown_ || !coordinator_)
    return {};
  std::vector<MermaidRenderCompletion> completions;
  try
  {
    completions = coordinator_->take_completions();
  }
  catch (...)
  {
    return {};
  }
  std::vector<ava::tui::TuiMermaidRenderCompletion> projected;
  projected.reserve(completions.size());
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

void MermaidTuiBridge::shutdown() noexcept
{
  std::lock_guard lock(mutex_);
  if (shutdown_)
    return;
  shutdown_ = true;
  if (coordinator_)
    coordinator_->shutdown();
}

}  // namespace ava::app

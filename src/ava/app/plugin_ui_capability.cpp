#include "sys.h"
#include "ava/app/plugin_ui_capability.h"
#include "ava/core/json.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace ava::app {
namespace {

constexpr std::size_t kCanonicalPluginCommandMaxBytes = 64 * 1024;

ava::core::Error capability_creation_error()
{
  return ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "plugin UI capability could not be created");
}

ava::core::Error capability_unavailable_error()
{
  return ava::core::Error(ava::core::ErrorCategory::PermissionDenied, "plugin UI capability is unavailable");
}

bool callback_canceled(ava::plugin::CancelCallback const& cancel_requested) noexcept
{
  try
  {
    return cancel_requested && cancel_requested();
  }
  catch (...)
  {
    return true;
  }
}

bool valid_plugin_id(std::string_view id)
{
  if (id.empty() || id.size() > 128)
    return false;
  bool last_was_separator = false;
  for (char const ch : id)
  {
    bool const separator = ch == '.' || ch == '_' || ch == '-';
    bool const allowed = (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || separator;
    if (!allowed || (separator && last_was_separator))
      return false;
    last_was_separator = separator;
  }
  return !last_was_separator;
}

bool valid_command_name(std::string_view name)
{
  if (name.empty() || name.size() > 96)
    return false;
  return std::ranges::all_of(
      name, [](char ch) { return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '_' || ch == '-' || ch == '.'; });
}

struct CanonicalPluginCommand
{
  std::string plugin_id;
  std::string command_name;
};

std::optional<CanonicalPluginCommand> parse_canonical_plugin_command(std::string_view command)
{
  constexpr std::string_view prefix = "/plugin run ";
  if (command.size() > kCanonicalPluginCommandMaxBytes || !command.starts_with(prefix) || command.size() == prefix.size() ||
      std::ranges::any_of(command, [](unsigned char ch) { return ch < 0x20U || ch == 0x7FU; }))
  {
    return std::nullopt;
  }
  auto remaining = command.substr(prefix.size());
  auto const plugin_end = remaining.find(' ');
  if (plugin_end == std::string_view::npos || plugin_end == 0)
    return std::nullopt;
  auto const plugin_id = remaining.substr(0, plugin_end);
  remaining.remove_prefix(plugin_end + 1);
  if (remaining.empty() || remaining.front() == ' ')
    return std::nullopt;

  auto const command_end = remaining.find(' ');
  auto const command_name = remaining.substr(0, command_end);
  if (!valid_plugin_id(plugin_id) || !valid_command_name(command_name))
    return std::nullopt;
  if (command_end != std::string_view::npos)
  {
    auto const arguments = remaining.substr(command_end + 1);
    if (arguments.empty() || arguments.front() != '{' || arguments.back() != '}' || !ava::core::json::is_valid_object(arguments))
      return std::nullopt;
  }
  return CanonicalPluginCommand{.plugin_id = std::string(plugin_id), .command_name = std::string(command_name)};
}

}  // namespace

class PluginUiInvocationCapability::Impl final
{
 public:
  enum class State
  {
    Available,
    Claimed,
    Closed,
  };

  Impl(std::string canonical_command, PluginUiInvocationBinding binding, std::chrono::steady_clock::time_point deadline,
       std::weak_ptr<void const> runtime_token, PluginUiPresenter presenter, PluginUiPresenterClose close_presenter)
      : canonical_command_(std::move(canonical_command)),
        binding_(std::move(binding)),
        deadline_(deadline),
        runtime_token_(std::move(runtime_token)),
        presenter_(std::move(presenter)),
        close_presenter_(std::move(close_presenter))
  {
  }

  [[nodiscard]] bool claim(std::string_view canonical_command, std::string_view plugin_id, std::string_view command_name)
  {
    PluginUiPresenterClose close_presenter;
    bool claimed = false;
    {
      std::lock_guard lock(mutex_);
      auto const runtime = runtime_token_.lock();
      claimed = state_ == State::Available && canonical_command == canonical_command_ && plugin_id == binding_.plugin_id &&
                command_name == binding_.command_name && std::chrono::steady_clock::now() < deadline_ && runtime && presenter_;
      if (claimed)
      {
        state_ = State::Claimed;
      }
      else
      {
        take_close_locked(close_presenter);
      }
    }
    notify_close(std::move(close_presenter), binding_);
    return claimed;
  }

  [[nodiscard]] std::chrono::steady_clock::time_point deadline() const noexcept { return deadline_; }

  [[nodiscard]] ava::core::Result<ava::plugin::PluginUiAction> present(ava::plugin::PluginUiRequest const& request,
                                                                       std::chrono::steady_clock::time_point handler_deadline,
                                                                       ava::plugin::CancelCallback cancel_requested)
  {
    if (auto valid = ava::plugin::validate_plugin_ui_request(request); !valid)
    {
      close();
      return std::unexpected(capability_unavailable_error());
    }
    bool accepted = false;
    {
      std::lock_guard lock(mutex_);
      accepted = state_ == State::Claimed && !presenting_ && handler_deadline == deadline_ && std::chrono::steady_clock::now() < deadline_ &&
                 !runtime_token_.expired();
      if (accepted)
        presenting_ = true;
    }
    auto authority_canceled = [this, cancel_requested] {
      if (callback_canceled(cancel_requested) || std::chrono::steady_clock::now() >= deadline_)
        return true;
      std::lock_guard lock(mutex_);
      return state_ != State::Claimed || runtime_token_.expired();
    };
    if (!accepted || authority_canceled())
    {
      close();
      return std::unexpected(capability_unavailable_error());
    }

    ava::core::Result<ava::plugin::PluginUiAction> action = [&]() -> ava::core::Result<ava::plugin::PluginUiAction> {
      try
      {
        PluginUiPresentationRequest presentation{.binding = binding_, .request = request};
        return presenter_(presentation, deadline_, authority_canceled);
      }
      catch (...)
      {
        return std::unexpected(capability_unavailable_error());
      }
    }();

    bool still_claimed = false;
    {
      std::lock_guard lock(mutex_);
      presenting_ = false;
      still_claimed = state_ == State::Claimed && !runtime_token_.expired();
    }
    if (!action || !still_claimed || authority_canceled())
    {
      close();
      return std::unexpected(capability_unavailable_error());
    }
    if (auto valid = ava::plugin::validate_plugin_ui_action(request, *action); !valid)
    {
      close();
      return std::unexpected(capability_unavailable_error());
    }
    return action;
  }

  void close() noexcept
  {
    PluginUiPresenterClose close_presenter;
    {
      std::lock_guard lock(mutex_);
      take_close_locked(close_presenter);
    }
    notify_close(std::move(close_presenter), binding_);
  }

 private:
  void take_close_locked(PluginUiPresenterClose& close_presenter)
  {
    if (state_ == State::Closed)
      return;
    state_ = State::Closed;
    close_presenter = std::move(close_presenter_);
  }

  static void notify_close(PluginUiPresenterClose close_presenter, PluginUiInvocationBinding const& binding) noexcept
  {
    if (!close_presenter)
      return;
    try
    {
      close_presenter(binding);
    }
    catch (...)
    {
    }
  }

  mutable std::mutex mutex_;
  std::string canonical_command_;
  PluginUiInvocationBinding binding_;
  std::chrono::steady_clock::time_point deadline_;
  std::weak_ptr<void const> runtime_token_;
  PluginUiPresenter presenter_;
  PluginUiPresenterClose close_presenter_;
  State state_ = State::Available;
  bool presenting_ = false;
};

PluginUiInvocationCapability::PluginUiInvocationCapability(ConstructionKey, std::unique_ptr<Impl> impl) : impl_(std::move(impl))
{
}

PluginUiInvocationCapability::~PluginUiInvocationCapability()
{
  if (impl_)
    impl_->close();
}

PluginUiInvocationGuard::PluginUiInvocationGuard(std::shared_ptr<PluginUiInvocationCapability> capability) : capability_(std::move(capability))
{
}

PluginUiInvocationGuard::~PluginUiInvocationGuard()
{
  if (capability_ && capability_->impl_)
    capability_->impl_->close();
}

PluginUiInvocationClaim::PluginUiInvocationClaim(std::shared_ptr<PluginUiInvocationCapability> capability) : capability_(std::move(capability))
{
}

PluginUiInvocationClaim::~PluginUiInvocationClaim()
{
  close();
}

PluginUiInvocationClaim::PluginUiInvocationClaim(PluginUiInvocationClaim&& other) noexcept : capability_(std::move(other.capability_))
{
}

PluginUiInvocationClaim& PluginUiInvocationClaim::operator=(PluginUiInvocationClaim&& other) noexcept
{
  if (this == &other)
    return *this;
  close();
  capability_ = std::move(other.capability_);
  return *this;
}

std::chrono::steady_clock::time_point PluginUiInvocationClaim::deadline() const noexcept
{
  return capability_ && capability_->impl_ ? capability_->impl_->deadline() : std::chrono::steady_clock::time_point{};
}

ava::plugin::PluginUiHandler PluginUiInvocationClaim::handler() const
{
  if (!capability_ || !capability_->impl_)
    return {};
  auto capability = capability_;
  return ava::plugin::PluginUiHandler{
      .deadline = capability->impl_->deadline(),
      .callback = [capability = std::move(capability)](ava::plugin::PluginUiRequest const& request, std::chrono::steady_clock::time_point deadline,
                                                       ava::plugin::CancelCallback cancel_requested) {
        return capability->impl_->present(request, deadline, std::move(cancel_requested));
      }};
}

void PluginUiInvocationClaim::close() noexcept
{
  if (!capability_)
    return;
  if (capability_->impl_)
    capability_->impl_->close();
  capability_.reset();
}

ava::core::Result<std::shared_ptr<PluginUiInvocationCapability>> make_tui_plugin_ui_invocation_capability(
    std::string canonical_command, std::string invocation_id, std::chrono::steady_clock::time_point deadline, std::weak_ptr<void const> runtime_token,
    PluginUiPresenter presenter, PluginUiPresenterClose close_presenter)
{
  auto parsed = parse_canonical_plugin_command(canonical_command);
  auto const now = std::chrono::steady_clock::now();
  auto const runtime = runtime_token.lock();
  if (!parsed || !ava::plugin::is_valid_plugin_ui_id(invocation_id) || deadline <= now || deadline - now > ava::plugin::kPluginUiCommandDeadlineMax ||
      !runtime || !presenter || !close_presenter)
  {
    return std::unexpected(capability_creation_error());
  }

  PluginUiInvocationBinding binding{
      .plugin_id = std::move(parsed->plugin_id), .command_name = std::move(parsed->command_name), .invocation_id = std::move(invocation_id)};
  auto impl = std::make_unique<PluginUiInvocationCapability::Impl>(std::move(canonical_command), std::move(binding), deadline, std::move(runtime_token),
                                                                   std::move(presenter), std::move(close_presenter));
  return std::make_shared<PluginUiInvocationCapability>(PluginUiInvocationCapability::ConstructionKey{}, std::move(impl));
}

ava::core::Result<PluginUiInvocationClaim> claim_plugin_ui_invocation_capability(std::shared_ptr<PluginUiInvocationCapability> const& capability,
                                                                                 std::string_view canonical_command, std::string_view plugin_id,
                                                                                 std::string_view command_name)
{
  if (!capability || !capability->impl_ || !capability->impl_->claim(canonical_command, plugin_id, command_name))
    return std::unexpected(capability_unavailable_error());
  return PluginUiInvocationClaim(capability);
}

}  // namespace ava::app

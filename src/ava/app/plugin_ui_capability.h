#pragma once

#include "ava/plugin/runner.h"
#include "ava/core/result.h"

#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace ava::app {

struct PluginUiInvocationBinding
{
  std::string plugin_id;
  std::string command_name;
  std::string invocation_id;

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

struct PluginUiPresentationRequest
{
  PluginUiInvocationBinding binding;
  ava::plugin::PluginUiRequest request;

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

using PluginUiPresenter = std::function<ava::core::Result<ava::plugin::PluginUiAction>(PluginUiPresentationRequest const&,
                                                                                       std::chrono::steady_clock::time_point, ava::plugin::CancelCallback)>;
using PluginUiPresenterClose = std::function<void(PluginUiInvocationBinding const&)>;

class PluginUiInvocationClaim;
class PluginUiInvocationGuard;

class PluginUiInvocationCapability final
{
  class Impl;
  class ConstructionKey
  {
    friend ava::core::Result<std::shared_ptr<PluginUiInvocationCapability>> make_tui_plugin_ui_invocation_capability(
        std::string canonical_command, std::string invocation_id, std::chrono::steady_clock::time_point deadline, std::weak_ptr<void const> runtime_token,
        PluginUiPresenter presenter, PluginUiPresenterClose close_presenter);
    ConstructionKey() = default;

   public:
    AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
  };

 public:
  PluginUiInvocationCapability(ConstructionKey, std::unique_ptr<Impl> impl);
  ~PluginUiInvocationCapability();

  PluginUiInvocationCapability(PluginUiInvocationCapability const&) = delete;
  PluginUiInvocationCapability& operator=(PluginUiInvocationCapability const&) = delete;
  PluginUiInvocationCapability(PluginUiInvocationCapability&&) = delete;
  PluginUiInvocationCapability& operator=(PluginUiInvocationCapability&&) = delete;

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT

 private:
  std::unique_ptr<Impl> impl_;

  friend ava::core::Result<std::shared_ptr<PluginUiInvocationCapability>> make_tui_plugin_ui_invocation_capability(
      std::string canonical_command, std::string invocation_id, std::chrono::steady_clock::time_point deadline, std::weak_ptr<void const> runtime_token,
      PluginUiPresenter presenter, PluginUiPresenterClose close_presenter);
  friend ava::core::Result<PluginUiInvocationClaim> claim_plugin_ui_invocation_capability(std::shared_ptr<PluginUiInvocationCapability> const& capability,
                                                                                          std::string_view canonical_command, std::string_view plugin_id,
                                                                                          std::string_view command_name);
  friend class PluginUiInvocationClaim;
  friend class PluginUiInvocationGuard;
};

// Closes an attached one-command authority on every return path, including
// failures that occur before the capability is eligible to be claimed.
class PluginUiInvocationGuard final
{
 public:
  explicit PluginUiInvocationGuard(std::shared_ptr<PluginUiInvocationCapability> capability);
  ~PluginUiInvocationGuard();

  PluginUiInvocationGuard(PluginUiInvocationGuard const&) = delete;
  PluginUiInvocationGuard& operator=(PluginUiInvocationGuard const&) = delete;
  PluginUiInvocationGuard(PluginUiInvocationGuard&&) = delete;
  PluginUiInvocationGuard& operator=(PluginUiInvocationGuard&&) = delete;

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT

 private:
  std::shared_ptr<PluginUiInvocationCapability> capability_;
};

class PluginUiInvocationClaim final
{
 public:
  ~PluginUiInvocationClaim();

  PluginUiInvocationClaim(PluginUiInvocationClaim const&) = delete;
  PluginUiInvocationClaim& operator=(PluginUiInvocationClaim const&) = delete;
  PluginUiInvocationClaim(PluginUiInvocationClaim&& other) noexcept;
  PluginUiInvocationClaim& operator=(PluginUiInvocationClaim&& other) noexcept;

  [[nodiscard]] std::chrono::steady_clock::time_point deadline() const noexcept;
  [[nodiscard]] ava::plugin::PluginUiHandler handler() const;
  void close() noexcept;

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT

 private:
  explicit PluginUiInvocationClaim(std::shared_ptr<PluginUiInvocationCapability> capability);

  std::shared_ptr<PluginUiInvocationCapability> capability_;

  friend ava::core::Result<PluginUiInvocationClaim> claim_plugin_ui_invocation_capability(std::shared_ptr<PluginUiInvocationCapability> const& capability,
                                                                                          std::string_view canonical_command, std::string_view plugin_id,
                                                                                          std::string_view command_name);
};

// Opaque TUI mint seam. The production source inventory permits exactly one
// foreground call site and keeps all non-TUI command paths default-null.
[[nodiscard]] ava::core::Result<std::shared_ptr<PluginUiInvocationCapability>> make_tui_plugin_ui_invocation_capability(
    std::string canonical_command, std::string invocation_id, std::chrono::steady_clock::time_point deadline, std::weak_ptr<void const> runtime_token,
    PluginUiPresenter presenter, PluginUiPresenterClose close_presenter);

[[nodiscard]] ava::core::Result<PluginUiInvocationClaim> claim_plugin_ui_invocation_capability(std::shared_ptr<PluginUiInvocationCapability> const& capability,
                                                                                               std::string_view canonical_command, std::string_view plugin_id,
                                                                                               std::string_view command_name);

}  // namespace ava::app

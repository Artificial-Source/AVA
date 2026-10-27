#pragma once

#include "ava/debug/print_members_on.h"
#include "ava/core/NoLogString.h"

#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace ava::tui {

inline constexpr std::size_t kTuiPluginUiMaxWidgets = 4;
inline constexpr std::size_t kTuiPluginUiMaxWidgetLines = 32;
inline constexpr std::size_t kTuiPluginUiMaxTextBytes = 8 * 1024;
inline constexpr std::size_t kTuiPluginUiMaxQueuedRecords = 64;
inline constexpr auto kTuiPluginUiInvocationDeadline = std::chrono::seconds(120);

struct TuiPluginUiBinding
{
  std::string plugin_id;
  ava::core::NoLogString command;
  std::string invocation_id;

  friend bool operator==(TuiPluginUiBinding const&, TuiPluginUiBinding const&) = default;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

enum class TuiPluginUiKind
{
  Status,
  Widget,
  Select,
  Confirm,
};

struct TuiPluginUiOption
{
  // The identifier is protocol data returned to the plugin. Host rendering
  // deliberately never includes it.
  std::string id;
  ava::core::NoLogString label;
  std::optional<ava::core::NoLogString> description;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

struct TuiPluginUiRequest
{
  TuiPluginUiBinding binding;
  // Correlation data for the coordinator only. It is never rendered.
  std::string request_id;
  TuiPluginUiKind kind = TuiPluginUiKind::Status;
  ava::core::NoLogString text;
  ava::core::NoLogString title;
  ava::core::NoLogString description;
  std::vector<ava::core::NoLogString> lines;
  std::vector<TuiPluginUiOption> options;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

// Pure host-chrome fit policy shared by the coordinator and renderer. A
// plugin-controlled surface is never authorized unless the complete canonical
// plugin id, command, and mandatory controls fit in the exact render geometry.
[[nodiscard]] bool plugin_ui_host_chrome_fits(TuiPluginUiBinding const& binding, TuiPluginUiKind kind, std::size_t width, std::size_t max_lines) noexcept;

enum class TuiPluginUiReplyKind
{
  Ack,
  Select,
  Confirm,
  Cancel,
};

struct TuiPluginUiReply
{
  TuiPluginUiReplyKind action = TuiPluginUiReplyKind::Cancel;
  std::string option_id;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

using TuiPluginUiCancelCallback = std::function<bool()>;
using TuiPluginUiPresenter = std::function<TuiPluginUiReply(TuiPluginUiRequest const&, std::chrono::steady_clock::time_point, TuiPluginUiCancelCallback)>;
using TuiPluginUiPresenterClose = std::function<void(TuiPluginUiBinding const&)>;

struct TuiPluginUiEndpoint
{
  std::weak_ptr<void const> runtime_token;
  std::chrono::steady_clock::time_point deadline{};
  TuiPluginUiPresenter present;
  TuiPluginUiPresenterClose close;

  [[nodiscard]] explicit operator bool() const noexcept
  {
    return !runtime_token.expired() && deadline != std::chrono::steady_clock::time_point{} && present && close;
  }

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

struct TuiPluginUiWidgetView
{
  ava::core::NoLogString title;
  std::vector<ava::core::NoLogString> lines;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

struct TuiPluginUiDockView
{
  TuiPluginUiBinding binding;
  std::optional<ava::core::NoLogString> status;
  std::vector<TuiPluginUiWidgetView> widgets;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

struct TuiPluginUiModalView
{
  TuiPluginUiBinding binding;
  // Correlation data remains hidden from host rendering.
  std::string request_id;
  TuiPluginUiKind kind = TuiPluginUiKind::Select;
  ava::core::NoLogString title;
  ava::core::NoLogString description;
  std::vector<TuiPluginUiOption> options;
  std::size_t selected_option = 0;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

}  // namespace ava::tui

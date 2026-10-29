#pragma once

#include "ava/debug/print_members_on.h"
#include "ava/core/NoLogString.h"
#include "ava/core/result.h"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <variant>
#include <vector>

namespace ava::plugin {

inline constexpr std::size_t kPluginUiIdMaxBytes = 96;
inline constexpr std::size_t kPluginUiTextComponentMaxBytes = 256;
inline constexpr std::size_t kPluginUiStatusTextMaxBytes = 256;
inline constexpr std::size_t kPluginUiWidgetMaxCount = 2;
inline constexpr std::size_t kPluginUiWidgetMaxLines = 8;
inline constexpr std::size_t kPluginUiWidgetTextMaxBytes = 2 * 1024;
inline constexpr std::size_t kPluginUiChoiceMaxCount = 32;
inline constexpr std::size_t kPluginUiModalPayloadMaxBytes = 8 * 1024;
inline constexpr std::size_t kPluginUiRecordMaxCount = 64;
inline constexpr std::size_t kPluginUiModalMaxCount = 8;

struct PluginUiStatusRequest
{
  std::string id;
  ava::core::NoLogString text;

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

struct PluginUiWidgetRequest
{
  std::string id;
  ava::core::NoLogString title;
  std::vector<ava::core::NoLogString> lines;

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

struct PluginUiChoice
{
  std::string id;
  ava::core::NoLogString label;
  std::optional<ava::core::NoLogString> description = std::nullopt;

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

struct PluginUiSelectRequest
{
  std::string id;
  ava::core::NoLogString title;
  ava::core::NoLogString description;
  std::vector<PluginUiChoice> choices;

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

struct PluginUiConfirmRequest
{
  std::string id;
  ava::core::NoLogString title;
  ava::core::NoLogString description;

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

using PluginUiRequest = std::variant<PluginUiStatusRequest, PluginUiWidgetRequest, PluginUiSelectRequest, PluginUiConfirmRequest>;

enum class PluginUiActionKind
{
  Ack,
  Select,
  Confirm,
  Cancel,
};

struct PluginUiAction
{
  PluginUiActionKind action = PluginUiActionKind::Cancel;
  std::string option_id;

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

class PluginUiProtocolState final
{
 public:
  PluginUiProtocolState() = default;

  PluginUiProtocolState(PluginUiProtocolState const&) = delete;
  PluginUiProtocolState& operator=(PluginUiProtocolState const&) = delete;
  PluginUiProtocolState(PluginUiProtocolState&&) = delete;
  PluginUiProtocolState& operator=(PluginUiProtocolState&&) = delete;

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT

 private:
  friend ava::core::Result<PluginUiRequest> parse_plugin_ui_request(std::string_view record, PluginUiProtocolState& state);

  std::size_t record_count_ = 0;
  std::size_t status_count_ = 0;
  std::size_t widget_count_ = 0;
  std::size_t modal_count_ = 0;
  std::unordered_set<std::string> request_ids_;
};

[[nodiscard]] bool is_plugin_ui_record_type(std::string_view type) noexcept;
[[nodiscard]] bool is_valid_plugin_ui_id(std::string_view id) noexcept;
[[nodiscard]] std::string_view plugin_ui_request_type(PluginUiRequest const& request) noexcept;
[[nodiscard]] std::string_view plugin_ui_request_id(PluginUiRequest const& request) noexcept;
[[nodiscard]] std::string_view plugin_ui_request_capability(PluginUiRequest const& request) noexcept;
[[nodiscard]] ava::core::Result<PluginUiRequest> parse_plugin_ui_request(std::string_view record, PluginUiProtocolState& state);
[[nodiscard]] ava::core::VoidResult validate_plugin_ui_request(PluginUiRequest const& request);
[[nodiscard]] ava::core::VoidResult validate_plugin_ui_action(PluginUiRequest const& request, PluginUiAction const& action);
[[nodiscard]] ava::core::Result<std::string> serialize_plugin_ui_action(std::string_view request_id, PluginUiAction const& action);

}  // namespace ava::plugin

#pragma once

#include "ava/debug/print_members_on.h"

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace ava::agent {

// Private process-local launch presentation. Values describe AVA's configured
// launch request, never provider-confirmed serving identity. They are
// normalized once and remain detached from provider/model ids and prompts.
inline constexpr std::size_t kMaxSubagentLaunchModelDisplayNameBytes = 128;
inline constexpr std::size_t kMaxSubagentLaunchReasoningLabelBytes = 32;

class SubagentLaunchDisplay
{
 public:
  SubagentLaunchDisplay() = default;

  // Invalid UTF-8 or terminal controls omit the model name. A missing or
  // rejected explicit AVA level becomes the literal "default".
  [[nodiscard]] static SubagentLaunchDisplay normalized(std::string_view model_display_name,
                                                         std::optional<std::string_view> explicit_reasoning_level = std::nullopt);

  [[nodiscard]] std::string const& model_display_name() const noexcept { return model_display_name_; }
  [[nodiscard]] std::string const& reasoning_label() const noexcept { return reasoning_label_; }
  [[nodiscard]] bool operator==(SubagentLaunchDisplay const&) const = default;

  AVA_DEBUG_PRINT_MEMBERS_ON

 private:
  SubagentLaunchDisplay(std::string model_display_name, std::string reasoning_label)
      : model_display_name_(std::move(model_display_name)), reasoning_label_(std::move(reasoning_label))
  {
  }

  std::string model_display_name_ = {};
  std::string reasoning_label_ = "default";
};

struct SubagentLaunchNotification
{
  std::string tool_call_id;
  std::string request_id;
  std::string correlation_id;
  SubagentLaunchDisplay display;

  // Private metadata must never be printed into diagnostics.
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

// Best-effort private UI association seam. Callers must enqueue without
// blocking; dispatch isolates exceptions and never lets the observer affect
// task execution.
using SubagentLaunchSink = std::function<void(SubagentLaunchNotification const&)>;

// Focused run-scoped custody bundle. Keeping these values together prevents
// generated diagnostics for broader option aggregates from discovering them.
struct SubagentLaunchObserver
{
  SubagentLaunchDisplay display = {};
  std::string request_id = {};
  std::string correlation_id = {};
  SubagentLaunchSink sink = nullptr;

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

}  // namespace ava::agent

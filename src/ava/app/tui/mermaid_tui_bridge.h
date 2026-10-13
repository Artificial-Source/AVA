#pragma once

#include "ava/app/mermaid_render_coordinator.h"
#include "ava/app/tui/tui_display_settings.h"
#include "ava/tui/runtime.h"
#include "ava/core/result.h"

#include <cstdint>
#include <memory>
#include <mutex>

namespace ava::app {

struct MermaidTuiPresentationConfiguration
{
  std::uint64_t epoch = 0;
  bool enabled = false;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

// Application-owned adapter between validated display settings/process
// coordination and the TUI's process-neutral callback DTOs.
class MermaidTuiBridge final : public std::enable_shared_from_this<MermaidTuiBridge>
{
 public:
  [[nodiscard]] static ava::core::Result<std::shared_ptr<MermaidTuiBridge>> create(MermaidDisplaySettings const& settings);
  MermaidTuiBridge(MermaidTuiBridge const&) = delete;
  MermaidTuiBridge& operator=(MermaidTuiBridge const&) = delete;
  ~MermaidTuiBridge();

  [[nodiscard]] ava::core::VoidResult apply(MermaidDisplaySettings const& settings);
  [[nodiscard]] MermaidTuiPresentationConfiguration presentation_configuration() const noexcept;
  [[nodiscard]] ava::tui::TuiMermaidRenderBridge tui_bridge();
  void shutdown() noexcept;

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT

 private:
  MermaidTuiBridge(std::unique_ptr<MermaidRenderCoordinator> coordinator, MermaidDisplaySettings settings, std::uint64_t epoch);

  [[nodiscard]] ava::tui::TuiMermaidEnqueueResult enqueue(ava::tui::TuiMermaidRenderRequest request) noexcept;
  [[nodiscard]] bool cancel(std::uint64_t identity, std::uint64_t config_epoch) noexcept;
  [[nodiscard]] std::vector<ava::tui::TuiMermaidRenderCompletion> drain() noexcept;

  mutable std::mutex mutex_;
  std::unique_ptr<MermaidRenderCoordinator> coordinator_;
  MermaidDisplaySettings settings_;
  std::uint64_t epoch_ = 0;
  bool shutdown_ = false;
};

}  // namespace ava::app

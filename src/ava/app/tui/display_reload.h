#pragma once

#include "ava/app/runtime/session_ts.h"
#include "ava/app/tui/mermaid_tui_bridge.h"
#include "ava/app/tui/tui_display_settings.h"
#include "ava/tui/runtime.h"
#include "ava/config/xdg_paths.h"
#include "ava/core/result.h"

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ava::app {

// Headless reload validation/reporting. This never applies terminal theme or
// mutates the interactive frontend's local effective presentation.
[[nodiscard]] ava::core::Result<std::vector<std::pair<std::string, std::string>>> reload_tui_display_settings(ava::config::XdgPaths const& paths);

// Single-thread owner for local appearance preferences. Every mutating method
// is invoked by the TUI thread; only the retained Mermaid service bridge has its
// own existing synchronization for asynchronous helper traffic.
class LocalTuiDisplaySettings final
{
 public:
  // Construct the TUI-thread owner for unlocked_session and its retained Mermaid helper without reading or applying the display document.
  // Can throws core::Error.
  [[nodiscard]] static LocalTuiDisplaySettings create(runtime::session_ts const& unlocked_session);

  // Loads, validates, and applies the display document, returning only local
  // presentation values and caller-visible status/output.
  [[nodiscard]] ava::core::Result<ava::tui::TuiLocalDisplaySettingsResult> reload(std::string status = "display theme reloaded");
  // Polls document freshness; nullopt means unchanged and a value means the
  // newly loaded settings were applied even if their presentation is equal.
  [[nodiscard]] ava::core::Result<std::optional<ava::tui::TuiLocalDisplaySettingsResult>> maybe_reload();
  // Persists one recognized display command/settings token and applies the
  // resulting document before returning its local presentation.
  [[nodiscard]] ava::core::Result<ava::tui::TuiLocalDisplaySettingsResult> apply_action(std::string_view action);
  // Returns current display values and validated custom-theme choices for initial TUI hydration.
  [[nodiscard]] ava::tui::TuiLocalDisplaySettings presentation() const;
  // Returns the bounded rendering callback owned by the retained helper.
  [[nodiscard]] ava::tui::TuiMermaidRenderBridge mermaid_bridge() const;
  // Idempotently stops helper work before terminal teardown.
  void shutdown() noexcept;

  AVA_DEBUG_PRINT_MEMBERS_ON

 private:
  // Retain unlocked_session's stable wrapper and the shared Mermaid service, not a copy of session configuration paths.
  LocalTuiDisplaySettings(runtime::session_ts const& unlocked_session, std::shared_ptr<MermaidTuiBridge> mermaid);
  // Build local presentation using paths captured by the enclosing operation, without reacquiring Session or mixing sessions' configuration roots.
  [[nodiscard]] ava::tui::TuiLocalDisplaySettings presentation(ava::config::XdgPaths const& paths) const;
  [[nodiscard]] ava::core::Result<ava::tui::TuiLocalDisplaySettingsResult> apply_loaded(ava::config::XdgPaths const& paths, TuiDisplaySettings loaded,
                                                                                        TuiDisplaySettingsWatchState watched, std::string status,
                                                                                        std::vector<std::string> output = {});

  // Non-owning reference to the live wrapper, which outlives the TUI and remains stable across Session replacement.
  runtime::session_ts const& unlocked_session_;
  TuiDisplaySettings effective_;
  std::optional<TuiDisplaySettingsWatchState> watch_state_;
  // Shared ownership is required because the bridge's service callbacks retain
  // it while the local owner also applies settings and controls shutdown.
  std::shared_ptr<MermaidTuiBridge> mermaid_;
};

}  // namespace ava::app

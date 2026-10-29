#pragma once

#include "runtime.h"                    // TuiRuntimeOptions, TuiCatalogDelivery, TuiLocalDisplaySettingsResult
#include "composer.h"                   // StartupOverviewSnapshot
#include "keybindings.h"                // TuiKeyBindings
#include "ava/frontend/FrontEnd.h"
#include <filesystem>

namespace ava::tui {

struct TuiRuntimeOptions;

class TuiFrontEnd : public frontend::FrontEnd
{
 protected:
  TuiKeyBindings key_bindings_;
  TuiRuntimeOptions options_;

 public:
  TuiFrontEnd(TuiRuntimeOptions&& options) :
    key_bindings_(default_key_bindings()),
    options_(std::move(options)) { }

  // Load key bindings. Returns the keybind status.
  std::string load_key_bindings();
  // Load the display settings; optinally appends to the status_line.
  virtual void load_display_settings(std::string& status_line) = 0;
  // Load the cursor settings.
  virtual ava::tui::terminal::CursorSettings cursor_settings() const = 0;
  // Get a snapshot of the current display settings.
  virtual TuiLocalDisplaySettings display_settings_snapshot() const = 0;
  virtual TuiMermaidRenderBridge mermaid_bridge() const = 0;

  // Get the path to the keybinds.json configuration file.
  virtual std::filesystem::path keybinds_json_path() const = 0;

  // Get the currently active session ID.
  virtual std::string read_committed_session_id() const = 0;

  // Pending-review catalog and todo transports. Pull and apply only at
  // serialized action/completion boundaries, never during rendering, and
  // like read_committed_session_id never while holding the UI lock.
  virtual TuiCatalogDelivery read_catalog_delivery() = 0;

  // The first callback argument is the frontend that owns these options. It is
  // const unless the callback must mutate frontend-owned application state.
  virtual std::vector<agent::TodoItem> read_committed_todos() const = 0;

  // UI-thread popup initialization callbacks. Inputs: none. Outputs: owning,
  // already-filtered DTOs from current in-memory Session state. Side effects:
  // briefly acquire the backend Session read guard. Call once per popup open,
  // never during ordinary boundary refreshes or rendering.
  virtual StartupOverviewSnapshot read_startup_overview() const = 0;

  virtual core::Result<TuiLocalDisplaySettingsResult> on_reload_display_settings() = 0;

  // Optional local result is the explicit applied/unchanged signal from the frontend owner:
  // nullopt = unchanged, some(result) = applied. unexpected = terminal apply/watch failure.
  // The TUI action layer maps this to DisplaySettingsReloadPollOutcome and must not compare values.
  virtual core::Result<std::optional<TuiLocalDisplaySettingsResult>> on_maybe_reload_display_settings() = 0;

  // Persists and applies a recognized display command or settings action on the TUI thread.
  virtual core::Result<TuiLocalDisplaySettingsResult> on_local_display_settings_action(std::string_view) = 0;

  // Runs after the composer loop exits while curses is still active. Application-scoped
  // workers that can notify this runtime must be canceled and joined here.
  virtual void on_before_tui_shutdown() = 0;

  virtual SelectListView session_selector_view() const = 0;

  // Launch the terminal user interface (TUI) using runtime options `options`.
  // Called by ava::app::interactive_internal::run_tui after registering all frontend::Data objects.
  //
  // Returns the application exit status, typically returned by `main`, after the frontend has shut down.
  int run();

  // OLD API
  [[nodiscard]] int run_interactive_composer();

  // Access the runtime callbacks and presentation defaults owned by this frontend.
  TuiRuntimeOptions& options() { return options_; }
  TuiRuntimeOptions const& options() const { return options_; }

  // Accessors (this can be removed once all std::function's of tui::TuiRuntimeOptions have been turned into member functions).
  TuiKeyBindings& key_bindings() { return key_bindings_; }
  TuiKeyBindings const& key_bindings() const { return key_bindings_; }

  AVA_DEBUG_PRINT_MEMBERS_ON_BASE(frontend::FrontEnd)
};

} // namespace ava::tui

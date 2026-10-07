#pragma once

#include "ava/debug/print_members_on.h"
#include "ava/app/runtime/ContextSourceMetadata.h"
#include "ava/app/runtime/FreshnessSourceMetadata.h"
#include "ava/app/runtime/session_ts.h"
#include "ava/tui/composer.h"
#include "ava/tui/keybindings.h"
#include "ava/tui/theme.h"

#include <cstddef>
#include <span>
#include <string>
#include <string_view>

namespace ava::app {

// Conservative bounds for the path-free startup overview DTO.
// Input/work caps bound builder effort; output caps bound the delivered snapshot.
// When context/freshness input exceeds kMaxStartupOverviewInputSources, per-group
// aggregate counts and plugin-resource failure counts are lower bounds (N+). The
// instruction-source total remains exact via O(1) span size and is never N+.
inline constexpr std::size_t kMaxStartupOverviewInputSources = 64;
inline constexpr std::size_t kMaxStartupOverviewLabelsPerGroup = 12;
inline constexpr std::size_t kMaxStartupOverviewLabelBytes = 48;
inline constexpr std::size_t kMaxStartupOverviewNamedItems = 16;
inline constexpr std::size_t kMaxStartupOverviewResourceGroups = 24;
inline constexpr std::size_t kMaxStartupOverviewKeyHints = 6;
inline constexpr std::size_t kMaxStartupOverviewCompactBytes = 160;
inline constexpr std::size_t kMaxStartupOverviewDetailBytes = 160;

// Pure builder input. Callers must supply already-loaded in-memory values only;
// the builder never opens paths, starts MCP/LSP/plugins, appends sessions, or
// calls providers. Large collections are non-owning spans so construction never
// deep-copies unbounded context/freshness/keybinding data before hard caps apply.
struct StartupOverviewBuildInput
{
  std::string_view mode = {};
  std::string_view provider = {};
  std::string_view model = {};
  std::string_view trust_decision = {};
  std::string_view project_resources = {};
  std::size_t protected_resource_count = 0;
  std::span<runtime::ContextSourceMetadata const> context_sources = {};
  std::span<runtime::FreshnessSourceMetadata const> freshness_sources = {};
  std::string_view theme_name = {};
  std::string_view theme_badge = {};
  // Required non-null in production; null falls back to default bindings for tests.
  ava::tui::TuiKeyBindings const* key_bindings = nullptr;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

// Build one immutable, deterministic, path-free overview snapshot.
// Accepts a const reference so callers never move/copy large input collections.
[[nodiscard]] ava::tui::StartupOverviewSnapshot build_startup_overview_snapshot(StartupOverviewBuildInput const& input);

// Convenience seam for interactive TUI wiring from already-open runtime state.
// MCP/LSP counts and session titles/ids are intentionally omitted: those values
// are either not retained path-free or can be prompt-derived/private.
[[nodiscard]] ava::tui::StartupOverviewSnapshot build_startup_overview_snapshot(runtime::session_ts const& session,
                                                                                ava::tui::TuiKeyBindings const& key_bindings,
                                                                                ava::tui::TuiThemeInfo const& theme);

// First configured OverviewToggle key display, or empty when unbound.
[[nodiscard]] std::string startup_overview_toggle_keys_display(ava::tui::TuiKeyBindings const& bindings);

}  // namespace ava::app

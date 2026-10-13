#pragma once

#include "ava/app/command_palette.h"
#include "ava/app/session_user_turns.h"
#include "ava/tui/composer.h"

namespace ava::app {

// The application catalog remains frontend-neutral. Conversion into renderer
// DTOs happens only at this app/TUI integration seam.
[[nodiscard]] std::vector<tui::SlashCommandItem> project_slash_command_items(std::vector<SlashCommandCatalogItem> items);
[[nodiscard]] std::vector<tui::FileReferenceItem> project_file_reference_items(std::vector<FileReferenceCatalogItem> items);
[[nodiscard]] std::vector<tui::SlashCommandItem> command_catalog_slash_items(std::vector<CommandHotkey> const& hotkeys = {});
[[nodiscard]] std::vector<tui::SlashCommandItem> command_catalog_slash_items_1(runtime::session_ts const& unlocked_session,
                                                                               std::vector<CommandHotkey> const& hotkeys = {});
[[nodiscard]] std::vector<tui::FileReferenceItem> file_reference_items(runtime::session_ts const& unlocked_session);
[[nodiscard]] tui::SelectListView application_catalog_session_view(ApplicationCatalogCoordinator const& coordinator,
                                                                   SessionSelectorSort sort = SessionSelectorSort::Recent, std::string footer_hint = {},
                                                                   bool named_only = false, bool show_paths = false, bool show_archived = false,
                                                                   bool show_label_time = false, std::string summarize_parent_keys = {});

[[nodiscard]] tui::SelectListView model_selector_view(ava::config::ModelRegistry const& registry, ava::config::ModelInfo const& current_model,
                                                      std::shared_ptr<ava::provider::ProviderCatalog const> ensured_provider_catalog,
                                                      std::string footer_hint = {});
[[nodiscard]] tui::SelectListView model_selector_view_1(runtime::session_ts const& unlocked_session, std::string footer_hint = {});
[[nodiscard]] tui::SelectListView scoped_model_selector_view(ava::config::ModelRegistry const& registry, ava::config::ModelInfo const& current_model,
                                                             std::optional<std::vector<std::string>> const& scoped_model_cycle,
                                                             std::shared_ptr<ava::provider::ProviderCatalog const> ensured_provider_catalog,
                                                             std::string footer_hint = {});
[[nodiscard]] tui::SelectListView scoped_model_selector_view_1(runtime::session_ts const& unlocked_session, std::string footer_hint = {});
[[nodiscard]] tui::SelectListView session_selector_view(std::vector<ava::session::SessionSummary> summaries, std::string current_session_id = {},
                                                        SessionSelectorSort sort = SessionSelectorSort::Recent, std::string footer_hint = {},
                                                        bool show_paths = false);
[[nodiscard]] tui::SelectListView session_selector_view(ava::session::SessionTreeIndex const& tree, SessionSelectorSort sort = SessionSelectorSort::Recent,
                                                        std::string footer_hint = {}, bool named_only = false, bool show_paths = false,
                                                        bool show_archived = false, bool show_label_time = false);
[[nodiscard]] tui::SelectListView session_selector_view(ApplicationCatalogCache const& cache, SessionSelectorSort sort = SessionSelectorSort::Recent,
                                                        std::string footer_hint = {}, bool named_only = false, bool show_paths = false,
                                                        bool show_archived = false, bool show_label_time = false);
#if 0 // Nothing is calling this function.
[[nodiscard]] tui::SelectListView session_selector_view(runtime::session_ts const& unlocked_session, SessionSelectorSort sort = SessionSelectorSort::Recent,
                                                         std::string footer_hint = {}, bool named_only = false, bool show_paths = false,
                                                         bool show_archived = false, bool show_label_time = false);
#endif
// Newest public user turns first. Item values are stable session entry ids;
// rows keep only the backend-bounded preview/timestamp fields.
[[nodiscard]] tui::SelectListView user_turn_selector_view(std::vector<SessionUserTurn> turns, std::string title, std::string footer_hint = {},
                                                          std::string initial_query = {}, bool truncated_before = false);
[[nodiscard]] ava::core::Result<tui::SelectListView> user_turn_selector_view(runtime::session_ts const& unlocked_session, std::string title,
                                                                             std::string footer_hint = {}, std::string initial_query = {});

}  // namespace ava::app

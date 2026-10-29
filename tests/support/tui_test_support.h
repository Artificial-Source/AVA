#pragma once

#include "tests/support/test_harness.h"
#include "ava/agent/agent_definition_types.h"
#include "ava/agent/mode.h"
#include "ava/tui/composer.h"
#include "ava/tui/runtime.h"
#include "ava/config/model_config.h"
#include "ava/session/Stats.h"
#include "ava/session/session_store.h"

#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ava::tui {
class TuiFrontEnd;
struct TuiRuntimeOptions;
} // namespace ava::tui

namespace ava::app {
struct ApplicationCatalogCache;
} // namespace ava::app

namespace tui_test_support {

struct TuiFrontEndTestHooks
{
  std::function<ava::tui::TuiCatalogDelivery()> read_catalog_delivery;
  std::function<ava::tui::ProjectTrustSnapshot()> read_project_trust;
  std::function<ava::tui::TuiActiveRunQueues(ava::event::EventEnvelopeSink)> create_active_run_queues;
  std::function<ava::tui::TuiSubmitResult(std::string const&, ava::tui::TuiSubmitContext)> on_submit;
  std::function<ava::core::Result<ava::tui::TuiKeyBindings>()> on_reload_key_bindings;
  std::function<ava::core::Result<ava::tui::TuiLocalDisplaySettingsResult>()> on_reload_display_settings;
  std::function<ava::tui::TuiLocalDisplaySettings()> display_settings_snapshot;
  std::function<ava::core::Result<std::optional<ava::tui::TuiLocalDisplaySettingsResult>>()> on_maybe_reload_display_settings;
  std::function<ava::core::Result<ava::tui::TuiLocalDisplaySettingsResult>(std::string_view)> on_local_display_settings_action;
  std::function<ava::core::Result<ava::tui::SelectListView>()> list_subagents;
  std::function<ava::core::Result<std::shared_ptr<ava::agent::SubagentInspectorFrame const>>(std::string_view, std::optional<std::uint64_t>)> inspect_subagent;
  std::function<ava::tui::SubagentWorkspaceCancelOutcome(std::string_view)> cancel_subagent;
  std::function<ava::tui::SubagentWorkspacePromoteOutcome(std::string_view)> promote_subagent;
  std::function<void()> on_before_tui_shutdown;
};

struct TuiTestModel
{
  std::string provider_id = "openai";
  std::string model_id = "gpt-5.5";
  std::string display_name = "gpt-5.5";
  std::optional<bool> supports_reasoning = std::nullopt;
  std::optional<std::vector<std::string>> reasoning_levels = std::nullopt;
  std::optional<std::vector<ava::config::ModelReasoningLevelMapping>> reasoning_level_mappings = std::nullopt;
};

// Owns the canonical backend session before registering its stable ModelSelection
// address with a real frontend. The declaration order in Impl keeps the frontend
// alive until after the registered session data has been destroyed.
class TuiFrontEndFixture final
{
 public:
  explicit TuiFrontEndFixture(TuiTestModel model = {});
  explicit TuiFrontEndFixture(ava::tui::TuiRuntimeOptions options);
  TuiFrontEndFixture(ava::tui::TuiRuntimeOptions options, ava::app::ApplicationCatalogCache catalog_cache);
  TuiFrontEndFixture(TuiFrontEndFixture const&) = delete;
  TuiFrontEndFixture& operator=(TuiFrontEndFixture const&) = delete;
  TuiFrontEndFixture(TuiFrontEndFixture&&) = delete;
  TuiFrontEndFixture& operator=(TuiFrontEndFixture&&) = delete;
  ~TuiFrontEndFixture();

  [[nodiscard]] ava::tui::TuiFrontEnd* frontend() noexcept;
  // Access optional application-boundary overrides used by focused runtime-controller tests.
  [[nodiscard]] TuiFrontEndTestHooks& hooks() noexcept;
  // Append a committed todowrite snapshot to the fixture's current backend session.
  void seed_committed_todos(std::vector<ava::agent::TodoItem> todos);
  [[nodiscard]] std::pair<std::string, std::string> selected_model_metadata();
  [[nodiscard]] std::pair<std::string, bool> git_branch_metadata();
  [[nodiscard]] std::pair<std::string, bool> workspace_root_metadata();
  [[nodiscard]] std::pair<std::string, bool> current_directory_metadata();
  [[nodiscard]] std::pair<std::string, bool> prompt_mode_metadata();
  [[nodiscard]] std::pair<std::string, bool> context_source_count_metadata();
  // Consume independently tracked session identity display updates.
  [[nodiscard]] std::pair<std::string, bool> session_id_metadata();
  [[nodiscard]] std::pair<std::string, bool> session_path_metadata();
  // Copy canonical options while retaining the nested options read guard.
  [[nodiscard]] ava::session::SessionStoreOptions session_store_options();
  void set_model(TuiTestModel model);
  void set_git_branch(std::string branch);
  void set_directories(std::filesystem::path workspace_root, std::filesystem::path current_directory,
                       std::vector<std::filesystem::path> additional_writable_dirs = {});
  void set_prompt_metadata(ava::agent::Mode mode, std::size_t context_source_count);
  // Notify the frontend without changing canonical identity values.
  void touch_session_identity();
  // Change only the canonical store identity, without replacing unrelated registered values.
  void set_session_identity_metadata(std::string session_id);
  // Replace all three bounded fields of the canonical selected-agent metadata.
  void set_selected_agent_metadata(std::string name, ava::agent::AgentDefinitionProvenance provenance, ava::agent::AgentToolPreset tool_preset);
  // Notify the frontend without changing canonical selected-agent metadata.
  void touch_agent_selection();
  // Change only canonical context-window metadata for projection and percentage tests.
  void set_context_window_tokens(std::optional<long long> context_window_tokens);
  void set_reasoning(std::optional<std::string> level);
  void set_active_token_estimates(std::optional<std::size_t> history_tokens, std::optional<std::size_t> system_prompt_tokens = 0);
  void set_session_stats(ava::session::Stats stats);
  [[nodiscard]] bool replace_session_with_active_token_estimates(std::size_t history_tokens, std::size_t system_prompt_tokens = 0);
  [[nodiscard]] bool replace_session_with_git_branch(std::string branch);
  [[nodiscard]] bool replace_session_with_directories(std::filesystem::path workspace_root, std::filesystem::path current_directory);
  [[nodiscard]] bool replace_session_with_prompt_metadata(ava::agent::Mode mode, std::size_t context_source_count);
  [[nodiscard]] bool replace_session_with_agent_metadata(std::string name, ava::agent::AgentDefinitionProvenance provenance,
                                                         ava::agent::AgentToolPreset tool_preset);
  // Replace the registered session with a newly opened ephemeral identity.
  [[nodiscard]] bool replace_session_identity();
  // Replace all capability inputs used by supported_reasoning_levels; inherited built-in mappings are deliberately cleared.
  void set_reasoning_capabilities(std::optional<bool> supports_reasoning, std::vector<std::string> reasoning_levels);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

struct ScopedTerminalCapabilityProfile
{
  explicit ScopedTerminalCapabilityProfile(std::string term_program_value);

  ScopedEnvVar term;
  ScopedEnvVar term_program;
  ScopedEnvVar terminal_emulator;
  ScopedEnvVar tmux;
  ScopedEnvVar kitty_window_id;
  ScopedEnvVar ghostty_resources_dir;
  ScopedEnvVar wezterm_pane;
  ScopedEnvVar warp_session_id;
  ScopedEnvVar warp_terminal_session_uuid;
  ScopedEnvVar iterm_session_id;
  ScopedEnvVar wt_session;
  ScopedEnvVar tmux_hyperlinks;
};

std::vector<ava::tui::SlashCommandItem> const& standard_slash_commands();
ava::tui::SlashCommandItem const* find_slash_command_item(std::vector<ava::tui::SlashCommandItem> const& items, std::string_view command);
bool slash_argument_completion_matches(ava::tui::SlashCommandArgumentCompletion const& completion, std::size_t argument_index, std::string_view value,
                                       std::vector<std::string> const& required_previous_args = {});
bool has_slash_argument_completion(ava::tui::SlashCommandItem const* item, std::size_t argument_index, std::string_view value,
                                   std::vector<std::string> const& required_previous_args = {});
std::string_view tree_action_key_bindings_json() noexcept;
std::vector<std::string> plain_lines(std::vector<std::string> lines);
std::string join_plain_lines(std::vector<std::string> const& lines);
std::string join_visible_lines(std::vector<std::string> const& lines);
std::size_t count_occurrences(std::string_view text, std::string_view needle);

}  // namespace tui_test_support

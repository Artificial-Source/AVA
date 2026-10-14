#include "sys.h"
#include "tests/support/app_runtime_support.h"
#include "tests/support/tui_test_support.h"
#include "ava/app/runtime/OpenContext.h"
#include "ava/app/runtime/Session.h"
#include "ava/app/runtime_reasoning.h"
#include "ava/tui/TuiFrontEnd.h"
#include "ava/config/model_config.h"
#include "ava/core/Application.h"
#include "ava/core/string_utils.h"

#include <algorithm>
#include <filesystem>
#include <stdexcept>
#include <utility>

namespace tui_test_support {

namespace {

// Open an isolated offline, sessionless backend session beneath root for frontend rendering tests.
// Throw on setup failure rather than allowing a fixture with missing canonical data to reach a renderer.
ava::app::runtime::session_ts open_frontend_test_session(std::filesystem::path const& root)
{
  auto const workspace = root / "workspace";
  std::filesystem::create_directories(workspace);

  ava::app::runtime::OpenContext context;
  context.workspace_dir = workspace;
  context.current_dir = workspace;
  context.paths = ava::tests::app_test_paths(root);
  context.offline = true;
  auto session = ava::app::runtime::Session::open(context, {.sessionless = true});
  if (!session)
    throw std::runtime_error("could not open TUI frontend test session: " + session.error().format());
  return std::move(*session);
}

// Set the canonical model metadata in session to requested_model before registering the frontend.
// Keep built-in capabilities where available; synthetic test model names inherit the fallback model's capabilities.
void select_frontend_test_model(ava::app::runtime::session_ts& session, TuiTestModel const& requested_model)
{
  auto const registry = ava::config::builtin_model_registry();
  auto model = ava::config::find_model(registry, requested_model.provider_id, requested_model.model_id)
                   .value_or(ava::config::find_model(registry, "openai", "gpt-5.5").value_or(ava::config::ModelInfo{}));
  model.provider_id = requested_model.provider_id;
  model.model_id = requested_model.model_id;
  model.display_name = requested_model.display_name;
  model.display_name_is_configured = true;
  if (requested_model.supports_reasoning)
    model.supports_reasoning = requested_model.supports_reasoning;
  if (requested_model.reasoning_levels)
    model.reasoning_levels = *requested_model.reasoning_levels;
  if (requested_model.reasoning_level_mappings)
    model.reasoning_level_mappings = *requested_model.reasoning_level_mappings;

  ava::app::runtime::session_ts::wat session_w(session);
  session_w->model_selection().change([&](ava::app::runtime::ModelSelection& selection) {
    selection.model = std::move(model);
    if (selection.reasoning && (!ava::app::runtime::has_reasoning_choice(selection.model) ||
                                !ava::app::runtime::resolve_runtime_reasoning_selection(selection.model, *selection.reasoning)))
      selection.reasoning.reset();
  });
}

} // namespace

// Own the frontend and its registered session data at stable addresses until the rendering fixture is destroyed.
// The temporary configuration root outlives both objects and no provider requests are made.
struct TuiFrontEndFixture::Impl
{
  explicit Impl(TuiTestModel model)
      : root(ScopedTestDirectory::create_unique(temp_root(), "tui-frontend")), frontend(), session(open_frontend_test_session(root.path()))
  {
    select_frontend_test_model(session, model);
    frontend.register_frontend_data(session);
  }

  ScopedTestDirectory root;
  ava::tui::TuiFrontEnd frontend;
  ava::app::runtime::session_ts session;
};

TuiFrontEndFixture::TuiFrontEndFixture(TuiTestModel model) : impl_(std::make_unique<Impl>(std::move(model)))
{
}

TuiFrontEndFixture::~TuiFrontEndFixture() = default;

ava::tui::TuiFrontEnd* TuiFrontEndFixture::frontend() noexcept
{
  return &impl_->frontend;
}

std::pair<std::string, std::string> TuiFrontEndFixture::selected_model_metadata()
{
  auto provider = impl_->frontend.model_selection().refresh_provider();
  auto model = impl_->frontend.model_selection().refresh_model();
  return {provider.display, model.display};
}

void TuiFrontEndFixture::set_model(TuiTestModel model)
{
  select_frontend_test_model(impl_->session, model);
}

void TuiFrontEndFixture::set_reasoning(std::optional<std::string> level)
{
  ava::app::runtime::session_ts::wat session_w(impl_->session);
  session_w->model_selection().change([level = std::move(level)](ava::app::runtime::ModelSelection& selection) mutable {
    if (level)
    {
      *level = ava::core::trim(*level);
      if (level->empty())
        throw std::invalid_argument("reasoning level is required");
      if (!ava::app::runtime::has_reasoning_choice(selection.model))
      {
        selection.reasoning.reset();
        return;
      }
      auto resolved = ava::app::runtime::resolve_runtime_reasoning_selection(selection.model,
                                                                             ava::app::runtime::ReasoningSelection{.level = std::move(*level), .display = {}});
      if (!resolved)
        throw std::invalid_argument(resolved.error().format());
      selection.reasoning = std::move(*resolved);
    }
    else
      selection.reasoning.reset();
  });
}

void TuiFrontEndFixture::set_active_token_estimates(std::optional<std::size_t> history_tokens, std::optional<std::size_t> system_prompt_tokens)
{
  ava::session::SessionStats stats;
  stats.active_context_tokens = history_tokens;
  set_session_stats(std::move(stats));
  ava::app::runtime::session_ts::wat session_w(impl_->session);
  session_w->system_prompt_token_estimate().change(
      [system_prompt_tokens](ava::app::runtime::SystemPromptTokenEstimate& estimate) { estimate.tokens = system_prompt_tokens; });
}

void TuiFrontEndFixture::set_session_stats(ava::session::SessionStats stats)
{
  ava::app::runtime::session_ts::wat session_w(impl_->session);
  session_w->session_stats().change([stats = std::move(stats)](ava::session::SessionStats& canonical_stats) mutable { canonical_stats = std::move(stats); });
}

bool TuiFrontEndFixture::replace_session_with_active_token_estimates(std::size_t history_tokens, std::size_t system_prompt_tokens)
{
  ava::app::runtime::OpenContext replacement_context;
  {
    ava::app::runtime::session_ts::crat session_r(impl_->session);
    replacement_context = session_r->replacement_open_context({});
  }
  auto replacement = ava::app::runtime::Session::open(replacement_context, {.sessionless = true});
  if (!replacement)
    return false;
  {
    ava::app::runtime::session_ts::wat replacement_w(*replacement);
    replacement_w->session_stats().change([history_tokens](ava::session::SessionStats& stats) { stats.active_context_tokens = history_tokens; });
    replacement_w->system_prompt_token_estimate().change(
        [system_prompt_tokens](ava::app::runtime::SystemPromptTokenEstimate& estimate) { estimate.tokens = system_prompt_tokens; });
  }
  return ava::app::runtime::Session::replace_with(impl_->session, *replacement).has_value();
}

void TuiFrontEndFixture::set_reasoning_capabilities(std::optional<bool> supports_reasoning, std::vector<std::string> reasoning_levels)
{
  ava::app::runtime::session_ts::wat session_w(impl_->session);
  session_w->model_selection().change(
      [supports_reasoning, reasoning_levels = std::move(reasoning_levels)](ava::app::runtime::ModelSelection& selection) mutable {
        selection.model.supports_reasoning = supports_reasoning;
        selection.model.reasoning_levels = std::move(reasoning_levels);
        selection.model.reasoning_level_mappings.clear();
        if (selection.reasoning && (!ava::app::runtime::has_reasoning_choice(selection.model) ||
                                    !ava::app::runtime::resolve_runtime_reasoning_selection(selection.model, *selection.reasoning)))
          selection.reasoning.reset();
      });
}

ScopedTerminalCapabilityProfile::ScopedTerminalCapabilityProfile(std::string term_program_value)
    : term("TERM", "xterm-256color"),
      term_program("TERM_PROGRAM", std::move(term_program_value)),
      terminal_emulator("TERMINAL_EMULATOR", ""),
      tmux("TMUX", ""),
      kitty_window_id("KITTY_WINDOW_ID", ""),
      ghostty_resources_dir("GHOSTTY_RESOURCES_DIR", ""),
      wezterm_pane("WEZTERM_PANE", ""),
      warp_session_id("WARP_SESSION_ID", ""),
      warp_terminal_session_uuid("WARP_TERMINAL_SESSION_UUID", ""),
      iterm_session_id("ITERM_SESSION_ID", ""),
      wt_session("WT_SESSION", ""),
      tmux_hyperlinks("AVA_TUI_TMUX_HYPERLINKS", "")
{
}

std::vector<ava::tui::SlashCommandItem> const& standard_slash_commands()
{
  static std::vector<ava::tui::SlashCommandItem> const commands = {
      ava::tui::SlashCommandItem{.command = "/help", .description = "Show help", .category = "General"},
      ava::tui::SlashCommandItem{.command = "/grep", .description = "Search files", .hint = "<text> [glob]", .category = "Files"},
      ava::tui::SlashCommandItem{.command = "/glob", .description = "List matching files", .hint = "<pattern>", .category = "Files"},
      ava::tui::SlashCommandItem{.command = "/quit", .description = "Exit", .category = "General"}};
  return commands;
}

ava::tui::SlashCommandItem const* find_slash_command_item(std::vector<ava::tui::SlashCommandItem> const& items, std::string_view command)
{
  auto const found = std::ranges::find_if(items, [command](auto const& item) { return item.command == command; });
  return found == items.end() ? nullptr : &*found;
}

bool slash_argument_completion_matches(ava::tui::SlashCommandArgumentCompletion const& completion, std::size_t argument_index, std::string_view value,
                                       std::vector<std::string> const& required_previous_args)
{
  return completion.argument_index == argument_index && completion.value == value && completion.required_previous_args == required_previous_args;
}

bool has_slash_argument_completion(ava::tui::SlashCommandItem const* item, std::size_t argument_index, std::string_view value,
                                   std::vector<std::string> const& required_previous_args)
{
  return item != nullptr && std::ranges::any_of(item->argument_completions, [&](auto const& completion) {
           return slash_argument_completion_matches(completion, argument_index, value, required_previous_args);
         });
}

std::string_view tree_action_key_bindings_json() noexcept
{
  return "{\"app.tree.foldOrUp\":\"Ctrl+O\","
         "\"app.tree.unfoldOrDown\":\"Ctrl+Y\","
         "\"app.tree.editLabel\":\"Shift+L\","
         "\"app.tree.toggleLabelTimestamp\":\"Shift+T\","
         "\"app.tree.filter.labeledOnly\":\"Ctrl+Space\","
         "\"app.tree.filter.all\":\"Ctrl+/\"}";
}

std::vector<std::string> plain_lines(std::vector<std::string> lines)
{
  for (auto& line : lines)
    line = strip_sgr(line);
  return lines;
}

std::string join_plain_lines(std::vector<std::string> const& lines)
{
  std::string text;
  for (auto const& line : lines)
    text += line + '\n';
  return text;
}

std::string join_visible_lines(std::vector<std::string> const& lines)
{
  std::string text;
  for (auto const& line : lines)
    text += strip_sgr(line) + '\n';
  return text;
}

std::size_t count_occurrences(std::string_view text, std::string_view needle)
{
  std::size_t count = 0;
  for (auto offset = text.find(needle); offset != std::string_view::npos; offset = text.find(needle, offset + needle.size()))
    ++count;
  return count;
}

}  // namespace tui_test_support

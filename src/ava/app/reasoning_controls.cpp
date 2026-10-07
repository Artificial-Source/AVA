#include "sys.h"
#include "ava/app/reasoning_controls.h"
#include "ava/app/runtime/Session.h"
#include "ava/config/provider_profiles.h"
#include "ava/provider/catalog.h"
#include "ava/core/error.h"

#include <algorithm>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ava::app {
namespace {

std::string reasoning_selected_status(runtime::ReasoningSelection const& selection)
{
  std::string status = "reasoning set to " + selection.level;
  if (selection.budget_tokens)
    status += " budget " + std::to_string(*selection.budget_tokens);
  if (!selection.display.empty())
    status += " display " + selection.display;
  return status;
}

}  // namespace

std::optional<std::string> reasoning_status_for_session(runtime::session_ts const& unlocked_session)
{
  SCOPED_CRITICAL_AREA_CR(session_r, unlocked_session);
  auto const& model = session_r->model();
  if (!model.supports_reasoning.value_or(false) && ava::config::supported_reasoning_levels(model).size() <= 1)
    return std::nullopt;
  if (!session_r->reasoning() || session_r->reasoning()->level.empty())
    return std::nullopt;
  return session_r->reasoning()->level;
}

ava::core::Result<runtime::ReasoningSelection> reasoning_selection_for_level(ava::config::ModelInfo const& model, std::string level)
{
  if (level == "off")
  {
    auto error = ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "reasoning level off clears reasoning instead of enabling a selection");
    error.with_context("provider", model.provider_id);
    error.with_context("model", model.model_id);
    return std::unexpected(std::move(error));
  }
  auto const resolved = ava::config::resolve_reasoning_level(model, level);
  if (!resolved.supported)
  {
    auto error = ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "reasoning level is not supported by the current model");
    error.with_context("provider", model.provider_id);
    error.with_context("model", model.model_id);
    error.with_context("level", level);
    return std::unexpected(std::move(error));
  }
  auto profile = ava::provider::ProviderCatalog::build_builtins_only()->reasoning_profile_for_model(model);
  if (!profile || !profile->enabled_reasoning_requires_budget_tokens || level != "enabled")
  {
    return runtime::ReasoningSelection{.level = std::move(level), .provider_level = resolved.provider_level, .budget_tokens = std::nullopt, .display = {}};
  }

  auto const default_budget =
      profile->default_reasoning_budget_tokens > 0 ? profile->default_reasoning_budget_tokens : profile->minimum_reasoning_budget_tokens;
  auto const max_output = model.max_output_tokens.value_or(default_budget);
  if (max_output <= profile->minimum_reasoning_budget_tokens)
  {
    auto const provider_label = ava::provider::ProviderCatalog::build_builtins_only()->display_name(model.provider_id);
    auto error = ava::core::Error(ava::core::ErrorCategory::InvalidArgument,
                                  "reasoning unavailable: " + provider_label + " thinking needs output room for budget_tokens");
    error.with_context("provider", model.provider_id);
    error.with_context("model", model.model_id);
    error.with_context("max_output", std::to_string(max_output));
    return std::unexpected(std::move(error));
  }
  auto const budget = std::min<long long>(default_budget, max_output - 1);
  return runtime::ReasoningSelection{.level = std::move(level), .provider_level = resolved.provider_level, .budget_tokens = budget, .display = {}};
}

ava::core::Result<std::string> cycle_runtime_reasoning(runtime::session_ts& unlocked_session)
{
  ava::config::ModelInfo model;
  std::optional<runtime::ReasoningSelection> current_reasoning;
  {
    SCOPED_CRITICAL_AREA_R(session_r, unlocked_session);
    model = session_r->model();
    current_reasoning = session_r->reasoning();
  }
  if (!model.supports_reasoning.value_or(false))
  {
    return std::string("reasoning unavailable: current model does not declare reasoning support");
  }
  auto const supported_levels = ava::config::supported_reasoning_levels(model);
  std::vector<std::string> active_levels;
  for (auto const& level : supported_levels)
  {
    if (level != "off")
      active_levels.push_back(level);
  }
  if (active_levels.empty())
  {
    return std::string("reasoning unavailable: current model does not declare reasoning levels");
  }

  std::optional<std::size_t> current_index;
  if (current_reasoning)
  {
    for (std::size_t index = 0; index < active_levels.size(); ++index)
    {
      if (active_levels[index] == current_reasoning->level)
      {
        current_index = index;
        break;
      }
    }
  }

  if (current_index && *current_index + 1 >= active_levels.size())
  {
    auto cleared = runtime::Session::set_reasoning_and_refresh(unlocked_session, std::nullopt);
    if (!cleared)
      return std::unexpected(std::move(cleared.error()));
    return *cleared ? std::string("reasoning cleared") : std::string("reasoning already cleared");
  }

  auto const next_index = current_index ? *current_index + 1 : std::size_t{0};
  if (active_levels[next_index] == "disabled")
  {
    auto cleared = runtime::Session::set_reasoning_and_refresh(unlocked_session, std::nullopt);
    if (!cleared)
      return std::unexpected(std::move(cleared.error()));
    return *cleared ? std::string("reasoning cleared") : std::string("reasoning already cleared");
  }

  auto selection = reasoning_selection_for_level(model, active_levels[next_index]);
  if (!selection)
    return std::unexpected(std::move(selection.error()));
  auto selected = runtime::Session::set_reasoning_and_refresh(unlocked_session, *selection);
  if (!selected)
    return std::unexpected(std::move(selected.error()));
  return *selected ? reasoning_selected_status(*selection) : "reasoning already " + selection->level;
}

}  // namespace ava::app

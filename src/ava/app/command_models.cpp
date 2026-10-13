#include "sys.h"
#include "ava/app/command_format.h"
#include "ava/app/command_models.h"
#include "ava/app/runtime/Session.h"
#include "ava/config/auth.h"
#include "ava/config/auth_record.h"
#include "ava/config/model_config.h"
#include "ava/config/provider_profiles.h"
#include "ava/config/reasoning_profiles.h"
#include "ava/provider/catalog.h"
#include "ava/provider/registry.h"

#include <algorithm>
#include <cctype>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace ava::app {
namespace {

std::string trim_ascii(std::string_view text)
{
  while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front())) != 0) text.remove_prefix(1);
  while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())) != 0) text.remove_suffix(1);
  return std::string(text);
}

std::string lower_ascii(std::string_view text)
{
  std::string lowered(text);
  std::ranges::transform(lowered, lowered.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
  return lowered;
}

bool contains_ascii_case_insensitive(std::string_view text, std::string_view query)
{
  if (query.empty())
    return true;
  return lower_ascii(text).find(lower_ascii(query)) != std::string::npos;
}

bool model_matches_query(ava::config::ModelInfo const& model, std::string_view query)
{
  if (query.empty())
    return true;
  return contains_ascii_case_insensitive(model.provider_id + "/" + model.model_id, query) || contains_ascii_case_insensitive(model.model_id, query) ||
         contains_ascii_case_insensitive(model.display_name, query) || contains_ascii_case_insensitive(model.family, query);
}

std::string optional_bool_text(std::optional<bool> const& value)
{
  if (!value)
    return "unknown";
  return *value ? "yes" : "no";
}

bool contains_string(std::vector<std::string> const& values, std::string_view value)
{
  return std::ranges::find(values, value) != values.end();
}

bool model_is_builtin(ava::config::ModelInfo const& model)
{
  auto const builtin = ava::config::builtin_model_registry();
  return ava::config::find_model(builtin, model.provider_id, model.model_id).has_value();
}

bool api_family_is_known(std::span<ava::config::ProviderProfile const> profiles, std::string_view api_family)
{
  if (api_family.empty())
    return false;
  for (auto const& profile : profiles)
  {
    if (profile.api_family == api_family)
      return true;
  }
  return false;
}

std::shared_ptr<ava::provider::ProviderCatalog const> catalog_for(runtime::session_ts::crat const& session_r)
{
  if (session_r->provider_catalog())
    return session_r->provider_catalog();
  return ava::provider::ProviderCatalog::build_builtins_only();
}

std::vector<std::string> model_diagnostics(ava::config::ModelInfo const& model, bool provider_registered, ava::provider::ProviderCatalog const& catalog)
{
  std::vector<std::string> diagnostics;
  if (!ava::config::is_valid_provider_id(model.provider_id))
  {
    diagnostics.push_back("provider id contains unsupported characters; auth storage and provider lookup will reject it");
  }
  if (!provider_registered)
  {
    diagnostics.push_back("provider is not registered; selector is disabled and switching will fail");
  }

  bool const custom_model = !model_is_builtin(model);
  if (!custom_model)
    return diagnostics;

  if (!model.context_window_tokens)
  {
    diagnostics.push_back("custom model missing context_window_tokens; token percentage and auto-compaction use unknown context");
  }
  if (model.input_modalities.empty())
  {
    diagnostics.push_back("custom model missing input_modalities; image replay is disabled unless text/image are declared");
  }
  else if (!contains_string(model.input_modalities, "text"))
  {
    diagnostics.push_back("input_modalities does not include text; normal prompt replay may be unsupported");
  }
  if (model.api_family.empty())
  {
    diagnostics.push_back("custom model missing api_family; provider-specific reasoning and replay checks are limited");
  }
  else
  {
    auto const provider_profile = catalog.find_profile(model.provider_id);
    if (provider_profile && !provider_profile->api_family.empty() && provider_profile->api_family != model.api_family)
    {
      diagnostics.push_back("custom model api_family does not match provider profile; provider-specific request validation may fail");
    }
    if (!api_family_is_known(catalog.profiles(), model.api_family))
    {
      diagnostics.push_back("custom model api_family is not recognized by built-in provider profiles; compatibility validation is limited");
    }
  }
  if (!model.supports_tools)
  {
    diagnostics.push_back("custom model has unknown tool support; AVA will assume tools are supported");
  }
  if (!model.supports_streaming)
  {
    diagnostics.push_back("custom model has unknown streaming support; AVA may request streaming when the transport supports it");
  }
  if (!model.reports_usage && !model.pricing)
  {
    diagnostics.push_back("custom model has unknown usage/pricing; cost reporting may be incomplete");
  }
  if (model.supports_reasoning.value_or(false))
  {
    if (model.reasoning_levels.empty())
    {
      diagnostics.push_back("reasoning model has no reasoning_levels; Shift+Tab cannot cycle and Ctrl+T has no thinking modes to select");
    }
    if (!catalog.reasoning_profile_for_model(model))
    {
      diagnostics.push_back("reasoning api_family is not recognized; provider-specific reasoning validation is limited");
    }
  }
  return diagnostics;
}

std::string format_models_text(runtime::session_ts const& unlocked_session, ava::config::ModelRegistry const& registry, std::string_view query)
{
  ava::config::ModelInfo current_model;
  std::optional<runtime::ReasoningSelection> current_reasoning;
  std::shared_ptr<ava::provider::ProviderCatalog const> catalog;
  {
    SCOPED_CRITICAL_AREA_CR(session_r, unlocked_session);
    runtime::ModelSelection_ts::crat model_selection_r = session_r->model_selection_r();
    current_model = model_selection_r->model;
    current_reasoning = model_selection_r->reasoning;
    catalog = catalog_for(session_r);
  }
  auto models = ava::config::effective_models(registry);
  bool current_in_catalog = false;

  std::string output = "Models:\n";
  output += "current " + current_model.provider_id + "/" + current_model.model_id + "\n";
  output += current_reasoning ? "reasoning current " + current_reasoning->level + "\n" : "reasoning current default\n";
  output += "default " + registry.default_provider_id + "/" + registry.default_model_id + "\n";
  if (!query.empty())
    output += "filter " + sanitize_inline_text(std::string(query)) + "\n";
  std::size_t shown = 0;
  for (auto const& model : models)
  {
    current_in_catalog = current_in_catalog || (model.provider_id == current_model.provider_id && model.model_id == current_model.model_id);
    if (!model_matches_query(model, query))
      continue;
    ++shown;
    bool const registered = catalog->contains(model.provider_id);
    output += model.provider_id == current_model.provider_id && model.model_id == current_model.model_id ? "* " : "  ";
    output += model.provider_id + "/" + model.model_id;
    if (!model.display_name.empty())
      output += "  " + model.display_name;
    output += registered ? "\n" : "  unavailable: provider not registered\n";
    output += "    tools=" + optional_bool_text(model.supports_tools) + " streaming=" + optional_bool_text(model.supports_streaming) +
              " reasoning=" + optional_bool_text(model.supports_reasoning) + " usage=" + optional_bool_text(model.reports_usage);
    if (model.context_window_tokens)
      output += " context=" + std::to_string(*model.context_window_tokens);
    if (model.max_output_tokens)
      output += " max_output=" + std::to_string(*model.max_output_tokens);
    output += '\n';
    auto const diagnostics = model_diagnostics(model, registered, *catalog);
    if (!diagnostics.empty())
    {
      output += "    diagnostics:\n";
      for (auto const& diagnostic : diagnostics)
      {
        output += "      - " + diagnostic + "\n";
      }
    }
    if (!model.reasoning_levels.empty())
    {
      output += "    reasoning levels: " + joined_strings(model.reasoning_levels, ", ") + "\n";
    }
    if (model.supports_reasoning.value_or(false))
    {
      output += "    reasoning params: " + ava::config::reasoning_parameter_text(model) + "\n";
      if (!model.reasoning_format.empty())
        output += "    reasoning format: " + model.reasoning_format + "\n";
    }
  }
  if (!current_in_catalog)
  {
    output += "* " + current_model.provider_id + "/" + current_model.model_id + "  current model outside catalog\n";
  }
  if (shown == 0 && !query.empty())
  {
    output += "  no configured models match the filter\n";
  }
  output +=
      "\nModel switching is not enabled here. In the TUI, Shift+Tab cycles the current model's declared reasoning levels; "
      "Ctrl+T opens the policy-resolved thinking-mode selector between turns.";
  return output;
}

bool provider_matches_query(ava::config::ProviderProfile const& profile, std::string_view query)
{
  if (query.empty())
    return true;
  return contains_ascii_case_insensitive(profile.provider_id, query) || contains_ascii_case_insensitive(profile.display_name, query) ||
         contains_ascii_case_insensitive(profile.api_family, query) || contains_ascii_case_insensitive(profile.connect_detail, query);
}

std::size_t model_count_for_provider(ava::config::ModelRegistry const& registry, std::string_view provider_id)
{
  std::size_t count = 0;
  for (auto const& model : ava::config::effective_models(registry))
  {
    if (model.provider_id == provider_id)
      ++count;
  }
  return count;
}

std::string provider_oauth_status(ava::config::ProviderProfile const& profile)
{
  if (!profile.supports_oauth)
    return "no";
  if (profile.provider_id == "openai")
    return "interactive supported";
  if (profile.provider_id == "anthropic")
    return "stored/env bearer only; interactive deferred pending official third-party flow";
  return "stored/env bearer";
}

std::string provider_credential_status(ava::config::XdgPaths const& paths, ava::config::ProviderProfile const& profile)
{
  ava::config::ProviderCredentialPolicy policy{.auth_none = profile.auth_none,
                                               .user_defined = profile.user_defined,
                                               .api_key_env = profile.api_key_env};
  auto credential = ava::config::provider_credential_for_startup(paths, profile.provider_id, policy);
  if (!credential)
  {
    return "error: " + sanitize_inline_text(credential.error().format());
  }
  if (!*credential)
  {
    if (profile.user_defined && !profile.api_key_env.empty())
      return "missing env=" + sanitize_inline_text(profile.api_key_env);
    return "missing";
  }

  auto type = (*credential)->credential_type.empty() ? std::string("configured") : (*credential)->credential_type;
  if (type == "none")
    return "none (no credential required)";
  auto source = (*credential)->source.empty() ? std::string("configured") : (*credential)->source;
  return sanitize_inline_text(type) + " source=" + sanitize_inline_text(source);
}

std::string format_providers_text(runtime::session_ts const& unlocked_session, ava::config::ModelRegistry const& registry, std::string_view query)
{
  CRITICAL_AREA_BEGIN_CR(session);
  auto const paths = session_r->paths();
  auto const catalog = catalog_for(session_r);
  CRITICAL_AREA_END_R(session);
  auto const provider_registry = ava::provider::builtin_provider_registry();
  std::string output = "Providers:\n";
  if (!query.empty())
    output += "filter " + sanitize_inline_text(std::string(query)) + "\n";

  std::size_t shown = 0;
  for (auto const& profile : catalog->profiles())
  {
    if (!provider_matches_query(profile, query))
      continue;
    ++shown;
    bool const registered = catalog->contains(profile.provider_id);
    output += "  " + profile.provider_id;
    if (!profile.display_name.empty())
      output += "  " + profile.display_name;
    output += registered ? "\n" : "  unavailable: no runtime provider factory\n";
    output += "    runtime=" + std::string(registered ? "registered" : "not registered") +
              " selectable=" + std::string(profile.runtime_selectable ? "yes" : "no") +
              " models=" + std::to_string(model_count_for_provider(registry, profile.provider_id)) + "\n";
    output += "    auth=" + provider_credential_status(paths, profile) + "\n";
    output += "    connect=" + sanitize_inline_text(profile.connect_detail.empty() ? std::string("not documented") : profile.connect_detail) +
              " oauth=" + provider_oauth_status(profile) + "\n";
    if (!profile.api_family.empty())
      output += "    api_family=" + profile.api_family + "\n";
    if (!profile.endpoint.empty() || !profile.default_base_url.empty() || !profile.default_base_url_env.empty())
    {
      output += "    endpoint=";
      if (!profile.endpoint.empty())
        output += profile.endpoint;
      else
        output += profile.default_base_url.empty() ? "provider default" : profile.default_base_url;
      if (!profile.default_base_url_env.empty())
        output += " env=" + profile.default_base_url_env;
      output += "\n";
    }
    if (!profile.default_compatibility_quirks.empty())
      output += "    compatibility=" + joined_strings(profile.default_compatibility_quirks, ", ") + "\n";
  }
  if (shown == 0 && !query.empty())
    output += "  no providers match the filter\n";
  output +=
      "\nUse /connect <provider> to store API keys. OpenAI supports interactive OAuth; Anthropic interactive OAuth is deferred until Anthropic documents a "
      "third-party flow.";
  return output;
}

}  // namespace

std::vector<std::string> model_configuration_diagnostics(ava::config::ModelInfo const& model, bool provider_registered)
{
  // Compatibility surface for callers without a pinned catalog; uses the same
  // built-in descriptor set the process catalog composes at startup.
  return model_diagnostics(model, provider_registered, *ava::provider::ProviderCatalog::build_builtins_only());
}

ava::core::Result<CommandResult> run_models_command(runtime::session_ts& unlocked_session, std::string_view query)
{
  CommandResult result;
  result.handled = true;
  auto const trimmed_query = trim_ascii(query);
  auto const paths = runtime::session_ts::rat(unlocked_session)->paths();
  auto registry = ava::config::load_model_registry(paths);
  if (!registry)
    return std::unexpected(std::move(registry.error()));
  add_output(result, format_models_text(unlocked_session, *registry, trimmed_query));
  return result;
}

ava::core::Result<CommandResult> run_providers_command(runtime::session_ts& unlocked_session, std::string_view query)
{
  CommandResult result;
  result.handled = true;
  auto const trimmed_query = trim_ascii(query);
  auto const paths = runtime::session_ts::rat(unlocked_session)->paths();
  auto registry = ava::config::load_model_registry(paths);
  if (!registry)
    return std::unexpected(std::move(registry.error()));
  add_output(result, format_providers_text(unlocked_session, *registry, trimmed_query));
  return result;
}

}  // namespace ava::app

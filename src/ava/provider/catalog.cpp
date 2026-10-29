#include "sys.h"
#include "ava/config/reasoning_profiles.h"
#include "ava/provider/anthropic_provider.h"
#include "ava/provider/catalog.h"
#include "ava/provider/openai_compatible_provider.h"
#include "ava/provider/openai_provider.h"

#include <utility>

namespace ava::provider {
namespace {

ava::core::Error catalog_error(std::string message, std::string_view field = {})
{
  auto error = ava::core::Error(ava::core::ErrorCategory::Configuration, std::move(message));
  if (!field.empty())
    error.with_context("field", std::string(field));
  return error;
}

std::string api_family_for_protocol(ava::config::ProviderProtocol protocol)
{
  switch (protocol)
  {
    case ava::config::ProviderProtocol::OpenAIChatCompletions:
      return std::string(ava::config::openai_compatible_reasoning_content_profile().api_family);
    case ava::config::ProviderProtocol::OpenAIResponses:
      return std::string(ava::config::openai_responses_reasoning_profile().api_family);
    case ava::config::ProviderProtocol::AnthropicMessages:
      return std::string(ava::config::anthropic_thinking_reasoning_profile().api_family);
  }
  return {};
}

std::string connect_detail_for(ava::config::UserProviderDefinition const& definition)
{
  if (definition.auth == ava::config::ProviderAuthMode::None)
    return "no credential required";
  if (!definition.api_key_env.empty())
    return "API key (" + definition.api_key_env + ")";
  return "API key";
}

ava::config::ProviderProfile profile_from_user_definition(ava::config::UserProviderDefinition const& definition)
{
  ava::config::ProviderProfile profile;
  profile.provider_id = definition.id;
  profile.display_name = definition.display_name;
  profile.connect_detail = connect_detail_for(definition);
  profile.api_family = api_family_for_protocol(definition.protocol);
  profile.default_base_url = definition.base_url;
  profile.chat_completions_path = definition.request_path;
  profile.include_stream_usage = definition.compatibility.include_stream_usage;
  profile.supports_oauth = false;
  profile.runtime_selectable = true;
  profile.user_defined = true;
  profile.auth_none = definition.auth == ava::config::ProviderAuthMode::None;
  profile.api_key_env = definition.api_key_env;
  profile.endpoint = definition.endpoint;
  if (definition.protocol == ava::config::ProviderProtocol::OpenAIChatCompletions)
  {
    profile.default_compatibility_quirks = {"openai_compatible"};
    profile.default_reasoning_format = ava::config::openai_compatible_reasoning_content_profile().format;
    profile.default_reasoning_levels = ava::config::openai_compatible_reasoning_content_profile().levels;
    profile.reasoning_request_parameters = ava::config::openai_compatible_reasoning_content_profile().request_parameters;
  }
  else if (definition.protocol == ava::config::ProviderProtocol::OpenAIResponses)
  {
    profile.default_reasoning_format = ava::config::openai_responses_reasoning_profile().format;
    profile.default_reasoning_levels = ava::config::openai_responses_reasoning_profile().levels;
    profile.reasoning_request_parameters = ava::config::openai_responses_reasoning_profile().request_parameters;
  }
  else
  {
    profile.default_reasoning_format = ava::config::anthropic_thinking_reasoning_profile().format;
    profile.default_reasoning_levels = ava::config::anthropic_thinking_reasoning_profile().levels;
    profile.reasoning_request_parameters = ava::config::anthropic_thinking_reasoning_profile().request_parameters;
  }
  return profile;
}

ProviderRegistry::Factory factory_for_user_definition(ava::config::UserProviderDefinition definition)
{
  bool const auth_none = definition.auth == ava::config::ProviderAuthMode::None;
  switch (definition.protocol)
  {
    case ava::config::ProviderProtocol::OpenAIChatCompletions:
      return [definition = std::move(definition), auth_none]() -> std::unique_ptr<Provider> {
        return std::make_unique<OpenAICompatibleProvider>(OpenAICompatibleProviderOptions{
            .base_url = definition.base_url,
            .chat_completions_path = definition.request_path,
            .endpoint = definition.endpoint,
            .provider_name = definition.display_name,
            .include_stream_usage = definition.compatibility.include_stream_usage,
            .follow_redirects = false,
            .require_credential = !auth_none,
            .send_authorization_bearer = !auth_none,
        });
      };
    case ava::config::ProviderProtocol::OpenAIResponses:
      return [definition = std::move(definition), auth_none]() -> std::unique_ptr<Provider> {
        return std::make_unique<OpenAIProvider>(OpenAIProviderOptions{
            .base_url = definition.base_url,
            .endpoint = definition.endpoint,
            .follow_redirects = false,
            .require_credential = !auth_none,
            .send_authorization_bearer = !auth_none,
            .enable_codex_oauth_mutations = false,
            .force_include_max_output_tokens = true,
        });
      };
    case ava::config::ProviderProtocol::AnthropicMessages:
      return [definition = std::move(definition), auth_none]() -> std::unique_ptr<Provider> {
        return std::make_unique<AnthropicProvider>(AnthropicProviderOptions{
            .base_url = definition.base_url,
            .endpoint = definition.endpoint,
            .require_credential = !auth_none,
            .send_api_key_header = !auth_none,
            .enable_oauth_header_swap = false,
        });
      };
  }
  return {};
}

ava::core::VoidResult register_user_definitions(ProviderRegistry& registry, std::vector<ava::config::ProviderProfile>& profiles,
                                                std::vector<ava::config::UserProviderDefinition> const& definitions)
{
  for (auto const& definition : definitions)
  {
    auto profile = profile_from_user_definition(definition);
    auto factory = factory_for_user_definition(definition);
    if (!factory)
    {
      auto error = catalog_error("user provider protocol is not supported", "protocol");
      error.with_context("provider_id", definition.id);
      return std::unexpected(std::move(error));
    }
    if (auto registered = registry.register_provider(definition.id, std::move(factory)); !registered)
      return std::unexpected(std::move(registered.error()));
    profiles.push_back(std::move(profile));
  }
  return {};
}

}  // namespace

ProviderCatalog::ProviderCatalog(ConstructionKey, ProviderRegistry registry, std::vector<ava::config::ProviderProfile> profiles,
                                 std::vector<ava::config::UserProviderDefinition> user_definitions)
    : registry_(std::move(registry)), profiles_(std::move(profiles)), user_definitions_(std::move(user_definitions))
{
}

namespace {

std::vector<ava::config::ProviderProfile> profiles_with_resolved_generics(std::vector<ava::config::ResolvedBuiltinGenericProvider> const& resolved_generics)
{
  auto profiles = ava::config::builtin_provider_profiles();
  for (auto& profile : profiles)
  {
    for (auto const& resolved : resolved_generics) ava::config::apply_resolved_builtin_generic_to_profile(profile, resolved);
  }
  return profiles;
}

}  // namespace

ava::core::Result<std::shared_ptr<ProviderCatalog const>> ProviderCatalog::build(ava::config::XdgPaths const& paths)
{
  auto user_definitions = ava::config::load_user_provider_definitions(paths);
  if (!user_definitions)
    return std::unexpected(std::move(user_definitions.error()));
  if (auto collision = ava::config::validate_user_provider_ids_against_builtins(*user_definitions); !collision)
    return std::unexpected(std::move(collision.error()));

  // Resolve generic built-in base URL overrides exactly once before any session
  // mutation. Factories capture the canonical endpoint; catalog profiles match.
  auto composed = compose_builtin_provider_registry(/*read_generic_base_url_env=*/true);
  if (!composed)
    return std::unexpected(std::move(composed.error()));

  auto profiles = profiles_with_resolved_generics(composed->resolved_generics);
  if (auto registered = register_user_definitions(composed->registry, profiles, *user_definitions); !registered)
    return std::unexpected(std::move(registered.error()));
  return std::make_shared<ProviderCatalog const>(ConstructionKey{}, std::move(composed->registry), std::move(profiles), std::move(*user_definitions));
}

std::shared_ptr<ProviderCatalog const> ProviderCatalog::build_builtins_only()
{
  // Unit-test / fallback composition: pin compiled defaults (no generic env
  // override reads). Production startup uses build(), which resolves env once.
  auto composed = compose_builtin_provider_registry(/*read_generic_base_url_env=*/false);
  if (!composed)
  {
    return std::make_shared<ProviderCatalog const>(ConstructionKey{}, builtin_provider_registry(), ava::config::builtin_provider_profiles(),
                                                   std::vector<ava::config::UserProviderDefinition>{});
  }
  auto profiles = profiles_with_resolved_generics(composed->resolved_generics);
  return std::make_shared<ProviderCatalog const>(ConstructionKey{}, std::move(composed->registry), std::move(profiles),
                                                 std::vector<ava::config::UserProviderDefinition>{});
}

bool ProviderCatalog::contains(std::string_view provider_id) const noexcept
{
  return registry_.contains(provider_id);
}

ava::core::Result<std::unique_ptr<Provider>> ProviderCatalog::create(std::string_view provider_id) const
{
  return registry_.create(provider_id);
}

std::vector<std::string> ProviderCatalog::provider_ids() const
{
  return registry_.provider_ids();
}

std::span<ava::config::ProviderProfile const> ProviderCatalog::profiles() const noexcept
{
  return profiles_;
}

std::optional<ava::config::ProviderProfile> ProviderCatalog::find_profile(std::string_view provider_id) const
{
  for (auto const& profile : profiles_)
  {
    if (profile.provider_id == provider_id)
      return profile;
  }
  return std::nullopt;
}

std::optional<ava::config::ProviderProfile> ProviderCatalog::profile_for_model(ava::config::ModelInfo const& model) const
{
  return find_profile(model.provider_id);
}

std::optional<ava::config::ProviderProfile> ProviderCatalog::reasoning_profile_for_model(ava::config::ModelInfo const& model) const
{
  auto profile = find_profile(model.provider_id);
  if (profile && !model.api_family.empty() && profile->api_family != model.api_family)
    profile = std::nullopt;
  if (profile)
    return profile;
  return ava::config::reasoning_provider_profile_for_model(model);
}

std::string ProviderCatalog::display_name(std::string_view provider_id) const
{
  if (auto profile = find_profile(provider_id))
    return profile->display_name;
  return ava::config::provider_display_name(provider_id);
}

bool ProviderCatalog::accepts_reasoning_format(ava::config::ModelInfo const& model, std::string_view format) const
{
  return ava::config::provider_accepts_reasoning_format(model, format);
}

ava::core::VoidResult ProviderCatalog::validate_reasoning_request(ava::config::ModelInfo const& model, std::string_view level,
                                                                  std::optional<long long> budget_tokens, std::string_view display) const
{
  return ava::config::validate_reasoning_request(model, level, budget_tokens, display);
}

ava::core::VoidResult ProviderCatalog::validate_active_model(ava::config::ModelInfo const& model) const
{
  if (model.provider_id.empty() || model.model_id.empty())
    return std::unexpected(catalog_error("active model requires provider and model ids"));
  if (!contains(model.provider_id))
  {
    auto error = ava::core::Error(ava::core::ErrorCategory::NotFound, "provider is not registered");
    error.with_context("provider", model.provider_id);
    error.with_context("model", model.model_id);
    return std::unexpected(std::move(error));
  }
  auto profile = find_profile(model.provider_id);
  if (profile && !profile->api_family.empty() && !model.api_family.empty() && profile->api_family != model.api_family)
  {
    auto error = ava::core::Error(ava::core::ErrorCategory::Configuration, "model api_family does not match provider catalog descriptor");
    error.with_context("provider", model.provider_id);
    error.with_context("model", model.model_id);
    error.with_context("model_api_family", model.api_family);
    error.with_context("provider_api_family", profile->api_family);
    return std::unexpected(std::move(error));
  }
  return {};
}

bool ProviderCatalog::provider_has_runtime_factory(std::string_view provider_id) const noexcept
{
  return contains(provider_id);
}

std::span<ava::config::UserProviderDefinition const> ProviderCatalog::user_definitions() const noexcept
{
  return user_definitions_;
}

bool ProviderCatalog::provider_auth_is_none(std::string_view provider_id) const noexcept
{
  auto profile = find_profile(provider_id);
  return profile && profile->auth_none;
}

std::string ProviderCatalog::provider_api_key_env(std::string_view provider_id) const
{
  auto profile = find_profile(provider_id);
  if (!profile || !profile->user_defined || profile->auth_none)
    return {};
  return profile->api_key_env;
}

bool ProviderCatalog::provider_is_user_defined(std::string_view provider_id) const noexcept
{
  auto profile = find_profile(provider_id);
  return profile && profile->user_defined;
}

ava::core::Result<std::shared_ptr<ProviderCatalog const>> ensure_provider_catalog(std::shared_ptr<ProviderCatalog const> existing,
                                                                                  ava::config::XdgPaths const& paths)
{
  if (existing)
    return existing;
  return ProviderCatalog::build(paths);
}

}  // namespace ava::provider

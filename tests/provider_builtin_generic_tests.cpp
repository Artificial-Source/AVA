#include "sys.h"
#include "tests/support/test_env.h"
#include "tests/support/test_harness.h"
#include "ava/app/runtime_model.h"
#include "ava/config/builtin_generic_providers.h"
#include "ava/config/model_config.h"
#include "ava/config/provider_profiles.h"
#include "ava/config/xdg_paths.h"
#include "ava/provider/catalog.h"
#include "ava/provider/provider.h"
#include "ava/provider/registry.h"

#include <algorithm>
#include <filesystem>
#include <set>
#include <string>
#include <string_view>
#include <vector>
#include <sys/stat.h>

namespace {

ava::provider::ProviderRequest sample_request(std::string_view provider_id, std::string_view model_id, long long max_tokens = 128)
{
  ava::provider::ProviderRequest request;
  request.provider_id = std::string(provider_id);
  request.model_id = std::string(model_id);
  request.system_prompt = "sys";
  request.messages = {ava::provider::ChatMessage{.role = "user", .content = "hi"}};
  request.stream = true;
  request.max_output_tokens = max_tokens;
  return request;
}

std::string header_get(ava::http::HttpRequest const& request, std::string_view key)
{
  auto it = request.headers.find(std::string(key));
  return it == request.headers.end() ? std::string{} : it->second;
}

bool header_has(ava::http::HttpRequest const& request, std::string_view key)
{
  return request.headers.find(std::string(key)) != request.headers.end();
}

std::optional<ava::config::ModelInfo> find_builtin_model(std::string_view provider_id, std::string_view model_id)
{
  auto const registry = ava::config::builtin_model_registry();
  return ava::config::find_model(registry, provider_id, model_id);
}

void test_six_providers_unique_and_registered()
{
  auto const specs = ava::config::builtin_generic_provider_specs();
  expect(specs.size() == 6, "exactly six declarative generic built-ins");

  std::set<std::string> ids;
  for (auto const& spec : specs)
  {
    expect(ids.insert(std::string(spec.provider_id)).second, "generic built-in provider ids are unique");
    expect(ava::config::find_provider_profile(spec.provider_id).has_value(), "profile is discoverable via builtin_provider_profiles");
  }

  auto registry = ava::provider::builtin_provider_registry();
  auto catalog = ava::provider::ProviderCatalog::build_builtins_only();
  for (auto const& id : {"xai", "groq", "cerebras", "together", "fireworks", "mistral"})
  {
    expect(registry.contains(id), std::string("registry contains ") + id);
    expect(catalog->contains(id), std::string("catalog contains ") + id);
    auto created = catalog->create(id);
    expect(created.has_value(), std::string("factory creates ") + id);
  }
}

void test_endpoints_env_and_protocol_metadata()
{
  struct Expectation
  {
    char const* id;
    char const* env;
    char const* base_env;
    char const* endpoint;
    ava::config::BuiltinGenericProtocol protocol;
    ava::config::BuiltinMaxTokenField max_field;
    bool stream_usage;
  };
  Expectation const expected[] = {
      {"xai", "XAI_API_KEY", "XAI_BASE_URL", "https://api.x.ai/v1/responses", ava::config::BuiltinGenericProtocol::OpenAIResponses,
       ava::config::BuiltinMaxTokenField::MaxOutputTokens, false},
      {"groq", "GROQ_API_KEY", "GROQ_BASE_URL", "https://api.groq.com/openai/v1/chat/completions", ava::config::BuiltinGenericProtocol::OpenAIChatCompletions,
       ava::config::BuiltinMaxTokenField::MaxCompletionTokens, false},
      {"cerebras", "CEREBRAS_API_KEY", "CEREBRAS_BASE_URL", "https://api.cerebras.ai/v1/chat/completions",
       ava::config::BuiltinGenericProtocol::OpenAIChatCompletions, ava::config::BuiltinMaxTokenField::MaxCompletionTokens, false},
      {"together", "TOGETHER_API_KEY", "TOGETHER_BASE_URL", "https://api.together.ai/v1/chat/completions",
       ava::config::BuiltinGenericProtocol::OpenAIChatCompletions, ava::config::BuiltinMaxTokenField::MaxTokens, false},
      {"fireworks", "FIREWORKS_API_KEY", "FIREWORKS_BASE_URL", "https://api.fireworks.ai/inference/v1/chat/completions",
       ava::config::BuiltinGenericProtocol::OpenAIChatCompletions, ava::config::BuiltinMaxTokenField::MaxTokens, true},
      {"mistral", "MISTRAL_API_KEY", "MISTRAL_BASE_URL", "https://api.mistral.ai/v1/chat/completions",
       ava::config::BuiltinGenericProtocol::OpenAIChatCompletions, ava::config::BuiltinMaxTokenField::MaxTokens, true},
  };

  for (auto const& exp : expected)
  {
    auto spec = ava::config::find_builtin_generic_provider_spec(exp.id);
    expect(spec.has_value(), std::string(exp.id) + " spec exists");
    if (!spec)
      continue;
    expect(spec->api_key_env == exp.env, std::string(exp.id) + " api key env");
    expect(spec->base_url_env == exp.base_env, std::string(exp.id) + " base url env");
    expect(ava::config::builtin_generic_canonical_endpoint(*spec) == exp.endpoint, std::string(exp.id) + " canonical endpoint");
    expect(spec->protocol == exp.protocol, std::string(exp.id) + " protocol");
    expect(spec->max_token_field == exp.max_field, std::string(exp.id) + " max token field");
    expect(spec->include_stream_usage == exp.stream_usage, std::string(exp.id) + " stream usage");

    auto profile = ava::config::find_provider_profile(exp.id);
    expect(profile && profile->connect_detail == "API key" && !profile->supports_oauth && profile->runtime_selectable,
           std::string(exp.id) + " profile connect/oauth/selectable metadata");
    expect(profile && profile->endpoint == exp.endpoint, std::string(exp.id) + " profile endpoint matches canonical");
  }
}

void test_model_metadata_contracts()
{
  struct ModelExpectation
  {
    char const* provider;
    char const* model;
    std::optional<long long> context;
    std::optional<long long> max_out;
    std::optional<long double> in_price;
    std::optional<long double> out_price;
    std::optional<long double> cache_read;
    bool images;
    bool reasoning;
  };

  ModelExpectation const expected[] = {
      {"xai", "grok-4.5", 500'000, std::nullopt, std::nullopt, std::nullopt, std::nullopt, true, true},
      {"groq", "openai/gpt-oss-120b", 131'072, 65'536, 0.15L, 0.60L, std::nullopt, false, false},
      {"groq", "openai/gpt-oss-20b", 131'072, 65'536, 0.075L, 0.30L, std::nullopt, false, false},
      {"cerebras", "gpt-oss-120b", std::nullopt, std::nullopt, 0.35L, 0.75L, std::nullopt, false, false},
      {"together", "moonshotai/Kimi-K2.7-Code", 262'144, std::nullopt, 0.95L, 4.00L, 0.19L, false, false},
      {"together", "openai/gpt-oss-120b", 128'000, std::nullopt, 0.15L, 0.60L, std::nullopt, false, false},
      {"fireworks", "accounts/fireworks/models/deepseek-v4-pro", std::nullopt, std::nullopt, 1.74L, 3.48L, 0.145L, false, false},
      {"fireworks", "accounts/fireworks/models/deepseek-v4-flash", std::nullopt, std::nullopt, 0.14L, 0.28L, 0.028L, false, false},
      {"mistral", "mistral-medium-latest", 256'000, std::nullopt, 1.5L, 7.5L, std::nullopt, false, false},
      {"mistral", "mistral-small-latest", 256'000, std::nullopt, 0.15L, 0.6L, std::nullopt, false, false},
      {"mistral", "codestral-latest", 128'000, std::nullopt, 0.3L, 0.9L, std::nullopt, false, false},
  };

  for (auto const& exp : expected)
  {
    auto model = find_builtin_model(exp.provider, exp.model);
    expect(model.has_value(), std::string(exp.provider) + "/" + exp.model + " is in builtin model registry");
    if (!model)
      continue;
    expect(model->context_window_tokens == exp.context, std::string(exp.provider) + "/" + exp.model + " context");
    expect(model->max_output_tokens == exp.max_out, std::string(exp.provider) + "/" + exp.model + " max output");
    expect(model->supports_tools.value_or(false), std::string(exp.provider) + "/" + exp.model + " tools");
    expect(model->supports_reasoning.value_or(false) == exp.reasoning, std::string(exp.provider) + "/" + exp.model + " reasoning flag");
    bool const has_image = std::ranges::find(model->input_modalities, "image") != model->input_modalities.end();
    expect(has_image == exp.images, std::string(exp.provider) + "/" + exp.model + " image modality");
    if (!exp.in_price && !exp.out_price && !exp.cache_read)
    {
      expect(!model->pricing.has_value(), std::string(exp.provider) + "/" + exp.model + " omits pricing when unknown/tiered");
    }
    else
    {
      expect(model->pricing.has_value(), std::string(exp.provider) + "/" + exp.model + " has pricing");
      if (model->pricing)
      {
        expect(model->pricing->input_per_million == exp.in_price, std::string(exp.provider) + "/" + exp.model + " input price");
        expect(model->pricing->output_per_million == exp.out_price, std::string(exp.provider) + "/" + exp.model + " output price");
        expect(model->pricing->cache_read_per_million == exp.cache_read, std::string(exp.provider) + "/" + exp.model + " cache read");
      }
    }
    if (exp.reasoning)
    {
      expect(model->reasoning_levels == std::vector<std::string>({"low", "medium", "high"}), "xAI reasoning levels");
      expect(model->api_family == "openai_responses", "xAI api_family is responses");
    }
  }
}

void test_request_body_and_auth_contracts()
{
  auto catalog = ava::provider::ProviderCatalog::build_builtins_only();

  auto xai = catalog->create("xai");
  expect(xai.has_value(), "xAI factory");
  if (xai)
  {
    ava::provider::ProviderAuthContext auth;
    auth.access_token = "xai-key";
    auth.credential_type = "api_key";
    auto req = (*xai)->build_request(sample_request("xai", "grok-4.5"), auth);
    expect(req && req->url == "https://api.x.ai/v1/responses", "xAI exact responses endpoint");
    expect(req && !req->follow_redirects, "xAI redirects disabled");
    expect(req && header_get(*req, "Authorization") == "Bearer xai-key", "xAI Bearer auth");
    expect(req && req->body.find("max_output_tokens") != std::string::npos, "xAI uses max_output_tokens");
    expect(req && req->body.find("stream_options") == std::string::npos, "xAI has no chat stream_options");
    expect(req && req->body.find("max_tokens") == std::string::npos && req->body.find("max_completion_tokens") == std::string::npos,
           "xAI does not emit chat max token fields");
    auth.credential_type = "oauth";
    auth.account_id = "acct";
    auto oauth = (*xai)->build_request(sample_request("xai", "grok-4.5"), auth);
    expect(oauth && oauth->url == "https://api.x.ai/v1/responses", "xAI never rewrites to Codex OAuth URL");
    expect(oauth && !header_has(*oauth, "ChatGPT-Account-Id"), "xAI never applies Codex account headers");
  }

  struct ChatCase
  {
    char const* id;
    char const* model;
    char const* endpoint;
    char const* max_field;  // substring in body
    bool stream_options;
    bool max_completion_quirk;
  };
  ChatCase const chats[] = {
      {"groq", "openai/gpt-oss-120b", "https://api.groq.com/openai/v1/chat/completions", "max_completion_tokens", false, true},
      {"cerebras", "gpt-oss-120b", "https://api.cerebras.ai/v1/chat/completions", "max_completion_tokens", false, true},
      {"together", "openai/gpt-oss-120b", "https://api.together.ai/v1/chat/completions", "max_tokens", false, false},
      {"fireworks", "accounts/fireworks/models/deepseek-v4-flash", "https://api.fireworks.ai/inference/v1/chat/completions", "max_tokens", true, false},
      {"mistral", "mistral-small-latest", "https://api.mistral.ai/v1/chat/completions", "max_tokens", true, false},
  };

  for (auto const& chat : chats)
  {
    auto provider = catalog->create(chat.id);
    expect(provider.has_value(), std::string(chat.id) + " factory");
    if (!provider)
      continue;
    auto model = find_builtin_model(chat.id, chat.model);
    expect(model.has_value(), std::string(chat.id) + " model for request quirks");
    auto request = sample_request(chat.id, chat.model);
    if (model)
      request.compatibility_quirks = model->compatibility_quirks;
    auto req = (*provider)->build_request(request, "secret");
    expect(req && req->url == chat.endpoint, std::string(chat.id) + " exact endpoint");
    expect(req && !req->follow_redirects, std::string(chat.id) + " redirects disabled");
    expect(req && header_get(*req, "Authorization") == "Bearer secret", std::string(chat.id) + " Bearer");
    expect(req && !header_has(*req, "x-api-key"), std::string(chat.id) + " no x-api-key");
    expect(req && req->body.find(chat.max_field) != std::string::npos, std::string(chat.id) + " max token field");
    bool const has_stream_options = req && req->body.find("stream_options") != std::string::npos;
    expect(has_stream_options == chat.stream_options, std::string(chat.id) + " stream_options contract");
    if (chat.max_completion_quirk)
      expect(req && req->body.find("\"max_tokens\"") == std::string::npos, std::string(chat.id) + " does not also emit max_tokens");
  }
}

ava::config::XdgPaths empty_catalog_paths(std::filesystem::path const& root);

void test_model_resolution_and_selector_visibility()
{
  auto const paths = empty_catalog_paths(create_empty_root("builtin-model-resolution"));
  auto catalog = ava::provider::ProviderCatalog::build_builtins_only();
  for (auto const& model_spec : ava::config::builtin_generic_model_specs())
  {
    auto resolved = ava::app::resolve_runtime_model(paths, catalog, model_spec.provider_id, model_spec.model_id);
    expect(resolved.has_value(), std::string(model_spec.provider_id) + "/" + std::string(model_spec.model_id) + " resolves via catalog");
  }

  // Profiles appear in builtin list used by /providers and connect.
  auto profiles = ava::config::builtin_provider_profiles();
  for (auto const* id : {"xai", "groq", "cerebras", "together", "fireworks", "mistral"})
  {
    expect(std::ranges::any_of(profiles, [&](auto const& p) { return p.provider_id == id; }), std::string(id) + " visible in provider profiles");
  }
}

ava::config::XdgPaths empty_catalog_paths(std::filesystem::path const& root)
{
  auto const config_home = root / "config";
  auto const state_home = root / "state";
  auto const data_home = root / "data";
  auto const ava_config = config_home / "ava";
  auto const ava_state = state_home / "ava";
  for (auto const& directory : {config_home, state_home, data_home, ava_config, ava_state, ava_state / "sessions"})
  {
    std::filesystem::create_directories(directory);
    ::chmod(directory.c_str(), 0700);
  }
  return ava::config::XdgPaths{.config_home = config_home,
                               .state_home = state_home,
                               .data_home = data_home,
                               .ava_config_dir = ava_config,
                               .ava_state_dir = ava_state,
                               .auth_file = ava_config / "auth.json",
                               .compaction_file = ava_config / "compaction.json",
                               .global_agents_file = ava_config / "AGENTS.md",
                               .models_file = ava_config / "models.json",
                               .providers_file = ava_config / "providers.json",
                               .prompts_dir = ava_config / "prompts",
                               .sessions_dir = ava_state / "sessions"};
}

void test_generic_base_url_override_validation_and_pinning()
{
  auto const root = create_empty_root("builtin-generic-base-url");
  auto paths = empty_catalog_paths(root);

  struct AdversarialCase
  {
    char const* value;
    char const* label;
  };
  AdversarialCase const adversarial[] = {
      {"http://evil.example/v1", "remote http"}, {"https://user:pass@api.x.ai", "userinfo"}, {"https://api.x.ai?x=1", "query"},
      {"https://api.x.ai#frag", "fragment"},     {"https://api.x.ai\\path", "backslash"},    {"https://api.x.ai/%2e%2e/v1", "encoded ambiguity"},
  };

  for (auto const& bad : adversarial)
  {
    ScopedEnvVar override("XAI_BASE_URL", bad.value);
    auto built = ava::provider::ProviderCatalog::build(paths);
    expect(!built, std::string("invalid XAI_BASE_URL rejected: ") + bad.label);
    if (!built)
    {
      auto const formatted = built.error().format();
      expect(formatted.find("xai") != std::string::npos && formatted.find("XAI_BASE_URL") != std::string::npos,
             std::string("invalid override error names provider/env: ") + bad.label);
      expect(formatted.find(bad.value) == std::string::npos, std::string("invalid override error omits raw value: ") + bad.label);
    }
  }

  {
    ScopedEnvVar https_override("GROQ_BASE_URL", "https://groq.override.test/openai/");
    ScopedEnvVar loopback_override("MISTRAL_BASE_URL", "http://127.0.0.1:8080");
    auto built = ava::provider::ProviderCatalog::build(paths);
    expect(built.has_value(), built ? "valid https + loopback overrides build catalog" : built.error().format());
    if (!built)
      return;

    auto groq_profile = (*built)->find_profile("groq");
    expect(groq_profile && groq_profile->default_base_url == "https://groq.override.test/openai" &&
               groq_profile->endpoint == "https://groq.override.test/openai/v1/chat/completions",
           "groq profile reports resolved base/endpoint");
    auto mistral_profile = (*built)->find_profile("mistral");
    expect(mistral_profile && mistral_profile->default_base_url == "http://127.0.0.1:8080" &&
               mistral_profile->endpoint == "http://127.0.0.1:8080/v1/chat/completions",
           "mistral loopback profile reports resolved base/endpoint");

    auto groq = (*built)->create("groq");
    expect(groq.has_value(), "groq factory creates after override pin");
    if (groq)
    {
      auto model = find_builtin_model("groq", "openai/gpt-oss-120b");
      auto request = sample_request("groq", "openai/gpt-oss-120b");
      if (model)
        request.compatibility_quirks = model->compatibility_quirks;
      auto req = (*groq)->build_request(request, "secret");
      expect(req && req->url == "https://groq.override.test/openai/v1/chat/completions", "groq request uses pinned override endpoint");
      expect(req && header_get(*req, "Authorization") == "Bearer secret", "groq Bearer still applied on override endpoint");
    }

    auto mistral = (*built)->create("mistral");
    expect(mistral.has_value(), "mistral factory creates after loopback pin");
    if (mistral)
    {
      auto model = find_builtin_model("mistral", "mistral-small-latest");
      auto request = sample_request("mistral", "mistral-small-latest");
      if (model)
        request.compatibility_quirks = model->compatibility_quirks;
      auto req = (*mistral)->build_request(request, "secret");
      expect(req && req->url == "http://127.0.0.1:8080/v1/chat/completions", "mistral request uses pinned loopback endpoint");
    }

    // Post-build env mutation must not move the already-pinned catalog endpoint.
    ScopedEnvVar mutated("GROQ_BASE_URL", "https://attacker.example/exfil");
    auto groq_again = (*built)->create("groq");
    expect(groq_again.has_value(), "groq factory still creates after env mutation");
    if (groq_again)
    {
      auto model = find_builtin_model("groq", "openai/gpt-oss-120b");
      auto request = sample_request("groq", "openai/gpt-oss-120b");
      if (model)
        request.compatibility_quirks = model->compatibility_quirks;
      auto req = (*groq_again)->build_request(request, "pin-check");
      expect(req && req->url == "https://groq.override.test/openai/v1/chat/completions", "post-build GROQ_BASE_URL mutation does not change pinned endpoint");
      expect(req && header_get(*req, "Authorization") == "Bearer pin-check", "pinned endpoint still receives Bearer credential");
      auto profile_again = (*built)->find_profile("groq");
      expect(profile_again && profile_again->endpoint == "https://groq.override.test/openai/v1/chat/completions",
             "catalog profile endpoint stays pinned after env mutation");
    }
  }
}

}  // namespace

void run_provider_builtin_generic_tests()
{
  ava::tests::HermeticTestEnvironment environment("builtin-generic-suite-");
  test_six_providers_unique_and_registered();
  test_endpoints_env_and_protocol_metadata();
  test_model_metadata_contracts();
  test_request_body_and_auth_contracts();
  test_model_resolution_and_selector_visibility();
  test_generic_base_url_override_validation_and_pinning();
}

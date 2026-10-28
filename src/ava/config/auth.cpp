#include "sys.h"
#include "ava/http/transport.h"
#include "ava/config/auth.h"
#include "ava/config/auth_file_store.h"
#include "ava/config/auth_record.h"
#include "ava/config/openai_oauth.h"
#include "ava/core/json.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace ava::config {
namespace {

constexpr long long kOAuthRefreshSkewSeconds = 300;
constexpr std::string_view kAnthropicProviderId = "anthropic";
constexpr std::string_view kAnthropicOAuthTokenEnv = "ANTHROPIC_OAUTH_TOKEN";
constexpr std::string_view kAnthropicAuthTokenEnv = "ANTHROPIC_AUTH_TOKEN";
constexpr std::string_view kAnthropicOAuthTokenUrl = "https://platform.claude.com/v1/oauth/token";
constexpr std::string_view kAnthropicOAuthClientId = "9d1c250a-e61b-44d9-88ed-5944d1962f5e";

std::string env_key_from_provider_id(std::string_view provider_id)
{
  std::string key;
  key.reserve(provider_id.size() + std::string_view("_API_KEY").size());
  for (char const ch : provider_id)
  {
    auto const uch = static_cast<unsigned char>(ch);
    if (std::isalnum(uch) != 0)
    {
      key.push_back(static_cast<char>(std::toupper(uch)));
    }
    else if (ch == '-' || ch == '_')
    {
      key.push_back('_');
    }
  }
  key += "_API_KEY";
  return key;
}

std::vector<std::string> provider_env_keys(std::string_view provider_id)
{
  std::vector<std::string> keys;
  if (provider_id == "openai")
    keys.push_back("OPENAI_API_KEY");
  if (provider_id == "anthropic")
    keys.push_back("ANTHROPIC_API_KEY");
  auto const generic = env_key_from_provider_id(provider_id);
  if (!generic.empty() && std::find(keys.begin(), keys.end(), generic) == keys.end())
    keys.push_back(generic);
  return keys;
}

std::optional<ProviderCredential> provider_credential_from_env(std::string_view provider_id)
{
  if (provider_id == kAnthropicProviderId)
  {
    for (auto const env_key : {kAnthropicOAuthTokenEnv, kAnthropicAuthTokenEnv})
    {
      char const* oauth = std::getenv(std::string(env_key).c_str());
      if (oauth == nullptr || std::string_view(oauth).empty())
        continue;
      return ProviderCredential{.provider_id = std::string(provider_id),
                                .access_token = oauth,
                                .credential_type = "oauth",
                                .account_id = "",
                                .source = "env:" + std::string(env_key)};
    }
  }
  for (auto const& key : provider_env_keys(provider_id))
  {
    char const* value = std::getenv(key.c_str());
    if (value == nullptr || std::string_view(value).empty())
      continue;
    return ProviderCredential{
        .provider_id = std::string(provider_id), .access_token = value, .credential_type = "api_key", .account_id = "", .source = "env:" + key};
  }
  return std::nullopt;
}

std::optional<std::string> oauth_token_from(std::string_view scope)
{
  auto access = ava::core::json::string_field(scope, "access_token");
  if (!access)
    access = ava::core::json::string_field(scope, "access");
  if (!access)
    access = ava::core::json::string_field(scope, "token");
  return access;
}

std::optional<std::string> api_key_from(std::string_view scope)
{
  auto key = ava::core::json::string_field(scope, "api_key");
  if (!key)
    key = ava::core::json::string_field(scope, "key");
  if (!key)
    key = ava::core::json::string_field(scope, "OPENAI_API_KEY");
  if (!key)
    key = ava::core::json::string_field(scope, "openai_api_key");
  return key;
}

std::optional<std::string> generic_api_key_from(std::string_view scope)
{
  auto key = ava::core::json::string_field(scope, "api_key");
  if (!key)
    key = ava::core::json::string_field(scope, "key");
  return key;
}

bool field_present(std::string_view object, std::string_view key)
{
  return ava::core::json::field_value_start(object, key).has_value();
}

ava::core::Error malformed_provider_credential_error(std::string_view provider_id, std::filesystem::path const& source)
{
  auto error = ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "stored provider credential is malformed or unsupported");
  error.with_context("provider", std::string(provider_id));
  error.with_context("source", source.string());
  return error;
}

std::optional<ProviderCredential> parse_provider_credential(std::string_view content, std::string_view provider_id, std::filesystem::path const& source_path)
{
  if (!ava::core::json::is_valid_object(content))
    return std::nullopt;
  auto const provider = ava::core::json::object_field(content, provider_id);
  if (!provider)
    return std::nullopt;
  std::string_view const scope(*provider);
  auto const type = ava::core::json::string_field(scope, "type");
  if (!type && field_present(scope, "type"))
    return std::nullopt;
  auto account_id = ava::core::json::string_field(scope, "account_id");
  if (!account_id && field_present(scope, "account_id"))
    return std::nullopt;
  if (!account_id)
    account_id = ava::core::json::string_field(scope, "accountId");
  if (!account_id && field_present(scope, "accountId"))
    return std::nullopt;
  auto source_metadata = ava::core::json::string_field(scope, "source");
  if (!source_metadata && field_present(scope, "source"))
    return std::nullopt;

  if (provider_id == kAnthropicProviderId && type && *type == "oauth")
  {
    auto access = oauth_token_from(scope);
    if (!access || access->empty())
      return std::nullopt;
    auto refresh = ava::core::json::string_field(scope, "refresh_token");
    if (!refresh && field_present(scope, "refresh_token"))
      return std::nullopt;
    if (!refresh)
      refresh = ava::core::json::string_field(scope, "refresh");
    if (!refresh && field_present(scope, "refresh"))
      return std::nullopt;
    auto expires = ava::core::json::integer_field(scope, "expires_at");
    if (!expires && field_present(scope, "expires_at"))
      return std::nullopt;
    if (!expires)
      expires = ava::core::json::integer_field(scope, "expires");
    if (!expires && field_present(scope, "expires"))
      return std::nullopt;
    return ProviderCredential{.provider_id = std::string(provider_id),
                              .access_token = *access,
                              .credential_type = "oauth",
                              .account_id = account_id.value_or(""),
                              .source = source_path.string(),
                              .refresh_token = refresh.value_or(""),
                              .expires_at = expires.value_or(0),
                              .source_metadata = source_metadata.value_or("")};
  }

  if (type && *type != "api" && *type != "api_key")
    return std::nullopt;
  auto key = generic_api_key_from(scope);
  if (!key)
    return std::nullopt;
  return ProviderCredential{.provider_id = std::string(provider_id),
                            .access_token = *key,
                            .credential_type = "api_key",
                            .account_id = account_id.value_or(""),
                            .source = source_path.string()};
}

ava::core::Result<std::optional<ProviderCredential>> load_provider_credential_from_auth_file(XdgPaths const& paths, std::string_view provider_id)
{
  auto content = read_text_if_exists(paths.auth_file, true);
  if (!content)
    return std::unexpected(std::move(content.error()));
  if (!content->content)
    return std::optional<ProviderCredential>{};
  auto credential = parse_provider_credential(*content->content, provider_id, paths.auth_file);
  if (credential)
    return credential;
  if (provider_id == kAnthropicProviderId && field_present(*content->content, provider_id))
  {
    return std::unexpected(malformed_provider_credential_error(provider_id, paths.auth_file));
  }
  return std::optional<ProviderCredential>{};
}

std::optional<OpenAICredential> parse_oauth_credential(std::string_view scope, std::filesystem::path const& source_path)
{
  auto access = oauth_token_from(scope);
  if (!access)
    return std::nullopt;
  auto refresh = ava::core::json::string_field(scope, "refresh_token");
  if (!refresh)
    refresh = ava::core::json::string_field(scope, "refresh");
  auto expires = ava::core::json::integer_field(scope, "expires_at");
  if (!expires)
    expires = ava::core::json::integer_field(scope, "expires");
  auto account_id = ava::core::json::string_field(scope, "account_id");
  if (!account_id)
    account_id = ava::core::json::string_field(scope, "accountId");

  return OpenAICredential{
      .type = OpenAICredentialType::OAuth,
      .access_token = *access,
      .refresh_token = refresh.value_or(""),
      .expires_at = expires.value_or(0),
      .account_id = account_id.value_or(""),
      .source_path = source_path,
  };
}

std::optional<OpenAICredential> parse_api_key_credential(std::string_view scope, std::filesystem::path const& source_path)
{
  auto key = api_key_from(scope);
  if (!key)
    return std::nullopt;
  return OpenAICredential{
      .type = OpenAICredentialType::ApiKey,
      .access_token = *key,
      .refresh_token = "",
      .expires_at = 0,
      .account_id = "",
      .source_path = source_path,
  };
}

long long unix_time_seconds()
{
  return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

bool oauth_refresh_due(long long expires_at, long long now_seconds)
{
  return expires_at > 0 &&
         (now_seconds > std::numeric_limits<long long>::max() - kOAuthRefreshSkewSeconds || expires_at <= now_seconds + kOAuthRefreshSkewSeconds);
}

bool should_refresh_openai_credential(OpenAICredential const& credential, long long now_seconds)
{
  return credential.type == OpenAICredentialType::OAuth && oauth_refresh_due(credential.expires_at, now_seconds);
}

bool should_refresh_anthropic_credential(ProviderCredential const& credential, long long now_seconds)
{
  return credential.provider_id == kAnthropicProviderId && credential.credential_type == "oauth" && oauth_refresh_due(credential.expires_at, now_seconds);
}

ava::core::Result<ProviderCredential> parse_anthropic_oauth_refresh_response(std::string_view body, ProviderCredential const& credential, long long now_seconds)
{
  if (!ava::core::json::is_valid_object(body))
  {
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Provider, "Anthropic OAuth refresh response was malformed JSON"));
  }
  auto access = ava::core::json::string_field(body, "access_token");
  if (!access || access->empty())
  {
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Provider, "Anthropic OAuth refresh response did not include an access token"));
  }
  auto refresh = ava::core::json::string_field(body, "refresh_token");
  if (!refresh || refresh->empty())
  {
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Provider, "Anthropic OAuth refresh response did not include a refresh token"));
  }
  auto expires_in = ava::core::json::integer_field(body, "expires_in");
  if (!expires_in || *expires_in <= 0 || now_seconds > std::numeric_limits<long long>::max() - *expires_in)
  {
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Provider, "Anthropic OAuth refresh response did not include a valid expires_in"));
  }
  auto account_id = ava::core::json::string_field(body, "account_id");
  if (!account_id || account_id->empty())
    account_id = credential.account_id;

  return ProviderCredential{.provider_id = std::string(kAnthropicProviderId),
                            .access_token = *access,
                            .credential_type = "oauth",
                            .account_id = account_id.value_or(""),
                            .source = credential.source,
                            .refresh_token = *refresh,
                            .expires_at = now_seconds + *expires_in,
                            .source_metadata = credential.source_metadata};
}

ava::core::Result<ProviderCredential> refresh_anthropic_oauth_credential(ProviderCredential const& credential, ava::http::Transport& transport,
                                                                         long long now_seconds)
{
  if (credential.provider_id != kAnthropicProviderId || credential.credential_type != "oauth")
  {
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "Anthropic OAuth refresh requires an Anthropic OAuth credential"));
  }
  if (credential.refresh_token.empty())
  {
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "Anthropic OAuth refresh token is missing"));
  }

  std::string body = "{\"grant_type\":\"refresh_token\",\"client_id\":\"" + ava::core::json::escape(kAnthropicOAuthClientId) + "\",\"refresh_token\":\"" +
                     ava::core::json::escape(credential.refresh_token) + "\"}";
  auto response = transport.send(ava::http::HttpRequest{.method = "POST",
                                                        .url = std::string(kAnthropicOAuthTokenUrl),
                                                        .headers = {{"Content-Type", "application/json"}, {"Accept", "application/json"}},
                                                        .body = std::move(body),
                                                        .timeout_ms = 60000,
                                                        .follow_redirects = false,
                                                        .include_response_headers = false,
                                                        .resolve_hosts = {}});
  if (!response)
    return std::unexpected(response.error());
  if (response->status_code < 200 || response->status_code >= 300)
  {
    auto error = ava::core::Error(ava::core::ErrorCategory::Provider, "Anthropic OAuth refresh failed");
    error.with_context("status", std::to_string(response->status_code));
    return std::unexpected(std::move(error));
  }
  return parse_anthropic_oauth_refresh_response(response->body, credential, now_seconds);
}

ava::core::Result<ProviderCredential> anthropic_credential_for_request(XdgPaths const& paths, ProviderCredential const& credential,
                                                                       ava::http::Transport& transport, long long now_seconds)
{
  if (credential.credential_type != "oauth")
    return credential;
  if (credential.access_token.empty())
  {
    auto error = ava::core::Error(ava::core::ErrorCategory::PermissionDenied, "Anthropic OAuth credential access token is missing");
    if (!credential.source.empty())
      error.with_context("source", credential.source);
    return std::unexpected(std::move(error));
  }
  if (!should_refresh_anthropic_credential(credential, now_seconds))
    return credential;

  if (credential.refresh_token.empty())
  {
    auto error = ava::core::Error(ava::core::ErrorCategory::PermissionDenied,
                                  "Anthropic OAuth credential needs refresh but has no refresh token; update auth.json with a fresh Anthropic "
                                  "OAuth credential or remove the stored Anthropic entry to use environment credentials");
    error.with_context("expires_at", std::to_string(credential.expires_at));
    if (!credential.source.empty())
      error.with_context("source", credential.source);
    return std::unexpected(std::move(error));
  }

  auto refreshed = refresh_anthropic_oauth_credential(credential, transport, now_seconds);
  if (!refreshed)
  {
    auto error = ava::core::Error(ava::core::ErrorCategory::PermissionDenied,
                                  "failed to refresh Anthropic OAuth credential; update auth.json with a fresh Anthropic OAuth credential or "
                                  "remove the stored Anthropic entry to use environment credentials");
    error.with_context("cause", refreshed.error().format());
    if (!credential.source.empty())
      error.with_context("source", credential.source);
    return std::unexpected(std::move(error));
  }

  refreshed->source = paths.auth_file.string();
  auto stored = store_provider_credential(paths, *refreshed);
  if (!stored)
    return std::unexpected(std::move(stored.error()));
  return refreshed;
}

}  // namespace

std::optional<OpenAICredential> parse_openai_credential(std::string_view content, std::filesystem::path const& source_path)
{
  if (!ava::core::json::is_valid_object(content))
    return std::nullopt;
  auto const openai = ava::core::json::object_field(content, "openai");
  std::string_view const scope = openai ? std::string_view(*openai) : content;
  if (auto const type = ava::core::json::string_field(scope, "type"))
  {
    if (*type == "oauth")
      return parse_oauth_credential(scope, source_path);
    if (*type == "api" || *type == "api_key")
      return parse_api_key_credential(scope, source_path);
    return std::nullopt;
  }

  if (auto oauth = parse_oauth_credential(scope, source_path))
    return oauth;
  if (auto api_key = parse_api_key_credential(scope, source_path))
    return api_key;

  return std::nullopt;
}

ava::core::Result<std::optional<OpenAICredential>> load_openai_credential(XdgPaths const& paths)
{
  auto explicit_content = read_text_if_exists(paths.auth_file, true);
  if (!explicit_content)
    return std::unexpected(explicit_content.error());
  if (explicit_content->content)
  {
    if (auto credential = parse_openai_credential(*explicit_content->content, paths.auth_file))
      return credential;
  }

  std::array const candidates{legacy_ava_credentials_path(), legacy_compatible_auth_path()};
  std::optional<OpenAICredential> first_api_key;
  for (auto const& path : candidates)
  {
    auto content = read_text_if_exists(path, false);
    if (!content)
      return std::unexpected(content.error());
    if (!content->content)
      continue;
    auto credential = parse_openai_credential(*content->content, path);
    if (!credential)
      continue;
    if (credential->type == OpenAICredentialType::OAuth)
      return credential;
    if (!first_api_key)
      first_api_key = std::move(credential);
  }
  if (first_api_key)
    return first_api_key;
  return std::optional<OpenAICredential>{};
}

ava::core::VoidResult store_openai_credential(XdgPaths const& paths, OpenAICredential const& credential)
{
  std::string body;
  if (credential.type == OpenAICredentialType::ApiKey)
  {
    body = "{\n    \"type\": \"api_key\",\n    \"api_key\": \"" + ava::core::json::escape(credential.access_token) + "\"\n  }";
  }
  else
  {
    body = "{\n    \"type\": \"oauth\",\n    \"access_token\": \"" + ava::core::json::escape(credential.access_token) + "\",\n    \"refresh_token\": \"" +
           ava::core::json::escape(credential.refresh_token) + "\",\n    \"expires_at\": " + std::to_string(credential.expires_at);
    if (!credential.account_id.empty())
    {
      body += ",\n    \"account_id\": \"" + ava::core::json::escape(credential.account_id) + "\"";
    }
    body += "\n  }";
  }

  return store_provider_object(paths, "openai", std::move(body));
}

bool is_openai_credential_expired(OpenAICredential const& credential, long long now_seconds)
{
  return credential.type == OpenAICredentialType::OAuth && credential.expires_at > 0 && credential.expires_at <= now_seconds;
}

ava::core::Result<std::string> openai_access_token_for_request(OpenAICredential const& credential, long long now_seconds)
{
  if (credential.access_token.empty())
  {
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::PermissionDenied, "OpenAI credential token is missing"));
  }
  if (is_openai_credential_expired(credential, now_seconds))
  {
    auto error = ava::core::Error(ava::core::ErrorCategory::PermissionDenied, "OpenAI OAuth credential is expired");
    error.with_context("expires_at", std::to_string(credential.expires_at));
    if (!credential.source_path.empty())
      error.with_context("source", credential.source_path.string());
    return std::unexpected(std::move(error));
  }
  return credential.access_token;
}

ava::core::Result<std::string> openai_access_token_for_request(OpenAICredential const& credential)
{
  return openai_access_token_for_request(credential, unix_time_seconds());
}

ava::core::Result<OpenAICredential> openai_credential_for_request(XdgPaths const& paths, OpenAICredential const& credential, ava::http::Transport& transport,
                                                                  long long now_seconds)
{
  if (credential.type == OpenAICredentialType::ApiKey)
  {
    auto token = openai_access_token_for_request(credential, now_seconds);
    if (!token)
      return std::unexpected(token.error());
    return credential;
  }

  if (!should_refresh_openai_credential(credential, now_seconds))
  {
    auto token = openai_access_token_for_request(credential, now_seconds);
    if (!token)
      return std::unexpected(token.error());
    return credential;
  }

  if (credential.refresh_token.empty())
  {
    auto error = ava::core::Error(ava::core::ErrorCategory::PermissionDenied,
                                  "OpenAI OAuth credential needs refresh but has no refresh token; run `ava connect "
                                  "openai` to re-authenticate");
    error.with_context("expires_at", std::to_string(credential.expires_at));
    if (!credential.source_path.empty())
      error.with_context("source", credential.source_path.string());
    return std::unexpected(std::move(error));
  }

  auto refreshed = refresh_openai_oauth_credential(credential, transport, now_seconds);
  if (!refreshed)
  {
    auto error = ava::core::Error(ava::core::ErrorCategory::PermissionDenied,
                                  "failed to refresh OpenAI OAuth credential; run `ava connect openai` to "
                                  "re-authenticate");
    error.with_context("cause", refreshed.error().format());
    if (!credential.source_path.empty())
      error.with_context("source", credential.source_path.string());
    return std::unexpected(std::move(error));
  }

  refreshed->source_path = paths.auth_file;
  auto stored = store_openai_credential(paths, *refreshed);
  if (!stored)
    return std::unexpected(stored.error());

  auto token = openai_access_token_for_request(*refreshed, now_seconds);
  if (!token)
    return std::unexpected(token.error());
  return refreshed;
}

ava::core::Result<OpenAICredential> openai_credential_for_request(XdgPaths const& paths, OpenAICredential const& credential, ava::http::Transport& transport)
{
  return openai_credential_for_request(paths, credential, transport, unix_time_seconds());
}

ava::core::Result<std::optional<ProviderCredential>> provider_credential_for_request(XdgPaths const& paths, std::string_view provider_id,
                                                                                     ava::http::Transport& transport)
{
  return provider_credential_for_request(paths, provider_id, transport, unix_time_seconds());
}

ava::core::Result<std::optional<ProviderCredential>> provider_credential_for_request(XdgPaths const& paths, std::string_view provider_id,
                                                                                     ava::http::Transport& transport, long long now_seconds)
{
  if (provider_id == "openai")
  {
    auto stored = load_openai_credential(paths);
    if (!stored)
      return std::unexpected(std::move(stored.error()));
    if (*stored)
    {
      auto credential = openai_credential_for_request(paths, **stored, transport, now_seconds);
      if (!credential)
        return std::unexpected(std::move(credential.error()));
      auto access_token = openai_access_token_for_request(*credential, now_seconds);
      if (!access_token)
        return std::unexpected(std::move(access_token.error()));
      std::string account_id = credential->account_id;
      if (credential->type == OpenAICredentialType::OAuth && account_id.empty())
      {
        account_id = openai_oauth_account_id_from_token(credential->access_token).value_or("");
      }
      return ProviderCredential{.provider_id = "openai",
                                .access_token = *access_token,
                                .credential_type = credential->type == OpenAICredentialType::OAuth ? "oauth" : "api_key",
                                .account_id = std::move(account_id),
                                .source = credential->source_path.empty() ? "auth_file" : credential->source_path.string()};
    }
  }
  else
  {
    auto stored = load_provider_credential_from_auth_file(paths, provider_id);
    if (!stored)
      return std::unexpected(std::move(stored.error()));
    if (*stored)
    {
      if (provider_id == kAnthropicProviderId)
      {
        auto credential = anthropic_credential_for_request(paths, **stored, transport, now_seconds);
        if (!credential)
          return std::unexpected(std::move(credential.error()));
        return credential;
      }
      return *stored;
    }
  }

  if (auto env_credential = provider_credential_from_env(provider_id))
    return env_credential;
  return std::optional<ProviderCredential>{};
}

ava::core::Result<std::optional<ProviderCredential>> provider_credential_for_startup(XdgPaths const& paths, std::string_view provider_id)
{
  return provider_credential_for_startup(paths, provider_id, ProviderCredentialPolicy{});
}

namespace {

std::optional<ProviderCredential> provider_credential_from_exact_env(std::string_view provider_id, std::string_view env_name)
{
  if (env_name.empty())
    return std::nullopt;
  char const* value = std::getenv(std::string(env_name).c_str());
  if (value == nullptr || std::string_view(value).empty())
    return std::nullopt;
  return ProviderCredential{
      .provider_id = std::string(provider_id), .access_token = value, .credential_type = "api_key", .account_id = "", .source = "env:" + std::string(env_name)};
}

ava::core::Result<std::optional<ProviderCredential>> resolve_user_defined_api_key_credential(XdgPaths const& paths, std::string_view provider_id,
                                                                                             std::string_view api_key_env)
{
  auto stored = load_provider_credential_from_auth_file(paths, provider_id);
  if (!stored)
    return std::unexpected(std::move(stored.error()));
  if (*stored && (*stored)->credential_type == "api_key" && !(*stored)->access_token.empty())
    return *stored;
  if (auto env_credential = provider_credential_from_exact_env(provider_id, api_key_env))
    return env_credential;
  return std::optional<ProviderCredential>{};
}

}  // namespace

ava::core::Result<std::optional<ProviderCredential>> provider_credential_for_request(XdgPaths const& paths, std::string_view provider_id,
                                                                                     ava::http::Transport& transport, ProviderCredentialPolicy const& policy)
{
  return provider_credential_for_request(paths, provider_id, transport, unix_time_seconds(), policy);
}

ava::core::Result<std::optional<ProviderCredential>> provider_credential_for_request(XdgPaths const& paths, std::string_view provider_id,
                                                                                     ava::http::Transport& transport, long long now_seconds,
                                                                                     ProviderCredentialPolicy const& policy)
{
  if (policy.auth_none)
  {
    return ProviderCredential{.provider_id = std::string(provider_id), .access_token = {}, .credential_type = "none", .account_id = {}, .source = "auth:none"};
  }
  if (policy.user_defined)
    return resolve_user_defined_api_key_credential(paths, provider_id, policy.api_key_env);
  return provider_credential_for_request(paths, provider_id, transport, now_seconds);
}

ava::core::Result<std::optional<ProviderCredential>> provider_credential_for_startup(XdgPaths const& paths, std::string_view provider_id,
                                                                                     ProviderCredentialPolicy const& policy)
{
  if (policy.auth_none)
  {
    return ProviderCredential{.provider_id = std::string(provider_id), .access_token = {}, .credential_type = "none", .account_id = {}, .source = "auth:none"};
  }
  if (policy.user_defined)
    return resolve_user_defined_api_key_credential(paths, provider_id, policy.api_key_env);

  if (provider_id == "openai")
  {
    auto stored = load_openai_credential(paths);
    if (!stored)
      return std::unexpected(std::move(stored.error()));
    if (*stored)
    {
      return ProviderCredential{.provider_id = "openai",
                                .access_token = (*stored)->access_token,
                                .credential_type = (*stored)->type == OpenAICredentialType::OAuth ? "oauth" : "api_key",
                                .account_id = (*stored)->account_id,
                                .source = (*stored)->source_path.empty() ? "auth_file" : (*stored)->source_path.string()};
    }
  }
  else
  {
    auto stored = load_provider_credential_from_auth_file(paths, provider_id);
    if (!stored)
      return std::unexpected(std::move(stored.error()));
    if (*stored)
      return *stored;
  }

  if (auto env_credential = provider_credential_from_env(provider_id))
    return env_credential;
  return std::optional<ProviderCredential>{};
}

ava::core::VoidResult store_provider_credential(XdgPaths const& paths, ProviderCredential const& credential)
{
  auto body = provider_credential_object_json(credential);
  if (!body)
    return std::unexpected(std::move(body.error()));
  return store_provider_object(paths, credential.provider_id, std::move(*body));
}

std::string authorization_header_value(OpenAICredential const& credential)
{
  return "Bearer " + credential.access_token;
}

}  // namespace ava::config

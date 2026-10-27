#pragma once

#include "ava/http/transport.h"
#include "ava/config/xdg_paths.h"
#include "ava/core/NoLogString.h"
#include "ava/core/result.h"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace ava::config {

enum class OpenAICredentialType
{
  OAuth,
  ApiKey,
};

struct OpenAICredential
{
  OpenAICredentialType type = OpenAICredentialType::OAuth;
  ava::core::NoLogString access_token;
  ava::core::NoLogString refresh_token;
  long long expires_at = 0;
  ava::core::NoLogString account_id = "";
  std::filesystem::path source_path;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

struct ProviderCredential
{
  std::string provider_id = {};
  ava::core::NoLogString access_token = {};
  std::string credential_type = {};
  ava::core::NoLogString account_id = {};
  ava::core::NoLogString source = {};
  ava::core::NoLogString refresh_token = {};
  long long expires_at = 0;
  ava::core::NoLogString source_metadata = {};

  AVA_DEBUG_PRINT_MEMBERS_ON
};

[[nodiscard]] std::optional<OpenAICredential> parse_openai_credential(std::string_view content, std::filesystem::path const& source_path = {});
[[nodiscard]] ava::core::Result<std::optional<OpenAICredential>> load_openai_credential(XdgPaths const& paths);
[[nodiscard]] ava::core::VoidResult store_openai_credential(XdgPaths const& paths, OpenAICredential const& credential);
[[nodiscard]] bool is_openai_credential_expired(OpenAICredential const& credential, long long now_seconds);
[[nodiscard]] ava::core::Result<std::string> openai_access_token_for_request(OpenAICredential const& credential, long long now_seconds);
[[nodiscard]] ava::core::Result<std::string> openai_access_token_for_request(OpenAICredential const& credential);
[[nodiscard]] ava::core::Result<OpenAICredential> openai_credential_for_request(XdgPaths const& paths, OpenAICredential const& credential,
                                                                                ava::http::Transport& transport, long long now_seconds);
[[nodiscard]] ava::core::Result<OpenAICredential> openai_credential_for_request(XdgPaths const& paths, OpenAICredential const& credential,
                                                                                ava::http::Transport& transport);
[[nodiscard]] ava::core::Result<std::optional<ProviderCredential>> provider_credential_for_request(XdgPaths const& paths, std::string_view provider_id,
                                                                                                   ava::http::Transport& transport);
[[nodiscard]] ava::core::Result<std::optional<ProviderCredential>> provider_credential_for_request(XdgPaths const& paths, std::string_view provider_id,
                                                                                                   ava::http::Transport& transport, long long now_seconds);
[[nodiscard]] ava::core::Result<std::optional<ProviderCredential>> provider_credential_for_startup(XdgPaths const& paths, std::string_view provider_id);
// Catalog-bound resolution for user-defined providers:
// - auth_none => successful credential_type "none" with no secret material
// - user api_key => stored provider-scoped key first, then only api_key_env
// - built-ins => existing provider_credential_for_request behavior
struct ProviderCredentialPolicy
{
  bool auth_none = false;
  bool user_defined = false;
  std::string api_key_env = {};

  AVA_DEBUG_PRINT_MEMBERS_ON
};
[[nodiscard]] ava::core::Result<std::optional<ProviderCredential>> provider_credential_for_request(XdgPaths const& paths, std::string_view provider_id,
                                                                                                   ava::http::Transport& transport,
                                                                                                   ProviderCredentialPolicy const& policy);
[[nodiscard]] ava::core::Result<std::optional<ProviderCredential>> provider_credential_for_request(XdgPaths const& paths, std::string_view provider_id,
                                                                                                   ava::http::Transport& transport, long long now_seconds,
                                                                                                   ProviderCredentialPolicy const& policy);
[[nodiscard]] ava::core::Result<std::optional<ProviderCredential>> provider_credential_for_startup(XdgPaths const& paths, std::string_view provider_id,
                                                                                                   ProviderCredentialPolicy const& policy);
[[nodiscard]] ava::core::VoidResult store_provider_credential(XdgPaths const& paths, ProviderCredential const& credential);
[[nodiscard]] std::string authorization_header_value(OpenAICredential const& credential);

}  // namespace ava::config

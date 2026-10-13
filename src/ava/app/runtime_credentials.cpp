#include "sys.h"
#include "ava/http/curl_transport.h"
#include "ava/app/runtime/Session.h"
#include "ava/app/runtime_credentials.h"
#include "ava/config/auth.h"
#include "ava/config/openai_oauth.h"
#include "ava/provider/catalog.h"

#include <memory>
#include <string>
#include <utility>

namespace ava::app {
namespace {

void clear_sensitive_string(std::string& value) noexcept
{
  auto* cursor = static_cast<unsigned char volatile*>(static_cast<void*>(value.data()));
  auto size = value.size();
  while (size-- > 0)
    *cursor++ = 0;
  value.clear();
}

void clear_runtime_credential_fields(runtime::RunOptions& options) noexcept
{
  clear_sensitive_string(options.access_token);
  clear_sensitive_string(options.credential_type);
  clear_sensitive_string(options.openai_account_id);
}

template <typename Cleanup>
class ScopeCleanup final
{
 public:
  explicit ScopeCleanup(Cleanup cleanup) : cleanup_(std::move(cleanup)) { }
  ~ScopeCleanup() noexcept { cleanup_(); }
  ScopeCleanup(ScopeCleanup const&) = delete;
  ScopeCleanup& operator=(ScopeCleanup const&) = delete;

 private:
  Cleanup cleanup_;
};

template <typename Cleanup>
ScopeCleanup(Cleanup) -> ScopeCleanup<Cleanup>;

ava::config::ProviderCredentialPolicy credential_policy_for(std::shared_ptr<ava::provider::ProviderCatalog const> const& catalog, std::string_view provider_id)
{
  ava::config::ProviderCredentialPolicy policy;
  if (!catalog)
    return policy;
  policy.auth_none = catalog->provider_auth_is_none(provider_id);
  policy.user_defined = catalog->provider_is_user_defined(provider_id);
  policy.api_key_env = catalog->provider_api_key_env(provider_id);
  return policy;
}

}  // namespace

ava::core::Result<runtime::RunOptions> prepare_runtime_credentials(ava::config::XdgPaths const& paths, std::string_view provider_id,
                                                                   runtime::RunOptions options, ava::http::Transport& auth_transport, std::string_view purpose,
                                                                   std::shared_ptr<ava::provider::ProviderCatalog const> catalog)
{
  ScopeCleanup wipe_options([&]() noexcept { clear_runtime_credential_fields(options); });
  if (options.offline)
    return std::unexpected(offline_provider_error(purpose));
  if (options.credential_type == "none")
  {
    clear_sensitive_string(options.access_token);
    options.openai_oauth = false;
    clear_sensitive_string(options.openai_account_id);
    return options;
  }
  if (!options.access_token.empty())
    return options;

  auto policy = credential_policy_for(catalog, provider_id);
  auto credential = ava::config::provider_credential_for_request(paths, provider_id, auth_transport, policy);
  ScopeCleanup wipe_credential([&]() noexcept {
    if (credential && *credential)
    {
      clear_sensitive_string((*credential)->provider_id);
      clear_sensitive_string((*credential)->access_token);
      clear_sensitive_string((*credential)->credential_type);
      clear_sensitive_string((*credential)->account_id);
      clear_sensitive_string((*credential)->source);
      clear_sensitive_string((*credential)->refresh_token);
      clear_sensitive_string((*credential)->source_metadata);
    }
  });
  if (!credential)
    return std::unexpected(std::move(credential.error()));
  if (!*credential)
  {
    auto error =
        ava::core::Error(ava::core::ErrorCategory::InvalidArgument, std::string(purpose) + " requires auth for provider `" + std::string(provider_id) + "`");
    error.with_context("provider", std::string(provider_id));
    error.with_context("auth_file", paths.auth_file.string());
    if (policy.user_defined && !policy.api_key_env.empty())
      error.with_context("api_key_env", policy.api_key_env);
    error.with_context("hint", "configure a provider API key environment variable or run `ava connect " + std::string(provider_id) + "`");
    return std::unexpected(std::move(error));
  }

  options.access_token = (*credential)->access_token;
  options.credential_type = (*credential)->credential_type;
  options.openai_oauth = (*credential)->provider_id == "openai" && (*credential)->credential_type == "oauth";
  options.openai_account_id = (*credential)->account_id;
  if (options.credential_type == "none")
  {
    clear_sensitive_string(options.access_token);
    options.openai_oauth = false;
    clear_sensitive_string(options.openai_account_id);
  }
  if (options.openai_oauth && options.openai_account_id.empty())
    options.openai_account_id = ava::config::openai_oauth_account_id_from_token((*credential)->access_token).value_or("");
  return options;
}

ava::core::Result<runtime::RunOptions> prepare_runtime_credentials(ava::config::XdgPaths const& paths, std::string_view provider_id,
                                                                   runtime::RunOptions options, ava::http::Transport& auth_transport, std::string_view purpose)
{
  ScopeCleanup wipe_options([&]() noexcept { clear_runtime_credential_fields(options); });
  return prepare_runtime_credentials(paths, provider_id, std::move(options), auth_transport, purpose, nullptr);
}

ava::core::Result<RuntimeProviderRunBundle> create_runtime_provider_run_bundle(runtime::session_ts const& unlocked_session, runtime::RunOptions options,
                                                                               std::string_view purpose)
{
  ScopeCleanup wipe_options([&]() noexcept { clear_runtime_credential_fields(options); });
  CRITICAL_AREA_BEGIN_CR(session);
  if (!session_r->session_process_scope())
  {
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Configuration, "runtime provider process authority is unavailable"));
  }
  auto const session_process_scope = *session_r->session_process_scope();
  auto auth_transport = std::make_unique<ava::http::CurlCliTransport>(session_process_scope);
  auto catalog = session_r->provider_catalog();
  if (!catalog)
  {
    auto built = ava::provider::ensure_provider_catalog(nullptr, session_r->paths());
    if (!built)
      return std::unexpected(std::move(built.error()));
    catalog = std::move(*built);
  }
  auto prepared =
      prepare_runtime_credentials(session_r->paths(), session_r->model_selection_r()->model.provider_id, std::move(options), *auth_transport, purpose, catalog);
  ScopeCleanup wipe_prepared([&]() noexcept {
    if (prepared)
      clear_runtime_credential_fields(*prepared);
  });
  if (!prepared)
    return std::unexpected(std::move(prepared.error()));

  auto provider = catalog->create(session_r->model_selection_r()->model.provider_id);
  if (!provider)
    return std::unexpected(std::move(provider.error()));
  CRITICAL_AREA_END_R(session);

  std::unique_ptr<ava::http::Transport> transport = std::make_unique<ava::http::CurlCliTransport>(session_process_scope);
  return RuntimeProviderRunBundle{
      .provider = std::move(*provider), .transport = std::move(transport), .auth_transport = std::move(auth_transport), .options = std::move(*prepared)};
}

}  // namespace ava::app

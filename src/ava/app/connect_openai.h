#pragma once
#include "ava/debug/print_members_on.h"
#include "ava/http/transport.h"
#include "ava/process/scope.h"
#include "ava/config/openai_oauth.h"
#include "ava/config/xdg_paths.h"
#include "ava/provider/provider.h"

#include <functional>
#include <iosfwd>
#include <optional>
#include <string>

namespace ava::app {

enum class ConnectCredentialType
{
  ApiKey,
};

struct ConnectProviderCredentialOptions
{
  std::string provider_id;
  ConnectCredentialType credential_type = ConnectCredentialType::ApiKey;
  std::optional<std::string> env_var = std::nullopt;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

struct ConnectProviderWizardOptions
{
  std::optional<std::string> provider_id = std::nullopt;
  std::optional<ConnectCredentialType> credential_type = std::nullopt;
  bool stdin_is_tty = false;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

[[nodiscard]] int run_connect_openai(ava::config::XdgPaths const& paths, std::optional<ava::process::ProcessScopeV1> process_scope = std::nullopt);
[[nodiscard]] int run_connect_openai_browser(ava::config::XdgPaths const& paths, std::ostream& out, std::ostream& err,
                                             std::optional<ava::process::ProcessScopeV1> process_scope = std::nullopt);
[[nodiscard]] int run_connect_openai_headless(ava::config::XdgPaths const& paths, std::ostream& out, std::ostream& err,
                                              std::optional<ava::process::ProcessScopeV1> process_scope = std::nullopt);
[[nodiscard]] int run_connect_openai_wizard(ava::config::XdgPaths const& paths, ConnectProviderWizardOptions const& options, std::istream& in,
                                            std::ostream& out, std::ostream& err, std::optional<ava::process::ProcessScopeV1> process_scope = std::nullopt);
[[nodiscard]] int run_connect_provider_wizard(ava::config::XdgPaths const& paths, ConnectProviderWizardOptions const& options, std::istream& in,
                                              std::ostream& out, std::ostream& err, std::optional<ava::process::ProcessScopeV1> process_scope = std::nullopt);
[[nodiscard]] int run_connect_provider_credential(ava::config::XdgPaths const& paths, ConnectProviderCredentialOptions const& options, std::istream& in,
                                                  std::ostream& out, std::ostream& err);
[[nodiscard]] ava::core::Result<ava::config::OpenAICredential> complete_openai_browser_oauth(ava::config::OpenAIOAuthSession const& session,
                                                                                             ava::http::Transport& transport, long long now_seconds,
                                                                                             std::function<bool()> cancel_requested = nullptr);
[[nodiscard]] ava::core::Result<ava::config::OpenAICredential> wait_for_openai_device_oauth(ava::config::OpenAIOAuthDeviceAuthorization const& authorization,
                                                                                            ava::http::Transport& transport, long long now_seconds,
                                                                                            std::function<bool()> cancel_requested = nullptr);

}  // namespace ava::app

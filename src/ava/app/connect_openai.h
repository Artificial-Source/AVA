#pragma once
#include "ava/debug/print_members_on.h"
#include "ava/http/transport.h"
#include "ava/process/scope.h"
#include "ava/app/ExitCode.h"
#include "ava/config/openai_oauth.h"
#include "ava/config/xdg_paths.h"
#include "ava/provider/provider.h"

#include <functional>
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

// The run_connect_* functions below implement `ava connect`.
//
// They read answers and secrets from std::cin and write prompts and results to std::cout.
// They return normally when the credential was stored, or when the provider needs none.
// Otherwise they throw an ava::app::AIAlert::ExitCode whose code is the ExitCode that main must return
// (usage, environment_unavailable, configuration_invalid, credentials_unavailable or interrupted)
// and whose message is meant to be printed after the program name.
// Tests can feed input and capture output by replacing the stream buffers of std::cin and std::cout.

// Connect OpenAI through browser OAuth, using process_scope to run curl.
void run_connect_openai(ava::config::XdgPaths const& paths, std::optional<ava::process::ProcessScopeV1> process_scope = std::nullopt);

// Connect OpenAI through browser OAuth and store the credential below paths, using process_scope to run curl.
void run_connect_openai_browser(ava::config::XdgPaths const& paths, std::optional<ava::process::ProcessScopeV1> process_scope = std::nullopt);

// Connect OpenAI through headless (device) OAuth and store the credential below paths, using process_scope to run curl.
void run_connect_openai_headless(ava::config::XdgPaths const& paths, std::optional<ava::process::ProcessScopeV1> process_scope = std::nullopt);

// Ask which OpenAI login method to use, unless options already selects one, and run it with paths and process_scope.
//
// Requires options.stdin_is_tty when it has to ask.
void run_connect_openai_wizard(ava::config::XdgPaths const& paths, ConnectProviderWizardOptions const& options,
                               std::optional<ava::process::ProcessScopeV1> process_scope = std::nullopt);

// Ask for the provider, unless options names one, and its credential, and store it below paths;
// process_scope is passed on when the provider turns out to be OpenAI.
//
// Requires options.stdin_is_tty.
void run_connect_provider_wizard(ava::config::XdgPaths const& paths, ConnectProviderWizardOptions const& options,
                                 std::optional<ava::process::ProcessScopeV1> process_scope = std::nullopt);

// Store the credential of the provider described by options below paths, reading it from the environment
// variable options.env_var when set and otherwise from all of std::cin.
void run_connect_provider_credential(ava::config::XdgPaths const& paths, ConnectProviderCredentialOptions const& options);

[[nodiscard]] ava::core::Result<ava::config::OpenAICredential> complete_openai_browser_oauth(ava::config::OpenAIOAuthSession const& session,
                                                                                             ava::http::Transport& transport, long long now_seconds,
                                                                                             std::function<bool()> cancel_requested = nullptr);
[[nodiscard]] ava::core::Result<ava::config::OpenAICredential> wait_for_openai_device_oauth(ava::config::OpenAIOAuthDeviceAuthorization const& authorization,
                                                                                            ava::http::Transport& transport, long long now_seconds,
                                                                                            std::function<bool()> cancel_requested = nullptr);

}  // namespace ava::app

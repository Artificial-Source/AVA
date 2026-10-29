#include "sys.h"
#include "ava/debug/print_members_on.h"
#include "ava/http/curl_transport.h"
#include "ava/app/browser_open.h"
#include "ava/app/command_connect.h"
#include "ava/app/command_format.h"
#include "ava/app/connect_openai.h"
#include "ava/app/runtime/Session.h"
#include "ava/config/auth.h"
#include "ava/config/openai_oauth.h"
#include "ava/config/provider_profiles.h"
#include "ava/provider/catalog.h"
#include "ava/core/text_case.h"
#include "ava/core/thread.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <functional>
#include <future>
#include <optional>

namespace ava::app {
namespace {

enum class ConnectMethod
{
  ApiKey,
  OpenAIBrowserOAuth,
  OpenAIHeadlessOAuth,
};

struct ConnectMethodSelection
{
  ConnectMethod method = ConnectMethod::ApiKey;
  bool back = false;

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

bool is_valid_connect_provider_id(std::string_view provider_id)
{
  if (provider_id.empty() || provider_id.size() > 128)
    return false;
  return std::ranges::all_of(provider_id, [](char ch) {
    auto const uch = static_cast<unsigned char>(ch);
    return std::isalnum(uch) != 0 || ch == '-' || ch == '_';
  });
}

std::string trim_secret_text(std::string secret)
{
  auto is_edge_space = [](char ch) { return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r'; };
  auto first = std::find_if_not(secret.begin(), secret.end(), is_edge_space);
  auto last = std::find_if_not(secret.rbegin(), secret.rend(), is_edge_space).base();
  if (first >= last)
    return {};
  return std::string(first, last);
}

std::string credential_type_value(std::string_view method)
{
  if (method == "api" || method == "api-key" || method == "apikey" || method == "key" || method == "api_key")
  {
    return "api_key";
  }
  return {};
}

std::string credential_type_value(ConnectMethod method)
{
  static_cast<void>(method);
  return "api_key";
}

std::string credential_type_label(ConnectMethod method)
{
  static_cast<void>(method);
  return "API key";
}

std::string connect_method_label(ConnectMethod method)
{
  switch (method)
  {
    case ConnectMethod::ApiKey:
      return "API key";
    case ConnectMethod::OpenAIBrowserOAuth:
      return "ChatGPT Pro/Plus browser OAuth";
    case ConnectMethod::OpenAIHeadlessOAuth:
      return "ChatGPT Pro/Plus headless OAuth";
  }
  return "credential";
}

std::optional<ConnectMethod> parse_connect_method(std::string_view value, std::string_view provider_id)
{
  auto const lowered = ava::core::locale_lower_copy(value);
  if (provider_id == "openai")
  {
    if (lowered == "browser" || lowered == "browser-oauth" || lowered == "browser_oauth" || lowered == "chatgpt" || lowered == "oauth")
    {
      return ConnectMethod::OpenAIBrowserOAuth;
    }
    if (lowered == "headless" || lowered == "headless-oauth" || lowered == "headless_oauth" || lowered == "device" || lowered == "device-oauth" ||
        lowered == "device_oauth")
    {
      return ConnectMethod::OpenAIHeadlessOAuth;
    }
  }

  auto const credential_type = credential_type_value(value);
  if (credential_type == "api_key")
    return ConnectMethod::ApiKey;
  return std::nullopt;
}

bool is_question_prompt_cancel(ava::core::Error const& error)
{
  return error.category() == ava::core::ErrorCategory::Tool && error.message() == "question prompt canceled";
}

long long unix_time_seconds()
{
  return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string selected_or_custom_answer(ava::agent::QuestionAnswer const& answer)
{
  if (!answer.custom_text.empty())
    return answer.custom_text;
  if (!answer.selected_options.empty())
    return answer.selected_options.front();
  return {};
}

ava::core::Result<ava::agent::QuestionAnswer> ask_connect_question(CommandRequest const& request, ava::agent::QuestionPrompt prompt)
{
  if (!request.question_resolver)
  {
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::InvalidArgument,
                                            "/connect requires the interactive TUI; use `ava connect <provider> --api-key-stdin`, "
                                            "`--api-key-env ENV`, or `ava connect openai --headless-oauth` for headless setup"));
  }
  return request.question_resolver(prompt);
}

std::string connect_provider_group_id(std::string_view provider_id)
{
  if (provider_id == "kimi" || provider_id == "moonshot")
    return "kimi-moonshot";
  return std::string(provider_id);
}

std::string connect_provider_display_name(runtime::session_ts const& unlocked_session, std::string_view provider_id)
{
  SCOPED_CRITICAL_AREA_CR(session_r, unlocked_session);
  if (session_r->provider_catalog())
    return session_r->provider_catalog()->display_name(provider_id);
  if (connect_provider_group_id(provider_id) == "kimi-moonshot")
    return "Kimi / Moonshot";
  return ava::config::provider_display_name(provider_id);
}

std::vector<ava::agent::QuestionOption> provider_options(runtime::session_ts const& unlocked_session)
{
  auto const configured_provider_id = runtime::session_ts::crat(unlocked_session)->model_selection_r()->model.provider_id;
  std::vector<ava::agent::QuestionOption> options;
  std::vector<std::string> groups;
  auto add = [&](std::string value, std::string label, std::string group) {
    if (std::ranges::find(groups, group) != groups.end())
      return;
    groups.push_back(std::move(group));
    options.push_back(ava::agent::QuestionOption{.value = std::move(value), .label = std::move(label)});
  };
  if (!configured_provider_id.empty())
  {
    add(configured_provider_id, connect_provider_display_name(unlocked_session, configured_provider_id) + " ✓",
        connect_provider_group_id(configured_provider_id));
  }
  auto ensured_provider_catalog = runtime::session_ts::crat(unlocked_session)->ensure_provider_catalog();
  for (auto const& profile : ensured_provider_catalog->profiles())
  {
    if (profile.provider_id == "kimi")
      continue;
    if (profile.provider_id == "moonshot")
    {
      add(profile.provider_id, "Kimi / Moonshot - API key", connect_provider_group_id(profile.provider_id));
      continue;
    }
    auto label = profile.display_name;
    if (!profile.connect_detail.empty())
      label += " - " + profile.connect_detail;
    add(profile.provider_id, std::move(label), connect_provider_group_id(profile.provider_id));
  }
  return options;
}

ava::core::Result<std::string> resolve_connect_provider(runtime::session_ts& unlocked_session, CommandRequest const& request,
                                                        std::vector<std::string> const& args)
{
  if (!args.empty())
    return args[0];
  auto answer = ask_connect_question(request, ava::agent::QuestionPrompt{.header = "Connect a provider",
                                                                         .question = "Select provider",
                                                                         .options = provider_options(unlocked_session),
                                                                         .multiple = false,
                                                                         .allow_custom = true,
                                                                         .secret = false,
                                                                         .modal = true,
                                                                         .searchable = true});
  if (!answer)
    return std::unexpected(std::move(answer.error()));
  return selected_or_custom_answer(*answer);
}

std::vector<ava::agent::QuestionOption> method_options_for_provider(std::string_view provider_id)
{
  if (provider_id == "openai")
  {
    return {ava::agent::QuestionOption{.value = "openai_browser_oauth", .label = "B ChatGPT Pro/Plus (browser)"},
            ava::agent::QuestionOption{.value = "openai_headless_oauth", .label = "H ChatGPT Pro/Plus (headless)"},
            ava::agent::QuestionOption{.value = "api_key", .label = "A OpenAI API key"}, ava::agent::QuestionOption{.value = "back", .label = "P Previous"}};
  }
  return {ava::agent::QuestionOption{.value = "api_key", .label = "A API key"}};
}

ava::core::Result<ConnectMethodSelection> resolve_connect_method(CommandRequest const& request, std::vector<std::string> const& args,
                                                                 std::string_view provider_id)
{
  if (args.size() >= 2)
  {
    auto method = parse_connect_method(args[1], provider_id);
    if (method)
      return ConnectMethodSelection{.method = *method, .back = false};
    auto error = ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "/connect credential type must be api-key, browser-oauth, or headless-oauth");
    error.with_context("usage", "/connect [provider] [api-key|browser-oauth|headless-oauth]");
    return std::unexpected(std::move(error));
  }

  if (provider_id != "openai")
    return ConnectMethodSelection{.method = ConnectMethod::ApiKey, .back = false};

  auto answer = ask_connect_question(request, ava::agent::QuestionPrompt{.header = provider_id == "openai" ? "Connect OpenAI" : "Connect a provider",
                                                                         .question = "Choose login method",
                                                                         .options = method_options_for_provider(provider_id),
                                                                         .multiple = false,
                                                                         .allow_custom = false,
                                                                         .secret = false,
                                                                         .modal = true});
  if (!answer)
    return std::unexpected(std::move(answer.error()));
  auto const selected = selected_or_custom_answer(*answer);
  if (selected == "back")
    return ConnectMethodSelection{.method = ConnectMethod::ApiKey, .back = true};
  if (selected == "openai_browser_oauth")
  {
    return ConnectMethodSelection{.method = ConnectMethod::OpenAIBrowserOAuth, .back = false};
  }
  if (selected == "openai_headless_oauth")
  {
    return ConnectMethodSelection{.method = ConnectMethod::OpenAIHeadlessOAuth, .back = false};
  }
  if (selected == "api_key")
    return ConnectMethodSelection{.method = ConnectMethod::ApiKey, .back = false};
  return std::unexpected(ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "unknown connect login method"));
}

ava::core::Result<std::string> prompt_connect_secret(CommandRequest const& request, std::string_view provider_id, ConnectMethod method)
{
  auto answer =
      ask_connect_question(request, ava::agent::QuestionPrompt{.header = "Connect a provider",
                                                               .question = "Paste " + credential_type_label(method) + " for " + std::string(provider_id),
                                                               .options = {},
                                                               .multiple = false,
                                                               .allow_custom = true,
                                                               .secret = true,
                                                               .modal = true});
  if (!answer)
    return std::unexpected(std::move(answer.error()));
  auto secret = trim_secret_text(selected_or_custom_answer(*answer));
  if (secret.empty())
  {
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "credential was empty"));
  }
  return secret;
}

ava::core::VoidResult prompt_oauth_wait(CommandRequest const& request, std::string header, std::string question, std::string copy_text,
                                        std::function<bool()> auto_resolve)
{
  auto answer = ask_connect_question(
      request, ava::agent::QuestionPrompt{.header = std::move(header),
                                          .question = std::move(question),
                                          .options = {ava::agent::QuestionOption{.value = "copy:" + std::move(copy_text), .label = "C Copy"}},
                                          .multiple = false,
                                          .allow_custom = false,
                                          .secret = false,
                                          .modal = true,
                                          .searchable = false,
                                          .auto_resolve = std::move(auto_resolve)});
  if (!answer)
    return std::unexpected(std::move(answer.error()));
  return {};
}

ava::core::VoidResult store_connect_credential(runtime::session_ts const& unlocked_session, std::string_view provider_id, ConnectMethod method,
                                               std::string secret)
{
  auto const paths = runtime::session_ts::crat(unlocked_session)->paths();
  if (provider_id == "openai")
  {
    auto credential = ava::config::OpenAICredential{.type = ava::config::OpenAICredentialType::ApiKey,
                                                    .access_token = std::move(secret),
                                                    .refresh_token = "",
                                                    .expires_at = 0,
                                                    .account_id = "",
                                                    .source_path = {}};
    return ava::config::store_openai_credential(paths, credential);
  }

  return ava::config::store_provider_credential(paths, ava::config::ProviderCredential{.provider_id = std::string(provider_id),
                                                                                       .access_token = std::move(secret),
                                                                                       .credential_type = credential_type_value(method),
                                                                                       .account_id = "",
                                                                                       .source = "connect"});
}

ava::core::Result<std::string> store_openai_oauth_result(runtime::session_ts const& unlocked_session, ava::config::OpenAICredential const& credential)
{
  auto const paths = runtime::session_ts::crat(unlocked_session)->paths();
  auto stored = ava::config::store_openai_credential(paths, credential);
  if (!stored)
    return std::unexpected(std::move(stored.error()));
  return "Stored OpenAI OAuth credential at " + paths.auth_file.string();
}

ava::core::Result<ava::process::ProcessScopeV1> connect_process_scope(runtime::session_ts const& unlocked_session)
{
  SCOPED_CRITICAL_AREA_CR(session_r, unlocked_session);
  if (!session_r->session_process_scope())
  {
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Configuration, "session connect process authority is unavailable"));
  }
  return *session_r->session_process_scope();
}

ava::core::Result<std::string> run_openai_browser_oauth(runtime::session_ts const& unlocked_session, CommandRequest const& request)
{
  auto process_scope = connect_process_scope(unlocked_session);
  if (!process_scope)
    return std::unexpected(std::move(process_scope.error()));
  auto oauth_session = ava::config::make_openai_oauth_session();
  if (!oauth_session)
    return std::unexpected(std::move(oauth_session.error()));

  ava::http::CurlCliTransport transport(*process_scope);
  std::atomic_bool prompt_cancelled{false};
  auto cancel_requested = [&]() { return prompt_cancelled.load() || (request.cancel_requested && request.cancel_requested()); };
  auto credential_future = ava::core::make_async(
      "openai_browser_oauth", [&]() { return complete_openai_browser_oauth(*oauth_session, transport, unix_time_seconds(), cancel_requested); });

  auto const browser_opened = open_url_in_browser(oauth_session->authorization_url);
  auto prompt = prompt_oauth_wait(request, "ChatGPT Pro/Plus (browser)",
                                  oauth_session->authorization_url + "\n\n" +
                                      (browser_opened ? "Complete authorization in your browser. This window will close automatically."
                                                      : "Open the URL in your browser. This window will close automatically.") +
                                      "\n\nWaiting for authorization...",
                                  oauth_session->authorization_url,
                                  [&]() { return credential_future.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready; });
  if (!prompt)
  {
    prompt_cancelled.store(true);
    static_cast<void>(credential_future.get());
    return std::unexpected(std::move(prompt.error()));
  }
  auto credential = credential_future.get();
  if (!credential)
    return std::unexpected(std::move(credential.error()));
  return store_openai_oauth_result(unlocked_session, *credential);
}

ava::core::Result<std::string> run_openai_headless_oauth(runtime::session_ts const& unlocked_session, CommandRequest const& request)
{
  auto process_scope = connect_process_scope(unlocked_session);
  if (!process_scope)
    return std::unexpected(std::move(process_scope.error()));
  ava::http::CurlCliTransport transport(*process_scope);
  auto authorization = ava::config::start_openai_oauth_device_authorization(transport);
  if (!authorization)
    return std::unexpected(std::move(authorization.error()));

  std::atomic_bool prompt_cancelled{false};
  auto cancel_requested = [&]() { return prompt_cancelled.load() || (request.cancel_requested && request.cancel_requested()); };
  auto credential_future = ava::core::make_async(
      "openai_device_oauth", [&]() { return wait_for_openai_device_oauth(*authorization, transport, unix_time_seconds(), cancel_requested); });

  auto prompt = prompt_oauth_wait(request, "ChatGPT Pro/Plus (headless)",
                                  authorization->verification_url + "\n\nEnter code: " + authorization->user_code + "\n\nWaiting for authorization...",
                                  authorization->verification_url + "\n" + authorization->user_code,
                                  [&]() { return credential_future.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready; });
  if (!prompt)
  {
    prompt_cancelled.store(true);
    static_cast<void>(credential_future.get());
    return std::unexpected(std::move(prompt.error()));
  }
  auto credential = credential_future.get();
  if (!credential)
    return std::unexpected(std::move(credential.error()));
  return store_openai_oauth_result(unlocked_session, *credential);
}

}  // namespace

ava::core::Result<CommandResult> run_connect_command(runtime::session_ts& unlocked_session, CommandRequest const& request)
{
  CommandResult result;
  result.handled = true;
  auto args = split_command_arguments(command_argument(request.command, "/connect"));
  if (args.size() > 2)
  {
    add_output(result, missing_argument("/connect [provider] [api-key|browser-oauth|headless-oauth]"));
    return result;
  }

  std::string provider_id;
  ConnectMethod method = ConnectMethod::ApiKey;
  while (true)
  {
    auto resolved_provider_id = resolve_connect_provider(unlocked_session, request, args);
    if (!resolved_provider_id)
    {
      if (!is_question_prompt_cancel(resolved_provider_id.error()))
        add_output(result, resolved_provider_id.error().format());
      return result;
    }
    if (!is_valid_connect_provider_id(*resolved_provider_id))
    {
      add_output(result, "provider id must contain only letters, numbers, '-' or '_'");
      return result;
    }

    auto selected_method = resolve_connect_method(request, args, *resolved_provider_id);
    if (!selected_method)
    {
      if (!is_question_prompt_cancel(selected_method.error()))
        add_output(result, selected_method.error().format());
      return result;
    }
    if (selected_method->back)
    {
      args.clear();
      continue;
    }
    provider_id = std::move(*resolved_provider_id);
    method = selected_method->method;
    break;
  }

  auto ensured_provider_catalog = runtime::session_ts::crat(unlocked_session)->ensure_provider_catalog();
  if (ensured_provider_catalog->provider_auth_is_none(provider_id))
  {
    add_output(result, ensured_provider_catalog->display_name(provider_id) + " requires no credential (auth:none). auth.json was not modified.");
    return result;
  }
  if (ensured_provider_catalog->provider_is_user_defined(provider_id) && method != ConnectMethod::ApiKey)
  {
    add_output(result, "user-defined providers only support API key credentials");
    return result;
  }

  if (method == ConnectMethod::OpenAIBrowserOAuth)
  {
    auto stored = run_openai_browser_oauth(unlocked_session, request);
    if (stored)
    {
      add_output(result, *stored);
    }
    else if (!is_question_prompt_cancel(stored.error()))
    {
      add_output(result, stored.error().format());
    }
    return result;
  }
  if (method == ConnectMethod::OpenAIHeadlessOAuth)
  {
    auto stored = run_openai_headless_oauth(unlocked_session, request);
    if (stored)
    {
      add_output(result, *stored);
    }
    else if (!is_question_prompt_cancel(stored.error()))
    {
      add_output(result, stored.error().format());
    }
    return result;
  }

  auto secret = prompt_connect_secret(request, provider_id, method);
  if (!secret)
  {
    if (!is_question_prompt_cancel(secret.error()))
      add_output(result, secret.error().format());
    return result;
  }

  auto stored = store_connect_credential(unlocked_session, provider_id, method, *secret);
  if (!stored)
  {
    add_output(result, stored.error().format());
    return result;
  }
  auto const auth_file = runtime::session_ts::rat(unlocked_session)->paths().auth_file;
  add_output(result, "Stored " + provider_id + " " + connect_method_label(method) + " credential at " + auth_file.string());
  return result;
}

}  // namespace ava::app

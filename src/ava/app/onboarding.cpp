#include "sys.h"
#include "ava/app/onboarding.h"
#include "ava/app/runtime/Session.h"
#include "ava/config/auth.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <string>

namespace ava::app {
namespace {

std::string provider_env_key(std::string_view provider_id)
{
  if (provider_id == "openai")
    return "OPENAI_API_KEY";
  if (provider_id == "anthropic")
    return "ANTHROPIC_API_KEY";

  std::string key;
  key.reserve(provider_id.size() + std::string_view("_API_KEY").size());
  for (char const ch : provider_id)
  {
    auto const byte = static_cast<unsigned char>(ch);
    if (std::isalnum(byte) != 0)
    {
      key.push_back(static_cast<char>(std::toupper(byte)));
    }
    else if (ch == '-' || ch == '_')
    {
      key.push_back('_');
    }
  }
  key += "_API_KEY";
  return key;
}

std::string provider_connect_command(std::string_view provider_id)
{
  if (provider_id == "openai")
    return "ava connect openai --headless-oauth";
  return "printf '%s\\n' \"$" + provider_env_key(provider_id) + "\" | ava connect " + std::string(provider_id) + " --api-key-stdin";
}

std::string provider_display_name(std::string_view provider_id)
{
  if (provider_id == "openai")
    return "OpenAI";
  if (provider_id == "anthropic")
    return "Anthropic";
  auto display = std::string(provider_id);
  if (!display.empty())
    display.front() = static_cast<char>(std::toupper(static_cast<unsigned char>(display.front())));
  return display;
}

std::string auth_setup_message(runtime::session_ts const& unlocked_session, std::string_view prefix)
{
  std::string configured_provider_id;
  std::filesystem::path auth_file;
  {
    SCOPED_CRITICAL_AREA_CR(session_r, unlocked_session);
    configured_provider_id = session_r->model().provider_id;
    auth_file = session_r->paths().auth_file;
  }
  auto const provider_id = configured_provider_id.empty() ? std::string("openai") : configured_provider_id;
  auto const env_key = provider_env_key(provider_id);
  std::string message(prefix);
  if (!message.empty() && message.back() != '\n')
    message.push_back('\n');
  message += "Connect with /connect or /login in this TUI, or run `" + provider_connect_command(provider_id) + "`.\n";
  message += "Environment setup also works with `" + env_key + "`.\n";
  message += "Auth file: " + auth_file.string();
  return message;
}

}  // namespace

std::optional<std::string> first_run_auth_onboarding_message(runtime::session_ts const& unlocked_session)
{
  ava::config::XdgPaths paths;
  std::string provider_id;
  {
    SCOPED_CRITICAL_AREA_CR(session_r, unlocked_session);
    paths = session_r->paths();
    provider_id = session_r->model().provider_id;
  }
  auto credential = ava::config::provider_credential_for_startup(paths, provider_id);
  if (provider_id.empty())
    provider_id = "openai";
  if (!credential)
    return "! " + provider_display_name(provider_id) + " auth unavailable · /connect";
  if (*credential)
    return std::nullopt;

  return "! " + provider_display_name(provider_id) + " not connected · /connect";
}

std::string provider_auth_required_message(runtime::session_ts const& unlocked_session, std::string_view offline_suffix)
{
  auto const configured_provider_id = runtime::session_ts::crat(unlocked_session)->model().provider_id;
  auto const provider_id = configured_provider_id.empty() ? std::string("openai") : configured_provider_id;
  auto message = auth_setup_message(unlocked_session, "Auth is required for provider `" + provider_id + "`.");
  if (!offline_suffix.empty())
  {
    message += std::string(offline_suffix);
  }
  return message;
}

}  // namespace ava::app

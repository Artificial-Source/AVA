#include "sys.h"
#include "ava/http/curl_transport.h"
#include "ava/app/browser_open.h"
#include "ava/app/connect_oauth_callback.h"
#include "ava/app/connect_openai.h"
#include "ava/app/signal_policy.h"
#include "ava/app/terminal_text.h"
#include "ava/config/auth.h"
#include "ava/config/openai_oauth.h"
#include "ava/config/provider_profiles.h"
#include "ava/provider/catalog.h"
#include "ava/core/result.h"
#include "utils/AIAlert.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>
#include <poll.h>
#include <termios.h>
#include <unistd.h>

#ifdef CWDEBUG
#include "utils/print_pointer.h"
#endif

namespace ava::app {
namespace {

constexpr std::size_t max_connect_secret_bytes = 64 * 1024;

thread_local ModeSignalLatch connect_signal_latch;
thread_local unsigned int connect_invocation_depth = 0;

class ScopedConnectSignalInvocation final
{
 public:
  ScopedConnectSignalInvocation()
  {
    if (connect_invocation_depth++ == 0)
      connect_signal_latch.reset();
  }
  ~ScopedConnectSignalInvocation() { --connect_invocation_depth; }

  ScopedConnectSignalInvocation(ScopedConnectSignalInvocation const&) = delete;
  ScopedConnectSignalInvocation& operator=(ScopedConnectSignalInvocation const&) = delete;

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

bool terminal_mode_cancelled_by_signal()
{
  return connect_signal_latch.poll();
}

enum class ConnectInputReadiness
{
  Ready,
  Cancelled,
  Failed,
};

// The stream buffer of std::cin at program start, which reads file descriptor STDIN_FILENO.
//
// Tests replace the stream buffer of std::cin with one that holds their input.
std::streambuf* const stdin_descriptor_streambuf = std::cin.rdbuf();

// Return true when std::cin currently reads STDIN_FILENO.
//
// Only then does polling STDIN_FILENO tell whether reading std::cin can block.
bool cin_reads_stdin_descriptor()
{
  return std::cin.rdbuf() == stdin_descriptor_streambuf;
}

// Wait until std::cin can be read without blocking, while polling the shared process signal bits.
//
// A replacement stream buffer (see cin_reads_stdin_descriptor) is not backed by STDIN_FILENO
// and is read directly once no signal is pending.
ConnectInputReadiness wait_for_connect_input()
{
  while (true)
  {
    if (terminal_mode_cancelled_by_signal())
      return ConnectInputReadiness::Cancelled;
    if (!cin_reads_stdin_descriptor() || std::cin.rdbuf()->in_avail() > 0)
      return ConnectInputReadiness::Ready;
    pollfd descriptor{.fd = STDIN_FILENO, .events = POLLIN, .revents = 0};
    int const ready = ::poll(&descriptor, 1, 50);
    if (ready > 0)
    {
      if ((descriptor.revents & (POLLIN | POLLHUP)) != 0)
        return ConnectInputReadiness::Ready;
      if ((descriptor.revents & (POLLERR | POLLNVAL)) != 0)
        return ConnectInputReadiness::Failed;
      continue;
    }
    if (ready < 0 && errno != EINTR)
      return ConnectInputReadiness::Failed;
  }
}

long long unix_time_seconds()
{
  return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

bool tcsetattr_with_restart(struct termios const* termios_p)
{
  bool success;
  do
    success = ::tcsetattr(STDIN_FILENO, TCSAFLUSH, termios_p) == 0;
  while (!success && errno == EINTR);
  Dout(dc::warning(!success) | error_cf, "::tcsetattr(STDIN_FILENO, TCSAFLUSH, " << print_pointer(termios_p) << ") == -1");
  return success;
}

class ScopedTerminalEcho
{
 public:
  explicit ScopedTerminalEcho(bool enabled) : active_(enabled && ::tcgetattr(STDIN_FILENO, &original_) == 0)
  {
    if (!active_)
      return;
    auto current = original_;
    current.c_lflag &= ~ECHO;
    if (!tcsetattr_with_restart(&current))
      active_ = false;
  }

  ScopedTerminalEcho(ScopedTerminalEcho const&) = delete;
  ScopedTerminalEcho& operator=(ScopedTerminalEcho const&) = delete;

  ~ScopedTerminalEcho()
  {
    if (active_)
      static_cast<void>(tcsetattr_with_restart(&original_));
  }

 private:
  bool active_ = false;
  termios original_{};
};

class ScopedTerminalRawMode
{
 public:
  explicit ScopedTerminalRawMode(bool enabled) : active_(enabled && ::tcgetattr(STDIN_FILENO, &original_) == 0)
  {
    if (!active_)
      return;
    auto current = original_;
    current.c_lflag &= static_cast<tcflag_t>(~(ICANON | ECHO));
    current.c_cc[VMIN] = 1;
    current.c_cc[VTIME] = 0;
    if (!tcsetattr_with_restart(&current))
    {
      active_ = false;
      return;
    }
  }

  ScopedTerminalRawMode(ScopedTerminalRawMode const&) = delete;
  ScopedTerminalRawMode& operator=(ScopedTerminalRawMode const&) = delete;

  ~ScopedTerminalRawMode()
  {
    if (!active_)
      return;
    static_cast<void>(tcsetattr_with_restart(&original_));
  }

 private:
  bool active_ = false;
  termios original_{};
};

struct ConnectProviderMenuItem
{
  std::string id;
  std::string label;
  std::string detail;
};

enum class MenuInputKind
{
  Character,
  Enter,
  Escape,
  Backspace,
  Up,
  Down,
  Other
};

enum class OpenAIConnectMethod
{
  BrowserOAuth,
  HeadlessOAuth,
  ApiKey,
};

struct MenuInput
{
  MenuInputKind kind = MenuInputKind::Other;
  std::string text;
};

ava::core::Error connect_error(ava::core::ErrorCategory category, std::string message)
{
  return ava::core::Error(category, std::move(message));
}

std::string credential_type_value(ConnectCredentialType type)
{
  static_cast<void>(type);
  return "api_key";
}

std::string credential_type_label(ConnectCredentialType type)
{
  static_cast<void>(type);
  return "API key";
}

std::string openai_connect_method_label(OpenAIConnectMethod method)
{
  switch (method)
  {
    case OpenAIConnectMethod::BrowserOAuth:
      return "ChatGPT Pro/Plus (browser OAuth)";
    case OpenAIConnectMethod::HeadlessOAuth:
      return "ChatGPT Pro/Plus (headless OAuth)";
    case OpenAIConnectMethod::ApiKey:
      return "OpenAI API key";
  }
  return "OpenAI login";
}

std::string connect_provider_group_id(std::string_view provider_id)
{
  if (provider_id == "kimi" || provider_id == "moonshot")
    return "kimi-moonshot";
  return std::string(provider_id);
}

std::string lower_ascii(std::string_view text)
{
  std::string lowered;
  lowered.reserve(text.size());
  for (char const ch : text)
    lowered.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
  return lowered;
}

std::vector<ConnectProviderMenuItem> connect_provider_menu_items()
{
  std::vector<ConnectProviderMenuItem> items;
  std::vector<std::string> groups;
  auto add = [&](ConnectProviderMenuItem item, std::string group) {
    if (std::ranges::find(groups, group) != groups.end())
      return;
    groups.push_back(std::move(group));
    items.push_back(std::move(item));
  };
  auto const builtin_catalog = ava::provider::ProviderCatalog::build_builtins_only();
  for (auto const& profile : builtin_catalog->profiles())
  {
    if (profile.provider_id == "kimi")
      continue;
    if (profile.provider_id == "moonshot")
    {
      add(ConnectProviderMenuItem{.id = profile.provider_id, .label = "Kimi / Moonshot", .detail = "API key"}, connect_provider_group_id(profile.provider_id));
      continue;
    }
    add(
        ConnectProviderMenuItem{
            .id = profile.provider_id, .label = profile.display_name, .detail = profile.connect_detail.empty() ? "API key" : profile.connect_detail},
        connect_provider_group_id(profile.provider_id));
  }
  return items;
}

bool provider_menu_item_matches(ConnectProviderMenuItem const& item, std::string_view query)
{
  if (query.empty())
    return true;
  auto const lowered_query = lower_ascii(query);
  auto const haystack = lower_ascii(item.id + " " + item.label + " " + item.detail);
  return haystack.find(lowered_query) != std::string::npos;
}

std::vector<ConnectProviderMenuItem> filtered_provider_menu_items(std::string_view query)
{
  std::vector<ConnectProviderMenuItem> filtered;
  for (auto item : connect_provider_menu_items())
  {
    if (provider_menu_item_matches(item, query))
      filtered.push_back(std::move(item));
  }
  return filtered;
}

void render_provider_menu(std::string_view query, std::size_t selected_index)
{
  std::ostream& out = std::cout;
  auto const filtered = filtered_provider_menu_items(query);
  out << "\x1b[2J\x1b[H";
  out << "Add credential\n\n";
  out << "Select provider\n\n";
  out << "Search: " << sanitize_terminal_text(std::string(query)) << "\n\n";
  out << "Popular\n";
  if (filtered.empty())
  {
    out << "  No matches. Press Enter to use custom provider id.\n";
  }
  else
  {
    for (std::size_t index = 0; index < filtered.size(); ++index)
    {
      auto const& item = filtered[index];
      out << (index == selected_index ? "> " : "  ") << sanitize_terminal_text(item.label) << "  " << sanitize_terminal_text(item.detail) << "\n";
    }
  }
  out << "\n↑/↓ to select • Enter: confirm • Type: search • Esc: cancel\n" << std::flush;
}

bool menu_input_pending(bool stdin_is_tty)
{
  if (std::cin.rdbuf() != nullptr && std::cin.rdbuf()->in_avail() > 0)
    return true;
  if (!stdin_is_tty || !cin_reads_stdin_descriptor())
    return false;
  pollfd descriptor{.fd = STDIN_FILENO, .events = POLLIN, .revents = 0};
  int ready = 0;
  do
  {
    ready = ::poll(&descriptor, 1, 25);
  }
  while (ready < 0 && errno == EINTR);
  return ready > 0 && (descriptor.revents & POLLIN) != 0;
}

void consume_escape_sequence_tail(bool stdin_is_tty)
{
  while (menu_input_pending(stdin_is_tty))
  {
    char ignored = 0;
    if (!std::cin.get(ignored))
      return;
    if (ignored >= '@' && ignored <= '~')
      return;
  }
}

ava::core::Result<MenuInput> read_menu_input(bool stdin_is_tty)
{
  std::istream& in = std::cin;
  auto const readiness = wait_for_connect_input();
  if (readiness == ConnectInputReadiness::Cancelled)
    return MenuInput{.kind = MenuInputKind::Escape, .text = {}};
  if (readiness == ConnectInputReadiness::Failed)
    return std::unexpected(connect_error(ava::core::ErrorCategory::Io, "failed to wait for provider menu input"));
  char ch = 0;
  if (!in.get(ch))
  {
    if (terminal_mode_cancelled_by_signal())
      return MenuInput{.kind = MenuInputKind::Escape, .text = {}};
    return std::unexpected(connect_error(ava::core::ErrorCategory::Io, "failed to read provider menu input"));
  }
  if (ch == '\n' || ch == '\r')
    return MenuInput{.kind = MenuInputKind::Enter, .text = {}};
  if (ch == '\x03' || ch == '\x04')
    return MenuInput{.kind = MenuInputKind::Escape, .text = {}};
  if (ch == '\x7f' || ch == '\b')
    return MenuInput{.kind = MenuInputKind::Backspace, .text = {}};
  if (ch == '\x1b')
  {
    if (menu_input_pending(stdin_is_tty))
    {
      char prefix = 0;
      if (!in.get(prefix))
        return MenuInput{.kind = MenuInputKind::Escape, .text = {}};
      if (prefix != '[')
      {
        static_cast<void>(in.unget());
        return MenuInput{.kind = MenuInputKind::Escape, .text = {}};
      }
      if (!menu_input_pending(stdin_is_tty))
      {
        return MenuInput{.kind = MenuInputKind::Escape, .text = {}};
      }
      char code = 0;
      if (in.get(code))
      {
        if (code == 'A')
          return MenuInput{.kind = MenuInputKind::Up, .text = {}};
        if (code == 'B')
          return MenuInput{.kind = MenuInputKind::Down, .text = {}};
        if (code >= '@' && code <= '~')
          return MenuInput{.kind = MenuInputKind::Other, .text = {}};
        consume_escape_sequence_tail(stdin_is_tty);
        return MenuInput{.kind = MenuInputKind::Other, .text = {}};
      }
    }
    return MenuInput{.kind = MenuInputKind::Escape, .text = {}};
  }
  if (std::isprint(static_cast<unsigned char>(ch)) != 0)
  {
    return MenuInput{.kind = MenuInputKind::Character, .text = std::string(1, ch)};
  }
  return MenuInput{.kind = MenuInputKind::Other, .text = {}};
}

ava::core::Result<std::string> select_provider_from_menu(bool stdin_is_tty)
{
  std::string query;
  std::size_t selected_index = 0;
  ScopedTerminalRawMode const raw_mode(stdin_is_tty);
  while (true)
  {
    auto filtered = filtered_provider_menu_items(query);
    if (!filtered.empty())
      selected_index = std::min(selected_index, filtered.size() - 1);
    render_provider_menu(query, selected_index);
    auto input = read_menu_input(stdin_is_tty);
    if (!input)
      return std::unexpected(std::move(input.error()));
    switch (input->kind)
    {
      case MenuInputKind::Character:
        query += input->text;
        selected_index = 0;
        break;
      case MenuInputKind::Backspace:
        if (!query.empty())
          query.pop_back();
        selected_index = 0;
        break;
      case MenuInputKind::Up:
        filtered = filtered_provider_menu_items(query);
        if (!filtered.empty())
          selected_index = selected_index == 0 ? filtered.size() - 1 : selected_index - 1;
        break;
      case MenuInputKind::Down:
        filtered = filtered_provider_menu_items(query);
        if (!filtered.empty())
          selected_index = (selected_index + 1) % filtered.size();
        break;
      case MenuInputKind::Enter:
        filtered = filtered_provider_menu_items(query);
        if (!filtered.empty())
          return filtered[std::min(selected_index, filtered.size() - 1)].id;
        if (!query.empty())
          return query;
        break;
      case MenuInputKind::Escape:
        return std::unexpected(connect_error(ava::core::ErrorCategory::InvalidArgument, "provider login cancelled"));
      case MenuInputKind::Other:
        break;
    }
  }
}

std::optional<OpenAIConnectMethod> parse_openai_connect_method(std::string_view value)
{
  auto const lowered = lower_ascii(value);
  if (lowered == "1" || lowered == "browser" || lowered == "browser-oauth" || lowered == "browser_oauth" || lowered == "chatgpt" || lowered == "oauth")
  {
    return OpenAIConnectMethod::BrowserOAuth;
  }
  if (lowered == "2" || lowered == "headless" || lowered == "headless-oauth" || lowered == "headless_oauth" || lowered == "device" ||
      lowered == "device-oauth" || lowered == "device_oauth")
  {
    return OpenAIConnectMethod::HeadlessOAuth;
  }
  if (lowered == "3" || lowered == "api" || lowered == "api-key" || lowered == "apikey" || lowered == "key" || lowered == "api_key")
  {
    return OpenAIConnectMethod::ApiKey;
  }
  return std::nullopt;
}

std::string trim_stdin_secret(std::string secret)
{
  auto is_edge_space = [](char ch) { return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r'; };
  auto first = std::find_if_not(secret.begin(), secret.end(), is_edge_space);
  auto last = std::find_if_not(secret.rbegin(), secret.rend(), is_edge_space).base();
  if (first >= last)
    return {};
  return std::string(first, last);
}

bool is_valid_env_var_name(std::string_view name)
{
  if (name.empty())
    return false;
  auto const first = static_cast<unsigned char>(name.front());
  if (std::isalpha(first) == 0 && name.front() != '_')
    return false;
  for (char const ch : name.substr(1))
  {
    auto const uch = static_cast<unsigned char>(ch);
    if (std::isalnum(uch) != 0 || ch == '_')
      continue;
    return false;
  }
  return true;
}

bool is_valid_connect_provider_id(std::string_view provider_id)
{
  if (provider_id.empty() || provider_id.size() > 128)
    return false;
  return std::ranges::all_of(provider_id, [](char ch) {
    auto const uch = static_cast<unsigned char>(ch);
    return std::isalnum(uch) != 0 || ch == '-' || ch == '_';
  });
}

ava::core::Result<std::string> read_prompt_line(std::string_view prompt, bool secret, bool stdin_is_tty)
{
  if (terminal_mode_cancelled_by_signal())
    return std::unexpected(connect_error(ava::core::ErrorCategory::InvalidArgument, "connect prompt cancelled"));
  std::cout << prompt << std::flush;
  std::string line;
  {
    ScopedTerminalEcho const echo_guard(secret && stdin_is_tty);
    while (true)
    {
      auto const readiness = wait_for_connect_input();
      if (readiness == ConnectInputReadiness::Cancelled)
        return std::unexpected(connect_error(ava::core::ErrorCategory::InvalidArgument, "connect prompt cancelled"));
      if (readiness == ConnectInputReadiness::Failed)
        return std::unexpected(connect_error(ava::core::ErrorCategory::Io, "failed to wait for connect prompt input"));
      char character = 0;
      if (!std::cin.get(character))
      {
        if (std::cin.eof() && !line.empty())
          break;
        return std::unexpected(connect_error(ava::core::ErrorCategory::Io, "failed to read connect prompt input"));
      }
      if (character == '\n')
        break;
      if (character != '\r')
        line.push_back(character);
    }
  }
  if (secret && stdin_is_tty)
    std::cout << '\n';
  line = trim_stdin_secret(std::move(line));
  if (line.size() > max_connect_secret_bytes)
  {
    auto error = connect_error(ava::core::ErrorCategory::InvalidArgument, "connect prompt input is too large");
    error.with_context("max_bytes", std::to_string(max_connect_secret_bytes));
    return std::unexpected(std::move(error));
  }
  return line;
}

ava::core::VoidResult store_connect_secret(ava::config::XdgPaths const& paths, std::string_view provider_id, ConnectCredentialType credential_type,
                                           std::string secret)
{
  if (!is_valid_connect_provider_id(provider_id))
  {
    return std::unexpected(connect_error(ava::core::ErrorCategory::InvalidArgument, "provider id must contain only letters, numbers, '-' or '_'"));
  }
  if (secret.empty())
  {
    return std::unexpected(connect_error(ava::core::ErrorCategory::InvalidArgument, "credential was empty"));
  }
  return ava::config::store_provider_credential(paths, ava::config::ProviderCredential{.provider_id = std::string(provider_id),
                                                                                       .access_token = std::move(secret),
                                                                                       .credential_type = credential_type_value(credential_type),
                                                                                       .account_id = "",
                                                                                       .source = "connect"});
}

ava::core::Result<std::string> read_connect_secret(ConnectProviderCredentialOptions const& options)
{
  std::istream& in = std::cin;
  if (options.env_var)
  {
    if (!is_valid_env_var_name(*options.env_var))
    {
      return std::unexpected(connect_error(ava::core::ErrorCategory::InvalidArgument, "credential env var name is invalid"));
    }
    char const* value = std::getenv(options.env_var->c_str());
    if (value == nullptr || std::string_view(value).empty())
    {
      return std::unexpected(connect_error(ava::core::ErrorCategory::PermissionDenied, "credential env var is not set"));
    }
    if (std::string_view(value).size() > max_connect_secret_bytes)
    {
      auto error = connect_error(ava::core::ErrorCategory::InvalidArgument, "credential env var value is too large");
      error.with_context("max_bytes", std::to_string(max_connect_secret_bytes));
      return std::unexpected(std::move(error));
    }
    return std::string(value);
  }

  std::string secret;
  while (true)
  {
    auto const readiness = wait_for_connect_input();
    if (readiness == ConnectInputReadiness::Cancelled)
      return std::unexpected(connect_error(ava::core::ErrorCategory::InvalidArgument, "credential input cancelled"));
    if (readiness == ConnectInputReadiness::Failed)
      return std::unexpected(connect_error(ava::core::ErrorCategory::Io, "failed to wait for credential input"));
    char ch = 0;
    if (!in.get(ch))
      break;
    if (secret.size() >= max_connect_secret_bytes)
    {
      auto error = connect_error(ava::core::ErrorCategory::InvalidArgument, "credential stdin is too large");
      error.with_context("max_bytes", std::to_string(max_connect_secret_bytes));
      return std::unexpected(std::move(error));
    }
    secret.push_back(ch);
  }
  if (terminal_mode_cancelled_by_signal())
    return std::unexpected(connect_error(ava::core::ErrorCategory::InvalidArgument, "credential input cancelled"));
  if (in.bad())
  {
    return std::unexpected(connect_error(ava::core::ErrorCategory::Io, "failed to read credential from stdin"));
  }
  secret = trim_stdin_secret(std::move(secret));
  if (secret.empty())
  {
    return std::unexpected(connect_error(ava::core::ErrorCategory::InvalidArgument, "credential stdin was empty"));
  }
  return secret;
}

}  // namespace

ava::core::Result<ava::config::OpenAICredential> complete_openai_browser_oauth(ava::config::OpenAIOAuthSession const& session, ava::http::Transport& transport,
                                                                               long long now_seconds, std::function<bool()> cancel_requested)
{
  auto callback = wait_for_oauth_callback(session.state, cancel_requested);
  if (!callback)
    return std::unexpected(std::move(callback.error()));
  return ava::config::exchange_openai_oauth_code(callback->code, session.code_verifier, transport, now_seconds);
}

ava::core::Result<ava::config::OpenAICredential> wait_for_openai_device_oauth(ava::config::OpenAIOAuthDeviceAuthorization const& authorization,
                                                                              ava::http::Transport& transport, long long now_seconds,
                                                                              std::function<bool()> cancel_requested)
{
  auto const deadline = std::chrono::steady_clock::now() + std::chrono::minutes(10);
  auto const interval = std::chrono::seconds(std::max(1, authorization.interval_seconds) + 3);
  while (std::chrono::steady_clock::now() < deadline)
  {
    if (cancel_requested && cancel_requested())
    {
      return std::unexpected(connect_error(ava::core::ErrorCategory::InvalidArgument, "OpenAI OAuth login cancelled"));
    }
    auto credential = ava::config::poll_openai_oauth_device_authorization(authorization, transport, now_seconds);
    if (!credential)
      return std::unexpected(std::move(credential.error()));
    if (*credential)
      return std::move(**credential);
    auto const sleep_until = std::min(deadline, std::chrono::steady_clock::now() + interval);
    while (std::chrono::steady_clock::now() < sleep_until)
    {
      if (cancel_requested && cancel_requested())
      {
        return std::unexpected(connect_error(ava::core::ErrorCategory::InvalidArgument, "OpenAI OAuth login cancelled"));
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
  }
  return std::unexpected(connect_error(ava::core::ErrorCategory::Io, "timed out waiting for OpenAI device OAuth"));
}

// Throw the ExitCode for a failed connect step whose error is error.
//
// A failure caused by a signal is reported as interrupted, regardless of error.
[[noreturn]] void throw_connect_failure(ExitCode code, ava::core::Error const& error)
{
  if (terminal_mode_cancelled_by_signal())
    THROW_EXIT_CODE(interrupted, "interrupted by a signal");
  THROW_EXIT_CODE_V(code, "[ERRMSG]", AIArgs("[ERRMSG]", sanitize_terminal_text(error.format())));
}

// Throw the interrupted ExitCode if a signal arrived during the connect invocation.
void throw_if_interrupted()
{
  if (terminal_mode_cancelled_by_signal())
    THROW_EXIT_CODE(interrupted, "interrupted by a signal");
}

void run_connect_openai_browser(ava::config::XdgPaths const& paths, std::optional<ava::process::ProcessScopeV1> process_scope)
{
  ScopedConnectSignalInvocation const signal_invocation;
  if (!process_scope)
    THROW_EXIT_CODE(environment_unavailable, "OpenAI OAuth process authority is unavailable");
  auto session = ava::config::make_openai_oauth_session();
  if (!session)
    THROW_EXIT_CODE(environment_unavailable, "[ERRMSG]", AIArgs("[ERRMSG]", sanitize_terminal_text(session.error().format())));
  std::cout << "Open this URL to connect AVA to OpenAI:\n\n" << session->authorization_url << "\n\n";
  if (open_url_in_browser(session->authorization_url))
  {
    std::cout << "Opened your browser.\n";
  }
  else
  {
    std::cout << "Could not open your browser automatically.\n";
  }
  std::cout << "Waiting for browser callback on http://localhost:1455/auth/callback ...\n";

  ava::http::CurlCliTransport transport(*process_scope);
  auto credential = complete_openai_browser_oauth(*session, transport, unix_time_seconds(), [] { return terminal_mode_cancelled_by_signal(); });
  if (!credential)
    throw_connect_failure(ExitCode::credentials_unavailable, credential.error());
  throw_if_interrupted();
  auto stored = ava::config::store_openai_credential(paths, *credential);
  if (!stored)
    throw_connect_failure(ExitCode::credentials_unavailable, stored.error());
  std::cout << "OpenAI OAuth credential stored at " << sanitize_terminal_text(paths.auth_file.string()) << '\n';
}

void run_connect_openai_headless(ava::config::XdgPaths const& paths, std::optional<ava::process::ProcessScopeV1> process_scope)
{
  ScopedConnectSignalInvocation const signal_invocation;
  if (!process_scope)
    THROW_EXIT_CODE(environment_unavailable, "OpenAI OAuth process authority is unavailable");
  ava::http::CurlCliTransport transport(*process_scope);
  auto authorization = ava::config::start_openai_oauth_device_authorization(transport);
  if (!authorization)
    THROW_EXIT_CODE(credentials_unavailable, "[ERRMSG]", AIArgs("[ERRMSG]", sanitize_terminal_text(authorization.error().format())));
  std::cout << "Open this URL on any device:\n\n" << authorization->verification_url << "\n\n";
  std::cout << "Enter this code: " << sanitize_terminal_text(authorization->user_code) << "\n\n";
  std::cout << "Waiting for OpenAI authorization ...\n";

  auto credential = wait_for_openai_device_oauth(*authorization, transport, unix_time_seconds(), [] { return terminal_mode_cancelled_by_signal(); });
  if (!credential)
    throw_connect_failure(ExitCode::credentials_unavailable, credential.error());
  throw_if_interrupted();
  auto stored = ava::config::store_openai_credential(paths, *credential);
  if (!stored)
    throw_connect_failure(ExitCode::credentials_unavailable, stored.error());
  std::cout << "OpenAI OAuth credential stored at " << sanitize_terminal_text(paths.auth_file.string()) << '\n';
}

void run_connect_openai(ava::config::XdgPaths const& paths, std::optional<ava::process::ProcessScopeV1> process_scope)
{
  ScopedConnectSignalInvocation const signal_invocation;
  run_connect_openai_browser(paths, std::move(process_scope));
}

void run_connect_openai_wizard(ava::config::XdgPaths const& paths, ConnectProviderWizardOptions const& options,
                               std::optional<ava::process::ProcessScopeV1> process_scope)
{
  ScopedConnectSignalInvocation const signal_invocation;
  if (options.credential_type)
  {
    run_connect_provider_wizard(
        paths,
        ConnectProviderWizardOptions{.provider_id = std::string("openai"), .credential_type = options.credential_type, .stdin_is_tty = options.stdin_is_tty},
        process_scope);
    return;
  }

  if (!options.stdin_is_tty)
    THROW_EXIT_CODE(usage, "interactive OpenAI login requires a terminal; use --browser-oauth, --headless-oauth, --api-key-stdin, or --api-key-env");

  std::cout << "OpenAI login method:\n";
  std::cout << "  1. " << openai_connect_method_label(OpenAIConnectMethod::BrowserOAuth) << '\n';
  std::cout << "  2. " << openai_connect_method_label(OpenAIConnectMethod::HeadlessOAuth) << '\n';
  std::cout << "  3. " << openai_connect_method_label(OpenAIConnectMethod::ApiKey) << '\n';
  auto method_text = read_prompt_line("Select method [1-3]: ", false, options.stdin_is_tty);
  if (!method_text)
    throw_connect_failure(ExitCode::credentials_unavailable, method_text.error());
  auto method = parse_openai_connect_method(*method_text);
  if (!method)
    THROW_EXIT_CODE(usage, "OpenAI login method must be 1, 2, or 3");
  switch (*method)
  {
    case OpenAIConnectMethod::BrowserOAuth:
      run_connect_openai_browser(paths, process_scope);
      return;
    case OpenAIConnectMethod::HeadlessOAuth:
      run_connect_openai_headless(paths, process_scope);
      return;
    case OpenAIConnectMethod::ApiKey:
      run_connect_provider_wizard(
          paths,
          ConnectProviderWizardOptions{
              .provider_id = std::string("openai"), .credential_type = ConnectCredentialType::ApiKey, .stdin_is_tty = options.stdin_is_tty},
          process_scope);
      return;
  }
}

void run_connect_provider_wizard(ava::config::XdgPaths const& paths, ConnectProviderWizardOptions const& options,
                                 std::optional<ava::process::ProcessScopeV1> process_scope)
{
  ScopedConnectSignalInvocation const signal_invocation;
  if (!options.stdin_is_tty)
    THROW_EXIT_CODE(usage, "interactive provider login requires a terminal; use --api-key-stdin or --api-key-env");

  std::string provider_id;
  if (options.provider_id)
  {
    provider_id = *options.provider_id;
  }
  else
  {
    auto provider = select_provider_from_menu(options.stdin_is_tty);
    if (!provider)
      throw_connect_failure(ExitCode::credentials_unavailable, provider.error());
    provider_id = *provider;
  }

  if (!is_valid_connect_provider_id(provider_id))
    THROW_EXIT_CODE(usage, "provider id must contain only letters, numbers, '-' or '_'");
  {
    auto catalog = ava::provider::ProviderCatalog::build(paths);
    if (!catalog)
      THROW_EXIT_CODE(configuration_invalid, "[ERRMSG]", AIArgs("[ERRMSG]", sanitize_terminal_text(catalog.error().format())));
    if ((*catalog)->provider_auth_is_none(provider_id))
    {
      std::cout << sanitize_terminal_text((*catalog)->display_name(provider_id)) << " requires no credential (auth:none). auth.json was not modified.\n";
      return;
    }
    if ((*catalog)->provider_is_user_defined(provider_id) && options.credential_type && *options.credential_type != ConnectCredentialType::ApiKey)
      THROW_EXIT_CODE(usage, "user-defined providers only support API key credentials");
  }
  if (provider_id == "openai" && !options.credential_type)
  {
    run_connect_openai_wizard(paths, ConnectProviderWizardOptions{.provider_id = provider_id, .stdin_is_tty = options.stdin_is_tty}, std::move(process_scope));
    return;
  }

  ConnectCredentialType credential_type = ConnectCredentialType::ApiKey;
  if (options.credential_type)
  {
    credential_type = *options.credential_type;
  }

  auto secret = read_prompt_line(credential_type_label(credential_type) + " for " + provider_id + ": ", true, options.stdin_is_tty);
  if (!secret)
    throw_connect_failure(ExitCode::credentials_unavailable, secret.error());
  throw_if_interrupted();
  auto stored = store_connect_secret(paths, provider_id, credential_type, *secret);
  if (!stored)
    throw_connect_failure(ExitCode::credentials_unavailable, stored.error());
  std::cout << "Stored " << sanitize_terminal_text(provider_id) << ' ' << credential_type_label(credential_type) << " credential at "
            << sanitize_terminal_text(paths.auth_file.string()) << '\n';
  if (provider_id == "openai")
  {
    std::cout << "Tip: `ava connect openai` opens the OpenAI login method picker.\n";
  }
}

void run_connect_provider_credential(ava::config::XdgPaths const& paths, ConnectProviderCredentialOptions const& options)
{
  ScopedConnectSignalInvocation const signal_invocation;
  {
    auto catalog = ava::provider::ProviderCatalog::build(paths);
    if (!catalog)
      THROW_EXIT_CODE(configuration_invalid, "[ERRMSG]", AIArgs("[ERRMSG]", sanitize_terminal_text(catalog.error().format())));
    if ((*catalog)->provider_auth_is_none(options.provider_id))
    {
      std::cout << sanitize_terminal_text((*catalog)->display_name(options.provider_id))
                << " requires no credential (auth:none). auth.json was not modified.\n";
      return;
    }
    if ((*catalog)->provider_is_user_defined(options.provider_id) && options.credential_type != ConnectCredentialType::ApiKey)
      THROW_EXIT_CODE(usage, "user-defined providers only support API key credentials");
  }
  auto secret = read_connect_secret(options);
  if (!secret)
    throw_connect_failure(ExitCode::credentials_unavailable, secret.error());
  throw_if_interrupted();

  auto stored = store_connect_secret(paths, options.provider_id, options.credential_type, *secret);
  if (!stored)
    throw_connect_failure(ExitCode::credentials_unavailable, stored.error());
  std::cout << "Stored " << credential_type_label(options.credential_type) << " credential at " << sanitize_terminal_text(paths.auth_file.string()) << '\n';
}

}  // namespace ava::app

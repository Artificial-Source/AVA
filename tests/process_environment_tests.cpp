#include "sys.h"
#include "tests/support/test_harness.h"
#include "tests/support/test_timeout.h"
#include "ava/process/environment.h"
#include "ava/process/environment_test_support.h"
#include "ava/process/scope.h"
#include "ava/process/supervisor.h"
#include "ava/core/AnchorSet.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iomanip>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#if !defined(_WIN32)
extern char** environ;
#endif

namespace {

using ava::process::EnvironmentVariableV1;
using ava::process::ExactEnvironmentV1;
using ava::process::HostEnvironmentV1;
using ava::process::testing::EnvironmentTestAccess;

ava::process::AnchoredWorkingDirectoryV1 anchored_root_cwd()
{
#if defined(_WIN32)
  return {};
#else
  auto anchors = ava::core::AnchorSet::open({"/"});
  if (!anchors)
    throw std::runtime_error(anchors.error().format());
  auto capability = ava::process::mint_anchored_working_directory(std::move(*anchors), "/");
  if (!capability)
    throw std::runtime_error(capability.error().format());
  return std::move(*capability);
#endif
}

class EnvironmentSandbox final
{
 public:
  EnvironmentSandbox()
  {
#if defined(_WIN32)
    auto current = _environ;
#else
    auto current = ::environ;
#endif
    for (; current != nullptr && *current != nullptr; ++current)
    {
      std::string_view entry(*current);
      auto const separator = entry.find('=');
      if (separator != std::string_view::npos && separator != 0)
        original_.push_back({std::string(entry.substr(0, separator)), std::string(entry.substr(separator + 1))});
    }
    clear();
  }

  EnvironmentSandbox(EnvironmentSandbox const&) = delete;
  EnvironmentSandbox& operator=(EnvironmentSandbox const&) = delete;

  ~EnvironmentSandbox()
  {
    clear();
    for (auto const& variable : original_)
      set_unchecked(variable.name, variable.value);
  }

  void set(std::string const& name, std::string const& value)
  {
    if (!set_unchecked(name, value))
      throw std::runtime_error("failed to set deterministic process environment fixture");
  }

 private:
  static bool set_unchecked(std::string const& name, std::string const& value) noexcept
  {
#if defined(_WIN32)
    return _putenv_s(name.c_str(), value.c_str()) == 0;
#else
    return ::setenv(name.c_str(), value.c_str(), 1) == 0;
#endif
  }

  static void clear() noexcept
  {
#if defined(_WIN32)
    std::vector<std::string> names;
    for (auto current = _environ; current != nullptr && *current != nullptr; ++current)
    {
      std::string_view entry(*current);
      auto const separator = entry.find('=');
      if (separator != std::string_view::npos && separator != 0)
        names.emplace_back(entry.substr(0, separator));
    }
    for (auto const& name : names)
      static_cast<void>(_putenv_s(name.c_str(), ""));
#else
    static_cast<void>(::clearenv());
#endif
  }

  std::vector<EnvironmentVariableV1> original_;
};

std::string assignment(std::string_view name, std::string_view value)
{
  return std::string(name) + "=" + std::string(value);
}

std::vector<std::string> assignments(ExactEnvironmentV1 const& environment)
{
  std::vector<std::string> result;
  for (auto const& variable : EnvironmentTestAccess::variables(environment))
    result.push_back(assignment(variable.name, variable.value));
  return result;
}

bool exact_matches(ava::core::Result<ExactEnvironmentV1> const& environment, ava::process::EnvironmentProfileV1 profile, ava::process::ProcessRoleV1 role,
                   std::vector<std::string> const& expected)
{
  return environment && environment->valid() && environment->profile() == profile &&
         environment->profile_id() == ava::process::environment_profile_id_v1(profile) && environment->role() == role && assignments(*environment) == expected;
}

bool has_assignment(ExactEnvironmentV1 const& environment, std::string_view name, std::string_view value)
{
  auto const& variables = EnvironmentTestAccess::variables(environment);
  return std::ranges::any_of(variables, [name, value](auto const& variable) { return variable.name == name && variable.value == value; });
}

bool has_name(ExactEnvironmentV1 const& environment, std::string_view name)
{
  auto const& variables = EnvironmentTestAccess::variables(environment);
  return std::ranges::any_of(variables, [name](auto const& variable) { return variable.name == name; });
}

std::vector<EnvironmentVariableV1> bash_variables()
{
  return {{"LANG", "C.UTF-8"},
          {"LC_ALL", "C.UTF-8"},
          {"LC_CTYPE", "C.UTF-8"},
          {"TZ", "UTC"},
          {"USER", "synthetic-user"},
          {"LOGNAME", "synthetic-logname"},
          {"PWD", "/logical/workspace"},
          {"PATH", "/sealed/tools:/usr/bin:/bin"},
          {"HOME", "/private/home"},
          {"XDG_CONFIG_HOME", "/private/xdg-config"},
          {"XDG_CACHE_HOME", "/private/xdg-cache"},
          {"XDG_DATA_HOME", "/private/xdg-data"},
          {"XDG_STATE_HOME", "/private/xdg-state"},
          {"TMPDIR", "/private/tmp"}};
}

void set_complete_host_fixture(EnvironmentSandbox& sandbox)
{
  sandbox.set("PATH", "/desktop/bin:/usr/bin");
  sandbox.set("HOME", "/host/home");
  sandbox.set("USER", "host-user");
  sandbox.set("LOGNAME", "host-logname");
  sandbox.set("SHELL", "/bin/host-shell");
  sandbox.set("TMPDIR", "/host/tmpdir");
  sandbox.set("TMP", "/host/tmp");
  sandbox.set("TEMP", "/host/temp");
  sandbox.set("LANG", "host-lang");
  sandbox.set("LANGUAGE", "host-language");
  sandbox.set("LC_ALL", "host-lc-all");
  sandbox.set("LC_ZZZ", "host-lc-z");
  sandbox.set("LC_AAA", "host-lc-a");
  sandbox.set("XDG_CONFIG_HOME", "/host/xdg-config");
  sandbox.set("XDG_CACHE_HOME", "/host/xdg-cache");
  sandbox.set("XDG_DATA_HOME", "/host/xdg-data");
  sandbox.set("XDG_STATE_HOME", "/host/xdg-state");
  sandbox.set("XDG_RUNTIME_DIR", "/host/xdg-runtime");
  sandbox.set("TERM", "host-term");
  sandbox.set("COLORTERM", "host-colorterm");
  sandbox.set("DISPLAY", "host-display");
  sandbox.set("WAYLAND_DISPLAY", "host-wayland");
  sandbox.set("XAUTHORITY", "/host/xauthority");
  sandbox.set("DBUS_SESSION_BUS_ADDRESS", "host-dbus");
  sandbox.set("DESKTOP_STARTUP_ID", "host-startup");
  sandbox.set("BROWSER", "/parent/browser-selector");
  sandbox.set("VISUAL", "/parent/visual-selector");
  sandbox.set("EDITOR", "/parent/editor-selector");
  for (auto const name : std::array<std::string_view, 13>{"http_proxy", "https_proxy", "ftp_proxy", "all_proxy", "no_proxy", "HTTP_PROXY", "HTTPS_PROXY",
                                                          "FTP_PROXY", "ALL_PROXY", "NO_PROXY", "CURL_CA_BUNDLE", "SSL_CERT_FILE", "SSL_CERT_DIR"})
  {
    sandbox.set(std::string(name), std::string(name) + "-ambient");
  }
  sandbox.set("AWS_SECRET_ACCESS_KEY", "ambient-cloud-secret");
  sandbox.set("OPENAI_API_KEY", "ambient-provider-secret");
  sandbox.set("AVA_ARBITRARY_CANARY", "ambient-ava-secret");
  sandbox.set("LD_PRELOAD", "ambient-loader-secret");
  sandbox.set("SSH_AUTH_SOCK", "ambient-agent-secret");
  sandbox.set("DOCKER_HOST", "ambient-docker-secret");
  sandbox.set("PYTHONPATH", "ambient-runtime-secret");
  sandbox.set("MCP_INTENTIONAL_SECRET", "ambient-mcp-secret");
}

void test_all_profile_entries_order_and_allowlists()
{
  EnvironmentSandbox sandbox;
  set_complete_host_fixture(sandbox);
  auto host = EnvironmentTestAccess::capture_host();
  expect(host.has_value(), "complete sanctioned parent fixture produces one bounded host projection");
  if (!host)
    return;

  std::vector<std::string> curl_expected{assignment("PATH", ava::process::kTrustedEnvironmentPathV1), "LANG=C.UTF-8", "LC_ALL=C.UTF-8", "PWD=/"};
  for (auto const name : std::array<std::string_view, 13>{"http_proxy", "https_proxy", "ftp_proxy", "all_proxy", "no_proxy", "HTTP_PROXY", "HTTPS_PROXY",
                                                          "FTP_PROXY", "ALL_PROXY", "NO_PROXY", "CURL_CA_BUNDLE", "SSL_CERT_FILE", "SSL_CERT_DIR"})
  {
    curl_expected.push_back(assignment(name, std::string(name) + "-ambient"));
  }

  auto curl = ava::process::make_curl_environment_v1(*host);
  auto bash = ava::process::validate_bash_environment_v1(ava::process::kBashEnvironmentProfileIdV1, "/logical/workspace", bash_variables());
  auto plugin = ava::process::make_plugin_environment_v1("/logical/plugin");
  auto mcp = ava::process::make_mcp_environment_v1(
      "/logical/mcp", {{"MCP_INTENTIONAL_SECRET", "explicit-secret"}, {"PATH", "/explicit/bin:/usr/bin"}, {"HTTP_PROXY", "explicit-proxy"}});
  auto lsp = ava::process::make_lsp_environment_v1(*host, "/logical/lsp");
  auto mermaid = ava::process::make_mermaid_environment_v1();
  auto browser = ava::process::make_browser_desktop_environment_v1(*host);
  auto clipboard = ava::process::make_clipboard_desktop_environment_v1(*host);
  auto editor = ava::process::make_external_editor_environment_v1(*host, "/private/editor/draft");

  expect(exact_matches(curl, ava::process::EnvironmentProfileV1::Curl, ava::process::ProcessRoleV1::Curl, curl_expected),
         "curl has its canonical fixed entries followed by the exact proxy and CA allowlist order");
  expect(exact_matches(bash, ava::process::EnvironmentProfileV1::Bash, ava::process::ProcessRoleV1::Bash,
                       {"LANG=C.UTF-8", "LC_ALL=C.UTF-8", "LC_CTYPE=C.UTF-8", "TZ=UTC", "USER=synthetic-user", "LOGNAME=synthetic-logname",
                        "PWD=/logical/workspace", "PATH=/sealed/tools:/usr/bin:/bin", "HOME=/private/home", "XDG_CONFIG_HOME=/private/xdg-config",
                        "XDG_CACHE_HOME=/private/xdg-cache", "XDG_DATA_HOME=/private/xdg-data", "XDG_STATE_HOME=/private/xdg-state", "TMPDIR=/private/tmp"}),
         "bash validates imported sealed values and emits the canonical fourteen-entry profile");
  expect(exact_matches(plugin, ava::process::EnvironmentProfileV1::PluginMinimal, ava::process::ProcessRoleV1::Plugin,
                       {assignment("PATH", ava::process::kTrustedEnvironmentPathV1), "LANG=C.UTF-8", "LC_ALL=C.UTF-8", "PWD=/logical/plugin"}),
         "plugin has only trusted fixed values and computed PWD");
  expect(exact_matches(mcp, ava::process::EnvironmentProfileV1::McpExplicit, ava::process::ProcessRoleV1::Mcp,
                       {"PWD=/logical/mcp", "PATH=/explicit/bin:/usr/bin", "MCP_INTENTIONAL_SECRET=explicit-secret", "HTTP_PROXY=explicit-proxy"}),
         "MCP retains intentional explicit proxy and secret grants after computed PWD and validated PATH");
  expect(exact_matches(lsp, ava::process::EnvironmentProfileV1::LspStrict, ava::process::ProcessRoleV1::Lsp,
                       {assignment("PATH", ava::process::kTrustedEnvironmentPathV1), "PWD=/logical/lsp", "HOME=/host/home", "USER=host-user",
                        "LOGNAME=host-logname", "TMPDIR=/host/tmpdir", "TMP=/host/tmp", "TEMP=/host/temp", "LANG=host-lang", "LANGUAGE=host-language",
                        "LC_ALL=host-lc-all", "LC_AAA=host-lc-a", "LC_ZZZ=host-lc-z", "XDG_CONFIG_HOME=/host/xdg-config", "XDG_CACHE_HOME=/host/xdg-cache",
                        "XDG_DATA_HOME=/host/xdg-data", "XDG_STATE_HOME=/host/xdg-state", "TERM=host-term", "COLORTERM=host-colorterm"}),
         "LSP uses trusted PATH, computed PWD, its strict identity/temp/locale/XDG/terminal allowlist, and bytewise LC order");
  expect(exact_matches(mermaid, ava::process::EnvironmentProfileV1::Mermaid, ava::process::ProcessRoleV1::Mermaid,
                       {"PATH=/usr/local/bin:/usr/bin:/bin", "LANG=C.UTF-8", "LC_ALL=C.UTF-8", "TERM=dumb", "NO_COLOR=1", "PWD=/", "AVA_MERMAID_PROTOCOL=1"}),
         "Mermaid has the exact fixed seven-entry profile");
  expect(exact_matches(browser, ava::process::EnvironmentProfileV1::BrowserDesktop, ava::process::ProcessRoleV1::BrowserOpener,
                       {"PATH=/desktop/bin:/usr/bin",
                        "HOME=/host/home",
                        "USER=host-user",
                        "LOGNAME=host-logname",
                        "TMPDIR=/host/tmpdir",
                        "TMP=/host/tmp",
                        "TEMP=/host/temp",
                        "LANG=host-lang",
                        "LANGUAGE=host-language",
                        "LC_ALL=host-lc-all",
                        "LC_AAA=host-lc-a",
                        "LC_ZZZ=host-lc-z",
                        "XDG_CONFIG_HOME=/host/xdg-config",
                        "XDG_CACHE_HOME=/host/xdg-cache",
                        "XDG_DATA_HOME=/host/xdg-data",
                        "XDG_STATE_HOME=/host/xdg-state",
                        "XDG_RUNTIME_DIR=/host/xdg-runtime",
                        "DISPLAY=host-display",
                        "WAYLAND_DISPLAY=host-wayland",
                        "XAUTHORITY=/host/xauthority",
                        "DBUS_SESSION_BUS_ADDRESS=host-dbus",
                        "DESKTOP_STARTUP_ID=host-startup"}),
         "browser desktop uses inherited validated PATH and only its desktop allowlist without forwarding BROWSER");
  expect(exact_matches(clipboard, ava::process::EnvironmentProfileV1::ClipboardDesktop, ava::process::ProcessRoleV1::ClipboardHelper,
                       {assignment("PATH", ava::process::kTrustedEnvironmentPathV1), "HOME=/host/home", "USER=host-user", "LOGNAME=host-logname",
                        "TMPDIR=/host/tmpdir", "TMP=/host/tmp", "TEMP=/host/temp", "LANG=host-lang", "LANGUAGE=host-language", "LC_ALL=host-lc-all",
                        "LC_AAA=host-lc-a", "LC_ZZZ=host-lc-z", "XDG_RUNTIME_DIR=/host/xdg-runtime", "DISPLAY=host-display", "WAYLAND_DISPLAY=host-wayland",
                        "XAUTHORITY=/host/xauthority", "DBUS_SESSION_BUS_ADDRESS=host-dbus"}),
         "clipboard uses trusted PATH and the narrower runtime/display desktop allowlist");
  expect(exact_matches(editor, ava::process::EnvironmentProfileV1::ExternalEditor, ava::process::ProcessRoleV1::ExternalEditor,
                       {"PATH=/desktop/bin:/usr/bin",
                        "HOME=/host/home",
                        "USER=host-user",
                        "LOGNAME=host-logname",
                        "SHELL=/bin/host-shell",
                        "TMPDIR=/host/tmpdir",
                        "TMP=/host/tmp",
                        "TEMP=/host/temp",
                        "LANG=host-lang",
                        "LANGUAGE=host-language",
                        "LC_ALL=host-lc-all",
                        "LC_AAA=host-lc-a",
                        "LC_ZZZ=host-lc-z",
                        "TERM=host-term",
                        "COLORTERM=host-colorterm",
                        "XDG_CONFIG_HOME=/host/xdg-config",
                        "XDG_CACHE_HOME=/host/xdg-cache",
                        "XDG_DATA_HOME=/host/xdg-data",
                        "XDG_STATE_HOME=/host/xdg-state",
                        "XDG_RUNTIME_DIR=/host/xdg-runtime",
                        "DISPLAY=host-display",
                        "WAYLAND_DISPLAY=host-wayland",
                        "XAUTHORITY=/host/xauthority",
                        "DBUS_SESSION_BUS_ADDRESS=host-dbus",
                        "AVA_EXTERNAL_EDITOR_FILE=/private/editor/draft"}),
         "external editor uses its terminal/XDG desktop allowlist and one bounded child-local draft path without forwarding selectors");

  bool no_ambient_canaries = bash && plugin && mcp && lsp && mermaid && browser && clipboard && editor;
  if (no_ambient_canaries)
  {
    for (auto const* environment : std::array<ExactEnvironmentV1 const*, 8>{&*bash, &*plugin, &*mcp, &*lsp, &*mermaid, &*browser, &*clipboard, &*editor})
    {
      no_ambient_canaries = no_ambient_canaries && !has_assignment(*environment, "HTTP_PROXY", "HTTP_PROXY-ambient") &&
                            !has_assignment(*environment, "CURL_CA_BUNDLE", "CURL_CA_BUNDLE-ambient") && !has_name(*environment, "AWS_SECRET_ACCESS_KEY") &&
                            !has_name(*environment, "OPENAI_API_KEY") && !has_name(*environment, "AVA_ARBITRARY_CANARY") &&
                            !has_name(*environment, "LD_PRELOAD") && !has_name(*environment, "SSH_AUTH_SOCK") && !has_name(*environment, "DOCKER_HOST") &&
                            !has_name(*environment, "PYTHONPATH");
    }
  }
  expect(
      no_ambient_canaries && mcp && has_assignment(*mcp, "HTTP_PROXY", "explicit-proxy") && has_assignment(*mcp, "MCP_INTENTIONAL_SECRET", "explicit-secret"),
      "only curl receives ambient proxy/CA while MCP retains only explicitly granted equivalents and every other credential canary is absent");
  expect(host->selector(ava::process::HostEnvironmentSelectorV1::Browser) == "/parent/browser-selector" &&
             host->selector(ava::process::HostEnvironmentSelectorV1::Visual) == "/parent/visual-selector" &&
             host->selector(ava::process::HostEnvironmentSelectorV1::Editor) == "/parent/editor-selector",
         "the host projection exposes only closed parent selector lookup");
  expect(std::string_view(std::getenv("HOME")) == "/host/home" && std::string_view(std::getenv("HTTP_PROXY")) == "HTTP_PROXY-ambient" &&
             std::string_view(std::getenv("BROWSER")) == "/parent/browser-selector",
         "capture and all nine derivations leave the parent environment unchanged");
}

void test_input_and_aggregate_bounds()
{
  EnvironmentSandbox sandbox;

  std::vector<EnvironmentVariableV1> sixty_four;
  for (std::size_t index = 0; index < ava::process::kMaxMcpExplicitEnvironmentEntriesV1; ++index)
    sixty_four.push_back({"E" + std::to_string(index), "v"});
  auto count_accepted = ava::process::make_mcp_environment_v1("/", sixty_four);
  sixty_four.push_back({"one-too-many", "v"});
  auto count_rejected = ava::process::make_mcp_environment_v1("/", sixty_four);

  auto name_accepted = ava::process::make_mcp_environment_v1("/", {{std::string(ava::process::kMaxEnvironmentNameBytesV1, 'N'), "v"}});
  auto name_rejected = ava::process::make_mcp_environment_v1("/", {{std::string(ava::process::kMaxEnvironmentNameBytesV1 + 1, 'N'), "v"}});
  auto value_accepted = ava::process::make_mcp_environment_v1("/", {{"VALUE", std::string(ava::process::kMaxEnvironmentValueBytesV1, 'v')}});
  auto value_rejected = ava::process::make_mcp_environment_v1("/", {{"VALUE", std::string(ava::process::kMaxEnvironmentValueBytesV1 + 1, 'v')}});
  auto empty_name = ava::process::make_mcp_environment_v1("/", {{"", "v"}});
  auto nul_name = ava::process::make_mcp_environment_v1("/", {{std::string("N\0AME", 5), "v"}});
  auto equals_name = ava::process::make_mcp_environment_v1("/", {{"N=AME", "v"}});
  auto nul_value = ava::process::make_mcp_environment_v1("/", {{"NAME", std::string("v\0x", 3)}});
  auto duplicate = ava::process::make_mcp_environment_v1("/", {{"NAME", "one"}, {"NAME", "two"}});

  std::vector<EnvironmentVariableV1> aggregate;
  std::size_t const fixed_size = EnvironmentTestAccess::encoded_size(*ava::process::make_mcp_environment_v1("/", {}));
  std::size_t remaining = ava::process::kMaxEnvironmentEncodedBytesV1 - fixed_size;
  for (std::size_t index = 0; index < ava::process::kMaxMcpExplicitEnvironmentEntriesV1; ++index)
  {
    std::ostringstream name;
    name << 'E' << std::setw(2) << std::setfill('0') << index;
    auto const overhead = name.str().size() + 2;
    auto const value_size = index + 1 == ava::process::kMaxMcpExplicitEnvironmentEntriesV1 ? remaining - overhead : ava::process::kMaxEnvironmentValueBytesV1;
    aggregate.push_back({name.str(), std::string(value_size, 'a')});
    remaining -= overhead + value_size;
  }
  auto aggregate_accepted = ava::process::make_mcp_environment_v1("/", aggregate);
  aggregate.back().value.push_back('x');
  auto aggregate_rejected = ava::process::make_mcp_environment_v1("/", aggregate);

  expect(count_accepted && !count_rejected && name_accepted && !name_rejected && value_accepted && !value_rejected,
         "explicit MCP accepts exactly 64 entries, 128-byte names, and 16-KiB values and rejects each first excess");
  expect(!empty_name && !nul_name && !equals_name && !nul_value && !duplicate,
         "raw bounded input rejects empty, NUL, equals-bearing, and duplicate names plus NUL values");
  expect(aggregate_accepted && EnvironmentTestAccess::encoded_size(*aggregate_accepted) == ava::process::kMaxEnvironmentEncodedBytesV1 && !aggregate_rejected,
         "complete exact environment accepts exactly one MiB of name=value-NUL encoding and rejects the next byte");

  for (std::size_t index = 0; index < 254; ++index)
    sandbox.set("LC_COUNT_" + std::to_string(index), "v");
  auto host_254 = EnvironmentTestAccess::capture_host();
  auto exact_256 = host_254 ? ava::process::make_lsp_environment_v1(*host_254, "/") : ava::core::Result<ExactEnvironmentV1>(std::unexpected(host_254.error()));
  sandbox.set("LC_COUNT_254", "v");
  auto host_255 = EnvironmentTestAccess::capture_host();
  auto exact_257 = host_255 ? ava::process::make_lsp_environment_v1(*host_255, "/") : ava::core::Result<ExactEnvironmentV1>(std::unexpected(host_255.error()));
  sandbox.set("LC_COUNT_255", "v");
  auto host_256 = EnvironmentTestAccess::capture_host();
  sandbox.set("LC_COUNT_256", "v");
  auto host_257 = EnvironmentTestAccess::capture_host();
  expect(exact_256 && EnvironmentTestAccess::variables(*exact_256).size() == ava::process::kMaxEnvironmentEntriesV1 && !exact_257 && host_256 &&
             EnvironmentTestAccess::host_variables(*host_256).size() == ava::process::kMaxEnvironmentEntriesV1 && !host_257,
         "host projection and complete exact environments accept 256 entries and reject the first excess independently");
}

void test_host_name_value_and_aggregate_bounds()
{
  {
    EnvironmentSandbox sandbox;
    sandbox.set("LC_" + std::string(ava::process::kMaxEnvironmentNameBytesV1 - 3, 'N'), "v");
    auto accepted = EnvironmentTestAccess::capture_host();
    sandbox.set("LC_" + std::string(ava::process::kMaxEnvironmentNameBytesV1 - 2, 'N'), "v");
    auto rejected = EnvironmentTestAccess::capture_host();
    expect(accepted && !rejected, "host projection accepts a 128-byte sanctioned name and rejects a 129-byte sanctioned name");
  }
  {
    EnvironmentSandbox sandbox;
    sandbox.set("LC_VALUE_BOUND", std::string(ava::process::kMaxEnvironmentValueBytesV1, 'v'));
    auto accepted = EnvironmentTestAccess::capture_host();
    sandbox.set("LC_VALUE_BOUND", std::string(ava::process::kMaxEnvironmentValueBytesV1 + 1, 'v'));
    auto rejected = EnvironmentTestAccess::capture_host();
    expect(accepted && !rejected, "host projection accepts a 16-KiB sanctioned value and rejects its first excess byte");
  }
  {
    EnvironmentSandbox sandbox;
    std::size_t remaining = ava::process::kMaxEnvironmentEncodedBytesV1;
    std::string last_name;
    std::string last_value;
    for (std::size_t index = 0; index < 64; ++index)
    {
      std::ostringstream name;
      name << "LC_" << std::setw(2) << std::setfill('0') << index;
      auto const overhead = name.str().size() + 2;
      auto const value_size = index == 63 ? remaining - overhead : ava::process::kMaxEnvironmentValueBytesV1;
      auto value = std::string(value_size, 'v');
      sandbox.set(name.str(), value);
      remaining -= overhead + value_size;
      last_name = name.str();
      last_value = std::move(value);
    }
    auto accepted = EnvironmentTestAccess::capture_host();
    last_value.push_back('x');
    sandbox.set(last_name, last_value);
    auto rejected = EnvironmentTestAccess::capture_host();
    expect(accepted && EnvironmentTestAccess::host_encoded_size(*accepted) == ava::process::kMaxEnvironmentEncodedBytesV1 && !rejected,
           "host projection accepts exactly one MiB of encoded entries and rejects the next byte");
  }
}

void test_immutable_capture_and_scope_sharing()
{
  EnvironmentSandbox sandbox;
  sandbox.set("PATH", "/captured/bin:/usr/bin");
  sandbox.set("HOME", "/captured/home");
  sandbox.set("BROWSER", "/captured/browser");
  auto captured = EnvironmentTestAccess::capture_host();
  sandbox.set("HOME", "/mutated/home");
  sandbox.set("BROWSER", "/mutated/browser");
  auto lsp = captured ? ava::process::make_lsp_environment_v1(*captured, "/logical") : ava::core::Result<ExactEnvironmentV1>(std::unexpected(captured.error()));

  auto supervisor = std::make_shared<ava::process::Supervisor>();
  auto application = ava::process::ProcessScopeV1::application(supervisor);
  sandbox.set("HOME", "/mutated/again");
  auto session = application ? application->session() : ava::core::Result<ava::process::ProcessScopeV1>(std::unexpected(application.error()));
  auto operation = session ? session->operation() : ava::core::Result<ava::process::ProcessScopeV1>(std::unexpected(session.error()));
  auto snapshot = supervisor->snapshot();

  expect(captured && lsp && has_assignment(*lsp, "HOME", "/captured/home") &&
             captured->selector(ava::process::HostEnvironmentSelectorV1::Browser) == "/captured/browser",
         "a captured host projection remains immutable after deterministic parent environment mutation");
  expect(application && session && operation && EnvironmentTestAccess::shares_capture(application->host_environment(), session->host_environment()) &&
             EnvironmentTestAccess::shares_capture(application->host_environment(), operation->host_environment()) && !snapshot.monitor_started &&
             snapshot.live_records == 0 && snapshot.records.empty(),
         "application creation captures once and all derived scopes share it without starting a monitor or creating a process record");
}

void test_mcp_path_and_reserved_input_validation()
{
  auto default_path = ava::process::make_mcp_environment_v1("/logical", {{"SECRET", "intentional"}});
  auto explicit_path = ava::process::make_mcp_environment_v1("/logical", {{"PATH", "/one:/two"}, {"SECRET", "intentional"}});
  auto pwd = ava::process::make_mcp_environment_v1("/logical", {{"PWD", "/caller"}});
  auto two_paths = ava::process::make_mcp_environment_v1("/logical", {{"PATH", "/one"}, {"PATH", "/two"}});
  bool bad_paths_rejected = true;
  for (auto const path : std::array<std::string_view, 6>{"", "relative", "/one:relative", "/one::/two", ":/two", "/one:"})
  {
    std::vector<EnvironmentVariableV1> variables{{"PATH", std::string(path)}};
    auto result = ava::process::make_mcp_environment_v1("/logical", variables);
    bad_paths_rejected = bad_paths_rejected && !result;
  }

  expect(default_path &&
             assignments(*default_path) ==
                 std::vector<std::string>{"PWD=/logical", assignment("PATH", ava::process::kTrustedEnvironmentPathV1), "SECRET=intentional"} &&
             explicit_path && assignments(*explicit_path) == std::vector<std::string>{"PWD=/logical", "PATH=/one:/two", "SECRET=intentional"},
         "MCP computes PWD, supplies trusted PATH when absent, and canonicalizes one validated explicit PATH before other explicit entries");
  expect(!pwd && !two_paths && bad_paths_rejected, "MCP rejects reserved PWD, duplicate PATH, and every empty, relative, or empty-component PATH shape");
}

void test_desktop_path_and_external_draft_validation()
{
  EnvironmentSandbox sandbox;
  sandbox.set("PATH", "relative:/usr/bin");
  auto bad_host = EnvironmentTestAccess::capture_host();
  auto bad_browser =
      bad_host ? ava::process::make_browser_desktop_environment_v1(*bad_host) : ava::core::Result<ExactEnvironmentV1>(std::unexpected(bad_host.error()));
  auto bad_editor = bad_host ? ava::process::make_external_editor_environment_v1(*bad_host, "/draft")
                             : ava::core::Result<ExactEnvironmentV1>(std::unexpected(bad_host.error()));
  sandbox.set("PATH", "/desktop/bin:/usr/bin");
  auto host = EnvironmentTestAccess::capture_host();
  auto relative_draft =
      host ? ava::process::make_external_editor_environment_v1(*host, "relative-draft") : ava::core::Result<ExactEnvironmentV1>(std::unexpected(host.error()));
  auto nul_draft = host ? ava::process::make_external_editor_environment_v1(*host, std::string("/draft\0tail", 11))
                        : ava::core::Result<ExactEnvironmentV1>(std::unexpected(host.error()));
  auto long_draft = host ? ava::process::make_external_editor_environment_v1(*host, "/" + std::string(ava::process::kMaxEnvironmentValueBytesV1, 'd'))
                         : ava::core::Result<ExactEnvironmentV1>(std::unexpected(host.error()));
  expect(!bad_browser && !bad_editor, "browser and external editor reject inherited PATH with a relative component");
  expect(!relative_draft && !nul_draft && !long_draft, "external editor rejects relative, NUL-bearing, and first-oversize draft paths");
}

void test_bash_validation_matrix()
{
  auto valid = bash_variables();
  std::ranges::reverse(valid);
  auto canonical = ava::process::validate_bash_environment_v1(ava::process::kBashEnvironmentProfileIdV1, "/logical/workspace", valid);
  auto wrong_profile = ava::process::validate_bash_environment_v1("profile-canary", "/logical/workspace", bash_variables());

  auto duplicate_values = bash_variables();
  duplicate_values.push_back({"PWD", "/logical/workspace"});
  auto duplicate = ava::process::validate_bash_environment_v1(ava::process::kBashEnvironmentProfileIdV1, "/logical/workspace", duplicate_values);
  auto missing_values = bash_variables();
  missing_values.pop_back();
  auto missing = ava::process::validate_bash_environment_v1(ava::process::kBashEnvironmentProfileIdV1, "/logical/workspace", missing_values);
  auto extra_values = bash_variables();
  extra_values.push_back({"EXTRA_CANARY", "extra-value-canary"});
  auto extra = ava::process::validate_bash_environment_v1(ava::process::kBashEnvironmentProfileIdV1, "/logical/workspace", extra_values);

  bool fixed_rejected = true;
  for (std::size_t index : {0U, 1U, 2U, 3U})
  {
    auto values = bash_variables();
    values[index].value = "wrong-fixed-canary";
    fixed_rejected = fixed_rejected && !ava::process::validate_bash_environment_v1(ava::process::kBashEnvironmentProfileIdV1, "/logical/workspace", values);
  }
  auto wrong_pwd_values = bash_variables();
  wrong_pwd_values[6].value = "/different";
  auto wrong_pwd = ava::process::validate_bash_environment_v1(ava::process::kBashEnvironmentProfileIdV1, "/logical/workspace", wrong_pwd_values);

  bool private_paths_rejected = true;
  for (std::size_t index = 8; index < bash_variables().size(); ++index)
  {
    auto values = bash_variables();
    values[index].value = "relative-private-canary";
    private_paths_rejected =
        private_paths_rejected && !ava::process::validate_bash_environment_v1(ava::process::kBashEnvironmentProfileIdV1, "/logical/workspace", values);
  }
  bool paths_rejected = true;
  for (auto const path : std::array<std::string_view, 5>{"relative", "/one:relative", "/one::/two", ":/two", "/one:"})
  {
    auto values = bash_variables();
    values[7].value = path;
    paths_rejected = paths_rejected && !ava::process::validate_bash_environment_v1(ava::process::kBashEnvironmentProfileIdV1, "/logical/workspace", values);
  }
  auto empty_user_values = bash_variables();
  empty_user_values[4].value.clear();
  auto empty_user = ava::process::validate_bash_environment_v1(ava::process::kBashEnvironmentProfileIdV1, "/logical/workspace", empty_user_values);
  auto empty_logname_values = bash_variables();
  empty_logname_values[5].value.clear();
  auto empty_logname = ava::process::validate_bash_environment_v1(ava::process::kBashEnvironmentProfileIdV1, "/logical/workspace", empty_logname_values);

  expect(canonical && assignments(*canonical).front() == "LANG=C.UTF-8" && assignments(*canonical).back() == "TMPDIR=/private/tmp",
         "bash accepts reordered imported input but emits exactly one canonical instance of every synthetic name");
  expect(!wrong_profile && !duplicate && !missing && !extra && fixed_rejected && !wrong_pwd && private_paths_rejected && paths_rejected && !empty_user &&
             !empty_logname,
         "bash rejects wrong profile, duplicate/missing/extra names, wrong fixed/PWD values, nonabsolute private paths, malformed PATH, and empty identities");
  std::string const combined_errors = !wrong_profile && !extra ? wrong_profile.error().format() + extra.error().format() : std::string("unexpected-success");
  expect(combined_errors.find("profile-canary") == std::string::npos && combined_errors.find("EXTRA_CANARY") == std::string::npos &&
             combined_errors.find("extra-value-canary") == std::string::npos,
         "bash policy errors do not echo profile, name, or value canaries");
}

ava::process::OwnerPathV1 operation_owner(ava::process::OwnerPathV1 const& application)
{
  auto owner = application.operation();
  if (!owner)
    throw std::runtime_error(owner.error().format());
  return std::move(*owner);
}

void test_common_launch_rejects_mismatched_logical_cwd()
{
  EnvironmentSandbox sandbox;
  sandbox.set("PATH", "/usr/bin:/bin");
  auto host = EnvironmentTestAccess::capture_host();
  auto application = ava::process::OwnerPathV1::application();
  expect(host && application, "common cwd-binding fixture creates its host projection and application owner");
  if (!host || !application)
    return;

  auto const root = create_empty_root("process-environment-common-cwd-binding");
  auto const environment_cwd = root / "ENVIRONMENT_CWD_CANARY_42d7";
  auto const spawn_cwd = root / "SPAWN_CWD_CANARY_85ac";
  std::filesystem::create_directories(environment_cwd);
  std::filesystem::create_directories(spawn_cwd);

  ava::process::Supervisor supervisor;
  std::vector<std::string> errors;
  auto reject = [&](ava::process::ProcessRoleV1 role, ava::core::Result<ExactEnvironmentV1> environment) {
    if (!environment)
      return false;
    auto reservation = supervisor.reserve(operation_owner(*application), role);
    if (!reservation)
      return false;
    auto result = supervisor.spawn(std::move(*reservation),
                                   {.executable = "/bin/true", .argv = {"/bin/true"}, .environment = std::move(*environment), .cwd = spawn_cwd.string()});
    if (result)
    {
      static_cast<void>(supervisor.wait(result->handle, ava::tests::now_plus_seconds(2)));
      return false;
    }
    errors.push_back(result.error().format());
    return true;
  };

  bool const rejected = reject(ava::process::ProcessRoleV1::Curl, ava::process::make_curl_environment_v1(*host)) &&
                        reject(ava::process::ProcessRoleV1::Plugin, ava::process::make_plugin_environment_v1(environment_cwd)) &&
                        reject(ava::process::ProcessRoleV1::Mcp, ava::process::make_mcp_environment_v1(environment_cwd, {})) &&
                        reject(ava::process::ProcessRoleV1::Lsp, ava::process::make_lsp_environment_v1(*host, environment_cwd));
  auto snapshot = supervisor.snapshot();
  bool settled_once = snapshot.records.size() == 4;
  for (auto const& record : snapshot.records)
  {
    settled_once = settled_once && record.state == ava::process::ProcessStateV1::Finished && record.reason == ava::process::TerminationReasonV1::LaunchFailed &&
                   record.exit_kind == ava::process::ExitKindV1::LaunchError && record.settlement_count == 1;
  }
  bool content_free = errors.size() == 4;
  for (auto const& error : errors)
  {
    content_free = content_free && error.find(environment_cwd.string()) == std::string::npos && error.find(spawn_cwd.string()) == std::string::npos &&
                   error.find("PWD=/") == std::string::npos;
  }
  expect(rejected && settled_once && snapshot.live_records == 0 && !snapshot.monitor_started,
         "Curl, Plugin, MCP, and LSP cwd mismatches fail before fork or monitor with one launch-error settlement");
  expect(content_free, "common cwd-binding failures do not echo either logical path");
}

void test_secure_adoption_rejects_mismatched_logical_cwd()
{
  auto application = ava::process::OwnerPathV1::application();
  expect(application.has_value(), "secure-adoption cwd fixture creates its application owner");
  if (!application)
    return;

  constexpr std::string_view environment_cwd = "/ENVIRONMENT_CWD_CANARY_92ac";
  constexpr std::string_view adoption_cwd = "/ADOPTION_CWD_CANARY_71be";
  auto bash_values = bash_variables();
  for (auto& variable : bash_values)
  {
    if (variable.name == "PWD")
      variable.value = environment_cwd;
  }
  auto bash_environment = ava::process::validate_bash_environment_v1(ava::process::kBashEnvironmentProfileIdV1, environment_cwd, bash_values);
  auto mermaid_environment = ava::process::make_mermaid_environment_v1();
  ava::process::Supervisor supervisor;
  std::vector<std::string> errors;

  auto reject = [&](ava::process::ProcessRoleV1 role, ExactEnvironmentV1 environment) {
    auto reservation = supervisor.reserve(operation_owner(*application), role);
    if (!reservation)
      return false;
    auto result = supervisor.begin_secure_adoption(std::move(*reservation), {.environment = std::move(environment),
                                                                             .argv = {"adopted-process"},
                                                                             .cwd = std::string(adoption_cwd),
                                                                             .anchored_cwd = anchored_root_cwd(),
                                                                             .bash_containment = ava::process::BashContainmentHandshakeV1::None});
    if (!result)
      errors.push_back(result.error().format());
    return !result;
  };
  bool const rejected = bash_environment && reject(ava::process::ProcessRoleV1::Bash, std::move(*bash_environment)) && mermaid_environment &&
                        reject(ava::process::ProcessRoleV1::Mermaid, std::move(*mermaid_environment));
  auto snapshot = supervisor.snapshot();
  bool exact = snapshot.records.size() == 2 && !snapshot.monitor_started && snapshot.live_records == 0;
  for (auto const& record : snapshot.records)
  {
    exact = exact && record.reason == ava::process::TerminationReasonV1::LaunchFailed && record.exit_kind == ava::process::ExitKindV1::LaunchError &&
            record.settlement_count == 1;
  }
  bool content_free = errors.size() == 2;
  for (auto const& error : errors)
  {
    content_free = content_free && error.find(environment_cwd) == std::string::npos && error.find(adoption_cwd) == std::string::npos;
  }
  expect(rejected && exact, "Bash and Mermaid secure-adoption cwd mismatches fail before fork or monitor with one launch-error settlement");
  expect(content_free, "secure-adoption cwd mismatch errors omit both retained and requested logical paths");
}

void test_closed_launch_surface_and_content_free_failures()
{
  EnvironmentSandbox sandbox;
  sandbox.set("PATH", "/usr/bin:/bin");
  auto application = ava::process::OwnerPathV1::application();
  expect(application.has_value(), "launch-surface fixture creates a generated application owner");
  if (!application)
    return;

  ava::process::Supervisor supervisor;
  std::vector<std::string> errors;
  auto common_failure = [&](ava::process::ProcessRoleV1 role, ExactEnvironmentV1 environment) {
    auto reservation = supervisor.reserve(operation_owner(*application), role);
    if (!reservation)
      return false;
    auto result =
        supervisor.spawn(std::move(*reservation),
                         {.executable = "/CANARY_EXECUTABLE", .argv = {"CANARY_ARG_VALUE"}, .environment = std::move(environment), .cwd = "/CANARY_CWD"});
    if (!result)
      errors.push_back(result.error().format());
    return !result;
  };
  auto adoption_failure = [&](ava::process::ProcessRoleV1 role, ExactEnvironmentV1 environment) {
    auto reservation = supervisor.reserve(operation_owner(*application), role);
    if (!reservation)
      return false;
    auto result = supervisor.begin_secure_adoption(std::move(*reservation), {.environment = std::move(environment),
                                                                             .argv = {"adopted-process"},
                                                                             .cwd = "/",
                                                                             .anchored_cwd = anchored_root_cwd(),
                                                                             .bash_containment = ava::process::BashContainmentHandshakeV1::None});
    if (!result)
      errors.push_back(result.error().format());
    return !result;
  };

  auto mismatch = ava::process::make_mcp_environment_v1("/", {});
  auto bash_common = ava::process::validate_bash_environment_v1(ava::process::kBashEnvironmentProfileIdV1, "/logical/workspace", bash_variables());
  auto mermaid_common = ava::process::make_mermaid_environment_v1();
  auto plugin_adoption = ava::process::make_plugin_environment_v1("/");
  auto mermaid_for_bash = ava::process::make_mermaid_environment_v1();
  bool const rejected = mismatch && common_failure(ava::process::ProcessRoleV1::Plugin, std::move(*mismatch)) && bash_common &&
                        common_failure(ava::process::ProcessRoleV1::Bash, std::move(*bash_common)) && mermaid_common &&
                        common_failure(ava::process::ProcessRoleV1::Mermaid, std::move(*mermaid_common)) && plugin_adoption &&
                        adoption_failure(ava::process::ProcessRoleV1::Plugin, std::move(*plugin_adoption)) && mermaid_for_bash &&
                        adoption_failure(ava::process::ProcessRoleV1::Bash, std::move(*mermaid_for_bash));

  auto mermaid_reservation = supervisor.reserve(operation_owner(*application), ava::process::ProcessRoleV1::Mermaid);
  auto mermaid_environment = ava::process::make_mermaid_environment_v1();
  auto mermaid_gate =
      mermaid_reservation && mermaid_environment
          ? supervisor.begin_secure_adoption(std::move(*mermaid_reservation), {.environment = std::move(*mermaid_environment),
                                                                               .argv = {"adopted-process"},
                                                                               .cwd = "/",
                                                                               .anchored_cwd = anchored_root_cwd(),
                                                                               .bash_containment = ava::process::BashContainmentHandshakeV1::None})
          : ava::core::Result<ava::process::AdoptionGate>(
                std::unexpected(ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "failed to prepare sentinel fixture")));
  auto sentinel =
      mermaid_gate ? mermaid_gate->fork_sentinel()
                   : ava::core::VoidResult(std::unexpected(ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "failed to prepare sentinel fixture")));
  if (!sentinel)
    errors.push_back(sentinel.error().format());

  auto snapshot = supervisor.snapshot();
  bool settled_once = snapshot.records.size() == 6;
  for (auto const& record : snapshot.records)
  {
    settled_once = settled_once && record.state == ava::process::ProcessStateV1::Finished && record.reason == ava::process::TerminationReasonV1::LaunchFailed &&
                   record.exit_kind == ava::process::ExitKindV1::LaunchError && record.settlement_count == 1;
  }
  std::string error_text;
  for (auto const& error : errors)
    error_text += error;
  expect(rejected && !sentinel && settled_once && snapshot.live_records == 0 && !snapshot.monitor_started,
         "common/adoption role, profile, and surface mismatches plus non-bash sentinel request fail before fork with one launch-error settlement");
  expect(error_text.find("CANARY_EXECUTABLE") == std::string::npos && error_text.find("CANARY_ARG_VALUE") == std::string::npos &&
             error_text.find("CANARY_CWD") == std::string::npos && error_text.find("ambient") == std::string::npos,
         "launch mismatch diagnostics remain content-free");
}

}  // namespace

void run_process_environment_tests()
{
  static_assert(!std::is_copy_constructible_v<ExactEnvironmentV1>);
  static_assert(!std::is_copy_assignable_v<ExactEnvironmentV1>);

  test_all_profile_entries_order_and_allowlists();
  test_input_and_aggregate_bounds();
  test_host_name_value_and_aggregate_bounds();
  test_immutable_capture_and_scope_sharing();
  test_mcp_path_and_reserved_input_validation();
  test_desktop_path_and_external_draft_validation();
  test_bash_validation_matrix();
  test_common_launch_rejects_mismatched_logical_cwd();
  test_secure_adoption_rejects_mismatched_logical_cwd();
  test_closed_launch_surface_and_content_free_failures();
}

#include "ava/core/json.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>

namespace {

enum class Mode
{
  Normal,
  EchoUriDiagnostics,
  SleepInitialize,
  SleepInitializeMarker,
  DelayedInitialize,
  MalformedDiagnostics,
  PublishDiagnostics,
  PublishDuringRequest,
  PublishUnrelatedFirst,
  PublishTwoDocuments,
  PublishDidChange,
  PublishUnversionedDidChange,
  PublishEmptyClear,
  PublishDocumentOverflow,
  PublishCacheOverflow,
  PublishOutside,
  MalformedPublishDiagnostics,
  MalformedCapabilities,
  ServerConfigurationRequest,
  CrashDiagnostics,
  SleepDiagnostics,
  SleepDiagnosticsMarker,
  HugeContentLength,
  HugeHeader,
  MalformedSymbols,
  CwdMarker,
  TermIgnoringDescendantDiagnostics,
  LeaderExitsFirstDiagnostics,
  GatedExitDiagnostics,
  SlowDidOpenDefinition,
};

struct Options
{
  Mode mode = Mode::Normal;
  std::string marker_path;
  std::string descendant_marker_path;
  std::string launch_marker_path;
  std::string environment_marker_path;
  std::string exit_gate_path;
  bool builtin_clangd = false;
};

std::string current_logical_directory()
{
  if (auto const* pwd = std::getenv("PWD"); pwd != nullptr && *pwd != '\0' && *pwd == '/')
    return pwd;
  char buffer[4096]{};
  if (getcwd(buffer, sizeof(buffer)) != nullptr)
    return buffer;
  return {};
}

ssize_t read_retry(char* data, std::size_t size)
{
  while (true)
  {
    auto const bytes = read(STDIN_FILENO, data, size);
    if (bytes < 0 && errno == EINTR)
      continue;
    return bytes;
  }
}

std::optional<std::string> read_exact(std::size_t size, bool slow)
{
  std::string result(size, '\0');
  std::size_t offset = 0;
  while (offset < size)
  {
    auto const requested = slow ? std::min<std::size_t>(4096, size - offset) : size - offset;
    auto const bytes = read_retry(result.data() + offset, requested);
    if (bytes <= 0)
      return std::nullopt;
    offset += static_cast<std::size_t>(bytes);
    if (slow)
      usleep(1000);
  }
  return result;
}

std::optional<std::size_t> content_length(std::string_view header)
{
  constexpr std::string_view key = "Content-Length:";
  auto const position = header.find(key);
  if (position == std::string_view::npos)
    return std::nullopt;
  std::size_t index = position + key.size();
  while (index < header.size() && (header[index] == ' ' || header[index] == '\t')) ++index;
  std::size_t end = index;
  while (end < header.size() && std::isdigit(static_cast<unsigned char>(header[end])) != 0) ++end;
  if (end == index)
    return std::nullopt;
  return static_cast<std::size_t>(std::stoull(std::string(header.substr(index, end - index))));
}

std::optional<std::string> read_message(Options const& options)
{
  std::string header;
  char ch = '\0';
  while (header.find("\r\n\r\n") == std::string::npos)
  {
    auto const bytes = read_retry(&ch, 1);
    if (bytes <= 0)
      return std::nullopt;
    header.push_back(ch);
  }
  auto const length = content_length(header);
  if (!length)
    return std::nullopt;
  return read_exact(*length, options.mode == Mode::SlowDidOpenDefinition);
}

void write_message(std::string_view body)
{
  std::cout << "Content-Length: " << body.size() << "\r\n\r\n" << body;
  std::cout.flush();
}

Options parse_options(int argc, char** argv)
{
  Options options;
  for (int index = 1; index < argc; ++index)
  {
    if (std::strcmp(argv[index], "--background-index") == 0)
      options.builtin_clangd = true;
    if (std::strcmp(argv[index], "--malformed-diagnostics") == 0)
      options.mode = Mode::MalformedDiagnostics;
    if (std::strcmp(argv[index], "--crash-diagnostics") == 0)
      options.mode = Mode::CrashDiagnostics;
    if (std::strcmp(argv[index], "--publish-diagnostics") == 0)
      options.mode = Mode::PublishDiagnostics;
    if (std::strcmp(argv[index], "--publish-during-request") == 0)
      options.mode = Mode::PublishDuringRequest;
    if (std::strcmp(argv[index], "--publish-unrelated-first") == 0)
      options.mode = Mode::PublishUnrelatedFirst;
    if (std::strcmp(argv[index], "--publish-two-documents") == 0)
      options.mode = Mode::PublishTwoDocuments;
    if (std::strcmp(argv[index], "--publish-did-change") == 0)
      options.mode = Mode::PublishDidChange;
    if (std::strcmp(argv[index], "--publish-unversioned-did-change") == 0)
      options.mode = Mode::PublishUnversionedDidChange;
    if (std::strcmp(argv[index], "--publish-empty-clear") == 0)
      options.mode = Mode::PublishEmptyClear;
    if (std::strcmp(argv[index], "--publish-document-overflow") == 0)
      options.mode = Mode::PublishDocumentOverflow;
    if (std::strcmp(argv[index], "--publish-cache-overflow") == 0)
      options.mode = Mode::PublishCacheOverflow;
    if (std::strcmp(argv[index], "--publish-outside") == 0)
      options.mode = Mode::PublishOutside;
    if (std::strcmp(argv[index], "--malformed-publish-diagnostics") == 0)
      options.mode = Mode::MalformedPublishDiagnostics;
    if (std::strcmp(argv[index], "--malformed-capabilities") == 0)
      options.mode = Mode::MalformedCapabilities;
    if (std::strcmp(argv[index], "--server-configuration-request") == 0)
      options.mode = Mode::ServerConfigurationRequest;
    if (std::strcmp(argv[index], "--echo-uri-diagnostics") == 0)
      options.mode = Mode::EchoUriDiagnostics;
    if (std::strcmp(argv[index], "--sleep-initialize") == 0)
      options.mode = Mode::SleepInitialize;
    if (std::strcmp(argv[index], "--sleep-initialize-marker") == 0 && index + 1 < argc)
    {
      options.mode = Mode::SleepInitializeMarker;
      options.marker_path = argv[++index];
    }
    if (std::strcmp(argv[index], "--delayed-initialize") == 0)
      options.mode = Mode::DelayedInitialize;
    if (std::strcmp(argv[index], "--launch-marker") == 0 && index + 1 < argc)
      options.launch_marker_path = argv[++index];
    if (std::strcmp(argv[index], "--sleep-diagnostics") == 0)
      options.mode = Mode::SleepDiagnostics;
    if (std::strcmp(argv[index], "--sleep-diagnostics-marker") == 0 && index + 1 < argc)
    {
      options.mode = Mode::SleepDiagnosticsMarker;
      options.marker_path = argv[++index];
    }
    if (std::strcmp(argv[index], "--huge-content-length") == 0)
      options.mode = Mode::HugeContentLength;
    if (std::strcmp(argv[index], "--huge-header") == 0)
      options.mode = Mode::HugeHeader;
    if (std::strcmp(argv[index], "--malformed-symbols") == 0)
      options.mode = Mode::MalformedSymbols;
    if (std::strcmp(argv[index], "--cwd-marker") == 0 && index + 1 < argc)
    {
      options.mode = Mode::CwdMarker;
      options.marker_path = argv[++index];
    }
    if (std::strcmp(argv[index], "--term-ignoring-descendant-diagnostics-markers") == 0 && index + 2 < argc)
    {
      options.mode = Mode::TermIgnoringDescendantDiagnostics;
      options.marker_path = argv[++index];
      options.descendant_marker_path = argv[++index];
    }
    if (std::strcmp(argv[index], "--leader-exits-first-diagnostics-markers") == 0 && index + 2 < argc)
    {
      options.mode = Mode::LeaderExitsFirstDiagnostics;
      options.marker_path = argv[++index];
      options.descendant_marker_path = argv[++index];
    }
    if (std::strcmp(argv[index], "--gated-exit-diagnostics-marker") == 0 && index + 2 < argc)
    {
      options.mode = Mode::GatedExitDiagnostics;
      options.marker_path = argv[++index];
      options.exit_gate_path = argv[++index];
    }
    if (std::strcmp(argv[index], "--slow-did-open-definition") == 0)
      options.mode = Mode::SlowDidOpenDefinition;
    if (std::strcmp(argv[index], "--environment-marker") == 0 && index + 1 < argc)
      options.environment_marker_path = argv[++index];
  }
  return options;
}

void write_process_group_marker(std::string const& path)
{
  if (path.empty())
    return;
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  file << static_cast<long long>(getpid()) << ' ' << static_cast<long long>(getpgrp()) << '\n';
}

void spawn_term_ignoring_descendant(Options const& options)
{
  write_process_group_marker(options.marker_path);
  int readiness_pipe[2];
  if (pipe(readiness_pipe) != 0)
    return;

  pid_t const descendant = fork();
  if (descendant < 0)
  {
    close(readiness_pipe[0]);
    close(readiness_pipe[1]);
    return;
  }
  if (descendant == 0)
  {
    close(readiness_pipe[0]);
    struct sigaction ignored{};
    ignored.sa_handler = SIG_IGN;
    sigemptyset(&ignored.sa_mask);
    sigaction(SIGTERM, &ignored, nullptr);
    write_process_group_marker(options.descendant_marker_path);
    char const ready = 1;
    while (write(readiness_pipe[1], &ready, sizeof(ready)) < 0 && errno == EINTR)
    {
    }
    close(readiness_pipe[1]);
    for (int attempt = 0; attempt < 50; ++attempt) usleep(100000);
    _exit(0);
  }

  close(readiness_pipe[1]);
  char ready{};
  while (read(readiness_pipe[0], &ready, sizeof(ready)) < 0 && errno == EINTR)
  {
  }
  close(readiness_pipe[0]);
}

void write_launch_marker(std::string const& path)
{
  if (path.empty())
    return;
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  file << "started\n";
}

void write_builtin_clangd_marker(Options const& options)
{
  if (!options.builtin_clangd)
    return;
  auto const* state_home = std::getenv("XDG_STATE_HOME");
  if (state_home == nullptr || *state_home == '\0')
    return;
  auto const cwd = current_logical_directory();
  if (cwd.empty())
    return;
  std::ofstream marker(std::string(state_home) + "/fake-clangd-launches.txt", std::ios::binary | std::ios::app);
  marker << cwd << '\n';
}

void write_environment_marker(std::string const& path)
{
  if (path.empty())
    return;
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  for (auto const* name : {"HOME", "LANG", "LC_ALL", "OPENAI_API_KEY", "AVA_LSP_TEST_SECRET", "AVA_UNRELATED"})
  {
    auto const* value = std::getenv(name);
    file << name << '=' << (value == nullptr ? "<unset>" : value) << '\n';
  }
}

void write_cwd_marker(std::string const& path)
{
  if (path.empty())
    return;
  auto const cwd = current_logical_directory();
  if (cwd.empty())
    return;
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  file << cwd << '\n';
}

bool uses_push_diagnostics(Mode mode)
{
  switch (mode)
  {
    case Mode::PublishDiagnostics:
    case Mode::PublishDuringRequest:
    case Mode::PublishUnrelatedFirst:
    case Mode::PublishTwoDocuments:
    case Mode::PublishDidChange:
    case Mode::PublishUnversionedDidChange:
    case Mode::PublishEmptyClear:
    case Mode::PublishDocumentOverflow:
    case Mode::PublishCacheOverflow:
    case Mode::PublishOutside:
    case Mode::MalformedPublishDiagnostics:
      return true;
    default:
      return false;
  }
}

void respond_initialize(long long id, Options const& options)
{
  if (options.mode == Mode::MalformedCapabilities)
  {
    write_message("{\"jsonrpc\":\"2.0\",\"id\":" + std::to_string(id) + ",\"result\":{\"capabilities\":{\"diagnosticProvider\":\"invalid\"}}}");
    return;
  }
  auto const diagnostic_capability = uses_push_diagnostics(options.mode)
                                         ? std::string{}
                                         : std::string("\"diagnosticProvider\":{\"interFileDependencies\":false,\"workspaceDiagnostics\":false},");
  std::string const body = "{\"jsonrpc\":\"2.0\",\"id\":" + std::to_string(id) + ",\"result\":{\"capabilities\":{" + diagnostic_capability +
                           "\"documentSymbolProvider\":true,"
                           "\"workspaceSymbolProvider\":true,\"definitionProvider\":true,\"referencesProvider\":true}}}";
  write_message(body);
}

std::string workspace_uri(std::string_view filename)
{
  auto const cwd = current_logical_directory();
  if (cwd.empty())
    return "file:///workspace/" + std::string(filename);
  return "file://" + cwd + "/" + std::string(filename);
}

std::string workspace_main_uri()
{
  return workspace_uri("main.cpp");
}

std::string request_uri(std::string_view message)
{
  auto const params = ava::core::json::object_field(message, "params");
  auto const document = params ? ava::core::json::object_field(*params, "textDocument") : std::nullopt;
  auto const uri = document ? ava::core::json::string_field(*document, "uri") : std::nullopt;
  return uri.value_or(std::string{});
}

std::optional<int> request_document_version(std::string_view message)
{
  auto const params = ava::core::json::object_field(message, "params");
  auto const document = params ? ava::core::json::object_field(*params, "textDocument") : std::nullopt;
  auto const version = document ? ava::core::json::integer_field(*document, "version") : std::nullopt;
  if (!version)
    return std::nullopt;
  return static_cast<int>(*version);
}

void respond_diagnostics(long long id, Options const& options, std::string_view uri, bool initialized)
{
  auto const mode = options.mode;
  if (mode == Mode::DelayedInitialize && !initialized)
  {
    write_message("{\"jsonrpc\":\"2.0\",\"id\":" + std::to_string(id) + ",\"error\":{\"code\":-32002,\"message\":\"initialized notification required\"}}");
    return;
  }
  if (mode == Mode::CrashDiagnostics)
    std::exit(23);
  if (mode == Mode::TermIgnoringDescendantDiagnostics)
  {
    spawn_term_ignoring_descendant(options);
    return;
  }
  if (mode == Mode::LeaderExitsFirstDiagnostics)
  {
    spawn_term_ignoring_descendant(options);
    _exit(23);
  }
  if (mode == Mode::GatedExitDiagnostics)
  {
    int const gate = open(options.exit_gate_path.c_str(), O_RDONLY | O_CLOEXEC);
    if (gate < 0)
      _exit(2);
    write_process_group_marker(options.marker_path);
    char release{};
    ssize_t received;
    do
    {
      received = read(gate, &release, sizeof(release));
    }
    while (received < 0 && errno == EINTR);
    close(gate);
    _exit(received == 1 && release == 'X' ? 23 : 2);
  }
  if (mode == Mode::SleepDiagnostics || mode == Mode::SleepDiagnosticsMarker)
  {
    write_process_group_marker(options.marker_path);
    usleep(1000000);
    return;
  }
  if (mode == Mode::MalformedDiagnostics)
  {
    write_message("not-json");
    return;
  }
  if (mode == Mode::HugeContentLength)
  {
    std::cout << "Content-Length: 4194305\r\n\r\n";
    std::cout.flush();
    return;
  }
  if (mode == Mode::HugeHeader)
  {
    std::cout << std::string(70 * 1024, 'X');
    std::cout.flush();
    return;
  }
  auto const message = mode == Mode::EchoUriDiagnostics ? std::string(uri) : std::string("fake diagnostic from LSP");
  std::string const body = "{\"jsonrpc\":\"2.0\",\"id\":" + std::to_string(id) +
                           ",\"result\":{\"kind\":\"full\",\"items\":[{\"range\":{\"start\":{\"line\":2,"
                           "\"character\":4},\"end\":{\"line\":2,\"character\":9}},\"severity\":1,"
                           "\"code\":\"AVA_FAKE\",\"source\":\"ava_fake_lsp\","
                           "\"message\":\"" +
                           ava::core::json::escape(message) + "\"}]}}";
  write_message(body);
}

void publish_diagnostics_payload(std::string_view uri, std::string_view message, std::optional<int> version = std::nullopt, bool empty = false,
                                 bool two_items = false)
{
  std::string diagnostics;
  if (empty)
  {
    diagnostics = "[]";
  }
  else
  {
    auto const item =
        "{\"range\":{\"start\":{\"line\":3,\"character\":2},\"end\":{\"line\":3,\"character\":7}},"
        "\"severity\":2,\"code\":\"AVA_PUBLISH\",\"message\":\"" +
        ava::core::json::escape(message) + "\"}";
    diagnostics = "[" + item + (two_items ? "," + item : std::string{}) + "]";
  }
  std::string const version_field = version ? ",\"version\":" + std::to_string(*version) : std::string{};
  write_message("{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/publishDiagnostics\",\"params\":{\"uri\":\"" + ava::core::json::escape(uri) + "\"" +
                version_field + ",\"diagnostics\":" + diagnostics + "}}");
}

void publish_cache_bound_notifications(Mode mode)
{
  auto const message = std::string(16 * 1024, 'x');
  for (int index = 0; index < (mode == Mode::PublishDocumentOverflow ? 65 : 64); ++index)
  {
    publish_diagnostics_payload(workspace_uri("cached-" + std::to_string(index) + ".cpp"), message, std::nullopt, false, mode == Mode::PublishCacheOverflow);
  }
}

void publish_diagnostics(std::string_view uri, Options const& options)
{
  if (options.mode == Mode::MalformedPublishDiagnostics)
  {
    write_message("{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/publishDiagnostics\",\"params\":{\"uri\":42}}");
    return;
  }
  if (options.mode == Mode::PublishOutside)
  {
    publish_diagnostics_payload("file:///outside-ava-workspace.cpp", "outside diagnostic");
    return;
  }
  publish_diagnostics_payload(uri, "fake published diagnostic");
}

void respond_document_symbols(long long id, Options const& options)
{
  if (options.mode == Mode::MalformedSymbols)
  {
    std::string const body = "{\"jsonrpc\":\"2.0\",\"id\":" + std::to_string(id) + ",\"result\":[{\"name\":\"broken\",\"kind\":12}]}";
    write_message(body);
    return;
  }
  std::string const body = "{\"jsonrpc\":\"2.0\",\"id\":" + std::to_string(id) +
                           ",\"result\":[{\"name\":\"main\",\"kind\":12,\"range\":{\"start\":{\"line\":0,"
                           "\"character\":4},\"end\":{\"line\":0,\"character\":8}},\"selectionRange\":{\"start\":{"
                           "\"line\":0,\"character\":4},\"end\":{\"line\":0,\"character\":8}},\"children\":[{"
                           "\"name\":\"return statement\",\"kind\":12,\"range\":{\"start\":{\"line\":0,"
                           "\"character\":13},\"end\":{\"line\":0,\"character\":21}},\"selectionRange\":{\"start\":{"
                           "\"line\":0,\"character\":13},\"end\":{\"line\":0,\"character\":21}}}]}]}";
  write_message(body);
}

void respond_workspace_symbols(long long id)
{
  auto const uri = workspace_main_uri();
  std::string const body = "{\"jsonrpc\":\"2.0\",\"id\":" + std::to_string(id) +
                           ",\"result\":[{\"name\":\"main\",\"kind\":12,\"containerName\":\"global\","
                           "\"location\":{\"uri\":\"" +
                           ava::core::json::escape(uri) +
                           "\",\"range\":{\"start\":{\"line\":0,\"character\":4},\"end\":{\"line\":0,"
                           "\"character\":8}}}}]}";
  write_message(body);
}

void respond_definition(long long id)
{
  auto const uri = workspace_main_uri();
  std::string const body = "{\"jsonrpc\":\"2.0\",\"id\":" + std::to_string(id) + ",\"result\":{\"uri\":\"" + ava::core::json::escape(uri) +
                           "\",\"range\":{\"start\":{\"line\":0,\"character\":4},\"end\":{\"line\":0,"
                           "\"character\":8}}}}";
  write_message(body);
}

void respond_references(long long id, bool did_open)
{
  if (!did_open)
  {
    std::string const body = "{\"jsonrpc\":\"2.0\",\"id\":" + std::to_string(id) + ",\"result\":[]}";
    write_message(body);
    return;
  }
  auto const uri = workspace_main_uri();
  std::string const body = "{\"jsonrpc\":\"2.0\",\"id\":" + std::to_string(id) + ",\"result\":[{\"uri\":\"" + ava::core::json::escape(uri) +
                           "\",\"range\":{\"start\":{\"line\":0,\"character\":4},\"end\":{\"line\":0,"
                           "\"character\":8}}},{\"uri\":\"" +
                           ava::core::json::escape(uri) +
                           "\",\"range\":{\"start\":{\"line\":0,\"character\":13},\"end\":{\"line\":0,"
                           "\"character\":17}}}]}";
  write_message(body);
}

}  // namespace

int main(int argc, char** argv)
{
  auto const options = parse_options(argc, argv);
  write_builtin_clangd_marker(options);
  write_launch_marker(options.launch_marker_path);
  write_environment_marker(options.environment_marker_path);
  bool did_open = false;
  bool initialized = false;
  while (auto message = read_message(options))
  {
    auto const method = ava::core::json::string_field(*message, "method");
    auto const id = ava::core::json::integer_field(*message, "id");
    if (!method)
      continue;
    if (*method == "initialize" && id)
    {
      if (options.mode == Mode::SleepInitialize || options.mode == Mode::SleepInitializeMarker)
      {
        write_process_group_marker(options.marker_path);
        usleep(1000000);
        continue;
      }
      if (options.mode == Mode::DelayedInitialize)
        usleep(500000);
      if (options.mode == Mode::CwdMarker)
        write_cwd_marker(options.marker_path);
      if (options.mode == Mode::ServerConfigurationRequest)
      {
        write_message(
            "{\"jsonrpc\":\"2.0\",\"id\":900,\"method\":\"workspace/configuration\",\"params\":{\"items\":[{\"section\":\"one\"},{\"section\":\"two\"}]}}");
        auto configuration = read_message(options);
        if (!configuration || configuration->find("\"id\":900") == std::string::npos || configuration->find("\"result\":[null,null]") == std::string::npos)
          return 2;
        write_message(R"({"jsonrpc":"2.0","id":"config-\"quoted\"","method":"workspace/configuration","params":{"items":[{"section":"three"}]}})");
        auto string_configuration = read_message(options);
        if (!string_configuration || ava::core::json::string_field(*string_configuration, "id") != std::optional<std::string>("config-\"quoted\"") ||
            string_configuration->find("\"result\":[null]") == std::string::npos)
          return 2;
      }
      respond_initialize(*id, options);
    }
    else if (*method == "initialized")
    {
      initialized = true;
    }
    else if (*method == "textDocument/diagnostic" && id)
    {
      respond_diagnostics(*id, options, request_uri(*message), initialized);
    }
    else if (*method == "textDocument/documentSymbol" && id)
    {
      if (options.mode == Mode::PublishDuringRequest)
        publish_diagnostics_payload(request_uri(*message), "diagnostic published during document symbols");
      if (options.mode == Mode::PublishEmptyClear)
        publish_diagnostics_payload(request_uri(*message), {}, std::nullopt, true);
      respond_document_symbols(*id, options);
    }
    else if (*method == "workspace/symbol" && id)
    {
      if (options.mode == Mode::PublishDocumentOverflow || options.mode == Mode::PublishCacheOverflow)
        publish_cache_bound_notifications(options.mode);
      respond_workspace_symbols(*id);
    }
    else if (*method == "textDocument/definition" && id)
    {
      respond_definition(*id);
    }
    else if (*method == "textDocument/didOpen")
    {
      did_open = true;
      if (options.mode == Mode::SlowDidOpenDefinition)
        usleep(250000);
      if (options.mode == Mode::PublishDiagnostics || options.mode == Mode::MalformedPublishDiagnostics || options.mode == Mode::PublishOutside)
        publish_diagnostics(request_uri(*message), options);
      if (options.mode == Mode::PublishUnrelatedFirst)
      {
        publish_diagnostics_payload(workspace_uri("other.cpp"), "unrelated diagnostic first");
        publish_diagnostics_payload(request_uri(*message), "target diagnostic second");
      }
      if (options.mode == Mode::PublishTwoDocuments)
        publish_diagnostics_payload(request_uri(*message), request_uri(*message));
      if (options.mode == Mode::PublishDidChange || options.mode == Mode::PublishUnversionedDidChange)
        publish_diagnostics_payload(request_uri(*message), "initial diagnostic", 1);
      if (options.mode == Mode::PublishEmptyClear)
        publish_diagnostics_payload(request_uri(*message), "diagnostic before clear");
    }
    else if (*method == "textDocument/didChange")
    {
      if (options.mode == Mode::PublishDidChange)
      {
        publish_diagnostics_payload(request_uri(*message), "stale diagnostic after didChange", 1);
        publish_diagnostics_payload(request_uri(*message), "fresh diagnostic after didChange", request_document_version(*message));
      }
      if (options.mode == Mode::PublishUnversionedDidChange)
        publish_diagnostics_payload(request_uri(*message), "fresh unversioned diagnostic after didChange");
    }
    else if (*method == "textDocument/references" && id)
    {
      respond_references(*id, did_open);
    }
    else if (*method == "exit")
    {
      return 0;
    }
  }
  return 0;
}

#include "sys.h"
#include "tests/backend_benchmark_cases.h"
#include "ava/http/curl_transport.h"
#include "ava/process/scope.h"
#include "ava/process/supervisor.h"
#include "ava/tools/bash_tool.h"
#include "ava/plugin/manifest.h"
#include "ava/plugin/runner.h"
#include "ava/mcp/config.h"
#include "ava/mcp/stdio_client.h"
#include "ava/lsp/lsp_client.h"
#include "ava/core/AnchorSet.h"
#include "ava/core/trusted_home.h"

#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace ava::benchmark {
namespace {

using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

constexpr std::string_view kFixtureUnavailableReason = "A required repository-owned benchmark fixture is unavailable.";
constexpr std::string_view kCallerNotMigratedReason =
    "The family authority flag requests supervision, but this benchmark driver has not been adapted to verify a finished Supervisor record.";

[[noreturn]] void fail(std::string const& message)
{
  std::cerr << message << '\n';
  std::exit(2);
}

class TemporaryDirectory final
{
 public:
  explicit TemporaryDirectory(std::string_view label)
  {
    auto const nonce = Clock::now().time_since_epoch().count();
    path_ = std::filesystem::temp_directory_path() / ("ava-process-benchmark-" + std::string(label) + "-" + std::to_string(static_cast<long long>(::getpid())) +
                                                      "-" + std::to_string(static_cast<long long>(nonce)));
    std::filesystem::create_directories(path_);
    if (::chmod(path_.c_str(), S_IRWXU) != 0)
      fail("failed to secure benchmark temporary directory");
  }

  TemporaryDirectory(TemporaryDirectory const&) = delete;
  TemporaryDirectory& operator=(TemporaryDirectory const&) = delete;

  ~TemporaryDirectory()
  {
    std::error_code error;
    std::filesystem::remove_all(path_, error);
  }

  [[nodiscard]] std::filesystem::path const& path() const noexcept { return path_; }

 private:
  std::filesystem::path path_;
};

[[nodiscard]] double elapsed_nanoseconds(Clock::time_point const started)
{
  return static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - started).count());
}

[[nodiscard]] bool executable_file(std::filesystem::path const& path)
{
  return !path.empty() && std::filesystem::is_regular_file(path) && ::access(path.c_str(), X_OK) == 0;
}

[[nodiscard]] bool no_waitable_children()
{
  errno = 0;
  int status = 0;
  auto const waited = ::waitpid(-1, &status, WNOHANG);
  return waited == -1 && errno == ECHILD;
}

[[nodiscard]] std::string_view authority_for(std::string_view benchmark_case)
{
  if (benchmark_case == "family-curl-lifecycle")
  {
#if defined(AVA_BENCHMARK_CURL_AUTHORITY_SUPERVISED)
    return "supervised";
#else
    return "legacy_local";
#endif
  }
  if (benchmark_case == "family-plugin-lifecycle")
  {
#if defined(AVA_BENCHMARK_PLUGIN_AUTHORITY_SUPERVISED)
    return "supervised";
#else
    return "legacy_local";
#endif
  }
  if (benchmark_case == "family-mcp-lifecycle")
  {
#if defined(AVA_BENCHMARK_MCP_AUTHORITY_SUPERVISED)
    return "supervised";
#else
    return "legacy_local";
#endif
  }
  if (benchmark_case == "family-lsp-lifecycle")
  {
#if defined(AVA_BENCHMARK_LSP_AUTHORITY_SUPERVISED)
    return "supervised";
#else
    return "legacy_local";
#endif
  }
#if defined(AVA_BENCHMARK_BASH_AUTHORITY_SUPERVISED)
  return "supervised";
#else
  return "legacy_local";
#endif
}

[[nodiscard]] JsonFields family_case_metrics(std::string_view authority)
{
  return {{"authority", std::string(authority)}, {"cleanup_scope", std::string("immediate_children_only")}};
}

void emit_family(std::string_view benchmark_case, double elapsed, bool protocol_compatible, bool expected_response, bool shutdown_complete,
                 bool immediate_child_guard)
{
  auto const authority = authority_for(benchmark_case);
  emit_helper_measurement(benchmark_case, "lifecycle_ns", "ns",
                          {{.ordinal = 1,
                            .value = elapsed,
                            .metrics = {},
                            .checks = {{"protocol_compatible", protocol_compatible},
                                       {"expected_response", expected_response},
                                       {"shutdown_complete", shutdown_complete},
                                       {"immediate_child_guard", immediate_child_guard}}}},
                          family_case_metrics(authority));
}

[[nodiscard]] bool refuse_false_supervised_claim(BackendBenchmarkOptions const& options)
{
  auto const authority = authority_for(options.benchmark_case);
  if (authority != "supervised")
    return false;
#if defined(AVA_BENCHMARK_CURL_AUTHORITY_SUPERVISED)
  if (options.benchmark_case == "family-curl-lifecycle")
    return false;
#endif
#if defined(AVA_BENCHMARK_PLUGIN_AUTHORITY_SUPERVISED)
  if (options.benchmark_case == "family-plugin-lifecycle")
    return false;
#endif
#if defined(AVA_BENCHMARK_MCP_AUTHORITY_SUPERVISED)
  if (options.benchmark_case == "family-mcp-lifecycle")
    return false;
#endif
  emit_helper_unsupported(options.benchmark_case, "lifecycle_ns", "ns", "caller_not_migrated", kCallerNotMigratedReason, family_case_metrics(authority));
  return true;
}

void benchmark_curl(BackendBenchmarkOptions const& options)
{
  if (options.loopback_port == 0)
  {
    emit_helper_unsupported(options.benchmark_case, "lifecycle_ns", "ns", "fixture_unavailable", kFixtureUnavailableReason,
                            family_case_metrics("legacy_local"));
    return;
  }
  bool const clean_before = no_waitable_children();
#if defined(AVA_BENCHMARK_CURL_AUTHORITY_SUPERVISED)
  auto supervisor = std::make_shared<ava::process::Supervisor>();
  auto application_scope = ava::process::ProcessScopeV1::application(supervisor);
  if (!application_scope)
    fail(application_scope.error().format());
  ava::core::Result<ava::http::HttpResponse> response = std::unexpected(ava::core::Error(ava::core::ErrorCategory::Unknown, "curl benchmark was not run"));
  auto const started = Clock::now();
  {
    ava::http::CurlCliTransport transport(*application_scope);
    response = transport.send(ava::http::HttpRequest{.method = "GET",
                                                     .url = "http://127.0.0.1:" + std::to_string(options.loopback_port) + "/benchmark",
                                                     .headers = {{"Accept", "text/plain"}},
                                                     .body = {},
                                                     .timeout_ms = 5000,
                                                     .follow_redirects = false,
                                                     .include_response_headers = false,
                                                     .resolve_hosts = {}});
  }
  auto const elapsed = elapsed_nanoseconds(started);
  supervisor->stop_accepting();
  auto const shutdown = supervisor->shutdown(Clock::now() + 2s);
  auto const snapshot = supervisor->snapshot();
  std::size_t curl_records = 0;
  bool record_finished = false;
  bool settlement_once = false;
  for (auto const& record : snapshot.records)
  {
    if (record.role != ava::process::ProcessRoleV1::Curl)
      continue;
    ++curl_records;
    record_finished = record.state == ava::process::ProcessStateV1::Finished && record.cleanup == ava::process::CleanupStateV1::Complete;
    settlement_once = record.settlement_count == 1;
  }
  bool const supervisor_verified = shutdown.complete && snapshot.live_records == 0 && curl_records == 1 && record_finished && settlement_once;
  if (!supervisor_verified)
    fail("supervised curl benchmark did not produce one completely settled managed-group record");
  bool const expected = response && response->status_code == 200 && response->body == "ava-backend-benchmark\n";
  bool const clean_after = no_waitable_children();
  emit_helper_measurement(options.benchmark_case, "lifecycle_ns", "ns",
                          {{.ordinal = 1,
                            .value = elapsed,
                            .metrics = {},
                            .checks = {{"protocol_compatible", static_cast<bool>(response)},
                                       {"expected_response", expected},
                                       {"shutdown_complete", shutdown.complete},
                                       {"immediate_child_guard", clean_before && clean_after},
                                       {"supervisor_record_finished", true},
                                       {"supervisor_settlement_once", true}}}},
                          {{"authority", std::string("supervised")},
                           {"supervisor_record_finished", true},
                           {"supervisor_settlement_once", true},
                           {"cleanup_scope", std::string("managed_group")}});
#else
  ava::http::CurlCliTransport transport;
  auto const started = Clock::now();
  auto response = transport.send(ava::http::HttpRequest{.method = "GET",
                                                        .url = "http://127.0.0.1:" + std::to_string(options.loopback_port) + "/benchmark",
                                                        .headers = {{"Accept", "text/plain"}},
                                                        .body = {},
                                                        .timeout_ms = 5000,
                                                        .follow_redirects = false,
                                                        .include_response_headers = false,
                                                        .resolve_hosts = {}});
  auto const elapsed = elapsed_nanoseconds(started);
  bool const expected = response && response->status_code == 200 && response->body == "ava-backend-benchmark\n";
  bool const clean_after = no_waitable_children();
  emit_family(options.benchmark_case, elapsed, static_cast<bool>(response), expected, true, clean_before && clean_after);
#endif
}

void benchmark_plugin(BackendBenchmarkOptions const& options)
{
#if !defined(AVA_BENCHMARK_PLUGIN_AUTHORITY_SUPERVISED)
  emit_helper_unsupported(options.benchmark_case, "lifecycle_ns", "ns", "caller_not_migrated", kCallerNotMigratedReason,
                          family_case_metrics(authority_for(options.benchmark_case)));
  return;
#else
  if (options.sample_plugin.empty() || !std::filesystem::is_regular_file(options.sample_plugin / "plugin.json") ||
      !std::filesystem::is_regular_file(options.sample_plugin / "plugin.sh"))
  {
    emit_helper_unsupported(options.benchmark_case, "lifecycle_ns", "ns", "fixture_unavailable", kFixtureUnavailableReason,
                            family_case_metrics(authority_for(options.benchmark_case)));
    return;
  }
  auto manifest = ava::plugin::load_plugin_manifest(options.sample_plugin / "plugin.json");
  if (!manifest)
    fail(manifest.error().format());
  TemporaryDirectory temporary("plugin-family");
  auto const workspace = temporary.path() / "workspace";
  std::filesystem::create_directories(workspace);
  bool const clean_before = no_waitable_children();
  auto supervisor = std::make_shared<ava::process::Supervisor>();
  auto application_scope = ava::process::ProcessScopeV1::application(supervisor);
  if (!application_scope)
    fail(application_scope.error().format());
  auto const started = Clock::now();
  auto process =
      ava::plugin::PluginProcess::start(*manifest, ava::plugin::PluginRunnerOptions{.workspace_dir = workspace, .process_scope = *application_scope});
  bool initialized = process && (*process)->initialization().api_version == "ava.plugin.v1";
  auto called = process ? (*process)->call_tool("todo_add", "{\"text\":\"benchmark\"}", "benchmark")
                        : ava::core::Result<ava::plugin::PluginToolCallResult>(std::unexpected(process.error()));
  auto plugin_shutdown = process ? (*process)->shutdown(500ms) : ava::core::VoidResult(std::unexpected(process.error()));
  if (process)
    process->reset();
  auto const elapsed = elapsed_nanoseconds(started);

  supervisor->stop_accepting();
  auto const supervisor_shutdown = supervisor->shutdown(Clock::now() + 2s);
  auto const snapshot = supervisor->snapshot();
  std::size_t plugin_records = 0;
  bool record_finished = false;
  bool settlement_once = false;
  for (auto const& record : snapshot.records)
  {
    if (record.role != ava::process::ProcessRoleV1::Plugin)
      continue;
    ++plugin_records;
    record_finished = record.state == ava::process::ProcessStateV1::Finished && record.cleanup == ava::process::CleanupStateV1::Complete;
    settlement_once = record.settlement_count == 1;
  }
  bool const supervisor_verified = supervisor_shutdown.complete && snapshot.live_records == 0 && plugin_records == 1 && record_finished && settlement_once;
  if (!supervisor_verified)
    fail("supervised plugin benchmark did not produce one completely settled managed-group record");
  bool const clean_after = no_waitable_children();
  emit_helper_measurement(options.benchmark_case, "lifecycle_ns", "ns",
                          {{.ordinal = 1,
                            .value = elapsed,
                            .metrics = {},
                            .checks = {{"protocol_compatible", initialized},
                                       {"expected_response", called && called->ok},
                                       {"shutdown_complete", static_cast<bool>(plugin_shutdown) && supervisor_shutdown.complete},
                                       {"immediate_child_guard", clean_before && clean_after},
                                       {"supervisor_record_finished", true},
                                       {"supervisor_settlement_once", true}}}},
                          {{"authority", std::string("supervised")},
                           {"supervisor_record_finished", true},
                           {"supervisor_settlement_once", true},
                           {"cleanup_scope", std::string("managed_group")}});
#endif
}

void benchmark_mcp(BackendBenchmarkOptions const& options)
{
#if !defined(AVA_BENCHMARK_MCP_AUTHORITY_SUPERVISED)
  emit_helper_unsupported(options.benchmark_case, "lifecycle_ns", "ns", "caller_not_migrated", kCallerNotMigratedReason,
                          family_case_metrics(authority_for(options.benchmark_case)));
  return;
#else
  if (!executable_file(options.fake_mcp_server))
  {
    emit_helper_unsupported(options.benchmark_case, "lifecycle_ns", "ns", "fixture_unavailable", kFixtureUnavailableReason,
                            family_case_metrics(authority_for(options.benchmark_case)));
    return;
  }
  TemporaryDirectory temporary("mcp-family");
  auto const workspace = temporary.path() / "workspace";
  std::filesystem::create_directories(workspace);
  ava::mcp::McpServerConfig server{.id = "benchmark",
                                   .name = "Benchmark MCP",
                                   .command = options.fake_mcp_server.string(),
                                   .args = {},
                                   .env = {},
                                   .enabled = true,
                                   .scope = ava::mcp::McpServerScope::Project,
                                   .source_path = temporary.path() / "mcp.json"};
  bool const clean_before = no_waitable_children();
  auto supervisor = std::make_shared<ava::process::Supervisor>();
  auto application_scope = ava::process::ProcessScopeV1::application(supervisor);
  if (!application_scope)
    fail(application_scope.error().format());
  ava::mcp::McpStdioClientOptions client_options;
  client_options.workspace_dir = workspace;
  client_options.process_scope = *application_scope;
  client_options.startup_timeout = 5s;
  client_options.request_timeout = 5s;
  auto const started = Clock::now();
  auto client = ava::mcp::McpStdioClient::start(std::move(server), client_options);
  bool initialized = client && (*client)->initialization().server_name == "fake-mcp";
  auto tools = client ? (*client)->list_tools() : ava::core::Result<std::vector<ava::mcp::McpToolDescription>>(std::unexpected(client.error()));
  auto mcp_shutdown = client ? (*client)->shutdown(500ms) : ava::core::VoidResult(std::unexpected(client.error()));
  if (client)
    client->reset();
  auto const elapsed = elapsed_nanoseconds(started);

  supervisor->stop_accepting();
  auto const supervisor_shutdown = supervisor->shutdown(Clock::now() + 2s);
  auto const snapshot = supervisor->snapshot();
  std::size_t mcp_record_count = 0;
  bool record_finished = false;
  bool settlement_once = false;
  for (auto const& record : snapshot.records)
  {
    if (record.role != ava::process::ProcessRoleV1::Mcp)
      continue;
    ++mcp_record_count;
    record_finished = record.state == ava::process::ProcessStateV1::Finished && record.cleanup == ava::process::CleanupStateV1::Complete;
    settlement_once = record.settlement_count == 1;
  }
  bool const supervisor_verified = supervisor_shutdown.complete && snapshot.live_records == 0 && mcp_record_count == 1 && record_finished && settlement_once;
  if (!supervisor_verified)
    fail("supervised MCP benchmark did not produce one completely settled managed-group record");
  bool const clean_after = no_waitable_children();
  bool const expected = tools && tools->size() == 1 && tools->front().name == "echo";
  emit_helper_measurement(options.benchmark_case, "lifecycle_ns", "ns",
                          {{.ordinal = 1,
                            .value = elapsed,
                            .metrics = {},
                            .checks = {{"protocol_compatible", initialized},
                                       {"expected_response", expected},
                                       {"shutdown_complete", static_cast<bool>(mcp_shutdown) && supervisor_shutdown.complete},
                                       {"immediate_child_guard", clean_before && clean_after},
                                       {"supervisor_record_finished", true},
                                       {"supervisor_settlement_once", true}}}},
                          {{"authority", std::string("supervised")},
                           {"supervisor_record_finished", true},
                           {"supervisor_settlement_once", true},
                           {"cleanup_scope", std::string("managed_group")}});
#endif
}

void benchmark_lsp(BackendBenchmarkOptions const& options)
{
  if (!executable_file(options.fake_lsp_server))
  {
    emit_helper_unsupported(options.benchmark_case, "lifecycle_ns", "ns", "fixture_unavailable", kFixtureUnavailableReason,
                            family_case_metrics("legacy_local"));
    return;
  }
  TemporaryDirectory temporary("lsp-family");
  auto const workspace = temporary.path() / "workspace";
  std::filesystem::create_directories(workspace);
  std::ofstream(workspace / "main.cpp", std::ios::binary | std::ios::trunc) << "int main() { return 0; }\n";
  auto anchors = ava::core::AnchorSet::open({workspace});
  if (!anchors)
    fail(anchors.error().format());
  bool const clean_before = no_waitable_children();
  auto const started = Clock::now();
  auto client = ava::lsp::SubprocessLspClient::start(ava::lsp::ServerConfig{.argv = {options.fake_lsp_server.string()},
                                                                            .workspace_root = workspace,
                                                                            .anchor_set = *anchors,
                                                                            .process_cwd = workspace,
                                                                            .startup_timeout = 5s,
                                                                            .request_timeout = 5s,
                                                                            .language_id = "cpp"});
  auto diagnostics =
      client ? (*client)->diagnostics(workspace / "main.cpp") : ava::core::Result<std::vector<ava::lsp::Diagnostic>>(std::unexpected(client.error()));
  bool const alive = client && (*client)->is_alive();
  if (client)
    client->reset();
  auto const elapsed = elapsed_nanoseconds(started);
  bool const expected = diagnostics && diagnostics->size() == 1 && diagnostics->front().severity == 1 && diagnostics->front().line == 2;
  bool const clean_after = no_waitable_children();
  emit_family(options.benchmark_case, elapsed, alive, expected, true, clean_before && clean_after);
}

void benchmark_bash(BackendBenchmarkOptions const& options)
{
  TemporaryDirectory temporary("bash-family");
  auto const trusted_home = temporary.path() / "trusted-home";
  std::filesystem::create_directories(trusted_home);
  if (::chmod(trusted_home.c_str(), S_IRWXU) != 0 || ::setenv("HOME", trusted_home.c_str(), 1) != 0)
    fail("failed to secure the bash benchmark trusted home");
  if (auto loaded = ava::core::load_account_once_and_freeze(); !loaded)
    fail(loaded.error().format());
  auto const workspace = temporary.path() / "workspace";
  auto const spill = temporary.path() / "spill";
  std::filesystem::create_directories(workspace);
  std::filesystem::create_directories(spill);
  std::ofstream(workspace / "fixture", std::ios::binary | std::ios::trunc) << "fixed\n";
  if (::chmod(workspace.c_str(), S_IRWXU) != 0 || ::chmod(spill.c_str(), S_IRWXU) != 0)
    fail("failed to secure bash benchmark roots");
  auto anchors = ava::core::AnchorSet::open({workspace, spill});
  if (!anchors)
    fail(anchors.error().format());
  ava::tools::ToolContext context;
  context.workspace_dir = workspace;
  context.spill_dir = spill;
  context.anchor_set = *anchors;
  bool const clean_before = no_waitable_children();
  auto const started = Clock::now();
  auto result = ava::tools::run_bash(context, "ls", ava::tools::BashOptions{.timeout = 5s, .max_bytes = 4096, .max_lines = 16});
  auto const elapsed = elapsed_nanoseconds(started);
  bool const expected = result && result->exit_code == 0 && !result->timed_out && !result->canceled && !result->truncated && !result->output.empty();
  bool const clean_after = no_waitable_children();
  emit_family(options.benchmark_case, elapsed, static_cast<bool>(result), expected, true, clean_before && clean_after);
}

}  // namespace

bool is_family_benchmark_case(std::string_view const benchmark_case) noexcept
{
  return benchmark_case == "family-curl-lifecycle" || benchmark_case == "family-plugin-lifecycle" || benchmark_case == "family-mcp-lifecycle" ||
         benchmark_case == "family-lsp-lifecycle" || benchmark_case == "family-bash-lifecycle";
}

void append_family_authorities(JsonFields& fields)
{
  fields.emplace_back("curl_authority", std::string(authority_for("family-curl-lifecycle")));
  fields.emplace_back("plugin_authority", std::string(authority_for("family-plugin-lifecycle")));
  fields.emplace_back("mcp_authority", std::string(authority_for("family-mcp-lifecycle")));
  fields.emplace_back("lsp_authority", std::string(authority_for("family-lsp-lifecycle")));
  fields.emplace_back("bash_authority", std::string(authority_for("family-bash-lifecycle")));
}

void run_family_benchmark(BackendBenchmarkOptions const& options)
{
  if (refuse_false_supervised_claim(options))
    return;
  if (options.benchmark_case == "family-curl-lifecycle")
    benchmark_curl(options);
  else if (options.benchmark_case == "family-plugin-lifecycle")
    benchmark_plugin(options);
  else if (options.benchmark_case == "family-mcp-lifecycle")
    benchmark_mcp(options);
  else if (options.benchmark_case == "family-lsp-lifecycle")
    benchmark_lsp(options);
  else if (options.benchmark_case == "family-bash-lifecycle")
    benchmark_bash(options);
  else
    fail("unknown family benchmark case");
}

}  // namespace ava::benchmark

#include "tests/backend_benchmark_cases.h"
#include "ava/process/scope.h"
#include "ava/process/supervisor.h"
#include "ava/agent/tool_dispatcher.h"
#include "ava/agent/tool_registry.h"
#include "ava/plugin/discovery.h"
#include "ava/plugin/manifest.h"
#include "ava/plugin/runner.h"
#include "ava/session/session_store.h"

#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include <sys/resource.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {
using Clock = std::chrono::steady_clock;

using Options = ava::benchmark::BackendBenchmarkOptions;

[[noreturn]] void fail(std::string const& message)
{
  std::cerr << message << '\n';
  std::exit(2);
}

std::size_t parse_size(char const* value, std::string_view option)
{
  try
  {
    auto const parsed = std::stoull(value);
    if (parsed == 0)
      fail(std::string(option) + " must be greater than zero");
    return static_cast<std::size_t>(parsed);
  }
  catch (std::exception const&)
  {
    fail(std::string(option) + " requires a positive integer");
  }
}

Options parse_options(int argc, char** argv)
{
  Options options;
  for (int index = 1; index < argc; ++index)
  {
    std::string_view const argument(argv[index]);
    if (argument == "--case" && index + 1 < argc)
      options.benchmark_case = argv[++index];
    else if (argument == "--iterations" && index + 1 < argc)
      options.iterations = parse_size(argv[++index], argument);
    else if (argument == "--entries" && index + 1 < argc)
      options.entries = parse_size(argv[++index], argument);
    else if (argument == "--records" && index + 1 < argc)
      options.records = parse_size(argv[++index], argument);
    else if (argument == "--hold-ms" && index + 1 < argc)
      options.hold_milliseconds = parse_size(argv[++index], argument);
    else if (argument == "--grace-ms" && index + 1 < argc)
      options.grace_milliseconds = parse_size(argv[++index], argument);
    else if (argument == "--deadline-ms" && index + 1 < argc)
      options.deadline_milliseconds = parse_size(argv[++index], argument);
    else if (argument == "--loopback-port" && index + 1 < argc)
      options.loopback_port = static_cast<unsigned int>(parse_size(argv[++index], argument));
    else if (argument == "--sample-plugin" && index + 1 < argc)
      options.sample_plugin = argv[++index];
    else if (argument == "--fake-process-child" && index + 1 < argc)
      options.fake_process_child = argv[++index];
    else if (argument == "--fake-mcp-server" && index + 1 < argc)
      options.fake_mcp_server = argv[++index];
    else if (argument == "--fake-lsp-server" && index + 1 < argc)
      options.fake_lsp_server = argv[++index];
    else
      fail("unknown or incomplete argument: " + std::string(argument));
  }
  if (options.benchmark_case.empty())
    fail("--case is required");
  return options;
}

class TemporaryDirectory
{
 public:
  explicit TemporaryDirectory(std::string_view label)
  {
    auto const nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    path_ = std::filesystem::temp_directory_path() /
            ("ava-backend-benchmark-" + std::string(label) + "-" + std::to_string(static_cast<long long>(getpid())) + "-" + std::to_string(nonce));
    std::filesystem::create_directories(path_);
  }

  ~TemporaryDirectory()
  {
    std::error_code error;
    std::filesystem::remove_all(path_, error);
  }

  std::filesystem::path const& path() const noexcept { return path_; }

 private:
  std::filesystem::path path_;
};

double elapsed_nanoseconds(Clock::time_point started)
{
  return static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - started).count());
}

void emit(double value, std::string_view unit, std::string_view details_json = "{}")
{
  std::cout << "{\"value\":" << value << ",\"unit\":\"" << unit << "\",\"details\":" << details_json << "}\n";
}

[[maybe_unused]] void emit_unsupported(std::string_view reason_code, std::string_view reason, std::string_view details_json = "{}")
{
  std::cout << "{\"status\":\"unsupported\",\"reason_code\":\"" << reason_code << "\",\"reason\":\"" << reason << "\",\"details\":" << details_json << "}\n";
}

ava::agent::RegisteredTool make_noop_tool(std::size_t index)
{
  auto const name = "bench_" + std::to_string(index);
  return ava::agent::RegisteredTool{
      .metadata = ava::agent::RegisteredToolMetadata{.name = name,
                                                     .description = "benchmark no-op",
                                                     .schema_json = "{\"type\":\"function\",\"function\":{\"name\":\"" + name +
                                                                    "\",\"description\":\"benchmark no-op\",\"parameters\":{\"type\":\"object\"}}}",
                                                     .permission_category = "none",
                                                     .output_bound_summary = "constant",
                                                     .execution_mode = "synchronous",
                                                     .event_rendering_hint = "none",
                                                     .description_family = std::nullopt},
      .executor = [](ava::tools::ToolContext const&, ava::agent::ToolDispatchServices const&, ava::agent::ProviderToolCall const& call) {
        return ava::agent::ToolDispatchResult{.call_id = call.id, .name = call.name, .success = true, .result_text = "{}"};
      }};
}

ava::agent::ToolRegistry make_registry(std::size_t entries)
{
  ava::agent::ToolRegistry registry;
  for (std::size_t index = 0; index < entries; ++index)
  {
    auto registered = registry.register_tool(make_noop_tool(index));
    if (!registered)
      fail(registered.error().format());
  }
  return registry;
}

void benchmark_short_calls(Options const& options)
{
  auto registry = make_registry(1);
  auto const* tool = registry.find("bench_0");
  if (tool == nullptr)
    fail("benchmark no-op tool was not registered");
  ava::tools::ToolContext const context;
  ava::agent::ToolDispatchServices const services;
  ava::agent::ProviderToolCall const call{.id = "bench", .name = "bench_0", .arguments_json = "{}"};
  auto const started = Clock::now();
  std::size_t successes = 0;
  for (std::size_t index = 0; index < options.iterations; ++index)
    successes += tool->executor(context, services, call).success ? 1U : 0U;
  if (successes != options.iterations)
    fail("no-op dispatch did not complete every call");
  emit(elapsed_nanoseconds(started) / static_cast<double>(options.iterations), "ns_per_call", "{\"calls\":" + std::to_string(options.iterations) + "}");
}

void benchmark_file_dispatch(Options const& options, bool canceled)
{
  TemporaryDirectory temporary("file");
  auto const workspace = temporary.path() / "workspace";
  std::filesystem::create_directories(workspace);
  {
    std::ofstream file(workspace / "note.txt", std::ios::binary);
    file << "hello benchmark\n";
  }
  ava::tools::ToolContext context{.workspace_dir = workspace};
  if (canceled)
    context.cancel_requested = [] { return true; };
  auto const dispatcher_result = ava::agent::ToolDispatcher::create_strict(context, {}, ava::agent::ToolVisibilityOptions{.included_tools = {"read_file"}});
  if (!dispatcher_result)
    fail(dispatcher_result.error().format());
  auto const& dispatcher = *dispatcher_result;
  ava::agent::ProviderToolCall const call{.id = "bench", .name = "read_file", .arguments_json = "{\"path\":\"note.txt\"}"};
  auto const started = Clock::now();
  std::size_t expected = 0;
  for (std::size_t index = 0; index < options.iterations; ++index)
  {
    auto result = dispatcher.dispatch(call);
    if (!result)
      fail(result.error().format());
    expected += (canceled ? !result->success : result->success) ? 1U : 0U;
  }
  if (expected != options.iterations)
    fail(canceled ? "canceled dispatch unexpectedly succeeded" : "file dispatch failed");
  emit(elapsed_nanoseconds(started) / static_cast<double>(options.iterations), "ns_per_call",
       "{\"calls\":" + std::to_string(options.iterations) + ",\"canceled\":" + (canceled ? "true" : "false") + "}");
}

void benchmark_native_registry(Options const& options)
{
  auto const& registry = ava::agent::builtin_tool_registry();
  auto const entries = registry.entries();
  if (entries.empty())
    fail("native registry returned no entries");
  std::string const target(entries.back().metadata.name);
  auto const* expected = &entries.back();

  auto const started = Clock::now();
  std::size_t matches = 0;
  for (std::size_t index = 0; index < options.iterations; ++index)
    matches += registry.find(target) == expected ? 1U : 0U;
  auto const elapsed = elapsed_nanoseconds(started);
  if (matches != options.iterations)
    fail("native registry failed exact lookup of its final entry");
  emit(elapsed / static_cast<double>(options.iterations), "ns_per_lookup",
       "{\"lookups\":" + std::to_string(options.iterations) + ",\"target\":\"" + target + "\",\"entry_count\":" + std::to_string(entries.size()) + "}");
}

void benchmark_catalog(Options const& options)
{
  auto const target = "bench_" + std::to_string(options.entries - 1);
  auto const started = Clock::now();
  auto registry = make_registry(options.entries);
  auto const schemas = registry.tool_schemas_json(ava::tools::ToolContext{});
  auto const* selected = registry.find(target);
  auto const elapsed = elapsed_nanoseconds(started);
  if (schemas.size() != options.entries || selected == nullptr)
    fail("catalog schema materialization or linear selection failed");
  emit(elapsed, "ns_per_composite",
       "{\"entries\":" + std::to_string(options.entries) + ",\"schemas\":" + std::to_string(schemas.size()) + ",\"lookup_target\":\"" + target +
           "\",\"timed_boundary\":\"registry_construction_plus_full_schema_materialization_plus_last_entry_lookup\"}");
}

std::vector<ava::session::SessionEntry> session_entries(std::size_t records)
{
  std::vector<ava::session::SessionEntry> entries;
  entries.reserve(records);
  for (std::size_t index = 0; index < records; ++index)
  {
    entries.push_back(ava::session::SessionEntry{.id = "entry-" + std::to_string(index),
                                                 .parent_id = {},
                                                 .type = ava::session::EntryType::UserMessage,
                                                 .timestamp = "2026-01-01T00:00:00Z",
                                                 .data_json = "{\"text\":\"benchmark\"}"});
  }
  return entries;
}

ava::session::SessionStore create_session_fixture(std::filesystem::path const& root, std::filesystem::path const& workspace, std::string id,
                                                  std::size_t records)
{
  ava::session::SessionStore store(ava::session::SessionStoreOptions{.root_dir = root, .workspace_dir = workspace, .session_id = std::move(id)});
  auto lease = ava::session::SessionLease::create_and_acquire(store.session_path());
  if (!lease)
    fail(lease.error().format());
  auto appended = store.append_validated_copy(*lease, session_entries(records));
  if (!appended)
    fail(appended.error().format());
  return store;
}

ava::session::SessionReadLimits limits_for(std::size_t records)
{
  return ava::session::SessionReadLimits{.max_file_bytes = 256U * 1024U * 1024U, .max_line_bytes = 1024U * 1024U, .max_entries = records + 1U};
}

void benchmark_session_open(Options const& options)
{
  TemporaryDirectory temporary("session");
  auto const workspace = temporary.path() / "workspace";
  auto const root = temporary.path() / "sessions";
  std::filesystem::create_directories(workspace);
  static_cast<void>(create_session_fixture(root, workspace, "bench", options.records));
  auto const started = Clock::now();
  auto opened = ava::session::SessionStore::open(workspace, "bench", root);
  if (!opened)
    fail(opened.error().format());
  auto loaded = opened->load_bounded(limits_for(options.records));
  if (!loaded || loaded->size() != options.records)
    fail(loaded ? "session record count mismatch" : loaded.error().format());
  emit(elapsed_nanoseconds(started), "ns", "{\"records\":" + std::to_string(options.records) + "}");
}

void benchmark_metadata(Options const& options)
{
  TemporaryDirectory temporary("metadata");
  auto const workspace = temporary.path() / "workspace";
  auto const root = temporary.path() / "sessions";
  std::filesystem::create_directories(workspace);
  for (std::size_t index = 0; index < options.entries; ++index)
    static_cast<void>(create_session_fixture(root, workspace, "session-" + std::to_string(index), 1));
  auto const started = Clock::now();
  auto listed = ava::session::SessionStore::list_sessions_bounded(
      workspace, root,
      ava::session::SessionListLimits{.per_session = limits_for(1), .max_sessions = options.entries + 1U, .max_total_file_bytes = 64U * 1024U * 1024U});
  if (!listed || listed->size() != options.entries)
    fail(listed ? "metadata listing count mismatch" : listed.error().format());
  emit(elapsed_nanoseconds(started), "ns", "{\"sessions\":" + std::to_string(options.entries) + "}");
}

void benchmark_cancellation_acknowledgement(Options const& options)
{
  TemporaryDirectory temporary("cancel");
  auto const workspace = temporary.path() / "workspace";
  auto const root = temporary.path() / "sessions";
  std::filesystem::create_directories(workspace);
  auto store = create_session_fixture(root, workspace, "bench", 1000);
  auto const started = Clock::now();
  for (std::size_t index = 0; index < options.iterations; ++index)
  {
    auto loaded = store.load_bounded(limits_for(1000), [] { return true; });
    if (loaded)
      fail("pre-canceled session read unexpectedly completed");
  }
  emit(elapsed_nanoseconds(started) / static_cast<double>(options.iterations), "ns_per_acknowledgement",
       "{\"attempts\":" + std::to_string(options.iterations) + "}");
}

ava::plugin::PluginManifest load_sample_manifest(std::filesystem::path const& directory)
{
  if (directory.empty())
    fail("--sample-plugin is required for plugin cases");
  auto manifest = ava::plugin::load_plugin_manifest(directory / "plugin.json");
  if (!manifest)
    fail(manifest.error().format());
  return std::move(*manifest);
}

bool no_waitable_children()
{
  errno = 0;
  int status = 0;
  auto const result = waitpid(-1, &status, WNOHANG);
  return result == -1 && errno == ECHILD;
}

void benchmark_plugin(Options const& options, bool cleanup_only)
{
  auto const manifest = load_sample_manifest(options.sample_plugin);
  auto const workspace = options.sample_plugin.parent_path();
  auto supervisor = std::make_shared<ava::process::Supervisor>();
  auto process_scope = ava::process::ProcessScopeV1::application(supervisor);
  if (!process_scope)
    fail(process_scope.error().format());
  auto const started = Clock::now();
  for (std::size_t index = 0; index < options.iterations; ++index)
  {
    auto process = ava::plugin::PluginProcess::start(manifest, ava::plugin::PluginRunnerOptions{.workspace_dir = workspace, .process_scope = *process_scope});
    if (!process)
      fail(process.error().format());
    if (!cleanup_only)
    {
      auto called = (*process)->call_tool("todo_add", "{\"text\":\"benchmark\"}", "bench");
      if (!called || !called->ok)
        fail(called ? "sample plugin returned failure" : called.error().format());
    }
    auto shutdown = (*process)->shutdown();
    if (!shutdown)
      fail(shutdown.error().format());
  }
  supervisor->stop_accepting();
  auto const supervisor_shutdown = supervisor->shutdown(Clock::now() + std::chrono::seconds(2));
  auto const snapshot = supervisor->snapshot();
  if (!supervisor_shutdown.complete || snapshot.live_records != 0)
    fail("plugin benchmark supervisor cleanup did not complete");
  if (!no_waitable_children())
    fail("plugin benchmark left a child process waitable");
  emit(elapsed_nanoseconds(started) / static_cast<double>(options.iterations), cleanup_only ? "ns_per_cleanup" : "ns_per_one_shot_call",
       "{\"calls\":" + std::to_string(options.iterations) + ",\"children_after\":0}");
}

std::string read_file(std::filesystem::path const& path)
{
  std::ifstream input(path, std::ios::binary);
  if (!input)
    fail("failed to read " + path.string());
  return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

void benchmark_manifest_discovery(Options const& options)
{
  auto manifest_text = read_file(options.sample_plugin / "plugin.json");
  std::string const original = "\"id\": \"com.example.todo\"";
  auto const id_offset = manifest_text.find(original);
  if (id_offset == std::string::npos)
    fail("sample manifest id fixture changed");
  TemporaryDirectory temporary("manifests");
  auto const plugins = temporary.path() / "plugins";
  std::filesystem::create_directories(plugins);
  for (std::size_t index = 0; index < options.entries; ++index)
  {
    auto text = manifest_text;
    auto const replacement = "\"id\": \"com.example.todo" + std::to_string(index) + "\"";
    text.replace(id_offset, original.size(), replacement);
    auto const directory = plugins / ("plugin-" + std::to_string(index));
    std::filesystem::create_directories(directory);
    std::ofstream(directory / "plugin.json", std::ios::binary) << text;
  }
  if (!no_waitable_children())
    fail("manifest benchmark began with a waitable child");
  auto const started = Clock::now();
  auto discovered = ava::plugin::discover_plugins({.global_plugins_dir = {}, .project_plugins_dir = plugins});
  if (!discovered || discovered->size() != options.entries)
    fail(discovered ? "manifest discovery count mismatch" : discovered.error().format());
  if (!no_waitable_children())
    fail("manifest discovery started a plugin process");
  emit(elapsed_nanoseconds(started), "ns", "{\"manifests\":" + std::to_string(options.entries) + ",\"children_before\":0,\"children_after\":0}");
}

#if defined(__linux__) || defined(__APPLE__)
long peak_rss_kib()
{
  struct rusage usage{};
  if (getrusage(RUSAGE_SELF, &usage) != 0)
    fail("getrusage failed");
#if defined(__APPLE__)
  return usage.ru_maxrss / 1024L;
#else
  return usage.ru_maxrss;
#endif
}
#endif

#if defined(__linux__)
struct CurrentResidentRss
{
  double kib = 0.0;
  long page_size_bytes = 0;
};

std::optional<CurrentResidentRss> current_resident_rss()
{
  std::ifstream statm("/proc/self/statm");
  std::uint64_t total_pages = 0;
  std::uint64_t resident_pages = 0;
  if (!(statm >> total_pages >> resident_pages))
    return std::nullopt;
  static_cast<void>(total_pages);
  auto const page_size = sysconf(_SC_PAGESIZE);
  if (page_size <= 0 || resident_pages > std::numeric_limits<std::uint64_t>::max() / static_cast<std::uint64_t>(page_size))
    return std::nullopt;
  auto const resident_bytes = resident_pages * static_cast<std::uint64_t>(page_size);
  return CurrentResidentRss{.kib = static_cast<double>(resident_bytes) / 1024.0, .page_size_bytes = page_size};
}
#endif

void benchmark_repeated_memory(Options const& options)
{
#if !defined(__linux__) && !defined(__APPLE__)
  emit_unsupported("resident_rss_platform_unsupported", "Repeated-call resident RSS is implemented only for Linux, with a macOS peak high-water fallback.",
                   "{\"calls\":" + std::to_string(options.iterations) + "}");
#else
  auto registry = make_registry(1);
  auto const* tool = registry.find("bench_0");
  if (tool == nullptr)
    fail("benchmark no-op tool was not registered");
  ava::tools::ToolContext const context;
  ava::agent::ToolDispatchServices const services;
  ava::agent::ProviderToolCall const call{.id = "bench", .name = "bench_0", .arguments_json = "{}"};
#if defined(__linux__)
  auto const current_before = current_resident_rss();
  if (!current_before)
  {
    emit_unsupported("procfs_statm_unavailable", "Current resident RSS requires readable Linux /proc/self/statm and a valid page size.",
                     "{\"calls\":" + std::to_string(options.iterations) + "}");
    return;
  }
  auto const peak_before = peak_rss_kib();
  for (std::size_t index = 0; index < options.iterations; ++index)
    static_cast<void>(tool->executor(context, services, call));
  auto const current_after = current_resident_rss();
  auto const peak_after = peak_rss_kib();
  if (!current_after)
  {
    emit_unsupported("procfs_statm_unavailable", "Current resident RSS requires readable Linux /proc/self/statm and a valid page size.",
                     "{\"calls\":" + std::to_string(options.iterations) + "}");
    return;
  }
  emit(current_after->kib - current_before->kib, "current_rss_delta_kib",
       "{\"calls\":" + std::to_string(options.iterations) +
           ",\"measurement\":\"current_resident_rss_after_minus_before\",\"current_rss_source\":\"/proc/self/statm\",\"page_size_bytes\":" +
           std::to_string(current_before->page_size_bytes) + ",\"current_before_kib\":" + std::to_string(current_before->kib) +
           ",\"current_after_kib\":" + std::to_string(current_after->kib) + ",\"peak_high_water_before_kib\":" + std::to_string(peak_before) +
           ",\"peak_high_water_after_kib\":" + std::to_string(peak_after) + ",\"peak_high_water_delta_kib\":" + std::to_string(peak_after - peak_before) +
           ",\"platform_fallback\":false}");
#else
  auto const peak_before = peak_rss_kib();
  for (std::size_t index = 0; index < options.iterations; ++index)
    static_cast<void>(tool->executor(context, services, call));
  auto const peak_after = peak_rss_kib();
  emit(static_cast<double>(peak_after - peak_before), "peak_rss_high_water_delta_kib",
       "{\"calls\":" + std::to_string(options.iterations) + ",\"measurement\":\"peak_rss_high_water_after_minus_before\",\"peak_high_water_before_kib\":" +
           std::to_string(peak_before) + ",\"peak_high_water_after_kib\":" + std::to_string(peak_after) +
           ",\"peak_high_water_delta_kib\":" + std::to_string(peak_after - peak_before) + ",\"platform_fallback\":true}");
#endif
#endif
}
}  // namespace

int main(int argc, char** argv)
{
  auto const options = parse_options(argc, argv);
  if (ava::benchmark::is_process_benchmark_case(options.benchmark_case))
    ava::benchmark::run_process_benchmark(options);
  else if (ava::benchmark::is_family_benchmark_case(options.benchmark_case))
    ava::benchmark::run_family_benchmark(options);
  else if (options.benchmark_case == "short-calls")
    benchmark_short_calls(options);
  else if (options.benchmark_case == "file-dispatch")
    benchmark_file_dispatch(options, false);
  else if (options.benchmark_case == "canceled-calls")
    benchmark_file_dispatch(options, true);
  else if (options.benchmark_case == "native-registry")
    benchmark_native_registry(options);
  else if (options.benchmark_case == "catalog")
    benchmark_catalog(options);
  else if (options.benchmark_case == "session-open")
    benchmark_session_open(options);
  else if (options.benchmark_case == "metadata")
    benchmark_metadata(options);
  else if (options.benchmark_case == "cancellation-ack")
    benchmark_cancellation_acknowledgement(options);
  else if (options.benchmark_case == "plugin-call")
    benchmark_plugin(options, false);
  else if (options.benchmark_case == "plugin-cleanup")
    benchmark_plugin(options, true);
  else if (options.benchmark_case == "manifest-discovery")
    benchmark_manifest_discovery(options);
  else if (options.benchmark_case == "repeated-memory")
    benchmark_repeated_memory(options);
  else
    fail("unknown benchmark case: " + options.benchmark_case);
  return 0;
}

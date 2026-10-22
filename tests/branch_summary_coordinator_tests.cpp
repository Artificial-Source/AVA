#include "sys.h"
#include "tests/support/app_runtime_support.h"
#include "tests/support/session_test_support.h"
#include "tests/support/test_env.h"
#include "tests/support/test_harness.h"
#include "tests/support/test_timeout.h"
#include "ava/http/transport.h"
#include "ava/app/branch_summary_coordinator.h"
#include "ava/app/runtime/OpenContext.h"
#include "ava/app/runtime/Session.h"
#include "ava/app/session_run_controller.h"
#include "ava/agent/message_builder.h"
#include "ava/config/model_config.h"
#include "ava/session/assistant_output.h"
#include "ava/session/record.h"
#include "ava/session/session_metadata.h"
#include "ava/provider/catalog.h"
#include "ava/core/AnchorSet.h"
#include "ava/core/ids.h"
#include "ava/core/json.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>
#include <unistd.h>

namespace {

using namespace std::chrono_literals;

constexpr ava::session::SessionReadLimits kReadLimits{.max_file_bytes = 8U * 1024U * 1024U, .max_line_bytes = 1024U * 1024U, .max_entries = 16384};

ava::session::SessionEntry entry(std::string id, std::string parent_id, ava::session::EntryType type, std::string data_json, long long version = 4)
{
  return {.id = std::move(id),
          .parent_id = std::move(parent_id),
          .type = type,
          .timestamp = "2026-05-01T00:00:00Z",
          .data_json = std::move(data_json),
          .version = version};
}

ava::session::SessionEntry session_start(std::string id)
{
  return entry(
      std::move(id), "", ava::session::EntryType::SessionStart,
      R"({"mode":"build","provider":"openai","model":"gpt-5.5","context_sources":0,"context_window_tokens":272000,"max_output_tokens":128000,"prompt_override":false,"supports_tools":true,"supports_streaming":true,"supports_reasoning":true,"reports_usage":true})",
      0);
}

bool raw_append(std::filesystem::path const& path, ava::session::SessionEntry const& value)
{
  auto line = ava::session::serialize_session_entry_line(value);
  if (!line)
    return false;
  std::ofstream out(path, std::ios::binary | std::ios::app);
  out << *line << '\n';
  out.flush();
  return out.good();
}

bool raw_append_all(std::filesystem::path const& path, std::vector<ava::session::SessionEntry> const& values)
{
  std::ofstream out(path, std::ios::binary | std::ios::app);
  for (auto const& value : values)
  {
    auto line = ava::session::serialize_session_entry_line(value);
    if (!line)
      return false;
    out << *line << '\n';
  }
  out.flush();
  return out.good();
}

std::size_t count_type(std::vector<ava::session::SessionEntry> const& entries, ava::session::EntryType type)
{
  return static_cast<std::size_t>(std::ranges::count(entries, type, &ava::session::SessionEntry::type));
}

struct BranchFixture
{
  std::filesystem::path root;
  std::filesystem::path workspace;
  std::filesystem::path sessions_dir;
  ava::session::SessionStore source_store;
  ava::session::SessionStore current_store;
  std::shared_ptr<ava::session::SessionAppendTarget> current_target;
  std::shared_ptr<ava::app::SessionRunController> current_controller;
  ava::session::SessionReadAuthority current_authority;
  std::string fork_entry_id;
  std::string branch_root_entry_id;
  std::string branch_tip_entry_id;

  BranchFixture(std::filesystem::path root_in, std::filesystem::path workspace_in, std::filesystem::path sessions_dir_in,
                ava::session::SessionStore source_store_in, ava::session::SessionStore current_store_in,
                std::shared_ptr<ava::session::SessionAppendTarget> current_target_in, std::shared_ptr<ava::app::SessionRunController> current_controller_in,
                ava::session::SessionReadAuthority current_authority_in, std::string fork_entry_id_in, std::string branch_root_entry_id_in,
                std::string branch_tip_entry_id_in)
      : root(std::move(root_in)),
        workspace(std::move(workspace_in)),
        sessions_dir(std::move(sessions_dir_in)),
        source_store(std::move(source_store_in)),
        current_store(std::move(current_store_in)),
        current_target(std::move(current_target_in)),
        current_controller(std::move(current_controller_in)),
        current_authority(std::move(current_authority_in)),
        fork_entry_id(std::move(fork_entry_id_in)),
        branch_root_entry_id(std::move(branch_root_entry_id_in)),
        branch_tip_entry_id(std::move(branch_tip_entry_id_in))
  {
  }

  ~BranchFixture()
  {
    if (current_controller)
      current_controller->shutdown();
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
  }

  ava::app::BranchSummaryOperationRequest request(ava::app::BranchSummaryGenerator generator = nullptr) const
  {
    auto registry = ava::config::builtin_model_registry();
    return {.current_session_id = current_store.options_r()->session_id,
            .source_session_id = source_store.options_session_id_copy(),
            .source_session_path = source_store.session_path(),
            .source_label = "Abandoned parent work",
            .workspace_dir = workspace,
            .root_dir = sessions_dir,
            .read_limits = kReadLimits,
            .current_read_authority = current_authority,
            .current_controller = current_controller,
            .paths = {},
            .selected_model = ava::config::select_default_model(registry),
            .provider_catalog = ava::provider::ProviderCatalog::build_builtins_only(),
            .anchor_set = nullptr,
            .provider_options = {.access_token = "snapshot-secret", .credential_type = "bearer"},
            .generator = std::move(generator)};
  }
};

std::unique_ptr<BranchFixture> make_fixture(std::string_view name, bool append_suffix = true)
{
  auto const root = create_empty_root("branch-summary-" + std::string(name) + "-" + ava::core::make_id("test"));
  auto const workspace = root / "workspace";
  auto const sessions_dir = root / "sessions";
  std::filesystem::create_directories(workspace);
  auto source = ava::session::SessionStore::create(workspace, sessions_dir);
  expect(source.has_value(), source ? "branch summary fixture creates source" : source.error().format());
  if (!source)
    return nullptr;
  auto source_lease = ava::session::SessionLease::create_and_acquire(source->session_path());
  expect(source_lease.has_value(), source_lease ? "branch summary fixture owns source" : source_lease.error().format());
  if (!source_lease)
    return nullptr;
  auto const start_id = ava::core::make_id("entry");
  auto const fork_id = ava::core::make_id("entry");
  auto appended_start = source->append(*source_lease, session_start(start_id));
  auto appended_fork = source->append(*source_lease, entry(fork_id, start_id, ava::session::EntryType::UserMessage, R"({"text":"fork request"})"));
  expect(appended_start && appended_fork, "branch summary fixture appends source prefix");
  if (!appended_start || !appended_fork)
    return nullptr;

  auto branched = ava::session::create_session_branch({.workspace_dir = workspace,
                                                       .root_dir = sessions_dir,
                                                       .source_session_id = source->options_r()->session_id,
                                                       .branch_from_entry_id = fork_id,
                                                       .name = std::nullopt,
                                                       .labels = std::nullopt,
                                                       .read_limits = kReadLimits,
                                                       .source_lease = &*source_lease,
                                                       .mode = ava::session::SessionBranchMode::Fork,
                                                       .actor = "test"});
  expect(branched.has_value(), branched ? "branch summary fixture creates current fork" : branched.error().format());
  if (!branched)
    return nullptr;

  std::string root_id;
  std::string tip_id;
  if (append_suffix)
  {
    root_id = ava::core::make_id("entry");
    tip_id = ava::core::make_id("entry");
    auto appended_user =
        source->append(*source_lease, entry(root_id, fork_id, ava::session::EntryType::UserMessage, R"({"text":"implement the abandoned parent approach"})"));
    auto appended_assistant = source->append(
        *source_lease, entry(tip_id, root_id, ava::session::EntryType::AssistantMessage, R"({"text":"the parent approach reached a useful result"})"));
    expect(appended_user && appended_assistant, "branch summary fixture appends abandoned source suffix");
    if (!appended_user || !appended_assistant)
      return nullptr;
  }
  source_lease = ava::session::SessionLease{};

  auto target = ava::session::SessionAppendTarget::create_persistent(branched->store, branched->lease, kReadLimits);
  expect(target.has_value(), target ? "branch summary fixture creates current append target" : target.error().format());
  if (!target)
    return nullptr;
  auto authority = (*target)->read_authority();
  expect(authority.has_value(), authority ? "branch summary fixture creates current read authority" : authority.error().format());
  if (!authority)
    return nullptr;
  auto controller = std::make_shared<ava::app::SessionRunController>(*target);
  return std::make_unique<BranchFixture>(root, workspace, sessions_dir, std::move(*source), std::move(branched->store), *target, std::move(controller),
                                         std::move(*authority), fork_id, std::move(root_id), std::move(tip_id));
}

struct BlockingGenerator
{
  std::mutex mutex;
  std::condition_variable_any changed;
  std::size_t calls = 0;
  bool started = false;
  bool release = false;
  bool stop_seen = false;
  std::string output = "Useful abandoned parent context";
  ava::app::BranchSummaryGenerationPrompt prompt;

  ava::core::Result<std::string> generate(ava::app::BranchSummaryGenerationPrompt const& value, std::stop_token stop_token,
                                          std::chrono::steady_clock::time_point deadline)
  {
    std::unique_lock lock(mutex);
    ++calls;
    prompt = value;
    started = true;
    changed.notify_all();
    if (!changed.wait_until(lock, stop_token, deadline, [&] { return release; }) && !stop_token.stop_requested())
      return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Provider, "fake generator deadline expired"));
    stop_seen = stop_token.stop_requested();
    if (stop_seen)
      return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Provider, "fake generator canceled"));
    return output;
  }

  bool wait_started()
  {
    std::unique_lock lock(mutex);
    return changed.wait_until(lock, ava::tests::now_plus_seconds(5), [&] { return started; });
  }

  void allow()
  {
    std::lock_guard lock(mutex);
    release = true;
    changed.notify_all();
  }
};

ava::app::BranchSummaryGenerator generator_for(std::shared_ptr<BlockingGenerator> const& state)
{
  return [state](ava::app::BranchSummaryGenerationPrompt const& prompt, std::stop_token stop_token, std::chrono::steady_clock::time_point deadline) {
    return state->generate(prompt, stop_token, deadline);
  };
}

bool coordinator_ready(ava::core::Result<std::shared_ptr<ava::app::BranchSummaryCoordinator>> const& coordinator, std::string_view label)
{
  return ava::tests::require(coordinator.has_value(),
                             coordinator ? std::string(label) + ": coordinator starts" : std::string(label) + ": " + coordinator.error().format());
}

ava::core::Result<std::uint64_t> prepare_checked(std::shared_ptr<ava::app::BranchSummaryCoordinator> const& coordinator,
                                                 ava::app::BranchSummaryOperationRequest request, std::string_view label)
{
  auto generation = coordinator->prepare(std::move(request));
  (void)ava::tests::require(generation.has_value(),
                            generation ? std::string(label) + ": preparation starts" : std::string(label) + ": " + generation.error().format());
  return generation;
}

bool await_confirmation(std::shared_ptr<ava::app::BranchSummaryCoordinator> const& coordinator, std::uint64_t generation, std::string_view label)
{
  auto const reached =
      coordinator->wait_for_phase(generation, ava::app::BranchSummaryPhase::AwaitingConfirmation,
                                  std::chrono::duration_cast<std::chrono::milliseconds>(ava::tests::now_plus_seconds(5) - std::chrono::steady_clock::now()));
  auto const snapshot = coordinator->snapshot();
  return ava::tests::require(
      reached, std::string(label) + ": awaiting confirmation (phase=" + std::string(ava::app::to_string(snapshot.phase)) + ", reason=" + snapshot.reason + ")");
}

std::optional<std::uint64_t> prepare_awaiting(std::shared_ptr<ava::app::BranchSummaryCoordinator> const& coordinator,
                                              ava::app::BranchSummaryOperationRequest request, std::string_view label)
{
  auto generation = prepare_checked(coordinator, std::move(request), label);
  if (!generation || !await_confirmation(coordinator, *generation, label))
    return std::nullopt;
  return *generation;
}

bool confirm_generation(std::shared_ptr<ava::app::BranchSummaryCoordinator> const& coordinator, std::uint64_t generation, std::string_view label)
{
  auto confirmed = coordinator->confirm(generation);
  return ava::tests::require(confirmed && *confirmed,
                             confirmed ? std::string(label) + ": confirmation accepted" : std::string(label) + ": " + confirmed.error().format());
}

ava::app::BranchSummarySnapshot await_terminal(std::shared_ptr<ava::app::BranchSummaryCoordinator> const& coordinator, std::uint64_t generation,
                                               std::chrono::milliseconds timeout = 4s)
{
  auto const idle = coordinator->wait_until_idle(
      std::chrono::duration_cast<std::chrono::milliseconds>(ava::tests::now_plus_seconds(timeout) - std::chrono::steady_clock::now()));
  auto snapshot = coordinator->snapshot();
  ava::tests::expect_all("branch summary terminal state (phase=" + std::string(ava::app::to_string(snapshot.phase)) + ", reason=" + snapshot.reason + ")",
                         {{"worker idle", idle}, {"requested generation", snapshot.generation == generation}, {"terminal phase", snapshot.terminal()}});
  return snapshot;
}

ava::app::BranchSummarySnapshot preparation_terminal(ava::app::BranchSummaryOperationRequest request)
{
  auto coordinator = ava::app::BranchSummaryCoordinator::create();
  if (!coordinator)
  {
    expect(false, coordinator.error().format());
    return {};
  }
  auto generation = (*coordinator)->prepare(std::move(request));
  if (!generation)
  {
    expect(false, generation.error().format());
    return {};
  }
  return await_terminal(*coordinator, *generation);
}

ava::app::BranchSummarySnapshot confirmed_terminal(BranchFixture const& fixture, ava::app::BranchSummaryGenerator generator,
                                                   ava::app::BranchSummaryCoordinatorOptions options = {})
{
  auto coordinator = ava::app::BranchSummaryCoordinator::create(std::move(options));
  if (!coordinator)
  {
    expect(false, coordinator.error().format());
    return {};
  }
  auto generation = prepare_awaiting(*coordinator, fixture.request(std::move(generator)), "confirmed branch summary");
  if (!generation || !confirm_generation(*coordinator, *generation, "confirmed branch summary"))
    return (*coordinator)->snapshot();
  return await_terminal(*coordinator, *generation);
}

void test_recoverable_snapshot_fingerprint_is_exact_sha256()
{
  auto const root = create_empty_root("branch-summary-fingerprint-sha256");
  auto const workspace = root / "workspace";
  auto const sessions = root / "sessions";
  std::filesystem::create_directories(workspace);
  auto store = ava::session::SessionStore::create(workspace, sessions);
  auto lease = store ? ava::session::SessionLease::create_and_acquire(store->session_path())
                     : ava::core::Result<ava::session::SessionLease>(std::unexpected(store.error()));
  if (!ava::tests::require(store && lease, "fingerprint fixture creates and leases its store"))
    return;
  auto snapshot = store->load_recoverable_snapshot_bounded(*lease, kReadLimits);
  constexpr std::array<std::uint8_t, 32> empty_sha256{0xe3, 0xb0, 0xc4, 0x42, 0x98, 0xfc, 0x1c, 0x14, 0x9a, 0xfb, 0xf4, 0xc8, 0x99, 0x6f, 0xb9, 0x24,
                                                      0x27, 0xae, 0x41, 0xe4, 0x64, 0x9b, 0x93, 0x4c, 0xa4, 0x95, 0x99, 0x1b, 0x78, 0x52, 0xb8, 0x55};
  expect(snapshot && snapshot->entries.empty() && snapshot->fingerprint.byte_count == 0 && snapshot->fingerprint.sha256 == empty_sha256,
         "recoverable exact snapshots expose the standard SHA-256 fingerprint of every covered byte");
}

void test_projection_is_pure_and_strict()
{
  std::vector<ava::session::SessionEntry> entries;
  entries.push_back(entry("fork", "", ava::session::EntryType::UserMessage, R"({"text":"fork"})"));
  entries.push_back(entry("user", "fork", ava::session::EntryType::UserMessage,
                          R"({"text":"question","attachments":[{"id":"attachment-secret","storage_path":"/secret/path"}],"provider":"hidden"})"));
  entries.push_back(entry("tool", "user", ava::session::EntryType::ToolCall, R"({"call_id":"private-call","name":"shell","arguments":{"cmd":"secret"}})"));
  auto text_data = ava::session::serialize_assistant_output_item_data_json(
      {.assistant_turn_id = "turn",
       .sequence = 0,
       .kind = ava::session::AssistantOutputItemKind::Text,
       .provider_item_id = std::nullopt,
       .provider_output_index = std::nullopt,
       .payload = ava::session::AssistantOutputText{.text = "committed answer", .assistant_phase = ava::session::AssistantOutputTextPhase::FinalAnswer}});
  auto reasoning_data = ava::session::serialize_assistant_output_item_data_json(
      {.assistant_turn_id = "turn",
       .sequence = 1,
       .kind = ava::session::AssistantOutputItemKind::Reasoning,
       .provider_item_id = "reasoning-item",
       .provider_output_index = 1,
       .payload = ava::session::AssistantOutputReasoning{.text = "private reasoning",
                                                         .format = "openai_responses",
                                                         .redacted = false,
                                                         .signature = "private-signature",
                                                         .redacted_data = std::nullopt,
                                                         .native_item_json = R"({"id":"reasoning-item","type":"reasoning","summary":[]})",
                                                         .private_replay_metadata_omitted = false}});
  auto commit_data = ava::session::serialize_assistant_turn_commit_data_json({.assistant_turn_id = "turn",
                                                                              .item_count = 2,
                                                                              .provider = "hidden-provider",
                                                                              .model = "hidden-model",
                                                                              .api_family = std::nullopt,
                                                                              .reasoning_format = std::nullopt,
                                                                              .finish_reason = "completed",
                                                                              .usage_json = std::nullopt});
  expect(text_data.has_value(), text_data ? "branch summary text fixture serializes" : "branch summary text fixture: " + text_data.error().format());
  expect(reasoning_data.has_value(),
         reasoning_data ? "branch summary reasoning fixture serializes" : "branch summary reasoning fixture: " + reasoning_data.error().format());
  expect(commit_data.has_value(), commit_data ? "branch summary commit fixture serializes" : "branch summary commit fixture: " + commit_data.error().format());
  if (!text_data || !reasoning_data || !commit_data)
    return;
  entries.push_back(entry("text", "tool", ava::session::EntryType::AssistantOutputItem, *text_data));
  entries.push_back(entry("reasoning", "tool", ava::session::EntryType::AssistantOutputItem, *reasoning_data));
  entries.push_back(entry("commit", "tool", ava::session::EntryType::AssistantTurnCommit, *commit_data));
  entries.push_back(entry("private", "commit", ava::session::EntryType::UserMessage, R"({"text":"private replay","internal_replay":true})"));
  entries.push_back(entry("compaction", "private", ava::session::EntryType::Compaction, R"({"summary":"durable compacted context"})"));
  entries.push_back(entry("legacy", "compaction", ava::session::EntryType::AssistantMessage, R"({"text":"legacy committed answer"})"));
  entries.push_back(entry(
      "marker", "legacy", ava::session::EntryType::BranchSummary,
      R"({"schema_version":1,"summary":"old","source_session_id":"source","branch_root_entry_id":"other","branch_tip_entry_id":"other","provider":"p","model":"m","reason":"abandoned_parent","actor":"tui"})"));
  auto staged_data = ava::session::serialize_assistant_output_item_data_json(
      {.assistant_turn_id = "staged",
       .sequence = 0,
       .kind = ava::session::AssistantOutputItemKind::Text,
       .provider_item_id = std::nullopt,
       .provider_output_index = std::nullopt,
       .payload =
           ava::session::AssistantOutputText{.text = "uncommitted staged answer", .assistant_phase = ava::session::AssistantOutputTextPhase::FinalAnswer}});
  expect(staged_data.has_value(), "branch summary staged projection fixture serializes");
  if (!staged_data)
    return;
  entries.push_back(entry("staged", "legacy", ava::session::EntryType::AssistantOutputItem, *staged_data));
  auto coverage = ava::session::inspect_branch_summary_coverage(entries, "source", "fork");
  auto projection = ava::app::project_branch_summary_prompt(entries, coverage);
  auto const expected =
      std::string("USER:\nquestion\n\nASSISTANT:\ncommitted answer\n\nCOMPACTION:\ndurable compacted context\n\nASSISTANT:\nlegacy committed answer");
  expect(projection && *projection == expected && projection->find("attachment-secret") == std::string::npos &&
             projection->find("private reasoning") == std::string::npos && projection->find("private replay") == std::string::npos &&
             projection->find("uncommitted") == std::string::npos && projection->find("hidden-provider") == std::string::npos &&
             projection->find("/secret/path") == std::string::npos,
         "branch summary projection contains only committed user/assistant text and compaction summaries in exact source order");

  auto projection_rejects_user_data = [&](std::string data_json) {
    auto invalid = entries;
    invalid[1].data_json = std::move(data_json);
    return !ava::app::project_branch_summary_prompt(invalid, ava::session::inspect_branch_summary_coverage(invalid, "source", "fork"));
  };
  auto malformed_utf8 = std::string("{\"text\":\"bad");
  malformed_utf8.push_back(static_cast<char>(0x80));
  malformed_utf8 += "\"}";
  expect(projection_rejects_user_data(R"({"text":"bad\u0080text"})") && projection_rejects_user_data(R"({"text":"bad\u001btext"})") &&
             projection_rejects_user_data(R"({"text":"bad\u007ftext"})") && projection_rejects_user_data(std::move(malformed_utf8)) &&
             projection_rejects_user_data(R"({"attachments":[]})"),
         "branch summary projection rejects C1, escape, DEL, malformed UTF-8, and missing projected text instead of sanitizing it");
}

void test_projection_boundaries_and_summary_sanitizer()
{
  std::vector<ava::session::SessionEntry> exact{{entry("fork", "", ava::session::EntryType::Error, R"({"message":"fork"})")}};
  std::string parent = "fork";
  for (std::size_t index = 0; index < 16; ++index)
  {
    auto const size = index == 15 ? 8066U : ava::app::kMaxBranchSummaryProjectedTextBytes;
    auto const id = "text-" + std::to_string(index);
    exact.push_back(entry(id, parent, ava::session::EntryType::UserMessage, "{\"text\":\"" + std::string(size, static_cast<char>('a' + index % 20)) + "\"}"));
    parent = id;
  }
  auto exact_coverage = ava::session::inspect_branch_summary_coverage(exact, "source", "fork");
  auto exact_projection = ava::app::project_branch_summary_prompt(exact, exact_coverage);
  expect(exact_projection && exact_projection->size() == ava::app::kMaxBranchSummaryProjectionBytes,
         "branch summary projection accepts exactly 128 KiB with each projected text at or below 8 KiB");
  exact.back().data_json.insert(exact.back().data_json.size() - 2, "x");
  auto oversized_projection = ava::app::project_branch_summary_prompt(exact, ava::session::inspect_branch_summary_coverage(exact, "source", "fork"));
  expect(!oversized_projection, "branch summary projection rejects one byte beyond its 128 KiB cap without truncation");

  std::vector<ava::session::SessionEntry> records{{entry("fork", "", ava::session::EntryType::Error, R"({"message":"fork"})")}};
  parent = "fork";
  for (std::size_t index = 0; index < ava::app::kMaxBranchSummaryCandidateRecords; ++index)
  {
    auto const id = "record-" + std::to_string(index);
    records.push_back(entry(id, parent, index == 0 ? ava::session::EntryType::UserMessage : ava::session::EntryType::Error,
                            index == 0 ? R"({"text":"one safe text"})" : R"({"message":"ignored"})"));
    parent = id;
  }
  auto exact_records = ava::app::project_branch_summary_prompt(records, ava::session::inspect_branch_summary_coverage(records, "source", "fork"));
  records.push_back(entry("one-too-many", parent, ava::session::EntryType::Error, R"({"message":"ignored"})"));
  auto too_many_records = ava::app::project_branch_summary_prompt(records, ava::session::inspect_branch_summary_coverage(records, "source", "fork"));
  expect(exact_records && !too_many_records, "branch summary projection accepts exactly 4096 candidate records and rejects 4097");

  auto exact_summary = ava::app::sanitize_generated_branch_summary(std::string(ava::app::kMaxGeneratedBranchSummaryBytes, 's'));
  auto oversized_summary = ava::app::sanitize_generated_branch_summary(std::string(ava::app::kMaxGeneratedBranchSummaryBytes + 1, 's'));
  auto wrapped = ava::app::sanitize_generated_branch_summary(
      "<analysis><analysis>inner private chain</analysis>outer private chain</analysis><summary kind=\"durable\">Durable result</summary>");
  auto large_hidden_reasoning = ava::app::sanitize_generated_branch_summary("<analysis>" + std::string(ava::app::kMaxGeneratedBranchSummaryBytes + 1, 'x') +
                                                                            "</analysis><summary>Bounded result</summary>");
  auto stray_reasoning = ava::app::sanitize_generated_branch_summary("private chain</analysis>Durable result");
  auto control = ava::app::sanitize_generated_branch_summary(std::string("bad\x7fsummary", 11));
  expect(exact_summary && !oversized_summary && wrapped && *wrapped == "Durable result" && large_hidden_reasoning &&
             *large_hidden_reasoning == "Bounded result" && !stray_reasoning && !control,
         "generated branch summaries enforce the final 8 KiB limit, reject controls and malformed reasoning, and strip bounded nested reasoning/wrapper tags");
  expect(
      !ava::app::BranchSummaryCoordinator::create({.operation_deadline = 0ms}) && !ava::app::BranchSummaryCoordinator::create({.operation_deadline = 30001ms}),
      "coordinator creation rejects zero and over-30-second operation deadlines");
}

void test_explicit_confirmation_success_and_existing()
{
  auto fixture = make_fixture("success");
  if (!fixture)
    return;
  auto state = std::make_shared<BlockingGenerator>();
  state->release = true;
  state->output = "<think>not persisted</think><summary>Useful abandoned parent context</summary>";
  auto coordinator_result = ava::app::BranchSummaryCoordinator::create();
  expect(coordinator_result.has_value(), coordinator_result ? "branch summary coordinator starts" : coordinator_result.error().format());
  if (!coordinator_result)
    return;
  auto coordinator = *coordinator_result;
  auto generation = prepare_awaiting(coordinator, fixture->request(generator_for(state)), "explicit confirmation");
  if (!generation)
    return;
  auto source_contender = ava::session::SessionLease::acquire(fixture->source_store.session_path());
  auto awaiting = coordinator->snapshot();
  expect(source_contender && state->calls == 0 && awaiting.source_label == "Abandoned parent work" &&
             awaiting.reason.find(fixture->source_store.options_r()->session_id) == std::string::npos && awaiting.reason.find("snapshot-secret") == std::string::npos &&
             !awaiting.refresh_required,
         "preparation is read-only, releases temporary source authority, invokes no provider, and publishes only bounded display state");
  source_contender = ava::session::SessionLease{};
  auto confirmed = coordinator->confirm(*generation);
  expect(confirmed && *confirmed, "branch summary confirmation is accepted exactly from the awaiting phase");
  auto terminal = await_terminal(coordinator, *generation);
  auto source_lease = ava::session::SessionLease::acquire(fixture->source_store.session_path());
  auto entries = source_lease ? fixture->source_store.load_bounded(*source_lease, kReadLimits)
                              : ava::core::Result<std::vector<ava::session::SessionEntry>>(std::unexpected(source_lease.error()));
  auto const marker = entries ? std::ranges::find_if(*entries, [](auto const& item) { return item.type == ava::session::EntryType::BranchSummary; })
                              : std::vector<ava::session::SessionEntry>::const_iterator{};
  bool marker_valid =
      entries && marker != entries->end() && ava::core::json::string_field(marker->data_json, "summary") == "Useful abandoned parent context" &&
      ava::core::json::string_field(marker->data_json, "provider") == "openai" && ava::core::json::string_field(marker->data_json, "model") == "gpt-5.5" &&
      ava::core::json::string_field(marker->data_json, "reason") == "abandoned_parent" && ava::core::json::string_field(marker->data_json, "actor") == "tui";
  expect(terminal.phase == ava::app::BranchSummaryPhase::Succeeded && terminal.refresh_required && entries &&
             count_type(*entries, ava::session::EntryType::BranchSummary) == 1 && marker_valid && state->calls == 1 &&
             state->prompt.system_instruction == ava::app::branch_summary_system_instruction() &&
             state->prompt.user_payload == "USER:\nimplement the abandoned parent approach\n\nASSISTANT:\nthe parent approach reached a useful result" &&
             state->prompt.user_payload.find("/private/path") == std::string::npos,
         "confirmed generation appends exactly one sanitized metadata record using only the pure candidate projection");
  source_lease = ava::session::SessionLease{};

  auto second_state = std::make_shared<BlockingGenerator>();
  second_state->release = true;
  auto second = coordinator->prepare(fixture->request(generator_for(second_state)));
  expect(second && coordinator->wait_until_idle(3s), "a completed coordinator accepts a later exact eligibility check");
  auto existing = coordinator->snapshot();
  expect(existing.phase == ava::app::BranchSummaryPhase::Existing && existing.refresh_required && second_state->calls == 0,
         "exact final coverage resolves to Existing without another provider call");
  coordinator->shutdown();
}

void test_cancel_before_confirmation_is_nonmutating_and_conflicts_reject()
{
  auto fixture = make_fixture("cancel-before-confirm");
  if (!fixture)
    return;
  auto const before = ava::tests::read_session_test_binary_file(fixture->source_store.session_path());
  auto state = std::make_shared<BlockingGenerator>();
  auto coordinator_result = ava::app::BranchSummaryCoordinator::create();
  if (!coordinator_result)
  {
    expect(false, coordinator_result.error().format());
    return;
  }
  auto coordinator = *coordinator_result;
  auto generation = prepare_awaiting(coordinator, fixture->request(generator_for(state)), "pre-confirm cancellation");
  if (!generation)
    return;
  auto conflict = coordinator->prepare(fixture->request(generator_for(state)));
  auto canceled = coordinator->cancel(*generation);
  auto terminal = await_terminal(coordinator, *generation);
  auto const after = ava::tests::read_session_test_binary_file(fixture->source_store.session_path());
  auto late_confirm = coordinator->confirm(*generation);
  expect(!conflict && canceled && *canceled && terminal.phase == ava::app::BranchSummaryPhase::Canceled && !terminal.refresh_required && before == after &&
             state->calls == 0 && late_confirm && !*late_confirm,
         "one operation is serialized, pre-confirm cancellation changes no source byte, and stale confirmation is rejected");
  coordinator->shutdown();
}

void test_typed_preparation_ineligibilities()
{
  {
    auto fixture = make_fixture("empty-suffix", false);
    if (fixture)
    {
      auto coordinator = ava::app::BranchSummaryCoordinator::create();
      if (!coordinator_ready(coordinator, "empty suffix"))
        return;
      auto generation = prepare_checked(*coordinator, fixture->request(), "empty suffix");
      if (generation)
        expect((*coordinator)->wait_until_idle(3s) && (*coordinator)->snapshot().eligibility_code == ava::app::BranchSummaryEligibilityCode::EmptySuffix,
               "empty direct-source suffix is a fixed typed ineligibility");
    }
  }
  {
    auto fixture = make_fixture("busy-source");
    if (fixture)
    {
      auto busy_lease = ava::session::SessionLease::acquire(fixture->source_store.session_path());
      auto coordinator = ava::app::BranchSummaryCoordinator::create();
      if (!coordinator_ready(coordinator, "busy source"))
        return;
      auto generation = prepare_checked(*coordinator, fixture->request(), "busy source");
      if (generation)
        expect((*coordinator)->wait_until_idle(3s) && (*coordinator)->snapshot().eligibility_code == ava::app::BranchSummaryEligibilityCode::SourceLeaseBusy,
               "an independently owned source lease is a fixed typed ineligibility");
    }
  }
  {
    auto fixture = make_fixture("active-run");
    if (fixture)
    {
      auto guard = fixture->current_controller->admit({.request_id = "active-normal-run"});
      auto coordinator = ava::app::BranchSummaryCoordinator::create();
      if (!coordinator_ready(coordinator, "active run"))
        return;
      auto generation = prepare_checked(*coordinator, fixture->request(), "active run");
      if (generation)
        expect((*coordinator)->wait_until_idle(3s) && (*coordinator)->snapshot().eligibility_code == ava::app::BranchSummaryEligibilityCode::ActiveRun,
               "an active normal run rejects preparation with a fixed typed reason");
    }
  }
  {
    auto fixture = make_fixture("unrelated");
    if (fixture)
    {
      auto request = fixture->request();
      request.source_session_id = "unrelated-session";
      request.source_session_path = fixture->sessions_dir / "unrelated-session.jsonl";
      auto coordinator = ava::app::BranchSummaryCoordinator::create();
      if (!coordinator_ready(coordinator, "unrelated source"))
        return;
      auto generation = prepare_checked(*coordinator, std::move(request), "unrelated source");
      if (generation)
        expect((*coordinator)->wait_until_idle(3s) && (*coordinator)->snapshot().eligibility_code == ava::app::BranchSummaryEligibilityCode::NotDirectSource,
               "unrelated, sibling, and descendant selections fail the exact direct-source relation check");
    }
  }
  {
    auto fixture = make_fixture("corrupt-source");
    if (fixture)
    {
      std::ofstream out(fixture->source_store.session_path(), std::ios::binary | std::ios::app);
      out << "{broken}\n";
      out.close();
      auto coordinator = ava::app::BranchSummaryCoordinator::create();
      if (!coordinator_ready(coordinator, "corrupt source"))
        return;
      auto generation = prepare_checked(*coordinator, fixture->request(), "corrupt source");
      if (generation)
        expect((*coordinator)->wait_until_idle(3s) && (*coordinator)->snapshot().eligibility_code == ava::app::BranchSummaryEligibilityCode::SourceCorrupt,
               "corrupt source history is a read-only fixed typed ineligibility");
    }
  }
  {
    auto fixture = make_fixture("ephemeral-current");
    if (fixture)
    {
      auto ephemeral = ava::session::SessionStore::create_ephemeral(fixture->workspace);
      auto target = ephemeral ? ava::session::SessionAppendTarget::create_ephemeral(*ephemeral, kReadLimits)
                              : ava::core::Result<std::shared_ptr<ava::session::SessionAppendTarget>>(std::unexpected(ephemeral.error()));
      auto authority = target ? (*target)->read_authority() : ava::core::Result<ava::session::SessionReadAuthority>(std::unexpected(target.error()));
      if (ava::tests::require(ephemeral && target && authority, "ephemeral fixture creates store, target, and authority"))
      {
        auto request = fixture->request();
        request.current_session_id = ephemeral->options_r()->session_id;
        request.current_read_authority = *authority;
        request.current_controller = std::make_shared<ava::app::SessionRunController>(*target);
        auto coordinator = ava::app::BranchSummaryCoordinator::create();
        if (!coordinator_ready(coordinator, "ephemeral current"))
          return;
        auto generation = prepare_checked(*coordinator, std::move(request), "ephemeral current");
        if (generation)
          expect((*coordinator)->wait_until_idle(3s) &&
                     (*coordinator)->snapshot().eligibility_code == ava::app::BranchSummaryEligibilityCode::CurrentSessionEphemeral,
                 "ephemeral current sessions fail with a fixed typed ineligibility");
      }
    }
  }
}

void test_remaining_typed_preparation_ineligibilities()
{
  auto fixture = make_fixture("remaining-ineligibilities");
  if (!fixture)
    return;

  {
    auto request = fixture->request();
    request.current_controller.reset();
    auto terminal = preparation_terminal(std::move(request));
    expect(terminal.eligibility_code == ava::app::BranchSummaryEligibilityCode::CurrentSessionUnavailable,
           "missing current controller ownership fails with CurrentSessionUnavailable");
  }
  {
    auto request = fixture->request();
    request.current_session_id += "-mismatch";
    auto terminal = preparation_terminal(std::move(request));
    expect(terminal.eligibility_code == ava::app::BranchSummaryEligibilityCode::CurrentSessionUnavailable,
           "mismatched current read authority fails with CurrentSessionUnavailable");
  }
  {
    auto request = fixture->request();
    request.source_session_id.clear();
    request.source_session_path.clear();
    auto terminal = preparation_terminal(std::move(request));
    expect(terminal.eligibility_code == ava::app::BranchSummaryEligibilityCode::InvalidSourceSelection,
           "empty selected source identity fails with InvalidSourceSelection");
  }
  {
    auto request = fixture->request();
    request.source_session_path += ".wrong";
    auto terminal = preparation_terminal(std::move(request));
    expect(terminal.eligibility_code == ava::app::BranchSummaryEligibilityCode::SourceUnavailable,
           "a selected path that does not exactly name the source fails with SourceUnavailable");
  }
  {
    auto current_entries = fixture->current_authority.load();
    auto missing_fork = ava::core::make_id("entry");
    std::string const session_id = fixture->source_store.options_r()->session_id;
    auto metadata = current_entries ? ava::session::make_session_metadata_entry({.parent_session_id = session_id,
                                                                                 .source_session_id = session_id,
                                                                                 .branch_from_entry_id = missing_fork,
                                                                                 .branch_origin = "fork",
                                                                                 .actor = "test"},
                                                                                current_entries->back().id)
                                    : ava::core::Result<ava::session::SessionEntry>(std::unexpected(current_entries.error()));
    auto appended = metadata ? fixture->current_controller->append(*metadata) : ava::core::VoidResult(std::unexpected(metadata.error()));
    expect(appended.has_value(), appended ? "missing fork relation metadata appends" : appended.error().format());
    if (appended)
    {
      auto terminal = preparation_terminal(fixture->request());
      expect(terminal.eligibility_code == ava::app::BranchSummaryEligibilityCode::ForkEntryNotFound,
             "a direct relation whose fork ID is absent from the source fails with ForkEntryNotFound");
    }
  }

  auto invalid_current = ava::session::SessionStore::create(fixture->workspace, fixture->sessions_dir);
  auto invalid_lease = invalid_current ? ava::session::SessionLease::create_and_acquire(invalid_current->session_path())
                                       : ava::core::Result<ava::session::SessionLease>(std::unexpected(invalid_current.error()));
  if (ava::tests::require(invalid_current && invalid_lease, "invalid fork fixture creates and leases current session"))
  {
    auto const start_id = ava::core::make_id("entry");
    auto started = invalid_current->append(*invalid_lease, session_start(start_id));
    std::string const session_id = fixture->source_store.options_r()->session_id;
    auto metadata = ava::session::make_session_metadata_entry({.parent_session_id = session_id,
                                                               .source_session_id = session_id,
                                                               .branch_from_entry_id = {},
                                                               .branch_origin = "fork",
                                                               .actor = "test"},
                                                              start_id);
    auto metadata_appended = metadata ? invalid_current->append(*invalid_lease, *metadata) : ava::core::VoidResult(std::unexpected(metadata.error()));
    auto authority = metadata_appended ? ava::session::SessionReadAuthority::create_persistent(*invalid_current, *invalid_lease, kReadLimits)
                                       : ava::core::Result<ava::session::SessionReadAuthority>(std::unexpected(metadata_appended.error()));
    if (ava::tests::require(started && metadata_appended && authority, "invalid fork fixture appends metadata and obtains read authority"))
    {
      auto request = fixture->request();
      request.current_session_id = invalid_current->options_r()->session_id;
      request.current_read_authority = *authority;
      auto terminal = preparation_terminal(std::move(request));
      expect(terminal.eligibility_code == ava::app::BranchSummaryEligibilityCode::InvalidFork,
             "a direct fork relation with no bounded fork entry ID fails with InvalidFork");
    }
  }
}

void test_generation_cancel_deadline_and_typed_failures()
{
  {
    auto fixture = make_fixture("cancel-generating");
    auto state = std::make_shared<BlockingGenerator>();
    auto coordinator = ava::app::BranchSummaryCoordinator::create();
    if (fixture && coordinator_ready(coordinator, "cancel generating"))
    {
      auto const before = ava::tests::read_session_test_binary_file(fixture->source_store.session_path());
      auto generation = prepare_awaiting(*coordinator, fixture->request(generator_for(state)), "cancel generating");
      if (generation && confirm_generation(*coordinator, *generation, "cancel generating") &&
          ava::tests::require(state->wait_started(), "cancel generating reaches the provider boundary"))
      {
        auto source_contender = ava::session::SessionLease::acquire(fixture->source_store.session_path());
        expect(!source_contender, "the exact source lease remains retained throughout provider generation");
        auto canceled = (*coordinator)->cancel(*generation);
        auto terminal = await_terminal(*coordinator, *generation);
        ava::tests::expect_all("generation cancellation propagates a stop token without mutating the source",
                               {{"cancellation accepted", canceled && *canceled},
                                {"canceled phase", terminal.phase == ava::app::BranchSummaryPhase::Canceled},
                                {"generator stopped", state->stop_seen},
                                {"no refresh", !terminal.refresh_required},
                                {"source unchanged", ava::tests::read_session_test_binary_file(fixture->source_store.session_path()) == before}});
      }
    }
  }
  {
    auto fixture = make_fixture("deadline");
    auto state = std::make_shared<BlockingGenerator>();
    auto coordinator = ava::app::BranchSummaryCoordinator::create({.operation_deadline = 40ms});
    if (fixture && coordinator_ready(coordinator, "deadline"))
    {
      auto const before = ava::tests::read_session_test_binary_file(fixture->source_store.session_path());
      auto generation = prepare_awaiting(*coordinator, fixture->request(generator_for(state)), "deadline");
      if (generation)
      {
        expect((*coordinator)->confirm(*generation).value_or(false), "deadline fixture confirms");
        auto terminal = await_terminal(*coordinator, *generation);
        ava::tests::expect_all("a confirmed operation expires with a typed nonmutating deadline result",
                               {{"failed phase", terminal.phase == ava::app::BranchSummaryPhase::Failed},
                                {"deadline code", terminal.failure_code == ava::app::BranchSummaryFailureCode::Deadline},
                                {"no refresh", !terminal.refresh_required},
                                {"source unchanged", ava::tests::read_session_test_binary_file(fixture->source_store.session_path()) == before}});
      }
    }
  }
  {
    auto fixture = make_fixture("provider-failure");
    auto coordinator = ava::app::BranchSummaryCoordinator::create();
    if (fixture && coordinator_ready(coordinator, "provider failure"))
    {
      auto provider_failure = [](ava::app::BranchSummaryGenerationPrompt const&, std::stop_token,
                                 std::chrono::steady_clock::time_point) -> ava::core::Result<std::string> {
        return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Provider, "raw provider payload must not escape"));
      };
      auto generation = prepare_awaiting(*coordinator, fixture->request(provider_failure), "provider failure");
      if (generation)
      {
        expect((*coordinator)->confirm(*generation).value_or(false), "provider failure fixture confirms");
        auto terminal = await_terminal(*coordinator, *generation);
        ava::tests::expect_all("provider failure becomes a payload-free typed result",
                               {{"provider failure code", terminal.failure_code == ava::app::BranchSummaryFailureCode::ProviderFailed},
                                {"raw payload absent", terminal.reason.find("raw provider") == std::string::npos}});
      }
    }
  }
  {
    auto fixture = make_fixture("auth-failure");
    auto coordinator = ava::app::BranchSummaryCoordinator::create();
    if (fixture && coordinator_ready(coordinator, "authentication failure"))
    {
      auto auth_failure = [](ava::app::BranchSummaryGenerationPrompt const&, std::stop_token,
                             std::chrono::steady_clock::time_point) -> ava::core::Result<std::string> {
        return std::unexpected(ava::core::Error(ava::core::ErrorCategory::PermissionDenied, "private token rejected"));
      };
      auto generation = prepare_awaiting(*coordinator, fixture->request(auth_failure), "authentication failure");
      if (generation)
      {
        expect((*coordinator)->confirm(*generation).value_or(false), "authentication failure fixture confirms");
        auto terminal = await_terminal(*coordinator, *generation);
        ava::tests::expect_all("authentication failure becomes a credential-free typed result",
                               {{"authentication code", terminal.failure_code == ava::app::BranchSummaryFailureCode::AuthenticationUnavailable},
                                {"private token absent", terminal.reason.find("private token") == std::string::npos}});
      }
    }
  }
  {
    auto fixture = make_fixture("model-failure");
    auto coordinator = ava::app::BranchSummaryCoordinator::create();
    if (fixture && coordinator_ready(coordinator, "model failure"))
    {
      auto state = std::make_shared<BlockingGenerator>();
      state->release = true;
      auto request = fixture->request(generator_for(state));
      request.selected_model.model_id = "missing-model";
      request.selected_model.api_family = "invalid-family";
      auto generation = prepare_awaiting(*coordinator, std::move(request), "model failure");
      if (generation)
      {
        expect((*coordinator)->confirm(*generation).value_or(false), "model failure fixture confirms");
        auto terminal = await_terminal(*coordinator, *generation);
        ava::tests::expect_all("invalid selected model fails before invoking an injected generator",
                               {{"model failure code", terminal.failure_code == ava::app::BranchSummaryFailureCode::ModelUnavailable},
                                {"generator not called", state->calls == 0}});
      }
    }
  }
}

void test_projection_and_recovery_failures_are_typed_before_provider_or_append()
{
  auto immediate = [](std::shared_ptr<BlockingGenerator> const& state) {
    state->release = true;
    return generator_for(state);
  };
  {
    auto fixture = make_fixture("projection-record-limit");
    if (fixture)
    {
      std::vector<ava::session::SessionEntry> additions;
      additions.reserve(4095);
      std::string parent = fixture->branch_tip_entry_id;
      for (std::size_t index = 0; index < 4095; ++index)
      {
        auto const id = ava::core::make_id("entry");
        additions.push_back(entry(id, parent, ava::session::EntryType::Error, R"({"message":"bounded ignored record"})"));
        parent = id;
      }
      expect(raw_append_all(fixture->source_store.session_path(), additions), "record-limit fixture appends its bounded candidate records");
      auto state = std::make_shared<BlockingGenerator>();
      auto terminal = confirmed_terminal(*fixture, immediate(state));
      expect(terminal.failure_code == ava::app::BranchSummaryFailureCode::ProjectionRecordLimit && state->calls == 0,
             "4097 candidate records fail with ProjectionRecordLimit before provider invocation");
    }
  }
  {
    auto fixture = make_fixture("projection-text-limit");
    if (fixture)
    {
      auto const id = ava::core::make_id("entry");
      expect(
          raw_append(fixture->source_store.session_path(), entry(id, fixture->branch_tip_entry_id, ava::session::EntryType::UserMessage,
                                                                 "{\"text\":\"" + std::string(ava::app::kMaxBranchSummaryProjectedTextBytes + 1, 'x') + "\"}")),
          "text-limit fixture appends one oversized projected text");
      auto state = std::make_shared<BlockingGenerator>();
      auto terminal = confirmed_terminal(*fixture, immediate(state));
      expect(terminal.failure_code == ava::app::BranchSummaryFailureCode::ProjectionTextLimit && state->calls == 0,
             "one projected text over 8 KiB fails with ProjectionTextLimit before provider invocation");
    }
  }
  {
    auto fixture = make_fixture("projection-byte-limit");
    if (fixture)
    {
      std::vector<ava::session::SessionEntry> additions;
      std::string parent = fixture->branch_tip_entry_id;
      for (std::size_t index = 0; index < 17; ++index)
      {
        auto const id = ava::core::make_id("entry");
        additions.push_back(
            entry(id, parent, ava::session::EntryType::UserMessage, "{\"text\":\"" + std::string(ava::app::kMaxBranchSummaryProjectedTextBytes, 'b') + "\"}"));
        parent = id;
      }
      expect(raw_append_all(fixture->source_store.session_path(), additions), "byte-limit fixture appends bounded individual messages");
      auto state = std::make_shared<BlockingGenerator>();
      auto terminal = confirmed_terminal(*fixture, immediate(state));
      expect(terminal.failure_code == ava::app::BranchSummaryFailureCode::ProjectionByteLimit && state->calls == 0,
             "a combined projection over 128 KiB fails with ProjectionByteLimit before provider invocation");
    }
  }
  {
    auto fixture = make_fixture("projection-invalid-text");
    if (fixture)
    {
      auto const id = ava::core::make_id("entry");
      expect(raw_append(fixture->source_store.session_path(),
                        entry(id, fixture->branch_tip_entry_id, ava::session::EntryType::UserMessage, R"({"text":"bad\u0080text"})")),
             "invalid-projection fixture appends schema-valid text with a forbidden control code point");
      auto state = std::make_shared<BlockingGenerator>();
      auto terminal = confirmed_terminal(*fixture, immediate(state));
      expect(terminal.failure_code == ava::app::BranchSummaryFailureCode::ProjectionInvalidText && state->calls == 0,
             "forbidden projected controls fail with ProjectionInvalidText before provider invocation");
    }
  }
  {
    auto fixture = make_fixture("projection-empty", false);
    if (fixture)
    {
      auto const id = ava::core::make_id("entry");
      expect(raw_append(fixture->source_store.session_path(),
                        entry(id, fixture->fork_entry_id, ava::session::EntryType::Error, R"({"message":"no projected text"})")),
             "empty-projection fixture appends one substantive nonprojected record");
      auto state = std::make_shared<BlockingGenerator>();
      auto terminal = confirmed_terminal(*fixture, immediate(state));
      expect(terminal.failure_code == ava::app::BranchSummaryFailureCode::ProjectionEmpty && state->calls == 0,
             "a substantive range with no admitted text fails with ProjectionEmpty before provider invocation");
    }
  }
  {
    auto fixture = make_fixture("invalid-generated-summary");
    if (fixture)
    {
      auto state = std::make_shared<BlockingGenerator>();
      state->output.assign(ava::app::kMaxGeneratedBranchSummaryBytes + 1, 's');
      auto terminal = confirmed_terminal(*fixture, immediate(state));
      expect(terminal.failure_code == ava::app::BranchSummaryFailureCode::InvalidGeneratedSummary && !terminal.refresh_required,
             "generated output over 8 KiB fails closed with InvalidGeneratedSummary and no append");
    }
  }
  {
    auto fixture = make_fixture("authentication-unavailable");
    if (fixture)
    {
      auto terminal = confirmed_terminal(*fixture, nullptr);
      expect(terminal.failure_code == ava::app::BranchSummaryFailureCode::AuthenticationUnavailable && !terminal.refresh_required,
             "missing credential authority fails deterministically with AuthenticationUnavailable before provider I/O");
    }
  }
  {
    auto fixture = make_fixture("recovery-failure");
    if (fixture)
    {
      auto state = std::make_shared<BlockingGenerator>();
      ava::app::BranchSummaryCoordinatorOptions options;
      auto const path = fixture->source_store.session_path();
      options.configure_source_store_for_test = [path](ava::session::SessionStore&) {
        std::ofstream out(path, std::ios::binary | std::ios::app);
        out << "{malformed-newline-terminated}\n";
      };
      auto terminal = confirmed_terminal(*fixture, immediate(state), std::move(options));
      expect(terminal.failure_code == ava::app::BranchSummaryFailureCode::RecoveryFailed && state->calls == 0,
             "newline-terminated corruption introduced before recovery fails with RecoveryFailed before provider invocation");
    }
  }
  {
    auto fixture = make_fixture("internal-setup-failure");
    if (fixture)
    {
      auto state = std::make_shared<BlockingGenerator>();
      ava::app::BranchSummaryCoordinatorOptions options;
      options.configure_source_store_for_test = [](ava::session::SessionStore&) { throw std::runtime_error("private test hook detail"); };
      auto terminal = confirmed_terminal(*fixture, immediate(state), std::move(options));
      expect(terminal.failure_code == ava::app::BranchSummaryFailureCode::Internal && terminal.reason.find("private test hook") == std::string::npos &&
                 state->calls == 0,
             "unexpected setup exceptions become a fixed payload-free Internal failure");
    }
  }
}

void test_confirmed_recovery_precedes_generation()
{
  {
    auto fixture = make_fixture("recover-torn-tail");
    if (fixture)
    {
      std::string const torn_suffix = "{\"incomplete\":";
      std::ofstream out(fixture->source_store.session_path(), std::ios::binary | std::ios::app);
      out << torn_suffix;
      out.close();
      auto const before = ava::tests::read_session_test_binary_file(fixture->source_store.session_path());
      auto state = std::make_shared<BlockingGenerator>();
      state->release = true;
      auto reservation_at_recovery = std::make_shared<std::atomic<bool>>(false);
      ava::app::BranchSummaryCoordinatorOptions options;
      options.configure_source_store_for_test = [reservation_at_recovery, controller = fixture->current_controller](ava::session::SessionStore&) {
        reservation_at_recovery->store(controller->snapshot().maintenance_reserved);
      };
      auto coordinator = ava::app::BranchSummaryCoordinator::create(std::move(options));
      if (!coordinator_ready(coordinator, "torn tail recovery"))
        return;
      auto generation = prepare_awaiting(*coordinator, fixture->request(generator_for(state)), "torn tail recovery");
      if (generation)
      {
        expect(ava::tests::read_session_test_binary_file(fixture->source_store.session_path()) == before && state->calls == 0,
               "read-only preparation tolerates but never repairs one recoverable invalid final suffix");
        expect((*coordinator)->confirm(*generation).value_or(false), "torn-tail recovery fixture confirms");
        auto terminal = await_terminal(*coordinator, *generation);
        std::string quarantined;
        for (auto const& item : std::filesystem::directory_iterator(fixture->source_store.session_path().parent_path()))
        {
          if (item.path().filename().string().find(".torn-tail.") != std::string::npos)
            quarantined = ava::tests::read_session_test_binary_file(item.path());
        }
        auto recovered = fixture->source_store.load_bounded(kReadLimits);
        expect(terminal.phase == ava::app::BranchSummaryPhase::Succeeded && reservation_at_recovery->load() && recovered &&
                   count_type(*recovered, ava::session::EntryType::BranchSummary) == 1 && quarantined == torn_suffix &&
                   ava::tests::read_session_test_binary_file(fixture->source_store.session_path()).find(torn_suffix) == std::string::npos,
               "confirmation performs lease-gated torn-tail quarantine before baseline generation and metadata append");
      }
    }
  }
  {
    auto fixture = make_fixture("recover-staged-output");
    if (fixture)
    {
      auto staged_data = ava::session::serialize_assistant_output_item_data_json(
          {.assistant_turn_id = "abandoned-staged-turn",
           .sequence = 0,
           .kind = ava::session::AssistantOutputItemKind::Text,
           .provider_item_id = std::nullopt,
           .provider_output_index = std::nullopt,
           .payload = ava::session::AssistantOutputText{.text = "never committed private staging",
                                                        .assistant_phase = ava::session::AssistantOutputTextPhase::FinalAnswer}});
      auto const staged_id = ava::core::make_id("entry");
      expect(staged_data && raw_append(fixture->source_store.session_path(),
                                       entry(staged_id, fixture->branch_tip_entry_id, ava::session::EntryType::AssistantOutputItem, staged_data.value_or(""))),
             "staged assistant recovery fixture appends one complete uncommitted v4 item");
      auto state = std::make_shared<BlockingGenerator>();
      state->release = true;
      auto coordinator = ava::app::BranchSummaryCoordinator::create();
      if (!coordinator_ready(coordinator, "staged output recovery"))
        return;
      auto generation = prepare_awaiting(*coordinator, fixture->request(generator_for(state)), "staged output recovery");
      if (generation)
      {
        expect((*coordinator)->confirm(*generation).value_or(false), "staged assistant recovery fixture confirms");
        auto terminal = await_terminal(*coordinator, *generation);
        auto recovered = fixture->source_store.load_bounded(kReadLimits);
        bool staged_absent = recovered && std::ranges::none_of(*recovered, [&](auto const& item) { return item.id == staged_id; });
        expect(terminal.phase == ava::app::BranchSummaryPhase::Succeeded && staged_absent &&
                   state->prompt.user_payload.find("never committed private staging") == std::string::npos,
               "confirmation removes a valid but uncommitted assistant-output suffix before fixing the exact projection baseline");
      }
    }
  }
}

struct PhaseBlocker
{
  explicit PhaseBlocker(ava::app::BranchSummaryPhase target_in) : target(target_in) { }

  void observe(ava::app::BranchSummarySnapshot const& snapshot)
  {
    if (snapshot.phase != target)
      return;
    std::unique_lock lock(mutex);
    reached = true;
    changed.notify_all();
    changed.wait(lock, [&] { return release; });
  }

  bool wait_reached()
  {
    std::unique_lock lock(mutex);
    return changed.wait_until(lock, ava::tests::now_plus_seconds(5), [&] { return reached; });
  }

  void allow()
  {
    std::lock_guard lock(mutex);
    release = true;
    changed.notify_all();
  }

  ava::app::BranchSummaryPhase target;
  std::mutex mutex;
  std::condition_variable changed;
  bool reached = false;
  bool release = false;
};

struct ReleasePhaseOnExit
{
  std::shared_ptr<PhaseBlocker> blocker;
  ~ReleasePhaseOnExit() { blocker->allow(); }
};

void test_active_run_checks_at_confirmation_and_preappend()
{
  {
    auto fixture = make_fixture("active-at-confirm");
    auto coordinator = ava::app::BranchSummaryCoordinator::create();
    if (fixture && coordinator_ready(coordinator, "active at confirmation"))
    {
      auto state = std::make_shared<BlockingGenerator>();
      state->release = true;
      auto generation = prepare_awaiting(*coordinator, fixture->request(generator_for(state)), "active at confirmation");
      if (generation)
      {
        auto guard = fixture->current_controller->admit({.request_id = "confirm-conflict"});
        if (!ava::tests::require(guard.has_value(), "confirmation conflict admits a normal run") ||
            !confirm_generation(*coordinator, *generation, "active at confirmation"))
          return;
        auto terminal = await_terminal(*coordinator, *generation);
        ava::tests::expect_all("confirmation rechecks active normal-run ownership before provider work",
                               {{"ineligible phase", terminal.phase == ava::app::BranchSummaryPhase::Ineligible},
                                {"active-run code", terminal.eligibility_code == ava::app::BranchSummaryEligibilityCode::ActiveRun},
                                {"generator not called", state->calls == 0}});
      }
    }
  }
  {
    auto fixture = make_fixture("maintenance-at-confirm");
    auto coordinator = ava::app::BranchSummaryCoordinator::create();
    if (fixture && coordinator_ready(coordinator, "maintenance at confirmation"))
    {
      auto state = std::make_shared<BlockingGenerator>();
      state->release = true;
      auto generation = prepare_awaiting(*coordinator, fixture->request(generator_for(state)), "maintenance at confirmation");
      if (generation)
      {
        auto reservation = fixture->current_controller->reserve_maintenance();
        if (!ava::tests::require(reservation.has_value(), "confirmation conflict reserves independent maintenance") ||
            !confirm_generation(*coordinator, *generation, "maintenance at confirmation"))
          return;
        auto terminal = await_terminal(*coordinator, *generation);
        ava::tests::expect_all("maintenance conflict rejects confirmation without active-run attribution",
                               {{"ineligible phase", terminal.phase == ava::app::BranchSummaryPhase::Ineligible},
                                {"current unavailable code", terminal.eligibility_code == ava::app::BranchSummaryEligibilityCode::CurrentSessionUnavailable},
                                {"generator not called", state->calls == 0}});
      }
    }
  }
  {
    auto fixture = make_fixture("active-at-preappend");
    auto coordinator = ava::app::BranchSummaryCoordinator::create();
    if (fixture && coordinator_ready(coordinator, "active at preappend"))
    {
      auto state = std::make_shared<BlockingGenerator>();
      auto generation = prepare_awaiting(*coordinator, fixture->request(generator_for(state)), "active at preappend");
      if (generation && confirm_generation(*coordinator, *generation, "active at preappend") &&
          ava::tests::require(state->wait_started(), "preappend generator starts"))
      {
        auto guard = fixture->current_controller->admit({.request_id = "preappend-conflict"});
        auto const reserved = fixture->current_controller->snapshot().maintenance_reserved;
        state->allow();
        auto terminal = await_terminal(*coordinator, *generation);
        (*coordinator)->shutdown();
        ava::tests::expect_all("exclusive maintenance rejects normal work until generation terminates",
                               {{"normal run rejected", !guard},
                                {"reserved during generation", reserved},
                                {"summary succeeded", terminal.phase == ava::app::BranchSummaryPhase::Succeeded},
                                {"reservation released", !fixture->current_controller->snapshot().maintenance_reserved}});
      }
    }
  }
  {
    auto fixture = make_fixture("active-at-append-linearization");
    auto blocker = std::make_shared<PhaseBlocker>(ava::app::BranchSummaryPhase::Appending);
    auto coordinator = ava::app::BranchSummaryCoordinator::create({.operation_deadline = 30s,
                                                                   .on_snapshot = [blocker](auto const& snapshot) { blocker->observe(snapshot); },
                                                                   .configure_source_store_for_test = nullptr});
    ReleasePhaseOnExit release_phase{blocker};
    if (fixture && coordinator_ready(coordinator, "append linearization"))
    {
      auto generator = [](ava::app::BranchSummaryGenerationPrompt const&, std::stop_token,
                          std::chrono::steady_clock::time_point) -> ava::core::Result<std::string> { return "append linearization summary"; };
      auto generation = prepare_awaiting(*coordinator, fixture->request(generator), "append linearization");
      if (generation && confirm_generation(*coordinator, *generation, "append linearization") &&
          ava::tests::require(blocker->wait_reached(), "append linearization reaches its phase barrier"))
      {
        auto guard = fixture->current_controller->admit({.request_id = "append-linearization-conflict"});
        blocker->allow();
        auto terminal = await_terminal(*coordinator, *generation);
        auto entries = fixture->source_store.load_bounded(kReadLimits);
        ava::tests::expect_all("exclusive maintenance closes final normal-run admission while appending",
                               {{"normal run rejected", !guard},
                                {"summary succeeded", terminal.phase == ava::app::BranchSummaryPhase::Succeeded},
                                {"one durable summary", entries && count_type(*entries, ava::session::EntryType::BranchSummary) == 1}});
      }
    }
  }
}

void test_confirmation_binds_exact_source_fingerprint()
{
  {
    auto fixture = make_fixture("mutated-while-awaiting");
    auto state = std::make_shared<BlockingGenerator>();
    auto setup_calls = std::make_shared<std::atomic<int>>(0);
    ava::app::BranchSummaryCoordinatorOptions options;
    options.configure_source_store_for_test = [setup_calls](ava::session::SessionStore&) { setup_calls->fetch_add(1); };
    auto coordinator = ava::app::BranchSummaryCoordinator::create(std::move(options));
    if (fixture && coordinator_ready(coordinator, "mutated while awaiting"))
    {
      auto generation = prepare_awaiting(*coordinator, fixture->request(generator_for(state)), "mutated while awaiting");
      if (generation)
      {
        {
          auto contender = ava::session::SessionLease::acquire(fixture->source_store.session_path());
          expect(contender.has_value(), contender ? "awaiting consent retains no parent lease" : contender.error().format());
          std::ofstream out(fixture->source_store.session_path(), std::ios::binary | std::ios::app);
          out << R"({"unterminated":"changed after consent snapshot")";
          out.flush();
          expect(out.good(), "leased contender mutates an unterminated suffix while consent is pending");
        }
        auto const mutated = ava::tests::read_session_test_binary_file(fixture->source_store.session_path());
        expect((*coordinator)->confirm(*generation).value_or(false), "mutated-source fixture confirms after the contender releases its lease");
        auto terminal = await_terminal(*coordinator, *generation);
        auto const after = ava::tests::read_session_test_binary_file(fixture->source_store.session_path());
        ava::tests::expect_all("changed exact bytes fail before recovery setup, provider work, or append",
                               {{"stale-source code", terminal.failure_code == ava::app::BranchSummaryFailureCode::StaleSource},
                                {"generator not called", state->calls == 0},
                                {"recovery not configured", setup_calls->load() == 0},
                                {"source unchanged", after == mutated}});
      }
    }
  }
  {
    auto fixture = make_fixture("same-bytes-replaced-while-awaiting");
    auto state = std::make_shared<BlockingGenerator>();
    state->release = true;
    auto coordinator = ava::app::BranchSummaryCoordinator::create();
    if (fixture && coordinator_ready(coordinator, "same bytes replaced"))
    {
      auto generation = prepare_awaiting(*coordinator, fixture->request(generator_for(state)), "same bytes replaced");
      if (generation)
      {
        auto const bytes = ava::tests::read_session_test_binary_file(fixture->source_store.session_path());
        {
          auto contender = ava::session::SessionLease::acquire(fixture->source_store.session_path());
          expect(contender.has_value(), contender ? "same-byte replacement acquires the released consent lease" : contender.error().format());
          auto const old_path = fixture->source_store.session_path().string() + ".consent-old";
          std::filesystem::rename(fixture->source_store.session_path(), old_path);
          ava::tests::write_session_test_binary_file(fixture->source_store.session_path(), bytes);
        }
        expect((*coordinator)->confirm(*generation).value_or(false), "same-byte replacement fixture confirms");
        auto terminal = await_terminal(*coordinator, *generation);
        ava::tests::expect_all(
            "identical bounded bytes and coverage survive exact-path inode replacement",
            {{"summary succeeded", terminal.phase == ava::app::BranchSummaryPhase::Succeeded}, {"generator called once", state->calls == 1}});
      }
    }
  }
  {
    auto fixture = make_fixture("changed-bytes-replaced-while-awaiting");
    auto state = std::make_shared<BlockingGenerator>();
    auto coordinator = ava::app::BranchSummaryCoordinator::create();
    if (fixture && coordinator_ready(coordinator, "changed bytes replaced"))
    {
      auto generation = prepare_awaiting(*coordinator, fixture->request(generator_for(state)), "changed bytes replaced");
      if (generation)
      {
        auto bytes = ava::tests::read_session_test_binary_file(fixture->source_store.session_path());
        auto const timestamp = bytes.find("2026-05-01T00:00:00Z");
        expect(timestamp != std::string::npos, "changed replacement locates a non-identity timestamp byte");
        if (timestamp != std::string::npos)
          bytes[timestamp + 9] = '2';
        auto const old_path = fixture->source_store.session_path().string() + ".changed-consent-old";
        std::filesystem::rename(fixture->source_store.session_path(), old_path);
        ava::tests::write_session_test_binary_file(fixture->source_store.session_path(), bytes);
        expect((*coordinator)->confirm(*generation).value_or(false), "changed replacement fixture confirms");
        auto terminal = await_terminal(*coordinator, *generation);
        ava::tests::expect_all(
            "one changed replacement byte fails before generation",
            {{"stale-source code", terminal.failure_code == ava::app::BranchSummaryFailureCode::StaleSource}, {"generator not called", state->calls == 0}});
      }
    }
  }
}

void test_coordinator_enforces_hard_read_policy()
{
  {
    auto fixture = make_fixture("legacy-unbounded-request");
    auto state = std::make_shared<BlockingGenerator>();
    auto coordinator = ava::app::BranchSummaryCoordinator::create();
    if (fixture && coordinator_ready(coordinator, "hard entry policy"))
    {
      std::vector<ava::session::SessionEntry> additions;
      additions.reserve(ava::app::kBranchSummaryHardReadLimits.max_entries);
      std::string parent = fixture->branch_tip_entry_id;
      for (std::size_t index = 0; index < ava::app::kBranchSummaryHardReadLimits.max_entries; ++index)
      {
        auto const id = "legacy-hard-limit-" + std::to_string(index);
        additions.push_back(entry(id, parent, ava::session::EntryType::Error, R"({"message":"bounded hard-limit fixture"})"));
        parent = id;
      }
      bool const appended = raw_append_all(fixture->source_store.session_path(), additions);
      expect(appended, "legacy-unbounded fixture appends more records than the coordinator hard cap");
      if (!appended)
        return;
      auto request = fixture->request(generator_for(state));
      request.read_limits = ava::session::legacy_unbounded_session_read_limits();
      auto generation = prepare_checked(*coordinator, std::move(request), "hard entry policy");
      if (generation)
      {
        // Parsing the full entry-count boundary is intentionally expensive under TSan;
        // keep the ordinary helper deadline strict while allowing this instrumentation case.
        auto terminal = await_terminal(*coordinator, *generation, 20s);
        ava::tests::expect_all("legacy-unbounded request is clamped to the immutable entry-count hard limit",
                               {{"ineligible phase", terminal.phase == ava::app::BranchSummaryPhase::Ineligible},
                                {"source corrupt code", terminal.eligibility_code == ava::app::BranchSummaryEligibilityCode::SourceCorrupt},
                                {"generator not called", state->calls == 0}});
      }
    }
  }
  {
    auto fixture = make_fixture("legacy-unbounded-file-request");
    auto state = std::make_shared<BlockingGenerator>();
    auto coordinator = ava::app::BranchSummaryCoordinator::create();
    if (fixture && coordinator_ready(coordinator, "hard file policy"))
    {
      std::vector<ava::session::SessionEntry> additions;
      additions.reserve(9000);
      std::string parent = fixture->branch_tip_entry_id;
      for (std::size_t index = 0; index < 9000; ++index)
      {
        auto const id = "legacy-hard-file-" + std::to_string(index);
        additions.push_back(entry(id, parent, ava::session::EntryType::Error, "{\"message\":\"" + std::string(1000, 'f') + "\"}"));
        parent = id;
      }
      bool const appended = raw_append_all(fixture->source_store.session_path(), additions);
      expect(appended && std::filesystem::file_size(fixture->source_store.session_path()) > ava::app::kBranchSummaryHardReadLimits.max_file_bytes,
             "legacy-unbounded fixture creates a valid-record file beyond the coordinator byte cap");
      if (!appended)
        return;
      auto request = fixture->request(generator_for(state));
      request.read_limits = ava::session::legacy_unbounded_session_read_limits();
      auto generation = prepare_checked(*coordinator, std::move(request), "hard file policy");
      if (generation)
      {
        auto terminal = await_terminal(*coordinator, *generation);
        ava::tests::expect_all("legacy-unbounded request is clamped to the immutable 8 MiB file cap",
                               {{"ineligible phase", terminal.phase == ava::app::BranchSummaryPhase::Ineligible},
                                {"source corrupt code", terminal.eligibility_code == ava::app::BranchSummaryEligibilityCode::SourceCorrupt},
                                {"generator not called", state->calls == 0}});
      }
    }
  }
  {
    auto fixture = make_fixture("legacy-unbounded-line-request");
    auto state = std::make_shared<BlockingGenerator>();
    auto coordinator = ava::app::BranchSummaryCoordinator::create();
    if (fixture && coordinator_ready(coordinator, "hard line policy"))
    {
      auto oversized_line = ava::session::serialize_session_entry_line(
          entry("legacy-hard-line", fixture->branch_tip_entry_id, ava::session::EntryType::Error, R"({"message":"line limit"})"));
      bool appended = false;
      if (oversized_line)
      {
        oversized_line->insert(oversized_line->size() - 1, ava::app::kBranchSummaryHardReadLimits.max_line_bytes, ' ');
        std::ofstream out(fixture->source_store.session_path(), std::ios::binary | std::ios::app);
        out << *oversized_line << '\n';
        out.flush();
        appended = out.good();
      }
      expect(appended && std::filesystem::file_size(fixture->source_store.session_path()) < ava::app::kBranchSummaryHardReadLimits.max_file_bytes,
             "legacy-unbounded fixture creates one valid JSON record beyond only the coordinator line cap");
      if (!appended)
        return;
      auto request = fixture->request(generator_for(state));
      request.read_limits = ava::session::legacy_unbounded_session_read_limits();
      auto generation = prepare_checked(*coordinator, std::move(request), "hard line policy");
      if (generation)
      {
        auto terminal = await_terminal(*coordinator, *generation);
        ava::tests::expect_all("legacy-unbounded request is clamped to the immutable 1 MiB line cap",
                               {{"ineligible phase", terminal.phase == ava::app::BranchSummaryPhase::Ineligible},
                                {"source corrupt code", terminal.eligibility_code == ava::app::BranchSummaryEligibilityCode::SourceCorrupt},
                                {"generator not called", state->calls == 0}});
      }
    }
  }
  {
    auto fixture = make_fixture("caller-read-restriction");
    auto coordinator = ava::app::BranchSummaryCoordinator::create();
    if (fixture && coordinator_ready(coordinator, "caller read restriction"))
    {
      auto request = fixture->request();
      request.read_limits.max_entries = 2;
      auto generation = prepare_checked(*coordinator, std::move(request), "caller read restriction");
      if (generation)
      {
        auto terminal = await_terminal(*coordinator, *generation);
        expect(terminal.eligibility_code == ava::app::BranchSummaryEligibilityCode::CurrentSessionUnavailable,
               "a stricter caller entry limit remains authoritative for the current relation and source after hard-policy intersection (phase=" +
                   std::string(ava::app::to_string(terminal.phase)) + ", reason=" + terminal.reason + ")");
      }

      auto invalid = fixture->request();
      invalid.read_limits.max_file_bytes = 0;
      expect(!(*coordinator)->prepare(std::move(invalid)).has_value(), "zero-valued direct read limits reject synchronously");
    }
  }
}

void test_stale_source_relation_identity_and_final_existing()
{
  {
    auto fixture = make_fixture("stale-tip");
    auto state = std::make_shared<BlockingGenerator>();
    auto coordinator = ava::app::BranchSummaryCoordinator::create();
    if (fixture && coordinator_ready(coordinator, "stale tip"))
    {
      auto generation = prepare_awaiting(*coordinator, fixture->request(generator_for(state)), "stale tip");
      if (generation && confirm_generation(*coordinator, *generation, "stale tip") && ava::tests::require(state->wait_started(), "stale tip generation starts"))
      {
        auto const changed_id = ava::core::make_id("entry");
        expect(raw_append(fixture->source_store.session_path(),
                          entry(changed_id, fixture->branch_tip_entry_id, ava::session::EntryType::Error, R"({"message":"unrelated marker"})")),
               "stale-tip fixture bypass-appends an unrelated valid marker");
        state->allow();
        auto terminal = await_terminal(*coordinator, *generation);
        ava::tests::expect_all(
            "an unrelated source marker changes the recovery baseline and fails stale",
            {{"stale-source code", terminal.failure_code == ava::app::BranchSummaryFailureCode::StaleSource}, {"no refresh", !terminal.refresh_required}});
      }
    }
  }
  {
    auto fixture = make_fixture("stale-relation");
    auto state = std::make_shared<BlockingGenerator>();
    auto coordinator = ava::app::BranchSummaryCoordinator::create();
    if (fixture && coordinator_ready(coordinator, "stale relation"))
    {
      auto generation = prepare_awaiting(*coordinator, fixture->request(generator_for(state)), "stale relation");
      if (generation)
      {
        auto current_entries = fixture->current_authority.load();
        auto metadata = current_entries ? ava::session::make_session_metadata_entry({.parent_session_id = "changed-source",
                                                                                     .source_session_id = "changed-source",
                                                                                     .branch_from_entry_id = fixture->fork_entry_id,
                                                                                     .branch_origin = "fork",
                                                                                     .actor = "test"},
                                                                                    current_entries->back().id)
                                        : ava::core::Result<ava::session::SessionEntry>(std::unexpected(current_entries.error()));
        auto appended = metadata ? fixture->current_controller->append(*metadata) : ava::core::VoidResult(std::unexpected(metadata.error()));
        expect(appended.has_value(), appended ? "stale relation metadata appends before confirmation" : appended.error().format());
        expect((*coordinator)->confirm(*generation).value_or(false), "changed relation fixture confirms after mutation");
        auto terminal = await_terminal(*coordinator, *generation);
        ava::tests::expect_all(
            "a current relation mutation while awaiting consent fails before provider work",
            {{"stale-source code", terminal.failure_code == ava::app::BranchSummaryFailureCode::StaleSource}, {"generator not called", state->calls == 0}});
      }
    }
  }
  {
    auto fixture = make_fixture("stale-inode");
    auto state = std::make_shared<BlockingGenerator>();
    auto coordinator = ava::app::BranchSummaryCoordinator::create();
    if (fixture && coordinator_ready(coordinator, "stale inode"))
    {
      auto generation = prepare_awaiting(*coordinator, fixture->request(generator_for(state)), "stale inode");
      if (generation && confirm_generation(*coordinator, *generation, "stale inode") &&
          ava::tests::require(state->wait_started(), "stale inode generation starts"))
      {
        auto const bytes = ava::tests::read_session_test_binary_file(fixture->source_store.session_path());
        auto const moved = fixture->source_store.session_path().string() + ".old";
        std::filesystem::rename(fixture->source_store.session_path(), moved);
        ava::tests::write_session_test_binary_file(fixture->source_store.session_path(), bytes);
        state->allow();
        auto terminal = await_terminal(*coordinator, *generation);
        expect(terminal.failure_code == ava::app::BranchSummaryFailureCode::StaleSource, "same-byte path replacement fails exact retained-inode revalidation");
      }
    }
  }
  {
    auto fixture = make_fixture("final-existing");
    auto state = std::make_shared<BlockingGenerator>();
    auto coordinator = ava::app::BranchSummaryCoordinator::create();
    if (fixture && coordinator_ready(coordinator, "final existing"))
    {
      auto generation = prepare_awaiting(*coordinator, fixture->request(generator_for(state)), "final existing");
      if (generation && confirm_generation(*coordinator, *generation, "final existing") &&
          ava::tests::require(state->wait_started(), "final existing generation starts"))
      {
        auto const marker_id = ava::core::make_id("entry");
        std::string data = "{\"schema_version\":1,\"summary\":\"won elsewhere\",\"source_session_id\":\"" +
                           ava::core::json::escape(fixture->source_store.options_r()->session_id) + "\",\"branch_root_entry_id\":\"" +
                           ava::core::json::escape(fixture->branch_root_entry_id) + "\",\"branch_tip_entry_id\":\"" +
                           ava::core::json::escape(fixture->branch_tip_entry_id) +
                           "\",\"provider\":\"openai\",\"model\":\"gpt-5.5\",\"reason\":\"abandoned_parent\",\"actor\":\"tui\"}";
        expect(raw_append(fixture->source_store.session_path(),
                          entry(marker_id, fixture->branch_tip_entry_id, ava::session::EntryType::BranchSummary, std::move(data))),
               "final-existing fixture bypass-appends the exact competing summary");
        state->allow();
        auto terminal = await_terminal(*coordinator, *generation);
        ava::tests::expect_all("an exact competing summary wins over generic baseline staleness",
                               {{"existing phase", terminal.phase == ava::app::BranchSummaryPhase::Existing}, {"refresh required", terminal.refresh_required}});
      }
    }
  }
}

void test_matching_summary_is_existing_only_for_exact_final_coverage()
{
  auto fixture = make_fixture("nonfinal-matching-summary");
  auto state = std::make_shared<BlockingGenerator>();
  auto coordinator = ava::app::BranchSummaryCoordinator::create();
  if (!fixture || !coordinator_ready(coordinator, "nonfinal existing"))
    return;
  auto generation = prepare_awaiting(*coordinator, fixture->request(generator_for(state)), "nonfinal existing");
  if (!generation || !confirm_generation(*coordinator, *generation, "nonfinal existing") ||
      !ava::tests::require(state->wait_started(), "nonfinal existing generation starts"))
    return;
  auto const marker_id = ava::core::make_id("entry");
  std::string data = "{\"schema_version\":1,\"summary\":\"nonfinal competing summary\",\"source_session_id\":\"" +
                     ava::core::json::escape(fixture->source_store.options_r()->session_id) + "\",\"branch_root_entry_id\":\"" +
                     ava::core::json::escape(fixture->branch_root_entry_id) + "\",\"branch_tip_entry_id\":\"" +
                     ava::core::json::escape(fixture->branch_tip_entry_id) +
                     "\",\"provider\":\"openai\",\"model\":\"gpt-5.5\",\"reason\":\"abandoned_parent\",\"actor\":\"tui\"}";
  auto const later_id = ava::core::make_id("entry");
  expect(raw_append(fixture->source_store.session_path(),
                    entry(marker_id, fixture->branch_tip_entry_id, ava::session::EntryType::BranchSummary, std::move(data))) &&
             raw_append(fixture->source_store.session_path(),
                        entry(later_id, marker_id, ava::session::EntryType::Error, R"({"message":"later substantive source change"})")),
         "nonfinal existing fixture appends a matching marker followed by a substantive record");
  state->allow();
  auto terminal = await_terminal(*coordinator, *generation);
  expect(terminal.phase == ava::app::BranchSummaryPhase::Failed && terminal.failure_code == ava::app::BranchSummaryFailureCode::StaleSource &&
             !terminal.refresh_required,
         "a matching marker followed by changed final coverage is stale, never authoritative Existing");
}

void test_append_commit_states_are_terminal_and_never_retried()
{
  enum class Fault
  {
    NotStarted,
    Partial,
    Committed,
  };
  for (auto const fault : {Fault::NotStarted, Fault::Partial, Fault::Committed})
  {
    auto fixture = make_fixture(fault == Fault::NotStarted ? "append-not-started" : fault == Fault::Partial ? "append-partial" : "append-committed");
    if (!fixture)
      continue;
    auto writes = std::make_shared<std::atomic<int>>(0);
    auto const source_path = fixture->source_store.session_path();
    ava::app::BranchSummaryCoordinatorOptions options;
    options.configure_source_store_for_test = [fault, writes, source_path](ava::session::SessionStore& source) {
      if (fault == Fault::NotStarted)
      {
        source.set_append_write_for_test([](int, std::string_view) -> ssize_t {
          errno = EIO;
          return -1;
        });
      }
      else if (fault == Fault::Partial)
      {
        source.set_append_write_for_test([writes](int fd, std::string_view bytes) -> ssize_t {
          if (writes->fetch_add(1) == 0)
            return ::write(fd, bytes.data(), std::min<std::size_t>(7, bytes.size()));
          errno = EIO;
          return -1;
        });
      }
      else
      {
        source.set_after_append_write_for_test([source_path] { std::filesystem::rename(source_path, source_path.string() + ".committed-to-leased-inode"); });
      }
    };
    auto coordinator = ava::app::BranchSummaryCoordinator::create(std::move(options));
    if (!coordinator_ready(coordinator, "append fault"))
      continue;
    auto state = std::make_shared<BlockingGenerator>();
    state->release = true;
    auto generation = prepare_awaiting(*coordinator, fixture->request(generator_for(state)), "append fault");
    if (!generation)
      continue;
    expect((*coordinator)->confirm(*generation).value_or(false), "append fault fixture confirms");
    auto terminal = await_terminal(*coordinator, *generation);
    auto const expected_state = fault == Fault::NotStarted ? "not_started" : fault == Fault::Partial ? "partial_or_unknown" : "committed_to_leased_inode";
    expect(terminal.phase == ava::app::BranchSummaryPhase::Failed && terminal.failure_code == ava::app::BranchSummaryFailureCode::AppendFailed &&
               terminal.append_commit_state == expected_state && !terminal.refresh_required && state->calls == 1,
           "append commit state " + std::string(expected_state) + " is exposed once without retry or optimistic refresh");
    (*coordinator)->shutdown();
  }
}

struct ProviderTransportState
{
  std::mutex mutex;
  std::size_t sends = 0;
  std::vector<ava::http::HttpRequest> requests;
  std::string response_body =
      R"({"status":"completed","output":[{"id":"msg_summary","type":"message","phase":"final_answer","content":[{"type":"output_text","text":"Direct isolated parent context"}]}]})";
};

class ProviderTransport final : public ava::http::Transport
{
 public:
  explicit ProviderTransport(std::shared_ptr<ProviderTransportState> state) : state_(std::move(state)) { }

  ava::core::Result<ava::http::HttpResponse> send(ava::http::HttpRequest const& request) override
  {
    std::lock_guard lock(state_->mutex);
    ++state_->sends;
    state_->requests.push_back(request);
    return ava::http::HttpResponse{.status_code = 200, .headers = {}, .body = state_->response_body};
  }

 private:
  std::shared_ptr<ProviderTransportState> state_;
};

class ThrowingProviderTransport final : public ava::http::Transport
{
 public:
  ava::core::Result<ava::http::HttpResponse> send(ava::http::HttpRequest const&) override { throw std::runtime_error("transport secret must not escape"); }
};

void test_production_provider_request_is_metadata_only()
{
  auto fixture = make_fixture("direct-provider");
  if (!fixture)
    return;
  auto paths = ava::tests::app_test_paths(fixture->root / "xdg");
  auto anchors = ava::core::AnchorSet::open({fixture->workspace, paths.ava_config_dir, paths.ava_state_dir});
  expect(anchors.has_value(), anchors ? "direct provider fixture opens exact anchors" : anchors.error().format());
  if (!anchors)
    return;
  auto state = std::make_shared<ProviderTransportState>();
  auto request = fixture->request();
  request.paths = paths;
  request.anchor_set = *anchors;
  request.provider_options.access_token = "direct-fake-token";
  request.provider_options.transport_factory = [state] { return std::make_unique<ProviderTransport>(state); };
  auto coordinator = ava::app::BranchSummaryCoordinator::create();
  if (!coordinator_ready(coordinator, "direct provider"))
    return;
  auto generation = prepare_awaiting(*coordinator, std::move(request), "direct provider");
  if (!generation)
    return;
  expect((*coordinator)->confirm(*generation).value_or(false), "direct provider branch summary confirms");
  auto terminal = await_terminal(*coordinator, *generation);
  bool isolated = false;
  std::string captured_body;
  {
    std::lock_guard lock(state->mutex);
    if (!state->requests.empty())
      captured_body = state->requests.front().body;
    isolated = state->sends == 1 && state->requests.size() == 1 && state->requests.front().timeout_ms > 0 && state->requests.front().timeout_ms <= 30000 &&
               state->requests.front().body.find("Summarize only the supplied abandoned") != std::string::npos &&
               state->requests.front().body.find("implement the abandoned parent approach") != std::string::npos &&
               state->requests.front().body.find("\"max_output_tokens\":2048") != std::string::npos &&
               state->requests.front().body.find("\"tools\":[]") != std::string::npos &&
               state->requests.front().body.find("\"reasoning\":") == std::string::npos &&
               state->requests.front().body.find("/private/path") == std::string::npos;
  }
  expect(terminal.phase == ava::app::BranchSummaryPhase::Succeeded, "production branch summary succeeds through the isolated provider path (phase=" +
                                                                        std::string(ava::app::to_string(terminal.phase)) + ", reason=" + terminal.reason + ")");
  expect(isolated,
         "production generation performs one endpoint-aware, deadline-bounded, 2048-token, tool-free and context-free provider request: " + captured_body);
  (*coordinator)->shutdown();

  auto oauth_fixture = make_fixture("direct-provider-oauth");
  if (oauth_fixture)
  {
    auto oauth_paths = ava::tests::app_test_paths(oauth_fixture->root / "xdg");
    auto oauth_anchors = ava::core::AnchorSet::open({oauth_fixture->workspace, oauth_paths.ava_config_dir, oauth_paths.ava_state_dir});
    auto oauth_state = std::make_shared<ProviderTransportState>();
    if (ava::tests::require(oauth_anchors.has_value(), "OAuth provider opens fixture anchors"))
    {
      auto oauth_request = oauth_fixture->request();
      oauth_request.paths = oauth_paths;
      oauth_request.anchor_set = *oauth_anchors;
      oauth_request.selected_model.supports_streaming = false;
      oauth_request.provider_options.access_token = "direct-oauth-token";
      oauth_request.provider_options.credential_type = "oauth";
      oauth_request.provider_options.openai_oauth = true;
      oauth_request.provider_options.account_id = "oauth-account";
      oauth_request.provider_options.transport_factory = [oauth_state] { return std::make_unique<ProviderTransport>(oauth_state); };
      auto oauth_coordinator = ava::app::BranchSummaryCoordinator::create();
      if (!coordinator_ready(oauth_coordinator, "OAuth provider"))
        return;
      auto oauth_generation = prepare_awaiting(*oauth_coordinator, std::move(oauth_request), "OAuth provider");
      if (oauth_generation)
      {
        expect((*oauth_coordinator)->confirm(*oauth_generation).value_or(false), "OAuth provider fixture confirms");
        auto oauth_terminal = await_terminal(*oauth_coordinator, *oauth_generation);
        bool oauth_contract = false;
        {
          std::lock_guard lock(oauth_state->mutex);
          if (oauth_state->requests.size() == 1)
          {
            auto const& sent = oauth_state->requests.front();
            oauth_contract = sent.url == "https://chatgpt.com/backend-api/codex/responses" && sent.timeout_ms > 0 && sent.timeout_ms <= 30000 &&
                             sent.body.find("\"max_output_tokens\"") == std::string::npos && sent.headers.contains("Authorization") &&
                             sent.headers.contains("ChatGPT-Account-Id") && sent.headers.contains("chatgpt-account-id");
          }
        }
        expect(oauth_terminal.phase == ava::app::BranchSummaryPhase::Succeeded && oauth_contract,
               "delegated Codex OAuth remains accepted and intentionally omits the unsupported max_output_tokens field");
      }
    }
  }

  for (int const failure : {0, 1, 2})
  {
    auto exception_fixture = make_fixture("direct-provider-exception-" + std::to_string(failure));
    if (!exception_fixture)
      continue;
    auto exception_paths = ava::tests::app_test_paths(exception_fixture->root / "xdg");
    auto exception_anchors = ava::core::AnchorSet::open({exception_fixture->workspace, exception_paths.ava_config_dir, exception_paths.ava_state_dir});
    if (!ava::tests::require(exception_anchors.has_value(), "provider exception opens fixture anchors"))
      continue;
    auto exception_request = exception_fixture->request();
    exception_request.paths = exception_paths;
    exception_request.anchor_set = *exception_anchors;
    exception_request.provider_options.access_token = "throw-path-secret";
    if (failure == 0)
      exception_request.provider_options.transport_factory = []() -> std::unique_ptr<ava::http::Transport> { throw std::runtime_error("factory secret"); };
    else if (failure == 1)
      exception_request.provider_options.transport_factory = []() -> std::unique_ptr<ava::http::Transport> { return nullptr; };
    else
      exception_request.provider_options.transport_factory = [] { return std::make_unique<ThrowingProviderTransport>(); };
    auto exception_coordinator = ava::app::BranchSummaryCoordinator::create();
    if (!coordinator_ready(exception_coordinator, "provider exception"))
      continue;
    auto exception_generation = prepare_awaiting(*exception_coordinator, std::move(exception_request), "provider exception");
    if (!exception_generation)
      continue;
    expect((*exception_coordinator)->confirm(*exception_generation).value_or(false), "provider exception fixture confirms");
    auto exception_terminal = await_terminal(*exception_coordinator, *exception_generation);
    expect(
        exception_terminal.failure_code == ava::app::BranchSummaryFailureCode::ProviderFailed && exception_terminal.reason.find("secret") == std::string::npos,
        "transport factory null/throw and send throw are contained as fixed provider failures without exception payloads");
  }

  auto oversized_fixture = make_fixture("direct-provider-raw-cap");
  if (oversized_fixture)
  {
    auto oversized_paths = ava::tests::app_test_paths(oversized_fixture->root / "xdg");
    auto oversized_anchors = ava::core::AnchorSet::open({oversized_fixture->workspace, oversized_paths.ava_config_dir, oversized_paths.ava_state_dir});
    auto oversized_state = std::make_shared<ProviderTransportState>();
    oversized_state->response_body.assign(ava::app::kMaxBranchSummaryRawResponseBytes + 1, 'x');
    if (ava::tests::require(oversized_anchors.has_value(), "oversized provider opens fixture anchors"))
    {
      auto oversized_request = oversized_fixture->request();
      oversized_request.paths = oversized_paths;
      oversized_request.anchor_set = *oversized_anchors;
      oversized_request.provider_options.access_token = "direct-fake-token";
      oversized_request.provider_options.transport_factory = [oversized_state] { return std::make_unique<ProviderTransport>(oversized_state); };
      auto oversized_coordinator = ava::app::BranchSummaryCoordinator::create();
      if (!coordinator_ready(oversized_coordinator, "oversized provider"))
        return;
      auto oversized_generation = prepare_awaiting(*oversized_coordinator, std::move(oversized_request), "oversized provider");
      if (oversized_generation)
      {
        expect((*oversized_coordinator)->confirm(*oversized_generation).value_or(false), "raw provider cap fixture confirms");
        auto oversized_terminal = await_terminal(*oversized_coordinator, *oversized_generation);
        expect(oversized_terminal.failure_code == ava::app::BranchSummaryFailureCode::ProviderFailed && !oversized_terminal.refresh_required,
               "a direct provider response one byte over 64 KiB fails closed before parsing or append");
      }
    }
  }
}

void test_cancellation_linearizes_before_revalidation_and_append()
{
  for (auto const phase : {ava::app::BranchSummaryPhase::Revalidating, ava::app::BranchSummaryPhase::Appending})
  {
    auto fixture = make_fixture("cancel-" + std::string(ava::app::to_string(phase)));
    if (!fixture)
      continue;
    auto blocker = std::make_shared<PhaseBlocker>(phase);
    auto coordinator = ava::app::BranchSummaryCoordinator::create({.operation_deadline = 30s,
                                                                   .on_snapshot = [blocker](auto const& snapshot) { blocker->observe(snapshot); },
                                                                   .configure_source_store_for_test = nullptr});
    ReleasePhaseOnExit release_phase{blocker};
    if (!coordinator_ready(coordinator, "cancellation linearization"))
      continue;
    auto generator = [](ava::app::BranchSummaryGenerationPrompt const&, std::stop_token,
                        std::chrono::steady_clock::time_point) -> ava::core::Result<std::string> { return "cancel linearization summary"; };
    auto generation = prepare_awaiting(*coordinator, fixture->request(generator), "cancellation linearization");
    if (!generation || !confirm_generation(*coordinator, *generation, "cancellation linearization"))
      continue;
    expect(blocker->wait_reached(), "cancellation reaches " + std::string(ava::app::to_string(phase)));
    auto const before = ava::tests::read_session_test_binary_file(fixture->source_store.session_path());
    auto canceled = (*coordinator)->cancel(*generation);
    blocker->allow();
    auto terminal = await_terminal(*coordinator, *generation);
    auto const after = ava::tests::read_session_test_binary_file(fixture->source_store.session_path());
    expect(canceled && *canceled && terminal.phase == ava::app::BranchSummaryPhase::Canceled && before == after && !terminal.refresh_required,
           "cancellation in " + std::string(ava::app::to_string(phase)) + " wins before append and is nonmutating");
  }
}

void test_shutdown_is_safe_in_every_nonterminal_phase()
{
  for (auto const phase : {ava::app::BranchSummaryPhase::Preparing, ava::app::BranchSummaryPhase::AwaitingConfirmation,
                           ava::app::BranchSummaryPhase::Generating, ava::app::BranchSummaryPhase::Revalidating, ava::app::BranchSummaryPhase::Appending})
  {
    auto fixture = make_fixture("shutdown-" + std::string(ava::app::to_string(phase)));
    if (!fixture)
      continue;
    auto blocker = std::make_shared<PhaseBlocker>(phase);
    auto coordinator = ava::app::BranchSummaryCoordinator::create({.operation_deadline = 30s,
                                                                   .on_snapshot = [blocker](auto const& snapshot) { blocker->observe(snapshot); },
                                                                   .configure_source_store_for_test = nullptr});
    ReleasePhaseOnExit release_phase{blocker};
    if (!coordinator_ready(coordinator, "shutdown phase"))
      continue;
    auto generator = [](ava::app::BranchSummaryGenerationPrompt const&, std::stop_token,
                        std::chrono::steady_clock::time_point) -> ava::core::Result<std::string> { return "shutdown phase summary"; };
    auto prepare_future =
        std::async(std::launch::async, [&, request = fixture->request(generator)]() mutable { return (*coordinator)->prepare(std::move(request)); });
    std::optional<std::uint64_t> generation;
    if (phase != ava::app::BranchSummaryPhase::Preparing)
    {
      auto prepared = prepare_future.get();
      expect(prepared.has_value(), prepared ? "shutdown phase preparation starts" : prepared.error().format());
      if (!prepared)
        continue;
      generation = *prepared;
      if (phase != ava::app::BranchSummaryPhase::AwaitingConfirmation)
      {
        if (!await_confirmation(*coordinator, *generation, "shutdown phase") || !confirm_generation(*coordinator, *generation, "shutdown phase"))
          continue;
      }
    }
    expect(blocker->wait_reached(), "shutdown test reaches " + std::string(ava::app::to_string(phase)));
    auto shutdown_future = std::async(std::launch::async, [&] { (*coordinator)->shutdown(); });
    blocker->allow();
    if (phase == ava::app::BranchSummaryPhase::Preparing)
    {
      auto prepared = prepare_future.get();
      if (prepared)
        generation = *prepared;
    }
    expect(shutdown_future.wait_for(3s) == std::future_status::ready, "shutdown joins outside locks in " + std::string(ava::app::to_string(phase)));
    shutdown_future.get();
    auto snapshot = (*coordinator)->snapshot();
    expect(!generation || snapshot.generation == *generation, "shutdown preserves bounded generation state in " + std::string(ava::app::to_string(phase)));
  }
}

void test_runtime_request_factory_snapshots_without_authentication()
{
  auto const root = create_empty_root("branch-summary-request-factory");
  auto const workspace = root / "workspace";
  std::filesystem::create_directories(workspace);
  auto paths = ava::tests::app_test_paths(root / "xdg");
  auto catalog = ava::provider::ProviderCatalog::build_builtins_only();
  auto anchors = ava::core::AnchorSet::open({workspace, paths.ava_config_dir, paths.ava_state_dir, paths.sessions_dir});
  expect(anchors.has_value(), anchors ? "request factory opens runtime anchors" : anchors.error().format());
  if (!anchors)
    return;
  auto registry = ava::config::builtin_model_registry();
  ava::app::runtime::OpenContext context;
  context.workspace_dir = workspace;
  context.current_dir = workspace;
  context.paths = paths;
  context.offline = true;
  context.anchor_set = *anchors;
  context.default_model_override = ava::config::select_default_model(registry);
  context.pin_model_override = true;
  context.session_read_limits = ava::session::legacy_unbounded_session_read_limits();
  context.provider_catalog = catalog;
  auto runtime = ava::app::runtime::Session::open(context);
  expect(runtime.has_value(), runtime ? "request factory runtime opens" : runtime.error().format());
  if (!runtime)
    return;
  auto auth_calls = std::make_shared<std::atomic<int>>(0);
  ava::app::BranchSummaryProviderOptions provider_options;
  provider_options.access_token = "factory-secret";
  provider_options.transport_factory = [auth_calls]() -> std::unique_ptr<ava::http::Transport> {
    auth_calls->fetch_add(1);
    return nullptr;
  };
  ava::core::Result<ava::app::BranchSummaryOperationRequest> request =
      std::unexpected(ava::core::Error(ava::core::ErrorCategory::Unknown, "request was not built"));
  request = ava::app::make_branch_summary_operation_request(*runtime,
                                                            {.session_id = "session_parent_for_factory",
                                                             .path = paths.sessions_dir / "session_parent_for_factory.jsonl",
                                                             .last_updated = "2026-05-01T00:00:00Z",
                                                             .entry_count = 3,
                                                             .original_cwd = workspace,
                                                             .title = std::string(400, 'P')},
                                                            nullptr, std::move(provider_options));
  {
    SCOPED_CRITICAL_AREA_R(session_r, *runtime);
    auto const options_r(session_r->store.options_r());
    expect(request && request->current_session_id == options_r->session_id &&
               request->current_read_authority.session_id() == options_r->session_id && request->current_controller == session_r->run_controller() &&
               request->provider_catalog.get() == catalog.get() && request->anchor_set == *anchors &&
               request->read_limits.max_file_bytes == ava::app::kBranchSummaryHardReadLimits.max_file_bytes &&
               request->read_limits.max_line_bytes == ava::app::kBranchSummaryHardReadLimits.max_line_bytes &&
               request->read_limits.max_entries == ava::app::kBranchSummaryHardReadLimits.max_entries && request->provider_options.offline,
           "runtime request factory copies current ownership while clamping a legacy policy to immutable coordinator limits");
  }
  expect(request && request->source_label.size() == ava::app::kMaxBranchSummaryDisplayLabelBytes && auth_calls->load() == 0,
         "runtime request factory bounds display labels and performs no authentication or provider I/O");
}

void test_ordinary_provider_context_ignores_branch_summaries()
{
  std::vector<ava::session::SessionEntry> entries{
      entry("user", "", ava::session::EntryType::UserMessage, R"({"text":"ordinary user"})"),
      entry("assistant", "user", ava::session::EntryType::AssistantMessage, R"({"text":"ordinary assistant"})"),
      entry(
          "summary", "assistant", ava::session::EntryType::BranchSummary,
          R"({"schema_version":1,"summary":"metadata-only secret summary","source_session_id":"source","branch_root_entry_id":"user","branch_tip_entry_id":"assistant","provider":"openai","model":"gpt-5.5","reason":"abandoned_parent","actor":"tui"})"),
      entry("later", "summary", ava::session::EntryType::UserMessage, R"({"text":"later ordinary user"})")};
  auto messages = ava::agent::build_provider_messages_from_entries(entries);
  std::string flattened;
  if (messages)
    for (auto const& message : *messages) flattened += message.content;
  expect(messages && flattened.find("ordinary user") != std::string::npos && flattened.find("ordinary assistant") != std::string::npos &&
             flattened.find("later ordinary user") != std::string::npos && flattened.find("metadata-only secret summary") == std::string::npos,
         "ordinary provider context continues to ignore BranchSummary metadata records");
}

}  // namespace

void run_branch_summary_coordinator_tests()
{
  ava::tests::HermeticTestEnvironment environment("branch-summary-tests-");
  test_recoverable_snapshot_fingerprint_is_exact_sha256();
  test_projection_is_pure_and_strict();
  test_projection_boundaries_and_summary_sanitizer();
  test_explicit_confirmation_success_and_existing();
  test_cancel_before_confirmation_is_nonmutating_and_conflicts_reject();
  test_typed_preparation_ineligibilities();
  test_remaining_typed_preparation_ineligibilities();
  test_generation_cancel_deadline_and_typed_failures();
  test_projection_and_recovery_failures_are_typed_before_provider_or_append();
  test_confirmed_recovery_precedes_generation();
  test_active_run_checks_at_confirmation_and_preappend();
  test_confirmation_binds_exact_source_fingerprint();
  test_coordinator_enforces_hard_read_policy();
  test_stale_source_relation_identity_and_final_existing();
  test_matching_summary_is_existing_only_for_exact_final_coverage();
  test_append_commit_states_are_terminal_and_never_retried();
  test_production_provider_request_is_metadata_only();
  test_cancellation_linearizes_before_revalidation_and_append();
  test_shutdown_is_safe_in_every_nonterminal_phase();
  test_runtime_request_factory_snapshots_without_authentication();
  test_ordinary_provider_context_ignores_branch_summaries();
}

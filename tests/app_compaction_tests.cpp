#include "sys.h"
#include "tests/support/app_runtime_support.h"
#include "tests/support/fake_transport.h"
#include "tests/support/runtime_event_test_support.h"
#include "tests/support/test_harness.h"
#include "ava/http/transport.h"
#include "ava/observability/run_observer.h"
#include "ava/process/scope.h"
#include "ava/process/supervisor.h"
#include "ava/app/commands.h"
#include "ava/app/runtime.h"
#include "ava/app/runtime/Session.h"
#include "ava/agent/context_compaction.h"
#include "ava/session/assistant_output.h"
#include "ava/session/compaction.h"
#include "ava/session/export.h"
#include "ava/session/record.h"
#include "ava/session/session_store.h"
#include "ava/session/stats.h"
#include "ava/provider/openai_provider.h"
#include "ava/provider/provider_utils.h"
#include "ava/core/json.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace {

using namespace ava::tests;

class ThrowingRunObserver final : public ava::observability::RunObserver
{
 public:
  void on_event(ava::observability::TraceEvent const&) override { throw std::runtime_error("observer failure"); }
};

class CancelAfterRequestTransport final : public ava::http::Transport
{
 public:
  explicit CancelAfterRequestTransport(ava::http::HttpResponse response) : response_(std::move(response)) { }

  [[nodiscard]] ava::core::Result<ava::http::HttpResponse> send(ava::http::HttpRequest const& request) override
  {
    requests_.push_back(request);
    canceled_ = true;
    return response_;
  }

  [[nodiscard]] bool canceled() const noexcept { return canceled_; }
  [[nodiscard]] std::vector<ava::http::HttpRequest> const& requests() const noexcept { return requests_; }

 private:
  ava::http::HttpResponse response_;
  bool canceled_ = false;
  std::vector<ava::http::HttpRequest> requests_;
};

void test_shared_compaction_transaction()
{
  auto store = ava::session::SessionStore::create_ephemeral(create_empty_root("shared-compaction"));
  expect(store.has_value(), "shared compaction creates isolated history");
  if (!store)
    return;
  auto target = ava::session::SessionAppendTarget::create_ephemeral(*store);
  if (!target)
  {
    expect(false, "shared compaction creates append authority");
    return;
  }
  auto authority = (*target)->read_authority();
  if (!authority)
  {
    expect(false, "shared compaction creates read authority");
    return;
  }
  ava::session::CompactionConfig config;
  config.auto_threshold_tokens = 0;
  config.auto_threshold_tokens_explicit = true;
  std::size_t summaries = 0;
  std::size_t appends = 0;
  bool cancel = false;
  bool always_stale = false;
  std::vector<ava::agent::CompactionTransactionPhase> phases;
  using Phase = ava::agent::CompactionTransactionPhase;
  ava::agent::CompactionTransactionAdapters adapters{
      .summarize = [&](auto const&, std::size_t) -> ava::core::Result<std::string> {
        ++summaries;
        return "shared summary";
      },
      .append = [&](ava::session::SessionEntry entry,
                    std::vector<ava::session::SessionEntry> snapshot) -> ava::core::Result<ava::session::SessionCompactionAppendResult> {
        ++appends;
        if (always_stale || appends == 1)
          return ava::session::SessionCompactionAppendResult::SnapshotMismatch;
        return (*target)->append_compaction_if_snapshot_matches(entry, snapshot);
      },
      .event = [&](Phase phase, ava::agent::CompactionTransactionStats const& stats) -> ava::core::VoidResult {
        phases.push_back(phase);
        expect(stats.max_attempts == 2 && stats.attempt >= 1 && stats.attempt <= 2, "shared compaction reports bounded attempt metadata");
        if (phase == Phase::Committed)
          expect(stats.summary_bytes == 14, "commit metadata describes the committed summary");
        return {};
      }};
  auto run = [&](std::string_view trigger) {
    return ava::agent::compact_context_transaction(*authority, config, 100, trigger, {}, [&] { return cancel; }, adapters);
  };
  auto disabled = run("auto");
  expect(disabled && !*disabled && summaries == 0 && phases.empty(), "explicit zero auto threshold suppresses summary and events in shared transaction");
  auto committed = run("context_overflow");
  expect(committed && *committed && summaries == 2 && appends == 2 &&
             phases == std::vector<Phase>{Phase::Started, Phase::SnapshotRetry, Phase::Started, Phase::Committed},
         "one shared transaction owns the two-attempt CAS flow and publishes end only after commit");
  always_stale = true;
  phases.clear();
  auto stale = run("context_overflow");
  expect(!stale && stale.error().message() == "session changed during context compaction after retry" && stale.error().context().size() == 3 &&
             summaries == 4 && appends == 4 && phases == std::vector<Phase>{Phase::Started, Phase::SnapshotRetry, Phase::Started},
         "shared transaction bounds repeated staleness without a false completion event");
  cancel = true;
  phases.clear();
  auto canceled = run("context_overflow");
  expect(!canceled && canceled.error().code() == ava::core::ErrorCode::Canceled && phases.empty() && summaries == 4,
         "shared transaction checks cancellation before loading or summarizing");
  cancel = false;
  adapters.summarize = [&](auto const&, std::size_t) -> ava::core::Result<std::string> {
    cancel = true;
    return "canceled summary";
  };
  auto canceled_summary = run("context_overflow");
  expect(!canceled_summary && canceled_summary.error().code() == ava::core::ErrorCode::Canceled && appends == 4,
         "shared transaction checks cancellation after summary and before checkpoint append");
}

void test_app_compact_provider_summary_success()
{
  auto const root = create_empty_root("app-compact-provider-success");

  auto const workspace = root / "workspace";
  auto const paths = app_test_paths(root);
  std::filesystem::create_directories(workspace);

  ava::app::runtime::OpenContext open_context;
  open_context.workspace_dir = workspace;
  open_context.current_dir = workspace;
  open_context.paths = paths;
  auto unlocked_session_result = ava::app::runtime::Session::open(open_context);
  expect(unlocked_session_result.has_value(), "provider-backed /compact test opens runtime session");
  if (!unlocked_session_result)
    return;

  ava::app::runtime::session_ts& unlocked_session = *unlocked_session_result;
  CRITICAL_AREA_BEGIN_W(session);

  auto seeded = session_w->append_owned(ava::session::SessionEntry{.id = "entry_user_compact_source",
                                                                   .parent_id = "",
                                                                   .type = ava::session::EntryType::UserMessage,
                                                                   .timestamp = "2026-05-01T00:00:00Z",
                                                                   .data_json = "{\"text\":\"Goal: refactor compaction\"}"});
  expect(seeded.has_value(), "provider-backed /compact test seeds source entry");
  auto seeded_reasoning = session_w->append_owned(ava::session::SessionEntry{
      .id = "entry_reasoning_compact_source",
      .parent_id = "",
      .type = ava::session::EntryType::ReasoningBlock,
      .timestamp = "2026-05-01T00:00:01Z",
      .data_json =
          R"({"provider":"anthropic","model":"claude","format":"anthropic_thinking","text":"visible compact reasoning","signature":"compact-secret-signature","redacted_data":"opaque-compaction-redacted","redacted":false})"});
  expect(seeded_reasoning.has_value(), "provider-backed /compact test seeds reasoning source entry");
  auto seeded_redacted_reasoning = session_w->append_owned(ava::session::SessionEntry{
      .id = "entry_redacted_reasoning_compact_source",
      .parent_id = "",
      .type = ava::session::EntryType::ReasoningBlock,
      .timestamp = "2026-05-01T00:00:02Z",
      .data_json =
          R"({"provider":"anthropic","model":"claude","format":"anthropic_thinking","text":"hidden redacted compact reasoning","signature":"redacted-compact-secret","redacted_data":"opaque-hidden-compaction-redacted","redacted": true })"});
  expect(seeded_redacted_reasoning.has_value(), "provider-backed /compact test seeds redacted reasoning source entry");

  std::string const summary =
      "# Goal\nShip compact\n# Constraints / Preferences\nKeep provider backed\n# Decisions\nUse callback\n"
      "# Files Read or Modified\nsrc/ava/app/commands.cpp\n# Unresolved Tasks\nNone noted.\n# Next Steps\nRun tests.";
  ava::provider::OpenAIProvider const provider("https://api.example.test");
  ava::tests::FakeTransport transport(
      {ava::http::HttpResponse{.status_code = 200, .headers = {}, .body = "{\"output_text\":\"" + ava::core::json::escape(summary) + "\"}"}});
  ava::app::runtime::RunOptions run_options;
  run_options.access_token = "token";

  CRITICAL_AREA_END_W(session);
  auto compact = ava::app::run_command(
      unlocked_session, ava::app::CommandRequest{
                      .command = "/compact Keep decisions",
                      .compaction_summary_generator = [&](std::vector<ava::session::SessionEntry> const& entries, ava::session::CompactionConfig const& config,
                                                          std::string_view instructions, std::size_t estimated_tokens) {
                        return ava::app::generate_compaction_summary(unlocked_session, entries, config, instructions, estimated_tokens, provider, transport,
                                                                      run_options);
                      }});
  CRITICAL_AREA_CONTINUE_W(session);
  expect(compact && compact->handled && !compact->output.empty() && compact->output[0].find("compaction summary recorded") != std::string::npos,
         "/compact records a provider-generated summary");
  expect(transport.requests().size() == 1 && transport.requests()[0].body.find("Goal: refactor compaction") != std::string::npos &&
             transport.requests()[0].body.find("visible compact reasoning") == std::string::npos &&
             transport.requests()[0].body.find("hidden redacted compact reasoning") == std::string::npos &&
             transport.requests()[0].body.find("signature_present") == std::string::npos &&
             transport.requests()[0].body.find("compact-secret-signature") == std::string::npos &&
             transport.requests()[0].body.find("redacted-compact-secret") == std::string::npos &&
             transport.requests()[0].body.find("opaque-compaction-redacted") == std::string::npos &&
             transport.requests()[0].body.find("opaque-hidden-compaction-redacted") == std::string::npos &&
             transport.requests()[0].body.find("# Files Read or Modified") != std::string::npos &&
             transport.requests()[0].body.find("Keep decisions") != std::string::npos,
         "provider-backed /compact sends deterministic prompt with sanitized source data and required sections");

  auto entries = session_w->store.load();
  expect(entries && std::ranges::any_of(
                        *entries,
                        [&](ava::session::SessionEntry const& entry) {
                          return entry.type == ava::session::EntryType::Compaction && ava::core::json::string_field(entry.data_json, "summary") == summary &&
                                 ava::core::json::string_field(entry.data_json, "recent_context").value_or("").find("Goal: refactor compaction") !=
                                     std::string::npos &&
                                 entry.data_json.find("\"reason\":\"manual\"") != std::string::npos &&
                                 entry.data_json.find("\"provider\":\"openai\"") != std::string::npos &&
                                 entry.data_json.find("\"history_projection\":\"portable-v1\"") != std::string::npos &&
                                 entry.data_json.find("\"summary_unavailable\":false") != std::string::npos;
                        }),
         "/compact appends the returned summary and the shared bounded recent-turn projection");
}

void test_app_compact_rejects_replaced_current_session_history()
{
  auto const root = create_empty_root("app-compact-path-replacement");

  auto const workspace = root / "workspace";
  auto const paths = app_test_paths(root);
  std::filesystem::create_directories(workspace);

  ava::app::runtime::OpenContext open_context;
  open_context.workspace_dir = workspace;
  open_context.current_dir = workspace;
  open_context.paths = paths;
  auto unlocked_session_result = ava::app::runtime::Session::open(open_context);
  expect(unlocked_session_result.has_value(), "replacement-safe /compact test opens runtime session");
  if (!unlocked_session_result)
    return;
  ava::app::runtime::session_ts& unlocked_session = *unlocked_session_result;
  CRITICAL_AREA_BEGIN_W(session);

  expect(session_w
             ->append_owned(ava::session::SessionEntry{.id = "original_compaction_source",
                                                       .parent_id = "",
                                                       .type = ava::session::EntryType::UserMessage,
                                                       .timestamp = "2026-05-01T00:00:00Z",
                                                       .data_json = "{\"text\":\"ORIGINAL_COMPACTION_SOURCE\"}"})
             .has_value(),
         "replacement-safe /compact test seeds original history");

  auto replacement = ava::session::serialize_session_entry_line(ava::session::SessionEntry{.id = "replacement_compaction_source",
                                                                                           .parent_id = "",
                                                                                           .type = ava::session::EntryType::UserMessage,
                                                                                           .timestamp = "2026-05-01T00:00:01Z",
                                                                                           .data_json = "{\"text\":\"REPLACEMENT_COMPACTION_CANARY\"}"});
  expect(replacement.has_value(), "replacement-safe /compact test serializes replacement history");
  if (!replacement)
    return;
  bool replaced = false;
  auto const session_path = session_w->store.session_path();
  session_w->store.set_after_lease_bound_read_for_test([&, session_path] {
    if (replaced)
      return;
    replaced = true;
    std::filesystem::rename(session_path, session_path.string() + ".parked");
    std::ofstream file(session_path, std::ios::binary | std::ios::trunc);
    file << *replacement << '\n';
  });
  std::size_t generator_calls = 0;
  CRITICAL_AREA_END_W(session);
  auto compact = ava::app::run_command(
      unlocked_session,
      ava::app::CommandRequest{.command = "/compact",
                               .compaction_summary_generator = [&](std::vector<ava::session::SessionEntry> const&, ava::session::CompactionConfig const&,
                                                                   std::string_view, std::size_t) -> ava::core::Result<std::string> {
                                 ++generator_calls;
                                 return std::string("must not summarize replacement");
                                }});
  CRITICAL_AREA_CONTINUE_W(session);
  auto pathname_entries = session_w->store.load();
  expect(replaced && compact && compact->handled && generator_calls == 0 && !compact->output.empty() &&
             compact->output.front().find("replaced") != std::string::npos && pathname_entries && pathname_entries->size() == 1 &&
             pathname_entries->front().data_json.find("REPLACEMENT_COMPACTION_CANARY") != std::string::npos,
         "manual compaction snapshot fails closed after authority binding and never summarizes replacement pathname content");
}

void test_app_compact_openai_oauth_streaming_summary_success()
{
  auto const root = create_empty_root("app-compact-oauth-streaming");

  auto const workspace = root / "workspace";
  auto const paths = app_test_paths(root);
  std::filesystem::create_directories(workspace);

  ava::app::runtime::OpenContext open_context;
  open_context.workspace_dir = workspace;
  open_context.current_dir = workspace;
  open_context.paths = paths;
  auto unlocked_session_result = ava::app::runtime::Session::open(open_context);
  expect(unlocked_session_result.has_value(), "OAuth streaming /compact test opens runtime session");
  if (!unlocked_session_result)
    return;

  ava::app::runtime::session_ts& unlocked_session = *unlocked_session_result;
  CRITICAL_AREA_BEGIN_W(session);

  std::string const summary = "# Goal\nLive compaction works.";
  std::string const sse_body = "data: {\"type\":\"response.output_text.delta\",\"delta\":\"" + ava::core::json::escape(summary) +
                               "\"}\n\n"
                               "data: [DONE]\n\n";
  ava::provider::OpenAIProvider const provider("https://api.example.test");
  ava::tests::FakeTransport transport({ava::http::HttpResponse{.status_code = 200, .headers = {}, .body = sse_body}});
  ava::app::runtime::RunOptions run_options;
  run_options.access_token = "token";
  run_options.openai_oauth = true;
  run_options.openai_account_id = "acct_test";

  auto config = ava::session::default_compaction_config();
  auto entries = session_w->store.load();
  expect(entries.has_value(), "OAuth streaming /compact test loads entries");
  if (!entries)
    return;
  CRITICAL_AREA_END_W(session);
  auto generated = ava::app::generate_compaction_summary(unlocked_session, *entries, config, "live", 12, provider, transport, run_options);
  expect(generated && *generated == summary, "OAuth streaming compaction summary parses SSE text deltas");
  expect(transport.requests().size() == 1 && transport.requests()[0].url == "https://chatgpt.com/backend-api/codex/responses" &&
             transport.requests()[0].body.find("\"stream\":true") != std::string::npos &&
             transport.requests()[0].body.find("\"store\":false") != std::string::npos &&
             transport.requests()[0].body.find("\"max_output_tokens\"") == std::string::npos,
         "OAuth compaction summary request uses the delegated request shape without public-only output-token parameters");
}

void test_app_compact_provider_failure_leaves_session_untouched()
{
  auto const root = create_empty_root("app-compact-provider-failure");

  auto const workspace = root / "workspace";
  auto const paths = app_test_paths(root);
  std::filesystem::create_directories(workspace);

  ava::app::runtime::OpenContext open_context;
  open_context.workspace_dir = workspace;
  open_context.current_dir = workspace;
  open_context.paths = paths;
  auto unlocked_session_result = ava::app::runtime::Session::open(open_context);
  expect(unlocked_session_result.has_value(), "provider failure /compact test opens runtime session");
  if (!unlocked_session_result)
    return;

  ava::app::runtime::session_ts& unlocked_session = *unlocked_session_result;
  CRITICAL_AREA_BEGIN_W(session);

  ava::provider::OpenAIProvider const provider("https://api.example.test");
  ava::tests::FakeTransport transport({ava::http::HttpResponse{.status_code = 500, .headers = {}, .body = "{\"error\":{\"message\":\"boom\"}}"}});
  ava::app::runtime::RunOptions run_options;
  run_options.access_token = "token";

  CRITICAL_AREA_END_W(session);
  auto compact = ava::app::run_command(
      unlocked_session, ava::app::CommandRequest{
                      .command = "/compact",
                      .compaction_summary_generator = [&](std::vector<ava::session::SessionEntry> const& entries, ava::session::CompactionConfig const& config,
                                                          std::string_view instructions, std::size_t estimated_tokens) {
                        return ava::app::generate_compaction_summary(unlocked_session, entries, config, instructions, estimated_tokens, provider, transport,
                                                                      run_options);
                      }});
  CRITICAL_AREA_CONTINUE_W(session);
  auto entries = session_w->store.load();
  expect(compact && compact->handled && !compact->output.empty() &&
             compact->output[0].find("compaction summary request failed with status 500") != std::string::npos &&
             compact->output[0].find("boom") == std::string::npos,
         "provider-backed /compact reports fixed local failure status without a provider body diagnostic");
  expect(entries && std::ranges::none_of(*entries, [](ava::session::SessionEntry const& entry) { return entry.type == ava::session::EntryType::Compaction; }),
         "provider-backed /compact failure leaves session without compaction entry");
}

void test_compaction_observation_preserves_cancellation_callback_contract()
{
  auto const root = create_empty_root("app-compaction-observer-callback");

  auto const workspace = root / "workspace";
  auto const paths = app_test_paths(root);
  std::filesystem::create_directories(workspace);
  ava::app::runtime::OpenContext open_context;
  open_context.workspace_dir = workspace;
  open_context.current_dir = workspace;
  open_context.paths = paths;
  auto unlocked_session_result = ava::app::runtime::Session::open(open_context);
  expect(unlocked_session_result.has_value(), "compaction callback-contract test opens runtime session");
  if (!unlocked_session_result)
    return;

  ava::app::runtime::session_ts& unlocked_session = *unlocked_session_result;
  CRITICAL_AREA_BEGIN_W(session);

  ava::provider::OpenAIProvider const provider("https://api.example.test");
  auto const config = ava::session::default_compaction_config();
  auto const entries = session_w->store.load();
  expect(entries.has_value(), "compaction callback-contract test loads session entries");
  if (!entries)
    return;

  CRITICAL_AREA_END_W(session);
  auto run_summary = [&](std::shared_ptr<ava::observability::RunObservation> observation, bool throwing_callback) {
    ava::tests::FakeTransport transport({ava::http::HttpResponse{.status_code = 200, .headers = {}, .body = "{\"output_text\":\"summary\"}"}});
    unsigned callback_calls = 0;
    ava::app::runtime::RunOptions options;
    options.access_token = "token";
    options.observation = std::move(observation);
    options.cancel_requested = [&callback_calls, throwing_callback]() -> bool {
      ++callback_calls;
      if (throwing_callback)
        throw std::runtime_error("authoritative callback failure");
      return false;
    };
    bool callback_threw = false;
    ava::core::Result<std::string> result = std::unexpected(ava::core::Error(ava::core::ErrorCategory::Unknown, "not run"));
    try
    {
      result = ava::app::generate_compaction_summary(unlocked_session, *entries, config, "", 1, provider, transport, options);
    }
    catch (std::runtime_error const&)
    {
      callback_threw = true;
    }
    return std::tuple{std::move(result), callback_calls, callback_threw};
  };

  auto const [disabled_result, disabled_calls, disabled_threw] = run_summary(nullptr, false);
  auto throwing_observer = std::make_shared<ThrowingRunObserver>();
  auto enabled_observation = std::make_shared<ava::observability::RunObservation>(throwing_observer);
  auto const [enabled_result, enabled_calls, enabled_threw] = run_summary(enabled_observation, false);
  auto const [disabled_throw_result, disabled_throw_calls, disabled_throw_threw] = run_summary(nullptr, true);
  auto const [enabled_throw_result, enabled_throw_calls, enabled_throw_threw] = run_summary(enabled_observation, true);
  expect(disabled_result && enabled_result && *disabled_result == "summary" && *enabled_result == "summary" && disabled_calls == 3 && enabled_calls == 3 &&
             !disabled_threw && !enabled_threw && enabled_observation->counters().callback_failures == 1 && !disabled_throw_result && !enabled_throw_result &&
             disabled_throw_threw && enabled_throw_threw && disabled_throw_calls == 1 && enabled_throw_calls == 1,
         "compaction observation isolates observer failures and preserves stateful and throwing cancellation callback behavior/counts");
}

void test_app_auto_compaction_provider_cancellation_leaves_session_untouched()
{
  auto const root = create_empty_root("app-auto-compact-canceled");

  auto const workspace = root / "workspace";
  auto const paths = app_test_paths(root);
  std::filesystem::create_directories(workspace);

  ava::app::runtime::OpenContext open_context;
  open_context.workspace_dir = workspace;
  open_context.current_dir = workspace;
  open_context.paths = paths;
  auto unlocked_session_result = ava::app::runtime::Session::open(open_context);
  expect(unlocked_session_result.has_value(), "provider cancellation auto compaction test opens runtime session");
  if (!unlocked_session_result)
    return;
  ava::app::runtime::session_ts& unlocked_session = *unlocked_session_result;

  CRITICAL_AREA_BEGIN_W(session);

  session_w->model_selection().model.context_window_tokens = 100;
  static_cast<void>(session_w->append_owned(ava::session::SessionEntry{.id = "entry_canceled_auto_compact",
                                                                       .parent_id = "",
                                                                       .type = ava::session::EntryType::UserMessage,
                                                                       .timestamp = ava::session::now_timestamp(),
                                                                       .data_json = "{\"text\":\"" + std::string(420, 'c') + "\"}"}));

  ava::provider::OpenAIProvider const provider("https://api.example.test");
  CancelAfterRequestTransport transport(ava::http::HttpResponse{.status_code = 200, .headers = {}, .body = "{\"output_text\":\"CANCELED SUMMARY\"}"});
  ava::app::runtime::RunOptions run_options;
  run_options.access_token = "token";
  run_options.cancel_requested = [&transport] { return transport.canceled(); };

  CRITICAL_AREA_END_W(session);

  auto result = ava::app::run_prompt(unlocked_session, "cancel during compaction", provider, transport, run_options);

  CRITICAL_AREA_CONTINUE_W(session);

  auto entries = session_w->store.load();
  expect(!result && result.error().message() == "agent loop canceled", "auto compaction reports cancellation raised during the provider summary request");
  expect(transport.requests().size() == 1, "canceled auto compaction dispatches only the summary request");
  expect(entries && count_compaction_entries(*entries) == 0, "canceled auto compaction leaves no partial compaction entry");
}

void test_app_compact_oversized_summary_leaves_session_untouched()
{
  auto const root = create_empty_root("app-compact-oversized");

  auto const workspace = root / "workspace";
  auto const paths = app_test_paths(root);
  std::filesystem::create_directories(workspace);
  std::filesystem::create_directories(paths.ava_config_dir);
  {
    std::ofstream file(paths.compaction_file, std::ios::binary | std::ios::trunc);
    file << "{\"max_summary_bytes\":8}";
  }

  ava::app::runtime::OpenContext open_context;
  open_context.workspace_dir = workspace;
  open_context.current_dir = workspace;
  open_context.paths = paths;
  auto unlocked_session_result = ava::app::runtime::Session::open(open_context);
  expect(unlocked_session_result.has_value(), "oversized /compact test opens runtime session");
  if (!unlocked_session_result)
    return;

  ava::app::runtime::session_ts& unlocked_session = *unlocked_session_result;
  CRITICAL_AREA_BEGIN_W(session);

  CRITICAL_AREA_END_W(session);
  auto compact = ava::app::run_command(
      unlocked_session, ava::app::CommandRequest{.command = "/compact",
                                           .compaction_summary_generator =
                                               [](std::vector<ava::session::SessionEntry> const&, ava::session::CompactionConfig const&, std::string_view,
                                                   std::size_t) -> ava::core::Result<std::string> { return std::string("this summary is too large"); }});
  CRITICAL_AREA_CONTINUE_W(session);
  auto entries = session_w->store.load();
  expect(compact && compact->handled && !compact->output.empty() && compact->output[0].find("generated compaction summary is too large") != std::string::npos,
         "/compact reports oversized generated summary");
  expect(entries && std::ranges::none_of(*entries, [](ava::session::SessionEntry const& entry) { return entry.type == ava::session::EntryType::Compaction; }),
         "oversized generated summary leaves session without compaction entry");
}

void test_app_compact_cancellation_before_append_leaves_session_untouched()
{
  auto const root = create_empty_root("app-compact-cancel-before-append");

  auto const workspace = root / "workspace";
  auto const paths = app_test_paths(root);
  std::filesystem::create_directories(workspace);

  ava::app::runtime::OpenContext open_context;
  open_context.workspace_dir = workspace;
  open_context.current_dir = workspace;
  open_context.paths = paths;
  auto unlocked_session_result = ava::app::runtime::Session::open(open_context);
  expect(unlocked_session_result.has_value(), "manual compaction cancellation test opens runtime session");
  if (!unlocked_session_result)
    return;

  ava::app::runtime::session_ts& unlocked_session = *unlocked_session_result;
  CRITICAL_AREA_BEGIN_W(session);

  bool cancel = false;
  CRITICAL_AREA_END_W(session);
  auto compact = ava::app::run_command(
      unlocked_session,
      ava::app::CommandRequest{.command = "/compact",
                               .compaction_summary_generator = [&cancel](std::vector<ava::session::SessionEntry> const&, ava::session::CompactionConfig const&,
                                                                         std::string_view, std::size_t) -> ava::core::Result<std::string> {
                                 cancel = true;
                                 return std::string("summary generated just before cancellation");
                               },
                               .cancel_requested = [&cancel] { return cancel; },
                                .propagate_compaction_errors = true});
  CRITICAL_AREA_CONTINUE_W(session);
  auto entries = session_w->store.load();
  expect(!compact && compact.error().message() == "agent loop canceled", "manual compaction observes cancellation before appending the generated summary");
  expect(entries && count_compaction_entries(*entries) == 0, "manual compaction cancellation leaves no partial compaction entry");
}

void test_app_compaction_prompt_builder_sections()
{
  auto config = ava::session::default_compaction_config();
  std::vector<ava::session::SessionEntry> const entries = {
      ava::session::SessionEntry{.id = "entry_tool_call",
                                 .parent_id = "",
                                 .type = ava::session::EntryType::ToolCall,
                                 .timestamp = "2026-05-01T00:00:00Z",
                                 .data_json = "{\"call_id\":\"call_compaction\",\"name\":\"read\",\"arguments\":\"{}\"}"},
      ava::session::SessionEntry{.id = "entry_tool",
                                 .parent_id = "",
                                 .type = ava::session::EntryType::ToolResult,
                                 .timestamp = "2026-05-01T00:00:00Z",
                                 .data_json = "{\"call_id\":\"call_compaction\",\"name\":\"read\",\"success\":true,\"result\":\"src/main.cpp contents\"}"},
      ava::session::SessionEntry{.id = "entry_replay",
                                 .parent_id = "",
                                 .type = ava::session::EntryType::UserMessage,
                                 .timestamp = "2026-05-01T00:00:01Z",
                                 .data_json = "{\"text\":\"duplicated active prompt\",\"internal_replay\":true,"
                                              "\"replay_of\":\"entry_user\"}"}};
  auto const prompt = ava::app::build_compaction_summary_prompt(entries, config, "preserve files", 42);
  expect(prompt && prompt->find("# Goal") != std::string::npos && prompt->find("# Constraints / Preferences") != std::string::npos &&
             prompt->find("# Files Read or Modified") != std::string::npos && prompt->find("src/main.cpp") != std::string::npos &&
             prompt->find("preserve files") != std::string::npos && prompt->find("internal_replay") == std::string::npos &&
             prompt->find("duplicated active prompt") == std::string::npos,
         "compaction prompt builder includes source data and skips internal replay messages");

  auto compacted_entries = entries;
  compacted_entries.push_back(ava::session::SessionEntry{.id = "boundary",
                                                         .parent_id = "",
                                                         .type = ava::session::EntryType::Compaction,
                                                         .timestamp = "2026-05-01T00:00:02Z",
                                                         .data_json = "{\"summary\":\"ACTIVE_BOUNDARY_SUMMARY\",\"history_projection\":\"portable-v1\"}"});
  compacted_entries.push_back(ava::session::SessionEntry{.id = "active_user",
                                                         .parent_id = "",
                                                         .type = ava::session::EntryType::UserMessage,
                                                         .timestamp = "2026-05-01T00:00:03Z",
                                                         .data_json = "{\"text\":\"ACTIVE_AFTER_BOUNDARY\"}"});
  auto const active_prompt = ava::app::build_compaction_summary_prompt(compacted_entries, config, "", 10);
  expect(active_prompt && active_prompt->find("ACTIVE_BOUNDARY_SUMMARY") != std::string::npos &&
             active_prompt->find("ACTIVE_AFTER_BOUNDARY") != std::string::npos && active_prompt->find("src/main.cpp") == std::string::npos,
         "compaction prompt builder summarizes only context active after the latest compaction boundary");
}

void test_app_compaction_model_selection_uses_runtime_catalog()
{
  auto const root = temp_root() / "app-compaction-model-selection";
  std::error_code remove_error;
  std::filesystem::remove_all(root, remove_error);
  auto const workspace = root / "workspace";
  std::filesystem::create_directories(workspace);
  ava::app::runtime::OpenContext options;
  options.workspace_dir = workspace;
  options.current_dir = workspace;
  options.paths = app_test_paths(root);
  auto unlocked_session_result = ava::app::runtime::Session::open(options);
  expect(unlocked_session_result.has_value(), "compaction model-selection test opens runtime session");
  if (!unlocked_session_result)
    return;

  ava::app::runtime::session_ts& unlocked_session = *unlocked_session_result;
  CRITICAL_AREA_BEGIN_W(session);

  auto const active_model = session_w->model();
  CRITICAL_AREA_END_W(session);
  auto active = ava::app::resolve_compaction_config(unlocked_session, ava::session::default_compaction_config());
  auto same_config = ava::session::parse_compaction_config(R"({"model":"gpt-5.5"})");
  auto same = same_config ? ava::app::resolve_compaction_config(unlocked_session, std::move(*same_config))
                          : ava::core::Result<ava::session::CompactionConfig>(std::unexpected(same_config.error()));
  auto cross_config = ava::session::parse_compaction_config(R"({"provider":"anthropic","model":"claude-sonnet-4-5"})");
  auto cross = cross_config ? ava::app::resolve_compaction_config(unlocked_session, std::move(*cross_config))
                            : ava::core::Result<ava::session::CompactionConfig>(std::unexpected(cross_config.error()));
  auto unknown_config = ava::session::parse_compaction_config(R"({"provider":"anthropic","model":"not-configured"})");
  auto unknown = unknown_config ? ava::app::resolve_compaction_config(unlocked_session, std::move(*unknown_config))
                                : ava::core::Result<ava::session::CompactionConfig>(std::unexpected(unknown_config.error()));
  expect(active && active->provider_id == active_model.provider_id && active->model_id == active_model.model_id && same &&
             same->provider_id == active_model.provider_id && same->model_id == "gpt-5.5" && cross && cross->provider_id == "anthropic" &&
             cross->model_id == "claude-sonnet-4-5" && !unknown && unknown.error().format().find("compaction_model: not-configured") != std::string::npos,
         "compaction selection defaults active, resolves same/cross-provider overrides, and rejects unknown models without fallback");
}

void test_app_compaction_recent_tail_preserves_tool_group()
{
  auto config = ava::session::default_compaction_config();
  config.keep_recent_messages = 1;
  config.keep_recent_messages_explicit = true;
  config.keep_recent_tokens = 1000;
  std::vector<ava::session::SessionEntry> const entries = {
      ava::session::SessionEntry{.id = "tool_turn_user",
                                 .parent_id = "",
                                 .type = ava::session::EntryType::UserMessage,
                                 .timestamp = "2026-05-01T00:00:00Z",
                                 .data_json = "{\"text\":\"inspect file\"}"},
      ava::session::SessionEntry{.id = "tool_turn_assistant",
                                 .parent_id = "",
                                 .type = ava::session::EntryType::AssistantMessage,
                                 .timestamp = "2026-05-01T00:00:01Z",
                                 .data_json = "{\"text\":\"\",\"tool_calls\":1}"},
      ava::session::SessionEntry{.id = "tool_turn_call",
                                 .parent_id = "",
                                 .type = ava::session::EntryType::ToolCall,
                                 .timestamp = "2026-05-01T00:00:02Z",
                                 .data_json = "{\"call_id\":\"call_safe_tail\",\"name\":\"read_file\",\"arguments\":\"{}\"}"},
      ava::session::SessionEntry{.id = "tool_turn_result",
                                 .parent_id = "",
                                 .type = ava::session::EntryType::ToolResult,
                                 .timestamp = "2026-05-01T00:00:03Z",
                                 .data_json = "{\"call_id\":\"call_safe_tail\",\"name\":\"read_file\",\"success\":true,\"result\":\"SAFE_RESULT\"}"}};
  auto prepared = ava::app::prepare_compaction_context(entries, config);
  expect(prepared && prepared->recent_context.find("Tool call (read_file)") != std::string::npos &&
             prepared->recent_context.find("SAFE_RESULT") != std::string::npos &&
             prepared->recent_context.find("Tool call (read_file)") < prepared->recent_context.find("SAFE_RESULT") &&
             prepared->recent_context.find("call_safe_tail") == std::string::npos,
         "portable compaction retention expands a selected tool result backward while removing persisted call ids");
}

void test_app_compaction_recent_tail_budget_never_orphans_tools()
{
  auto config = ava::session::default_compaction_config();
  config.keep_recent_messages = 1;
  config.keep_recent_messages_explicit = true;
  config.keep_recent_tokens = 1000;
  std::vector<ava::session::SessionEntry> const entries = {
      ava::session::SessionEntry{.id = "budget_tool_assistant",
                                 .parent_id = "",
                                 .type = ava::session::EntryType::AssistantMessage,
                                 .timestamp = "2026-05-01T00:00:00Z",
                                 .data_json = "{\"text\":\"\",\"tool_calls\":1}"},
      ava::session::SessionEntry{.id = "budget_tool_call",
                                 .parent_id = "",
                                 .type = ava::session::EntryType::ToolCall,
                                 .timestamp = "2026-05-01T00:00:01Z",
                                 .data_json = "{\"call_id\":\"call_exact_budget\",\"name\":\"read_file\",\"arguments\":\"{\\\"path\\\":\\\"large.cpp\\\"}\"}"},
      ava::session::SessionEntry{.id = "budget_tool_result",
                                 .parent_id = "",
                                 .type = ava::session::EntryType::ToolResult,
                                 .timestamp = "2026-05-01T00:00:02Z",
                                 .data_json = "{\"call_id\":\"call_exact_budget\",\"name\":\"read_file\",\"success\":true,\"result\":\"EXACT_TOOL_RESULT\"}"}};
  auto unbounded = ava::app::prepare_compaction_context(entries, config);
  auto const exact_tokens = unbounded ? ava::session::estimate_tokens(unbounded->recent_context) : 0;
  config.keep_recent_tokens = exact_tokens;
  auto exact = ava::app::prepare_compaction_context(entries, config);
  config.keep_recent_tokens = exact_tokens > 0 ? exact_tokens - 1 : 0;
  auto tight = ava::app::prepare_compaction_context(entries, config);
  expect(exact && !exact->recent_context_omitted && exact->recent_context.find("Tool call (read_file)") != std::string::npos &&
             exact->recent_context.find("EXACT_TOOL_RESULT") != std::string::npos && exact->recent_context.find("call_exact_budget") == std::string::npos &&
             ava::session::estimate_tokens(exact->recent_context) <= exact_tokens,
         "an exact recent-context budget retains a complete portable tool call/result group");
  expect(tight && tight->recent_context_omitted && tight->recent_context.find("Tool call (read_file)") == std::string::npos &&
             tight->recent_context.find("EXACT_TOOL_RESULT") == std::string::npos &&
             ava::session::estimate_tokens(tight->recent_context) <= config.keep_recent_tokens,
         "a budget too tight for a structured tool group omits the whole group instead of retaining an orphan or truncating JSON");
}

void test_app_compaction_oversized_turn_retains_latest_user_anchor()
{
  auto config = ava::session::default_compaction_config();
  config.keep_recent_tokens = 40;
  config.keep_recent_turns = 1;
  std::vector<ava::session::SessionEntry> const entries = {
      ava::session::SessionEntry{.id = "huge_latest_user",
                                 .parent_id = "",
                                 .type = ava::session::EntryType::UserMessage,
                                 .timestamp = "2026-05-01T00:00:00Z",
                                 .data_json = "{\"text\":\"LATEST_USER_ANCHOR " + std::string(2000, 'x') + "\"}"},
      ava::session::SessionEntry{.id = "short_latest_assistant",
                                 .parent_id = "",
                                 .type = ava::session::EntryType::AssistantMessage,
                                 .timestamp = "2026-05-01T00:00:01Z",
                                 .data_json = "{\"text\":\"LATEST_ASSISTANT_SUFFIX\"}"}};
  auto prepared = ava::app::prepare_compaction_context(entries, config);
  auto const user = prepared ? prepared->recent_context.find("LATEST_USER_ANCHOR") : std::string::npos;
  auto const assistant = prepared ? prepared->recent_context.find("LATEST_ASSISTANT_SUFFIX") : std::string::npos;
  expect(prepared && prepared->recent_context_omitted && user != std::string::npos && assistant != std::string::npos && user < assistant &&
             prepared->recent_context.find("plain text truncated") != std::string::npos &&
             ava::session::estimate_tokens(prepared->recent_context) <= config.keep_recent_tokens,
         "an oversized complete turn keeps a recognizable truncated latest-user anchor before the assistant suffix");
}

void test_app_manual_compaction_uses_only_active_context()
{
  auto const root = temp_root() / "app-manual-compact-active-context";
  std::error_code remove_error;
  std::filesystem::remove_all(root, remove_error);
  auto const workspace = root / "workspace";
  std::filesystem::create_directories(workspace);
  ava::app::runtime::OpenContext options;
  options.workspace_dir = workspace;
  options.current_dir = workspace;
  options.paths = app_test_paths(root);
  auto unlocked_session_result = ava::app::runtime::Session::open(options);
  expect(unlocked_session_result.has_value(), "active-context manual /compact test opens runtime session");
  if (!unlocked_session_result)
    return;

  ava::app::runtime::session_ts& unlocked_session = *unlocked_session_result;
  CRITICAL_AREA_BEGIN_W(session);

  static_cast<void>(session_w->append_owned(ava::session::SessionEntry{.id = "replaced_old_user",
                                                                       .parent_id = "",
                                                                       .type = ava::session::EntryType::UserMessage,
                                                                       .timestamp = "2026-05-01T00:00:00Z",
                                                                       .data_json = "{\"text\":\"REPLACED_OLD_CONTEXT\"}"}));
  static_cast<void>(
      session_w->append_owned(ava::session::SessionEntry{.id = "existing_boundary",
                                                         .parent_id = "",
                                                         .type = ava::session::EntryType::Compaction,
                                                         .timestamp = "2026-05-01T00:00:01Z",
                                                         .data_json = "{\"summary\":\"EXISTING_ACTIVE_SUMMARY\",\"history_projection\":\"portable-v1\"}"}));
  static_cast<void>(session_w->append_owned(ava::session::SessionEntry{.id = "active_new_user",
                                                                       .parent_id = "",
                                                                       .type = ava::session::EntryType::UserMessage,
                                                                       .timestamp = "2026-05-01T00:00:02Z",
                                                                       .data_json = "{\"text\":\"ACTIVE_NEW_CONTEXT\"}"}));

  bool saw_active_projection = false;
  CRITICAL_AREA_END_W(session);
  auto compact = ava::app::run_command(
      unlocked_session, ava::app::CommandRequest{
                      .command = "/compact",
                      .compaction_summary_generator = [&](std::vector<ava::session::SessionEntry> const& entries, ava::session::CompactionConfig const&,
                                                          std::string_view, std::size_t estimated_tokens) -> ava::core::Result<std::string> {
                        saw_active_projection = entries.size() == 2 && entries.front().type == ava::session::EntryType::UserMessage &&
                                                entries.front().data_json.find("EXISTING_ACTIVE_SUMMARY") != std::string::npos &&
                                                entries.back().data_json.find("ACTIVE_NEW_CONTEXT") != std::string::npos && estimated_tokens > 0;
                        return std::string("NEXT ACTIVE SUMMARY");
                      }});
  CRITICAL_AREA_CONTINUE_W(session);
  auto entries = session_w->store.load();
  auto const checkpoint = entries ? latest_compaction_entry(*entries) : std::nullopt;
  auto const recent = checkpoint ? ava::core::json::string_field(checkpoint->data_json, "recent_context").value_or("") : std::string{};
  expect(compact && saw_active_projection && checkpoint && recent.find("ACTIVE_NEW_CONTEXT") != std::string::npos &&
             recent.find("REPLACED_OLD_CONTEXT") == std::string::npos,
         "manual /compact shares active-boundary input and recent-tail selection without re-summarizing replaced physical history");
}

void test_app_cross_provider_compaction_requires_process_scope()
{
  auto const root = temp_root() / "app-compact-cross-provider-no-scope";
  std::error_code remove_error;
  std::filesystem::remove_all(root, remove_error);
  auto const workspace = root / "workspace";
  auto const paths = app_test_paths(root);
  std::filesystem::create_directories(workspace);

  auto const* prior_key = std::getenv("ANTHROPIC_API_KEY");
  auto const saved_key = prior_key ? std::optional<std::string>(prior_key) : std::nullopt;
  setenv("ANTHROPIC_API_KEY", "test-anthropic-key", 1);

  ava::app::runtime::OpenContext options;
  options.workspace_dir = workspace;
  options.current_dir = workspace;
  options.paths = paths;
  auto unlocked_session_result = ava::app::runtime::Session::open(options);
  expect(unlocked_session_result.has_value(), "cross-provider missing-scope test opens runtime session");
  if (!unlocked_session_result)
  {
    if (saved_key)
      setenv("ANTHROPIC_API_KEY", saved_key->c_str(), 1);
    else
      unsetenv("ANTHROPIC_API_KEY");
    return;
  }

  ava::app::runtime::session_ts& unlocked_session = *unlocked_session_result;
  CRITICAL_AREA_BEGIN_W(session);
  auto entries = session_w->store.load();
  auto config = ava::session::parse_compaction_config(R"({"provider":"anthropic","model":"claude-sonnet-4-5"})");
  ava::provider::OpenAIProvider const active_provider("https://api.example.test");
  ava::tests::FakeTransport transport({ava::http::HttpResponse{
      .status_code = 200, .headers = {}, .body = R"({"content":[{"type":"text","text":"MUST NOT DISPATCH"}],"stop_reason":"end_turn"})"}});
  ava::app::runtime::RunOptions run_options;
  run_options.access_token = "active-openai-token";
  CRITICAL_AREA_END_W(session);

  auto generated = entries && config
                       ? ava::app::generate_compaction_summary(unlocked_session, *entries, *config, "", 1, active_provider, transport, run_options)
                       : ava::core::Result<std::string>(std::unexpected(ava::core::Error(ava::core::ErrorCategory::Unknown, "invalid test setup")));
  if (saved_key)
    setenv("ANTHROPIC_API_KEY", saved_key->c_str(), 1);
  else
    unsetenv("ANTHROPIC_API_KEY");

  expect(!generated && generated.error().message() == "compaction process authority is unavailable" && transport.requests().empty(),
         "cross-provider compaction without captured session process authority fails before auth or provider transport dispatch");
}

void test_app_compact_honors_cross_provider_selection()
{
  auto const root = temp_root() / "app-compact-cross-provider";
  std::error_code remove_error;
  std::filesystem::remove_all(root, remove_error);
  auto const workspace = root / "workspace";
  auto const paths = app_test_paths(root);
  std::filesystem::create_directories(workspace);
  std::filesystem::create_directories(paths.ava_config_dir);
  {
    std::ofstream file(paths.compaction_file, std::ios::binary | std::ios::trunc);
    file << R"({"provider":"anthropic","model":"claude-sonnet-4-5","auto_threshold_tokens":0})";
  }
  auto const* prior_key = std::getenv("ANTHROPIC_API_KEY");
  auto const saved_key = prior_key ? std::optional<std::string>(prior_key) : std::nullopt;
  setenv("ANTHROPIC_API_KEY", "test-anthropic-key", 1);

  auto supervisor = std::make_shared<ava::process::Supervisor>();
  auto process_scope = ava::process::ProcessScopeV1::application(supervisor);
  expect(process_scope.has_value(), "cross-provider /compact test creates explicit process authority");
  if (!process_scope)
  {
    if (saved_key)
      setenv("ANTHROPIC_API_KEY", saved_key->c_str(), 1);
    else
      unsetenv("ANTHROPIC_API_KEY");
    return;
  }

  ava::app::runtime::OpenContext options;
  options.workspace_dir = workspace;
  options.current_dir = workspace;
  options.paths = paths;
  options.application_process_scope = *process_scope;
  auto unlocked_session_result = ava::app::runtime::Session::open(options);
  expect(unlocked_session_result.has_value(), "cross-provider /compact test opens runtime session");
  if (!unlocked_session_result)
  {
    if (saved_key)
      setenv("ANTHROPIC_API_KEY", saved_key->c_str(), 1);
    else
      unsetenv("ANTHROPIC_API_KEY");
    return;
  }

  ava::app::runtime::session_ts& unlocked_session = *unlocked_session_result;
  CRITICAL_AREA_BEGIN_W(session);

  static_cast<void>(session_w->append_owned(ava::session::SessionEntry{.id = "cross_provider_user",
                                                                       .parent_id = "",
                                                                       .type = ava::session::EntryType::UserMessage,
                                                                       .timestamp = ava::session::now_timestamp(),
                                                                       .data_json = "{\"text\":\"cross provider source\"}"}));

  ava::provider::OpenAIProvider const active_provider("https://api.example.test");
  ava::tests::FakeTransport transport({ava::http::HttpResponse{
      .status_code = 200, .headers = {}, .body = R"({"content":[{"type":"text","text":"CROSS PROVIDER SUMMARY"}],"stop_reason":"end_turn"})"}});
  ava::app::runtime::RunOptions run_options;
  run_options.access_token = "active-openai-token";
  CRITICAL_AREA_END_W(session);
  auto compact = ava::app::run_command(
      unlocked_session, ava::app::CommandRequest{
                      .command = "/compact",
                      .compaction_summary_generator = [&](std::vector<ava::session::SessionEntry> const& entries, ava::session::CompactionConfig const& config,
                                                          std::string_view instructions, std::size_t estimated_tokens) {
                        return ava::app::generate_compaction_summary(unlocked_session, entries, config, instructions, estimated_tokens, active_provider, transport,
                                                                      run_options);
                      }});
  if (saved_key)
    setenv("ANTHROPIC_API_KEY", saved_key->c_str(), 1);
  else
    unsetenv("ANTHROPIC_API_KEY");

  CRITICAL_AREA_CONTINUE_W(session);
  auto entries = session_w->store.load();
  auto const compaction = entries ? latest_compaction_entry(*entries) : std::nullopt;
  auto const process_snapshot = supervisor->snapshot();
  expect(compact && transport.requests().size() == 1 && transport.requests().front().url.find("anthropic.com") != std::string::npos &&
             transport.requests().front().body.find("claude-sonnet-4-5") != std::string::npos && compaction && process_snapshot.records.empty() &&
             ava::core::json::string_field(compaction->data_json, "provider") == "anthropic" &&
             ava::core::json::string_field(compaction->data_json, "model") == "claude-sonnet-4-5",
         "/compact isolates credential transport and dispatches only the exact configured cross-provider summary model");
}

void test_app_auto_compaction_appends_summary_and_rebuilds_context()
{
  auto const root = create_empty_root("app-auto-compact");

  auto const workspace = root / "workspace";
  auto const paths = app_test_paths(root);
  std::filesystem::create_directories(workspace);

  ava::app::runtime::OpenContext open_context;
  open_context.workspace_dir = workspace;
  open_context.current_dir = workspace;
  open_context.paths = paths;
  auto unlocked_session_result = ava::app::runtime::Session::open(open_context);
  expect(unlocked_session_result.has_value(), "auto compaction test opens runtime session");
  if (!unlocked_session_result)
    return;
  ava::app::runtime::session_ts& unlocked_session = *unlocked_session_result;

  CRITICAL_AREA_BEGIN_W(session);

  session_w->model_selection().model.context_window_tokens = 100;

  std::string const old_context = "old context marker " + std::string(420, 'x');
  static_cast<void>(session_w->append_owned(ava::session::SessionEntry{.id = "entry_old_user",
                                                                       .parent_id = "",
                                                                       .type = ava::session::EntryType::UserMessage,
                                                                       .timestamp = ava::session::now_timestamp(),
                                                                       .data_json = "{\"text\":\"" + ava::core::json::escape(old_context) + "\"}"}));
  for (int index = 0; index < 6; ++index)
  {
    static_cast<void>(session_w->append_owned(ava::session::SessionEntry{.id = "entry_recent_" + std::to_string(index),
                                                                         .parent_id = "",
                                                                         .type = ava::session::EntryType::AssistantMessage,
                                                                         .timestamp = ava::session::now_timestamp(),
                                                                         .data_json = "{\"text\":\"recent filler " + std::to_string(index) + "\"}"}));
  }

  ava::provider::OpenAIProvider const provider("https://api.example.test");
  ava::tests::FakeTransport transport({ava::http::HttpResponse{.status_code = 200, .headers = {}, .body = "{\"output_text\":\"AUTO SUMMARY\"}"},
                                       sse_response(final_text_sse("compacted answer"))});
  ava::app::runtime::RunOptions run_options;
  run_options.access_token = "token";

  CRITICAL_AREA_END_W(session);

  auto result = ava::app::run_prompt(unlocked_session, "continue after compaction", provider, transport, run_options);

  CRITICAL_AREA_CONTINUE_W(session);

  auto entries = session_w->store.load();
  auto const compaction = entries ? latest_compaction_entry(*entries) : std::nullopt;
  expect(result && result->final_text == "compacted answer", "auto compaction prompt succeeds");
  expect(transport.requests().size() == 2, "auto compaction performs summary request then provider request");
  expect(transport.requests().size() == 2 && transport.requests()[0].body.find("old context marker") != std::string::npos,
         "auto compaction summary request sees pre-compaction context");
  expect(transport.requests().size() == 2 && transport.requests()[1].body.find("AUTO SUMMARY") != std::string::npos &&
             transport.requests()[1].body.find("\"content\":\"continue after compaction\"") != std::string::npos &&
             transport.requests()[1].body.find("old context marker") != std::string::npos,
         "provider request is rebuilt from the new compaction boundary with the configured recent turn and active prompt");
  expect(compaction && compaction->data_json.find("\"trigger\":\"auto\"") != std::string::npos &&
             compaction->data_json.find("\"summary\":\"AUTO SUMMARY\"") != std::string::npos &&
             compaction->data_json.find("\"threshold_tokens\":80") != std::string::npos &&
             compaction->data_json.find("\"keep_recent_turns\":2") != std::string::npos &&
             compaction->data_json.find("\"provider\":\"openai\"") != std::string::npos &&
             compaction->data_json.find("\"model\":\"gpt-5.5\"") != std::string::npos,
         "auto compaction entry records trigger, summary, threshold, retention, and model metadata");
}

void test_app_auto_compaction_recent_context_respects_token_budget()
{
  auto const root = create_empty_root("app-auto-compact-recent-budget");

  auto const workspace = root / "workspace";
  auto const paths = app_test_paths(root);
  std::filesystem::create_directories(workspace);
  std::filesystem::create_directories(paths.ava_config_dir);
  {
    std::ofstream file(paths.compaction_file, std::ios::binary | std::ios::trunc);
    file << "{\"auto_threshold_tokens\":1,\"keep_recent_tokens\":20,\"keep_recent_messages\":8}";
  }

  ava::app::runtime::OpenContext open_context;
  open_context.workspace_dir = workspace;
  open_context.current_dir = workspace;
  open_context.paths = paths;
  auto unlocked_session_result = ava::app::runtime::Session::open(open_context);
  expect(unlocked_session_result.has_value(), "recent context token budget test opens runtime session");
  if (!unlocked_session_result)
    return;
  ava::app::runtime::session_ts& unlocked_session = *unlocked_session_result;

  CRITICAL_AREA_BEGIN_W(session);

  session_w->model_selection().model.context_window_tokens = 1000;
  for (int index = 0; index < 4; ++index)
  {
    static_cast<void>(session_w->append_owned(
        ava::session::SessionEntry{.id = "entry_budget_" + std::to_string(index),
                                   .parent_id = "",
                                   .type = ava::session::EntryType::UserMessage,
                                   .timestamp = ava::session::now_timestamp(),
                                   .data_json = "{\"text\":\"budget filler " + std::to_string(index) + " " + std::string(160, 'b') + "\"}"}));
  }

  ava::provider::OpenAIProvider const provider("https://api.example.test");
  ava::tests::FakeTransport transport({ava::http::HttpResponse{.status_code = 200, .headers = {}, .body = "{\"output_text\":\"BUDGET SUMMARY\"}"},
                                       sse_response(final_text_sse("budget answer"))});
  ava::app::runtime::RunOptions run_options;
  run_options.access_token = "token";

  CRITICAL_AREA_END_W(session);

  auto result = ava::app::run_prompt(unlocked_session, "after budget compaction", provider, transport, run_options);

  CRITICAL_AREA_CONTINUE_W(session);

  auto entries = session_w->store.load();
  auto const compaction = entries ? latest_compaction_entry(*entries) : std::nullopt;
  auto const recent_context = compaction ? ava::core::json::string_field(compaction->data_json, "recent_context") : std::optional<std::string>{};
  expect(result && result->final_text == "budget answer", "recent context token budget prompt succeeds");
  expect(recent_context && recent_context->find("recent context tail truncated") != std::string::npos && ava::session::estimate_tokens(*recent_context) <= 20,
         "auto compaction stores recent context bounded by keep_recent_tokens with an explicit marker");
}

void test_app_auto_compaction_recent_context_truncates_utf8_safely()
{
  auto const root = create_empty_root("app-auto-compact-recent-utf8");

  auto const workspace = root / "workspace";
  auto const paths = app_test_paths(root);
  std::filesystem::create_directories(workspace);
  std::filesystem::create_directories(paths.ava_config_dir);
  {
    std::ofstream file(paths.compaction_file, std::ios::binary | std::ios::trunc);
    file << "{\"auto_threshold_tokens\":1,\"keep_recent_tokens\":20,\"keep_recent_messages\":8}";
  }

  ava::app::runtime::OpenContext open_context;
  open_context.workspace_dir = workspace;
  open_context.current_dir = workspace;
  open_context.paths = paths;
  auto unlocked_session_result = ava::app::runtime::Session::open(open_context);
  expect(unlocked_session_result.has_value(), "recent context UTF-8 truncation test opens runtime session");
  if (!unlocked_session_result)
    return;
  ava::app::runtime::session_ts& unlocked_session = *unlocked_session_result;

  CRITICAL_AREA_BEGIN_W(session);

  session_w->model_selection().model.context_window_tokens = 1000;

  std::string emoji_tail;
  for (int index = 0; index < 80; ++index) emoji_tail += "\xF0\x9F\x98\x80";
  static_cast<void>(session_w->append_owned(ava::session::SessionEntry{.id = "entry_utf8_budget",
                                                                       .parent_id = "",
                                                                       .type = ava::session::EntryType::UserMessage,
                                                                       .timestamp = ava::session::now_timestamp(),
                                                                       .data_json = "{\"text\":\"" + ava::core::json::escape(emoji_tail) + "\"}"}));

  ava::provider::OpenAIProvider const provider("https://api.example.test");
  ava::tests::FakeTransport transport(
      {ava::http::HttpResponse{.status_code = 200, .headers = {}, .body = "{\"output_text\":\"UTF8 SUMMARY\"}"}, sse_response(final_text_sse("utf8 answer"))});
  ava::app::runtime::RunOptions run_options;
  run_options.access_token = "token";

  CRITICAL_AREA_END_W(session);

  auto result = ava::app::run_prompt(unlocked_session, "after utf8 compaction", provider, transport, run_options);

  CRITICAL_AREA_CONTINUE_W(session);

  auto entries = session_w->store.load();
  auto const compaction = entries ? latest_compaction_entry(*entries) : std::nullopt;
  auto const recent_context = compaction ? ava::core::json::string_field(compaction->data_json, "recent_context") : std::optional<std::string>{};
  expect(result && result->final_text == "utf8 answer", "recent context UTF-8 prompt succeeds");
  auto const marker_end = recent_context ? recent_context->find('\n') : std::string::npos;
  bool const suffix_starts_on_codepoint = recent_context && marker_end != std::string::npos && marker_end + 1 < recent_context->size() &&
                                          (static_cast<unsigned char>((*recent_context)[marker_end + 1]) & 0xC0U) != 0x80U;
  expect(recent_context && recent_context->find("recent context tail truncated") != std::string::npos && suffix_starts_on_codepoint,
         "recent context truncation starts UTF-8 suffix on a code point boundary");
}

void test_app_auto_compaction_explicit_zero_disables()
{
  auto const root = create_empty_root("app-auto-compact-disabled");

  auto const workspace = root / "workspace";
  auto const paths = app_test_paths(root);
  std::filesystem::create_directories(workspace);
  std::filesystem::create_directories(paths.ava_config_dir);
  {
    std::ofstream file(paths.compaction_file, std::ios::binary | std::ios::trunc);
    file << "{\"auto_threshold_tokens\":0}";
  }

  ava::app::runtime::OpenContext open_context;
  open_context.workspace_dir = workspace;
  open_context.current_dir = workspace;
  open_context.paths = paths;
  auto unlocked_session_result = ava::app::runtime::Session::open(open_context);
  expect(unlocked_session_result.has_value(), "disabled auto compaction test opens runtime session");
  if (!unlocked_session_result)
    return;
  ava::app::runtime::session_ts& unlocked_session = *unlocked_session_result;

  CRITICAL_AREA_BEGIN_W(session);

  session_w->model_selection().model.context_window_tokens = 10;
  static_cast<void>(session_w->append_owned(ava::session::SessionEntry{.id = "entry_big_user",
                                                                       .parent_id = "",
                                                                       .type = ava::session::EntryType::UserMessage,
                                                                       .timestamp = ava::session::now_timestamp(),
                                                                       .data_json = "{\"text\":\"" + std::string(240, 'd') + "\"}"}));

  ava::provider::OpenAIProvider const provider("https://api.example.test");
  ava::tests::FakeTransport transport({sse_response(final_text_sse("no compact answer"))});
  ava::app::runtime::RunOptions run_options;
  run_options.access_token = "token";

  CRITICAL_AREA_END_W(session);

  auto result = ava::app::run_prompt(unlocked_session, "do not compact", provider, transport, run_options);

  CRITICAL_AREA_CONTINUE_W(session);

  auto entries = session_w->store.load();
  expect(result && result->final_text == "no compact answer", "explicit disabled auto compaction prompt succeeds");
  expect(transport.requests().size() == 1, "explicit disabled auto compaction does not call summary provider");
  expect(entries && count_compaction_entries(*entries) == 0, "explicit disabled auto compaction appends no compaction");
}

void test_app_auto_compaction_uses_default_threshold_without_context_window_metadata()
{
  auto const root = create_empty_root("app-auto-compact-default-threshold");

  auto const workspace = root / "workspace";
  auto const paths = app_test_paths(root);
  std::filesystem::create_directories(workspace);

  ava::app::runtime::OpenContext open_context;
  open_context.workspace_dir = workspace;
  open_context.current_dir = workspace;
  open_context.paths = paths;
  auto unlocked_session_result = ava::app::runtime::Session::open(open_context);
  expect(unlocked_session_result.has_value(), "default threshold auto compaction test opens runtime session");
  if (!unlocked_session_result)
    return;
  ava::app::runtime::session_ts& unlocked_session = *unlocked_session_result;

  CRITICAL_AREA_BEGIN_W(session);

  session_w->model_selection().model.context_window_tokens = std::nullopt;

  auto const config = ava::session::default_compaction_config();
  auto const threshold = ava::session::effective_auto_threshold_tokens(config, std::nullopt);
  static_cast<void>(session_w->append_owned(ava::session::SessionEntry{.id = "entry_default_threshold_big",
                                                                       .parent_id = "",
                                                                       .type = ava::session::EntryType::UserMessage,
                                                                       .timestamp = ava::session::now_timestamp(),
                                                                       .data_json = "{\"text\":\"" + std::string(threshold * 4, 'f') + "\"}"}));

  ava::provider::OpenAIProvider const provider("https://api.example.test");
  ava::tests::FakeTransport transport({ava::http::HttpResponse{.status_code = 200, .headers = {}, .body = "{\"output_text\":\"DEFAULT SUMMARY\"}"},
                                       sse_response(final_text_sse("default compact answer"))});
  ava::app::runtime::RunOptions run_options;
  run_options.access_token = "token";

  CRITICAL_AREA_END_W(session);

  auto result = ava::app::run_prompt(unlocked_session, "default threshold prompt", provider, transport, run_options);

  CRITICAL_AREA_CONTINUE_W(session);

  auto entries = session_w->store.load();
  expect(result && result->final_text == "default compact answer", "default threshold auto compaction prompt succeeds");
  expect(transport.requests().size() == 2, "default threshold auto compaction performs a summary request before provider request");
  expect(entries && count_compaction_entries(*entries) == 1, "default threshold auto compaction appends a compaction entry without model context metadata");
  expect(transport.requests().size() == 2 && transport.requests()[1].body.find("\"content\":\"default threshold prompt\"") != std::string::npos,
         "default threshold auto compaction keeps the active prompt as a normal user message");
}

void test_app_auto_compaction_retries_stale_snapshot_before_append()
{
  auto const root = create_empty_root("app-auto-compact-revalidate");

  auto const workspace = root / "workspace";
  auto const paths = app_test_paths(root);
  std::filesystem::create_directories(workspace);

  ava::app::runtime::OpenContext open_context;
  open_context.workspace_dir = workspace;
  open_context.current_dir = workspace;
  open_context.paths = paths;
  auto unlocked_session_result = ava::app::runtime::Session::open(open_context);
  expect(unlocked_session_result.has_value(), "auto compaction revalidation test opens runtime session");
  if (!unlocked_session_result)
    return;
  ava::app::runtime::session_ts& unlocked_session = *unlocked_session_result;

  CRITICAL_AREA_BEGIN_W(session);

  session_w->model_selection().model.context_window_tokens = 100;

  static_cast<void>(session_w->append_owned(ava::session::SessionEntry{.id = "entry_revalidate_big",
                                                                       .parent_id = "",
                                                                       .type = ava::session::EntryType::UserMessage,
                                                                       .timestamp = ava::session::now_timestamp(),
                                                                       .data_json = "{\"text\":\"" + std::string(420, 'r') + "\"}"}));

  ava::provider::OpenAIProvider const provider("https://api.example.test");
  MutatingSummaryTransport transport(session_w->owner_append_route_1(),
                                     {ava::http::HttpResponse{.status_code = 200, .headers = {}, .body = "{\"output_text\":\"STALE SUMMARY\"}"},
                                      ava::http::HttpResponse{.status_code = 200, .headers = {}, .body = "{\"output_text\":\"RETRIED SUMMARY\"}"},
                                      sse_response(final_text_sse("retry after stale"))});
  ava::app::runtime::RunOptions run_options;
  run_options.access_token = "token";
  std::vector<ava::event::RuntimeEvent> events;
  run_options.event_sink = [&events](ava::event::RuntimeEvent const& event) {
    events.push_back(event);
    return ava::core::VoidResult{};
  };

  CRITICAL_AREA_END_W(session);

  auto result = ava::app::run_prompt(unlocked_session, "retry stale summary", provider, transport, run_options);

  CRITICAL_AREA_CONTINUE_W(session);

  auto entries = session_w->store.load();
  expect(result && result->final_text == "retry after stale", "auto compaction retries a stale snapshot and continues after a fresh summary");
  expect(transport.requests().size() == 3, "stale auto compaction regenerates one summary before the provider request");
  expect(entries && count_compaction_entries(*entries) == 1, "stale auto compaction appends only the summary generated from the fresh snapshot");
  auto const compaction = entries ? latest_compaction_entry(*entries) : std::nullopt;
  expect(compaction && compaction->data_json.find("RETRIED SUMMARY") != std::string::npos && compaction->data_json.find("STALE SUMMARY") == std::string::npos,
         "auto compaction discards the stale summary instead of recording it");
  expect(entries && std::ranges::any_of(*entries,
                                        [](ava::session::SessionEntry const& entry) { return entry.data_json.find("concurrent change") != std::string::npos; }),
         "auto compaction retry test introduced a concurrent session change");
  expect(std::ranges::any_of(events,
                             [](ava::event::RuntimeEvent const& event) {
                               auto const* retry = ava::tests::runtime_event_as<ava::event::RetryEvent>(event);
                               return retry && retry->payload.reason == "stale_compaction_snapshot" && retry->payload.trigger == "auto" &&
                                      retry->payload.attempt == 2 && retry->payload.max_attempts == 2 && retry->diagnostics.snapshot_entries > 0 &&
                                      retry->diagnostics.current_entries > retry->diagnostics.snapshot_entries;
                             }),
         "stale auto compaction emits RetryEvent with internal snapshot diagnostics for typed live consumers");
}

void test_app_auto_compaction_repeated_stale_snapshot_fails_without_append()
{
  auto const root = create_empty_root("app-auto-compact-repeated-stale");

  auto const workspace = root / "workspace";
  auto const paths = app_test_paths(root);
  std::filesystem::create_directories(workspace);

  ava::app::runtime::OpenContext open_context;
  open_context.workspace_dir = workspace;
  open_context.current_dir = workspace;
  open_context.paths = paths;
  auto unlocked_session_result = ava::app::runtime::Session::open(open_context);
  expect(unlocked_session_result.has_value(), "repeated stale auto compaction test opens runtime session");
  if (!unlocked_session_result)
    return;
  ava::app::runtime::session_ts& unlocked_session = *unlocked_session_result;

  CRITICAL_AREA_BEGIN_W(session);

  session_w->model_selection().model.context_window_tokens = 100;

  static_cast<void>(session_w->append_owned(ava::session::SessionEntry{.id = "entry_repeated_stale_big",
                                                                       .parent_id = "",
                                                                       .type = ava::session::EntryType::UserMessage,
                                                                       .timestamp = ava::session::now_timestamp(),
                                                                       .data_json = "{\"text\":\"" + std::string(420, 's') + "\"}"}));

  ava::provider::OpenAIProvider const provider("https://api.example.test");
  MutatingSummaryTransport transport(session_w->owner_append_route_1(),
                                     {ava::http::HttpResponse{.status_code = 200, .headers = {}, .body = "{\"output_text\":\"STALE ONE\"}"},
                                      ava::http::HttpResponse{.status_code = 200, .headers = {}, .body = "{\"output_text\":\"STALE TWO\"}"}},
                                     2);
  ava::app::runtime::RunOptions run_options;
  run_options.access_token = "token";

  CRITICAL_AREA_END_W(session);

  auto result = ava::app::run_prompt(unlocked_session, "repeated stale", provider, transport, run_options);

  CRITICAL_AREA_CONTINUE_W(session);

  auto entries = session_w->store.load();
  expect(!result && result.error().message().find("session changed during context compaction") != std::string::npos,
         "auto compaction returns a clear stale snapshot error after bounded retries are exhausted");
  expect(transport.requests().size() == 2, "repeated stale auto compaction stops after two summary attempts");
  expect(entries && count_compaction_entries(*entries) == 0, "repeated stale auto compaction appends no summary from stale snapshots");
}

void test_app_context_overflow_compacts_and_retries_once_successfully()
{
  auto const root = create_empty_root("app-context-overflow-retry");

  auto const workspace = root / "workspace";
  auto const paths = app_test_paths(root);
  std::filesystem::create_directories(workspace);

  ava::app::runtime::OpenContext open_context;
  open_context.workspace_dir = workspace;
  open_context.current_dir = workspace;
  open_context.paths = paths;
  auto unlocked_session_result = ava::app::runtime::Session::open(open_context);
  expect(unlocked_session_result.has_value(), "context overflow retry test opens runtime session");
  if (!unlocked_session_result)
    return;
  ava::app::runtime::session_ts& unlocked_session = *unlocked_session_result;

  CRITICAL_AREA_BEGIN_W(session);

  ava::provider::OpenAIProvider const provider("https://api.example.test");
  ava::tests::FakeTransport transport(
      {ava::http::HttpResponse{.status_code = 400, .headers = {}, .body = "{\"error\":{\"message\":\"context length exceeded the token limit\"}}"},
       ava::http::HttpResponse{.status_code = 200, .headers = {}, .body = "{\"output_text\":\"OVERFLOW SUMMARY\"}"},
       sse_response(final_text_sse("retry answer"))});
  ava::app::runtime::RunOptions run_options;
  run_options.access_token = "token";
  std::vector<ava::event::RuntimeEvent> events;
  run_options.event_sink = [&events](ava::event::RuntimeEvent const& event) {
    events.push_back(event);
    return ava::core::VoidResult{};
  };

  CRITICAL_AREA_END_W(session);

  auto result = ava::app::run_prompt(unlocked_session, "overflow prompt", provider, transport, run_options);

  CRITICAL_AREA_CONTINUE_W(session);

  auto entries = session_w->store.load();
  auto const compaction = entries ? latest_compaction_entry(*entries) : std::nullopt;
  expect(result && result->final_text == "retry answer", "context overflow retry succeeds after compaction");
  expect(transport.requests().size() == 3, "context overflow performs original call, compaction, and one retry");
  expect(compaction && compaction->data_json.find("\"trigger\":\"context_overflow\"") != std::string::npos &&
             compaction->data_json.find("OVERFLOW SUMMARY") != std::string::npos,
         "context overflow retry appends a context_overflow compaction summary");
  expect(std::ranges::any_of(events,
                             [](ava::event::RuntimeEvent const& event) {
                               auto const* retry = ava::tests::runtime_event_as<ava::event::RetryEvent>(event);
                               return retry && retry->payload.reason == "context_overflow" && retry->payload.trigger == "context_overflow" &&
                                      retry->payload.attempt == 1 && retry->payload.max_attempts == 1 && retry->diagnostics.threshold_tokens > 0 &&
                                      retry->diagnostics.snapshot_entries == 0 && retry->diagnostics.current_entries == 0;
                             }) &&
             std::ranges::any_of(events,
                                 [](ava::event::RuntimeEvent const& event) {
                                   auto const* start = ava::tests::runtime_event_as<ava::event::CompactionStartEvent>(event);
                                   return start && start->payload.trigger == "context_overflow" && start->payload.attempt == 1 &&
                                          start->payload.max_attempts == 2;
                                 }) &&
             std::ranges::any_of(events,
                                 [](ava::event::RuntimeEvent const& event) {
                                   auto const* end = ava::tests::runtime_event_as<ava::event::CompactionEndEvent>(event);
                                   return end && end->payload.summary_bytes == std::string("OVERFLOW SUMMARY").size() && end->payload.attempt == 1 &&
                                          end->payload.max_attempts == 2;
                                 }),
         "context overflow retry emits backend lifecycle events for replaying TUI compaction and retry state");
  expect(transport.requests().size() == 3 && transport.requests()[2].body.find("OVERFLOW SUMMARY") != std::string::npos &&
             transport.requests()[2].body.find("\"content\":\"overflow prompt\"") != std::string::npos &&
             count_substrings(transport.requests()[2].body, "overflow prompt") == 1,
         "context overflow retry rebuilds provider context with one active prompt replay");
  auto const recent_context = compaction ? ava::core::json::string_field(compaction->data_json, "recent_context") : std::optional<std::string>{};
  expect(recent_context && recent_context->find("overflow prompt") == std::string::npos,
         "context overflow compaction excludes active prompts that will be replayed from recent context");
  expect(entries && std::ranges::count_if(*entries, ava::session::is_internal_replay_user_message) == 1,
         "context overflow compaction stores active prompt replay as an internal user message");
  auto const markdown = entries ? ava::session::format_session_markdown(*entries) : std::string{};
  auto const stats = entries ? ava::session::compute_session_stats(*entries) : ava::core::Result<ava::session::SessionStats>(ava::session::SessionStats{});
  expect(markdown.find("internal_replay") == std::string::npos && count_substrings(markdown, "overflow prompt") == 1 && stats->counts.user_message == 1,
         "consumer-facing export and stats hide internal active prompt replays");
}

void test_app_context_overflow_compaction_failure_leaves_no_partial_entry()
{
  auto const root = create_empty_root("app-context-overflow-compaction-fails");

  auto const workspace = root / "workspace";
  auto const paths = app_test_paths(root);
  std::filesystem::create_directories(workspace);

  ava::app::runtime::OpenContext open_context;
  open_context.workspace_dir = workspace;
  open_context.current_dir = workspace;
  open_context.paths = paths;
  auto unlocked_session_result = ava::app::runtime::Session::open(open_context);
  expect(unlocked_session_result.has_value(), "context overflow compaction failure test opens runtime session");
  if (!unlocked_session_result)
    return;
  ava::app::runtime::session_ts& unlocked_session = *unlocked_session_result;

  CRITICAL_AREA_BEGIN_W(session);

  ava::provider::OpenAIProvider const provider("https://api.example.test");
  ava::tests::FakeTransport transport(
      {ava::http::HttpResponse{.status_code = 400, .headers = {}, .body = "{\"error\":{\"message\":\"too many tokens for context window\"}}"},
       ava::http::HttpResponse{.status_code = 429, .headers = {}, .body = "{\"error\":{\"message\":\"summary quota exhausted\"}}"}});
  ava::app::runtime::RunOptions run_options;
  run_options.access_token = "token";

  CRITICAL_AREA_END_W(session);

  auto result = ava::app::run_prompt(unlocked_session, "overflow then summary fails", provider, transport, run_options);

  CRITICAL_AREA_CONTINUE_W(session);

  auto entries = session_w->store.load();
  expect(!result && result.error().message() == "context overflow compaction failed", "context overflow returns clear compaction failure");
  expect(!result && result.error().format().find("compaction_provider_status: 429") != std::string::npos &&
             result.error().format().find("summary quota exhausted") == std::string::npos,
         "context overflow compaction failure preserves only allowlisted provider status metadata");
  expect(transport.requests().size() == 2, "failed context overflow compaction does not retry provider call");
  expect(entries && count_compaction_entries(*entries) == 0, "failed context overflow compaction leaves no partial compaction entry");
}

void test_app_non_overflow_provider_error_does_not_compact_or_retry()
{
  auto const root = create_empty_root("app-non-overflow-error");

  auto const workspace = root / "workspace";
  auto const paths = app_test_paths(root);
  std::filesystem::create_directories(workspace);

  ava::app::runtime::OpenContext open_context;
  open_context.workspace_dir = workspace;
  open_context.current_dir = workspace;
  open_context.paths = paths;
  auto unlocked_session_result = ava::app::runtime::Session::open(open_context);
  expect(unlocked_session_result.has_value(), "non-overflow provider error test opens runtime session");
  if (!unlocked_session_result)
    return;
  ava::app::runtime::session_ts& unlocked_session = *unlocked_session_result;

  CRITICAL_AREA_BEGIN_W(session);

  ava::provider::OpenAIProvider const provider("https://api.example.test");
  ava::tests::FakeTransport transport({ava::http::HttpResponse{.status_code = 500, .headers = {}, .body = "server unavailable"}});
  ava::app::runtime::RunOptions run_options;
  run_options.access_token = "token";

  CRITICAL_AREA_END_W(session);

  auto result = ava::app::run_prompt(unlocked_session, "server error", provider, transport, run_options);

  CRITICAL_AREA_CONTINUE_W(session);

  auto entries = session_w->store.load();
  expect(!result && result.error().message().find("OpenAI HTTP request failed") != std::string::npos, "non-overflow provider error is returned");
  expect(transport.requests().size() == 1, "non-overflow provider error does not retry");
  expect(entries && count_compaction_entries(*entries) == 0, "non-overflow provider error does not compact");

  auto context_error = ava::core::Error(ava::core::ErrorCategory::Provider, "too many tokens for context window");
  auto auth_error = ava::core::Error(ava::core::ErrorCategory::Provider, "authentication failed");
  expect(ava::provider::is_context_overflow_error(context_error) && !ava::provider::is_context_overflow_error(auth_error),
         "context overflow helper distinguishes token-window errors from unrelated provider errors");
}

void test_app_compaction_projects_committed_v4_and_ignores_incomplete_staging()
{
  auto config = ava::session::default_compaction_config();
  auto item = [](std::string id, std::size_t sequence, ava::session::AssistantOutputItemPayload payload) {
    auto data = ava::session::serialize_assistant_output_item_data_json(ava::session::AssistantOutputItem{
        .assistant_turn_id = "turn_compaction",
        .sequence = sequence,
        .kind = std::holds_alternative<ava::session::AssistantOutputText>(payload)
                    ? ava::session::AssistantOutputItemKind::Text
                    : (std::holds_alternative<ava::session::AssistantOutputReasoning>(payload) ? ava::session::AssistantOutputItemKind::Reasoning
                                                                                               : ava::session::AssistantOutputItemKind::FunctionCall),
        .provider_item_id = std::nullopt,
        .provider_output_index = std::nullopt,
        .payload = std::move(payload)});
    return ava::session::SessionEntry{.id = std::move(id),
                                      .parent_id = "",
                                      .type = ava::session::EntryType::AssistantOutputItem,
                                      .timestamp = "2026-07-18T00:00:01Z",
                                      .data_json = data.value_or("{}")};
  };
  auto commit_data = ava::session::serialize_assistant_turn_commit_data_json(
      ava::session::AssistantTurnCommit{.assistant_turn_id = "turn_compaction",
                                        .item_count = 3,
                                        .provider = "openai",
                                        .model = "gpt-5.5",
                                        .finish_reason = "tool_calls",
                                        .usage_json = "{\"input_tokens\":1,\"output_tokens\":2,\"total_tokens\":3,\"source\":\"provider\"}"});
  std::vector<ava::session::SessionEntry> const entries = {
      ava::session::SessionEntry{.id = "safe_user",
                                 .parent_id = "",
                                 .type = ava::session::EntryType::UserMessage,
                                 .timestamp = "2026-07-18T00:00:00Z",
                                 .data_json = "{\"text\":\"safe compaction context\"}"},
      item("v4_reasoning", 0,
           ava::session::AssistantOutputReasoning{.text = "visible v4 reasoning",
                                                  .format = "openai_responses",
                                                  .redacted = false,
                                                  .signature = "V4_COMPACTION_PRIVATE_SIGNATURE",
                                                  .redacted_data = "V4_COMPACTION_PRIVATE_REDACTED",
                                                  .native_item_json = "{\"id\":\"rs_compaction\",\"type\":\"reasoning\",\"summary\":[]}"}),
      item("v4_function", 1,
           ava::session::AssistantOutputFunctionCall{.call_id = "call_compaction_v4", .name = "read_file", .arguments_json = "{\"path\":\"src/main.cpp\"}"}),
      item("v4_text", 2,
           ava::session::AssistantOutputText{.text = "visible v4 commentary", .assistant_phase = ava::session::AssistantOutputTextPhase::Commentary}),
      ava::session::SessionEntry{.id = "v4_commit",
                                 .parent_id = "",
                                 .type = ava::session::EntryType::AssistantTurnCommit,
                                 .timestamp = "2026-07-18T00:00:02Z",
                                 .data_json = commit_data.value_or("{}")},
      ava::session::SessionEntry{.id = "v4_result",
                                 .parent_id = "",
                                 .type = ava::session::EntryType::ToolResult,
                                 .timestamp = "2026-07-18T00:00:03Z",
                                 .data_json = "{\"assistant_output_entry_id\":\"v4_function\",\"call_id\":\"call_compaction_v4\",\"name\":\"read_file\","
                                              "\"success\":true,\"result\":\"BOUND_V4_RESULT\"}"},
      ava::session::SessionEntry{.id = "v4_staged_private",
                                 .parent_id = "",
                                 .type = ava::session::EntryType::AssistantOutputItem,
                                 .timestamp = "2026-07-18T00:00:04Z",
                                 .data_json = "{\"schema_version\":1,\"assistant_turn_id\":\"tail_compaction\",\"sequence\":0,\"kind\":\"text\",\"text\":\"V4_"
                                              "COMPACTION_STAGED_CANARY\",\"assistant_phase\":\"commentary\"}"},
  };

  auto const prompt = ava::app::build_compaction_summary_prompt(entries, config, "", 42);
  expect(prompt && prompt->find("safe compaction context") != std::string::npos && prompt->find("visible v4 commentary") != std::string::npos &&
             prompt->find("visible v4 reasoning") == std::string::npos && prompt->find("src/main.cpp") != std::string::npos &&
             prompt->find("V4_COMPACTION_PRIVATE_SIGNATURE") == std::string::npos && prompt->find("V4_COMPACTION_PRIVATE_REDACTED") == std::string::npos &&
             prompt->find("rs_compaction") == std::string::npos && prompt->find("V4_COMPACTION_STAGED_CANARY") == std::string::npos &&
             prompt->find("assistant_output_entry_id") == std::string::npos,
         "compaction prompt uses forced-portable committed v4 projection and excludes reasoning, private, and incomplete staging data");

  auto prepared = ava::app::prepare_compaction_context(entries, config);
  auto const function = prepared ? prepared->recent_context.find("Tool call (read_file)") : std::string::npos;
  auto const text = prepared ? prepared->recent_context.find("visible v4 commentary") : std::string::npos;
  auto const result = prepared ? prepared->recent_context.find("BOUND_V4_RESULT") : std::string::npos;
  expect(prepared && prepared->recent_context.find("visible v4 reasoning") == std::string::npos && function != std::string::npos && text != std::string::npos &&
             result != std::string::npos && function < text && text < result && prepared->recent_context.find("call_compaction_v4") == std::string::npos &&
             prepared->recent_context.find("V4_COMPACTION_PRIVATE_SIGNATURE") == std::string::npos &&
             prepared->recent_context.find("V4_COMPACTION_PRIVATE_REDACTED") == std::string::npos &&
             prepared->recent_context.find("V4_COMPACTION_STAGED_CANARY") == std::string::npos,
         "retained compaction tail keeps forced-portable function/text/result order while dropping reasoning and persisted call ids");
}

void test_app_context_overflow_retry_is_bounded()
{
  auto const root = create_empty_root("app-context-overflow-bounded");

  auto const workspace = root / "workspace";
  auto const paths = app_test_paths(root);
  std::filesystem::create_directories(workspace);

  ava::app::runtime::OpenContext open_context;
  open_context.workspace_dir = workspace;
  open_context.current_dir = workspace;
  open_context.paths = paths;
  auto unlocked_session_result = ava::app::runtime::Session::open(open_context);
  expect(unlocked_session_result.has_value(), "bounded overflow retry test opens runtime session");
  if (!unlocked_session_result)
    return;
  ava::app::runtime::session_ts& unlocked_session = *unlocked_session_result;

  CRITICAL_AREA_BEGIN_W(session);

  ava::provider::OpenAIProvider const provider("https://api.example.test");
  ava::tests::FakeTransport transport(
      {ava::http::HttpResponse{.status_code = 400, .headers = {}, .body = "{\"error\":{\"message\":\"context length exceeded token limit\"}}"},
       ava::http::HttpResponse{.status_code = 200, .headers = {}, .body = "{\"output_text\":\"BOUNDED SUMMARY\"}"},
       ava::http::HttpResponse{.status_code = 400, .headers = {}, .body = "{\"error\":{\"message\":\"context length exceeded token limit again\"}}"}});
  ava::app::runtime::RunOptions run_options;
  run_options.access_token = "token";

  CRITICAL_AREA_END_W(session);

  auto result = ava::app::run_prompt(unlocked_session, "overflow twice", provider, transport, run_options);

  CRITICAL_AREA_CONTINUE_W(session);

  auto entries = session_w->store.load();
  expect(!result && ava::provider::is_context_overflow_error(result.error()), "second context overflow is returned instead of retried indefinitely");
  expect(transport.requests().size() == 3, "context overflow retry is attempted at most once");
  expect(entries && count_compaction_entries(*entries) == 1, "bounded context overflow retry appends one compaction entry");
}

}  // namespace

void run_app_compaction_tests()
{
  test_shared_compaction_transaction();
  test_app_compact_provider_summary_success();
  test_app_compact_rejects_replaced_current_session_history();
  test_app_compact_openai_oauth_streaming_summary_success();
  test_app_compact_provider_failure_leaves_session_untouched();
  test_compaction_observation_preserves_cancellation_callback_contract();
  test_app_auto_compaction_provider_cancellation_leaves_session_untouched();
  test_app_compact_oversized_summary_leaves_session_untouched();
  test_app_compact_cancellation_before_append_leaves_session_untouched();
  test_app_compaction_prompt_builder_sections();
  test_app_compaction_model_selection_uses_runtime_catalog();
  test_app_compaction_recent_tail_preserves_tool_group();
  test_app_compaction_recent_tail_budget_never_orphans_tools();
  test_app_compaction_oversized_turn_retains_latest_user_anchor();
  test_app_manual_compaction_uses_only_active_context();
  test_app_cross_provider_compaction_requires_process_scope();
  test_app_compact_honors_cross_provider_selection();
  test_app_auto_compaction_appends_summary_and_rebuilds_context();
  test_app_auto_compaction_recent_context_respects_token_budget();
  test_app_auto_compaction_recent_context_truncates_utf8_safely();
  test_app_auto_compaction_explicit_zero_disables();
  test_app_auto_compaction_uses_default_threshold_without_context_window_metadata();
  test_app_auto_compaction_retries_stale_snapshot_before_append();
  test_app_auto_compaction_repeated_stale_snapshot_fails_without_append();
  test_app_context_overflow_compacts_and_retries_once_successfully();
  test_app_context_overflow_compaction_failure_leaves_no_partial_entry();
  test_app_non_overflow_provider_error_does_not_compact_or_retry();
  test_app_compaction_projects_committed_v4_and_ignores_incomplete_staging();
  test_app_context_overflow_retry_is_bounded();
}

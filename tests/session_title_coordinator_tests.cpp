#include "sys.h"
#include "tests/support/app_runtime_support.h"
#include "tests/support/fake_transport.h"
#include "tests/support/test_harness.h"
#include "ava/http/transport.h"
#include "ava/app/command_palette.h"
#include "ava/app/rpc/serialization.h"
#include "ava/app/runtime.h"
#include "ava/app/runtime/Session.h"
#include "ava/app/session_title_coordinator.h"
#include "ava/config/session_title_config.h"
#include "ava/session/assistant_output.h"
#include "ava/session/compaction.h"
#include "ava/provider/openai_provider.h"
#include "ava/core/ids.h"
#include "ava/core/json.h"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <stop_token>
#include <string>
#include <system_error>
#include <vector>
#include <unistd.h>             // ::write

namespace {

using namespace std::chrono_literals;

struct TitleTransportState
{
  std::mutex mutex;
  std::size_t sends = 0;
  std::vector<ava::http::HttpRequest> requests;
};

class TitleProviderTransport final : public ava::http::Transport
{
 public:
  explicit TitleProviderTransport(std::shared_ptr<TitleTransportState> state) : state_(std::move(state)) { }

  ava::core::Result<ava::http::HttpResponse> send(ava::http::HttpRequest const& request) override
  {
    std::lock_guard lock(state_->mutex);
    ++state_->sends;
    state_->requests.push_back(request);
    return ava::http::HttpResponse{
        .status_code = 200,
        .headers = {},
        .body =
            R"({"status":"completed","output":[{"id":"msg_title","type":"message","phase":"final_answer","content":[{"type":"output_text","text":"Direct Fake Provider Session Title"}]}]})"};
  }

 private:
  std::shared_ptr<TitleTransportState> state_;
};

struct GeneratorState
{
  std::mutex mutex;
  std::condition_variable changed;
  std::size_t calls = 0;
  bool started = false;
  bool release = true;
  bool stop_seen = false;
  bool fail = false;
  std::string output = "Generated Session Title";
  std::string source;

  ava::core::Result<std::string> generate(ava::app::SessionTitleGenerationRequest& request, std::stop_token stop_token,
                                          std::chrono::steady_clock::time_point deadline)
  {
    std::stop_callback wake(stop_token, [&] { changed.notify_all(); });
    std::unique_lock lock(mutex);
    ++calls;
    source = request.source_text;
    started = true;
    changed.notify_all();
    changed.wait_until(lock, deadline, [&] { return release || stop_token.stop_requested(); });
    stop_seen = stop_token.stop_requested();
    if (stop_seen)
      return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Provider, "fake title generation canceled"));
    if (fail)
      return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Provider, "fake title generation failed"));
    return output;
  }

  bool wait_started()
  {
    std::unique_lock lock(mutex);
    return changed.wait_for(lock, 3s, [&] { return started; });
  }

  void allow_completion()
  {
    std::lock_guard lock(mutex);
    release = true;
    changed.notify_all();
  }
};

std::shared_ptr<ava::app::SessionTitleCoordinator> make_coordinator(std::shared_ptr<GeneratorState> const& state, ava::config::SessionTitleConfig config = {},
                                                                    std::size_t max_queued = 4)
{
  auto coordinator = ava::app::SessionTitleCoordinator::create(
      {.config = std::move(config),
       .generator = [state](ava::app::SessionTitleGenerationRequest& request, std::stop_token token,
                            std::chrono::steady_clock::time_point deadline) { return state->generate(request, token, deadline); },
       .worker_count = 1,
       .max_queued = max_queued,
       .request_deadline = 2s});
  expect(coordinator.has_value(), coordinator ? "session title coordinator starts" : coordinator.error().format());
  return coordinator ? *coordinator : nullptr;
}

ava::core::Result<std::string> seed_committed_ordinary_turn(ava::app::runtime::session_ts& unlocked_session, std::string text)
{
  auto const append_target = ava::app::runtime::session_ts::rat(unlocked_session)->append_target();
  auto authority = append_target->read_authority();
  if (!authority)
    return std::unexpected(std::move(authority.error()));
  auto entries = authority->load();
  if (!entries)
    return std::unexpected(std::move(entries.error()));
  ava::session::SessionEntry user{.id = ava::core::make_id("entry"),
                                  .parent_id = entries->empty() ? std::string{} : entries->back().id,
                                  .type = ava::session::EntryType::UserMessage,
                                  .timestamp = ava::session::now_timestamp(),
                                  .data_json = "{\"text\":\"" + ava::core::json::escape(text) + "\"}"};
  if (auto appended = append_target->append(user); !appended)
    return std::unexpected(std::move(appended.error()));
  auto const turn_id = ava::core::make_id("assistant_turn");
  auto output_data = ava::session::serialize_assistant_output_item_data_json(
      {.assistant_turn_id = turn_id,
       .sequence = 0,
       .kind = ava::session::AssistantOutputItemKind::Text,
       .provider_item_id = {},
       .provider_output_index = std::nullopt,
       .payload = ava::session::AssistantOutputText{.text = "completed", .assistant_phase = ava::session::AssistantOutputTextPhase::FinalAnswer}});
  auto commit_data = ava::session::serialize_assistant_turn_commit_data_json(
      {.assistant_turn_id = turn_id, .item_count = 1, .provider = "openai", .model = "gpt-5.5", .finish_reason = "completed", .usage_json = std::nullopt});
  if (!output_data)
    return std::unexpected(std::move(output_data.error()));
  if (!commit_data)
    return std::unexpected(std::move(commit_data.error()));
  auto const commit_entry_id = ava::core::make_id("entry");
  std::vector<ava::session::SessionEntry> batch{{.id = ava::core::make_id("entry"),
                                                 .parent_id = user.id,
                                                 .type = ava::session::EntryType::AssistantOutputItem,
                                                 .timestamp = ava::session::now_timestamp(),
                                                 .data_json = std::move(*output_data)},
                                                {.id = commit_entry_id,
                                                 .parent_id = user.id,
                                                 .type = ava::session::EntryType::AssistantTurnCommit,
                                                 .timestamp = ava::session::now_timestamp(),
                                                 .data_json = std::move(*commit_data)}};
  if (auto appended = append_target->append_batch(std::move(batch)); !appended)
    return std::unexpected(std::move(appended.error()));
  return commit_entry_id;
}

ava::core::Result<ava::app::runtime::session_ts> open_title_session(std::filesystem::path const& root,
                                                                    std::shared_ptr<ava::app::SessionTitleCoordinator> coordinator, bool sessionless = false)
{
  auto const workspace = root / "workspace";
  auto paths = ava::tests::app_test_paths(root);
  std::filesystem::create_directories(workspace);
  ava::app::runtime::OpenContext options;
  options.workspace_dir = workspace;
  options.current_dir = workspace;
  options.paths = std::move(paths);
  options.session_title_coordinator = std::move(coordinator);
  return ava::app::runtime::Session::open(options, {.sessionless = sessionless,
                                                    .requested_session_id = std::nullopt,
                                                    .fork_session_id = std::nullopt,
                                                    .initial_session_name = std::nullopt,
                                                    .continue_last_session = false,
                                                    .initial_reasoning_level = std::nullopt,
                                                    .expected_original_cwd = std::nullopt});
}

void test_session_title_config_is_strict()
{
  auto defaults = ava::config::default_session_title_config();
  auto disabled = ava::config::parse_session_title_config(R"({"schema_version":1,"enabled":false})");
  auto model_only = ava::config::parse_session_title_config(R"({"schema_version":1,"model":"gpt-5.5"})");
  auto cross_provider = ava::config::parse_session_title_config(R"({"schema_version":1,"provider":"anthropic","model":"claude-sonnet-4-6"})");
  auto unknown = ava::config::parse_session_title_config(R"({"schema_version":1,"enabled":true,"extra":1})");
  auto duplicate = ava::config::parse_session_title_config(R"({"schema_version":1,"enabled":true,"enabled":false})");
  auto provider_only = ava::config::parse_session_title_config(R"({"schema_version":1,"provider":"anthropic"})");
  expect(defaults.enabled && disabled && !disabled->enabled && model_only && model_only->model_id == "gpt-5.5" && !model_only->provider_id && cross_provider &&
             cross_provider->provider_id == "anthropic" && !unknown && !duplicate && !provider_only,
         "session title config defaults enabled, permits explicit same/cross-provider models, and rejects unknown, duplicate, or incomplete members");
}

void test_title_config_uses_logical_runtime_anchors()
{
  auto const root = temp_root() / ("session-title-logical-anchors-" + ava::core::make_id("test"));
  std::error_code ignored;
  std::filesystem::remove_all(root, ignored);
  auto paths = ava::tests::app_test_paths(root);
  auto const config_target = root / "config-target";
  auto const state_target = root / "state-target";
  std::filesystem::rename(paths.ava_config_dir, config_target);
  std::filesystem::rename(paths.ava_state_dir, state_target);
  std::filesystem::create_directory_symlink(config_target, paths.ava_config_dir);
  std::filesystem::create_directory_symlink(state_target, paths.ava_state_dir);

  auto const workspace = root / "workspace";
  std::filesystem::create_directories(workspace);
  ava::app::runtime::OpenContext options;
  options.workspace_dir = workspace;
  options.current_dir = workspace;
  options.paths = paths;
  auto unlocked_session_result = ava::app::runtime::Session::open(options);
  expect(unlocked_session_result.has_value(),
         unlocked_session_result ? "session title anchor test opens runtime session" : unlocked_session_result.error().format());
  if (!unlocked_session_result)
    return;
  ava::app::runtime::session_ts& unlocked_session = *unlocked_session_result;
  CRITICAL_AREA_BEGIN_W(session);
  auto config_anchor = session_w->anchor_set()->find_anchor(paths.ava_config_dir);
  auto state_anchor = session_w->anchor_set()->find_anchor(paths.ava_state_dir);
  expect(session_w->session_title_coordinator() && config_anchor && config_anchor->relative().empty() && config_anchor->anchor().root == paths.ava_config_dir &&
             state_anchor && state_anchor->relative().empty() && state_anchor->anchor().root == paths.ava_state_dir,
         "session title config and state preserve trusted logical roots through shared descriptor anchors");
  std::filesystem::remove_all(root, ignored);
}

void test_title_text_boundaries()
{
  auto source =
      ava::app::normalize_session_title_source("  Build\t a useful API\n<system-reminder>secret injected text</system-reminder> with tests \x01 now  ");
  auto generated = ava::app::sanitize_generated_session_title("<think>private reasoning</think>\n## `Useful API Implementation Test Plan`\nignored");
  auto marked_up = ava::app::sanitize_generated_session_title("Title: [Useful](https://invalid.example) API Implementation Test Plan");
  auto malformed = ava::app::sanitize_generated_session_title(std::string("bad\x01title", 9));
  auto malformed_source = ava::app::normalize_session_title_source(std::string("left") + std::string("\xff", 1) + "right");
  auto fallback = ava::app::fallback_session_title(source);
  expect(source == "Build a useful API with tests now" && generated && *generated == "Useful API Implementation Test Plan" && marked_up &&
             *marked_up == "Useful API Implementation Test Plan" && !malformed && malformed_source == "left right" &&
             fallback == "Build a useful API with tests now",
         "session title input strips injected scaffolding and controls while output strips reasoning/markup and deterministic fallback remains bounded");

  std::string long_utf8;
  for (int index = 0; index < 100; ++index) long_utf8 += "é";
  long_utf8 += " one two three four";
  auto capped = ava::app::sanitize_generated_session_title(long_utf8);
  expect(capped && capped->size() <= ava::session::kMaxGeneratedSessionTitleBytes && ava::core::json::is_valid_utf8(*capped),
         "generated session titles hard-cap on a UTF-8 boundary");

  auto const multilingual_source = std::string("日本語 セッション タイトル を 安全に 設計。");
  auto multilingual_generated = ava::app::sanitize_generated_session_title(multilingual_source);
  auto multilingual_fallback = ava::app::fallback_session_title(multilingual_source);
  expect(multilingual_generated && *multilingual_generated == multilingual_source && multilingual_fallback == multilingual_source &&
             ava::core::json::is_valid_utf8(multilingual_fallback),
         "ASCII-only punctuation trimming preserves valid multilingual generated titles and fallbacks");
}

void test_metadata_manual_precedence_and_summary_projection()
{
  std::vector<ava::session::SessionEntry> entries{{.id = "generated-1",
                                                   .parent_id = "",
                                                   .type = ava::session::EntryType::SessionMetadata,
                                                   .timestamp = "1",
                                                   .data_json = R"({"schema_version":1,"generated_title":"Generated First","actor":"auto-title"})"},
                                                  {.id = "manual-empty",
                                                   .parent_id = "generated-1",
                                                   .type = ava::session::EntryType::SessionMetadata,
                                                   .timestamp = "2",
                                                   .data_json = R"({"schema_version":1,"name":"","actor":"tui"})"},
                                                  {.id = "generated-2",
                                                   .parent_id = "manual-empty",
                                                   .type = ava::session::EntryType::SessionMetadata,
                                                   .timestamp = "3",
                                                   .data_json = R"({"schema_version":1,"generated_title":"Generated Later","actor":"auto-title"})"}};
  auto metadata = ava::session::session_metadata_from_entries({}, entries);
  expect(
      metadata && metadata->has_manual_name && metadata->name.empty() && metadata->generated_title == "Generated Later" && metadata->effective_title().empty(),
      "an empty historical manual name permanently suppresses generated titles regardless of later record order");

  auto store = ava::session::SessionStore::create_ephemeral(std::filesystem::current_path());
  expect(store.has_value(), "effective title summary test creates ephemeral store");
  if (!store)
    return;
  for (auto const& entry : entries) expect(store->append_ephemeral(entry).has_value(), "effective title summary test appends metadata");
  auto summary = store->inspect_bounded(ava::session::legacy_unbounded_session_read_limits());
  expect(summary && summary->title.empty(), "bounded session summaries apply durable manual-empty suppression");

  std::vector<ava::session::SessionEntry> snapshot{entries.front()};
  ava::session::SessionMetadataUpdate generated_update;
  generated_update.generated_title = "Context Neutral Generated Title";
  generated_update.actor = "auto-title";
  auto generated_entry = ava::session::make_session_metadata_entry(std::move(generated_update), snapshot.back().id);
  auto manual_entry = ava::session::make_session_metadata_entry({.name = "Concurrent Manual Title", .actor = "test"}, snapshot.back().id);
  expect(generated_entry && manual_entry, "compaction snapshot title fixtures serialize");
  if (generated_entry && manual_entry)
  {
    auto with_generated = snapshot;
    with_generated.push_back(*generated_entry);
    auto with_manual = snapshot;
    with_manual.push_back(*manual_entry);
    expect(ava::session::compaction_snapshot_matches(snapshot, with_generated) && !ava::session::compaction_snapshot_matches(snapshot, with_manual),
           "in-flight compaction tolerates only context-neutral automatic title metadata");
  }
}

void test_direct_provider_generation_is_isolated()
{
  auto const root = temp_root() / ("session-title-provider-" + ava::core::make_id("test"));
  std::error_code ignored;
  std::filesystem::remove_all(root, ignored);
  auto transport_state = std::make_shared<TitleTransportState>();
  auto coordinator =
      ava::app::SessionTitleCoordinator::create({.config = {},
                                                 .transport_factory = [transport_state] { return std::make_unique<TitleProviderTransport>(transport_state); },
                                                 .worker_count = 1,
                                                 .max_queued = 2,
                                                 .request_deadline = 2s});
  expect(coordinator.has_value(), coordinator ? "direct title provider coordinator starts" : coordinator.error().format());
  if (!coordinator)
    return;
  auto unlocked_session_result = open_title_session(root, *coordinator);
  expect(unlocked_session_result.has_value(), unlocked_session_result ? "direct title provider session opens" : unlocked_session_result.error().format());
  if (!unlocked_session_result)
    return;
  ava::app::runtime::session_ts& unlocked_session = *unlocked_session_result;

  auto seeded = seed_committed_ordinary_turn(unlocked_session, "Inspect isolated provider title generation");
  expect(seeded.has_value(), seeded ? "direct title provider committed turn is seeded" : seeded.error().format());
  if (!seeded)
    return;
  ava::app::runtime::RunOptions options;
  options.access_token = "fake-provider-token";
  coordinator.value()->schedule(unlocked_session, "Inspect isolated provider title generation", *seeded, options);
  expect(coordinator.value()->wait_until_idle(3s), "direct title provider generation becomes idle");
  CRITICAL_AREA_BEGIN_W(session);
  auto metadata = ava::session::load_session_metadata(session_w->store, session_w->lease());
  bool isolated_request = false;
  {
    std::lock_guard lock(transport_state->mutex);
    isolated_request = transport_state->sends == 1 && transport_state->requests.size() == 1 && transport_state->requests.front().timeout_ms > 0 &&
                       transport_state->requests.front().timeout_ms <= 2000 &&
                       transport_state->requests.front().body.find("Inspect isolated provider title generation") != std::string::npos &&
                       transport_state->requests.front().body.find("\"type\":\"function\"") == std::string::npos &&
                       transport_state->requests.front().body.find("Direct Fake Provider Session Title") == std::string::npos;
  }
  expect(metadata && metadata->effective_title() == "Direct Fake Provider Session Title" && isolated_request,
         "direct title provider makes one bounded tool-free request and persists only its sanitized title metadata");
  auto list_json = session_w->list_sessions_result_json_1();
  auto tree_json = session_w->tree_result_json();
  auto metadata_json = metadata ? ava::session::session_metadata_json(*metadata) : std::string{};
  expect(list_json && tree_json && list_json->find("\"title\":\"Direct Fake Provider Session Title\"") != std::string::npos &&
             tree_json->find("\"title\":\"Direct Fake Provider Session Title\"") != std::string::npos &&
             tree_json->find("\"name\":\"\"") != std::string::npos &&
             metadata_json.find("\"generated_title\":\"Direct Fake Provider Session Title\"") != std::string::npos &&
             metadata_json.find("\"title\":\"Direct Fake Provider Session Title\"") != std::string::npos,
         "list, tree, and metadata serialization immediately expose the effective generated title");
  CRITICAL_AREA_END_W(session);
  coordinator.value()->shutdown();
}

void test_coordinator_dedupes_and_preserves_manual_rename_races()
{
  auto const root = temp_root() / ("session-title-race-" + ava::core::make_id("test"));
  std::error_code ignored;
  std::filesystem::remove_all(root, ignored);
  auto state = std::make_shared<GeneratorState>();
  state->release = false;
  auto coordinator = make_coordinator(state);
  auto unlocked_session_result = open_title_session(root, coordinator);
  expect(unlocked_session_result.has_value(), unlocked_session_result ? "title race session opens" : unlocked_session_result.error().format());
  if (!unlocked_session_result || !coordinator)
    return;
  ava::app::runtime::session_ts& unlocked_session = *unlocked_session_result;

  auto seeded = seed_committed_ordinary_turn(unlocked_session, "First ordinary prompt");
  expect(seeded.has_value(), seeded ? "title race committed turn is seeded" : seeded.error().format());
  if (!seeded)
    return;
  ava::app::runtime::RunOptions options;
  options.access_token = "not-used-by-fake";
  coordinator->schedule(unlocked_session, "First ordinary prompt", *seeded, options);
  coordinator->schedule(unlocked_session, "Second prompt must not replace first", *seeded, options);
  expect(state->wait_started(), "title race fake generator starts");
  CRITICAL_AREA_BEGIN_W(session);
  auto renamed = session_w->append_metadata_1(ava::session::SessionMetadataUpdate{.name = "Manual Rename", .actor = "test", .generated_title = std::nullopt});
  expect(renamed.has_value(), renamed ? "manual rename wins title race" : renamed.error().format());
  state->allow_completion();
  expect(coordinator->wait_until_idle(3s), "title race coordinator becomes idle");
  auto metadata = ava::session::load_session_metadata(session_w->store, session_w->lease());
  expect(metadata && metadata->effective_title() == "Manual Rename" && metadata->generated_title == "First ordinary prompt Overview and" && state->calls == 1 &&
             state->source == "First ordinary prompt",
         "coordinator permanently deduplicates by session id, binds the first prompt, and never refines over a concurrent manual rename");
  CRITICAL_AREA_END_W(session);
  coordinator->shutdown();
}

void test_coordinator_fallback_and_navigation_lifetime()
{
  auto const root = temp_root() / ("session-title-navigation-" + ava::core::make_id("test"));
  std::error_code ignored;
  std::filesystem::remove_all(root, ignored);
  auto state = std::make_shared<GeneratorState>();
  state->release = false;
  state->output = "Five Word Navigation Provider Title";
  auto coordinator = make_coordinator(state);
  auto unlocked_session_result = open_title_session(root, coordinator);
  expect(unlocked_session_result.has_value(), unlocked_session_result ? "title navigation source opens" : unlocked_session_result.error().format());
  if (!unlocked_session_result || !coordinator)
    return;
  ava::app::runtime::session_ts& unlocked_session = *unlocked_session_result;
  CRITICAL_AREA_BEGIN_W(session);
  auto const source_id = session_w->store.session_id();
  CRITICAL_AREA_END_W(session);
  auto seeded = seed_committed_ordinary_turn(unlocked_session, "Design <skill>hidden scaffold</skill> durable navigation titles");
  expect(seeded.has_value(), seeded ? "title navigation committed turn is seeded" : seeded.error().format());
  if (!seeded)
    return;
  ava::app::runtime::RunOptions options;
  coordinator->schedule(unlocked_session, "Design <skill>hidden scaffold</skill> durable navigation titles", *seeded, options);
  expect(state->wait_started(), "title navigation generator starts");
  auto unlocked_replacement_result = ava::app::runtime::Session::create_like(unlocked_session, ava::app::runtime::OpenContext{});
  expect(unlocked_replacement_result.has_value(), unlocked_replacement_result ? "replacement session opens" : unlocked_replacement_result.error().format());

  if (unlocked_replacement_result)
    expect(ava::app::runtime::Session::replace_with(unlocked_session, *unlocked_replacement_result).has_value(),
           "visible session navigation succeeds during title work");
  CRITICAL_AREA_CONTINUE_W(session);
  state->allow_completion();
  expect(coordinator->wait_until_idle(3s), "navigation title coordinator becomes idle");
  auto summaries = ava::session::SessionStore::list_sessions(session_w->workspace_dir(), session_w->paths().sessions_dir);
  auto found = summaries ? std::ranges::find_if(*summaries, [&](auto const& item) { return item.session_id == source_id; }) : decltype(summaries->begin()){};
  expect(summaries && found != summaries->end() && found->title == "Five Word Navigation Provider Title",
         "provider refinement survives in-process navigation through its retained controller-owned route" +
             std::string(summaries && found != summaries->end() ? ": observed title '" + found->title + "'" : ": source summary missing"));
  CRITICAL_AREA_END_W(session);
  coordinator->shutdown();
}

void test_session_specific_catalog_notifications_survive_navigation()
{
  auto const root = temp_root() / ("session-title-catalog-navigation-" + ava::core::make_id("test"));
  std::error_code ignored;
  std::filesystem::remove_all(root, ignored);
  auto state = std::make_shared<GeneratorState>();
  state->release = false;
  state->output = "Refined Old Session Catalog Title";
  auto coordinator = make_coordinator(state);
  auto unlocked_old_session_result = open_title_session(root, coordinator);
  auto unlocked_new_session_result = unlocked_old_session_result
                                         ? open_title_session(root, coordinator)
                                         : ava::core::Result<ava::app::runtime::session_ts>(std::unexpected(unlocked_old_session_result.error()));
  auto unlocked_late_session_result = unlocked_new_session_result
                                          ? open_title_session(root, coordinator)
                                          : ava::core::Result<ava::app::runtime::session_ts>(std::unexpected(unlocked_new_session_result.error()));
  expect(unlocked_old_session_result && unlocked_new_session_result && unlocked_late_session_result && coordinator,
         "session-specific title catalog test opens old, new-current, and late-dirty runtime sessions");
  if (!unlocked_old_session_result || !unlocked_new_session_result || !unlocked_late_session_result || !coordinator)
    return;
  ava::app::runtime::session_ts& unlocked_old_session = *unlocked_old_session_result;
  ava::app::runtime::session_ts& unlocked_new_session = *unlocked_new_session_result;
  ava::app::runtime::session_ts& unlocked_late_session = *unlocked_late_session_result;

  CRITICAL_AREA_BEGIN_W(new_session);
  auto named = new_session_w->append_metadata_1({.name = "New Current Session", .actor = "test"});
  auto committed = seed_committed_ordinary_turn(unlocked_old_session, "Fallback Old Session Catalog Title");
  auto late_committed = seed_committed_ordinary_turn(unlocked_late_session, "Late Session Notification Title");
  auto new_authority = new_session_w->append_target()->read_authority();
  auto new_entries = new_authority ? new_authority->load() : ava::core::Result<std::vector<ava::session::SessionEntry>>(std::unexpected(new_authority.error()));
  auto marked = new_entries ? new_session_w->append_target()->append(ava::session::SessionEntry{.id = ava::core::make_id("entry"),
                                                                                                .parent_id = new_entries->back().id,
                                                                                                .type = ava::session::EntryType::UserMessage,
                                                                                                .timestamp = "2099-01-01T00:00:00Z",
                                                                                                .data_json = R"({"text":"deterministic recent current"})"})
                            : ava::core::VoidResult(std::unexpected(new_entries.error()));
  expect(named && committed && late_committed && marked,
         "session-specific title catalog fixture persists names, old and late committed turns, and deterministic new-current activity");
  if (!named || !committed || !late_committed || !marked)
    return;
  CRITICAL_AREA_BEGIN_W(old_session);
  CRITICAL_AREA_BEGIN_W(late_session);
  auto const old_session_id = old_session_w->store.session_id();
  auto const late_session_id = late_session_w->store.session_id();
  auto const new_session_id = new_session_w->store.session_id();
  CRITICAL_AREA_END_W(late_session);
  CRITICAL_AREA_END_W(old_session);
  CRITICAL_AREA_END_W(new_session);

  std::size_t workspace_walks = 0;
  std::size_t tree_builds = 0;
  auto workspace_walker = [&](ava::app::runtime::session_ts const&) {
    ++workspace_walks;
    return std::vector<ava::app::WorkspacePathCandidate>{};
  };
  auto tree_builder = [&](ava::app::runtime::session_ts const& unlocked_current) {
    ++tree_builds;
    SCOPED_CRITICAL_AREA_CR(current_r, unlocked_current);
    return ava::session::build_session_tree(current_r->workspace_dir(), current_r->paths().sessions_dir, current_r->store.session_id());
  };
  auto cache = ava::app::build_application_catalog_cache(unlocked_old_session, {}, workspace_walker, tree_builder);
  ava::app::ApplicationCatalogCoordinator catalog(std::move(cache));

  ava::app::runtime::RunOptions options;
  coordinator->schedule(unlocked_old_session, "Fallback Old Session Catalog Title", *committed, options);
  expect(state->wait_started(), "session-specific title refinement blocks after durable fallback notification");
  auto fallback_changes = coordinator->catalog_changes_since(0);
  auto fallback_refresh = catalog.refresh_title_changes(unlocked_old_session, fallback_changes, {}, tree_builder);
  catalog.retarget_session(new_session_id);
  auto after_fallback = catalog.snapshot();
  auto old_after_fallback = std::ranges::find_if(after_fallback.session_tree->sessions,
                                                  [&](auto const& node) { return node.summary.session_id == old_session_id; });
  expect(fallback_changes.cursor == 1 && fallback_changes.dirty_session_ids == std::vector<std::string>{old_session_id} &&
             fallback_refresh && *fallback_refresh && old_after_fallback != after_fallback.session_tree->sessions.end() &&
             old_after_fallback->summary.title == "Fallback Old Session Catalog Title" && tree_builds == 1 && workspace_walks == 1,
         "fallback notification refreshes only its exact current authority before navigation");

  state->allow_completion();
  expect(coordinator->wait_until_idle(3s), "session-specific old-session refinement becomes idle after navigation");
  auto refinement_changes = coordinator->catalog_changes_since(catalog.title_catalog_cursor());
  auto topology_refresh = catalog.refresh_session_tree_and_consume_title_changes(unlocked_new_session, refinement_changes, {}, tree_builder);
  auto remaining_changes = coordinator->catalog_changes_since(catalog.title_catalog_cursor());
  auto duplicate_refresh = topology_refresh ? catalog.refresh_title_changes(unlocked_new_session, remaining_changes, {}, tree_builder)
                                            : ava::core::Result<bool>(std::unexpected(topology_refresh.error()));
  auto after_refinement = catalog.snapshot();
  auto selector = catalog.session_view(ava::app::SessionSelectorSort::Recent, {});
  auto old_node = std::ranges::find_if(after_refinement.session_tree->sessions,
                                        [&](auto const& node) { return node.summary.session_id == old_session_id; });
  auto new_node = std::ranges::find_if(after_refinement.session_tree->sessions,
                                        [&](auto const& node) { return node.summary.session_id == new_session_id; });
  expect(refinement_changes.cursor == 2 && refinement_changes.dirty_session_ids == std::vector<std::string>{old_session_id} &&
             topology_refresh && *topology_refresh && remaining_changes.cursor == 2 && remaining_changes.dirty_session_ids.empty() && duplicate_refresh &&
             !*duplicate_refresh && old_node != after_refinement.session_tree->sessions.end() && new_node != after_refinement.session_tree->sessions.end() &&
             old_node->summary.title == "Refined Old Session Catalog Title" && new_node->summary.title == "New Current Session" && !selector.items.empty() &&
              selector.items.front().value == new_session_id && tree_builds == 2 && workspace_walks == 1 &&
             after_refinement.operations.session_tree_builds == 2,
         "an actual current-session switch/topology rebuild consumes the captured old-session refinement once, preserves titles and Recent ordering, and "
         "causes no "
         "second selector rebuild or workspace walk");

  coordinator->schedule(unlocked_late_session, "Late Session Notification Title", *late_committed, options);
  expect(coordinator->wait_until_idle(3s), "late title notification arriving after the topology capture becomes idle");
  auto late_changes = coordinator->catalog_changes_since(catalog.title_catalog_cursor());
  auto late_refresh = catalog.refresh_title_changes(unlocked_new_session, late_changes, {}, tree_builder);
  auto late_duplicate = late_refresh ? catalog.refresh_title_changes(unlocked_new_session, late_changes, {}, tree_builder) : late_refresh;
  auto after_late = catalog.snapshot();
  auto late_node =
      std::ranges::find_if(after_late.session_tree->sessions, [&](auto const& node) { return node.summary.session_id == late_session_id; });
  expect(late_changes.cursor == 4 && late_changes.dirty_session_ids == std::vector<std::string>{late_session_id} && late_refresh &&
             *late_refresh && late_duplicate && !*late_duplicate && catalog.title_catalog_cursor() == late_changes.cursor && tree_builds == 3 &&
             workspace_walks == 1 && late_node != after_late.session_tree->sessions.end() && late_node->summary.title == "Refined Old Session Catalog Title",
         "a notification published after the captured topology cursor remains pending and is consumed exactly once by the later refresh");
  coordinator->shutdown();
}

void test_queue_delay_allows_later_turns_after_captured_first_commit()
{
  auto const root = temp_root() / ("session-title-queue-delay-" + ava::core::make_id("test"));
  std::error_code ignored;
  std::filesystem::remove_all(root, ignored);
  auto state = std::make_shared<GeneratorState>();
  state->release = false;
  state->output = "Five Word Delayed Queue Title";
  auto coordinator = make_coordinator(state, {}, 1);
  auto unlocked_blocker_result = open_title_session(root / "blocker", coordinator);
  auto unlocked_delayed_result = open_title_session(root / "delayed", coordinator);
  auto unlocked_overflow_result = open_title_session(root / "overflow", coordinator);
  expect(unlocked_blocker_result && unlocked_delayed_result && unlocked_overflow_result, "queue-delay title sessions open");
  if (!unlocked_blocker_result || !unlocked_delayed_result || !unlocked_overflow_result || !coordinator)
    return;
  ava::app::runtime::session_ts& unlocked_blocker = *unlocked_blocker_result;
  ava::app::runtime::session_ts& unlocked_delayed = *unlocked_delayed_result;
  ava::app::runtime::session_ts& unlocked_overflow = *unlocked_overflow_result;

  auto blocker_commit = seed_committed_ordinary_turn(unlocked_blocker, "Block title worker until released");
  expect(blocker_commit.has_value(), "queue-delay blocker commits its first ordinary turn");
  if (!blocker_commit)
    return;
  ava::app::runtime::RunOptions options;
  coordinator->schedule(unlocked_blocker, "Block title worker until released", *blocker_commit, options);
  expect(state->wait_started(), "queue-delay blocker occupies the title worker");

  auto first_commit = seed_committed_ordinary_turn(unlocked_delayed, "Capture this first ordinary title source");
  expect(first_commit.has_value(), "queue-delay target commits its first ordinary turn");
  if (!first_commit)
    return;
  coordinator->schedule(unlocked_delayed, "Capture this first ordinary title source", *first_commit, options);
  auto second_commit = seed_committed_ordinary_turn(unlocked_delayed, "A later turn arrives while title work is queued");
  expect(second_commit.has_value(), "queue-delay target appends a later ordinary turn");
  auto overflow_commit = seed_committed_ordinary_turn(unlocked_overflow, "Queue pressure retains this fallback");
  expect(overflow_commit.has_value(), "queue-pressure target commits its first ordinary turn");
  if (!overflow_commit)
    return;
  coordinator->schedule(unlocked_overflow, "Queue pressure retains this fallback", *overflow_commit, options);
  CRITICAL_AREA_BEGIN_W(overflow);
  auto overflow_metadata = ava::session::load_session_metadata(overflow_w->store, overflow_w->lease());
  expect(overflow_metadata && overflow_metadata->effective_title() == "Queue pressure retains this fallback",
         "a full provider-refinement queue still retains the synchronous fallback");

  state->allow_completion();
  expect(coordinator->wait_until_idle(3s), "queue-delay title coordinator becomes idle");
  CRITICAL_AREA_BEGIN_W(delayed);
  auto metadata = ava::session::load_session_metadata(delayed_w->store, delayed_w->lease());
  expect(metadata && metadata->effective_title() == "Five Word Delayed Queue Title" && state->calls == 2,
         "captured first-turn identity remains valid when later turns append during queue delay");
  CRITICAL_AREA_END_W(delayed);
  CRITICAL_AREA_END_W(overflow);
  coordinator->shutdown();
}

void test_first_admission_permanently_binds_commit_identity()
{
  auto const root = temp_root() / ("session-title-commit-binding-" + ava::core::make_id("test"));
  std::error_code ignored;
  std::filesystem::remove_all(root, ignored);
  auto state = std::make_shared<GeneratorState>();
  auto coordinator = make_coordinator(state);
  auto unlocked_session_result = open_title_session(root, coordinator);
  expect(unlocked_session_result.has_value(), "commit-binding title session opens");
  if (!unlocked_session_result || !coordinator)
    return;
  ava::app::runtime::session_ts& unlocked_session = *unlocked_session_result;

  auto first_commit = seed_committed_ordinary_turn(unlocked_session, "The actual first ordinary title source");
  auto second_commit = seed_committed_ordinary_turn(unlocked_session, "The second turn must not be rebound");
  expect(first_commit && second_commit, "commit-binding fixture appends two ordinary turns");
  if (!first_commit || !second_commit)
    return;
  ava::app::runtime::RunOptions options;
  coordinator->schedule(unlocked_session, "The second turn must not be rebound", *second_commit, options);
  coordinator->schedule(unlocked_session, "The actual first ordinary title source", *first_commit, options);
  expect(coordinator->wait_until_idle(100ms), "invalid captured commit schedules no title work");
  CRITICAL_AREA_BEGIN_W(session);
  auto metadata = ava::session::load_session_metadata(session_w->store, session_w->lease());
  expect(metadata && metadata->effective_title().empty() && state->calls == 0,
         "the first admission is permanent and a commit outside the first ordinary turn cannot be replaced later");
  CRITICAL_AREA_END_W(session);
  coordinator->shutdown();
}

void test_fallback_append_failure_latches_without_retry()
{
  auto const root = temp_root() / ("session-title-append-latch-" + ava::core::make_id("test"));
  std::error_code ignored;
  std::filesystem::remove_all(root, ignored);
  auto state = std::make_shared<GeneratorState>();
  auto coordinator = make_coordinator(state);
  auto unlocked_session_result = open_title_session(root, coordinator);
  expect(unlocked_session_result.has_value(), "append-latch title session opens");
  if (!unlocked_session_result || !coordinator)
    return;
  ava::app::runtime::session_ts& unlocked_session = *unlocked_session_result;
  auto committed = seed_committed_ordinary_turn(unlocked_session, "Never retry a title metadata mutation");
  expect(committed.has_value(), "append-latch fixture commits its first ordinary turn");
  if (!committed)
    return;

  auto write_calls = std::make_shared<std::atomic<std::size_t>>(0);
  CRITICAL_AREA_BEGIN_W(session);
  session_w->store.set_append_write_for_test([write_calls](int fd, std::string_view bytes) -> ssize_t {
    if (write_calls->fetch_add(1) == 0)
    {
      errno = EAGAIN;
      return -1;
    }
    return ::write(fd, bytes.data(), bytes.size());
  });
  auto replacement = ava::session::SessionAppendTarget::create_persistent(session_w->store, session_w->lease(), session_w->session_read_limits());
  expect(replacement.has_value(), "append-latch target is recreated with the deterministic write seam");
  if (!replacement)
    return;
  session_w->run_controller()->shutdown();
  session_w->resources().append_target = *replacement;
  session_w->resources().run_controller = std::make_shared<ava::app::SessionRunController>(*replacement);
  CRITICAL_AREA_END_W(session);

  ava::app::runtime::RunOptions options;
  coordinator->schedule(unlocked_session, "Never retry a title metadata mutation", *committed, options);
  expect(coordinator->wait_until_idle(1s), "failed fallback leaves no provider refinement work queued");

  CRITICAL_AREA_CONTINUE_W(session);
  auto later = session_w->append_metadata_1({.name = "must remain latched", .actor = "test"});
  auto metadata = ava::session::load_session_metadata(session_w->store, session_w->lease());
  expect(!later && metadata && metadata->effective_title().empty() && state->calls == 0 && write_calls->load() == 1,
         "title fallback uses the controller route, never retries a mutation error, and preserves its append-failure latch");
  CRITICAL_AREA_END_W(session);

  coordinator->shutdown();
}

void test_runtime_trigger_is_after_completion_and_excludes_synthetic_turns()
{
  auto const root = temp_root() / ("session-title-trigger-" + ava::core::make_id("test"));
  std::error_code ignored;
  std::filesystem::remove_all(root, ignored);
  auto state = std::make_shared<GeneratorState>();
  state->output = "<think>not persisted</think>\nFive Word Runtime Trigger Title";
  auto coordinator = make_coordinator(state);
  auto unlocked_session_result = open_title_session(root, coordinator);
  expect(unlocked_session_result.has_value(), unlocked_session_result ? "runtime title trigger session opens" : unlocked_session_result.error().format());
  if (!unlocked_session_result || !coordinator)
    return;

  bool title_absent_at_done = false;
  ava::app::runtime::RunOptions options;
  options.access_token = "fake-token";
  options.event_sink = [&](ava::event::RuntimeEvent const& event) -> ava::core::VoidResult {
    if (event.type() == ava::event::RuntimeEventType::Done)
    {
      SCOPED_CRITICAL_AREA_W(session_w, *unlocked_session_result);
      auto authority = session_w->read_authority_1();
      auto entries = authority ? authority->load() : ava::core::Result<std::vector<ava::session::SessionEntry>>(std::unexpected(authority.error()));
      auto metadata = entries ? ava::session::session_metadata_from_entries(session_w->store.session_id(), *entries)
                              : ava::core::Result<ava::session::SessionMetadataView>(std::unexpected(entries.error()));
      title_absent_at_done = metadata && metadata->effective_title().empty();
    }
    return {};
  };
  ava::provider::OpenAIProvider provider;
  ava::tests::FakeTransport transport({ava::tests::sse_response(ava::tests::final_text_sse("completed answer"))});
  auto result = ava::app::run_prompt(*unlocked_session_result, "Implement the runtime title trigger", provider, transport, options);
  expect(result && result->committed_turn_id && title_absent_at_done, "runtime does not trigger title work from the earlier Done event");
  expect(coordinator->wait_until_idle(3s), "runtime title trigger coordinator becomes idle");
  auto metadata = [&unlocked_session_result] {
    SCOPED_CRITICAL_AREA_R(session_r, *unlocked_session_result);
    return ava::session::load_session_metadata(session_r->store, session_r->lease());
  }();
  expect(metadata && metadata->effective_title() == "Five Word Runtime Trigger Title" && state->calls == 1 && coordinator->catalog_generation() == 2,
         "runtime triggers asynchronous title generation only after admission completion succeeds and publishes fallback/refinement catalog generations");
  options.event_sink = nullptr;
  ava::tests::FakeTransport second_transport({ava::tests::sse_response(ava::tests::final_text_sse("second completed answer"))});
  auto second = ava::app::run_prompt(*unlocked_session_result, "A later ordinary prompt", provider, second_transport, options);
  expect(second && coordinator->wait_until_idle(3s) && state->calls == 1 && coordinator->catalog_generation() == 2,
         "later prompts never regenerate an existing automatic title or its catalog generation");

  auto synthetic_state = std::make_shared<GeneratorState>();
  auto synthetic_coordinator = make_coordinator(synthetic_state);
  auto unlocked_synthetic_result = open_title_session(root / "synthetic", synthetic_coordinator);
  expect(unlocked_synthetic_result.has_value(), "synthetic exclusion session opens");
  if (unlocked_synthetic_result)
  {
    ava::app::runtime::RunOptions synthetic_options;
    synthetic_options.access_token = "fake-token";
    synthetic_options.synthetic_subagent_delivery = true;
    ava::tests::FakeTransport synthetic_transport({ava::tests::sse_response(ava::tests::final_text_sse("delivery answer"))});
    auto synthetic_result = ava::app::run_prompt(*unlocked_synthetic_result, "synthetic delivery", provider, synthetic_transport, synthetic_options);
    expect(synthetic_result && synthetic_coordinator->wait_until_idle(100ms) && synthetic_state->calls == 0,
           "synthetic subagent-delivery turns never trigger automatic titles");
  }
  coordinator->shutdown();
  synthetic_coordinator->shutdown();
}

void test_existing_manual_child_generated_and_sessionless_exclusions()
{
  auto const root = temp_root() / ("session-title-exclusions-" + ava::core::make_id("test"));
  std::error_code ignored;
  std::filesystem::remove_all(root, ignored);
  auto state = std::make_shared<GeneratorState>();
  auto coordinator = make_coordinator(state);
  ava::app::runtime::RunOptions options;

  auto unlocked_manual_result = open_title_session(root / "manual", coordinator);
  expect(unlocked_manual_result.has_value(), "manual-title exclusion session opens");
  if (unlocked_manual_result)
  {
    auto named = ava::app::runtime::session_ts::wat(*unlocked_manual_result)->append_metadata_1({.name = "Manual Session Title", .actor = "test"});
    expect(named.has_value(), "manual-title exclusion metadata appends");
    coordinator->schedule(*unlocked_manual_result, "Prompt for a manually titled session", "missing_commit", options);
    expect(coordinator->wait_until_idle(3s), "manual-title exclusion check becomes idle");
  }

  auto unlocked_child_result = open_title_session(root / "child", coordinator);
  expect(unlocked_child_result.has_value(), "child exclusion session opens");
  if (unlocked_child_result)
  {
    auto marked = ava::app::runtime::session_ts::wat(*unlocked_child_result)->append_metadata_1({.parent_session_id = "session_parent", .actor = "subagent"});
    expect(marked.has_value(), "child exclusion metadata appends");
    coordinator->schedule(*unlocked_child_result, "Prompt for a child session", "missing_commit", options);
    expect(coordinator->wait_until_idle(3s), "child exclusion check becomes idle");
  }

  auto unlocked_generated_result = open_title_session(root / "generated", coordinator);
  expect(unlocked_generated_result.has_value(), "generated-title exclusion session opens");
  if (unlocked_generated_result)
  {
    ava::session::SessionMetadataUpdate update;
    update.generated_title = "Existing Generated Session Title";
    update.actor = "auto-title";
    auto titled = ava::app::runtime::session_ts::wat(*unlocked_generated_result)->append_metadata_1(std::move(update));
    expect(titled.has_value(), "generated-title exclusion metadata appends");
    coordinator->schedule(*unlocked_generated_result, "Prompt for an already titled session", "missing_commit", options);
    expect(coordinator->wait_until_idle(3s), "generated-title exclusion check becomes idle");
  }

  auto unlocked_resumed_result = open_title_session(root / "resumed", coordinator);
  expect(unlocked_resumed_result.has_value(), "resumed-history exclusion session opens");
  if (unlocked_resumed_result)
  {
    auto first = seed_committed_ordinary_turn(*unlocked_resumed_result, "Historical first prompt");
    auto second = seed_committed_ordinary_turn(*unlocked_resumed_result, "Current resumed prompt");
    expect(first && second, "resumed-history exclusion seeds prior committed turns");
    coordinator->schedule(*unlocked_resumed_result, "Current resumed prompt", second ? *second : "missing_commit", options);
    expect(coordinator->wait_until_idle(3s), "resumed-history exclusion check becomes idle");
  }

  auto unlocked_sessionless_result = open_title_session(root / "sessionless", coordinator, true);
  expect(unlocked_sessionless_result.has_value(), "sessionless exclusion session opens");
  if (unlocked_sessionless_result)
  {
    coordinator->schedule(*unlocked_sessionless_result, "Prompt without persistent session state", "missing_commit", options);
  }

  expect(state->calls == 0, "manual, child, generated, resumed-history, and sessionless sessions make no title-provider attempt");
  coordinator->shutdown();

  auto disabled_state = std::make_shared<GeneratorState>();
  auto disabled_coordinator = make_coordinator(disabled_state, {.enabled = false});
  auto unlocked_disabled_result = open_title_session(root / "disabled", disabled_coordinator);
  expect(unlocked_disabled_result.has_value(), "strict-disable title session opens");
  if (unlocked_disabled_result)
  {
    disabled_coordinator->schedule(*unlocked_disabled_result, "Prompt while titles are disabled", "missing_commit", options);
    expect(disabled_coordinator->wait_until_idle(100ms) && disabled_state->calls == 0, "strict title disable starts no worker or provider attempt");
  }
  disabled_coordinator->shutdown();
}

void test_shutdown_cancels_owned_generation()
{
  auto const root = temp_root() / ("session-title-shutdown-" + ava::core::make_id("test"));
  std::error_code ignored;
  std::filesystem::remove_all(root, ignored);
  auto state = std::make_shared<GeneratorState>();
  state->release = false;
  auto coordinator = make_coordinator(state);
  auto unlocked_session_result = open_title_session(root, coordinator);
  expect(unlocked_session_result.has_value(), "shutdown title session opens");
  if (!unlocked_session_result || !coordinator)
    return;
  ava::app::runtime::session_ts& unlocked_session = *unlocked_session_result;
  auto seeded = seed_committed_ordinary_turn(unlocked_session, "Cancel this title generation safely");
  expect(seeded.has_value(), seeded ? "shutdown committed turn is seeded" : seeded.error().format());
  if (!seeded)
    return;
  ava::app::runtime::RunOptions options;
  coordinator->schedule(unlocked_session, "Cancel this title generation safely", *seeded, options);
  expect(state->wait_started(), "shutdown title generation starts");
  CRITICAL_AREA_BEGIN_W(session);
  auto metadata_before_shutdown = ava::session::load_session_metadata(session_w->store, session_w->lease());
  CRITICAL_AREA_END_W(session);
  coordinator->shutdown();
  CRITICAL_AREA_CONTINUE_W(session);
  auto metadata_after_shutdown = ava::session::load_session_metadata(session_w->store, session_w->lease());
  {
    std::lock_guard lock(state->mutex);
    expect(state->stop_seen && metadata_before_shutdown && metadata_after_shutdown &&
               metadata_before_shutdown->effective_title() == "Cancel this title generation safely" &&
               metadata_after_shutdown->effective_title() == "Cancel this title generation safely",
           "title coordinator shutdown cancels provider refinement without removing the synchronous durable fallback");
  }
}

}  // namespace

void run_session_title_coordinator_tests()
{
  test_session_title_config_is_strict();
  test_title_config_uses_logical_runtime_anchors();
  test_title_text_boundaries();
  test_metadata_manual_precedence_and_summary_projection();
  test_direct_provider_generation_is_isolated();
  test_coordinator_dedupes_and_preserves_manual_rename_races();
  test_coordinator_fallback_and_navigation_lifetime();
  test_session_specific_catalog_notifications_survive_navigation();
  test_queue_delay_allows_later_turns_after_captured_first_commit();
  test_first_admission_permanently_binds_commit_identity();
  test_fallback_append_failure_latches_without_retry();
  test_runtime_trigger_is_after_completion_and_excludes_synthetic_turns();
  test_existing_manual_child_generated_and_sessionless_exclusions();
  test_shutdown_cancels_owned_generation();
}

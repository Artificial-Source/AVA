#include "sys.h"
#include "tests/session_test_declarations.h"
#include "tests/support/session_test_support.h"
#include "tests/support/syscall_errno_injection.h"
#include "tests/support/test_harness.h"
#include "ava/session/assistant_output.h"
#include "ava/session/attachments.h"
#include "ava/session/record.h"
#include "ava/session/session_store.h"
#include "ava/session/validation.h"
#include "ava/core/result.h"

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

namespace session_tests {
void test_session_store_round_trip()
{
  auto const root = create_empty_root("test_session_store_round_trip");

  auto store = ava::session::SessionStore::create(std::filesystem::current_path(), root);
  expect(store.has_value(), "session store creates");
  if (!store)
    return;

  auto append = append_session_entry_for_test(*store, ava::session::SessionEntry{
                                                          .id = "entry_1",
                                                          .parent_id = "",
                                                          .type = ava::session::EntryType::SessionStart,
                                                          .timestamp = "2026-04-27T00:00:00Z",
                                                          .data_json = "{\"mode\":\"build\"}",
                                                      });
  expect(append.has_value(), "session entry appends");

  struct stat session_stat{};
  if (stat(store->session_path().c_str(), &session_stat) == 0)
  {
    expect((session_stat.st_mode & 0777) == 0600, "session file is owner read/write only");
  }
  struct stat session_dir_stat{};
  auto const session_dir = store->session_path().parent_path();
  if (stat(session_dir.c_str(), &session_dir_stat) == 0)
  {
    expect((session_dir_stat.st_mode & 0777) == 0700, "session directory is owner-only");
  }

  auto loaded = store->load();
  expect(loaded.has_value(), "session entries load");
  if (!loaded)
    return;

  expect(loaded->size() == 1, "one entry loaded");
  expect((*loaded)[0].id == "entry_1", "entry id round trips");
  expect((*loaded)[0].type == ava::session::EntryType::SessionStart, "entry type round trips");
  expect((*loaded)[0].version == ava::session::kCurrentSessionEntryVersion, "session entry version round trips from current JSONL records");

  auto text_store = ava::session::SessionStore::create(std::filesystem::current_path(), root);
  expect(text_store.has_value(), "text session store creates");
  if (!text_store)
    return;
  auto const text_append = append_session_entry_for_test(*text_store, ava::session::SessionEntry{
                                                                          .id = "entry_2\n",
                                                                          .parent_id = "",
                                                                          .type = ava::session::EntryType::UserMessage,
                                                                          .timestamp = "2026-04-27T00:00:00Z",
                                                                          .data_json = "{\"text\":\"hello\"}",
                                                                      });
  expect(text_append.has_value(), "session entry with escaped newline appends");
  auto text_loaded = text_store->load();
  expect(text_loaded && !text_loaded->empty() && (*text_loaded)[0].id == "entry_2\n", "session escaped strings round trip");

  auto const raw_newline_append = append_session_entry_for_test(*text_store, ava::session::SessionEntry{
                                                                                 .id = "entry_raw_newline",
                                                                                 .parent_id = "",
                                                                                 .type = ava::session::EntryType::UserMessage,
                                                                                 .timestamp = "2026-04-27T00:00:00Z",
                                                                                 .data_json = "{\"text\":\"bad\nsplit\"}",
                                                                             });
  expect(!raw_newline_append, "session data_json rejects raw newlines to preserve JSONL entries");

  auto const raw_carriage_return_append = append_session_entry_for_test(*text_store, ava::session::SessionEntry{
                                                                                         .id = "entry_raw_carriage_return",
                                                                                         .parent_id = "",
                                                                                         .type = ava::session::EntryType::UserMessage,
                                                                                         .timestamp = "2026-04-27T00:00:00Z",
                                                                                         .data_json = "{\"text\":\"bad\rsplit\"}",
                                                                                     });
  expect(!raw_carriage_return_append, "session data_json rejects raw carriage returns to preserve JSONL entries");

  auto large_store = ava::session::SessionStore::create(std::filesystem::current_path(), root);
  expect(large_store.has_value(), "large session store creates");
  if (large_store)
  {
    auto const large_append = append_session_entry_for_test(*large_store, ava::session::SessionEntry{
                                                                              .id = std::string(600000, '"'),
                                                                              .parent_id = "",
                                                                              .type = ava::session::EntryType::UserMessage,
                                                                              .timestamp = "2026-04-27T00:00:00Z",
                                                                              .data_json = "{\"text\":\"hello\"}",
                                                                          });
    expect(!large_append, "session append rejects entries whose escaped JSONL line is too large");
    auto large_loaded = large_store->load();
    expect(large_loaded && large_loaded->empty(), "oversized rejected session entry does not corrupt later loads");
  }

  ava::session::SessionStore oversized_load_store(ava::session::SessionStoreOptions{
      .root_dir = root,
      .workspace_dir = std::filesystem::current_path(),
      .session_id = "oversized-load",
  });
  std::filesystem::create_directories(oversized_load_store.session_path().parent_path());
  {
    std::ofstream file(oversized_load_store.session_path(), std::ios::binary | std::ios::trunc);
    file << std::string(1024 * 1024, '{') << '\n';
  }
  expect(!oversized_load_store.load(), "session load rejects oversized JSONL lines without parsing them");

  std::vector<std::string> const bad_session_ids = {
      "", ".", "..", "/", "\\", "../escape", "with/slash", "with\\slash", std::string("bad\0id", 6), std::string("bad\x1Fid", 6)};
  for (auto const& bad_session_id : bad_session_ids)
  {
    ava::session::SessionStore bad_store(ava::session::SessionStoreOptions{
        .root_dir = root,
        .workspace_dir = std::filesystem::current_path(),
        .session_id = bad_session_id,
    });
    auto const bad_append = append_session_entry_for_test(bad_store, ava::session::SessionEntry{
                                                                         .id = "entry_bad_session",
                                                                         .parent_id = "",
                                                                         .type = ava::session::EntryType::UserMessage,
                                                                         .timestamp = "2026-04-27T00:00:00Z",
                                                                         .data_json = "{\"text\":\"hello\"}",
                                                                     });
    expect(!bad_append, "session append rejects invalid externally supplied session ids");
    expect(!bad_store.load(), "session load rejects invalid externally supplied session ids");
  }

  ava::session::SessionStore traversal_store(ava::session::SessionStoreOptions{
      .root_dir = root,
      .workspace_dir = std::filesystem::current_path(),
      .session_id = "../escape",
  });
  auto const attempted_traversal_path = traversal_store.session_path().lexically_normal();
  std::error_code cleanup_error;
  std::filesystem::remove(attempted_traversal_path, cleanup_error);
  auto const traversal_append = append_session_entry_for_test(traversal_store, ava::session::SessionEntry{
                                                                                   .id = "entry_traversal",
                                                                                   .parent_id = "",
                                                                                   .type = ava::session::EntryType::UserMessage,
                                                                                   .timestamp = "2026-04-27T00:00:00Z",
                                                                                   .data_json = "{\"text\":\"hello\"}",
                                                                               });
  expect(!traversal_append && !std::filesystem::exists(attempted_traversal_path), "session traversal id append is rejected before creating attempted path");

  ava::session::SessionStore unicode_store(ava::session::SessionStoreOptions{
      .root_dir = root,
      .workspace_dir = std::filesystem::current_path(),
      .session_id = "unicode",
  });
  std::filesystem::create_directories(unicode_store.session_path().parent_path());
  {
    std::ofstream file(unicode_store.session_path(), std::ios::binary | std::ios::trunc);
    file << "{\"version\":1,\"id\":\"entry_\\u00e9\",\"parent_id\":\"\",\"type\":\"user_message\","
            "\"timestamp\":\"2026-04-27T00:00:00Z\",\"data\":{\"text\":\"hello\"}}\n";
  }
  auto unicode_loaded = unicode_store.load();
  expect(unicode_loaded && !unicode_loaded->empty() && (*unicode_loaded)[0].id == std::string("entry_\xC3\xA9"), "session unicode escapes decode as utf-8");

  ava::session::SessionStore surrogate_store(ava::session::SessionStoreOptions{
      .root_dir = root,
      .workspace_dir = std::filesystem::current_path(),
      .session_id = "surrogate",
  });
  std::filesystem::create_directories(surrogate_store.session_path().parent_path());
  {
    std::ofstream file(surrogate_store.session_path(), std::ios::binary | std::ios::trunc);
    file << "{\"version\":1,\"id\":\"entry_\\uD834\\uDD1E\",\"parent_id\":\"\",\"type\":\"user_message\","
            "\"timestamp\":\"2026-04-27T00:00:00Z\",\"data\":{\"text\":\"hello\"}}\n";
  }
  auto surrogate_loaded = surrogate_store.load();
  expect(surrogate_loaded && !surrogate_loaded->empty() && (*surrogate_loaded)[0].id == std::string("entry_\xF0\x9D\x84\x9E"),
         "session surrogate pairs decode as utf-8");

  ava::session::SessionStore bad_surrogate_store(ava::session::SessionStoreOptions{
      .root_dir = root,
      .workspace_dir = std::filesystem::current_path(),
      .session_id = "bad-surrogate",
  });
  std::filesystem::create_directories(bad_surrogate_store.session_path().parent_path());
  {
    std::ofstream file(bad_surrogate_store.session_path(), std::ios::binary | std::ios::trunc);
    file << "{\"version\":1,\"id\":\"entry_\\uD834\",\"parent_id\":\"\",\"type\":\"user_message\","
            "\"timestamp\":\"2026-04-27T00:00:00Z\",\"data\":{\"text\":\"hello\"}}\n";
  }
  auto bad_surrogate_loaded = bad_surrogate_store.load();
  expect(bad_surrogate_loaded && !bad_surrogate_loaded->empty() && (*bad_surrogate_loaded)[0].id == std::string("entry_\xEF\xBF\xBD"),
         "session lone surrogate decodes as replacement character");

  ava::session::SessionStore nested_key_store(ava::session::SessionStoreOptions{
      .root_dir = root,
      .workspace_dir = std::filesystem::current_path(),
      .session_id = "nested-key",
  });
  std::filesystem::create_directories(nested_key_store.session_path().parent_path());
  {
    std::ofstream file(nested_key_store.session_path(), std::ios::binary | std::ios::trunc);
    file << "{\"version\":1,\"data\":{\"id\":\"bad\",\"parent_id\":\"bad\",\"type\":\"bad\","
            "\"timestamp\":\"bad\",\"text\":\"contains \\\"type\\\":\\\"bad\\\"\"},"
            "\"id\":\"entry_safe\",\"parent_id\":\"parent_safe\",\"type\":\"user_message\","
            "\"timestamp\":\"2026-04-27T00:00:00Z\"}\n";
  }
  auto nested_key_loaded = nested_key_store.load();
  expect(nested_key_loaded && !nested_key_loaded->empty() && (*nested_key_loaded)[0].id == "entry_safe" && (*nested_key_loaded)[0].parent_id == "parent_safe" &&
             (*nested_key_loaded)[0].type == ava::session::EntryType::UserMessage && (*nested_key_loaded)[0].timestamp == "2026-04-27T00:00:00Z",
         "session load ignores key-looking strings and nested data keys");

  ava::session::SessionStore missing_version_store(ava::session::SessionStoreOptions{
      .root_dir = root,
      .workspace_dir = std::filesystem::current_path(),
      .session_id = "missing-version",
  });
  std::filesystem::create_directories(missing_version_store.session_path().parent_path());
  {
    std::ofstream file(missing_version_store.session_path(), std::ios::binary | std::ios::trunc);
    file << "{\"id\":\"entry_legacy\",\"parent_id\":\"\",\"type\":\"user_message\","
            "\"timestamp\":\"2026-04-27T00:00:00Z\",\"data\":{\"text\":\"hello\"}}\n";
  }
  auto missing_version_loaded = missing_version_store.load();
  expect(missing_version_loaded && missing_version_loaded->size() == 1 && (*missing_version_loaded)[0].id == "entry_legacy" &&
             (*missing_version_loaded)[0].version == 0,
         "session loader treats missing entry version as legacy-compatible");

  ava::session::SessionStore future_version_store(ava::session::SessionStoreOptions{
      .root_dir = root,
      .workspace_dir = std::filesystem::current_path(),
      .session_id = "future-version",
  });
  std::filesystem::create_directories(future_version_store.session_path().parent_path());
  {
    std::ofstream file(future_version_store.session_path(), std::ios::binary | std::ios::trunc);
    file << "{\"version\":" << (ava::session::kCurrentSessionEntryVersion + 1)
         << ",\"id\":\"entry_future\",\"parent_id\":\"\",\"type\":\"user_message\","
            "\"timestamp\":\"2026-04-27T00:00:00Z\",\"data\":{\"text\":\"hello\"}}\n";
  }
  auto future_version_loaded = future_version_store.load();
  expect(!future_version_loaded && future_version_loaded.error().category() == ava::core::ErrorCategory::Session,
         "session loader rejects unsupported future entry versions");

  ava::session::SessionStore bad_parent_store(ava::session::SessionStoreOptions{
      .root_dir = root,
      .workspace_dir = std::filesystem::current_path(),
      .session_id = "bad-parent",
  });
  std::filesystem::create_directories(bad_parent_store.session_path().parent_path());
  {
    std::ofstream file(bad_parent_store.session_path(), std::ios::binary | std::ios::trunc);
    file << "{\"version\":1,\"id\":\"entry_bad_parent\",\"parent_id\":\"../escape\","
            "\"type\":\"user_message\",\"timestamp\":\"2026-04-27T00:00:00Z\","
            "\"data\":{\"text\":\"hello\"}}\n";
  }
  auto bad_parent_loaded = bad_parent_store.load();
  expect(!bad_parent_loaded && bad_parent_loaded.error().category() == ava::core::ErrorCategory::InvalidArgument,
         "session loader rejects unsafe parent_id values without requiring parent existence");

  auto invalid_parent_append = append_session_entry_for_test(*text_store, ava::session::SessionEntry{.id = "entry_invalid_parent",
                                                                                                     .parent_id = "bad/parent",
                                                                                                     .type = ava::session::EntryType::UserMessage,
                                                                                                     .timestamp = "2026-04-27T00:00:00Z",
                                                                                                     .data_json = "{\"text\":\"hello\"}"});
  expect(!invalid_parent_append, "session append rejects unsafe parent_id values");
}

void test_bounded_session_reads_strictly_classify_framed_records()
{
  auto const root = create_empty_root("bounded-strict-session-records");

  auto const workspace = root / "workspace";
  auto const sessions = root / "sessions";
  std::filesystem::create_directories(workspace);

  std::string deep_record =
      "{\"version\":3,\"id\":\"deep\",\"parent_id\":\"\",\"type\":\"user_message\",\"timestamp\":\"2026-07-14T00:00:00Z\",\"data\":{\"x\":";
  deep_record += std::string(70, '[') + "0" + std::string(70, ']') + "}}";
  std::vector<std::pair<std::string, std::string>> const records = {
      {"duplicate",
       "{\"version\":3,\"id\":\"first\",\"id\":\"second\",\"parent_id\":\"\",\"type\":\"user_message\","
       "\"timestamp\":\"2026-07-14T00:00:00Z\",\"data\":{}}"},
      {"nesting", std::move(deep_record)},
  };

  for (auto const& [kind, record] : records)
  {
    ava::session::SessionStore store(ava::session::SessionStoreOptions{.root_dir = sessions, .workspace_dir = workspace, .session_id = "strict_" + kind});
    ava::tests::write_session_test_binary_file(store.session_path(), record + "\n");
    auto loaded = store.load_bounded(ava::session::SessionReadLimits{});
    std::size_t visited_entries = 0;
    auto visited = store.visit_entries(ava::session::SessionReadLimits{}, [&](ava::session::SessionEntry const&) -> ava::core::Result<bool> {
      ++visited_entries;
      return true;
    });
    expect(!loaded && !visited && visited_entries == 0 && loaded.error().message().find(kind) != std::string::npos &&
               visited.error().message().find(kind) != std::string::npos,
           "bounded loads and visits reject newline-terminated " + kind + " JSON before permissive session parsing");
  }
}

void test_ephemeral_session_store_stays_in_memory()
{
  auto const root = create_empty_root("ephemeral-session-store");

  auto const workspace = root / "workspace";
  std::filesystem::create_directories(workspace);

  std::filesystem::path scratch_root;
  std::filesystem::path session_path;
  {
    auto store = ava::session::SessionStore::create_ephemeral(workspace);
    expect(store.has_value() && store->is_ephemeral(), "ephemeral session store creates in no-session mode");
    if (!store)
      return;

    session_path = store->session_path();
    scratch_root = session_path.parent_path().parent_path();
    auto append = append_session_entry_for_test(*store, ava::session::SessionEntry{
                                                            .id = "entry_ephemeral",
                                                            .parent_id = "",
                                                            .type = ava::session::EntryType::UserMessage,
                                                            .timestamp = "2026-04-27T00:00:00Z",
                                                            .data_json = "{\"text\":\"hello\"}",
                                                        });
    expect(append.has_value(), "ephemeral session store accepts valid entries");
    expect(!std::filesystem::exists(session_path), "ephemeral session store does not create a JSONL history file");

    auto loaded = store->load();
    expect(loaded && loaded->size() == 1 && (*loaded)[0].id == "entry_ephemeral", "ephemeral session store reloads entries from memory");

    auto imported = ava::session::import_image_attachment_bytes(*store, ava::tests::session_test_tiny_png_bytes(), std::string_view("image/png"));
    expect(imported.has_value(), "ephemeral session store supports temp-only attachment storage");
    if (imported)
    {
      auto loaded_attachment = ava::session::load_image_attachment(*store, *imported);
      expect(loaded_attachment && loaded_attachment->bytes == ava::tests::session_test_tiny_png_bytes(),
             "ephemeral session attachments reload while the store is alive");
    }
    expect(!std::filesystem::exists(session_path), "ephemeral attachments do not create a JSONL history file");
  }

  expect(!scratch_root.empty() && !std::filesystem::exists(scratch_root), "ephemeral session scratch directory is removed when the store is released");
}

void test_session_record_round_trip()
{
  ava::session::SessionEntry const original{.id = "entry_\n",
                                            .parent_id = "parent_id",
                                            .type = ava::session::EntryType::ToolResult,
                                            .timestamp = "2026-04-27T00:00:00Z",
                                            .data_json = "{\"text\":\"hello\",\"nested\":{\"ok\":true}}"};

  auto line = ava::session::serialize_session_entry_line(original);
  auto const current_version_text = std::string("\"version\":") + std::to_string(ava::session::kCurrentSessionEntryVersion);
  expect(line && line->find('\n') == std::string::npos && line->find(current_version_text) != std::string::npos,
         line ? "session record serializer emits one current-version JSONL record"
              : "session record serializer emits one current-version JSONL record: " + line.error().format());
  if (!line)
    return;

  auto parsed = ava::session::parse_session_entry_line(*line, "/tmp/session.jsonl");
  expect(parsed && parsed->id == original.id && parsed->parent_id == original.parent_id && parsed->type == original.type &&
             parsed->timestamp == original.timestamp && parsed->data_json == original.data_json && parsed->version == ava::session::kCurrentSessionEntryVersion,
         parsed ? "session record parser round-trips serialized entries" : "session record parser round-trips serialized entries: " + parsed.error().format());

  auto legacy = ava::session::parse_session_entry_line(
      "{\"version\":1,\"id\":\"entry_\\uD834\\uDD1E\",\"parent_id\":\"\",\"type\":\"user_message\","
      "\"timestamp\":\"2026-04-27T00:00:00Z\",\"data\":{\"text\":\"hello\"}}",
      "/tmp/session.jsonl");
  expect(legacy && legacy->id == std::string("entry_\xF0\x9D\x84\x9E") && legacy->version == 1,
         legacy ? "session record parser decodes legacy unicode escapes" : "session record parser decodes legacy unicode escapes: " + legacy.error().format());

  auto unsupported = ava::session::parse_session_entry_line(
      "{\"version\":999,\"id\":\"entry\",\"parent_id\":\"\",\"type\":\"user_message\","
      "\"timestamp\":\"2026-04-27T00:00:00Z\",\"data\":{\"text\":\"hello\"}}",
      "/tmp/session.jsonl");
  expect(!unsupported && ava::session::is_unsupported_session_version_error(unsupported.error()),
         "session record parser flags unsupported versions explicitly");

  auto malformed = ava::session::parse_session_entry_line(
      "{\"version\":2,\"id\":\"entry\",\"parent_id\":\"bad/slash\",\"type\":\"user_message\","
      "\"timestamp\":\"2026-04-27T00:00:00Z\",\"data\":{\"text\":\"hello\"}}",
      "/tmp/session.jsonl");
  expect(!malformed && malformed.error().format().find("parent_id") != std::string::npos, "session record parser rejects unsafe parent ids");

  std::string invalid_utf8_record =
      "{\"version\":2,\"id\":\"entry\",\"parent_id\":\"\",\"type\":\"user_message\",\"timestamp\":\"2026-04-27T00:00:00Z\",\"data\":{\"text\":\"";
  invalid_utf8_record.push_back(static_cast<char>(0xFF));
  invalid_utf8_record += "\"}}";
  auto invalid_utf8 = ava::session::parse_session_entry_line(invalid_utf8_record, "/tmp/session.jsonl");
  expect(!invalid_utf8 && invalid_utf8.error().message() == "session entry is not valid UTF-8",
         "session persistence parsing rejects invalid UTF-8 before semantic field extraction");

  auto too_large = ava::session::serialize_session_entry_line(ava::session::SessionEntry{
      .id = std::string(600000, '"'),
      .parent_id = "",
      .type = ava::session::EntryType::UserMessage,
      .timestamp = "2026-04-27T00:00:00Z",
      .data_json = "{\"text\":\"hello\"}",
  });
  expect(!too_large, "session record serializer rejects oversized records before append");
}

void test_session_store_sync_failures_are_not_committed()
{
  auto const root = create_empty_root("session-store-sync-failure");
  auto const workspace = root / "workspace";
  std::filesystem::create_directories(workspace);
  auto const has_context = [](ava::core::Error const& error, std::string_view key, std::string_view value) {
    return std::ranges::any_of(error.context(), [&](ava::core::ErrorContext const& item) { return item.key == key && item.value == value; });
  };
  ava::session::SessionEntry const ordinary{.id = "durable-user",
                                            .parent_id = "",
                                            .type = ava::session::EntryType::UserMessage,
                                            .timestamp = "2026-10-19T00:00:00Z",
                                            .data_json = "{\"text\":\"durable\"}"};
  auto item_data = ava::session::serialize_assistant_output_item_data_json(ava::session::AssistantOutputItem{
      .assistant_turn_id = "sync-turn",
      .sequence = 0,
      .kind = ava::session::AssistantOutputItemKind::Text,
      .provider_item_id = "sync-provider-item",
      .provider_output_index = 0,
      .payload = ava::session::AssistantOutputText{.text = "staged", .assistant_phase = ava::session::AssistantOutputTextPhase::Commentary}});
  auto commit_data = ava::session::serialize_assistant_turn_commit_data_json(ava::session::AssistantTurnCommit{.assistant_turn_id = "sync-turn",
                                                                                                               .item_count = 1,
                                                                                                               .provider = "openai",
                                                                                                               .model = "offline-test",
                                                                                                               .finish_reason = "completed",
                                                                                                               .usage_json = std::nullopt});
  expect(item_data && commit_data, "sync failure fixture serializes a complete assistant transaction");
  if (!item_data || !commit_data)
    return;

  for (bool const batch : {false, true})
  {
    auto store = ava::session::SessionStore::create(workspace, root / (batch ? "batch" : "single"));
    expect(store.has_value(), "sync failure fixture creates a session store");
    if (!store)
      return;
    auto lease = ava::session::SessionLease::create_and_acquire(store->session_path());
    expect(lease.has_value(), "normal new-session creation succeeds before injected append sync failure");
    if (!lease)
      return;
    auto normal_append = store->append(*lease, ordinary);
    expect(normal_append.has_value(), "normal append succeeds before injected sync failure");
    if (!normal_append)
      return;

    bool closed_for_sync_failure = false;
    bool post_write_hook_called = false;
    std::size_t writes = 0;
    // Reuse the write seam: invalidate only the append descriptor after all
    // bytes are written, forcing the following durability barrier to fail.
    store->set_append_write_for_test([&](int fd, std::string_view bytes) -> ssize_t {
      ++writes;
      auto const count = ::write(fd, bytes.data(), bytes.size());
      if (count > 0 && static_cast<std::size_t>(count) == bytes.size())
        closed_for_sync_failure = ::close(fd) == 0;
      return count;
    });
    store->set_after_append_write_for_test([&] { post_write_hook_called = true; });
    auto target = ava::session::SessionAppendTarget::create_persistent(*store, *lease, ava::session::SessionReadLimits{});
    expect(target.has_value(), "sync failure fixture creates an append authority");
    if (!target)
      return;

    auto candidate = ordinary;
    candidate.id = "sync-failed-user";
    std::vector<ava::session::SessionEntry> const batch_entries{ava::session::SessionEntry{.id = "sync-output",
                                                                                           .parent_id = "",
                                                                                           .type = ava::session::EntryType::AssistantOutputItem,
                                                                                           .timestamp = "2026-10-19T00:00:00Z",
                                                                                           .data_json = *item_data},
                                                                ava::session::SessionEntry{.id = "sync-commit",
                                                                                           .parent_id = "",
                                                                                           .type = ava::session::EntryType::AssistantTurnCommit,
                                                                                           .timestamp = "2026-10-19T00:00:00Z",
                                                                                           .data_json = *commit_data}};
    auto appended = batch ? (*target)->append_batch(batch_entries) : (*target)->append(candidate);
    auto blocked = (*target)->append(candidate);
    auto entries = store->load();
    expect(closed_for_sync_failure && !appended && appended.error().message().find("sync") != std::string::npos &&
               has_context(appended.error(), "append_commit_state", "partial_or_unknown"),
           "a complete write whose data sync fails reports partial_or_unknown rather than committed");
    expect(!appended && has_context(appended.error(), "append_close_cause", std::strerror(EBADF)),
           "sync failure preserves checked append-descriptor cleanup errors");
    expect(!post_write_hook_called && writes == 1 && entries && entries->size() == 2,
           "sync failure precedes post-write hooks and prevents a later batch commit record");
    expect(!blocked, "sync failure blocks further append authority mutations until explicit recovery");
    if (batch)
      expect(!appended && has_context(appended.error(), "batch_persisted_entries", "0"),
             "a batch whose first record fails data sync acknowledges no durably persisted records");
  }
}

void test_session_lease_creation_sync_failures()
{
  auto const root = create_empty_root("session-lease-sync-failure");
  auto const normal_path = root / "normal" / "ancestor" / "sessions" / "normal.jsonl";
  auto normal = ava::session::SessionLease::create_and_acquire(normal_path);
  expect(normal && normal->active() && std::filesystem::exists(normal_path), "normal new-session creation succeeds with newly created ancestor directories");

  for (int const denied_syscall : {SYS_fdatasync, SYS_fsync})
  {
    auto const path = root / (denied_syscall == SYS_fdatasync ? "file-sync" : "directory-sync") / "ancestor" / "sessions" / "failed.jsonl";
    auto const status = ava::tests::run_in_child_with_syscall_errno(denied_syscall, std::nullopt, EIO, [&] {
      auto failed = ava::session::SessionLease::create_and_acquire(path);
      bool const rejected = !failed && failed.error().message().find("sync") != std::string::npos && !std::filesystem::exists(path);
      return rejected ? 0 : 2;
    });
    ava::tests::expect_child_success(status, denied_syscall == SYS_fdatasync
                                                 ? "new-session creation rejects file sync failure before publication"
                                                 : "new-session creation rejects directory sync failure and removes its published name");
    if (ava::tests::skip_requested())
      return;
  }

  for (auto const& [ancestor_error, create_subtree] :
       {std::pair{EIO, false}, std::pair{EIO, true}, std::pair{EINVAL, true}, std::pair{ENOTSUP, true}, std::pair{EROFS, true}, std::pair{EACCES, true}})
  {
    auto const existing_ancestor = root / ("ancestor-error-" + std::to_string(ancestor_error) + (create_subtree ? "-new" : "-existing"));
    std::filesystem::create_directories(existing_ancestor);
    auto const path = existing_ancestor / "sessions" / "created.jsonl";
    if (!create_subtree)
      std::filesystem::create_directory(path.parent_path());
    // Reserve then release the first free slot before forking. Creation's parent
    // descriptor reuses it; its mandatory fsync stays allowed while ancestor fsyncs fail.
    int const parent_descriptor = ::open(root.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    bool const reserved = parent_descriptor >= 0 && ::close(parent_descriptor) == 0;
    expect(reserved, "ancestor sync fixture determines creation's parent descriptor");
    if (!reserved)
      return;
    auto const status = ava::tests::run_in_child_with_syscall_errno(
        SYS_fsync, ava::tests::SyscallArgumentFilter{.index = 0, .value = static_cast<std::uint32_t>(parent_descriptor), .equal = false}, ancestor_error, [&] {
          auto created = ava::session::SessionLease::create_and_acquire(path);
          if (ancestor_error == EIO && create_subtree)
          {
            bool const synced_receiving_directory =
                !created && created.error().message() == "failed to sync session lease ancestor directory" && !std::filesystem::exists(path);
            return synced_receiving_directory ? 0 : 2;
          }
          return created && created->active() && std::filesystem::exists(path) ? 0 : 2;
        });
    std::string const message = !create_subtree         ? "new-session creation does not sync pre-existing ancestors when no directories were created"
                                : ancestor_error == EIO ? "new-session creation syncs the pre-existing directory that received the new subtree"
                                                        : "new-session creation tolerates receiving-ancestor sync errno " + std::to_string(ancestor_error);
    ava::tests::expect_child_success(status, message);
    if (ava::tests::skip_requested())
      return;
  }
}

}  // namespace session_tests

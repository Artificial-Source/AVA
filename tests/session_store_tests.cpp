#include "sys.h"
#include "tests/session_test_declarations.h"
#include "tests/support/session_test_support.h"
#include "tests/support/test_harness.h"
#include "ava/session/attachments.h"
#include "ava/session/record.h"
#include "ava/session/session_store.h"
#include "ava/session/validation.h"
#include "ava/core/result.h"

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>
#include <vector>
#include <sys/stat.h>

namespace session_tests {
void test_session_store_round_trip()
{
  auto const root = create_empty_root("test_session_store_round_trip");

  static_assert(std::is_copy_constructible_v<ava::session::SessionStore>);
  static_assert(std::is_copy_assignable_v<ava::session::SessionStore>);
  static_assert(std::is_move_constructible_v<ava::session::SessionStore>);
  static_assert(std::is_move_assignable_v<ava::session::SessionStore>);

  // Explicit store special members must preserve historical shared attachments while every store
  // owns a distinct options wrapper and assignment retains the destination wrapper's stable address.
  auto copy_source_result = ava::session::SessionStore::create_ephemeral(root / "copy-source");
  expect(copy_source_result.has_value(), "ephemeral copy source creates");
  if (copy_source_result)
  {
    ava::session::SessionStore copy_source = std::move(*copy_source_result);
    auto appended = copy_source.append_ephemeral(
        {.id = "shared-entry", .parent_id = "", .type = ava::session::EntryType::SessionStart, .timestamp = "2026-04-27T00:00:00Z", .data_json = "{}"});
    ava::session::SessionStore copied(copy_source);
    auto copied_entries = copied.load();
    expect(appended && copied_entries && copied_entries->size() == 1 && copied_entries->front().id == "shared-entry",
           "copy construction retains shared ephemeral state");
    expect(&copied.options() != &copy_source.options(), "copy construction creates a distinct options wrapper");
    expect(copied.session_id() == copy_source.session_id(), "copy construction snapshots the protected options value");

    auto detached = copy_source.detached_copy_for_background_persistence();
    expect(&detached.options() != &copy_source.options() && detached.session_id() == copy_source.session_id(),
           "background persistence copy has a fresh options wrapper with the same value");

    ava::session::SessionStore assigned(ava::session::SessionStoreOptions{.root_dir = root, .workspace_dir = root, .session_id = "assignment-destination"});
    auto* assigned_options_address = &assigned.options();
    assigned = copy_source;
    auto assigned_entries = assigned.load();
    expect(&assigned.options() == assigned_options_address && assigned.session_id() == copy_source.session_id(),
           "copy assignment preserves the destination options wrapper address and copies its value");
    expect(assigned_entries && assigned_entries->size() == 1, "copy assignment retains shared ephemeral state");

    ava::session::SessionStore moved(std::move(copied));
    expect(moved.session_id() == copy_source.session_id(), "move construction transfers unregistered wrapped options");
    assigned_options_address = &assigned.options();
    assigned = std::move(moved);
    assigned_entries = assigned.load();
    expect(&assigned.options() == assigned_options_address && assigned.session_id() == copy_source.session_id(),
           "move assignment preserves the destination options wrapper address and transfers its value");
    expect(assigned_entries && assigned_entries->size() == 1, "move assignment retains transferred shared ephemeral state");

    std::string guarded_session_id;
    {
      auto assigned_options_r = std::as_const(assigned).options_r();
      guarded_session_id = assigned_options_r->session_id;
    }
    expect(guarded_session_id == assigned.session_id(), "const options accessor supports guarded wrapper reads");
  }

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

}  // namespace session_tests

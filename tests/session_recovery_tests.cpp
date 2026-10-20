#include "sys.h"
#include "tests/session_test_declarations.h"
#include "tests/support/session_test_support.h"
#include "tests/support/test_harness.h"
#include "ava/session/record.h"
#include "ava/session/session_branch.h"
#include "ava/session/session_store.h"
#include "ava/core/result.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <expected>
#include <filesystem>
#include <fstream>
#include <optional>
#include <ranges>
#include <string>
#include <system_error>
#include <utility>
#include <vector>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace session_tests {
void test_session_lease_creation_and_link_safety()
{
  auto const root = create_empty_root("session-lease-creation");

  auto const workspace = root / "workspace";
  auto const sessions = root / "sessions";
  std::filesystem::create_directories(workspace);

  ava::session::SessionStore fresh_store(ava::session::SessionStoreOptions{.root_dir = sessions, .workspace_dir = workspace, .session_id = "fresh_owned"});
  expect(!std::filesystem::exists(fresh_store.session_path()), "fresh lease fixture starts without a session file");
  auto fresh_lease = ava::session::SessionLease::create_and_acquire(fresh_store.session_path());
  struct stat fresh_status{};
  bool const fresh_status_valid = stat(fresh_store.session_path().c_str(), &fresh_status) == 0;
  auto fresh_contender = ava::session::SessionLease::acquire(fresh_store.session_path());
  expect(fresh_lease && fresh_status_valid && S_ISREG(fresh_status.st_mode) && (fresh_status.st_mode & 0777) == 0600 && fresh_status.st_nlink == 1 &&
             fresh_status.st_size == 0 && !fresh_contender && fresh_contender.error().message().find("already owned") != std::string::npos,
         "create_and_acquire atomically publishes a mode-0600 regular file already owned before its first append");

  auto created_again = ava::session::SessionLease::create_and_acquire(fresh_store.session_path());
  expect(!created_again && fresh_status_valid && ava::tests::read_session_test_binary_file(fresh_store.session_path()).empty(),
         "create_and_acquire uses exclusive creation and leaves an existing fresh session unchanged");

  auto first_append = fresh_lease ? fresh_store.append(*fresh_lease, ava::session::SessionEntry{.id = "fresh_first",
                                                                                                .parent_id = "",
                                                                                                .type = ava::session::EntryType::SessionStart,
                                                                                                .timestamp = "2026-07-14T00:00:00Z",
                                                                                                .data_json = "{\"mode\":\"build\"}"})
                                  : ava::core::VoidResult(std::unexpected(std::move(fresh_lease.error())));
  expect(first_append && ava::tests::read_session_test_binary_file(fresh_store.session_path()).ends_with('\n'),
         "the fresh owner can append the first framed record while retaining its lease");
  fresh_lease = ava::session::SessionLease{};

  auto const linked_path = std::filesystem::path(fresh_store.session_path().string() + ".linked");
  std::error_code link_error;
  std::filesystem::create_hard_link(fresh_store.session_path(), linked_path, link_error);
  auto linked_original_lease = ava::session::SessionLease::acquire(fresh_store.session_path());
  auto linked_alias_lease = ava::session::SessionLease::acquire(linked_path);
  expect(!link_error && !linked_original_lease && !linked_alias_lease &&
             linked_original_lease.error().message().find("exactly one link") != std::string::npos &&
             linked_alias_lease.error().message().find("exactly one link") != std::string::npos,
         "existing session leases reject multiply-linked files through either name");
}

void test_session_torn_tail_recovery()
{
  auto const root = create_empty_root("session-torn-tail-recovery");

  auto const workspace = root / "workspace";
  auto const sessions = root / "sessions";
  std::filesystem::create_directories(workspace);

  auto first_line = ava::session::serialize_session_entry_line(ava::session::SessionEntry{.id = "entry_first",
                                                                                          .parent_id = "",
                                                                                          .type = ava::session::EntryType::SessionStart,
                                                                                          .timestamp = "2026-07-14T00:00:00Z",
                                                                                          .data_json = "{\"mode\":\"build\"}"});
  auto second_line = ava::session::serialize_session_entry_line(ava::session::SessionEntry{.id = "entry_second",
                                                                                           .parent_id = "entry_first",
                                                                                           .type = ava::session::EntryType::UserMessage,
                                                                                           .timestamp = "2026-07-14T00:00:01Z",
                                                                                           .data_json = "{\"text\":\"escaped \\\\ slash \\\" quote\"}"});
  expect(first_line && second_line, "torn tail recovery test serializes representative entries");
  if (!first_line || !second_line)
    return;
  auto const valid_prefix = *first_line + "\n";
  auto recovery_files_for = [](std::filesystem::path const& session_path) {
    std::vector<std::filesystem::path> files;
    auto const final_prefix = session_path.filename().string() + ".torn-tail.";
    auto const temporary_prefix = "." + session_path.filename().string() + ".torn-tail.tmp.";
    std::error_code iter_error;
    for (std::filesystem::directory_iterator iterator(session_path.parent_path(), iter_error), end; !iter_error && iterator != end;
         iterator.increment(iter_error))
    {
      auto const name = iterator->path().filename().string();
      if (name.starts_with(final_prefix) || name.starts_with(temporary_prefix))
        files.push_back(iterator->path());
    }
    return files;
  };

  bool every_cut_recovered = true;
  for (std::size_t cut = 1; cut < second_line->size(); ++cut)
  {
    ava::session::SessionStore store(
        ava::session::SessionStoreOptions{.root_dir = sessions, .workspace_dir = workspace, .session_id = "cut_" + std::to_string(cut)});
    auto const suffix = second_line->substr(0, cut);
    ava::tests::write_session_test_binary_file(store.session_path(), valid_prefix + suffix);
    auto lease = ava::session::SessionLease::acquire(store.session_path());
    auto recovered = lease ? store.recover_torn_tail(*lease, ava::session::legacy_unbounded_session_read_limits())
                           : ava::core::Result<std::optional<std::filesystem::path>>(std::unexpected(std::move(lease.error())));
    if (!recovered || !recovered->has_value() || ava::tests::read_session_test_binary_file(store.session_path()) != valid_prefix ||
        ava::tests::read_session_test_binary_file(**recovered) != suffix)
    {
      every_cut_recovered = false;
      break;
    }
    struct stat quarantine_status{};
    if (stat(recovered->value().c_str(), &quarantine_status) != 0 || (quarantine_status.st_mode & 0777) != 0600)
    {
      every_cut_recovered = false;
      break;
    }
  }
  expect(every_cut_recovered, "every byte cut of a serialized second entry is quarantined exactly and repaired to the validated prefix");

  ava::session::SessionStore idempotent_store(ava::session::SessionStoreOptions{.root_dir = sessions, .workspace_dir = workspace, .session_id = "idempotent"});
  std::string const idempotent_suffix = "{\"version\":3,\"id\":\"partial";
  ava::tests::write_session_test_binary_file(idempotent_store.session_path(), valid_prefix + idempotent_suffix);
  auto idempotent_lease = ava::session::SessionLease::acquire(idempotent_store.session_path());
  auto first_recovery = idempotent_lease ? idempotent_store.recover_torn_tail(*idempotent_lease, ava::session::legacy_unbounded_session_read_limits())
                                         : ava::core::Result<std::optional<std::filesystem::path>>(std::unexpected(std::move(idempotent_lease.error())));
  auto second_recovery =
      idempotent_lease
          ? idempotent_store.recover_torn_tail(*idempotent_lease, ava::session::legacy_unbounded_session_read_limits())
          : ava::core::Result<std::optional<std::filesystem::path>>(std::unexpected(ava::core::Error(ava::core::ErrorCategory::Session, "missing lease")));
  expect(first_recovery && first_recovery->has_value() && second_recovery && !second_recovery->has_value() &&
             ava::tests::read_session_test_binary_file(idempotent_store.session_path()) == valid_prefix,
         "torn tail recovery is idempotent after quarantining one invalid suffix");

  ava::session::SessionStore no_lf_store(ava::session::SessionStoreOptions{.root_dir = sessions, .workspace_dir = workspace, .session_id = "complete_no_lf"});
  ava::tests::write_session_test_binary_file(no_lf_store.session_path(), *first_line);
  auto no_lf_lease = ava::session::SessionLease::acquire(no_lf_store.session_path());
  auto no_lf_recovered = no_lf_lease ? no_lf_store.recover_torn_tail(*no_lf_lease, ava::session::legacy_unbounded_session_read_limits())
                                     : ava::core::Result<std::optional<std::filesystem::path>>(std::unexpected(std::move(no_lf_lease.error())));
  expect(no_lf_recovered && !no_lf_recovered->has_value() && ava::tests::read_session_test_binary_file(no_lf_store.session_path()) == *first_line + "\n",
         "a complete supported final record gains exactly one LF without changing its bytes");

  ava::session::SessionStore framed_store(ava::session::SessionStoreOptions{.root_dir = sessions, .workspace_dir = workspace, .session_id = "framed"});
  ava::tests::write_session_test_binary_file(framed_store.session_path(), valid_prefix);
  auto framed_lease = ava::session::SessionLease::acquire(framed_store.session_path());
  auto framed_recovered = framed_lease ? framed_store.recover_torn_tail(*framed_lease, ava::session::legacy_unbounded_session_read_limits())
                                       : ava::core::Result<std::optional<std::filesystem::path>>(std::unexpected(std::move(framed_lease.error())));
  expect(framed_recovered && !framed_recovered->has_value() && ava::tests::read_session_test_binary_file(framed_store.session_path()) == valid_prefix,
         "a fully framed session is unchanged by recovery");

  std::vector<std::string> invalid_suffixes = {
      "{\"version\":3,\"id\":\"escape\\",
      std::string("{\"version\":3,\"id\":\"") + std::string("\xF0\x9F", 2),
      std::string("{\"version\":3,\"id\":\"nul") + std::string(1, '\0'),
      "{\r",
  };
  bool special_tails_recovered = true;
  for (std::size_t index = 0; index < invalid_suffixes.size(); ++index)
  {
    ava::session::SessionStore store(
        ava::session::SessionStoreOptions{.root_dir = sessions, .workspace_dir = workspace, .session_id = "special_" + std::to_string(index)});
    ava::tests::write_session_test_binary_file(store.session_path(), valid_prefix + invalid_suffixes[index]);
    auto lease = ava::session::SessionLease::acquire(store.session_path());
    auto recovered = lease ? store.recover_torn_tail(*lease, ava::session::legacy_unbounded_session_read_limits())
                           : ava::core::Result<std::optional<std::filesystem::path>>(std::unexpected(std::move(lease.error())));
    if (!recovered || !recovered->has_value() || ava::tests::read_session_test_binary_file(**recovered) != invalid_suffixes[index] ||
        ava::tests::read_session_test_binary_file(store.session_path()) != valid_prefix)
    {
      special_tails_recovered = false;
      break;
    }
  }
  expect(special_tails_recovered, "escape, partial UTF-8, NUL, and CR torn suffixes are quarantined byte-for-byte");

  ava::session::SessionStore quarantine_failure_store(
      ava::session::SessionStoreOptions{.root_dir = sessions, .workspace_dir = workspace, .session_id = std::string(240, 'q')});
  auto const quarantine_failure_bytes = valid_prefix + "{";
  ava::tests::write_session_test_binary_file(quarantine_failure_store.session_path(), quarantine_failure_bytes);
  auto quarantine_failure_lease = ava::session::SessionLease::acquire(quarantine_failure_store.session_path());
  auto quarantine_failure = quarantine_failure_lease
                                ? quarantine_failure_store.recover_torn_tail(*quarantine_failure_lease, ava::session::legacy_unbounded_session_read_limits())
                                : ava::core::Result<std::optional<std::filesystem::path>>(std::unexpected(std::move(quarantine_failure_lease.error())));
  expect(!quarantine_failure && quarantine_failure.error().message().find("quarantine") != std::string::npos &&
             ava::tests::read_session_test_binary_file(quarantine_failure_store.session_path()) == quarantine_failure_bytes,
         "a quarantine creation failure leaves the source session byte-for-byte unchanged");

  ava::session::SessionStore oversized_store(ava::session::SessionStoreOptions{.root_dir = sessions, .workspace_dir = workspace, .session_id = "oversized"});
  auto const oversized_bytes = valid_prefix + std::string(ava::session::kMaxSessionLineBytes + 1, '{');
  ava::tests::write_session_test_binary_file(oversized_store.session_path(), oversized_bytes);
  auto oversized_lease = ava::session::SessionLease::acquire(oversized_store.session_path());
  auto oversized_recovery = oversized_lease ? oversized_store.recover_torn_tail(*oversized_lease, ava::session::legacy_unbounded_session_read_limits())
                                            : ava::core::Result<std::optional<std::filesystem::path>>(std::unexpected(std::move(oversized_lease.error())));
  expect(!oversized_recovery && oversized_recovery.error().message().find("scan limit") != std::string::npos &&
             ava::tests::read_session_test_binary_file(oversized_store.session_path()) == oversized_bytes,
         "an oversized unterminated suffix fails the bounded scan without mutation");

  ava::session::SessionStore byte_limited_store(
      ava::session::SessionStoreOptions{.root_dir = sessions, .workspace_dir = workspace, .session_id = "byte_limited"});
  auto const byte_limited_bytes = valid_prefix + idempotent_suffix;
  ava::tests::write_session_test_binary_file(byte_limited_store.session_path(), byte_limited_bytes);
  auto byte_limited_lease = ava::session::SessionLease::acquire(byte_limited_store.session_path());
  auto byte_limited_recovery =
      byte_limited_lease ? byte_limited_store.recover_torn_tail(
                               *byte_limited_lease,
                               ava::session::SessionReadLimits{.max_file_bytes = valid_prefix.size(), .max_line_bytes = valid_prefix.size(), .max_entries = 8})
                         : ava::core::Result<std::optional<std::filesystem::path>>(std::unexpected(std::move(byte_limited_lease.error())));
  expect(!byte_limited_recovery && byte_limited_recovery.error().message().find("byte limit") != std::string::npos &&
             ava::tests::read_session_test_binary_file(byte_limited_store.session_path()) == byte_limited_bytes &&
             recovery_files_for(byte_limited_store.session_path()).empty(),
         "recovery enforces the initial file byte limit before publishing quarantine or mutating the source");

  ava::session::SessionStore entry_limited_store(
      ava::session::SessionStoreOptions{.root_dir = sessions, .workspace_dir = workspace, .session_id = "entry_limited"});
  auto const entry_limited_bytes = valid_prefix + *second_line + "\n" + idempotent_suffix;
  ava::tests::write_session_test_binary_file(entry_limited_store.session_path(), entry_limited_bytes);
  auto entry_limited_lease = ava::session::SessionLease::acquire(entry_limited_store.session_path());
  auto entry_limited_recovery =
      entry_limited_lease ? entry_limited_store.recover_torn_tail(
                                *entry_limited_lease, ava::session::SessionReadLimits{.max_file_bytes = 4096, .max_line_bytes = 2048, .max_entries = 1})
                          : ava::core::Result<std::optional<std::filesystem::path>>(std::unexpected(std::move(entry_limited_lease.error())));
  expect(!entry_limited_recovery && entry_limited_recovery.error().message().find("entry count") != std::string::npos &&
             ava::tests::read_session_test_binary_file(entry_limited_store.session_path()) == entry_limited_bytes &&
             recovery_files_for(entry_limited_store.session_path()).empty(),
         "recovery enforces the entry limit before publishing quarantine or mutating the source");

  ava::session::SessionStore canceled_store(ava::session::SessionStoreOptions{.root_dir = sessions, .workspace_dir = workspace, .session_id = "canceled_scan"});
  auto const canceled_bytes = valid_prefix + idempotent_suffix;
  ava::tests::write_session_test_binary_file(canceled_store.session_path(), canceled_bytes);
  auto canceled_lease = ava::session::SessionLease::acquire(canceled_store.session_path());
  int cancellation_checks = 0;
  auto canceled_recovery = canceled_lease ? canceled_store.recover_torn_tail(*canceled_lease, ava::session::legacy_unbounded_session_read_limits(),
                                                                             [&cancellation_checks] { return ++cancellation_checks >= 2; })
                                          : ava::core::Result<std::optional<std::filesystem::path>>(std::unexpected(std::move(canceled_lease.error())));
  expect(!canceled_recovery && canceled_recovery.error().message().find("canceled") != std::string::npos &&
             ava::tests::read_session_test_binary_file(canceled_store.session_path()) == canceled_bytes &&
             recovery_files_for(canceled_store.session_path()).empty(),
         "recovery observes cancellation while scanning before any source or quarantine mutation");

  std::vector<std::pair<std::string, std::string>> strict_failures = {
      {"semantic", "{}"},
      {"future", "{\"version\":99,\"id\":\"future\",\"parent_id\":\"\",\"type\":\"user_message\",\"timestamp\":\"2026-07-14T00:00:02Z\",\"data\":{}}"},
      {"unknown", "{\"version\":3,\"id\":\"unknown\",\"parent_id\":\"\",\"type\":\"new_type\",\"timestamp\":\"2026-07-14T00:00:02Z\",\"data\":{}}"},
      {"duplicate",
       "{\"version\":3,\"id\":\"one\",\"id\":\"two\",\"parent_id\":\"\",\"type\":\"user_message\",\"timestamp\":\"2026-07-14T00:00:02Z\",\"data\":{}}"},
  };
  std::string nested = "{\"version\":3,\"id\":\"deep\",\"parent_id\":\"\",\"type\":\"user_message\",\"timestamp\":\"2026-07-14T00:00:02Z\",\"data\":{\"x\":";
  nested += std::string(70, '[') + "0" + std::string(70, ']') + "}}";
  strict_failures.emplace_back("depth", std::move(nested));
  bool strict_failures_unchanged = true;
  for (auto const& [name, suffix] : strict_failures)
  {
    ava::session::SessionStore store(ava::session::SessionStoreOptions{.root_dir = sessions, .workspace_dir = workspace, .session_id = "strict_" + name});
    auto const original = valid_prefix + suffix;
    ava::tests::write_session_test_binary_file(store.session_path(), original);
    auto lease = ava::session::SessionLease::acquire(store.session_path());
    auto recovered = lease ? store.recover_torn_tail(*lease, ava::session::legacy_unbounded_session_read_limits())
                           : ava::core::Result<std::optional<std::filesystem::path>>(std::unexpected(std::move(lease.error())));
    if (recovered || ava::tests::read_session_test_binary_file(store.session_path()) != original)
    {
      strict_failures_unchanged = false;
      break;
    }
  }
  expect(strict_failures_unchanged, "strict-valid semantic/future/unknown records and duplicate/deep JSON tails fail unchanged");

  bool framed_legacy_records_unchanged = true;
  for (auto const& [name, record] : strict_failures)
  {
    ava::session::SessionStore store(
        ava::session::SessionStoreOptions{.root_dir = sessions, .workspace_dir = workspace, .session_id = "framed_strict_" + name});
    auto const original = valid_prefix + record + "\n";
    ava::tests::write_session_test_binary_file(store.session_path(), original);
    auto lease = ava::session::SessionLease::acquire(store.session_path());
    auto recovered = lease ? store.recover_torn_tail(*lease, ava::session::legacy_unbounded_session_read_limits())
                           : ava::core::Result<std::optional<std::filesystem::path>>(std::unexpected(std::move(lease.error())));
    if (!recovered || recovered->has_value() || ava::tests::read_session_test_binary_file(store.session_path()) != original)
    {
      framed_legacy_records_unchanged = false;
      break;
    }
  }
  expect(framed_legacy_records_unchanged,
         "newline-terminated legacy semantic/future/unknown and duplicate/deep JSON records bypass strict recovery classification without mutation");

  auto duplicate_id_line = ava::session::serialize_session_entry_line(ava::session::SessionEntry{.id = "entry_first",
                                                                                                 .parent_id = "",
                                                                                                 .type = ava::session::EntryType::UserMessage,
                                                                                                 .timestamp = "2026-07-14T00:00:02Z",
                                                                                                 .data_json = "{\"text\":\"duplicate\"}"});
  auto unknown_parent_line = ava::session::serialize_session_entry_line(ava::session::SessionEntry{.id = "entry_orphan",
                                                                                                   .parent_id = "entry_missing",
                                                                                                   .type = ava::session::EntryType::UserMessage,
                                                                                                   .timestamp = "2026-07-14T00:00:02Z",
                                                                                                   .data_json = "{\"text\":\"orphan\"}"});
  expect(duplicate_id_line && unknown_parent_line, "recovery integrity tests serialize duplicate and orphan records");
  if (duplicate_id_line && unknown_parent_line)
  {
    std::vector<std::pair<std::string, std::string>> const integrity_records = {{"duplicate_id", *duplicate_id_line}, {"unknown_parent", *unknown_parent_line}};
    for (auto const& [name, record] : integrity_records)
    {
      ava::session::SessionStore integrity_store(ava::session::SessionStoreOptions{.root_dir = sessions, .workspace_dir = workspace, .session_id = name});
      auto const integrity_bytes = valid_prefix + record + "\n" + idempotent_suffix;
      ava::tests::write_session_test_binary_file(integrity_store.session_path(), integrity_bytes);
      auto integrity_lease = ava::session::SessionLease::acquire(integrity_store.session_path());
      auto integrity_recovery = integrity_lease ? integrity_store.recover_torn_tail(*integrity_lease, ava::session::legacy_unbounded_session_read_limits())
                                                : ava::core::Result<std::optional<std::filesystem::path>>(std::unexpected(std::move(integrity_lease.error())));
      auto const expected_message = name == "duplicate_id" ? "duplicate entry id" : "earlier record";
      expect(!integrity_recovery && integrity_recovery.error().message().find(expected_message) != std::string::npos &&
                 ava::tests::read_session_test_binary_file(integrity_store.session_path()) == integrity_bytes &&
                 recovery_files_for(integrity_store.session_path()).empty(),
             "recovery rejects " + name + " without source mutation or quarantine publication");
    }
  }

  ava::session::SessionStore middle_store(ava::session::SessionStoreOptions{.root_dir = sessions, .workspace_dir = workspace, .session_id = "middle"});
  auto const middle_bytes = valid_prefix + "{\n" + *second_line + "\n";
  ava::tests::write_session_test_binary_file(middle_store.session_path(), middle_bytes);
  auto middle_lease = ava::session::SessionLease::acquire(middle_store.session_path());
  auto middle_recovery = middle_lease ? middle_store.recover_torn_tail(*middle_lease, ava::session::legacy_unbounded_session_read_limits())
                                      : ava::core::Result<std::optional<std::filesystem::path>>(std::unexpected(std::move(middle_lease.error())));
  expect(middle_recovery && !middle_recovery->has_value() && ava::tests::read_session_test_binary_file(middle_store.session_path()) == middle_bytes,
         "newline-terminated middle corruption is outside torn-tail recovery and is never mutated");
  auto branch = ava::session::create_session_branch(ava::session::SessionBranchOptions{.workspace_dir = workspace,
                                                                                       .root_dir = sessions,
                                                                                       .source_session_id = "middle",
                                                                                       .branch_from_entry_id = {},
                                                                                       .name = std::nullopt,
                                                                                       .labels = std::nullopt,
                                                                                       .mode = ava::session::SessionBranchMode::Clone,
                                                                                       .actor = "test"});
  expect(!branch && ava::tests::read_session_test_binary_file(middle_store.session_path()) == middle_bytes,
         "low-level branch creation remains read-only and fail-closed on corruption");

  ava::session::SessionStore append_store(ava::session::SessionStoreOptions{.root_dir = sessions, .workspace_dir = workspace, .session_id = "append_guard"});
  auto const first_entry = ava::session::SessionEntry{.id = "append_first",
                                                      .parent_id = "",
                                                      .type = ava::session::EntryType::UserMessage,
                                                      .timestamp = "2026-07-14T00:00:03Z",
                                                      .data_json = "{\"text\":\"first\"}"};
  auto const second_entry = ava::session::SessionEntry{.id = "append_second",
                                                       .parent_id = "append_first",
                                                       .type = ava::session::EntryType::UserMessage,
                                                       .timestamp = "2026-07-14T00:00:04Z",
                                                       .data_json = "{\"text\":\"second\"}"};
  expect(append_session_entry_for_test(append_store, first_entry).has_value(), "append guard test seeds a framed session");
  auto append_unterminated = ava::tests::read_session_test_binary_file(append_store.session_path());
  append_unterminated.pop_back();
  ava::tests::write_session_test_binary_file(append_store.session_path(), append_unterminated);
  auto guarded_append = append_session_entry_for_test(append_store, second_entry);
  expect(!guarded_append && guarded_append.error().message().find("unterminated tail") != std::string::npos &&
             ava::tests::read_session_test_binary_file(append_store.session_path()) == append_unterminated,
         "append refuses to concatenate onto an existing nonempty unterminated session");
  auto append_lease = ava::session::SessionLease::acquire(append_store.session_path());
  auto append_recovered = append_lease ? append_store.recover_torn_tail(*append_lease, ava::session::legacy_unbounded_session_read_limits())
                                       : ava::core::Result<std::optional<std::filesystem::path>>(std::unexpected(std::move(append_lease.error())));
  auto append_after_recovery =
      append_recovered ? append_store.append(*append_lease, second_entry) : ava::core::VoidResult(std::unexpected(append_recovered.error()));
  expect(append_recovered && append_after_recovery && append_store.load() && append_store.load()->size() == 2,
         "append proceeds after the leased recovery restores JSONL framing");

  ava::session::SessionStore replaced_append_store(
      ava::session::SessionStoreOptions{.root_dir = sessions, .workspace_dir = workspace, .session_id = "append_replaced_target"});
  expect(append_session_entry_for_test(replaced_append_store, first_entry).has_value(), "append replacement test seeds the intended inode");
  auto const append_original_bytes = ava::tests::read_session_test_binary_file(replaced_append_store.session_path());
  auto const append_replacement_bytes = valid_prefix;
  auto const append_displaced_path = replaced_append_store.session_path().string() + ".displaced";
  replaced_append_store.set_before_append_identity_check_for_test([&] {
    std::filesystem::rename(replaced_append_store.session_path(), append_displaced_path);
    ava::tests::write_session_test_binary_file(replaced_append_store.session_path(), append_replacement_bytes);
  });
  auto replaced_append = append_session_entry_for_test(replaced_append_store, second_entry);
  expect(!replaced_append && replaced_append.error().message().find("replaced") != std::string::npos &&
             ava::tests::read_session_test_binary_file(append_displaced_path) == append_original_bytes &&
             ava::tests::read_session_test_binary_file(replaced_append_store.session_path()) == append_replacement_bytes,
         "append rejects target replacement after opening and mutates neither the displaced inode nor replacement path");

  ava::session::SessionStore post_write_replaced_store(
      ava::session::SessionStoreOptions{.root_dir = sessions, .workspace_dir = workspace, .session_id = "append_post_write_replaced"});
  expect(append_session_entry_for_test(post_write_replaced_store, first_entry).has_value(), "post-write append replacement test seeds the intended inode");
  auto const post_write_original_bytes = ava::tests::read_session_test_binary_file(post_write_replaced_store.session_path());
  auto const post_write_displaced_path = post_write_replaced_store.session_path().string() + ".displaced";
  post_write_replaced_store.set_after_append_write_for_test([&] {
    std::filesystem::rename(post_write_replaced_store.session_path(), post_write_displaced_path);
    ava::tests::write_session_test_binary_file(post_write_replaced_store.session_path(), append_replacement_bytes);
  });
  auto post_write_replaced = append_session_entry_for_test(post_write_replaced_store, second_entry);
  expect(!post_write_replaced && post_write_replaced.error().message().find("after the entry write") != std::string::npos &&
             ava::tests::read_session_test_binary_file(post_write_displaced_path).starts_with(post_write_original_bytes) &&
             ava::tests::read_session_test_binary_file(post_write_displaced_path).find("append_second") != std::string::npos &&
             ava::tests::read_session_test_binary_file(post_write_displaced_path).ends_with("\n") &&
             ava::tests::read_session_test_binary_file(post_write_replaced_store.session_path()) == append_replacement_bytes,
         "append reports namespace replacement after mutating only its anchored descriptor");

  ava::session::SessionStore strict_append_store(
      ava::session::SessionStoreOptions{.root_dir = sessions, .workspace_dir = workspace, .session_id = "strict_append"});
  std::string deeply_nested_data = "{\"nested\":" + std::string(70, '[') + "0" + std::string(70, ']') + "}";
  std::vector<ava::session::SessionEntry> strict_append_failures = {
      ava::session::SessionEntry{.id = "append_duplicate_json",
                                 .parent_id = "",
                                 .type = ava::session::EntryType::UserMessage,
                                 .timestamp = "2026-07-14T00:00:05Z",
                                 .data_json = "{\"text\":\"one\",\"text\":\"two\"}"},
      ava::session::SessionEntry{.id = "append_invalid_json",
                                 .parent_id = "",
                                 .type = ava::session::EntryType::UserMessage,
                                 .timestamp = "2026-07-14T00:00:05Z",
                                 .data_json = "{\"text\":}"},
      ava::session::SessionEntry{.id = "append_deep_json",
                                 .parent_id = "",
                                 .type = ava::session::EntryType::UserMessage,
                                 .timestamp = "2026-07-14T00:00:05Z",
                                 .data_json = std::move(deeply_nested_data)},
      ava::session::SessionEntry{.id = "append_future_version",
                                 .parent_id = "",
                                 .type = ava::session::EntryType::UserMessage,
                                 .timestamp = "2026-07-14T00:00:05Z",
                                 .data_json = "{}",
                                 .version = 99},
  };
  bool strict_disk_append_rejected = true;
  for (auto const& invalid_entry : strict_append_failures)
    strict_disk_append_rejected = strict_disk_append_rejected && !append_session_entry_for_test(strict_append_store, invalid_entry);
  auto strict_ephemeral_store = ava::session::SessionStore::create_ephemeral(workspace);
  auto strict_ephemeral_append = strict_ephemeral_store ? append_session_entry_for_test(*strict_ephemeral_store, strict_append_failures.front())
                                                        : ava::core::VoidResult(std::unexpected(strict_ephemeral_store.error()));
  expect(strict_disk_append_rejected && strict_append_store.load() && strict_append_store.load()->empty() && !strict_ephemeral_append,
         "disk and ephemeral append reject records that the strict recovery parser would reject");

  auto competing_lease = ava::session::SessionLease::acquire(append_store.session_path());
  expect(!competing_lease && competing_lease.error().message().find("already owned") != std::string::npos,
         "torn tail recovery lease excludes a competing owner");
  ava::session::SessionStore other_store(ava::session::SessionStoreOptions{.root_dir = sessions, .workspace_dir = workspace, .session_id = "other_lease"});
  ava::tests::write_session_test_binary_file(other_store.session_path(), valid_prefix);
  auto other_lease = ava::session::SessionLease::acquire(other_store.session_path());
  auto mismatch = other_lease ? append_store.recover_torn_tail(*other_lease, ava::session::legacy_unbounded_session_read_limits())
                              : ava::core::Result<std::optional<std::filesystem::path>>(std::unexpected(std::move(other_lease.error())));
  expect(!mismatch && mismatch.error().message().find("does not match") != std::string::npos, "torn tail recovery rejects a lease for another session");

  bool stale_rejected = false;
  if (other_lease)
  {
    [[maybe_unused]] ava::session::SessionLease moved_stale_owner = std::move(*other_lease);
    auto stale = other_store.recover_torn_tail(*other_lease, ava::session::legacy_unbounded_session_read_limits());
    stale_rejected = !stale && stale.error().message().find("active session lease") != std::string::npos;
  }
  expect(stale_rejected, "torn tail recovery rejects a moved-from stale lease");

  ava::session::SessionStore replaced_store(
      ava::session::SessionStoreOptions{.root_dir = sessions, .workspace_dir = workspace, .session_id = "replaced_target"});
  auto const replaced_bytes = valid_prefix + idempotent_suffix;
  ava::tests::write_session_test_binary_file(replaced_store.session_path(), replaced_bytes);
  auto replaced_lease = ava::session::SessionLease::acquire(replaced_store.session_path());
  auto const displaced_path = replaced_store.session_path().string() + ".displaced";
  std::filesystem::rename(replaced_store.session_path(), displaced_path);
  ava::tests::write_session_test_binary_file(replaced_store.session_path(), replaced_bytes);
  auto replaced = replaced_lease ? replaced_store.recover_torn_tail(*replaced_lease, ava::session::legacy_unbounded_session_read_limits())
                                 : ava::core::Result<std::optional<std::filesystem::path>>(std::unexpected(std::move(replaced_lease.error())));
  expect(!replaced && replaced.error().message().find("does not identify the recovery target") != std::string::npos &&
             ava::tests::read_session_test_binary_file(replaced_store.session_path()) == replaced_bytes &&
             ava::tests::read_session_test_binary_file(displaced_path) == replaced_bytes,
         "torn tail recovery rejects a path replaced with a different inode and leaves both files unchanged");

  ava::session::SessionStore prepublication_store(
      ava::session::SessionStoreOptions{.root_dir = sessions, .workspace_dir = workspace, .session_id = "prepublication_cleanup"});
  auto const prepublication_bytes = valid_prefix + idempotent_suffix;
  ava::tests::write_session_test_binary_file(prepublication_store.session_path(), prepublication_bytes);
  auto prepublication_lease = ava::session::SessionLease::acquire(prepublication_store.session_path());
  bool saw_prepublication_temporary = false;
  auto prepublication_recovery =
      prepublication_lease
          ? prepublication_store.recover_torn_tail(*prepublication_lease, ava::session::legacy_unbounded_session_read_limits(),
                                                   [&] {
                                                     auto const files = recovery_files_for(prepublication_store.session_path());
                                                     saw_prepublication_temporary = std::ranges::any_of(files, [](std::filesystem::path const& candidate) {
                                                       return candidate.filename().string().find(".torn-tail.tmp.") != std::string::npos;
                                                     });
                                                     return saw_prepublication_temporary;
                                                   })
          : ava::core::Result<std::optional<std::filesystem::path>>(std::unexpected(std::move(prepublication_lease.error())));
  expect(!prepublication_recovery && saw_prepublication_temporary &&
             ava::tests::read_session_test_binary_file(prepublication_store.session_path()) == prepublication_bytes &&
             recovery_files_for(prepublication_store.session_path()).empty(),
         "a prepublication cancellation removes the temporary quarantine and leaves no final-looking artifact");

  ava::session::SessionStore quarantine_swap_store(
      ava::session::SessionStoreOptions{.root_dir = sessions, .workspace_dir = workspace, .session_id = "quarantine_temp_swap"});
  auto const quarantine_swap_bytes = valid_prefix + idempotent_suffix;
  ava::tests::write_session_test_binary_file(quarantine_swap_store.session_path(), quarantine_swap_bytes);
  auto quarantine_swap_lease = ava::session::SessionLease::acquire(quarantine_swap_store.session_path());
  std::filesystem::path attacker_temporary_path;
  std::filesystem::path validated_stash_path;
  struct stat attacker_status_before{};
  bool quarantine_temporary_swapped = false;
  quarantine_swap_store.set_before_recovery_quarantine_publication_for_test([&](std::filesystem::path const& temporary_path) {
    attacker_temporary_path = temporary_path;
    validated_stash_path = temporary_path.string() + ".validated-stash";
    std::filesystem::rename(temporary_path, validated_stash_path);
    ava::tests::write_session_test_binary_file(temporary_path, "ATTACKER_QUARANTINE_INODE_CANARY");
    quarantine_temporary_swapped = ::stat(temporary_path.c_str(), &attacker_status_before) == 0;
  });
  auto quarantine_swap_recovery = quarantine_swap_lease
                                      ? quarantine_swap_store.recover_torn_tail(*quarantine_swap_lease, ava::session::legacy_unbounded_session_read_limits())
                                      : ava::core::Result<std::optional<std::filesystem::path>>(std::unexpected(std::move(quarantine_swap_lease.error())));
  std::vector<std::filesystem::path> exact_swap_quarantines;
  std::error_code swap_iter_error;
  auto const swap_final_prefix = quarantine_swap_store.session_path().filename().string() + ".torn-tail.";
  for (std::filesystem::directory_iterator iterator(quarantine_swap_store.session_path().parent_path(), swap_iter_error), end;
       !swap_iter_error && iterator != end; iterator.increment(swap_iter_error))
  {
    if (iterator->path().filename().string().starts_with(swap_final_prefix))
      exact_swap_quarantines.push_back(iterator->path());
  }
  struct stat attacker_status_after{};
  struct stat published_status{};
  bool const attacker_preserved = quarantine_temporary_swapped && ::stat(attacker_temporary_path.c_str(), &attacker_status_after) == 0 &&
                                  attacker_status_before.st_dev == attacker_status_after.st_dev &&
                                  attacker_status_before.st_ino == attacker_status_after.st_ino &&
                                  ava::tests::read_session_test_binary_file(attacker_temporary_path) == "ATTACKER_QUARANTINE_INODE_CANARY";
  bool const exact_descriptor_published = exact_swap_quarantines.size() == 1 && ::stat(exact_swap_quarantines.front().c_str(), &published_status) == 0 &&
                                          ava::tests::read_session_test_binary_file(exact_swap_quarantines.front()) == idempotent_suffix &&
                                          ava::tests::read_session_test_binary_file(validated_stash_path) == idempotent_suffix &&
                                          (published_status.st_dev != attacker_status_after.st_dev || published_status.st_ino != attacker_status_after.st_ino);
  expect(!quarantine_swap_recovery && quarantine_swap_recovery.error().message().find("temporary name was replaced") != std::string::npos &&
             ava::tests::read_session_test_binary_file(quarantine_swap_store.session_path()) == quarantine_swap_bytes && attacker_preserved &&
             exact_descriptor_published,
         "quarantine publication links the validated descriptor, preserves a swapped attacker inode, and fails before source recovery");

  ava::session::SessionStore committed_cancellation_store(
      ava::session::SessionStoreOptions{.root_dir = sessions, .workspace_dir = workspace, .session_id = "committed_cancellation"});
  auto const committed_cancellation_bytes = valid_prefix + idempotent_suffix;
  ava::tests::write_session_test_binary_file(committed_cancellation_store.session_path(), committed_cancellation_bytes);
  auto committed_cancellation_lease = ava::session::SessionLease::acquire(committed_cancellation_store.session_path());
  bool cancel_after_publication = false;
  committed_cancellation_store.set_after_recovery_quarantine_publication_for_test([&] { cancel_after_publication = true; });
  auto committed_cancellation_recovery =
      committed_cancellation_lease ? committed_cancellation_store.recover_torn_tail(*committed_cancellation_lease, ava::session::SessionReadLimits{},
                                                                                    [&] { return cancel_after_publication; })
                                   : ava::core::Result<std::optional<std::filesystem::path>>(std::unexpected(std::move(committed_cancellation_lease.error())));
  auto repeated_committed_recovery = committed_cancellation_lease
                                         ? committed_cancellation_store.recover_torn_tail(*committed_cancellation_lease, ava::session::SessionReadLimits{})
                                         : ava::core::Result<std::optional<std::filesystem::path>>(
                                               std::unexpected(ava::core::Error(ava::core::ErrorCategory::Session, "missing committed recovery lease")));
  auto const committed_recovery_files = recovery_files_for(committed_cancellation_store.session_path());
  expect(committed_cancellation_recovery && committed_cancellation_recovery->has_value() && repeated_committed_recovery &&
             !repeated_committed_recovery->has_value() &&
             ava::tests::read_session_test_binary_file(committed_cancellation_store.session_path()) == valid_prefix && committed_recovery_files.size() == 1 &&
             ava::tests::read_session_test_binary_file(committed_recovery_files.front()) == idempotent_suffix,
         "quarantine publication commits cancellation handling through truncation and repeated recovery does not leak another artifact");

  ava::session::SessionStore truncate_replacement_store(
      ava::session::SessionStoreOptions{.root_dir = sessions, .workspace_dir = workspace, .session_id = "replace_before_truncate"});
  auto const truncate_replacement_bytes = valid_prefix + idempotent_suffix;
  auto const truncate_replacement_new_bytes = valid_prefix;
  ava::tests::write_session_test_binary_file(truncate_replacement_store.session_path(), truncate_replacement_bytes);
  auto truncate_replacement_lease = ava::session::SessionLease::acquire(truncate_replacement_store.session_path());
  auto const truncate_displaced_path = truncate_replacement_store.session_path().string() + ".displaced";
  bool replaced_before_truncate = false;
  truncate_replacement_store.set_after_recovery_quarantine_publication_for_test([&] {
    std::filesystem::rename(truncate_replacement_store.session_path(), truncate_displaced_path);
    ava::tests::write_session_test_binary_file(truncate_replacement_store.session_path(), truncate_replacement_new_bytes);
    replaced_before_truncate = true;
  });
  auto truncate_replacement =
      truncate_replacement_lease
          ? truncate_replacement_store.recover_torn_tail(*truncate_replacement_lease, ava::session::legacy_unbounded_session_read_limits())
          : ava::core::Result<std::optional<std::filesystem::path>>(std::unexpected(std::move(truncate_replacement_lease.error())));
  auto truncate_replacement_quarantines = recovery_files_for(truncate_replacement_store.session_path());
  bool exact_published_quarantine =
      truncate_replacement_quarantines.size() == 1 && ava::tests::read_session_test_binary_file(truncate_replacement_quarantines.front()) == idempotent_suffix;
  if (exact_published_quarantine)
  {
    struct stat status{};
    exact_published_quarantine = stat(truncate_replacement_quarantines.front().c_str(), &status) == 0 && (status.st_mode & 0777) == 0600;
  }
  expect(!truncate_replacement && replaced_before_truncate && truncate_replacement.error().format().find("quarantine_path") != std::string::npos &&
             ava::tests::read_session_test_binary_file(truncate_displaced_path) == truncate_replacement_bytes &&
             ava::tests::read_session_test_binary_file(truncate_replacement_store.session_path()) == truncate_replacement_new_bytes &&
             exact_published_quarantine,
         "session-name replacement after quarantine publication is detected before truncate and preserves the exact mode-0600 quarantine");

  ava::session::SessionStore parent_replacement_store(
      ava::session::SessionStoreOptions{.root_dir = sessions, .workspace_dir = workspace, .session_id = "parent_replace_before_truncate"});
  auto const parent_replacement_bytes = valid_prefix + idempotent_suffix;
  ava::tests::write_session_test_binary_file(parent_replacement_store.session_path(), parent_replacement_bytes);
  auto parent_replacement_lease = ava::session::SessionLease::acquire(parent_replacement_store.session_path());
  auto const original_parent = parent_replacement_store.session_path().parent_path();
  auto const displaced_parent = std::filesystem::path(original_parent.string() + ".displaced");
  bool parent_replaced_before_truncate = false;
  parent_replacement_store.set_after_recovery_quarantine_publication_for_test([&] {
    std::filesystem::rename(original_parent, displaced_parent);
    std::filesystem::create_directories(original_parent);
    ava::tests::write_session_test_binary_file(parent_replacement_store.session_path(), valid_prefix);
    parent_replaced_before_truncate = true;
  });
  auto parent_replacement = parent_replacement_lease
                                ? parent_replacement_store.recover_torn_tail(*parent_replacement_lease, ava::session::legacy_unbounded_session_read_limits())
                                : ava::core::Result<std::optional<std::filesystem::path>>(std::unexpected(std::move(parent_replacement_lease.error())));
  auto const displaced_session = displaced_parent / parent_replacement_store.session_path().filename();
  std::vector<std::filesystem::path> displaced_quarantines;
  std::error_code displaced_iter_error;
  auto const displaced_final_prefix = parent_replacement_store.session_path().filename().string() + ".torn-tail.";
  for (std::filesystem::directory_iterator iterator(displaced_parent, displaced_iter_error), end; !displaced_iter_error && iterator != end;
       iterator.increment(displaced_iter_error))
  {
    auto const name = iterator->path().filename().string();
    if (name.starts_with(displaced_final_prefix) && name.find(".tmp.") == std::string::npos)
      displaced_quarantines.push_back(iterator->path());
  }
  expect(!parent_replacement && parent_replaced_before_truncate &&
             parent_replacement.error().message().find("directory namespace changed") != std::string::npos &&
             ava::tests::read_session_test_binary_file(displaced_session) == parent_replacement_bytes &&
             ava::tests::read_session_test_binary_file(parent_replacement_store.session_path()) == valid_prefix && displaced_quarantines.size() == 1 &&
             ava::tests::read_session_test_binary_file(displaced_quarantines.front()) == idempotent_suffix,
         "parent-directory replacement after quarantine publication is detected before truncate and preserves source and quarantine");

  auto ephemeral = ava::session::SessionStore::create_ephemeral(workspace);
  auto ephemeral_recovery = ephemeral && other_lease ? ephemeral->recover_torn_tail(*other_lease, ava::session::legacy_unbounded_session_read_limits())
                                                     : ava::core::Result<std::optional<std::filesystem::path>>(
                                                           std::unexpected(ava::core::Error(ava::core::ErrorCategory::Session, "missing test fixture")));
  expect(!ephemeral_recovery && ephemeral_recovery.error().message().find("ephemeral") != std::string::npos, "torn tail recovery rejects ephemeral stores");
}

void test_session_torn_tail_listing()
{
  auto const root = create_empty_root("session-torn-tail-listing");

  auto const workspace = root / "workspace";
  auto const sessions = root / "sessions";
  std::filesystem::create_directories(workspace);

  auto line = ava::session::serialize_session_entry_line(ava::session::SessionEntry{.id = "listed",
                                                                                    .parent_id = "",
                                                                                    .type = ava::session::EntryType::UserMessage,
                                                                                    .timestamp = "2026-07-14T00:00:00Z",
                                                                                    .data_json = "{\"text\":\"listed\"}"});
  expect(line.has_value(), "torn listing test serializes a valid record");
  if (!line)
    return;
  ava::session::SessionStore torn_store(ava::session::SessionStoreOptions{.root_dir = sessions, .workspace_dir = workspace, .session_id = "listed_torn"});
  auto const torn_bytes = *line + "\n{\"version\":3,\"id\":\"partial";
  ava::tests::write_session_test_binary_file(torn_store.session_path(), torn_bytes);
  ava::session::SessionStore complete_store(
      ava::session::SessionStoreOptions{.root_dir = sessions, .workspace_dir = workspace, .session_id = "listed_complete_no_lf"});
  ava::tests::write_session_test_binary_file(complete_store.session_path(), *line);

  auto listed = ava::session::SessionStore::list_sessions(workspace, sessions);
  bool normal_summaries_valid = listed && listed->size() == 2;
  if (normal_summaries_valid)
  {
    normal_summaries_valid = std::ranges::all_of(
        *listed, [](ava::session::SessionSummary const& summary) { return summary.entry_count == 1 && summary.last_updated == "2026-07-14T00:00:00Z"; });
  }
  expect(normal_summaries_valid && ava::tests::read_session_test_binary_file(torn_store.session_path()) == torn_bytes,
         "normal listing selects a valid prefix plus Invalid final suffix and includes a complete no-LF record without mutation");

  auto bounded = ava::session::SessionStore::list_sessions_bounded(workspace, sessions, ava::session::SessionListLimits{});
  expect(bounded && bounded->size() == 2 &&
             std::ranges::all_of(*bounded, [](ava::session::SessionSummary const& summary) { return summary.entry_count == 1; }) &&
             ava::tests::read_session_test_binary_file(torn_store.session_path()) == torn_bytes,
         "bounded and ACP-style listing derives summaries from validated records without repairing the file");

  ava::session::SessionStore framed_corrupt(
      ava::session::SessionStoreOptions{.root_dir = sessions, .workspace_dir = workspace, .session_id = "framed_corrupt"});
  auto const framed_corrupt_bytes = *line + "\n{\n";
  ava::tests::write_session_test_binary_file(framed_corrupt.session_path(), framed_corrupt_bytes);
  listed = ava::session::SessionStore::list_sessions(workspace, sessions);
  bounded = ava::session::SessionStore::list_sessions_bounded(workspace, sessions, ava::session::SessionListLimits{});
  expect(listed && listed->size() == 2 && !bounded && ava::tests::read_session_test_binary_file(framed_corrupt.session_path()) == framed_corrupt_bytes,
         "newline-terminated corruption keeps normal skip and bounded fail policies and is never treated as torn");
  std::filesystem::remove(framed_corrupt.session_path());

  std::string deep_record =
      "{\"version\":3,\"id\":\"deep\",\"parent_id\":\"\",\"type\":\"user_message\",\"timestamp\":\"2026-07-14T00:00:01Z\",\"data\":{\"x\":";
  deep_record += std::string(70, '[') + "0" + std::string(70, ']') + "}}";
  std::vector<std::pair<std::string, std::string>> non_torn_records = {
      {"semantic", "{}"},
      {"duplicate",
       "{\"version\":3,\"id\":\"one\",\"id\":\"two\",\"parent_id\":\"\",\"type\":\"user_message\",\"timestamp\":\"2026-07-14T00:00:01Z\",\"data\":{}}"},
      {"depth", std::move(deep_record)},
  };
  bool non_torn_listing_policy_preserved = true;
  for (auto const& [name, record] : non_torn_records)
  {
    ava::session::SessionStore store(ava::session::SessionStoreOptions{.root_dir = sessions, .workspace_dir = workspace, .session_id = "listed_" + name});
    auto const original = *line + "\n" + record;
    ava::tests::write_session_test_binary_file(store.session_path(), original);
    auto normal_result = ava::session::SessionStore::list_sessions(workspace, sessions);
    auto bounded_result = ava::session::SessionStore::list_sessions_bounded(workspace, sessions, ava::session::SessionListLimits{});
    if (!normal_result || normal_result->size() != 2 || bounded_result || ava::tests::read_session_test_binary_file(store.session_path()) != original)
    {
      non_torn_listing_policy_preserved = false;
      break;
    }
    std::filesystem::remove(store.session_path());
  }
  expect(non_torn_listing_policy_preserved, "strict-valid semantic errors and duplicate/deep final records keep normal-skip and bounded-fail listing policy");

  ava::session::SessionStore future_store(ava::session::SessionStoreOptions{.root_dir = sessions, .workspace_dir = workspace, .session_id = "listed_future"});
  auto const future_bytes =
      *line + "\n{\"version\":99,\"id\":\"future\",\"parent_id\":\"\",\"type\":\"user_message\",\"timestamp\":\"2026-07-14T00:00:01Z\",\"data\":{}}";
  ava::tests::write_session_test_binary_file(future_store.session_path(), future_bytes);
  listed = ava::session::SessionStore::list_sessions(workspace, sessions);
  bounded = ava::session::SessionStore::list_sessions_bounded(workspace, sessions, ava::session::SessionListLimits{});
  expect(!listed && listed.error().message().find("unsupported session entry version") != std::string::npos && !bounded &&
             bounded.error().message().find("unsupported session entry version") != std::string::npos &&
             ava::tests::read_session_test_binary_file(future_store.session_path()) == future_bytes,
         "a strict-valid future-version final record remains an actionable listing error and is not treated as torn");
}

void test_session_resume_and_listing()
{
  auto const root = create_empty_root("session-resume");

  auto const workspace = root / "workspace";
  std::filesystem::create_directories(workspace);
  auto const session_root = root / "sessions";

  auto first = ava::session::SessionStore::create(workspace, session_root);
  auto second = ava::session::SessionStore::create(workspace, session_root);
  expect(first && second, "resume test sessions create");
  if (!first || !second)
    return;

  expect(append_session_entry_for_test(*first, ava::session::SessionEntry{.id = "entry_first",
                                                                          .parent_id = "",
                                                                          .type = ava::session::EntryType::UserMessage,
                                                                          .timestamp = "2026-04-27T00:00:00Z",
                                                                          .data_json = "{\"text\":\"first\"}"})
             .has_value(),
         "first resume test session appends");
  expect(append_session_entry_for_test(*second, ava::session::SessionEntry{.id = "entry_second",
                                                                           .parent_id = "",
                                                                           .type = ava::session::EntryType::UserMessage,
                                                                           .timestamp = "2026-04-27T00:01:00Z",
                                                                           .data_json = "{\"text\":\"second\"}"})
             .has_value(),
         "second resume test session appends");

  auto const old_time = std::filesystem::file_time_type::clock::now() - std::chrono::minutes(2);
  auto const new_time = std::filesystem::file_time_type::clock::now();
  std::filesystem::last_write_time(first->session_path(), old_time);
  std::filesystem::last_write_time(second->session_path(), new_time);

  std::string const first_session_id = first->options_r()->session_id;
  auto reopened = ava::session::SessionStore::open(workspace, first_session_id, session_root);
  expect(reopened && reopened->options_r()->session_id == first_session_id, "session opens by exact id");
  if (reopened)
  {
    auto reopened_entries = reopened->load();
    expect(reopened_entries && reopened_entries->size() == 1 && (*reopened_entries)[0].id == "entry_first", "opened session loads original history");
  }

  auto listed = ava::session::SessionStore::list_sessions(workspace, session_root);
  expect(listed && listed->size() == 2, "sessions list for workspace");
  expect(listed && (*listed)[0].session_id == second->options_r()->session_id, "sessions list newest first");
  expect(listed && (*listed)[0].entry_count == 1 && (*listed)[0].last_updated == "2026-04-27T00:01:00Z",
         "session summary includes entry count and last timestamp");

  ava::session::SessionStore corrupt_store(ava::session::SessionStoreOptions{
      .root_dir = session_root,
      .workspace_dir = workspace,
      .session_id = "corrupt",
  });
  std::filesystem::create_directories(corrupt_store.session_path().parent_path());
  {
    std::ofstream file(corrupt_store.session_path(), std::ios::binary | std::ios::trunc);
    file << "{\"version\":1,\"id\":\"bad\",\"parent_id\":\"\",\"type\":\"not_real\","
            "\"timestamp\":\"2026-04-27T00:02:00Z\",\"data\":{}}\n";
  }
  listed = ava::session::SessionStore::list_sessions(workspace, session_root);
  expect(listed && listed->size() == 2, "session listing skips one corrupt session file");

  ava::session::SessionStore future_store(ava::session::SessionStoreOptions{
      .root_dir = session_root,
      .workspace_dir = workspace,
      .session_id = "future",
  });
  std::filesystem::create_directories(future_store.session_path().parent_path());
  {
    std::ofstream file(future_store.session_path(), std::ios::binary | std::ios::trunc);
    file << "{\"version\":99,\"id\":\"entry_future\",\"parent_id\":\"\",\"type\":\"user_message\","
            "\"timestamp\":\"2026-04-27T00:02:00Z\",\"data\":{\"text\":\"future\"}}\n";
  }
  listed = ava::session::SessionStore::list_sessions(workspace, session_root);
  expect(!listed && listed.error().message().find("unsupported session entry version") != std::string::npos,
         "session listing reports unsupported future-version session files instead of hiding them");
  std::filesystem::remove(future_store.session_path());

  ava::session::SessionStore crlf_store(ava::session::SessionStoreOptions{
      .root_dir = session_root,
      .workspace_dir = workspace,
      .session_id = "crlf",
  });
  std::filesystem::create_directories(crlf_store.session_path().parent_path());
  {
    std::ofstream file(crlf_store.session_path(), std::ios::binary | std::ios::trunc);
    file << "{\"version\":1,\"id\":\"entry_crlf\",\"parent_id\":\"\",\"type\":\"user_message\","
            "\"timestamp\":\"2026-04-27T00:03:00Z\",\"data\":{\"text\":\"hello\"}}\r\n";
  }
  auto crlf_loaded = crlf_store.load();
  expect(crlf_loaded && crlf_loaded->size() == 1 && (*crlf_loaded)[0].id == "entry_crlf", "session loader accepts CRLF line endings");

  ava::session::SessionStore symlink_store(ava::session::SessionStoreOptions{
      .root_dir = session_root,
      .workspace_dir = workspace,
      .session_id = "link",
  });
  std::filesystem::create_directories(symlink_store.session_path().parent_path());
  std::error_code symlink_error;
  std::filesystem::create_symlink(first->session_path(), symlink_store.session_path(), symlink_error);
  if (!symlink_error)
  {
    auto const symlink_target_bytes = ava::tests::read_session_test_binary_file(first->session_path());
    auto symlink_open = ava::session::SessionStore::open(workspace, "link", session_root);
    expect(!symlink_open && symlink_open.error().category() == ava::core::ErrorCategory::PermissionDenied, "session open rejects symlink session files");
    auto symlink_load = symlink_store.load();
    expect(!symlink_load && symlink_load.error().category() == ava::core::ErrorCategory::PermissionDenied, "session load rejects symlink session files");
    auto symlink_append = append_session_entry_for_test(symlink_store, ava::session::SessionEntry{.id = "entry_symlink",
                                                                                                  .parent_id = "",
                                                                                                  .type = ava::session::EntryType::UserMessage,
                                                                                                  .timestamp = "2026-04-27T00:04:00Z",
                                                                                                  .data_json = "{\"text\":\"bad\"}"});
    expect(!symlink_append && symlink_append.error().category() == ava::core::ErrorCategory::PermissionDenied &&
               ava::tests::read_session_test_binary_file(first->session_path()) == symlink_target_bytes,
           "session append rejects symlink session files without mutating the target");
    auto symlink_lease = ava::session::SessionLease::acquire(symlink_store.session_path());
    expect(!symlink_lease && symlink_lease.error().category() == ava::core::ErrorCategory::PermissionDenied &&
               ava::tests::read_session_test_binary_file(first->session_path()) == symlink_target_bytes,
           "session lease opens the original final component with O_NOFOLLOW and never follows its target");
  }

  auto missing = ava::session::SessionStore::open(workspace, "missing-session", session_root);
  expect(!missing && missing.error().category() == ava::core::ErrorCategory::NotFound, "missing session open fails");
  auto bad = ava::session::SessionStore::open(workspace, "../escape", session_root);
  expect(!bad && bad.error().category() == ava::core::ErrorCategory::InvalidArgument, "resume rejects invalid session id");
}

}  // namespace session_tests

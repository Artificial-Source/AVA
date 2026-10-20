#include "sys.h"
#include "tests/session_test_declarations.h"
#include "tests/support/session_test_support.h"
#include "tests/support/test_harness.h"
#include "ava/session/assistant_output.h"
#include "ava/session/record.h"
#include "ava/session/session_store.h"
#include "ava/core/result.h"

#include <algorithm>
#include <barrier>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <expected>
#include <filesystem>
#include <fstream>
#include <future>
#include <iterator>
#include <memory>
#include <new>
#include <optional>
#include <ranges>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>
#include <sys/stat.h>
#include <unistd.h>

namespace session_tests {
namespace {
std::size_t error_context_count(ava::core::Error const& error, std::string_view key)
{
  return static_cast<std::size_t>(std::ranges::count_if(error.context(), [&](ava::core::ErrorContext const& item) { return item.key == key; }));
}

std::optional<std::string> error_context_value(ava::core::Error const& error, std::string_view key)
{
  auto const item = std::ranges::find_if(error.context(), [&](ava::core::ErrorContext const& context) { return context.key == key; });
  return item == error.context().end() ? std::nullopt : std::optional<std::string>(item->value);
}

}  // namespace

void test_assistant_output_append_target_state_and_batches()
{
  auto const root = create_empty_root("assistant-output-append-target");
  auto const workspace = root / "workspace";
  auto const sessions = root / "sessions";
  std::filesystem::create_directories(workspace);

  auto item = [](std::string id, std::string turn_id, std::size_t sequence, std::string provider_item_id, std::size_t provider_output_index) {
    auto data = ava::session::serialize_assistant_output_item_data_json(ava::session::AssistantOutputItem{
        .assistant_turn_id = std::move(turn_id),
        .sequence = sequence,
        .kind = ava::session::AssistantOutputItemKind::Text,
        .provider_item_id = std::move(provider_item_id),
        .provider_output_index = provider_output_index,
        .payload = ava::session::AssistantOutputText{.text = "staged", .assistant_phase = ava::session::AssistantOutputTextPhase::Commentary}});
    return ava::session::SessionEntry{.id = std::move(id),
                                      .parent_id = "",
                                      .type = ava::session::EntryType::AssistantOutputItem,
                                      .timestamp = ava::session::now_timestamp(),
                                      .data_json = data.value_or("{}")};
  };
  auto commit = [](std::string id, std::string turn_id, std::size_t item_count) {
    auto data = ava::session::serialize_assistant_turn_commit_data_json(ava::session::AssistantTurnCommit{.assistant_turn_id = std::move(turn_id),
                                                                                                          .item_count = item_count,
                                                                                                          .provider = "openai",
                                                                                                          .model = "gpt-5.5",
                                                                                                          .finish_reason = "completed",
                                                                                                          .usage_json = std::nullopt});
    return ava::session::SessionEntry{.id = std::move(id),
                                      .parent_id = "",
                                      .type = ava::session::EntryType::AssistantTurnCommit,
                                      .timestamp = ava::session::now_timestamp(),
                                      .data_json = data.value_or("{}")};
  };
  auto ordinary = [](std::string id) {
    return ava::session::SessionEntry{.id = std::move(id),
                                      .parent_id = "",
                                      .type = ava::session::EntryType::UserMessage,
                                      .timestamp = ava::session::now_timestamp(),
                                      .data_json = "{\"text\":\"ordinary\"}"};
  };

  {
    auto persistent = ava::session::SessionStore::create(workspace, sessions);
    auto persistent_lease = persistent ? ava::session::SessionLease::create_and_acquire(persistent->session_path())
                                       : ava::core::Result<ava::session::SessionLease>(std::unexpected(persistent.error()));
    auto ephemeral = ava::session::SessionStore::create_ephemeral(workspace / "raw-v4");
    auto persistent_raw = persistent && persistent_lease ? persistent->append(*persistent_lease, item("raw-persistent", "raw-turn", 0, "raw-item", 0))
                                                         : ava::core::VoidResult(std::unexpected(persistent.error()));
    auto ephemeral_raw = ephemeral ? ephemeral->append_ephemeral(item("raw-ephemeral", "raw-ephemeral-turn", 0, "raw-ephemeral-item", 0))
                                   : ava::core::VoidResult(std::unexpected(ephemeral.error()));
    auto persistent_entries =
        persistent ? persistent->load(*persistent_lease) : ava::core::Result<std::vector<ava::session::SessionEntry>>(std::unexpected(persistent.error()));
    expect(!persistent_raw && !ephemeral_raw && persistent_entries && persistent_entries->empty(),
           "public raw SessionStore append APIs reject all v4 assistant-output mutations without writing records");
  }

  {
    auto destination = ava::session::SessionStore::create(workspace, sessions);
    auto lease = destination ? ava::session::SessionLease::create_and_acquire(destination->session_path())
                             : ava::core::Result<ava::session::SessionLease>(std::unexpected(destination.error()));
    if (destination && lease)
    {
      std::vector<ava::session::SessionEntry> copied{ordinary("copy-user"), item("copy-output", "copy-turn", 0, "copy-provider-item", 0),
                                                     commit("copy-commit", "copy-turn", 1)};
      auto copied_once = destination->append_validated_copy(*lease, copied);
      auto copied_twice = destination->append_validated_copy(*lease, copied);
      auto entries = destination->load(*lease);
      expect(copied_once && !copied_twice && entries && entries->size() == copied.size() && ava::session::classify_assistant_output(*entries).turns.size() == 1,
             "validated copy preflights a complete v4 history and permits it only once into an empty creating destination");
    }
  }

  {
    auto store = ava::session::SessionStore::create(workspace, sessions);
    auto lease = store ? ava::session::SessionLease::create_and_acquire(store->session_path())
                       : ava::core::Result<ava::session::SessionLease>(std::unexpected(store.error()));
    if (store && lease)
    {
      auto first_target = ava::session::SessionAppendTarget::create_persistent(*store, *lease, ava::session::SessionReadLimits{});
      auto stale_target = ava::session::SessionAppendTarget::create_persistent(*store, *lease, ava::session::SessionReadLimits{});
      if (first_target && stale_target)
      {
        auto first = (*first_target)->append_batch({commit("stale-first", "stale-turn", 0)});
        auto stale = (*stale_target)->append_batch({commit("stale-duplicate", "stale-turn", 0)});
        auto physical_first = first ? (*first_target)
                                          ->append_batch({item("duplicate-physical-output", "physical-turn", 0, "physical-provider-item", 0),
                                                          commit("physical-first-commit", "physical-turn", 1)})
                                    : ava::core::VoidResult(std::unexpected(first.error()));
        auto physical_duplicate = physical_first
                                      ? (*stale_target)
                                            ->append_batch({item("duplicate-physical-output", "physical-turn-two", 0, "physical-provider-item-two", 0),
                                                            commit("physical-duplicate-commit", "physical-turn-two", 1)})
                                      : ava::core::VoidResult(std::unexpected(physical_first.error()));
        auto entries = store->load(*lease);
        expect(first && !stale && physical_first && !physical_duplicate && entries && entries->size() == 3,
               "persistent targets reload v4 state under shared append serialization and reject stale turns or duplicate physical output ids");
      }
    }
  }

  {
    auto store = ava::session::SessionStore::create(workspace, sessions);
    auto lease = store ? ava::session::SessionLease::create_and_acquire(store->session_path())
                       : ava::core::Result<ava::session::SessionLease>(std::unexpected(store.error()));
    if (store && lease)
    {
      bool fail_first_write = true;
      std::size_t writes = 0;
      store->set_append_write_for_test([&fail_first_write, &writes](int fd, std::string_view bytes) -> ssize_t {
        if (fail_first_write && writes++ == 0)
          return ::write(fd, bytes.data(), std::max<std::size_t>(1, bytes.size() / 2));
        if (fail_first_write)
        {
          errno = EIO;
          return -1;
        }
        return ::write(fd, bytes.data(), bytes.size());
      });
      auto first_target = ava::session::SessionAppendTarget::create_persistent(*store, *lease, ava::session::SessionReadLimits{});
      auto second_target = ava::session::SessionAppendTarget::create_persistent(*store, *lease, ava::session::SessionReadLimits{});
      auto partial = first_target ? (*first_target)->append(ordinary("partial-first-target")) : ava::core::VoidResult(std::unexpected(first_target.error()));
      fail_first_write = false;
      auto bypass = second_target ? (*second_target)->append(ordinary("partial-second-target")) : ava::core::VoidResult(std::unexpected(second_target.error()));
      expect(first_target && second_target && !partial && !bypass,
             "a second persistent append target cannot bypass a malformed partial tail before explicit recovery");
    }
  }

  {
    auto store = ava::session::SessionStore::create(workspace, sessions);
    auto lease = store ? ava::session::SessionLease::create_and_acquire(store->session_path())
                       : ava::core::Result<ava::session::SessionLease>(std::unexpected(store.error()));
    if (store && lease)
    {
      auto target = ava::session::SessionAppendTarget::create_persistent(*store, *lease, ava::session::SessionReadLimits{});
      auto staged = item("persistent-staged-0", "persistent-turn", 0, "persistent-item-0", 0);
      auto seeded = target ? (*target)->append(staged) : ava::core::VoidResult(std::unexpected(target.error()));
      if (target && seeded)
      {
        auto unrelated = (*target)->append(ordinary("persistent-unrelated"));
        auto raw_unrelated = store->append(*lease, ordinary("persistent-raw-unrelated"));
        auto zero_while_pending = (*target)->append(commit("persistent-wrong-zero", "persistent-turn", 0));
        auto batch_while_pending = (*target)->append_batch(
            {item("persistent-batch-staged-1", "persistent-turn", 1, "persistent-batch-item-1", 1), commit("persistent-batch-commit", "persistent-turn", 2)});
        auto after_reject = store->load();
        auto continued = (*target)->append(item("persistent-staged-1", "persistent-turn", 1, "persistent-item-1", 1));
        auto committed =
            continued ? (*target)->append(commit("persistent-commit", "persistent-turn", 2)) : ava::core::VoidResult(std::unexpected(continued.error()));
        auto after_commit = committed ? (*target)->append(ordinary("persistent-after-commit")) : ava::core::VoidResult(std::unexpected(committed.error()));
        auto final_entries = store->load();
        expect(!unrelated && !raw_unrelated && !zero_while_pending && !batch_while_pending && after_reject && after_reject->size() == 1 && continued &&
                   committed && after_commit && final_entries && final_entries->size() == 4 && final_entries->back().id == "persistent-after-commit",
               "persistent target and raw ordinary appends preserve a valid staged suffix, require its exact continuation and commit, then reopen ordinary "
               "appends");
      }
    }
  }

  {
    auto store = ava::session::SessionStore::create_ephemeral(workspace / "ephemeral-racing-targets");
    if (store)
    {
      auto first_target = ava::session::SessionAppendTarget::create_ephemeral(*store);
      auto second_target = ava::session::SessionAppendTarget::create_ephemeral(*store);
      if (first_target && second_target)
      {
        std::barrier start(2);
        auto first_append = std::async(std::launch::async, [target = *first_target, &start, &commit] {
          start.arrive_and_wait();
          return target->append_batch({commit("ephemeral-race-first", "ephemeral-race-turn", 0)});
        });
        auto second_append = std::async(std::launch::async, [target = *second_target, &start, &commit] {
          start.arrive_and_wait();
          return target->append_batch({commit("ephemeral-race-second", "ephemeral-race-turn", 0)});
        });
        bool const completed = first_append.wait_for(std::chrono::seconds(2)) == std::future_status::ready &&
                               second_append.wait_for(std::chrono::seconds(2)) == std::future_status::ready;
        bool one_committed = false;
        if (completed)
        {
          auto first = first_append.get();
          auto second = second_append.get();
          one_committed = static_cast<bool>(first) != static_cast<bool>(second);
        }
        auto entries = completed ? store->load()
                                 : ava::core::Result<std::vector<ava::session::SessionEntry>>(
                                       std::unexpected(ava::core::Error(ava::core::ErrorCategory::Unknown, "append targets did not complete")));
        expect(completed && one_committed && entries && entries->size() == 1,
               "independent ephemeral targets serialize stale v4 batches through one shared mutation lock without deadlocking");
      }
    }
  }

  {
    auto store = ava::session::SessionStore::create_ephemeral(workspace / "ephemeral");
    if (store)
    {
      auto target = ava::session::SessionAppendTarget::create_ephemeral(*store);
      auto seeded = target ? (*target)->append(item("ephemeral-staged-0", "ephemeral-turn", 0, "ephemeral-item-0", 0))
                           : ava::core::VoidResult(std::unexpected(target.error()));
      if (target && seeded)
      {
        auto unrelated = (*target)->append(ordinary("ephemeral-unrelated"));
        auto raw_unrelated = store->append_ephemeral(ordinary("ephemeral-raw-unrelated"));
        auto after_reject = store->load();
        auto continued = (*target)->append(item("ephemeral-staged-1", "ephemeral-turn", 1, "ephemeral-item-1", 1));
        auto committed =
            continued ? (*target)->append(commit("ephemeral-commit", "ephemeral-turn", 2)) : ava::core::VoidResult(std::unexpected(continued.error()));
        auto after_commit = committed ? (*target)->append(ordinary("ephemeral-after-commit")) : ava::core::VoidResult(std::unexpected(committed.error()));
        auto zero = after_commit ? (*target)->append(commit("ephemeral-zero", "zero-turn", 0)) : ava::core::VoidResult(std::unexpected(after_commit.error()));
        auto after_zero = zero ? (*target)->append(ordinary("ephemeral-after-zero")) : ava::core::VoidResult(std::unexpected(zero.error()));
        auto final_entries = store->load();
        expect(!unrelated && !raw_unrelated && after_reject && after_reject->size() == 1 && continued && committed && after_commit && zero && after_zero &&
                   final_entries && final_entries->size() == 6 && final_entries->back().id == "ephemeral-after-zero",
               "ephemeral target and raw ordinary appends preserve staged state and accept matching zero-item commits only from the closed state");

        auto before_invalid_batch = final_entries ? final_entries->size() : 0;
        auto invalid_batch = (*target)->append_batch({item("invalid-batch-item", "invalid-batch", 1, "invalid", 0)});
        auto ordinary_batch = (*target)->append_batch({ordinary("ordinary-batch")});
        auto multiple_transaction_batch =
            (*target)->append_batch({commit("first-zero-commit", "first-zero-turn", 0), commit("second-zero-commit", "second-zero-turn", 0)});
        std::vector<ava::session::SessionEntry> over_limit(ava::session::kMaxSessionAppendBatchEntries + 1, ordinary("over-limit"));
        auto over_limit_batch = (*target)->append_batch(std::move(over_limit));
        std::vector<ava::session::SessionEntry> oversize;
        for (std::size_t index = 0; index < 5; ++index)
        {
          auto large = ordinary("oversize-" + std::to_string(index));
          large.data_json = "{\"text\":\"" + std::string(900U * 1024U, 'x') + "\"}";
          oversize.push_back(std::move(large));
        }
        auto oversize_batch = (*target)->append_batch(std::move(oversize));
        auto after_invalid_batches = store->load();
        expect(!invalid_batch && !ordinary_batch && !multiple_transaction_batch && !over_limit_batch && !oversize_batch && after_invalid_batches &&
                   after_invalid_batches->size() == before_invalid_batch,
               "assistant-output batch preflight rejects non-transaction, multiple-transaction, invalid, over-limit, and oversize shapes without writing any "
               "record");
      }
    }
  }

  {
    auto store = ava::session::SessionStore::create_ephemeral(workspace / "max-batch");
    auto target = store ? ava::session::SessionAppendTarget::create_ephemeral(*store)
                        : ava::core::Result<std::shared_ptr<ava::session::SessionAppendTarget>>(std::unexpected(store.error()));
    if (store && target)
    {
      std::vector<ava::session::SessionEntry> maximum_transaction;
      maximum_transaction.reserve(ava::session::kMaxSessionAppendBatchEntries);
      for (std::size_t index = 0; index < ava::session::kMaxAssistantOutputItemsPerTurn; ++index)
      {
        maximum_transaction.push_back(
            item("maximum-item-" + std::to_string(index), "maximum-turn", index, "maximum-provider-item-" + std::to_string(index), index));
      }
      maximum_transaction.push_back(commit("maximum-commit", "maximum-turn", ava::session::kMaxAssistantOutputItemsPerTurn));
      auto appended = (*target)->append_batch(std::move(maximum_transaction));
      auto after_batch = store->load();
      auto ordinary_after_batch = appended ? (*target)->append(ordinary("maximum-after-commit")) : ava::core::VoidResult(std::unexpected(appended.error()));
      auto final_entries = store->load();
      expect(appended && after_batch && after_batch->size() == ava::session::kMaxSessionAppendBatchEntries && ordinary_after_batch && final_entries &&
                 final_entries->size() == ava::session::kMaxSessionAppendBatchEntries + 1,
             "one maximum-size v4 assistant transaction preflights and commits with linear state growth before reopening ordinary appends");
    }
  }

  {
    auto store = ava::session::SessionStore::create(workspace, sessions);
    auto lease = store ? ava::session::SessionLease::create_and_acquire(store->session_path())
                       : ava::core::Result<ava::session::SessionLease>(std::unexpected(store.error()));
    if (store && lease)
    {
      bool allow_writes = false;
      store->set_append_write_for_test([&allow_writes](int fd, std::string_view bytes) -> ssize_t {
        if (allow_writes)
          return ::write(fd, bytes.data(), bytes.size());
        errno = EIO;
        return -1;
      });
      auto target = ava::session::SessionAppendTarget::create_persistent(*store, *lease, ava::session::SessionReadLimits{});
      if (target)
      {
        auto first_failure = (*target)->append_batch({commit("no-durable-commit", "no-durable-turn", 0)});
        auto after_failure = store->load();
        allow_writes = true;
        auto ordinary_after_failure = (*target)->append(ordinary("after-no-durable-failure"));
        auto final_entries = store->load();
        expect(!first_failure && after_failure && after_failure->empty() && ordinary_after_failure && final_entries && final_entries->size() == 1 &&
                   final_entries->front().id == "after-no-durable-failure",
               "a batch write failure before its first durable record leaves the ready append target usable");
      }
    }
  }

  {
    auto store = ava::session::SessionStore::create(workspace, sessions);
    auto lease = store ? ava::session::SessionLease::create_and_acquire(store->session_path())
                       : ava::core::Result<ava::session::SessionLease>(std::unexpected(store.error()));
    if (store && lease)
    {
      std::size_t writes = 0;
      bool allow_later_writes = false;
      store->set_append_write_for_test([&writes, &allow_later_writes](int fd, std::string_view bytes) -> ssize_t {
        if (writes++ == 0)
        {
          auto const short_count = std::max<std::size_t>(1, bytes.size() / 2);
          return ::write(fd, bytes.data(), short_count);
        }
        if (allow_later_writes)
          return ::write(fd, bytes.data(), bytes.size());
        errno = EIO;
        return -1;
      });
      auto target = ava::session::SessionAppendTarget::create_persistent(*store, *lease, ava::session::SessionReadLimits{});
      if (target)
      {
        auto partial = (*target)->append(ordinary("single-short-write"));
        auto blocked = (*target)->append(ordinary("single-short-write-blocked"));
        allow_later_writes = true;
        auto recovered = (*target)->recover();
        auto reopened = recovered ? (*target)->append(ordinary("single-short-write-reopened")) : ava::core::VoidResult(std::unexpected(recovered.error()));
        auto final_entries = store->load();
        expect(!partial && error_context_value(partial.error(), "append_commit_state") == "partial_or_unknown",
               "a first-record single append short write reports partial_or_unknown");
        expect(!blocked, "a first-record single append short write blocks later mutation before recovery");
        expect(static_cast<bool>(recovered), "a first-record single append short write recovers its torn tail");
        expect(static_cast<bool>(reopened), reopened ? "a recovered first-record single append short write accepts a new mutation" : reopened.error().format());
        expect(final_entries && final_entries->size() == 1 && final_entries->front().id == "single-short-write-reopened",
               "a recovered first-record single append short write retains only the new mutation");
      }
    }
  }

  {
    auto store = ava::session::SessionStore::create(workspace, sessions);
    auto lease = store ? ava::session::SessionLease::create_and_acquire(store->session_path())
                       : ava::core::Result<ava::session::SessionLease>(std::unexpected(store.error()));
    if (store && lease)
    {
      std::size_t writes = 0;
      bool allow_later_writes = false;
      store->set_append_write_for_test([&writes, &allow_later_writes](int fd, std::string_view bytes) -> ssize_t {
        if (writes++ == 0)
        {
          auto const short_count = std::max<std::size_t>(1, bytes.size() / 2);
          return ::write(fd, bytes.data(), short_count);
        }
        if (allow_later_writes)
          return ::write(fd, bytes.data(), bytes.size());
        errno = EIO;
        return -1;
      });
      auto target = ava::session::SessionAppendTarget::create_persistent(*store, *lease, ava::session::SessionReadLimits{});
      if (target)
      {
        auto partial = (*target)->append_batch({commit("batch-short-write", "batch-short-turn", 0)});
        auto blocked = (*target)->append(ordinary("batch-short-write-blocked"));
        allow_later_writes = true;
        auto recovered = (*target)->recover();
        auto reopened = recovered ? (*target)->append(ordinary("batch-short-write-reopened")) : ava::core::VoidResult(std::unexpected(recovered.error()));
        auto final_entries = store->load();
        expect(!partial && error_context_value(partial.error(), "append_commit_state") == "partial_or_unknown" &&
                   error_context_value(partial.error(), "batch_persisted_entries") == "0",
               "a first-record batch short write reports partial_or_unknown at zero completed records");
        expect(!blocked, "a first-record batch short write blocks later mutation before recovery");
        expect(static_cast<bool>(recovered), "a first-record batch short write recovers its torn tail");
        expect(static_cast<bool>(reopened), reopened ? "a recovered first-record batch short write accepts a new mutation" : reopened.error().format());
        expect(final_entries && final_entries->size() == 1 && final_entries->front().id == "batch-short-write-reopened",
               "a recovered first-record batch short write retains only the new mutation");
      }
    }
  }

  {
    auto store = ava::session::SessionStore::create(workspace, sessions);
    auto lease = store ? ava::session::SessionLease::create_and_acquire(store->session_path())
                       : ava::core::Result<ava::session::SessionLease>(std::unexpected(store.error()));
    if (store && lease)
    {
      int writes = 0;
      bool allow_later_writes = false;
      store->set_append_write_for_test([&writes, &allow_later_writes](int fd, std::string_view bytes) -> ssize_t {
        if (writes++ == 0 || allow_later_writes)
          return ::write(fd, bytes.data(), bytes.size());
        errno = EIO;
        return -1;
      });
      auto target = ava::session::SessionAppendTarget::create_persistent(*store, *lease, ava::session::SessionReadLimits{});
      if (target)
      {
        auto partial = (*target)->append_batch({item("batch-staged-0", "batch-turn", 0, "batch-item-0", 0),
                                                item("batch-staged-1", "batch-turn", 1, "batch-item-1", 1), commit("batch-commit", "batch-turn", 2)});
        auto persisted = store->load();
        auto blocked_append = (*target)->append(ordinary("blocked-after-partial"));
        auto blocked_batch = (*target)->append_batch({commit("blocked-zero-commit", "blocked-zero-turn", 0)});
        allow_later_writes = true;
        auto recovered = (*target)->recover();
        auto recovered_entries = store->load();
        auto reopened = recovered ? (*target)->append(ordinary("after-partial-recovery")) : ava::core::VoidResult(std::unexpected(recovered.error()));
        auto final_entries = store->load();
        expect(!partial && error_context_value(partial.error(), "append_commit_state") == "partial_or_unknown" &&
                   error_context_value(partial.error(), "batch_persisted_entries") == "1" &&
                   error_context_value(partial.error(), "staged_prefix_recovery").has_value() && persisted && persisted->size() == 1 &&
                   persisted->front().id == "batch-staged-0" && !blocked_append && !blocked_batch && recovered && recovered_entries &&
                   recovered_entries->empty() && reopened && final_entries && final_entries->size() == 1 &&
                   final_entries->front().id == "after-partial-recovery",
               "a partial v4 batch latches mutation until explicit recovery truncates its staged prefix and reopens ordinary appends");
      }
    }
  }

  {
    auto store = ava::session::SessionStore::create(workspace, sessions);
    auto lease = store ? ava::session::SessionLease::create_and_acquire(store->session_path())
                       : ava::core::Result<ava::session::SessionLease>(std::unexpected(store.error()));
    if (store && lease)
    {
      store->set_after_append_write_for_test([] { throw 1; });
      auto target = ava::session::SessionAppendTarget::create_persistent(*store, *lease, ava::session::SessionReadLimits{});
      if (target)
      {
        auto staged = (*target)->append(item("post-write-staged", "post-write-turn", 0, "post-write-item", 0));
        store->set_after_append_write_for_test({});
        auto ordinary_after_staged = (*target)->append(ordinary("post-write-ordinary"));
        auto persisted = store->load();
        expect(!staged && error_context_value(staged.error(), "append_commit_state") == "committed_to_leased_inode" && !ordinary_after_staged && persisted &&
                   persisted->size() == 1 && persisted->front().id == "post-write-staged",
               "a post-write failure publishes its durable staged state so ordinary appends remain rejected");
      }
    }
  }

  {
    auto store = ava::session::SessionStore::create(workspace, sessions);
    auto lease = store ? ava::session::SessionLease::create_and_acquire(store->session_path())
                       : ava::core::Result<ava::session::SessionLease>(std::unexpected(store.error()));
    if (store && lease)
    {
      std::size_t writes = 0;
      store->set_after_append_write_for_test([&writes] {
        if (++writes == 2)
          throw 1;
      });
      auto target = ava::session::SessionAppendTarget::create_persistent(*store, *lease, ava::session::SessionReadLimits{});
      if (target)
      {
        auto partial = (*target)->append_batch(
            {item("post-write-batch-0", "post-write-batch", 0, "post-write-batch-item-0", 0), commit("post-write-batch-commit", "post-write-batch", 1)});
        store->set_after_append_write_for_test({});
        auto persisted = store->load();
        auto blocked = (*target)->append(ordinary("blocked-after-known-commit"));
        auto recovered = (*target)->recover();
        auto recovered_entries = store->load();
        auto reopened = recovered ? (*target)->append(ordinary("after-known-commit-recovery")) : ava::core::VoidResult(std::unexpected(recovered.error()));
        auto final_entries = store->load();
        auto projection = recovered_entries ? ava::session::classify_assistant_output(*recovered_entries) : ava::session::AssistantOutputProjection{};
        expect(!partial && error_context_value(partial.error(), "append_commit_state") == "partial_or_unknown" &&
                   error_context_value(partial.error(), "batch_persisted_entries") == "2" && error_context_value(partial.error(), "recovery").has_value() &&
                   persisted && persisted->size() == 2 && persisted->back().id == "post-write-batch-commit" && !blocked && recovered && recovered_entries &&
                   recovered_entries->size() == 2 && projection.turns.size() == 1 && reopened && final_entries && final_entries->size() == 3 &&
                   final_entries->back().id == "after-known-commit-recovery",
               "a final known-committed batch commit still requires explicit recovery but never truncates its completed turn");
      }
    }
  }

  {
    auto malformed = ava::session::SessionStore::create_ephemeral(workspace / "malformed");
    if (malformed)
    {
      auto sparse = item("malformed-sparse", "malformed-turn", 1, "malformed-item", 0);
      auto seeded = malformed->append_ephemeral(std::move(sparse));
      auto target = seeded ? ava::session::SessionAppendTarget::create_ephemeral(*malformed)
                           : ava::core::Result<std::shared_ptr<ava::session::SessionAppendTarget>>(std::unexpected(seeded.error()));
      expect(!target, "structurally malformed v4 history cannot create an append target");
    }
  }
}

void test_incomplete_assistant_output_suffix_recovery()
{
  auto const root = create_empty_root("incomplete-assistant-output-suffix-recovery");
  auto const workspace = root / "workspace";
  auto const sessions = root / "sessions";
  std::filesystem::create_directories(workspace);

  auto ordinary = [](std::string id, std::string text = "ordinary") {
    return ava::session::SessionEntry{.id = std::move(id),
                                      .parent_id = "",
                                      .type = ava::session::EntryType::UserMessage,
                                      .timestamp = ava::session::now_timestamp(),
                                      .data_json = "{\"text\":\"" + text + "\"}"};
  };
  auto staged = [](std::string id, std::string turn_id, std::size_t sequence) {
    auto data = ava::session::serialize_assistant_output_item_data_json(ava::session::AssistantOutputItem{
        .assistant_turn_id = std::move(turn_id),
        .sequence = sequence,
        .kind = ava::session::AssistantOutputItemKind::Text,
        .provider_item_id = "provider-item-" + std::to_string(sequence),
        .provider_output_index = sequence,
        .payload = ava::session::AssistantOutputText{.text = "staged", .assistant_phase = ava::session::AssistantOutputTextPhase::Commentary}});
    return ava::session::SessionEntry{.id = std::move(id),
                                      .parent_id = "",
                                      .type = ava::session::EntryType::AssistantOutputItem,
                                      .timestamp = ava::session::now_timestamp(),
                                      .data_json = data.value_or("{}")};
  };
  auto commit = [](std::string id, std::string turn_id, std::size_t item_count) {
    auto data = ava::session::serialize_assistant_turn_commit_data_json(ava::session::AssistantTurnCommit{.assistant_turn_id = std::move(turn_id),
                                                                                                          .item_count = item_count,
                                                                                                          .provider = "openai",
                                                                                                          .model = "gpt-5.5",
                                                                                                          .finish_reason = "tool_calls",
                                                                                                          .usage_json = std::nullopt});
    return ava::session::SessionEntry{.id = std::move(id),
                                      .parent_id = "",
                                      .type = ava::session::EntryType::AssistantTurnCommit,
                                      .timestamp = ava::session::now_timestamp(),
                                      .data_json = data.value_or("{}")};
  };
  auto exact_line = [](ava::session::SessionEntry const& entry) {
    auto line = ava::session::serialize_session_entry_line(entry);
    return line ? *line + "\n" : std::string{};
  };
  // Crash-recovery fixtures intentionally model physical v4 bytes that exist
  // before an authority can finish the transaction. Do not route these through
  // the public raw append API, which correctly rejects v4 mutations.
  auto append_physical_v4_fixture = [&](ava::session::SessionStore const& store, ava::session::SessionEntry const& entry) {
    auto const line = exact_line(entry);
    if (line.empty())
      return ava::core::VoidResult(std::unexpected(ava::core::Error(ava::core::ErrorCategory::Session, "fixture entry did not serialize")));
    std::ofstream file(store.session_path(), std::ios::binary | std::ios::app);
    if (!file)
      return ava::core::VoidResult(std::unexpected(ava::core::Error(ava::core::ErrorCategory::Io, "failed to append physical v4 fixture")));
    file << line;
    file.flush();
    if (!file)
      return ava::core::VoidResult(std::unexpected(ava::core::Error(ava::core::ErrorCategory::Io, "failed to flush physical v4 fixture")));
    return ava::core::VoidResult{};
  };
  auto recovery_artifact_count = [](std::filesystem::path const& path) {
    std::size_t count = 0;
    std::error_code iter_error;
    auto const prefix = path.filename().string() + ".incomplete-assistant-output.";
    for (std::filesystem::directory_iterator it(path.parent_path(), iter_error), end; !iter_error && it != end; it.increment(iter_error))
      if (it->path().filename().string().starts_with(prefix))
        ++count;
    return count;
  };

  for (std::size_t count = 1; count <= 3; ++count)
  {
    auto store = ava::session::SessionStore::create(workspace, sessions);
    auto lease = store ? ava::session::SessionLease::create_and_acquire(store->session_path())
                       : ava::core::Result<ava::session::SessionLease>(std::unexpected(store.error()));
    if (!store || !lease)
      continue;
    auto const session_id = store->options_r()->session_id;
    auto const path = store->session_path();
    auto seeded = store->append(*lease, ordinary("seed-" + std::to_string(count)));
    std::string expected_suffix;
    for (std::size_t index = 0; seeded && index < count; ++index)
    {
      auto entry = staged("stage-" + std::to_string(count) + "-" + std::to_string(index), "turn-" + std::to_string(count), index);
      expected_suffix += exact_line(entry);
      seeded = append_physical_v4_fixture(*store, entry);
    }
    lease = ava::session::SessionLease{};
    auto reopened = seeded ? ava::session::SessionStore::open(workspace, session_id, sessions)
                           : ava::core::Result<ava::session::SessionStore>(std::unexpected(seeded.error()));
    auto recovery_lease = reopened ? ava::session::SessionLease::acquire(reopened->session_path())
                                   : ava::core::Result<ava::session::SessionLease>(std::unexpected(reopened.error()));
    auto recovered = recovery_lease ? reopened->recover_incomplete_assistant_output_suffix(*recovery_lease, ava::session::SessionReadLimits{})
                                    : ava::core::Result<std::optional<ava::session::AssistantOutputSuffixRecovery>>(std::unexpected(recovery_lease.error()));
    auto entries = recovered && *recovered ? reopened->load(*recovery_lease)
                                           : ava::core::Result<std::vector<ava::session::SessionEntry>>(std::unexpected(
                                                 ava::core::Error(ava::core::ErrorCategory::Unknown, "staging recovery did not return metadata")));
    auto target = entries ? ava::session::SessionAppendTarget::create_persistent(*reopened, *recovery_lease, ava::session::SessionReadLimits{})
                          : ava::core::Result<std::shared_ptr<ava::session::SessionAppendTarget>>(std::unexpected(entries.error()));
    auto next = target ? (*target)->append(ordinary("ordinary-after-" + std::to_string(count))) : ava::core::VoidResult(std::unexpected(target.error()));
    struct stat quarantine_status{};
    bool const quarantine_mode = recovered && *recovered && (*recovered)->quarantine_path &&
                                 ::stat((*recovered)->quarantine_path->c_str(), &quarantine_status) == 0 && (quarantine_status.st_mode & 0777) == 0600;
    expect(recovered && *recovered && (*recovered)->removed_entry_count == count && (*recovered)->removed_byte_count == expected_suffix.size() &&
               (*recovered)->quarantine_path && ava::tests::read_session_test_binary_file(*(*recovered)->quarantine_path) == expected_suffix &&
               quarantine_mode && entries && entries->size() == 1 && next && ava::tests::read_session_test_binary_file(path).ends_with("\n") &&
               recovery_artifact_count(path) == 1,
           "restart recovery quarantines and removes exactly each complete uncommitted assistant-output crash prefix before a normal append");
  }

  auto make_persistent_fixture = [&](std::string id) {
    auto store = ava::session::SessionStore::create(workspace, sessions);
    auto lease = store ? ava::session::SessionLease::create_and_acquire(store->session_path())
                       : ava::core::Result<ava::session::SessionLease>(std::unexpected(store.error()));
    if (store && lease)
      static_cast<void>(store->append(*lease, ordinary("seed-" + id)));
    return std::pair(std::move(store), std::move(lease));
  };

  {
    auto [store, lease] = make_persistent_fixture("malformed");
    if (store && lease)
    {
      auto malformed = staged("malformed-stage", "malformed-turn", 1);
      static_cast<void>(append_physical_v4_fixture(*store, malformed));
      auto const before = ava::tests::read_session_test_binary_file(store->session_path());
      auto recovered = store->recover_incomplete_assistant_output_suffix(*lease, ava::session::SessionReadLimits{});
      expect(!recovered && ava::tests::read_session_test_binary_file(store->session_path()) == before && recovery_artifact_count(store->session_path()) == 0,
             "malformed complete staged suffix fails closed without a truncate or quarantine");
    }
  }
  {
    auto [store, lease] = make_persistent_fixture("interior");
    if (store && lease)
    {
      static_cast<void>(append_physical_v4_fixture(*store, staged("interior-stage", "interior-turn", 0)));
      static_cast<void>(append_physical_v4_fixture(*store, ordinary("interior-unrelated")));
      auto const before = ava::tests::read_session_test_binary_file(store->session_path());
      auto recovered = store->recover_incomplete_assistant_output_suffix(*lease, ava::session::SessionReadLimits{});
      expect(!recovered && ava::tests::read_session_test_binary_file(store->session_path()) == before && recovery_artifact_count(store->session_path()) == 0,
             "an interior staged group followed by an unrelated entry fails closed unchanged");
    }
  }
  {
    auto [store, lease] = make_persistent_fixture("committed");
    if (store && lease)
    {
      static_cast<void>(append_physical_v4_fixture(*store, staged("committed-stage", "committed-turn", 0)));
      static_cast<void>(append_physical_v4_fixture(*store, commit("committed-commit", "committed-turn", 1)));
      auto const before = ava::tests::read_session_test_binary_file(store->session_path());
      auto recovered = store->recover_incomplete_assistant_output_suffix(*lease, ava::session::SessionReadLimits{});
      expect(recovered && !*recovered && ava::tests::read_session_test_binary_file(store->session_path()) == before,
             "a committed assistant turn is never recovered or removed");
    }
  }
  {
    auto [store, lease] = make_persistent_fixture("limits-and-cancel");
    if (store && lease)
    {
      static_cast<void>(append_physical_v4_fixture(*store, staged("limited-stage", "limited-turn", 0)));
      auto const before = ava::tests::read_session_test_binary_file(store->session_path());
      auto canceled = store->recover_incomplete_assistant_output_suffix(*lease, ava::session::SessionReadLimits{}, [] { return true; });
      auto limited = store->recover_incomplete_assistant_output_suffix(
          *lease, ava::session::SessionReadLimits{.max_file_bytes = before.size(), .max_line_bytes = before.size(), .max_entries = 1});
      expect(!canceled && !limited && ava::tests::read_session_test_binary_file(store->session_path()) == before &&
                 recovery_artifact_count(store->session_path()) == 0,
             "assistant-output suffix recovery honors cancellation and entry limits before mutation");
    }
  }
  {
    auto [left_store, left_lease] = make_persistent_fixture("lease-mismatch-left");
    auto [right_store, right_lease] = make_persistent_fixture("lease-mismatch-right");
    if (left_store && left_lease && right_store && right_lease)
    {
      static_cast<void>(append_physical_v4_fixture(*left_store, staged("mismatch-stage", "mismatch-turn", 0)));
      auto const before = ava::tests::read_session_test_binary_file(left_store->session_path());
      auto mismatch = left_store->recover_incomplete_assistant_output_suffix(*right_lease, ava::session::SessionReadLimits{});
      expect(!mismatch && ava::tests::read_session_test_binary_file(left_store->session_path()) == before,
             "assistant-output suffix recovery rejects a lease for another session without touching the target");
    }
  }
  std::error_code remove_error;
  for (std::string const replacement_kind : {"replacement", "symlink", "fifo"})
  {
    auto [store, lease] = make_persistent_fixture("unsafe-" + replacement_kind);
    if (!store || !lease)
      continue;
    static_cast<void>(append_physical_v4_fixture(*store, staged("unsafe-stage-" + replacement_kind, "unsafe-turn-" + replacement_kind, 0)));
    auto const path = store->session_path();
    auto const parked = path.string() + ".parked";
    std::filesystem::rename(path, parked, remove_error);
    if (replacement_kind == "replacement")
      ava::tests::write_session_test_binary_file(path, "replacement\\n");
    else if (replacement_kind == "symlink")
      std::filesystem::create_symlink(parked, path, remove_error);
    else
      ::mkfifo(path.c_str(), 0600);
    auto recovered = store->recover_incomplete_assistant_output_suffix(*lease, ava::session::SessionReadLimits{});
    expect(!recovered && ava::tests::read_session_test_binary_file(parked).find("unsafe-stage-") != std::string::npos,
           "assistant-output suffix recovery rejects " + replacement_kind + " replacement without mutating the leased inode");
  }
  {
    auto ephemeral = ava::session::SessionStore::create_ephemeral(workspace / "ephemeral");
    if (ephemeral)
    {
      auto target = ava::session::SessionAppendTarget::create_ephemeral(*ephemeral);
      auto staged_append = target ? (*target)->append(staged("ephemeral-stage", "ephemeral-turn", 0)) : ava::core::VoidResult(std::unexpected(target.error()));
      auto recovered = staged_append && target ? (*target)->recover() : ava::core::VoidResult(std::unexpected(staged_append.error()));
      auto next = recovered && target ? (*target)->append(ordinary("ephemeral-ordinary")) : ava::core::VoidResult(std::unexpected(recovered.error()));
      auto entries = ephemeral->load();
      expect(staged_append && recovered && next && entries && entries->size() == 1 && entries->front().id == "ephemeral-ordinary",
             "ephemeral recovery erases only the proven trailing staging entries and rebuilds append state");
    }
  }
}

void test_lease_bound_session_reads_hold_exact_authority()
{
  auto const root = create_empty_root("lease-bound-session-reads");

  auto const workspace = root / "workspace";
  std::filesystem::create_directories(workspace);

  auto entry = [](std::string id, std::string text = "snapshot") {
    return ava::session::SessionEntry{.id = std::move(id),
                                      .parent_id = "",
                                      .type = ava::session::EntryType::UserMessage,
                                      .timestamp = ava::session::now_timestamp(),
                                      .data_json = "{\"text\":\"" + text + "\"}"};
  };
  auto make_owned = [&](std::string id) -> std::pair<ava::session::SessionStore, ava::session::SessionLease> {
    ava::session::SessionStore store(ava::session::SessionStoreOptions{.root_dir = root / id, .workspace_dir = workspace, .session_id = std::move(id)});
    auto lease = ava::session::SessionLease::create_and_acquire(store.session_path());
    if (!lease)
      return {std::move(store), ava::session::SessionLease{}};
    auto seeded = store.append(*lease, entry("seed"));
    expect(seeded.has_value(), "lease-bound read fixture seeds a framed session record");
    return {std::move(store), std::move(*lease)};
  };

  {
    auto [store, lease] = make_owned("static-replacement");
    if (lease.active())
    {
      auto const parked = store.session_path().string() + ".parked";
      std::filesystem::rename(store.session_path(), parked);
      ava::tests::write_session_test_binary_file(store.session_path(), "replacement\n");
      auto loaded = store.load(lease);
      expect(!loaded, "lease-bound load rejects a basename replaced before snapshot validation");
    }
  }

  {
    auto [store, lease] = make_owned("read-swap");
    if (lease.active())
    {
      auto const parked = store.session_path().string() + ".parked";
      store.set_after_lease_bound_read_for_test([&] {
        std::filesystem::rename(store.session_path(), parked);
        ava::tests::write_session_test_binary_file(store.session_path(), "replacement\n");
      });
      auto loaded = store.load_bounded(lease, ava::session::legacy_unbounded_session_read_limits());
      store.set_after_lease_bound_read_for_test({});
      expect(!loaded, "lease-bound load rejects a basename swap after its exact-offset read begins");
    }
  }

  {
    auto [store, lease] = make_owned("parent-swap");
    if (lease.active())
    {
      auto const parent = store.session_path().parent_path();
      auto const moved_parent = parent.string() + ".moved";
      store.set_after_lease_bound_read_for_test([&] {
        std::filesystem::rename(parent, moved_parent);
        std::filesystem::create_directories(parent);
        ava::tests::write_session_test_binary_file(store.session_path(), "replacement\n");
      });
      auto inspected = store.inspect_bounded(lease, ava::session::legacy_unbounded_session_read_limits());
      store.set_after_lease_bound_read_for_test({});
      expect(!inspected, "lease-bound inspection rejects replacement of its canonical parent publication during the read");
    }
  }

  {
    auto [store, lease] = make_owned("shrink");
    if (lease.active())
    {
      store.set_after_lease_bound_read_for_test([&] { std::filesystem::resize_file(store.session_path(), 1); });
      auto loaded = store.load(lease);
      store.set_after_lease_bound_read_for_test({});
      expect(!loaded, "lease-bound load rejects shrink of the leased inode during its initial-size snapshot");
    }
  }

  {
    auto [store, lease] = make_owned("growth");
    if (lease.active())
    {
      bool growth_appended = false;
      auto const initial_offset = lease.offset_for_test();
      store.set_after_lease_bound_read_for_test([&] { growth_appended = store.append(lease, entry("growth")).has_value(); });
      auto snapshot = store.load(lease);
      store.set_after_lease_bound_read_for_test({});
      auto complete = store.load(lease);
      expect(growth_appended && snapshot && snapshot->size() == 1 && snapshot->front().id == "seed" && complete && complete->size() == 2,
             "lease-bound load permits concurrent valid append growth while returning only its captured initial-size snapshot");
      expect(initial_offset >= 0 && lease.offset_for_test() == initial_offset,
             "lease-bound exact-offset pread leaves the shared lease open-file-description offset unchanged");
    }
  }
}

void test_session_read_authority_binding_and_descriptor_lifetime()
{
  auto const root = create_empty_root("session-read-authority");

  auto const workspace = root / "workspace";
  auto const sessions = root / "sessions";
  std::filesystem::create_directories(workspace);

  auto entry = [](std::string id, std::string text) {
    return ava::session::SessionEntry{.id = std::move(id),
                                      .parent_id = "",
                                      .type = ava::session::EntryType::UserMessage,
                                      .timestamp = ava::session::now_timestamp(),
                                      .data_json = "{\"text\":\"" + text + "\"}"};
  };

  ava::session::SessionStore store(ava::session::SessionStoreOptions{.root_dir = sessions, .workspace_dir = workspace, .session_id = "persistent-authority"});
  auto lease = ava::session::SessionLease::create_and_acquire(store.session_path());
  expect(lease.has_value(), "session read authority fixture acquires its persistent lease");
  if (!lease)
    return;
  expect(store.append(*lease, entry("original", "ORIGINAL_HISTORY")).has_value(), "session read authority fixture seeds original history");

  auto bound = ava::session::SessionReadAuthority::create_persistent(store, *lease);
  expect(bound.has_value(), "persistent read authority binds the exact store and lease identity");
  if (!bound)
    return;
  std::optional<ava::session::SessionReadAuthority> authority(std::move(*bound));
  std::optional<ava::session::SessionReadAuthority> retained_copy(*authority);
  *lease = ava::session::SessionLease{};
  auto blocked_while_copied = ava::session::SessionLease::acquire(store.session_path());
  expect(!blocked_while_copied, "a copied read authority retains the duplicated lease after caller lease release");

  auto loaded = authority->load();
  expect(loaded && loaded->size() == 1 && loaded->front().id == "original", "bound read authority loads the leased original history");

  auto const parked = store.session_path().string() + ".parked";
  std::filesystem::rename(store.session_path(), parked);
  auto replacement_line = ava::session::serialize_session_entry_line(entry("replacement", "REPLACEMENT_CANARY"));
  expect(replacement_line.has_value(), "replacement read-authority fixture serializes a valid session record");
  if (!replacement_line)
    return;
  ava::tests::write_session_test_binary_file(store.session_path(), *replacement_line + "\n");
  auto rejected = authority->load();
  auto pathname_loaded = store.load();
  expect(!rejected && pathname_loaded && pathname_loaded->size() == 1 && pathname_loaded->front().id == "replacement",
         "bound read authority rejects a replaced live pathname while observational pathname loading sees only the replacement");

  authority.reset();
  auto still_blocked = ava::session::SessionLease::acquire(parked);
  expect(!still_blocked, "one retained authority copy continues holding the original inode lease");
  retained_copy.reset();
  auto released = ava::session::SessionLease::acquire(parked);
  expect(released.has_value(), "destroying the last read authority copy releases its duplicated lease descriptor");

  ava::session::SessionStore wrong_store(ava::session::SessionStoreOptions{.root_dir = sessions, .workspace_dir = workspace, .session_id = "wrong-authority"});
  auto wrong_lease = ava::session::SessionLease::create_and_acquire(wrong_store.session_path());
  if (wrong_lease)
  {
    auto wrong_binding = ava::session::SessionReadAuthority::create_persistent(store, *wrong_lease);
    expect(!wrong_binding, "persistent read authority rejects a lease for a different exact store path");
  }

  ava::session::SessionStore allocation_store(
      ava::session::SessionStoreOptions{.root_dir = sessions, .workspace_dir = workspace, .session_id = "read-authority-allocation"});
  auto allocation_lease = ava::session::SessionLease::create_and_acquire(allocation_store.session_path());
  if (allocation_lease)
  {
    allocation_store.fail_persistent_read_authority_allocation_for_test();
    auto failed = ava::session::SessionReadAuthority::create_persistent(allocation_store, *allocation_lease);
    allocation_store.fail_persistent_read_authority_allocation_for_test(false);
    *allocation_lease = ava::session::SessionLease{};
    auto reacquired = ava::session::SessionLease::acquire(allocation_store.session_path());
    expect(!failed && reacquired,
           "read authority allocation failure closes its immediately adopted duplicate descriptor and releases flock with the caller lease");
  }

  auto ephemeral = ava::session::SessionStore::create_ephemeral(workspace);
  if (ephemeral)
  {
    auto ephemeral_authority = ava::session::SessionReadAuthority::create_ephemeral(*ephemeral);
    expect(ephemeral_authority.has_value(), "ephemeral read authority binds copied shared in-memory state");
    auto appended = ephemeral->append_ephemeral(entry("ephemeral", "shared"));
    auto ephemeral_entries = ephemeral_authority ? ephemeral_authority->load() : ava::core::Result<std::vector<ava::session::SessionEntry>>{};
    expect(appended && ephemeral_entries && ephemeral_entries->size() == 1 && ephemeral_entries->front().id == "ephemeral",
           "ephemeral read authority observes later appends through shared in-memory state");
  }

#if defined(__linux__)
  ava::session::SessionStore throwing_store(ava::session::SessionStoreOptions{.root_dir = sessions, .workspace_dir = workspace, .session_id = "throwing-read"});
  auto throwing_lease = ava::session::SessionLease::create_and_acquire(throwing_store.session_path());
  if (throwing_lease && throwing_store.append(*throwing_lease, entry("throwing", "visitor")).has_value())
  {
    auto count_fds = [] {
      return static_cast<std::size_t>(std::distance(std::filesystem::directory_iterator("/proc/self/fd"), std::filesystem::directory_iterator{}));
    };
    throwing_store.set_after_lease_bound_read_for_test([] { throw std::bad_alloc(); });
    auto const before_visitor = count_fds();
    bool visitor_threw = false;
    try
    {
      auto loaded = throwing_store.load_bounded(*throwing_lease, ava::session::legacy_unbounded_session_read_limits());
      visitor_threw = !loaded;
    }
    catch (std::bad_alloc const&)
    {
      visitor_threw = true;
    }
    auto const after_visitor = count_fds();
    throwing_store.set_after_lease_bound_read_for_test({});

    auto const before_cancel = count_fds();
    bool cancel_threw = false;
    try
    {
      static_cast<void>(throwing_store.load_bounded(*throwing_lease, ava::session::legacy_unbounded_session_read_limits(),
                                                    []() -> bool { throw std::runtime_error("cancel callback failure"); }));
    }
    catch (std::runtime_error const&)
    {
      cancel_threw = true;
    }
    auto const after_cancel = count_fds();
    expect(visitor_threw, "lease-bound read test hook reports allocation exceptions");
    expect(before_visitor == after_visitor, "lease-bound read RAII closes owned descriptors after an allocation exception");
    expect(cancel_threw, "lease-bound read propagates cancellation callback exceptions");
    expect(before_cancel == after_cancel, "lease-bound read RAII closes owned descriptors after a cancellation callback exception");
  }
#endif
}

void test_session_read_authority_retains_runtime_policy()
{
  auto const root = create_empty_root("session-read-authority-policy");
  auto const workspace = root / "workspace";
  auto const sessions = root / "sessions";
  std::filesystem::create_directories(workspace);

  auto entry = [](std::string id) {
    return ava::session::SessionEntry{.id = std::move(id),
                                      .parent_id = "",
                                      .type = ava::session::EntryType::UserMessage,
                                      .timestamp = ava::session::now_timestamp(),
                                      .data_json = "{\"text\":\"policy\"}"};
  };
  auto const limits = ava::session::SessionReadLimits{.max_file_bytes = 4096, .max_line_bytes = 2048, .max_entries = 1};

  auto store = ava::session::SessionStore::create(workspace, sessions);
  auto lease = store ? ava::session::SessionLease::create_and_acquire(store->session_path())
                     : ava::core::Result<ava::session::SessionLease>(std::unexpected(store.error()));
  if (!store || !lease)
    return;

  auto policy_authority = ava::session::SessionReadAuthority::create_persistent(*store, *lease, limits);
  auto target = ava::session::SessionAppendTarget::create_persistent(*store, *lease, limits);
  auto first = store->append(*lease, entry("first"));
  auto first_load =
      policy_authority ? policy_authority->load() : ava::core::Result<std::vector<ava::session::SessionEntry>>(std::unexpected(policy_authority.error()));
  auto second = first ? store->append(*lease, entry("second")) : ava::core::VoidResult(std::unexpected(first.error()));
  auto provider_iteration_load =
      second && policy_authority ? policy_authority->load() : ava::core::Result<std::vector<ava::session::SessionEntry>>(std::unexpected(second.error()));
  auto legacy_authority = ava::session::SessionReadAuthority::create_persistent(*store, *lease);
  auto legacy_load =
      legacy_authority ? legacy_authority->load() : ava::core::Result<std::vector<ava::session::SessionEntry>>(std::unexpected(legacy_authority.error()));
  auto target_authority = target ? (*target)->read_authority() : ava::core::Result<ava::session::SessionReadAuthority>(std::unexpected(target.error()));
  auto target_load =
      target_authority ? target_authority->load() : ava::core::Result<std::vector<ava::session::SessionEntry>>(std::unexpected(target_authority.error()));
  auto target_recovery = target ? (*target)->recover() : ava::core::VoidResult(std::unexpected(target.error()));

  expect(policy_authority && target && first && first_load && first_load->size() == 1 && second && !provider_iteration_load && legacy_authority &&
             legacy_load && legacy_load->size() == 2 && target_authority && !target_load && !target_recovery,
         "policy-bound runtime authorities reject history growth between provider iterations while legacy authorities remain compatible and targets retain the "
         "exact policy");
}

void test_session_read_authority_identity_fingerprint_and_clamp()
{
  auto const root = create_empty_root("session-read-authority-fingerprint");
  auto const workspace = root / "workspace";
  auto const sessions = root / "sessions";
  std::filesystem::create_directories(workspace);

  auto entry = [](std::string id, std::string text) {
    return ava::session::SessionEntry{.id = std::move(id),
                                      .parent_id = "",
                                      .type = ava::session::EntryType::UserMessage,
                                      .timestamp = ava::session::now_timestamp(),
                                      .data_json = "{\"text\":\"" + text + "\"}"};
  };

  auto const policy = ava::session::SessionReadLimits{.max_file_bytes = 4096, .max_line_bytes = 2048, .max_entries = 2};
  ava::session::SessionStore store(ava::session::SessionStoreOptions{.root_dir = sessions, .workspace_dir = workspace, .session_id = "fp-persistent"});
  auto lease = ava::session::SessionLease::create_and_acquire(store.session_path());
  expect(lease.has_value(), "fingerprint fixture acquires lease");
  if (!lease)
    return;
  expect(store.append(*lease, entry("one", "first")).has_value(), "fingerprint fixture seeds history");

  auto authority = ava::session::SessionReadAuthority::create_persistent(store, *lease, policy);
  expect(authority && authority->active() && !authority->is_ephemeral() && authority->session_id() == "fp-persistent",
         "persistent authority exposes session identity and ephemeral=false");
  if (!authority)
    return;

  auto first_fp = authority->content_fingerprint();
  expect(first_fp && !first_fp->ephemeral && first_fp->size > 0, "persistent fingerprint is lease-bound and non-empty");
  auto same_fp = authority->content_fingerprint();
  expect(first_fp && same_fp && *first_fp == *same_fp, "unchanged persistent content keeps an equal fingerprint");

  // Wider request limits must clamp to the embedded policy and never load a third entry.
  auto wide = ava::session::SessionReadLimits{.max_file_bytes = 8U * 1024U * 1024U, .max_line_bytes = 1024U * 1024U, .max_entries = 1000};
  expect(store.append(*lease, entry("two", "second")).has_value(), "fingerprint fixture appends second entry");
  expect(store.append(*lease, entry("three", "third")).has_value(), "fingerprint fixture appends third entry");
  auto clamped = authority->load_bounded(wide);
  expect(!clamped, "load_bounded clamps max_entries to the authority policy and rejects growth beyond it");

  auto tighter = ava::session::SessionReadLimits{.max_file_bytes = 4096, .max_line_bytes = 2048, .max_entries = 1};
  auto narrowed = authority->load_bounded(tighter);
  // With max_entries=1 effective, visiting stops... actually visit loads all and fails if count exceeds.
  // Session store rejects when entries.size() > max_entries for ephemeral; for persistent it fails during visit when count exceeds.
  // With 3 entries and max_entries=1, load should fail.
  expect(!narrowed, "load_bounded honors a tighter caller max_entries without widening");

  auto after_append_fp = authority->content_fingerprint();
  expect(first_fp && after_append_fp && *first_fp != *after_append_fp, "persistent fingerprint changes after durable appends");

  auto ephemeral = ava::session::SessionStore::create_ephemeral(workspace);
  expect(ephemeral.has_value(), "ephemeral fingerprint fixture creates store");
  if (!ephemeral)
    return;
  auto ephemeral_authority = ava::session::SessionReadAuthority::create_ephemeral(*ephemeral, policy);
  expect(ephemeral_authority && ephemeral_authority->is_ephemeral() && !ephemeral_authority->session_id().empty(),
         "ephemeral authority exposes identity and ephemeral=true");
  if (!ephemeral_authority)
    return;
  auto empty_fp = ephemeral_authority->content_fingerprint();
  expect(ephemeral->append_ephemeral(entry("e1", "ephemeral")).has_value(), "ephemeral fingerprint fixture appends");
  auto filled_fp = ephemeral_authority->content_fingerprint();
  expect(empty_fp && filled_fp && empty_fp->ephemeral && filled_fp->ephemeral && *empty_fp != *filled_fp && filled_fp->entry_count == 1,
         "ephemeral fingerprint tracks shared in-memory tip changes");

  auto eph_clamped = ephemeral_authority->load_bounded(wide);
  expect(eph_clamped && eph_clamped->size() == 1, "ephemeral load_bounded serves content under clamped policy");
  expect(ephemeral->append_ephemeral(entry("e2", "two")).has_value(), "ephemeral grows to policy edge");
  expect(ephemeral->append_ephemeral(entry("e3", "three")).has_value(), "ephemeral exceeds policy edge");
  auto eph_over = ephemeral_authority->load_bounded(wide);
  expect(!eph_over, "ephemeral load_bounded never widens past the authority max_entries policy");
}

void test_session_append_authority_and_commit_state()
{
  auto const root = create_empty_root("session-append-authority");

  auto const workspace = root / "workspace";
  auto const sessions = root / "sessions";
  std::filesystem::create_directories(workspace);

  auto entry = [](std::string id) {
    return ava::session::SessionEntry{.id = std::move(id),
                                      .parent_id = "",
                                      .type = ava::session::EntryType::UserMessage,
                                      .timestamp = ava::session::now_timestamp(),
                                      .data_json = "{\"text\":\"authority\"}"};
  };
  auto has_one_state = [](ava::core::VoidResult const& result, std::string_view expected) {
    return !result && error_context_count(result.error(), "append_commit_state") == 1 && error_context_value(result.error(), "append_commit_state") == expected;
  };

  ava::session::SessionStore store(ava::session::SessionStoreOptions{.root_dir = sessions, .workspace_dir = workspace, .session_id = "persistent"});
  auto wrong_mode = store.append_ephemeral(entry("wrong-mode"));
  auto no_lease = store.append(ava::session::SessionLease{}, entry("no-lease"));
  expect(!wrong_mode && error_context_count(wrong_mode.error(), "append_commit_state") == 0,
         "ephemeral append API failures remain free of persistent disk commit state");
  expect(has_one_state(no_lease, "not_started"), "persistent stores reject missing append authority with exactly one not-started state");

  auto lease = ava::session::SessionLease::create_and_acquire(store.session_path());
  expect(lease.has_value(), "append authority fixture creates its exact lease");
  if (!lease)
    return;
  auto invalid_data = entry("invalid-data");
  invalid_data.data_json.clear();
  auto invalid_data_result = store.append(*lease, invalid_data);
  auto invalid_parent = entry("invalid-parent");
  invalid_parent.parent_id = ".";
  auto invalid_parent_result = store.append(*lease, invalid_parent);
  auto oversized = entry("oversized");
  oversized.data_json = "{\"text\":\"" + std::string(ava::session::kMaxSessionLineBytes, 'x') + "\"}";
  auto oversized_result = store.append(*lease, oversized);
  ava::session::SessionStore invalid_session(ava::session::SessionStoreOptions{.root_dir = sessions, .workspace_dir = workspace, .session_id = "../invalid"});
  auto invalid_session_result = invalid_session.append(*lease, entry("invalid-session"));
  expect(has_one_state(invalid_data_result, "not_started") && has_one_state(invalid_parent_result, "not_started") &&
             has_one_state(oversized_result, "not_started") && has_one_state(invalid_session_result, "not_started"),
         "persistent invalid data, parent, serialization, and session failures each expose exactly one not-started commit state");

  auto first = store.append(*lease, entry("first"));
  auto second = store.append(*lease, entry("second"));
  auto loaded = store.load();
  expect(first && second && loaded && loaded->size() == 2, "one matching active lease authorizes multiple persistent appends");

  ava::session::SessionStore other(ava::session::SessionStoreOptions{.root_dir = sessions, .workspace_dir = workspace, .session_id = "other"});
  auto other_lease = ava::session::SessionLease::create_and_acquire(other.session_path());
  auto wrong = other_lease ? store.append(*other_lease, entry("wrong-lease")) : ava::core::VoidResult(std::unexpected(std::move(other_lease.error())));
  expect(has_one_state(wrong, "not_started") && store.load() && store.load()->size() == 2,
         "persistent append rejects an active lease for another exact path with exactly one not-started state");

  auto target = ava::session::SessionAppendTarget::create_persistent(store, *lease, ava::session::SessionReadLimits{});
  expect(target.has_value(), "persistent append target duplicates a matching lease");
  if (target)
  {
    lease = ava::session::SessionLease{};
    auto through_target = (*target)->append(entry("target-owned"));
    expect(through_target && store.load() && store.load()->size() == 3,
           "persistent append target retains duplicated same-description lease authority after caller release");
  }

  ava::session::SessionStore moved_store(ava::session::SessionStoreOptions{.root_dir = sessions, .workspace_dir = workspace, .session_id = "moved"});
  auto movable = ava::session::SessionLease::create_and_acquire(moved_store.session_path());
  if (movable)
  {
    auto moved_to = std::move(*movable);
    auto moved_from_append = moved_store.append(*movable, entry("moved-from"));
    expect(has_one_state(moved_from_append, "not_started") && !movable->active() && moved_to.active(),
           "a moved-from lease cannot authorize a persistent append and reports one not-started state");
  }

  auto ephemeral = ava::session::SessionStore::create_ephemeral(workspace);
  if (ephemeral)
  {
    auto ephemeral_target = ava::session::SessionAppendTarget::create_ephemeral(*ephemeral);
    auto direct = ephemeral->append_ephemeral(entry("ephemeral"));
    auto persistent_target = ava::session::SessionAppendTarget::create_persistent(*ephemeral, ava::session::SessionLease{}, ava::session::SessionReadLimits{});
    auto ephemeral_with_lease = ephemeral->append(ava::session::SessionLease{}, entry("ephemeral-with-lease"));
    auto invalid_ephemeral = entry("invalid-ephemeral");
    invalid_ephemeral.data_json.clear();
    auto invalid_ephemeral_result = ephemeral->append_ephemeral(invalid_ephemeral);
    expect(direct && ephemeral_target && (*ephemeral_target)->append(entry("ephemeral-target")) && !persistent_target &&
               has_one_state(ephemeral_with_lease, "not_started") && !invalid_ephemeral_result &&
               error_context_count(invalid_ephemeral_result.error(), "append_commit_state") == 0,
           "ephemeral stores and targets accept only explicit in-memory authority without leaking disk commit state");
  }

  {
    ava::session::SessionStore allocation_store(
        ava::session::SessionStoreOptions{.root_dir = sessions, .workspace_dir = workspace, .session_id = "target-allocation"});
    auto allocation_lease = ava::session::SessionLease::create_and_acquire(allocation_store.session_path());
    if (allocation_lease)
    {
      allocation_store.fail_persistent_append_target_allocation_for_test();
      auto failed_target = ava::session::SessionAppendTarget::create_persistent(allocation_store, *allocation_lease, ava::session::SessionReadLimits{});
      allocation_store.fail_persistent_append_target_allocation_for_test(false);
      *allocation_lease = ava::session::SessionLease{};
      auto reacquired = ava::session::SessionLease::acquire(allocation_store.session_path());
      expect(!failed_target && reacquired,
             "persistent append target allocation failure closes its immediately adopted duplicate descriptor and releases flock with the caller lease");
    }
  }

  ava::session::SessionStore partial_store(ava::session::SessionStoreOptions{.root_dir = sessions, .workspace_dir = workspace, .session_id = "partial"});
  auto partial_lease = ava::session::SessionLease::create_and_acquire(partial_store.session_path());
  if (partial_lease)
  {
    expect(partial_store.append(*partial_lease, entry("prefix")).has_value(), "partial-write fixture seeds framed record");
    int calls = 0;
    partial_store.set_append_write_for_test([&calls](int, std::string_view bytes) -> ssize_t {
      if (++calls == 1)
        return static_cast<ssize_t>(std::min<std::size_t>(1, bytes.size()));
      errno = EIO;
      return -1;
    });
    auto partial = partial_store.append(*partial_lease, entry("partial"));
    partial_store.set_append_write_for_test({});
    expect(has_one_state(partial, "partial_or_unknown") && partial.error().format().find("recover_torn_tail") != std::string::npos,
           "partial append failures carry stable recovery-required commit state");
    auto recovered = partial_store.recover_torn_tail(*partial_lease, ava::session::legacy_unbounded_session_read_limits());
    auto recovered_append =
        recovered ? partial_store.append(*partial_lease, entry("after-recovery")) : ava::core::VoidResult(std::unexpected(std::move(recovered.error())));
    expect(recovered && recovered_append, "the same retained lease repairs a partial append tail before a later append");

    partial_store.set_after_append_write_for_test([] { throw 1; });
    auto committed = partial_store.append(*partial_lease, entry("committed"));
    partial_store.set_after_append_write_for_test({});
    expect(has_one_state(committed, "committed_to_leased_inode") &&
               ava::tests::read_session_test_binary_file(partial_store.session_path()).find("committed") != std::string::npos,
           "post-write failures report committed-to-leased-inode state without retrying");
  }

  ava::session::SessionStore parent_swap_store(
      ava::session::SessionStoreOptions{.root_dir = sessions, .workspace_dir = workspace, .session_id = "parent-swap"});
  auto parent_swap_lease = ava::session::SessionLease::create_and_acquire(parent_swap_store.session_path());
  if (parent_swap_lease)
  {
    expect(parent_swap_store.append(*parent_swap_lease, entry("parent-prefix")).has_value(), "parent-swap fixture seeds its leased inode");
    auto const original_bytes = ava::tests::read_session_test_binary_file(parent_swap_store.session_path());
    auto const parent = parent_swap_store.session_path().parent_path();
    auto const moved_parent = parent.string() + ".moved";
    parent_swap_store.set_before_append_identity_check_for_test([&] {
      std::filesystem::rename(parent, moved_parent);
      std::filesystem::create_directories(parent);
      ava::tests::write_session_test_binary_file(parent_swap_store.session_path(), "replacement\\n");
    });
    auto parent_swap = parent_swap_store.append(*parent_swap_lease, entry("parent-race"));
    parent_swap_store.set_before_append_identity_check_for_test({});
    expect(has_one_state(parent_swap, "not_started") &&
               ava::tests::read_session_test_binary_file(moved_parent / parent_swap_store.session_path().filename()) == original_bytes &&
               ava::tests::read_session_test_binary_file(parent_swap_store.session_path()) == "replacement\\n",
           "persistent append rejects a parent-directory replacement before mutating either original or replacement name");
  }

  ava::session::SessionStore fifo_store(ava::session::SessionStoreOptions{.root_dir = sessions, .workspace_dir = workspace, .session_id = "fifo"});
  auto fifo_lease = ava::session::SessionLease::create_and_acquire(fifo_store.session_path());
  if (fifo_lease)
  {
    auto const parked = fifo_store.session_path().string() + ".parked";
    std::filesystem::rename(fifo_store.session_path(), parked);
    int const fifo_result = ::mkfifo(fifo_store.session_path().c_str(), 0600);
    auto fifo_append = fifo_result == 0 ? fifo_store.append(*fifo_lease, entry("fifo"))
                                        : ava::core::VoidResult(std::unexpected(ava::core::Error(ava::core::ErrorCategory::Io, "mkfifo failed")));
    expect(!fifo_append, "persistent append rejects a FIFO replacement without blocking or recreating it");
  }
}

}  // namespace session_tests

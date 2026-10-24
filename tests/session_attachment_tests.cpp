#include "sys.h"
#include "tests/session_test_declarations.h"
#include "tests/support/session_test_support.h"
#include "tests/support/syscall_errno_injection.h"
#include "tests/support/test_harness.h"
#include "ava/agent/message_builder.h"
#include "ava/session/attachments.h"
#include "ava/session/session_branch.h"
#include "ava/session/session_store.h"
#include "ava/session/validation.h"
#include "ava/provider/provider.h"
#include "ava/provider/provider_utils.h"
#include "ava/core/UniqueFd.h"
#include "ava/core/result.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <expected>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/ptrace.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#endif

namespace session_tests {
namespace {
void expect_image_error(auto const& result, ava::core::ErrorCategory category, std::string_view context_key, std::string_view context_value,
                        std::string const& message, std::source_location location = std::source_location::current())
{
  if (!ava::tests::require(!result, message, location))
    return;
  ava::tests::expect_eq(result.error().category(), category, message + ": category", location);
  ava::tests::expect_all(message,
                         {{"error context", std::ranges::any_of(result.error().context(),
                                                                [&](auto const& item) { return item.key == context_key && item.value == context_value; })}},
                         location);
}

std::optional<std::filesystem::path> created_session_rollback_quarantine(std::filesystem::path const& session_path)
{
  auto const prefix = "." + session_path.filename().string() + ".rollback.";
  std::error_code iterator_error;
  for (std::filesystem::directory_iterator iterator(session_path.parent_path(), iterator_error), end; !iterator_error && iterator != end;
       iterator.increment(iterator_error))
  {
    if (iterator->path().filename().string().starts_with(prefix))
      return iterator->path();
  }
  return std::nullopt;
}

struct AttachmentReplacement
{
  std::filesystem::path canary{};
  std::filesystem::path component{};
  bool directory = false;
  bool write = false;
};

// Stop at the actual open syscall, after any pathname validation. For canary
// cases keep tracing reads as well: matching bytes and hashes must not mask an
// outside read. Directory swaps may safely use an already-held original inode.
template <typename ReadAttachment>
void test_attachment_replacement_before_open(std::filesystem::path const& path, ReadAttachment&& read_attachment, AttachmentReplacement const& replacement = {})
{
#if defined(__linux__) && defined(PTRACE_GET_SYSCALL_INFO)
  for (bool const symlink : {false, true})
  {
    if (!replacement.canary.empty() && !symlink)
      continue;
    std::filesystem::path swapped_path;
    std::filesystem::path parked;
    auto const canary_bytes = replacement.canary.empty() ? std::string{} : ava::tests::read_session_test_binary_file(replacement.canary);
    struct stat canary_status{};
    if (!replacement.canary.empty() && !ava::tests::require(::stat(replacement.canary.c_str(), &canary_status) == 0, "outside canary is inspectable"))
      return;
    auto const child = ::fork();
    expect(child >= 0, "attachment replacement regression forks a watchdog-protected reader");
    if (child < 0)
      return;
    if (child == 0)
    {
      if (::ptrace(PTRACE_TRACEME, 0, nullptr, nullptr) != 0)
        ::_exit(77);
      ::raise(SIGSTOP);
      auto result = read_attachment();
      if (!replacement.canary.empty())
        ::_exit(result || result.error().category() == ava::core::ErrorCategory::PermissionDenied || result.error().category() == ava::core::ErrorCategory::Io
                    ? 0
                    : 1);
      auto const expected_category = symlink ? ava::core::ErrorCategory::PermissionDenied : ava::core::ErrorCategory::InvalidArgument;
      ::_exit(!result && result.error().category() == expected_category ? 0 : 1);
    }

    ava::tests::ScopedChild child_owner(child);
    auto const deadline = ava::tests::now_plus_seconds(10);
    int status = 0;
    bool child_reaped = false;
    auto const wait_child = [&] {
      // A pidfd signals exit, not ptrace stops. Observe stop predicates directly
      // instead of blocking an exit-only wait on a child deliberately stopped.
      while (std::chrono::steady_clock::now() < deadline)
      {
        auto const waited = ::waitpid(child, &status, WNOHANG | WUNTRACED);
        if (waited == child)
        {
          child_reaped = WIFEXITED(status) || WIFSIGNALED(status);
          return true;
        }
        if (waited < 0 && errno != EINTR)
        {
          child_reaped = errno == ECHILD;
          return false;
        }
        std::this_thread::yield();
      }
      return false;
    };
    auto const kill_child = [&] {
      if (child_reaped)
        return;
      static_cast<void>(::kill(child, SIGKILL));
      auto const cleanup_deadline = ava::tests::now_plus_seconds(3);
      for (;;)
      {
        auto const reaped = ava::tests::wait_child_until(child, cleanup_deadline);
        if (!reaped || WIFEXITED(*reaped) || WIFSIGNALED(*reaped))
        {
          child_reaped = true;
          return;
        }
      }
    };
    if (!wait_child())
    {
      kill_child();
      expect(false, "attachment replacement watchdog reader is waitable");
      return;
    }
    if (WIFEXITED(status))
    {
      auto const outcome = ava::tests::expect_child_success(status, "attachment replacement reader exits successfully",
                                                            "attachment replacement race requires permitted parent/child ptrace");
      if (outcome != ava::tests::ChildTestOutcome::Passed)
        return;
      expect(false, "attachment replacement reader must stop at its tracing gate before reading");
      return;
    }
    if (!WIFSTOPPED(status) || ::ptrace(PTRACE_SETOPTIONS, child, nullptr, PTRACE_O_TRACESYSGOOD | PTRACE_O_EXITKILL) != 0)
    {
      kill_child();
      ava::tests::request_skip("attachment replacement race requires syscall tracing support");
      return;
    }

    bool swapped = false;
    bool original_parked = false;
    bool trace_ok = true;
    bool unsupported = false;
    bool outside_read = false;
    int signal_to_deliver = 0;
    while (true)
    {
      auto const request = swapped && replacement.canary.empty() ? PTRACE_CONT : PTRACE_SYSCALL;
      if (::ptrace(request, child, nullptr, reinterpret_cast<void*>(static_cast<std::intptr_t>(signal_to_deliver))) != 0 || !wait_child())
      {
        trace_ok = false;
        kill_child();
        break;
      }
      if (WIFEXITED(status) || WIFSIGNALED(status))
        break;
      if (!WIFSTOPPED(status))
      {
        trace_ok = false;
        kill_child();
        break;
      }
      signal_to_deliver = WSTOPSIG(status);
      if (signal_to_deliver != (SIGTRAP | 0x80))
        continue;
      signal_to_deliver = 0;

      struct __ptrace_syscall_info info{};
      if (::ptrace(PTRACE_GET_SYSCALL_INFO, child, sizeof(info), &info) < 0)
      {
        unsupported = errno == EINVAL || errno == EIO || errno == ENOSYS;
        trace_ok = false;
        kill_child();
        break;
      }
      if (info.op != PTRACE_SYSCALL_INFO_ENTRY)
        continue;
      if (swapped)
      {
        if (info.entry.nr == SYS_read)
        {
          struct stat accessed{};
          auto const descriptor = "/proc/" + std::to_string(child) + "/fd/" + std::to_string(info.entry.args[0]);
          if (::stat(descriptor.c_str(), &accessed) == 0 && accessed.st_dev == canary_status.st_dev && accessed.st_ino == canary_status.st_ino)
            outside_read = true;
        }
        continue;
      }
      bool is_open = info.entry.nr == SYS_openat;
#ifdef SYS_openat2
      is_open = is_open || info.entry.nr == SYS_openat2;
#endif
      if (!is_open)
        continue;
      if (!replacement.canary.empty())
      {
        auto flags = info.entry.args[2];
#ifdef SYS_openat2
        if (info.entry.nr == SYS_openat2)
        {
          errno = 0;
          auto const word = ::ptrace(PTRACE_PEEKDATA, child, reinterpret_cast<void*>(static_cast<std::uintptr_t>(info.entry.args[2])), nullptr);
          if (word == -1 && errno != 0)
          {
            trace_ok = false;
            kill_child();
            break;
          }
          flags = static_cast<std::uint64_t>(word);
        }
#endif
        if (((flags & O_CREAT) != 0) != replacement.write)
          continue;
      }
      std::string opened_path;
      for (std::size_t offset = 0; offset < 4096; offset += sizeof(long))
      {
        errno = 0;
        auto const word = ::ptrace(PTRACE_PEEKDATA, child, reinterpret_cast<void*>(static_cast<std::uintptr_t>(info.entry.args[1]) + offset), nullptr);
        if (word == -1 && errno != 0)
          break;
        std::array<char, sizeof(word)> bytes{};
        std::memcpy(bytes.data(), &word, sizeof(word));
        for (char const byte : bytes)
        {
          if (byte == '\0')
            break;
          opened_path += byte;
        }
        if (std::find(bytes.begin(), bytes.end(), '\0') != bytes.end())
          break;
      }
      auto const opened = std::filesystem::path(opened_path);
      if (opened != path && ((!replacement.write && opened.is_absolute()) || opened.filename() != path.filename()))
        continue;

      auto actual = replacement.canary.empty() ? path : opened;
      if (!replacement.canary.empty() && !actual.is_absolute())
      {
        std::array<char, 4096> directory{};
        auto const descriptor = "/proc/" + std::to_string(child) + "/fd/" + std::to_string(static_cast<int>(info.entry.args[0]));
        auto const count = ::readlink(descriptor.c_str(), directory.data(), directory.size());
        if (count <= 0 || static_cast<std::size_t>(count) == directory.size())
        {
          trace_ok = false;
          kill_child();
          break;
        }
        actual = std::filesystem::path(std::string(directory.data(), static_cast<std::size_t>(count))) / actual;
      }
      swapped_path = replacement.component.empty() ? (replacement.directory ? actual.parent_path() : actual) : replacement.component;
      parked = std::filesystem::path(swapped_path.string() + ".before-open");
      original_parked = ::rename(swapped_path.c_str(), parked.c_str()) == 0;
      auto const target = replacement.canary.empty() ? parked : (replacement.directory ? replacement.canary.parent_path() : replacement.canary);
      swapped = (original_parked || (replacement.write && errno == ENOENT)) &&
                (symlink ? ::symlink(target.c_str(), swapped_path.c_str()) == 0 : ::mkfifo(swapped_path.c_str(), 0600) == 0);
      if (original_parked && !swapped && (errno == ENOSYS || errno == EOPNOTSUPP))
        unsupported = true;
      if (!swapped)
      {
        trace_ok = false;
        kill_child();
        break;
      }
    }
    if (swapped)
      expect(::unlink(swapped_path.c_str()) == 0, "attachment replacement fixture removes its replacement");
    if (original_parked)
      expect(::rename(parked.c_str(), swapped_path.c_str()) == 0, "attachment replacement fixture restores its original inode");
    if (!replacement.canary.empty())
    {
      struct stat after{};
      expect(!outside_read, "attachment operation never reads the outside canary, even when its bytes match the expected hash");
      expect(ava::tests::read_session_test_binary_file(replacement.canary) == canary_bytes && ::stat(replacement.canary.c_str(), &after) == 0 &&
                 after.st_mode == canary_status.st_mode,
             "attachment operation preserves outside canary bytes and permissions");
    }
    if (unsupported)
    {
      ava::tests::request_skip("attachment replacement race requires syscall tracing and FIFO/symlink filesystem support");
      return;
    }
    expect(trace_ok && swapped && WIFEXITED(status) && WEXITSTATUS(status) == 0,
           symlink ? "attachment reader rejects a symlink installed after pathname inspection"
                   : "attachment reader rejects a FIFO installed after pathname inspection without blocking");
  }
#else
  static_cast<void>(path);
  static_cast<void>(read_attachment);
  static_cast<void>(replacement);
  ava::tests::request_skip("attachment replacement race requires Linux syscall tracing");
#endif
}

}  // namespace

void test_image_attachment_message_reconstruction_and_validation()
{
  std::string const attachment_json = R"({"id":"img_1","type":"image","mime_type":"image/png","byte_size":1234,)"
                                      R"("sha256":"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",)"
                                      R"("storage_path":"attachments/img_1.png"})";
  std::vector<ava::session::SessionEntry> const entries = {
      ava::session::SessionEntry{.id = "image_user",
                                 .parent_id = "",
                                 .type = ava::session::EntryType::UserMessage,
                                 .timestamp = "2026-05-08T00:00:00Z",
                                 .data_json = "{\"text\":\"describe this\",\"attachments\":[" + attachment_json + "]}"}};

  auto messages = ava::agent::build_provider_messages_from_entries(
      entries, ava::agent::MessageBuildOptions{.target = ava::agent::HistoryReplayTarget{.provider_id = "openai",
                                                                                         .model_id = "gpt-image",
                                                                                         .api_family = "openai_responses",
                                                                                         .reasoning_format = "openai_responses",
                                                                                         .supports_tools = false,
                                                                                         .supports_images = true},
                                               .active_turn_user_entry_ids = {"image_user"}});
  expect(messages && messages->size() == 1, "active image user message reconstructs as one provider message");
  if (!messages || messages->empty())
    return;
  expect((*messages)[0].content.find("describe this") != std::string::npos &&
             (*messages)[0].content.find("[image attachment: id=img_1 mime=image/png bytes=1234]") != std::string::npos,
         "image user message keeps text fallback metadata without raw bytes");
  expect((*messages)[0].content_parts.size() == 2 && (*messages)[0].content_parts[0].type == ava::provider::ContentPartType::Text &&
             (*messages)[0].content_parts[1].type == ava::provider::ContentPartType::Image && (*messages)[0].content_parts[1].attachment_id == "img_1" &&
             (*messages)[0].content_parts[1].mime_type == "image/png" && (*messages)[0].content_parts[1].storage_path == "attachments/img_1.png" &&
             (*messages)[0].content_parts[1].byte_size == 1234,
         "image user message carries provider-neutral image content metadata");

  auto const valid = ava::session::validate_session_replay(entries);
  expect(valid.ok(), "image attachment metadata validates when bounded and referenced");

  std::vector<ava::session::SessionEntry> const inline_data_entries = {
      ava::session::SessionEntry{.id = "image_inline",
                                 .parent_id = "",
                                 .type = ava::session::EntryType::UserMessage,
                                 .timestamp = "2026-05-08T00:00:00Z",
                                 .data_json = R"({"text":"bad","attachments":[{"id":"img_2","type":"image","mime_type":"image/png",)"
                                              R"("byte_size":12,"sha256":"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",)"
                                              R"("storage_path":"attachments/img_2.png","data_base64":"AAAA"}]})"}};
  auto const inline_validation = ava::session::validate_session_replay(inline_data_entries);
  expect(!inline_validation.ok() && inline_validation.issues.front().kind == ava::session::SessionReplayIssueKind::InvalidMessageEntry,
         "session replay rejects inline image bytes in message attachment metadata");

  std::vector<ava::session::SessionEntry> const unsupported_mime_entries = {
      ava::session::SessionEntry{.id = "image_svg",
                                 .parent_id = "",
                                 .type = ava::session::EntryType::UserMessage,
                                 .timestamp = "2026-05-08T00:00:00Z",
                                 .data_json = R"({"text":"bad","attachments":[{"id":"img_3","type":"image","mime_type":"image/svg+xml",)"
                                              R"("byte_size":12,"sha256":"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",)"
                                              R"("storage_path":"attachments/img_3.svg"}]})"}};
  auto const mime_validation = ava::session::validate_session_replay(unsupported_mime_entries);
  expect(!mime_validation.ok() && mime_validation.issues.front().kind == ava::session::SessionReplayIssueKind::InvalidMessageEntry,
         "session replay rejects unsupported image attachment MIME types");

  std::vector<ava::session::SessionEntry> const mixed_array_entries = {
      ava::session::SessionEntry{.id = "image_mixed_array",
                                 .parent_id = "",
                                 .type = ava::session::EntryType::UserMessage,
                                 .timestamp = "2026-05-08T00:00:00Z",
                                 .data_json = "{\"text\":\"bad\",\"attachments\":[" + attachment_json + R"(,"raw-bytes"]})"}};
  auto const mixed_array_validation = ava::session::validate_session_replay(mixed_array_entries);
  expect(!mixed_array_validation.ok() && mixed_array_validation.issues.front().kind == ava::session::SessionReplayIssueKind::InvalidMessageEntry,
         "session replay rejects non-object attachment array elements");

  std::vector<ava::session::SessionEntry> const unknown_raw_entries = {
      ava::session::SessionEntry{.id = "image_unknown_raw",
                                 .parent_id = "",
                                 .type = ava::session::EntryType::UserMessage,
                                 .timestamp = "2026-05-08T00:00:00Z",
                                 .data_json = R"({"text":"bad","attachments":[{"id":"img_4","type":"image","mime_type":"image/png",)"
                                              R"("byte_size":12,"sha256":"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",)"
                                              R"("storage_path":"attachments/img_4.png","raw_bytes_base64":"AAAA"}]})"}};
  auto const unknown_raw_validation = ava::session::validate_session_replay(unknown_raw_entries);
  expect(!unknown_raw_validation.ok() && unknown_raw_validation.issues.front().kind == ava::session::SessionReplayIssueKind::InvalidMessageEntry,
         "session replay rejects unknown attachment fields that could carry inline bytes");

  std::vector<ava::session::SessionEntry> const traversal_path_entries = {
      ava::session::SessionEntry{.id = "image_traversal",
                                 .parent_id = "",
                                 .type = ava::session::EntryType::UserMessage,
                                 .timestamp = "2026-05-08T00:00:00Z",
                                 .data_json = R"({"text":"bad","attachments":[{"id":"img_5","type":"image","mime_type":"image/png",)"
                                              R"("byte_size":12,"sha256":"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",)"
                                              R"("storage_path":"../img_5.png"}]})"}};
  auto const traversal_validation = ava::session::validate_session_replay(traversal_path_entries);
  expect(!traversal_validation.ok() && traversal_validation.issues.front().kind == ava::session::SessionReplayIssueKind::InvalidMessageEntry,
         "session replay rejects escaping image storage paths");

  std::vector<ava::session::SessionEntry> const unanchored_path_entries = {
      ava::session::SessionEntry{.id = "image_unanchored",
                                 .parent_id = "",
                                 .type = ava::session::EntryType::UserMessage,
                                 .timestamp = "2026-05-08T00:00:00Z",
                                 .data_json = R"({"text":"bad","attachments":[{"id":"img_6","type":"image","mime_type":"image/png",)"
                                              R"("byte_size":12,"sha256":"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",)"
                                              R"("storage_path":"README.md"}]})"}};
  auto const unanchored_validation = ava::session::validate_session_replay(unanchored_path_entries);
  expect(!unanchored_validation.ok() && unanchored_validation.issues.front().kind == ava::session::SessionReplayIssueKind::InvalidMessageEntry,
         "session replay rejects image storage paths outside attachments namespace");

  std::vector<ava::session::SessionEntry> const fractional_size_entries = {
      ava::session::SessionEntry{.id = "image_fractional_size",
                                 .parent_id = "",
                                 .type = ava::session::EntryType::UserMessage,
                                 .timestamp = "2026-05-08T00:00:00Z",
                                 .data_json = R"({"text":"bad","attachments":[{"id":"img_6a","type":"image","mime_type":"image/png",)"
                                              R"("byte_size":12.5,"sha256":"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",)"
                                              R"("storage_path":"attachments/img_6a.png"}]})"}};
  auto const fractional_size_validation = ava::session::validate_session_replay(fractional_size_entries);
  expect(!fractional_size_validation.ok() && fractional_size_validation.issues.front().kind == ava::session::SessionReplayIssueKind::InvalidMessageEntry,
         "session replay rejects fractional image byte sizes");
  auto const fractional_size_sanitized = ava::session::sanitized_message_data_json(fractional_size_entries.front().data_json);
  expect(fractional_size_sanitized.find("attachments") == std::string::npos, "message data sanitizer omits fractional image byte sizes");
  auto const fractional_size_messages = ava::agent::build_provider_messages_from_entries(fractional_size_entries);
  expect_image_error(fractional_size_messages, ava::core::ErrorCategory::InvalidArgument, "entry_id", "image_fractional_size",
                     "provider replay rejects invalid fractional image byte sizes instead of dropping image metadata");

  std::vector<ava::session::SessionEntry> const exponent_size_entries = {
      ava::session::SessionEntry{.id = "image_exponent_size",
                                 .parent_id = "",
                                 .type = ava::session::EntryType::UserMessage,
                                 .timestamp = "2026-05-08T00:00:00Z",
                                 .data_json = R"({"text":"bad","attachments":[{"id":"img_6b","type":"image","mime_type":"image/png",)"
                                              R"("byte_size":1e3,"sha256":"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",)"
                                              R"("storage_path":"attachments/img_6b.png"}]})"}};
  auto const exponent_size_validation = ava::session::validate_session_replay(exponent_size_entries);
  expect(!exponent_size_validation.ok() && exponent_size_validation.issues.front().kind == ava::session::SessionReplayIssueKind::InvalidMessageEntry,
         "session replay rejects exponent image byte sizes");

  for (auto const byte_size : {ava::session::kMaxImageAttachmentBytes, ava::session::kMaxImageAttachmentBytes + 1})
  {
    auto boundary_entries = entries;
    auto& data = boundary_entries.front().data_json;
    auto const offset = data.find("\"byte_size\":1234");
    if (!ava::tests::require(offset != std::string::npos, "image replay boundary fixture locates its byte size"))
      return;
    data.replace(offset, std::string_view("\"byte_size\":1234").size(), "\"byte_size\":" + std::to_string(byte_size));
    auto const validation = ava::session::validate_session_replay(boundary_entries);
    auto const replay = ava::agent::build_provider_messages_from_entries(boundary_entries);
    if (byte_size == ava::session::kMaxImageAttachmentBytes)
    {
      ava::tests::expect_all("image metadata accepts the exact per-attachment byte limit",
                             {{"session validation", validation.ok()}, {"provider replay", replay.has_value()}});
    }
    else
    {
      ava::tests::expect_all("image metadata rejects an integer byte size one above the limit",
                             {{"invalid message", ava::tests::session_replay_has_issue(validation, ava::session::SessionReplayIssueKind::InvalidMessageEntry)},
                              {"provider rejection", !replay}});
    }
  }

  std::vector<ava::session::SessionEntry> const duplicate_key_entries = {
      ava::session::SessionEntry{.id = "image_duplicate_key",
                                 .parent_id = "",
                                 .type = ava::session::EntryType::UserMessage,
                                 .timestamp = "2026-05-08T00:00:00Z",
                                 .data_json = R"({"text":"bad","attachments":[{"id":"img_7","id":"img_8","type":"image","mime_type":"image/png",)"
                                              R"("byte_size":12,"sha256":"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",)"
                                              R"("storage_path":"attachments/img_7.png"}]})"}};
  auto const duplicate_key_validation = ava::session::validate_session_replay(duplicate_key_entries);
  expect(!duplicate_key_validation.ok() && duplicate_key_validation.issues.front().kind == ava::session::SessionReplayIssueKind::InvalidMessageEntry,
         "session replay rejects duplicate image attachment object keys");

  std::vector<ava::session::SessionEntry> const duplicate_id_entries = {
      ava::session::SessionEntry{.id = "image_duplicate_id",
                                 .parent_id = "",
                                 .type = ava::session::EntryType::UserMessage,
                                 .timestamp = "2026-05-08T00:00:00Z",
                                 .data_json = "{\"text\":\"bad\",\"attachments\":[" + attachment_json + "," + attachment_json + "]}"}};
  auto const duplicate_id_validation = ava::session::validate_session_replay(duplicate_id_entries);
  expect(!duplicate_id_validation.ok() && duplicate_id_validation.issues.front().kind == ava::session::SessionReplayIssueKind::InvalidMessageEntry,
         "session replay rejects duplicate image attachment ids");
  auto const duplicate_id_sanitized = ava::session::sanitized_message_data_json(duplicate_id_entries.front().data_json);
  expect(duplicate_id_sanitized.find("attachments") == std::string::npos, "message data sanitizer omits duplicate image attachment ids");

  std::vector<ava::session::SessionEntry> const duplicate_top_level_entries = {
      ava::session::SessionEntry{.id = "image_duplicate_top_level",
                                 .parent_id = "",
                                 .type = ava::session::EntryType::UserMessage,
                                 .timestamp = "2026-05-08T00:00:00Z",
                                 .data_json = "{\"text\":\"bad\",\"attachments\":[" + attachment_json + R"(],"attachments":["raw-bytes"]})"}};
  auto const duplicate_top_level_validation = ava::session::validate_session_replay(duplicate_top_level_entries);
  expect(
      !duplicate_top_level_validation.ok() && duplicate_top_level_validation.issues.front().kind == ava::session::SessionReplayIssueKind::InvalidMessageEntry,
      "session replay rejects duplicate top-level message attachment keys");
  auto const duplicate_top_level_sanitized = ava::session::sanitized_message_data_json(duplicate_top_level_entries.front().data_json);
  expect(duplicate_top_level_sanitized.find("raw-bytes") == std::string::npos, "message data sanitizer omits duplicate top-level attachment payloads");

  std::vector<ava::session::SessionEntry> const escaped_top_level_entries = {
      ava::session::SessionEntry{.id = "image_escaped_top_level",
                                 .parent_id = "",
                                 .type = ava::session::EntryType::UserMessage,
                                 .timestamp = "2026-05-08T00:00:00Z",
                                 .data_json = "{\"text\":\"bad\",\"attach\\u006dents\":[" + attachment_json + "]}"}};
  auto const escaped_top_level_validation = ava::session::validate_session_replay(escaped_top_level_entries);
  expect(!escaped_top_level_validation.ok() && escaped_top_level_validation.issues.front().kind == ava::session::SessionReplayIssueKind::InvalidMessageEntry,
         "session replay rejects escaped top-level message keys in attachment-bearing data");

  std::vector<ava::session::SessionEntry> const assistant_attachment_entries = {
      ava::session::SessionEntry{.id = "assistant_image",
                                 .parent_id = "",
                                 .type = ava::session::EntryType::AssistantMessage,
                                 .timestamp = "2026-05-08T00:00:00Z",
                                 .data_json = "{\"text\":\"assistant\",\"attachments\":[" + attachment_json + "]}"}};
  auto const assistant_validation = ava::session::validate_session_replay(assistant_attachment_entries);
  expect(!assistant_validation.ok() && assistant_validation.issues.front().kind == ava::session::SessionReplayIssueKind::InvalidMessageEntry,
         "session replay rejects assistant image attachments until assistant image semantics exist");
  auto const assistant_sanitized = ava::session::sanitized_message_data_json(assistant_attachment_entries.front().data_json, false);
  expect(assistant_sanitized.find("attachments") == std::string::npos,
         "message data sanitizer omits assistant image attachment metadata when attachments are disallowed");

  auto const sanitized = ava::session::sanitized_message_data_json(unknown_raw_entries.front().data_json);
  expect(sanitized.find("raw_bytes_base64") == std::string::npos && sanitized.find("AAAA") == std::string::npos,
         "message data sanitizer omits unknown attachment fields and inline bytes");

  ava::provider::ProviderRequest text_only_request{
      .provider_id = "test", .model_id = "text-only", .system_prompt = "", .messages = *messages, .tools_json = {}};
  auto const text_only = ava::provider::validate_image_content_parts(text_only_request, false);
  expect_image_error(text_only, ava::core::ErrorCategory::InvalidArgument, "content_part_index", "1",
                     "provider image validation rejects image parts for text-only models");
  auto const image_model = ava::provider::validate_image_content_parts(text_only_request, true);
  expect(image_model.has_value(), "provider image validation accepts bounded user image metadata for image models");

  auto too_many_images = *messages;
  too_many_images[0].content_parts.clear();
  for (int index = 0; index < 17; ++index)
  {
    too_many_images[0].content_parts.push_back(ava::provider::ContentPart{.type = ava::provider::ContentPartType::Image,
                                                                          .attachment_id = "img_" + std::to_string(index),
                                                                          .mime_type = "image/png",
                                                                          .storage_path = "attachments/img_" + std::to_string(index) + ".png",
                                                                          .sha256 = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",
                                                                          .byte_size = 1});
  }
  ava::provider::ProviderRequest too_many_request{
      .provider_id = "test", .model_id = "image-model", .system_prompt = "", .messages = too_many_images, .tools_json = {}};
  auto const too_many = ava::provider::validate_image_content_parts(too_many_request, true);
  expect_image_error(too_many, ava::core::ErrorCategory::InvalidArgument, "content_part_index", "16",
                     "provider image validation caps image attachment count per request");

  auto too_large_total = *messages;
  too_large_total[0].content_parts.clear();
  for (int index = 0; index < 3; ++index)
  {
    too_large_total[0].content_parts.push_back(ava::provider::ContentPart{.type = ava::provider::ContentPartType::Image,
                                                                          .attachment_id = "big_" + std::to_string(index),
                                                                          .mime_type = "image/png",
                                                                          .storage_path = "attachments/big_" + std::to_string(index) + ".png",
                                                                          .sha256 = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",
                                                                          .byte_size = 15 * 1024 * 1024});
  }
  too_many_request.messages = too_large_total;
  auto const too_large = ava::provider::validate_image_content_parts(too_many_request, true);
  expect_image_error(too_large, ava::core::ErrorCategory::InvalidArgument, "content_part_index", "2",
                     "provider image validation caps aggregate image bytes per request");

  auto invalid_path_messages = *messages;
  invalid_path_messages[0].content_parts[1].storage_path = "/tmp/img_1.png";
  ava::provider::ProviderRequest invalid_path_request{
      .provider_id = "test", .model_id = "image-model", .system_prompt = "", .messages = invalid_path_messages, .tools_json = {}};
  auto const invalid_path = ava::provider::validate_image_content_parts(invalid_path_request, true);
  expect_image_error(invalid_path, ava::core::ErrorCategory::InvalidArgument, "content_part_index", "1",
                     "provider image validation rejects absolute image storage paths");
  invalid_path_messages[0].content_parts[1].storage_path = "README.md";
  invalid_path_request.messages = invalid_path_messages;
  auto const unanchored_path = ava::provider::validate_image_content_parts(invalid_path_request, true);
  expect_image_error(unanchored_path, ava::core::ErrorCategory::InvalidArgument, "content_part_index", "1",
                     "provider image validation rejects storage paths outside attachments namespace");
}

void test_synthetic_delivery_provenance_validation()
{
  ava::session::SessionEntry legacy{.id = "legacy_user",
                                    .parent_id = "",
                                    .type = ava::session::EntryType::UserMessage,
                                    .timestamp = "2026-07-20T00:00:00Z",
                                    .data_json = R"({"text":"ordinary legacy message"})"};
  auto legacy_provenance = ava::session::parse_synthetic_delivery_provenance(legacy);
  expect(legacy_provenance && !*legacy_provenance, "legacy ordinary user messages remain compatible without synthetic provenance");

  auto synthetic = legacy;
  synthetic.id = "synthetic_user";
  synthetic.data_json =
      R"({"text":"backend delivery","provenance":{"source":"synthetic_subagent_delivery","delivery_id":"delivery_1","prompt_fingerprint":"0123456789abcdef"}})";
  auto parsed = ava::session::parse_synthetic_delivery_provenance(synthetic);
  auto valid_replay = ava::session::validate_session_replay({synthetic});
  expect(parsed && *parsed && (*parsed)->delivery_id == "delivery_1" && (*parsed)->prompt_fingerprint == "0123456789abcdef" && valid_replay.ok(),
         "session-v4 parsing retains strict bounded backend-only delivery provenance");

  auto malformed = synthetic;
  malformed.id = "malformed_synthetic_user";
  malformed.data_json =
      R"({"text":"bad","provenance":{"source":"synthetic_subagent_delivery","delivery_id":"delivery_1","prompt_fingerprint":"0123456789abcdef","forged":true}})";
  auto malformed_replay = ava::session::validate_session_replay({malformed});
  auto oversized = synthetic;
  oversized.id = "oversized_synthetic_user";
  oversized.data_json = "{\"text\":\"bad\",\"provenance\":{\"source\":\"synthetic_subagent_delivery\",\"delivery_id\":\"" +
                        std::string(ava::session::kMaxSyntheticDeliveryProvenanceIdBytes + 1, 'x') + "\",\"prompt_fingerprint\":\"0123456789abcdef\"}}";
  auto oversized_replay = ava::session::validate_session_replay({oversized});
  expect(!malformed_replay.ok() && !oversized_replay.ok(), "session-v4 parsing rejects unknown and unbounded synthetic provenance fields");
}

void test_image_attachment_storage_boundary()
{
  auto const root = create_empty_root("image-attachment-storage");

  auto const workspace = root / "workspace";
  std::filesystem::create_directories(workspace);
  auto store = ava::session::SessionStore::create(workspace, root / "sessions");
  expect(store.has_value(), "session store opens for image attachment storage test");
  if (!store)
    return;

  auto const attachment_root = ava::session::attachment_storage_root(*store);
  auto const attachment_path = attachment_root / "attachments" / "img_1.txt";
  std::filesystem::create_directories(attachment_path.parent_path());
  {
    std::ofstream file(attachment_path, std::ios::binary);
    file << "hello";
  }

  ava::session::ImageAttachmentRef attachment{.id = "img_1",
                                              .mime_type = "image/png",
                                              .storage_path = "attachments/img_1.txt",
                                              .sha256 = "2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824",
                                              .byte_size = 5};
  auto loaded = ava::session::load_image_attachment(*store, attachment);
  expect(loaded && loaded->bytes == "hello" && loaded->path == attachment_path.lexically_normal(),
         "image attachment storage loads only verified session-owned bytes");

  auto escaped = ava::session::resolve_attachment_storage_path(*store, "attachments/../secrets.txt");
  expect(!escaped, "image attachment storage rejects escaping relative paths");

  auto wrong_size = attachment;
  wrong_size.byte_size = 4;
  auto const size_result = ava::session::load_image_attachment(*store, wrong_size);
  expect_image_error(size_result, ava::core::ErrorCategory::InvalidArgument, "storage_path", attachment.storage_path,
                     "image attachment storage verifies file size before returning bytes");

  auto wrong_hash = attachment;
  wrong_hash.sha256 = "0000000000000000000000000000000000000000000000000000000000000000";
  auto const hash_result = ava::session::load_image_attachment(*store, wrong_hash);
  expect_image_error(hash_result, ava::core::ErrorCategory::InvalidArgument, "storage_path", attachment.storage_path,
                     "image attachment storage verifies sha256 before returning bytes");

  auto const symlink_path = attachment_root / "attachments" / "link.txt";
  std::error_code symlink_error;
  std::filesystem::create_symlink(attachment_path, symlink_path, symlink_error);
  if (ava::tests::require(!symlink_error, "attachment storage fixture creates its final-component symlink: " + symlink_error.message()))
  {
    auto symlink_attachment = attachment;
    symlink_attachment.storage_path = "attachments/link.txt";
    auto const symlink_result = ava::session::load_image_attachment(*store, symlink_attachment);
    expect_image_error(symlink_result, ava::core::ErrorCategory::PermissionDenied, "storage_path", symlink_attachment.storage_path,
                       "image attachment storage rejects symlink attachment paths");
  }

  auto const outside_dir = root / "outside";
  std::filesystem::create_directories(outside_dir);
  auto const outside_file = outside_dir / "secret.txt";
  {
    std::ofstream file(outside_file, std::ios::binary);
    file << "hello";
  }
  auto const symlink_directory = attachment_root / "attachments" / "linked-dir";
  symlink_error.clear();
  std::filesystem::create_directory_symlink(outside_dir, symlink_directory, symlink_error);
  if (ava::tests::require(!symlink_error, "attachment storage fixture creates its intermediate symlink: " + symlink_error.message()))
  {
    auto intermediate_symlink = attachment;
    intermediate_symlink.storage_path = "attachments/linked-dir/secret.txt";
    auto const intermediate_result = ava::session::load_image_attachment(*store, intermediate_symlink);
    expect_image_error(intermediate_result, ava::core::ErrorCategory::PermissionDenied, "storage_path", intermediate_symlink.storage_path,
                       "image attachment storage rejects symlinked attachment directories");
  }

  auto symlink_root_store = ava::session::SessionStore::create(workspace, root / "symlink-root-sessions");
  expect(symlink_root_store.has_value(), "session store opens for symlinked attachment root test");
  if (symlink_root_store)
  {
    auto const symlink_root = ava::session::attachment_storage_root(*symlink_root_store);
    std::filesystem::create_directories(symlink_root.parent_path());
    symlink_error.clear();
    std::filesystem::create_directory_symlink(outside_dir, symlink_root, symlink_error);
    if (ava::tests::require(!symlink_error, "attachment storage fixture creates its root symlink: " + symlink_error.message()))
    {
      auto const symlink_root_result = ava::session::load_image_attachment(*symlink_root_store, attachment);
      expect_image_error(symlink_root_result, ava::core::ErrorCategory::PermissionDenied, "storage_path", attachment.storage_path,
                         "image attachment storage rejects symlinked attachment roots");
    }
  }

  test_attachment_replacement_before_open(attachment_path, [&] { return ava::session::load_image_attachment(*store, attachment); });
  for (auto const& component : {attachment_path, attachment_path.parent_path(), attachment_root, attachment_root.parent_path()})
  {
    auto const canary = outside_dir / attachment_path.filename();
    ava::tests::write_test_file(canary, "hello", 0640);
    test_attachment_replacement_before_open(attachment_path, [&] { return ava::session::load_image_attachment(*store, attachment); },
                                            {.canary = canary, .component = component, .directory = component != attachment_path});
  }
}

void test_image_attachment_import()
{
  auto const root = create_empty_root("image-attachment-import");

  auto const workspace = root / "workspace";
  std::filesystem::create_directories(workspace);
  auto store = ava::session::SessionStore::create(workspace, root / "sessions");
  expect(store.has_value(), "session store opens for image attachment import test");
  if (!store)
    return;

  auto const image_path = root / "input" / "input.png";
  auto const bytes = ava::tests::session_test_tiny_png_bytes();
  ava::tests::write_session_test_binary_file(image_path, bytes);

  auto imported = ava::session::import_image_attachment(*store, image_path);
  expect(imported && imported->id.starts_with("img_") && imported->mime_type == "image/png" && imported->storage_path.starts_with("attachments/") &&
             imported->byte_size == bytes.size(),
         "image attachment import stores byte-sniffed PNG metadata under the session attachment namespace");
  if (!imported)
    return;
  auto loaded = ava::session::load_image_attachment(*store, *imported);
  expect(loaded && loaded->bytes == bytes, "imported image attachment reloads only after size and sha verification");

  auto const unsupported_path = root / "not-image.txt";
  ava::tests::write_session_test_binary_file(unsupported_path, "hello");
  auto unsupported = ava::session::import_image_attachment(*store, unsupported_path);
  expect_image_error(unsupported, ava::core::ErrorCategory::InvalidArgument, "path", unsupported_path.string(),
                     "image attachment import rejects unsupported byte signatures");

  auto const outside = root / "outside.png";
  ava::tests::write_session_test_binary_file(outside, bytes);
  auto const link = root / "linked.png";
  std::error_code symlink_error;
  std::filesystem::create_symlink(outside, link, symlink_error);
  if (ava::tests::require(!symlink_error, "attachment import fixture creates its source symlink: " + symlink_error.message()))
  {
    auto symlink_import = ava::session::import_image_attachment(*store, link);
    expect_image_error(symlink_import, ava::core::ErrorCategory::PermissionDenied, "path", link.string(),
                       "image attachment import rejects symlink source paths");
  }

  for (auto const byte_size : {std::size_t{0}, ava::session::kMaxImageAttachmentBytes, ava::session::kMaxImageAttachmentBytes + 1})
  {
    auto const boundary_path = root / ("boundary-" + std::to_string(byte_size) + ".png");
    // A sparse file exercises the actual stat/read boundary without writing a
    // second full-size fixture. The accepted case has a real PNG signature.
    ava::tests::write_test_file(boundary_path, byte_size == 0 ? std::string_view{} : std::string_view(bytes));
    ava::core::UniqueFd descriptor(::open(boundary_path.c_str(), O_WRONLY | O_CLOEXEC));
    if (!ava::tests::require(descriptor && ::ftruncate(descriptor.get(), static_cast<off_t>(byte_size)) == 0,
                             "attachment import sizes its sparse boundary fixture"))
      return;
    descriptor.reset();
    auto result = ava::session::import_image_attachment(*store, boundary_path);
    if (byte_size == ava::session::kMaxImageAttachmentBytes)
    {
      if (!ava::tests::require(result.has_value(), "file import accepts the exact attachment byte limit"))
        return;
      ava::tests::expect_eq(result->byte_size, byte_size, "exact-limit file import records its full size");
      auto reloaded = ava::session::load_image_attachment(*store, *result);
      ava::tests::expect_all("exact-limit file import remains readable with matching content", {{"load", reloaded.has_value()},
                                                                                                {"size", reloaded && reloaded->bytes.size() == byte_size},
                                                                                                {"signature", reloaded && reloaded->bytes.starts_with(bytes)}});
    }
    else
    {
      if (!ava::tests::require(!result, "file import rejects empty and over-limit byte sizes"))
        return;
      ava::tests::expect_eq(result.error().category(), ava::core::ErrorCategory::InvalidArgument, "file byte-size rejection is a caller error");
      auto const& context = result.error().context();
      expect(std::ranges::any_of(context, [&](auto const& item) { return item.key == "byte_size" && item.value == std::to_string(byte_size); }),
             "file byte-size rejection reports the rejected size");
    }
  }

  auto empty_bytes = ava::session::import_image_attachment_bytes(*store, {});
  std::string oversized_bytes(ava::session::kMaxImageAttachmentBytes + 1, '\0');
  oversized_bytes.replace(0, bytes.size(), bytes);
  auto oversized_result = ava::session::import_image_attachment_bytes(*store, oversized_bytes);
  expect_image_error(empty_bytes, ava::core::ErrorCategory::InvalidArgument, "byte_size", "0",
                     "in-memory image import rejects empty data at the byte-size boundary");
  expect_image_error(oversized_result, ava::core::ErrorCategory::InvalidArgument, "byte_size", std::to_string(oversized_bytes.size()),
                     "in-memory image import rejects valid image bytes one above the size limit");
  oversized_bytes.resize(ava::session::kMaxImageAttachmentBytes);
  auto exact_bytes = ava::session::import_image_attachment_bytes(*store, oversized_bytes, "image/png");
  ava::tests::expect_all("in-memory image import accepts the exact byte limit",
                         {{"import", exact_bytes.has_value()}, {"metadata size", exact_bytes && exact_bytes->byte_size == oversized_bytes.size()}});
  auto mismatched_mime = ava::session::import_image_attachment_bytes(*store, bytes, "image/jpeg");
  if (ava::tests::require(!mismatched_mime, "in-memory image import rejects a MIME type that disagrees with the signature"))
  {
    ava::tests::expect_eq(mismatched_mime.error().category(), ava::core::ErrorCategory::InvalidArgument, "MIME mismatch is a caller error");
    auto const& context = mismatched_mime.error().context();
    ava::tests::expect_all(
        "MIME mismatch identifies both MIME types",
        {{"expected", std::ranges::any_of(context, [](auto const& item) { return item.key == "expected_mime_type" && item.value == "image/jpeg"; })},
         {"detected", std::ranges::any_of(context, [](auto const& item) { return item.key == "detected_mime_type" && item.value == "image/png"; })}});
  }
  for (auto const& [signature, mime] : std::array<std::pair<std::string, std::string_view>, 4>{
           std::pair{std::string("\xff\xd8\xff", 3), std::string_view("image/jpeg")}, std::pair{std::string("GIF87a"), std::string_view("image/gif")},
           std::pair{std::string("GIF89a"), std::string_view("image/gif")},
           std::pair{std::string("RIFF\x01\x00\x00\x00WEBP", 12), std::string_view("image/webp")}})
  {
    auto result = ava::session::import_image_attachment_bytes(*store, signature, mime);
    if (!ava::tests::require(result.has_value(), "byte-signature import accepts " + std::string(mime)))
      return;
    ava::tests::expect_eq(result->mime_type, std::string(mime), "byte-signature import records its sniffed MIME type");
    auto reloaded = ava::session::load_image_attachment(*store, *result);
    if (ava::tests::require(reloaded.has_value(), "byte-signature import reloads its stored bytes"))
      ava::tests::expect_eq(reloaded->bytes, signature, "byte-signature import preserves exactly the supplied bytes");
  }

  test_attachment_replacement_before_open(image_path, [&] { return ava::session::import_image_attachment(*store, image_path); });

  auto const imported_path = *ava::session::resolve_attachment_storage_path(*store, imported->storage_path);
  auto const canary_dir = root / "outside-canaries";
  auto const source_canary = canary_dir / image_path.filename();
  auto const destination_canary = canary_dir / imported_path.filename();
  ava::tests::write_test_file(source_canary, bytes, 0640);
  ava::tests::write_test_file(destination_canary, bytes, 0640);
  for (bool const directory : {false, true})
  {
    test_attachment_replacement_before_open(image_path, [&] { return ava::session::import_image_attachment(*store, image_path); },
                                            {.canary = source_canary, .directory = directory});
    test_attachment_replacement_before_open(imported_path, [&] { return ava::session::import_image_attachment_bytes(*store, bytes); },
                                            {.canary = destination_canary, .directory = directory, .write = true});
  }

  auto const user = append_session_entry_for_test(
      *store, ava::session::SessionEntry{.id = "attachment_user",
                                         .parent_id = "",
                                         .type = ava::session::EntryType::UserMessage,
                                         .timestamp = "2026-10-24T00:00:00Z",
                                         .data_json = "{\"text\":\"image\",\"attachments\":[{\"id\":\"" + imported->id +
                                                      "\",\"type\":\"image\",\"mime_type\":\"image/png\",\"byte_size\":" + std::to_string(imported->byte_size) +
                                                      ",\"sha256\":\"" + imported->sha256 + "\",\"storage_path\":\"" + imported->storage_path + "\"}]}"});
  if (!ava::tests::require(user.has_value(), "branch replacement fixture persists attachment metadata"))
    return;
  for (auto const mode : {ava::session::SessionBranchMode::Fork, ava::session::SessionBranchMode::Clone})
  {
    for (bool const directory : {false, true})
    {
      auto branch = [&] {
        return ava::session::create_session_branch(ava::session::SessionBranchOptions{.workspace_dir = workspace,
                                                                                      .root_dir = root / "sessions",
                                                                                      .source_session_id = store->options_session_id_copy(),
                                                                                      .branch_from_entry_id = "",
                                                                                      .name = std::nullopt,
                                                                                      .labels = std::nullopt,
                                                                                      .mode = mode,
                                                                                      .actor = "test"});
      };
      test_attachment_replacement_before_open(imported_path, branch, {.canary = destination_canary, .directory = directory});
      test_attachment_replacement_before_open(imported_path, branch, {.canary = destination_canary, .directory = directory, .write = true});
    }
  }
}

void test_created_session_rollback_is_identity_safe_and_preserves_attachments()
{
  auto const root = create_empty_root("created-session-rollback");

  auto const workspace = root / "workspace";
  auto const sessions_dir = root / "sessions";
  std::filesystem::create_directories(workspace);

  auto create_owned = [&]() -> std::pair<ava::session::SessionStore, ava::session::SessionLease> {
    auto store = ava::session::SessionStore::create(workspace, sessions_dir);
    expect(store.has_value(), "created-session rollback test creates a store");
    if (!store)
      return {ava::session::SessionStore(ava::session::SessionStoreOptions{}), ava::session::SessionLease{}};
    auto lease = ava::session::SessionLease::create_and_acquire(store->session_path());
    expect(lease.has_value(), "created-session rollback test acquires the creating lease");
    if (!lease)
      return {std::move(*store), ava::session::SessionLease{}};
    return {std::move(*store), std::move(*lease)};
  };

  {
    auto [store, lease] = create_owned();
    if (!lease.canonical_path().empty())
    {
      auto const path = store.session_path();
      auto removed = store.remove_created_file(lease);
      expect(removed && !std::filesystem::exists(path) && !created_session_rollback_quarantine(path),
             "created-session rollback removes exactly the creating JSONL without leaving a quarantine sibling");
    }
  }

  {
    auto [store, lease] = create_owned();
    if (!lease.canonical_path().empty())
    {
      auto const path = store.session_path();
      auto const parked_original = root / "parked-original.jsonl";
      store.set_before_created_file_rollback_detach_for_test([&] {
        std::filesystem::rename(path, parked_original);
        ava::tests::write_session_test_binary_file(path, "replacement-session");
      });
      auto removed = store.remove_created_file(lease);
      expect(!removed && ava::tests::read_session_test_binary_file(path) == "replacement-session" && std::filesystem::exists(parked_original),
             "created-session rollback fails closed and preserves a replaced session basename");
    }
  }

  {
    auto [store, lease] = create_owned();
    if (!lease.canonical_path().empty())
    {
      auto const path = store.session_path();
      auto const parked_original = root / "fifo-parked-original.jsonl";
      store.set_before_created_file_rollback_detach_for_test([&] {
        std::filesystem::rename(path, parked_original);
        expect(::mkfifo(path.c_str(), 0600) == 0, "created-session rollback test installs a FIFO replacement");
      });
      auto removed = store.remove_created_file(lease);
      std::error_code fifo_status_error;
      auto const fifo_status = std::filesystem::symlink_status(path, fifo_status_error);
      expect(!removed && !fifo_status_error && std::filesystem::is_fifo(fifo_status) && std::filesystem::exists(parked_original) &&
                 !created_session_rollback_quarantine(path),
             "created-session rollback inspects a FIFO replacement without blocking and restores it without deletion");
    }
  }

  {
    auto [store, lease] = create_owned();
    if (!lease.canonical_path().empty())
    {
      auto const path = store.session_path();
      auto const original_parent = path.parent_path();
      auto const moved_parent = root / "moved-session-parent";
      store.set_after_created_file_rollback_detach_for_test([&] {
        std::filesystem::rename(original_parent, moved_parent);
        std::filesystem::create_directories(original_parent);
        ava::tests::write_session_test_binary_file(path, "parent-replacement-session");
      });
      auto removed = store.remove_created_file(lease);
      expect(removed && ava::tests::read_session_test_binary_file(path) == "parent-replacement-session" &&
                 !created_session_rollback_quarantine(moved_parent / path.filename()),
             "descriptor-anchored rollback cannot redirect deletion into a replacement parent directory");
    }
  }

  {
    auto [store, lease] = create_owned();
    if (!lease.canonical_path().empty())
    {
      auto const path = store.session_path();
      store.set_after_created_file_rollback_detach_for_test([&] { ava::tests::write_session_test_binary_file(path, "republished-session"); });
      auto removed = store.remove_created_file(lease);
      expect(!removed && ava::tests::read_session_test_binary_file(path) == "republished-session" && created_session_rollback_quarantine(path) &&
                 removed.error().format().find("created session name was republished") != std::string::npos &&
                 removed.error().format().find("quarantine_path:") != std::string::npos,
             "rollback preserves and reports its exact quarantine when the original name is republished after detach");
    }
  }

  {
    auto [store, lease] = create_owned();
    if (!lease.canonical_path().empty())
    {
      auto const path = store.session_path();
      auto const parked_original = root / "mismatch-parked-original.jsonl";
      store.set_after_created_file_rollback_detach_for_test([&] {
        auto quarantine = created_session_rollback_quarantine(path);
        if (!quarantine)
          return;
        std::filesystem::rename(*quarantine, parked_original);
        ava::tests::write_session_test_binary_file(*quarantine, "quarantine-replacement");
      });
      auto removed = store.remove_created_file(lease);
      expect(!removed && std::filesystem::exists(parked_original) && ava::tests::read_session_test_binary_file(path) == "quarantine-replacement" &&
                 !created_session_rollback_quarantine(path),
             "rollback restores a detached quarantine mismatch to the original name without deleting either inode");
    }
  }

  {
    auto [store, creating_lease] = create_owned();
    if (!creating_lease.canonical_path().empty())
    {
      auto const path = store.session_path();
      auto no_token = store.remove_created_file(ava::session::SessionLease{});
      creating_lease = ava::session::SessionLease{};
      auto noncreating_lease = ava::session::SessionLease::acquire(path);
      auto noncreating =
          noncreating_lease ? store.remove_created_file(*noncreating_lease) : ava::core::VoidResult(std::unexpected(std::move(noncreating_lease.error())));
      expect(!no_token && !noncreating && std::filesystem::exists(path),
             "created-session rollback rejects missing and non-creating leases without deleting the session");
    }
  }

  {
    auto [store, lease] = create_owned();
    if (!lease.canonical_path().empty())
    {
      auto const attachment_file = ava::session::attachment_storage_root(store) / "nested" / "attachment.bin";
      ava::tests::write_session_test_binary_file(attachment_file, "retained attachment bytes");
      ava::core::Error primary(ava::core::ErrorCategory::Unknown, "runtime construction failed");
      ava::session::rollback_created_session_with_context(store, lease, primary);
      auto const formatted = primary.format();
      expect(!std::filesystem::exists(store.session_path()) && ava::tests::read_session_test_binary_file(attachment_file) == "retained attachment bytes" &&
                 primary.message() == "runtime construction failed" && formatted.find("created_session_id: " + store.options_session_id_copy()) != std::string::npos &&
                 formatted.find("rollback_attachment_path: " + ava::session::attachment_storage_root(store).string()) != std::string::npos &&
                 formatted.find("rollback_attachment_disposition: preserved") != std::string::npos,
             "session rollback preserves an attachment subtree byte-for-byte and reports its retained path without replacing the primary error");
    }
  }
}

}  // namespace session_tests

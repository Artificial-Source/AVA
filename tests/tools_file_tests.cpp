#include "sys.h"
#include "tests/support/test_harness.h"
#include "ava/agent/mode.h"
#include "ava/tools/bash_tool.h"
#include "ava/tools/file_tools.h"
#include "ava/tools/mutation_queue.h"
#include "ava/tools/secure_workspace.h"
#include "ava/tools/tool_permission.h"
#include "ava/session/export.h"
#include "ava/session/session_store.h"
#include "ava/permissions/permission.h"
#include "ava/core/AnchorOpen.h"
#include "ava/core/AnchorSet.h"
#include "ava/core/json.h"
#include "ava/core/open_beneath.h"
#include "ava/core/path.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include "debug.h"

namespace {

std::string read_text_file_for_test(std::filesystem::path const& path)
{
  std::ifstream file(path, std::ios::binary);
  std::ostringstream out;
  out << file.rdbuf();
  return out.str();
}

bool has_secure_write_temp_for(std::filesystem::path const& directory, std::string_view target_filename = {})
{
  auto const prefix = target_filename.empty() ? std::string{} : "." + std::string(target_filename) + ".ava-write-";
  std::error_code iter_error;
  for (std::filesystem::directory_iterator it(directory, iter_error), end; !iter_error && it != end; it.increment(iter_error))
  {
    auto const name = it->path().filename().string();
    if ((prefix.empty() && name.find(".ava-write-") != std::string::npos) || (!prefix.empty() && name.starts_with(prefix)))
      return true;
  }
  return false;
}

bool has_error_context(ava::core::Error const& error, std::string_view key, std::string_view value)
{
  return std::ranges::any_of(error.context(), [key, value](ava::core::ErrorContext const& item) { return item.key == key && item.value == value; });
}

#ifdef __linux__
std::optional<std::vector<int>> proc_open_file_descriptors()
{
  DIR* directory = ::opendir("/proc/self/fd");
  if (directory == nullptr)
    return std::nullopt;
  int const listing_fd = ::dirfd(directory);
  if (listing_fd < 0)
  {
    ::closedir(directory);
    return std::nullopt;
  }

  std::vector<int> descriptors;
  errno = 0;
  while (auto* entry = ::readdir(directory))
  {
    auto const* begin = entry->d_name;
    auto const* end = begin + std::strlen(begin);
    int descriptor = -1;
    auto const parsed = std::from_chars(begin, end, descriptor);
    if (parsed.ec == std::errc{} && parsed.ptr == end && descriptor != listing_fd)
      descriptors.push_back(descriptor);
  }
  int const read_error = errno;
  ::closedir(directory);
  if (read_error != 0)
    return std::nullopt;

  std::ranges::sort(descriptors);
  return descriptors;
}

std::optional<std::vector<int>> added_file_descriptors(std::vector<int> const& before)
{
  auto after = proc_open_file_descriptors();
  if (!after)
    return std::nullopt;
  std::vector<int> added;
  std::ranges::set_difference(*after, before, std::back_inserter(added));
  return added;
}
#endif

class MemoryExactFileAccess final : public ava::tools::ExactFileAccess
{
 public:
  [[nodiscard]] bool supports_read_text_file() const noexcept override { return supports_reads; }
  [[nodiscard]] bool supports_write_text_file() const noexcept override { return supports_writes; }

  [[nodiscard]] ava::core::Result<std::string> read_text_file(std::filesystem::path const& path, ava::tools::ToolIoCancelCallback) const override
  {
    ++read_calls;
    paths.push_back(path);
    if (fail_reads)
      return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Io, "injected exact read failed"));
    auto found = files.find(path);
    if (found == files.end())
      return std::unexpected(ava::core::Error(ava::core::ErrorCategory::NotFound, "injected exact file is missing"));
    return found->second;
  }

  [[nodiscard]] ava::core::VoidResult write_text_file(std::filesystem::path const& path, std::string_view content,
                                                      ava::tools::ToolIoCancelCallback) const override
  {
    ++write_calls;
    paths.push_back(path);
    if (fail_writes)
      return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Io, "injected exact write failed"));
    files[path] = std::string(content);
    return {};
  }

  mutable std::map<std::filesystem::path, std::string> files;
  mutable std::vector<std::filesystem::path> paths;
  mutable int read_calls = 0;
  mutable int write_calls = 0;
  bool supports_reads = true;
  bool supports_writes = true;
  bool fail_reads = false;
  bool fail_writes = false;
};

void test_mutation_queue_cleans_drained_path_entries()
{
  auto const root = create_empty_root("test_mutation_queue_cleans_drained_path_entries");

  auto const workspace = root / "workspace";
  std::filesystem::create_directories(workspace);

  ava::tools::MutationQueue queue;
  expect(queue.tracked_path_count() == 0, "mutation queue starts without tracked path entries");

  auto const single_path = workspace / "single.txt";
  {
    [[maybe_unused]] auto lock = queue.lock_path(single_path);
    expect(queue.tracked_path_count() == 1, "mutation queue tracks a locked path entry");
  }
  expect(queue.tracked_path_count() == 0, "mutation queue removes a path entry after queued work drains");

  std::array const paths{workspace / "multi-a.txt", workspace / "." / "multi-a.txt", workspace / "multi-b.txt"};
  {
    [[maybe_unused]] auto lock = queue.lock_paths(paths);
    expect(queue.tracked_path_count() == 2, "mutation queue deduplicates normalized aliases while locked");
  }
  expect(queue.tracked_path_count() == 0, "mutation queue removes deduped multi-path entries after queued work drains");
}

void test_file_tools()
{
  auto const root = create_empty_root("test_file_tools");

  auto const workspace = root / "workspace";
  std::filesystem::create_directories(workspace / "docs");
  auto const source_path = workspace / "src.txt";
  ava::tools::ToolContext const build_context{.workspace_dir = workspace, .mode = ava::agent::Mode::Build};
  ava::tools::ToolContext const plan_context{.workspace_dir = workspace, .mode = ava::agent::Mode::Plan};
  auto const has_write_temp = [](std::filesystem::path const& directory) {
    std::error_code iter_error;
    for (std::filesystem::directory_iterator it(directory, iter_error), end; !iter_error && it != end; it.increment(iter_error))
    {
      if (it->path().filename().string().find(".ava-write-") != std::string::npos)
        return true;
    }
    return false;
  };
  auto const permission_bits = [](std::filesystem::path const& permission_path) {
    constexpr auto mask = std::filesystem::perms::owner_all | std::filesystem::perms::group_all | std::filesystem::perms::others_all;
    std::error_code status_error;
    return std::filesystem::status(permission_path, status_error).permissions() & mask;
  };

  auto write = ava::tools::write_file(build_context, source_path, "hello world");
  expect(write.has_value(), "write_file writes in build mode");
  expect(write && permission_bits(source_path) == (std::filesystem::perms::owner_read | std::filesystem::perms::owner_write),
         "write_file creates new files with 0600 permissions");

  auto read = ava::tools::read_file(build_context, source_path, ava::tools::ReadOptions{.max_bytes = 5});
  expect(read.has_value(), "read_file reads content");
  if (read)
  {
    expect(read->content == "hello", "read_file truncates head");
    expect(read->truncated && read->byte_limited && !read->line_limited && read->totals_known && read->total_bytes == 11 && read->total_lines == 1 &&
               read->output_lines == 1,
           "local read_file reports byte-cap truncation with exact full-file totals");
  }
  auto const ranged_path = workspace / "range.txt";
  {
    std::ofstream ranged_file(ranged_path, std::ios::binary | std::ios::trunc);
    ranged_file << "one\n"
                   "two\n"
                   "three\n"
                   "four\n";
  }
  auto ranged = ava::tools::read_file(build_context, ranged_path, ava::tools::ReadOptions{.max_bytes = 1024, .offset_line = 2, .max_lines = 2});
  expect(ranged && ranged->content == "two\nthree\n" && ranged->start_line == 2 && ranged->end_line == 3 && ranged->output_lines == 2 && ranged->totals_known &&
             ranged->total_bytes == 19 && ranged->total_lines == 4 && ranged->line_limited && !ranged->byte_limited && ranged->next_offset_line == 4,
         "local read_file supports line offset and continuation metadata with exact full-file totals");

  auto edit = ava::tools::edit_file(build_context, source_path, "world", "ava");
  expect(edit.has_value(), "edit_file edits unique text");

  auto edited = ava::tools::read_file(build_context, source_path);
  expect(edited && edited->content == "hello ava", "edit_file result is persisted");

  auto const overlapping_path = workspace / "overlapping.txt";
  {
    std::ofstream overlapping_file(overlapping_path, std::ios::binary | std::ios::trunc);
    overlapping_file << "aaa";
  }
  auto overlapping_edit = ava::tools::edit_file(build_context, overlapping_path, "aa", "b");
  auto overlapping_read = ava::tools::read_file(build_context, overlapping_path);
  expect(
      !overlapping_edit && overlapping_read && overlapping_read->content == "aaa" && overlapping_edit.error().format().find("not unique") != std::string::npos,
      "edit_file rejects overlapping old_text matches as ambiguous");

  auto const atomic_path = workspace / "atomic.txt";
  {
    std::ofstream atomic_file(atomic_path, std::ios::binary | std::ios::trunc);
    atomic_file << "original content";
  }
  auto atomic_write = ava::tools::write_file(build_context, atomic_path, "replacement content");
  auto atomic_read = ava::tools::read_file(build_context, atomic_path);
  expect(atomic_write && atomic_read && atomic_read->content == "replacement content" && !has_write_temp(workspace),
         "write_file atomically replaces existing content and cleans the staging file on success");

  auto const protected_path = workspace / "protected.txt";
  {
    std::ofstream protected_file(protected_path, std::ios::binary | std::ios::trunc);
    protected_file << "private original";
  }
  std::error_code chmod_error;
  std::filesystem::permissions(protected_path, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write, std::filesystem::perm_options::replace,
                               chmod_error);
  expect(!chmod_error, "test can set private file permissions");
  auto protected_write = ava::tools::write_file(build_context, protected_path, "private replacement");
  auto protected_read = ava::tools::read_file(build_context, protected_path);
  expect(protected_write && protected_read && protected_read->content == "private replacement" &&
             permission_bits(protected_path) == (std::filesystem::perms::owner_read | std::filesystem::perms::owner_write),
         "write_file preserves 0600 permissions when overwriting a file");

  auto const edit_private_path = workspace / "edit-private.txt";
  {
    std::ofstream edit_private_file(edit_private_path, std::ios::binary | std::ios::trunc);
    edit_private_file << "alpha beta";
  }
  chmod_error.clear();
  std::filesystem::permissions(edit_private_path, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
                               std::filesystem::perm_options::replace, chmod_error);
  expect(!chmod_error, "test can set private edit file permissions");
  auto edit_private = ava::tools::edit_file(build_context, edit_private_path, "beta", "gamma");
  auto edit_private_read = ava::tools::read_file(build_context, edit_private_path);
  expect(edit_private && edit_private_read && edit_private_read->content == "alpha gamma" &&
             permission_bits(edit_private_path) == (std::filesystem::perms::owner_read | std::filesystem::perms::owner_write),
         "edit_file preserves 0600 permissions through atomic write_file");

  auto const crlf_path = workspace / "crlf.txt";
  {
    std::ofstream crlf_file(crlf_path, std::ios::binary | std::ios::trunc);
    crlf_file << "alpha\r\nbeta\r\n";
  }
  auto crlf_lf_only_edit = ava::tools::edit_file(build_context, crlf_path, "alpha\nbeta\n", "gamma\n");
  expect(!crlf_lf_only_edit && crlf_lf_only_edit.error().format().find("CRLF") != std::string::npos, "edit_file explains CRLF-sensitive exact match failures");
  auto crlf_exact_edit = ava::tools::edit_file(build_context, crlf_path, "alpha\r\nbeta\r\n", "gamma\r\ndelta\r\n");
  expect(crlf_exact_edit && read_text_file_for_test(crlf_path) == "gamma\r\ndelta\r\n" && crlf_exact_edit->line_endings == "CRLF",
         "edit_file preserves CRLF bytes when the provider supplies an exact CRLF match");

  auto const bom_path = workspace / "bom.txt";
  {
    std::ofstream bom_file(bom_path, std::ios::binary | std::ios::trunc);
    bom_file << "\xEF\xBB\xBF"
                "alpha\n";
  }
  auto bom_edit = ava::tools::edit_file(build_context, bom_path, "alpha\n", "beta\n");
  expect(bom_edit && bom_edit->had_utf8_bom &&
             read_text_file_for_test(bom_path) ==
                 "\xEF\xBB\xBF"
                 "beta\n",
         "edit_file preserves UTF-8 BOM bytes across exact replacements");

  auto const shared_queue = std::make_shared<ava::tools::MutationQueue>();
  ava::tools::ToolContext const queued_context{.workspace_dir = workspace, .mode = ava::agent::Mode::Build, .mutation_queue = shared_queue};
  auto const queued_path = workspace / "queued-edit.txt";
  auto queued_write = ava::tools::write_file(queued_context, queued_path, "one two");
  auto queued_edit = ava::tools::edit_file(queued_context, queued_path, "two", "three");
  auto queued_read = ava::tools::read_file(queued_context, queued_path);
  expect(queued_write && queued_edit && queued_read && queued_read->content == "one three",
         "write_file and edit_file can share a mutation queue without nested edit deadlock");

  auto const audit_edit_path = workspace / "audit-edit.txt";
  {
    std::ofstream audit_edit_file(audit_edit_path, std::ios::binary | std::ios::trunc);
    audit_edit_file << "read then edit";
  }
  std::vector<ava::tools::PermissionAuditEvent> edit_audits;
  ava::tools::ToolContext const audit_edit_context{
      .workspace_dir = workspace,
      .mode = ava::agent::Mode::Build,
      .permission_audit_sink = [&edit_audits](ava::tools::PermissionAuditEvent const& event) -> ava::core::VoidResult {
        edit_audits.push_back(event);
        return {};
      }};
  auto audited_edit = ava::tools::edit_file(audit_edit_context, audit_edit_path, "then", "before");
  expect(audited_edit && edit_audits.size() == 2 && edit_audits[0].operation == ava::permissions::Operation::ReadFile &&
             edit_audits[1].operation == ava::permissions::Operation::EditFile,
         "edit_file audits read permission before edit permission");

  auto const audit_write_path = workspace / "audit-write.txt";
  std::vector<ava::tools::PermissionAuditEvent> write_audits;
  ava::tools::ToolContext const audit_write_context{
      .workspace_dir = workspace,
      .mode = ava::agent::Mode::Build,
      .permission_resolver = [](ava::permissions::PermissionPrompt const& prompt) -> ava::core::Result<ava::permissions::PermissionResolutionDecision> {
        expect(prompt.tool_name == "write_file", "write_file permission prompt carries write_file tool metadata");
        return ava::permissions::PermissionResolution::Allow;
      },
      .permission_audit_sink = [&write_audits](ava::tools::PermissionAuditEvent const& event) -> ava::core::VoidResult {
        write_audits.push_back(event);
        return {};
      },
      .permission_tool_name = "write_file",
      .current_tool_name = "write_file"};
  auto audited_write = ava::tools::write_file(audit_write_context, audit_write_path, "written");
  expect(audited_write && std::ranges::any_of(write_audits,
                                              [](ava::tools::PermissionAuditEvent const& event) {
                                                return event.operation == ava::permissions::Operation::EditFile && event.tool_name == "write_file";
                                              }),
         "write_file audits its file-mutation permission with write_file tool metadata");

  auto const rename_failure_path = workspace / "rename-failure-target";
  std::filesystem::create_directories(rename_failure_path);
  {
    std::ofstream sentinel(rename_failure_path / "sentinel.txt", std::ios::binary | std::ios::trunc);
    sentinel << "original sentinel";
  }
  auto failed_write = ava::tools::write_file(build_context, rename_failure_path, "replacement");
  auto sentinel_read = ava::tools::read_file(build_context, rename_failure_path / "sentinel.txt");
  expect(!failed_write && sentinel_read && sentinel_read->content == "original sentinel" && !has_write_temp(workspace),
         "write_file cleans the staging file and leaves existing data unchanged when commit rename fails");

  auto denied = ava::tools::write_file(plan_context, workspace / "main.cpp", "int main() {}\n");
  expect(!denied, "plan mode source write is denied");

  auto denied_edit = ava::tools::edit_file(build_context, workspace / ".env", "secret", "other");
  expect(!denied_edit, "edit_file checks read permission before reading");

  auto plan_denied_edit = ava::tools::edit_file(plan_context, workspace / "missing.cpp", "old", "new");
  expect(!plan_denied_edit && plan_denied_edit.error().category() == ava::core::ErrorCategory::PermissionDenied,
         "edit_file checks edit permission before reading target content");

  {
    std::ofstream secret_file(workspace / ".env", std::ios::binary | std::ios::trunc);
    secret_file << "secret";
  }
  auto direct_secret = ava::tools::read_file(build_context, workspace / ".env");
  expect(!direct_secret, "read_file denies direct .env reads");
  auto external_ssh_secret = ava::permissions::decide(ava::permissions::PermissionRequest{
      .operation = ava::permissions::Operation::ReadFile,
      .mode = ava::agent::Mode::Build,
      .workspace_dir = workspace,
      .target_path = "/home/user/.ssh/id_rsa",
      .command = "",
  });
  expect(external_ssh_secret.action == ava::permissions::PermissionAction::Deny, "external ssh key reads deny before outside-workspace ask");
  auto external_npmrc_secret = ava::permissions::decide(ava::permissions::PermissionRequest{
      .operation = ava::permissions::Operation::EditFile,
      .mode = ava::agent::Mode::Build,
      .workspace_dir = workspace,
      .target_path = "/home/user/.npmrc",
      .command = "",
  });
  expect(external_npmrc_secret.action == ava::permissions::PermissionAction::Deny, "external npm credential edits deny before outside-workspace ask");
  auto external_ava_auth = ava::permissions::decide(ava::permissions::PermissionRequest{
      .operation = ava::permissions::Operation::ReadFile,
      .mode = ava::agent::Mode::Build,
      .workspace_dir = workspace,
      .target_path = "/home/user/.config/ava/auth.json",
      .command = "",
  });
  expect(external_ava_auth.action == ava::permissions::PermissionAction::Deny, "external ava auth reads deny before outside-workspace ask");
  std::error_code symlink_error;
  auto const secret_link = workspace / "safe.txt";
  std::filesystem::create_symlink(workspace / ".env", secret_link, symlink_error);
  if (!symlink_error)
  {
    auto linked_secret = ava::tools::read_file(build_context, secret_link);
    expect(!linked_secret, "read_file denies symlink to secret file");
  }

  auto const source_link = workspace / "docs" / "plan-link.md";
  symlink_error.clear();
  std::filesystem::create_symlink(source_path, source_link, symlink_error);
  if (!symlink_error)
  {
    auto linked_source_read = ava::tools::read_file(build_context, source_link);
    expect(!linked_source_read && linked_source_read.error().message().find("symlink") != std::string::npos,
           "read_file rejects symlinks before opening file contents");
    auto linked_source_edit = ava::tools::write_file(plan_context, source_link, "bad");
    expect(!linked_source_edit, "plan mode denies symlink edit to source file");
  }

  auto plan_doc = ava::tools::write_file(plan_context, workspace / "docs" / "plan.md", "# Plan\n");
  expect(plan_doc.has_value(), "plan mode markdown plan write is allowed");

  auto const diffed_write_path = workspace / "diffed-write.txt";
  {
    std::ofstream file(diffed_write_path, std::ios::binary | std::ios::trunc);
    file << "old line\n";
  }
  auto diffed_write = ava::tools::write_file(build_context, diffed_write_path, "new line\n");
  expect(diffed_write && diffed_write->diff.find("-old line") != std::string::npos && diffed_write->diff.find("+new line") != std::string::npos &&
             !diffed_write->diff_truncated,
         "write_file returns the backend-generated unified diff after successful mutation");

  auto const large_path = workspace / "large.txt";
  {
    std::ofstream large(large_path, std::ios::binary | std::ios::trunc);
    large << std::string(8192, 'x');
  }
  auto large_read = ava::tools::read_file(build_context, large_path, ava::tools::ReadOptions{.max_bytes = 16});
  expect(large_read && large_read->content.size() == 16 && large_read->total_bytes == 8192 && large_read->byte_limited && large_read->output_lines == 1,
         "read_file bounds output while counting bytes");

  auto const oversized_path = workspace / "oversized.txt";
  {
    std::ofstream oversized(oversized_path, std::ios::binary | std::ios::trunc);
    oversized.seekp(10 * 1024 * 1024);
    oversized.put('x');
  }
  auto oversized_read = ava::tools::read_file(build_context, oversized_path, ava::tools::ReadOptions{.max_bytes = 16});
  expect(!oversized_read && oversized_read.error().message().find("too large") != std::string::npos,
         "read_file rejects oversized files instead of scanning unbounded input");

  auto const canceled_existing_path = workspace / "canceled-existing.txt";
  {
    std::ofstream canceled_file(canceled_existing_path, std::ios::binary | std::ios::trunc);
    canceled_file << "keep original";
  }
  auto const canceled_outside_path = root / "canceled-outside.txt";
  {
    std::ofstream canceled_outside(canceled_outside_path, std::ios::binary | std::ios::trunc);
    canceled_outside << "outside original";
  }
  int canceled_file_prompts = 0;
  ava::tools::ToolContext const canceled_file_context{
      .workspace_dir = workspace,
      .mode = ava::agent::Mode::Build,
      .permission_resolver =
          [&canceled_file_prompts](ava::permissions::PermissionPrompt const&) -> ava::core::Result<ava::permissions::PermissionResolutionDecision> {
        ++canceled_file_prompts;
        return ava::permissions::PermissionResolution::Allow;
      },
      .cancel_requested = [] { return true; }};
  auto canceled_read = ava::tools::read_file(canceled_file_context, canceled_outside_path);
  expect(!canceled_read && canceled_read.error().message() == "tool canceled" && canceled_file_prompts == 0,
         "read_file observes cancellation before prompting for permission");
  auto canceled_write = ava::tools::write_file(canceled_file_context, workspace / "canceled-write.txt", "bad");
  expect(!canceled_write && canceled_write.error().message() == "tool canceled" && !std::filesystem::exists(workspace / "canceled-write.txt") &&
             !has_write_temp(workspace),
         "write_file observes cancellation before mutating the filesystem");
  auto canceled_edit = ava::tools::edit_file(canceled_file_context, canceled_existing_path, "original", "changed");
  expect(!canceled_edit && canceled_edit.error().message() == "tool canceled" && read_text_file_for_test(canceled_existing_path) == "keep original",
         "edit_file observes cancellation before mutating the filesystem");

  int write_cancel_checks = 0;
  ava::tools::ToolContext const cancel_during_write_context{
      .workspace_dir = workspace, .mode = ava::agent::Mode::Build, .cancel_requested = [&write_cancel_checks] {
        ++write_cancel_checks;
        return write_cancel_checks >= 8;
      }};
  auto const canceled_large_write_path = workspace / "canceled-large-write.txt";
  auto canceled_large_write = ava::tools::write_file(cancel_during_write_context, canceled_large_write_path, std::string(64 * 1024, 'x'));
  expect(!canceled_large_write && canceled_large_write.error().message() == "tool canceled" && !std::filesystem::exists(canceled_large_write_path) &&
             !has_write_temp(workspace),
         "write_file cleans staged data when cancellation arrives during chunked writes");

  auto const outside_path = root / "outside.txt";
  {
    std::ofstream outside_file(outside_path, std::ios::binary | std::ios::trunc);
    outside_file << "outside content";
  }
  auto outside_without_resolver = ava::tools::read_file(build_context, outside_path);
  expect(!outside_without_resolver && outside_without_resolver.error().format().find("resolution: no_resolver") != std::string::npos,
         "read_file fails closed for ask decisions without a resolver");

  int allow_prompts = 0;
  ava::tools::ToolContext allow_context{.workspace_dir = workspace,
                                        .mode = ava::agent::Mode::Build,
                                        .permission_resolver = [&allow_prompts, &outside_path](ava::permissions::PermissionPrompt const& prompt)
                                            -> ava::core::Result<ava::permissions::PermissionResolutionDecision> {
                                          ++allow_prompts;
                                          expect(prompt.operation == ava::permissions::Operation::ReadFile, "file resolver receives read operation");
                                          expect(prompt.target_path == outside_path, "file resolver receives target path");
                                          return ava::permissions::PermissionResolution::Allow;
                                        }};
  auto outside_allowed = ava::tools::read_file(allow_context, outside_path);
  expect(outside_allowed && outside_allowed->content == "outside content" && allow_prompts == 1, "read_file allows ask decisions when resolver allows once");

  ava::tools::ToolContext deny_context{
      .workspace_dir = workspace,
      .mode = ava::agent::Mode::Build,
      .permission_resolver = [](ava::permissions::PermissionPrompt const&) -> ava::core::Result<ava::permissions::PermissionResolutionDecision> {
        return ava::permissions::PermissionResolutionDecision{ava::permissions::PermissionResolution::Deny, "not approved by test resolver"};
      }};
  auto outside_denied = ava::tools::read_file(deny_context, outside_path);
  expect(!outside_denied && outside_denied.error().format().find("resolution: deny") != std::string::npos &&
             outside_denied.error().format().find("resolution_reason: not approved by test resolver") != std::string::npos,
         "read_file fails closed with resolver denial reasons when resolver denies ask decisions");

  ava::tools::ToolContext failing_context{
      .workspace_dir = workspace,
      .mode = ava::agent::Mode::Build,
      .permission_resolver = [](ava::permissions::PermissionPrompt const&) -> ava::core::Result<ava::permissions::PermissionResolutionDecision> {
        return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Io, "resolver failed"));
      }};
  auto outside_failed = ava::tools::read_file(failing_context, outside_path);
  expect(!outside_failed && outside_failed.error().format().find("resolution: resolver_failed") != std::string::npos,
         "read_file fails closed when resolver fails");

  auto const outside_write_path = root / "outside-write.txt";
  auto outside_write_without_resolver = ava::tools::write_file(build_context, outside_write_path, "bad");
  expect(!outside_write_without_resolver && !std::filesystem::exists(outside_write_path) &&
             outside_write_without_resolver.error().format().find("resolution: no_resolver") != std::string::npos,
         "write_file fails closed for external ask decisions without writing");

  std::vector<ava::permissions::PermissionPrompt> write_denials;
  ava::tools::ToolContext write_deny_context{
      .workspace_dir = workspace,
      .mode = ava::agent::Mode::Build,
      .permission_resolver =
          [&write_denials](ava::permissions::PermissionPrompt const& prompt) -> ava::core::Result<ava::permissions::PermissionResolutionDecision> {
        write_denials.push_back(prompt);
        return ava::permissions::PermissionResolution::Deny;
      }};
  auto outside_write_denied = ava::tools::write_file(write_deny_context, outside_write_path, "bad");
  expect(!outside_write_denied && write_denials.size() == 1 && !std::filesystem::exists(outside_write_path) &&
             outside_write_denied.error().format().find("resolution: deny") != std::string::npos,
         "write_file fails closed when resolver denies external writes");
  if (!write_denials.empty())
  {
    expect(write_denials[0].operation == ava::permissions::Operation::EditFile && write_denials[0].diff_preview.find("+bad") != std::string::npos &&
               !write_denials[0].diff_truncated,
           "write_file includes backend-generated diff preview for denied new-file mutation prompts");
  }

  auto const outside_existing_write_path = root / "outside-existing-write.txt";
  {
    std::ofstream file(outside_existing_write_path, std::ios::binary | std::ios::trunc);
    file << "external secret";
  }
  std::vector<ava::permissions::PermissionPrompt> existing_write_denials;
  ava::tools::ToolContext existing_write_deny_context{
      .workspace_dir = workspace,
      .mode = ava::agent::Mode::Build,
      .permission_resolver =
          [&existing_write_denials](ava::permissions::PermissionPrompt const& prompt) -> ava::core::Result<ava::permissions::PermissionResolutionDecision> {
        existing_write_denials.push_back(prompt);
        return ava::permissions::PermissionResolution::Deny;
      }};
  auto existing_write_denied = ava::tools::write_file(existing_write_deny_context, outside_existing_write_path, "replacement");
  expect(!existing_write_denied && existing_write_denials.size() == 1 && read_text_file_for_test(outside_existing_write_path) == "external secret",
         "write_file fails closed when resolver denies existing external writes");
  if (!existing_write_denials.empty())
  {
    expect(existing_write_denials[0].diff_preview.empty(), "write_file does not leak existing external file content into a diff before read approval");
  }

  int write_fail_prompts = 0;
  ava::tools::ToolContext write_fail_context{
      .workspace_dir = workspace,
      .mode = ava::agent::Mode::Build,
      .permission_resolver =
          [&write_fail_prompts](ava::permissions::PermissionPrompt const&) -> ava::core::Result<ava::permissions::PermissionResolutionDecision> {
        ++write_fail_prompts;
        return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Io, "resolver failed"));
      }};
  auto outside_write_failed = ava::tools::write_file(write_fail_context, outside_write_path, "bad");
  expect(!outside_write_failed && write_fail_prompts == 1 && !std::filesystem::exists(outside_write_path) &&
             outside_write_failed.error().format().find("resolution: resolver_failed") != std::string::npos,
         "write_file fails closed when resolver fails external writes");

  auto outside_edit_without_resolver = ava::tools::edit_file(build_context, outside_path, "outside", "bad");
  auto outside_after_no_resolver_edit = ava::tools::read_file(allow_context, outside_path);
  expect(!outside_edit_without_resolver && outside_after_no_resolver_edit && outside_after_no_resolver_edit->content == "outside content" &&
             outside_edit_without_resolver.error().format().find("resolution: no_resolver") != std::string::npos,
         "edit_file fails closed for external ask decisions without editing");

  int edit_prompts = 0;
  ava::tools::ToolContext edit_deny_context{
      .workspace_dir = workspace,
      .mode = ava::agent::Mode::Build,
      .permission_resolver =
          [&edit_prompts](ava::permissions::PermissionPrompt const& prompt) -> ava::core::Result<ava::permissions::PermissionResolutionDecision> {
        ++edit_prompts;
        expect(prompt.operation == ava::permissions::Operation::ReadFile, "edit_file resolver sees read operation before denied external edit");
        return ava::permissions::PermissionResolution::Deny;
      }};
  auto outside_edit_denied = ava::tools::edit_file(edit_deny_context, outside_path, "outside", "bad");
  auto outside_after_denied_edit = ava::tools::read_file(allow_context, outside_path);
  expect(!outside_edit_denied && edit_prompts == 1 && outside_after_denied_edit && outside_after_denied_edit->content == "outside content" &&
             outside_edit_denied.error().format().find("resolution: deny") != std::string::npos,
         "edit_file leaves content unchanged when external read permission is denied");

  int edit_fail_prompts = 0;
  ava::tools::ToolContext edit_fail_context{
      .workspace_dir = workspace,
      .mode = ava::agent::Mode::Build,
      .permission_resolver =
          [&edit_fail_prompts](ava::permissions::PermissionPrompt const& prompt) -> ava::core::Result<ava::permissions::PermissionResolutionDecision> {
        ++edit_fail_prompts;
        expect(prompt.operation == ava::permissions::Operation::ReadFile, "edit_file failure resolver sees read operation before external edit");
        return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Io, "resolver failed"));
      }};
  auto outside_edit_failed = ava::tools::edit_file(edit_fail_context, outside_path, "outside", "bad");
  auto outside_after_failed_edit = ava::tools::read_file(allow_context, outside_path);
  expect(!outside_edit_failed && edit_fail_prompts == 1 && outside_after_failed_edit && outside_after_failed_edit->content == "outside content" &&
             outside_edit_failed.error().format().find("resolution: resolver_failed") != std::string::npos,
         "edit_file fails closed when resolver fails and leaves content unchanged");

  std::vector<ava::permissions::PermissionPrompt> edit_diff_denials;
  ava::tools::ToolContext edit_diff_deny_context{
      .workspace_dir = workspace,
      .mode = ava::agent::Mode::Build,
      .permission_resolver =
          [&edit_diff_denials](ava::permissions::PermissionPrompt const& prompt) -> ava::core::Result<ava::permissions::PermissionResolutionDecision> {
        edit_diff_denials.push_back(prompt);
        if (prompt.operation == ava::permissions::Operation::ReadFile)
        {
          return ava::permissions::PermissionResolution::Allow;
        }
        return ava::permissions::PermissionResolution::Deny;
      }};
  auto outside_edit_diff_denied = ava::tools::edit_file(edit_diff_deny_context, outside_path, "outside", "external");
  auto outside_after_diff_denied_edit = ava::tools::read_file(allow_context, outside_path);
  expect(!outside_edit_diff_denied && edit_diff_denials.size() == 2 && outside_after_diff_denied_edit &&
             outside_after_diff_denied_edit->content == "outside content" &&
             outside_edit_diff_denied.error().format().find("resolution: deny") != std::string::npos,
         "edit_file leaves content unchanged when external edit permission is denied after read approval");
  if (edit_diff_denials.size() >= 2)
  {
    expect(edit_diff_denials[0].operation == ava::permissions::Operation::ReadFile && edit_diff_denials[1].operation == ava::permissions::Operation::EditFile &&
               edit_diff_denials[1].diff_preview.find("-outside content") != std::string::npos &&
               edit_diff_denials[1].diff_preview.find("+external content") != std::string::npos && !edit_diff_denials[1].diff_truncated,
           "edit_file includes backend-generated diff preview before denied external edit approval");
  }

  std::vector<ava::permissions::Operation> edit_allow_prompts;
  ava::tools::ToolContext edit_allow_context{
      .workspace_dir = workspace,
      .mode = ava::agent::Mode::Build,
      .permission_resolver =
          [&edit_allow_prompts](ava::permissions::PermissionPrompt const& prompt) -> ava::core::Result<ava::permissions::PermissionResolutionDecision> {
        edit_allow_prompts.push_back(prompt.operation);
        return ava::permissions::PermissionResolution::Allow;
      }};
  auto outside_edit_allowed = ava::tools::edit_file(edit_allow_context, outside_path, "outside", "external");
  auto outside_after_allowed_edit = ava::tools::read_file(allow_context, outside_path);
  expect(outside_edit_allowed && edit_allow_prompts.size() == 2 && edit_allow_prompts[0] == ava::permissions::Operation::ReadFile &&
             edit_allow_prompts[1] == ava::permissions::Operation::EditFile && outside_after_allowed_edit &&
             outside_after_allowed_edit->content == "external content",
         "edit_file resolves external read permission before edit permission");
}

void test_permission_audit_persistence()
{
  auto const root = create_empty_root("permission-audit");

  auto const workspace = root / "workspace";
  auto const spill = root / "spill";
  std::filesystem::create_directories(workspace);
  std::filesystem::create_directories(spill);
  expect(::chmod(root.c_str(), S_IRWXU) == 0 && ::chmod(workspace.c_str(), S_IRWXU) == 0 && ::chmod(spill.c_str(), S_IRWXU) == 0,
         "permission audit command workspace is owner-only for sealed planning");
  auto anchors = ava::core::AnchorSet::open({workspace, spill});
  expect(anchors.has_value(), "permission audit opens its shared command AnchorSet");
  if (!anchors)
    return;

  auto const allowed_path = workspace / "allowed.txt";
  {
    std::ofstream file(allowed_path, std::ios::binary | std::ios::trunc);
    file << "allowed";
  }
  auto const secret_path = workspace / ".env";
  {
    std::ofstream file(secret_path, std::ios::binary | std::ios::trunc);
    file << "secret";
  }
  auto const outside_path = root / "outside.txt";
  {
    std::ofstream file(outside_path, std::ios::binary | std::ios::trunc);
    file << "outside";
  }

  ava::session::SessionStore store(ava::session::SessionStoreOptions{.root_dir = root / "sessions", .workspace_dir = workspace, .session_id = "audit"});
  auto sink = [&store](ava::tools::PermissionAuditEvent const& event) -> ava::core::VoidResult { return append_permission_audit_for_test(store, event); };
  ava::tools::ToolContext const context{.workspace_dir = workspace,
                                        .spill_dir = spill,
                                        .mode = ava::agent::Mode::Build,
                                        .permission_audit_sink = sink,
                                        .anchor_set = *anchors};

  auto allowed = ava::tools::read_file(context, allowed_path);
  expect(allowed && allowed->content == "allowed", "permission audit allows normal read");
  auto loaded = store.load();
  auto audits = loaded ? permission_entries(*loaded) : std::vector<ava::session::SessionEntry>{};
  expect(audits.size() == 1, "allowed policy decision appends one audit entry");
  if (!audits.empty())
  {
    expect(ava::core::json::string_field(audits[0].data_json, "operation") == "read" &&
               ava::core::json::string_field(audits[0].data_json, "permission_request_id").value_or("").starts_with("permreq_") &&
               ava::core::json::string_field(audits[0].data_json, "action") == "allow" &&
               ava::core::json::string_field(audits[0].data_json, "resolution") == "allow" &&
               ava::core::json::string_field(audits[0].data_json, "resolution_source") == "policy" &&
               ava::core::json::string_field(audits[0].data_json, "risk") == "low" &&
               ava::core::json::string_field(audits[0].data_json, "target_path") == allowed_path.string(),
           "allowed audit records policy resolution, risk, and target path");
  }

  auto denied = ava::tools::read_file(context, secret_path);
  expect(!denied, "permission audit denied read still fails closed");
  loaded = store.load();
  audits = loaded ? permission_entries(*loaded) : std::vector<ava::session::SessionEntry>{};
  expect(audits.size() == 2, "denied policy decision appends an audit entry");
  if (audits.size() >= 2)
  {
    expect(ava::core::json::string_field(audits[1].data_json, "action") == "deny" &&
               ava::core::json::string_field(audits[1].data_json, "permission_request_id").value_or("").starts_with("permreq_") &&
               ava::core::json::string_field(audits[1].data_json, "resolution") == "deny" &&
               ava::core::json::string_field(audits[1].data_json, "resolution_source") == "policy" &&
               ava::core::json::string_field(audits[1].data_json, "risk") == "critical",
           "denied audit records policy denial and critical risk without resolver");
  }

  int prompts = 0;
  std::string prompt_permission_request_id;
  ava::tools::ToolContext const resolving_context{
      .workspace_dir = workspace,
      .mode = ava::agent::Mode::Build,
      .permission_resolver = [&prompts, &prompt_permission_request_id](
                                 ava::permissions::PermissionPrompt const& prompt) -> ava::core::Result<ava::permissions::PermissionResolutionDecision> {
        ++prompts;
        prompt_permission_request_id = prompt.permission_request_id;
        return ava::permissions::PermissionResolution::Allow;
      },
      .permission_audit_sink = sink};
  auto outside = ava::tools::read_file(resolving_context, outside_path);
  expect(outside && outside->content == "outside" && prompts == 1 && prompt_permission_request_id.starts_with("permreq_"),
         "permission audit preserves resolver-approved read behavior and stable request id");
  loaded = store.load();
  audits = loaded ? permission_entries(*loaded) : std::vector<ava::session::SessionEntry>{};
  expect(audits.size() == 4, "ask policy and resolver outcome append separate audit entries");
  if (audits.size() >= 4)
  {
    auto const ask_permission_request_id = ava::core::json::string_field(audits[2].data_json, "permission_request_id").value_or("");
    expect(ava::core::json::string_field(audits[2].data_json, "action") == "ask" && ask_permission_request_id.starts_with("permreq_") &&
               ask_permission_request_id == prompt_permission_request_id && !ava::core::json::string_field(audits[2].data_json, "resolution") &&
               ava::core::json::string_field(audits[2].data_json, "resolution_source") == "policy" &&
               ava::core::json::string_field(audits[2].data_json, "risk") == "high",
           "ask audit records policy request and high risk before resolver outcome");
    expect(ava::core::json::string_field(audits[3].data_json, "action") == "ask" &&
               ava::core::json::string_field(audits[3].data_json, "permission_request_id") == ask_permission_request_id &&
               ava::core::json::string_field(audits[3].data_json, "resolution") == "allow" &&
               ava::core::json::string_field(audits[3].data_json, "resolution_source") == "resolver" &&
               ava::core::json::string_field(audits[3].data_json, "risk") == "high",
           "ask audit records resolver allow outcome and original risk");
  }

  auto bash_denied = ava::tools::run_bash(context, "rm -rf important");
  expect(!bash_denied && bash_denied.error().format().find("resolution: no_resolver") != std::string::npos,
         "sealed destructive bash command asks and fails closed without a resolver");
  loaded = store.load();
  audits = loaded ? permission_entries(*loaded) : std::vector<ava::session::SessionEntry>{};
  expect(audits.size() == 6, "sealed command Ask appends policy and no-resolver audit entries");
  if (audits.size() >= 6)
  {
    expect(ava::core::json::string_field(audits[4].data_json, "operation") == "bash" &&
               ava::core::json::string_field(audits[4].data_json, "command") == "<redacted one-shot command>" &&
               ava::core::json::string_field(audits[4].data_json, "action") == "ask" &&
               ava::core::json::string_field(audits[4].data_json, "risk") == "critical" && !ava::core::json::string_field(audits[4].data_json, "target_path") &&
               ava::core::json::string_field(audits[5].data_json, "resolution") == "deny" &&
               ava::core::json::string_field(audits[5].data_json, "resolution_source") == "no_resolver",
           "bash audit records critical sealed metadata without a path-only target field");
  }

  ava::tools::ToolContext const denying_context{
      .workspace_dir = workspace,
      .mode = ava::agent::Mode::Build,
      .permission_resolver = [](ava::permissions::PermissionPrompt const&) -> ava::core::Result<ava::permissions::PermissionResolutionDecision> {
        return ava::permissions::PermissionResolutionDecision{ava::permissions::PermissionResolution::Deny, "manual resolver denial"};
      },
      .permission_audit_sink = sink};
  auto resolver_denied = ava::tools::read_file(denying_context, outside_path);
  expect(!resolver_denied, "permission audit preserves resolver denial behavior");
  loaded = store.load();
  audits = loaded ? permission_entries(*loaded) : std::vector<ava::session::SessionEntry>{};
  expect(audits.size() == 8, "resolver denial appends ask and outcome audit entries after sealed command auditing");
  if (audits.size() >= 8)
  {
    expect(ava::core::json::string_field(audits[7].data_json, "resolution") == "deny" &&
               ava::core::json::string_field(audits[7].data_json, "resolution_source") == "resolver" &&
               ava::core::json::string_field(audits[7].data_json, "resolution_reason") == "manual resolver denial",
           "resolver denial audit records the client-supplied resolution reason");
  }

  auto const exported = ava::session::format_session_markdown(audits);
  expect(exported.find("## Permission Decision") != std::string::npos && exported.find("\"operation\":\"read\"") != std::string::npos &&
             exported.find("\"risk\":\"high\"") != std::string::npos && exported.find("\"resolution_source\":\"resolver\"") != std::string::npos &&
             exported.find("\"resolution_reason\":\"manual resolver denial\"") != std::string::npos,
         "session export includes permission decision audit data");
}

void test_secure_workspace_staged_write_contracts()
{
  auto root = create_empty_root("secure-workspace-staged-write");

  auto workspace = root / "workspace";
  std::filesystem::create_directories(workspace);
  auto secure = ava::tools::SecureWorkspace::open(ava::core::normalized_absolute_path(workspace));
  expect(secure.has_value(), "staged write contract test anchors a canonical workspace");
  if (!secure)
    return;

#ifdef __linux__
  auto const fd_baseline = proc_open_file_descriptors();
  expect(fd_baseline.has_value(), "staged write contract test can enumerate /proc/self/fd without counting its own directory descriptor");
  if (!fd_baseline)
    return;
  auto const fds_restored = [&fd_baseline] {
    auto current = proc_open_file_descriptors();
    return current && *current == *fd_baseline;
  };
#else
  auto const fds_restored = [] { return true; };
  expect(true, "staged write descriptor accounting explicitly requires Linux /proc/self/fd");
#endif

  auto const abandoned_path = workspace / "abandoned.txt";
  bool abandoned_held_parent_fd = true;
  {
    auto abandoned = (*secure)->stage_write(abandoned_path, "abandoned bytes");
    expect(abandoned.has_value(), "staged write can be abandoned after staging");
#ifdef __linux__
    if (abandoned)
    {
      auto added = added_file_descriptors(*fd_baseline);
      abandoned_held_parent_fd = added && added->size() == 1;
    }
#endif
  }
  expect(abandoned_held_parent_fd && !std::filesystem::exists(abandoned_path) && !has_secure_write_temp_for(workspace, "abandoned.txt") && fds_restored(),
         "abandoned staged write removes its temp and restores the file descriptor baseline");

  auto const committed_path = workspace / "committed.txt";
  bool successful_commit_contract = false;
  {
    auto staged = (*secure)->stage_write(committed_path, "committed bytes");
    if (staged)
    {
      auto committed = staged->commit();
      successful_commit_contract = committed.has_value() && staged->target_changed() && staged->path() == committed_path && staged->bytes_written() == 15;
    }
  }
  expect(successful_commit_contract && read_text_file_for_test(committed_path) == "committed bytes" && !has_secure_write_temp_for(workspace, "committed.txt") &&
             fds_restored(),
         "successful staged commit publishes bytes, cleans its temp, and restores the file descriptor baseline");

  auto const mode_path = workspace / "mode-preserved.txt";
  {
    std::ofstream original(mode_path, std::ios::binary | std::ios::trunc);
    original << "mode original";
  }
  bool const mode_setup = ::chmod(mode_path.c_str(), 0640) == 0;
  auto mode_stage = (*secure)->stage_write(mode_path, "mode replacement");
  bool mode_committed = false;
  if (mode_stage)
  {
    auto committed = mode_stage->commit();
    mode_committed = committed.has_value() && mode_stage->target_changed();
  }
  struct stat mode_status{};
  bool const mode_stat = ::stat(mode_path.c_str(), &mode_status) == 0;
  expect(mode_setup && mode_committed && read_text_file_for_test(mode_path) == "mode replacement" && mode_stat && (mode_status.st_mode & 07777) == 0640 &&
             !has_secure_write_temp_for(workspace, "mode-preserved.txt") && fds_restored(),
         "descriptor-secure stage and commit preserve an existing file's mode");

  auto const move_source_path = workspace / "move-source.txt";
  auto const move_destination_path = workspace / "move-destination.txt";
  bool move_assignment_contract = false;
  {
    auto move_source = (*secure)->stage_write(move_source_path, "moved bytes");
    auto move_destination = (*secure)->stage_write(move_destination_path, "discarded bytes");
    if (move_source && move_destination)
    {
      bool two_staged_parent_fds = true;
#ifdef __linux__
      auto two_added = added_file_descriptors(*fd_baseline);
      two_staged_parent_fds = two_added && two_added->size() == 2;
#endif
      bool const both_temps = has_secure_write_temp_for(workspace, "move-source.txt") && has_secure_write_temp_for(workspace, "move-destination.txt");
      *move_destination = std::move(*move_source);
      bool sole_staged_parent_fd = true;
#ifdef __linux__
      auto sole_added = added_file_descriptors(*fd_baseline);
      sole_staged_parent_fd = sole_added && sole_added->size() == 1;
#endif
      bool const old_temp_cleaned = has_secure_write_temp_for(workspace, "move-source.txt") && !has_secure_write_temp_for(workspace, "move-destination.txt");
      auto moved_from_commit = move_source->commit();
      auto moved_commit = move_destination->commit();
      move_assignment_contract = two_staged_parent_fds && both_temps && sole_staged_parent_fd && old_temp_cleaned && !moved_from_commit && moved_commit &&
                                 move_destination->path() == move_source_path && move_destination->target_changed();
    }
  }
  expect(move_assignment_contract && read_text_file_for_test(move_source_path) == "moved bytes" && !std::filesystem::exists(move_destination_path) &&
             !has_secure_write_temp_for(workspace) && fds_restored(),
         "staged write move assignment cleans its old temp and transfers sole ownership without leaking descriptors");

  auto const rename_failure_target = workspace / "rename-failure.txt";
  auto const preserved_original = workspace / "rename-failure-original.txt";
  auto const non_target = workspace / "rename-failure-non-target.txt";
  {
    std::ofstream original(rename_failure_target, std::ios::binary | std::ios::trunc);
    original << "rename original";
    std::ofstream unrelated(non_target, std::ios::binary | std::ios::trunc);
    unrelated << "unrelated safe bytes";
  }
  bool rename_failure_contract = false;
  {
    auto staged = (*secure)->stage_write(rename_failure_target, "rename replacement");
    if (staged)
    {
      std::error_code rename_error;
      std::filesystem::rename(rename_failure_target, preserved_original, rename_error);
      std::error_code directory_error;
      bool const directory_created = std::filesystem::create_directory(rename_failure_target, directory_error);
      if (directory_created)
      {
        std::ofstream sentinel(rename_failure_target / "sentinel.txt", std::ios::binary | std::ios::trunc);
        sentinel << "directory sentinel";
      }
      auto committed = staged->commit();
      rename_failure_contract = !rename_error && directory_created && !directory_error && !committed &&
                                committed.error().message().find("staged write commit") != std::string::npos &&
                                has_error_context(committed.error(), "path", rename_failure_target.string()) && !staged->target_changed() &&
                                has_secure_write_temp_for(workspace, "rename-failure.txt") &&
                                read_text_file_for_test(preserved_original) == "rename original" &&
                                read_text_file_for_test(rename_failure_target / "sentinel.txt") == "directory sentinel" &&
                                read_text_file_for_test(non_target) == "unrelated safe bytes";
    }
  }
  expect(rename_failure_contract && !has_secure_write_temp_for(workspace, "rename-failure.txt") && fds_restored(),
         "failed staged rename reports operation/path context, preserves original and non-target data, cleans up, and restores descriptors");

#ifdef __linux__
  auto const sync_parent = workspace / "sync-parent";
  std::filesystem::create_directories(sync_parent);
  auto const sync_target = sync_parent / "sync-target.txt";
  {
    std::ofstream original(sync_target, std::ios::binary | std::ios::trunc);
    original << "sync original";
  }
  bool parent_sync_failure_contract = false;
  {
    auto before_stage = proc_open_file_descriptors();
    auto staged = (*secure)->stage_write(sync_target, "sync replacement");
    if (before_stage && staged)
    {
      auto added = added_file_descriptors(*before_stage);
      if (added && added->size() == 1)
      {
        int const path_parent = ::open(sync_parent.c_str(), O_PATH | O_DIRECTORY | O_CLOEXEC);
        bool const parent_replaced = path_parent >= 0 && ::dup2(path_parent, added->front()) == added->front();
        if (path_parent >= 0)
          ::close(path_parent);
        if (parent_replaced)
        {
          auto committed = staged->commit();
          parent_sync_failure_contract = !committed && committed.error().message().find("write parent sync") != std::string::npos &&
                                         has_error_context(committed.error(), "path", sync_target.string()) &&
                                         has_error_context(committed.error(), "cause", std::generic_category().message(EBADF)) &&
                                         has_error_context(committed.error(), "target_changed", "true") && staged->target_changed();
        }
      }
    }
  }
  expect(parent_sync_failure_contract && read_text_file_for_test(sync_target) == "sync replacement" &&
             !has_secure_write_temp_for(sync_parent, "sync-target.txt") && fds_restored(),
         "post-rename parent fsync failure reports target_changed while leaving replacement bytes visible and restoring descriptors");
#else
  expect(true, "post-rename parent fsync fault injection is explicitly Linux-only because it requires O_PATH and /proc/self/fd");
#endif
}

void test_injected_exact_file_access()
{
  auto const root = create_empty_root("injected-exact-file-access");

  auto const workspace = root / "workspace";
  auto const outside = root / "outside";
  std::filesystem::create_directories(workspace);
  std::filesystem::create_directories(outside);
  auto secure = ava::tools::SecureWorkspace::open(ava::core::normalized_absolute_path(workspace));
  expect(secure.has_value(), "injected exact file test anchors its workspace");
  if (!secure)
    return;

  auto adapter = std::make_shared<MemoryExactFileAccess>();
  auto const remote_path = ava::core::normalized_absolute_path(workspace) / "remote.txt";
  adapter->files[remote_path] = "one\ntwo\nthree\n";
  ava::tools::ToolContext context{
      .workspace_dir = ava::core::normalized_absolute_path(workspace), .mode = ava::agent::Mode::Build, .secure_workspace = *secure, .exact_file_access = adapter};

  auto read = ava::tools::read_file(context, remote_path, ava::tools::ReadOptions{.max_bytes = 1024, .offset_line = 2, .max_lines = 1});
  auto write = ava::tools::write_file(context, workspace / "created.txt", "created remotely");
  auto edit = ava::tools::edit_file(context, remote_path, "two", "changed");
  expect(read && read->content == "two\n" && read->line_limited && !read->totals_known && read->total_bytes == 0 && read->total_lines == 0 && write && edit &&
             adapter->files[remote_path] == "one\nchanged\nthree\n" &&
             adapter->files[ava::core::normalized_absolute_path(workspace) / "created.txt"] == "created remotely" && !std::filesystem::exists(workspace / "created.txt"),
         "injected exact access keeps bounded window totals unknown while coherently handling writes and edits without local I/O");

  auto const local_only = workspace / "local-only.txt";
  {
    std::ofstream file(local_only, std::ios::binary | std::ios::trunc);
    file << "must not fall back";
  }
  adapter->fail_reads = true;
  auto failed = ava::tools::read_file(context, local_only);
  expect(!failed && failed.error().message() == "injected exact read failed", "an installed exact adapter failure never falls back to readable local bytes");
  adapter->fail_reads = false;

  adapter->supports_writes = false;
  int partial_permission_requests = 0;
  ava::tools::ToolContext partial_context = context;
  partial_context.require_explicit_file_permissions = true;
  partial_context.permission_resolver = [&](ava::permissions::PermissionPrompt const&) -> ava::core::Result<ava::permissions::PermissionResolutionDecision> {
    ++partial_permission_requests;
    return ava::permissions::PermissionResolution::Allow;
  };
  auto const partial_calls_before = adapter->read_calls + adapter->write_calls;
  auto partial_edit = ava::tools::edit_file(partial_context, remote_path, "two", "partial");
  expect(!partial_edit && partial_edit.error().message().find("capabilities are partial") != std::string::npos && partial_permission_requests == 0 &&
             adapter->read_calls + adapter->write_calls == partial_calls_before && adapter->files[remote_path] == "one\nchanged\nthree\n",
         "partial exact-file capabilities reject edit_file before permissions or I/O");
  adapter->supports_writes = true;

  auto const calls_before_denial = adapter->read_calls + adapter->write_calls;
  auto outside_read = ava::tools::read_file(context, outside / "secret.txt");
  ava::tools::ToolContext plan_context = context;
  plan_context.mode = ava::agent::Mode::Plan;
  auto denied_write = ava::tools::write_file(plan_context, workspace / "source.cpp", "int changed;\n");
  expect(!outside_read && !denied_write && adapter->read_calls + adapter->write_calls == calls_before_denial,
         "workspace-root and permission denials happen before injected exact-file callbacks");
}

void test_secure_workspace_file_tools()
{
  auto const root = create_empty_root("secure-workspace-files");

  auto const workspace = root / "workspace";
  auto const outside = root / "outside";
  std::filesystem::create_directories(workspace);
  std::filesystem::create_directories(outside);
  {
    std::ofstream file(outside / "secret.txt", std::ios::binary | std::ios::trunc);
    file << "outside secret";
  }
  auto secure = ava::tools::SecureWorkspace::open(ava::core::normalized_absolute_path(workspace));
  expect(secure.has_value(), "secure workspace anchors a canonical root descriptor");
  if (!secure)
    return;
  ava::tools::ToolContext secure_context{.workspace_dir = ava::core::normalized_absolute_path(workspace), .mode = ava::agent::Mode::Build, .secure_workspace = *secure};

  auto nested_write = ava::tools::write_file(secure_context, workspace / "nested" / "deeper" / "note.txt", "alpha beta");
  auto nested_edit = ava::tools::edit_file(secure_context, workspace / "nested" / "deeper" / "note.txt", "beta", "gamma");
  auto nested_read = ava::tools::read_file(secure_context, workspace / "nested" / "deeper" / "note.txt");
  expect(nested_write && nested_edit && nested_read && nested_read->content == "alpha gamma",
         "secure workspace creates nested parents and preserves ordinary read/edit regressions");

  std::error_code symlink_error;
  std::filesystem::create_directory_symlink(outside, workspace / "linked-parent", symlink_error);
  if (!symlink_error)
  {
    auto linked_read = ava::tools::read_file(secure_context, workspace / "linked-parent" / "secret.txt");
    auto linked_write = ava::tools::write_file(secure_context, workspace / "linked-parent" / "created.txt", "escape");
    auto linked_edit = ava::tools::edit_file(secure_context, workspace / "linked-parent" / "secret.txt", "outside", "changed");
    expect(!linked_read && !linked_write && !linked_edit && !std::filesystem::exists(outside / "created.txt") &&
               read_text_file_for_test(outside / "secret.txt") == "outside secret",
           "secure file reads, writes, and edits reject symlinked parents without touching outside files");
  }

  std::filesystem::create_directories(workspace / "race-parent");
  std::filesystem::create_directories(outside / "race-parent");
  {
    std::ofstream inside_file(workspace / "race-parent" / "target.txt", std::ios::binary | std::ios::trunc);
    inside_file << "inside original";
    std::ofstream outside_file(outside / "race-parent" / "target.txt", std::ios::binary | std::ios::trunc);
    outside_file << "outside original";
  }
  int permission_requests = 0;
  ava::tools::ToolContext race_context{
      .workspace_dir = ava::core::normalized_absolute_path(workspace),
      .mode = ava::agent::Mode::Build,
      .permission_resolver = [&](ava::permissions::PermissionPrompt const& prompt) -> ava::core::Result<ava::permissions::PermissionResolutionDecision> {
        ++permission_requests;
        expect(prompt.target_path == workspace / "race-parent" / "target.txt", "secure permission identity is canonical and root-relative before the grant");
        std::filesystem::rename(workspace / "race-parent", workspace / "race-parent-original");
        std::error_code link_error;
        std::filesystem::create_directory_symlink(outside / "race-parent", workspace / "race-parent", link_error);
        expect(!link_error, "retarget race installs a parent symlink after permission validation");
        return ava::permissions::PermissionResolution::Allow;
      },
      .require_explicit_file_permissions = true,
      .secure_workspace = *secure};
  auto raced_write = ava::tools::write_file(race_context, workspace / "race-parent" / "target.txt", "attacker content");
  expect(!raced_write && permission_requests == 1 && read_text_file_for_test(outside / "race-parent" / "target.txt") == "outside original" &&
             read_text_file_for_test(workspace / "race-parent-original" / "target.txt") == "inside original",
         "retargeting a parent symlink after a grant fails closed at descriptor-anchored I/O");
}

void test_secure_workspace_symlinked_root()
{
  // Regression for a workspace whose configured path traverses an absolute
  // symlink (e.g. /home/user/projects/github -> /usr/src/projects_github).
  // The anchor descriptor is opened at startup and must follow such symlinks;
  // only the relative paths resolved against the anchor are contained.
  auto const root = create_empty_root("symlinked-root");

  auto const workspace = root / "workspace";
  auto const real_target = root / "real-projects-github";
  auto const projects = root / "projects";
  auto const real_workspace = real_target / "ai-cli" / "AVA";
  std::filesystem::create_directories(real_workspace / "src");
  std::filesystem::create_directories(projects);
  {
    std::ofstream file(real_workspace / "src" / "main.cpp", std::ios::binary | std::ios::trunc);
    file << "int main()\n";
  }
  std::error_code link_error;
  std::filesystem::create_directory_symlink(real_target, projects / "github", link_error);
  expect(!link_error, "test creates absolute symlink in workspace path");
  if (link_error)
    return;
  auto const workspace_via_link = projects / "github" / "ai-cli" / "AVA";

  auto secure = ava::tools::SecureWorkspace::open(workspace_via_link);
  expect(secure.has_value(), "secure workspace opens a root path that traverses an absolute symlink");
  if (!secure)
    return;
  ava::tools::ToolContext context{.workspace_dir = workspace_via_link, .mode = ava::agent::Mode::Build, .secure_workspace = *secure};

  auto read_back = ava::tools::read_file(context, workspace_via_link / "src" / "main.cpp");
  expect(read_back.has_value() && read_back->content == "int main()\n", "contained read works through a symlinked workspace root");

  auto const outside = root / "outside";
  std::filesystem::create_directories(outside);
  {
    std::ofstream file(outside / "secret.txt", std::ios::binary | std::ios::trunc);
    file << "outside secret";
  }
  std::filesystem::create_directory_symlink(outside, real_workspace / "linked-parent", link_error);
  if (!link_error)
  {
    auto escaping_read = ava::tools::read_file(context, workspace_via_link / "linked-parent" / "secret.txt");
    expect(!escaping_read, "contained reads still reject a symlink that escapes the anchored workspace");
  }
}

// Verify that the descriptor-anchored workspace follows non-escaping symlinks
// in the relative path (both intermediate directory components and the final
// file component) and rejects symlinks that escape the anchor — for reads,
// writes, and AllowMissing resolution alike. The anchor is the workspace root
// fd; every symlink below is evaluated against that single anchor.
void test_secure_workspace_symlink_containment()
{
  auto const root = create_empty_root("symlink-containment");

  auto const workspace = root / "workspace";
  // Anchor: ws is the workspace root opened as the SecureWorkspace descriptor.
  auto const ws = root / "ws";
  auto const outside = root / "outside";
  std::filesystem::create_directories(ws / "real_dir");
  std::filesystem::create_directories(outside);
  {
    std::ofstream f(ws / "real_dir" / "file.txt", std::ios::binary | std::ios::trunc);
    f << "real content";
  }
  {
    std::ofstream f(ws / "real_file.txt", std::ios::binary | std::ios::trunc);
    f << "real file content";
  }
  {
    std::ofstream f(outside / "secret.txt", std::ios::binary | std::ios::trunc);
    f << "outside secret";
  }
  {
    std::ofstream f(outside / "file.txt", std::ios::binary | std::ios::trunc);
    f << "outside file";
  }

  auto secure = ava::tools::SecureWorkspace::open(ws);
  expect(secure.has_value(), "secure workspace opens the anchor directory");
  if (!secure)
    return;
  ava::tools::ToolContext context{.workspace_dir = ws, .mode = ava::agent::Mode::Build, .secure_workspace = *secure};

  std::error_code link_error;

  // linked_dir is a relative symlink to real_dir (both inside ws, no "..").
  // Anchor: ws. Symlink location: ws/linked_dir (intermediate component).
  // Symlink target: ws/real_dir — inside the anchor, so this must be ACCEPTED.
  std::filesystem::create_directory_symlink("real_dir", ws / "linked_dir", link_error);
  expect(!link_error, "create non-escaping intermediate directory symlink");
  if (!link_error)
  {
    auto read = ava::tools::read_file(context, ws / "linked_dir" / "file.txt");
    expect(read.has_value() && read->content == "real content",
           "read through non-escaping intermediate directory symlink is accepted");
  }

  // linked_file.txt is a relative symlink to real_file.txt (both inside ws).
  // Anchor: ws. Symlink location: ws/linked_file.txt (final component).
  // Symlink target: ws/real_file.txt — inside the anchor, so this must be ACCEPTED.
  std::filesystem::create_symlink("real_file.txt", ws / "linked_file.txt", link_error);
  expect(!link_error, "create non-escaping final-component file symlink");
  if (!link_error)
  {
    auto read = ava::tools::read_file(context, ws / "linked_file.txt");
    expect(read.has_value() && read->content == "real file content",
           "read through non-escaping final-component symlink is accepted");
  }

  // escape_dir is a relative symlink to ../outside (the ".." leaves ws).
  // Anchor: ws. Symlink location: ws/escape_dir (intermediate component).
  // Symlink target: root/outside — outside the anchor, so this must be REJECTED.
  std::filesystem::create_directory_symlink("../outside", ws / "escape_dir", link_error);
  expect(!link_error, "create escaping intermediate directory symlink");
  if (!link_error)
  {
    auto read = ava::tools::read_file(context, ws / "escape_dir" / "secret.txt");
    expect(!read, "read through escaping intermediate directory symlink is rejected");
  }

  // escape_file.txt is a relative symlink to ../outside/file.txt (leaves ws).
  // Anchor: ws. Symlink location: ws/escape_file.txt (final component).
  // Symlink target: root/outside/file.txt — outside the anchor, so this must be REJECTED.
  std::filesystem::create_symlink("../outside/file.txt", ws / "escape_file.txt", link_error);
  expect(!link_error, "create escaping final-component file symlink");
  if (!link_error)
  {
    auto read = ava::tools::read_file(context, ws / "escape_file.txt");
    expect(!read, "read through escaping final-component symlink is rejected");
  }

  // Writing a new file through a non-escaping intermediate directory symlink.
  // Anchor: ws. Symlink: ws/linked_dir -> real_dir (intermediate, non-escaping).
  // The parent directory resolves inside the anchor, so the write must be ACCEPTED.
  auto written = ava::tools::write_file(context, ws / "linked_dir" / "new.txt", "new content");
  expect(written.has_value(), "write through non-escaping intermediate directory symlink is accepted");

  // Writing a new file through an escaping intermediate directory symlink.
  // Anchor: ws. Symlink: ws/escape_dir -> ../outside (intermediate, escaping).
  // The parent directory escapes the anchor, so the write must be REJECTED.
  auto escape_write = ava::tools::write_file(context, ws / "escape_dir" / "new.txt", "bad");
  expect(!escape_write, "write through escaping intermediate directory symlink is rejected");

  // resolve(AllowMissing) through a non-escaping symlinked parent directory.
  // Anchor: ws. Symlink: ws/linked_dir -> real_dir (intermediate, non-escaping).
  // The file does not exist yet, but the parent path is contained, so this must be ACCEPTED.
  auto resolved = (*secure)->resolve(ws / "linked_dir" / "nonexistent.txt", ava::tools::SecureWorkspaceResolveMode::AllowMissing);
  expect(resolved.has_value() && !resolved->exists,
         "resolve AllowMissing through non-escaping symlinked parent is accepted");

  // resolve(AllowMissing) through an escaping symlinked parent directory.
  // Anchor: ws. Symlink: ws/escape_dir -> ../outside (intermediate, escaping).
  // The parent path escapes the anchor, so this must be REJECTED.
  auto escape_resolved = (*secure)->resolve(ws / "escape_dir" / "nonexistent.txt", ava::tools::SecureWorkspaceResolveMode::AllowMissing);
  expect(!escape_resolved, "resolve AllowMissing through escaping symlinked parent is rejected");
}

// Verify that AnchorSet correctly resolves candidate paths to the right anchor
// when multiple writable directories are configured. Tests cover:
//   - Basic multi-anchor resolution (longest prefix wins)
//   - Paths outside all anchors are rejected
//   - Relative paths resolve against the first (primary) anchor
//   - Non-existent directories are silently skipped
//   - contains() is consistent with find_anchor()
//   - Cross-anchor symlink escape is rejected by open_beneath
//   - Non-escaping symlinks within an anchor are followed
void test_anchor_set_multi_anchor()
{
  auto const root = create_empty_root("anchor-set");

  auto const workspace = root / "workspace";
  std::filesystem::create_directories(root);

  // Create two anchor directories.
  auto const ws_a = root / "workspace-a";
  auto const ws_b = root / "workspace-b";
  std::filesystem::create_directories(ws_a / "subdir");
  std::filesystem::create_directories(ws_b / "subdir");
  {
    std::ofstream file(ws_a / "subdir" / "file_a.txt", std::ios::binary | std::ios::trunc);
    file << "anchor A content";
  }
  {
    std::ofstream file(ws_b / "subdir" / "file_b.txt", std::ios::binary | std::ios::trunc);
    file << "anchor B content";
  }

  // Open an AnchorSet with both directories.
  auto opened = ava::core::AnchorSet::open({ws_a, ws_b});
  expect(opened.has_value(), "AnchorSet::open succeeds with two existing directories");
  if (!opened)
    return;
  auto const& anchors = **opened;

  // find_anchor for a path beneath anchor A.
  auto ref_a = anchors.find_anchor(ws_a / "subdir" / "file_a.txt");
  expect(ref_a.has_value(), "find_anchor resolves path beneath anchor A");
  expect(ref_a->anchor().root == ws_a, "find_anchor selects anchor A for A's path");
  expect(ref_a->relative() == "subdir/file_a.txt", "find_anchor returns correct relative path for anchor A");

  // find_anchor for a path beneath anchor B.
  auto ref_b = anchors.find_anchor(ws_b / "subdir" / "file_b.txt");
  expect(ref_b.has_value(), "find_anchor resolves path beneath anchor B");
  expect(ref_b->anchor().root == ws_b, "find_anchor selects anchor B for B's path");
  expect(ref_b->relative() == "subdir/file_b.txt", "find_anchor returns correct relative path for anchor B");

  // find_anchor for a path outside all anchors.
  auto ref_outside = anchors.find_anchor(root / "outside.txt");
  expect(!ref_outside, "find_anchor rejects path outside all anchors");

  // find_anchor for a relative path resolves against the first anchor.
  auto ref_rel = anchors.find_anchor("subdir/file_a.txt");
  expect(ref_rel.has_value(), "find_anchor resolves relative path against primary anchor");
  expect(ref_rel->anchor().root == ws_a, "relative path resolves to anchor A (first/primary)");
  expect(ref_rel->relative() == "subdir/file_a.txt", "relative path produces correct relative component");

  // contains() is consistent with find_anchor().
  expect(anchors.contains_lexical(ws_a / "subdir" / "file_a.txt"), "contains_lexical returns true for path in anchor A");
  expect(anchors.contains_lexical(ws_b / "subdir" / "file_b.txt"), "contains_lexical returns true for path in anchor B");
  expect(!anchors.contains_lexical(root / "outside.txt"), "contains_lexical returns false for path outside all anchors");

  // Longest prefix wins: create a nested directory structure.
  auto const ws_nested = ws_a / "nested";
  std::filesystem::create_directories(ws_nested / "deep");
  auto opened_nested = ava::core::AnchorSet::open({ws_a, ws_nested});
  expect(opened_nested.has_value(), "AnchorSet::open succeeds with nested anchors");
  if (!opened_nested)
    return;
  auto ref_nested = (*opened_nested)->find_anchor(ws_a / "nested" / "deep" / "file.txt");
  expect(ref_nested.has_value(), "find_anchor resolves path in nested anchor");
  expect(ref_nested->anchor().root == ws_nested, "find_anchor selects the longest prefix (nested) anchor");

  // Non-existent directories are silently skipped.
  auto opened_skip = ava::core::AnchorSet::open({ws_a, root / "does-not-exist", ws_b});
  expect(opened_skip.has_value(), "AnchorSet::open skips non-existent directories");
  if (!opened_skip)
    return;
  expect((*opened_skip)->number_of_anchors() == 2, "AnchorSet skips non-existent anchor, keeping two");
  expect((*opened_skip)->contains_lexical(ws_a / "subdir" / "file_a.txt"), "AnchorSet with skipped dir still contains A");
  expect((*opened_skip)->contains_lexical(ws_b / "subdir" / "file_b.txt"), "AnchorSet with skipped dir still contains B");

  // Cross-anchor symlink escape is rejected by open_beneath.
  // Create a relative symlink in anchor A that escapes to anchor B.
  // RESOLVE_BENEATH rejects absolute symlinks unconditionally, so we use a
  // relative target with ".." to test the actual escape detection.
  std::error_code link_error;
  std::filesystem::create_directory_symlink("../workspace-b", ws_a / "link-to-b", link_error);
  expect(!link_error, "test creates cross-anchor symlink");
  if (!link_error)
  {
    // The symlink is inside anchor A but points to anchor B. open_beneath
    // with RESOLVE_BENEATH should reject it (EXDEV) because the target is
    // outside anchor A's directory tree.
    auto ref_cross = anchors.find_anchor(ws_a / "link-to-b" / "subdir" / "file_b.txt");
    expect(ref_cross.has_value(), "find_anchor selects anchor A for cross-anchor symlink path");
    if (ref_cross)
    {
      int probe = ava::core::open_beneath(ref_cross->anchor().fd, ref_cross->relative(), O_PATH | O_CLOEXEC);
      expect(probe < 0, "open_beneath rejects cross-anchor symlink escape (EXDEV/ELOOP)");
      if (probe >= 0)
        ::close(probe);
    }
  }

  // Non-escaping symlink within an anchor is followed.
  // Create a relative symlink in anchor A pointing to another directory in
  // anchor A. RESOLVE_BENEATH rejects absolute symlinks unconditionally, so
  // the target must be relative for the symlink to be followed.
  std::filesystem::create_directory_symlink("subdir", ws_a / "link-to-subdir", link_error);
  expect(!link_error, "test creates non-escaping symlink");
  if (!link_error)
  {
    auto ref_internal = anchors.find_anchor(ws_a / "link-to-subdir" / "file_a.txt");
    expect(ref_internal.has_value(), "find_anchor resolves non-escaping symlink path");
    if (ref_internal)
    {
      int probe = ava::core::open_beneath(ref_internal->anchor().fd, ref_internal->relative(), O_PATH | O_CLOEXEC);
      expect(probe >= 0, "open_beneath follows non-escaping symlink within anchor");
      if (probe >= 0)
        ::close(probe);
    }
  }
}

// Verify that AnchorSet stores the lexically-normalized anchor root path,
// not the canonical (symlink-resolved) path. The anchor fd follows symlinks
// (opened with openat), but the stored root must remain the configured path
// so that find_anchor matches candidate paths expressed in terms of the
// configured path, not the resolved target.
void test_anchor_set_symlinked_root()
{
  auto const root = create_empty_root("anchor-set-symlinked-root");

  std::filesystem::create_directories(root);

  // Create a real directory with content, and a symlink pointing to it.
  auto const real_target = root / "real-projects";
  auto const symlinked_root = root / "projects";
  std::filesystem::create_directories(real_target / "src");
  {
    std::ofstream file(real_target / "src" / "main.cpp", std::ios::binary | std::ios::trunc);
    file << "int main()\n";
  }
  std::error_code link_error;
  std::filesystem::create_directory_symlink(real_target, symlinked_root, link_error);
  expect(!link_error, "test creates symlink for anchor root");
  if (link_error)
    return;

  // Open an AnchorSet using the symlinked path.
  auto opened = ava::core::AnchorSet::open({symlinked_root});
  expect(opened.has_value(), "AnchorSet::open succeeds through a symlinked root");
  if (!opened)
    return;
  auto const& anchors = **opened;

  // The stored root must be the lexically-normalized symlinked path, not
  // the canonical resolved path.
  auto ref = anchors.find_anchor(symlinked_root / "src" / "main.cpp");
  expect(ref.has_value(), "find_anchor resolves path through symlinked root");
  expect(ref->anchor().root == symlinked_root, "stored root is the lexical (symlinked) path, not the resolved target");
  expect(ref->relative() == "src/main.cpp", "relative path is correct through symlinked root");

  // open_beneath through the symlinked anchor fd works (the fd follows the
  // symlink at open time; subsequent open_beneath resolves relative to the
  // real directory).
  int probe = ava::core::open_beneath(ref->anchor().fd, ref->relative(), O_RDONLY | O_CLOEXEC);
  expect(probe >= 0, "open_beneath reads through symlinked anchor fd");
  if (probe >= 0)
    ::close(probe);

  // A path expressed through the resolved target must NOT match, because the
  // stored root is the symlinked path, not the canonical one. This ensures
  // that the anchor set only accepts paths in terms of the configured path.
  auto ref_resolved = anchors.find_anchor(real_target / "src" / "main.cpp");
  expect(!ref_resolved, "find_anchor rejects resolved path that does not lexically match the stored root");

  // contains_lexical is consistent.
  expect(anchors.contains_lexical(symlinked_root / "src" / "main.cpp"), "contains_lexical accepts symlinked root path");
  expect(!anchors.contains_lexical(real_target / "src" / "main.cpp"), "contains_lexical rejects resolved target path");
}

// Read all bytes available on fd into a string. Used to verify that the
// descriptor returned by open_writable/open_readable points at the expected
// file content.
std::string read_all_from_fd(int fd)
{
  std::string content;
  std::array<char, 4096> buffer{};
  while (true)
  {
    auto const count = ::read(fd, buffer.data(), buffer.size());
    if (count <= 0)
    {
      if (count < 0 && errno == EINTR)
        continue;
      break;
    }
    content.append(buffer.data(), static_cast<std::size_t>(count));
  }
  return content;
}

struct Info
{
  std::filesystem::path path;           // That path that is opened.
  std::filesystem::path writable_root;  // The the root of the writable anchor, or empty if not writable.
  std::filesystem::path target_path;    // The non-symlinked target.
  bool is_link;                         // path contains a symbol link.
  bool crosses_boundary;                // True if this contains a symbol link that crosses a boundary.
  std::string content;                  // The content of the file that opening this path reads.

#ifdef CWDEBUG
  void print_on(std::ostream& os) const;
  [[maybe_unused]] friend std::ostream& operator<<(std::ostream& os, Info const& info)
  {
    info.print_on(os);
    return os;
  }
#endif
};

#ifdef CWDEBUG
void Info::print_on(std::ostream& os) const
{
  os << std::boolalpha << "{path:" << path << ", writable_root:" << writable_root << ", target_path:" << target_path <<
    ", is_link:" << is_link << ", crosses_boundary:" << crosses_boundary << ", content:\"" << content << "\"}";
}
#endif

// Verify the AnchorOpen entry points (open_writable / open_readable). These are
// the single, orthogonal point through which tools open paths against the anchor
// set; the deeper find_anchor/open_beneath mechanics are covered by the
// existing AnchorSet and SecureWorkspace tests, so this test stays at the
// AnchorOpen contract level and avoids re-walking the full symlink matrix.
//   - open_writable: in-anchor open + identity; outside -> PermissionDenied;
//     O_CREAT creates beneath the anchor; a cross-anchor escaping symlink is
//     rejected.
//   - open_readable: in-anchor and ordinary external opens; descriptor-resolved
//     direct, chained, and ancestor aliases that enter an anchor are rejected;
//     O_WRONLY/O_RDWR are rejected; an escaping symlink inside an anchor is
//     rejected.
//   - AnchorOpen owns its descriptor: move transfers it, the source goes empty.
void test_anchor_open()
{
  namespace fs = std::filesystem;

  auto const root = create_empty_root("anchor-open");


  auto const workspace = root / "workspace";
  fs::create_directories(root);

  // Two writable anchors and an external (outside-all-anchors) directory.
  auto const ws_a = root / "workspace-a";
  auto const ws_b = root / "workspace-b";
  auto const external = root / "external";

  // Vector to store all six directories in.
  std::vector<fs::path> directories;

  // Helper to create and store a new directory.
  auto create_directory = [&directories](fs::path const& d) {
    directories.push_back(d);
    fs::create_directories(d);
  };

  // Vector to store all files and links to files.
  std::vector<Info> files;
  // Vector to store all links to directories.
  std::vector<Info> dirlinks;

  // Helper to create symbolic link -> target.
  auto create_link = [&files, &dirlinks](fs::path const& link, fs::path const& target, ava::core::AnchorSet::Anchor const* writable_anchor, bool crosses_boundary) {
    std::error_code link_error;
    fs::create_symlink(target.lexically_relative(link.parent_path()), link, link_error);
    expect(!link_error, "anchor-open test creates a symlink");
    if (!link_error)
    {
      std::error_code status_error;
      fs::file_status status = fs::status(link, status_error);
      expect(!status_error, "anchor-open test gets symlink status");
      if (!status_error)
      {
        if (fs::is_regular_file(status))
          // The file that this link resolves to lives in target.parent_path(), and each
          // file.txt contains its own parent directory's path string, so that is the
          // content that a read through the link must return.
          files.emplace_back(link, writable_anchor ? writable_anchor->root : fs::path{}, target, true, crosses_boundary, target.parent_path().string());
        else if (fs::is_directory(link))
          dirlinks.emplace_back(link, writable_anchor ? writable_anchor->root : fs::path{}, target, true, crosses_boundary, std::string{});
      }
    }
    else
      Dout(dc::warning, "Failed to create symlink: " << std::strerror(link_error.value()));
  };

  // clang-format off
  //                                               n
  create_directory(ws_a);                       // 0
  create_directory(ws_a / "subdir");            // 1
  create_directory(ws_b);                       // 2
  create_directory(ws_b / "subdir");            // 3
  create_directory(external);                   // 4
  create_directory(external / "subdir");        // 5

  // Open AnchorSet with the two workspaces.
  auto opened_set = ava::core::AnchorSet::open({ws_a, ws_b});
  expect(opened_set.has_value(), "AnchorSet::open succeeds for anchor-open test");
  if (!opened_set)
    return;
  auto const& anchor_set = **opened_set;
  auto const& anchors = anchor_set.anchors();

  // Helper to convert directory index to a writable anchor pointer, or nullptr if not writable.
  auto n_to_anchor = [&anchors](int n) -> ava::core::AnchorSet::Anchor const* {
    ava::core::AnchorSet::Anchor const* writable_anchor = nullptr;
    if (n == 0 || n == 1)
      writable_anchor = &anchors[0];     // ws_a
    else if (n == 2 || n == 3)
      writable_anchor = &anchors[1];     // ws_b
    return writable_anchor;
  };

  // Create a test file inside each directory.
  int n = -1;
  for (auto const& d : directories)
  {
    ++n;
    auto writable_anchor = n_to_anchor(n);
    files.emplace_back(d / "file.txt", writable_anchor ? writable_anchor->root : fs::path{}, d / "file.txt", false, false, d.string());
    std::ofstream file(files.back().path, std::ios::binary | std::ios::trunc);
    file << d.string();
  }

  // Create 30 symbolic links.
  int from_n = -1;
  for (auto const& from : directories)
  {
    ++from_n;
    int n = -1;
    for (auto const& to : directories)
    {
      ++n;                                      // Enumeration of the `to` directory.
      std::string N(1, static_cast<char>('0' + n));

      if (from == to)
        continue;

      int from_root = from_n / 2;
      int to_root = n / 2;
      bool crosses_boundary = from_root != to_root;

      auto writable_anchor = crosses_boundary ? nullptr : n_to_anchor(from_n);                          // The link appears to reside inside the anchor that belongs to from_n.
      create_link(from / ("dirlink-" + N), to, writable_anchor, crosses_boundary);                      // from/dirlink-N -> to
      create_link(from / ("filelink-" + N), to / "file.txt", writable_anchor, crosses_boundary);        // from/filelink-N -> to/file.txt
    }
  }
  // clang-format on

  // Run over all files and links to files and test trying to open them using open_writable and open_readable.
  for (Info const& info : files)
  {
    bool external = false;
    auto writable = ava::core::open_writable(anchor_set, info.path, O_RDONLY | O_CLOEXEC);
    if (writable)
    {
      fs::path const absolute = writable->absolute();
      expect(absolute == absolute.lexically_normal(), "open_writable (existing): returned absolute path is normalized");
      expect(writable->root() == info.writable_root && absolute == info.path && writable->fd() >= 0,
          "open_writable (existing): opens an in-anchor file using the correct anchor");
      expect(read_all_from_fd(writable->fd()) == info.content, "open_writable returns fd to the correct file");
      expect(writable->relative() == absolute.lexically_relative(writable->root()), "open_writable (existing): returned relative path is normalized");
    }
    else
    {
      expect(writable.error().category() == ava::core::ErrorCategory::PermissionDenied,
         "open_writable (existing): rejects a path outside all anchors with PermissionDenied");
      expect(info.writable_root.empty(), "open_writable (existing): refuses to open files that escape their anchor");
      external = true;
    }

    auto readable = ava::core::open_readable(anchor_set, info.path, O_RDONLY | O_CLOEXEC);
    expect(readable.has_value() != info.crosses_boundary, "open_readable (existing): only opens files that do not cross anchor boundaries");
    if (readable.has_value() == info.crosses_boundary)
    {
      if (!readable.has_value())
        Dout(dc::warning, "open_readable denied reading with " << readable.error() << "; expected was that we would be able to open it!");
      else
        Dout(dc::warning, "open_readable allowed reading; expected was that it would be denied!");
    }
    if (readable)
    {
      fs::path const absolute = readable->absolute();
      expect(absolute == absolute.lexically_normal(), "open_readable (existing): returned absolute path is normalized");
      expect(absolute == info.path, "open_readable (existing): returns the expected absolute path");
      expect(readable->fd() >= 0, "open_readable (existing): returns a readable fd");
      expect(read_all_from_fd(readable->fd()) == info.content, "open_readable (existing): returns fd to the correct file");
      expect(!external || (readable->root().empty() && readable->relative().empty()),
          "open_readable (existing): returns an empty root for external file");
      expect(external || (readable->root() == writable->root() && readable->relative() == absolute.lexically_relative(readable->root())),
          "open_readable (existing): returns writable root if in-anchor and relative path is normalized");
    }

    // AnchorOpen owns its descriptor: move transfers it, the source becomes empty.
    if (writable)
    {
      int const held_fd = writable->fd();
      auto moved = std::move(*writable);
      expect(moved.fd() == held_fd && writable->fd() == -1, "AnchorOpen move transfers the descriptor and empties the source");
    }
  }

  // Run over all dirlinks.
  for (Info const& info : dirlinks)
  {
    std::string dp = info.path.string();
    int dir_n = dp[dp.length() - 1] - '0';
    for (int file_n = 0; file_n < 6; ++file_n)
    {
      if (file_n == dir_n)
        continue;
      std::string N(1, static_cast<char>('0' + file_n));
      fs::path fp = info.path / ("filelink-" + N);

      bool const crosses_boundary = info.crosses_boundary || (file_n / 2) != (dir_n / 2);

      auto writable = ava::core::open_writable(anchor_set, fp, O_RDONLY | O_CLOEXEC);
      if (writable)
      {
        fs::path const absolute = writable->absolute();
        expect(absolute == absolute.lexically_normal(), "returned dirlink/filelink absolute path is normalized");
        expect(writable->root() == info.writable_root && absolute == fp && writable->fd() >= 0,
            "open_writable opens an in-anchor dirlink/filelink using the correct anchor");
        expect(read_all_from_fd(writable->fd()) == directories[file_n].string(), "open_writable dirlink/filelink returns fd to the correct file");
        expect(writable->relative() == absolute.lexically_relative(writable->root()), "returned dirlink/filelink relative path is normalized");
      }
      else
      {
        expect(writable.error().category() == ava::core::ErrorCategory::PermissionDenied,
           "open_writable rejects an escaping dirlink/filelink path with PermissionDenied");
        if (writable.error().category() != ava::core::ErrorCategory::PermissionDenied)
          Dout(dc::warning, "open_writable denied writing with " << writable.error() << "; expected was " <<
              (crosses_boundary ? "that it would be denied with PermissionDenied!" : "that we would be able to open it!"));
      }

      auto readable = ava::core::open_readable(anchor_set, fp, O_RDONLY | O_CLOEXEC);
      expect(readable.has_value() != crosses_boundary, "open_readable (dirlink): only opens files that do not cross anchor boundaries");
      if (readable.has_value() == crosses_boundary)
      {
        if (!readable.has_value())
          Dout(dc::warning, "open_readable denied reading with " << readable.error() << "; expected was that we would be able to open it!");
        else
          Dout(dc::warning, "open_readable allowed reading; expected was that it would be denied!");
      }
      if (readable)
      {
        bool const external = dir_n > 3;

        fs::path const absolute = readable->absolute();
        expect(absolute == absolute.lexically_normal(), "open_readable (dirlink): returned absolute path is normalized");
        expect(absolute == fp, "open_readable (dirlink): returns the expected absolute path");
        expect(readable->fd() >= 0, "open_readable (dirlink): returns a readable fd");
        expect(read_all_from_fd(readable->fd()) == directories[file_n].string(), "open_readable (dirlink): returns fd to the correct file");
        expect(!external || (readable->root().empty() && readable->relative().empty()),
            "open_readable (dirlink): returns an empty root for external file");
        expect(external || (readable->root() == info.writable_root && readable->relative() == absolute.lexically_relative(readable->root())),
            "open_readable (dirlink): returns writable root if in-anchor and relative path is normalized");
      }
    }
  }

  // Remove all files, causing all symbolic links to them to becomes dangling links.
  for (Info const& info : files)
  {
    if (info.is_link)
      continue;
    std::error_code remove_error;
    bool success = fs::remove(info.path, remove_error);
    expect(success, "remove plain file");
    if (!success)
      Dout(dc::warning, "fs::remove(" << info.path << ") : " << std::strerror(remove_error.value()));
  }

  // Run over all symbolic link to, now non-existing, "file.txt" targets.
  for (Info const& info : files)
  {
    if (!info.is_link)
      continue;

    // open_writable creates a new file beneath the anchor with O_CREAT. O_EXCL is
    // intentionally NOT used: it checks the directory entry itself rather than
    // following the symlink, so it would return EEXIST for every symlink path
    // regardless of whether the (now removed) target exists — defeating the
    // purpose of exercising creation through a dangling link. RESOLVE_BENEATH
    // still contains the creation: a contained symlink creates its target inside
    // the anchor; an escaping symlink is rejected with EXDEV before any file is
    // created.
    auto created = ava::core::open_writable(anchor_set, info.path, O_WRONLY | O_CREAT | O_CLOEXEC, 0600);
    if (created)
    {
      fs::path const absolute = created->absolute();
      expect(absolute == absolute.lexically_normal(), "open_writable (create): returned absolute path is normalized");
      expect(created->root() == info.writable_root && absolute == info.path && created->fd() >= 0,
          "open_writable (create): opens an in-anchor file using the correct anchor");
      expect(created->relative() == absolute.lexically_relative(created->root()), "open_writable (create): returned relative path is normalized");
      expect(::write(created->fd(), "created", 7) == 7, "open_writable created descriptor is writable");
      std::ifstream target(info.target_path);
      std::string s;
      target >> s;
      expect(s == "created", "open_writable (create): created file contains the written data");
      std::error_code cleanup;
      fs::remove(info.target_path, cleanup);
    }
    else
    {
      expect(created.error().category() == ava::core::ErrorCategory::PermissionDenied,
         "open_writable (create): rejects a path outside all anchors with PermissionDenied");
      if (created.error().category() != ava::core::ErrorCategory::PermissionDenied)
        Dout(dc::warning, "The error is " << created.error());
      expect(info.writable_root.empty(), "open_writable (create): refuses to open files that escape their anchor");
    }
  }

  auto const ordinary_external_path = external / "subdir" / "ordinary.txt";
  {
    std::ofstream ordinary(ordinary_external_path, std::ios::binary | std::ios::trunc);
    ordinary << "ordinary external";
  }
  auto const anchor_secret_path = ws_a / "subdir" / "anchor-secret.txt";
  {
    std::ofstream secret(anchor_secret_path, std::ios::binary | std::ios::trunc);
    secret << "anchor secret";
  }
  auto ordinary_external = ava::core::open_readable(anchor_set, ordinary_external_path, O_RDONLY | O_CLOEXEC);
  expect(ordinary_external && ordinary_external->absolute() == ordinary_external_path && read_all_from_fd(ordinary_external->fd()) == "ordinary external",
         ordinary_external ? "open_readable preserves ordinary external reads and their logical absolute identity"
                           : "open_readable preserves ordinary external reads and their logical absolute identity: " + ordinary_external.error().format());

  std::error_code alias_error;
  fs::create_symlink(anchor_secret_path, external / "direct-anchor-alias", alias_error);
  expect(!alias_error, "anchor-open creates a direct external alias into an anchor");
  auto direct_alias = ava::core::open_readable(anchor_set, external / "direct-anchor-alias", O_RDONLY | O_CLOEXEC);
  expect(!direct_alias, "open_readable rejects a direct external alias entering an anchor");

  alias_error.clear();
  fs::create_symlink("direct-anchor-alias", external / "first-chain-alias", alias_error);
  expect(!alias_error, "anchor-open creates a chained external alias into an anchor");
  auto chained_alias = ava::core::open_readable(anchor_set, external / "first-chain-alias", O_RDONLY | O_CLOEXEC);
  expect(!chained_alias, "open_readable rejects chained external aliases entering an anchor");

  alias_error.clear();
  fs::create_directory_symlink(root, external / "ancestor-alias", alias_error);
  expect(!alias_error, "anchor-open creates an external alias to an anchor ancestor");
  auto ancestor_alias =
      ava::core::open_readable(anchor_set, external / "ancestor-alias" / "workspace-a" / "subdir" / "anchor-secret.txt", O_RDONLY | O_CLOEXEC);
  expect(!ancestor_alias, "open_readable rejects an ancestor alias followed by normal components that enter an anchor");

  // open_readable rejects write access modes.
  auto const write_mode = ava::core::open_readable(anchor_set, ws_a / "subdir" / "file.txt", O_WRONLY | O_CLOEXEC);
  auto const rdwr_mode = ava::core::open_readable(anchor_set, ws_a / "subdir" / "file.txt", O_RDWR | O_CLOEXEC);
  expect(!write_mode && write_mode.error().category() == ava::core::ErrorCategory::InvalidArgument && !rdwr_mode &&
             rdwr_mode.error().category() == ava::core::ErrorCategory::InvalidArgument,
         "open_readable rejects O_WRONLY and O_RDWR with InvalidArgument");
}

void test_permission_user_guidance_propagation()
{
  auto const root = create_empty_root("permission-user-guidance");
  auto const workspace = root / "workspace";
  std::filesystem::create_directories(workspace);
  auto const outside_path = root / "outside-guidance.txt";
  {
    std::ofstream outside_file(outside_path, std::ios::binary | std::ios::trunc);
    outside_file << "outside content";
  }

  std::vector<ava::tools::PermissionAuditEvent> audits;
  auto audit_sink = [&audits](ava::tools::PermissionAuditEvent const& event) -> ava::core::VoidResult {
    audits.push_back(event);
    return {};
  };

  auto guided_capture = std::make_shared<ava::tools::PermissionDenialGuidanceCapture>();
  ava::tools::ToolContext guided_context{
      .workspace_dir = workspace,
      .mode = ava::agent::Mode::Build,
      .permission_resolver =
          [](ava::permissions::PermissionPrompt const&) -> ava::core::Result<ava::permissions::PermissionResolutionDecision> {
            ava::permissions::PermissionResolutionDecision denied{ava::permissions::PermissionResolution::Deny, "not approved"};
            denied.user_guidance = "stay inside the workspace";
            return denied;
          },
      .permission_audit_sink = audit_sink,
      .permission_denial_guidance_capture = guided_capture};
  auto guided_denied = ava::tools::read_file(guided_context, outside_path);
  expect(!guided_denied && guided_capture->provider_user_guidance == "stay inside the workspace" &&
             guided_denied.error().format().find("user_guidance") == std::string::npos &&
             guided_denied.error().format().find("stay inside the workspace") == std::string::npos &&
             guided_denied.error().format().find("resolution: deny") != std::string::npos &&
             !has_error_context(guided_denied.error(), "user_guidance", "stay inside the workspace"),
         "non-command permission denial captures validated guidance without entering Error context/format");

  bool audits_free_of_guidance = !audits.empty();
  for (auto const& event : audits)
  {
    auto const json = ava::tools::permission_audit_data_json(event);
    audits_free_of_guidance = audits_free_of_guidance && json.find("user_guidance") == std::string::npos &&
                              json.find("provider_user_guidance") == std::string::npos && json.find("stay inside the workspace") == std::string::npos &&
                              event.reason.find("stay inside") == std::string::npos && event.resolution_reason.find("stay inside") == std::string::npos;
  }
  expect(audits_free_of_guidance, "permission audits never serialize one-shot user_guidance");

  // Direct app-command contexts without a capture discard guidance entirely.
  ava::tools::ToolContext no_capture_context{
      .workspace_dir = workspace,
      .mode = ava::agent::Mode::Build,
      .permission_resolver =
          [](ava::permissions::PermissionPrompt const&) -> ava::core::Result<ava::permissions::PermissionResolutionDecision> {
            ava::permissions::PermissionResolutionDecision denied{ava::permissions::PermissionResolution::Deny, "not approved"};
            denied.user_guidance = "discard me";
            return denied;
          },
      .permission_audit_sink = audit_sink};
  auto discarded = ava::tools::read_file(no_capture_context, outside_path);
  expect(!discarded && discarded.error().format().find("discard me") == std::string::npos &&
             discarded.error().format().find("user_guidance") == std::string::npos,
         "direct command contexts without a capture discard denial guidance");

  audits.clear();
  auto forged_capture = std::make_shared<ava::tools::PermissionDenialGuidanceCapture>();
  ava::tools::ToolContext forged_context{
      .workspace_dir = workspace,
      .mode = ava::agent::Mode::Build,
      .permission_resolver =
          [](ava::permissions::PermissionPrompt const&) -> ava::core::Result<ava::permissions::PermissionResolutionDecision> {
            ava::permissions::PermissionResolutionDecision denied{ava::permissions::PermissionResolution::Deny, "forged"};
            denied.user_guidance = "evil\nguidance\x01";
            return denied;
          },
      .permission_audit_sink = audit_sink,
      .permission_denial_guidance_capture = forged_capture};
  auto forged_denied = ava::tools::read_file(forged_context, outside_path);
  expect(!forged_denied && forged_capture->provider_user_guidance.empty() && forged_denied.error().format().find("user_guidance") == std::string::npos &&
             forged_denied.error().format().find("evil") == std::string::npos,
         "malformed forged user_guidance is dropped at the backend trust boundary");

  audits.clear();
  auto overcap_capture = std::make_shared<ava::tools::PermissionDenialGuidanceCapture>();
  ava::tools::ToolContext overcap_context{
      .workspace_dir = workspace,
      .mode = ava::agent::Mode::Build,
      .permission_resolver =
          [](ava::permissions::PermissionPrompt const&) -> ava::core::Result<ava::permissions::PermissionResolutionDecision> {
            ava::permissions::PermissionResolutionDecision denied{ava::permissions::PermissionResolution::Deny};
            denied.user_guidance = std::string(ava::permissions::kMaxPermissionUserGuidanceBytes + 8, 'z');
            return denied;
          },
      .permission_audit_sink = audit_sink,
      .permission_denial_guidance_capture = overcap_capture};
  auto overcap_denied = ava::tools::read_file(overcap_context, outside_path);
  expect(!overcap_denied && overcap_capture->provider_user_guidance.empty() && overcap_denied.error().format().find("user_guidance") == std::string::npos,
         "over-cap forged user_guidance never enters the per-dispatch capture or Error format");
}

}  // namespace

void run_tools_file_tests()
{
  test_mutation_queue_cleans_drained_path_entries();
  test_file_tools();
  test_permission_audit_persistence();
  test_secure_workspace_staged_write_contracts();
  test_injected_exact_file_access();
  test_secure_workspace_file_tools();
  test_secure_workspace_symlinked_root();
  test_secure_workspace_symlink_containment();
  test_anchor_set_multi_anchor();
  test_anchor_set_symlinked_root();
  test_anchor_open();
  test_permission_user_guidance_propagation();
}

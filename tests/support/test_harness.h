#pragma once

#include "ava/tools/file_tools.h"
#include "ava/session/compaction.h"
#include "ava/session/session_metadata.h"
#include "ava/session/session_store.h"
#include "ava/core/AnchorSet.h"
#include "ava/core/error.h"

#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace ava::tests {
int& failure_count();
int failures();
void clear_skip();
void request_skip(std::string message);
bool skip_requested();
std::string skip_message();
}  // namespace ava::tests

void expect(bool condition, std::string const& message);

bool read_exact_from_descriptor_for_test(int descriptor, void* buffer, std::size_t byte_count) noexcept;
bool write_all_to_descriptor_for_test(int descriptor, void const* buffer, std::size_t byte_count) noexcept;

class FailingStreambuf final : public std::streambuf
{
 protected:
  int overflow(int ch) override;
  std::streamsize xsputn(char const* s, std::streamsize count) override;
};

// Physical per-process test namespace used by security fixtures that need to
// control ancestor permissions explicitly. Each calling process uniquely creates
// the directories it actually acquires; they are not adopted from leftover PID
// names. cleanup_owned_test_directories() removes this process's namespace.
std::filesystem::path temp_root();
// Create an empty logical root. If the directory already exists it will be cleaned out.
std::filesystem::path create_empty_root(std::filesystem::path root_name);
// Remove directories acquired by this process without following symlinks. Forked
// children skip parent-owned entries. Failures are reported through expect() and
// counted as test failures. ENOENT is success. Does not glob or purge leftovers.
bool cleanup_owned_test_directories() noexcept;
std::shared_ptr<ava::core::AnchorSet> command_anchors_for_test(std::filesystem::path const& workspace, std::filesystem::path const& spill_dir);

class ScopedEnvVar
{
 public:
  ScopedEnvVar(std::string name, std::string value);

  ScopedEnvVar(ScopedEnvVar const&) = delete;
  ScopedEnvVar& operator=(ScopedEnvVar const&) = delete;
  ScopedEnvVar(ScopedEnvVar&&) = delete;
  ScopedEnvVar& operator=(ScopedEnvVar&&) = delete;

  ~ScopedEnvVar();

 private:
  std::string name_;
  std::optional<std::string> previous_ = std::nullopt;
};

// Owns one std::tmpfile stream: get() borrows its FILE*, destruction closes it,
// and construction throws std::system_error with errno when allocation fails.
class ScopedTmpFile
{
 public:
  ScopedTmpFile()
  {
    file_ptr_ = std::tmpfile();
    if (!file_ptr_)
    {
      int const error = errno;
      throw std::system_error(error, std::generic_category(), "std::tmpfile");
    }
  }

  ScopedTmpFile(ScopedTmpFile const&) = delete;
  ScopedTmpFile& operator=(ScopedTmpFile const&) = delete;
  ScopedTmpFile(ScopedTmpFile&& other) noexcept : file_ptr_(std::exchange(other.file_ptr_, nullptr)) { }
  ScopedTmpFile& operator=(ScopedTmpFile&&) = delete;

  ~ScopedTmpFile() noexcept
  {
    if (file_ptr_)
      static_cast<void>(std::fclose(file_ptr_));
  }

  FILE* get() const { return file_ptr_; }

 private:
  FILE* file_ptr_ = nullptr;
};

// Owns one uniquely created directory. Destruction removes that directory without
// following symlinks, so a fixture replaced by a symlink to a foreign target does
// not delete the target. Forked children do not remove a parent's directory.
// Teardown never throws; removal failures are reported through expect().
class ScopedTestDirectory
{
 public:
  [[nodiscard]] static ScopedTestDirectory create_unique(std::filesystem::path const& parent, std::string_view prefix);

  ScopedTestDirectory(ScopedTestDirectory const&) = delete;
  ScopedTestDirectory& operator=(ScopedTestDirectory const&) = delete;
  ScopedTestDirectory(ScopedTestDirectory&& other) noexcept;
  ScopedTestDirectory& operator=(ScopedTestDirectory&& other) noexcept;
  ~ScopedTestDirectory() noexcept;

  [[nodiscard]] std::filesystem::path const& path() const noexcept { return path_; }
  [[nodiscard]] operator std::filesystem::path const&() const noexcept { return path_; }
  [[nodiscard]] std::filesystem::path operator/(std::filesystem::path const& rhs) const { return path_ / rhs; }

 private:
  explicit ScopedTestDirectory(std::filesystem::path path);
  void teardown() noexcept;

  std::filesystem::path path_;
  long owner_pid_ = 0;
  bool armed_ = false;
};

std::string strip_sgr(std::string_view text);
bool has_active_sgr_at_text(std::string_view line, std::string_view text, std::string_view sgr);

namespace ava::tests {
inline std::size_t count_occurrences(std::string_view text, std::string_view needle)
{
  if (needle.empty())
    return 0;

  std::size_t count = 0;
  for (std::size_t offset = text.find(needle); offset != std::string_view::npos; offset = text.find(needle, offset + needle.size()))
    ++count;
  return count;
}
}  // namespace ava::tests

// Test-only authority adapter. Persistent test fixtures acquire the exact
// lease for the duration of one append; runtime tests instead use owner routes.
ava::core::VoidResult append_session_entry_for_test(ava::session::SessionStore& store, ava::session::SessionEntry const& entry);
std::function<ava::core::VoidResult(ava::session::SessionEntry const&)> append_route_for_test(ava::session::SessionStore const& store);
std::function<ava::core::VoidResult(std::vector<ava::session::SessionEntry>)> append_batch_route_for_test(ava::session::SessionStore const& store);
ava::session::SessionReadAuthority read_authority_for_test(ava::session::SessionStore const& store);
ava::core::Result<ava::session::SessionMetadataView> append_session_metadata_for_test(ava::session::SessionStore& store,
                                                                                      ava::session::SessionMetadataUpdate update);
ava::core::VoidResult append_manual_compaction_for_test(ava::session::SessionStore& store, ava::session::ManualCompactionRequest request);
ava::core::VoidResult append_permission_audit_for_test(ava::session::SessionStore& store, ava::tools::PermissionAuditEvent const& event);
std::vector<ava::session::SessionEntry> permission_entries(std::vector<ava::session::SessionEntry> const& entries);
std::size_t visible_columns(std::string_view text);

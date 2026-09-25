#include "tests/support/test_harness.h"
#include "ava/core/ids.h"
#include "ava/session/record.h"
#include "ava/session/validation.h"

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstdlib>
#include <cwchar>
#include <filesystem>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>
#include <pthread.h>
#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>

namespace ava::tests {
namespace {
int failures_value = 0;
bool skip_requested_value = false;
std::string skip_message_value;
} // namespace

int& failure_count()
{
  return failures_value;
}

int failures()
{
  return failures_value;
}

void clear_skip()
{
  skip_requested_value = false;
  skip_message_value.clear();
}

void request_skip(std::string message)
{
  skip_requested_value = true;
  skip_message_value = std::move(message);
}

bool skip_requested()
{
  return skip_requested_value;
}

std::string skip_message()
{
  return skip_message_value;
}
}  // namespace ava::tests

namespace {

bool skip_osc_sequence(std::string_view text, std::size_t& index)
{
  if (index + 1 >= text.size() || text[index] != '\x1b' || text[index + 1] != ']')
  {
    return false;
  }
  auto end = index + 2;
  while (end < text.size())
  {
    if (text[end] == '\a')
    {
      index = end + 1;
      return true;
    }
    if (text[end] == '\x1b' && end + 1 < text.size() && text[end + 1] == '\\')
    {
      index = end + 2;
      return true;
    }
    ++end;
  }
  return false;
}

bool is_zero_width_codepoint(char32_t codepoint)
{
  return (codepoint >= 0x0300 && codepoint <= 0x036F) || (codepoint >= 0x0483 && codepoint <= 0x0489) || (codepoint >= 0x0591 && codepoint <= 0x05BD) ||
         codepoint == 0x05BF || (codepoint >= 0x05C1 && codepoint <= 0x05C2) || (codepoint >= 0x05C4 && codepoint <= 0x05C5) || codepoint == 0x05C7 ||
         (codepoint >= 0x0610 && codepoint <= 0x061A) || (codepoint >= 0x064B && codepoint <= 0x065F) || codepoint == 0x0670 ||
         (codepoint >= 0x06D6 && codepoint <= 0x06DC) || (codepoint >= 0x06DF && codepoint <= 0x06E4) || (codepoint >= 0x06E7 && codepoint <= 0x06E8) ||
         (codepoint >= 0x06EA && codepoint <= 0x06ED) || codepoint == 0x0711 || (codepoint >= 0x0730 && codepoint <= 0x074A) ||
         (codepoint >= 0x07A6 && codepoint <= 0x07B0) || (codepoint >= 0x07EB && codepoint <= 0x07F3) || (codepoint >= 0x0816 && codepoint <= 0x0819) ||
         (codepoint >= 0x081B && codepoint <= 0x0823) || (codepoint >= 0x0825 && codepoint <= 0x0827) || (codepoint >= 0x0829 && codepoint <= 0x082D) ||
         (codepoint >= 0x0859 && codepoint <= 0x085B) || (codepoint >= 0x08D3 && codepoint <= 0x08E1) || (codepoint >= 0x08E3 && codepoint <= 0x0903) ||
         (codepoint >= 0x093A && codepoint <= 0x093C) || (codepoint >= 0x0941 && codepoint <= 0x0948) || codepoint == 0x094D ||
         (codepoint >= 0x0951 && codepoint <= 0x0957) || (codepoint >= 0x0962 && codepoint <= 0x0963) || codepoint == 0x200C || codepoint == 0x200D ||
         (codepoint >= 0x20D0 && codepoint <= 0x20FF) || (codepoint >= 0xFE00 && codepoint <= 0xFE0F) || (codepoint >= 0xFE20 && codepoint <= 0xFE2F) ||
         (codepoint >= 0xE0100 && codepoint <= 0xE01EF);
}

}  // namespace

void expect(bool condition, std::string const& message)
{
  if (!condition)
  {
    std::cerr << "FAIL: " << message << '\n';
    ++ava::tests::failure_count();
  }
}

bool read_exact_from_descriptor_for_test(int descriptor, void* buffer, std::size_t byte_count) noexcept
{
  auto* next = static_cast<char*>(buffer);
  std::size_t offset = 0;
  while (offset < byte_count)
  {
    auto const transferred = ::read(descriptor, next + offset, byte_count - offset);
    if (transferred < 0 && errno == EINTR)
      continue;
    if (transferred <= 0)
      return false;
    offset += static_cast<std::size_t>(transferred);
  }
  return true;
}

bool write_all_to_descriptor_for_test(int descriptor, void const* buffer, std::size_t byte_count) noexcept
{
  sigset_t sigpipe_mask{};
  if (::sigemptyset(&sigpipe_mask) != 0 || ::sigaddset(&sigpipe_mask, SIGPIPE) != 0)
    return false;

  sigset_t previous_mask{};
  if (::pthread_sigmask(SIG_BLOCK, &sigpipe_mask, &previous_mask) != 0)
    return false;

  int const sigpipe_was_blocked = ::sigismember(&previous_mask, SIGPIPE);
  bool succeeded = sigpipe_was_blocked >= 0;
  bool received_epipe = false;
  auto const* next = static_cast<char const*>(buffer);
  std::size_t offset = 0;
  while (succeeded && offset < byte_count)
  {
    auto const transferred = ::write(descriptor, next + offset, byte_count - offset);
    if (transferred < 0 && errno == EINTR)
      continue;
    if (transferred <= 0)
    {
      received_epipe = transferred < 0 && errno == EPIPE;
      succeeded = false;
      break;
    }
    offset += static_cast<std::size_t>(transferred);
  }

  if (received_epipe && sigpipe_was_blocked == 0)
  {
    timespec const no_wait{};
    int waited = -1;
    do
    {
      waited = ::sigtimedwait(&sigpipe_mask, nullptr, &no_wait);
    } while (waited < 0 && errno == EINTR);
  }

  if (::pthread_sigmask(SIG_SETMASK, &previous_mask, nullptr) != 0)
    return false;
  return succeeded;
}

int FailingStreambuf::overflow(int ch)
{
  static_cast<void>(ch);
  return traits_type::eof();
}

std::streamsize FailingStreambuf::xsputn(char const* s, std::streamsize count)
{
  static_cast<void>(s);
  static_cast<void>(count);
  return 0;
}

namespace {

std::mutex owned_directory_mutex;
struct OwnedDirectory
{
  std::filesystem::path path;
  long owner_pid = 0;
};
std::vector<OwnedDirectory> owned_directories;
std::vector<std::pair<std::filesystem::path, std::filesystem::path>> temp_roots_by_tmpdir;

void register_owned_directory(std::filesystem::path path)
{
  std::lock_guard lock(owned_directory_mutex);
  owned_directories.push_back(OwnedDirectory{.path = std::move(path), .owner_pid = static_cast<long>(::getpid())});
}

void unregister_owned_directory(std::filesystem::path const& path)
{
  std::lock_guard lock(owned_directory_mutex);
  auto const pid = static_cast<long>(::getpid());
  std::erase_if(owned_directories, [&](OwnedDirectory const& entry) { return entry.owner_pid == pid && entry.path == path; });
}

bool remove_path_without_following_symlinks(std::filesystem::path const& path, std::string& error_message) noexcept
{
  std::error_code status_error;
  auto const status = std::filesystem::symlink_status(path, status_error);
  if (status_error)
  {
    if (status_error == std::errc::no_such_file_or_directory)
      return true;
    error_message = "stat owned test fixture " + path.string() + ": " + status_error.message();
    return false;
  }
  if (std::filesystem::is_symlink(status) || !std::filesystem::is_directory(status))
  {
    std::error_code remove_error;
    std::filesystem::remove(path, remove_error);
    if (remove_error && remove_error != std::errc::no_such_file_or_directory)
    {
      error_message = "remove owned test fixture " + path.string() + ": " + remove_error.message();
      return false;
    }
    return true;
  }

  std::error_code iterator_error;
  for (auto const& entry : std::filesystem::directory_iterator(path, std::filesystem::directory_options::skip_permission_denied, iterator_error))
  {
    if (!remove_path_without_following_symlinks(entry.path(), error_message))
      return false;
  }
  if (iterator_error && iterator_error != std::errc::no_such_file_or_directory)
  {
    error_message = "scan owned test fixture " + path.string() + ": " + iterator_error.message();
    return false;
  }

  std::error_code remove_error;
  std::filesystem::remove(path, remove_error);
  if (remove_error && remove_error != std::errc::no_such_file_or_directory)
  {
    error_message = "remove owned test fixture " + path.string() + ": " + remove_error.message();
    return false;
  }
  return true;
}

std::filesystem::path create_unique_owned_directory(std::filesystem::path const& parent, std::string_view prefix)
{
  std::error_code parent_error;
  if (!std::filesystem::is_directory(parent, parent_error))
  {
    throw std::runtime_error("owned test directory parent does not exist: " + parent.string() +
                             (parent_error ? (": " + parent_error.message()) : std::string()));
  }

  std::string tmpl = (parent / (std::string(prefix) + "XXXXXX")).string();
  if (::mkdtemp(tmpl.data()) == nullptr)
    throw std::system_error(errno, std::generic_category(), "create unique test directory under " + parent.string());
  std::filesystem::path created{std::move(tmpl)};
  if (::chmod(created.c_str(), S_IRWXU) != 0)
  {
    int const error = errno;
    std::error_code ignored;
    std::filesystem::remove(created, ignored);
    throw std::system_error(error, std::generic_category(), "set owner-only permissions on " + created.string());
  }
  register_owned_directory(created);
  return created;
}

}  // namespace

ScopedTestDirectory::ScopedTestDirectory(std::filesystem::path path) : path_(std::move(path)), owner_pid_(static_cast<long>(::getpid())), armed_(true)
{
}

ScopedTestDirectory ScopedTestDirectory::create_unique(std::filesystem::path const& parent, std::string_view prefix)
{
  return ScopedTestDirectory(create_unique_owned_directory(parent, prefix));
}

ScopedTestDirectory::ScopedTestDirectory(ScopedTestDirectory&& other) noexcept
    : path_(std::move(other.path_)), owner_pid_(other.owner_pid_), armed_(other.armed_)
{
  other.armed_ = false;
  other.owner_pid_ = 0;
}

ScopedTestDirectory& ScopedTestDirectory::operator=(ScopedTestDirectory&& other) noexcept
{
  if (this != &other)
  {
    teardown();
    path_ = std::move(other.path_);
    owner_pid_ = other.owner_pid_;
    armed_ = other.armed_;
    other.armed_ = false;
    other.owner_pid_ = 0;
  }
  return *this;
}

ScopedTestDirectory::~ScopedTestDirectory() noexcept
{
  teardown();
}

void ScopedTestDirectory::teardown() noexcept
{
  if (!armed_)
    return;
  armed_ = false;
  if (owner_pid_ != static_cast<long>(::getpid()))
    return;
  std::string error_message;
  if (remove_path_without_following_symlinks(path_, error_message))
    unregister_owned_directory(path_);
  else
    expect(false, "failed to remove owned test fixture " + path_.string() + ": " + error_message);
}

bool cleanup_owned_test_directories() noexcept
{
  std::vector<OwnedDirectory> snapshot;
  {
    std::lock_guard lock(owned_directory_mutex);
    snapshot = owned_directories;
  }
  auto const pid = static_cast<long>(::getpid());
  bool ok = true;
  for (auto const& entry : snapshot)
  {
    if (entry.owner_pid != pid)
      continue;
    std::string error_message;
    if (!remove_path_without_following_symlinks(entry.path, error_message))
    {
      expect(false, "failed to remove owned test fixture " + entry.path.string() + ": " + error_message);
      ok = false;
      continue;
    }
    unregister_owned_directory(entry.path);
  }
  {
    std::lock_guard lock(owned_directory_mutex);
    std::erase_if(temp_roots_by_tmpdir, [&](auto const& cached) {
      return std::none_of(owned_directories.begin(), owned_directories.end(), [&](OwnedDirectory const& entry) { return entry.path == cached.second; });
    });
  }
  return ok;
}

std::filesystem::path temp_root()
{
  auto const tmpdir = std::filesystem::temp_directory_path();
  {
    std::lock_guard lock(owned_directory_mutex);
    for (auto const& cached : temp_roots_by_tmpdir)
    {
      if (cached.first == tmpdir)
        return cached.second;
    }
  }
  auto root = create_unique_owned_directory(tmpdir, "ava_core_tests_");
  std::filesystem::path winner;
  bool created_this_thread = false;
  {
    std::lock_guard lock(owned_directory_mutex);
    for (auto const& cached : temp_roots_by_tmpdir)
    {
      if (cached.first == tmpdir)
      {
        winner = cached.second;
        break;
      }
    }
    if (winner.empty())
    {
      temp_roots_by_tmpdir.emplace_back(tmpdir, root);
      created_this_thread = true;
      winner = root;
    }
  }
  if (!created_this_thread)
  {
    // Another thread won the create race; drop the extra unique directory.
    std::string error_message;
    if (remove_path_without_following_symlinks(root, error_message))
      unregister_owned_directory(root);
  }
  return winner;
}

std::filesystem::path create_empty_root(std::filesystem::path root_name)
{
  auto const base = temp_root();

  // Create a directory <base>/physical/<root_name> and a symbolic link
  // <base>/logical -> <base>/physical so that the returned path
  // <base>/logical/<root_name> resolves to <base>/physical/<root_name>.
  auto const real_root = base / "physical" / root_name;
  std::error_code error;
  std::filesystem::remove_all(real_root, error);
  if (error)
    throw std::runtime_error("failed to clean test root: " + error.message());
  std::filesystem::create_directories(real_root, error);
  if (error)
    throw std::runtime_error("failed to create test root: " + error.message());
  if (::chmod(real_root.c_str(), S_IRWXU) != 0)
    throw std::system_error(errno, std::generic_category(), "set owner-only test root permissions");

  auto const link = base / "logical";
  std::filesystem::remove(link, error);
  if (error)
    throw std::runtime_error("failed to remove logical test root symlink: " + error.message());
  std::filesystem::create_symlink("physical", link, error);
  if (error)
    throw std::runtime_error("failed to create logical test root symlink: " + error.message());
  return link / root_name;
}

std::shared_ptr<ava::core::AnchorSet> command_anchors_for_test(std::filesystem::path const& workspace, std::filesystem::path const& spill_dir)
{
  std::error_code error;
  std::filesystem::create_directories(spill_dir, error);
  if (error || ::chmod(spill_dir.c_str(), S_IRWXU) != 0)
    throw std::runtime_error("failed to create command test spill anchor");
  auto anchors = ava::core::AnchorSet::open({workspace, spill_dir});
  if (!anchors || !(*anchors)->find_anchor(workspace) || !(*anchors)->find_anchor(spill_dir))
    throw std::runtime_error("failed to open command test AnchorSet");
  return *anchors;
}

ScopedEnvVar::ScopedEnvVar(std::string name, std::string value) : name_(std::move(name))
{
  if (char const* current = std::getenv(name_.c_str()))
  {
    previous_ = current;
  }
  setenv(name_.c_str(), value.c_str(), 1);
}

ScopedEnvVar::~ScopedEnvVar()
{
  if (previous_)
  {
    setenv(name_.c_str(), previous_->c_str(), 1);
  }
  else
  {
    unsetenv(name_.c_str());
  }
}

std::string strip_sgr(std::string_view text)
{
  std::string stripped;
  stripped.reserve(text.size());
  for (std::size_t index = 0; index < text.size();)
  {
    if (skip_osc_sequence(text, index))
    {
      continue;
    }
    if (text[index] == '\x1b' && index + 1 < text.size() && text[index + 1] == '[')
    {
      auto end = index + 2;
      while (end < text.size() && text[end] != 'm')
      {
        ++end;
      }
      if (end < text.size())
      {
        index = end + 1;
        continue;
      }
    }
    stripped.push_back(text[index]);
    ++index;
  }
  return stripped;
}

bool has_active_sgr_at_text(std::string_view line, std::string_view text, std::string_view sgr)
{
  auto const text_pos = line.find(text);
  if (text_pos == std::string_view::npos)
    return false;
  auto const sgr_pos = line.rfind(sgr, text_pos);
  if (sgr_pos == std::string_view::npos)
    return false;
  auto const reset_pos = line.rfind("\x1b[0m", text_pos);
  return reset_pos == std::string_view::npos || reset_pos < sgr_pos;
}

namespace {

std::mutex test_authority_mutex;
std::map<std::string, std::weak_ptr<ava::session::SessionAppendTarget>> test_append_targets;

ava::core::Result<ava::session::SessionLease> acquire_test_session_lease(ava::session::SessionStore const& store)
{
  if (auto valid = ava::session::validate_session_id(store.session_id()); !valid)
    return std::unexpected(std::move(valid.error()));
  auto lease = ava::session::SessionLease::acquire(store.session_path());
  if (!lease)
  {
    std::error_code exists_error;
    if (std::filesystem::exists(store.session_path(), exists_error) || exists_error)
      return std::unexpected(std::move(lease.error()));
    lease = ava::session::SessionLease::create_and_acquire(store.session_path());
  }
  return lease;
}

}  // namespace

ava::core::VoidResult append_session_entry_for_test(ava::session::SessionStore& store, ava::session::SessionEntry const& entry)
{
  // Test fixtures follow the same authority boundary as production for v4.
  // Tests that need malformed physical bytes write their fixture directly.
  if (entry.type == ava::session::EntryType::AssistantOutputItem || entry.type == ava::session::EntryType::AssistantTurnCommit)
  {
    if (store.is_ephemeral())
    {
      auto target = ava::session::SessionAppendTarget::create_ephemeral(store);
      return target ? (*target)->append(entry) : ava::core::VoidResult(std::unexpected(std::move(target.error())));
    }
    auto lease = acquire_test_session_lease(store);
    if (!lease)
      return std::unexpected(std::move(lease.error()));
    auto target = ava::session::SessionAppendTarget::create_persistent(store, *lease, ava::session::legacy_unbounded_session_read_limits());
    return target ? (*target)->append(entry) : ava::core::VoidResult(std::unexpected(std::move(target.error())));
  }
  if (store.is_ephemeral())
    return store.append_ephemeral(entry);
  auto lease = acquire_test_session_lease(store);
  if (!lease)
    return std::unexpected(std::move(lease.error()));
  return store.append(*lease, entry);
}

std::function<ava::core::VoidResult(ava::session::SessionEntry const&)> append_route_for_test(ava::session::SessionStore const& store)
{
  ava::core::Result<std::shared_ptr<ava::session::SessionAppendTarget>> target =
      store.is_ephemeral() ? ava::session::SessionAppendTarget::create_ephemeral(store)
                           : [&]() -> ava::core::Result<std::shared_ptr<ava::session::SessionAppendTarget>> {
    auto lease = acquire_test_session_lease(store);
    if (!lease)
      return std::unexpected(std::move(lease.error()));
    return ava::session::SessionAppendTarget::create_persistent(store, *lease, ava::session::legacy_unbounded_session_read_limits());
  }();
  if (target)
  {
    auto retained_target = std::move(*target);
    {
      std::lock_guard lock(test_authority_mutex);
      test_append_targets[store.session_path().string()] = retained_target;
    }
    return [retained_target = std::move(retained_target)](ava::session::SessionEntry const& entry) { return retained_target->append(entry); };
  }
  auto const message = target.error().format();
  return [message](ava::session::SessionEntry const&) {
    return ava::core::VoidResult(std::unexpected(ava::core::Error(ava::core::ErrorCategory::Session, message)));
  };
}

std::function<ava::core::VoidResult(std::vector<ava::session::SessionEntry>)> append_batch_route_for_test(ava::session::SessionStore const& store)
{
  std::shared_ptr<ava::session::SessionAppendTarget> target;
  {
    std::lock_guard lock(test_authority_mutex);
    auto const found = test_append_targets.find(store.session_path().string());
    if (found != test_append_targets.end())
      target = found->second.lock();
  }
  if (target)
    return [target = std::move(target)](std::vector<ava::session::SessionEntry> entries) { return target->append_batch(std::move(entries)); };
  return [](std::vector<ava::session::SessionEntry>) {
    return ava::core::VoidResult(
        std::unexpected(ava::core::Error(ava::core::ErrorCategory::Session, "test batch append route requires append_route_for_test to be initialized first")));
  };
}

ava::session::SessionReadAuthority read_authority_for_test(ava::session::SessionStore const& store)
{
  std::shared_ptr<ava::session::SessionAppendTarget> target;
  {
    std::lock_guard lock(test_authority_mutex);
    auto const found = test_append_targets.find(store.session_path().string());
    if (found != test_append_targets.end())
      target = found->second.lock();
  }
  if (!target)
    throw std::runtime_error("test session read authority requires a retained append_route_for_test");
  auto read_authority = target->read_authority();
  if (!read_authority)
    throw std::runtime_error(read_authority.error().format());
  return std::move(*read_authority);
}

ava::core::Result<ava::session::SessionMetadataView> append_session_metadata_for_test(ava::session::SessionStore& store,
                                                                                      ava::session::SessionMetadataUpdate update)
{
  if (store.is_ephemeral())
    return ava::session::append_session_metadata_ephemeral(store, std::move(update));
  auto lease = acquire_test_session_lease(store);
  if (!lease)
    return std::unexpected(std::move(lease.error()));
  return ava::session::append_session_metadata(store, *lease, std::move(update));
}

ava::core::VoidResult append_manual_compaction_for_test(ava::session::SessionStore& store, ava::session::ManualCompactionRequest request)
{
  if (store.is_ephemeral())
    return ava::session::append_manual_compaction_ephemeral(store, std::move(request));
  auto lease = acquire_test_session_lease(store);
  if (!lease)
    return std::unexpected(std::move(lease.error()));
  return ava::session::append_manual_compaction(store, *lease, std::move(request));
}

ava::core::VoidResult append_permission_audit_for_test(ava::session::SessionStore& store, ava::tools::PermissionAuditEvent const& event)
{
  return append_session_entry_for_test(store, ava::session::SessionEntry{.id = ava::core::make_id("entry"),
                                                                         .parent_id = "",
                                                                         .type = ava::session::EntryType::PermissionDecision,
                                                                         .timestamp = ava::session::now_timestamp(),
                                                                         .data_json = ava::tools::permission_audit_data_json(event)});
}

std::vector<ava::session::SessionEntry> permission_entries(std::vector<ava::session::SessionEntry> const& entries)
{
  std::vector<ava::session::SessionEntry> filtered;
  for (auto const& entry : entries)
  {
    if (entry.type == ava::session::EntryType::PermissionDecision)
      filtered.push_back(entry);
  }
  return filtered;
}

std::size_t visible_columns(std::string_view text)
{
  auto const stripped = strip_sgr(text);
  std::size_t columns = 0;
  for (std::size_t index = 0; index < stripped.size();)
  {
    auto const byte = static_cast<unsigned char>(stripped[index]);
    char32_t codepoint = 0;
    std::size_t length = 1;
    if ((byte & 0x80U) == 0)
    {
      codepoint = byte;
    }
    else if (byte >= 0xC2U && byte <= 0xDFU)
    {
      codepoint = byte & 0x1FU;
      length = 2;
    }
    else if ((byte & 0xF0U) == 0xE0U)
    {
      codepoint = byte & 0x0FU;
      length = 3;
    }
    else if (byte >= 0xF0U && byte <= 0xF4U)
    {
      codepoint = byte & 0x07U;
      length = 4;
    }
    if (index + length > stripped.size())
    {
      ++columns;
      break;
    }
    bool valid = length == 1;
    for (std::size_t offset = 1; offset < length; ++offset)
    {
      auto const continuation = static_cast<unsigned char>(stripped[index + offset]);
      valid = (continuation & 0xC0U) == 0x80U;
      if (!valid)
        break;
      codepoint = (codepoint << 6U) | (continuation & 0x3FU);
    }
    if (!valid)
    {
      ++columns;
      ++index;
      continue;
    }
    if (is_zero_width_codepoint(codepoint))
    {
      index += length;
      continue;
    }
    auto const width = codepoint <= static_cast<char32_t>(WCHAR_MAX) ? ::wcwidth(static_cast<wchar_t>(codepoint)) : 1;
    auto const fallback_wide =
        (codepoint >= 0x1100 && codepoint <= 0x115F) || (codepoint >= 0x2329 && codepoint <= 0x232A) || (codepoint >= 0x2E80 && codepoint <= 0xA4CF) ||
        (codepoint >= 0xAC00 && codepoint <= 0xD7A3) || (codepoint >= 0xF900 && codepoint <= 0xFAFF) || (codepoint >= 0xFE10 && codepoint <= 0xFE19) ||
        (codepoint >= 0xFE30 && codepoint <= 0xFE6F) || (codepoint >= 0xFF00 && codepoint <= 0xFF60) || (codepoint >= 0xFFE0 && codepoint <= 0xFFE6) ||
        (codepoint >= 0x1F300 && codepoint <= 0x1FAFF) || (codepoint >= 0x20000 && codepoint <= 0x3FFFD);
    columns += width > 0 ? static_cast<std::size_t>(width) : (fallback_wide ? std::size_t{2} : std::size_t{1});
    index += length;
  }
  return columns;
}

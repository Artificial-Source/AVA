#include "sys.h"
#include "ava/session/record.h"
#include "ava/session/session_store_internal.h"
#include "ava/core/json.h"
#include "ava/core/path.h"
#include "ava/core/strict_json.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace ava::session {
namespace detail {

ava::core::Result<std::pair<int, off_t>> open_regular_snapshot(std::filesystem::path const& path, std::size_t max_file_bytes)
{
  // Inspect the final component with O_PATH first. In particular, never open a
  // pathname replaced by a FIFO through blocking O_RDONLY before classifying it.
  ScopedFd metadata_fd(::open(path.c_str(), O_PATH | O_CLOEXEC | O_NOFOLLOW));
  if (metadata_fd.get() < 0)
  {
    auto category =
        errno == ENOENT ? ava::core::ErrorCategory::NotFound : (errno == ELOOP ? ava::core::ErrorCategory::PermissionDenied : ava::core::ErrorCategory::Io);
    auto error = ava::core::Error(category, errno == ENOENT ? "session not found" : "failed to securely inspect session file");
    error.with_context("path", path.string()).with_context("cause", std::strerror(errno));
    return std::unexpected(std::move(error));
  }
  struct stat metadata_status{};
  int metadata_error = fstat(metadata_fd.get(), &metadata_status) == 0 ? 0 : errno;
  int metadata_close_error = metadata_fd.close_checked();
  if (metadata_error != 0)
    return std::unexpected(path_io_error("failed to inspect session file metadata", path, metadata_error));
  if (metadata_close_error != 0)
    return std::unexpected(path_io_error("failed to close session file metadata descriptor", path, metadata_close_error));
  if (!S_ISREG(metadata_status.st_mode))
  {
    if (S_ISLNK(metadata_status.st_mode))
    {
      auto error = ava::core::Error(ava::core::ErrorCategory::PermissionDenied, "session path must not be a symlink");
      error.with_context("path", path.string());
      return std::unexpected(std::move(error));
    }
    return std::unexpected(path_io_error("session path is not a regular file", path));
  }
  if (metadata_status.st_size < 0)
    return std::unexpected(path_io_error("session snapshot has an invalid size", path));
  auto const size = static_cast<std::uintmax_t>(metadata_status.st_size);
  if (size > max_file_bytes)
  {
    auto error = ava::core::Error(ava::core::ErrorCategory::Session, "session file exceeds bounded read limit");
    error.with_context("path", path.string()).with_context("max_file_bytes", std::to_string(max_file_bytes));
    return std::unexpected(std::move(error));
  }

  ScopedFd fd(::open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW));
  if (fd.get() < 0)
    return std::unexpected(path_io_error("failed to securely open session snapshot", path, errno));
  struct stat status{};
  int status_error = fstat(fd.get(), &status) == 0 ? 0 : errno;
  if (status_error != 0 || !S_ISREG(status.st_mode) || metadata_status.st_dev != status.st_dev || metadata_status.st_ino != status.st_ino)
  {
    if (status_error != 0)
      return std::unexpected(path_io_error("failed to inspect session snapshot", path, status_error));
    return std::unexpected(path_io_error("session snapshot target changed while opening", path));
  }
  return std::pair<int, off_t>(fd.release(), metadata_status.st_size);
}

}  // namespace detail

namespace {

using detail::open_regular_snapshot;
using detail::parse_strict_session_record;
using detail::path_io_error;
using detail::ScopedFd;
using detail::strict_session_record_error;

void wipe_bytes(void* data, std::size_t size) noexcept
{
  auto* cursor = static_cast<unsigned char volatile*>(data);
  while (size-- > 0) *cursor++ = 0;
}

class ByteVectorWiper final
{
 public:
  explicit ByteVectorWiper(std::vector<std::uint8_t>& bytes) noexcept : bytes_(bytes) { }
  ~ByteVectorWiper() noexcept { wipe_bytes(bytes_.data(), bytes_.size()); }
  ByteVectorWiper(ByteVectorWiper const&) = delete;
  ByteVectorWiper& operator=(ByteVectorWiper const&) = delete;

 private:
  std::vector<std::uint8_t>& bytes_;
};

void wipe_string(std::string& value) noexcept
{
  wipe_bytes(value.data(), value.size());
  value.clear();
}

void wipe_session_entry(SessionEntry& entry) noexcept
{
  wipe_string(entry.id);
  wipe_string(entry.parent_id);
  wipe_string(entry.timestamp);
  wipe_string(entry.data_json);
}

class StringWiper final
{
 public:
  explicit StringWiper(std::string& value) noexcept : value_(value) { }
  ~StringWiper() noexcept { wipe_string(value_); }
  StringWiper(StringWiper const&) = delete;
  StringWiper& operator=(StringWiper const&) = delete;

 private:
  std::string& value_;
};

template <std::size_t Size>
class CharArrayWiper final
{
 public:
  explicit CharArrayWiper(std::array<char, Size>& value) noexcept : value_(value) { }
  ~CharArrayWiper() noexcept { wipe_bytes(value_.data(), value_.size()); }
  CharArrayWiper(CharArrayWiper const&) = delete;
  CharArrayWiper& operator=(CharArrayWiper const&) = delete;

 private:
  std::array<char, Size>& value_;
};

class SessionEntryWiper final
{
 public:
  explicit SessionEntryWiper(SessionEntry& entry) noexcept : entry_(entry) { }
  ~SessionEntryWiper() noexcept { wipe_session_entry(entry_); }
  SessionEntryWiper(SessionEntryWiper const&) = delete;
  SessionEntryWiper& operator=(SessionEntryWiper const&) = delete;

 private:
  SessionEntry& entry_;
};

class SessionEntryVectorWiper final
{
 public:
  explicit SessionEntryVectorWiper(std::vector<SessionEntry>& entries) noexcept : entries_(entries) { }
  ~SessionEntryVectorWiper() noexcept
  {
    if (!active_)
      return;
    for (auto& entry : entries_) wipe_session_entry(entry);
    entries_.clear();
  }
  SessionEntryVectorWiper(SessionEntryVectorWiper const&) = delete;
  SessionEntryVectorWiper& operator=(SessionEntryVectorWiper const&) = delete;
  void dismiss() noexcept { active_ = false; }

 private:
  std::vector<SessionEntry>& entries_;
  bool active_ = true;
};

std::uint32_t rotate_right(std::uint32_t value, std::uint32_t count) noexcept
{
  return (value >> count) | (value << (32U - count));
}

std::array<std::uint8_t, 32> sha256(std::vector<std::uint8_t> const& bytes)
{
  static constexpr std::array<std::uint32_t, 64> constants{
      0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U, 0xd807aa98U, 0x12835b01U, 0x243185beU,
      0x550c7dc3U, 0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U, 0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU, 0x2de92c6fU, 0x4a7484aaU,
      0x5cb0a9dcU, 0x76f988daU, 0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U, 0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U, 0x27b70a85U,
      0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U, 0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U, 0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U,
      0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U, 0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU,
      0x682e6ff3U, 0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U, 0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U};

  std::vector<std::uint8_t> data = bytes;
  ByteVectorWiper wipe_data(data);
  auto const bit_length = static_cast<std::uint64_t>(data.size()) * 8ULL;
  data.push_back(0x80U);
  while ((data.size() % 64U) != 56U) data.push_back(0U);
  for (int shift = 56; shift >= 0; shift -= 8) data.push_back(static_cast<std::uint8_t>((bit_length >> shift) & 0xffU));

  std::array<std::uint32_t, 8> hash{0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU, 0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U};
  for (std::size_t chunk = 0; chunk < data.size(); chunk += 64U)
  {
    std::array<std::uint32_t, 64> words{};
    for (std::size_t index = 0; index < 16U; ++index)
    {
      auto const offset = chunk + index * 4U;
      words[index] = (static_cast<std::uint32_t>(data[offset]) << 24U) | (static_cast<std::uint32_t>(data[offset + 1U]) << 16U) |
                     (static_cast<std::uint32_t>(data[offset + 2U]) << 8U) | static_cast<std::uint32_t>(data[offset + 3U]);
    }
    for (std::size_t index = 16U; index < words.size(); ++index)
    {
      auto const s0 = rotate_right(words[index - 15U], 7U) ^ rotate_right(words[index - 15U], 18U) ^ (words[index - 15U] >> 3U);
      auto const s1 = rotate_right(words[index - 2U], 17U) ^ rotate_right(words[index - 2U], 19U) ^ (words[index - 2U] >> 10U);
      words[index] = words[index - 16U] + s0 + words[index - 7U] + s1;
    }
    auto a = hash[0];
    auto b = hash[1];
    auto c = hash[2];
    auto d = hash[3];
    auto e = hash[4];
    auto f = hash[5];
    auto g = hash[6];
    auto h = hash[7];
    for (std::size_t index = 0; index < words.size(); ++index)
    {
      auto const s1 = rotate_right(e, 6U) ^ rotate_right(e, 11U) ^ rotate_right(e, 25U);
      auto const choose = (e & f) ^ ((~e) & g);
      auto const temporary_one = h + s1 + choose + constants[index] + words[index];
      auto const s0 = rotate_right(a, 2U) ^ rotate_right(a, 13U) ^ rotate_right(a, 22U);
      auto const majority = (a & b) ^ (a & c) ^ (b & c);
      auto const temporary_two = s0 + majority;
      h = g;
      g = f;
      f = e;
      e = d + temporary_one;
      d = c;
      c = b;
      b = a;
      a = temporary_one + temporary_two;
    }
    hash[0] += a;
    hash[1] += b;
    hash[2] += c;
    hash[3] += d;
    hash[4] += e;
    hash[5] += f;
    hash[6] += g;
    hash[7] += h;
  }

  std::array<std::uint8_t, 32> digest{};
  for (std::size_t index = 0; index < hash.size(); ++index)
  {
    digest[index * 4U] = static_cast<std::uint8_t>((hash[index] >> 24U) & 0xffU);
    digest[index * 4U + 1U] = static_cast<std::uint8_t>((hash[index] >> 16U) & 0xffU);
    digest[index * 4U + 2U] = static_cast<std::uint8_t>((hash[index] >> 8U) & 0xffU);
    digest[index * 4U + 3U] = static_cast<std::uint8_t>(hash[index] & 0xffU);
  }
  return digest;
}

bool exact_snapshot_metadata_matches(struct stat const& before, struct stat const& after) noexcept
{
  if (before.st_size != after.st_size)
    return false;
#if defined(__linux__)
  return before.st_mtim.tv_sec == after.st_mtim.tv_sec && before.st_mtim.tv_nsec == after.st_mtim.tv_nsec && before.st_ctim.tv_sec == after.st_ctim.tv_sec &&
         before.st_ctim.tv_nsec == after.st_ctim.tv_nsec;
#else
  return before.st_mtime == after.st_mtime && before.st_ctime == after.st_ctime;
#endif
}

ava::core::VoidResult visit_session_snapshot_fd(int fd, off_t snapshot_size, std::filesystem::path const& path, SessionReadLimits const& limits,
                                                SessionEntryVisitor const& visitor, SessionCancelCallback const& cancel_requested,
                                                bool tolerate_invalid_final_suffix, std::function<void()> const* after_first_read_for_test = nullptr,
                                                std::vector<std::uint8_t>* exact_bytes = nullptr)
{
  std::array<char, 8192> buffer{};
  CharArrayWiper wipe_buffer(buffer);
  std::string line;
  StringWiper wipe_line(line);
  line.reserve(std::min<std::size_t>(limits.max_line_bytes, buffer.size()));
  std::size_t entry_count = 0;
  auto consume_line = [&](bool final_unterminated) -> ava::core::Result<bool> {
    auto const strict_status = ava::core::validate_strict_json(line, ava::core::json::kMaxNestingDepth);
    if (tolerate_invalid_final_suffix && final_unterminated && strict_status == ava::core::StrictJsonStatus::Invalid)
    {
      wipe_string(line);
      return false;
    }
    if (strict_status != ava::core::StrictJsonStatus::Valid)
      return std::unexpected(strict_session_record_error(strict_status, path, final_unterminated));
    if (++entry_count > limits.max_entries)
    {
      auto error = ava::core::Error(ava::core::ErrorCategory::Session, "session entry count exceeds bounded read limit");
      error.with_context("max_entries", std::to_string(limits.max_entries));
      return std::unexpected(std::move(error));
    }
    auto entry = parse_strict_session_record(line, path, final_unterminated);
    if (!entry)
      return std::unexpected(std::move(entry.error()));
    SessionEntryWiper wipe_entry(*entry);
    auto keep_going = visitor(*entry);
    if (!keep_going)
      return std::unexpected(std::move(keep_going.error()));
    wipe_string(line);
    return *keep_going;
  };

  off_t offset = 0;
  bool ran_read_hook = false;
  while (offset < snapshot_size)
  {
    if (cancel_requested && cancel_requested())
      return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Unknown, "session read canceled", ava::core::ErrorCode::Canceled));
    auto const remaining = snapshot_size - offset;
    auto const wanted = remaining < static_cast<off_t>(buffer.size()) ? static_cast<std::size_t>(remaining) : buffer.size();
    ssize_t count = 0;
    do
    {
      count = ::pread(fd, buffer.data(), wanted, offset);
    } while (count < 0 && errno == EINTR);
    if (count < 0)
      return std::unexpected(path_io_error("failed while reading session file", path, errno));
    if (count == 0 || static_cast<std::size_t>(count) > wanted)
      return std::unexpected(path_io_error("session file shrank or became unreadable during bounded read", path));
    offset += count;
    if (exact_bytes != nullptr)
    {
      auto const* begin = reinterpret_cast<std::uint8_t const*>(buffer.data());
      exact_bytes->insert(exact_bytes->end(), begin, begin + count);
    }

    if (!ran_read_hook && after_first_read_for_test != nullptr && *after_first_read_for_test)
    {
      ran_read_hook = true;
      try
      {
        (*after_first_read_for_test)();
      }
      catch (...)
      {
        return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Unknown, "lease-bound session read test hook failed"));
      }
    }

    for (ssize_t index = 0; index < count; ++index)
    {
      char const ch = buffer[static_cast<std::size_t>(index)];
      if (ch == '\n')
      {
        auto keep_going = consume_line(false);
        if (!keep_going)
          return std::unexpected(std::move(keep_going.error()));
        if (!*keep_going)
          return {};
      }
      else
      {
        if (line.size() >= limits.max_line_bytes)
        {
          auto error = ava::core::Error(ava::core::ErrorCategory::Session, "session line exceeds bounded read limit");
          error.with_context("path", path.string()).with_context("max_line_bytes", std::to_string(limits.max_line_bytes));
          return std::unexpected(std::move(error));
        }
        line.push_back(ch);
      }
    }
  }
  if (!line.empty())
  {
    auto keep_going = consume_line(true);
    if (!keep_going)
      return std::unexpected(std::move(keep_going.error()));
  }
  return {};
}

ava::core::VoidResult visit_regular_session_snapshot(std::filesystem::path const& path, SessionReadLimits const& limits, SessionEntryVisitor const& visitor,
                                                     SessionCancelCallback const& cancel_requested, bool tolerate_invalid_final_suffix)
{
  auto opened = open_regular_snapshot(path, limits.max_file_bytes);
  if (!opened)
    return std::unexpected(std::move(opened.error()));
  ScopedFd fd(opened->first);
  auto visited = visit_session_snapshot_fd(fd.get(), opened->second, path, limits, visitor, cancel_requested, tolerate_invalid_final_suffix);
  int const close_error = fd.close_checked();
  if (!visited)
    return std::unexpected(std::move(visited.error()));
  if (close_error != 0)
    return std::unexpected(path_io_error("failed to close session snapshot", path, close_error));
  return {};
}

}  // namespace

using ava::core::normalized_absolute_path;
using detail::append_authority_error;
using detail::append_mutex_for_path;
using detail::path_io_error;
using detail::same_directory_identity;
using detail::same_file_identity;
using detail::ScopedFd;
using detail::SessionTraceScope;
using detail::validate_read_limits;

ava::core::Result<std::vector<SessionEntry>> SessionStore::load() const
{
  std::optional<SessionTraceScope> trace;
  if (observation_attachment_->enabled.load(std::memory_order_acquire))
  {
    std::shared_ptr<ava::observability::RunObservation> observation;
    try
    {
      std::shared_ptr<ava::observability::TraceContext const> context;
      {
        std::lock_guard lock(observation_attachment_->mutex);
        observation = observation_attachment_->observation;
        context = observation_attachment_->context;
      }
      if (observation && observation->enabled() && context)
      {
        trace.emplace(std::move(observation), std::move(context), ava::observability::TraceEventType::SessionLoadAttempt,
                      ava::observability::TraceEventType::SessionLoadResult, std::nullopt, is_ephemeral());
      }
    }
    catch (...)
    {
      if (observation)
        observation->account_external_failure();
    }
  }
  if (auto valid_session_id = validate_session_id(options_.session_id); !valid_session_id)
  {
    return std::unexpected(std::move(valid_session_id.error()));
  }

  if (ephemeral_state_)
  {
    std::lock_guard lock(ephemeral_state_->entries_mutex);
    auto entries = ephemeral_state_->entries;
    if (trace)
      trace->succeed(entries.size());
    return entries;
  }

  auto const path = session_path();
  auto& append_mutex = append_mutex_for_path(path);
  std::lock_guard append_lock(append_mutex);

  std::error_code status_error;
  auto const status = std::filesystem::symlink_status(path, status_error);
  if (status_error || !std::filesystem::exists(status))
  {
    if (trace)
      trace->succeed();
    return std::vector<SessionEntry>{};
  }
  if (std::filesystem::is_symlink(status))
  {
    auto error = ava::core::Error(ava::core::ErrorCategory::PermissionDenied, "session path must not be a symlink");
    error.with_context("session_id", session_id());
    error.with_context("path", path.string());
    return std::unexpected(std::move(error));
  }
  if (!std::filesystem::is_regular_file(status))
  {
    auto error = ava::core::Error(ava::core::ErrorCategory::Session, "session path is not a regular file");
    error.with_context("session_id", session_id());
    error.with_context("path", path.string());
    return std::unexpected(std::move(error));
  }

  std::ifstream file(path);
  if (!file)
  {
    auto error = ava::core::Error(ava::core::ErrorCategory::Io, "failed to open session file");
    error.with_context("session_id", session_id());
    error.with_context("path", path.string());
    return std::unexpected(std::move(error));
  }

  std::vector<SessionEntry> entries;
  std::string line;
  while (true)
  {
    auto line_read = read_limited_session_line(file, line);
    if (!line_read)
      return std::unexpected(line_read.error());
    if (!*line_read)
      break;
    auto entry = parse_session_entry_line(line, path);
    if (!entry)
      return std::unexpected(std::move(entry.error()));
    entries.push_back(std::move(*entry));
  }
  if (trace)
    trace->succeed(entries.size());
  return entries;
}

ava::core::VoidResult SessionStore::visit_entries(SessionReadLimits limits, SessionEntryVisitor const& visitor, SessionCancelCallback cancel_requested) const
{
  if (auto valid = validate_read_limits(limits); !valid)
    return valid;
  if (!visitor)
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "session entry visitor is required"));
  if (auto valid_session_id = validate_session_id(options_.session_id); !valid_session_id)
    return valid_session_id;

  if (ephemeral_state_)
  {
    std::vector<SessionEntry> snapshot;
    {
      std::lock_guard lock(ephemeral_state_->entries_mutex);
      if (ephemeral_state_->entries.size() > limits.max_entries)
        return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Session, "session entry count exceeds bounded read limit"));
      snapshot = ephemeral_state_->entries;
    }
    for (auto const& entry : snapshot)
    {
      if (cancel_requested && cancel_requested())
        return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Unknown, "session read canceled", ava::core::ErrorCode::Canceled));
      auto keep_going = visitor(entry);
      if (!keep_going)
        return std::unexpected(std::move(keep_going.error()));
      if (!*keep_going)
        break;
    }
    return {};
  }

  // Reads are pinned to one descriptor/fstat byte snapshot. No registry or
  // append mutex is held across open/read/parse; a concurrently incomplete
  // trailing record fails this transient read rather than being accepted.
  return visit_regular_session_snapshot(session_path(), limits, visitor, cancel_requested, false);
}

ava::core::VoidResult SessionStore::visit_entries_leased(SessionLease const& lease, SessionReadLimits limits, SessionEntryVisitor const& visitor,
                                                         SessionCancelCallback cancel_requested, bool invoke_after_lease_bound_read_test_hook,
                                                         bool tolerate_invalid_final_suffix, SessionByteFingerprint* exact_fingerprint) const
{
  if (auto valid = validate_read_limits(limits); !valid)
    return valid;
  if (!visitor)
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "session entry visitor is required"));
  if (auto valid_session_id = validate_session_id(options_.session_id); !valid_session_id)
    return valid_session_id;
  if (ephemeral_state_)
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "lease-bound reads require a persistent session store"));
  if (!lease.active())
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "lease-bound session read requires an active session lease"));

  auto const path = normalized_absolute_path(session_path());
  if (path != lease.canonical_path())
  {
    auto error = append_authority_error("session lease does not exactly match the persistent read target", path);
    error.with_context("lease_path", lease.canonical_path().string());
    return std::unexpected(std::move(error));
  }
  auto const parent_path = path.parent_path();
  auto const session_name = path.filename().string();
  if (session_name.empty())
    return std::unexpected(path_io_error("lease-bound session read target has no basename", path));

  ScopedFd parent_fd(::open(parent_path.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
  if (parent_fd.get() < 0)
    return std::unexpected(path_io_error("failed to anchor lease-bound session read directory", parent_path, errno));

  struct stat initial_parent_status{};
  if (fstat(parent_fd.get(), &initial_parent_status) != 0 || !S_ISDIR(initial_parent_status.st_mode))
  {
    int const error_number = errno;
    return std::unexpected(path_io_error("lease-bound session read parent is not a directory", parent_path, error_number));
  }

  struct ReadIdentity
  {
    struct stat lease_status{};
    struct stat named_status{};
  };
  auto validate_identity = [&](std::string_view timing) -> ava::core::Result<ReadIdentity> {
    ReadIdentity identity;
    if (fstat(lease.fd_, &identity.lease_status) != 0)
      return std::unexpected(path_io_error("failed to inspect session lease " + std::string(timing), path, errno));
    if (fstatat(parent_fd.get(), session_name.c_str(), &identity.named_status, AT_SYMLINK_NOFOLLOW) != 0)
      return std::unexpected(path_io_error("failed to inspect leased session name " + std::string(timing), path, errno));
    if (!same_file_identity(identity.lease_status, identity.named_status) || identity.lease_status.st_nlink != 1 || identity.named_status.st_nlink != 1)
    {
      auto error = ava::core::Error(ava::core::ErrorCategory::Session, "lease-bound session read target was replaced " + std::string(timing));
      error.with_context("timing", std::string(timing));
      error.with_context("lease_links", std::to_string(identity.lease_status.st_nlink));
      error.with_context("name_links", std::to_string(identity.named_status.st_nlink));
      return std::unexpected(std::move(error));
    }

    struct stat anchored_parent_status{};
    if (fstat(parent_fd.get(), &anchored_parent_status) != 0)
      return std::unexpected(path_io_error("failed to inspect anchored session directory " + std::string(timing), parent_path, errno));
    if (!same_directory_identity(initial_parent_status, anchored_parent_status))
    {
      auto error = ava::core::Error(ava::core::ErrorCategory::Session, "anchored session directory changed during lease-bound read");
      error.with_context("timing", std::string(timing)).with_context("path", parent_path.string());
      return std::unexpected(std::move(error));
    }

    ScopedFd publication_fd(::open(parent_path.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    if (publication_fd.get() < 0)
      return std::unexpected(path_io_error("failed to re-open session directory publication " + std::string(timing), parent_path, errno));
    struct stat publication_status{};
    int const publication_error = fstat(publication_fd.get(), &publication_status) == 0 ? 0 : errno;
    int const publication_close_error = publication_fd.close_checked();
    if (publication_error != 0)
      return std::unexpected(path_io_error("failed to inspect session directory publication " + std::string(timing), parent_path, publication_error));
    if (publication_close_error != 0)
      return std::unexpected(path_io_error("failed to close session directory publication " + std::string(timing), parent_path, publication_close_error));
    if (!same_directory_identity(initial_parent_status, publication_status))
    {
      auto error = ava::core::Error(ava::core::ErrorCategory::Session, "session directory publication changed during lease-bound read");
      error.with_context("timing", std::string(timing)).with_context("path", parent_path.string());
      return std::unexpected(std::move(error));
    }
    return identity;
  };

  auto initial = validate_identity("before snapshot");
  if (!initial)
    return std::unexpected(std::move(initial.error()));
  if (initial->lease_status.st_size < 0)
    return std::unexpected(path_io_error("lease-bound session snapshot has an invalid size", path));
  auto const initial_size = initial->lease_status.st_size;
  if (static_cast<std::uintmax_t>(initial_size) > limits.max_file_bytes)
  {
    auto error = ava::core::Error(ava::core::ErrorCategory::Session, "session file exceeds bounded read limit");
    error.with_context("path", path.string()).with_context("max_file_bytes", std::to_string(limits.max_file_bytes));
    return std::unexpected(std::move(error));
  }

  std::vector<std::uint8_t> exact_bytes;
  ByteVectorWiper wipe_exact_bytes(exact_bytes);
  if (exact_fingerprint != nullptr)
    exact_bytes.reserve(static_cast<std::size_t>(initial_size));
  auto visited =
      visit_session_snapshot_fd(lease.fd_, initial_size, path, limits, visitor, cancel_requested, tolerate_invalid_final_suffix,
                                invoke_after_lease_bound_read_test_hook && after_lease_bound_read_for_test_ ? &after_lease_bound_read_for_test_ : nullptr,
                                exact_fingerprint != nullptr ? &exact_bytes : nullptr);
  auto final = validate_identity("after snapshot");
  bool const exact_changed = exact_fingerprint != nullptr && final &&
                             (!exact_snapshot_metadata_matches(initial->lease_status, final->lease_status) ||
                              !exact_snapshot_metadata_matches(initial->named_status, final->named_status));
  if (final && (!same_file_identity(initial->lease_status, final->lease_status) || final->lease_status.st_size < initial_size ||
                final->named_status.st_size < initial_size || exact_changed))
  {
    auto error = ava::core::Error(ava::core::ErrorCategory::Session, exact_fingerprint != nullptr ? "leased session content changed during exact snapshot"
                                                                                                  : "leased session inode shrank or changed during snapshot");
    error.with_context("path", path.string());
    final = std::unexpected(std::move(error));
  }
  int const parent_close_error = parent_fd.close_checked();
  if (!final)
    return std::unexpected(std::move(final.error()));
  if (!visited)
    return std::unexpected(std::move(visited.error()));
  if (parent_close_error != 0)
    return std::unexpected(path_io_error("failed to close lease-bound session read directory", parent_path, parent_close_error));
  if (exact_fingerprint != nullptr)
  {
    if (exact_bytes.size() != static_cast<std::size_t>(initial_size))
      return std::unexpected(path_io_error("exact leased session snapshot byte count changed", path));
    *exact_fingerprint = SessionByteFingerprint{.byte_count = static_cast<std::uint64_t>(exact_bytes.size()), .sha256 = sha256(exact_bytes)};
  }
  return {};
}

ava::core::Result<std::vector<SessionEntry>> SessionStore::load(SessionLease const& lease) const
{
  return load_bounded(lease, legacy_unbounded_session_read_limits());
}

ava::core::Result<std::vector<SessionEntry>> SessionStore::load_bounded(SessionReadLimits limits, SessionCancelCallback cancel_requested) const
{
  std::vector<SessionEntry> entries;
  SessionEntryVectorWiper wipe_entries(entries);
  entries.reserve(std::min<std::size_t>(limits.max_entries, 256));
  auto visited = visit_entries(
      limits,
      [&](SessionEntry const& entry) -> ava::core::Result<bool> {
        entries.push_back(entry);
        return true;
      },
      std::move(cancel_requested));
  if (!visited)
    return std::unexpected(std::move(visited.error()));
  wipe_entries.dismiss();
  return entries;
}

ava::core::Result<std::vector<SessionEntry>> SessionStore::load_bounded(SessionLease const& lease, SessionReadLimits limits,
                                                                        SessionCancelCallback cancel_requested) const
{
  std::vector<SessionEntry> entries;
  SessionEntryVectorWiper wipe_entries(entries);
  entries.reserve(std::min<std::size_t>(limits.max_entries, 256));
  auto visited = visit_entries_leased(
      lease, limits,
      [&](SessionEntry const& entry) -> ava::core::Result<bool> {
        entries.push_back(entry);
        return true;
      },
      std::move(cancel_requested));
  if (!visited)
    return std::unexpected(std::move(visited.error()));
  wipe_entries.dismiss();
  return entries;
}

ava::core::Result<std::vector<SessionEntry>> SessionStore::load_recoverable_prefix_bounded(SessionLease const& lease, SessionReadLimits limits,
                                                                                           SessionCancelCallback cancel_requested) const
{
  std::vector<SessionEntry> entries;
  SessionEntryVectorWiper wipe_entries(entries);
  entries.reserve(std::min<std::size_t>(limits.max_entries, 256));
  auto visited = visit_entries_leased(
      lease, limits,
      [&](SessionEntry const& entry) -> ava::core::Result<bool> {
        entries.push_back(entry);
        return true;
      },
      std::move(cancel_requested), true, true);
  if (!visited)
    return std::unexpected(std::move(visited.error()));
  wipe_entries.dismiss();
  return entries;
}

ava::core::Result<SessionRecoverableSnapshot> SessionStore::load_recoverable_snapshot_bounded(SessionLease const& lease, SessionReadLimits limits,
                                                                                              SessionCancelCallback cancel_requested) const
{
  SessionRecoverableSnapshot snapshot;
  SessionEntryVectorWiper wipe_entries(snapshot.entries);
  snapshot.entries.reserve(std::min<std::size_t>(limits.max_entries, 256));
  auto visited = visit_entries_leased(
      lease, limits,
      [&](SessionEntry const& entry) -> ava::core::Result<bool> {
        snapshot.entries.push_back(entry);
        return true;
      },
      std::move(cancel_requested), true, true, &snapshot.fingerprint);
  if (!visited)
    return std::unexpected(std::move(visited.error()));
  wipe_entries.dismiss();
  return snapshot;
}

ava::core::Result<SessionSummary> SessionStore::inspect_bounded(SessionReadLimits limits, SessionCancelCallback cancel_requested) const
{
  SessionSummary summary{.session_id = session_id(), .path = session_path(), .last_updated = {}, .entry_count = 0, .original_cwd = {}, .title = {}};
  bool has_manual_title = false;
  auto visited = visit_entries(
      limits,
      [&](SessionEntry const& entry) -> ava::core::Result<bool> {
        ++summary.entry_count;
        summary.last_updated = entry.timestamp;
        if (entry.type == EntryType::SessionMetadata)
        {
          if (auto name = ava::core::json::string_field(entry.data_json, "name"))
          {
            summary.title = std::move(*name);
            has_manual_title = true;
          }
          else if (!has_manual_title)
          {
            if (auto generated = ava::core::json::string_field(entry.data_json, "generated_title"))
              summary.title = std::move(*generated);
          }
        }
        if (entry.type == EntryType::SessionStart || entry.type == EntryType::SessionMetadata)
        {
          if (auto cwd = ava::core::json::string_field(entry.data_json, "original_cwd"); cwd && !cwd->empty())
          {
            std::filesystem::path candidate(*cwd);
            if (!candidate.is_absolute() || candidate.lexically_normal() != candidate)
              return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Session, "persisted session cwd is invalid"));
            if (!summary.original_cwd.empty() && summary.original_cwd != candidate)
              return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Session, "persisted session cwd changed"));
            summary.original_cwd = std::move(candidate);
          }
        }
        return true;
      },
      std::move(cancel_requested));
  if (!visited)
    return std::unexpected(std::move(visited.error()));
  return summary;
}

ava::core::Result<SessionSummary> SessionStore::inspect_bounded(SessionLease const& lease, SessionReadLimits limits,
                                                                SessionCancelCallback cancel_requested) const
{
  SessionSummary summary{.session_id = session_id(), .path = session_path(), .last_updated = {}, .entry_count = 0, .original_cwd = {}, .title = {}};
  bool has_manual_title = false;
  auto visited = visit_entries_leased(
      lease, limits,
      [&](SessionEntry const& entry) -> ava::core::Result<bool> {
        ++summary.entry_count;
        summary.last_updated = entry.timestamp;
        if (entry.type == EntryType::SessionMetadata)
        {
          if (auto name = ava::core::json::string_field(entry.data_json, "name"))
          {
            summary.title = std::move(*name);
            has_manual_title = true;
          }
          else if (!has_manual_title)
          {
            if (auto generated = ava::core::json::string_field(entry.data_json, "generated_title"))
              summary.title = std::move(*generated);
          }
        }
        if (entry.type == EntryType::SessionStart || entry.type == EntryType::SessionMetadata)
        {
          if (auto cwd = ava::core::json::string_field(entry.data_json, "original_cwd"); cwd && !cwd->empty())
          {
            std::filesystem::path candidate(*cwd);
            if (!candidate.is_absolute() || candidate.lexically_normal() != candidate)
              return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Session, "persisted session cwd is invalid"));
            if (!summary.original_cwd.empty() && summary.original_cwd != candidate)
              return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Session, "persisted session cwd changed"));
            summary.original_cwd = std::move(candidate);
          }
        }
        return true;
      },
      std::move(cancel_requested));
  if (!visited)
    return std::unexpected(std::move(visited.error()));
  return summary;
}

ava::core::Result<SessionSummary> SessionStore::inspect_bounded_for_listing(SessionReadLimits limits, bool inspect_metadata,
                                                                            SessionCancelCallback cancel_requested) const
{
  if (auto valid = validate_read_limits(limits); !valid)
    return std::unexpected(std::move(valid.error()));
  SessionSummary summary{.session_id = session_id(), .path = session_path(), .last_updated = {}, .entry_count = 0, .original_cwd = {}, .title = {}};
  bool has_manual_title = false;
  auto visited = visit_regular_session_snapshot(
      session_path(), limits,
      [&](SessionEntry const& entry) -> ava::core::Result<bool> {
        ++summary.entry_count;
        summary.last_updated = entry.timestamp;
        if (inspect_metadata && entry.type == EntryType::SessionMetadata)
        {
          if (auto name = ava::core::json::string_field(entry.data_json, "name"))
          {
            summary.title = std::move(*name);
            has_manual_title = true;
          }
          else if (!has_manual_title)
          {
            if (auto generated = ava::core::json::string_field(entry.data_json, "generated_title"))
              summary.title = std::move(*generated);
          }
        }
        if (inspect_metadata && (entry.type == EntryType::SessionStart || entry.type == EntryType::SessionMetadata))
        {
          if (auto cwd = ava::core::json::string_field(entry.data_json, "original_cwd"); cwd && !cwd->empty())
          {
            std::filesystem::path candidate(*cwd);
            if (!candidate.is_absolute() || candidate.lexically_normal() != candidate)
              return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Session, "persisted session cwd is invalid"));
            if (!summary.original_cwd.empty() && summary.original_cwd != candidate)
              return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Session, "persisted session cwd changed"));
            summary.original_cwd = std::move(candidate);
          }
        }
        return true;
      },
      cancel_requested, true);
  if (!visited)
    return std::unexpected(std::move(visited.error()));
  return summary;
}

SessionReadLimits legacy_unbounded_session_read_limits()
{
  return SessionReadLimits{.max_file_bytes = std::numeric_limits<std::size_t>::max(),
                           .max_line_bytes = kMaxSessionLineBytes,
                           .max_entries = std::numeric_limits<std::size_t>::max()};
}

}  // namespace ava::session

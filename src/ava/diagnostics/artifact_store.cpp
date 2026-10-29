#include "sys.h"
#include "ava/diagnostics/artifact_store.h"
#include "ava/core/fd_io.h"
#include "ava/core/hex.h"
#include "ava/core/saturating.h"
#include "ava/core/strict_json.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <expected>
#include <filesystem>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <dirent.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace ava::diagnostics {
namespace {

constexpr std::size_t kMaxRecordBytes = 64 * 1024;
constexpr mode_t kPrivateDirectoryMode = S_IRWXU;
constexpr mode_t kPrivateFileMode = S_IRUSR | S_IWUSR;
constexpr std::size_t kMaxRetainedTraces = 32;
constexpr std::uint64_t kMaxRetainedTraceBytes = 100U * 1024U * 1024U;
constexpr std::uint64_t kTraceReservationBytes = 10U * 1024U * 1024U;
constexpr std::size_t kMaxRetainedSupportExports = 32;
constexpr std::uint64_t kMaxRetainedSupportBytes = 1024U * 1024U;
constexpr auto kOrphanTemporaryAge = std::chrono::hours(24);

enum class OpenState
{
  Ready,
  Missing,
  Unsafe,
  Unavailable,
};

struct OpenDirectory
{
  OpenState state = OpenState::Unavailable;
  ava::core::UniqueFd fd{};
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

struct OpenFile
{
  OpenState state = OpenState::Unavailable;
  ava::core::UniqueFd fd{};
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

bool private_directory_metadata(struct stat const& metadata) noexcept
{
  return S_ISDIR(metadata.st_mode) && metadata.st_uid == ::geteuid() && metadata.st_gid == ::getegid() &&
         (metadata.st_mode & static_cast<mode_t>(07777)) == kPrivateDirectoryMode;
}

bool private_file_metadata(struct stat const& metadata) noexcept
{
  return S_ISREG(metadata.st_mode) && metadata.st_uid == ::geteuid() && metadata.st_gid == ::getegid() && metadata.st_nlink == 1 &&
         (metadata.st_mode & static_cast<mode_t>(07777)) == kPrivateFileMode;
}

OpenDirectory open_private_child(int parent_fd, std::string_view name, bool create)
{
  std::string const child(name);
  int fd = ::openat(parent_fd, child.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NONBLOCK | O_NOFOLLOW);
  if (fd < 0 && errno == ENOENT && create)
  {
    if (::mkdirat(parent_fd, child.c_str(), kPrivateDirectoryMode) != 0 && errno != EEXIST)
      return {.state = OpenState::Unavailable};
    fd = ::openat(parent_fd, child.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NONBLOCK | O_NOFOLLOW);
  }
  if (fd < 0)
  {
    if (errno == ENOENT)
      return {.state = OpenState::Missing};
    if (errno == ELOOP || errno == ENOTDIR)
      return {.state = OpenState::Unsafe};
    return {.state = OpenState::Unavailable};
  }
  ava::core::UniqueFd opened(fd);
  struct stat metadata{};
  if (::fstat(opened.get(), &metadata) != 0)
    return {.state = OpenState::Unavailable};
  if (!private_directory_metadata(metadata))
    return {.state = OpenState::Unsafe};
  return {.state = OpenState::Ready, .fd = std::move(opened)};
}

OpenDirectory open_ava_state(ava::config::XdgPaths const& paths, ava::core::AnchorSet const& anchors) noexcept
{
  try
  {
    auto const ava_state = paths.ava_state_dir.lexically_normal();
    auto ref = anchors.find_anchor(ava_state);
    if (!ref || !ref->relative().empty() || ref->anchor().root != ava_state)
      return {.state = OpenState::Unsafe};
    int const duplicated = ::fcntl(ref->anchor().fd, F_DUPFD_CLOEXEC, 0);
    if (duplicated < 0)
      return {.state = OpenState::Unavailable};
    ava::core::UniqueFd opened(duplicated);
    struct stat metadata{};
    if (::fstat(opened.get(), &metadata) != 0)
      return {.state = OpenState::Unavailable};
    if (!private_directory_metadata(metadata))
      return {.state = OpenState::Unsafe};
    return {.state = OpenState::Ready, .fd = std::move(opened)};
  }
  catch (...)
  {
    return {.state = OpenState::Unavailable};
  }
}

OpenDirectory open_artifact_directory(ava::config::XdgPaths const& paths, ava::core::AnchorSet const& anchors, std::string_view name, bool create)
{
  auto state = open_ava_state(paths, anchors);
  if (state.state != OpenState::Ready)
    return state;
  return open_private_child(state.fd.get(), name, create);
}

StoredRecordState stored_state(OpenState state) noexcept
{
  switch (state)
  {
    case OpenState::Ready:
      return StoredRecordState::Present;
    case OpenState::Missing:
      return StoredRecordState::Absent;
    case OpenState::Unsafe:
      return StoredRecordState::Unsafe;
    case OpenState::Unavailable:
      return StoredRecordState::Unavailable;
  }
  return StoredRecordState::Unavailable;
}

ArtifactWriteStatus write_state(OpenState state) noexcept
{
  switch (state)
  {
    case OpenState::Ready:
      return ArtifactWriteStatus::Success;
    case OpenState::Unsafe:
      return ArtifactWriteStatus::UnsafeStorage;
    case OpenState::Missing:
    case OpenState::Unavailable:
      return ArtifactWriteStatus::StorageUnavailable;
  }
  return ArtifactWriteStatus::StorageUnavailable;
}

struct PrivateFile
{
  StoredRecordState state = StoredRecordState::Unavailable;
  std::string body{};
  // Preserved even for malformed/oversized records, so quarantine can verify
  // the inode that was inspected rather than trusting the name a second time.
  struct stat metadata{};
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

PrivateFile read_private_file(int directory_fd, std::string_view name)
{
  std::string const filename(name);
  struct stat before{};
  if (::fstatat(directory_fd, filename.c_str(), &before, AT_SYMLINK_NOFOLLOW) != 0)
    return {.state = errno == ENOENT ? StoredRecordState::Absent : StoredRecordState::Unavailable};
  if (!private_file_metadata(before))
    return {.state = StoredRecordState::Unsafe};
  ava::core::UniqueFd file(::openat(directory_fd, filename.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK | O_NOFOLLOW));
  if (!file)
    return {.state = errno == ELOOP ? StoredRecordState::Unsafe : StoredRecordState::Unavailable};
  struct stat opened{};
  if (::fstat(file.get(), &opened) != 0)
    return {.state = StoredRecordState::Unavailable};
  if (!private_file_metadata(opened) || opened.st_dev != before.st_dev || opened.st_ino != before.st_ino)
    return {.state = StoredRecordState::Unsafe};
  if (opened.st_size < 0 || static_cast<std::uint64_t>(opened.st_size) > kMaxRecordBytes)
    return {.state = StoredRecordState::Malformed, .metadata = opened};
  auto body = ava::core::read_descriptor_bounded(file.get(), kMaxRecordBytes);
  if (!body)
    return {.state = body.error() == EFBIG ? StoredRecordState::Malformed : StoredRecordState::Unavailable, .metadata = opened};
  return {.state = StoredRecordState::Present, .body = std::move(*body), .metadata = opened};
}

bool same_private_file(int directory_fd, char const* name, struct stat const& opened) noexcept
{
  struct stat published{};
  return ::fstatat(directory_fd, name, &published, AT_SYMLINK_NOFOLLOW) == 0 && private_file_metadata(opened) && private_file_metadata(published) &&
         published.st_dev == opened.st_dev && published.st_ino == opened.st_ino;
}

bool same_private_file(int directory_fd, char const* name, int fd, struct stat const& expected) noexcept
{
  struct stat opened{};
  return ::fstat(fd, &opened) == 0 && opened.st_dev == expected.st_dev && opened.st_ino == expected.st_ino && same_private_file(directory_fd, name, opened);
}

OpenFile open_private_lock(int directory_fd, char const* name) noexcept
{
  int const fd = ::openat(directory_fd, name, O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK, kPrivateFileMode);
  if (fd < 0)
  {
    struct stat existing{};
    if (errno == ELOOP || errno == EISDIR || ::fstatat(directory_fd, name, &existing, AT_SYMLINK_NOFOLLOW) == 0)
      return {.state = OpenState::Unsafe};
    return {.state = OpenState::Unavailable};
  }
  ava::core::UniqueFd opened(fd);
  struct stat metadata{};
  struct stat published{};
  if (::fstat(opened.get(), &metadata) != 0 || ::fstatat(directory_fd, name, &published, AT_SYMLINK_NOFOLLOW) != 0)
    return {.state = OpenState::Unavailable};
  if (!private_file_metadata(metadata) || !private_file_metadata(published) || metadata.st_dev != published.st_dev || metadata.st_ino != published.st_ino)
    return {.state = OpenState::Unsafe};
  return {.state = OpenState::Ready, .fd = std::move(opened)};
}

bool lock_private_file(int directory_fd, int lock_fd, char const* name) noexcept
{
  while (::flock(lock_fd, LOCK_EX) != 0)
  {
    if (errno == EINTR)
      continue;
    return false;
  }
  struct stat metadata{};
  return ::fstat(lock_fd, &metadata) == 0 && same_private_file(directory_fd, name, metadata);
}

std::string opaque_token()
{
  std::array<unsigned char, 16> bytes{};
  std::size_t offset = 0;
  while (offset < bytes.size())
  {
    auto const count = ::getrandom(bytes.data() + offset, bytes.size() - offset, 0);
    if (count < 0 && errno == EINTR)
      continue;
    if (count <= 0)
      break;
    offset += static_cast<std::size_t>(count);
  }
  if (offset != bytes.size())
  {
    static std::atomic<std::uint64_t> sequence{0};
    auto value = static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count()) ^ (static_cast<std::uint64_t>(::getpid()) << 32U) ^
                 sequence.fetch_add(1, std::memory_order_relaxed);
    for (std::size_t index = 0; index < bytes.size(); ++index)
    {
      value ^= value << 13U;
      value ^= value >> 7U;
      value ^= value << 17U;
      bytes[index] = static_cast<unsigned char>(value & 0xffU);
    }
  }
  return ava::core::hex_encode(bytes);
}

bool safe_existing_replace_target(int directory_fd, char const* name) noexcept
{
  struct stat metadata{};
  if (::fstatat(directory_fd, name, &metadata, AT_SYMLINK_NOFOLLOW) != 0)
    return errno == ENOENT;
  return private_file_metadata(metadata);
}

OpenFile lock_artifact_directory(int directory_fd)
{
  constexpr char name[] = ".retention-v1.lock";
  auto lock = open_private_lock(directory_fd, name);
  if (lock.state == OpenState::Ready && !lock_private_file(directory_fd, lock.fd.get(), name))
    lock.state = OpenState::Unsafe;
  return lock;
}

struct RetainedArtifact
{
  std::string name;
  struct stat metadata{};
  std::uint64_t reserved_bytes = 0;
  bool active = false;
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

struct RetentionPolicy
{
  std::string_view prefix;
  std::string_view suffix;
  std::size_t max_count;
  std::uint64_t max_bytes;
  std::uint64_t incoming_bytes;
  std::uint64_t active_reservation;
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

struct LockedArtifact
{
  ava::core::UniqueFd fd;
  bool active;
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

std::expected<LockedArtifact, ArtifactWriteStatus> lock_private_artifact(int directory_fd, char const* name, struct stat const& metadata)
{
  ava::core::UniqueFd file(::openat(directory_fd, name, O_RDONLY | O_CLOEXEC | O_NONBLOCK | O_NOFOLLOW));
  if (!file || !same_private_file(directory_fd, name, file.get(), metadata))
    return std::unexpected(ArtifactWriteStatus::UnsafeStorage);
  bool const active = ::flock(file.get(), LOCK_EX | LOCK_NB) != 0;
  if (active && errno != EWOULDBLOCK && errno != EAGAIN)
    return std::unexpected(ArtifactWriteStatus::IoFailure);
  return LockedArtifact{.fd = std::move(file), .active = active};
}

// Visit in place rather than allocating a collection for temporary-only sweeps.
template <typename Predicate, typename Visitor>
ArtifactWriteStatus visit_private_entries(int directory_fd, Predicate matches, Visitor visit)
{
  auto directory = ava::core::open_directory_stream(ava::core::UniqueFd(::openat(directory_fd, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW)));
  if (!directory)
    return ArtifactWriteStatus::IoFailure;
  while (true)
  {
    errno = 0;
    auto const* entry = ::readdir(directory->get());
    if (!entry)
      return errno == 0 ? ArtifactWriteStatus::Success : ArtifactWriteStatus::IoFailure;
    if (!matches(std::string_view(entry->d_name)))
      continue;
    struct stat metadata{};
    if (::fstatat(directory_fd, entry->d_name, &metadata, AT_SYMLINK_NOFOLLOW) != 0)
    {
      if (errno == ENOENT)
        continue;
      return ArtifactWriteStatus::IoFailure;
    }
    if (!private_file_metadata(metadata) || metadata.st_size < 0)
    {
      // Retention manages only verified-private artifacts. Shared files and
      // unsafe names are neither deleted nor budgeted: they may alias external
      // storage, and an unmanageable sibling must not disable future exports.
      continue;
    }
    if (auto const status = visit(entry->d_name, metadata); status != ArtifactWriteStatus::Success)
      return status;
  }
}

// The caller holds the directory's retention lock. Live writers additionally
// hold a lock on their inode, including while it is a temporary.
ArtifactWriteStatus sweep_stale_temporaries(int directory_fd)
{
  auto const stale_before = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch() - kOrphanTemporaryAge).count();
  return visit_private_entries(
      directory_fd, [](std::string_view name) { return name.starts_with(".diagnostic-tmp-") || name.starts_with(".support-tmp-"); },
      [&](char const* name, struct stat const& metadata) {
        if (metadata.st_mtim.tv_sec >= stale_before)
          return ArtifactWriteStatus::Success;
        auto file = lock_private_artifact(directory_fd, name, metadata);
        if (!file)
          return file.error();
        if (!file->active && same_private_file(directory_fd, name, file->fd.get(), metadata) && ::unlinkat(directory_fd, name, 0) != 0)
          return ArtifactWriteStatus::IoFailure;
        return ArtifactWriteStatus::Success;
      });
}

// The caller keeps the retention lock through publication of the incoming file.
ArtifactWriteStatus enforce_retention(int directory_fd, RetentionPolicy const& policy)
{
  if (auto const status = sweep_stale_temporaries(directory_fd); status != ArtifactWriteStatus::Success)
    return status;
  std::vector<RetainedArtifact> artifacts;
  std::uint64_t total_bytes = policy.incoming_bytes;
  auto const status = visit_private_entries(
      directory_fd, [&](std::string_view name) { return name.starts_with(policy.prefix) && name.ends_with(policy.suffix); },
      [&](char const* name, struct stat const& metadata) {
        auto file = lock_private_artifact(directory_fd, name, metadata);
        if (!file)
          return file.error();
        auto const bytes =
            file->active ? std::max(static_cast<std::uint64_t>(metadata.st_size), policy.active_reservation) : static_cast<std::uint64_t>(metadata.st_size);
        if (bytes > std::numeric_limits<std::uint64_t>::max() - total_bytes)
          return ArtifactWriteStatus::StorageUnavailable;
        total_bytes += bytes;
        artifacts.push_back({.name = name, .metadata = metadata, .reserved_bytes = bytes, .active = file->active});
        return ArtifactWriteStatus::Success;
      });
  if (status != ArtifactWriteStatus::Success)
    return status;
  std::ranges::sort(artifacts, [](auto const& left, auto const& right) {
    if (left.metadata.st_mtim.tv_sec != right.metadata.st_mtim.tv_sec)
      return left.metadata.st_mtim.tv_sec < right.metadata.st_mtim.tv_sec;
    if (left.metadata.st_mtim.tv_nsec != right.metadata.st_mtim.tv_nsec)
      return left.metadata.st_mtim.tv_nsec < right.metadata.st_mtim.tv_nsec;
    return left.name < right.name;
  });
  auto remaining = artifacts.size();
  for (auto const& artifact : artifacts)
  {
    if (remaining < policy.max_count && total_bytes <= policy.max_bytes)
      break;
    if (artifact.active)
      continue;
    auto file = lock_private_artifact(directory_fd, artifact.name.c_str(), artifact.metadata);
    if (!file)
    {
      // A failed lock leaves this candidate in the retained budget.
      if (file.error() == ArtifactWriteStatus::IoFailure)
        continue;
      return file.error();
    }
    if (file->active)
      continue;
    if (!same_private_file(directory_fd, artifact.name.c_str(), file->fd.get(), artifact.metadata))
      return ArtifactWriteStatus::UnsafeStorage;
    if (::unlinkat(directory_fd, artifact.name.c_str(), 0) != 0)
      return ArtifactWriteStatus::IoFailure;
    --remaining;
    total_bytes -= artifact.reserved_bytes;
  }
  if (remaining >= policy.max_count || total_bytes > policy.max_bytes)
    return ArtifactWriteStatus::StorageUnavailable;
  return ArtifactWriteStatus::Success;
}

ArtifactWriteStatus sweep_temporaries(int directory_fd)
{
  auto lock = lock_artifact_directory(directory_fd);
  if (lock.state != OpenState::Ready)
    return write_state(lock.state);
  return sweep_stale_temporaries(directory_fd);
}

ArtifactWriteStatus quarantine_counters(int directory_fd, struct stat const& verified)
{
  constexpr char source[] = "trace-counters-v1.json";
  constexpr char target[] = "trace-counters-v1.corrupt";
  ava::core::UniqueFd file(::openat(directory_fd, source, O_RDWR | O_CLOEXEC | O_NONBLOCK | O_NOFOLLOW));
  if (!file || !same_private_file(directory_fd, source, file.get(), verified) || !safe_existing_replace_target(directory_fd, target))
    return ArtifactWriteStatus::UnsafeStorage;
  if (::flock(file.get(), LOCK_EX | LOCK_NB) != 0)
    return ArtifactWriteStatus::StorageUnavailable;
  if (!same_private_file(directory_fd, source, file.get(), verified))
    return ArtifactWriteStatus::UnsafeStorage;
  if (::renameat(directory_fd, source, directory_fd, target) != 0)
    return ArtifactWriteStatus::IoFailure;
  // Keep one bounded evidence sample, including when corruption is oversized.
  struct stat quarantined{};
  if (::fstat(file.get(), &quarantined) != 0 ||
      (quarantined.st_size > static_cast<off_t>(kMaxRecordBytes) &&
       ava::core::restart_on_eintr([&] { return ::ftruncate(file.get(), static_cast<off_t>(kMaxRecordBytes)); }) != 0) ||
      ava::core::fsync_with_restart(file.get()) != 0 || ava::core::fsync_with_restart(directory_fd) != 0)
    return ArtifactWriteStatus::IoFailure;
  return ArtifactWriteStatus::Success;
}

ArtifactWriteStatus atomic_replace(int directory_fd, char const* target, std::string_view body)
{
  if (body.empty() || body.size() > kMaxRecordBytes || !safe_existing_replace_target(directory_fd, target))
    return ArtifactWriteStatus::UnsafeStorage;
  if (auto const status = sweep_temporaries(directory_fd); status != ArtifactWriteStatus::Success)
    return status;
  auto const temporary = ".diagnostic-tmp-" + opaque_token();
  ava::core::UniqueFd file(::openat(directory_fd, temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NONBLOCK | O_NOFOLLOW, kPrivateFileMode));
  if (!file)
    return ArtifactWriteStatus::IoFailure;
  auto rollback = [&] { static_cast<void>(::unlinkat(directory_fd, temporary.c_str(), 0)); };
  if (::flock(file.get(), LOCK_EX | LOCK_NB) != 0)
  {
    rollback();
    return ArtifactWriteStatus::IoFailure;
  }
  if (ava::core::write_all(file.get(), body) != 0 || ava::core::fsync_with_restart(file.get()) != 0)
  {
    rollback();
    return ArtifactWriteStatus::IoFailure;
  }
  struct stat metadata{};
  if (::fstat(file.get(), &metadata) != 0 || !private_file_metadata(metadata))
  {
    rollback();
    return ArtifactWriteStatus::UnsafeStorage;
  }
  if (::renameat(directory_fd, temporary.c_str(), directory_fd, target) != 0)
  {
    rollback();
    return ArtifactWriteStatus::IoFailure;
  }
  if (ava::core::fsync_with_restart(directory_fd) != 0)
    return ArtifactWriteStatus::IoFailure;
  return ArtifactWriteStatus::Success;
}

struct UniquePublication
{
  ArtifactWriteStatus status = ArtifactWriteStatus::IoFailure;
  std::string filename;
};

UniquePublication publish_unique(int directory_fd, std::string_view body)
{
  if (body.empty() || body.size() > kMaxRecordBytes)
    return {.status = ArtifactWriteStatus::InvalidRecord, .filename = {}};
  for (int attempt = 0; attempt < 32; ++attempt)
  {
    auto const token = opaque_token();
    auto const temporary = ".support-tmp-" + token;
    auto final_name = "ava-support-v1-" + token + ".json";
    ava::core::UniqueFd file(::openat(directory_fd, temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NONBLOCK | O_NOFOLLOW, kPrivateFileMode));
    if (!file)
    {
      if (errno == EEXIST)
        continue;
      return {.status = ArtifactWriteStatus::IoFailure, .filename = {}};
    }
    auto rollback_temp = [&] { static_cast<void>(::unlinkat(directory_fd, temporary.c_str(), 0)); };
    if (::flock(file.get(), LOCK_EX | LOCK_NB) != 0)
    {
      rollback_temp();
      return {.status = ArtifactWriteStatus::IoFailure, .filename = {}};
    }
    if (ava::core::write_all(file.get(), body) != 0 || ava::core::fsync_with_restart(file.get()) != 0)
    {
      rollback_temp();
      return {.status = ArtifactWriteStatus::IoFailure, .filename = {}};
    }
    struct stat metadata{};
    if (::fstat(file.get(), &metadata) != 0 || !private_file_metadata(metadata))
    {
      rollback_temp();
      return {.status = ArtifactWriteStatus::UnsafeStorage, .filename = {}};
    }
    auto error = ava::core::rename_noreplace(directory_fd, temporary.c_str(), directory_fd, final_name.c_str());
    if (ava::core::rename_noreplace_unavailable(error))
    {
      // Diagnostic files only: linkat never replaces an existing name, so this fallback is safe here.
      if (::linkat(directory_fd, temporary.c_str(), directory_fd, final_name.c_str(), 0) != 0)
        error = errno;
      else if (::unlinkat(directory_fd, temporary.c_str(), 0) != 0)
      {
        error = errno;
        static_cast<void>(::unlinkat(directory_fd, final_name.c_str(), 0));
      }
      else
        error = 0;
    }
    if (error != 0)
    {
      rollback_temp();
      if (error == EEXIST)
        continue;
      return {.status = ArtifactWriteStatus::IoFailure, .filename = {}};
    }
    if (ava::core::fsync_with_restart(directory_fd) != 0)
    {
      static_cast<void>(::unlinkat(directory_fd, final_name.c_str(), 0));
      static_cast<void>(ava::core::fsync_with_restart(directory_fd));
      return {.status = ArtifactWriteStatus::IoFailure, .filename = {}};
    }
    return {.status = ArtifactWriteStatus::Success, .filename = std::move(final_name)};
  }
  return {.status = ArtifactWriteStatus::IoFailure, .filename = {}};
}

TraceArtifactPreparation create_trace_artifact(int directory_fd, ava::config::XdgPaths const& paths)
{
  for (int attempt = 0; attempt < 32; ++attempt)
  {
    auto const filename = "trace-v1-" + opaque_token() + ".jsonl";
    ava::core::UniqueFd file(
        ::openat(directory_fd, filename.c_str(), O_WRONLY | O_APPEND | O_CREAT | O_EXCL | O_CLOEXEC | O_NONBLOCK | O_NOFOLLOW, kPrivateFileMode));
    if (!file)
    {
      if (errno == EEXIST)
        continue;
      return {.status = ArtifactWriteStatus::IoFailure, .path = {}, .descriptor = {}};
    }
    if (::flock(file.get(), LOCK_EX | LOCK_NB) != 0)
    {
      static_cast<void>(::unlinkat(directory_fd, filename.c_str(), 0));
      return {.status = ArtifactWriteStatus::IoFailure, .path = {}, .descriptor = {}};
    }
    struct stat metadata{};
    if (::fstat(file.get(), &metadata) != 0 || !private_file_metadata(metadata))
    {
      static_cast<void>(::unlinkat(directory_fd, filename.c_str(), 0));
      return {.status = ArtifactWriteStatus::UnsafeStorage, .path = {}, .descriptor = {}};
    }
    if (ava::core::fsync_with_restart(file.get()) != 0 || ava::core::fsync_with_restart(directory_fd) != 0)
    {
      static_cast<void>(::unlinkat(directory_fd, filename.c_str(), 0));
      static_cast<void>(ava::core::fsync_with_restart(directory_fd));
      return {.status = ArtifactWriteStatus::IoFailure, .path = {}, .descriptor = {}};
    }
    auto descriptor = std::make_shared<TraceArtifactDescriptor>(file.get());
    static_cast<void>(file.release());
    return {.status = ArtifactWriteStatus::Success,
            .path = paths.ava_state_dir / "diagnostics" / "traces" / filename,
            .descriptor = std::move(descriptor)};
  }
  return {.status = ArtifactWriteStatus::IoFailure, .path = {}, .descriptor = {}};
}

TraceCounterSnapshot merge_trace_counter_snapshots(TraceCounterSnapshot const& aggregate, TraceCounterSnapshot const& contribution) noexcept
{
  return {.captured_at = std::max(aggregate.captured_at, contribution.captured_at),
          .runtime_starts = ava::core::saturating_add(aggregate.runtime_starts, contribution.runtime_starts),
          .provider_requests = ava::core::saturating_add(aggregate.provider_requests, contribution.provider_requests),
          .provider_failures = ava::core::saturating_add(aggregate.provider_failures, contribution.provider_failures),
          .session_failures = ava::core::saturating_add(aggregate.session_failures, contribution.session_failures),
          .plugin_failures = ava::core::saturating_add(aggregate.plugin_failures, contribution.plugin_failures),
          .mcp_failures = ava::core::saturating_add(aggregate.mcp_failures, contribution.mcp_failures),
          .writer_health = {.complete = aggregate.writer_health.complete && contribution.writer_health.complete,
                            .events_written = ava::core::saturating_add(aggregate.writer_health.events_written, contribution.writer_health.events_written),
                            .events_dropped = ava::core::saturating_add(aggregate.writer_health.events_dropped, contribution.writer_health.events_dropped),
                            .writer_failures = ava::core::saturating_add(aggregate.writer_health.writer_failures, contribution.writer_health.writer_failures),
                            .bytes_written = ava::core::saturating_add(aggregate.writer_health.bytes_written, contribution.writer_health.bytes_written),
                            .delta_summaries_refused = ava::core::saturating_add(aggregate.writer_health.delta_summaries_refused,
                                                                                 contribution.writer_health.delta_summaries_refused)}};
}

template <typename Record, typename Parser>
StoredRecord<Record> read_record(ava::config::XdgPaths const& paths, ava::core::AnchorSet const& anchors, std::string_view filename, Parser parser) noexcept
{
  try
  {
    auto directory = open_artifact_directory(paths, anchors, "diagnostics", false);
    if (directory.state != OpenState::Ready)
      return {.state = stored_state(directory.state)};
    auto file = read_private_file(directory.fd.get(), filename);
    if (file.state != StoredRecordState::Present)
      return {.state = file.state};
    auto record = parser(file.body);
    if (!record)
      return {.state = StoredRecordState::Malformed};
    return {.state = StoredRecordState::Present, .record = std::move(*record)};
  }
  catch (...)
  {
    return {.state = StoredRecordState::Unavailable};
  }
}

}  // namespace

TraceArtifactDescriptor::TraceArtifactDescriptor(int fd) noexcept : fd_(fd)
{
}

TraceArtifactDescriptor::~TraceArtifactDescriptor()
{
  if (fd_ >= 0)
    static_cast<void>(::close(fd_));
}

int TraceArtifactDescriptor::fd() const noexcept
{
  return fd_;
}

std::string_view to_string(ArtifactWriteStatus status) noexcept
{
  switch (status)
  {
    case ArtifactWriteStatus::Success:
      return "success";
    case ArtifactWriteStatus::UnsafeStorage:
      return "unsafe_storage";
    case ArtifactWriteStatus::StorageUnavailable:
      return "storage_unavailable";
    case ArtifactWriteStatus::IoFailure:
      return "io_failure";
    case ArtifactWriteStatus::InvalidRecord:
      return "invalid_record";
  }
  return "io_failure";
}

StoredRecord<LastFailureRecord> read_last_failure_record(ava::config::XdgPaths const& paths, ava::core::AnchorSet const& anchors) noexcept
{
  return read_record<LastFailureRecord>(paths, anchors, "last-failure-v1.json", parse_last_failure_record);
}

StoredRecord<TraceCounterSnapshot> read_trace_counter_snapshot(ava::config::XdgPaths const& paths, ava::core::AnchorSet const& anchors) noexcept
{
  return read_record<TraceCounterSnapshot>(paths, anchors, "trace-counters-v1.json", parse_trace_counter_snapshot);
}

ArtifactWriteStatus write_last_failure_record(ava::config::XdgPaths const& paths, ava::core::AnchorSet const& anchors,
                                                    LastFailureRecord const& record) noexcept
{
  try
  {
    if (record.schema_version != kDiagnosticSchemaVersion)
      return ArtifactWriteStatus::InvalidRecord;
    if (!parse_last_failure_record(serialize_last_failure_record(record)))
      return ArtifactWriteStatus::InvalidRecord;
    auto directory = open_artifact_directory(paths, anchors, "diagnostics", true);
    if (directory.state != OpenState::Ready)
      return write_state(directory.state);
    auto lock = open_private_lock(directory.fd.get(), "last-failure-v1.lock");
    if (lock.state != OpenState::Ready)
      return write_state(lock.state);
    if (!lock_private_file(directory.fd.get(), lock.fd.get(), "last-failure-v1.lock"))
      return ArtifactWriteStatus::UnsafeStorage;
    auto merged = record;
    auto const file = read_private_file(directory.fd.get(), "last-failure-v1.json");
    if (file.state == StoredRecordState::Present)
    {
      auto const existing = parse_last_failure_record(file.body);
      if (existing)
      {
        auto const& left = existing->failure;
        auto const& right = record.failure;
        if (left.component == right.component && left.category == right.category && left.code == right.code && left.retryability == right.retryability &&
            left.recovery_hint == right.recovery_hint)
          merged.occurrences = ava::core::saturating_add(existing->occurrences, record.occurrences);
      }
    }
    else if (file.state == StoredRecordState::Unsafe)
      return ArtifactWriteStatus::UnsafeStorage;
    else if (file.state == StoredRecordState::Unavailable)
      return ArtifactWriteStatus::StorageUnavailable;
    auto const body = serialize_last_failure_record(merged);
    return atomic_replace(directory.fd.get(), "last-failure-v1.json", body);
  }
  catch (...)
  {
    return ArtifactWriteStatus::IoFailure;
  }
}

ArtifactWriteStatus write_trace_counter_snapshot(ava::config::XdgPaths const& paths, ava::core::AnchorSet const& anchors,
                                                       TraceCounterSnapshot const& snapshot) noexcept
{
  try
  {
    if (snapshot.schema_version != kDiagnosticSchemaVersion || !parse_trace_counter_snapshot(serialize_trace_counter_snapshot(snapshot)))
      return ArtifactWriteStatus::InvalidRecord;
    auto directory = open_artifact_directory(paths, anchors, "diagnostics", true);
    if (directory.state != OpenState::Ready)
      return write_state(directory.state);
    auto lock = open_private_lock(directory.fd.get(), "trace-counters-v1.lock");
    if (lock.state != OpenState::Ready)
      return write_state(lock.state);
    if (!lock_private_file(directory.fd.get(), lock.fd.get(), "trace-counters-v1.lock"))
      return ArtifactWriteStatus::UnsafeStorage;

    TraceCounterSnapshot aggregate{.writer_health = {.complete = true}};
    auto file = read_private_file(directory.fd.get(), "trace-counters-v1.json");
    if (file.state == StoredRecordState::Present)
    {
      auto existing = parse_trace_counter_snapshot(file.body);
      if (existing)
        aggregate = std::move(*existing);
      else
        file.state = StoredRecordState::Malformed;
    }
    if (file.state == StoredRecordState::Malformed)
    {
      if (auto const status = quarantine_counters(directory.fd.get(), file.metadata); status != ArtifactWriteStatus::Success)
        return status;
    }
    else if (file.state == StoredRecordState::Unsafe)
      return ArtifactWriteStatus::UnsafeStorage;
    else if (file.state == StoredRecordState::Unavailable)
      return ArtifactWriteStatus::StorageUnavailable;

    auto const merged = merge_trace_counter_snapshots(aggregate, snapshot);
    auto const body = serialize_trace_counter_snapshot(merged);
    if (!parse_trace_counter_snapshot(body))
      return ArtifactWriteStatus::InvalidRecord;
    return atomic_replace(directory.fd.get(), "trace-counters-v1.json", body);
  }
  catch (...)
  {
    return ArtifactWriteStatus::IoFailure;
  }
}

void write_last_failure_record_best_effort(ava::config::XdgPaths const& paths, ava::core::AnchorSet const& anchors,
                                           LastFailureRecord const& record) noexcept
{
  static_cast<void>(write_last_failure_record(paths, anchors, record));
}

TraceArtifactPreparation prepare_trace_artifact(ava::config::XdgPaths const& paths, ava::core::AnchorSet const& anchors) noexcept
{
  try
  {
    auto diagnostics = open_artifact_directory(paths, anchors, "diagnostics", true);
    if (diagnostics.state != OpenState::Ready)
      return {.status = write_state(diagnostics.state), .path = {}, .descriptor = {}};
    if (auto const status = sweep_temporaries(diagnostics.fd.get()); status != ArtifactWriteStatus::Success)
      return {.status = status, .path = {}, .descriptor = {}};
    auto traces = open_private_child(diagnostics.fd.get(), "traces", true);
    if (traces.state != OpenState::Ready)
      return {.status = write_state(traces.state), .path = {}, .descriptor = {}};
    auto lock = lock_artifact_directory(traces.fd.get());
    if (lock.state != OpenState::Ready)
      return {.status = write_state(lock.state), .path = {}, .descriptor = {}};
    RetentionPolicy const policy{.prefix = "trace-v1-",
                                 .suffix = ".jsonl",
                                 .max_count = kMaxRetainedTraces,
                                 .max_bytes = kMaxRetainedTraceBytes,
                                 .incoming_bytes = kTraceReservationBytes,
                                 .active_reservation = kTraceReservationBytes};
    if (auto const status = enforce_retention(traces.fd.get(), policy); status != ArtifactWriteStatus::Success)
      return {.status = status, .path = {}, .descriptor = {}};
    return create_trace_artifact(traces.fd.get(), paths);
  }
  catch (...)
  {
    return {.status = ArtifactWriteStatus::IoFailure, .path = {}, .descriptor = {}};
  }
}

SupportArtifactPublication publish_support_artifact(ava::config::XdgPaths const& paths, ava::core::AnchorSet const& anchors,
                                                      SupportArtifact const& artifact) noexcept
{
  try
  {
    if (artifact.schema_version != kDiagnosticSchemaVersion || artifact.generated_at < 0 || artifact.doctor.schema_version != kDiagnosticSchemaVersion)
      return {.status = ArtifactWriteStatus::InvalidRecord, .path = {}};
    if ((artifact.trace.state == StoredRecordState::Present) != artifact.trace.record.has_value() ||
        (artifact.last_failure.state == StoredRecordState::Present) != artifact.last_failure.record.has_value())
      return {.status = ArtifactWriteStatus::InvalidRecord, .path = {}};
    if (artifact.trace.record && (artifact.trace.record->schema_version != kDiagnosticSchemaVersion ||
                                  !parse_trace_counter_snapshot(serialize_trace_counter_snapshot(*artifact.trace.record))))
      return {.status = ArtifactWriteStatus::InvalidRecord, .path = {}};
    if (artifact.last_failure.record && (artifact.last_failure.record->schema_version != kDiagnosticSchemaVersion ||
                                         !parse_last_failure_record(serialize_last_failure_record(*artifact.last_failure.record))))
      return {.status = ArtifactWriteStatus::InvalidRecord, .path = {}};
    auto const doctor_json = serialize_doctor_report_json(artifact.doctor);
    if (!parse_doctor_report_json(doctor_json))
      return {.status = ArtifactWriteStatus::InvalidRecord, .path = {}};
    auto const body = serialize_support_artifact(artifact);
    if (body.size() > kMaxRecordBytes || ava::core::validate_strict_json(body, 16) != ava::core::StrictJsonStatus::Valid)
      return {.status = ArtifactWriteStatus::InvalidRecord, .path = {}};
    auto diagnostics = open_artifact_directory(paths, anchors, "diagnostics", false);
    if (diagnostics.state == OpenState::Ready)
    {
      if (auto const status = sweep_temporaries(diagnostics.fd.get()); status != ArtifactWriteStatus::Success)
        return {.status = status, .path = {}};
    }
    auto directory = open_artifact_directory(paths, anchors, "support", true);
    if (directory.state != OpenState::Ready)
      return {.status = write_state(directory.state), .path = {}};
    auto lock = lock_artifact_directory(directory.fd.get());
    if (lock.state != OpenState::Ready)
      return {.status = write_state(lock.state), .path = {}};
    RetentionPolicy const policy{.prefix = "ava-support-v1-",
                                 .suffix = ".json",
                                 .max_count = kMaxRetainedSupportExports,
                                 .max_bytes = kMaxRetainedSupportBytes,
                                 .incoming_bytes = body.size(),
                                 .active_reservation = kMaxRecordBytes};
    if (auto const status = enforce_retention(directory.fd.get(), policy); status != ArtifactWriteStatus::Success)
      return {.status = status, .path = {}};
    auto published = publish_unique(directory.fd.get(), body);
    return {.status = published.status,
            .path = published.status == ArtifactWriteStatus::Success ? paths.ava_state_dir / "support" / published.filename : std::filesystem::path{}};
  }
  catch (...)
  {
    return {.status = ArtifactWriteStatus::IoFailure, .path = {}};
  }
}

}  // namespace ava::diagnostics

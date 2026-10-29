#include "sys.h"
#include "ava/debug/print_members_on.h"
#include "ava/session/attachments.h"
#include "ava/session/session_store.h"
#include "ava/core/UniqueFd.h"
#include "ava/core/ascii.h"
#include "ava/core/error.h"
#include "ava/core/fd_io.h"
#include "ava/core/ids.h"
#include "ava/core/open_beneath.h"
#include "ava/core/sha256.h"

#include <cctype>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace ava::session {
namespace {

struct DetectedImageType {
  std::string_view mime_type;
  std::string_view extension;
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

bool hex_string(std::string_view value)
{
  for (auto const ch : value) {
    if (std::isxdigit(static_cast<unsigned char>(ch)) == 0) return false;
  }
  return true;
}

bool valid_attachment_storage_path(std::string_view path)
{
  if (path.empty() || path.size() > 4096 || ava::core::has_c0_or_del(path))
    return false;
  if (!path.starts_with("attachments/")) return false;
  if (path.starts_with('/') || path.starts_with('~') || path.find('\\') != std::string_view::npos) return false;
  if (path.find(':') != std::string_view::npos) return false;
  std::size_t segment_start = 0;
  while (segment_start <= path.size()) {
    auto const slash = path.find('/', segment_start);
    auto const segment = path.substr(segment_start, slash == std::string_view::npos ? std::string_view::npos : slash - segment_start);
    if (segment.empty() || segment == "." || segment == "..") return false;
    if (slash == std::string_view::npos) break;
    segment_start = slash + 1;
  }
  return true;
}

ava::core::Error attachment_error(ava::core::ErrorCategory category, std::string_view message,
                                  std::string_view storage_path)
{
  auto error = ava::core::Error(category, std::string(message));
  error.with_context("storage_path", std::string(storage_path));
  return error;
}

ava::core::Error source_attachment_error(ava::core::ErrorCategory category, std::string_view message,
                                         std::filesystem::path const& source_path)
{
  auto error = ava::core::Error(category, std::string(message));
  error.with_context("path", source_path.string());
  return error;
}

std::optional<DetectedImageType> detect_image_type(std::string_view bytes)
{
  auto const has_prefix = [&](std::string_view prefix) {
    return bytes.size() >= prefix.size() && bytes.substr(0, prefix.size()) == prefix;
  };
  if (bytes.size() >= 8 && static_cast<unsigned char>(bytes[0]) == 0x89U && bytes.substr(1, 3) == "PNG" &&
      bytes[4] == '\r' && bytes[5] == '\n' && static_cast<unsigned char>(bytes[6]) == 0x1AU && bytes[7] == '\n') {
    return DetectedImageType{.mime_type = "image/png", .extension = ".png"};
  }
  if (bytes.size() >= 3 && static_cast<unsigned char>(bytes[0]) == 0xFFU &&
      static_cast<unsigned char>(bytes[1]) == 0xD8U && static_cast<unsigned char>(bytes[2]) == 0xFFU) {
    return DetectedImageType{.mime_type = "image/jpeg", .extension = ".jpg"};
  }
  if (has_prefix("GIF87a") || has_prefix("GIF89a")) {
    return DetectedImageType{.mime_type = "image/gif", .extension = ".gif"};
  }
  if (bytes.size() >= 12 && bytes.substr(0, 4) == "RIFF" && bytes.substr(8, 4) == "WEBP") {
    return DetectedImageType{.mime_type = "image/webp", .extension = ".webp"};
  }
  return std::nullopt;
}

ava::core::Error attachment_io_error(std::string_view operation, std::filesystem::path const& path, std::string_view storage_path, int cause)
{
  auto const category = cause == ELOOP || cause == ENOTDIR || cause == EXDEV ? ava::core::ErrorCategory::PermissionDenied : ava::core::ErrorCategory::Io;
  auto error = source_attachment_error(category, operation, path);
  if (!storage_path.empty())
    error.with_context("storage_path", std::string(storage_path));
  error.with_context("cause", std::strerror(cause));
  return error;
}

// Each open_beneath call receives one component, so O_NOFOLLOW rejects every
// symlink even on kernels where open_beneath permits contained symlinks.
ava::core::Result<ava::core::UniqueFd> walk_attachment_directories(ava::core::UniqueFd directory, std::filesystem::path const& relative,
                                                                   std::filesystem::path const& path, bool create, bool owner_only,
                                                                   std::string_view storage_path)
{
  for (auto const& component : relative)
  {
    if (component.empty() || component == ".")
      continue;
    if (create && ::mkdirat(directory.get(), component.c_str(), 0700) != 0 && errno != EEXIST)
      return std::unexpected(attachment_io_error("failed to create image attachment directory", path, storage_path, errno));
    ava::core::UniqueFd next(ava::core::open_beneath(directory.get(), component, O_RDONLY | O_DIRECTORY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC));
    if (!next)
      return std::unexpected(attachment_io_error("failed to open image attachment directory without following symlinks", path, storage_path, errno));
    if (owner_only && ::fchmod(next.get(), 0700) != 0)
      return std::unexpected(attachment_io_error("failed to set image attachment directory permissions", path, storage_path, errno));
    if (create)
    {
      // Persist both the new directory inode and its name before descending.
      if (int const cause = ava::core::fsync_with_restart(next.get()); cause != 0)
        return std::unexpected(attachment_io_error("failed to sync image attachment directory", path, storage_path, cause));
      if (int const cause = ava::core::fsync_with_restart(directory.get()); cause != 0)
        return std::unexpected(attachment_io_error("failed to sync image attachment ancestor", path, storage_path, cause));
    }
    directory = std::move(next);
  }
  return directory;
}

ava::core::Result<ava::core::UniqueFd> open_attachment_directory(std::filesystem::path const& path, bool create, std::string_view storage_path = {})
{
  std::error_code absolute_error;
  auto const absolute = std::filesystem::absolute(path, absolute_error).lexically_normal();
  if (absolute_error)
  {
    auto error = source_attachment_error(ava::core::ErrorCategory::Io, "failed to locate image attachment directory", path);
    error.with_context("cause", absolute_error.message());
    return std::unexpected(std::move(error));
  }
  // Match session/configured-root authority: logical ancestor aliases may name
  // the initial parent, but its final component must be a real directory. Once
  // held, no attachment component is resolved through that pathname again.
  auto candidate = absolute;
  std::filesystem::path missing;
  for (;;)
  {
    ava::core::UniqueFd directory(::open(candidate.c_str(), O_RDONLY | O_DIRECTORY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC));
    if (directory)
      return walk_attachment_directories(std::move(directory), missing, path, create, false, storage_path);
    int const cause = errno;
    if (!create || cause != ENOENT || candidate == candidate.root_path())
      return std::unexpected(attachment_io_error("failed to anchor image attachment directory", path, storage_path, cause));
    missing = candidate.filename() / missing;
    candidate = candidate.parent_path();
  }
}

ava::core::Result<ava::core::UniqueFd> open_stored_attachment(SessionStore const& store, std::string_view storage_path)
{
  auto const root = attachment_storage_root(store);
  auto const path = root / storage_path;
  auto parent = open_attachment_directory(root.parent_path(), false, storage_path);
  if (!parent)
    return std::unexpected(std::move(parent.error()));
  auto directory = walk_attachment_directories(std::move(*parent), root.filename() / std::filesystem::path(storage_path).parent_path(), path.parent_path(),
                                               false, false, storage_path);
  if (!directory)
    return std::unexpected(std::move(directory.error()));
  ava::core::UniqueFd file(ava::core::open_beneath(directory->get(), path.filename(), O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC));
  if (!file)
    return std::unexpected(attachment_io_error("failed to open image attachment without following symlinks", path, storage_path, errno));
  return file;
}

ava::core::Result<std::string> read_attachment_snapshot(int fd, std::filesystem::path const& path, std::optional<std::uint64_t> expected_size = std::nullopt)
{
  auto prepared = ava::core::prepare_regular_descriptor(fd);
  if (!prepared)
  {
    auto error = ava::core::open_regular_error(prepared.error(), "opened image attachment");
    error.with_context("path", path.string());
    return std::unexpected(std::move(error));
  }
  auto const& status = *prepared;
  if (status.st_size <= 0 || static_cast<std::uintmax_t>(status.st_size) > kMaxImageAttachmentBytes)
  {
    auto error = source_attachment_error(ava::core::ErrorCategory::InvalidArgument, "image attachment byte size is invalid", path);
    error.with_context("max_bytes", std::to_string(kMaxImageAttachmentBytes));
    error.with_context("byte_size", std::to_string(status.st_size));
    return std::unexpected(std::move(error));
  }
  if (expected_size && static_cast<std::uintmax_t>(status.st_size) != *expected_size)
  {
    return std::unexpected(source_attachment_error(ava::core::ErrorCategory::InvalidArgument, "image attachment byte size does not match metadata", path));
  }

  auto bytes = ava::core::read_descriptor_bounded(fd, static_cast<std::size_t>(status.st_size));
  if (!bytes)
  {
    int const cause = bytes.error();
    auto error = source_attachment_error(cause == EFBIG ? ava::core::ErrorCategory::InvalidArgument : ava::core::ErrorCategory::Io,
                                         cause == EFBIG ? "image attachment byte size changed while reading" : "failed to read image attachment", path);
    error.with_context("cause", std::strerror(cause));
    return std::unexpected(std::move(error));
  }
  if (bytes->size() != static_cast<std::size_t>(status.st_size))
  {
    auto error = source_attachment_error(ava::core::ErrorCategory::Io, "image attachment ended before its inspected byte size was read", path);
    error.with_context("expected_bytes", std::to_string(status.st_size));
    error.with_context("read_bytes", std::to_string(bytes->size()));
    return std::unexpected(std::move(error));
  }
  return std::move(*bytes);
}

ava::core::Result<std::string> read_import_source(std::filesystem::path const& source_path)
{
  auto parent = open_attachment_directory(source_path.has_parent_path() ? source_path.parent_path() : std::filesystem::path("."), false);
  if (!parent)
    return std::unexpected(std::move(parent.error()));
  ava::core::UniqueFd file(ava::core::open_beneath(parent->get(), source_path.filename(), O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC));
  if (!file)
    return std::unexpected(attachment_io_error("failed to open image attachment import source without following symlinks", source_path, {}, errno));
  return read_attachment_snapshot(file.get(), source_path);
}

// Borrows the directory, descriptor and basename until this guard is destroyed.
// Keep it before the descriptor's destruction so cleanup can verify the inode.
class AttachmentTemporaryName
{
 public:
  AttachmentTemporaryName(int directory_fd, ava::core::UniqueFd const& file, std::string const& name) noexcept
      : directory_fd_(directory_fd), file_(file), name_(name)
  {
  }
  AttachmentTemporaryName(AttachmentTemporaryName const&) = delete;
  AttachmentTemporaryName& operator=(AttachmentTemporaryName const&) = delete;
  AttachmentTemporaryName(AttachmentTemporaryName&&) = delete;
  AttachmentTemporaryName& operator=(AttachmentTemporaryName&&) = delete;
  ~AttachmentTemporaryName() noexcept
  {
    if (active_ && remove_checked() == 0)
      static_cast<void>(ava::core::fsync_with_restart(directory_fd_));
  }

  void arm() noexcept { active_ = true; }

  [[nodiscard]] int remove_checked() noexcept
  {
    if (!active_)
      return 0;
    struct stat opened{};
    struct stat named{};
    if (ava::core::restart_on_eintr([&] { return ::fstat(file_.get(), &opened); }) != 0)
      return errno;
    if (ava::core::restart_on_eintr([&] { return ::fstatat(directory_fd_, name_.c_str(), &named, AT_SYMLINK_NOFOLLOW); }) != 0)
    {
      if (errno != ENOENT)
        return errno;
      active_ = false;
      return 0;
    }
    if (opened.st_dev != named.st_dev || opened.st_ino != named.st_ino)
      return ESTALE;
    if (ava::core::restart_on_eintr([&] { return ::unlinkat(directory_fd_, name_.c_str(), 0); }) != 0)
      return errno;
    active_ = false;
    return 0;
  }

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT

 private:
  int directory_fd_;
  ava::core::UniqueFd const& file_;
  std::string const& name_;
  bool active_ = false;
};

ava::core::VoidResult write_stored_attachment(SessionStore const& store, ImageAttachmentRef const& metadata, std::string_view bytes)
{
  auto destination = resolve_attachment_storage_path(store, metadata.storage_path);
  if (!destination)
    return std::unexpected(std::move(destination.error()));
  auto const root = attachment_storage_root(store);
  auto parent = open_attachment_directory(root.parent_path(), true, metadata.storage_path);
  if (!parent)
    return std::unexpected(std::move(parent.error()));
  auto directory = walk_attachment_directories(std::move(*parent), root.filename() / std::filesystem::path(metadata.storage_path).parent_path(),
                                               destination->parent_path(), true, true, metadata.storage_path);
  if (!directory)
    return std::unexpected(std::move(directory.error()));

  auto reuse_existing = [&](ava::core::UniqueFd existing_file) -> ava::core::VoidResult {
    // Content-addressed duplicates are read-only, including raced-in hardlinks.
    auto existing = read_attachment_snapshot(existing_file.get(), *destination, metadata.byte_size);
    if (!existing)
    {
      existing.error().with_context("storage_path", metadata.storage_path);
      return std::unexpected(std::move(existing.error()));
    }
    if (*existing != bytes)
    {
      auto error =
          attachment_error(ava::core::ErrorCategory::InvalidArgument, "existing image attachment destination has different bytes", metadata.storage_path);
      error.with_context("path", destination->string());
      return std::unexpected(std::move(error));
    }
    if (int const cause = ava::core::fsync_with_restart(existing_file.get()); cause != 0)
      return std::unexpected(attachment_io_error("failed to sync existing image attachment", *destination, metadata.storage_path, cause));
    if (int const cause = existing_file.close_checked(); cause != 0)
      return std::unexpected(attachment_io_error("failed to close existing image attachment", *destination, metadata.storage_path, cause));
    if (int const cause = ava::core::fsync_with_restart(directory->get()); cause != 0)
      return std::unexpected(attachment_io_error("failed to sync image attachment publication", *destination, metadata.storage_path, cause));
    return {};
  };
  ava::core::UniqueFd existing_file(ava::core::open_beneath(directory->get(), destination->filename(), O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC));
  if (existing_file)
    return reuse_existing(std::move(existing_file));
  if (errno != ENOENT)
    return std::unexpected(attachment_io_error("failed to inspect existing image attachment", *destination, metadata.storage_path, errno));

  // Prefer an unnamed inode. Filesystems without O_TMPFILE use an exclusively
  // created basename in this same held directory; neither path publishes bytes
  // until the completed inode has been synced.
  ava::core::UniqueFd file(::openat(directory->get(), ".", O_TMPFILE | O_WRONLY | O_CLOEXEC, 0600));
  std::string temporary_name;
  AttachmentTemporaryName temporary(directory->get(), file, temporary_name);
  if (!file)
  {
    int const cause = errno;
    if (cause != EOPNOTSUPP && cause != EISDIR && cause != EINVAL)
      return std::unexpected(attachment_io_error("failed to create private image attachment inode", *destination, metadata.storage_path, cause));
    for (int attempt = 0; attempt < 16; ++attempt)
    {
      temporary_name = ".ava-attachment-" + ava::core::make_id("tmp");
      file.reset(::openat(directory->get(), temporary_name.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600));
      if (file)
      {
        temporary.arm();
        break;
      }
      if (errno != EEXIST)
        return std::unexpected(attachment_io_error("failed to create temporary image attachment", *destination, metadata.storage_path, errno));
    }
    if (!file)
      return std::unexpected(attachment_io_error("image attachment temporary names collided", *destination, metadata.storage_path, EEXIST));
  }
  auto fail = [&](ava::core::Error error) -> ava::core::VoidResult {
    if (int const cleanup_error = temporary.remove_checked(); cleanup_error != 0)
      error.with_context("temporary_cleanup", cleanup_error == ESTALE ? "temporary name changed; replacement preserved" : std::strerror(cleanup_error));
    else if (!temporary_name.empty())
    {
      if (int const sync_error = ava::core::fsync_with_restart(directory->get()); sync_error != 0)
        error.with_context("temporary_cleanup_sync", std::strerror(sync_error));
    }
    return std::unexpected(std::move(error));
  };
  if (::fchmod(file.get(), 0600) != 0)
    return fail(attachment_io_error("failed to set image attachment file permissions", *destination, metadata.storage_path, errno));
  if (int const cause = ava::core::write_all(file.get(), bytes); cause != 0)
    return fail(attachment_io_error("failed to write image attachment destination", *destination, metadata.storage_path, cause));
  if (int const cause = ava::core::fsync_with_restart(file.get()); cause != 0)
    return fail(attachment_io_error("failed to sync image attachment data", *destination, metadata.storage_path, cause));

  int publication_error = 0;
  if (::linkat(file.get(), "", directory->get(), destination->filename().c_str(), AT_EMPTY_PATH) != 0)
  {
    publication_error = errno;
    if (publication_error == ENOENT || publication_error == EPERM)
    {
      // Match session recovery's unprivileged descriptor-link publication.
      auto const descriptor_path = ava::core::descriptor_proc_path(file.get());
      publication_error = ::linkat(AT_FDCWD, descriptor_path.c_str(), directory->get(), destination->filename().c_str(), AT_SYMLINK_FOLLOW) == 0 ? 0 : errno;
    }
  }
  // Descriptor-link publication never consults the temporary basename. Remove
  // only our verified temporary inode; the final directory sync covers both the
  // published name and this unlink. RAII also covers exceptional unwinding.
  if (int const cause = temporary.remove_checked(); cause != 0)
    return fail(attachment_io_error("failed to clean temporary image attachment", *destination, metadata.storage_path, cause));
  if (publication_error == EEXIST)
  {
    existing_file.reset(ava::core::open_beneath(directory->get(), destination->filename(), O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC));
    if (!existing_file)
      return fail(attachment_io_error("failed to open raced image attachment", *destination, metadata.storage_path, errno));
    auto reused = reuse_existing(std::move(existing_file));
    if (!reused)
      return fail(std::move(reused.error()));
    return {};
  }
  if (publication_error != 0)
    return fail(attachment_io_error("failed to publish image attachment", *destination, metadata.storage_path, publication_error));
  // After link publication, failures leave a complete file for verified retry.
  if (int const cause = ava::core::fsync_with_restart(directory->get()); cause != 0)
    return std::unexpected(attachment_io_error("failed to sync image attachment publication", *destination, metadata.storage_path, cause));
  if (int const cause = file.close_checked(); cause != 0)
    return std::unexpected(attachment_io_error("failed to close image attachment destination", *destination, metadata.storage_path, cause));
  return {};
}

ava::core::Error bytes_attachment_error(ava::core::ErrorCategory category, std::string_view message)
{
  return ava::core::Error(category, std::string(message));
}

ava::core::Result<ImageAttachmentRef> import_detected_image_attachment(SessionStore const& store,
                                                                       std::string_view bytes,
                                                                       DetectedImageType const& detected)
{
  auto const sha = ava::core::sha256_hex(bytes);
  auto const id = "img_" + sha.substr(0, 16);
  auto const storage_path = "attachments/" + id + std::string(detected.extension);
  ImageAttachmentRef metadata{.id = id,
                              .mime_type = std::string(detected.mime_type),
                              .storage_path = storage_path,
                              .sha256 = sha,
                              .byte_size = bytes.size()};

  if (auto written = write_stored_attachment(store, metadata, bytes); !written)
    return std::unexpected(std::move(written.error()));
  return metadata;
}

}  // namespace

std::filesystem::path attachment_storage_root(SessionStore const& store)
{
  return store.session_path().parent_path() / (store.options_session_id_copy() + ".attachments");
}

ava::core::Result<std::filesystem::path> resolve_attachment_storage_path(SessionStore const& store,
                                                                           std::string_view storage_path)
{
  if (!valid_attachment_storage_path(storage_path)) {
    return std::unexpected(attachment_error(ava::core::ErrorCategory::InvalidArgument,
                                            "image attachment storage path is invalid", storage_path));
  }
  auto const root = attachment_storage_root(store).lexically_normal();
  auto const resolved = (root / std::filesystem::path(storage_path)).lexically_normal();
  auto const root_string = root.string();
  auto const resolved_string = resolved.string();
  if (resolved_string != root_string && resolved_string.rfind(root_string + std::string(1, std::filesystem::path::preferred_separator), 0) != 0) {
    return std::unexpected(attachment_error(ava::core::ErrorCategory::PermissionDenied,
                                            "image attachment path escapes session attachment storage", storage_path));
  }
  return resolved;
}

ava::core::Result<ImageAttachmentRef> import_image_attachment(SessionStore const& store,
                                                              std::filesystem::path const& source_path)
{
  auto bytes = read_import_source(source_path);
  if (!bytes) return std::unexpected(std::move(bytes.error()));

  auto const detected = detect_image_type(*bytes);
  if (!detected) {
    return std::unexpected(source_attachment_error(ava::core::ErrorCategory::InvalidArgument,
                                                   "image attachment source has unsupported image format",
                                                   source_path));
  }

  return import_detected_image_attachment(store, *bytes, *detected);
}

ava::core::Result<ImageAttachmentRef> import_image_attachment_bytes(
    SessionStore const& store,
    std::string_view bytes,
    std::optional<std::string_view> expected_mime_type)
{
  if (bytes.empty() || bytes.size() > kMaxImageAttachmentBytes) {
    auto error = bytes_attachment_error(ava::core::ErrorCategory::InvalidArgument,
                                        "image attachment byte size is invalid");
    error.with_context("max_bytes", std::to_string(kMaxImageAttachmentBytes));
    error.with_context("byte_size", std::to_string(bytes.size()));
    return std::unexpected(std::move(error));
  }

  auto const detected = detect_image_type(bytes);
  if (!detected) {
    return std::unexpected(bytes_attachment_error(ava::core::ErrorCategory::InvalidArgument,
                                                 "image attachment bytes have unsupported image format"));
  }
  if (expected_mime_type && *expected_mime_type != detected->mime_type) {
    auto error = bytes_attachment_error(ava::core::ErrorCategory::InvalidArgument,
                                        "image attachment MIME type does not match detected bytes");
    error.with_context("expected_mime_type", std::string(*expected_mime_type));
    error.with_context("detected_mime_type", std::string(detected->mime_type));
    return std::unexpected(std::move(error));
  }

  return import_detected_image_attachment(store, bytes, *detected);
}

ava::core::Result<LoadedImageAttachment> load_image_attachment(SessionStore const& store,
                                                               ImageAttachmentRef const& attachment)
{
  if (attachment.byte_size == 0 || attachment.byte_size > kMaxImageAttachmentBytes) {
    return std::unexpected(attachment_error(ava::core::ErrorCategory::InvalidArgument,
                                            "image attachment byte size is invalid", attachment.storage_path));
  }
  if (attachment.sha256.size() != 64 || !hex_string(attachment.sha256)) {
    return std::unexpected(attachment_error(ava::core::ErrorCategory::InvalidArgument,
                                            "image attachment sha256 is invalid", attachment.storage_path));
  }
  auto path = resolve_attachment_storage_path(store, attachment.storage_path);
  if (!path) return std::unexpected(std::move(path.error()));
  auto file = open_stored_attachment(store, attachment.storage_path);
  if (!file)
    return std::unexpected(std::move(file.error()));
  auto snapshot = read_attachment_snapshot(file->get(), *path, attachment.byte_size);
  if (!snapshot)
  {
    snapshot.error().with_context("storage_path", attachment.storage_path);
    return std::unexpected(std::move(snapshot.error()));
  }
  auto bytes = std::move(*snapshot);
  if (ava::core::sha256_hex(bytes) != attachment.sha256)
  {
    return std::unexpected(attachment_error(ava::core::ErrorCategory::InvalidArgument,
                                            "image attachment sha256 does not match metadata", attachment.storage_path));
  }
  return LoadedImageAttachment{.metadata = attachment, .bytes = std::move(bytes), .path = *path};
}

ava::core::VoidResult copy_image_attachment(SessionStore const& source, SessionStore const& destination, ImageAttachmentRef const& attachment)
{
  auto loaded = load_image_attachment(source, attachment);
  if (!loaded)
    return std::unexpected(std::move(loaded.error()));
  return write_stored_attachment(destination, attachment, loaded->bytes);
}

}  // namespace ava::session

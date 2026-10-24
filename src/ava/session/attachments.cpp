#include "sys.h"
#include "ava/session/attachments.h"
#include "ava/session/session_store.h"
#include "ava/core/UniqueFd.h"
#include "ava/core/error.h"
#include "ava/core/fd_io.h"
#include "ava/core/open_beneath.h"

#include <array>
#include <cctype>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <optional>
#include <sstream>
#include <string_view>
#include <utility>
#include <vector>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace ava::session {
namespace {

struct DetectedImageType {
  std::string_view mime_type;
  std::string_view extension;
};

bool has_control_byte(std::string_view value)
{
  for (auto const ch : value) {
    auto const byte = static_cast<unsigned char>(ch);
    if (byte < 0x20 || byte == 0x7F) return true;
  }
  return false;
}

bool hex_string(std::string_view value)
{
  for (auto const ch : value) {
    if (std::isxdigit(static_cast<unsigned char>(ch)) == 0) return false;
  }
  return true;
}

bool valid_attachment_storage_path(std::string_view path)
{
  if (path.empty() || path.size() > 4096 || has_control_byte(path)) return false;
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

  ava::core::UniqueFd file(ava::core::open_beneath(directory->get(), destination->filename(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600));
  if (!file)
  {
    int const cause = errno;
    if (cause != EEXIST)
      return std::unexpected(attachment_io_error("failed to create image attachment destination", *destination, metadata.storage_path, cause));
    // Content-addressed duplicates are read-only. Never truncate a raced-in file
    // (including a hardlink), and never reopen its parent by pathname.
    file.reset(ava::core::open_beneath(directory->get(), destination->filename(), O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC));
    if (!file)
      return std::unexpected(attachment_io_error("failed to open existing image attachment destination", *destination, metadata.storage_path, errno));
    auto existing = read_attachment_snapshot(file.get(), *destination, metadata.byte_size);
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
    return {};
  }
  if (::fchmod(file.get(), 0600) != 0)
    return std::unexpected(attachment_io_error("failed to set image attachment file permissions", *destination, metadata.storage_path, errno));
  if (int const cause = ava::core::write_all(file.get(), bytes); cause != 0)
    return std::unexpected(attachment_io_error("failed to write image attachment destination", *destination, metadata.storage_path, cause));
  if (int const cause = file.close_checked(); cause != 0)
    return std::unexpected(attachment_io_error("failed to close image attachment destination", *destination, metadata.storage_path, cause));
  return {};
}

std::uint32_t rotr(std::uint32_t value, std::uint32_t count)
{
  return (value >> count) | (value << (32U - count));
}

std::array<std::uint8_t, 32> sha256(std::string_view bytes)
{
  static constexpr std::array<std::uint32_t, 64> k{
      0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U,
      0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U, 0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U,
      0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU, 0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
      0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U, 0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U,
      0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U, 0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
      0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U, 0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
      0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
      0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U, 0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U};

  std::vector<std::uint8_t> data(bytes.begin(), bytes.end());
  auto const bit_len = static_cast<std::uint64_t>(data.size()) * 8ULL;
  data.push_back(0x80U);
  while ((data.size() % 64U) != 56U) data.push_back(0U);
  for (int shift = 56; shift >= 0; shift -= 8) data.push_back(static_cast<std::uint8_t>((bit_len >> shift) & 0xFFU));

  std::array<std::uint32_t, 8> hash{0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
                                    0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U};
  for (std::size_t chunk = 0; chunk < data.size(); chunk += 64U) {
    std::array<std::uint32_t, 64> w{};
    for (std::size_t i = 0; i < 16U; ++i) {
      auto const offset = chunk + i * 4U;
      w[i] = (static_cast<std::uint32_t>(data[offset]) << 24U) |
             (static_cast<std::uint32_t>(data[offset + 1]) << 16U) |
             (static_cast<std::uint32_t>(data[offset + 2]) << 8U) | static_cast<std::uint32_t>(data[offset + 3]);
    }
    for (std::size_t i = 16U; i < 64U; ++i) {
      auto const s0 = rotr(w[i - 15U], 7U) ^ rotr(w[i - 15U], 18U) ^ (w[i - 15U] >> 3U);
      auto const s1 = rotr(w[i - 2U], 17U) ^ rotr(w[i - 2U], 19U) ^ (w[i - 2U] >> 10U);
      w[i] = w[i - 16U] + s0 + w[i - 7U] + s1;
    }
    auto a = hash[0];
    auto b = hash[1];
    auto c = hash[2];
    auto d = hash[3];
    auto e = hash[4];
    auto f = hash[5];
    auto g = hash[6];
    auto h = hash[7];
    for (std::size_t i = 0; i < 64U; ++i) {
      auto const s1 = rotr(e, 6U) ^ rotr(e, 11U) ^ rotr(e, 25U);
      auto const ch = (e & f) ^ ((~e) & g);
      auto const temp1 = h + s1 + ch + k[i] + w[i];
      auto const s0 = rotr(a, 2U) ^ rotr(a, 13U) ^ rotr(a, 22U);
      auto const maj = (a & b) ^ (a & c) ^ (b & c);
      auto const temp2 = s0 + maj;
      h = g;
      g = f;
      f = e;
      e = d + temp1;
      d = c;
      c = b;
      b = a;
      a = temp1 + temp2;
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
  for (std::size_t index = 0; index < hash.size(); ++index) {
    digest[index * 4U] = static_cast<std::uint8_t>((hash[index] >> 24U) & 0xFFU);
    digest[index * 4U + 1U] = static_cast<std::uint8_t>((hash[index] >> 16U) & 0xFFU);
    digest[index * 4U + 2U] = static_cast<std::uint8_t>((hash[index] >> 8U) & 0xFFU);
    digest[index * 4U + 3U] = static_cast<std::uint8_t>(hash[index] & 0xFFU);
  }
  return digest;
}

std::string sha256_hex(std::string_view bytes)
{
  auto digest = sha256(bytes);
  std::ostringstream out;
  out << std::hex << std::setfill('0');
  for (auto const byte : digest) out << std::setw(2) << static_cast<int>(byte);
  return out.str();
}

ava::core::Error bytes_attachment_error(ava::core::ErrorCategory category, std::string_view message)
{
  return ava::core::Error(category, std::string(message));
}

ava::core::Result<ImageAttachmentRef> import_detected_image_attachment(SessionStore const& store,
                                                                       std::string_view bytes,
                                                                       DetectedImageType const& detected)
{
  auto const sha = sha256_hex(bytes);
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
  if (sha256_hex(bytes) != attachment.sha256) {
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

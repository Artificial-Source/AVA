#pragma once

#include "ava/debug/print_members_on.h"
#include "ava/core/result.h"

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace ava::session {

class SessionStore;

inline constexpr std::size_t kMaxImageAttachmentBytes = 20 * 1024 * 1024;

struct ImageAttachmentRef {
  std::string id;
  std::string mime_type;
  std::string storage_path;
  std::string sha256;
  std::size_t byte_size = 0;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

struct LoadedImageAttachment {
  ImageAttachmentRef metadata;
  std::string bytes;
  std::filesystem::path path;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

[[nodiscard]] std::filesystem::path attachment_storage_root(SessionStore const& store);
[[nodiscard]] ava::core::Result<std::filesystem::path> resolve_attachment_storage_path(SessionStore const& store,
                                                                                        std::string_view storage_path);
// Successful import/copy persists data before no-replace name publication and
// syncs the destination directory/created ancestors before returning metadata.
// O_TMPFILE is preferred; unsupported filesystems use an exclusive same-directory
// temporary name with RAII cleanup. Neither path replaces an existing name.
// Failures before publication leave no final name; published content permits retry.
[[nodiscard]] ava::core::Result<ImageAttachmentRef> import_image_attachment(SessionStore const& store,
                                                                            std::filesystem::path const& source_path);
[[nodiscard]] ava::core::Result<ImageAttachmentRef> import_image_attachment_bytes(
    SessionStore const& store,
    std::string_view bytes,
    std::optional<std::string_view> expected_mime_type = std::nullopt);
[[nodiscard]] ava::core::Result<LoadedImageAttachment> load_image_attachment(SessionStore const& store,
                                                                             ImageAttachmentRef const& attachment);

// Copies verified bytes through held no-follow source/destination descriptors.
// Existing destination content is verified read-only rather than overwritten.
[[nodiscard]] ava::core::VoidResult copy_image_attachment(SessionStore const& source, SessionStore const& destination, ImageAttachmentRef const& attachment);

}  // namespace ava::session

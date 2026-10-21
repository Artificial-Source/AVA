#pragma once

#include "ava/tools/secure_workspace.h"
#include "ava/tools/tool_context.h"
#include "ava/core/AnchorOpen.h"
#include "ava/core/result.h"

#include <filesystem>
#include <variant>

namespace ava::tools::file_tools_internal {

[[nodiscard]] ava::core::VoidResult reject_permission_rules_file_mutation(ToolContext const& context, std::filesystem::path const& path);

// Probe before descending: recursive_directory_iterator otherwise ends the
// entire walk when it cannot open a child directory.
[[nodiscard]] bool directory_is_listable(std::filesystem::path const& directory);

// Select descriptor authority for either the logical or physical root spelling.
// nullptr means an ordinary outside path; missing strict authority is an error.
[[nodiscard]] ava::core::Result<SecureWorkspace const*> secure_workspace_for(ToolContext const& context, std::filesystem::path const& path);

// Keep the original owner, including AnchorOpen's held external identity,
// rather than duplicating descriptors merely to change their wrapper type.
struct ReadFileHandle
{
  struct ExternalFile
  {
    ava::core::AnchorOpen owner;
    std::uintmax_t size = 0;
    AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
  };

  std::variant<SecureWorkspaceHandle, ExternalFile> owner;

  [[nodiscard]] int fd() const noexcept;
  [[nodiscard]] std::uintmax_t size() const noexcept;
  [[nodiscard]] std::filesystem::path const& path() const noexcept;
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

// Open a read_file target while context.secure_workspace is installed. Paths
// selected by secure_workspace_for open beneath the anchored root; approved
// lexically outside paths require context.anchor_set and use its held-descriptor
// external resolution (no final symlink, regular files only).
[[nodiscard]] ava::core::Result<ReadFileHandle> open_secure_read_file(ToolContext const& context, std::filesystem::path const& path);

}  // namespace ava::tools::file_tools_internal

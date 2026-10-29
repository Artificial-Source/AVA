#pragma once

#include "ava/debug/print_members_on.h"
#include "ava/lsp/lsp_client.h"

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include "debug.h"

namespace ava::lsp {

enum class BuiltinServerStatus
{
  Disabled,
  Available,
  NotFound,
  Unsafe,
};

struct BuiltinDiscoveryOptions
{
  bool use_default_search_directories = true;
  std::vector<std::filesystem::path> system_directories;
  std::vector<std::filesystem::path> user_directories;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

struct BuiltinServerInspection
{
  std::string id;
  BuiltinServerStatus status = BuiltinServerStatus::Disabled;
  std::string reason;
  std::optional<ExecutableIdentity> executable = std::nullopt;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

struct BuiltinServerRecipe
{
  std::string id;
  std::vector<std::string> arguments;
  std::vector<std::string> file_extensions;
  std::string language_id;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

[[nodiscard]] std::vector<BuiltinServerRecipe> builtin_server_recipes();
[[nodiscard]] std::vector<BuiltinServerInspection> inspect_builtin_servers(std::vector<std::string> const& enabled_ids,
                                                                           std::filesystem::path const& workspace_root,
                                                                           std::shared_ptr<ava::core::AnchorSet const> anchor_set,
                                                                           BuiltinDiscoveryOptions const& options = {});
[[nodiscard]] std::filesystem::path select_builtin_server_root(std::string_view server_id, std::filesystem::path const& document_path,
                                                               std::filesystem::path const& workspace_root,
                                                               std::shared_ptr<ava::core::AnchorSet const> anchor_set);
// Defined inline: generated debug printers in AVA::debug call this, and AVA::debug links
// after AVA::lsp, so an out-of-line definition can be missing from static link order.
[[nodiscard]] constexpr std::string_view to_string(BuiltinServerStatus status) noexcept
{
  switch (status)
  {
    case BuiltinServerStatus::Disabled:
      return "disabled";
    case BuiltinServerStatus::Available:
      return "available";
    case BuiltinServerStatus::NotFound:
      return "not-found";
    case BuiltinServerStatus::Unsafe:
      return "unsafe";
  }
  return "unsafe";
}

}  // namespace ava::lsp

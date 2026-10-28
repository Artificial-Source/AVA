#pragma once

#include "ava/app/commands.h"
#include "ava/permissions/permission_rules.h"

#include <filesystem>
#include <string>

namespace ava::app {

// Focused human summary for a durable permission rule. Safe for list/receipt
// primary lines and later command-palette glue; never includes rule ids,
// recipe-key hashes, raw command bodies, or request/resolver identities.
[[nodiscard]] std::string format_permission_rule_summary(ava::permissions::PersistentPermissionRule const& rule,
                                                         std::filesystem::path const& workspace_dir);

// Action parsing and list filters ignore surrounding whitespace.
[[nodiscard]] ava::core::Result<CommandResult> run_permissions_command(runtime::session_ts& unlocked_session, CommandRequest const& request);

}  // namespace ava::app

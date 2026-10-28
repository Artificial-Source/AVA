#pragma once

#include "ava/app/commands.h"
#include "ava/session/record.h"

#include <cstddef>
#include <filesystem>
#include <functional>
#include <string_view>
#include <vector>

namespace ava::app {

// `/import` is intentionally bounded independently of legacy runtime reads:
// files are at most 8 MiB, records are strictly less than 1 MiB, and archives
// contain at most 16,384 entries.
inline constexpr std::size_t kMaxSessionImportFileBytes = 8U * 1024U * 1024U;
inline constexpr std::size_t kMaxSessionImportLineBytes = 1024U * 1024U;
inline constexpr std::size_t kMaxSessionImportEntries = 16384;

// Reads and validates an import without publishing it. The optional race-test hook
// belongs to this invocation only and runs after descriptor validation. Linux
// close EINTR means ownership is released; close is never retried.
[[nodiscard]] ava::core::Result<std::vector<ava::session::SessionEntry>> load_import_session_entries(std::filesystem::path const& path,
                                                                                                     std::function<void()> const& after_open_for_test = {});

[[nodiscard]] ava::core::Result<CommandResult> run_sessions_command(runtime::session_ts& unlocked_session, std::string_view query = {});
[[nodiscard]] ava::core::Result<CommandResult> run_fork_command(runtime::session_ts& unlocked_session, std::string_view name = {},
                                                                std::string_view branch_from_entry_id = {});
[[nodiscard]] ava::core::Result<CommandResult> run_clone_command(runtime::session_ts& unlocked_session, std::string_view name = {});
[[nodiscard]] ava::core::Result<CommandResult> run_new_session_command(runtime::session_ts& unlocked_session, std::string_view name = {});
[[nodiscard]] ava::core::Result<CommandResult> run_resume_command(runtime::session_ts& unlocked_session, std::string_view session_id);
[[nodiscard]] ava::core::Result<CommandResult> run_name_command(runtime::session_ts& unlocked_session, std::string_view name);
[[nodiscard]] ava::core::Result<CommandResult> run_labels_command(runtime::session_ts& unlocked_session, std::string_view labels);
[[nodiscard]] ava::core::Result<CommandResult> run_mode_command(runtime::session_ts& unlocked_session);
// Freshness probes revalidate opened regular files and never read beyond their loaded byte count.
[[nodiscard]] ava::core::Result<CommandResult> run_context_command(runtime::session_ts& unlocked_session, std::string_view query = {});
[[nodiscard]] ava::core::Result<CommandResult> run_stats_command(runtime::session_ts& unlocked_session);
[[nodiscard]] ava::core::Result<CommandResult> run_compact_command(runtime::session_ts& unlocked_session, CommandRequest const& request);
[[nodiscard]] ava::core::Result<CommandResult> run_import_command(runtime::session_ts& unlocked_session, std::string_view argument);
[[nodiscard]] ava::core::Result<CommandResult> run_export_command(runtime::session_ts& unlocked_session, CommandRequest const& request);
// Explicit fail-closed recovery after a verified terminal/drained append failure.
[[nodiscard]] ava::core::Result<CommandResult> run_recover_persistence_command(runtime::session_ts& unlocked_session);

}  // namespace ava::app

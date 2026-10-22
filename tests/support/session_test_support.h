#pragma once

#include "ava/session/validation.h"

#include <filesystem>
#include <string>
#include <string_view>
#include <sys/types.h>

namespace ava::tests {
bool session_replay_has_issue(session::SessionReplayValidation const& validation, session::SessionReplayIssueKind kind);
std::string session_test_tiny_png_bytes();
// Writes exact bytes, creating parents, truncating the file and applying mode
// (0600 by default). Throws with path context on setup/write/close/chmod failure.
// Does not alter existing ancestor permissions; security fixtures own those.
void write_test_file(std::filesystem::path const& path, std::string_view bytes, mode_t mode = 0600);
// Writes an owner-only executable using the checked writer (mode 0700).
// Returns true on success for fixture guards; setup/write/chmod failures throw.
bool write_executable_for_test(std::filesystem::path const& path, std::string_view bytes);
// Binary convenience using write_test_file's checked private-file defaults.
void write_session_test_binary_file(std::filesystem::path const& path, std::string_view bytes);
std::string read_session_test_binary_file(std::filesystem::path const& path);
}  // namespace ava::tests

#pragma once

#include "ava/debug/print_members_on.h"

#include <memory>
#include <string>
#include <string_view>

namespace ava::debug {

// Route libcwd diagnostics to a private per-run log named from log_stem.
//
// When AVA_DEBUG_OUTPUT_DIR is a nonempty absolute path, construction safely
// opens <directory>/<log_stem>.libcwd.log and redirects libcwd there. The
// directory and file must remain current-user-owned, non-symlink objects with
// exact private permissions. An unset or empty directory disables file output
// without being an error. The stem may contain ASCII letters, digits, '.',
// '_' and '-' only, preventing an environment-provided test name from
// influencing the directory traversal.
class LibcwdOutputSink final
{
 public:
  explicit LibcwdOutputSink(std::string const log_stem);
  ~LibcwdOutputSink();

  LibcwdOutputSink(LibcwdOutputSink const&) = delete;
  LibcwdOutputSink& operator=(LibcwdOutputSink const&) = delete;
  LibcwdOutputSink(LibcwdOutputSink&&) = delete;
  LibcwdOutputSink& operator=(LibcwdOutputSink&&) = delete;

  // Return true when a validated private log is open and owns libcwd output.
  [[nodiscard]] bool enabled() const noexcept;

  // Return false only when a configured directory or log could not be opened safely.
  [[nodiscard]] bool setup_succeeded() const noexcept;

  // Return the actionable setup error, or an empty string after successful setup.
  [[nodiscard]] std::string const& setup_error() const noexcept;

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace ava::debug

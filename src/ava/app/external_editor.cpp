#include "sys.h"
#include "ava/debug/print_members_on.h"
#include "ava/app/interactive_internal.h"
#include "ava/core/UniqueFd.h"
#include "ava/core/environment_value.h"
#include "ava/core/error.h"
#include "ava/core/fd_io.h"

#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

namespace ava::app::interactive_internal {
namespace {

constexpr std::uintmax_t kExternalEditorMaxBytes = 1024 * 1024;

ava::core::Error errno_io_error(ava::core::ErrorCategory category, std::string message)
{
  auto error = ava::core::Error(category, std::move(message));
  error.with_context("cause", std::strerror(errno));
  return error;
}

class ScopedEnvVar
{
 public:
  ScopedEnvVar(std::string name, std::string value) : name_(std::move(name))
  {
    if (auto const* previous = std::getenv(name_.c_str()))
      previous_ = std::string(previous);
    set_ = ::setenv(name_.c_str(), value.c_str(), 1) == 0;
  }

  ScopedEnvVar(ScopedEnvVar const&) = delete;
  ScopedEnvVar& operator=(ScopedEnvVar const&) = delete;

  ~ScopedEnvVar()
  {
    if (!set_)
      return;
    if (previous_)
      static_cast<void>(::setenv(name_.c_str(), previous_->c_str(), 1));
    else
      static_cast<void>(::unsetenv(name_.c_str()));
  }

  [[nodiscard]] bool ok() const { return set_; }

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT

 private:
  std::string name_;
  std::optional<std::string> previous_;
  bool set_ = false;
};

class ScopedTempFile
{
 public:
  ScopedTempFile(std::filesystem::path path, ava::core::UniqueFd fd) : path_(std::move(path)), fd_(std::move(fd)) { }
  ScopedTempFile(ScopedTempFile const&) = delete;
  ScopedTempFile& operator=(ScopedTempFile const&) = delete;

  ~ScopedTempFile()
  {
    fd_.reset();
    if (!path_.empty())
    {
      std::error_code ignored;
      std::filesystem::remove(path_, ignored);
    }
  }

  [[nodiscard]] int fd() const { return fd_.get(); }
  [[nodiscard]] std::filesystem::path const& path() const { return path_; }

  [[nodiscard]] bool close()
  {
    if (auto const error = fd_.close_checked(); error != 0)
    {
      errno = error;
      return false;
    }
    return true;
  }

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT

 private:
  std::filesystem::path path_;
  ava::core::UniqueFd fd_;
};

}  // namespace

ava::core::Result<std::optional<std::string>> edit_text_with_external_editor(std::string_view initial_text)
{
  auto editor = ava::core::nonempty_environment_value("VISUAL");
  if (!editor)
    editor = ava::core::nonempty_environment_value("EDITOR");
  if (!editor)
  {
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "external editor requires VISUAL or EDITOR"));
  }

  std::error_code temp_error;
  auto const temp_dir = std::filesystem::temp_directory_path(temp_error);
  if (temp_error)
  {
    auto error = ava::core::Error(ava::core::ErrorCategory::Io, "failed to resolve temporary directory");
    error.with_context("cause", temp_error.message());
    return std::unexpected(std::move(error));
  }

  auto path_template = (temp_dir / "ava-editor-XXXXXX").string();
  std::vector<char> mutable_path(path_template.begin(), path_template.end());
  mutable_path.push_back('\0');
  ava::core::UniqueFd fd(::mkostemp(mutable_path.data(), O_CLOEXEC));
  if (!fd)
    return std::unexpected(errno_io_error(ava::core::ErrorCategory::Io, "failed to create external editor temp file"));

  ScopedTempFile temp_file(std::filesystem::path(mutable_path.data()), std::move(fd));
  if (::fchmod(temp_file.fd(), S_IRUSR | S_IWUSR) != 0)
    return std::unexpected(errno_io_error(ava::core::ErrorCategory::Io, "failed to secure external editor temp file"));
  if (auto const error_number = ava::core::write_all(temp_file.fd(), initial_text); error_number != 0)
  {
    auto error = ava::core::Error(ava::core::ErrorCategory::Io, "failed to write external editor temp file");
    error.with_context("cause", std::strerror(error_number));
    return std::unexpected(std::move(error));
  }
  if (!temp_file.close())
    return std::unexpected(errno_io_error(ava::core::ErrorCategory::Io, "failed to close external editor temp file"));

  ScopedEnvVar file_env("AVA_EXTERNAL_EDITOR_FILE", temp_file.path().string());
  if (!file_env.ok())
    return std::unexpected(errno_io_error(ava::core::ErrorCategory::Io, "failed to prepare external editor file environment"));

  auto const command = std::string("exec ") + *editor + " \"$AVA_EXTERNAL_EDITOR_FILE\"";
  int const status = std::system(command.c_str());
  if (status == -1)
    return std::unexpected(errno_io_error(ava::core::ErrorCategory::Io, "failed to launch external editor"));
  if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
    return std::optional<std::string>{};

  // Open first, then validate and bound reads on that exact descriptor. Editors
  // may replace a file atomically, but symlinks and special files are not drafts.
  auto input = ava::core::open_regular_nofollow(AT_FDCWD, temp_file.path().c_str());
  if (!input)
    return std::unexpected(ava::core::open_regular_error(input.error(), "external editor temp file"));
  auto edited = ava::core::read_descriptor_bounded(input->fd.get(), kExternalEditorMaxBytes);
  if (!edited && edited.error() == EFBIG)
  {
    auto error = ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "external editor draft is too large");
    error.with_context("limit", std::to_string(kExternalEditorMaxBytes));
    return std::unexpected(std::move(error));
  }

  if (!edited)
  {
    auto error = ava::core::Error(ava::core::ErrorCategory::Io, "failed to read external editor temp file");
    error.with_context("cause", std::strerror(edited.error()));
    return std::unexpected(std::move(error));
  }
  return std::optional<std::string>{std::move(*edited)};
}

}  // namespace ava::app::interactive_internal

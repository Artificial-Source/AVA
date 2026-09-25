#include "sys.h"
#include "ava/app/interactive_internal.h"
#include "ava/core/error.h"

#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

namespace ava::app::interactive_internal {

constexpr std::uintmax_t kExternalEditorMaxBytes = 1024 * 1024;

ava::core::Error errno_io_error(ava::core::ErrorCategory category, std::string message)
{
  auto error = ava::core::Error(category, std::move(message));
  error.with_context("cause", std::strerror(errno));
  return error;
}

std::optional<std::string> env_value(std::string_view name)
{
  auto const* value = std::getenv(std::string(name).c_str());
  if (value == nullptr || *value == '\0')
    return std::nullopt;
  return std::string(value);
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

 private:
  std::string name_;
  std::optional<std::string> previous_;
  bool set_ = false;
};

class ScopedTempFile
{
 public:
  explicit ScopedTempFile(std::filesystem::path path, int fd) : path_(std::move(path)), fd_(fd) { }
  ScopedTempFile(ScopedTempFile const&) = delete;
  ScopedTempFile& operator=(ScopedTempFile const&) = delete;

  ~ScopedTempFile()
  {
    if (fd_ >= 0)
      static_cast<void>(::close(fd_));
    if (!path_.empty())
    {
      std::error_code ignored;
      std::filesystem::remove(path_, ignored);
    }
  }

  [[nodiscard]] int fd() const { return fd_; }
  [[nodiscard]] std::filesystem::path const& path() const { return path_; }

  [[nodiscard]] bool close()
  {
    if (fd_ < 0)
      return true;
    if (::close(fd_) != 0)
      return false;
    fd_ = -1;
    return true;
  }

 private:
  std::filesystem::path path_;
  int fd_ = -1;
};

bool write_all_fd(int fd, std::string_view text)
{
  std::size_t offset = 0;
  while (offset < text.size())
  {
    auto const written = ::write(fd, text.data() + offset, text.size() - offset);
    if (written < 0)
    {
      if (errno == EINTR)
        continue;
      return false;
    }
    if (written == 0)
      return false;
    offset += static_cast<std::size_t>(written);
  }
  return true;
}

ava::core::Result<std::optional<std::string>> edit_text_with_external_editor(std::string_view initial_text)
{
  auto editor = env_value("VISUAL");
  if (!editor)
    editor = env_value("EDITOR");
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
  int const fd = ::mkstemp(mutable_path.data());
  if (fd < 0)
    return std::unexpected(errno_io_error(ava::core::ErrorCategory::Io, "failed to create external editor temp file"));

  ScopedTempFile temp_file(std::filesystem::path(mutable_path.data()), fd);
  if (::fchmod(temp_file.fd(), S_IRUSR | S_IWUSR) != 0)
    return std::unexpected(errno_io_error(ava::core::ErrorCategory::Io, "failed to secure external editor temp file"));
  if (!write_all_fd(temp_file.fd(), initial_text))
    return std::unexpected(errno_io_error(ava::core::ErrorCategory::Io, "failed to write external editor temp file"));
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

  std::error_code size_error;
  auto const edited_size = std::filesystem::file_size(temp_file.path(), size_error);
  if (size_error)
  {
    auto error = ava::core::Error(ava::core::ErrorCategory::Io, "failed to inspect external editor temp file");
    error.with_context("cause", size_error.message());
    return std::unexpected(std::move(error));
  }
  if (edited_size > kExternalEditorMaxBytes)
  {
    auto error = ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "external editor draft is too large");
    error.with_context("limit", std::to_string(kExternalEditorMaxBytes));
    return std::unexpected(std::move(error));
  }

  std::ifstream input(temp_file.path(), std::ios::binary);
  if (!input)
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Io, "failed to read external editor temp file"));
  std::string edited((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
  return std::optional<std::string>{std::move(edited)};
}

}  // namespace ava::app::interactive_internal

#include "sys.h"
#include "ava/debug/libcwd_output_sink.h"

#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <ostream>
#include <streambuf>
#include <string>
#include <string_view>
#include <system_error>
#include <ext/stdio_filebuf.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include "debug.h"

namespace ava::debug {
namespace {

class UniqueFd final
{
 public:
  explicit UniqueFd(int fd = -1) noexcept : fd_(fd) { }
  ~UniqueFd()
  {
    if (fd_ >= 0)
      ::close(fd_);
  }

  UniqueFd(UniqueFd const&) = delete;
  UniqueFd& operator=(UniqueFd const&) = delete;

  [[nodiscard]] int get() const noexcept { return fd_; }
  [[nodiscard]] int release() noexcept
  {
    int const fd = fd_;
    fd_ = -1;
    return fd;
  }

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT

 private:
  int fd_;
};

std::string errno_message(std::string_view operation, std::filesystem::path const& path, int error_number)
{
  return std::string(operation) + " '" + path.string() + "': " + std::error_code(error_number, std::generic_category()).message();
}

bool same_identity(struct stat const& lhs, struct stat const& rhs)
{
  return lhs.st_dev == rhs.st_dev && lhs.st_ino == rhs.st_ino;
}

bool valid_log_stem(std::string_view log_stem)
{
  if (log_stem.empty() || log_stem == "." || log_stem == "..")
    return false;
  for (unsigned char character : log_stem)
  {
    bool const ascii_alphanumeric = (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') || (character >= '0' && character <= '9');
    if (!ascii_alphanumeric && character != '.' && character != '_' && character != '-')
      return false;
  }
  return true;
}

}  // namespace

struct LibcwdOutputSink::Impl
{
  std::unique_ptr<__gnu_cxx::stdio_filebuf<char>> file_buffer;
  std::unique_ptr<std::ostream> file_stream;
  bool enabled = false;
  std::string error;

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

// Redirect libcwd before its initialization can emit parser diagnostics, then
// safely create the configured private destination when one was requested.
LibcwdOutputSink::LibcwdOutputSink(std::string const log_stem) : impl_(std::make_unique<Impl>())
{
  char const* configured_directory = std::getenv("AVA_DEBUG_OUTPUT_DIR");
  if (configured_directory == nullptr || configured_directory[0] == '\0')
    return;

  if (!valid_log_stem(log_stem))
  {
    impl_->error = "libcwd log stem must contain only ASCII letters, digits, '.', '_' or '-': '" + log_stem + "'";
    return;
  }

  std::filesystem::path output_directory(configured_directory);
  if (!output_directory.is_absolute())
  {
    impl_->error = "AVA_DEBUG_OUTPUT_DIR must be an absolute path: '" + output_directory.string() + "'";
    return;
  }

  while (output_directory != output_directory.root_path() && output_directory.filename().empty())
    output_directory = output_directory.parent_path();
  if (output_directory.filename().empty())
  {
    impl_->error = "AVA_DEBUG_OUTPUT_DIR must name a final directory component: '" + output_directory.string() + "'";
    return;
  }

  bool created = false;
  mode_t const previous_umask = ::umask(0077);
  int const mkdir_result = ::mkdir(output_directory.c_str(), 0700);
  int const mkdir_error = errno;
  ::umask(previous_umask);
  if (mkdir_result == 0)
  {
    created = true;
  }
  else if (mkdir_error != EEXIST)
  {
    impl_->error = errno_message("cannot create AVA_DEBUG_OUTPUT_DIR", output_directory, mkdir_error);
    return;
  }

  struct stat path_status{};
  if (::lstat(output_directory.c_str(), &path_status) != 0)
  {
    impl_->error = errno_message("cannot inspect AVA_DEBUG_OUTPUT_DIR", output_directory, errno);
    return;
  }
  if (S_ISLNK(path_status.st_mode) || !S_ISDIR(path_status.st_mode))
  {
    impl_->error = "AVA_DEBUG_OUTPUT_DIR is not a non-symlink directory: '" + output_directory.string() + "'";
    return;
  }
  if (path_status.st_uid != ::geteuid())
  {
    impl_->error = "AVA_DEBUG_OUTPUT_DIR is not owned by the current user: '" + output_directory.string() + "'";
    return;
  }
  if (!created && (path_status.st_mode & 07777) != 0700)
  {
    impl_->error = "AVA_DEBUG_OUTPUT_DIR must have exact mode 0700: '" + output_directory.string() + "'";
    return;
  }

  UniqueFd directory_fd(::open(output_directory.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
  if (directory_fd.get() < 0)
  {
    impl_->error = errno_message("cannot open AVA_DEBUG_OUTPUT_DIR", output_directory, errno);
    return;
  }

  struct stat descriptor_status{};
  if (::fstat(directory_fd.get(), &descriptor_status) != 0)
  {
    impl_->error = errno_message("cannot verify AVA_DEBUG_OUTPUT_DIR descriptor", output_directory, errno);
    return;
  }
  if (!same_identity(path_status, descriptor_status) || !S_ISDIR(descriptor_status.st_mode) || descriptor_status.st_uid != ::geteuid())
  {
    impl_->error = "AVA_DEBUG_OUTPUT_DIR changed while it was being opened: '" + output_directory.string() + "'";
    return;
  }
  if (created && ::fchmod(directory_fd.get(), 0700) != 0)
  {
    impl_->error = errno_message("cannot set AVA_DEBUG_OUTPUT_DIR mode", output_directory, errno);
    return;
  }
  if (::fstat(directory_fd.get(), &descriptor_status) != 0)
  {
    impl_->error = errno_message("cannot reverify AVA_DEBUG_OUTPUT_DIR descriptor", output_directory, errno);
    return;
  }
  if ((descriptor_status.st_mode & 07777) != 0700)
  {
    impl_->error = "AVA_DEBUG_OUTPUT_DIR must have exact mode 0700: '" + output_directory.string() + "'";
    return;
  }

  std::string const filename = log_stem + ".libcwd.log";
  std::filesystem::path const display_path = output_directory / filename;
  UniqueFd output_fd(::openat(directory_fd.get(), filename.c_str(), O_WRONLY | O_CREAT | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC, 0600));
  if (output_fd.get() < 0)
  {
    impl_->error = errno_message("cannot open libcwd log", display_path, errno);
    return;
  }

  struct stat file_status{};
  if (::fstat(output_fd.get(), &file_status) != 0)
  {
    impl_->error = errno_message("cannot verify libcwd log", display_path, errno);
    return;
  }
  if (!S_ISREG(file_status.st_mode) || file_status.st_uid != ::geteuid() || file_status.st_nlink != 1)
  {
    impl_->error = "libcwd log must be a current-user regular file with link count 1: '" + display_path.string() + "'";
    return;
  }
  if (::fchmod(output_fd.get(), 0600) != 0)
  {
    impl_->error = errno_message("cannot set libcwd log mode", display_path, errno);
    return;
  }
  if (::ftruncate(output_fd.get(), 0) != 0 || ::lseek(output_fd.get(), 0, SEEK_SET) < 0)
  {
    impl_->error = errno_message("cannot truncate libcwd log", display_path, errno);
    return;
  }

  int const descriptor_flags = ::fcntl(output_fd.get(), F_GETFL);
  if (descriptor_flags < 0 || ::fcntl(output_fd.get(), F_SETFL, descriptor_flags & ~O_NONBLOCK) != 0)
  {
    impl_->error = errno_message("cannot prepare libcwd log", display_path, errno);
    return;
  }

  // The descriptor-taking GNU stream buffer owns and closes the fd. Keep the
  // ostream separately so both objects have explicit sanitizer-visible RAII.
  impl_->file_buffer = std::make_unique<__gnu_cxx::stdio_filebuf<char>>(output_fd.release(), std::ios::out);
  if (!impl_->file_buffer->is_open())
  {
    impl_->file_buffer.reset();
    impl_->error = "cannot attach a stream to libcwd log: '" + display_path.string() + "'";
    return;
  }
  impl_->file_stream = std::make_unique<std::ostream>(impl_->file_buffer.get());
  *impl_->file_stream << std::unitbuf;
  Debug(libcw_do.set_ostream(impl_->file_stream.get()));
  impl_->enabled = true;
}

LibcwdOutputSink::~LibcwdOutputSink()
{
  // set_ostream waits for the configured libcwd stream mutex, so no writer can
  // retain the file stream when its owning RAII objects are destroyed. Standard
  // error remains alive through static teardown; the implementation-owned null
  // stream does not.
  if (impl_->file_stream)
    impl_->file_stream->flush();
  impl_->file_stream.reset();
  impl_->file_buffer.reset();
}

bool LibcwdOutputSink::enabled() const noexcept
{
  return impl_->enabled;
}

bool LibcwdOutputSink::setup_succeeded() const noexcept
{
  return impl_->error.empty();
}

std::string const& LibcwdOutputSink::setup_error() const noexcept
{
  return impl_->error;
}

}  // namespace ava::debug

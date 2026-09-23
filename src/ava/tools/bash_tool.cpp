#include "sys.h"
#include "ava/containment/containment.h"
#include "ava/core/AnchorOpen.h"
#include "ava/core/AnchorSet.h"
#include "ava/core/trusted_home.h"
#include "ava/tools/bash_tool.h"
#include "ava/tools/spill_files.h"
#include "ava/core/AnchorOpen.h"
#include "ava/core/AnchorSet.h"
#include "ava/core/Signals.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <initializer_list>
#include <limits>
#include <optional>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>
#include <dirent.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace ava::tools {
namespace {

constexpr char kDefaultStartupPath[] = "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin";
constexpr std::size_t kBashProgressByteInterval = 128 * 1024;
constexpr auto kBashProgressTimeInterval = std::chrono::seconds(2);
constexpr auto kProcessGroupGrace = std::chrono::milliseconds(300);
constexpr auto kProcessGroupKillWait = std::chrono::milliseconds(500);
constexpr auto kGateTimeout = std::chrono::milliseconds(1000);
constexpr std::size_t kCleanupEntryLimit = 4096;

class UniqueFd final
{
 public:
  explicit UniqueFd(int fd = -1) noexcept : fd_(fd) { }
  ~UniqueFd() { reset(); }
  UniqueFd(UniqueFd const&) = delete;
  UniqueFd& operator=(UniqueFd const&) = delete;
  UniqueFd(UniqueFd&& other) noexcept : fd_(std::exchange(other.fd_, -1)) { }
  UniqueFd& operator=(UniqueFd&& other) noexcept
  {
    if (this != &other)
      reset(std::exchange(other.fd_, -1));
    return *this;
  }

  [[nodiscard]] int get() const noexcept { return fd_; }
  [[nodiscard]] int release() noexcept { return std::exchange(fd_, -1); }
  void reset(int fd = -1) noexcept
  {
    if (fd_ >= 0)
      static_cast<void>(::close(fd_));
    fd_ = fd;
  }

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT

 private:
  int fd_ = -1;
};

ava::core::Error errno_error(std::string message)
{
  auto error = ava::core::Error(ava::core::ErrorCategory::Io, std::move(message));
  error.with_context("cause", std::strerror(errno));
  return error;
}

ssize_t read_retry(int fd, void* data, std::size_t size);

bool matches_sealed_executable(struct stat const& status, ava::command::ExecutableMetadata const& sealed) noexcept
{
  return S_ISREG(status.st_mode) && static_cast<std::uintmax_t>(status.st_dev) == sealed.device && static_cast<std::uintmax_t>(status.st_ino) == sealed.inode &&
         static_cast<std::uintmax_t>(status.st_mode) == sealed.mode && static_cast<std::uintmax_t>(status.st_size) == sealed.size &&
         static_cast<std::uintmax_t>(status.st_uid) == sealed.owner && static_cast<std::uintmax_t>(status.st_gid) == sealed.group &&
         static_cast<std::uintmax_t>(status.st_nlink) == sealed.link_count && static_cast<std::int64_t>(status.st_ctim.tv_sec) == sealed.changed_seconds &&
         static_cast<std::int64_t>(status.st_ctim.tv_nsec) == sealed.changed_nanoseconds;
}

ava::core::Result<UniqueFd> open_approved_executable(ava::command::ExecutableMetadata const& sealed, std::shared_ptr<ava::core::AnchorSet const> const& anchors)
{
  if (!anchors)
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "approved executable has no shared AnchorSet"));
  auto opened = ava::core::open_readable(*anchors, sealed.requested_path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
  if (!opened)
    return std::unexpected(std::move(opened.error()));
  int const duplicate = ::fcntl(opened->fd(), F_DUPFD_CLOEXEC, STDERR_FILENO + 1);
  if (duplicate < 0)
    return std::unexpected(errno_error("approved executable descriptor could not be retained"));
  UniqueFd executable_fd(duplicate);
  struct stat status{};
  if (::fstat(executable_fd.get(), &status) != 0)
  {
    auto error = ava::core::Error(ava::core::ErrorCategory::Io, "approved executable descriptor could not be inspected; prepare and approve a new command");
    error.with_context("cause", std::strerror(errno));
    return std::unexpected(std::move(error));
  }
  if (!matches_sealed_executable(status, sealed))
  {
    return std::unexpected(
        ava::core::Error(ava::core::ErrorCategory::PermissionDenied, "approved executable changed after permission; prepare and approve a new command"));
  }
  return executable_fd;
}

ava::core::Result<UniqueFd> open_approved_cwd(ava::command::CommandPlan const& plan)
{
  if (!plan.anchor_set())
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "approved cwd has no shared AnchorSet"));
  auto opened = ava::core::open_readable(*plan.anchor_set(), plan.cwd(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (!opened)
    return std::unexpected(std::move(opened.error()));
  struct stat status{};
  auto const& sealed = plan.cwd_metadata();
  if (::fstat(opened->fd(), &status) != 0 || !S_ISDIR(status.st_mode) || static_cast<std::uintmax_t>(status.st_dev) != sealed.device ||
      static_cast<std::uintmax_t>(status.st_ino) != sealed.inode || static_cast<std::uintmax_t>(status.st_mode) != sealed.mode ||
      static_cast<std::uintmax_t>(status.st_uid) != sealed.owner || static_cast<std::uintmax_t>(status.st_gid) != sealed.group ||
      static_cast<std::int64_t>(status.st_ctim.tv_sec) != sealed.changed_seconds ||
      static_cast<std::int64_t>(status.st_ctim.tv_nsec) != sealed.changed_nanoseconds)
  {
    return std::unexpected(
        ava::core::Error(ava::core::ErrorCategory::PermissionDenied, "approved cwd changed after permission; prepare and approve a new command"));
  }
  int const duplicate = ::fcntl(opened->fd(), F_DUPFD_CLOEXEC, STDERR_FILENO + 1);
  if (duplicate < 0)
    return std::unexpected(errno_error("approved cwd descriptor could not be retained"));
  return UniqueFd(duplicate);
}

ava::core::Result<std::optional<int>> child_descriptor_exec_failure(int status_fd)
{
  int child_errno = 0;
  auto const bytes = read_retry(status_fd, &child_errno, sizeof(child_errno));
  if (bytes == 0)
    return std::optional<int>{};
  if (bytes != static_cast<ssize_t>(sizeof(child_errno)) || child_errno == 0)
  {
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Io, "sealed command child reported an invalid descriptor-exec status"));
  }
  return std::optional<int>{child_errno};
}

ava::core::Error descriptor_exec_error(int error_number)
{
  auto error =
      ava::core::Error(ava::core::ErrorCategory::Io, "approved executable could not start from its bound descriptor; prepare and approve a new command");
  error.with_context("cause", std::strerror(error_number));
  return error;
}

ssize_t read_retry(int fd, void* data, std::size_t size)
{
  while (true)
  {
    auto const bytes = ::read(fd, data, size);
    if (bytes < 0 && errno == EINTR)
      continue;
    return bytes;
  }
}

ssize_t write_retry(int fd, void const* data, std::size_t size)
{
  auto const* cursor = static_cast<char const*>(data);
  std::size_t remaining = size;
  while (remaining > 0)
  {
    auto const bytes = ::write(fd, cursor, remaining);
    if (bytes < 0 && errno == EINTR)
      continue;
    if (bytes <= 0)
      return bytes;
    cursor += bytes;
    remaining -= static_cast<std::size_t>(bytes);
  }
  return static_cast<ssize_t>(size);
}

pid_t waitpid_retry(pid_t pid, int* status, int options)
{
  while (true)
  {
    auto const waited = ::waitpid(pid, status, options);
    if (waited < 0 && errno == EINTR)
      continue;
    return waited;
  }
}

ava::core::Result<std::array<int, 2>> make_pipe_cloexec()
{
  std::array<int, 2> pipe_fds{-1, -1};
  if (::pipe2(pipe_fds.data(), O_CLOEXEC) != 0)
    return std::unexpected(errno_error("failed to create close-on-exec process pipe"));
  return pipe_fds;
}

void close_nonstandard_fds_except(std::vector<int> const& keep_fds) noexcept
{
  constexpr std::size_t kMaxKeptDescriptors = 16;
  std::array<int, kMaxKeptDescriptors> sorted{};
  std::size_t count = 0;
  for (int const fd : keep_fds)
  {
    auto const sorted_end = sorted.begin() + static_cast<std::ptrdiff_t>(count);
    if (fd <= STDERR_FILENO || std::find(sorted.begin(), sorted_end, fd) != sorted_end)
      continue;
    if (count == sorted.size())
      break;
    auto position = count;
    while (position > 0 && sorted[position - 1] > fd)
    {
      sorted[position] = sorted[position - 1];
      --position;
    }
    sorted[position] = fd;
    ++count;
  }

#if defined(SYS_close_range)
  // Close every interval around the small bounded keep set. The final range
  // also covers descriptors above _SC_OPEN_MAX if the process inherited one.
  unsigned int next = static_cast<unsigned int>(STDERR_FILENO + 1);
  for (std::size_t index = 0; index < count; ++index)
  {
    auto const keep = static_cast<unsigned int>(sorted[index]);
    if (next < keep)
      static_cast<void>(::syscall(SYS_close_range, next, keep - 1, 0u));
    next = keep + 1;
  }
  static_cast<void>(::syscall(SYS_close_range, next, ~0u, 0u));
#endif

  // Fall back to the finite descriptor-table bound and also catch any range
  // segment rejected by an older kernel. The hard shebang-depth limit keeps
  // every execution keep set well below kMaxKeptDescriptors.
  long const open_max = ::sysconf(_SC_OPEN_MAX);
  int const max_fd = open_max > 0 && open_max <= std::numeric_limits<int>::max() ? static_cast<int>(open_max) : 1024;
  auto const sorted_end = sorted.begin() + static_cast<std::ptrdiff_t>(count);
  for (int fd = STDERR_FILENO + 1; fd < max_fd; ++fd)
  {
    if (std::find(sorted.begin(), sorted_end, fd) != sorted_end)
      continue;
    static_cast<void>(::close(fd));
  }
}

bool is_canceled(ToolContext const& context)
{
  return context.cancel_requested && context.cancel_requested();
}

std::size_t logical_line_count(std::string_view text)
{
  if (text.empty())
    return 0;
  auto const newline_count = static_cast<std::size_t>(std::ranges::count(text, '\n'));
  return text.back() == '\n' ? newline_count : newline_count + 1;
}

std::size_t byte_offset_for_line(std::string_view text, std::size_t line)
{
  if (line <= 1)
    return 0;
  std::size_t current_line = 1;
  for (std::size_t index = 0; index < text.size(); ++index)
  {
    if (text[index] != '\n')
      continue;
    ++current_line;
    if (current_line == line)
      return index + 1;
  }
  return text.size();
}

void trim_to_last_lines(BashResult& result, std::size_t max_lines)
{
  auto const output_lines = logical_line_count(result.output);
  if (output_lines == 0)
    return;
  if (max_lines == 0)
  {
    result.output.clear();
    result.line_limited = true;
    return;
  }
  if (output_lines <= max_lines)
    return;
  auto const offset = byte_offset_for_line(result.output, output_lines - max_lines + 1);
  if (offset != 0)
  {
    result.output.erase(0, offset);
    result.line_limited = true;
  }
}

void trim_to_max_bytes(BashResult& result, std::size_t max_bytes)
{
  if (result.output.empty())
    return;
  if (max_bytes == 0)
  {
    result.output.clear();
    result.byte_limited = true;
    return;
  }
  if (result.output.size() <= max_bytes)
    return;
  auto offset = result.output.size() - max_bytes;
  auto const newline = result.output.find('\n', offset);
  if (newline != std::string::npos && newline + 1 < result.output.size())
    offset = newline + 1;
  result.output.erase(0, offset);
  result.byte_limited = true;
}

void append_output(BashResult& result, std::string_view chunk, BashOptions const& options, bool& saw_output, bool& previous_was_newline,
                   std::size_t& newline_count)
{
  result.total_bytes += chunk.size();
  for (char const ch : chunk)
  {
    saw_output = true;
    if (ch == '\n')
    {
      ++newline_count;
      previous_was_newline = true;
    }
    else
      previous_was_newline = false;
  }
  result.output.append(chunk);
  trim_to_last_lines(result, options.max_lines);
  trim_to_max_bytes(result, options.max_bytes);
  result.output_bytes = result.output.size();
}

void finalize_output(BashResult& result, BashOptions const& options, bool saw_output, bool previous_was_newline, std::size_t newline_count)
{
  result.total_lines = saw_output ? newline_count + (previous_was_newline ? 0 : 1) : 0;
  if (options.max_lines > 0 && result.total_lines > options.max_lines)
    result.line_limited = true;
  result.output_bytes = result.output.size();
  result.output_lines = logical_line_count(result.output);
  result.omitted_lines = result.total_lines > result.output_lines ? result.total_lines - result.output_lines : 0;
  result.truncated = result.truncated || result.byte_limited || result.line_limited || result.output_bytes < result.total_bytes;
}

ava::core::VoidResult remove_tree_at(int directory_fd, std::size_t& remaining_entries)
{
  auto duplicate = ::dup(directory_fd);
  if (duplicate < 0)
    return std::unexpected(errno_error("failed to duplicate synthetic environment cleanup directory"));
  DIR* directory = ::fdopendir(duplicate);
  if (!directory)
  {
    static_cast<void>(::close(duplicate));
    return std::unexpected(errno_error("failed to inspect synthetic environment cleanup directory"));
  }
  while (dirent* entry = ::readdir(directory))
  {
    std::string_view const name(entry->d_name);
    if (name == "." || name == "..")
      continue;
    if (remaining_entries == 0)
    {
      static_cast<void>(::closedir(directory));
      return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Io, "synthetic environment cleanup exceeded its bounded entry limit"));
    }
    --remaining_entries;
    struct stat status{};
    if (::fstatat(directory_fd, entry->d_name, &status, AT_SYMLINK_NOFOLLOW) != 0)
      continue;
    if (S_ISDIR(status.st_mode))
    {
      UniqueFd child(::openat(directory_fd, entry->d_name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
      if (child.get() >= 0)
        static_cast<void>(remove_tree_at(child.get(), remaining_entries));
      static_cast<void>(::unlinkat(directory_fd, entry->d_name, AT_REMOVEDIR));
    }
    else
      static_cast<void>(::unlinkat(directory_fd, entry->d_name, 0));
  }
  static_cast<void>(::closedir(directory));
  return {};
}

class SyntheticEnvironmentRoot final
{
 public:
  SyntheticEnvironmentRoot() = default;
  ~SyntheticEnvironmentRoot() { cleanup(); }
  SyntheticEnvironmentRoot(SyntheticEnvironmentRoot const&) = delete;
  SyntheticEnvironmentRoot& operator=(SyntheticEnvironmentRoot const&) = delete;
  SyntheticEnvironmentRoot(SyntheticEnvironmentRoot&& other) noexcept
      : root_(std::move(other.root_)),
        name_(std::move(other.name_)),
        parent_fd_(std::move(other.parent_fd_)),
        root_fd_(std::move(other.root_fd_)),
        device_(std::exchange(other.device_, 0)),
        inode_(std::exchange(other.inode_, 0))
  {
  }
  SyntheticEnvironmentRoot& operator=(SyntheticEnvironmentRoot&& other) noexcept
  {
    if (this != &other)
    {
      cleanup();
      root_ = std::move(other.root_);
      name_ = std::move(other.name_);
      parent_fd_ = std::move(other.parent_fd_);
      root_fd_ = std::move(other.root_fd_);
      device_ = std::exchange(other.device_, 0);
      inode_ = std::exchange(other.inode_, 0);
    }
    return *this;
  }

  [[nodiscard]] static ava::core::Result<SyntheticEnvironmentRoot> create(ToolContext const& context)
  {
    if (!context.anchor_set || context.spill_dir.empty())
    {
      return std::unexpected(
          ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "sealed command execution requires a shared AnchorSet and pre-opened spill anchor"));
    }
    auto selected = context.anchor_set->find_anchor(context.spill_dir);
    if (!selected || selected->anchor().root == context.anchor_set->launch_workspace_root())
    {
      return std::unexpected(
          ava::core::Error(ava::core::ErrorCategory::PermissionDenied, "sealed command synthetic environment must use a non-workspace writable anchor"));
    }
    auto opened_parent = ava::core::open_readable(*context.anchor_set, context.spill_dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (!opened_parent)
      return std::unexpected(std::move(opened_parent.error()));
    int const parent_duplicate = ::fcntl(opened_parent->fd(), F_DUPFD_CLOEXEC, STDERR_FILENO + 1);
    if (parent_duplicate < 0)
      return std::unexpected(errno_error("failed to retain synthetic environment parent descriptor"));

    SyntheticEnvironmentRoot result;
    result.parent_fd_ = UniqueFd(parent_duplicate);
    static std::atomic<std::uint64_t> sequence{0};
    for (std::size_t attempt = 0; attempt < 128; ++attempt)
    {
      auto const value = sequence.fetch_add(1, std::memory_order_relaxed);
      result.name_ = ".ava-command-" + std::to_string(static_cast<unsigned long long>(::getpid())) + "-" + std::to_string(value);
      if (::mkdirat(result.parent_fd_.get(), result.name_.c_str(), S_IRWXU) == 0)
        break;
      if (errno != EEXIST)
        return std::unexpected(errno_error("failed to create descriptor-bound synthetic command environment root"));
      result.name_.clear();
    }
    if (result.name_.empty())
      return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Io, "failed to allocate a bounded unique synthetic command environment root"));

    result.root_ = (context.spill_dir / result.name_).lexically_normal();
    result.root_fd_ = UniqueFd(::openat(result.parent_fd_.get(), result.name_.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    struct stat status{};
    if (result.root_fd_.get() < 0 || ::fstat(result.root_fd_.get(), &status) != 0 || !S_ISDIR(status.st_mode) || status.st_uid != ::geteuid() ||
        (status.st_mode & (S_IRWXG | S_IRWXO)) != 0)
    {
      return std::unexpected(
          ava::core::Error(ava::core::ErrorCategory::PermissionDenied, "private synthetic command environment root is not an owner-only directory"));
    }
    result.device_ = status.st_dev;
    result.inode_ = status.st_ino;
    for (auto const name : {"home", "xdg-config", "xdg-cache", "xdg-data", "xdg-state", "tmp"})
    {
      if (::mkdirat(result.root_fd_.get(), name, S_IRWXU) != 0)
        return std::unexpected(errno_error("failed to create descriptor-bound synthetic command environment directory"));
      struct stat child_status{};
      if (::fstatat(result.root_fd_.get(), name, &child_status, AT_SYMLINK_NOFOLLOW) != 0 || !S_ISDIR(child_status.st_mode) ||
          child_status.st_uid != ::geteuid() || (child_status.st_mode & (S_IRWXG | S_IRWXO)) != 0)
      {
        return std::unexpected(
            ava::core::Error(ava::core::ErrorCategory::PermissionDenied, "private synthetic command environment directory is not owner-only"));
      }
    }
    return result;
  }

  [[nodiscard]] std::filesystem::path const& root() const noexcept { return root_; }

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT

 private:
  void cleanup() noexcept
  {
    if (root_fd_.get() < 0)
      return;
    std::size_t remaining_entries = kCleanupEntryLimit;
    static_cast<void>(remove_tree_at(root_fd_.get(), remaining_entries));
    struct stat named_status{};
    if (parent_fd_.get() >= 0 && !name_.empty() && ::fstatat(parent_fd_.get(), name_.c_str(), &named_status, AT_SYMLINK_NOFOLLOW) == 0 &&
        S_ISDIR(named_status.st_mode) && named_status.st_dev == device_ && named_status.st_ino == inode_)
    {
      static_cast<void>(::unlinkat(parent_fd_.get(), name_.c_str(), AT_REMOVEDIR));
    }
    root_fd_.reset();
    parent_fd_.reset();
    root_.clear();
    name_.clear();
  }

  std::filesystem::path root_;
  std::string name_;
  UniqueFd parent_fd_;
  UniqueFd root_fd_;
  dev_t device_ = 0;
  ino_t inode_ = 0;
};

ava::core::Result<ava::command::CommandBuildOptions> local_command_build_options(ToolContext const& context, SyntheticEnvironmentRoot const& synthetic)
{
  ava::core::TrustedAccount const& trusted_account = ava::core::cached_trusted_account();
  // Read exactly one inherited value: startup PATH. Bound the read before
  // copying it into planning options; no child inherits it directly and no
  // execution-time PATH lookup occurs.
  ava::command::CommandLimits limits;
  char const* const inherited_path = ::getenv("PATH");
  std::string startup_path = kDefaultStartupPath;
  if (inherited_path)
  {
    auto const length = ::strnlen(inherited_path, limits.max_path_bytes + 1);
    if (length > limits.max_path_bytes)
    {
      return std::unexpected(ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "inherited startup PATH exceeds the bounded input size"));
    }
    startup_path.assign(inherited_path, length);
  }
  auto const& root = synthetic.root();
  return ava::command::CommandBuildOptions{.workspace = context.workspace_dir,
                                           .anchor_set = context.anchor_set,
                                           .trusted_home = trusted_account.home,
                                           .discover_host_user_tools = !static_cast<bool>(context.command_executor),
                                           .startup_path = std::move(startup_path),
                                           .shell = "/bin/sh",
                                           .ava_authority_roots = context.ava_authority_roots,
                                           .environment = ava::command::CommandEnvironmentOptions{.profile_id = "ava-local-bash-prompt-v2",
                                                                                                  .user = trusted_account.user,
                                                                                                  .logname = trusted_account.home,
                                                                                                  .home = root / "home",
                                                                                                  .xdg_config_home = root / "xdg-config",
                                                                                                  .xdg_cache_home = root / "xdg-cache",
                                                                                                  .xdg_data_home = root / "xdg-data",
                                                                                                  .xdg_state_home = root / "xdg-state",
                                                                                                  .tmpdir = root / "tmp"},
                                           .workspace_script_recipes = {},
                                           .limits = limits};
}

ava::core::Result<std::vector<std::string>> execution_argv(ava::command::CommandPlan const& plan)
{
  if (!plan.resolved_executable())
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "sealed command plan has no resolved executable"));
  if (plan.execution_domain() == ava::command::CommandExecutionDomain::RawShell)
  {
    return std::vector<std::string>{plan.resolved_executable()->executable.requested_path.string(), "-c", plan.raw_shell_text()};
  }
  if (plan.argv().empty())
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "sealed command plan has no argv"));
  return plan.argv();
}

struct DescriptorExecution
{
  std::vector<UniqueFd> descriptors;
  std::vector<std::size_t> retained_script_descriptors;
  std::vector<std::string> argv;
  std::size_t executable_descriptor = 0;
};

ava::core::Result<DescriptorExecution> prepare_descriptor_execution(ava::command::CommandPlan const& plan, std::vector<std::string> argv)
{
  if (!plan.resolved_executable())
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "sealed command plan has no resolved executable"));
  auto const& resolved = *plan.resolved_executable();
  if (!resolved.shebang_fully_resolved)
  {
    return std::unexpected(
        ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "sealed command shebang chain cannot execute until every interpreter is descriptor-bound"));
  }

  DescriptorExecution execution;
  execution.argv = std::move(argv);
  execution.descriptors.reserve(1 + resolved.shebang_interpreters.size());
  auto source = open_approved_executable(resolved.executable, plan.anchor_set());
  if (!source)
    return std::unexpected(std::move(source.error()));
  execution.descriptors.push_back(std::move(*source));

  std::size_t current_script_descriptor = 0;
  for (auto const& interpreter : resolved.shebang_interpreters)
  {
    auto opened = open_approved_executable(interpreter.interpreter, plan.anchor_set());
    if (!opened)
      return std::unexpected(std::move(opened.error()));
    execution.descriptors.push_back(std::move(*opened));
    auto const interpreter_descriptor = execution.descriptors.size() - 1;

    std::vector<std::string> transformed;
    if (interpreter.resolved_via_env)
    {
      // The preceding simple /usr/bin/env <name> would discard its own argv
      // and PATH-resolve <name>. Invoke the already-sealed target descriptor
      // directly while retaining the script descriptor argument.
      if (execution.argv.size() < 2)
        return std::unexpected(ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "sealed env shebang argv is incomplete"));
      transformed.reserve(execution.argv.size() - 1);
      transformed.push_back(interpreter.interpreter.requested_path.string());
      transformed.insert(transformed.end(), execution.argv.begin() + 2, execution.argv.end());
    }
    else
    {
      transformed.reserve(execution.argv.size() + 2);
      transformed.push_back(interpreter.interpreter.requested_path.string());
      if (!interpreter.argument.empty())
        transformed.push_back(interpreter.argument);
      transformed.push_back("/proc/self/fd/" + std::to_string(execution.descriptors[current_script_descriptor].get()));
      transformed.insert(transformed.end(), execution.argv.begin() + 1, execution.argv.end());
      if (std::ranges::find(execution.retained_script_descriptors, current_script_descriptor) == execution.retained_script_descriptors.end())
        execution.retained_script_descriptors.push_back(current_script_descriptor);
    }
    execution.argv = std::move(transformed);
    current_script_descriptor = interpreter_descriptor;
  }
  execution.executable_descriptor = current_script_descriptor;
  return execution;
}

bool descriptor_execution_is_fresh(DescriptorExecution const& execution, ava::command::CommandPlan const& plan)
{
  if (!plan.resolved_executable() || execution.descriptors.size() != 1 + plan.resolved_executable()->shebang_interpreters.size())
    return false;
  for (std::size_t index = 0; index < execution.descriptors.size(); ++index)
  {
    auto const& sealed = index == 0 ? plan.resolved_executable()->executable : plan.resolved_executable()->shebang_interpreters[index - 1].interpreter;
    struct stat status{};
    if (::fstat(execution.descriptors[index].get(), &status) != 0)
      return false;
    if (matches_sealed_executable(status, sealed))
      continue;
    // A pathname replacement after descriptor binding unlinks the approved
    // inode and changes only its ctime/link count. Retain that already-open
    // authority, but reject mode, size, owner, or identity changes.
    bool const safely_unlinked = S_ISREG(status.st_mode) && static_cast<std::uintmax_t>(status.st_dev) == sealed.device &&
                                 static_cast<std::uintmax_t>(status.st_ino) == sealed.inode && static_cast<std::uintmax_t>(status.st_mode) == sealed.mode &&
                                 static_cast<std::uintmax_t>(status.st_size) == sealed.size && static_cast<std::uintmax_t>(status.st_uid) == sealed.owner &&
                                 static_cast<std::uintmax_t>(status.st_gid) == sealed.group && status.st_nlink == 0 && sealed.link_count == 1;
    if (!safely_unlinked)
      return false;
  }
  return true;
}

ava::tools::CommandExecutionPlanMetadata execution_metadata(ava::command::CommandPlan const& plan,
                                                            ava::permissions::CommandPermissionMetadata const& permission)
{
  return ava::tools::CommandExecutionPlanMetadata{.fingerprint = plan.fingerprint(),
                                                  .execution_domain = plan.execution_domain(),
                                                  .level = permission.level,
                                                  .family = permission.family,
                                                  .backend_maximum_scope = permission.backend_maximum_scope,
                                                  .resolved_executable = permission.resolved_executable,
                                                  .cwd = plan.cwd(),
                                                  .executor_identity_verified = false,
                                                  .containment_available = permission.containment_available,
                                                  .containment_profile_id = permission.containment_profile_id,
                                                  .containment_network_allowed = permission.containment_network_allowed,
                                                  .containment_applied = false,
                                                  .containment_network_mode = {}};
}

bool group_exists(pid_t pgid)
{
  if (::kill(-pgid, 0) == 0)
    return true;
  return errno == EPERM;
}

ava::core::VoidResult wait_for_group_exit(pid_t pgid, std::chrono::steady_clock::time_point deadline)
{
  while (group_exists(pgid))
  {
    if (std::chrono::steady_clock::now() >= deadline)
      return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Io, "verified command process group did not exit within its finite cleanup deadline"));
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  if (errno != ESRCH && errno != EPERM)
    return std::unexpected(errno_error("failed to verify command process-group cleanup"));
  return {};
}

ava::core::VoidResult signal_verified_group(pid_t pgid, int signal_number)
{
  if (::kill(-pgid, signal_number) == 0 || errno == ESRCH)
    return {};
  return std::unexpected(errno_error("failed to signal verified command process group"));
}

ava::core::VoidResult terminate_verified_group(pid_t pgid, pid_t leader, pid_t sentinel, int& status, bool& leader_reaped, bool& sentinel_reaped)
{
  if (auto signaled = signal_verified_group(pgid, SIGTERM); !signaled)
    return signaled;
  // The grace period applies to the entire verified group, not just the
  // leader. Even when the leader has already exited, background children with
  // TERM handlers receive their finite grace window before SIGKILL.
  auto const grace_deadline = std::chrono::steady_clock::now() + kProcessGroupGrace;
  while (std::chrono::steady_clock::now() < grace_deadline)
  {
    if (!leader_reaped)
    {
      auto const waited = waitpid_retry(leader, &status, WNOHANG);
      if (waited == leader)
        leader_reaped = true;
      else if (waited < 0 && errno != ECHILD)
        return std::unexpected(errno_error("failed to wait for command process leader during cleanup"));
    }
    if (!sentinel_reaped && sentinel > 0)
    {
      int sentinel_status = 0;
      auto const waited = waitpid_retry(sentinel, &sentinel_status, WNOHANG);
      if (waited == sentinel)
        sentinel_reaped = true;
      else if (waited < 0 && errno != ECHILD)
        return std::unexpected(errno_error("failed to wait for command sentinel during cleanup"));
    }
    if (!group_exists(pgid))
      break;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  if (group_exists(pgid))
  {
    if (auto signaled = signal_verified_group(pgid, SIGKILL); !signaled)
      return signaled;
  }
  if (!leader_reaped)
  {
    auto const waited = waitpid_retry(leader, &status, 0);
    if (waited == leader)
      leader_reaped = true;
    else if (waited < 0 && errno != ECHILD)
      return std::unexpected(errno_error("failed to reap command process leader during cleanup"));
  }
  if (!sentinel_reaped && sentinel > 0)
  {
    int sentinel_status = 0;
    static_cast<void>(waitpid_retry(sentinel, &sentinel_status, 0));
    sentinel_reaped = true;
  }
  return wait_for_group_exit(pgid, std::chrono::steady_clock::now() + kProcessGroupKillWait);
}

ava::core::VoidResult reap_owned_child(pid_t child)
{
  if (child <= 0)
    return {};
  int status = 0;
  auto const waited = waitpid_retry(child, &status, WNOHANG);
  if (waited == child || (waited < 0 && errno == ECHILD))
    return {};
  if (waited < 0)
    return std::unexpected(errno_error("failed to wait for owned child during cleanup"));
  // The child is still alive; kill and reap it directly. This is a verified
  // PID from fork, not a PGID signal.
  static_cast<void>(::kill(child, SIGKILL));
  static_cast<void>(waitpid_retry(child, &status, 0));
  return {};
}

ava::core::VoidResult gate_failure_cleanup(pid_t child, UniqueFd& control_write)
{
  // Closing the gate is the child-side cleanup signal before exec. Do not
  // signal a process group until parent and child have both verified it.
  // The sentinel sibling has not been forked yet at gate-failure time, so
  // only the leader child needs reaping.
  control_write.reset();
  int status = 0;
  auto const deadline = std::chrono::steady_clock::now() + kProcessGroupGrace;
  while (std::chrono::steady_clock::now() < deadline)
  {
    auto const waited = waitpid_retry(child, &status, WNOHANG);
    if (waited == child || (waited < 0 && errno == ECHILD))
      return {};
    if (waited < 0)
      return std::unexpected(errno_error("failed to wait for gated command child"));
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  static_cast<void>(::kill(child, SIGKILL));
  static_cast<void>(waitpid_retry(child, &status, 0));
  return {};
}

}  // namespace

ava::core::Result<BashResult> run_bash(ToolContext const& context, std::string_view command, BashOptions options)
{
  DoutEntering(dc::notice, "run_bash(command_bytes=" << command.size() << ", " << options << ")");

  if (command.empty())
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "command must not be empty"));
  if (!ava::command::command_mode_is_enabled(context.command_runtime))
  {
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::InvalidArgument,
                                            "local sealed bash execution requires the Enabled command runtime mode; Legacy and PromptOnly do not execute"));
  }

  auto synthetic = SyntheticEnvironmentRoot::create(context);
  if (!synthetic)
    return std::unexpected(std::move(synthetic.error()));
  auto build_options = local_command_build_options(context, *synthetic);
  if (!build_options)
    return std::unexpected(std::move(build_options.error()));
  auto intent = options.invocation_source == BashOptions::InvocationSource::UserRawShell
                    ? ava::command::CommandIntent::raw_shell(std::string(command), build_options->limits)
                    : ava::command::CommandIntent::compatibility(std::string(command), build_options->limits);
  if (!intent)
    return std::unexpected(std::move(intent.error()));
  // Prepare exactly once, before permission. The scoped synthetic root remains
  // live through freshness validation, fork, and child completion.
  auto preparation = ava::command::prepare_command(*intent, *build_options);
  if (!preparation)
    return std::unexpected(std::move(preparation.error()));
  auto argv_strings = execution_argv(preparation->plan());
  if (!argv_strings)
    return std::unexpected(std::move(argv_strings.error()));

  auto const tool_name = context.permission_tool_name.empty() ? std::string("bash") : context.permission_tool_name;
  bool const unverified_delegated_executor = static_cast<bool>(context.command_executor);
  // Prepare a development containment plan for commands that execute mutable
  // project code. The plan probes kernel features (Landlock/seccomp) without
  // mutating parent state and determines whether standard mutable commands
  // can auto-run (containment available) or must downgrade to Ask.
  bool const needs_containment = !unverified_delegated_executor && (preparation->plan().classification().capabilities.requires_containment ||
                                                                    preparation->plan().classification().capabilities.executes_mutable_project_code ||
                                                                    preparation->plan().classification().level == ava::command::CommandLevel::Sensitive);
  std::optional<ava::containment::DevelopmentContainmentPlan> containment_plan;
  if (needs_containment)
  {
    containment_plan = ava::containment::prepare_development_containment(*preparation, preparation->plan().classification().capabilities.network_enabled);
  }
  bool const containment_available = containment_plan && ava::containment::containment_is_available(*containment_plan);
  ava::permissions::CommandContainmentInfo const containment_info{.available = containment_available,
                                                                  .profile_id = containment_plan ? containment_plan->profile_id : std::string{},
                                                                  .network_allowed = containment_plan ? containment_plan->network_allowed : false};
  if (auto permission =
          ensure_command_permission(context, command, *preparation, containment_info, unverified_delegated_executor, tool_name, "command requires permission");
      !permission)
  {
    return std::unexpected(std::move(permission.error()));
  }

  BashResult result;
  std::string process_outcome = "error";
  auto trace_process_call_id = context.trace_call_id;
  if (context.observation && trace_process_call_id.empty())
    trace_process_call_id = context.observation->next_id("process");
  struct ProcessTraceScope
  {
    ToolContext const& context;
    std::string const& call_id;
    std::string& outcome;
    BashResult const& result;
    ~ProcessTraceScope() noexcept
    {
      if (context.observation)
        context.observation->emit(ava::observability::TraceEventType::ProcessResult, context.trace_context, [this](auto& event) {
          event.call_id = call_id;
          event.phase = ava::observability::TracePhase::Process;
          event.outcome = outcome == "canceled" ? ava::observability::TraceOutcome::Canceled
                                                : (outcome == "success" ? ava::observability::TraceOutcome::Success : ava::observability::TraceOutcome::Error);
          event.fields = {{.key = "tool", .value = "bash"}, {.key = "output_bytes", .value = std::to_string(result.output_bytes)}};
          // Containment status is included only after parent verification.
          // The profile ID is safe to emit; secret paths are never included.
          if (result.containment_applied)
          {
            event.fields.push_back({.key = "containment_applied", .value = "true"});
            if (!result.containment_profile_id.empty())
              event.fields.push_back({.key = "containment_profile", .value = result.containment_profile_id});
            if (!result.containment_network_mode.empty())
              event.fields.push_back({.key = "containment_network", .value = result.containment_network_mode});
          }
        });
    }
  } trace{context, trace_process_call_id, process_outcome, result};
  auto observe_process_start = [&] {
    if (context.observation)
      context.observation->emit(ava::observability::TraceEventType::ProcessStart, context.trace_context, [&trace_process_call_id](auto& event) {
        event.call_id = trace_process_call_id;
        event.phase = ava::observability::TracePhase::Process;
        event.outcome = ava::observability::TraceOutcome::Started;
        event.fields = {{.key = "tool", .value = "bash"}};
      });
  };
  observe_process_start();
  if (is_canceled(context))
  {
    result.canceled = true;
    process_outcome = "canceled";
    return result;
  }
  if (context.command_executor)
  {
    if (auto started = announce_tool_execution_start(context); !started)
      return std::unexpected(std::move(started.error()));
    auto permission_metadata = ava::permissions::command_permission_metadata(preparation->plan(), true);
    auto executed = context.command_executor->execute(CommandExecutionRequest{
        .argv = std::move(*argv_strings),
        .cwd = preparation->plan().cwd(),
        .timeout = options.timeout,
        .output_byte_limit = options.max_bytes,
        .cancel_requested = context.cancel_requested,
        .plan_metadata = execution_metadata(preparation->plan(), permission_metadata),
        .environment_profile = CommandEnvironmentProfileContract{
            .profile_id = preparation->environment().profile_id(), .digest = preparation->environment().digest(), .local_execution_authority = false}});
    if (!executed)
      return std::unexpected(std::move(executed.error()));

    bool saw_output = false;
    bool previous_was_newline = false;
    std::size_t newline_count = 0;
    append_output(result, executed->output, options, saw_output, previous_was_newline, newline_count);
    result.exit_code = executed->exit_code;
    result.timed_out = executed->timed_out;
    result.canceled = executed->canceled;
    result.truncated = executed->truncated;
    result.byte_limited = result.byte_limited || executed->truncated;
    result.totals_known = !executed->truncated;
    // Containment status is reported by the delegated executor only after it
    // verifies the child installed containment before exec.
    result.containment_applied = executed->containment_applied;
    result.containment_profile_id = executed->containment_profile_id;
    result.containment_network_mode = executed->containment_network_mode;
    finalize_output(result, options, saw_output, previous_was_newline, newline_count);
    if (!result.totals_known)
    {
      result.total_bytes = 0;
      result.total_lines = 0;
      result.omitted_lines = 0;
    }
    if (result.truncated && !context.spill_dir.empty())
    {
      SpillBuffer spill_buffer;
      spill_buffer.append(executed->output);
      auto spill = write_spill_file(context, "bash", "txt", spill_buffer);
      if (!spill)
        return std::unexpected(std::move(spill.error()));
      result.spill_path = spill->path;
      result.spill_truncated = spill->truncated || executed->truncated;
      if (auto progress = emit_tool_progress(context, "bash output spilled " + std::to_string(spill->bytes_written) + " bytes", "running"); !progress)
        return std::unexpected(std::move(progress.error()));
    }
    if (result.output_bytes > 0)
    {
      auto const summary = result.totals_known
                               ? "bash completed with " + std::to_string(result.total_bytes) + " output bytes"
                               : "bash completed with " + std::to_string(result.output_bytes) + " retained output bytes; original total unknown";
      if (auto progress = emit_tool_progress(context, summary, "completed"); !progress)
        return std::unexpected(std::move(progress.error()));
    }
    process_outcome = result.canceled ? "canceled" : (result.timed_out ? "timed_out" : (result.exit_code == 0 ? "success" : "error"));
    return result;
  }

  auto fresh = ava::command::plan_is_fresh(preparation->plan());
  if (!fresh)
    return std::unexpected(std::move(fresh.error()));
  if (!*fresh)
  {
    return std::unexpected(
        ava::core::Error(ava::core::ErrorCategory::PermissionDenied, "sealed command plan is stale; prepare a new command under a fresh permission decision"));
  }

  auto descriptor_execution = prepare_descriptor_execution(preparation->plan(), std::move(*argv_strings));
  if (!descriptor_execution)
    return std::unexpected(std::move(descriptor_execution.error()));
  auto cwd_fd = open_approved_cwd(preparation->plan());
  if (!cwd_fd)
    return std::unexpected(std::move(cwd_fd.error()));

  // This notification is the strict adapter's existing execution boundary.
  // The approved descriptor is already held, so a namespace replacement after
  // this point cannot redirect execution to a different executable.
  if (auto started = announce_tool_execution_start(context); !started)
    return std::unexpected(std::move(started.error()));
  if (!descriptor_execution_is_fresh(*descriptor_execution, preparation->plan()))
  {
    return std::unexpected(
        ava::core::Error(ava::core::ErrorCategory::PermissionDenied, "approved bound descriptor changed before execution; prepare and approve a new command"));
  }

  std::vector<char*> argv;
  argv.reserve(descriptor_execution->argv.size() + 1);
  for (auto& argument : descriptor_execution->argv) argv.push_back(argument.data());
  argv.push_back(nullptr);
  std::vector<std::string> environment_strings;
  environment_strings.reserve(preparation->environment().entries().size() + 1);
  for (auto const& entry : preparation->environment().entries()) environment_strings.push_back(entry.key + "=" + entry.value);
  environment_strings.push_back("PWD=" + preparation->plan().cwd().string());
  std::vector<char*> environment;
  environment.reserve(environment_strings.size() + 1);
  for (auto& entry : environment_strings) environment.push_back(entry.data());
  environment.push_back(nullptr);

  auto output_pipe = make_pipe_cloexec();
  if (!output_pipe)
    return std::unexpected(std::move(output_pipe.error()));
  auto status_pipe = make_pipe_cloexec();
  if (!status_pipe)
    return std::unexpected(std::move(status_pipe.error()));
  auto control_pipe = make_pipe_cloexec();
  if (!control_pipe)
    return std::unexpected(std::move(control_pipe.error()));
  auto sentinel_pipe = make_pipe_cloexec();
  if (!sentinel_pipe)
    return std::unexpected(std::move(sentinel_pipe.error()));
  UniqueFd read_fd((*output_pipe)[0]);
  UniqueFd write_fd((*output_pipe)[1]);
  UniqueFd status_read((*status_pipe)[0]);
  UniqueFd status_write((*status_pipe)[1]);
  UniqueFd control_read((*control_pipe)[0]);
  UniqueFd control_write((*control_pipe)[1]);
  UniqueFd sentinel_read((*sentinel_pipe)[0]);
  UniqueFd sentinel_write((*sentinel_pipe)[1]);

  pid_t const pid = ::fork();
  if (pid < 0)
    return std::unexpected(errno_error("failed to fork sealed command process"));
  if (pid == 0)
  {
    // Leader child: establish the process group behind the gate, then wait
    // for the parent's release before exec. The sentinel is NOT forked here;
    // the parent forks it as an AVA-owned sibling so a direct command calling
    // waitpid(-1) never sees a sentinel child.
    read_fd.reset();
    status_read.reset();
    control_write.reset();
    sentinel_read.reset();
    sentinel_write.reset();
    int gate_status = 0;
    if (!core::Signals::reset_child_signal_state())
    {
      gate_status = errno == 0 ? EIO : errno;
      static_cast<void>(write_retry(status_write.get(), &gate_status, sizeof(gate_status)));
      _exit(127);
    }
    if (::setpgid(0, 0) != 0)
    {
      gate_status = errno == 0 ? EIO : errno;
      static_cast<void>(write_retry(status_write.get(), &gate_status, sizeof(gate_status)));
      _exit(127);
    }
    static_cast<void>(write_retry(status_write.get(), &gate_status, sizeof(gate_status)));
    char release = '\0';
    if (read_retry(control_read.get(), &release, 1) != 1 || release != 'G')
      _exit(127);
    // When containment is available, keep the status/control pipes open for
    // the containment handshake. The child applies containment after
    // stdio/cwd setup, reports status, and waits for the parent's final
    // release before exec.
    UniqueFd null_input(::open("/dev/null", O_RDONLY | O_CLOEXEC));
    if (null_input.get() < 0 || ::dup2(null_input.get(), STDIN_FILENO) < 0 || ::dup2(write_fd.get(), STDOUT_FILENO) < 0 ||
        ::dup2(write_fd.get(), STDERR_FILENO) < 0 || ::fchdir(cwd_fd->get()) != 0)
    {
      int const before_exec_error = errno == 0 ? EIO : errno;
      static_cast<void>(write_retry(status_write.get(), &before_exec_error, sizeof(before_exec_error)));
      _exit(127);
    }
    write_fd.reset();
    if (null_input.get() != STDIN_FILENO)
      null_input.reset();
    if (containment_available)
    {
      // Landlock rule paths are opened through the inherited shared AnchorSet.
      // No user code runs here; install containment first, then close every
      // inherited descriptor except the handshake and approved execution
      // descriptors before descriptor exec.
      int containment_status = 0;
      if (auto containment_result = ava::containment::apply_containment_in_child(*containment_plan); !containment_result)
        containment_status = errno == 0 ? EIO : errno;
      static_cast<void>(write_retry(status_write.get(), &containment_status, sizeof(containment_status)));
      if (containment_status != 0)
        _exit(127);
      char final_release = '\0';
      if (read_retry(control_read.get(), &final_release, 1) != 1 || final_release != 'X')
        _exit(127);
    }
    control_read.reset();
    if (!descriptor_execution_is_fresh(*descriptor_execution, preparation->plan()))
    {
      int const descriptor_error = ESTALE;
      static_cast<void>(write_retry(status_write.get(), &descriptor_error, sizeof(descriptor_error)));
      _exit(127);
    }
    std::vector<int> keep_descriptors{status_write.get(), descriptor_execution->descriptors[descriptor_execution->executable_descriptor].get()};
    keep_descriptors.reserve(keep_descriptors.size() + descriptor_execution->retained_script_descriptors.size());
    for (auto const index : descriptor_execution->retained_script_descriptors)
    {
      int const fd = descriptor_execution->descriptors[index].get();
      int const descriptor_flags = ::fcntl(fd, F_GETFD);
      if (descriptor_flags < 0 || ::fcntl(fd, F_SETFD, descriptor_flags & ~FD_CLOEXEC) != 0)
      {
        int const descriptor_error = errno == 0 ? EIO : errno;
        static_cast<void>(write_retry(status_write.get(), &descriptor_error, sizeof(descriptor_error)));
        _exit(127);
      }
      keep_descriptors.push_back(fd);
    }
    close_nonstandard_fds_except(keep_descriptors);
    ::fexecve(descriptor_execution->descriptors[descriptor_execution->executable_descriptor].get(), argv.data(), environment.data());
    int const descriptor_error = errno == 0 ? EIO : errno;
    static_cast<void>(write_retry(status_write.get(), &descriptor_error, sizeof(descriptor_error)));
    _exit(127);
  }

  write_fd.reset();
  status_write.reset();
  control_read.reset();
  bool parent_setpgid = false;
  for (int attempt = 0; attempt < 20; ++attempt)
  {
    if (::setpgid(pid, pid) == 0)
    {
      parent_setpgid = true;
      break;
    }
    if (errno != EINTR)
      break;
  }
  pollfd gate_poll{.fd = status_read.get(), .events = POLLIN, .revents = 0};
  int poll_result = 0;
  do
  {
    poll_result = ::poll(&gate_poll, 1, static_cast<int>(kGateTimeout.count()));
  } while (poll_result < 0 && errno == EINTR);
  int gate_status = EIO;
  if (poll_result <= 0 || (gate_poll.revents & POLLIN) == 0 || read_retry(status_read.get(), &gate_status, sizeof(gate_status)) != sizeof(gate_status))
  {
    static_cast<void>(gate_failure_cleanup(pid, control_write));
    return std::unexpected(poll_result == 0 ? ava::core::Error(ava::core::ErrorCategory::Io, "sealed command process-group setup timed out")
                                            : errno_error("sealed command process-group setup failed"));
  }
  pid_t const observed_pgid = ::getpgid(pid);
  bool const group_verified = gate_status == 0 && observed_pgid == pid && (parent_setpgid || observed_pgid == pid) && ::kill(-pid, 0) == 0;
  if (!group_verified)
  {
    static_cast<void>(gate_failure_cleanup(pid, control_write));
    return std::unexpected(
        ava::core::Error(ava::core::ErrorCategory::Io, "failed to establish a parent/child-acknowledged verified command process group before exec"));
  }
  // Fork the sentinel as an AVA-owned sibling of the leader. The sentinel
  // joins the leader's verified PGID and acknowledges before the leader is
  // released for exec, so a direct command calling waitpid(-1) never sees a
  // sentinel child. AVA reaps both the leader and the sentinel.
  pid_t sentinel_pid = ::fork();
  if (sentinel_pid < 0)
  {
    int status = 0;
    bool reaped = false;
    static_cast<void>(terminate_verified_group(pid, pid, 0, status, reaped, reaped));
    return std::unexpected(errno_error("failed to fork sealed command sentinel"));
  }
  if (sentinel_pid == 0)
  {
    read_fd.reset();
    write_fd.reset();
    status_read.reset();
    status_write.reset();
    control_read.reset();
    control_write.reset();
    sentinel_read.reset();
    for (auto& descriptor : descriptor_execution->descriptors) descriptor.reset();
    // The TUI installs process-level handlers. The sentinel is an internal
    // supervisor child and must retain default termination behavior so the
    // verified group can leave during the finite TERM grace period.
    if (!core::Signals::reset_child_signal_state())
      _exit(127);
    // Join the verified command process group. The leader established it
    // behind the gate; the parent verified it before forking here.
    if (::setpgid(0, pid) != 0)
      _exit(127);
    char const ack = 'S';
    static_cast<void>(write_retry(sentinel_write.get(), &ack, 1));
    sentinel_write.reset();
    close_nonstandard_fds_except({});
    // The sentinel keeps the verified group identity from being recycled
    // between leader completion and descendant cleanup. It never execs user
    // code and has no stdio or control descriptors.
    while (true) static_cast<void>(::pause());
  }
  sentinel_write.reset();
  pollfd sentinel_poll{.fd = sentinel_read.get(), .events = POLLIN, .revents = 0};
  int sentinel_poll_result = 0;
  do
  {
    sentinel_poll_result = ::poll(&sentinel_poll, 1, static_cast<int>(kGateTimeout.count()));
  } while (sentinel_poll_result < 0 && errno == EINTR);
  char sentinel_ack = '\0';
  bool sentinel_verified = false;
  if (sentinel_poll_result > 0 && (sentinel_poll.revents & POLLIN) != 0 && read_retry(sentinel_read.get(), &sentinel_ack, 1) == 1 && sentinel_ack == 'S')
  {
    pid_t const sentinel_pgid = ::getpgid(sentinel_pid);
    sentinel_verified = sentinel_pgid == pid;
  }
  sentinel_read.reset();
  if (!sentinel_verified)
  {
    static_cast<void>(reap_owned_child(sentinel_pid));
    int status = 0;
    bool leader_reaped = false;
    bool sentinel_reaped = false;
    static_cast<void>(terminate_verified_group(pid, pid, sentinel_pid, status, leader_reaped, sentinel_reaped));
    return std::unexpected(
        ava::core::Error(ava::core::ErrorCategory::Io, "failed to establish a parent/child-acknowledged sentinel in the verified command process group"));
  }
  char const release = 'G';
  if (write_retry(control_write.get(), &release, 1) != 1)
  {
    int status = 0;
    bool leader_reaped = false;
    bool sentinel_reaped = false;
    static_cast<void>(terminate_verified_group(pid, pid, sentinel_pid, status, leader_reaped, sentinel_reaped));
    return std::unexpected(errno_error("failed to release sealed command process for exec"));
  }
  if (containment_available)
  {
    // Containment handshake: the child applies Landlock + seccomp after
    // stdio/cwd setup and reports success/failure before exec. The parent
    // must acknowledge success before the child proceeds to descriptor exec.
    pollfd containment_poll{.fd = status_read.get(), .events = POLLIN, .revents = 0};
    int containment_poll_result = 0;
    do
    {
      containment_poll_result = ::poll(&containment_poll, 1, static_cast<int>(kGateTimeout.count()));
    } while (containment_poll_result < 0 && errno == EINTR);
    int containment_status = EIO;
    if (containment_poll_result <= 0 || (containment_poll.revents & POLLIN) == 0 ||
        read_retry(status_read.get(), &containment_status, sizeof(containment_status)) != sizeof(containment_status))
    {
      int cleanup_status = 0;
      bool cleanup_leader_reaped = false;
      bool cleanup_sentinel_reaped = false;
      static_cast<void>(terminate_verified_group(pid, pid, sentinel_pid, cleanup_status, cleanup_leader_reaped, cleanup_sentinel_reaped));
      return std::unexpected(containment_poll_result == 0 ? ava::core::Error(ava::core::ErrorCategory::Io, "sealed command containment installation timed out")
                                                          : errno_error("sealed command containment installation failed"));
    }
    if (containment_status != 0)
    {
      int cleanup_status = 0;
      bool cleanup_leader_reaped = false;
      bool cleanup_sentinel_reaped = false;
      static_cast<void>(terminate_verified_group(pid, pid, sentinel_pid, cleanup_status, cleanup_leader_reaped, cleanup_sentinel_reaped));
      return std::unexpected(
          ava::core::Error(ava::core::ErrorCategory::PermissionDenied, "sealed command containment installation failed before exec; user code did not start"));
    }
    char const final_release = 'X';
    if (write_retry(control_write.get(), &final_release, 1) != 1)
    {
      int cleanup_status = 0;
      bool cleanup_leader_reaped = false;
      bool cleanup_sentinel_reaped = false;
      static_cast<void>(terminate_verified_group(pid, pid, sentinel_pid, cleanup_status, cleanup_leader_reaped, cleanup_sentinel_reaped));
      return std::unexpected(errno_error("failed to release sealed command process after containment"));
    }
    control_write.reset();
    // Containment was successfully applied in the child before descriptor exec. Report
    // this only after parent verification, never in pre-permission metadata.
    result.containment_applied = true;
    result.containment_profile_id = containment_plan->profile_id;
    result.containment_network_mode = containment_plan->network_allowed ? "allowed" : "denied";
  }
  else
  {
    control_write.reset();
  }

  int const flags = ::fcntl(read_fd.get(), F_GETFL, 0);
  if (flags < 0 || ::fcntl(read_fd.get(), F_SETFL, flags | O_NONBLOCK) < 0)
  {
    int status = 0;
    bool leader_reaped = false;
    bool sentinel_reaped = false;
    static_cast<void>(terminate_verified_group(pid, pid, sentinel_pid, status, leader_reaped, sentinel_reaped));
    return std::unexpected(errno_error("failed to configure sealed command output pipe"));
  }

  SpillBuffer spill_buffer;
  bool running = true;
  bool leader_reaped = false;
  bool sentinel_reaped = false;
  int status = 0;
  bool saw_output = false;
  bool previous_was_newline = false;
  std::size_t newline_count = 0;
  auto const deadline = std::chrono::steady_clock::now() + options.timeout;
  auto last_progress = std::chrono::steady_clock::now();
  std::size_t next_progress_bytes = kBashProgressByteInterval;
  std::array<char, 4096> buffer{};
  auto maybe_emit_progress = [&]() -> ava::core::VoidResult {
    if (!context.progress_sink)
      return {};
    auto const now = std::chrono::steady_clock::now();
    if (result.total_bytes < next_progress_bytes && now - last_progress < kBashProgressTimeInterval)
      return {};
    while (result.total_bytes >= next_progress_bytes) next_progress_bytes += kBashProgressByteInterval;
    last_progress = now;
    return emit_tool_progress(context, "bash output " + std::to_string(result.total_bytes) + " bytes", "running");
  };
  auto fail_after_cleanup = [&](ava::core::Error error) -> ava::core::Result<BashResult> {
    static_cast<void>(terminate_verified_group(pid, pid, sentinel_pid, status, leader_reaped, sentinel_reaped));
    return std::unexpected(std::move(error));
  };

  while (running)
  {
    while (true)
    {
      auto const bytes = read_retry(read_fd.get(), buffer.data(), buffer.size());
      if (bytes > 0)
      {
        std::string_view const chunk(buffer.data(), static_cast<std::size_t>(bytes));
        spill_buffer.append(chunk);
        append_output(result, chunk, options, saw_output, previous_was_newline, newline_count);
        if (auto progress = maybe_emit_progress(); !progress)
          return fail_after_cleanup(std::move(progress.error()));
        continue;
      }
      if (bytes == 0 || errno == EAGAIN || errno == EWOULDBLOCK)
        break;
      return fail_after_cleanup(errno_error("failed to read sealed command output"));
    }

    if (is_canceled(context))
    {
      result.canceled = true;
      if (auto stopped = terminate_verified_group(pid, pid, sentinel_pid, status, leader_reaped, sentinel_reaped); !stopped)
        return std::unexpected(std::move(stopped.error()));
      running = false;
      break;
    }
    auto const waited = waitpid_retry(pid, &status, WNOHANG);
    if (waited == pid)
    {
      leader_reaped = true;
      // Even a successful leader may have background descendants. The
      // AVA-owned sentinel preserves the verified PGID until TERM-to-KILL
      // cleanup ends, and the grace period applies to the entire group.
      if (auto stopped = terminate_verified_group(pid, pid, sentinel_pid, status, leader_reaped, sentinel_reaped); !stopped)
        return std::unexpected(std::move(stopped.error()));
      running = false;
      break;
    }
    if (waited < 0)
      return fail_after_cleanup(errno_error("failed to wait for sealed command process"));
    if (std::chrono::steady_clock::now() >= deadline)
    {
      result.timed_out = true;
      if (auto stopped = terminate_verified_group(pid, pid, sentinel_pid, status, leader_reaped, sentinel_reaped); !stopped)
        return std::unexpected(std::move(stopped.error()));
      running = false;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  while (true)
  {
    auto const bytes = read_retry(read_fd.get(), buffer.data(), buffer.size());
    if (bytes <= 0)
    {
      if (bytes < 0 && errno != EAGAIN && errno != EWOULDBLOCK)
        return std::unexpected(errno_error("failed to drain sealed command output"));
      break;
    }
    std::string_view const chunk(buffer.data(), static_cast<std::size_t>(bytes));
    spill_buffer.append(chunk);
    append_output(result, chunk, options, saw_output, previous_was_newline, newline_count);
    if (auto progress = maybe_emit_progress(); !progress)
      return std::unexpected(std::move(progress.error()));
  }
  read_fd.reset();
  auto descriptor_failure = child_descriptor_exec_failure(status_read.get());
  status_read.reset();
  if (!descriptor_failure)
    return std::unexpected(std::move(descriptor_failure.error()));
  if (*descriptor_failure)
    return std::unexpected(descriptor_exec_error(**descriptor_failure));

  if (result.timed_out || result.canceled)
    result.exit_code = -1;
  else if (WIFEXITED(status))
    result.exit_code = WEXITSTATUS(status);
  else if (WIFSIGNALED(status))
    result.exit_code = 128 + WTERMSIG(status);
  finalize_output(result, options, saw_output, previous_was_newline, newline_count);
  if (result.truncated && !context.spill_dir.empty())
  {
    auto spill = write_spill_file(context, "bash", "txt", spill_buffer);
    if (!spill)
      return std::unexpected(std::move(spill.error()));
    result.spill_path = spill->path;
    result.spill_truncated = spill->truncated;
    if (auto progress = emit_tool_progress(context, "bash output spilled " + std::to_string(spill->bytes_written) + " bytes", "running"); !progress)
      return std::unexpected(std::move(progress.error()));
  }
  if (result.total_bytes > 0)
  {
    if (auto progress = emit_tool_progress(context, "bash completed with " + std::to_string(result.total_bytes) + " output bytes", "completed"); !progress)
      return std::unexpected(std::move(progress.error()));
  }
  process_outcome = result.canceled ? "canceled" : (result.timed_out ? "timed_out" : (result.exit_code == 0 ? "success" : "error"));
  return result;
}

}  // namespace ava::tools

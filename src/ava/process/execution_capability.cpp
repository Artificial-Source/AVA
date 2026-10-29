#include "sys.h"
#include "ava/process/execution_capability_internal.h"
#include "ava/process/execution_capability_test_support.h"
#include "ava/core/AnchorOpen.h"
#include "ava/core/AnchorSet.h"

#include <cerrno>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#if !defined(_WIN32)
#include "ava/core/UniqueFd.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace ava::process::detail {

struct ExecutionCapabilityState
{
  ~ExecutionCapabilityState()
  {
#if !defined(_WIN32)
    if (target >= 0)
      static_cast<void>(::close(target));
    if (route_base >= 0)
      static_cast<void>(::close(route_base));
    if (physical_parent >= 0)
      static_cast<void>(::close(physical_parent));
#endif
  }

  std::shared_ptr<ava::core::AnchorSet const> anchors;
  std::string logical_path;
  std::string route_relative;
  std::string final_component;
  ExpectedFileIdentityV1 identity;
  int target = -1;
  int route_base = -1;
  int physical_parent = -1;
  bool executable = false;
  bool follow_final_symlink = false;

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

}  // namespace ava::process::detail

namespace ava::process {
namespace {

std::mutex mint_hook_mutex;
std::shared_ptr<std::function<void(testing::CapabilityMintHookStage)>> mint_hook;

void invoke_mint_hook(testing::CapabilityMintHookStage stage)
{
  std::shared_ptr<std::function<void(testing::CapabilityMintHookStage)>> hook;
  {
    std::lock_guard lock(mint_hook_mutex);
    hook = mint_hook;
  }
  if (hook && *hook)
    (*hook)(stage);
}

ava::core::Error capability_error(ava::core::ErrorCategory category, std::string_view stage, int error_number = 0)
{
  auto error = ava::core::Error(category, std::string(stage));
  if (error_number > 0)
    error.with_context("errno", std::to_string(error_number));
  return error;
}

[[maybe_unused]] ava::core::Error unsupported_capability_error()
{
  return ava::core::Error(ava::core::ErrorCategory::Io, "process supervision is unsupported on this platform");
}

#if !defined(_WIN32)

std::int64_t changed_seconds(struct stat const& status) noexcept
{
#if defined(__APPLE__)
  return static_cast<std::int64_t>(status.st_ctimespec.tv_sec);
#else
  return static_cast<std::int64_t>(status.st_ctim.tv_sec);
#endif
}

std::int64_t changed_nanoseconds(struct stat const& status) noexcept
{
#if defined(__APPLE__)
  return static_cast<std::int64_t>(status.st_ctimespec.tv_nsec);
#else
  return static_cast<std::int64_t>(status.st_ctim.tv_nsec);
#endif
}

ExpectedFileIdentityV1 identity_of(struct stat const& status) noexcept
{
  return {.uid = static_cast<std::uint64_t>(status.st_uid),
          .gid = static_cast<std::uint64_t>(status.st_gid),
          .mode = static_cast<std::uint64_t>(status.st_mode),
          .nlink = static_cast<std::uint64_t>(status.st_nlink),
          .dev = static_cast<std::uint64_t>(status.st_dev),
          .inode = static_cast<std::uint64_t>(status.st_ino),
          .size = static_cast<std::uint64_t>(status.st_size),
          .ctime_sec = changed_seconds(status),
          .ctime_nsec = changed_nanoseconds(status)};
}

bool same_identity(ExpectedFileIdentityV1 const& left, ExpectedFileIdentityV1 const& right) noexcept
{
  return left.uid == right.uid && left.gid == right.gid && left.mode == right.mode && left.nlink == right.nlink && left.dev == right.dev &&
         left.inode == right.inode && left.size == right.size && left.ctime_sec == right.ctime_sec && left.ctime_nsec == right.ctime_nsec;
}

bool same_object(ExpectedFileIdentityV1 const& left, ExpectedFileIdentityV1 const& right) noexcept
{
  return left.dev == right.dev && left.inode == right.inode;
}

ava::core::Result<ava::core::UniqueFd> duplicate_above_standard(int descriptor, std::string_view stage)
{
  int const copy = ::fcntl(descriptor, F_DUPFD_CLOEXEC, STDERR_FILENO + 1);
  if (copy < 0)
    return std::unexpected(capability_error(ava::core::ErrorCategory::Io, stage, errno));
  return ava::core::UniqueFd(copy);
}

struct RouteAuthority
{
  ava::core::UniqueFd base;
  std::string relative;

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

ava::core::Result<RouteAuthority> retain_route_authority(ava::core::AnchorSet const& anchors, std::filesystem::path const& logical)
{
  auto anchored = anchors.find_anchor(logical);
  if (anchored)
  {
    auto base = duplicate_above_standard(anchored->anchor().fd, "capability route retention failed");
    if (!base)
      return std::unexpected(std::move(base.error()));
    auto relative = anchored->relative().empty() ? std::string(".") : anchored->relative().generic_string();
    return RouteAuthority{.base = std::move(*base), .relative = std::move(relative)};
  }
  if (anchored.error().category() != ava::core::ErrorCategory::PermissionDenied)
    return std::unexpected(capability_error(anchored.error().category(), "capability route selection failed"));

  ava::core::UniqueFd root(::open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NONBLOCK));
  if (root.get() < 0)
    return std::unexpected(capability_error(ava::core::ErrorCategory::Io, "capability route retention failed", errno));
  auto base = duplicate_above_standard(root.get(), "capability route retention failed");
  if (!base)
    return std::unexpected(std::move(base.error()));
  auto relative = logical == "/" ? std::string(".") : logical.relative_path().generic_string();
  return RouteAuthority{.base = std::move(*base), .relative = std::move(relative)};
}

ava::core::Result<ExpectedFileIdentityV1> observe_route(int base, std::string const& relative, bool follow_final, std::string_view stage)
{
  struct stat status{};
  int const flags = follow_final ? 0 : AT_SYMLINK_NOFOLLOW;
  if (::fstatat(base, relative.c_str(), &status, flags) != 0)
    return std::unexpected(capability_error(errno == ENOENT ? ava::core::ErrorCategory::NotFound : ava::core::ErrorCategory::Io, stage, errno));
  return identity_of(status);
}

ava::core::Result<ExpectedFileIdentityV1> observe_descriptor(int descriptor, std::string_view stage)
{
  struct stat status{};
  if (::fstat(descriptor, &status) != 0)
    return std::unexpected(capability_error(ava::core::ErrorCategory::Io, stage, errno));
  return identity_of(status);
}

bool valid_logical_path(std::filesystem::path const& logical)
{
  if (logical.empty() || !logical.is_absolute() || logical.native().find('\0') != std::string::npos)
    return false;
  if (logical.native() != logical.lexically_normal().native())
    return false;
  return logical == "/" || !logical.filename().empty();
}

bool executable_policy(ExpectedFileIdentityV1 const& identity) noexcept
{
  auto const mode = static_cast<mode_t>(identity.mode);
  return S_ISREG(mode) && identity.nlink == 1 && (mode & (S_IXUSR | S_IXGRP | S_IXOTH)) != 0 && (mode & (S_IWGRP | S_IWOTH)) == 0;
}

ava::core::Result<std::unique_ptr<detail::ExecutionCapabilityState>> mint_state(std::shared_ptr<ava::core::AnchorSet const> anchors,
                                                                                std::filesystem::path const& logical,
                                                                                std::optional<ExpectedFileIdentityV1> const& expected, bool executable)
{
  if (!anchors || anchors->anchors().empty())
    return std::unexpected(capability_error(ava::core::ErrorCategory::InvalidArgument, "capability requires retained AnchorSet authority"));
  if (!valid_logical_path(logical))
    return std::unexpected(capability_error(ava::core::ErrorCategory::InvalidArgument, "capability logical path validation failed"));

  auto route = retain_route_authority(*anchors, logical);
  if (!route)
    return std::unexpected(std::move(route.error()));
  bool const follow_final = !executable;
  auto before = observe_route(route->base.get(), route->relative, follow_final, "capability route observation before open failed");
  if (!before)
    return std::unexpected(std::move(before.error()));
  invoke_mint_hook(testing::CapabilityMintHookStage::AfterBeforeObservation);

  auto const parent_path = logical == "/" ? logical : logical.parent_path();
  auto opened_parent = ava::core::open_readable(*anchors, parent_path, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NONBLOCK);
  if (!opened_parent)
    return std::unexpected(capability_error(opened_parent.error().category(), "capability physical parent open failed"));
  auto parent = duplicate_above_standard(opened_parent->fd(), "capability physical parent retention failed");
  if (!parent)
    return std::unexpected(std::move(parent.error()));

  int flags = O_RDONLY | O_CLOEXEC | O_NONBLOCK;
  if (executable)
    flags |= O_NOFOLLOW;
  else
    flags |= O_DIRECTORY;
  auto opened_target = ava::core::open_readable(*anchors, logical, flags);
  if (!opened_target)
    return std::unexpected(capability_error(opened_target.error().category(), "capability target open failed"));
  auto target = duplicate_above_standard(opened_target->fd(), "capability target retention failed");
  if (!target)
    return std::unexpected(std::move(target.error()));
  auto opened = observe_descriptor(target->get(), "capability opened-target observation failed");
  if (!opened)
    return std::unexpected(std::move(opened.error()));
  invoke_mint_hook(testing::CapabilityMintHookStage::AfterOpenedObservation);
  auto after = observe_route(route->base.get(), route->relative, follow_final, "capability route observation after open failed");
  if (!after)
    return std::unexpected(std::move(after.error()));

  auto final_component = logical == "/" ? std::string(".") : logical.filename().string();
  auto parent_observation = observe_route(parent->get(), final_component, follow_final, "capability retained final-route observation failed");
  if (!parent_observation)
    return std::unexpected(std::move(parent_observation.error()));
  if (!same_identity(*before, *opened) || !same_identity(*opened, *after) || !same_identity(*opened, *parent_observation))
    return std::unexpected(capability_error(ava::core::ErrorCategory::Io, "capability identity changed during mint"));
  if (expected && !same_identity(*expected, *opened))
    return std::unexpected(capability_error(ava::core::ErrorCategory::PermissionDenied, "capability expected identity mismatch"));
  if (executable ? !executable_policy(*opened) : !S_ISDIR(static_cast<mode_t>(opened->mode)))
    return std::unexpected(capability_error(ava::core::ErrorCategory::PermissionDenied, "capability target policy rejected"));

  auto state = std::make_unique<detail::ExecutionCapabilityState>();
  state->anchors = std::move(anchors);
  state->logical_path = logical.string();
  state->route_relative = std::move(route->relative);
  state->final_component = std::move(final_component);
  state->identity = *opened;
  state->target = target->release();
  state->route_base = route->base.release();
  state->physical_parent = parent->release();
  state->executable = executable;
  state->follow_final_symlink = follow_final;
  return state;
}

ava::core::VoidResult refresh_state(detail::ExecutionCapabilityState const* state, bool executable)
{
  if (state == nullptr || state->target < 0 || state->route_base < 0 || state->physical_parent < 0 || state->executable != executable)
    return std::unexpected(capability_error(ava::core::ErrorCategory::InvalidArgument, "execution capability is invalid"));
  auto target = observe_descriptor(state->target, "capability freshness target observation failed");
  auto route =
      target ? observe_route(state->route_base, state->route_relative, state->follow_final_symlink, "capability freshness logical-route observation failed")
             : ava::core::Result<ExpectedFileIdentityV1>(std::unexpected(target.error()));
  auto final_route =
      route ? observe_route(state->physical_parent, state->final_component, state->follow_final_symlink, "capability freshness final-route observation failed")
            : ava::core::Result<ExpectedFileIdentityV1>(std::unexpected(route.error()));
  if (!target)
    return std::unexpected(std::move(target.error()));
  if (!route)
    return std::unexpected(std::move(route.error()));
  if (!final_route)
    return std::unexpected(std::move(final_route.error()));
  bool const fresh = executable
                         ? same_identity(state->identity, *target) && same_identity(state->identity, *route) && same_identity(state->identity, *final_route)
                         : same_object(state->identity, *target) && same_object(state->identity, *route) && same_object(state->identity, *final_route);
  if (!fresh)
    return std::unexpected(capability_error(ava::core::ErrorCategory::Io, "capability freshness identity mismatch"));
  if (executable ? !executable_policy(*target) : !S_ISDIR(static_cast<mode_t>(target->mode)))
    return std::unexpected(capability_error(ava::core::ErrorCategory::PermissionDenied, "capability freshness policy rejected"));
  return {};
}

#endif

}  // namespace

void testing::ExecutionCapabilityTestAccess::set_mint_hook(std::function<void(CapabilityMintHookStage)> hook)
{
  auto retained = std::make_shared<std::function<void(CapabilityMintHookStage)>>(std::move(hook));
  std::lock_guard lock(mint_hook_mutex);
  mint_hook = std::move(retained);
}

void testing::ExecutionCapabilityTestAccess::clear_mint_hook() noexcept
{
  std::lock_guard lock(mint_hook_mutex);
  mint_hook.reset();
}

PreopenedExecutableV1::PreopenedExecutableV1() noexcept = default;
PreopenedExecutableV1::PreopenedExecutableV1(std::unique_ptr<detail::ExecutionCapabilityState> state) noexcept : state_(std::move(state))
{
}
PreopenedExecutableV1::PreopenedExecutableV1(PreopenedExecutableV1&&) noexcept = default;
PreopenedExecutableV1& PreopenedExecutableV1::operator=(PreopenedExecutableV1&&) noexcept = default;
PreopenedExecutableV1::~PreopenedExecutableV1() = default;
bool PreopenedExecutableV1::valid() const noexcept
{
#if defined(_WIN32)
  return false;
#else
  return state_ && state_->target >= 0;
#endif
}

AnchoredWorkingDirectoryV1::AnchoredWorkingDirectoryV1() noexcept = default;
AnchoredWorkingDirectoryV1::AnchoredWorkingDirectoryV1(std::unique_ptr<detail::ExecutionCapabilityState> state) noexcept : state_(std::move(state))
{
}
AnchoredWorkingDirectoryV1::AnchoredWorkingDirectoryV1(AnchoredWorkingDirectoryV1&&) noexcept = default;
AnchoredWorkingDirectoryV1& AnchoredWorkingDirectoryV1::operator=(AnchoredWorkingDirectoryV1&&) noexcept = default;
AnchoredWorkingDirectoryV1::~AnchoredWorkingDirectoryV1() = default;
bool AnchoredWorkingDirectoryV1::valid() const noexcept
{
#if defined(_WIN32)
  return false;
#else
  return state_ && state_->target >= 0;
#endif
}

ava::core::Result<PreopenedExecutableV1> mint_preopened_executable(std::shared_ptr<ava::core::AnchorSet const> anchors, std::filesystem::path const& logical,
                                                                   std::optional<ExpectedFileIdentityV1> expected)
{
#if defined(_WIN32)
  static_cast<void>(anchors);
  static_cast<void>(logical);
  static_cast<void>(expected);
  return std::unexpected(unsupported_capability_error());
#else
  try
  {
    auto state = mint_state(std::move(anchors), logical, expected, true);
    return state ? ava::core::Result<PreopenedExecutableV1>(PreopenedExecutableV1(std::move(*state)))
                 : ava::core::Result<PreopenedExecutableV1>(std::unexpected(std::move(state.error())));
  }
  catch (...)
  {
    return std::unexpected(capability_error(ava::core::ErrorCategory::Io, "executable capability allocation failed"));
  }
#endif
}

ava::core::Result<AnchoredWorkingDirectoryV1> mint_anchored_working_directory(std::shared_ptr<ava::core::AnchorSet const> anchors,
                                                                              std::filesystem::path const& logical,
                                                                              std::optional<ExpectedFileIdentityV1> expected)
{
#if defined(_WIN32)
  static_cast<void>(anchors);
  static_cast<void>(logical);
  static_cast<void>(expected);
  return std::unexpected(unsupported_capability_error());
#else
  try
  {
    auto state = mint_state(std::move(anchors), logical, expected, false);
    return state ? ava::core::Result<AnchoredWorkingDirectoryV1>(AnchoredWorkingDirectoryV1(std::move(*state)))
                 : ava::core::Result<AnchoredWorkingDirectoryV1>(std::unexpected(std::move(state.error())));
  }
  catch (...)
  {
    return std::unexpected(capability_error(ava::core::ErrorCategory::Io, "working-directory capability allocation failed"));
  }
#endif
}

namespace detail {

bool ExecutionCapabilityAccess::logical_matches(PreopenedExecutableV1 const& capability, std::string_view logical_path) noexcept
{
  return capability.state_ && capability.state_->logical_path == logical_path;
}

bool ExecutionCapabilityAccess::logical_matches(AnchoredWorkingDirectoryV1 const& capability, std::string_view logical_path) noexcept
{
  return capability.state_ && capability.state_->logical_path == logical_path;
}

ava::core::VoidResult ExecutionCapabilityAccess::refresh(PreopenedExecutableV1 const& capability)
{
#if defined(_WIN32)
  static_cast<void>(capability);
  return std::unexpected(unsupported_capability_error());
#else
  return refresh_state(capability.state_.get(), true);
#endif
}

ava::core::VoidResult ExecutionCapabilityAccess::refresh(AnchoredWorkingDirectoryV1 const& capability)
{
#if defined(_WIN32)
  static_cast<void>(capability);
  return std::unexpected(unsupported_capability_error());
#else
  return refresh_state(capability.state_.get(), false);
#endif
}

ava::core::VoidResult ExecutionCapabilityAccess::refresh_spawn(std::optional<PreopenedExecutableV1> const& executable, std::string_view executable_logical,
                                                               std::optional<AnchoredWorkingDirectoryV1> const& cwd, std::string_view cwd_logical)
{
  if (executable)
  {
    if (!logical_matches(*executable, executable_logical))
      return std::unexpected(capability_error(ava::core::ErrorCategory::InvalidArgument, "prepared executable capability lost its logical binding"));
    auto refreshed = refresh(*executable);
    if (!refreshed)
      return std::unexpected(std::move(refreshed.error()));
  }
  if (cwd)
  {
    if (!logical_matches(*cwd, cwd_logical))
      return std::unexpected(capability_error(ava::core::ErrorCategory::InvalidArgument, "prepared cwd capability lost its logical binding"));
    auto refreshed = refresh(*cwd);
    if (!refreshed)
      return std::unexpected(std::move(refreshed.error()));
  }
  return {};
}

int ExecutionCapabilityAccess::target_descriptor(PreopenedExecutableV1 const& capability) noexcept
{
  return capability.state_ ? capability.state_->target : -1;
}

int ExecutionCapabilityAccess::target_descriptor(AnchoredWorkingDirectoryV1 const& capability) noexcept
{
  return capability.state_ ? capability.state_->target : -1;
}

void ExecutionCapabilityAccess::child_close_after_fchdir(AnchoredWorkingDirectoryV1& capability) noexcept
{
#if !defined(_WIN32)
  if (!capability.state_)
    return;
  auto close_owned = [](int& descriptor) noexcept {
    if (descriptor >= 0)
      static_cast<void>(::close(descriptor));
    descriptor = -1;
  };
  close_owned(capability.state_->target);
  close_owned(capability.state_->route_base);
  close_owned(capability.state_->physical_parent);
#else
  static_cast<void>(capability);
#endif
}

}  // namespace detail

}  // namespace ava::process

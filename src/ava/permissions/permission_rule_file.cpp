#include "sys.h"
#include "ava/permissions/permission_rules_internal.h"
#include "ava/core/NoLogString.h"
#include "ava/core/fd_io.h"

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace ava::permissions::permission_rules_internal {

constexpr std::size_t kMaxPermissionRulesFileBytes = 1024 * 1024;

namespace {

class TempPathCleanup final
{
 public:
  TempPathCleanup(int parent_fd, std::string name) : parent_fd_(parent_fd), name_(std::move(name)) { }
  TempPathCleanup(TempPathCleanup const&) = delete;
  TempPathCleanup& operator=(TempPathCleanup const&) = delete;
  TempPathCleanup(TempPathCleanup&& other) noexcept : parent_fd_(other.parent_fd_), name_(std::move(other.name_)), active_(std::exchange(other.active_, false))
  {
  }
  TempPathCleanup& operator=(TempPathCleanup&& other) noexcept
  {
    if (this != &other)
    {
      cleanup();
      parent_fd_ = other.parent_fd_;
      name_ = std::move(other.name_);
      active_ = std::exchange(other.active_, false);
    }
    return *this;
  }
  ~TempPathCleanup() { cleanup(); }

  void dismiss() noexcept { active_ = false; }

  // Owns descriptor-relative temporary cleanup authority; never generate debug output.
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT

 private:
  void cleanup() noexcept
  {
    if (active_ && parent_fd_ >= 0 && !name_.empty())
      static_cast<void>(::unlinkat(parent_fd_, name_.c_str(), 0));
    active_ = false;
  }

  int parent_fd_ = -1;
  ava::core::NoLogString name_;
  bool active_ = true;
};

}  // namespace

ava::core::VoidResult write_all_to_fd(int fd, std::string_view body, std::filesystem::path const& path)
{
  int const error_number = ava::core::write_all(fd, body);
  if (error_number == 0)
    return {};
  auto error = rule_file_error(ava::core::ErrorCategory::Io, "failed to write permission rules file", path);
  error.with_context("cause", errno_message(error_number));
  return std::unexpected(std::move(error));
}

ava::core::VoidResult fsync_fd(int fd, std::filesystem::path const& path, std::string_view message)
{
  int const error_number = ava::core::fsync_with_restart(fd);
  if (error_number == 0)
    return {};
  auto error = rule_file_error(ava::core::ErrorCategory::Io, std::string(message), path);
  error.with_context("cause", errno_message(error_number));
  return std::unexpected(std::move(error));
}

ava::core::VoidResult write_rules_file_atomic(RuleDirectory const& directory, std::string_view body)
{
  if (body.size() > kMaxPermissionRulesFileBytes)
  {
    auto error = rule_file_error(ava::core::ErrorCategory::InvalidArgument, "permission rules file capacity exceeded; remove existing rules before adding more",
                                 directory.path / directory.file_name);
    error.with_context("max_bytes", std::to_string(kMaxPermissionRulesFileBytes));
    error.with_context("candidate_bytes", std::to_string(body.size()));
    return std::unexpected(std::move(error));
  }
  if (auto checked = validate_unsafe_replace_target(directory); !checked)
    return checked;

  auto const path = directory.path / directory.file_name;
  for (int attempt = 0; attempt < 100; ++attempt)
  {
    auto const temp_name = directory.file_name + ".tmp." + std::to_string(::getpid()) + "." + std::to_string(attempt);
    ava::core::UniqueFd const fd(
        ::openat(directory.fd.get(), temp_name.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW, S_IRUSR | S_IWUSR));
    if (fd.get() < 0)
    {
      if (errno == EEXIST)
        continue;
      auto error = rule_file_error(ava::core::ErrorCategory::Io, "failed to create temporary permission rules file", directory.path / temp_name);
      error.with_context("cause", errno_message());
      return std::unexpected(std::move(error));
    }
    TempPathCleanup cleanup(directory.fd.get(), temp_name);
    struct stat opened_st{};
    if (::fstat(fd.get(), &opened_st) != 0)
    {
      auto error = rule_file_error(ava::core::ErrorCategory::Io, "failed to inspect temporary permission rules file", directory.path / temp_name);
      error.with_context("cause", errno_message());
      return std::unexpected(std::move(error));
    }
    if (!S_ISREG(opened_st.st_mode) || opened_st.st_uid != ::geteuid() || opened_st.st_nlink != 1)
    {
      return std::unexpected(rule_file_error(ava::core::ErrorCategory::PermissionDenied,
                                             "temporary permission rules file is not a private current-user regular file", directory.path / temp_name));
    }
    if (::fchmod(fd.get(), S_IRUSR | S_IWUSR) != 0)
    {
      auto error = rule_file_error(ava::core::ErrorCategory::Io, "failed to set temporary permission rules permissions", directory.path / temp_name);
      error.with_context("cause", errno_message());
      return std::unexpected(std::move(error));
    }
    if (auto written = write_all_to_fd(fd.get(), body, directory.path / temp_name); !written)
      return written;
    if (auto synced = fsync_fd(fd.get(), directory.path / temp_name, "failed to sync temporary permission rules file"); !synced)
      return synced;
    if (auto checked = validate_unsafe_replace_target(directory); !checked)
      return checked;
    if (::renameat(directory.fd.get(), temp_name.c_str(), directory.fd.get(), directory.file_name.c_str()) != 0)
    {
      auto error = rule_file_error(ava::core::ErrorCategory::Io, "failed to replace permission rules file", path);
      error.with_context("cause", errno_message());
      return std::unexpected(std::move(error));
    }
    cleanup.dismiss();
    return fsync_fd(directory.fd.get(), directory.path, "failed to sync permission rules directory");
  }
  return std::unexpected(rule_file_error(ava::core::ErrorCategory::Io, "failed to create unique temporary permission rules file", path));
}

ava::core::Result<std::optional<std::string>> read_rules_text_if_exists(RuleDirectory const& directory)
{
  auto const path = directory.path / directory.file_name;
  ava::core::UniqueFd const fd(::openat(directory.fd.get(), directory.file_name.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW));
  if (fd.get() < 0)
  {
    if (errno == ENOENT)
      return std::nullopt;
    auto error = rule_file_error(ava::core::ErrorCategory::Io, "failed to open permission rules file", path);
    error.with_context("cause", errno_message());
    return std::unexpected(std::move(error));
  }

  struct stat st{};
  if (::fstat(fd.get(), &st) != 0)
  {
    auto error = rule_file_error(ava::core::ErrorCategory::Io, "failed to inspect opened permission rules file", path);
    error.with_context("cause", errno_message());
    return std::unexpected(std::move(error));
  }
  if (!S_ISREG(st.st_mode) || st.st_uid != ::geteuid() || (st.st_mode & 07777) != (S_IRUSR | S_IWUSR))
  {
    auto error = rule_file_error(ava::core::ErrorCategory::PermissionDenied,
                                 "permission rules file must be a current-user-owned regular file with exact mode 0600", path);
    error.with_context("expected_permissions", "0600");
    return std::unexpected(std::move(error));
  }
  if (st.st_size < 0 || static_cast<std::uintmax_t>(st.st_size) > kMaxPermissionRulesFileBytes)
  {
    auto error = rule_file_error(ava::core::ErrorCategory::Io, "permission rules file is too large", path);
    error.with_context("max_bytes", std::to_string(kMaxPermissionRulesFileBytes));
    return std::unexpected(std::move(error));
  }

  std::string content;
  std::array<char, 4096> buffer{};
  while (true)
  {
    auto const bytes_read = ::read(fd.get(), buffer.data(), buffer.size());
    if (bytes_read == 0)
      break;
    if (bytes_read < 0)
    {
      auto error = rule_file_error(ava::core::ErrorCategory::Io, "failed while reading permission rules file", path);
      error.with_context("cause", errno_message());
      return std::unexpected(std::move(error));
    }
    content.append(buffer.data(), static_cast<std::size_t>(bytes_read));
    if (content.size() > kMaxPermissionRulesFileBytes)
    {
      auto error = rule_file_error(ava::core::ErrorCategory::Io, "permission rules file is too large", path);
      error.with_context("max_bytes", std::to_string(kMaxPermissionRulesFileBytes));
      return std::unexpected(std::move(error));
    }
  }
  return content;
}
ava::core::Result<std::vector<PersistentPermissionRule>> load_scope_rules(PermissionRuleStore const& store, PermissionRuleScope scope,
                                                                          RuleDirectory const* locked_directory = nullptr)
{
  auto const path = rules_file_path(store, scope);
  if (locked_directory)
  {
    auto content = read_rules_text_if_exists(*locked_directory);
    if (!content)
      return std::unexpected(std::move(content.error()));
    if (!*content)
      return std::vector<PersistentPermissionRule>{};
    return parse_rules_file(**content, path, scope);
  }

  auto directory = open_rule_directory_for_read(path, store.anchor_set);
  if (!directory)
    return std::unexpected(std::move(directory.error()));
  if (!*directory)
    return std::vector<PersistentPermissionRule>{};
  auto content = read_rules_text_if_exists(**directory);
  if (!content)
    return std::unexpected(std::move(content.error()));
  if (!*content)
    return std::vector<PersistentPermissionRule>{};
  return parse_rules_file(**content, path, scope);
}

}  // namespace ava::permissions::permission_rules_internal

namespace ava::permissions {

using namespace permission_rules_internal;

ava::core::Result<std::vector<PersistentPermissionRule>> load_persistent_permission_rules(PermissionRuleStore const& store)
{
  register_enforceable_permission_rule_files(store);
  auto global = load_scope_rules(store, PermissionRuleScope::Global);
  if (!global)
    return std::unexpected(std::move(global.error()));
  auto workspace = load_scope_rules(store, PermissionRuleScope::Workspace);
  if (!workspace)
    return std::unexpected(std::move(workspace.error()));
  global->insert(global->end(), std::make_move_iterator(workspace->begin()), std::make_move_iterator(workspace->end()));
  return *global;
}

ava::core::Result<PersistentPermissionRule> add_persistent_permission_rule(PermissionRuleStore const& store, PermissionRuleDraft draft)
{
  register_enforceable_permission_rule_files(store);
  auto rule = rule_from_draft(store, std::move(draft));
  if (!rule)
    return std::unexpected(std::move(rule.error()));

  auto const scope = rule->scope;
  auto const path = rules_file_path(store, scope);
  auto lock = acquire_rule_lock(path, store.anchor_set);
  if (!lock)
    return std::unexpected(std::move(lock.error()));
  auto rules = load_scope_rules(store, scope, &lock->directory);
  if (!rules)
    return std::unexpected(std::move(rules.error()));
  if (contains_legacy_command_allow(*rules))
  {
    return std::unexpected(
        rule_parse_error("refusing to rewrite schema-v1 command Allows; remove them explicitly before adding schema-v2 rules", path, "schema_version"));
  }
  rules->push_back(*rule);
  if (auto written = write_rules_file_atomic(lock->directory, rules_file_json(*rules)); !written)
  {
    return std::unexpected(std::move(written.error()));
  }
  return *rule;
}

ava::core::Result<PersistentPermissionRule> remove_persistent_permission_rule(PermissionRuleStore const& store, std::string_view rule_id)
{
  register_enforceable_permission_rule_files(store);
  if (!valid_identifier(rule_id))
  {
    auto error = ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "permission_rule_remove requires valid rule_id");
    error.with_context("rule_id", std::string(rule_id));
    return std::unexpected(std::move(error));
  }

  for (auto const scope : {PermissionRuleScope::Workspace, PermissionRuleScope::Global})
  {
    auto const path = rules_file_path(store, scope);
    auto lock = acquire_rule_lock(path, store.anchor_set);
    if (!lock)
      return std::unexpected(std::move(lock.error()));
    auto rules = load_scope_rules(store, scope, &lock->directory);
    if (!rules)
      return std::unexpected(std::move(rules.error()));
    auto found = rules->end();
    for (auto it = rules->begin(); it != rules->end(); ++it)
    {
      if (it->rule_id == rule_id)
      {
        found = it;
        break;
      }
    }
    if (found == rules->end())
      continue;
    auto removed = *found;
    rules->erase(found);
    if (auto written = write_rules_file_atomic(lock->directory, rules_file_json(*rules)); !written)
    {
      return std::unexpected(std::move(written.error()));
    }
    return removed;
  }

  auto error = ava::core::Error(ava::core::ErrorCategory::NotFound, "permission_rule_remove has no matching rule_id");
  error.with_context("rule_id", std::string(rule_id));
  return std::unexpected(std::move(error));
}

}  // namespace ava::permissions

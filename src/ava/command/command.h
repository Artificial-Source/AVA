#pragma once

#include "ava/core/NoLogString.h"
#include "ava/core/result.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include "debug.h"

namespace ava::core {
class AnchorSet;
} // namespace ava::core

namespace ava::command {

enum class CommandIntentLane
{
  Compatibility,
  StructuredArgv,
  RawShell,
};

// Runtime integration is intentionally separate from planning. Legacy leaves
// existing command handling in place, PromptOnly permits callers to inspect a
// sealed plan without treating it as execution authority, and Enabled requires
// callers to gate execution on a fresh sealed plan plus their policy decision.
// No mode turns this planning library into an executor by itself.
enum class CommandRuntimeMode
{
  Legacy,
  PromptOnly,
  Enabled,
};

enum class CommandExecutionDomain
{
  DirectArgv,
  RawShell,
};

enum class PathProvenance
{
  StartupPath,
  UserLocal,
  WorkspaceVenv,
  WorkspaceNodeModules,
};

enum class ExecutableOrigin
{
  System,
  User,
  Workspace,
};

enum class CommandLevel
{
  Standard,
  Sensitive,
  Critical,
};

enum class CommandFamily
{
  Inspection,
  CmakeBuild,
  Ctest,
  Ninja,
  Make,
  PackageManagerScript,
  Pytest,
  WorkspaceScript,
  InstallOrUpdate,
  RemoteGitMutation,
  PublishOrDeploy,
  Network,
  WorkspaceMutation,
  InterpreterInline,
  DestructiveOrPrivileged,
  UnknownWrapper,
  RawShell,
  UnverifiedDelegatedExecutor,
};

enum class CommandRecipe
{
  Pwd,
  Ls,
  GitStatus,
  GitDiff,
  GitLogOne,
  CmakeBuild,
  Ctest,
  Ninja,
  Make,
  PackageManagerRunScript,
  Pytest,
  WorkspaceScript,
};

// Global approval is deliberately not an interactive scope. It remains an
// explicit configuration decision outside command prompts. Once applies to
// one sealed-plan attempt; Session and Workspace may persist only an exact
// canonical recipe under the active policy and environment versions.
enum class InteractiveScope
{
  Once,
  Session,
  Workspace,
};

struct CommandLimits
{
  std::size_t max_request_bytes = 16 * 1024;
  std::size_t max_argument_bytes = 4 * 1024;
  std::size_t max_argv_entries = 128;
  std::size_t max_path_entries = 64;
  std::size_t max_path_bytes = 16 * 1024;
  std::size_t max_shebang_bytes = 512;
  std::size_t max_shebang_depth = 4;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

// These are the paths exposed to the child, not paths used for host-side tool
// discovery. CommandBuildOptions::trusted_home supplies the latter. Every root
// must already exist as a non-symlink directory owned by the current user with
// no group or other permissions; planning never creates these authority roots.
struct CommandEnvironmentOptions
{
  std::string profile_id;
  ava::core::NoLogString user;
  ava::core::NoLogString logname;
  std::filesystem::path home;
  std::filesystem::path xdg_config_home;
  std::filesystem::path xdg_cache_home;
  std::filesystem::path xdg_data_home;
  std::filesystem::path xdg_state_home;
  std::filesystem::path tmpdir;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

struct WorkspaceScriptRecipe
{
  std::filesystem::path script;
  std::vector<ava::core::NoLogString> argv_tail;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

struct CommandRuntimeOptions
{
  CommandRuntimeMode mode = CommandRuntimeMode::Legacy;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

struct CommandBuildOptions
{
  std::filesystem::path workspace;
  // The application-scoped descriptor authority shared by file and command
  // tools. The launch workspace is the first anchor; synthetic command roots
  // must be descendants of one of the other pre-opened writable anchors.
  std::shared_ptr<ava::core::AnchorSet const> anchor_set;
  // The real, trusted host home used only to discover user-local tools.
  // It must be distinct from the synthetic child HOME.
  std::filesystem::path trusted_home;
  // Controls automatic trusted-home .local/bin discovery only; explicitly
  // supplied and workspace-local PATH entries stay enabled.
  bool discover_host_user_tools = true;
  std::optional<ava::core::NoLogString> startup_path;
  // Raw shell plans resolve and bind this exact logical executable; it is never
  // found by basename lookup. The local executor reopens it through the shared
  // anchor authority and executes the verified descriptor.
  std::filesystem::path shell = "/bin/sh";
  // Persistent AVA config, credential, and session roots which must stay
  // disjoint from every synthetic child environment root.
  std::vector<std::filesystem::path> ava_authority_roots;
  CommandEnvironmentOptions environment;
  std::vector<WorkspaceScriptRecipe> workspace_script_recipes;
  CommandLimits limits = {};

  AVA_DEBUG_PRINT_MEMBERS_ON
};

// Captures one non-final lexical path component. Symlink records preserve the
// lstat identity of the link itself; directory records preserve the identity of
// the directory reached through any preceding links.
struct PathAncestorMetadata
{
  std::filesystem::path path;
  std::uintmax_t device = 0;
  std::uintmax_t inode = 0;
  std::uintmax_t mode = 0;
  std::uintmax_t owner = 0;
  std::uintmax_t group = 0;
  std::uintmax_t link_count = 0;
  std::int64_t changed_seconds = 0;
  std::int64_t changed_nanoseconds = 0;
  bool is_symlink = false;
  // Shared sticky namespace boundaries (such as /tmp) are revalidated for
  // safety but not identity-bound because unrelated entries legitimately
  // change their timestamps and link counts.
  bool identity_bound = true;

  // Equality honors identity_bound. A non-identity-bound ancestor (a root-owned
  // sticky namespace such as /tmp) compares only its binding fields -- path,
  // device, inode, mode, owner, group, is_symlink, and identity_bound itself --
  // because its changed_seconds/changed_nanoseconds and link_count legitimately
  // fluctuate as unrelated processes create and remove sibling entries. This
  // keeps value comparisons stable across concurrent /tmp churn and matches the
  // freshness logic, which already skips those volatile fields for
  // non-identity-bound ancestors. Identity-bound ancestors compare every field.
  friend bool operator==(PathAncestorMetadata const& lhs, PathAncestorMetadata const& rhs)
  {
    if (lhs.path != rhs.path || lhs.device != rhs.device || lhs.inode != rhs.inode || lhs.mode != rhs.mode || lhs.owner != rhs.owner ||
        lhs.group != rhs.group || lhs.is_symlink != rhs.is_symlink || lhs.identity_bound != rhs.identity_bound)
    {
      return false;
    }
    if (!lhs.identity_bound)
      return true;
    return lhs.link_count == rhs.link_count && lhs.changed_seconds == rhs.changed_seconds && lhs.changed_nanoseconds == rhs.changed_nanoseconds;
  }

  AVA_DEBUG_PRINT_MEMBERS_ON
};

// Captures the public logical spelling and the descriptor-bound target
// identity. canonical_path is retained for source compatibility but contains
// that logical spelling; only ExecutableMetadata may contain a physical target
// spelling, and only as narrow executable identity metadata.
struct PathMetadata
{
  std::filesystem::path requested_path;
  std::filesystem::path canonical_path;
  std::uintmax_t device = 0;
  std::uintmax_t inode = 0;
  std::uintmax_t mode = 0;
  std::uintmax_t size = 0;
  std::uintmax_t owner = 0;
  std::uintmax_t group = 0;
  std::uintmax_t link_count = 0;
  std::int64_t changed_seconds = 0;
  std::int64_t changed_nanoseconds = 0;
  bool requested_path_is_symlink = false;
  std::uintmax_t requested_device = 0;
  std::uintmax_t requested_inode = 0;
  std::uintmax_t requested_mode = 0;
  std::uintmax_t requested_owner = 0;
  std::uintmax_t requested_group = 0;
  std::uintmax_t requested_link_count = 0;
  std::int64_t requested_changed_seconds = 0;
  std::int64_t requested_changed_nanoseconds = 0;
  std::vector<PathAncestorMetadata> ancestor_metadata;

  friend bool operator==(PathMetadata const&, PathMetadata const&) = default;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

// These descriptor-sealed logical roots are the only HOME/XDG/TMP locations
// exposed to a child. They are retained by plans and prepared environments
// rather than reconstructed from mutable input options.
struct SyntheticEnvironmentRoots
{
  PathMetadata home;
  PathMetadata xdg_config_home;
  PathMetadata xdg_cache_home;
  PathMetadata xdg_data_home;
  PathMetadata xdg_state_home;
  PathMetadata tmpdir;

  friend bool operator==(SyntheticEnvironmentRoots const&, SyntheticEnvironmentRoots const&) = default;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

struct CommandPathEntry
{
  std::filesystem::path directory;
  PathProvenance provenance = PathProvenance::StartupPath;
  PathMetadata metadata;

  friend bool operator==(CommandPathEntry const&, CommandPathEntry const&) = default;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

struct ExecutableMetadata
{
  std::filesystem::path requested_path;
  std::filesystem::path canonical_path;
  std::uintmax_t device = 0;
  std::uintmax_t inode = 0;
  std::uintmax_t mode = 0;
  std::uintmax_t size = 0;
  std::uintmax_t owner = 0;
  std::uintmax_t group = 0;
  std::uintmax_t link_count = 0;
  std::int64_t changed_seconds = 0;
  std::int64_t changed_nanoseconds = 0;
  bool requested_path_is_symlink = false;
  std::uintmax_t requested_device = 0;
  std::uintmax_t requested_inode = 0;
  std::uintmax_t requested_mode = 0;
  std::uintmax_t requested_owner = 0;
  std::uintmax_t requested_group = 0;
  std::uintmax_t requested_link_count = 0;
  std::int64_t requested_changed_seconds = 0;
  std::int64_t requested_changed_nanoseconds = 0;
  std::vector<PathAncestorMetadata> ancestor_metadata;

  friend bool operator==(ExecutableMetadata const&, ExecutableMetadata const&) = default;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

struct ShebangInterpreter
{
  ExecutableMetadata interpreter;
  ava::core::NoLogString argument;
  // True for the executable selected by a preceding simple
  // /usr/bin/env <name> shebang. Local execution bypasses env's mutable PATH
  // lookup and invokes this already-sealed descriptor directly.
  bool resolved_via_env = false;

  friend bool operator==(ShebangInterpreter const&, ShebangInterpreter const&) = default;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

struct ResolvedExecutable
{
  ExecutableMetadata executable;
  ExecutableOrigin origin = ExecutableOrigin::System;
  std::vector<ShebangInterpreter> shebang_interpreters;
  // False when a shebang references /usr/bin/env with a name that could not be
  // resolved through the sealed PATH, or uses an unsupported multi-argument
  // form. The plan still succeeds so policy can issue a one-shot prompt, but
  // the interpreter chain is not fully bound.
  bool shebang_fully_resolved = true;

  friend bool operator==(ResolvedExecutable const&, ResolvedExecutable const&) = default;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

struct CommandCapabilities
{
  bool executes_mutable_project_code = false;
  bool requires_containment = false;
  bool network_enabled = false;
  bool mutates_workspace = false;
  bool destructive_or_privileged = false;
  bool interpreter_inline = false;
  bool unknown_wrapper = false;
  bool raw_shell = false;

  friend bool operator==(CommandCapabilities const&, CommandCapabilities const&) = default;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

struct RecipeDescriptor
{
  CommandRecipe recipe = CommandRecipe::Pwd;
  std::vector<ava::core::NoLogString> canonical_argv;
  std::vector<PathMetadata> path_arguments;

  friend bool operator==(RecipeDescriptor const&, RecipeDescriptor const&) = default;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

struct CommandClassification
{
  CommandLevel level = CommandLevel::Critical;
  CommandFamily family = CommandFamily::UnknownWrapper;
  std::optional<RecipeDescriptor> recipe;
  CommandCapabilities capabilities;
  InteractiveScope max_interactive_scope = InteractiveScope::Once;
  bool ask_candidate = true;

  friend bool operator==(CommandClassification const&, CommandClassification const&) = default;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

struct EnvironmentVariable
{
  std::string key;
  ava::core::NoLogString value;

  friend bool operator==(EnvironmentVariable const&, EnvironmentVariable const&) = default;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

class CommandEnvironment;
class CommandIntent;
class CommandPlan;
class CommandPreparation;

namespace detail {
class EnvironmentFactory;
ava::core::VoidResult validate_environment_matches_plan(CommandEnvironment const& environment, CommandPlan const& plan);
}  // namespace detail

class CommandEnvironment final
{
 public:
  [[nodiscard]] std::string const& profile_id() const noexcept;
  [[nodiscard]] std::string const& digest() const noexcept;
  [[nodiscard]] std::vector<EnvironmentVariable> const& entries() const noexcept;
  [[nodiscard]] SyntheticEnvironmentRoots const& synthetic_roots() const noexcept;

  friend bool operator==(CommandEnvironment const&, CommandEnvironment const&) = default;

  AVA_DEBUG_PRINT_MEMBERS_ON

 private:
  // Including an internal header is not an authority boundary because src/ is
  // an include root. Only sealing functions can mint this passkey, so an
  // external caller cannot construct an environment through EnvironmentFactory.
  class FactoryPasskey final
  {
    friend class CommandEnvironment;

    FactoryPasskey() = default;

    AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
  };

  [[nodiscard]] static FactoryPasskey make_factory_passkey() noexcept { return {}; }

  friend ava::core::Result<CommandPlan> seal_command_plan(CommandIntent const&, CommandBuildOptions const&);
  friend ava::core::Result<CommandPreparation> prepare_command(CommandIntent const&, CommandBuildOptions const&);
  friend class detail::EnvironmentFactory;
  friend ava::core::VoidResult detail::validate_environment_matches_plan(CommandEnvironment const&, CommandPlan const&);

  explicit CommandEnvironment(FactoryPasskey, SyntheticEnvironmentRoots roots);

  std::string profile_id_;
  std::string digest_;
  std::vector<EnvironmentVariable> entries_;
  SyntheticEnvironmentRoots roots_;
};

class CommandIntent final
{
 public:
  [[nodiscard]] static ava::core::Result<CommandIntent> compatibility(std::string command, CommandLimits limits = {});
  [[nodiscard]] static ava::core::Result<CommandIntent> structured(std::vector<std::string> argv, std::optional<std::filesystem::path> cwd = std::nullopt,
                                                                   CommandLimits limits = {});
  [[nodiscard]] static ava::core::Result<CommandIntent> raw_shell(std::string shell_text, CommandLimits limits = {});

  [[nodiscard]] CommandIntentLane lane() const noexcept;
  [[nodiscard]] bool has_argv() const noexcept;
  [[nodiscard]] std::vector<std::string> const& argv() const noexcept;
  [[nodiscard]] std::optional<std::filesystem::path> const& requested_cwd() const noexcept;
  [[nodiscard]] std::string_view source_text() const noexcept;

  AVA_DEBUG_PRINT_MEMBERS_ON

 private:
  CommandIntent(CommandIntentLane lane, std::vector<std::string> argv, std::optional<std::filesystem::path> cwd, std::string source_text);

  CommandIntentLane lane_ = CommandIntentLane::Compatibility;
  std::vector<ava::core::NoLogString> argv_;
  std::optional<std::filesystem::path> cwd_;
  ava::core::NoLogString source_text_;
};

class CommandPlan final
{
 public:
  [[nodiscard]] CommandIntentLane intent_lane() const noexcept;
  [[nodiscard]] CommandExecutionDomain execution_domain() const noexcept;
  [[nodiscard]] std::filesystem::path const& workspace() const noexcept;
  [[nodiscard]] std::filesystem::path const& cwd() const noexcept;
  [[nodiscard]] PathMetadata const& cwd_metadata() const noexcept;
  [[nodiscard]] std::shared_ptr<ava::core::AnchorSet const> const& anchor_set() const noexcept;
  // Sealed host discovery boundary used only to scope user-tool freshness.
  [[nodiscard]] PathMetadata const& trusted_home_metadata() const noexcept;
  [[nodiscard]] std::vector<std::string> const& argv() const noexcept;
  [[nodiscard]] std::string const& raw_shell_text() const noexcept;
  [[nodiscard]] std::vector<CommandPathEntry> const& path_entries() const noexcept;
  [[nodiscard]] std::vector<std::filesystem::path> const& ava_authority_roots() const noexcept;
  [[nodiscard]] std::optional<ResolvedExecutable> const& resolved_executable() const noexcept;
  [[nodiscard]] CommandClassification const& classification() const noexcept;
  [[nodiscard]] std::string const& environment_profile_id() const noexcept;
  [[nodiscard]] std::string const& environment_digest() const noexcept;
  // An instantaneous integrity binding for this sealed plan, never a durable
  // recipe or grant identity. Durable grants must exact-match sealed logical
  // recipe fields plus their policy and environment versions, not this fingerprint.
  [[nodiscard]] std::string const& fingerprint() const noexcept;
  // Local user-facing sensitive text. It may contain workspace paths, argv,
  // and raw shell text and must never enter logs, telemetry, or RPC diagnostics.
  [[nodiscard]] std::string display_json() const;
  // Diagnostics-safe structural summary: deliberately excludes argv, shell
  // text, paths, environment values, and any other request payload.
  [[nodiscard]] std::string redacted_summary() const;

  friend bool operator==(CommandPlan const&, CommandPlan const&) = default;

  AVA_DEBUG_PRINT_MEMBERS_ON

 private:
  friend ava::core::Result<CommandPlan> seal_command_plan(CommandIntent const&, CommandBuildOptions const&);
  friend ava::core::Result<CommandPreparation> prepare_command(CommandIntent const&, CommandBuildOptions const&);
  friend ava::core::Result<bool> plan_is_fresh(CommandPlan const&);
  friend ava::core::VoidResult detail::validate_environment_matches_plan(CommandEnvironment const&, CommandPlan const&);

  CommandPlan() = default;

  CommandIntentLane intent_lane_ = CommandIntentLane::Compatibility;
  CommandExecutionDomain execution_domain_ = CommandExecutionDomain::DirectArgv;
  std::filesystem::path workspace_;
  std::filesystem::path cwd_;
  PathMetadata workspace_metadata_;
  PathMetadata cwd_metadata_;
  std::shared_ptr<ava::core::AnchorSet const> anchor_set_;
  PathMetadata trusted_home_metadata_;
  std::vector<std::filesystem::path> ava_authority_roots_;
  std::vector<PathMetadata> ava_authority_root_metadata_;
  SyntheticEnvironmentRoots synthetic_environment_roots_;
  std::vector<ava::core::NoLogString> argv_;
  ava::core::NoLogString raw_shell_text_;
  std::vector<CommandPathEntry> path_entries_;
  std::optional<ResolvedExecutable> resolved_executable_;
  CommandClassification classification_;
  std::string environment_profile_id_;
  std::string environment_digest_;
  std::string fingerprint_;
};

class CommandPreparation final
{
 public:
  [[nodiscard]] CommandPlan const& plan() const noexcept;
  [[nodiscard]] CommandEnvironment const& environment() const noexcept;

  friend bool operator==(CommandPreparation const&, CommandPreparation const&) = default;

  AVA_DEBUG_PRINT_MEMBERS_ON

 private:
  friend ava::core::Result<CommandPreparation> prepare_command(CommandIntent const&, CommandBuildOptions const&);

  CommandPreparation(CommandPlan plan, CommandEnvironment environment);

  CommandPlan plan_;
  CommandEnvironment environment_;
};

[[nodiscard]] ava::core::Result<CommandPlan> seal_command_plan(CommandIntent const& intent, CommandBuildOptions const& options);
[[nodiscard]] ava::core::Result<CommandPreparation> prepare_command(CommandIntent const& intent, CommandBuildOptions const& options);
[[nodiscard]] ava::core::Result<bool> plan_is_fresh(CommandPlan const& plan);

[[nodiscard]] bool command_mode_is_enabled(CommandRuntimeOptions const& options) noexcept;
[[nodiscard]] bool command_mode_is_prompt_only(CommandRuntimeOptions const& options) noexcept;

[[nodiscard]] std::string_view to_string(CommandIntentLane value) noexcept;
[[nodiscard]] std::string_view to_string(CommandRuntimeMode value) noexcept;
[[nodiscard]] std::string_view to_string(CommandExecutionDomain value) noexcept;
[[nodiscard]] std::string_view to_string(PathProvenance value) noexcept;
[[nodiscard]] std::string_view to_string(ExecutableOrigin value) noexcept;
[[nodiscard]] std::string_view to_string(CommandLevel value) noexcept;
[[nodiscard]] std::string_view to_string(CommandFamily value) noexcept;
[[nodiscard]] std::string_view to_string(CommandRecipe value) noexcept;
[[nodiscard]] std::string_view to_string(InteractiveScope value) noexcept;

}  // namespace ava::command

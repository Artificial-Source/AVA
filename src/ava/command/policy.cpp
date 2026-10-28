#include "sys.h"
#include "ava/command/policy.h"
#include "ava/core/text_case.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <span>
#include <utility>

namespace ava::command::detail {
namespace {

bool has_argument(std::vector<std::string> const& argv, std::initializer_list<std::string_view> candidates)
{
  return std::ranges::any_of(argv, [&candidates](std::string const& argument) {
    auto const lower = ava::core::locale_lower_copy(argument);
    return std::ranges::find(candidates, std::string_view(lower)) != candidates.end();
  });
}

bool has_argument_prefix(std::vector<std::string> const& argv, std::string_view prefix)
{
  return std::ranges::any_of(argv, [prefix](std::string const& argument) { return ava::core::locale_lower_copy(argument).starts_with(prefix); });
}

bool is_simple_script_name(std::string_view value)
{
  if (value.empty() || value.size() > 128)
    return false;
  return std::ranges::all_of(value, [](unsigned char ch) { return std::isalnum(ch) != 0 || ch == '-' || ch == '_' || ch == '.' || ch == ':'; });
}

std::vector<std::string> canonical_recipe_argv(std::vector<std::string> const& argv, ResolvedExecutable const& executable,
                                               std::vector<std::pair<std::size_t, PathMetadata>> const& path_arguments)
{
  auto result = argv;
  result.front() = executable.executable.canonical_path.string();
  for (auto const& [index, path] : path_arguments) result[index] = path.canonical_path.string();
  return result;
}

CommandClassification critical_classification(CommandFamily family, CommandCapabilities capabilities)
{
  return CommandClassification{.level = CommandLevel::Critical,
                               .family = family,
                               .recipe = std::nullopt,
                               .capabilities = std::move(capabilities),
                               .max_interactive_scope = InteractiveScope::Once,
                               .ask_candidate = true};
}

CommandClassification sensitive_classification(CommandFamily family, CommandCapabilities capabilities)
{
  return CommandClassification{.level = CommandLevel::Sensitive,
                               .family = family,
                               .recipe = std::nullopt,
                               .capabilities = std::move(capabilities),
                               .max_interactive_scope = InteractiveScope::Workspace,
                               .ask_candidate = true};
}

CommandClassification standard_classification(CommandFamily family, CommandRecipe recipe, std::vector<std::string> canonical_argv,
                                              std::vector<PathMetadata> path_arguments, bool executes_mutable_project_code,
                                              bool requires_path_containment = false)
{
  return CommandClassification{
      .level = CommandLevel::Standard,
      .family = family,
      .recipe = RecipeDescriptor{.recipe = recipe, .canonical_argv = std::move(canonical_argv), .path_arguments = std::move(path_arguments)},
      .capabilities = CommandCapabilities{.executes_mutable_project_code = executes_mutable_project_code,
                                          .requires_containment = executes_mutable_project_code || requires_path_containment},
      .max_interactive_scope = InteractiveScope::Workspace,
      .ask_candidate = true};
}

CommandClassification apply_executable_origin_invariants(CommandClassification classification, ResolvedExecutable const& executable)
{
  // Classification is about command family; executable provenance is an
  // independent execution invariant. Project- and user-owned executables can
  // replace a familiar inspection, sensitive, or critical binary with
  // arbitrary code and therefore require verified containment.
  if (executable.origin == ExecutableOrigin::Workspace || executable.origin == ExecutableOrigin::User)
  {
    classification.capabilities.executes_mutable_project_code = true;
    classification.capabilities.requires_containment = true;
  }
  return classification;
}

bool is_inline_interpreter(std::string_view executable_name, std::vector<std::string> const& argv)
{
  static constexpr std::array<std::string_view, 12> kInterpreters{"bash", "sh",   "zsh",  "fish", "python", "python3",
                                                                  "node", "perl", "ruby", "php",  "lua",    "pwsh"};
  if (std::ranges::find(kInterpreters, executable_name) == kInterpreters.end())
    return false;
  return has_argument(argv, {"-c", "-e", "--command", "--eval"});
}

std::optional<CommandClassification> classify_destructive_or_privileged(std::string_view executable_name, std::vector<std::string> const& argv)
{
  if (executable_name == "git" && argv.size() >= 2 && ava::core::locale_lower_copy(argv[1]) == "push" &&
      (has_argument(argv, {"--force", "-f", "--force-with-lease", "--force-if-includes"}) || has_argument_prefix(argv, "--force-with-lease=")))
  {
    return critical_classification(CommandFamily::DestructiveOrPrivileged, CommandCapabilities{.network_enabled = true, .destructive_or_privileged = true});
  }
  if (executable_name == "sudo" || executable_name == "doas" || executable_name == "pkexec" || executable_name == "rm" || executable_name == "dd" ||
      executable_name == "mkfs" || executable_name.starts_with("mkfs."))
  {
    return critical_classification(CommandFamily::DestructiveOrPrivileged, CommandCapabilities{.destructive_or_privileged = true});
  }
  if (has_argument(argv, {"--privileged", "--force-dangerous", "--hard", "--hard-reset"}))
  {
    return critical_classification(CommandFamily::DestructiveOrPrivileged, CommandCapabilities{.destructive_or_privileged = true});
  }
  if (executable_name == "git" && argv.size() >= 2)
  {
    auto const subcommand = ava::core::locale_lower_copy(argv[1]);
    if (subcommand == "clean" || (subcommand == "reset" && has_argument(argv, {"--hard"})))
      return critical_classification(CommandFamily::DestructiveOrPrivileged, CommandCapabilities{.destructive_or_privileged = true});
  }
  return std::nullopt;
}

std::optional<CommandClassification> classify_sensitive(std::string_view executable_name, std::vector<std::string> const& argv)
{
  auto const second = argv.size() >= 2 ? ava::core::locale_lower_copy(argv[1]) : std::string{};
  if (executable_name == "git")
  {
    if (second == "pull" || second == "fetch" || second == "clone")
    {
      // Fetch mutates the workspace's .git object/ref database even when it
      // does not update the worktree. Pull and clone additionally update files.
      return sensitive_classification(CommandFamily::RemoteGitMutation, CommandCapabilities{.network_enabled = true, .mutates_workspace = true});
    }
    if (second == "push")
      return sensitive_classification(CommandFamily::RemoteGitMutation, CommandCapabilities{.network_enabled = true});
    if (second == "submodule")
    {
      auto const action = argv.size() >= 3 ? ava::core::locale_lower_copy(argv[2]) : std::string{};
      bool const network = action.empty() || action == "update" || action == "add" || action == "sync" || action == "foreach";
      bool const mutates = action.empty() || action == "update" || action == "init" || action == "add" || action == "deinit" || action == "set-url" ||
                           action == "sync" || action == "absorbgitdirs" || action == "foreach";
      return sensitive_classification(CommandFamily::RemoteGitMutation, CommandCapabilities{.network_enabled = network, .mutates_workspace = mutates});
    }
    if (second == "remote")
    {
      auto const action = argv.size() >= 3 ? ava::core::locale_lower_copy(argv[2]) : std::string{};
      CommandCapabilities capabilities;
      if (action == "add" || action == "set-url" || action == "remove" || action == "rm" || action == "rename")
        capabilities.mutates_workspace = true;
      else if (action == "update")
      {
        capabilities.network_enabled = true;
        capabilities.mutates_workspace = true;
      }
      else if (action == "prune")
      {
        // `git remote prune` removes stale local remote-tracking refs but does
        // not itself fetch from the network.
        capabilities.mutates_workspace = true;
      }
      return sensitive_classification(CommandFamily::RemoteGitMutation, capabilities);
    }
    if (second == "apply" || second == "checkout" || second == "restore" || second == "commit" || second == "merge" || second == "rebase" ||
        second == "cherry-pick")
    {
      return sensitive_classification(CommandFamily::WorkspaceMutation, CommandCapabilities{.mutates_workspace = true});
    }
  }
  if ((executable_name == "npm" || executable_name == "pnpm" || executable_name == "yarn" || executable_name == "bun" || executable_name == "pip" ||
       executable_name == "pip3") &&
      (second == "install" || second == "update" || second == "upgrade" || second == "add" || second == "remove" || second == "uninstall"))
  {
    return sensitive_classification(CommandFamily::InstallOrUpdate, CommandCapabilities{.network_enabled = true, .mutates_workspace = true});
  }
  if (executable_name == "curl" || executable_name == "wget" || executable_name == "ssh" || executable_name == "scp" || executable_name == "rsync")
  {
    return sensitive_classification(CommandFamily::Network,
                                    CommandCapabilities{.network_enabled = true, .mutates_workspace = executable_name == "rsync" || executable_name == "scp"});
  }
  if (executable_name == "cmake" && has_argument(argv, {"--install", "--install-prefix"}))
    return sensitive_classification(CommandFamily::InstallOrUpdate, CommandCapabilities{.mutates_workspace = true});
  if (has_argument(argv, {"publish", "deploy", "release"}))
    return sensitive_classification(CommandFamily::PublishOrDeploy, CommandCapabilities{.network_enabled = true, .mutates_workspace = true});
  if (executable_name == "touch" || executable_name == "mkdir" || executable_name == "cp" || executable_name == "mv" || executable_name == "chmod" ||
      executable_name == "patch" || executable_name == "tee" || (executable_name == "sed" && has_argument(argv, {"-i", "--in-place"})))
  {
    return sensitive_classification(CommandFamily::WorkspaceMutation, CommandCapabilities{.mutates_workspace = true});
  }
  return std::nullopt;
}

std::optional<CommandClassification> classify_standard(std::string_view executable_name, std::vector<std::string> const& argv,
                                                       ResolvedExecutable const& executable, std::filesystem::path const& cwd,
                                                       std::filesystem::path const& workspace, std::vector<WorkspaceScriptRecipe> const& workspace_recipes,
                                                       std::shared_ptr<ava::core::AnchorSet const> const& anchor_set)
{
  auto const recipe_path = [&](std::size_t index) -> std::optional<PathMetadata> {
    if (index >= argv.size())
      return std::nullopt;
    return seal_recipe_path_argument(argv[index], cwd, workspace, anchor_set);
  };
  auto const canonical = [&](std::vector<std::pair<std::size_t, PathMetadata>> const& paths = {}) { return canonical_recipe_argv(argv, executable, paths); };
  // A workspace-local executable named ls/pwd/git is project code, not a
  // trusted inspection binary. User-owned aliases may retain a recognized
  // family but acquire the mutable-code containment invariant below.
  // Project-code recipes remain Standard only because they explicitly require
  // containment.
  bool const system_inspection_executable = executable.origin == ExecutableOrigin::System;
  if (system_inspection_executable && executable_name == "pwd" && argv.size() == 1)
    return standard_classification(CommandFamily::Inspection, CommandRecipe::Pwd, canonical(), {}, false);
  if (system_inspection_executable && executable_name == "ls" && argv.size() == 1)
    return standard_classification(CommandFamily::Inspection, CommandRecipe::Ls, canonical(), {}, false);
  if (system_inspection_executable && executable_name == "ls" && argv.size() == 2)
  {
    auto path = recipe_path(1);
    if (path)
      return standard_classification(CommandFamily::Inspection, CommandRecipe::Ls, canonical({{1, *path}}), {*path}, false, true);
  }
  // Git inspection can invoke repository-controlled fsmonitor, textconv,
  // external-diff, and related helpers. Keep the recipe Standard, but never
  // auto-run it without the mutable-project-code containment boundary.
  if (system_inspection_executable && executable_name == "git" && argv.size() == 2 && ava::core::locale_lower_copy(argv[1]) == "status")
    return standard_classification(CommandFamily::Inspection, CommandRecipe::GitStatus, canonical(), {}, true);
  if (system_inspection_executable && executable_name == "git" && argv.size() == 2 && ava::core::locale_lower_copy(argv[1]) == "diff")
    return standard_classification(CommandFamily::Inspection, CommandRecipe::GitDiff, canonical(), {}, true);
  if (system_inspection_executable && executable_name == "git" && argv.size() == 3 && ava::core::locale_lower_copy(argv[1]) == "log" && argv[2] == "-1")
    return standard_classification(CommandFamily::Inspection, CommandRecipe::GitLogOne, canonical(), {}, true);

  if (executable_name == "cmake" && argv.size() == 3 && argv[1] == "--build")
  {
    auto path = recipe_path(2);
    if (path)
      return standard_classification(CommandFamily::CmakeBuild, CommandRecipe::CmakeBuild, canonical({{2, *path}}), {*path}, true);
  }
  if (executable_name == "ctest" && argv.size() == 1)
    return standard_classification(CommandFamily::Ctest, CommandRecipe::Ctest, canonical(), {}, true);
  if (executable_name == "ctest" && argv.size() == 3 && argv[1] == "--test-dir")
  {
    auto path = recipe_path(2);
    if (path)
      return standard_classification(CommandFamily::Ctest, CommandRecipe::Ctest, canonical({{2, *path}}), {*path}, true);
  }
  if (executable_name == "ninja" && argv.size() == 1)
    return standard_classification(CommandFamily::Ninja, CommandRecipe::Ninja, canonical(), {}, true);
  if (executable_name == "ninja" && argv.size() == 3 && argv[1] == "-C")
  {
    auto path = recipe_path(2);
    if (path)
      return standard_classification(CommandFamily::Ninja, CommandRecipe::Ninja, canonical({{2, *path}}), {*path}, true);
  }
  if (executable_name == "make" && argv.size() == 1)
    return standard_classification(CommandFamily::Make, CommandRecipe::Make, canonical(), {}, true);
  if (executable_name == "make" && argv.size() == 3 && argv[1] == "-C")
  {
    auto path = recipe_path(2);
    if (path)
      return standard_classification(CommandFamily::Make, CommandRecipe::Make, canonical({{2, *path}}), {*path}, true);
  }
  if ((executable_name == "npm" || executable_name == "pnpm" || executable_name == "yarn" || executable_name == "bun") && argv.size() == 3 &&
      ava::core::locale_lower_copy(argv[1]) == "run" && is_simple_script_name(argv[2]))
  {
    return standard_classification(CommandFamily::PackageManagerScript, CommandRecipe::PackageManagerRunScript, canonical(), {}, true);
  }
  if (executable_name == "pytest" && argv.size() == 1)
    return standard_classification(CommandFamily::Pytest, CommandRecipe::Pytest, canonical(), {}, true);
  if (executable_name == "pytest" && argv.size() == 2)
  {
    auto path = recipe_path(1);
    if (path)
      return standard_classification(CommandFamily::Pytest, CommandRecipe::Pytest, canonical({{1, *path}}), {*path}, true);
  }

  for (auto const& configured_recipe : workspace_recipes)
  {
    auto configured_path = configured_recipe.script.is_absolute() ? configured_recipe.script : workspace / configured_recipe.script;
    auto script = seal_recipe_path_argument(configured_path.string(), workspace, workspace, anchor_set);
    if (!script || script->device != executable.executable.device || script->inode != executable.executable.inode ||
        argv.size() != configured_recipe.argv_tail.size() + 1)
      continue;
    if (!std::ranges::equal(std::span(argv).subspan(1), configured_recipe.argv_tail))
      continue;
    return standard_classification(CommandFamily::WorkspaceScript, CommandRecipe::WorkspaceScript, canonical(), {}, true);
  }
  return std::nullopt;
}

}  // namespace

CommandClassification classify_raw_shell(ResolvedExecutable const& executable)
{
  return apply_executable_origin_invariants(CommandClassification{.level = CommandLevel::Critical,
                                                                  .family = CommandFamily::RawShell,
                                                                  .recipe = std::nullopt,
                                                                  .capabilities = CommandCapabilities{.raw_shell = true},
                                                                  .max_interactive_scope = InteractiveScope::Once,
                                                                  .ask_candidate = true},
                                            executable);
}

CommandClassification classify_command(std::vector<std::string> const& argv, ResolvedExecutable const& executable, std::filesystem::path const& cwd,
                                       std::filesystem::path const& workspace, std::vector<WorkspaceScriptRecipe> const& workspace_recipes,
                                       std::shared_ptr<ava::core::AnchorSet const> const& anchor_set)
{
  // Classify by the invoked basename when the sealed executable is a symlink.
  // Multi-call tools commonly expose trusted aliases this way (for
  // example npm -> npm-cli.js). The canonical target and
  // origin still bind permission identity and execution, so bare and absolute
  // spellings of the same alias cannot select different authority.
  auto const& classification_path =
      executable.executable.requested_path_is_symlink ? executable.executable.requested_path : executable.executable.canonical_path;
  auto const executable_name = ava::core::locale_lower_copy(classification_path.filename().string());
  CommandClassification classification;
  if (!executable.shebang_fully_resolved)
    classification = critical_classification(CommandFamily::UnknownWrapper, CommandCapabilities{.unknown_wrapper = true});
  else if (auto destructive = classify_destructive_or_privileged(executable_name, argv))
    classification = std::move(*destructive);
  else if (is_inline_interpreter(executable_name, argv))
    classification = critical_classification(CommandFamily::InterpreterInline, CommandCapabilities{.interpreter_inline = true});
  else if (auto sensitive = classify_sensitive(executable_name, argv))
    classification = std::move(*sensitive);
  else if (auto standard = classify_standard(executable_name, argv, executable, cwd, workspace, workspace_recipes, anchor_set))
    classification = std::move(*standard);
  else
    classification = critical_classification(CommandFamily::UnknownWrapper, CommandCapabilities{.unknown_wrapper = true});
  return apply_executable_origin_invariants(std::move(classification), executable);
}

}  // namespace ava::command::detail

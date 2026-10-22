#include "sys.h"
#include "tests/support/fake_transport.h"
#include "tests/support/process_test_support.h"
#include "tests/support/syscall_errno_injection.h"
#include "tests/support/test_env.h"
#include "tests/support/test_harness.h"
#include "ava/http/transport.h"
#include "ava/app/commands.h"
#include "ava/app/connect_oauth_callback.h"
#include "ava/app/headless_policy.h"
#include "ava/app/print_mode.h"
#include "ava/app/rpc_mode.h"
#include "ava/app/runtime.h"
#include "ava/agent/agent_loop.h"
#include "ava/agent/mode.h"
#include "ava/agent/tool_dispatcher.h"
#include "ava/agent/usage_accounting.h"
#include "ava/tools/bash_tool.h"
#include "ava/tools/file_tools.h"
#include "ava/tools/search_tools.h"
#include "ava/tui/composer.h"
#include "ava/tui/terminal.h"
#include "ava/config/auth.h"
#include "ava/config/auth_record.h"
#include "ava/config/model_config.h"
#include "ava/config/model_profiles.h"
#include "ava/config/openai_oauth.h"
#include "ava/config/prompt_config.h"
#include "ava/config/provider_profiles.h"
#include "ava/config/reasoning_profiles.h"
#include "ava/config/xdg_paths.h"
#include "ava/session/compaction.h"
#include "ava/session/export.h"
#include "ava/session/session_store.h"
#include "ava/permissions/permission.h"
#include "ava/provider/openai_provider.h"
#include "ava/provider/registry.h"
#include "ava/context/context_loader.h"
#include "ava/context/markdown_resource.h"
#include "ava/context/skill_loader.h"
#include "ava/core/UniqueFd.h"
#include "ava/core/ids.h"
#include "ava/core/json.h"
#include "ava/core/path.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <climits>
#include <cstdlib>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <iostream>
#include <iterator>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {

bool contains_string(std::vector<std::string> const& values, std::string_view value)
{
  return std::ranges::find(values, value) != values.end();
}

void test_xdg_paths()
{
  auto const root = create_empty_root("xdg");

  std::filesystem::create_directories(root);

  ScopedEnvVar home("HOME", (root / "home").string());
  ScopedEnvVar config_unset("XDG_CONFIG_HOME", std::nullopt);
  ScopedEnvVar state_unset("XDG_STATE_HOME", std::nullopt);
  ScopedEnvVar data_unset("XDG_DATA_HOME", std::nullopt);
  auto fallback = ava::config::xdg_paths();
  expect(fallback.ava_config_dir == root / "home" / ".config" / "ava", "XDG config falls back to ~/.config/ava");
  expect(fallback.ava_state_dir == root / "home" / ".local" / "state" / "ava", "XDG state falls back to ~/.local/state/ava");
  expect(fallback.auth_file == fallback.ava_config_dir / "auth.json", "auth file is in XDG config dir");
  expect(fallback.compaction_file == fallback.ava_config_dir / "compaction.json", "compaction file is in XDG config dir");
  expect(fallback.global_agents_file == fallback.ava_config_dir / "AGENTS.md", "global AGENTS.md file is in XDG config dir");
  expect(fallback.models_file == fallback.ava_config_dir / "models.json", "models file is in XDG config dir");
  expect(fallback.providers_file == fallback.ava_config_dir / "providers.json", "providers file is in XDG config dir");
  expect(fallback.sessions_dir == fallback.ava_state_dir / "sessions", "sessions are in XDG state dir");

  ScopedEnvVar config_override("XDG_CONFIG_HOME", (root / "config").string());
  ScopedEnvVar state_override("XDG_STATE_HOME", (root / "state").string());
  ScopedEnvVar data_override("XDG_DATA_HOME", (root / "data").string());
  auto overridden = ava::config::xdg_paths();
  expect(overridden.ava_config_dir == root / "config" / "ava", "XDG config override is honored");
  expect(overridden.ava_state_dir == root / "state" / "ava", "XDG state override is honored");
  auto const compatible_auth_path = ava::config::legacy_compatible_auth_path();
  expect(compatible_auth_path.filename() == "auth.json", "compatible auth path points at auth file");
  expect(compatible_auth_path.parent_path().parent_path() == root / "data", "compatible auth path follows XDG data home");

  ScopedEnvVar relative_config("XDG_CONFIG_HOME", "relative-config");
  auto relative_ignored = ava::config::xdg_paths();
  expect(relative_ignored.config_home == root / "home" / ".config", "relative XDG config path is ignored safely");

  ScopedEnvVar no_home_env("HOME", std::nullopt);
  ScopedEnvVar no_config_env("XDG_CONFIG_HOME", std::nullopt);
  ScopedEnvVar no_state_env("XDG_STATE_HOME", std::nullopt);
  ScopedEnvVar no_data_env("XDG_DATA_HOME", std::nullopt);
  auto no_home = ava::config::xdg_paths();
  expect(no_home.config_home.is_absolute(), "XDG config fallback remains absolute when HOME is unset");
  expect(no_home.config_home != std::filesystem::current_path() / ".config", "XDG fallback does not use current directory");

  ScopedEnvVar empty_home_env("HOME", "");
  auto empty_home = ava::config::xdg_paths();
  expect(empty_home.state_home.is_absolute(), "XDG state fallback remains absolute when HOME is empty");
}

void test_context_loader()
{
  auto const root = create_empty_root("context");

  auto const workspace = root / "workspace";
  auto const nested = workspace / "src" / "feature";
  auto const config = root / "config" / "ava";
  auto const outside = root / "outside";
  std::error_code remove_error;
  std::filesystem::remove_all(root, remove_error);
  std::filesystem::create_directories(nested);
  std::filesystem::create_directories(config);
  std::filesystem::create_directories(outside);

  auto const root_agents = workspace / "AGENTS.md";
  auto const src_agents = workspace / "src" / "AGENTS.md";
  auto const nested_agents = nested / "AGENTS.md";
  auto const global_agents = config / "AGENTS.md";
  {
    std::ofstream file(root_agents, std::ios::binary | std::ios::trunc);
    file << "root instructions\n";
  }
  {
    std::ofstream file(src_agents, std::ios::binary | std::ios::trunc);
    file << "src instructions\n";
  }
  {
    std::ofstream file(nested_agents, std::ios::binary | std::ios::trunc);
    file << "nested instructions";
  }
  {
    std::ofstream file(global_agents, std::ios::binary | std::ios::trunc);
    file << "global instructions\n";
  }

  auto loaded = ava::context::load_context_files(ava::context::ContextLoadOptions{
      .workspace_root = workspace,
      .current_dir = nested,
      .global_agents_file = {},
      .max_file_bytes = 1024,
  });
  expect(loaded.has_value(), "context loader loads workspace AGENTS.md files");
  if (loaded)
  {
    expect(loaded->size() == 3, "context loader finds root-to-current workspace contexts");
    if (loaded->size() == 3)
    {
      expect((*loaded)[0].path == ava::core::normalized_absolute_path(root_agents), "root AGENTS.md loads first");
      expect((*loaded)[1].path == ava::core::normalized_absolute_path(src_agents), "intermediate AGENTS.md loads second");
      expect((*loaded)[2].path == ava::core::normalized_absolute_path(nested_agents), "nested AGENTS.md loads third");
      expect((*loaded)[0].source_type == ava::context::ContextSourceType::Workspace, "workspace context records workspace source type");
    }
  }

  auto outside_loaded = ava::context::load_context_files(ava::context::ContextLoadOptions{
      .workspace_root = workspace,
      .current_dir = outside,
      .global_agents_file = {},
      .max_file_bytes = 1024,
  });
  expect(outside_loaded.has_value(), "context loader handles current_dir outside workspace");
  if (outside_loaded)
  {
    expect(outside_loaded->size() == 1 && (*outside_loaded)[0].path == ava::core::normalized_absolute_path(root_agents),
           "outside current_dir only loads workspace root AGENTS.md");
  }

  auto with_global = ava::context::load_context_files(ava::context::ContextLoadOptions{
      .workspace_root = workspace,
      .current_dir = nested,
      .global_agents_file = global_agents,
      .max_file_bytes = 1024,
  });
  expect(with_global.has_value(), "context loader includes global AGENTS.md");
  if (with_global)
  {
    expect(with_global->size() == 4, "global AGENTS.md is appended to workspace contexts");
    if (with_global->size() == 4)
    {
      expect((*with_global)[3].path == ava::core::normalized_absolute_path(global_agents), "global AGENTS.md path is recorded");
      expect((*with_global)[3].source_type == ava::context::ContextSourceType::Global, "global AGENTS.md records global source type");

      auto const formatted = ava::context::format_context_for_prompt(*with_global);
      expect(formatted.find("# Loaded Project Instructions") != std::string::npos, "formatted context includes a section title");
      expect(formatted.find("## workspace: " + ava::core::normalized_absolute_path(root_agents).string()) != std::string::npos,
             "formatted context includes workspace source/path header");
      expect(formatted.find("## global: " + ava::core::normalized_absolute_path(global_agents).string()) != std::string::npos,
             "formatted context includes global source/path header");
      expect(formatted.find("root instructions") != std::string::npos && formatted.find("global instructions") != std::string::npos,
             "formatted context includes loaded content");
    }
  }

  auto const claude_workspace = root / "claude-workspace";
  auto const claude_nested = claude_workspace / "pkg";
  auto const claude_config = root / "claude-config" / "ava";
  std::filesystem::create_directories(claude_nested);
  std::filesystem::create_directories(claude_config);
  auto const root_claude = claude_workspace / "CLAUDE.md";
  auto const nested_agents_priority = claude_nested / "AGENTS.MD";
  auto const nested_claude_lower = claude_nested / "CLAUDE.md";
  auto const global_claude = claude_config / "CLAUDE.MD";
  {
    std::ofstream file(root_claude, std::ios::binary | std::ios::trunc);
    file << "root claude instructions\n";
  }
  {
    std::ofstream file(nested_agents_priority, std::ios::binary | std::ios::trunc);
    file << "nested agents priority\n";
  }
  {
    std::ofstream file(nested_claude_lower, std::ios::binary | std::ios::trunc);
    file << "nested claude should not load when agents exists\n";
  }
  {
    std::ofstream file(global_claude, std::ios::binary | std::ios::trunc);
    file << "global claude instructions\n";
  }
  auto claude_loaded = ava::context::load_context_files(ava::context::ContextLoadOptions{
      .workspace_root = claude_workspace,
      .current_dir = claude_nested,
      .global_agents_file = claude_config / "AGENTS.md",
      .max_file_bytes = 1024,
  });
  expect(claude_loaded.has_value(), "context loader accepts Pi-compatible CLAUDE.md context aliases");
  if (claude_loaded && claude_loaded->size() == 3)
  {
    expect((*claude_loaded)[0].path == ava::core::normalized_absolute_path(root_claude), "root CLAUDE.md loads as workspace context");
    expect((*claude_loaded)[1].path == ava::core::normalized_absolute_path(nested_agents_priority),
           "AGENTS.MD takes priority over CLAUDE.md in the same directory");
    expect((*claude_loaded)[2].path == ava::core::normalized_absolute_path(global_claude), "global CLAUDE.MD loads as a fallback context file");
  }
  else
  {
    expect(false, "context loader returns root, nested, and global compatible context files");
  }

  auto deduped_global = ava::context::load_context_files(ava::context::ContextLoadOptions{
      .workspace_root = workspace,
      .current_dir = nested,
      .global_agents_file = root_agents,
      .max_file_bytes = 1024,
  });
  expect(deduped_global.has_value(), "context loader handles duplicate global path");
  if (deduped_global)
  {
    expect(deduped_global->size() == 3, "duplicate global AGENTS.md is not loaded twice");
  }

  auto const oversized_workspace = root / "oversized";
  std::filesystem::create_directories(oversized_workspace);
  {
    std::ofstream file(oversized_workspace / "AGENTS.md", std::ios::binary | std::ios::trunc);
    file << std::string(6, 'x');
  }
  auto oversized = ava::context::load_context_files(ava::context::ContextLoadOptions{
      .workspace_root = oversized_workspace,
      .current_dir = oversized_workspace,
      .global_agents_file = {},
      .max_file_bytes = 5,
  });
  expect(!oversized && oversized.error().category() == ava::core::ErrorCategory::Io, "oversized context files fail safely");

  auto const symlink_workspace = root / "symlink-workspace";
  auto const symlink_target = root / "outside-secret.md";
  std::filesystem::create_directories(symlink_workspace);
  {
    std::ofstream file(symlink_target, std::ios::binary | std::ios::trunc);
    file << "secret instructions\n";
  }
  std::error_code symlink_error;
  std::filesystem::create_symlink(symlink_target, symlink_workspace / "AGENTS.md", symlink_error);
  expect(!symlink_error, "test creates symlinked AGENTS.md");
  if (!symlink_error)
  {
    auto symlinked = ava::context::load_context_files(ava::context::ContextLoadOptions{
        .workspace_root = symlink_workspace,
        .current_dir = symlink_workspace,
        .global_agents_file = {},
        .max_file_bytes = 1024,
    });
    expect(!symlinked && symlinked.error().category() == ava::core::ErrorCategory::Io, "context loader rejects symlinked AGENTS.md files");
  }

  auto const real_ancestor_workspace = root / "real-ancestor" / "workspace";
  auto const linked_ancestor = root / "linked-ancestor";
  std::filesystem::create_directories(real_ancestor_workspace);
  {
    std::ofstream file(real_ancestor_workspace / "AGENTS.md", std::ios::binary | std::ios::trunc);
    file << "loaded through linked ancestor\n";
  }
  symlink_error.clear();
  std::filesystem::create_directory_symlink(root / "real-ancestor", linked_ancestor, symlink_error);
  expect(!symlink_error, "test creates symlinked context ancestor");
  if (!symlink_error)
  {
    auto ancestor = ava::context::load_context_files(ava::context::ContextLoadOptions{
        .workspace_root = linked_ancestor / "workspace",
        .current_dir = linked_ancestor / "workspace",
        .global_agents_file = {},
        .max_file_bytes = 1024,
    });
    expect(ancestor.has_value(), "context loader follows symlinked ancestors in the workspace root path");
    if (ancestor)
    {
      expect(ancestor->size() == 1 && (*ancestor)[0].content.find("loaded through linked ancestor") != std::string::npos,
             "symlinked-ancestor workspace loads the real AGENTS.md through the link");
    }
  }

  auto const intermediate_workspace = root / "intermediate-workspace";
  auto const intermediate_target = root / "intermediate-target";
  std::filesystem::create_directories(intermediate_workspace);
  std::filesystem::create_directories(intermediate_target);
  {
    std::ofstream file(intermediate_target / "AGENTS.md", std::ios::binary | std::ios::trunc);
    file << "must not load through intermediate link\n";
  }
  symlink_error.clear();
  std::filesystem::create_directory_symlink(intermediate_target, intermediate_workspace / "nested", symlink_error);
  expect(!symlink_error, "test creates intermediate symlink below workspace root");
  if (!symlink_error)
  {
    auto intermediate = ava::context::load_context_files(ava::context::ContextLoadOptions{
        .workspace_root = intermediate_workspace,
        .current_dir = intermediate_workspace / "nested",
        .global_agents_file = {},
        .max_file_bytes = 1024,
    });
    expect(!intermediate && intermediate.error().category() == ava::core::ErrorCategory::Io,
           "context loader rejects symlinked intermediate directories instead of following swapped ancestors");
  }
}

void test_markdown_resource_parser_and_reader()
{
  auto const lf = ava::context::parse_markdown("---\nname: demo\ndescription: \"quoted value\"\n---\nbody line\n");
  expect(lf.frontmatter.at("name") == "demo" && lf.frontmatter.at("description") == "quoted value" && lf.body == "body line\n",
         "markdown parser accepts LF opening delimiter, strips matching quotes, and keeps the body");

  auto const crlf = ava::context::parse_markdown("---\r\nname: crlf\r\n---\r\ncrlf body\r\n");
  expect(crlf.frontmatter.at("name") == "crlf" && crlf.body == "crlf body\r\n", "markdown parser accepts CRLF opening delimiter and closing newline variants");

  auto const duplicates = ava::context::parse_markdown("---\nname: first\nname: second\n---\n");
  expect(duplicates.frontmatter.at("name") == "second" && ava::context::markdown_field(duplicates, "name") == "second",
         "markdown parser keeps the last duplicate frontmatter key");

  constexpr std::string_view kNoFrontmatter = "plain body without delimiter\n";
  auto const plain = ava::context::parse_markdown(kNoFrontmatter);
  expect(plain.frontmatter.empty() && plain.body == kNoFrontmatter, "markdown parser preserves full content as body when frontmatter is absent");

  constexpr std::string_view kUnclosed = "---\nname: open\nbody never closed\n";
  auto const unclosed = ava::context::parse_markdown(kUnclosed);
  expect(unclosed.frontmatter.empty() && unclosed.body == kUnclosed, "markdown parser preserves full content as body for malformed unclosed frontmatter");

  auto const root = create_empty_root("markdown-resource");
  auto const file = root / "resource.md";
  {
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out << "exact-size";
  }
  auto accepted = ava::context::read_resource_file(file, {.max_bytes = 10, .resource_description = "test resource"});
  expect(accepted && *accepted == "exact-size", "resource reader accepts a file whose size equals max_bytes");

  auto oversized = ava::context::read_resource_file(file, {.max_bytes = 9, .resource_description = "test resource"});
  ava::tests::expect_all(
      "resource reader rejects oversized files with structured size and path context",
      {{"rejected", !oversized},
       {"I/O category", !oversized && oversized.error().category() == ava::core::ErrorCategory::Io},
       {"exact path",
        !oversized && std::ranges::any_of(oversized.error().context(), [&](auto const& item) { return item.key == "path" && item.value == file.string(); })},
       {"max_bytes",
        !oversized && std::ranges::any_of(oversized.error().context(), [](auto const& item) { return item.key == "max_bytes" && item.value == "9"; })}});

  auto const symlink_path = root / "linked.md";
  std::error_code symlink_error;
  std::filesystem::create_symlink(file, symlink_path, symlink_error);
  expect(!symlink_error, "markdown resource test creates a final symlink");
  if (!symlink_error)
  {
    auto linked = ava::context::read_resource_file(symlink_path, {.max_bytes = 64, .resource_description = "test resource"});
    expect(!linked && linked.error().message() == "test resource is not a regular file", "resource reader rejects a final-path symlink");
  }

  auto const directory = root / "directory";
  std::filesystem::create_directories(directory);
  auto directory_read = ava::context::read_resource_file(directory, {.max_bytes = 64, .resource_description = "test resource"});
  expect(!directory_read && directory_read.error().message() == "test resource is not a regular file", "resource reader rejects directories as non-regular");

  auto const fifo_path = root / "blocking.fifo";
  if (!ava::tests::require(::mkfifo(fifo_path.c_str(), 0600) == 0, "markdown resource test creates a FIFO fixture"))
    return;
  auto const fifo_status = ava::tests::run_in_child_with_syscall_errno(std::span<ava::tests::SyscallErrno const>{}, [&] {
    auto const result = ava::context::read_resource_file(fifo_path, {.max_bytes = 64, .resource_description = "test resource"});
    return !result && result.error().category() == ava::core::ErrorCategory::Io ? 0 : 1;
  });
  ava::tests::expect_child_success(fifo_status, "resource reader rejects a FIFO as an I/O error under a parent watchdog");
}

void test_skill_loader()
{
  auto const root = create_empty_root("skills");

  auto const workspace = root / "workspace";
  auto const global = root / "global" / "skills";
  auto const project = root / "workspace" / ".ava" / "skills";
  auto const declared = root / "declared" / "plugin-skill.md";
  std::filesystem::create_directories(global / "release");
  std::filesystem::create_directories(global / "global-only");
  std::filesystem::create_directories(project / "release");
  std::filesystem::create_directories(project / "debugging" / "scripts");
  std::filesystem::create_directories(declared.parent_path());

  {
    std::ofstream file(global / "release" / "SKILL.md", std::ios::binary | std::ios::trunc);
    file << "---\nname: release\ndescription: Global release workflow\n---\nGlobal body\n";
  }
  {
    std::ofstream file(global / "global-only" / "SKILL.md", std::ios::binary | std::ios::trunc);
    file << "---\nname: global-only\ndescription: Global-only workflow\n---\nGlobal-only body\n";
  }
  {
    std::ofstream file(project / "release" / "SKILL.md", std::ios::binary | std::ios::trunc);
    file << "---\nname: release\ndescription: Project release workflow\n---\nProject body\n";
  }
  {
    std::ofstream file(project / "debugging" / "SKILL.md", std::ios::binary | std::ios::trunc);
    file << "---\nname: debugging\ndescription: Debug failures systematically\n---\nDebug body\n";
  }
  {
    std::ofstream file(project / "debugging" / "scripts" / "triage.sh", std::ios::binary | std::ios::trunc);
    file << "echo triage\n";
  }
  {
    std::ofstream file(declared, std::ios::binary | std::ios::trunc);
    file << "Declared plugin body\n";
  }

  auto loaded = ava::context::load_skills(ava::context::SkillLoadOptions{
      .workspace_root = root / "workspace", .global_skill_dirs = {global}, .project_skill_dirs = {project}, .max_file_bytes = 1024});
  expect(loaded.skills.size() == 3, "skill loader discovers global and project SKILL.md files");
  auto const release = std::ranges::find_if(loaded.skills, [](ava::context::LoadedSkill const& skill) { return skill.name == "release"; });
  expect(release != loaded.skills.end() && release->description == "Project release workflow" && release->content.find("Project body") != std::string::npos &&
             release->source_type == ava::context::SkillSourceType::Project,
         "project skills override global skills with the same name");
  auto const prompt = ava::context::format_available_skills_for_prompt(loaded.skills);
  expect(prompt.find("<available_skills>") != std::string::npos && prompt.find("<name>debugging</name>") != std::string::npos,
         "skill prompt formatting exposes names, descriptions, and locations");

  auto const debugging = std::ranges::find_if(loaded.skills, [](ava::context::LoadedSkill const& skill) { return skill.name == "debugging"; });
  expect(debugging != loaded.skills.end(), "debugging skill is discovered");
  if (debugging != loaded.skills.end())
  {
    auto sampled = ava::context::sample_skill_files(debugging->directory);
    auto const tool_content = ava::context::format_loaded_skill_for_tool(*debugging, sampled);
    expect(tool_content.find("Debug body") != std::string::npos && tool_content.find("Relative paths in this skill") != std::string::npos &&
               tool_content.find("triage.sh") != std::string::npos,
           "loaded skill tool content includes body, base-dir guidance, and sampled files");
  }

  auto global_disabled = ava::context::load_skills(
      ava::context::SkillLoadOptions{.workspace_root = workspace,
                                     .global_skill_dirs = {global},
                                     .project_skill_dirs = {project},
                                     .declared_skill_files = {ava::context::DeclaredSkillFileOptions{.path = declared,
                                                                                                     .name = "declared-plugin-skill",
                                                                                                     .description = "Declared plugin workflow",
                                                                                                     .source_type = ava::context::SkillSourceType::Plugin,
                                                                                                     .preloaded_content = std::nullopt,
                                                                                                     .max_file_bytes = 1024}},
                                     .max_file_bytes = 1024,
                                     .include_global_skills = false,
                                     .include_project_skills = true});
  expect(std::ranges::none_of(global_disabled.skills,
                              [](ava::context::LoadedSkill const& skill) { return skill.source_type == ava::context::SkillSourceType::Global; }) &&
             std::ranges::any_of(global_disabled.skills, [](ava::context::LoadedSkill const& skill) { return skill.name == "debugging"; }) &&
             std::ranges::any_of(global_disabled.skills, [](ava::context::LoadedSkill const& skill) { return skill.name == "declared-plugin-skill"; }),
         "skill loader suppresses explicit global directories while retaining independent project and declared skill sources");

  std::filesystem::create_directories(project / "invalid");
  {
    std::ofstream file(project / "invalid" / "SKILL.md", std::ios::binary | std::ios::trunc);
    file << "---\nname: Bad Name\ndescription: Invalid\n---\n";
  }
  loaded = ava::context::load_skills(
      ava::context::SkillLoadOptions{.workspace_root = root / "workspace", .global_skill_dirs = {}, .project_skill_dirs = {project}, .max_file_bytes = 1024});
  expect(std::ranges::any_of(
             loaded.diagnostics,
             [](ava::context::SkillDiagnostic const& diagnostic) { return diagnostic.message.find("skill name is invalid") != std::string::npos; }),
         "skill loader reports invalid skill files without blocking valid skills");
}

void test_auth_record_helpers()
{
  expect(ava::config::is_valid_provider_id("openai") && ava::config::is_valid_provider_id("custom-provider_1"),
         "auth record provider id accepts provider-safe names");
  expect(!ava::config::is_valid_provider_id("") && !ava::config::is_valid_provider_id("bad provider"),
         "auth record provider id rejects empty and whitespace names");

  auto const api_object = ava::config::provider_credential_object_json(ava::config::ProviderCredential{
      .provider_id = "custom", .access_token = "token\"with-quote", .credential_type = "api_key", .account_id = "", .source = "test"});
  expect(api_object && api_object->find("\"type\": \"api_key\"") != std::string::npos && api_object->find("token\\\"with-quote") != std::string::npos,
         "auth record serializes provider API key credential object with JSON escaping");

  auto const anthropic_oauth_object = ava::config::provider_credential_object_json(ava::config::ProviderCredential{.provider_id = "anthropic",
                                                                                                                   .access_token = "oauth-token",
                                                                                                                   .credential_type = "oauth",
                                                                                                                   .account_id = "acct_anthropic",
                                                                                                                   .source = "test",
                                                                                                                   .refresh_token = "refresh-token",
                                                                                                                   .expires_at = 1234,
                                                                                                                   .source_metadata = "claude"});
  expect(anthropic_oauth_object && anthropic_oauth_object->find("\"type\": \"oauth\"") != std::string::npos &&
             anthropic_oauth_object->find("\"access_token\": \"oauth-token\"") != std::string::npos &&
             anthropic_oauth_object->find("\"refresh_token\": \"refresh-token\"") != std::string::npos &&
             anthropic_oauth_object->find("\"expires_at\": 1234") != std::string::npos &&
             anthropic_oauth_object->find("\"account_id\": \"acct_anthropic\"") != std::string::npos &&
             anthropic_oauth_object->find("\"source\": \"claude\"") != std::string::npos,
         "auth record serializes Anthropic OAuth provider credentials");

  auto const unsupported_provider_credential = ava::config::provider_credential_object_json(ava::config::ProviderCredential{
      .provider_id = "custom", .access_token = "token", .credential_type = "token", .account_id = "acct_123", .source = "test"});
  expect(!unsupported_provider_credential && unsupported_provider_credential.error().category() == ava::core::ErrorCategory::InvalidArgument,
         "auth record rejects generic provider credential types other than API key");

  auto const bad_provider = ava::config::provider_credential_object_json(ava::config::ProviderCredential{
      .provider_id = "bad provider", .access_token = "token", .credential_type = "api_key", .account_id = "", .source = "test"});
  expect(!bad_provider && bad_provider.error().category() == ava::core::ErrorCategory::InvalidArgument,
         "auth record rejects invalid provider ids before serializing credentials");

  auto const path = std::filesystem::path("/tmp/auth-record.json");
  auto parsed = ava::config::parse_auth_record_members(
      "{\n"
      "  \"openai\": {\"type\":\"api_key\",\"api_key\":\"k\"},\n"
      "  \"nested\": [1, {\"text\": \"value, with comma\"}],\n"
      "  \"escaped\\nkey\": true\n"
      "}\n",
      path);
  expect(parsed && parsed->size() == 3, "auth record parser preserves top-level JSON members");
  if (parsed && parsed->size() == 3)
  {
    expect((*parsed)[0].key == "openai" && (*parsed)[0].raw_value.find("\"api_key\":\"k\"") != std::string::npos,
           "auth record parser captures object member raw JSON");
    expect((*parsed)[1].key == "nested" && (*parsed)[1].raw_value.find("value, with comma") != std::string::npos,
           "auth record parser handles nested arrays and string commas");
    expect((*parsed)[2].key == "escaped\nkey" && (*parsed)[2].raw_value == "true", "auth record parser decodes escaped member keys");

    auto serialized = ava::config::serialize_auth_record_members(*parsed);
    auto reparsed = ava::config::parse_auth_record_members(serialized, path);
    expect(reparsed && reparsed->size() == parsed->size() && (*reparsed)[2].key == "escaped\nkey",
           "auth record serialization round trips parser-visible members");
  }

  expect(ava::config::serialize_auth_record_members({}) == "{\n}\n", "auth record serialization emits an empty JSON object for no members");
  auto const trailing = ava::config::parse_auth_record_members("{\"openai\": true} trailing", path);
  expect(!trailing && trailing.error().format().find("trailing content") != std::string::npos, "auth record parser rejects trailing content");
  auto const missing_value = ava::config::parse_auth_record_members("{\"openai\": }", path);
  expect(!missing_value && missing_value.error().format().find("invalid value") != std::string::npos, "auth record parser rejects missing member values");
}

void test_auth_load_and_store()
{
  auto const root = create_empty_root("auth");

  std::filesystem::create_directories(root / "config" / "ava");
  ScopedEnvVar home("HOME", (root / "home").string());
  ScopedEnvVar config("XDG_CONFIG_HOME", (root / "config").string());
  ScopedEnvVar state("XDG_STATE_HOME", (root / "state").string());
  ScopedEnvVar data("XDG_DATA_HOME", (root / "data").string());

  auto const paths = ava::config::xdg_paths();
  {
    std::ofstream file(paths.auth_file, std::ios::binary | std::ios::trunc);
    file << "{\"openai\":{\"type\":\"oauth\",\"access_token\":\"secret-token\",\"refresh_token\":\"refresh\",\"expires_"
            "at\":42}}";
  }
  ::chmod(paths.auth_file.c_str(), S_IRUSR | S_IWUSR);
  auto loaded = ava::config::load_openai_credential(paths);
  expect(loaded && loaded->has_value(), "OpenAI OAuth credential loads from AVA XDG auth file");
  if (loaded && *loaded)
  {
    expect((*loaded)->access_token == "secret-token", "OpenAI access token parses");
    expect((*loaded)->source_path == paths.auth_file, "credential source path records location only");
  }

  std::error_code remove_error;
  std::filesystem::remove(paths.auth_file, remove_error);
  auto const compatible_auth_path = ava::config::legacy_compatible_auth_path();
  std::filesystem::create_directories(compatible_auth_path.parent_path());
  {
    std::ofstream file(compatible_auth_path, std::ios::binary | std::ios::trunc);
    file << "{\"openai\":{\"type\":\"oauth\",\"access\":\"legacy-token\",\"refresh\":\"r\",\"expires\":7}}";
  }
  ::chmod(compatible_auth_path.c_str(), S_IRUSR | S_IWUSR | S_IRGRP);
  auto insecure_import = ava::config::load_openai_credential(paths);
  expect(insecure_import && !insecure_import->has_value(), "OpenAI credential load skips group-readable fallback auth file");
  ::chmod(compatible_auth_path.c_str(), S_IRUSR | S_IWUSR);
  auto imported = ava::config::load_openai_credential(paths);
  expect(imported && imported->has_value() && (*imported)->access_token == "legacy-token", "OpenAI OAuth credential is recognized from compatible auth path");

  {
    std::ofstream file(paths.auth_file, std::ios::binary | std::ios::trunc);
    file << "{\"openai\":{\"type\":\"api\",\"key\":\"ava-api-key\"}}";
  }
  ::chmod(paths.auth_file.c_str(), S_IRUSR | S_IWUSR);
  auto explicit_ava_preferred = ava::config::load_openai_credential(paths);
  expect(explicit_ava_preferred && explicit_ava_preferred->has_value() && (*explicit_ava_preferred)->access_token == "ava-api-key" &&
             (*explicit_ava_preferred)->type == ava::config::OpenAICredentialType::ApiKey,
         "explicit AVA OpenAI auth is preferred over legacy fallback auth");

  std::filesystem::remove(paths.auth_file, remove_error);
  std::filesystem::remove_all(root / "home" / ".ava", remove_error);
  std::filesystem::create_directories(root / "home" / ".ava" / "credentials.json");
  {
    std::ofstream file(compatible_auth_path, std::ios::binary | std::ios::trunc);
    file << "{\"openai\":{\"type\":\"api\",\"key\":\"legacy-api-key\"}}";
  }
  ::chmod(compatible_auth_path.c_str(), S_IRUSR | S_IWUSR);
  auto api_key = ava::config::load_openai_credential(paths);
  expect(api_key && api_key->has_value() && (*api_key)->access_token == "legacy-api-key" && (*api_key)->type == ava::config::OpenAICredentialType::ApiKey,
         "OpenAI API key auth shape loads after skipping non-regular legacy candidate");

  auto stored = ava::config::store_openai_credential(paths, ava::config::OpenAICredential{.type = ava::config::OpenAICredentialType::OAuth,
                                                                                          .access_token = "stored-token",
                                                                                          .refresh_token = "stored-refresh",
                                                                                          .expires_at = 99,
                                                                                          .account_id = "acct_stored",
                                                                                          .source_path = {}});
  expect(stored.has_value(), "OpenAI OAuth credential stores to AVA XDG auth file");
  auto stored_oauth = ava::config::load_openai_credential(paths);
  expect(stored_oauth && stored_oauth->has_value() && (*stored_oauth)->type == ava::config::OpenAICredentialType::OAuth &&
             (*stored_oauth)->access_token == "stored-token" && (*stored_oauth)->refresh_token == "stored-refresh" && (*stored_oauth)->expires_at == 99 &&
             (*stored_oauth)->account_id == "acct_stored",
         "OpenAI OAuth credential store/load round trips type, token, and account fields");

  auto stored_api = ava::config::store_openai_credential(paths, ava::config::OpenAICredential{.type = ava::config::OpenAICredentialType::ApiKey,
                                                                                              .access_token = "stored-api-key",
                                                                                              .refresh_token = "ignored-refresh",
                                                                                              .expires_at = 1,
                                                                                              .account_id = "",
                                                                                              .source_path = {}});
  expect(stored_api.has_value(), "OpenAI API key credential stores to AVA XDG auth file");
  auto loaded_api = ava::config::load_openai_credential(paths);
  expect(loaded_api && loaded_api->has_value() && (*loaded_api)->type == ava::config::OpenAICredentialType::ApiKey &&
             (*loaded_api)->access_token == "stored-api-key" && (*loaded_api)->refresh_token.empty() && (*loaded_api)->expires_at == 0,
         "OpenAI API key credential store/load round trips without OAuth expiry");

  auto stored_anthropic_api = ava::config::store_provider_credential(
      paths, ava::config::ProviderCredential{
                 .provider_id = "anthropic", .access_token = "stored-anthropic-api-key", .credential_type = "api_key", .account_id = "", .source = "test"});
  expect(stored_anthropic_api.has_value(), "generic provider API key credential stores to auth file");
  loaded_api = ava::config::load_openai_credential(paths);
  expect(loaded_api && loaded_api->has_value() && (*loaded_api)->access_token == "stored-api-key",
         "generic provider credential store preserves existing OpenAI credential");
  ava::tests::FakeTransport generic_store_transport({});
  auto stored_anthropic_api_credential = ava::config::provider_credential_for_request(paths, "anthropic", generic_store_transport);
  expect(stored_anthropic_api_credential && stored_anthropic_api_credential->has_value() &&
             (*stored_anthropic_api_credential)->access_token == "stored-anthropic-api-key" && (*stored_anthropic_api_credential)->credential_type == "api_key",
         "generic provider API key credential loads after storing");

  auto stored_anthropic_unsupported = ava::config::store_provider_credential(
      paths, ava::config::ProviderCredential{
                 .provider_id = "anthropic", .access_token = "stored-anthropic-token", .credential_type = "token", .account_id = "", .source = "test"});
  expect(!stored_anthropic_unsupported, "generic provider credential store rejects non-API-key credentials");
  auto stored_anthropic_after_unsupported = ava::config::provider_credential_for_request(paths, "anthropic", generic_store_transport);
  expect(stored_anthropic_after_unsupported && stored_anthropic_after_unsupported->has_value() &&
             (*stored_anthropic_after_unsupported)->access_token == "stored-anthropic-api-key" &&
             (*stored_anthropic_after_unsupported)->credential_type == "api_key",
         "generic provider credential store preserves API key after unsupported credential attempt");

  auto stored_moonshot_api = ava::config::store_provider_credential(
      paths, ava::config::ProviderCredential{
                 .provider_id = "moonshot", .access_token = "stored-moonshot-api-key", .credential_type = "api_key", .account_id = "", .source = "test"});
  expect(stored_moonshot_api.has_value(), "Moonshot API key credential stores through generic provider auth");
  auto stored_moonshot_credential = ava::config::provider_credential_for_request(paths, "moonshot", generic_store_transport);
  expect(stored_moonshot_credential && stored_moonshot_credential->has_value() && (*stored_moonshot_credential)->access_token == "stored-moonshot-api-key" &&
             (*stored_moonshot_credential)->credential_type == "api_key",
         "Moonshot API key credential loads through generic provider auth");

  auto rotated_openai_api = ava::config::store_openai_credential(paths, ava::config::OpenAICredential{.type = ava::config::OpenAICredentialType::ApiKey,
                                                                                                      .access_token = "rotated-openai-api-key",
                                                                                                      .refresh_token = "",
                                                                                                      .expires_at = 0,
                                                                                                      .account_id = "",
                                                                                                      .source_path = {}});
  expect(rotated_openai_api.has_value(), "OpenAI credential update stores after generic provider credentials");
  loaded_api = ava::config::load_openai_credential(paths);
  expect(loaded_api && loaded_api->has_value() && (*loaded_api)->access_token == "rotated-openai-api-key",
         "OpenAI credential update loads after provider credential merge");
  stored_anthropic_after_unsupported = ava::config::provider_credential_for_request(paths, "anthropic", generic_store_transport);
  expect(stored_anthropic_after_unsupported && stored_anthropic_after_unsupported->has_value() &&
             (*stored_anthropic_after_unsupported)->access_token == "stored-anthropic-api-key",
         "OpenAI credential update preserves Anthropic provider credential");
  stored_moonshot_credential = ava::config::provider_credential_for_request(paths, "moonshot", generic_store_transport);
  expect(stored_moonshot_credential && stored_moonshot_credential->has_value() && (*stored_moonshot_credential)->access_token == "stored-moonshot-api-key",
         "OpenAI credential update preserves Moonshot provider credential");

  ::chmod(paths.auth_file.c_str(), S_IRUSR | S_IWUSR | S_IRGRP);
  auto repaired_broad_permissions = ava::config::store_provider_credential(
      paths, ava::config::ProviderCredential{
                 .provider_id = "kimi", .access_token = "stored-kimi-api-key", .credential_type = "api_key", .account_id = "", .source = "test"});
  expect(repaired_broad_permissions.has_value(), "provider credential store repairs user-owned broad auth permissions");
  struct stat auth_stat{};
  expect(::stat(paths.auth_file.c_str(), &auth_stat) == 0 && (auth_stat.st_mode & (S_IRWXG | S_IRWXO)) == 0,
         "provider credential store rewrites auth file with owner-only permissions");
  std::ifstream repaired_auth_file(paths.auth_file, std::ios::binary);
  std::stringstream repaired_auth_stream;
  repaired_auth_stream << repaired_auth_file.rdbuf();
  auto const repaired_auth_json = repaired_auth_stream.str();
  expect(repaired_auth_json.find("openai") == std::string::npos && repaired_auth_json.find("anthropic") == std::string::npos &&
             repaired_auth_json.find("moonshot") == std::string::npos,
         "provider credential store discards broad-permission auth contents instead of blessing them");
  auto stored_kimi_credential = ava::config::provider_credential_for_request(paths, "kimi", generic_store_transport);
  expect(stored_kimi_credential && stored_kimi_credential->has_value() && (*stored_kimi_credential)->access_token == "stored-kimi-api-key",
         "provider credential store writes only the requested credential while repairing permissions");

  {
    std::ofstream file(paths.auth_file, std::ios::binary | std::ios::trunc);
    file << "{\"openai\":{\"type\":\"api_key\",\"api_key\":\"bad\"}}trailing";
  }
  ::chmod(paths.auth_file.c_str(), S_IRUSR | S_IWUSR);
  auto malformed_auth_store = ava::config::store_provider_credential(
      paths, ava::config::ProviderCredential{
                 .provider_id = "anthropic", .access_token = "should-not-overwrite", .credential_type = "api_key", .account_id = "", .source = "test"});
  expect(!malformed_auth_store && malformed_auth_store.error().format().find("trailing content") != std::string::npos,
         "provider credential store rejects malformed auth JSON with actionable error");
  {
    std::ofstream file(paths.auth_file, std::ios::binary | std::ios::trunc);
    file << "{\"openai\": }";
  }
  ::chmod(paths.auth_file.c_str(), S_IRUSR | S_IWUSR);
  auto missing_value_auth_store = ava::config::store_provider_credential(
      paths, ava::config::ProviderCredential{
                 .provider_id = "anthropic", .access_token = "should-not-overwrite", .credential_type = "api_key", .account_id = "", .source = "test"});
  expect(!missing_value_auth_store && missing_value_auth_store.error().format().find("invalid value") != std::string::npos,
         "provider credential store rejects auth JSON members with missing values");
  std::filesystem::remove(paths.auth_file, remove_error);
  auto restored_after_malformed_auth =
      ava::config::store_openai_credential(paths, ava::config::OpenAICredential{.type = ava::config::OpenAICredentialType::ApiKey,
                                                                                .access_token = "rotated-openai-api-key",
                                                                                .refresh_token = "",
                                                                                .expires_at = 0,
                                                                                .account_id = "",
                                                                                .source_path = {}});
  expect(restored_after_malformed_auth.has_value(), "OpenAI credential stores after removing malformed auth file");

  expect(!ava::config::parse_openai_credential("{\"openai\":{\"type\":\"oauth\",\"api_key\":\"wrong\"}}"),
         "OpenAI credential parser rejects typed OAuth without access credential");
  expect(!ava::config::parse_openai_credential("{\"openai\":{\"type\":\"api_key\",\"access_token\":\"wrong\"}}"),
         "OpenAI credential parser rejects typed API key without key field");
  expect(!ava::config::parse_openai_credential("{\"openai\":{\"type\":\"unknown\",\"key\":\"wrong\"}}"), "OpenAI credential parser rejects unknown auth type");
  auto expired_oauth = ava::config::openai_access_token_for_request(ava::config::OpenAICredential{.type = ava::config::OpenAICredentialType::OAuth,
                                                                                                  .access_token = "expired-token",
                                                                                                  .refresh_token = "",
                                                                                                  .expires_at = 10,
                                                                                                  .account_id = "",
                                                                                                  .source_path = paths.auth_file},
                                                                    11);
  expect(!expired_oauth && expired_oauth.error().category() == ava::core::ErrorCategory::PermissionDenied,
         "OpenAI expired OAuth credential is not usable for requests");
  auto api_key_not_expired = ava::config::openai_access_token_for_request(ava::config::OpenAICredential{.type = ava::config::OpenAICredentialType::ApiKey,
                                                                                                        .access_token = "api-token",
                                                                                                        .refresh_token = "",
                                                                                                        .expires_at = 10,
                                                                                                        .account_id = "",
                                                                                                        .source_path = {}},
                                                                          11);
  expect(api_key_not_expired && *api_key_not_expired == "api-token", "OpenAI API key ignores OAuth expiry field");

  ScopedEnvVar openai_key("OPENAI_API_KEY", "env-openai-key");
  ava::tests::FakeTransport env_transport({});
  auto stored_provider_credential = ava::config::provider_credential_for_request(paths, "openai", env_transport);
  expect(stored_provider_credential && stored_provider_credential->has_value() && (*stored_provider_credential)->access_token == "rotated-openai-api-key" &&
             (*stored_provider_credential)->credential_type == "api_key",
         "provider credential discovery prefers stored OpenAI auth before env fallback");
  std::filesystem::remove(paths.auth_file, remove_error);
  std::filesystem::remove(compatible_auth_path, remove_error);
  auto env_openai_credential = ava::config::provider_credential_for_request(paths, "openai", env_transport);
  expect(env_openai_credential && env_openai_credential->has_value() && (*env_openai_credential)->access_token == "env-openai-key" &&
             (*env_openai_credential)->source == "env:OPENAI_API_KEY",
         "provider credential discovery falls back to OPENAI_API_KEY without storing it");
  ScopedEnvVar openai_key_unset("OPENAI_API_KEY", std::nullopt);
  {
    std::ofstream file(paths.auth_file, std::ios::binary | std::ios::trunc);
    file << "{\"anthropic\":{\"type\":\"api_key\",\"api_key\":\"stored-anthropic-key\"}}";
  }
  ::chmod(paths.auth_file.c_str(), S_IRUSR | S_IWUSR);
  auto stored_anthropic_credential = ava::config::provider_credential_for_request(paths, "anthropic", env_transport);
  expect(stored_anthropic_credential && stored_anthropic_credential->has_value() && (*stored_anthropic_credential)->provider_id == "anthropic" &&
             (*stored_anthropic_credential)->access_token == "stored-anthropic-key" && (*stored_anthropic_credential)->credential_type == "api_key" &&
             (*stored_anthropic_credential)->source == paths.auth_file.string(),
         "provider credential discovery loads non-OpenAI API keys from auth file");
  std::filesystem::remove(paths.auth_file, remove_error);
  ScopedEnvVar anthropic_oauth("ANTHROPIC_OAUTH_TOKEN", std::nullopt);
  ScopedEnvVar anthropic_auth("ANTHROPIC_AUTH_TOKEN", std::nullopt);
  ScopedEnvVar anthropic_key("ANTHROPIC_API_KEY", "env-anthropic-key");
  auto anthropic_credential = ava::config::provider_credential_for_request(paths, "anthropic", env_transport);
  expect(anthropic_credential && anthropic_credential->has_value() && (*anthropic_credential)->provider_id == "anthropic" &&
             (*anthropic_credential)->access_token == "env-anthropic-key",
         "provider credential discovery supports non-OpenAI API key environment variables");
  ScopedEnvVar anthropic_key_unset("ANTHROPIC_API_KEY", std::nullopt);
  auto restored_api = ava::config::store_openai_credential(paths, ava::config::OpenAICredential{.type = ava::config::OpenAICredentialType::ApiKey,
                                                                                                .access_token = "stored-api-key",
                                                                                                .refresh_token = "",
                                                                                                .expires_at = 0,
                                                                                                .account_id = "",
                                                                                                .source_path = {}});
  expect(restored_api.has_value(), "OpenAI API key credential restores after env discovery checks");
  struct stat st{};
  if (::stat(paths.auth_file.c_str(), &st) == 0)
  {
    expect((st.st_mode & 0777) == 0600, "auth file is owner-only");
  }
  else
  {
    expect(false, "auth file stat succeeds");
  }
  struct stat dir_st{};
  if (::stat(paths.auth_file.parent_path().c_str(), &dir_st) == 0)
  {
    expect((dir_st.st_mode & 0777) == 0700, "auth directory is owner-only");
  }
  else
  {
    expect(false, "auth directory stat succeeds");
  }

  std::filesystem::remove(paths.auth_file, remove_error);
  {
    std::ofstream file(paths.auth_file, std::ios::binary | std::ios::trunc);
    file << "{\"openai\":{\"type\":\"api_key\",\"api_key\":\"too-readable\"}}";
  }
  ::chmod(paths.auth_file.c_str(), S_IRUSR | S_IWUSR | S_IRGRP);
  auto broad_auth_file = ava::config::load_openai_credential(paths);
  expect(!broad_auth_file && broad_auth_file.error().category() == ava::core::ErrorCategory::PermissionDenied,
         "OpenAI credential load rejects group-readable auth file");

  auto const symlink_target = root / "symlink-target.json";
  {
    std::ofstream file(symlink_target, std::ios::binary | std::ios::trunc);
    file << "unchanged";
  }
  std::filesystem::remove(paths.auth_file, remove_error);
  std::error_code symlink_error;
  std::filesystem::create_symlink(symlink_target, paths.auth_file, symlink_error);
  expect(!symlink_error, "auth fixture creates a symlink instead of silently omitting the safety case");
  if (!symlink_error)
  {
    auto symlink_store = ava::config::store_openai_credential(paths, ava::config::OpenAICredential{.type = ava::config::OpenAICredentialType::ApiKey,
                                                                                                   .access_token = "must-not-write-through-symlink",
                                                                                                   .refresh_token = "",
                                                                                                   .expires_at = 0,
                                                                                                   .account_id = "",
                                                                                                   .source_path = {}});
    expect(!symlink_store, "OpenAI credential store rejects symlink auth file");
    std::ifstream target_read(symlink_target, std::ios::binary);
    std::string target_content;
    target_read >> target_content;
    expect(target_content == "unchanged", "OpenAI credential store does not write through symlink");
  }
}

void test_openai_oauth_helpers()
{
  std::string const verifier = "dBjftJeZ4CVP-mB92K27uhbUJU1p1r_wW1gFWFOEjXk";
  expect(ava::config::openai_oauth_code_challenge(verifier) == "E9Melhoa2OwvFrEMTJguCHaoeK1t8URWbuGJSstw-cM",
         "OpenAI OAuth PKCE challenge matches RFC 7636 test vector");
  expect(ava::config::openai_oauth_account_id_from_token(
             "header.eyJodHRwczovL2FwaS5vcGVuYWkuY29tL2F1dGgiOnsiY2hhdGdwdF9hY2NvdW50X2lkIjoiYWNjdF8xMjMifX0.sig") == "acct_123",
         "OpenAI OAuth account id extracts from compatibility JWT claim");

  auto session = ava::config::make_openai_oauth_session(verifier, "state value");
  expect(session.has_value(), "OpenAI OAuth deterministic session builds");
  if (session)
  {
    expect(session->authorization_url.find("https://auth.openai.com/oauth/authorize?") == 0, "OpenAI OAuth authorization URL uses auth.openai.com");
    expect(session->authorization_url.find("client_id=app_EMoamEEZ73f0CkXaXp7hrann") != std::string::npos,
           "OpenAI OAuth authorization URL includes compatibility client id");
    expect(session->authorization_url.find("redirect_uri=http%3A%2F%2Flocalhost%3A1455%2Fauth%2Fcallback") != std::string::npos,
           "OpenAI OAuth authorization URL includes local callback redirect");
    expect(session->authorization_url.find("code_challenge=E9Melhoa2OwvFrEMTJguCHaoeK1t8URWbuGJSstw-cM") != std::string::npos,
           "OpenAI OAuth authorization URL includes S256 code challenge");
    expect(session->authorization_url.find("state=state%20value") != std::string::npos, "OpenAI OAuth authorization URL percent-encodes state");
    expect(session->authorization_url.find("originator=ava") != std::string::npos, "OpenAI OAuth authorization URL identifies AVA originator");
  }

  ava::tests::FakeTransport transport({ava::http::HttpResponse{
      .status_code = 200,
      .headers = {},
      .body = "{\"access_token\":\"access\",\"refresh_token\":\"refresh\",\"expires_in\":120,"
              "\"id_token\":\"header."
              "eyJodHRwczovL2FwaS5vcGVuYWkuY29tL2F1dGgiOnsiY2hhdGdwdF9hY2NvdW50X2lkIjoiYWNjdF8xMjMifX0.sig\"}",
  }});
  auto credential = ava::config::exchange_openai_oauth_code("code value", verifier, transport, 1000);
  expect(credential.has_value(), "OpenAI OAuth code exchange parses token response");
  if (credential)
  {
    expect(credential->type == ava::config::OpenAICredentialType::OAuth && credential->access_token == "access" && credential->refresh_token == "refresh" &&
               credential->expires_at == 1120 && credential->account_id == "acct_123",
           "OpenAI OAuth code exchange returns OAuth credential with absolute expiry and account id");
  }
  auto const& requests = transport.requests();
  expect(requests.size() == 1 && requests.front().url == "https://auth.openai.com/oauth/token" && requests.front().method == "POST",
         "OpenAI OAuth code exchange posts to token endpoint");
  if (!requests.empty())
  {
    expect(requests.front().body.find("grant_type=authorization_code") != std::string::npos &&
               requests.front().body.find("code=code%20value") != std::string::npos &&
               requests.front().body.find("code_verifier=" + verifier) != std::string::npos,
           "OpenAI OAuth code exchange form-encodes authorization code and verifier");
  }

  ava::tests::FakeTransport device_start_transport({ava::http::HttpResponse{
      .status_code = 200,
      .headers = {},
      .body = "{\"device_auth_id\":\"device-123\",\"user_code\":\"ABCD-EFGH\",\"interval\":\"1\"}",
  }});
  auto device = ava::config::start_openai_oauth_device_authorization(device_start_transport);
  expect(device && device->device_auth_id == "device-123" && device->user_code == "ABCD-EFGH" &&
             device->verification_url == "https://auth.openai.com/codex/device" && device->interval_seconds == 1,
         "OpenAI headless OAuth device authorization parses user code and polling interval");
  auto const& device_start_requests = device_start_transport.requests();
  expect(device_start_requests.size() == 1 && device_start_requests.front().url == "https://auth.openai.com/api/accounts/deviceauth/usercode" &&
             device_start_requests.front().body.find("app_EMoamEEZ73f0CkXaXp7hrann") != std::string::npos,
         "OpenAI headless OAuth starts device authorization with the compatibility client id");

  if (device)
  {
    ava::tests::FakeTransport pending_transport({ava::http::HttpResponse{
        .status_code = 403,
        .headers = {},
        .body = "{}",
    }});
    auto pending = ava::config::poll_openai_oauth_device_authorization(*device, pending_transport, 1000);
    expect(pending && !pending->has_value(), "OpenAI headless OAuth treats 403 polling response as pending");

    ava::tests::FakeTransport approved_transport({ava::http::HttpResponse{
                                                      .status_code = 200,
                                                      .headers = {},
                                                      .body = "{\"authorization_code\":\"device code\","
                                                              "\"code_verifier\":\"device-verifier\"}",
                                                  },
                                                  ava::http::HttpResponse{
                                                      .status_code = 200,
                                                      .headers = {},
                                                      .body = "{\"access_token\":\"device-access\","
                                                              "\"refresh_token\":\"device-refresh\","
                                                              "\"expires_in\":90,\"account_id\":\"acct_device\"}",
                                                  }});
    auto approved = ava::config::poll_openai_oauth_device_authorization(*device, approved_transport, 1000);
    expect(approved && approved->has_value() && (*approved)->access_token == "device-access" && (*approved)->refresh_token == "device-refresh" &&
               (*approved)->expires_at == 1090 && (*approved)->account_id == "acct_device",
           "OpenAI headless OAuth exchanges approved device code for an OAuth credential");
    auto const& approved_requests = approved_transport.requests();
    expect(approved_requests.size() == 2 && approved_requests[0].url == "https://auth.openai.com/api/accounts/deviceauth/token" &&
               approved_requests[1].url == "https://auth.openai.com/oauth/token" &&
               approved_requests[1].body.find("redirect_uri=https%3A%2F%2Fauth.openai.com%2Fdeviceauth%2Fcallback") != std::string::npos,
           "OpenAI headless OAuth uses the device callback redirect during token exchange");
  }
}

void test_openai_oauth_refresh()
{
  ava::tests::FakeTransport refresh_transport({ava::http::HttpResponse{
      .status_code = 200,
      .headers = {},
      .body = "{\"access_token\":\"refreshed-access\",\"refresh_token\":\"rotated-refresh\","
              "\"expires_in\":120,\"account_id\":\"acct_rotated\"}",
  }});
  auto const refreshed = ava::config::refresh_openai_oauth_credential(ava::config::OpenAICredential{.type = ava::config::OpenAICredentialType::OAuth,
                                                                                                    .access_token = "old-access",
                                                                                                    .refresh_token = "old refresh/token",
                                                                                                    .expires_at = 900,
                                                                                                    .account_id = "acct_old",
                                                                                                    .source_path = {}},
                                                                      refresh_transport, 1000);
  expect(refreshed && refreshed->access_token == "refreshed-access" && refreshed->refresh_token == "rotated-refresh" && refreshed->expires_at == 1120 &&
             refreshed->account_id == "acct_rotated",
         "OpenAI OAuth refresh parses rotated token response");
  auto const& refresh_requests = refresh_transport.requests();
  expect(refresh_requests.size() == 1 && refresh_requests.front().url == "https://auth.openai.com/oauth/token" && refresh_requests.front().method == "POST",
         "OpenAI OAuth refresh posts to token endpoint");
  if (!refresh_requests.empty())
  {
    expect(refresh_requests.front().body.find("grant_type=refresh_token") != std::string::npos &&
               refresh_requests.front().body.find("refresh_token=old%20refresh%2Ftoken") != std::string::npos &&
               refresh_requests.front().body.find("client_id=app_EMoamEEZ73f0CkXaXp7hrann") != std::string::npos,
           "OpenAI OAuth refresh form-encodes refresh token and client id");
  }

  ava::tests::FakeTransport preserved_transport({ava::http::HttpResponse{
      .status_code = 200,
      .headers = {},
      .body = "{\"access_token\":\"preserved-access\",\"expires_at\":2222}",
  }});
  auto const preserved = ava::config::refresh_openai_oauth_credential(ava::config::OpenAICredential{.type = ava::config::OpenAICredentialType::OAuth,
                                                                                                    .access_token = "old-access",
                                                                                                    .refresh_token = "stable-refresh",
                                                                                                    .expires_at = 900,
                                                                                                    .account_id = "acct_stable",
                                                                                                    .source_path = {}},
                                                                      preserved_transport, 1000);
  expect(preserved && preserved->access_token == "preserved-access" && preserved->refresh_token == "stable-refresh" && preserved->expires_at == 2222 &&
             preserved->account_id == "acct_stable",
         "OpenAI OAuth refresh preserves existing refresh token when response omits rotation");

  ava::tests::FakeTransport id_token_transport({ava::http::HttpResponse{
      .status_code = 200,
      .headers = {},
      .body = "{\"access_token\":\"id-token-access\",\"refresh_token\":\"id-token-refresh\","
              "\"expires_in\":120,\"id_token\":\"header."
              "eyJodHRwczovL2FwaS5vcGVuYWkuY29tL2F1dGgiOnsiY2hhdGdwdF9hY2NvdW50X2lkIjoiYWNjdF8xMjMifX0.sig\"}",
  }});
  auto const id_token_refreshed = ava::config::refresh_openai_oauth_credential(ava::config::OpenAICredential{.type = ava::config::OpenAICredentialType::OAuth,
                                                                                                             .access_token = "old-access",
                                                                                                             .refresh_token = "id-token-refresh-input",
                                                                                                             .expires_at = 900,
                                                                                                             .account_id = "",
                                                                                                             .source_path = {}},
                                                                               id_token_transport, 1000);
  expect(id_token_refreshed && id_token_refreshed->account_id == "acct_123",
         "OpenAI OAuth refresh falls back to id_token account id when account_id is absent");

  ava::tests::FakeTransport malformed_transport({ava::http::HttpResponse{
      .status_code = 200,
      .headers = {},
      .body = "not json",
  }});
  auto const malformed = ava::config::refresh_openai_oauth_credential(ava::config::OpenAICredential{.type = ava::config::OpenAICredentialType::OAuth,
                                                                                                    .access_token = "old-access",
                                                                                                    .refresh_token = "refresh",
                                                                                                    .expires_at = 900,
                                                                                                    .account_id = "",
                                                                                                    .source_path = {}},
                                                                      malformed_transport, 1000);
  expect(!malformed && malformed.error().message().find("malformed JSON") != std::string::npos,
         "OpenAI OAuth refresh reports malformed JSON responses clearly");

  auto const root = create_empty_root("oauth-refresh");

  setenv("HOME", (root / "home").c_str(), 1);
  setenv("XDG_CONFIG_HOME", (root / "config").c_str(), 1);
  setenv("XDG_STATE_HOME", (root / "state").c_str(), 1);
  setenv("XDG_DATA_HOME", (root / "data").c_str(), 1);
  auto const paths = ava::config::xdg_paths();
  auto stored = ava::config::store_openai_credential(paths, ava::config::OpenAICredential{.type = ava::config::OpenAICredentialType::OAuth,
                                                                                          .access_token = "expired-access",
                                                                                          .refresh_token = "persist-refresh",
                                                                                          .expires_at = 100,
                                                                                          .account_id = "acct_old",
                                                                                          .source_path = {}});
  expect(stored.has_value(), "OpenAI OAuth refresh persistence test stores expired credential");
  auto loaded = ava::config::load_openai_credential(paths);
  expect(loaded && loaded->has_value(), "OpenAI OAuth refresh persistence test loads expired credential");
  if (loaded && *loaded)
  {
    ava::tests::FakeTransport persist_transport({ava::http::HttpResponse{
        .status_code = 200,
        .headers = {},
        .body = "{\"access_token\":\"persisted-access\",\"refresh_token\":\"persisted-refresh\","
                "\"expires_in\":60,\"account_id\":\"acct_persisted\"}",
    }});
    auto usable = ava::config::openai_credential_for_request(paths, **loaded, persist_transport, 1000);
    expect(usable && usable->access_token == "persisted-access" && usable->refresh_token == "persisted-refresh" && usable->expires_at == 1060 &&
               usable->account_id == "acct_persisted",
           "OpenAI OAuth credential preflight refreshes expired credential before use");
    auto persisted = ava::config::load_openai_credential(paths);
    expect(persisted && persisted->has_value() && (*persisted)->access_token == "persisted-access" && (*persisted)->refresh_token == "persisted-refresh",
           "OpenAI OAuth credential preflight persists refreshed token rotation");
  }

  auto failure_store = ava::config::store_openai_credential(paths, ava::config::OpenAICredential{.type = ava::config::OpenAICredentialType::OAuth,
                                                                                                 .access_token = "failure-access",
                                                                                                 .refresh_token = "failure-refresh",
                                                                                                 .expires_at = 100,
                                                                                                 .account_id = "acct_failure",
                                                                                                 .source_path = {}});
  expect(failure_store.has_value(), "OpenAI OAuth refresh failure test stores expired credential");
  loaded = ava::config::load_openai_credential(paths);
  if (loaded && *loaded)
  {
    ava::tests::FakeTransport failure_transport({ava::http::HttpResponse{
        .status_code = 400,
        .headers = {},
        .body = "{\"error\":\"invalid_grant\"}",
    }});
    auto failed = ava::config::openai_credential_for_request(paths, **loaded, failure_transport, 1000);
    expect(!failed && failed.error().message().find("ava connect openai") != std::string::npos, "OpenAI OAuth refresh failure suggests reconnecting");
    auto unchanged = ava::config::load_openai_credential(paths);
    expect(unchanged && unchanged->has_value() && (*unchanged)->access_token == "failure-access" && (*unchanged)->refresh_token == "failure-refresh" &&
               (*unchanged)->expires_at == 100,
           "OpenAI OAuth refresh failure does not overwrite existing credential");
  }
}

void test_anthropic_oauth_request_resolution()
{
  auto const root = create_empty_root("anthropic-oauth-resolution");

  setenv("HOME", (root / "home").c_str(), 1);
  setenv("XDG_CONFIG_HOME", (root / "config").c_str(), 1);
  setenv("XDG_STATE_HOME", (root / "state").c_str(), 1);
  setenv("XDG_DATA_HOME", (root / "data").c_str(), 1);
  auto const paths = ava::config::xdg_paths();
  std::filesystem::create_directories(paths.auth_file.parent_path());

  {
    ScopedEnvVar const api_key("ANTHROPIC_API_KEY", "env-api-key");
    ScopedEnvVar const auth_token("ANTHROPIC_AUTH_TOKEN", "env-auth-token");
    ScopedEnvVar const oauth_token("ANTHROPIC_OAUTH_TOKEN", "env-oauth-token");
    ava::tests::FakeTransport env_transport({});
    auto credential = ava::config::provider_credential_for_request(paths, "anthropic", env_transport, 1000);
    expect(credential && credential->has_value() && (*credential)->provider_id == "anthropic" && (*credential)->access_token == "env-oauth-token" &&
               (*credential)->credential_type == "oauth" && (*credential)->source == "env:ANTHROPIC_OAUTH_TOKEN" && env_transport.requests().empty(),
           "Anthropic OAuth environment token takes precedence over API-key environment fallback");
  }

  {
    ScopedEnvVar const api_key("ANTHROPIC_API_KEY", "env-api-key");
    ScopedEnvVar const oauth_token("ANTHROPIC_OAUTH_TOKEN", "");
    ScopedEnvVar const auth_token("ANTHROPIC_AUTH_TOKEN", "env-auth-token");
    ava::tests::FakeTransport env_transport({});
    auto credential = ava::config::provider_credential_for_request(paths, "anthropic", env_transport, 1000);
    expect(credential && credential->has_value() && (*credential)->access_token == "env-auth-token" && (*credential)->credential_type == "oauth" &&
               (*credential)->source == "env:ANTHROPIC_AUTH_TOKEN" && env_transport.requests().empty(),
           "Anthropic AUTH token environment alias is used before API-key fallback when OAuth token env is absent");
  }

  {
    std::ofstream file(paths.auth_file, std::ios::binary | std::ios::trunc);
    file << "{\"anthropic\":{\"type\":\"oauth\",\"access_token\":\"stored-oauth\",";
    file << "\"refresh_token\":\"stored-refresh\",\"expires_at\":2000,";
    file << "\"account_id\":\"acct_anthropic\",\"source\":\"claude\"}}";
  }
  ::chmod(paths.auth_file.c_str(), S_IRUSR | S_IWUSR);
  ava::tests::FakeTransport stored_transport({});
  auto stored = ava::config::provider_credential_for_request(paths, "anthropic", stored_transport, 1000);
  expect(stored && stored->has_value() && (*stored)->access_token == "stored-oauth" && (*stored)->credential_type == "oauth" &&
             (*stored)->refresh_token == "stored-refresh" && (*stored)->expires_at == 2000 && (*stored)->account_id == "acct_anthropic" &&
             (*stored)->source == paths.auth_file.string() && (*stored)->source_metadata == "claude" && stored_transport.requests().empty(),
         "stored Anthropic OAuth auth.json credentials parse token, refresh, expiry, account, and source metadata");

  {
    std::ofstream file(paths.auth_file, std::ios::binary | std::ios::trunc);
    file << "{\"anthropic\":{\"type\":\"oauth\",\"access_token\":\"old-oauth\",";
    file << "\"refresh_token\":\"refresh value\",\"expires_at\":1200,";
    file << "\"account_id\":\"acct_old\",\"source\":\"claude\"}}";
  }
  ::chmod(paths.auth_file.c_str(), S_IRUSR | S_IWUSR);
  ava::tests::FakeTransport refresh_transport({ava::http::HttpResponse{
      .status_code = 200,
      .headers = {},
      .body = "{\"access_token\":\"refreshed-oauth\",\"refresh_token\":\"rotated-refresh\","
              "\"expires_in\":600,\"account_id\":\"acct_refreshed\"}",
  }});
  auto refreshed = ava::config::provider_credential_for_request(paths, "anthropic", refresh_transport, 1000);
  expect(refreshed && refreshed->has_value() && (*refreshed)->access_token == "refreshed-oauth" && (*refreshed)->refresh_token == "rotated-refresh" &&
             (*refreshed)->expires_at == 1600 && (*refreshed)->account_id == "acct_refreshed" && (*refreshed)->credential_type == "oauth" &&
             (*refreshed)->source_metadata == "claude",
         "near-expiry Anthropic OAuth credential refreshes before request use");
  auto const& refresh_requests = refresh_transport.requests();
  expect(
      refresh_requests.size() == 1 && refresh_requests.front().url == "https://platform.claude.com/v1/oauth/token" && refresh_requests.front().method == "POST",
      "Anthropic OAuth refresh posts to the Claude token endpoint");
  if (!refresh_requests.empty())
  {
    expect(refresh_requests.front().headers.at("Content-Type") == "application/json" && !refresh_requests.front().follow_redirects &&
               refresh_requests.front().body.find("\"grant_type\":\"refresh_token\"") != std::string::npos &&
               refresh_requests.front().body.find("\"client_id\":\"9d1c250a-e61b-44d9-88ed-5944d1962f5e\"") != std::string::npos &&
               refresh_requests.front().body.find("\"refresh_token\":\"refresh value\"") != std::string::npos,
           "Anthropic OAuth refresh sends non-redirecting JSON refresh-token body with compatibility client id");
  }
  struct stat refreshed_auth_stat{};
  expect(::stat(paths.auth_file.c_str(), &refreshed_auth_stat) == 0 && (refreshed_auth_stat.st_mode & (S_IRWXG | S_IRWXO)) == 0,
         "Anthropic OAuth refresh persists auth file with owner-only permissions");
  ava::tests::FakeTransport persisted_transport({});
  auto persisted = ava::config::provider_credential_for_request(paths, "anthropic", persisted_transport, 1000);
  expect(persisted && persisted->has_value() && (*persisted)->access_token == "refreshed-oauth" && (*persisted)->refresh_token == "rotated-refresh" &&
             (*persisted)->expires_at == 1600,
         "Anthropic OAuth refresh persists rotated credentials for later requests");

  {
    std::ofstream file(paths.auth_file, std::ios::binary | std::ios::trunc);
    file << "{\"anthropic\":{\"type\":\"oauth\",\"access_token\":\"expired-oauth\",\"expires_at\":900}}";
  }
  ::chmod(paths.auth_file.c_str(), S_IRUSR | S_IWUSR);
  ava::tests::FakeTransport expired_transport({});
  auto expired = ava::config::provider_credential_for_request(paths, "anthropic", expired_transport, 1000);
  expect(!expired && expired.error().category() == ava::core::ErrorCategory::PermissionDenied &&
             expired.error().format().find("remove the stored Anthropic entry") != std::string::npos && expired_transport.requests().empty(),
         "expired Anthropic OAuth credential without refresh token fails closed before provider use");

  {
    ScopedEnvVar const oauth_token("ANTHROPIC_OAUTH_TOKEN", "fresh-env-oauth");
    ava::tests::FakeTransport blocked_transport({});
    auto blocked = ava::config::provider_credential_for_request(paths, "anthropic", blocked_transport, 1000);
    expect(!blocked && blocked.error().category() == ava::core::ErrorCategory::PermissionDenied &&
               blocked.error().format().find("remove the stored Anthropic entry") != std::string::npos && blocked_transport.requests().empty(),
           "stored expired Anthropic OAuth credential fails closed instead of falling through to env OAuth token");
  }

  {
    std::ofstream file(paths.auth_file, std::ios::binary | std::ios::trunc);
    file << "{\"anthropic\":{\"type\":\"oauth\",\"refresh_token\":\"missing-access\"}}";
  }
  ::chmod(paths.auth_file.c_str(), S_IRUSR | S_IWUSR);
  {
    ScopedEnvVar const oauth_token("ANTHROPIC_OAUTH_TOKEN", "fresh-env-oauth");
    ava::tests::FakeTransport malformed_transport({});
    auto malformed = ava::config::provider_credential_for_request(paths, "anthropic", malformed_transport, 1000);
    expect(!malformed && malformed.error().category() == ava::core::ErrorCategory::InvalidArgument &&
               malformed.error().format().find("stored provider credential is malformed") != std::string::npos && malformed_transport.requests().empty(),
           "malformed stored Anthropic credential fails closed instead of falling through to env credentials");
  }

  {
    std::ofstream file(paths.auth_file, std::ios::binary | std::ios::trunc);
    file << "{\"anthropic\":\"not-an-object\"}";
  }
  ::chmod(paths.auth_file.c_str(), S_IRUSR | S_IWUSR);
  {
    ScopedEnvVar const oauth_token("ANTHROPIC_OAUTH_TOKEN", "fresh-env-oauth");
    ava::tests::FakeTransport non_object_transport({});
    auto non_object = ava::config::provider_credential_for_request(paths, "anthropic", non_object_transport, 1000);
    expect(!non_object && non_object.error().category() == ava::core::ErrorCategory::InvalidArgument &&
               non_object.error().format().find("stored provider credential is malformed") != std::string::npos && non_object_transport.requests().empty(),
           "non-object stored Anthropic credential fails closed instead of falling through to env credentials");
  }

  {
    std::ofstream file(paths.auth_file, std::ios::binary | std::ios::trunc);
    file << "{\"anthropic\":{\"type\":\"oauth\",\"access_token\":\"stored\",\"expires_at\":\"soon\"}}";
  }
  ::chmod(paths.auth_file.c_str(), S_IRUSR | S_IWUSR);
  {
    ScopedEnvVar const oauth_token("ANTHROPIC_OAUTH_TOKEN", "fresh-env-oauth");
    ava::tests::FakeTransport bad_expiry_transport({});
    auto bad_expiry = ava::config::provider_credential_for_request(paths, "anthropic", bad_expiry_transport, 1000);
    expect(!bad_expiry && bad_expiry.error().category() == ava::core::ErrorCategory::InvalidArgument &&
               bad_expiry.error().format().find("stored provider credential is malformed") != std::string::npos && bad_expiry_transport.requests().empty(),
           "wrong-type Anthropic OAuth expiry fails closed instead of disabling refresh or falling through to env");
  }

  {
    std::ofstream file(paths.auth_file, std::ios::binary | std::ios::trunc);
    file << "{\"kimi\":\"not-an-object\"}";
  }
  ::chmod(paths.auth_file.c_str(), S_IRUSR | S_IWUSR);
  {
    ScopedEnvVar const kimi_key("KIMI_API_KEY", "env-kimi-key");
    ava::tests::FakeTransport kimi_transport({});
    auto kimi = ava::config::provider_credential_for_request(paths, "kimi", kimi_transport, 1000);
    expect(kimi && kimi->has_value() && (*kimi)->access_token == "env-kimi-key" && (*kimi)->credential_type == "api_key" &&
               (*kimi)->source == "env:KIMI_API_KEY" && kimi_transport.requests().empty(),
           "malformed stored non-Anthropic provider entries preserve legacy environment fallback behavior");
  }
}

void test_builtin_provider_model_metadata_contracts()
{
  auto const builtin = ava::config::builtin_model_registry();
  auto provider_registry = ava::provider::builtin_provider_registry();

  std::map<std::string, ava::config::ProviderProfile> profiles;
  for (auto const& profile : ava::config::builtin_provider_profiles())
  {
    expect(!profile.provider_id.empty(), "builtin provider profiles have ids");
    auto const inserted = profiles.emplace(profile.provider_id, profile).second;
    expect(inserted, "builtin provider profile ids are unique");
    if (profile.runtime_selectable)
    {
      expect(provider_registry.contains(profile.provider_id), "runtime-selectable provider profile has a registered factory: " + profile.provider_id);
      auto provider = provider_registry.create(profile.provider_id);
      expect(provider && *provider, "runtime-selectable provider factory creates a provider: " + profile.provider_id);
    }
    else
    {
      expect(!provider_registry.contains(profile.provider_id), "connect-only provider profile is not registered as a runtime provider: " + profile.provider_id);
      expect(!profile.connect_detail.empty(), "connect-only provider profile keeps explicit credential guidance: " + profile.provider_id);
    }
  }

  std::map<std::string, std::set<std::string>> model_ids_by_provider;
  std::set<std::string> selectable_provider_ids;
  for (auto const& model : builtin.models)
  {
    expect(!model.provider_id.empty() && !model.model_id.empty(), "builtin models have provider and model ids");
    auto const profile = profiles.find(model.provider_id);
    expect(profile != profiles.end(), "builtin model provider has a provider profile: " + model.provider_id);
    expect(provider_registry.contains(model.provider_id), "builtin selectable model provider has a registered factory: " + model.provider_id);
    if (profile != profiles.end())
    {
      expect(profile->second.runtime_selectable, "builtin selectable model provider is marked runtime-selectable: " + model.provider_id);
      for (auto const& quirk : profile->second.default_compatibility_quirks)
      {
        expect(contains_string(model.compatibility_quirks, quirk),
               "builtin model inherits provider compatibility quirk " + quirk + ": " + model.provider_id + "/" + model.model_id);
      }
    }

    selectable_provider_ids.insert(model.provider_id);
    expect(model_ids_by_provider[model.provider_id].insert(model.model_id).second,
           "builtin model ids are unique per provider: " + model.provider_id + "/" + model.model_id);
    expect(!model.display_name.empty() && !model.family.empty(),
           "builtin model display and family metadata is populated: " + model.provider_id + "/" + model.model_id);
    expect(model.display_name_is_configured && ava::config::proven_configured_model_display_name(model) == model.display_name,
           "builtin model display name retains explicit catalog provenance: " + model.provider_id + "/" + model.model_id);
    // Cerebras/Fireworks context windows are account-tier dependent; the catalog
    // intentionally omits them rather than claiming a single value (Phase D).
    bool const context_may_be_unknown = model.provider_id == "cerebras" || model.provider_id == "fireworks";
    if (context_may_be_unknown)
    {
      expect(!model.context_window_tokens.has_value(),
             "tier-dependent builtin model omits context window rather than guessing: " + model.provider_id + "/" + model.model_id);
    }
    else
    {
      expect(model.context_window_tokens && *model.context_window_tokens > 0,
             "builtin model context window metadata is populated: " + model.provider_id + "/" + model.model_id);
    }
    expect(contains_string(model.input_modalities, "text") && contains_string(model.output_modalities, "text"),
           "builtin model modality metadata declares text input and output: " + model.provider_id + "/" + model.model_id);
    expect(model.supports_tools.has_value() && model.supports_streaming.has_value() && model.supports_reasoning.has_value() && model.reports_usage.has_value(),
           "builtin model capability booleans are explicitly populated: " + model.provider_id + "/" + model.model_id);
    if (model.supports_reasoning.value_or(false))
    {
      expect(!model.reasoning_levels.empty() && !model.reasoning_format.empty() && !ava::config::reasoning_parameter_text(model).empty(),
             "reasoning-capable builtin model declares levels, format, and request parameters: " + model.provider_id + "/" + model.model_id);
      for (auto const& level : model.reasoning_levels)
      {
        expect(ava::config::resolve_reasoning_level(model, level).supported,
               "reasoning-capable builtin model resolves advertised level: " + model.provider_id + "/" + model.model_id + "/" + level);
      }
    }
    else
    {
      expect(model.reasoning_levels.empty() && model.reasoning_format.empty(),
             "non-reasoning builtin model does not advertise reasoning levels or format: " + model.provider_id + "/" + model.model_id);
      auto const supported_reasoning = ava::config::supported_reasoning_levels(model);
      expect(supported_reasoning.size() == 1 && supported_reasoning.front() == "off",
             "non-reasoning builtin model resolves only off reasoning policy: " + model.provider_id + "/" + model.model_id);
    }
    if (model.pricing)
    {
      expect(model.pricing->input_per_million || model.pricing->output_per_million || model.pricing->cache_read_per_million ||
                 model.pricing->cache_write_per_million || model.pricing->reasoning_per_million,
             "builtin pricing metadata has at least one populated rate: " + model.provider_id + "/" + model.model_id);
    }
  }

  for (auto const& [provider_id, profile] : profiles)
  {
    bool const has_builtin_model = selectable_provider_ids.contains(provider_id);
    if (profile.runtime_selectable)
    {
      expect(has_builtin_model, "runtime-selectable provider has at least one builtin selectable model: " + provider_id);
    }
    else
    {
      expect(!has_builtin_model, "connect-only provider has no builtin selectable models: " + provider_id);
    }
  }

  auto const openai = ava::config::find_model(builtin, "openai", "gpt-5.5");
  expect(openai.has_value(), "OpenAI reasoning metadata fixture exists");
  if (openai)
  {
    expect(contains_string(openai->input_modalities, "image"), "OpenAI builtin model metadata declares image input for storage-backed serialization");
    expect(ava::config::provider_accepts_reasoning_format(*openai, "openai_responses") &&
               !ava::config::provider_accepts_reasoning_format(*openai, "reasoning_content") && !ava::config::provider_accepts_reasoning_format(*openai, ""),
           "OpenAI Responses metadata accepts only its exact native reasoning replay format");
    auto const openai_off = ava::config::resolve_reasoning_level(*openai, "off");
    auto const openai_minimal = ava::config::resolve_reasoning_level(*openai, "minimal");
    auto const openai_xhigh = ava::config::resolve_reasoning_level(*openai, "xhigh");
    expect(openai_off.explicit_mapping && openai_off.supported && openai_off.provider_level && *openai_off.provider_level == "none" &&
               openai_minimal.explicit_mapping && !openai_minimal.supported && openai_xhigh.explicit_mapping && openai_xhigh.provider_level &&
               *openai_xhigh.provider_level == "xhigh",
           "GPT-5.5 reasoning policy maps off to none, blocks minimal, and preserves xhigh");
    expect(ava::config::validate_reasoning_request(*openai, "high", std::nullopt, "").has_value(), "OpenAI reasoning accepts effort-only requests");
    expect(!ava::config::validate_reasoning_request(*openai, "high", std::optional<long long>(1024), ""), "OpenAI reasoning rejects budget tokens");
    expect(!ava::config::validate_reasoning_request(*openai, "high", std::nullopt, "summarized"), "OpenAI reasoning rejects display options");
  }

  auto verify_gpt56_model = [&builtin](std::string_view model_id, long double input_price, long double output_price, long double cache_read_price,
                                       long double cache_write_price) {
    auto const model = ava::config::find_model(builtin, "openai", model_id);
    expect(model.has_value(), "GPT-5.6 builtin model exists: " + std::string(model_id));
    if (!model)
      return;
    expect(model->context_window_tokens && *model->context_window_tokens == 272'000 && model->max_output_tokens && *model->max_output_tokens == 128'000 &&
               model->supports_reasoning.value_or(false) && contains_string(model->reasoning_levels, "minimal") &&
               contains_string(model->reasoning_levels, "max") && contains_string(model->input_modalities, "image") && model->pricing &&
               model->pricing->input_per_million == input_price && model->pricing->output_per_million == output_price &&
               model->pricing->cache_read_per_million == cache_read_price && model->pricing->cache_write_per_million == cache_write_price &&
               ava::config::provider_accepts_reasoning_format(*model, "openai_responses") &&
               !ava::config::provider_accepts_reasoning_format(*model, "anthropic_thinking"),
           "GPT-5.6 model carries short-context limits, modalities, pricing, and exact OpenAI Responses replay support: " + std::string(model_id));
    auto const off = ava::config::resolve_reasoning_level(*model, "off");
    auto const minimal = ava::config::resolve_reasoning_level(*model, "minimal");
    auto const xhigh = ava::config::resolve_reasoning_level(*model, "xhigh");
    auto const max = ava::config::resolve_reasoning_level(*model, "max");
    expect(off.supported && off.provider_level && *off.provider_level == "none" && minimal.supported && minimal.provider_level &&
               *minimal.provider_level == "low" && xhigh.supported && xhigh.provider_level && *xhigh.provider_level == "xhigh" && max.supported &&
               max.provider_level && *max.provider_level == "max",
           "GPT-5.6 reasoning maps off, minimal, xhigh, and max compatibly across API-key and ChatGPT OAuth routes: " + std::string(model_id));
  };
  verify_gpt56_model("gpt-5.6-sol", 5.0L, 30.0L, 0.5L, 6.25L);
  verify_gpt56_model("gpt-5.6-terra", 2.5L, 15.0L, 0.25L, 3.125L);
  verify_gpt56_model("gpt-5.6-luna", 1.0L, 6.0L, 0.1L, 1.25L);

  auto const anthropic = ava::config::find_model(builtin, "anthropic", "claude-sonnet-4-5");
  expect(anthropic.has_value(), "Anthropic reasoning metadata fixture exists");
  auto const anthropic_profile = profiles.find("anthropic");
  expect(anthropic_profile != profiles.end() && anthropic_profile->second.supports_oauth,
         "Anthropic provider profile advertises OAuth bearer credential support");
  if (anthropic)
  {
    expect(contains_string(anthropic->input_modalities, "image"), "Anthropic builtin model metadata declares image input for storage-backed serialization");
    expect(contains_string(anthropic->reasoning_levels, "enabled") && !contains_string(anthropic->reasoning_levels, "adaptive"),
           "Claude Sonnet 4.5 metadata narrows Anthropic thinking to enabled-only reasoning");
    auto const anthropic_enabled = ava::config::resolve_reasoning_level(*anthropic, "enabled");
    auto const anthropic_adaptive = ava::config::resolve_reasoning_level(*anthropic, "adaptive");
    auto const anthropic_levels = ava::config::supported_reasoning_levels(*anthropic);
    expect(anthropic_enabled.explicit_mapping && anthropic_enabled.supported && anthropic_enabled.provider_level &&
               *anthropic_enabled.provider_level == "enabled" && anthropic_adaptive.explicit_mapping && !anthropic_adaptive.supported &&
               contains_string(anthropic_levels, "enabled") && !contains_string(anthropic_levels, "adaptive"),
           "Anthropic Sonnet reasoning policy preserves enabled-only native thinking and blocks adaptive for this model");
    expect(ava::config::provider_accepts_reasoning_format(*anthropic, "anthropic_thinking"), "Anthropic provider accepts replay of native thinking blocks");
    expect(ava::config::validate_reasoning_request(*anthropic, "enabled", std::optional<long long>(2048), "summarized").has_value(),
           "Anthropic enabled reasoning accepts a valid budget and display");
    expect(!ava::config::validate_reasoning_request(*anthropic, "enabled", std::nullopt, ""), "Anthropic enabled reasoning requires a budget");
    expect(!ava::config::validate_reasoning_request(*anthropic, "enabled", std::optional<long long>(512), ""),
           "Anthropic enabled reasoning rejects budgets below the provider minimum");
    expect(!ava::config::validate_reasoning_request(*anthropic, "enabled", std::optional<long long>(64'000), ""),
           "Anthropic reasoning budget must stay below max output tokens");
    expect(!ava::config::validate_reasoning_request(*anthropic, "adaptive", std::optional<long long>(2048), ""),
           "Anthropic adaptive reasoning rejects fixed budgets");
    expect(!ava::config::validate_reasoning_request(*anthropic, "enabled", std::optional<long long>(2048), "full"),
           "Anthropic reasoning rejects unsupported display values");
  }

  auto const deepseek = ava::config::find_model(builtin, "deepseek", "deepseek-v4-flash");
  expect(deepseek.has_value(), "DeepSeek metadata fixture exists");
  if (deepseek)
  {
    expect(deepseek->api_family == "openai_chat_completions" && deepseek->context_window_tokens && *deepseek->context_window_tokens == 1'000'000 &&
               deepseek->supports_reasoning.value_or(false) && deepseek->reasoning_levels.size() == 2 && contains_string(deepseek->reasoning_levels, "high") &&
               contains_string(deepseek->reasoning_levels, "xhigh") && deepseek->reasoning_format == "reasoning_content" &&
               !ava::config::provider_accepts_reasoning_format(*deepseek, "reasoning_content"),
           "DeepSeek builtin model uses the compatible chat API and reasoning-effort controls without replaying reasoning_content");
    auto const deepseek_low = ava::config::resolve_reasoning_level(*deepseek, "low");
    auto const deepseek_high = ava::config::resolve_reasoning_level(*deepseek, "high");
    auto const deepseek_xhigh = ava::config::resolve_reasoning_level(*deepseek, "xhigh");
    expect(deepseek_low.explicit_mapping && !deepseek_low.supported && deepseek_high.explicit_mapping && deepseek_high.provider_level &&
               *deepseek_high.provider_level == "high" && deepseek_xhigh.explicit_mapping && deepseek_xhigh.provider_level &&
               *deepseek_xhigh.provider_level == "max",
           "DeepSeek V4 reasoning policy blocks low/medium tiers and maps xhigh to provider max");
  }

  auto const kimi = ava::config::find_model(builtin, "kimi", "kimi-k2-thinking");
  expect(kimi.has_value(), "Kimi reasoning metadata fixture exists");
  if (kimi)
  {
    expect(ava::config::provider_accepts_reasoning_format(*kimi, "reasoning_content"), "Kimi provider accepts replay of preserved reasoning_content blocks");
    expect(ava::config::validate_reasoning_request(*kimi, "enabled", std::nullopt, "").has_value(),
           "OpenAI-compatible reasoning accepts enabled level-only requests");
    expect(!ava::config::validate_reasoning_request(*kimi, "disabled", std::nullopt, ""),
           "OpenAI-compatible reasoning rejects disabled requests in favor of clear_reasoning");
    expect(!ava::config::validate_reasoning_request(*kimi, "enabled", std::optional<long long>(1024), ""), "OpenAI-compatible reasoning rejects budget tokens");
    expect(!ava::config::validate_reasoning_request(*kimi, "enabled", std::nullopt, "summarized"), "OpenAI-compatible reasoning rejects display values");
  }

  auto const moonshot = ava::config::find_model(builtin, "moonshot", "kimi-k2.6");
  expect(moonshot.has_value(), "Moonshot reasoning metadata fixture exists");
  if (moonshot)
  {
    expect(!ava::config::provider_accepts_reasoning_format(*moonshot, "reasoning_content"),
           "Moonshot metadata does not claim replay of reasoning_content without preserve_reasoning_content support");
  }

  auto const openrouter = ava::config::find_model(builtin, "openrouter", "moonshotai/kimi-k2.6");
  expect(openrouter.has_value(), "OpenRouter metadata fixture exists");
  if (openrouter)
  {
    auto const openrouter_off = ava::config::resolve_reasoning_level(*openrouter, "off");
    auto const openrouter_high = ava::config::resolve_reasoning_level(*openrouter, "high");
    expect(!openrouter->supports_reasoning.value_or(false) && !ava::config::provider_accepts_reasoning_format(*openrouter, "reasoning_content") &&
               openrouter_off.explicit_mapping && openrouter_off.supported && !openrouter_off.provider_level && openrouter_high.explicit_mapping &&
               !openrouter_high.supported,
           "OpenRouter Kimi metadata stays non-reasoning until OpenRouter-native reasoning is implemented");
  }

  auto const gemini_model = ava::config::find_model(builtin, "gemini", "gemini-2.5-pro");
  expect(gemini_model.has_value(), "Gemini metadata fixture exists");
  if (gemini_model)
  {
    auto const gemini_levels = ava::config::supported_reasoning_levels(*gemini_model);
    auto const gemini_high = ava::config::resolve_reasoning_level(*gemini_model, "high");
    expect(!gemini_model->supports_reasoning.value_or(false) && gemini_levels.size() == 1 && gemini_levels.front() == "off" && gemini_high.explicit_mapping &&
               !gemini_high.supported,
           "Gemini reasoning policy explicitly blocks active reasoning levels while preserving non-reasoning off state");
  }

  auto const deepseek_profile = ava::config::find_provider_profile("deepseek");
  auto const kimi_profile = ava::config::find_provider_profile("kimi");
  auto const moonshot_profile = ava::config::find_provider_profile("moonshot");
  auto const zai = ava::config::find_model(builtin, "zai", "glm-5.2");
  expect(zai.has_value(), "Z.AI glm-5.2 metadata fixture exists");
  if (zai)
  {
    expect(zai->api_family == "openai_chat_completions" && zai->context_window_tokens && *zai->context_window_tokens == 1'000'000 &&
               zai->supports_reasoning.value_or(false) && contains_string(zai->reasoning_levels, "minimal") &&
               contains_string(zai->reasoning_levels, "xhigh") && zai->reasoning_format == "reasoning_content" &&
               ava::config::provider_accepts_reasoning_format(*zai, "reasoning_content") && contains_string(zai->compatibility_quirks, "zai") &&
               contains_string(zai->compatibility_quirks, "max_completion_tokens") && contains_string(zai->compatibility_quirks, "tool_stream") &&
               contains_string(zai->compatibility_quirks, "zai_reasoning_effort"),
           "Z.AI glm-5.2 metadata carries Coding Plan limits, preserved reasoning, and effort/tool-stream quirks");
    auto const minimal = ava::config::resolve_reasoning_level(*zai, "minimal");
    auto const low = ava::config::resolve_reasoning_level(*zai, "low");
    auto const xhigh = ava::config::resolve_reasoning_level(*zai, "xhigh");
    expect(minimal.supported && !minimal.provider_level && low.supported && low.provider_level && *low.provider_level == "high" && xhigh.supported &&
               xhigh.provider_level && *xhigh.provider_level == "max",
           "Z.AI glm-5.2 reasoning maps minimal without effort and low/xhigh to high/max");
    expect(ava::config::validate_reasoning_request(*zai, "high", std::nullopt, "").has_value(), "Z.AI reasoning accepts level-only requests");
    expect(!ava::config::validate_reasoning_request(*zai, "disabled", std::nullopt, ""), "Z.AI reasoning rejects disabled in favor of clear_reasoning");
  }

  auto const zai_air = ava::config::find_model(builtin, "zai", "glm-4.5-air");
  expect(zai_air.has_value(), "Z.AI glm-4.5-air metadata fixture exists");
  if (zai_air)
  {
    expect(zai_air->context_window_tokens && *zai_air->context_window_tokens == 131'072 && zai_air->max_output_tokens &&
               *zai_air->max_output_tokens == 98'304 && contains_string(zai_air->compatibility_quirks, "max_completion_tokens") &&
               !contains_string(zai_air->compatibility_quirks, "tool_stream") && !contains_string(zai_air->compatibility_quirks, "zai_reasoning_effort") &&
               contains_string(zai_air->reasoning_levels, "enabled"),
           "Z.AI glm-4.5-air remains enabled-only without tool_stream or effort and still opts into max_completion_tokens");
  }

  auto const zai_vision = ava::config::find_model(builtin, "zai", "glm-5v-turbo");
  expect(zai_vision.has_value() && contains_string(zai_vision->input_modalities, "image"), "Z.AI glm-5v-turbo declares image input");

  auto const zai_cn = ava::config::find_model(builtin, "zai-coding-cn", "glm-4.7");
  expect(zai_cn.has_value() && zai_cn->context_window_tokens && *zai_cn->context_window_tokens == 204'800 &&
             contains_string(zai_cn->compatibility_quirks, "tool_stream") && contains_string(zai_cn->compatibility_quirks, "max_completion_tokens"),
         "Z.AI Coding CN glm-4.7 catalog entry mirrors Global Coding Plan metadata");
  expect(std::ranges::count_if(builtin.models, [](auto const& model) { return model.provider_id == "zai"; }) == 6 &&
             std::ranges::count_if(builtin.models, [](auto const& model) { return model.provider_id == "zai-coding-cn"; }) == 6,
         "both Z.AI providers expose the six Coding Plan models");

  auto const openrouter_profile = ava::config::find_provider_profile("openrouter");
  expect(deepseek_profile && deepseek_profile->default_base_url_env == "DEEPSEEK_BASE_URL" && deepseek_profile->chat_completions_path == "/chat/completions" &&
             deepseek_profile->reasoning_request_field == "reasoning_effort" && deepseek_profile->reasoning_request_effort_string &&
             deepseek_profile->include_stream_usage && contains_string(deepseek_profile->default_compatibility_quirks, "deepseek") &&
             contains_string(deepseek_profile->default_compatibility_quirks, "reasoning_content"),
         "DeepSeek profile records compatible endpoint, reasoning-effort request, and reasoning-content parser metadata");
  expect(kimi_profile && kimi_profile->include_stream_usage && kimi_profile->preserve_reasoning_content && kimi_profile->default_temperature == 1.0 &&
             contains_string(kimi_profile->default_compatibility_quirks, "temperature_1") &&
             contains_string(kimi_profile->default_compatibility_quirks, "preserve_reasoning_content"),
         "Kimi profile records request-builder quirks for stream usage, preserved reasoning, and temperature=1");
  expect(moonshot_profile && moonshot_profile->include_stream_usage && contains_string(moonshot_profile->default_compatibility_quirks, "moonshot") &&
             contains_string(moonshot_profile->default_compatibility_quirks, "reasoning_content"),
         "Moonshot profile records OpenAI-compatible stream-usage and reasoning-content quirks");
  expect(openrouter_profile && openrouter_profile->include_stream_usage &&
             contains_string(openrouter_profile->default_compatibility_quirks, "openai_compatible") &&
             contains_string(openrouter_profile->default_compatibility_quirks, "reasoning_content"),
         "OpenRouter profile records OpenAI-compatible stream-usage and reasoning-content quirks");

  auto const zai_profile = ava::config::find_provider_profile("zai");
  auto const zai_cn_profile = ava::config::find_provider_profile("zai-coding-cn");
  expect(zai_profile && zai_profile->display_name == "Z.AI Coding Plan (Global)" && zai_profile->default_base_url_env == "ZAI_BASE_URL" &&
             zai_profile->default_base_url == "https://api.z.ai/api/coding/paas/v4" && zai_profile->chat_completions_path == "/chat/completions" &&
             zai_profile->include_stream_usage && zai_profile->preserve_reasoning_content && zai_profile->runtime_selectable &&
             contains_string(zai_profile->default_compatibility_quirks, "zai") &&
             contains_string(zai_profile->default_compatibility_quirks, "preserve_reasoning_content"),
         "Z.AI Global profile records Coding Plan endpoint and preserved-reasoning quirks");
  expect(zai_cn_profile && zai_cn_profile->display_name == "Z.AI Coding Plan (China)" && zai_cn_profile->default_base_url_env == "ZAI_CODING_CN_BASE_URL" &&
             zai_cn_profile->default_base_url == "https://open.bigmodel.cn/api/coding/paas/v4" &&
             zai_cn_profile->chat_completions_path == "/chat/completions" && zai_cn_profile->include_stream_usage &&
             zai_cn_profile->preserve_reasoning_content && zai_cn_profile->runtime_selectable &&
             contains_string(zai_cn_profile->default_compatibility_quirks, "zai"),
         "Z.AI Coding CN profile records regional endpoint and preserved-reasoning quirks");
  {
    auto const root = create_empty_root("zai-auth-env");
    ScopedEnvVar home("HOME", (root / "home").string());
    ScopedEnvVar xdg_config("XDG_CONFIG_HOME", (root / "config").string());
    ScopedEnvVar xdg_state("XDG_STATE_HOME", (root / "state").string());
    ScopedEnvVar xdg_data("XDG_DATA_HOME", (root / "data").string());
    ScopedEnvVar zai_key("ZAI_API_KEY", "env-zai-key");
    ScopedEnvVar zai_cn_key("ZAI_CODING_CN_API_KEY", "env-zai-cn-key");
    ava::tests::FakeTransport transport({});
    auto const paths = ava::config::xdg_paths();
    auto zai_cred = ava::config::provider_credential_for_request(paths, "zai", transport);
    auto zai_cn_cred = ava::config::provider_credential_for_request(paths, "zai-coding-cn", transport);
    expect(zai_cred && zai_cred->has_value() && (*zai_cred)->access_token == "env-zai-key" && (*zai_cred)->source == "env:ZAI_API_KEY",
           "Z.AI auth env key derives as ZAI_API_KEY");
    expect(zai_cn_cred && zai_cn_cred->has_value() && (*zai_cn_cred)->access_token == "env-zai-cn-key" && (*zai_cn_cred)->source == "env:ZAI_CODING_CN_API_KEY",
           "Z.AI Coding CN auth env key derives as ZAI_CODING_CN_API_KEY");
  }
}

void test_model_and_prompt_config()
{
  auto const root = create_empty_root("model");
  ScopedEnvVar home("HOME", (root / "home").string());
  ScopedEnvVar config("XDG_CONFIG_HOME", (root / "config").string());
  ScopedEnvVar state("XDG_STATE_HOME", (root / "state").string());
  ScopedEnvVar data("XDG_DATA_HOME", (root / "data").string());
  auto const paths = ava::config::xdg_paths();

  auto const builtin = ava::config::builtin_model_registry();
  auto selected = ava::config::select_default_model(builtin);
  auto const openai_profile = ava::config::find_provider_profile("openai");
  expect(openai_profile && openai_profile->display_name == "OpenAI" && openai_profile->api_family == selected.api_family && openai_profile->supports_oauth &&
             openai_profile->runtime_selectable,
         "provider profile centralizes OpenAI display, API family, and OAuth capability");
  expect(ava::config::model_display_label("gpt-5.5") == "GPT-5.5" && ava::config::model_display_label("gpt-5.6-sol") == "GPT-5.6 Sol" &&
             ava::config::model_display_label("gpt-5.6-terra") == "GPT-5.6 Terra" && ava::config::model_display_label("gpt-5.6-luna") == "GPT-5.6 Luna",
         "model profile centralizes current GPT display labels");
  auto const vercel_profile = ava::config::find_provider_profile("vercel");
  expect(vercel_profile && vercel_profile->display_name == "Vercel AI Gateway" && vercel_profile->connect_detail == "API key" &&
             !vercel_profile->runtime_selectable,
         "provider profile centralizes explicit connect-only gateway metadata");
  expect(selected.provider_id == "openai" && selected.model_id == "gpt-5.5", "default model is OpenAI GPT-5.5");
  expect(selected.context_window_tokens && *selected.context_window_tokens == 272'000 && selected.max_output_tokens && *selected.max_output_tokens == 128'000 &&
             selected.pricing && selected.pricing->input_per_million && *selected.pricing->input_per_million == 5.0L,
         "default model carries current context, output, and pricing metadata");
  expect(selected.supports_reasoning.value_or(false) && selected.reasoning_format == "openai_responses" &&
             std::find(selected.reasoning_levels.begin(), selected.reasoning_levels.end(), "xhigh") != selected.reasoning_levels.end(),
         "default GPT-5.5 model declares OpenAI reasoning effort levels including xhigh");
  bool saw_priced_builtin = false;
  bool saw_anthropic_builtin_reasoning_enabled = false;
  bool saw_deepseek_builtin = false;
  bool saw_deepseek_pro_pricing = false;
  bool saw_kimi_builtin = false;
  bool saw_moonshot_builtin = false;
  bool saw_openrouter_builtin_without_reasoning = false;
  bool saw_zai_builtin = false;
  bool saw_zai_cn_builtin = false;
  bool all_builtins_have_known_or_explicitly_omitted_context = !builtin.models.empty();
  bool all_builtins_have_text_output = !builtin.models.empty();
  for (auto const& model : builtin.models)
  {
    bool const context_may_be_unknown = model.provider_id == "cerebras" || model.provider_id == "fireworks";
    all_builtins_have_known_or_explicitly_omitted_context =
        all_builtins_have_known_or_explicitly_omitted_context && (context_may_be_unknown || model.context_window_tokens.has_value());
    all_builtins_have_text_output =
        all_builtins_have_text_output && std::find(model.output_modalities.begin(), model.output_modalities.end(), "text") != model.output_modalities.end();
    saw_priced_builtin =
        saw_priced_builtin || (model.model_id == "gpt-4.1-mini" && model.context_window_tokens && model.pricing && *model.context_window_tokens == 1'047'576 &&
                               model.pricing->input_per_million && model.pricing->output_per_million && model.api_family == "openai_responses" &&
                               model.supports_tools.value_or(false) && model.supports_streaming.value_or(false) && model.reports_usage.value_or(false));
    expect(ava::config::find_provider_profile(model.provider_id).has_value(), "each builtin model references a centralized provider profile");
    saw_anthropic_builtin_reasoning_enabled =
        saw_anthropic_builtin_reasoning_enabled ||
        (model.provider_id == "anthropic" && model.model_id == "claude-sonnet-4-5" && model.supports_reasoning.value_or(false) &&
         model.reasoning_format == "anthropic_thinking" && model.reasoning_levels.size() == 1 && model.reasoning_levels[0] == "enabled" &&
         std::find(model.reasoning_levels.begin(), model.reasoning_levels.end(), "adaptive") == model.reasoning_levels.end() &&
         std::find(model.compatibility_quirks.begin(), model.compatibility_quirks.end(), "anthropic_messages") != model.compatibility_quirks.end());
    saw_deepseek_builtin = saw_deepseek_builtin ||
                           (model.provider_id == "deepseek" && model.model_id == "deepseek-v4-flash" && model.api_family == "openai_chat_completions" &&
                            model.context_window_tokens && *model.context_window_tokens == 1'000'000 && model.max_output_tokens &&
                            *model.max_output_tokens == 384'000 && model.pricing && model.pricing->input_per_million && model.pricing->output_per_million &&
                            model.pricing->cache_read_per_million && model.supports_tools.value_or(false) && model.supports_streaming.value_or(false) &&
                            model.supports_reasoning.value_or(false) && model.reasoning_levels.size() == 2 && model.reasoning_format == "reasoning_content" &&
                            std::find(model.compatibility_quirks.begin(), model.compatibility_quirks.end(), "deepseek") != model.compatibility_quirks.end());
    saw_deepseek_pro_pricing = saw_deepseek_pro_pricing || (model.provider_id == "deepseek" && model.model_id == "deepseek-v4-pro" && model.pricing &&
                                                            model.pricing->input_per_million && *model.pricing->input_per_million == 0.435L &&
                                                            model.pricing->output_per_million && *model.pricing->output_per_million == 0.87L &&
                                                            model.pricing->cache_read_per_million && *model.pricing->cache_read_per_million == 0.003625L);
    saw_kimi_builtin =
        saw_kimi_builtin ||
        (model.provider_id == "kimi" && model.model_id == "kimi-k2-thinking" && model.api_family == "openai_chat_completions" &&
         model.supports_tools.value_or(false) && model.supports_reasoning.value_or(false) && model.reasoning_format == "reasoning_content" && model.pricing &&
         model.pricing->input_per_million && *model.pricing->input_per_million == 0.0L && model.pricing->output_per_million &&
         *model.pricing->output_per_million == 0.0L &&
         std::find(model.compatibility_quirks.begin(), model.compatibility_quirks.end(), "kimi") != model.compatibility_quirks.end() &&
         std::find(model.compatibility_quirks.begin(), model.compatibility_quirks.end(), "preserve_reasoning_content") != model.compatibility_quirks.end());
    saw_moonshot_builtin =
        saw_moonshot_builtin || (model.provider_id == "moonshot" && model.model_id == "kimi-k2.6" && model.api_family == "openai_chat_completions" &&
                                 model.reasoning_format == "reasoning_content" && model.max_output_tokens && *model.max_output_tokens == 262'144 &&
                                 model.pricing && model.pricing->input_per_million && *model.pricing->input_per_million == 0.95L &&
                                 model.pricing->output_per_million && *model.pricing->output_per_million == 4.0L);
    saw_openrouter_builtin_without_reasoning =
        saw_openrouter_builtin_without_reasoning ||
        (model.provider_id == "openrouter" && model.model_id == "moonshotai/kimi-k2.6" && model.api_family == "openai_chat_completions" &&
         model.max_output_tokens && *model.max_output_tokens == 262'144 && !model.supports_reasoning.value_or(false) && model.reasoning_levels.empty() &&
         model.reasoning_format.empty());
    saw_zai_builtin = saw_zai_builtin || (model.provider_id == "zai" && model.model_id == "glm-5.2" && model.api_family == "openai_chat_completions" &&
                                          model.supports_reasoning.value_or(false) &&
                                          std::find(model.compatibility_quirks.begin(), model.compatibility_quirks.end(), "zai_reasoning_effort") !=
                                              model.compatibility_quirks.end());
    saw_zai_cn_builtin = saw_zai_cn_builtin || (model.provider_id == "zai-coding-cn" && model.model_id == "glm-4.5-air" &&
                                                model.api_family == "openai_chat_completions" && model.supports_reasoning.value_or(false));
  }
  expect(all_builtins_have_known_or_explicitly_omitted_context,
         "builtin model registry provides context windows except documented tier-dependent omissions");
  expect(all_builtins_have_text_output, "builtin model registry always declares text output support");
  expect(saw_priced_builtin, "builtin model registry carries static pricing, context, and capability metadata");
  expect(saw_anthropic_builtin_reasoning_enabled, "Anthropic builtin exposes enabled-only native thinking reasoning metadata");
  expect(saw_deepseek_builtin && saw_deepseek_pro_pricing && saw_kimi_builtin && saw_moonshot_builtin,
         "builtin model registry includes DeepSeek, Kimi, and Moonshot OpenAI-compatible coding profiles");
  expect(saw_openrouter_builtin_without_reasoning, "builtin OpenRouter profile does not advertise reasoning until OpenRouter-native reasoning is implemented");
  expect(saw_zai_builtin, "builtin catalog includes Z.AI glm-5.2 with effort quirk");
  expect(saw_zai_cn_builtin, "builtin catalog includes Z.AI Coding CN glm-4.5-air");
  expect(ava::config::reasoning_parameter_text(selected) == openai_profile->reasoning_request_parameters,
         "model reasoning parameter text comes from centralized provider/reasoning profiles");

  std::filesystem::create_directories(paths.ava_config_dir);
  {
    std::ofstream file(paths.models_file, std::ios::binary | std::ios::trunc);
    file << "{\"default_provider\":\"openai\",\"default_model\":\"gpt-5.5-mini\","
            "\"models\":[{\"provider\":\"openai\",\"id\":\"gpt-5.5-mini\",\"name\":\"Mini\","
            "\"family\":\"gpt-5\",\"context_window_tokens\":12345,\"max_output_tokens\":678,"
            "\"api_family\":\"openai_responses\",\"input_modalities\":[\"text\",\"image\"],"
            "\"output_modalities\":[\"text\"],\"reasoning_format\":\"reasoning_content\","
            "\"supports_tools\":false,\"supports_streaming\":true,\"supports_reasoning\":true,"
            "\"reports_usage\":true,\"reasoning_levels\":[\"low\",\"high\"],"
            "\"reasoning_level_map\":{\"off\":\"none\",\"minimal\":null,\"low\":\"low\",\"high\":\"max\","
            "\"ultra\":\"turbo\",\"future\":true,\"typo\":123},"
            "\"compatibility_quirks\":[\"requires_strict_tools\"],"
            "\"pricing\":{\"input_per_million\":1.5,\"output_per_million\":2.5}}]}";
  }
  auto registry = ava::config::load_model_registry(paths);
  expect(registry.has_value(), "model registry loads XDG override");
  if (registry)
  {
    selected = ava::config::select_default_model(*registry);
    expect(selected.model_id == "gpt-5.5-mini" && selected.display_name == "Mini", "default model override selects user model");
    ava::provider::TokenUsage const usage{.input_tokens = 1000,
                                          .output_tokens = 2000,
                                          .reasoning_tokens = std::nullopt,
                                          .cache_read_tokens = std::nullopt,
                                          .cache_write_tokens = std::nullopt,
                                          .total_tokens = 3000,
                                          .estimated_input_bytes = std::nullopt,
                                          .estimated_output_bytes = std::nullopt,
                                          .estimated_total_bytes = std::nullopt,
                                          .estimated = false};
    auto cost = selected.pricing ? ava::agent::usage_cost_usd(*selected.pricing, usage) : std::nullopt;
    auto const cost_value = cost.value_or(0.0L);
    auto const custom_off = ava::config::resolve_reasoning_level(selected, "off");
    auto const custom_minimal = ava::config::resolve_reasoning_level(selected, "minimal");
    auto const custom_high = ava::config::resolve_reasoning_level(selected, "high");
    auto const custom_ultra = ava::config::resolve_reasoning_level(selected, "ultra");
    auto const custom_future = ava::config::resolve_reasoning_level(selected, "future");
    auto const custom_typo = ava::config::resolve_reasoning_level(selected, "typo");
    expect(selected.context_window_tokens == 12345 && selected.max_output_tokens == 678 && selected.api_family == "openai_responses" &&
               selected.input_modalities.size() == 2 && selected.output_modalities.size() == 1 && selected.output_modalities[0] == "text" &&
               selected.reasoning_format == "reasoning_content" && selected.supports_tools == false && selected.supports_streaming == true &&
               selected.supports_reasoning == true && selected.reports_usage == true && selected.reasoning_levels.size() == 2 &&
               selected.compatibility_quirks.size() == 1 && cost && cost_value > 0.0064L && cost_value < 0.0066L && custom_off.provider_level &&
               *custom_off.provider_level == "none" && custom_minimal.explicit_mapping && !custom_minimal.supported && custom_high.provider_level &&
               *custom_high.provider_level == "max" && custom_ultra.explicit_mapping && custom_ultra.supported && custom_ultra.provider_level &&
               *custom_ultra.provider_level == "turbo" && custom_future.explicit_mapping && custom_future.supported && !custom_future.provider_level &&
               custom_typo.explicit_mapping && !custom_typo.supported,
           "model registry parses local capability, reasoning-level policy, and pricing metadata and calculates cost");
    expect(!ava::agent::usage_cost_usd(ava::config::ModelPricing{}, usage), "usage cost remains unknown when pricing rates are absent");
    auto const maximum_price = std::numeric_limits<long double>::max();
    ava::config::ModelPricing const maximum_pricing{.input_per_million = maximum_price,
                                                    .output_per_million = maximum_price,
                                                    .cache_read_per_million = maximum_price,
                                                    .cache_write_per_million = maximum_price,
                                                    .reasoning_per_million = maximum_price};
    ava::provider::TokenUsage const maximum_usage{.input_tokens = LLONG_MAX,
                                                  .output_tokens = LLONG_MAX,
                                                  .reasoning_tokens = LLONG_MAX,
                                                  .cache_read_tokens = LLONG_MAX,
                                                  .cache_write_tokens = LLONG_MAX,
                                                  .total_tokens = LLONG_MAX,
                                                  .estimated_input_bytes = std::nullopt,
                                                  .estimated_output_bytes = std::nullopt,
                                                  .estimated_total_bytes = std::nullopt,
                                                  .estimated = false};
    auto const maximum_cost = ava::agent::usage_cost_usd(maximum_pricing, maximum_usage);
    expect(maximum_cost && *maximum_cost == maximum_price, "usage cost calculation saturates overflowing token-price components and totals");
    ava::provider::TokenUsage const cached_usage{.input_tokens = 1000,
                                                 .output_tokens = 0,
                                                 .reasoning_tokens = std::nullopt,
                                                 .cache_read_tokens = 100,
                                                 .cache_write_tokens = std::nullopt,
                                                 .total_tokens = 1000,
                                                 .estimated_input_bytes = std::nullopt,
                                                 .estimated_output_bytes = std::nullopt,
                                                 .estimated_total_bytes = std::nullopt,
                                                 .estimated = false};
    expect(!ava::agent::usage_cost_usd(*selected.pricing, cached_usage), "usage cost remains unknown when present cache usage has no cache pricing");
  }

  auto const provenance_registry = ava::config::parse_model_registry(
      R"JSON({"default_provider":"custom","default_model":"unnamed","models":[
        {"provider":"custom","id":"named","name":"Human Name"},
        {"provider":"custom","id":"unnamed"},
        {"provider":"custom","id":"same","name":"same"}
      ]})JSON");
  auto const named_custom = ava::config::find_model(provenance_registry, "custom", "named");
  auto const unnamed_custom = ava::config::find_model(provenance_registry, "custom", "unnamed");
  auto const same_custom = ava::config::find_model(provenance_registry, "custom", "same");
  auto const unresolved_default =
      ava::config::select_default_model(ava::config::ModelRegistry{.default_provider_id = "custom", .default_model_id = "missing", .models = {}});
  expect(named_custom && ava::config::proven_configured_model_display_name(*named_custom) == "Human Name" && same_custom &&
             ava::config::proven_configured_model_display_name(*same_custom) == "same",
         "explicit custom display names retain provenance, including a configured name equal to the model id");
  expect(unnamed_custom && unnamed_custom->display_name == "unnamed" && ava::config::proven_configured_model_display_name(*unnamed_custom).empty() &&
             unresolved_default.display_name == "missing" && ava::config::proven_configured_model_display_name(unresolved_default).empty(),
         "unnamed custom and unresolved models keep ordinary ID fallback labels but expose no proven launch display name");

  {
    std::ofstream file(paths.models_file, std::ios::binary | std::ios::trunc);
    file << "{\"default_provider\":\"openai\",\"default_model\":\"gpt-5.5\","
            "\"models\":[{\"provider\":\"openai\",\"id\":\"gpt-5.5\",\"name\":\"Custom GPT\"}]}";
  }
  registry = ava::config::load_model_registry(paths);
  expect(registry.has_value(), "model registry loads partial builtin override");
  if (registry)
  {
    selected = ava::config::select_default_model(*registry);
    auto const preserved_off = ava::config::resolve_reasoning_level(selected, "off");
    expect(selected.display_name == "Custom GPT" && selected.context_window_tokens == 272'000 && selected.max_output_tokens == 128'000 && selected.pricing &&
               selected.pricing->input_per_million && *selected.pricing->input_per_million == 5.0L && selected.supports_reasoning == true &&
               selected.reports_usage == true && selected.reasoning_levels.size() == 4 && selected.output_modalities.size() == 1 &&
               selected.output_modalities[0] == "text" && selected.reasoning_format == "openai_responses" && preserved_off.provider_level &&
               *preserved_off.provider_level == "none",
           "builtin model overrides preserve missing capability and reasoning-level policy metadata");
  }

  auto saved_scope = ava::config::store_scoped_model_cycle(paths, std::vector<std::string>{"openai/gpt-5.5", "anthropic/claude-sonnet-4-5"});
  expect(saved_scope.has_value(), "model config stores scoped model cycle");
  registry = ava::config::load_model_registry(paths);
  expect(registry && registry->scoped_model_cycle && registry->scoped_model_cycle->size() == 2 && (*registry->scoped_model_cycle)[0] == "openai/gpt-5.5" &&
             (*registry->scoped_model_cycle)[1] == "anthropic/claude-sonnet-4-5",
         "model registry loads persisted scoped model cycle order");
  if (registry)
  {
    selected = ava::config::select_default_model(*registry);
    expect(selected.display_name == "Custom GPT", "scoped model cycle save preserves existing custom model entries");
  }

  auto saved_empty_scope = ava::config::store_scoped_model_cycle(paths, std::vector<std::string>{});
  expect(saved_empty_scope.has_value(), "model config stores explicit empty scoped model cycle");
  registry = ava::config::load_model_registry(paths);
  expect(registry && registry->scoped_model_cycle && registry->scoped_model_cycle->empty(), "model registry preserves explicit empty scoped model cycle");

  auto saved_default_scope = ava::config::store_scoped_model_cycle(paths, std::nullopt);
  expect(saved_default_scope.has_value(), "model config removes scoped model cycle for default all-model cycling");
  registry = ava::config::load_model_registry(paths);
  expect(registry && !registry->scoped_model_cycle, "model registry treats a missing scoped model cycle as all registered models enabled");
  std::ifstream persisted_model_config(paths.models_file, std::ios::binary);
  auto const persisted_model_config_bytes = std::string(std::istreambuf_iterator<char>(persisted_model_config), std::istreambuf_iterator<char>());
  expect(persisted_model_config_bytes.find("display_name_is_configured") == std::string::npos,
         "process-local display-name provenance never alters the public model config schema");

  std::error_code remove_error;
  std::filesystem::remove(paths.models_file, remove_error);
  auto saved_new_scope = ava::config::store_scoped_model_cycle(paths, std::vector<std::string>{"openai/gpt-5.5"});
  expect(saved_new_scope.has_value(), "model config creates models.json for scoped model cycle persistence");
  registry = ava::config::load_model_registry(paths);
  expect(registry && registry->scoped_model_cycle && registry->scoped_model_cycle->size() == 1 && registry->scoped_model_cycle->front() == "openai/gpt-5.5",
         "model registry loads scoped model cycle from generated models.json");

  auto inferred_family =
      ava::config::select_default_model(ava::config::ModelRegistry{.default_provider_id = "openai", .default_model_id = "gpt-5.5", .models = {}});
  expect(inferred_family.family == "gpt-5", "GPT-5.5 model id infers GPT-5 prompt family");

  auto prompt = ava::config::select_prompt(paths, selected, ava::agent::Mode::Build);
  expect(prompt && !prompt->from_override && !prompt->source_path && prompt->text.find("Provider=openai") != std::string::npos &&
             prompt->text.find("Do not narrate or restate routine successful tool calls") != std::string::npos,
         "builtin prompt selects by provider and family with concise no-routine-success-narration guidance");
  std::filesystem::create_directories(paths.prompts_dir / "openai" / "gpt-5");
  {
    std::ofstream file(paths.prompts_dir / "openai" / "gpt-5" / "plan.txt", std::ios::binary | std::ios::trunc);
    file << "custom plan prompt";
  }
  auto override = ava::config::select_prompt(paths, selected, ava::agent::Mode::Plan);
  auto const override_path = paths.prompts_dir / "openai" / "gpt-5" / "plan.txt";
  expect(override && override->from_override && override->text == "custom plan prompt" && override->source_path && *override->source_path == override_path,
         "prompt override loads from XDG config and records its source path");

  {
    std::ofstream file(paths.models_file, std::ios::binary | std::ios::trunc);
    file << std::string((1024 * 1024) + 1, 'x');
  }
  auto oversized_registry = ava::config::load_model_registry(paths);
  expect(!oversized_registry, "oversized model config is rejected");

  {
    std::ofstream file(paths.prompts_dir / "openai" / "gpt-5" / "plan.txt", std::ios::binary | std::ios::trunc);
    file << std::string((256 * 1024) + 1, 'x');
  }
  auto oversized_prompt = ava::config::select_prompt(paths, selected, ava::agent::Mode::Plan);
  expect(!oversized_prompt, "oversized prompt override is rejected");
}

void test_oauth_loopback_callback()
{
  for (std::string_view const scenario : {"ignore-invalid-then-accept", "provider-error", "cancel", "deadline"})
  {
    auto const child = ava::tests::run_in_child_with_syscall_errno(std::span<ava::tests::SyscallErrno const>{}, [&] {
      auto const initial_failures = ava::tests::failures();
      std::promise<std::uint16_t> listening;
      auto ready = listening.get_future();
      std::atomic<bool> cancel = false;
      std::optional<ava::core::Result<ava::app::OAuthCallbackResult>> result;
      auto const deadline = ava::tests::now_plus_seconds(3);
      ava::app::detail::OAuthCallbackOptions const options{.port = 0,
                                                           .deadline = scenario == "deadline" ? std::chrono::steady_clock::time_point::min() : deadline,
                                                           .listener_ready = [&](std::uint16_t port) { listening.set_value(port); }};
      std::thread listener([&] { result = ava::app::detail::wait_for_oauth_callback("expected-state", [&] { return cancel.load(); }, options); });
      bool const listening_ready = ready.wait_until(deadline) == std::future_status::ready;
      expect(listening_ready, "OAuth callback listener publishes its ephemeral port");
      bool requests_succeeded = listening_ready;
      if (listening_ready)
      {
        auto const port = ready.get();
        auto send_request = [&](std::string_view request) {
          ava::core::UniqueFd client(::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0));
          if (!ava::tests::require(static_cast<bool>(client), "OAuth callback client socket opens"))
            return false;
          sockaddr_in address{};
          address.sin_family = AF_INET;
          address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
          address.sin_port = htons(port);
          if (!ava::tests::require(::connect(client.get(), reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0,
                                   "OAuth callback client connects to the ready listener"))
            return false;
          std::size_t offset = 0;
          while (offset < request.size())
          {
            auto const written = ::send(client.get(), request.data() + offset, request.size() - offset, MSG_NOSIGNAL);
            if (written < 0 && errno == EINTR)
              continue;
            if (!ava::tests::require(written > 0, "OAuth callback client sends its complete request"))
              return false;
            offset += static_cast<std::size_t>(written);
          }
          std::string response;
          return ava::tests::require(ava::tests::read_until(client.get(), response, "\r\n\r\n", deadline),
                                     "OAuth callback sends an HTTP response for each received request");
        };
        if (scenario == "ignore-invalid-then-accept")
        {
          for (std::string_view const request :
               {"POST /auth/callback?code=wrong&state=expected-state HTTP/1.1\r\n\r\n", "GET /auth/callback?code=wrong&state=expected-state\r\n\r\n",
                "GET /other?code=wrong&state=expected-state HTTP/1.1\r\n\r\n", "GET /auth/callback HTTP/1.1\r\n\r\n",
                "GET /auth/callback?state=expected-state HTTP/1.1\r\n\r\n", "GET /auth/callback?code=wrong HTTP/1.1\r\n\r\n",
                "GET /auth/callback?code=&state=expected-state HTTP/1.1\r\n\r\n", "GET /auth/callback?code=wrong&state=wrong-state HTTP/1.1\r\n\r\n",
                "GET /auth/callback?error=access_denied&state=wrong-state HTTP/1.1\r\n\r\n",
                "GET /auth/callback?code=accepted%2Bcode&state=expected-state HTTP/1.1\r\n\r\n"})
          {
            if (!send_request(request))
            {
              requests_succeeded = false;
              break;
            }
          }
        }
        else if (scenario == "provider-error")
          requests_succeeded = send_request("GET /auth/callback?error=access_denied&error_description=user+declined&state=expected-state HTTP/1.1\r\n\r\n");
        else if (scenario == "cancel")
          cancel.store(true);
      }
      if (!requests_succeeded)
        cancel.store(true);
      listener.join();
      if (!ava::tests::require(result.has_value(), "OAuth callback listener returns a result"))
        return 1;
      if (scenario == "ignore-invalid-then-accept")
      {
        if (ava::tests::require(result->has_value(), "invalid callbacks do not consume the eventual valid authorization"))
          ava::tests::expect_eq((*result)->code, std::string("accepted+code"), "OAuth callback returns only the valid decoded authorization code");
      }
      else if (ava::tests::require(!result->has_value(), "OAuth rejection, cancellation, or expired deadline returns an error"))
      {
        auto const& error = result->error();
        auto const category = scenario == "provider-error" ? ava::core::ErrorCategory::Provider
                              : scenario == "cancel"       ? ava::core::ErrorCategory::InvalidArgument
                                                           : ava::core::ErrorCategory::Io;
        ava::tests::expect_eq(error.category(), category, "OAuth callback failure has the expected stable category");
        if (scenario == "provider-error")
          ava::tests::expect_all(
              "OAuth provider callback retains structured rejection context",
              {{"error", std::ranges::any_of(error.context(), [](auto const& item) { return item.key == "error" && item.value == "access_denied"; })},
               {"description",
                std::ranges::any_of(error.context(), [](auto const& item) { return item.key == "description" && item.value == "user declined"; })}});
      }
      return ava::tests::failures() == initial_failures ? 0 : 1;
    });
    ava::tests::expect_child_success(child, "loopback OAuth callback: " + std::string(scenario));
  }
}

void test_auth_creation_and_replacement_ignore_ambient_umask()
{
  for (mode_t const mask : {mode_t{0000}, mode_t{0077}})
  {
    ava::tests::HermeticTestEnvironment environment("auth-umask-");
    auto const status = ava::tests::run_in_child_with_syscall_errno(std::span<ava::tests::SyscallErrno const>{}, [&] {
      static_cast<void>(::umask(mask));
      auto const paths = ava::config::xdg_paths();
      ava::config::OpenAICredential credential{.type = ava::config::OpenAICredentialType::ApiKey,
                                               .access_token = "first-token",
                                               .refresh_token = "",
                                               .expires_at = 0,
                                               .account_id = "",
                                               .source_path = {}};
      for (std::string_view const token : {"first-token", "replacement-token"})
      {
        credential.access_token = token;
        auto const stored = ava::config::store_openai_credential(paths, credential);
        if (!stored)
          return 1;
        struct stat file_status{};
        struct stat directory_status{};
        if (::stat(paths.auth_file.c_str(), &file_status) != 0 || ::stat(paths.auth_file.parent_path().c_str(), &directory_status) != 0 ||
            (file_status.st_mode & 0777) != 0600 || (directory_status.st_mode & 0777) != 0700 || file_status.st_nlink != 1)
          return 2;
        auto const loaded = ava::config::load_openai_credential(paths);
        if (!loaded || !*loaded || (*loaded)->access_token != token)
          return 3;
      }
      return 0;
    });
    ava::tests::expect_child_success(status, "auth creation and replacement remain private under umask " + std::to_string(mask));
  }
}

}  // namespace

void run_config_context_auth_oauth_tests()
{
  ava::tests::HermeticTestEnvironment environment("config-auth-oauth-");
  test_xdg_paths();
  test_context_loader();
  test_markdown_resource_parser_and_reader();
  test_skill_loader();
  test_auth_record_helpers();
  test_auth_load_and_store();
  test_auth_creation_and_replacement_ignore_ambient_umask();
  test_openai_oauth_helpers();
  test_oauth_loopback_callback();
  test_openai_oauth_refresh();
  test_anthropic_oauth_request_resolution();
  test_builtin_provider_model_metadata_contracts();
  test_model_and_prompt_config();
}

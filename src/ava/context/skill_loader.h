#pragma once

#include "ava/debug/print_members_on.h"
#include "ava/core/NoLogString.h"
#include "ava/core/result.h"

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace ava::config {
struct XdgPaths;
}  // namespace ava::config

namespace ava::context {

enum class SkillSourceType
{
  Global,
  Project,
  Plugin,
};

struct LoadedSkill
{
  std::string name;
  ava::core::NoLogString description;
  std::filesystem::path path;
  std::filesystem::path directory;
  SkillSourceType source_type = SkillSourceType::Project;
  std::size_t byte_count = 0;
  ava::core::NoLogString content;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

struct SkillDiagnostic
{
  std::filesystem::path path;
  ava::core::NoLogString message;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

struct DeclaredSkillFileOptions
{
  std::filesystem::path path;
  std::string name;
  ava::core::NoLogString description;
  SkillSourceType source_type = SkillSourceType::Project;
  std::optional<ava::core::NoLogString> preloaded_content;
  std::size_t max_file_bytes = 64 * 1024;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

struct SkillLoadOptions
{
  std::filesystem::path workspace_root;
  std::vector<std::filesystem::path> global_skill_dirs = {};
  std::vector<std::filesystem::path> project_skill_dirs = {};
  std::vector<DeclaredSkillFileOptions> declared_skill_files = {};
  std::size_t max_file_bytes = 64 * 1024;
  bool include_global_skills = true;
  bool include_project_skills = true;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

struct SkillLoadResult
{
  std::vector<LoadedSkill> skills;
  std::vector<SkillDiagnostic> diagnostics;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

[[nodiscard]] std::string to_string(SkillSourceType source_type);
[[nodiscard]] std::vector<std::filesystem::path> default_global_skill_dirs();
[[nodiscard]] std::vector<std::filesystem::path> default_global_skill_dirs(ava::config::XdgPaths const& paths);
[[nodiscard]] std::vector<std::filesystem::path> default_project_skill_dirs(std::filesystem::path const& workspace_root);
[[nodiscard]] ava::core::Result<LoadedSkill> load_declared_skill_file(DeclaredSkillFileOptions options);
[[nodiscard]] ava::core::Result<LoadedSkill> load_declared_skill_content(DeclaredSkillFileOptions options, std::string content);
[[nodiscard]] SkillLoadResult load_skills(SkillLoadOptions options);
[[nodiscard]] std::string format_available_skills_for_prompt(std::vector<LoadedSkill> const& skills);
[[nodiscard]] std::string format_loaded_skill_for_tool(LoadedSkill const& skill, std::vector<std::filesystem::path> const& sampled_files);
[[nodiscard]] std::vector<std::filesystem::path> sample_skill_files(std::filesystem::path const& skill_dir, std::size_t max_files = 10);

}  // namespace ava::context

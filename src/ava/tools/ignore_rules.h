#pragma once

#include "ava/debug/print_members_on.h"
#include "ava/core/result.h"

#include <filesystem>
#include <memory>
#include <regex>
#include <string>
#include <string_view>
#include <vector>

namespace ava::tools {

class SecureWorkspace;

[[nodiscard]] bool is_git_dir(std::filesystem::path const& path);
[[nodiscard]] bool is_generated_dir(std::filesystem::path const& path);

class IgnoreMatcher
{
 private:
  struct Rule
  {
    std::string base_relative;
    std::string pattern;
    std::regex matcher;
    bool negated = false;
    bool directory_only = false;
    bool anchored = false;
    bool contains_slash = false;

    AVA_DEBUG_PRINT_MEMBERS_ON
  };

 public:
  [[nodiscard]] static ava::core::Result<IgnoreMatcher> load(std::filesystem::path const& workspace_dir,
                                                             std::shared_ptr<SecureWorkspace> secure_workspace = nullptr);

  [[nodiscard]] bool ignored(std::filesystem::path const& path, bool is_directory) const;

  AVA_DEBUG_PRINT_MEMBERS_ON

 private:
  explicit IgnoreMatcher(std::filesystem::path workspace_dir, std::shared_ptr<SecureWorkspace> secure_workspace);

  [[nodiscard]] static bool rule_matches(Rule const& rule, std::string_view relative_to_base, bool is_directory);
  [[nodiscard]] ava::core::Result<void> load_rules();
  [[nodiscard]] ava::core::Result<void> load_file(std::filesystem::path const& ignore_file);
  [[nodiscard]] std::string relative_to_workspace(std::filesystem::path const& path) const;

  std::filesystem::path workspace_dir_;
  std::shared_ptr<SecureWorkspace> secure_workspace_;
  std::vector<Rule> rules_;
};

}  // namespace ava::tools

#pragma once

#include "ava/debug/print_members_on.h"
#include "ava/config/xdg_paths.h"
#include "ava/core/NoLogString.h"
#include "ava/core/result.h"

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace ava::app {

enum class ProjectTrustDecision
{
  Unknown,
  Trusted,
  Denied,
};

struct ProjectTrustResource
{
  std::string kind;
  std::filesystem::path path;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

struct ProjectTrustState
{
  std::filesystem::path workspace_dir;
  std::filesystem::path trust_file;
  ProjectTrustDecision decision = ProjectTrustDecision::Unknown;
  std::filesystem::path matched_path;
  std::vector<ProjectTrustResource> protected_resources;
  ava::core::NoLogString diagnostic;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

[[nodiscard]] std::string_view to_string(ProjectTrustDecision decision);
[[nodiscard]] bool project_resources_trusted(ProjectTrustState const& state);
[[nodiscard]] std::filesystem::path project_trust_file(ava::config::XdgPaths const& paths);
[[nodiscard]] ProjectTrustState load_project_trust_state(ava::config::XdgPaths const& paths, std::filesystem::path const& workspace_dir);

// Move-only read/modify/write staging for a manager-serialized trust
// transaction. Staging computes the effective post-mutation state without
// changing disk; commit performs the one atomic persistence step. This object
// itself provides no singleton or cross-process serialization.
class StagedProjectTrustMutation
{
 public:
  StagedProjectTrustMutation() = delete;
  ~StagedProjectTrustMutation();
  StagedProjectTrustMutation(StagedProjectTrustMutation&& other) noexcept;
  StagedProjectTrustMutation& operator=(StagedProjectTrustMutation&& other) noexcept;
  StagedProjectTrustMutation(StagedProjectTrustMutation const&) = delete;
  StagedProjectTrustMutation& operator=(StagedProjectTrustMutation const&) = delete;

  [[nodiscard]] ProjectTrustState const& effective_state() const;
  [[nodiscard]] ava::core::VoidResult commit();

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT

 private:
  struct Impl;
  explicit StagedProjectTrustMutation(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
  friend ava::core::Result<StagedProjectTrustMutation> stage_set_project_trust_decision(ava::config::XdgPaths const&, std::filesystem::path const&, bool);
  friend ava::core::Result<StagedProjectTrustMutation> stage_clear_project_trust_decision(ava::config::XdgPaths const&, std::filesystem::path const&);
};

[[nodiscard]] ava::core::Result<StagedProjectTrustMutation> stage_set_project_trust_decision(ava::config::XdgPaths const& paths,
                                                                                             std::filesystem::path const& workspace_dir, bool trusted);
[[nodiscard]] ava::core::Result<StagedProjectTrustMutation> stage_clear_project_trust_decision(ava::config::XdgPaths const& paths,
                                                                                               std::filesystem::path const& workspace_dir);
[[nodiscard]] ava::core::VoidResult set_project_trust_decision(ava::config::XdgPaths const& paths, std::filesystem::path const& workspace_dir, bool trusted);
[[nodiscard]] ava::core::VoidResult clear_project_trust_decision(ava::config::XdgPaths const& paths, std::filesystem::path const& workspace_dir);

}  // namespace ava::app

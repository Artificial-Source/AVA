#pragma once

// Internal lease-bound live inspection source. Include only from agent turn /
// coordinator implementation and tests that construct a source. App/TUI code
// must include subagent_inspector.h (path-free DTOs) only.

#include "ava/agent/subagent_inspector.h"
#include "ava/debug/print_members_on.h"
#include "ava/session/session_store.h"
#include "ava/core/result.h"

#include <memory>
#include <string>
#include <vector>

namespace ava::agent {

class SubagentCoordinator;

[[nodiscard]] inline ava::session::SessionReadLimits subagent_inspector_read_limits() noexcept
{
  return ava::session::SessionReadLimits{
      .max_file_bytes = kSubagentInspectorMaxFileBytes,
      .max_line_bytes = kSubagentInspectorMaxLineBytes,
      .max_entries = kSubagentInspectorMaxEntries,
  };
}

// Opaque lease-bound inspection source. Constructed from a COPY of an existing
// child SessionReadAuthority so no pathname or prompt escapes agent internals.
// Session types stay inside this internal header; public DTOs remain path-free.
class SubagentLiveInspectionSource final
{
 public:
  ~SubagentLiveInspectionSource();

  SubagentLiveInspectionSource(SubagentLiveInspectionSource const&) = delete;
  SubagentLiveInspectionSource& operator=(SubagentLiveInspectionSource const&) = delete;
  SubagentLiveInspectionSource(SubagentLiveInspectionSource&&) = delete;
  SubagentLiveInspectionSource& operator=(SubagentLiveInspectionSource&&) = delete;

  [[nodiscard]] static ava::core::Result<std::shared_ptr<SubagentLiveInspectionSource>> create(ava::session::SessionReadAuthority authority);

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT

 private:
  friend class SubagentCoordinator;

  struct Impl;
  explicit SubagentLiveInspectionSource(std::unique_ptr<Impl> impl);

  // Return an owned identity because the authority's wrapped store options are guard-protected.
  [[nodiscard]] std::string session_id() const;
  [[nodiscard]] bool is_ephemeral() const noexcept;
  // Persistent: exact fstat(2) of the owned lease FD. Ephemeral: append-only
  // in-memory tip summary under the store entries lock. The coordinator rejects
  // ephemeral sources before publication; inspectors never serve them.
  [[nodiscard]] ava::core::Result<ava::session::SessionContentFingerprint> content_fingerprint() const;
  // Strict capped load: clamps inspector request limits under the authority
  // policy, uses the owned lease FD only, and never repairs torn tails.
  [[nodiscard]] ava::core::Result<std::vector<ava::session::SessionEntry>> load_strict_capped() const;
  // Projects committed user/assistant text only. Caller stamps generation and
  // terminal/freeze metadata after validating source epoch identity.
  [[nodiscard]] ava::core::Result<std::vector<SubagentLiveMessage>> project_messages() const;

  std::unique_ptr<Impl> impl_;
};

}  // namespace ava::agent

#pragma once

#include "ava/debug/print_members_on.h"
#include "ava/process/scope.h"
#include "ava/session/session_store.h"

#include <memory>
#include <optional>

namespace ava::agent {
class SubagentCoordinator;
} // namespace ava::agent

namespace ava::core {
class AnchorSet;
} // namespace ava::core

namespace ava::diagnostics {
class RuntimeDiagnostics;
} // namespace ava::diagnostics

namespace ava::mcp {
struct McpConfig;
} // namespace ava::mcp

namespace ava::provider {
class ProviderCatalog;
} // namespace ava::provider

namespace ava::app {
class SessionRunController;
class SessionTitleCoordinator;
class SubagentDeliveryManager;
} // namespace ava::app

namespace ava::app::runtime {

// Session resources.
//
// Process-held live infrastructure bound to this process: the cross-process
// lease, pre-opened anchor descriptors, the run controller and append target,
// the bound read authority for detached capsules, and the shared
// application-scoped services (subagent coordinator/delivery, title
// coordinator, diagnostics, MCP config). These cannot round-trip through
// disk; the three app-scoped services are rebound by Session::replace_with
// and mcp_config is set once at ACP session setup.
struct SessionResources
{
  // Persistent runtime owners hold a cross-process lease for the complete session lifetime.
  ava::session::SessionLease lease;
  // One generated session owner, when explicit application process authority
  // was supplied. Copies representing this same runtime session share it.
  std::optional<ava::process::ProcessScopeV1> session_process_scope = std::nullopt;
  // Pre-opened anchor descriptors for all writable directories. Opened once
  // at session creation and shared across all prompts and subagent loops.
  std::shared_ptr<ava::core::AnchorSet> anchor_set = nullptr;
  std::shared_ptr<SessionRunController> run_controller = nullptr;
  // Immutable append target bound into the controller-owned serialized routes.
  // Tests and retained runtime capsules may inspect/copy it, but mutations flow
  // through the controller so append failures remain latched.
  std::shared_ptr<ava::session::SessionAppendTarget> append_target = nullptr;
  // Detached retained capsules bind reads to the exact original leased inode
  // without reacquiring the pathname. Ordinary visible sessions leave this
  // empty and derive an authority from their owned lease.
  std::optional<ava::session::SessionReadAuthority> bound_read_authority = std::nullopt;
  // Shared application process state. Direct coordinator injection remains a
  // compatibility seam; production sessions use the delivery manager.
  std::shared_ptr<ava::agent::SubagentCoordinator> subagent_coordinator = nullptr;
  std::shared_ptr<ava::app::SubagentDeliveryManager> subagent_delivery_manager = nullptr;
  std::shared_ptr<ava::app::SessionTitleCoordinator> session_title_coordinator = nullptr;
  std::shared_ptr<ava::diagnostics::RuntimeDiagnostics> diagnostics = nullptr;
  // Null uses normal global/project discovery; non-null is immutable session-local MCP composition.
  std::shared_ptr<ava::mcp::McpConfig const> mcp_config = nullptr;
  // Exact application-scoped provider catalog pinned for this session's lifetime.
  std::shared_ptr<ava::provider::ProviderCatalog const> provider_catalog = nullptr;

  // Exchange the complete process-held resource bundle without releasing either session's owners during replacement.
  void swap(SessionResources& other);

  AVA_DEBUG_PRINT_MEMBERS_ON
};

} // namespace ava::app::runtime

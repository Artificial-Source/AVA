#pragma once
#include "ava/http/transport.h"
#include "ava/app/headless_policy.h"
#include "ava/app/rpc/input.h"
#include "ava/app/rpc/protocol.h"
#include "ava/app/runtime.h"
#include "ava/app/runtime/session_ts.h"
#include "ava/provider/provider.h"
#include "ava/core/result.h"

#include <cstddef>
#include <iosfwd>

namespace ava::app {

struct RpcModeOptions
{
  runtime::OpenContext open_context;
  runtime::SessionLifecycleRequest lifecycle_request;
  HeadlessPermissionPolicyOptions permission_policy;
  bool parallel_read_search_tools = false;
  std::size_t parallel_read_search_max_workers = 4;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

[[nodiscard]] ava::core::VoidResult run_rpc_loop(runtime::session_ts& session, runtime::OpenContext const& open_context,
                                                 ava::provider::Provider const& provider, ava::http::Transport& transport, runtime::RunOptions runtime_options,
                                                 std::istream& in, std::ostream& out, rpc::RpcInputWake wake);
[[nodiscard]] ava::core::VoidResult run_rpc_loop(runtime::session_ts& session, runtime::OpenContext const& open_context,
                                                 ava::provider::Provider const& provider, ava::http::Transport& transport, ava::http::Transport& auth_transport,
                                                 runtime::RunOptions runtime_options, std::istream& in, std::ostream& out, rpc::RpcInputWake wake);

// Finite streams only. Supply an explicit wake callback for any stream that can block.
[[deprecated("finite streams only; pass an explicit rpc::RpcInputWake for blocking input")]] [[nodiscard]] ava::core::VoidResult run_rpc_loop(
    runtime::session_ts& session, runtime::OpenContext const& open_context, ava::provider::Provider const& provider, ava::http::Transport& transport,
    runtime::RunOptions runtime_options, std::istream& in, std::ostream& out);
// Finite streams only. Supply an explicit wake callback for any stream that can block.
[[deprecated("finite streams only; pass an explicit rpc::RpcInputWake for blocking input")]] [[nodiscard]] ava::core::VoidResult run_rpc_loop(
    runtime::session_ts& session, runtime::OpenContext const& open_context, ava::provider::Provider const& provider, ava::http::Transport& transport,
    ava::http::Transport& auth_transport, runtime::RunOptions runtime_options, std::istream& in, std::ostream& out);
[[nodiscard]] ava::core::VoidResult run_rpc_loop(runtime::session_ts& session, runtime::OpenContext const& open_context,
                                                 ava::provider::Provider const& provider, ava::http::Transport& transport, ava::http::Transport& auth_transport,
                                                 runtime::RunOptions runtime_options, rpc::RpcLineReader& input, std::ostream& out);

[[nodiscard]] int run_rpc_mode(RpcModeOptions const& options, std::istream& in, std::ostream& out, std::ostream& err, rpc::RpcInputWake wake);

// Finite streams only. Supply an explicit wake callback for any stream that can block.
[[deprecated("finite streams only; pass an explicit rpc::RpcInputWake for blocking input")]] [[nodiscard]] int run_rpc_mode(RpcModeOptions const& options,
                                                                                                                            std::istream& in, std::ostream& out,
                                                                                                                            std::ostream& err);

}  // namespace ava::app

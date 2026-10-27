#pragma once
#include "output.h"
#include "resolvers.h"
#include "run_state.h"
#include "ava/http/transport.h"
#include "ava/app/runtime.h"
#include "ava/session/attachments.h"
#include "ava/provider/provider.h"
#include "ava/core/NoLogString.h"
#include "ava/core/thread.h"

#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace ava::app::rpc {

struct RpcPromptWorkerOptions
{
  runtime::session_ts& unlocked_session;
  output_ts& output;                                    // Reference to threadsafe output stream.
  RpcRunState& run_state;
  PendingResolverState& pending_state;
  ava::provider::Provider const& injected_provider;
  std::string injected_provider_id;
  ava::http::Transport& transport;
  ava::http::Transport& auth_transport;
  runtime::RunOptions runtime_options;
  ava::config::XdgPaths paths;
  std::string request_id;
  ava::core::NoLogString message;
  std::vector<ava::session::ImageAttachmentRef> image_attachments = {};

  // Owns runtime credential-bearing options and non-owning process state.
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

[[nodiscard]] ava::core::JoinThread make_rpc_prompt_worker(RpcPromptWorkerOptions options);

}  // namespace ava::app::rpc

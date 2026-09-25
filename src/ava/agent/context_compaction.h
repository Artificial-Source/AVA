#pragma once

#include "ava/debug/print_members_on.h"
#include "ava/http/transport.h"
#include "ava/agent/model_invocation_options.h"
#include "ava/session/compaction.h"
#include "ava/session/session_store.h"
#include "ava/core/result.h"

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ava::provider {
class Provider;
}

namespace ava::agent {

struct PreparedCompactionContext
{
  std::vector<ava::session::SessionEntry> active_entries;
  std::string recent_context;
  std::size_t estimated_tokens = 0;
  std::size_t retained_tokens = 0;
  bool recent_context_omitted = false;

  // Contains raw projected conversation content; never expose it through generated diagnostics.
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

struct ChildContextCompactionBlueprint
{
  ava::session::CompactionConfig config;
  std::optional<long long> context_window_tokens = std::nullopt;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

struct ChildContextCompactionBinding
{
  ChildContextCompactionBlueprint blueprint;
  // Shared ownership is required because a promoted/background child loop can
  // outlive the parent invocation that created its exact leased append target.
  std::shared_ptr<ava::session::SessionAppendTarget> append_target;

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

struct ContextCompactionInvocation
{
  ModelInvocationOptions model;
  std::string access_token;
  std::string credential_type = "bearer";
  bool openai_oauth = false;
  std::string openai_account_id;
  std::function<bool()> cancel_requested = nullptr;

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

enum class CompactionTransactionPhase
{
  Started,
  Committed,
  SnapshotRetry,
};

struct CompactionTransactionStats
{
  std::size_t attempt = 0;
  std::size_t max_attempts = 2;
  std::size_t estimated_tokens = 0;
  std::size_t threshold_tokens = 0;
  std::size_t retained_tokens = 0;
  std::size_t post_compaction_tokens = 0;
  std::size_t summary_bytes = 0;
  std::size_t snapshot_entries = 0;
  std::size_t current_entries = 0;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

// All callbacks and their arguments are borrowed only for the synchronous
// transaction call. Each adapter retains its own exact append authority.
struct CompactionTransactionAdapters
{
  std::function<ava::core::Result<std::string>(std::vector<ava::session::SessionEntry> const&, std::size_t)> summarize;
  std::function<ava::core::Result<ava::session::SessionCompactionAppendResult>(ava::session::SessionEntry, std::vector<ava::session::SessionEntry>)> append;
  std::function<ava::core::VoidResult(CompactionTransactionPhase, CompactionTransactionStats const&)> event = nullptr;

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

[[nodiscard]] ava::core::Result<bool> compact_context_transaction(ava::session::SessionReadAuthority const& read_authority,
                                                                  ava::session::CompactionConfig const& config, std::optional<long long> context_window_tokens,
                                                                  std::string_view trigger, std::vector<std::string> const& replayed_user_messages,
                                                                  std::function<bool()> const& cancel_requested, CompactionTransactionAdapters const& adapters);

[[nodiscard]] ava::core::Result<PreparedCompactionContext> prepare_compaction_context(std::vector<ava::session::SessionEntry> const& entries,
                                                                                      ava::session::CompactionConfig const& config,
                                                                                      std::vector<std::string> const& replayed_user_messages = {});
[[nodiscard]] ava::core::Result<std::string> build_compaction_summary_prompt(std::vector<ava::session::SessionEntry> const& entries,
                                                                             ava::session::CompactionConfig const& config, std::string_view instructions,
                                                                             std::size_t estimated_tokens);
[[nodiscard]] ava::core::Result<std::string> generate_context_compaction_summary(std::vector<ava::session::SessionEntry> const& entries,
                                                                                 ava::session::CompactionConfig const& config, std::string_view instructions,
                                                                                 std::size_t estimated_tokens, ava::provider::Provider const& provider,
                                                                                 ava::http::Transport& transport,
                                                                                 ContextCompactionInvocation const& invocation);
[[nodiscard]] ava::core::Result<bool> compact_child_context(ChildContextCompactionBinding const& binding, std::string_view trigger,
                                                            std::vector<std::string> const& replayed_user_messages, ava::provider::Provider const& provider,
                                                            ava::http::Transport& transport, ContextCompactionInvocation const& invocation);

}  // namespace ava::agent

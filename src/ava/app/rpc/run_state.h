#pragma once

#include "ava/debug/print_members_on.h"
#include "ava/core/result.h"
#include "threadsafe/threadsafe.h"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <ostream>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ava::app::rpc {

enum class RpcInputTerminalOutcome;

// Forward declaration.
class Output;

// Output sink for RPC JSONL records.
using output_ts = threadsafe::Unlocked<Output, threadsafe::policy::Primitive<std::mutex>>;

enum class OutputWriteResult
{
  Written,
  Skipped,
};

class Output
{
 private:
  std::ostream& out_;                           // The stream to write new-line delimited JSON records to.
  std::function<void()> on_write_failure_;      // Called when the stream goes bad.

 public:
  // Construct an Output object for writing to `out`.
  //
  // Note: `on_write_failure` must not attempt to write to this same Output, even though
  // the lock on this object was dropped (because it touches other mutexes).
  explicit Output(std::ostream& out, std::function<void()> on_write_failure) : out_(out), on_write_failure_(std::move(on_write_failure)) { }

  // Write one JSONL record to the stream and flush.
  //
  // `record` must be a a complete JSONL record not containing any new-lines, also not a trailing new-line.
  //
  // Calls on_write_failure_ (if set) and returns an IO error when the stream is left in a bad state.
  [[nodiscard]] static ava::core::VoidResult write_record(output_ts& output, std::string_view record);

  // Evaluate `gate` and, only when it returns true, write one record while the output lock is held.
  // The gate must not retain the output lock or perform stream I/O. Failure callbacks are invoked after
  // releasing the output lock.
  [[nodiscard]] static ava::core::Result<OutputWriteResult> write_record_if(output_ts& output, std::string_view record, std::function<bool()> const& gate);

  AVA_DEBUG_PRINT_MEMBERS_ON
};

struct QueuedRpcMessage
{
  std::string request_id;
  std::string correlation_id;
  std::string message;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

struct ClearedRpcQueues
{
  std::vector<QueuedRpcMessage> steering_messages;
  std::vector<QueuedRpcMessage> follow_up_messages;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

enum class RpcFollowUpTransitionKind
{
  Skipped,
  Activated,
  Deactivated,
};

struct RpcFollowUpTransition
{
  RpcFollowUpTransitionKind kind = RpcFollowUpTransitionKind::Deactivated;
  std::optional<QueuedRpcMessage> follow_up;
  ClearedRpcQueues cleared;
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

struct RpcCancellation
{
  bool active_run = false;
  std::string active_request_id;
  ClearedRpcQueues cleared;
  std::size_t deferred_steering_count = 0;
  std::size_t deferred_follow_up_count = 0;
  bool deferred_to_terminal_publication = false;
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

enum class RpcRunKind
{
  None,
  Prompt,
  DirectCommand,
  Compaction,
};

struct RpcRunState
{
  std::mutex mutex;
  std::condition_variable publication_cv;
  std::atomic_bool cancel_requested = false;
  bool terminal_publication_in_progress = false;
  RpcRunKind active_run_kind = RpcRunKind::None;
  bool input_terminal_observed = false;
  bool input_closed = false;
  std::string active_request_id;
  std::set<std::string> outstanding_request_ids;
  std::deque<QueuedRpcMessage> steering_messages;
  std::deque<QueuedRpcMessage> follow_up_messages;
  std::optional<ava::core::Error> async_error;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

[[nodiscard]] ava::core::Error canceled_error();
[[nodiscard]] ava::core::Error skipped_follow_up_error(std::string_view reason);
[[nodiscard]] ava::core::Error active_run_reject_error(std::string_view command_type);
[[nodiscard]] ava::core::Error duplicate_request_id_error(std::string_view request_id);

[[nodiscard]] bool cancel_requested(RpcRunState& state);
[[nodiscard]] bool active_run(RpcRunState& state);
[[nodiscard]] bool active_prompt_run(RpcRunState& state);
[[nodiscard]] bool async_worker_reap_ready(RpcRunState& state);
[[nodiscard]] bool input_closed(RpcRunState& state);
void observe_input_terminal(RpcRunState& state, RpcInputTerminalOutcome outcome);
enum class RpcCommandAdmission
{
  Admitted,
  DuplicateRequestId,
  InputClosed,
};

[[nodiscard]] RpcCommandAdmission await_command_admission(RpcRunState& state, std::string_view request_id, bool wait_for_terminal_publication = true);
[[nodiscard]] bool outstanding_request_id(RpcRunState& state, std::string_view request_id);
void set_active_run(RpcRunState& state, RpcRunKind kind, std::string request_id = {});
void set_active_request_id(RpcRunState& state, std::string request_id);
void complete_outstanding_request(RpcRunState& state, std::string_view request_id);

[[nodiscard]] ava::core::Result<QueuedRpcMessage> queue_rpc_message(std::deque<QueuedRpcMessage>& queue, RpcRunState& state, std::string command_type,
                                                                    std::string request_id, std::string message);
[[nodiscard]] std::vector<QueuedRpcMessage> take_queued_steering_messages(RpcRunState& state, std::string_view correlation_id);
void begin_prompt_terminal_publication(RpcRunState& state);
[[nodiscard]] RpcFollowUpTransition transition_after_prompt_terminal_response(RpcRunState& state);
[[nodiscard]] ClearedRpcQueues begin_terminal_publication(RpcRunState& state);
void complete_terminal_publication(RpcRunState& state, std::string_view request_id);
[[nodiscard]] RpcCancellation begin_cancellation(RpcRunState& state);
void wait_for_terminal_publication(RpcRunState& state);
[[nodiscard]] std::vector<QueuedRpcMessage> clear_queued_steering_messages(RpcRunState& state);
[[nodiscard]] ClearedRpcQueues deactivate_and_clear_queued_messages(RpcRunState& state);
[[nodiscard]] ClearedRpcQueues close_input_and_cancel(RpcRunState& state);

void record_async_error(RpcRunState& state, ava::core::Error error);
[[nodiscard]] std::optional<ava::core::Error> take_async_error(RpcRunState& state);

}  // namespace ava::app::rpc

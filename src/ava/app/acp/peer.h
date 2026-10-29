#pragma once

#include "ava/debug/print_members_on.h"
#include "ava/app/acp/protocol.h"
#include "ava/app/acp/transport.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <future>
#include <memory>
#include <stop_token>
#include <string>
#include <string_view>

namespace ava::app::acp {

using RequestResult = std::expected<std::string, JsonRpcError>;
// Inbound stop callbacks run synchronously without the peer's in-flight mutex held.
using RequestHandler = std::function<RequestResult(Request const&, std::stop_token)>;
using NotificationHandler = std::function<void(Notification const&, std::stop_token)>;
// Reader-thread control notifications must be bounded and must not perform
// provider or other blocking work.
using ControlNotificationHandler = std::function<void(Notification const&)>;
// Called on the sole reader after an inbound id is reserved and before worker
// scheduling. The returned callback rolls back an unstarted reservation.
using RequestPreAdmissionHook = std::function<std::expected<std::function<void()>, JsonRpcError>(Request const&)>;
using DiagnosticSink = std::function<void(std::string_view)>;
using CallResult = std::expected<std::string, JsonRpcError>;

enum class OutboundCallPolicy
{
  Normal,
  AbortConnectionIfDelivered,
};

struct PendingCall
{
  JsonRpcId id;
  std::future<CallResult> completion;
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

class ShutdownEscalation
{
 public:
  virtual ~ShutdownEscalation() = default;
  [[noreturn]] virtual void escalate() noexcept = 0;
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

[[nodiscard]] std::unique_ptr<ShutdownEscalation> make_process_shutdown_escalation();

struct PeerStats
{
  std::size_t unknown_or_late_responses = 0;
  std::size_t duplicate_inbound_ids = 0;
  std::size_t canceled_inbound_requests = 0;
  std::size_t dropped_notifications = 0;
  AVA_DEBUG_PRINT_MEMBERS_ON
};

class JsonRpcPeer
{
 public:
  JsonRpcPeer(std::unique_ptr<RecordTransport> transport, RequestHandler request_handler, NotificationHandler notification_handler = {},
              DiagnosticSink diagnostic_sink = {}, std::unique_ptr<ShutdownEscalation> shutdown_escalation = {},
              std::chrono::milliseconds shutdown_grace = kShutdownGrace);
  ~JsonRpcPeer();

  JsonRpcPeer(JsonRpcPeer const&) = delete;
  JsonRpcPeer& operator=(JsonRpcPeer const&) = delete;
  JsonRpcPeer(JsonRpcPeer&&) = delete;
  JsonRpcPeer& operator=(JsonRpcPeer&&) = delete;

  // Runs the sole reader on the calling thread until EOF, cancellation, or a
  // transport failure. The peer may be run exactly once.
  [[nodiscard]] ava::core::VoidResult run();
  void shutdown() noexcept;
  void set_request_pre_admission_hook(RequestPreAdmissionHook hook);
  void set_control_notification_handler(ControlNotificationHandler handler);
  // Commits one inbound request's terminal arbitration by exact JSON-RPC id.
  // Returns false when cancellation already won or the request is no longer active.
  [[nodiscard]] bool commit_inbound_request(JsonRpcId const& id) noexcept;

  [[nodiscard]] ava::core::Result<PendingCall> send_request(std::string method, std::optional<std::string> params_json,
                                                            std::chrono::milliseconds timeout = kDefaultCallTimeout,
                                                            OutboundCallPolicy policy = OutboundCallPolicy::Normal);
  // Releases one outbound waiter. Normal requests already delivered to the
  // client may still produce a safely ignored late response. Fail-stop
  // requests abort the connection when delivery makes their outcome ambiguous.
  bool cancel_pending_call(JsonRpcId const& id, std::string reason = "ACP outbound request cancelled") noexcept;
  [[nodiscard]] ava::core::VoidResult send_notification(std::string method, std::optional<std::string> params_json);
  [[nodiscard]] PeerStats stats() const noexcept;

 private:
  class State;
  std::unique_ptr<State> state_;
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

}  // namespace ava::app::acp

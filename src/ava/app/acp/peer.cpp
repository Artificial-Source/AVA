#include "sys.h"
#include "ava/app/acp/codec.h"
#include "ava/app/acp/peer.h"
#include "ava/core/ids.h"
#include "ava/core/thread.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace ava::app::acp {
namespace {

std::string id_key(JsonRpcId const& id)
{
  if (std::holds_alternative<NullJsonRpcId>(id))
    return "n:";
  if (auto const* number = std::get_if<std::int64_t>(&id))
    return "i:" + std::to_string(*number);
  return "s:" + std::get<std::string>(id);
}

JsonRpcError canceled_error(std::string message)
{
  return JsonRpcError{.code = -32800, .message = std::move(message), .data_json = std::nullopt, .id = std::nullopt, .suppress_response = false};
}

JsonRpcError connection_error(std::string message)
{
  return JsonRpcError{.code = -32603, .message = std::move(message), .data_json = std::nullopt, .id = std::nullopt, .suppress_response = false};
}

JsonRpcError ambiguous_delivery_error()
{
  return connection_error("ACP outbound request was delivered, but the response outcome is unknown");
}

ava::core::Error peer_error(std::string message)
{
  return ava::core::Error(ava::core::ErrorCategory::Io, std::move(message));
}

class ProcessShutdownEscalation final : public ShutdownEscalation
{
 public:
  [[noreturn]] void escalate() noexcept override { std::_Exit(kShutdownEscalationExitCode); }
};

}  // namespace

std::unique_ptr<ShutdownEscalation> make_process_shutdown_escalation()
{
  return std::make_unique<ProcessShutdownEscalation>();
}

class JsonRpcPeer::State
{
 public:
  State(std::unique_ptr<RecordTransport> transport, RequestHandler request_handler, NotificationHandler notification_handler, DiagnosticSink diagnostic_sink,
        std::unique_ptr<ShutdownEscalation> shutdown_escalation, std::chrono::milliseconds shutdown_grace)
      : transport_(std::move(transport)),
        request_handler_(std::move(request_handler)),
        notification_handler_(std::move(notification_handler)),
        diagnostic_sink_(std::move(diagnostic_sink)),
        shutdown_escalation_(shutdown_escalation ? std::move(shutdown_escalation) : make_process_shutdown_escalation()),
        shutdown_grace_(shutdown_grace > std::chrono::milliseconds::zero() ? shutdown_grace : std::chrono::milliseconds(1)),
        request_nonce_(ava::core::make_id("connection"))
  {
  }

  ~State()
  {
    request_shutdown();
    finish();
  }

  ava::core::VoidResult run()
  {
    bool expected = false;
    if (!started_.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
      return std::unexpected(peer_error("ACP peer may only be run once"));
    if (!transport_ || !request_handler_)
      return std::unexpected(peer_error("ACP peer is missing transport or service"));
    start_threads();

    std::optional<ava::core::Error> failure;
    while (!aborting_.load(std::memory_order_acquire))
    {
      ReadRecord read = transport_->read_record();
      if (read.status == ReadRecordStatus::Record)
      {
        handle_record(read.record);
        continue;
      }
      if (read.status == ReadRecordStatus::RecoverableError)
      {
        if (read.intent == EnvelopeIntent::Notification || read.intent == EnvelopeIntent::Response)
        {
          if (read.intent == EnvelopeIntent::Response)
          {
            unknown_or_late_responses_.fetch_add(1, std::memory_order_relaxed);
            diagnose("ACP peer ignored an oversized possible response");
          }
          continue;
        }
        auto encoded = encode_error(std::nullopt, -32700, "Parse error");
        if (!encoded || !enqueue_record(OutboundRecord{.record = encoded ? std::move(*encoded) : std::string{}}))
        {
          failure = peer_error("ACP output saturated after input limit error");
          request_shutdown();
          break;
        }
        continue;
      }
      if (read.status == ReadRecordStatus::FatalError)
        failure = peer_error("ACP input transport failed");
      request_shutdown();
      break;
    }

    finish();
    if (auto async = take_async_failure())
      return std::unexpected(std::move(*async));
    if (failure)
      return std::unexpected(std::move(*failure));
    return {};
  }

  void request_shutdown() noexcept
  {
    aborting_.store(true, std::memory_order_release);
    accepting_.store(false, std::memory_order_release);
    if (transport_)
      transport_->cancel();
    std::deque<WorkerTask> abandoned_tasks;
    {
      std::lock_guard lock(task_mutex_);
      task_closing_ = true;
      abandoned_tasks.swap(tasks_);
    }
    abort_pending_connection(canceled_error("ACP connection closed"));
    request_inflight_stop();
    request_worker_stop();
    task_cv_.notify_all();
    queue_cv_.notify_all();
    deadline_cv_.notify_all();
  }

  void finish() noexcept
  {
    if (!started_.load(std::memory_order_acquire))
      return;
    bool expected = false;
    if (!finishing_.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
    {
      std::unique_lock lock(finish_mutex_);
      finish_cv_.wait(lock, [&] { return finished_; });
      return;
    }

    request_shutdown();
    {
      std::lock_guard lock(task_mutex_);
      task_closing_ = true;
    }
    request_worker_stop();
    task_cv_.notify_all();

    {
      std::unique_lock lock(worker_exit_mutex_);
      if (!worker_exit_cv_.wait_for(lock, shutdown_grace_, [&] { return workers_exited_ == worker_count_; }))
        shutdown_escalation_->escalate();
    }
    std::vector<ava::core::JoinThread> workers;
    {
      std::lock_guard workers_lock(workers_mutex_);
      workers.swap(workers_);
    }
    for (auto& worker : workers)
      if (worker.joinable())
        worker.join();

    {
      std::lock_guard lock(queue_mutex_);
      writer_closing_ = true;
    }
    queue_cv_.notify_all();
    if (aborting_.load(std::memory_order_acquire) && transport_)
      transport_->cancel();
    if (writer_.joinable())
      writer_.join();
    discard_queued_records();

    deadline_stop_.store(true, std::memory_order_release);
    deadline_cv_.notify_all();
    if (deadline_thread_.joinable())
      deadline_thread_.join();
    release_inflight();
    if (transport_)
      transport_->cancel();
    {
      std::lock_guard lock(finish_mutex_);
      finished_ = true;
    }
    finish_cv_.notify_all();
  }

  ava::core::Result<PendingCall> send_request(std::string method, std::optional<std::string> params_json, std::chrono::milliseconds timeout,
                                              OutboundCallPolicy policy)
  {
    if (!started_.load(std::memory_order_acquire) || !accepting_.load(std::memory_order_acquire) || aborting_.load(std::memory_order_acquire))
      return std::unexpected(peer_error("ACP peer is not accepting outbound calls"));
    if (timeout <= std::chrono::milliseconds::zero())
      return std::unexpected(peer_error("ACP outbound call timeout must be positive"));

    auto sequence = next_request_id_.fetch_add(1, std::memory_order_relaxed);
    JsonRpcId id = std::string("ava-acp-") + request_nonce_ + "-" + std::to_string(sequence);
    auto pending = std::make_unique<Pending>();
    pending->deadline = std::chrono::steady_clock::now() + timeout;
    pending->policy = policy;
    pending->ticket = std::make_shared<OutboundCallTicket>();
    pending->ticket->key = id_key(id);
    auto ticket = pending->ticket;
    auto future = pending->promise.get_future();
    {
      std::lock_guard lock(pending_mutex_);
      if (!accepting_.load(std::memory_order_acquire) || aborting_.load(std::memory_order_acquire))
        return std::unexpected(peer_error("ACP peer is not accepting outbound calls"));
      if (pending_.size() >= kMaxPendingCalls)
        return std::unexpected(peer_error("ACP pending outbound call limit reached"));
      pending_.emplace(id_key(id), std::move(pending));
    }

    auto encoded = encode_request(id, method, params_json ? std::optional<std::string_view>(*params_json) : std::nullopt);
    if (!encoded)
    {
      rollback_queued_pending(id, ticket);
      return std::unexpected(std::move(encoded.error()));
    }
    if (!enqueue_record(OutboundRecord{.record = std::move(*encoded), .outbound_call = ticket}))
    {
      rollback_queued_pending(id, ticket);
      return std::unexpected(peer_error("ACP outbound queue saturated"));
    }
    deadline_cv_.notify_all();
    return PendingCall{.id = std::move(id), .completion = std::move(future)};
  }

  bool cancel_pending_call(JsonRpcId const& id, std::string reason) noexcept
  {
    std::unique_ptr<Pending> pending;
    bool abort_connection = false;
    bool ambiguous_delivery = false;
    {
      std::lock_guard lock(pending_mutex_);
      auto const iterator = pending_.find(id_key(id));
      if (iterator == pending_.end())
        return false;
      auto const& ticket = iterator->second->ticket;
      if (ticket->state == OutboundCallState::Claimed)
      {
        ticket->state = OutboundCallState::CancellationRequested;
        ticket->cancellation_error = canceled_error(std::move(reason));
        ticket->staged_response.reset();
        poison_connection_locked();
        abort_connection = true;
      }
      else if (ticket->state == OutboundCallState::Queued)
      {
        ticket->state = OutboundCallState::Invalid;
        pending = std::move(iterator->second);
        pending_.erase(iterator);
      }
      else if (ticket->state == OutboundCallState::Delivered)
      {
        ambiguous_delivery = iterator->second->policy == OutboundCallPolicy::AbortConnectionIfDelivered;
        ticket->state = OutboundCallState::Invalid;
        pending = std::move(iterator->second);
        pending_.erase(iterator);
        if (ambiguous_delivery)
        {
          poison_connection_locked();
          abort_connection = true;
        }
      }
    }
    if (abort_connection)
      request_shutdown();
    if (!pending)
      return true;
    try
    {
      pending->promise.set_value(std::unexpected(ambiguous_delivery ? ambiguous_delivery_error() : canceled_error(std::move(reason))));
    }
    catch (...)
    {
    }
    return true;
  }

  ava::core::VoidResult send_notification(std::string method, std::optional<std::string> params_json)
  {
    if (!started_.load(std::memory_order_acquire) || !accepting_.load(std::memory_order_acquire) || aborting_.load(std::memory_order_acquire))
      return std::unexpected(peer_error("ACP peer is not accepting outbound notifications"));
    auto encoded = encode_notification(method, params_json ? std::optional<std::string_view>(*params_json) : std::nullopt);
    if (!encoded)
      return std::unexpected(std::move(encoded.error()));
    if (!enqueue_record(OutboundRecord{.record = std::move(*encoded)}))
      return std::unexpected(peer_error("ACP outbound queue saturated"));
    return {};
  }

  void set_request_pre_admission_hook(RequestPreAdmissionHook hook)
  {
    if (started_.load(std::memory_order_acquire))
      return;
    request_pre_admission_hook_ = std::move(hook);
  }

  void set_control_notification_handler(ControlNotificationHandler handler)
  {
    if (started_.load(std::memory_order_acquire))
      return;
    control_notification_handler_ = std::move(handler);
  }

  bool commit_inbound_request(JsonRpcId const& id) noexcept
  {
    std::lock_guard lock(inflight_mutex_);
    auto const iterator = inflight_.find(id_key(id));
    if (iterator == inflight_.end() || iterator->second.state == InboundState::CancellationWon)
      return false;
    iterator->second.state = InboundState::ResultCommitted;
    return true;
  }

  PeerStats stats() const noexcept
  {
    return PeerStats{.unknown_or_late_responses = unknown_or_late_responses_.load(std::memory_order_relaxed),
                     .duplicate_inbound_ids = duplicate_inbound_ids_.load(std::memory_order_relaxed),
                     .canceled_inbound_requests = canceled_inbound_requests_.load(std::memory_order_relaxed),
                     .dropped_notifications = dropped_notifications_.load(std::memory_order_relaxed)};
  }

 private:
  enum class OutboundCallState
  {
    Queued,
    Claimed,
    Delivered,
    CancellationRequested,
    Invalid,
  };

  struct OutboundCallTicket
  {
    std::string key;
    OutboundCallState state = OutboundCallState::Queued;
    std::optional<JsonRpcError> cancellation_error;
    std::optional<CallResult> staged_response;
    std::optional<bool> terminal_delivery;
    bool transport_abort_complete = false;
  };

  struct InboundTicket
  {
    InboundTicket(std::string value, JsonRpcId request_id, std::stop_token request_stop_token)
        : key(std::move(value)), id(std::move(request_id)), stop_token(std::move(request_stop_token))
    {
    }
    std::string key;
    JsonRpcId id;
    std::stop_token stop_token;
  };

  struct OutboundRecord
  {
    std::string record;
    std::shared_ptr<OutboundCallTicket> outbound_call{};
    std::shared_ptr<InboundTicket> inbound_terminal{};
  };

  struct Pending
  {
    std::promise<CallResult> promise;
    std::chrono::steady_clock::time_point deadline;
    OutboundCallPolicy policy = OutboundCallPolicy::Normal;
    std::shared_ptr<OutboundCallTicket> ticket;
  };

  enum class InboundState
  {
    Running,
    CancellationWon,
    ResultCommitted,
  };

  struct Inflight
  {
    std::stop_source source;
    std::shared_ptr<InboundTicket> ticket;
    InboundState state = InboundState::Running;
  };

  struct AdmissionTicket
  {
    explicit AdmissionTicket(std::function<void()> rollback_callback) : rollback(std::move(rollback_callback)) { }
    ~AdmissionTicket() { resolve(); }

    // Resolve only after the handler returns. The callback removes an
    // unconsumed reservation and is an idempotent no-op after service
    // consumption, so cancellation/EOF and handler consumption have one owner.
    void resolve() noexcept
    {
      if (!resolved.exchange(true, std::memory_order_acq_rel) && rollback)
        rollback();
    }

    std::function<void()> rollback;
    std::atomic_bool resolved = false;
  };

  using WorkerTask = std::function<void(std::stop_token)>;

  void start_threads()
  {
    accepting_.store(true, std::memory_order_release);
    writer_ = ava::core::make_thread("acp_writer", [this] { writer_loop(); });
    deadline_thread_ = ava::core::make_thread("acp_deadline", [this] { deadline_loop(); });
    std::lock_guard workers_lock(workers_mutex_);
    workers_.reserve(kWorkerCount);
    worker_count_ = kWorkerCount;
    for (std::size_t index = 0; index < kWorkerCount; ++index)
      workers_.emplace_back(ava::core::JoinThread::create("acp_worker", [this](std::stop_token token) { worker_loop(token); }));
  }

  void request_worker_stop() noexcept
  {
    std::lock_guard lock(workers_mutex_);
    for (auto& worker : workers_)
      worker.request_stop();
  }

  void diagnose(std::string_view message) noexcept
  {
    if (!diagnostic_sink_)
      return;
    try
    {
      diagnostic_sink_(message);
    }
    catch (...)
    {
    }
  }

  void record_async_failure(ava::core::Error error)
  {
    {
      std::lock_guard lock(failure_mutex_);
      if (!async_failure_)
        async_failure_ = std::move(error);
    }
    request_shutdown();
  }

  std::optional<ava::core::Error> take_async_failure()
  {
    std::lock_guard lock(failure_mutex_);
    return std::move(async_failure_);
  }

  bool enqueue_record(OutboundRecord record)
  {
    if (record.record.empty() || record.record.size() > kMaxRecordBytes)
      return false;
    std::lock_guard lock(queue_mutex_);
    if (writer_closing_ || aborting_.load(std::memory_order_acquire) || queue_.size() >= kMaxOutboundRecords ||
        queued_bytes_ + record.record.size() > kMaxOutboundBytes)
      return false;
    queued_bytes_ += record.record.size();
    queue_.push_back(std::move(record));
    queue_cv_.notify_one();
    return true;
  }

  void rollback_queued_pending(JsonRpcId const& id, std::shared_ptr<OutboundCallTicket> const& ticket) noexcept
  {
    std::unique_ptr<Pending> pending;
    std::lock_guard lock(pending_mutex_);
    auto const iterator = pending_.find(id_key(id));
    if (iterator == pending_.end() || iterator->second->ticket != ticket || ticket->state != OutboundCallState::Queued)
      return;
    ticket->state = OutboundCallState::Invalid;
    pending = std::move(iterator->second);
    pending_.erase(iterator);
  }

  void transition_to_cancellation_owned_locked(std::shared_ptr<OutboundCallTicket> const& ticket) noexcept
  {
    if (ticket->state == OutboundCallState::Queued)
      ticket->terminal_delivery = false;
    else if (ticket->state == OutboundCallState::Claimed)
      ticket->terminal_delivery.reset();
    else if (ticket->state == OutboundCallState::Delivered)
      ticket->terminal_delivery = true;
    else
    {
      ticket->staged_response.reset();
      return;
    }
    ticket->state = OutboundCallState::CancellationRequested;
    ticket->staged_response.reset();
  }

  void poison_connection_locked() noexcept
  {
    accepting_.store(false, std::memory_order_release);
    aborting_.store(true, std::memory_order_release);
    for (auto const& [key, pending] : pending_)
    {
      static_cast<void>(key);
      transition_to_cancellation_owned_locked(pending->ticket);
    }
  }

  bool claim_record(OutboundRecord const& record)
  {
    if (!record.outbound_call)
      return true;
    std::lock_guard lock(pending_mutex_);
    if (record.outbound_call->state != OutboundCallState::Queued)
      return false;
    record.outbound_call->state = OutboundCallState::Claimed;
    return true;
  }

  void writer_loop()
  {
    while (true)
    {
      OutboundRecord record;
      {
        std::unique_lock lock(queue_mutex_);
        queue_cv_.wait(lock, [&] { return writer_closing_ || !queue_.empty() || aborting_.load(std::memory_order_acquire); });
        if ((writer_closing_ || aborting_.load(std::memory_order_acquire)) && queue_.empty())
          return;
        if (aborting_.load(std::memory_order_acquire))
          return;
        record = std::move(queue_.front());
        queued_bytes_ -= record.record.size();
        queue_.pop_front();
      }
      if (!claim_record(record))
        continue;
      if (record.outbound_call && aborting_.load(std::memory_order_acquire))
      {
        acknowledge_outbound(record.outbound_call, false);
        return;
      }
      auto written = transport_->write_record(record.record);
      bool const outbound_terminal = record.outbound_call && acknowledge_outbound(record.outbound_call, written.has_value());
      if (!written)
      {
        if (!outbound_terminal)
          record_async_failure(peer_error("ACP output transport failed"));
        return;
      }
      if (outbound_terminal)
        return;
      if (record.inbound_terminal)
        acknowledge_inbound(record.inbound_terminal);
    }
  }

  void discard_queued_records()
  {
    std::deque<OutboundRecord> discarded;
    {
      std::lock_guard lock(queue_mutex_);
      discarded.swap(queue_);
      queued_bytes_ = 0;
    }
    std::lock_guard lock(pending_mutex_);
    for (auto& record : discarded)
      if (record.outbound_call && record.outbound_call->state == OutboundCallState::Queued)
        record.outbound_call->state = OutboundCallState::Invalid;
  }

  void handle_record(std::string_view record)
  {
    auto decoded = decode_message(record);
    if (!decoded)
    {
      if (decoded.error().suppress_response)
      {
        if (decoded.error().intent == EnvelopeIntent::Response)
        {
          unknown_or_late_responses_.fetch_add(1, std::memory_order_relaxed);
          diagnose("ACP peer ignored a malformed or uncorrelated response");
        }
        return;
      }
      auto encoded = encode_error(decoded.error().id, decoded.error().code, decoded.error().message,
                                  decoded.error().data_json ? std::optional<std::string_view>(*decoded.error().data_json) : std::nullopt);
      if (!encoded || !enqueue_record(OutboundRecord{.record = encoded ? std::move(*encoded) : std::string{}}))
        record_async_failure(peer_error("ACP output saturated while reporting a codec error"));
      return;
    }
    std::visit(
        [this](auto&& message) {
          using T = std::decay_t<decltype(message)>;
          if constexpr (std::is_same_v<T, Request>)
            dispatch_request(std::move(message));
          else if constexpr (std::is_same_v<T, Notification>)
            dispatch_notification(std::move(message));
          else if constexpr (std::is_same_v<T, Response>)
            complete_pending(message.id, CallResult(std::move(message.result_json)));
          else
            complete_pending(message.id, std::unexpected(std::move(message.error)));
        },
        std::move(*decoded));
  }

  void dispatch_request(Request request)
  {
    auto const key = id_key(request.id);
    JsonRpcId const response_id = request.id;
    std::stop_source source;
    auto ticket = std::make_shared<InboundTicket>(key, request.id, source.get_token());
    int rejection_code = 0;
    std::string_view rejection_message;
    {
      std::lock_guard lock(inflight_mutex_);
      if (inflight_.contains(key))
      {
        duplicate_inbound_ids_.fetch_add(1, std::memory_order_relaxed);
        rejection_code = -32600;
        rejection_message = "Duplicate in-flight request id";
      }
      else if (inflight_.size() >= kMaxInflightRequests)
      {
        rejection_code = -32603;
        rejection_message = "ACP in-flight request limit reached";
      }
      else
        inflight_.emplace(key, Inflight{.source = source, .ticket = ticket});
    }
    if (rejection_code != 0)
    {
      enqueue_request_error(response_id, rejection_code, rejection_message);
      return;
    }

    std::shared_ptr<AdmissionTicket> admission;
    if (request_pre_admission_hook_)
    {
      auto admitted = request_pre_admission_hook_(request);
      if (!admitted)
      {
        {
          std::lock_guard lock(inflight_mutex_);
          auto const iterator = inflight_.find(key);
          if (iterator != inflight_.end() && iterator->second.ticket == ticket)
            inflight_.erase(iterator);
        }
        enqueue_request_error(response_id, admitted.error().code, admitted.error().message);
        return;
      }
      admission = std::make_shared<AdmissionTicket>(std::move(*admitted));
    }

    auto task = [this, request = std::move(request), source, ticket, admission](std::stop_token) mutable {
      bool canceled = false;
      {
        std::lock_guard lock(inflight_mutex_);
        auto const iterator = inflight_.find(ticket->key);
        canceled = iterator != inflight_.end() && iterator->second.ticket == ticket && iterator->second.state == InboundState::CancellationWon;
      }
      RequestResult result = canceled ? RequestResult(std::unexpected(canceled_error("Request cancelled"))) : request_handler_(request, source.get_token());
      if (admission)
        admission->resolve();
      {
        std::lock_guard lock(inflight_mutex_);
        auto const iterator = inflight_.find(ticket->key);
        if (iterator != inflight_.end() && iterator->second.ticket == ticket)
        {
          canceled = iterator->second.state == InboundState::CancellationWon;
          if (!canceled && iterator->second.state == InboundState::Running)
            iterator->second.state = InboundState::ResultCommitted;
        }
      }
      if (aborting_.load(std::memory_order_acquire))
        return;
      ava::core::Result<std::string> encoded;
      if (canceled)
        encoded = encode_error(request.id, -32800, "Request cancelled");
      else if (result)
        encoded = encode_success(request.id, *result);
      else
        encoded = encode_error(request.id, result.error().code, result.error().message,
                               result.error().data_json ? std::optional<std::string_view>(*result.error().data_json) : std::nullopt);
      if (!encoded || !enqueue_record(OutboundRecord{.record = encoded ? std::move(*encoded) : std::string{}, .inbound_terminal = ticket}))
        record_async_failure(peer_error("ACP output saturated while completing a request"));
    };
    {
      std::lock_guard lock(task_mutex_);
      if (task_closing_ || tasks_.size() >= kMaxWorkerQueue)
      {
        {
          std::lock_guard inflight_lock(inflight_mutex_);
          auto const iterator = inflight_.find(key);
          if (iterator != inflight_.end() && iterator->second.ticket == ticket)
            inflight_.erase(iterator);
        }
        enqueue_request_error(response_id, -32603, "ACP worker queue limit reached");
        return;
      }
      tasks_.push_back(std::move(task));
    }
    task_cv_.notify_one();
  }

  void enqueue_request_error(JsonRpcId const& id, int code, std::string_view message)
  {
    auto encoded = encode_error(id, code, message);
    if (!encoded || !enqueue_record(OutboundRecord{.record = encoded ? std::move(*encoded) : std::string{}}))
      record_async_failure(peer_error("ACP output saturated while rejecting a request"));
  }

  void dispatch_notification(Notification notification)
  {
    if (notification.method == "$/cancel_request")
    {
      auto id = decode_cancel_request_params(notification);
      if (!id)
        return;
      std::lock_guard lock(inflight_mutex_);
      auto const iterator = inflight_.find(id_key(*id));
      if (iterator != inflight_.end() && iterator->second.state == InboundState::Running)
      {
        iterator->second.state = InboundState::CancellationWon;
        iterator->second.source.request_stop();
        canceled_inbound_requests_.fetch_add(1, std::memory_order_relaxed);
      }
      return;
    }
    if (notification.method == "session/cancel")
    {
      if (!control_notification_handler_)
        return;
      try
      {
        control_notification_handler_(notification);
      }
      catch (...)
      {
        record_async_failure(peer_error("ACP control notification callback failed"));
      }
      return;
    }
    if (!notification_handler_)
      return;
    std::lock_guard lock(task_mutex_);
    if (task_closing_ || tasks_.size() >= kMaxWorkerQueue)
    {
      dropped_notifications_.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    tasks_.push_back([this, notification = std::move(notification)](std::stop_token token) { notification_handler_(notification, token); });
    task_cv_.notify_one();
  }

  void worker_loop(std::stop_token token)
  {
    while (true)
    {
      WorkerTask task;
      {
        std::unique_lock lock(task_mutex_);
        task_cv_.wait(lock, [&] { return token.stop_requested() || task_closing_ || !tasks_.empty(); });
        if ((token.stop_requested() || task_closing_) && tasks_.empty())
          break;
        task = std::move(tasks_.front());
        tasks_.pop_front();
      }
      try
      {
        task(token);
      }
      catch (...)
      {
        record_async_failure(peer_error("ACP service callback failed"));
      }
    }
    {
      std::lock_guard lock(worker_exit_mutex_);
      ++workers_exited_;
    }
    worker_exit_cv_.notify_all();
  }

  void acknowledge_inbound(std::shared_ptr<InboundTicket> const& ticket)
  {
    std::lock_guard lock(inflight_mutex_);
    auto const iterator = inflight_.find(ticket->key);
    if (iterator != inflight_.end() && iterator->second.ticket == ticket)
      inflight_.erase(iterator);
  }

  using PendingFailure = std::pair<std::unique_ptr<Pending>, JsonRpcError>;
  using PendingCompletion = std::pair<std::unique_ptr<Pending>, CallResult>;

  static void fulfill_failures(std::vector<PendingFailure> failures)
  {
    for (auto& [pending, error] : failures)
      pending->promise.set_value(std::unexpected(std::move(error)));
  }

  static void fulfill_completions(std::vector<PendingCompletion> completions)
  {
    for (auto& [pending, result] : completions)
      pending->promise.set_value(std::move(result));
  }

  void finalize_canceled_locked(std::shared_ptr<OutboundCallTicket> const& ticket, std::vector<PendingFailure>& failures)
  {
    if (!ticket->transport_abort_complete || !ticket->terminal_delivery)
      return;
    auto const iterator = pending_.find(ticket->key);
    if (iterator == pending_.end() || iterator->second->ticket != ticket)
      return;
    auto error = *ticket->terminal_delivery ? ambiguous_delivery_error() : ticket->cancellation_error.value_or(canceled_error("ACP connection closed"));
    ticket->state = OutboundCallState::Invalid;
    ticket->staged_response.reset();
    failures.emplace_back(std::move(iterator->second), std::move(error));
    pending_.erase(iterator);
  }

  bool acknowledge_outbound(std::shared_ptr<OutboundCallTicket> const& ticket, bool delivered)
  {
    std::vector<PendingFailure> failures;
    std::vector<PendingCompletion> completions;
    bool cancellation_owned = false;
    {
      std::lock_guard lock(pending_mutex_);
      if (ticket->state == OutboundCallState::Claimed && aborting_.load(std::memory_order_acquire))
      {
        cancellation_owned = true;
        transition_to_cancellation_owned_locked(ticket);
        ticket->terminal_delivery = delivered;
        finalize_canceled_locked(ticket, failures);
      }
      else if (ticket->state == OutboundCallState::Claimed)
      {
        if (delivered)
        {
          ticket->state = OutboundCallState::Delivered;
          if (ticket->staged_response)
          {
            auto const iterator = pending_.find(ticket->key);
            if (iterator != pending_.end() && iterator->second->ticket == ticket)
            {
              ticket->state = OutboundCallState::Invalid;
              completions.emplace_back(std::move(iterator->second), std::move(*ticket->staged_response));
              ticket->staged_response.reset();
              pending_.erase(iterator);
            }
          }
        }
        else
        {
          ticket->state = OutboundCallState::CancellationRequested;
          ticket->cancellation_error = connection_error("ACP connection failed before outbound request delivery");
          ticket->staged_response.reset();
          ticket->terminal_delivery = false;
        }
      }
      else if (ticket->state == OutboundCallState::CancellationRequested)
      {
        cancellation_owned = true;
        ticket->staged_response.reset();
        ticket->terminal_delivery = delivered;
        finalize_canceled_locked(ticket, failures);
      }
      else
        cancellation_owned = aborting_.load(std::memory_order_acquire);
    }
    fulfill_failures(std::move(failures));
    fulfill_completions(std::move(completions));
    return cancellation_owned;
  }

  void complete_pending(JsonRpcId const& id, CallResult result)
  {
    std::unique_ptr<Pending> pending;
    bool ignored_for_cancellation = false;
    bool ignored_response = false;
    {
      std::lock_guard lock(pending_mutex_);
      auto const iterator = pending_.find(id_key(id));
      if (iterator == pending_.end())
        ignored_response = true;
      else
      {
        auto const& ticket = iterator->second->ticket;
        if (aborting_.load(std::memory_order_acquire) &&
            (ticket->state == OutboundCallState::Queued || ticket->state == OutboundCallState::Claimed || ticket->state == OutboundCallState::Delivered))
        {
          transition_to_cancellation_owned_locked(ticket);
          ignored_for_cancellation = true;
        }
        else if (ticket->state == OutboundCallState::Queued)
          ignored_response = true;
        else if (ticket->state == OutboundCallState::Claimed)
        {
          if (ticket->staged_response)
            ignored_response = true;
          else
            ticket->staged_response = std::move(result);
        }
        else if (ticket->state == OutboundCallState::Delivered)
        {
          ticket->state = OutboundCallState::Invalid;
          pending = std::move(iterator->second);
          pending_.erase(iterator);
        }
        else if (ticket->state == OutboundCallState::CancellationRequested)
          ignored_for_cancellation = true;
        else
          ignored_response = true;
      }
    }
    if (pending)
    {
      pending->promise.set_value(std::move(result));
      return;
    }
    if (ignored_response && !ignored_for_cancellation)
    {
      unknown_or_late_responses_.fetch_add(1, std::memory_order_relaxed);
      diagnose("ACP peer ignored an unknown, premature, or late response");
    }
  }

  void abort_pending_connection(JsonRpcError error)
  {
    std::vector<PendingFailure> failures;
    {
      std::lock_guard lock(pending_mutex_);
      for (auto iterator = pending_.begin(); iterator != pending_.end();)
      {
        auto const& ticket = iterator->second->ticket;
        if (ticket->state == OutboundCallState::Queued)
        {
          ticket->state = OutboundCallState::Invalid;
          failures.emplace_back(std::move(iterator->second), error);
          iterator = pending_.erase(iterator);
          continue;
        }
        if (ticket->state == OutboundCallState::Delivered)
        {
          ticket->state = OutboundCallState::Invalid;
          failures.emplace_back(std::move(iterator->second), ambiguous_delivery_error());
          iterator = pending_.erase(iterator);
          continue;
        }
        if (ticket->state == OutboundCallState::Claimed)
        {
          ticket->state = OutboundCallState::CancellationRequested;
          ticket->cancellation_error = error;
          ticket->staged_response.reset();
        }
        ++iterator;
      }
    }

    if (transport_)
      transport_->cancel();

    {
      std::lock_guard lock(pending_mutex_);
      std::vector<std::shared_ptr<OutboundCallTicket>> tickets;
      tickets.reserve(pending_.size());
      for (auto const& [key, pending] : pending_)
      {
        static_cast<void>(key);
        if (pending->ticket->state == OutboundCallState::CancellationRequested)
        {
          pending->ticket->transport_abort_complete = true;
          tickets.push_back(pending->ticket);
        }
      }
      for (auto const& ticket : tickets)
        finalize_canceled_locked(ticket, failures);
    }
    fulfill_failures(std::move(failures));
  }

  void deadline_loop()
  {
    while (!deadline_stop_.load(std::memory_order_acquire))
    {
      std::vector<PendingFailure> expired;
      bool abort_connection = false;
      auto wake = std::chrono::steady_clock::now() + std::chrono::milliseconds(20);
      {
        std::lock_guard lock(pending_mutex_);
        auto const now = std::chrono::steady_clock::now();
        for (auto iterator = pending_.begin(); iterator != pending_.end();)
        {
          wake = std::min(wake, iterator->second->deadline);
          if (iterator->second->deadline > now)
          {
            ++iterator;
            continue;
          }

          auto const& ticket = iterator->second->ticket;
          if (ticket->state == OutboundCallState::Queued)
          {
            ticket->state = OutboundCallState::Invalid;
            expired.emplace_back(std::move(iterator->second), canceled_error("ACP outbound request timed out"));
            iterator = pending_.erase(iterator);
            continue;
          }
          if (ticket->state == OutboundCallState::Delivered)
          {
            bool const fail_stop = iterator->second->policy == OutboundCallPolicy::AbortConnectionIfDelivered;
            ticket->state = OutboundCallState::Invalid;
            expired.emplace_back(std::move(iterator->second), ambiguous_delivery_error());
            iterator = pending_.erase(iterator);
            if (fail_stop)
            {
              poison_connection_locked();
              abort_connection = true;
            }
            continue;
          }
          if (ticket->state == OutboundCallState::Claimed)
          {
            ticket->state = OutboundCallState::CancellationRequested;
            ticket->cancellation_error = canceled_error("ACP outbound request timed out");
            ticket->staged_response.reset();
            poison_connection_locked();
            abort_connection = true;
          }
          ++iterator;
        }
      }
      if (abort_connection)
        request_shutdown();
      fulfill_failures(std::move(expired));
      std::unique_lock lock(deadline_mutex_);
      deadline_cv_.wait_until(lock, wake, [&] { return deadline_stop_.load(std::memory_order_acquire); });
    }
  }

  void request_inflight_stop() noexcept
  {
    std::lock_guard lock(inflight_mutex_);
    for (auto& [key, inflight] : inflight_)
    {
      static_cast<void>(key);
      if (inflight.state == InboundState::Running)
      {
        inflight.state = InboundState::CancellationWon;
        inflight.source.request_stop();
      }
    }
  }

  void release_inflight() noexcept
  {
    std::lock_guard lock(inflight_mutex_);
    inflight_.clear();
  }

  std::unique_ptr<RecordTransport> transport_;
  RequestHandler request_handler_;
  NotificationHandler notification_handler_;
  ControlNotificationHandler control_notification_handler_;
  RequestPreAdmissionHook request_pre_admission_hook_;
  DiagnosticSink diagnostic_sink_;
  std::unique_ptr<ShutdownEscalation> shutdown_escalation_;
  std::chrono::milliseconds shutdown_grace_;

  std::atomic_bool started_ = false;
  std::atomic_bool accepting_ = false;
  std::atomic_bool aborting_ = false;
  std::atomic_bool finishing_ = false;
  std::mutex finish_mutex_;
  std::condition_variable finish_cv_;
  bool finished_ = false;
  std::atomic_bool deadline_stop_ = false;
  std::atomic<std::uint64_t> next_request_id_ = 1;
  std::string request_nonce_;

  std::mutex queue_mutex_;
  std::condition_variable queue_cv_;
  std::deque<OutboundRecord> queue_;
  std::size_t queued_bytes_ = 0;
  bool writer_closing_ = false;
  std::thread writer_;

  std::mutex task_mutex_;
  std::condition_variable task_cv_;
  std::deque<WorkerTask> tasks_;
  bool task_closing_ = false;
  std::vector<ava::core::JoinThread> workers_;
  std::mutex workers_mutex_;
  std::mutex worker_exit_mutex_;
  std::condition_variable worker_exit_cv_;
  std::size_t workers_exited_ = 0;
  std::size_t worker_count_ = 0;

  std::mutex pending_mutex_;
  std::unordered_map<std::string, std::unique_ptr<Pending>> pending_;
  std::mutex deadline_mutex_;
  std::condition_variable deadline_cv_;
  std::thread deadline_thread_;

  std::mutex inflight_mutex_;
  std::unordered_map<std::string, Inflight> inflight_;

  std::mutex failure_mutex_;
  std::optional<ava::core::Error> async_failure_;

  std::atomic_size_t unknown_or_late_responses_ = 0;
  std::atomic_size_t duplicate_inbound_ids_ = 0;
  std::atomic_size_t canceled_inbound_requests_ = 0;
  std::atomic_size_t dropped_notifications_ = 0;
};

JsonRpcPeer::JsonRpcPeer(std::unique_ptr<RecordTransport> transport, RequestHandler request_handler, NotificationHandler notification_handler,
                         DiagnosticSink diagnostic_sink, std::unique_ptr<ShutdownEscalation> shutdown_escalation, std::chrono::milliseconds shutdown_grace)
    : state_(std::make_unique<State>(std::move(transport), std::move(request_handler), std::move(notification_handler), std::move(diagnostic_sink),
                                     std::move(shutdown_escalation), shutdown_grace))
{
}

JsonRpcPeer::~JsonRpcPeer() = default;

ava::core::VoidResult JsonRpcPeer::run()
{
  return state_->run();
}

void JsonRpcPeer::shutdown() noexcept
{
  state_->request_shutdown();
}

void JsonRpcPeer::set_request_pre_admission_hook(RequestPreAdmissionHook hook)
{
  state_->set_request_pre_admission_hook(std::move(hook));
}

void JsonRpcPeer::set_control_notification_handler(ControlNotificationHandler handler)
{
  state_->set_control_notification_handler(std::move(handler));
}

bool JsonRpcPeer::commit_inbound_request(JsonRpcId const& id) noexcept
{
  return state_->commit_inbound_request(id);
}

ava::core::Result<PendingCall> JsonRpcPeer::send_request(std::string method, std::optional<std::string> params_json, std::chrono::milliseconds timeout,
                                                         OutboundCallPolicy policy)
{
  return state_->send_request(std::move(method), std::move(params_json), timeout, policy);
}

bool JsonRpcPeer::cancel_pending_call(JsonRpcId const& id, std::string reason) noexcept
{
  return state_->cancel_pending_call(id, std::move(reason));
}

ava::core::VoidResult JsonRpcPeer::send_notification(std::string method, std::optional<std::string> params_json)
{
  return state_->send_notification(std::move(method), std::move(params_json));
}

PeerStats JsonRpcPeer::stats() const noexcept
{
  return state_->stats();
}

}  // namespace ava::app::acp

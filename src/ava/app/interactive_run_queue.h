#pragma once

#include "ava/event/events.h"
#include "ava/core/NoLogString.h"
#include "ava/core/result.h"

#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace ava::app {

inline constexpr std::size_t kMaxInteractiveQueuedMessages = 64;
inline constexpr std::size_t kMaxInteractiveQueuedMessageBytes = 64 * 1024;
inline constexpr std::size_t kMaxInteractiveQueueEventMessageBytes = 512;

struct InteractiveQueuedMessage
{
  std::string request_id;
  std::string correlation_id;
  ava::core::NoLogString message;
  std::size_t sequence = 0;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

struct InteractiveRestoredMessage
{
  std::string request_id;
  ava::core::NoLogString message;
  bool steering = false;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

class InteractiveRunQueue
{
 public:
  InteractiveRunQueue(std::string session_id, std::string active_request_id, ava::event::EventEnvelopeSink event_sink);

  [[nodiscard]] std::string const& active_request_id() const noexcept;
  [[nodiscard]] ava::core::VoidResult queue_steering(std::string message);
  [[nodiscard]] ava::core::VoidResult queue_follow_up(std::string message);
  [[nodiscard]] ava::core::Result<std::vector<std::string>> take_steering_messages();
  [[nodiscard]] ava::core::VoidResult skip_active_steering(std::string_view reason);
  [[nodiscard]] std::optional<InteractiveQueuedMessage> take_next_follow_up();
  [[nodiscard]] ava::core::VoidResult mark_follow_up_started(InteractiveQueuedMessage const& message);
  [[nodiscard]] ava::core::Result<InteractiveRestoredMessage> restore_latest();
  [[nodiscard]] ava::core::VoidResult finish(bool canceled);

  AVA_DEBUG_PRINT_MEMBERS_ON

 private:
  [[nodiscard]] ava::core::VoidResult queue_message_locked(std::deque<InteractiveQueuedMessage>& queue, std::string message, std::string_view event_name);
  [[nodiscard]] ava::core::VoidResult emit_event(std::string_view name, InteractiveQueuedMessage const& message, std::string_view reason = {}) const;

  std::string session_id_;
  std::string active_request_id_;
  ava::event::EventEnvelopeSink event_sink_;
  mutable std::mutex mutex_;
  bool active_ = true;
  std::size_t next_sequence_ = 1;
  std::deque<InteractiveQueuedMessage> steering_messages_;
  std::deque<InteractiveQueuedMessage> follow_up_messages_;
};

}  // namespace ava::app

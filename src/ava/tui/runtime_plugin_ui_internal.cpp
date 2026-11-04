// Queues plugin requests from workers and prepares their views on the TUI thread.
#include "sys.h"
#include "TuiEventLoop.h"
#include "TuiWakeup.h"
#include "ava/app/terminal_text.h"
#include "ava/tui/composer.h"
#include "ava/tui/runtime_plugin_ui_internal.h"
#include "ava/tui/runtime_state_internal.h"
#include "ava/core/json.h"

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <utility>
#include "debug.h"

namespace ava::tui {
using ava::app::sanitize_terminal_text;

namespace {

// One worker call and transport queue/visible slots share this pending reply.
// Both worker and TUI can complete it; mutex protects the first reply and the
// waiting worker retains a share until it consumes that reply. Final destruction
// can occur on either thread, after all shared owners have released it.
struct PendingPluginUiModal
{
  // Both participants lock before storing the first reply or checking readiness.
  std::mutex mutex;
  // Worker waits here between cancellation/deadline checks; completers notify unlocked.
  std::condition_variable ready;
  // First reply wins under mutex; the waiting worker copies it before returning.
  std::optional<TuiPluginUiReply> reply;

  // Internal-linkage synchronization state also contains arbitrary plugin replies.
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

// Worker-built record transferred to a finite TUI poll batch under the state mutex.
// The owned request survives the caller; generation rejects stale batches. The modal
// share pins its reply independently of queue membership and worker lifetime.
struct PluginUiQueueRecord
{
  // Captured when enqueueing; TUI compares against the current submission after batch swap.
  std::uint64_t generation = 0;
  // Owned plugin payload; only a current validated record may update screen data.
  TuiPluginUiRequest request;
  // The queue and waiting worker retain independent ownership until resolution.
  std::shared_ptr<PendingPluginUiModal> modal;

  // Internal-linkage records retain arbitrary plugin request/reply content.
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

// Worker thread outside transport/modal locks: treat a failing cancellation
// callback as cancellation so exceptions cannot strand the synchronous presenter.
bool callback_canceled(TuiPluginUiCancelCallback const& cancel_requested) noexcept
{
  try
  {
    return cancel_requested && cancel_requested();
  }
  catch (...)
  {
    return true;
  }
}

// Pure boundary validation for plugin strings: reject invalid UTF-8, terminal
// controls and size violations before any text enters a TUI surface.
bool safe_text(std::string_view text, std::size_t maximum, bool require_nonempty = false)
{
  return text.size() <= maximum && (!require_nonempty || !text.empty()) && ava::core::json::is_valid_utf8(text) && sanitize_terminal_text(text) == text;
}

// Pure transport check: all three identity components must be bounded printable text.
bool safe_binding(TuiPluginUiBinding const& binding)
{
  return safe_text(binding.plugin_id, 256, true) && safe_text(binding.command, 256, true) && safe_text(binding.invocation_id, 256, true);
}

// Worker request validation: bound aggregate content and enforce each request
// kind before allocating a waiter or trusting any presentation fields.
bool safe_request(TuiPluginUiRequest const& request)
{
  if (!safe_binding(request.binding) || !safe_text(request.request_id, 256, true) || request.lines.size() > kTuiPluginUiMaxWidgetLines ||
      request.options.size() > 32)
  {
    return false;
  }

  // Check the aggregate byte budget as each collection entry is added; each later
  // subtraction is safe because the running count was already bounded.
  std::size_t bytes = request.text.size() + request.title.size() + request.description.size();
  if (bytes > kTuiPluginUiMaxTextBytes || !safe_text(request.text, kTuiPluginUiMaxTextBytes) || !safe_text(request.title, kTuiPluginUiMaxTextBytes) ||
      !safe_text(request.description, kTuiPluginUiMaxTextBytes))
  {
    return false;
  }
  for (auto const& line : request.lines)
  {
    if (!safe_text(line, kTuiPluginUiMaxTextBytes) || line.size() > kTuiPluginUiMaxTextBytes - bytes)
      return false;
    bytes += line.size();
  }
  for (auto const& option : request.options)
  {
    auto const description_bytes = option.description ? option.description->size() : std::size_t{0};
    if (!safe_text(option.id, 256, true) || !safe_text(option.label, kTuiPluginUiMaxTextBytes, true) ||
        (option.description && !safe_text(*option.description, kTuiPluginUiMaxTextBytes)) ||
        option.id.size() + option.label.size() + description_bytes > kTuiPluginUiMaxTextBytes - bytes)
    {
      return false;
    }
    bytes += option.id.size() + option.label.size() + description_bytes;
  }

  // Shape checks keep unused fields from smuggling content around the selected
  // surface's limits or forcing the renderer to interpret an ambiguous request.
  switch (request.kind)
  {
    case TuiPluginUiKind::Status:
      return request.title.empty() && request.description.empty() && request.lines.empty() && request.options.empty();
    case TuiPluginUiKind::Widget:
      return request.text.empty() && !request.title.empty() && !request.lines.empty() && request.lines.size() <= kTuiPluginUiMaxWidgetLines &&
             request.options.empty();
    case TuiPluginUiKind::Select:
      return request.text.empty() && !request.title.empty() && request.lines.empty() && !request.options.empty() && request.options.size() <= 32;
    case TuiPluginUiKind::Confirm:
      return request.text.empty() && !request.title.empty() && request.lines.empty() && request.options.empty();
  }
  return false;
}

// Either thread: retain the pending request while storing its first reply under
// the modal lock. Notify after unlock; empty cancellation replies need no allocation.
void complete_modal(std::shared_ptr<PendingPluginUiModal> const& modal, TuiPluginUiReply reply)
{
  if (!modal)
    return;
  {
    std::lock_guard lock(modal->mutex);
    if (modal->reply)
      return;
    modal->reply = std::move(reply);
  }
  modal->ready.notify_all();
}

// Either thread while retaining a shared modal: test reply readiness under its
// mutex. A caller validating transport takes state mutex before this modal mutex.
bool modal_pending(std::shared_ptr<PendingPluginUiModal> const& modal)
{
  if (!modal)
    return false;
  std::lock_guard lock(modal->mutex);
  return !modal->reply;
}

// TUI candidate validation: count all retained dock text before updating screen data.
std::size_t dock_text_bytes(TuiPluginUiDockView const& dock)
{
  std::size_t bytes = dock.status ? dock.status->size() : 0;
  for (auto const& widget : dock.widgets)
  {
    bytes += widget.title.size();
    for (auto const& line : widget.lines)
      bytes += line.size();
  }
  return bytes;
}

// TUI candidate validation: cap accumulated widget rows across the entire dock.
std::size_t dock_line_count(TuiPluginUiDockView const& dock)
{
  std::size_t lines = 0;
  for (auto const& widget : dock.widgets)
    lines += widget.lines.size();
  return lines;
}

// Pure parser check before creating an endpoint; accept only the bounded spelling used
// by direct plugin submission recognition.
bool canonical_plugin_id(std::string_view value)
{
  if (value.empty() || value.size() > 128)
    return false;
  bool previous_separator = false;
  for (char const ch : value)
  {
    bool const separator = ch == '.' || ch == '_' || ch == '-';
    bool const valid = (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || separator;
    if (!valid || (separator && previous_separator))
      return false;
    previous_separator = separator;
  }
  return !previous_separator;
}

// Pure parser gate: delimit the command token before any optional JSON argument.
bool canonical_command_name(std::string_view value)
{
  return !value.empty() && value.size() <= 96 && std::ranges::all_of(value, [](char ch) {
    return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '_' || ch == '-' || ch == '.';
  });
}

// Pure submission parser: grant an endpoint only to the direct command spelling
// with canonical identity and an optional bounded-depth JSON object. Return owned
// identity strings so the endpoint never borrows the input buffer.
std::optional<std::pair<std::string, std::string>> direct_foreground_plugin_binding(std::string_view submitted)
{
  constexpr std::string_view prefix = "/plugin run ";
  if (submitted.size() > 64 * 1024 || !submitted.starts_with(prefix) || submitted.empty() || submitted.back() == ' ' ||
      sanitize_terminal_text(submitted) != submitted)
  {
    return std::nullopt;
  }
  // Recognize the direct command grammar before copying identities. Trailing and
  // repeated separator spaces are deliberately rejected by these token boundaries.
  auto remaining = submitted.substr(prefix.size());
  auto const plugin_end = remaining.find(' ');
  if (plugin_end == std::string_view::npos || !canonical_plugin_id(remaining.substr(0, plugin_end)))
    return std::nullopt;
  auto const plugin_id = remaining.substr(0, plugin_end);
  remaining.remove_prefix(plugin_end + 1);
  if (remaining.empty() || remaining.front() == ' ')
    return std::nullopt;
  auto const command_end = remaining.find(' ');
  auto const command = remaining.substr(0, command_end);
  if (!canonical_command_name(command))
    return std::nullopt;
  // Optional arguments must be one valid bounded-depth object; a recognized prefix
  // alone must not issue an endpoint for another command form.
  if (command_end != std::string_view::npos)
  {
    remaining.remove_prefix(command_end + 1);
    if (remaining.empty() || remaining.front() != '{' || remaining.back() != '}' || !ava::core::json::is_valid_object_with_max_depth(remaining, 128))
    {
      return std::nullopt;
    }
  }
  return std::pair{std::string(plugin_id), std::string(command)};
}

}  // namespace

// Shared transport for plugin endpoints. The coordinator opens/closes requests
// and owns the display; copied worker endpoints hold a weak reference and keep
// this state alive during a call. All private fields are protected by mutex_.
// TUI-only methods borrow screen data for a synchronous call and never retain it.
// Lock order is state mutex, then modal mutex; worker cancellation releases modal locks first.
// No timer or screen data is retained here. A worker can become the last shared owner,
// so final destruction may run on that worker after coordinator shutdown.
class RuntimePluginUiCoordinatorState final
{
 public:
  // TUI thread before endpoints exist: retain a weak loop wake handle. Workers
  // can later notify without extending loop or coordinator lifetime.
  explicit RuntimePluginUiCoordinatorState(TuiWakeup wakeup) : wakeup_(std::move(wakeup)) { }
  // TUI thread: cancel old waiters, then open one new generation until its deadline.
  [[nodiscard]] std::optional<std::uint64_t> begin(TuiPluginUiBinding binding, std::chrono::steady_clock::time_point deadline);
  // Worker thread with shared ownership: wait for a screen-data update acknowledgement,
  // modal choice, cancellation or deadline. Enqueue before notifying the loop.
  [[nodiscard]] TuiPluginUiReply present(std::uint64_t request_generation, TuiPluginUiRequest const& request,
                                         std::chrono::steady_clock::time_point request_deadline, TuiPluginUiCancelCallback cancel_requested);
  // Worker endpoint: a stale generation/binding cannot close a later submission.
  void close(std::uint64_t request_generation, TuiPluginUiBinding const& binding);
  // TUI timer: stop accepting requests and release waiters before requesting view removal.
  void expire(std::chrono::steady_clock::time_point now);
  // TUI thread: drain one finite batch. Check the submission, update screen data and
  // send Ack under one lock so cancellation cannot race those steps. Ack is not a paint.
  [[nodiscard]] TuiPluginUiPollResult poll(RuntimePresentationState& presentation_, bool host_modal_conflict);
  // TUI thread: retire modal ownership before completing the worker outside the lock.
  void resolve_visible(TuiPluginUiReply reply);
  // Either thread: observe cancellation/deadline without touching screen data.
  [[nodiscard]] bool deadline_reached(std::chrono::steady_clock::time_point now) const;
  // TUI thread: decide whether its deadline registration can be cancelled.
  [[nodiscard]] bool active() const;
  // Either thread: cancel without allocation, then notify after releasing the lock.
  void cancel();
  // TUI thread before worker join: release waiters and retire the current binding.
  void finish();
  // TUI thread before owner destruction: reject all future requests and clear the wake handle.
  void shutdown();
  // Test synchronization only; hooks are exchanged under lock and invoked outside it.
  void set_after_enqueue_for_test(std::function<void()> hook);
  // Test synchronization only; observes a swapped batch before generation validation.
  void set_after_queue_swap_for_test(std::function<void(std::size_t)> hook);

  // Plugin messages and callbacks may contain arbitrary user or plugin content.
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT

 private:
  // Caller holds mutex_. Reject new requests, release waits and flag screen-data removal.
  // No allocation or binding copy may interrupt the cancellation sequence.
  void cancel_locked();
  // Guards every transport field below across worker and TUI calls.
  mutable std::mutex mutex_;
  // Set once by shutdown; later begin/present calls cannot reopen requests.
  bool shutdown_ = false;
  // Set by begin and cleared by cancellation; TUI uses it to retire the deadline timer.
  bool active_ = false;
  // Whether the current submission accepts requests; cancellation clears it.
  bool accepting_ = false;
  // Latched expiry for worker cancellation predicates; begin/finish clear it.
  bool deadline_expired_ = false;
  // One TUI poll reports each expiry; expire sets and poll exchanges it.
  bool expiry_report_pending_ = false;
  // Allocation-free request to clear screen data; cancel sets and TUI poll consumes it.
  bool clear_pending_ = false;
  // Incremented by begin so copied endpoints cannot act on a later submission.
  std::uint64_t generation_ = 0;
  // Current invocation identity, set by begin before accepting requests.
  std::string invocation_id_;
  // Current monotonic deadline; begin sets it and both threads may observe it under lock.
  std::chrono::steady_clock::time_point deadline_{};
  // Current plugin/command/invocation binding; finish/shutdown retire it.
  std::optional<TuiPluginUiBinding> owner_;
  // Worker requests drained by TUI poll; cancellation releases their waiters first.
  std::deque<PluginUiQueueRecord> queue_;
  // Enqueueing a modal stores this share; TUI resolution/cancellation clears it.
  // Keeps a second interactive modal out while the first is queued or visible.
  std::shared_ptr<PendingPluginUiModal> outstanding_modal_;
  // TUI poll stores this share when preparing an interactive modal's view. Resolution
  // or cancellation clears it before worker release; protected by the state mutex.
  std::shared_ptr<PendingPluginUiModal> visible_modal_;
  // One-shot race-test hook, transferred to a worker-local callable before invocation.
  std::function<void()> after_enqueue_for_test_;
  // One-shot race-test hook, transferred to TUI-local storage before invocation.
  std::function<void(std::size_t)> after_queue_swap_for_test_;
  // Copied under lock, invoked outside it; shutdown clears the stored capability.
  TuiWakeup wakeup_;
};

namespace {
// TUI thread: remove only the matching published surfaces. The caller serializes
// transport validation where needed; this helper neither resolves nor owns waits.
bool clear_plugin_surface(RuntimePresentationState& presentation_, std::optional<TuiPluginUiBinding> const& binding = std::nullopt)
{
  bool changed = false;
  if (!binding || (presentation_.screen().plugin_ui_dock && presentation_.screen().plugin_ui_dock->binding == *binding))
  {
    changed = changed || presentation_.screen().plugin_ui_dock.has_value();
    presentation_.publish_plugin_dock(std::nullopt);
  }
  if (!binding || (presentation_.screen().plugin_ui_modal && presentation_.screen().plugin_ui_modal->binding == *binding))
  {
    changed = changed || presentation_.screen().plugin_ui_modal.has_value();
    presentation_.publish_plugin_modal(std::nullopt);
  }
  return changed;
}
} // namespace

std::optional<std::uint64_t> RuntimePluginUiCoordinatorState::begin(TuiPluginUiBinding binding, std::chrono::steady_clock::time_point deadline)
{
  std::lock_guard lock(mutex_);
  if (shutdown_)
    return std::nullopt;
  // Release the previous submission's waits before replacing identity and accepting requests.
  // State-before-modal lock order matches every other transport completion path.
  complete_modal(outstanding_modal_, {.action = TuiPluginUiReplyKind::Cancel, .option_id = {}});
  complete_modal(visible_modal_, {.action = TuiPluginUiReplyKind::Cancel, .option_id = {}});
  for (auto const& record : queue_)
    complete_modal(record.modal, {.action = TuiPluginUiReplyKind::Cancel, .option_id = {}});
  queue_.clear();
  invocation_id_ = binding.invocation_id;
  owner_ = std::move(binding);
  outstanding_modal_.reset();
  visible_modal_.reset();
  clear_pending_ = false;
  active_ = true;
  accepting_ = true;
  deadline_expired_ = false;
  expiry_report_pending_ = false;
  deadline_ = deadline;
  return ++generation_;
}

void RuntimePluginUiCoordinatorState::cancel_locked()
{
  if (!active_ && !accepting_ && !owner_ && !outstanding_modal_ && !visible_modal_)
    return;
  active_ = false;
  accepting_ = false;
  for (auto const& record : queue_)
    complete_modal(record.modal, {.action = TuiPluginUiReplyKind::Cancel, .option_id = {}});
  queue_.clear();
  // Release waiters before the TUI clears screen data. No queue allocation
  // or binding-string copy may stand between cancellation and their wakeup.
  clear_pending_ = true;
  complete_modal(outstanding_modal_, {.action = TuiPluginUiReplyKind::Cancel, .option_id = {}});
  complete_modal(visible_modal_, {.action = TuiPluginUiReplyKind::Cancel, .option_id = {}});
  outstanding_modal_.reset();
  visible_modal_.reset();
}

TuiPluginUiReply RuntimePluginUiCoordinatorState::present(std::uint64_t request_generation, TuiPluginUiRequest const& request,
                                                          std::chrono::steady_clock::time_point request_deadline, TuiPluginUiCancelCallback cancel_requested)
{
  if (!safe_request(request) || callback_canceled(cancel_requested))
    return {.action = TuiPluginUiReplyKind::Cancel, .option_id = {}};

  // The worker and transport keep independent shares of the pending reply.
  // Even status/widget requests wait until the TUI updates screen data, before paint.
  std::shared_ptr<PendingPluginUiModal> modal;
  std::function<void()> after_enqueue;
  TuiWakeup wakeup;
  {
    std::lock_guard lock(mutex_);
    if (shutdown_ || !active_ || !accepting_ || generation_ != request_generation || deadline_ != request_deadline ||
        std::chrono::steady_clock::now() >= request_deadline || request.binding.invocation_id != invocation_id_)
    {
      return {.action = TuiPluginUiReplyKind::Cancel, .option_id = {}};
    }
    if (!owner_ || *owner_ != request.binding || queue_.size() >= kTuiPluginUiMaxQueuedRecords - 1)
      return {.action = TuiPluginUiReplyKind::Cancel, .option_id = {}};

    // Reserve a single interactive modal, then enqueue the owned request and waiter
    // before notifying. No caller-supplied callback runs under this transport lock.
    bool const is_modal = request.kind == TuiPluginUiKind::Select || request.kind == TuiPluginUiKind::Confirm;
    if (is_modal && (outstanding_modal_ || visible_modal_))
      return {.action = TuiPluginUiReplyKind::Cancel, .option_id = {}};
    modal = std::make_shared<PendingPluginUiModal>();
    if (is_modal)
      outstanding_modal_ = modal;
    queue_.push_back(PluginUiQueueRecord{.generation = request_generation, .request = request, .modal = modal});
    after_enqueue = std::exchange(after_enqueue_for_test_, {});
    wakeup = wakeup_;
  }
  wakeup.notify();
  if (after_enqueue)
    after_enqueue();

  // Wait with only the modal mutex, then release it before cancellation callbacks
  // or state checks. This avoids inverting the state-then-modal completion order.
  while (true)
  {
    {
      std::unique_lock lock(modal->mutex);
      if (modal->reply)
        return *modal->reply;
      auto const now = std::chrono::steady_clock::now();
      if (now < request_deadline)
        modal->ready.wait_until(lock, std::min(request_deadline, now + std::chrono::milliseconds(20)));
      if (modal->reply)
        return *modal->reply;
    }
    if (callback_canceled(cancel_requested) || std::chrono::steady_clock::now() >= request_deadline)
    {
      complete_modal(modal, {.action = TuiPluginUiReplyKind::Cancel, .option_id = {}});
      std::lock_guard lock(mutex_);
      if (outstanding_modal_ == modal)
        outstanding_modal_.reset();
      if (visible_modal_ == modal)
        visible_modal_.reset();
      return {.action = TuiPluginUiReplyKind::Cancel, .option_id = {}};
    }
    {
      std::lock_guard lock(mutex_);
      if (shutdown_ || generation_ != request_generation || !active_)
      {
        complete_modal(modal, {.action = TuiPluginUiReplyKind::Cancel, .option_id = {}});
        return {.action = TuiPluginUiReplyKind::Cancel, .option_id = {}};
      }
    }
  }
}

void RuntimePluginUiCoordinatorState::close(std::uint64_t request_generation, TuiPluginUiBinding const& binding)
{
  TuiWakeup wakeup;
  {
    std::lock_guard lock(mutex_);
    if (shutdown_ || generation_ != request_generation || !owner_ || *owner_ != binding)
      return;
    cancel_locked();
    wakeup = wakeup_;
  }
  wakeup.notify();
}

void RuntimePluginUiCoordinatorState::expire(std::chrono::steady_clock::time_point now)
{
  TuiWakeup wakeup;
  {
    std::lock_guard lock(mutex_);
    if (!active_ || now < deadline_)
      return;
    deadline_expired_ = true;
    expiry_report_pending_ = true;
    cancel_locked();
    wakeup = wakeup_;
  }
  wakeup.notify();
}

TuiPluginUiPollResult RuntimePluginUiCoordinatorState::poll(RuntimePresentationState& presentation_, bool host_modal_conflict)
{
  auto const& snapshot = presentation_.screen();
  TuiPluginUiPollResult result;
  std::deque<PluginUiQueueRecord> records;
  std::function<void(std::size_t)> after_queue_swap;
  {
    std::lock_guard lock(mutex_);
    if (std::exchange(clear_pending_, false))
      result.changed = clear_plugin_surface(presentation_) || result.changed;
    result.deadline_expired = std::exchange(expiry_report_pending_, false);
    records.swap(queue_);
    after_queue_swap = std::exchange(after_queue_swap_for_test_, {});
  }
  if (after_queue_swap)
    after_queue_swap(records.size());

  // The batch is detached, so workers may cancel or enqueue while it is examined.
  // Resolve an existing conflicting modal before considering any new record.
  if (host_modal_conflict && snapshot.plugin_ui_modal)
  {
    std::shared_ptr<PendingPluginUiModal> modal;
    {
      std::lock_guard lock(mutex_);
      modal = std::move(visible_modal_);
      if (outstanding_modal_ == modal)
        outstanding_modal_.reset();
    }
    complete_modal(modal, {.action = TuiPluginUiReplyKind::Cancel, .option_id = {}});
    presentation_.publish_plugin_modal(std::nullopt);
    result.changed = true;
  }

  // Revalidate each record under the state lock after the swap. Update screen data
  // and send Ack under that same lock so cancellation cannot race these steps.
  // The caller requests a frame afterward; Ack does not confirm a completed paint.
  for (auto& record : records)
  {
    auto const& request = record.request;
    if (request.kind == TuiPluginUiKind::Status || request.kind == TuiPluginUiKind::Widget)
    {
      std::lock_guard lock(mutex_);
      auto const current = record.generation == generation_ && active_ && accepting_ && owner_ && *owner_ == request.binding && modal_pending(record.modal);
      if (!current)
      {
        complete_modal(record.modal, {.action = TuiPluginUiReplyKind::Cancel, .option_id = {}});
        continue;
      }

      auto candidate = snapshot.plugin_ui_dock && snapshot.plugin_ui_dock->binding == request.binding
                           ? *snapshot.plugin_ui_dock
                           : TuiPluginUiDockView{.binding = request.binding, .status = std::nullopt, .widgets = {}};
      if (request.kind == TuiPluginUiKind::Status)
        candidate.status = request.text;
      else
        candidate.widgets.push_back(TuiPluginUiWidgetView{.title = request.title, .lines = request.lines});

      auto candidate_snapshot = snapshot;
      candidate_snapshot.plugin_ui_dock = candidate;
      auto const geometry = plugin_ui_surface_geometry(candidate_snapshot, request.kind);
      auto const within_limits = candidate.widgets.size() <= kTuiPluginUiMaxWidgets && dock_line_count(candidate) <= kTuiPluginUiMaxWidgetLines &&
                                 dock_text_bytes(candidate) <= kTuiPluginUiMaxTextBytes;
      if (!within_limits)
      {
        complete_modal(record.modal, {.action = TuiPluginUiReplyKind::Cancel, .option_id = {}});
        continue;
      }
      if (!plugin_ui_host_chrome_fits(request.binding, request.kind, geometry.width, geometry.max_lines))
      {
        cancel_locked();
        result.changed = clear_plugin_surface(presentation_, request.binding) || result.changed;
        complete_modal(record.modal, {.action = TuiPluginUiReplyKind::Cancel, .option_id = {}});
        continue;
      }

      presentation_.publish_plugin_dock(std::move(candidate));
      result.changed = true;
      complete_modal(record.modal, {.action = TuiPluginUiReplyKind::Ack, .option_id = {}});
      continue;
    }

    std::lock_guard lock(mutex_);
    auto const current = record.generation == generation_ && active_ && accepting_ && owner_ && *owner_ == request.binding && modal_pending(record.modal);
    if (!current || host_modal_conflict || snapshot.plugin_ui_modal)
    {
      complete_modal(record.modal, {.action = TuiPluginUiReplyKind::Cancel, .option_id = {}});
      if (outstanding_modal_ == record.modal)
        outstanding_modal_.reset();
      continue;
    }

    // Preparing an interactive view keeps its worker waiting for a choice.
    // Confirm defaults to Cancel; only later TUI input may send an affirmative reply.
    auto const selected = request.kind == TuiPluginUiKind::Confirm ? std::size_t{1} : std::size_t{0};
    auto candidate = TuiPluginUiModalView{.binding = request.binding,
                                          .request_id = request.request_id,
                                          .kind = request.kind,
                                          .title = request.title,
                                          .description = request.description,
                                          .options = request.options,
                                          .selected_option = selected};
    auto candidate_snapshot = snapshot;
    candidate_snapshot.plugin_ui_modal = candidate;
    auto const geometry = plugin_ui_surface_geometry(candidate_snapshot, request.kind);
    if (!plugin_ui_host_chrome_fits(request.binding, request.kind, geometry.width, geometry.max_lines))
    {
      cancel_locked();
      result.changed = clear_plugin_surface(presentation_, request.binding) || result.changed;
      complete_modal(record.modal, {.action = TuiPluginUiReplyKind::Cancel, .option_id = {}});
      continue;
    }

    presentation_.publish_plugin_modal(std::move(candidate));
    visible_modal_ = record.modal;
    result.changed = true;
  }
  TuiWakeup wakeup;
  {
    std::lock_guard lock(mutex_);
    // Workers can enqueue after the batch swap. Keep their newer records runnable
    // after this pass releases the transport lock.
    if (!queue_.empty())
      wakeup = wakeup_;
  }
  wakeup.notify();
  return result;
}

void RuntimePluginUiCoordinatorState::resolve_visible(TuiPluginUiReply reply)
{
  std::shared_ptr<PendingPluginUiModal> modal;
  {
    std::lock_guard lock(mutex_);
    modal = std::move(visible_modal_);
    if (outstanding_modal_ == modal)
      outstanding_modal_.reset();
  }
  complete_modal(modal, std::move(reply));
}

bool RuntimePluginUiCoordinatorState::deadline_reached(std::chrono::steady_clock::time_point now) const
{
  std::lock_guard lock(mutex_);
  return deadline_expired_ || (active_ && now >= deadline_);
}

bool RuntimePluginUiCoordinatorState::active() const
{
  std::lock_guard lock(mutex_);
  return active_;
}

void RuntimePluginUiCoordinatorState::cancel()
{
  TuiWakeup wakeup;
  {
    std::lock_guard lock(mutex_);
    cancel_locked();
    wakeup = wakeup_;
  }
  wakeup.notify();
}

void RuntimePluginUiCoordinatorState::finish()
{
  {
    std::lock_guard lock(mutex_);
    cancel_locked();
    queue_.clear();
    owner_.reset();
    outstanding_modal_.reset();
    visible_modal_.reset();
    deadline_expired_ = false;
    expiry_report_pending_ = false;
  }
}

void RuntimePluginUiCoordinatorState::shutdown()
{
  {
    std::lock_guard lock(mutex_);
    shutdown_ = true;
    cancel_locked();
    queue_.clear();
    owner_.reset();
    wakeup_ = {};
  }
}

void RuntimePluginUiCoordinatorState::set_after_enqueue_for_test(std::function<void()> hook)
{
  std::lock_guard lock(mutex_);
  after_enqueue_for_test_ = std::move(hook);
}

void RuntimePluginUiCoordinatorState::set_after_queue_swap_for_test(std::function<void(std::size_t)> hook)
{
  std::lock_guard lock(mutex_);
  after_queue_swap_for_test_ = std::move(hook);
}

// TUI policy query shares the exact parser used to issue endpoints; recognition
// alone does not accept requests or create worker resources.
bool is_direct_foreground_plugin_run_submission(std::string_view submitted)
{
  return direct_foreground_plugin_binding(submitted).has_value();
}

RuntimePluginUiCoordinator::RuntimePluginUiCoordinator(TuiEventLoop& loop, RuntimePresentationState& presentation)
    : presentation_(presentation),
      state_(std::make_shared<RuntimePluginUiCoordinatorState>(loop.wakeup())),
      runtime_token_(std::make_shared<int const>(1)),
      deadline_timer_(loop.member_timer(*state_, &RuntimePluginUiCoordinatorState::expire))
{
}

RuntimePluginUiCoordinator::~RuntimePluginUiCoordinator()
{
  shutdown();
}

std::optional<TuiPluginUiEndpoint> RuntimePluginUiCoordinator::begin_submission(std::string_view submitted, std::string invocation_id,
                                                                                std::chrono::steady_clock::time_point now)
{
  // Validate submission identity before changing transport generation or arming time.
  // Only the accepted submission receives copied worker endpoints.
  auto const binding = direct_foreground_plugin_binding(submitted);
  if (!binding || invocation_id.empty())
    return std::nullopt;
  auto const deadline = now + kTuiPluginUiInvocationDeadline;
  auto const generation = state_->begin({.plugin_id = binding->first, .command = binding->second, .invocation_id = std::move(invocation_id)}, deadline);
  if (!generation)
    return std::nullopt;
  deadline_timer_.arm_at(deadline);
  std::weak_ptr<RuntimePluginUiCoordinatorState> weak_state = state_;
  // Copied endpoints can outlive the coordinator. A call pins transport only;
  // no endpoint captures screen data, timer, or coordinator references.
  return TuiPluginUiEndpoint{.runtime_token = runtime_token_,
                             .deadline = deadline,
                             .present =
                                 [weak_state, generation = *generation](TuiPluginUiRequest const& request, std::chrono::steady_clock::time_point deadline,
                                                                        TuiPluginUiCancelCallback cancel_requested) {
                                   if (auto state = weak_state.lock())
                                     return state->present(generation, request, deadline, std::move(cancel_requested));
                                   return TuiPluginUiReply{.action = TuiPluginUiReplyKind::Cancel, .option_id = {}};
                                 },
                             .close =
                                 [weak_state, generation = *generation](TuiPluginUiBinding const& binding) {
                                   if (auto state = weak_state.lock())
                                     state->close(generation, binding);
                                 }};
}

bool RuntimePluginUiCoordinator::cancel_unfittable_surfaces()
{
  auto const& snapshot = presentation_.screen();
  bool fits = true;
  if (snapshot.plugin_ui_dock)
  {
    auto const kind = snapshot.plugin_ui_dock->widgets.empty() ? TuiPluginUiKind::Status : TuiPluginUiKind::Widget;
    auto const geometry = plugin_ui_surface_geometry(snapshot, kind);
    fits = plugin_ui_host_chrome_fits(snapshot.plugin_ui_dock->binding, kind, geometry.width, geometry.max_lines);
  }
  if (fits && snapshot.plugin_ui_modal)
  {
    auto const geometry = plugin_ui_surface_geometry(snapshot, snapshot.plugin_ui_modal->kind);
    fits = plugin_ui_host_chrome_fits(snapshot.plugin_ui_modal->binding, snapshot.plugin_ui_modal->kind, geometry.width, geometry.max_lines);
  }
  if (fits)
    return false;

  // Close transport and release waiters before clearing their visible surfaces.
  // A buffered key can no longer authorize a choice in the discarded geometry.
  cancel_active();
  deadline_timer_.cancel();
  presentation_.publish_plugin_dock(std::nullopt);
  presentation_.publish_plugin_modal(std::nullopt);
  return true;
}

TuiPluginUiPollResult RuntimePluginUiCoordinator::poll(bool host_modal_conflict)
{
  auto const geometry_changed = cancel_unfittable_surfaces();
  auto result = state_->poll(presentation_, host_modal_conflict);
  result.changed = result.changed || geometry_changed;
  if (!state_->active())
    deadline_timer_.cancel();
  return result;
}

TuiPluginUiInputResult RuntimePluginUiCoordinator::handle_input(InputEvent const& event)
{
  auto const& snapshot = presentation_.screen();
  if (!snapshot.plugin_ui_modal)
    return TuiPluginUiInputResult::Unhandled;

  using Key = terminal::Key;
  if (event.key == Key::CtrlC)
  {
    cancel_active();
    presentation_.publish_plugin_modal(std::nullopt);
    presentation_.publish_plugin_dock(std::nullopt);
    return TuiPluginUiInputResult::RequestStop;
  }

  // Snapshot geometry can lag behind a buffered key after resize. Recheck the
  // renderer's fit rules before navigation or accepting an answer.
  if (cancel_unfittable_surfaces())
    return TuiPluginUiInputResult::Redraw;

  // After geometry validation, navigation only changes the selected display row.
  // An answer remains pending until a separate activation key or pointer click.
  auto const& view = *snapshot.plugin_ui_modal;
  auto const option_count = view.kind == TuiPluginUiKind::Confirm ? std::size_t{2} : view.options.size();
  if (event.key == Key::ArrowUp || event.key == Key::ArrowDown || event.key == Key::PageUp || event.key == Key::PageDown || event.key == Key::Home ||
      event.key == Key::End)
  {
    if (option_count == 0)
      return TuiPluginUiInputResult::Handled;
    auto selected = std::min(view.selected_option, option_count - 1);
    if (event.key == Key::ArrowUp)
      selected = selected == 0 ? option_count - 1 : selected - 1;
    else if (event.key == Key::ArrowDown)
      selected = (selected + 1) % option_count;
    else if (event.key == Key::PageUp || event.key == Key::Home)
      selected = 0;
    else
      selected = option_count - 1;
    presentation_.publish_plugin_selection(selected);
    return TuiPluginUiInputResult::Redraw;
  }

  if (event.key == Key::MouseLeftPress || event.key == Key::MouseLeftClick)
  {
    auto const option = plugin_ui_modal_option_for_screen_position(snapshot, event.mouse_row, event.mouse_column);
    if (!option || *option >= option_count)
      return TuiPluginUiInputResult::Handled;
    presentation_.publish_plugin_selection(*option);
  }
  else if (event.key == Key::MouseLeftDrag || event.key == Key::MouseLeftRelease || event.key == Key::MousePointerCancel || event.key == Key::MouseWheelUp ||
           event.key == Key::MouseWheelDown)
  {
    return TuiPluginUiInputResult::Handled;
  }

  TuiPluginUiReply reply;
  bool resolve = false;
  if (event.key == Key::Escape)
  {
    reply.action = TuiPluginUiReplyKind::Cancel;
    resolve = true;
  }
  else if (event.key == Key::Enter || event.key == Key::MouseLeftPress || event.key == Key::MouseLeftClick)
  {
    if (view.kind == TuiPluginUiKind::Confirm)
      reply.action = view.selected_option == 0 ? TuiPluginUiReplyKind::Confirm : TuiPluginUiReplyKind::Cancel;
    else if (view.kind == TuiPluginUiKind::Select && view.selected_option < view.options.size())
    {
      reply.action = TuiPluginUiReplyKind::Select;
      reply.option_id = view.options[view.selected_option].id;
    }
    resolve = true;
  }
  if (!resolve)
    return TuiPluginUiInputResult::Handled;

  // Copy any selected option identity into the reply before removing the view.
  // Transport detaches its modal ownership before waking the waiting worker.
  state_->resolve_visible(std::move(reply));
  presentation_.publish_plugin_modal(std::nullopt);
  return TuiPluginUiInputResult::Redraw;
}

bool RuntimePluginUiCoordinator::deadline_reached(std::chrono::steady_clock::time_point now) const
{
  return state_->deadline_reached(now);
}

void RuntimePluginUiCoordinator::cancel_active()
{
  state_->cancel();
}

void RuntimePluginUiCoordinator::finish_submission()
{
  deadline_timer_.cancel();
  state_->finish();
  static_cast<void>(clear_plugin_surface(presentation_));
}

void RuntimePluginUiCoordinator::shutdown()
{
  deadline_timer_.cancel();
  state_->shutdown();
  runtime_token_.reset();
  static_cast<void>(clear_plugin_surface(presentation_));
}

void RuntimePluginUiCoordinator::set_after_enqueue_for_test(std::function<void()> hook)
{
  state_->set_after_enqueue_for_test(std::move(hook));
}

void RuntimePluginUiCoordinator::set_after_queue_swap_for_test(std::function<void(std::size_t)> hook)
{
  state_->set_after_queue_swap_for_test(std::move(hook));
}

}  // namespace ava::tui

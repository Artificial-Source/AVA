#include "sys.h"
#include "TuiEventLoop.h"
#include "TuiWakeup.h"
#include "ava/app/terminal_text.h"
#include "ava/tui/composer.h"
#include "ava/tui/runtime_plugin_ui_internal.h"
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

struct PendingPluginUiModal
{
  std::mutex mutex;
  std::condition_variable ready;
  std::optional<TuiPluginUiReply> reply;

  // Internal-linkage synchronization state also contains arbitrary plugin replies.
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

struct PluginUiQueueRecord
{
  std::uint64_t generation = 0;
  TuiPluginUiRequest request;
  // The queue and waiting worker retain independent ownership until resolution.
  std::shared_ptr<PendingPluginUiModal> modal;

  // Internal-linkage records retain arbitrary plugin request/reply content.
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

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

bool safe_text(std::string_view text, std::size_t maximum, bool require_nonempty = false)
{
  return text.size() <= maximum && (!require_nonempty || !text.empty()) && ava::core::json::is_valid_utf8(text) && sanitize_terminal_text(text) == text;
}

bool safe_binding(TuiPluginUiBinding const& binding)
{
  return safe_text(binding.plugin_id, 256, true) && safe_text(binding.command, 256, true) && safe_text(binding.invocation_id, 256, true);
}

bool safe_request(TuiPluginUiRequest const& request)
{
  if (!safe_binding(request.binding) || !safe_text(request.request_id, 256, true) || request.lines.size() > kTuiPluginUiMaxWidgetLines ||
      request.options.size() > 32)
  {
    return false;
  }

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

bool modal_pending(std::shared_ptr<PendingPluginUiModal> const& modal)
{
  if (!modal)
    return false;
  std::lock_guard lock(modal->mutex);
  return !modal->reply;
}

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

std::size_t dock_line_count(TuiPluginUiDockView const& dock)
{
  std::size_t lines = 0;
  for (auto const& widget : dock.widgets)
    lines += widget.lines.size();
  return lines;
}

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

bool canonical_command_name(std::string_view value)
{
  return !value.empty() && value.size() <= 96 && std::ranges::all_of(value, [](char ch) {
    return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '_' || ch == '-' || ch == '.';
  });
}

std::optional<std::pair<std::string, std::string>> direct_foreground_plugin_binding(std::string_view submitted)
{
  constexpr std::string_view prefix = "/plugin run ";
  if (submitted.size() > 64 * 1024 || !submitted.starts_with(prefix) || submitted.empty() || submitted.back() == ' ' ||
      sanitize_terminal_text(submitted) != submitted)
  {
    return std::nullopt;
  }
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

struct RuntimePluginUiCoordinatorState
{
  mutable std::mutex mutex;
  bool shutdown = false;
  bool active = false;
  bool accepting = false;
  bool deadline_expired = false;
  bool expiry_report_pending = false;
  // Cancellation publishes an allocation-free clear request under mutex. The
  // TUI poll consumes it before applying any records from this generation.
  bool clear_pending = false;
  std::uint64_t generation = 0;
  std::string invocation_id;
  std::chrono::steady_clock::time_point deadline{};
  std::optional<TuiPluginUiBinding> owner;
  std::deque<PluginUiQueueRecord> queue;
  std::shared_ptr<PendingPluginUiModal> outstanding_modal;
  std::shared_ptr<PendingPluginUiModal> visible_modal;
  std::function<void()> after_enqueue_for_test;
  std::function<void(std::size_t)> after_queue_swap_for_test;
  TuiWakeup wakeup;

  // Opaque synchronized transport contains arbitrary plugin messages, callback
  // captures and mutex resources; it must not become generated debug output.
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

namespace {

void cancel_locked(RuntimePluginUiCoordinatorState& state)
{
  if (!state.active && !state.accepting && !state.owner && !state.outstanding_modal && !state.visible_modal)
    return;
  state.active = false;
  state.accepting = false;
  for (auto const& record : state.queue)
    complete_modal(record.modal, {.action = TuiPluginUiReplyKind::Cancel, .option_id = {}});
  state.queue.clear();
  // Release waiters before the TUI clears its projection. No queue allocation
  // or binding-string copy may stand between cancellation and their wakeup.
  state.clear_pending = true;
  complete_modal(state.outstanding_modal, {.action = TuiPluginUiReplyKind::Cancel, .option_id = {}});
  complete_modal(state.visible_modal, {.action = TuiPluginUiReplyKind::Cancel, .option_id = {}});
  state.outstanding_modal.reset();
  state.visible_modal.reset();
}

TuiPluginUiReply present_request(std::weak_ptr<RuntimePluginUiCoordinatorState> weak_state, std::uint64_t generation, TuiPluginUiRequest const& request,
                                 std::chrono::steady_clock::time_point deadline, TuiPluginUiCancelCallback cancel_requested)
{
  auto state = weak_state.lock();
  if (!state || !safe_request(request) || callback_canceled(cancel_requested))
    return {.action = TuiPluginUiReplyKind::Cancel, .option_id = {}};

  std::shared_ptr<PendingPluginUiModal> modal;
  std::function<void()> after_enqueue;
  TuiWakeup wakeup;
  {
    std::lock_guard lock(state->mutex);
    if (state->shutdown || !state->active || !state->accepting || state->generation != generation || state->deadline != deadline ||
        std::chrono::steady_clock::now() >= deadline || request.binding.invocation_id != state->invocation_id)
    {
      return {.action = TuiPluginUiReplyKind::Cancel, .option_id = {}};
    }
    if (!state->owner || *state->owner != request.binding || state->queue.size() >= kTuiPluginUiMaxQueuedRecords - 1)
      return {.action = TuiPluginUiReplyKind::Cancel, .option_id = {}};

    bool const is_modal = request.kind == TuiPluginUiKind::Select || request.kind == TuiPluginUiKind::Confirm;
    if (is_modal && (state->outstanding_modal || state->visible_modal))
      return {.action = TuiPluginUiReplyKind::Cancel, .option_id = {}};
    modal = std::make_shared<PendingPluginUiModal>();
    if (is_modal)
      state->outstanding_modal = modal;
    state->queue.push_back(PluginUiQueueRecord{.generation = generation, .request = request, .modal = modal});
    after_enqueue = std::exchange(state->after_enqueue_for_test, {});
    wakeup = state->wakeup;
  }
  wakeup.notify();
  if (after_enqueue)
    after_enqueue();

  while (true)
  {
    {
      std::unique_lock lock(modal->mutex);
      if (modal->reply)
        return *modal->reply;
      auto const now = std::chrono::steady_clock::now();
      if (now < deadline)
        modal->ready.wait_until(lock, std::min(deadline, now + std::chrono::milliseconds(20)));
      if (modal->reply)
        return *modal->reply;
    }
    if (callback_canceled(cancel_requested) || std::chrono::steady_clock::now() >= deadline)
    {
      complete_modal(modal, {.action = TuiPluginUiReplyKind::Cancel, .option_id = {}});
      std::lock_guard lock(state->mutex);
      if (state->outstanding_modal == modal)
        state->outstanding_modal.reset();
      if (state->visible_modal == modal)
        state->visible_modal.reset();
      return {.action = TuiPluginUiReplyKind::Cancel, .option_id = {}};
    }
    {
      std::lock_guard lock(state->mutex);
      if (state->shutdown || state->generation != generation || !state->active)
      {
        complete_modal(modal, {.action = TuiPluginUiReplyKind::Cancel, .option_id = {}});
        return {.action = TuiPluginUiReplyKind::Cancel, .option_id = {}};
      }
    }
  }
}

void close_binding(std::weak_ptr<RuntimePluginUiCoordinatorState> weak_state, std::uint64_t generation, TuiPluginUiBinding const& binding)
{
  auto state = weak_state.lock();
  if (!state)
    return;
  TuiWakeup wakeup;
  {
    std::lock_guard lock(state->mutex);
    if (state->shutdown || state->generation != generation || !state->owner || *state->owner != binding)
      return;
    cancel_locked(*state);
    wakeup = state->wakeup;
  }
  wakeup.notify();
}

}  // namespace

bool is_direct_foreground_plugin_run_submission(std::string_view submitted)
{
  return direct_foreground_plugin_binding(submitted).has_value();
}

RuntimePluginUiCoordinator::RuntimePluginUiCoordinator(ComposerSnapshot& snapshot)
    : snapshot_(snapshot), state_(std::make_shared<RuntimePluginUiCoordinatorState>()), runtime_token_(std::make_shared<int const>(1))
{
}

RuntimePluginUiCoordinator::~RuntimePluginUiCoordinator()
{
  shutdown();
}

void RuntimePluginUiCoordinator::attach_loop(TuiEventLoop& loop)
{
  // Attach this nonmoving coordinator exactly once, before opening a submission.
  ASSERT(!loop_attached_);
  if (loop_attached_)
    return;
  {
    std::lock_guard lock(state_->mutex);
    // Install scheduling before begin_submission, and never reattach a shut-down coordinator.
    ASSERT(!state_->active && !state_->shutdown);
    if (state_->active || state_->shutdown)
      return;
    state_->wakeup = loop.wakeup();
  }
  deadline_timer_ = loop.member_timer(*this, &RuntimePluginUiCoordinator::expire_submission);
  loop_attached_ = true;
}

void RuntimePluginUiCoordinator::expire_submission(std::chrono::steady_clock::time_point now)
{
  TuiWakeup wakeup;
  {
    std::lock_guard lock(state_->mutex);
    if (!state_->active || now < state_->deadline)
      return;
    state_->deadline_expired = true;
    state_->expiry_report_pending = true;
    cancel_locked(*state_);
    wakeup = state_->wakeup;
  }
  wakeup.notify();
}

std::optional<TuiPluginUiEndpoint> RuntimePluginUiCoordinator::begin_submission(std::string_view submitted, std::string invocation_id,
                                                                                std::chrono::steady_clock::time_point now)
{
  auto const binding = direct_foreground_plugin_binding(submitted);
  if (!binding || invocation_id.empty())
    return std::nullopt;
  auto const deadline = now + kTuiPluginUiInvocationDeadline;
  std::uint64_t generation = 0;
  {
    std::lock_guard lock(state_->mutex);
    if (state_->shutdown)
      return std::nullopt;
    complete_modal(state_->outstanding_modal, {.action = TuiPluginUiReplyKind::Cancel, .option_id = {}});
    complete_modal(state_->visible_modal, {.action = TuiPluginUiReplyKind::Cancel, .option_id = {}});
    for (auto const& record : state_->queue)
      complete_modal(record.modal, {.action = TuiPluginUiReplyKind::Cancel, .option_id = {}});
    state_->queue.clear();
    state_->owner = TuiPluginUiBinding{.plugin_id = binding->first, .command = binding->second, .invocation_id = invocation_id};
    state_->outstanding_modal.reset();
    state_->visible_modal.reset();
    state_->clear_pending = false;
    state_->active = true;
    state_->accepting = true;
    state_->deadline_expired = false;
    state_->expiry_report_pending = false;
    state_->invocation_id = std::move(invocation_id);
    state_->deadline = deadline;
    generation = ++state_->generation;
  }
  deadline_timer_.arm_at(deadline);
  std::weak_ptr<RuntimePluginUiCoordinatorState> weak_state = state_;
  return TuiPluginUiEndpoint{.runtime_token = runtime_token_,
                             .deadline = deadline,
                             .present = std::bind_front(present_request, weak_state, generation),
                             .close = std::bind_front(close_binding, weak_state, generation)};
}

bool RuntimePluginUiCoordinator::clear_surface(std::optional<TuiPluginUiBinding> const& binding)
{
  bool changed = false;
  if (!binding || (snapshot_.plugin_ui_dock && snapshot_.plugin_ui_dock->binding == *binding))
  {
    changed = changed || snapshot_.plugin_ui_dock.has_value();
    snapshot_.plugin_ui_dock.reset();
  }
  if (!binding || (snapshot_.plugin_ui_modal && snapshot_.plugin_ui_modal->binding == *binding))
  {
    changed = changed || snapshot_.plugin_ui_modal.has_value();
    snapshot_.plugin_ui_modal.reset();
  }
  return changed;
}

bool RuntimePluginUiCoordinator::cancel_unfittable_surfaces()
{
  auto& snapshot = snapshot_;
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

  cancel_active();
  deadline_timer_.cancel();
  snapshot.plugin_ui_dock.reset();
  snapshot.plugin_ui_modal.reset();
  return true;
}

TuiPluginUiPollResult RuntimePluginUiCoordinator::poll(bool host_modal_conflict, std::chrono::steady_clock::time_point now)
{
  auto& snapshot = snapshot_;
  TuiPluginUiPollResult result;
  result.changed = cancel_unfittable_surfaces();
  std::deque<PluginUiQueueRecord> records;
  std::function<void(std::size_t)> after_queue_swap;
  {
    std::lock_guard lock(state_->mutex);
    // Standalone fixtures retain explicit-clock servicing. An attached owner
    // receives expiry only from its registered member timer.
    if (!loop_attached_ && state_->active && now >= state_->deadline)
    {
      state_->deadline_expired = true;
      state_->expiry_report_pending = true;
      cancel_locked(*state_);
    }
    if (std::exchange(state_->clear_pending, false))
      result.changed = clear_surface() || result.changed;
    result.deadline_expired = std::exchange(state_->expiry_report_pending, false);
    records.swap(state_->queue);
    after_queue_swap = std::exchange(state_->after_queue_swap_for_test, {});
  }
  if (after_queue_swap)
    after_queue_swap(records.size());

  if (host_modal_conflict && snapshot.plugin_ui_modal)
  {
    std::shared_ptr<PendingPluginUiModal> modal;
    {
      std::lock_guard lock(state_->mutex);
      modal = std::move(state_->visible_modal);
      if (state_->outstanding_modal == modal)
        state_->outstanding_modal.reset();
    }
    complete_modal(modal, {.action = TuiPluginUiReplyKind::Cancel, .option_id = {}});
    snapshot.plugin_ui_modal.reset();
    result.changed = true;
  }

  for (auto& record : records)
  {
    auto const& request = record.request;
    if (request.kind == TuiPluginUiKind::Status || request.kind == TuiPluginUiKind::Widget)
    {
      std::lock_guard lock(state_->mutex);
      auto const current = record.generation == state_->generation && state_->active && state_->accepting && state_->owner &&
                           *state_->owner == request.binding && modal_pending(record.modal);
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
        cancel_locked(*state_);
        result.changed = clear_surface(request.binding) || result.changed;
        complete_modal(record.modal, {.action = TuiPluginUiReplyKind::Cancel, .option_id = {}});
        continue;
      }

      snapshot.plugin_ui_dock = std::move(candidate);
      result.changed = true;
      complete_modal(record.modal, {.action = TuiPluginUiReplyKind::Ack, .option_id = {}});
      continue;
    }

    std::lock_guard lock(state_->mutex);
    auto const current = record.generation == state_->generation && state_->active && state_->accepting && state_->owner && *state_->owner == request.binding &&
                         modal_pending(record.modal);
    if (!current || host_modal_conflict || snapshot.plugin_ui_modal)
    {
      complete_modal(record.modal, {.action = TuiPluginUiReplyKind::Cancel, .option_id = {}});
      if (state_->outstanding_modal == record.modal)
        state_->outstanding_modal.reset();
      continue;
    }

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
      cancel_locked(*state_);
      result.changed = clear_surface(request.binding) || result.changed;
      complete_modal(record.modal, {.action = TuiPluginUiReplyKind::Cancel, .option_id = {}});
      continue;
    }

    snapshot.plugin_ui_modal = std::move(candidate);
    state_->visible_modal = record.modal;
    result.changed = true;
  }
  TuiWakeup wakeup;
  bool active;
  {
    std::lock_guard lock(state_->mutex);
    active = state_->active;
    // A rejected surface may publish Clear while consuming this batch. Keep
    // that later record runnable after releasing the mailbox lock.
    if (!state_->queue.empty())
      wakeup = state_->wakeup;
  }
  if (!active)
    deadline_timer_.cancel();
  wakeup.notify();
  return result;
}

TuiPluginUiInputResult RuntimePluginUiCoordinator::handle_input(InputEvent const& event)
{
  auto& snapshot = snapshot_;
  if (!snapshot.plugin_ui_modal)
    return TuiPluginUiInputResult::Unhandled;

  using Key = terminal::Key;
  if (event.key == Key::CtrlC)
  {
    cancel_active();
    snapshot.plugin_ui_modal.reset();
    snapshot.plugin_ui_dock.reset();
    return TuiPluginUiInputResult::RequestStop;
  }

  // Snapshot geometry is not input authority: resize/poll can lag behind a
  // buffered key. Recheck the exact renderer fit before navigation or resolve.
  if (cancel_unfittable_surfaces())
    return TuiPluginUiInputResult::Redraw;

  auto& view = *snapshot.plugin_ui_modal;
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
    view.selected_option = selected;
    return TuiPluginUiInputResult::Redraw;
  }

  if (event.key == Key::MouseLeftPress || event.key == Key::MouseLeftClick)
  {
    auto const option = plugin_ui_modal_option_for_screen_position(snapshot, event.mouse_row, event.mouse_column);
    if (!option || *option >= option_count)
      return TuiPluginUiInputResult::Handled;
    view.selected_option = *option;
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

  std::shared_ptr<PendingPluginUiModal> modal;
  {
    std::lock_guard lock(state_->mutex);
    modal = std::move(state_->visible_modal);
    if (state_->outstanding_modal == modal)
      state_->outstanding_modal.reset();
  }
  complete_modal(modal, std::move(reply));
  snapshot.plugin_ui_modal.reset();
  return TuiPluginUiInputResult::Redraw;
}

bool RuntimePluginUiCoordinator::deadline_reached(std::chrono::steady_clock::time_point now) const
{
  std::lock_guard lock(state_->mutex);
  return state_->deadline_expired || (state_->active && now >= state_->deadline);
}

void RuntimePluginUiCoordinator::cancel_active()
{
  TuiWakeup wakeup;
  {
    std::lock_guard lock(state_->mutex);
    cancel_locked(*state_);
    wakeup = state_->wakeup;
  }
  wakeup.notify();
}

void RuntimePluginUiCoordinator::finish_submission()
{
  deadline_timer_.cancel();
  {
    std::lock_guard lock(state_->mutex);
    cancel_locked(*state_);
    state_->queue.clear();
    state_->owner.reset();
    state_->outstanding_modal.reset();
    state_->visible_modal.reset();
    state_->deadline_expired = false;
    state_->expiry_report_pending = false;
  }
  static_cast<void>(clear_surface());
}

void RuntimePluginUiCoordinator::shutdown()
{
  deadline_timer_.cancel();
  {
    std::lock_guard lock(state_->mutex);
    state_->shutdown = true;
    cancel_locked(*state_);
    state_->queue.clear();
    state_->owner.reset();
    state_->wakeup = {};
  }
  runtime_token_.reset();
  static_cast<void>(clear_surface());
}

void RuntimePluginUiCoordinator::set_after_enqueue_for_test(std::function<void()> hook)
{
  std::lock_guard lock(state_->mutex);
  state_->after_enqueue_for_test = std::move(hook);
}

void RuntimePluginUiCoordinator::set_after_queue_swap_for_test(std::function<void(std::size_t)> hook)
{
  std::lock_guard lock(state_->mutex);
  state_->after_queue_swap_for_test = std::move(hook);
}

}  // namespace ava::tui

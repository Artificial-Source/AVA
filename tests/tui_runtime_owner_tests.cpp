// Direct contracts for the active-run mailbox and ordered TUI input dispatch.
#include "sys.h"
#include "tests/support/test_harness.h"
#include "tests/support/test_timeout.h"
#include "ava/tui/ActiveRunInbox.h"
#include "ava/tui/ActiveRunOutcome.h"
#include "ava/tui/TuiEventLoop.h"
#include "ava/tui/TuiInputDispatcher.h"
#include "ava/tui/runtime_input_internal.h"
#include "ava/core/UniqueFd.h"

#include <array>
#include <chrono>
#include <cstddef>
#include <exception>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>
#include <fcntl.h>
#include <unistd.h>

namespace {

using ava::tui::ActiveRunInbox;
using ava::tui::TuiInputDispatcher;
using ava::tui::TuiInputOutcome;
using ava::tui::runtime_input::RuntimeInput;

// Shared ownership is required because producer endpoints retain the inbox after its UI owner leaves.
std::shared_ptr<ActiveRunInbox> make_inbox(ava::tui::TuiWakeup wakeup = {})
{
  return std::make_shared<ActiveRunInbox>(std::move(wakeup));
}

// Give each runtime publication an identifiable payload and metadata without a backend.
ava::event::RuntimeEvent message_event(std::string text)
{
  ava::event::MessagePayload payload;
  payload.text = text;
  return {{.timestamp = std::move(text), .session_id = "inbox-session"}, ava::event::MessageUpdateEvent{.payload = std::move(payload)}};
}

// Compare public queued values, including the request context captured at publication.
bool is_message(ava::tui::QueuedTuiEvent const& event, std::string const& text, std::string const& request)
{
  auto const* queued = std::get_if<ava::tui::QueuedRuntimeEvent>(&event);
  auto const* message = queued ? std::get_if<ava::event::MessageUpdateEvent>(&queued->event.payload()) : nullptr;
  return message && message->payload.text == text && queued->event.metadata().timestamp == text && queued->event.metadata().session_id == "inbox-session" &&
         queued->context.request_id == request && queued->context.correlation_id == request;
}

// Own both descriptors so an idle input source remains open for the whole loop lifetime.
class RuntimeOwnerPipe final
{
 public:
  // Acquire and configure both pipe ends, leaving valid() false if setup fails.
  RuntimeOwnerPipe()
  {
    std::array<int, 2> descriptors{-1, -1};
    if (::pipe(descriptors.data()) != 0)
      return;
    read_.reset(descriptors[0]);
    write_.reset(descriptors[1]);
    valid_ = ::fcntl(read_.get(), F_SETFL, O_NONBLOCK) == 0 && ::fcntl(write_.get(), F_SETFL, O_NONBLOCK) == 0 &&
             ::fcntl(read_.get(), F_SETFD, FD_CLOEXEC) == 0 && ::fcntl(write_.get(), F_SETFD, FD_CLOEXEC) == 0;
  }

  // Report whether construction produced a fully configured input fixture.
  [[nodiscard]] bool valid() const { return valid_; }
  // Borrow the read descriptor while this fixture retains both pipe ends.
  [[nodiscard]] int input() const { return read_.get(); }

  // This anonymous-namespace fixture has internal linkage, so generated member printing cannot define it.
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT

 private:
  ava::core::UniqueFd read_; // Input descriptor borrowed by the event loop.
  ava::core::UniqueFd write_; // Retained writer prevents an unrelated hangup wake.
  bool valid_ = false; // Both descriptors must have nonblocking and close-on-exec flags.
};

// A fallback timer bounds lost-notification failures without a polling loop or worker.
class WakeDeadline final
{
 public:
  // Record a fallback deadline firing without depending on its wall-clock timestamp.
  void expire(std::chrono::steady_clock::time_point) { ++expirations_; }
  // Return the number of fallback callbacks observed by the synchronous test.
  [[nodiscard]] std::size_t expirations() const { return expirations_; }

  // This anonymous-namespace fixture has internal linkage, so generated member printing cannot define it.
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT

 private:
  std::size_t expirations_ = 0; // A successful publication wakes before this callback is due.
};

// Bounded drains preserve mixed publication order, captured request context, and receive history.
void test_inbox_bounded_fifo_and_received_history()
{
  auto inbox = make_inbox();
  auto event_sink = inbox->event_sink();
  auto envelope_sink = inbox->envelope_sink();
  auto launch_sink = inbox->subagent_launch_sink();
  expect(!inbox->has_pending() && !inbox->received_any() && !inbox->terminal_ready() && inbox->drain().empty(),
         "a new inbox has no pending work or receive history");

  inbox->set_request_id("request-first");
  expect(event_sink(message_event("first")).has_value(), "the inbox accepts its first runtime publication");
  ava::event::EventEnvelope envelope;
  envelope.event_id = "control-between";
  envelope.name = "permission_replied";
  envelope.request_id = "envelope-request";
  envelope.correlation_id = "envelope-correlation";
  envelope.payload_json = R"({"decision":"allow"})";
  expect(envelope_sink(envelope).has_value(), "the inbox accepts an already contextualized envelope");
  launch_sink({.tool_call_id = "launch-between", .request_id = "launch-request", .correlation_id = "launch-correlation", .display = {}});
  inbox->set_request_id("request-last");
  expect(event_sink(message_event("last")).has_value(), "the inbox accepts a runtime publication after the request changes");

  expect(inbox->drain(0).empty() && inbox->has_pending() && inbox->received_any(), "a zero budget preserves all queued work and receive history");
  auto first = inbox->drain(2);
  if (!ava::tests::require(first.size() == 2, "a two-event budget returns exactly the first two publications"))
    return;
  expect(is_message(first[0], "first", "request-first"), "the first runtime publication retains its original request context");
  auto const* control = std::get_if<ava::event::EventEnvelope>(&first[1]);
  expect(control && control->event_id == envelope.event_id && control->name == envelope.name && control->request_id == envelope.request_id &&
             control->correlation_id == envelope.correlation_id && control->payload_json == envelope.payload_json && inbox->has_pending(),
         "the second publication preserves the envelope unchanged while later events remain pending");

  auto middle = inbox->drain(1);
  auto const* launch = middle.size() == 1 ? std::get_if<ava::agent::SubagentLaunchNotification>(&middle[0]) : nullptr;
  expect(launch && launch->tool_call_id == "launch-between" && launch->request_id == "launch-request" && launch->correlation_id == "launch-correlation" &&
             inbox->has_pending(),
         "the next bounded drain returns exactly the subagent launch notification");
  auto last = inbox->drain();
  expect(last.size() == 1 && is_message(last[0], "last", "request-last") && !inbox->has_pending() && inbox->received_any(),
         "the final drain preserves the new request context and historical receipt without reporting pending work");
  expect(inbox->drain(2).empty() && inbox->received_any(), "draining an empty inbox leaves receive history intact");
}

// Discard clears event storage and receive history while preserving an independently published terminal.
void test_inbox_discard_preserves_terminal()
{
  auto inbox = make_inbox();
  auto sink = inbox->event_sink();
  expect(sink(message_event("discarded")).has_value(), "the inbox accepts an event before discard");
  ava::tui::TuiSubmitResult result;
  result.output = {"kept terminal"};
  inbox->publish_terminal(std::move(result));
  inbox->discard();
  expect(inbox->drain().empty() && !inbox->received_any() && inbox->has_pending() && inbox->terminal_ready(),
         "discard resets queued-event history but leaves terminal work pending");
  auto terminal = inbox->take_terminal();
  auto const* submitted = terminal ? std::get_if<ava::tui::TuiSubmitResult>(&*terminal) : nullptr;
  expect(submitted && submitted->output == std::vector<std::string>{"kept terminal"} && !inbox->has_pending(),
         "the terminal payload survives event discard unchanged");
  expect(sink(message_event("after discard")).has_value() && inbox->received_any() && inbox->has_pending(),
         "discard keeps admission open and new publications establish new receive history");
}

// Retirement rejects every late producer route while retaining previously accepted events and terminal data.
void test_inbox_retirement_preserves_accepted_work()
{
  auto inbox = make_inbox();
  auto events = inbox->event_sink();
  auto envelopes = inbox->envelope_sink();
  auto launches = inbox->subagent_launch_sink();
  std::size_t follow_up_calls = 0;
  auto follow_up = inbox->wrap_follow_up_started([&follow_up_calls](ava::tui::TuiQueuedFollowUp const&) -> ava::core::VoidResult {
    ++follow_up_calls;
    return {};
  });
  inbox->set_request_id("accepted-request");
  expect(events(message_event("accepted")).has_value(), "the inbox accepts a runtime event before retirement");
  ava::event::EventEnvelope envelope;
  envelope.event_id = "accepted-envelope";
  expect(envelopes(envelope).has_value(), "the inbox accepts an envelope before retirement");
  launches({.tool_call_id = "accepted-launch", .request_id = {}, .correlation_id = {}, .display = {}});
  ava::tui::TuiSubmitResult result;
  result.output = {"accepted terminal"};
  inbox->publish_terminal(std::move(result));

  inbox->retire();
  inbox->retire();
  inbox->set_request_id("late-request");
  expect(events(message_event("late")).has_value(), "a retired event endpoint acknowledges its ignored publication");
  envelope.event_id = "late-envelope";
  expect(envelopes(envelope).has_value(), "a retired envelope endpoint acknowledges its ignored publication");
  launches({.tool_call_id = "late-launch", .request_id = {}, .correlation_id = {}, .display = {}});
  expect(follow_up({.request_id = "late-follow-up", .message = {}}).has_value() && follow_up_calls == 0,
         "retirement prevents a wrapped follow-up callback from running");
  inbox->publish_terminal(std::make_exception_ptr(std::runtime_error("late terminal")));

  auto accepted = inbox->drain();
  if (!ava::tests::require(accepted.size() == 3, "retirement retains exactly the three accepted events"))
    return;
  auto const* accepted_envelope = std::get_if<ava::event::EventEnvelope>(&accepted[1]);
  auto const* accepted_launch = std::get_if<ava::agent::SubagentLaunchNotification>(&accepted[2]);
  expect(is_message(accepted[0], "accepted", "accepted-request") && accepted_envelope && accepted_envelope->event_id == "accepted-envelope" &&
             accepted_launch && accepted_launch->tool_call_id == "accepted-launch" && inbox->received_any() && inbox->terminal_ready(),
         "retirement preserves accepted event order, identities, request context, and terminal readiness");
  auto terminal = inbox->take_terminal();
  auto const* submitted = terminal ? std::get_if<ava::tui::TuiSubmitResult>(&*terminal) : nullptr;
  expect(submitted && submitted->output == std::vector<std::string>{"accepted terminal"} && !inbox->has_pending(),
         "retirement retains the accepted terminal and late producers cannot replenish the drained inbox");
}

// The first submit result wins permanently, including after its terminal value has been consumed.
void test_inbox_first_submit_terminal_wins_after_take()
{
  auto inbox = make_inbox();
  ava::tui::TuiSubmitResult first;
  first.quit = true;
  first.output = {"first terminal"};
  inbox->publish_terminal(std::move(first));
  inbox->publish_terminal(std::make_exception_ptr(std::runtime_error("duplicate terminal")));
  expect(inbox->terminal_ready() && inbox->has_pending() && !inbox->received_any(), "terminal readiness is independent of historical event receipt");
  auto terminal = inbox->take_terminal();
  auto const* submitted = terminal ? std::get_if<ava::tui::TuiSubmitResult>(&*terminal) : nullptr;
  expect(submitted && submitted->quit && submitted->output == std::vector<std::string>{"first terminal"},
         "the first submit result survives a later exception publication");
  expect(!inbox->terminal_ready() && !inbox->has_pending() && !inbox->take_terminal(), "taking a terminal consumes its readiness exactly once");
  inbox->publish_terminal(ava::tui::TuiSubmitResult{});
  inbox->publish_terminal(std::make_exception_ptr(std::runtime_error("after consumption")));
  expect(!inbox->terminal_ready() && !inbox->has_pending() && !inbox->take_terminal(),
         "consuming a terminal never reopens terminal publication for another result or exception");
}

// The first exception retains its exact identity and also blocks publication after consumption.
void test_inbox_first_exception_terminal_wins_after_take()
{
  auto inbox = make_inbox();
  auto first = std::make_exception_ptr(std::runtime_error("first exception"));
  inbox->publish_terminal(first);
  inbox->publish_terminal(ava::tui::TuiSubmitResult{});
  auto terminal = inbox->take_terminal();
  auto const* error = terminal ? std::get_if<std::exception_ptr>(&*terminal) : nullptr;
  expect(error && *error == first, "an exception published first survives a duplicate submit result with its identity intact");
  inbox->publish_terminal(std::make_exception_ptr(std::runtime_error("second exception")));
  expect(!inbox->take_terminal() && !inbox->terminal_ready() && !inbox->has_pending(), "consuming the first exception does not admit a second exception");
}

// Stop requests latch once without inventing queued work or retiring producer admission.
void test_inbox_stop_request_is_idempotent()
{
  auto inbox = make_inbox();
  expect(!inbox->stop_requested(), "a new inbox has no stop request");
  expect(inbox->request_stop() && inbox->stop_requested(), "the first stop request reports and records a transition");
  expect(!inbox->request_stop() && inbox->stop_requested() && !inbox->request_stop(), "subsequent stop requests remain latched and report no transition");
  expect(!inbox->has_pending() && !inbox->received_any() && !inbox->terminal_ready(), "a stop request does not fabricate events or terminal data");
  auto sink = inbox->event_sink();
  expect(sink(message_event("after stop")).has_value() && inbox->has_pending(),
         "a stop request leaves accepted producer delivery available for final draining");
  inbox->retire();
  expect(!inbox->request_stop() && inbox->stop_requested(), "retirement preserves the existing stop latch");
}

// Event and terminal publications wake a real loop with their payloads already available to the consumer.
void test_inbox_publication_wakes_event_loop()
{
  RuntimeOwnerPipe pipe;
  if (!ava::tests::require(pipe.valid(), "create the inbox wake test input pipe"))
    return;
  ava::tui::TuiEventLoop loop(pipe.input());
  WakeDeadline fallback;
  auto deadline = loop.member_timer(fallback, &WakeDeadline::expire);
  auto inbox = make_inbox(loop.wakeup());
  auto sink = inbox->event_sink();
  inbox->set_request_id("wake-request");
  deadline.arm_at(ava::tests::now_plus_seconds(2));
  expect(sink(message_event("wake event")).has_value(), "publish an event before the loop begins waiting");
  loop.wait(ava::tui::TuiWaitMode::Block);
  loop.service_due();
  auto events = inbox->drain();
  expect(fallback.expirations() == 0 && !loop.notification_failed() && !loop.terminal_error() && !loop.terminal_hangup() && events.size() == 1 &&
             is_message(events[0], "wake event", "wake-request"),
         "the event notification wakes the idle loop before its fallback with the exact publication available");

  deadline.arm_at(ava::tests::now_plus_seconds(2));
  ava::tui::TuiSubmitResult result;
  result.output = {"wake terminal"};
  inbox->publish_terminal(std::move(result));
  loop.wait(ava::tui::TuiWaitMode::Block);
  loop.service_due();
  auto terminal = inbox->take_terminal();
  auto const* submitted = terminal ? std::get_if<ava::tui::TuiSubmitResult>(&*terminal) : nullptr;
  expect(fallback.expirations() == 0 && !loop.notification_failed() && submitted && submitted->output == std::vector<std::string>{"wake terminal"},
         "terminal publication produces a fresh wake with its complete payload available");
}

// Retained producer capabilities outlive both their UI owner and waiter and release the inbox with the final endpoint.
void test_inbox_retained_endpoints_outlive_owner_and_loop()
{
  RuntimeOwnerPipe pipe;
  if (!ava::tests::require(pipe.valid(), "create the retained inbox endpoint input pipe"))
    return;
  std::size_t follow_up_calls = 0;
  std::weak_ptr<ActiveRunInbox> lifetime;
  ava::event::RuntimeEventSink events;
  ava::event::EventEnvelopeSink envelopes;
  ava::agent::SubagentLaunchSink launches;
  ActiveRunInbox::FollowUpStarted follow_up;
  {
    ava::tui::TuiEventLoop loop(pipe.input());
    auto inbox = make_inbox(loop.wakeup());
    lifetime = inbox;
    events = inbox->event_sink();
    envelopes = inbox->envelope_sink();
    launches = inbox->subagent_launch_sink();
    follow_up = inbox->wrap_follow_up_started([&follow_up_calls](ava::tui::TuiQueuedFollowUp const&) -> ava::core::VoidResult {
      ++follow_up_calls;
      return {};
    });
  }
  expect(!lifetime.expired(), "producer endpoints retain the inbox after its UI owner and loop are destroyed");
  auto copied_events = events;
  expect(follow_up({.request_id = "retained-request", .message = {}}).has_value() && follow_up_calls == 1,
         "a retained follow-up endpoint still invokes its live callback after waiter destruction");
  expect(copied_events(message_event("retained event")).has_value(), "a copied event endpoint safely publishes through an expired wake capability");
  ava::event::EventEnvelope envelope;
  envelope.event_id = "retained-envelope";
  expect(envelopes(envelope).has_value(), "a retained envelope endpoint safely publishes after waiter destruction");
  launches({.tool_call_id = "retained-launch", .request_id = {}, .correlation_id = {}, .display = {}});
  {
    // Temporarily share ownership to inspect the public consumer API before the final endpoint release.
    auto inbox = lifetime.lock();
    if (!ava::tests::require(static_cast<bool>(inbox), "retained endpoints keep the inbox available for inspection"))
      return;
    auto accepted = inbox->drain();
    auto const* accepted_envelope = accepted.size() == 3 ? std::get_if<ava::event::EventEnvelope>(&accepted[1]) : nullptr;
    auto const* accepted_launch = accepted.size() == 3 ? std::get_if<ava::agent::SubagentLaunchNotification>(&accepted[2]) : nullptr;
    expect(accepted.size() == 3 && is_message(accepted[0], "retained event", "retained-request") && accepted_envelope &&
               accepted_envelope->event_id == "retained-envelope" && accepted_launch && accepted_launch->tool_call_id == "retained-launch",
           "all retained producer routes preserve publication order and follow-up context after waiter destruction");
    inbox->retire();
    expect(copied_events(message_event("late retained event")).has_value(), "a retained event endpoint remains safe after retirement");
    expect(envelopes(envelope).has_value(), "a retained envelope endpoint remains safe after retirement");
    launches({.tool_call_id = "late retained launch", .request_id = {}, .correlation_id = {}, .display = {}});
    expect(follow_up({.request_id = "late-request", .message = {}}).has_value() && follow_up_calls == 1 && !inbox->has_pending(),
           "retired retained endpoints neither invoke callbacks nor add events");
  }
  events = {};
  envelopes = {};
  launches = {};
  follow_up = {};
  expect(!lifetime.expired(), "the final copied producer alone retains the inbox");
  copied_events = {};
  expect(lifetime.expired(), "releasing the last producer destroys the inbox without retaining its expired waiter");
}

// Passing stages run in construction order and receive the original runtime input unchanged.
void test_dispatcher_pass_falls_through_in_stage_order()
{
  RuntimeInput input;
  input.event.character = 'x';
  input.event.text = "event text";
  input.text = "runtime text";
  input.bracketed_paste = true;
  input.resize = true;
  std::vector<int> calls;
  bool same_input = true;
  auto stage = [&](int index) -> TuiInputDispatcher::Handler {
    return [&, index](RuntimeInput const& observed) {
      calls.push_back(index);
      same_input = same_input && &observed == &input && observed.event.character == 'x' && observed.event.text == "event text" &&
                   observed.text == "runtime text" && observed.bracketed_paste && observed.resize;
      return TuiInputOutcome::Pass;
    };
  };
  TuiInputDispatcher dispatcher({stage(1), stage(2), stage(3)});
  expect(dispatcher.dispatch(input) == TuiInputOutcome::Pass && calls == std::vector<int>{1, 2, 3} && same_input,
         "all passing stages receive the same complete input in their declared priority order");
  TuiInputDispatcher empty({});
  expect(empty.dispatch(input) == TuiInputOutcome::Pass, "a dispatcher without stages leaves input unhandled");
}

// Every non-Pass outcome terminates dispatch at the consuming stage and is returned unchanged.
void test_dispatcher_stops_on_handled_failure_and_exit()
{
  RuntimeInput input;
  for (auto outcome : {TuiInputOutcome::Handled, TuiInputOutcome::Failure, TuiInputOutcome::Exit})
  {
    std::vector<int> calls;
    TuiInputDispatcher dispatcher({[&](RuntimeInput const&) {
                                     calls.push_back(1);
                                     return TuiInputOutcome::Pass;
                                   },
                                   [&](RuntimeInput const&) {
                                     calls.push_back(2);
                                     return outcome;
                                   },
                                   [&](RuntimeInput const&) {
                                     calls.push_back(3);
                                     return TuiInputOutcome::Handled;
                                   }});
    ava::tests::expect_eq(dispatcher.dispatch(input), outcome, "dispatch returns the exact consuming stage outcome");
    expect(calls == std::vector<int>{1, 2}, "Handled, Failure, and Exit each prevent all lower-priority stages from running");
  }
}

// Optional event-handler results adapt to Pass, Handled, or Failure and preserve dispatcher short-circuiting.
void test_dispatcher_optional_event_handler_adaptation()
{
  RuntimeInput input;
  input.event.character = 'q';
  input.event.text = "modal event";
  input.event.mouse_column = 7;
  input.event.mouse_row = 9;
  auto const responses = std::array<std::optional<bool>, 3>{std::nullopt, true, false};
  auto const outcomes = std::array{TuiInputOutcome::Pass, TuiInputOutcome::Handled, TuiInputOutcome::Failure};
  for (std::size_t index = 0; index < responses.size(); ++index)
  {
    std::vector<int> calls;
    bool same_event = false;
    auto adapted = TuiInputDispatcher::event_handler([&](ava::tui::InputEvent const& event) {
      calls.push_back(1);
      same_event = &event == &input.event && event.character == 'q' && event.text == "modal event" && event.mouse_column == 7 && event.mouse_row == 9;
      return responses[index];
    });
    TuiInputDispatcher dispatcher({std::move(adapted), [&](RuntimeInput const&) {
                                     calls.push_back(2);
                                     return TuiInputOutcome::Pass;
                                   }});
    ava::tests::expect_eq(dispatcher.dispatch(input), outcomes[index], "optional event-handler values map to the expected dispatch outcome");
    auto const expected_calls = responses[index].has_value() ? std::vector<int>{1} : std::vector<int>{1, 2};
    expect(same_event && calls == expected_calls, "adaptation forwards the original event exactly once and only nullopt reaches the next stage");
  }
}

// Named settlement decisions retain both failure causes regardless of annotation order.
void test_active_run_outcomes_preserve_signal_and_write_failure()
{
  using ava::tui::ActiveRunOutcome;
  auto const continued = ActiveRunOutcome::continue_session();
  auto const closed = ActiveRunOutcome::close_session();
  auto const failed_then_signal = continued.with_terminal_write_failure().with_terminal_signal();
  auto const signal_then_failed = closed.with_terminal_signal().with_terminal_write_failure();
  expect(!continued.break_loop() && !continued.terminal_write_failed() && !continued.terminal_signal_received(),
         "successful settlement continues without inventing an exit cause");
  expect(closed.break_loop() && !closed.terminal_write_failed() && !closed.terminal_signal_received(),
         "ordinary close remains distinct from terminal failure and signal exit");
  expect(failed_then_signal.break_loop() && failed_then_signal.terminal_write_failed() && failed_then_signal.terminal_signal_received() &&
             signal_then_failed.break_loop() && signal_then_failed.terminal_write_failed() && signal_then_failed.terminal_signal_received(),
         "a terminal failure and signal both survive named outcome composition");
}

} // namespace

// Register the direct owner contracts as one focused test suite.
void run_tui_runtime_owner_tests()
{
  test_active_run_outcomes_preserve_signal_and_write_failure();
  test_inbox_bounded_fifo_and_received_history();
  test_inbox_discard_preserves_terminal();
  test_inbox_retirement_preserves_accepted_work();
  test_inbox_first_submit_terminal_wins_after_take();
  test_inbox_first_exception_terminal_wins_after_take();
  test_inbox_stop_request_is_idempotent();
  test_inbox_publication_wakes_event_loop();
  test_inbox_retained_endpoints_outlive_owner_and_loop();
  test_dispatcher_pass_falls_through_in_stage_order();
  test_dispatcher_stops_on_handled_failure_and_exit();
  test_dispatcher_optional_event_handler_adaptation();
}

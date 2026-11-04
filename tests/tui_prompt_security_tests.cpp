// Exercises prompt input boundaries and callback locking through real TUI owners.
#include "sys.h"
#include "support/test_harness.h"
#include "support/test_timeout.h"
#include "support/tui_test_support.h"
#include "ava/tui/Composer.h"
#include "ava/tui/TuiEventLoop.h"
#include "ava/tui/composer_internal.h"
#include "ava/tui/runtime_input_internal.h"
#include "ava/tui/runtime_prompts_internal.h"
#include "ava/tui/runtime_render_internal.h"
#include "ava/tui/runtime_state_internal.h"
#include "ava/tui/session_grants.h"
#include "ava/tui/terminal.h"
#include "ava/core/Application.h"
#include "ava/core/UniqueFd.h"
#include "ava/core/thread.h"

#include <array>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <future>
#include <mutex>
#include <string>
#include <system_error>
#include <utility>
#include <unistd.h>

namespace {

// The watchdog cannot unwind a thread deadlocked inside the coordinator mutex.
[[noreturn]] void fail_prompt_deadline()
{
  std::fputs("prompt security test exceeded its worker or callback deadline\n", stderr);
  std::_Exit(EXIT_FAILURE);
}

// A separate thread bounds reentrant calls and teardown even if the TUI deadlocks.
class PromptDeadlockWatchdog final
{
 public:
  // The stable target owns its synchronization before the watchdog thread starts.
  PromptDeadlockWatchdog()
      : worker_(ava::core::JoinThread::create("prompt_security_watchdog", [this, deadline = ava::tests::now_plus_seconds(10)] {
          std::unique_lock lock(mutex_);
          if (!changed_.wait_until(lock, deadline, [this] { return finished_; }))
            fail_prompt_deadline();
        }))
  {
  }

  // Signal completion before the last-declared thread joins and state is destroyed.
  ~PromptDeadlockWatchdog()
  {
    {
      std::lock_guard lock(mutex_);
      finished_ = true;
    }
    changed_.notify_all();
  }

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT

 private:
  // Shared only by the test and watchdog; the predicate uses this mutex.
  std::mutex mutex_;
  // Owned notification channel remains alive until the watchdog joins.
  std::condition_variable changed_;
  // Protected completion latch; only the test thread publishes true.
  bool finished_ = false;
  // Declared last so destruction joins before any borrowed synchronization dies.
  ava::core::JoinThread worker_;
};

// An open pipe supplies real readiness without consuming the ncurses input file.
class EventLoopFixture final
{
 public:
  // Descriptor ownership starts before the scheduler borrows the read end.
  EventLoopFixture() : descriptors_(open_pipe()), loop_(descriptors_.first.get()) { }
  // Test-thread borrow; all feature owners retire before this fixture.
  [[nodiscard]] ava::tui::TuiEventLoop& loop() { return loop_; }

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT

 private:
  // Fail construction before exposing an unusable scheduler descriptor.
  static std::pair<ava::core::UniqueFd, ava::core::UniqueFd> open_pipe()
  {
    std::array<int, 2> descriptors{-1, -1};
    if (::pipe(descriptors.data()) != 0)
      throw std::system_error(errno, std::generic_category(), "create prompt security event loop pipe");
    return {ava::core::UniqueFd(descriptors[0]), ava::core::UniqueFd(descriptors[1])};
  }

  // Test-thread descriptors outlive the loop and retain the unused write end.
  std::pair<ava::core::UniqueFd, ava::core::UniqueFd> descriptors_;
  // Scheduler destruction precedes descriptor retirement.
  ava::tui::TuiEventLoop loop_;
};

// Stack-owned timer target; the local handle retires before this target dies.
struct PublicationDeadline
{
  // Only the TUI thread services and reads this timeout latch.
  void expire(ava::tui::TuiTimer::Clock::time_point) { expired = true; }
  // TUI-only state, never accessed by the publishing worker.
  bool expired = false;

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

// Own the blocking resolver until shutdown releases it and a bounded wait joins it.
class PromptWorker final
{
 public:
  // Resolver captures own their prompts; coordinator must outlive this worker.
  PromptWorker(ava::tui::RuntimePromptCoordinator& coordinator, std::function<bool()> resolve)
      : coordinator_(coordinator), result_(ava::core::make_async("prompt_security_resolver", std::move(resolve)))
  {
  }

  // Failed assertions and early returns still retire callbacks before joining.
  ~PromptWorker()
  {
    coordinator_.shutdown();
    if (result_.valid())
      await_ready();
  }

  // Test thread: observe result publication without blocking or consuming it.
  [[nodiscard]] bool waiting() { return result_.wait_for(std::chrono::milliseconds::zero()) == std::future_status::timeout; }

  // Shutdown guarantees cancellation on failure; get only follows bounded readiness.
  [[nodiscard]] bool finish()
  {
    coordinator_.shutdown();
    await_ready();
    return result_.get();
  }

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT

 private:
  // The earlier-declared fixture keeps this borrowed coordinator alive through join.
  ava::tui::RuntimePromptCoordinator& coordinator_;
  // Sole async ownership; destruction occurs only after readiness is established.
  std::future<bool> result_;

  // A stuck resolver fails the process instead of hanging in future destruction.
  void await_ready()
  {
    if (result_.wait_until(ava::tests::now_plus_seconds(2)) != std::future_status::ready)
      fail_prompt_deadline();
  }
};

// Supply a session-eligible recipe so 's' exercises a real grant shortcut.
ava::permissions::PermissionPrompt permission_prompt()
{
  ava::permissions::CommandPermissionMetadata metadata;
  metadata.level = ava::command::CommandLevel::Standard;
  metadata.containment_available = true;
  metadata.containment_status = ava::permissions::CommandContainmentStatus::Available;
  metadata.backend_maximum_scope = ava::command::InteractiveScope::Session;
  metadata.global_recipe_key = "prompt-security-global";
  metadata.workspace_recipe_key = "prompt-security-workspace";
  metadata.effective_allowed_scopes = {ava::command::InteractiveScope::Once, ava::command::InteractiveScope::Session};
  return {.permission_request_id = "prompt-security-request",
          .operation = ava::permissions::Operation::RunCommand,
          .mode = ava::core::Mode::Build,
          .workspace_dir = {},
          .target_path = {},
          .command = "echo SECURITY-PERMISSION-MARKER",
          .tool_name = "bash",
          .reason = "prompt security regression",
          .command_metadata = std::move(metadata)};
}

// All UI owners stay on the test thread; PromptWorker is declared after this fixture.
struct PromptSecurityFixture
{
  // Initialize the Application terminal exactly as the existing rendering fixtures do.
  PromptSecurityFixture()
      : presentation(frontend.frontend(), {}),
        renderer(frontend.frontend(), presentation, composer),
        coordinator(*frontend.frontend(), presentation, grants, renderer, event_loop.loop())
  {
    static_cast<void>(ava::core::Application::instance().terminal_context());
    static_cast<void>(ava::tui::terminal::Context::resizeterm(24, 120));
    presentation.finish_session_transition("prompt-security-session");
    presentation.publish_dimensions(120, 24);
    renderer.attach_loop(event_loop.loop());
    coordinator.open_admission();
  }

  // Own every resolver capture by value; worker teardown precedes fixture teardown.
  [[nodiscard]] PromptWorker worker(bool permission)
  {
    if (permission)
      return PromptWorker(coordinator, [resolve = coordinator.permission_resolver(), prompt = permission_prompt()] {
        auto answer = resolve(prompt);
        return answer && answer->resolution == ava::permissions::PermissionResolution::Allow;
      });
    return PromptWorker(coordinator, [resolve = coordinator.question_resolver()] {
      ava::agent::QuestionPrompt prompt{.header = "SECURITY-QUESTION-MARKER",
                                        .question = "Choose deliberately",
                                        .options = {{.value = "first", .label = "First"}, {.value = "second", .label = "Second"}}};
      auto answer = resolve(prompt);
      return answer && answer->selected_options.size() == 1 && answer->selected_options.front() == "first";
    });
  }

  // Wait for the producer's real wakeup, with a timer bounding missed publication.
  [[nodiscard]] bool start()
  {
    PublicationDeadline deadline;
    auto timeout = event_loop.loop().member_timer(deadline, &PublicationDeadline::expire);
    timeout.arm_at(ava::tests::now_plus_seconds(2));
    while (!coordinator.has_pending_requests() && !deadline.expired)
    {
      event_loop.loop().service_due();
      if (coordinator.has_pending_requests() || deadline.expired || event_loop.loop().terminal_hangup() || event_loop.loop().terminal_error() ||
          event_loop.loop().notification_failed())
        break;
      event_loop.loop().wait(ava::tui::TuiWaitMode::Block);
    }
    return coordinator.has_pending_requests() && coordinator.start_pending() == ava::tui::PromptServiceOutcome::Active;
  }

  // Inspect actual painted rows without servicing a pending frame on the test's behalf.
  [[nodiscard]] bool painted_prompt(bool permission) const
  {
    auto const& frame = renderer.painted_frame();
    auto const marker = permission ? "SECURITY-PERMISSION-MARKER" : "SECURITY-QUESTION-MARKER";
    return frame.valid && tui_test_support::join_visible_lines(frame.surfaces).find(marker) != std::string::npos;
  }

  // Consume setup resize events under the same quarantine as Session. A bounded
  // drain must reach actual Empty before the fixture can acknowledge it.
  [[nodiscard]] bool drain()
  {
    for (std::size_t count = 0; count < 32; ++count)
    {
      auto const polled = ava::tui::runtime_input::poll_curses_input();
      if (polled.kind == ava::tui::runtime_input::InputPollKind::Empty)
        return coordinator.input_drained();
      if (polled.kind == ava::tui::runtime_input::InputPollKind::Input)
        static_cast<void>(coordinator.handle_input(polled.input));
    }
    return false;
  }

  // Scheduler and descriptors outlive every timer below.
  EventLoopFixture event_loop;
  // Application session/frontend registration outlives projection and renderer.
  tui_test_support::TuiFrontEndFixture frontend;
  // TUI-only screen projection borrowed by renderer and coordinator.
  ava::tui::RuntimePresentationState presentation;
  // TUI-only editor borrowed by renderer until its timers retire.
  ava::tui::Composer composer;
  // Attached renderer dies after coordinator but before its loop and projection.
  ava::tui::RuntimeRenderer renderer;
  // Grant authority remains valid until coordinator destruction.
  ava::tui::TuiSessionGrantRegistry grants;
  // Last UI owner; resolver workers must retire before fixture destruction.
  ava::tui::RuntimePromptCoordinator coordinator;

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

// Build an owned event; newline represents a deliberate Enter key, never paste.
ava::tui::runtime_input::RuntimeInput prompt_key(char key)
{
  ava::tui::runtime_input::RuntimeInput input;
  input.event.key = key == '\n' ? ava::tui::terminal::Key::Enter : ava::tui::terminal::Key::Character;
  if (key != '\n')
  {
    input.event.character = key;
    input.event.text = std::string(1, key);
    input.text = std::string(1, key);
  }
  return input;
}

// Each stale key receives its own worker so an earlier rejection cannot hide a later shortcut.
void test_prompt_quarantines_retained_key(bool permission, char key)
{
  PromptDeadlockWatchdog watchdog;
  PromptSecurityFixture fixture;
  auto worker = fixture.worker(permission);
  if (!ava::tests::require(fixture.start(), "queued security prompt starts before its deadline"))
    return;
  expect(fixture.painted_prompt(permission), "attached renderer paints the prompt synchronously before input");
  if (permission)
  {
    auto const& view = fixture.presentation.screen().permission_prompt;
    expect(view && view->allow_session_available, "permission fixture offers the session grant shortcut");
  }
  auto const retained = fixture.coordinator.handle_input(prompt_key(key));
  expect(retained == ava::tui::PromptServiceOutcome::Active && fixture.coordinator.has_active() && worker.waiting(),
         "pre-drain permission/question shortcut leaves its prompt and worker active");
  expect(fixture.grants.size() == 0, "quarantined input does not create a session grant");
  if (permission)
  {
    auto const& view = fixture.presentation.screen().permission_prompt;
    expect(view && view->selected_choice == ava::tui::PermissionPromptChoice::Deny && !view->guidance_mode && view->guidance_text.empty(),
           "quarantined input preserves the permission prompt's initial rejection choice");
  }
  if (!ava::tests::require(fixture.drain(), "actual decoder emptiness arms deliberate prompt input"))
    return;
  auto const deliberate = fixture.coordinator.handle_input(prompt_key(permission ? 'a' : '1'));
  expect(deliberate == ava::tui::PromptServiceOutcome::Completed && !fixture.coordinator.has_active(), "fresh deliberate key resolves after drainage");
  expect(!fixture.painted_prompt(permission), "prompt answer clears the painted modal synchronously");
  expect(worker.finish(), "worker receives the fresh deliberate answer after prompt retirement");
}

// Paste begins after visibility and drainage, isolating paste policy from quarantine.
void test_prompt_rejects_pasted_shortcut(bool permission, char key)
{
  PromptDeadlockWatchdog watchdog;
  PromptSecurityFixture fixture;
  auto worker = fixture.worker(permission);
  if (!ava::tests::require(fixture.start() && fixture.painted_prompt(permission) && fixture.drain(), "paste test starts with a painted, armed prompt"))
    return;
  auto pasted = prompt_key(key);
  pasted.bracketed_paste = true;
  auto const outcome = fixture.coordinator.handle_input(pasted);
  expect(outcome == ava::tui::PromptServiceOutcome::Active && fixture.coordinator.has_active() && worker.waiting(),
         "pasted a/s/1 never acts as an answer shortcut on a visible prompt");
  expect(fixture.grants.size() == 0, "pasted permission shortcut does not create session authority");
  auto const deliberate = fixture.coordinator.handle_input(prompt_key(permission ? 'a' : '1'));
  expect(deliberate == ava::tui::PromptServiceOutcome::Completed, "paste rejection preserves later deliberate input");
  expect(worker.finish(), "resolver returns the deliberate answer after paste rejection");
}

// The callback borrows a fixture that outlives synchronous dispatch; watchdog bounds lock regressions.
void test_search_callback_can_reenter_prompt_queue()
{
  PromptDeadlockWatchdog watchdog;
  PromptSecurityFixture fixture;
  auto const outcome =
      fixture.coordinator.dispatch_search_input_with_prompt_precedence([coordinator = &fixture.coordinator] { return !coordinator->has_pending_requests(); });
  expect(outcome == ava::tui::SearchInputPromptDispatchResult::InputHandled, "search callback can query pending prompts without deadlocking admission");
}

}  // namespace

// Each case owns and retires its worker before the next terminal fixture starts.
void run_tui_prompt_security_tests()
{
  for (char key : {'a', 's', '\n'})
    test_prompt_quarantines_retained_key(true, key);
  for (char key : {'1', '\n'})
    test_prompt_quarantines_retained_key(false, key);
  for (bool permission : {true, false})
    for (char key : {'a', 's', '1'})
      test_prompt_rejects_pasted_shortcut(permission, key);
  test_search_callback_can_reenter_prompt_queue();
}

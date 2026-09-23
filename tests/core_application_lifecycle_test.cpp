#include "sys.h"
#include "ava/core/Application.h"
#include "ava/core/thread.h"

#include <csignal>
#include <cstdlib>
#include <future>
#include <iostream>
#include <optional>
#include <string_view>
#include <fcntl.h>
#include <pthread.h>
#include <sys/resource.h>
#ifdef __linux__
#include <sys/prctl.h>
#endif
#include <unistd.h>

namespace {

constexpr int lifecycle_child_setup_failed = 125;
constexpr int lifecycle_child_still_dumpable = 126;

int failures = 0;

// Record a scenario failure without aborting so ordinary lifecycle cleanup can still be exercised.
void check(bool condition, std::string_view description)
{
  if (condition)
    return;
  ++failures;
  std::cerr << "FAIL: " << description << '\n';
}

// Supply the minimal concrete Application used by one isolated lifecycle scenario.
class TestApplication final : public ava::core::Application
{
 public:
  TestApplication() : ava::core::Application(CWDEBUG_ONLY(false)) { }
  [[nodiscard]] std::string_view application_name() const noexcept override { return "test application"; }
};

// Provide a recognizable unrelated disposition without performing non-async-signal-safe work.
void unrelated_signal_handler(int) noexcept
{
}

// Read one process-wide signal disposition for contract checks.
struct sigaction read_signal_action(int signal_number)
{
  struct sigaction action{};
  check(::sigaction(signal_number, nullptr, &action) == 0, "sigaction reads the requested disposition");
  return action;
}

// Install a simple handler with a non-default flag so preservation checks include the complete disposition.
void install_test_signal_action(int signal_number)
{
  struct sigaction action{};
  action.sa_handler = unrelated_signal_handler;
  action.sa_flags = SA_RESTART;
  ::sigemptyset(&action.sa_mask);
  check(::sigaction(signal_number, &action, nullptr) == 0, "sigaction installs the test disposition");
}

// Return the calling thread's mask for main- and worker-thread inheritance checks.
sigset_t current_thread_signal_mask()
{
  sigset_t mask{};
  check(::pthread_sigmask(SIG_SETMASK, nullptr, &mask) == 0, "pthread_sigmask reads the current thread signal mask");
  return mask;
}

// Convert sigismember's tri-state result into a scenario predicate.
bool signal_is_blocked(sigset_t const& mask, int signal_number)
{
  return ::sigismember(&mask, signal_number) == 1;
}

// Establish unrelated mask and disposition state that Application must preserve.
void establish_unrelated_signal_state()
{
  install_test_signal_action(SIGUSR1);
  sigset_t mask = current_thread_signal_mask();
  ::sigaddset(&mask, SIGUSR1);
  ::sigdelset(&mask, SIGUSR2);
  check(::pthread_sigmask(SIG_SETMASK, &mask, nullptr) == 0, "test precondition installs unrelated signal mask bits");
}

// Disable core handling before a scenario intentionally triggers a lifecycle assertion.
int prepare_expected_death()
{
  rlimit const core_limit{.rlim_cur = 0, .rlim_max = 0};
  if (::setrlimit(RLIMIT_CORE, &core_limit) != 0)
    return lifecycle_child_setup_failed;
#ifdef __linux__
  if (::prctl(PR_SET_DUMPABLE, 0L, 0L, 0L, 0L) != 0)
    return lifecycle_child_setup_failed;
  if (::prctl(PR_GET_DUMPABLE, 0L, 0L, 0L, 0L) != 0)
    return lifecycle_child_still_dumpable;
#endif
  return EXIT_SUCCESS;
}

// Verify that a live Application publishes its concrete process singleton.
void scenario_initialized_instance()
{
  TestApplication application;
  auto const& instance = ava::core::Application::instance();
  check(&instance == &application && instance.application_name() == "test application",
        "Application publishes its initialized concrete instance and virtual application name");
}

// Exercise the contract that singleton access before Application construction is rejected.
void scenario_instance_before_initialize()
{
  static_cast<void>(ava::core::Application::instance());
}

// Exercise the contract that singleton access after ordinary destruction is rejected.
void scenario_instance_after_destruction()
{
  {
    TestApplication application;
  }
  static_cast<void>(ava::core::Application::instance());
}

// Exercise the contract that constructing a second simultaneously live Application is rejected.
void scenario_duplicate_live_instance()
{
  TestApplication first;
  TestApplication second;
  static_cast<void>(first);
  static_cast<void>(second);
}

// Verify signal construction, pending delivery, activation, and atomic claims in one Application lifetime.
void scenario_signal_construction()
{
  establish_unrelated_signal_state();
  struct sigaction default_action{};
  default_action.sa_handler = SIG_DFL;
  ::sigemptyset(&default_action.sa_mask);
  check(::sigaction(SIGINT, &default_action, nullptr) == 0 && ::sigaction(SIGTERM, &default_action, nullptr) == 0,
        "signal construction starts with default foreground dispositions");

  TestApplication application;
  auto const interrupt_action = read_signal_action(SIGINT);
  auto const terminate_action = read_signal_action(SIGTERM);
  auto const unrelated_action = read_signal_action(SIGUSR1);
  auto const startup_mask = current_thread_signal_mask();
  check(interrupt_action.sa_handler == terminate_action.sa_handler && interrupt_action.sa_handler != SIG_DFL && interrupt_action.sa_handler != SIG_IGN,
        "Application construction installs the same custom handler for SIGINT and SIGTERM");
  check(unrelated_action.sa_handler == unrelated_signal_handler && (unrelated_action.sa_flags & SA_RESTART) != 0,
        "Application construction preserves an unrelated signal disposition");
  check(signal_is_blocked(startup_mask, SIGINT) && signal_is_blocked(startup_mask, SIGTERM) && signal_is_blocked(startup_mask, SIGUSR1) &&
            !signal_is_blocked(startup_mask, SIGUSR2),
        "Application construction blocks foreground signals without changing unrelated mask bits");

  check(::raise(SIGINT) == 0 && ::raise(SIGTERM) == 0, "foreground signals can be raised while startup keeps them blocked");
  sigset_t pending{};
  check(::sigpending(&pending) == 0 && ::sigismember(&pending, SIGINT) == 1 && ::sigismember(&pending, SIGTERM) == 1,
        "both foreground signals remain kernel-pending before activation");
  check(!ava::core::Signals::signal_received(ava::core::Signals::bit_SIGINT | ava::core::Signals::bit_SIGTERM),
        "blocked pending signals have not reached the installed handler before activation");

  application.signals_manager().activate_handlers();
  auto const active_mask = current_thread_signal_mask();
  check(!signal_is_blocked(active_mask, SIGINT) && !signal_is_blocked(active_mask, SIGTERM) && signal_is_blocked(active_mask, SIGUSR1) &&
            !signal_is_blocked(active_mask, SIGUSR2),
        "activation unblocks only SIGINT and SIGTERM in its calling thread");
  check(ava::core::Signals::signal_received(ava::core::Signals::bit_SIGINT | ava::core::Signals::bit_SIGTERM),
        "activation delivers both pending signals to the installed handler");
  check(ava::core::Signals::clear_signal(ava::core::Signals::bit_SIGINT) && ava::core::Signals::signal_received(ava::core::Signals::bit_SIGTERM) &&
            !ava::core::Signals::clear_signal(ava::core::Signals::bit_SIGINT) && ava::core::Signals::clear_signal(ava::core::Signals::bit_SIGTERM),
        "atomic signal claim clears only its selected bit and a second claim is false");
}

// Verify ordinary teardown blocks and ignores foreground signals, preserves unrelated state, and clears recorded bits.
void scenario_signal_teardown()
{
  establish_unrelated_signal_state();
  install_test_signal_action(SIGINT);
  install_test_signal_action(SIGTERM);
  {
    TestApplication application;
    auto const interrupt_action = read_signal_action(SIGINT);
    auto const terminate_action = read_signal_action(SIGTERM);
    check(interrupt_action.sa_handler == terminate_action.sa_handler && interrupt_action.sa_handler != unrelated_signal_handler,
          "Application construction replaces prior foreground dispositions with its shared handler");
    check(!ava::core::Signals::signal_received(ava::core::Signals::bit_SIGINT | ava::core::Signals::bit_SIGTERM),
          "Application construction starts without recorded signal bits");
    application.signals_manager().activate_handlers();
    check(::raise(SIGINT) == 0 && ::raise(SIGTERM) == 0, "the Application handler receives both foreground signals");
    check(ava::core::Signals::signal_received(ava::core::Signals::bit_SIGINT | ava::core::Signals::bit_SIGTERM),
          "the Application handler records both foreground signals");
    // Leave both bits set so ordinary destruction must establish the clear-bit postcondition.
  }

  auto const interrupt_action = read_signal_action(SIGINT);
  auto const terminate_action = read_signal_action(SIGTERM);
  auto const unrelated_action = read_signal_action(SIGUSR1);
  auto const teardown_mask = current_thread_signal_mask();
  check(interrupt_action.sa_handler == SIG_IGN && terminate_action.sa_handler == SIG_IGN,
        "ordinary Application destruction leaves both foreground dispositions ignored");
  check(signal_is_blocked(teardown_mask, SIGINT) && signal_is_blocked(teardown_mask, SIGTERM) && signal_is_blocked(teardown_mask, SIGUSR1) &&
            !signal_is_blocked(teardown_mask, SIGUSR2),
        "ordinary Application destruction blocks foreground signals without changing unrelated mask bits");
  check(unrelated_action.sa_handler == unrelated_signal_handler && (unrelated_action.sa_flags & SA_RESTART) != 0,
        "ordinary Application destruction preserves an unrelated signal disposition");
  check(!ava::core::Signals::signal_received(ava::core::Signals::bit_SIGINT | ava::core::Signals::bit_SIGTERM) &&
            !ava::core::Signals::clear_signal(ava::core::Signals::bit_SIGINT) && !ava::core::Signals::clear_signal(ava::core::Signals::bit_SIGTERM),
        "ordinary Application destruction clears both received-signal bits");
}

// Verify a production worker inherits startup blocking and is joined before its Application is destroyed.
void scenario_joined_worker()
{
  TestApplication application;
  std::promise<sigset_t> startup_mask_promise;
  std::future<sigset_t> startup_mask_future = startup_mask_promise.get_future();
  std::promise<void> main_activated_promise;
  std::shared_future<void> main_activated = main_activated_promise.get_future().share();
  std::promise<sigset_t> active_mask_promise;
  std::future<sigset_t> active_mask_future = active_mask_promise.get_future();
  std::promise<void> release_promise;
  std::shared_future<void> release = release_promise.get_future().share();

  auto worker = ava::core::JoinThread::create("signal_lifecycle_worker", [&] {
    startup_mask_promise.set_value(current_thread_signal_mask());
    main_activated.wait();
    active_mask_promise.set_value(current_thread_signal_mask());
    release.wait();
  });
  auto const worker_startup_mask = startup_mask_future.get();
  application.signals_manager().activate_handlers();
  main_activated_promise.set_value();
  auto const worker_active_mask = active_mask_future.get();
  auto const main_active_mask = current_thread_signal_mask();
  check(signal_is_blocked(worker_startup_mask, SIGINT) && signal_is_blocked(worker_startup_mask, SIGTERM),
        "worker inherits both foreground signals blocked from Application construction");
  check(signal_is_blocked(worker_active_mask, SIGINT) && signal_is_blocked(worker_active_mask, SIGTERM) && !signal_is_blocked(main_active_mask, SIGINT) &&
            !signal_is_blocked(main_active_mask, SIGTERM),
        "main-thread activation leaves the worker blocked while unblocking the caller");
  release_promise.set_value();
  worker.join();
  check(!worker.joinable(), "the production worker is joined before Application destruction");
}

#if CW_DEBUG && defined(__linux__)
// Verify destroying Application while a synchronized worker remains live triggers the single-thread invariant.
void scenario_live_worker_destruction()
{
  std::optional<TestApplication> application;
  application.emplace();
  std::promise<void> ready_promise;
  std::future<void> ready = ready_promise.get_future();
  std::promise<void> release_promise;
  std::shared_future<void> release = release_promise.get_future().share();
  auto worker = ava::core::JoinThread::create("live_application_destructor_worker", [&] {
    ready_promise.set_value();
    release.wait();
  });
  ready.get();
  application.reset();
  // If the target destructor returns, release and join so thread destruction cannot produce a false-positive abort.
  release_promise.set_value();
  worker.join();
}
#endif

// Return whether the named scenario intentionally exercises an abort contract.
bool is_expected_death_scenario(std::string_view scenario)
{
  return scenario == "instance-before-initialize" || scenario == "instance-after-destruction" || scenario == "duplicate-live-instance" ||
         scenario == "live-worker-destruction";
}

// Dispatch exactly one lifecycle scenario so every Application starts in a fresh process.
bool run_scenario(std::string_view scenario)
{
  if (scenario == "initialized-instance")
    scenario_initialized_instance();
  else if (scenario == "instance-before-initialize")
    scenario_instance_before_initialize();
  else if (scenario == "instance-after-destruction")
    scenario_instance_after_destruction();
  else if (scenario == "duplicate-live-instance")
    scenario_duplicate_live_instance();
  else if (scenario == "signal-construction")
    scenario_signal_construction();
  else if (scenario == "signal-teardown")
    scenario_signal_teardown();
  else if (scenario == "joined-worker")
    scenario_joined_worker();
#if CW_DEBUG && defined(__linux__)
  else if (scenario == "live-worker-destruction")
    scenario_live_worker_destruction();
#endif
  else
    return false;
  return true;
}

} // namespace

int main(int argc, char** argv)
{
  if (argc != 2)
  {
    std::cerr << "usage: ava_core_application_lifecycle_test <scenario>\n";
    return 2;
  }

  std::string_view const scenario = argv[1];
  if (is_expected_death_scenario(scenario))
  {
    int const setup_result = prepare_expected_death();
    if (setup_result != EXIT_SUCCESS)
      return setup_result;
  }
  if (!run_scenario(scenario))
  {
    std::cerr << "unknown lifecycle scenario: " << scenario << '\n';
    return 2;
  }
  return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}

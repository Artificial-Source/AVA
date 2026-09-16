#pragma once

#include "debug.h"

// ava/core/thread.h
//
// Centralized thread creation that always registers the new thread with libcwd
// before any user code runs on it.
//
// libcwd requires that, as the very first thing a new thread does, it calls
//   Debug(NAMESPACE_DEBUG::init_thread("<label>"));
// so that debug output can be attributed to a named thread. Spelling that line
// out at the top of every worker lambda is error-prone (easy to forget on a new
// thread) and was previously only done at the entry of run_rpc_loop, which is no
// longer a reliable thread entry point since it is also called on the main
// thread.
//
// Instead, all AVA thread creation goes through the helpers in this header:
//
//   ava::core::JoinThread::create("run_rpc_loop", [](std::stop_token stop) { ... });
//   ava::core::make_thread("acp_writer",    [this] { writer_loop(); });
//   ava::core::make_async("openai_browser", [&] { return do_oauth(...); });
//
// They inject init_thread() as the literal first statement of the new thread,
// and the label is mandatory by construction: there is no overload that lets a
// thread be started through this API without being named.
//
// init_thread() itself is defined out-of-line in thread.cpp (the only place that
// includes libcwd's debug.h and expands the Debug(...) macro), so this header
// stays free of any libcwd dependency. The helpers therefore work unchanged
// whether or not libcwd (CWDEBUG) is enabled: with libcwd disabled the macro
// expands to nothing and init_thread() is a no-op.

#include <array>
#include <cstddef>
#include <future>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#ifdef CWDEBUG
#include <source_location>
#include <threadsafe/AIMutex.h>
#endif
#include "debug.h"      // DEBUG_ONLY

namespace ava::core {

#ifdef CWDEBUG
namespace detail {
inline constexpr std::size_t max_nested_long_wait_incompatible_locks = 32;
using utils::has_print_on::operator<<;
struct LongWaitIncompatibleLockRegistry
{
  std::array<void const*, max_nested_long_wait_incompatible_locks> locks{};
  std::size_t count = 0;

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT

  void print_on(std::ostream& os) const
  {
    char const* sep = "";
    for (size_t i = 0; i < count; ++i)
    {
      os << sep << locks[i];
      sep = ", ";
    }
  }
};
inline thread_local LongWaitIncompatibleLockRegistry long_wait_incompatible_locks;
}  // namespace detail

// Register a debug-only lock whose ownership forbids potentially unbounded waits on the current thread.
//
// The lock identity must remain alive until unregister_long_wait_incompatible_lock is called. Registration is allocation-free and supports nesting
// different locks; registering more than 32 locks or unregistering an unowned lock is an invariant failure.
inline void register_long_wait_incompatible_lock(void const* lock)
{
  auto& registry = detail::long_wait_incompatible_locks;
  // More locks were registered than the fixed registry holds; register fewer simultaneous locks on this thread or enlarge
  // LongWaitIncompatibleLockRegistry::locks.
  ASSERT(registry.count < registry.locks.size());
  registry.locks[registry.count++] = lock;
}

// Unregister a debug-only lock previously registered by the current thread.
//
// Locks may be released in a different order from acquisition. The lock identity must currently be registered exactly once.
inline void unregister_long_wait_incompatible_lock(void const* lock)
{
  auto& registry = detail::long_wait_incompatible_locks;
  std::size_t index = 0;
  while (index < registry.count && registry.locks[index] != lock) ++index;
  // The caller tried to unregister a lock that this thread never registered; only unregister locks after a successful register_long_wait_incompatible_lock on
  // the same thread.
  ASSERT(index < registry.count);
  registry.locks[index] = registry.locks[--registry.count];
  registry.locks[registry.count] = nullptr;
}

// Return whether the current thread owns a lock that forbids potentially unbounded waits.
[[nodiscard]] inline bool current_thread_holds_long_wait_incompatible_lock() noexcept
{
  return detail::long_wait_incompatible_locks.count != 0;
}

// Track session mutex ownership by the current thread while retaining AIMutex's self-deadlock detection.
//
// Every successful lock acquisition registers this mutex in thread-local state. Potentially blocking code can therefore reject execution while the
// current thread owns any session mutex, even when that code has no reference to the corresponding session_ts object.
class SessionDebugMutex final : public AIMutex
{
 public:
  // Acquire this session mutex and register it as owned by the current thread.
  void lock()
  {
    DoutEntering(dc::session, "SessionDebugMutex::lock() [" << this << "]");

    AIMutex::lock();
    register_long_wait_incompatible_lock(this);

    Dout(dc::session, "Locked SessionDebugMutex [" << this << "]");
  }

  // Try to acquire this session mutex, registering only a successful acquisition.
  //
  // Returns true only when the underlying mutex was acquired.
  [[nodiscard]] bool try_lock()
  {
    DoutEntering(dc::session, "SessionDebugMutex::try_lock() [" << this << "]");

    if (!AIMutex::try_lock())
      return false;
    register_long_wait_incompatible_lock(this);
    Dout(dc::session, "Locked SessionDebugMutex (try_lock) [" << this << "]");
    return true;
  }

  // Release this session mutex and remove it from the current thread's registry.
  void unlock()
  {
    DoutEntering(dc::session, "SessionDebugMutex::unlock() [" << this << "]");

    // unlock was called without the current thread holding this mutex; pair every unlock with a successful lock or try_lock on the same thread.
    ASSERT(is_self_locked());
    unregister_long_wait_incompatible_lock(this);
    AIMutex::unlock();
  }

  // Return whether the current thread owns at least one debug session mutex.
  [[nodiscard]] static bool current_thread_holds_session_lock() noexcept { return current_thread_holds_long_wait_incompatible_lock(); }

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

// Register the calling thread with libcwd under `label`, so that debug output
// can be attributed to it and the thread is named.
//
// Returns void. No-op when libcwd (CWDEBUG) is not enabled.
void init_thread(std::string const& label);

// Assert that the current thread owns no session mutex before entering a potentially blocking operation.
//
// `operation` describes the wait or other slow boundary.
// `location` identifies the assertion call site; this function does not require access to a particular
// session_ts because SessionDebugMutex maintains thread-local ownership state for every session.
inline void assert_no_session_lock_held(std::string_view operation, std::source_location location = std::source_location::current())
{
  if (SessionDebugMutex::current_thread_holds_session_lock())
    DoutFatal(dc::core,
              "Potentially blocking operation while holding one or more session locks [" <<
              detail::long_wait_incompatible_locks << "] while: " << operation << " at " << location.file_name() << ':' << location.line());
}

// Assert that the current thread does not own the mutex wrapped by unlocked_session before entering a potentially blocking operation.
//
// Operation describes the wait or other slow boundary. This narrower check complements assert_no_session_lock_held when the relevant session is
// already part of the API contract.
template <typename UnlockedSession>
inline void assert_session_unlocked(UnlockedSession const& unlocked_session, std::string_view operation,
                                    std::source_location location = std::source_location::current())
{
  if (unlocked_session.mutex().is_self_locked())
    DoutFatal(dc::core, "Session mutex [" << (void*)&unlocked_session.mutex() <<
              "] still locked while: " << operation << " at " << location.file_name() << ':' << location.line());
}

// Do not use the above functions directory. Use these macro's instead.
#define AVA_ASSERT_NO_SESSION_LOCK_HELD(operation) ::ava::core::assert_no_session_lock_held(operation)
#define AVA_ASSERT_SESSION_UNLOCKED(unlocked_session, operation) ::ava::core::assert_session_unlocked(unlocked_session, operation)
#else   // CWDEBUG
#define AVA_ASSERT_NO_SESSION_LOCK_HELD(operation) ((void)0)
#define AVA_ASSERT_SESSION_UNLOCKED(unlocked_session, operation) ((void)0)
#endif  // CWDEBUG

// Own a cooperative thread and reject implicit or explicit joins while the current thread holds a debug lock that forbids long waits.
//
// create starts the thread after associating the mandatory human-readable label with its debug output. The callable receives a stop token when it
// accepts one and is otherwise called without arguments. Destruction and move assignment preserve std::jthread's request-stop-and-join behavior,
// but first assert that joining is allowed. Explicit join performs the same assertion; detach and stop requests do not wait.
//
// JoinThread is move-only. It intentionally has no conversion to std::jthread because conversion would discard join-boundary checking.
class JoinThread
{
 public:
  JoinThread() noexcept = default;
  ~JoinThread()
  {
#ifdef CWDEBUG
    if (thread_.joinable())
      assert_join_allowed("implicitly joining JoinThread during destruction of thread");
#endif
  }

  JoinThread(JoinThread const&) = delete;
  JoinThread& operator=(JoinThread const&) = delete;
  JoinThread(JoinThread&&) noexcept = default;
  JoinThread& operator=(JoinThread&& other) noexcept
  {
    if (this != &other)
    {
#ifdef CWDEBUG
      if (thread_.joinable())
        assert_join_allowed("implicitly joining JoinThread during move assignment into thread");
      label_ = std::move(other.label_);
#endif
      thread_ = std::move(other.thread_);
    }
    return *this;
  }

  // Start a labeled cooperative thread with callable f.
  //
  // The callable is moved into the new thread and may therefore own move-only captures. It receives the new thread's stop token when invocable with
  // one; otherwise it is invoked without arguments.
  template <typename F>
  [[nodiscard]] static JoinThread create(std::string CWDEBUG_ONLY(label), F&& f)
  {
#ifdef CWDEBUG
    std::string owner_label = label;
    std::jthread thread([label = std::move(label), f = std::forward<F>(f)](std::stop_token st) mutable {
      init_thread(label);
      if constexpr (std::is_invocable_v<F&&, std::stop_token>)
        std::forward<F>(f)(std::move(st));
      else
        std::forward<F>(f)();
    });
    return JoinThread(std::move(thread), std::move(owner_label));
#else
    return JoinThread(std::jthread([f = std::forward<F>(f)](std::stop_token st) mutable {
      if constexpr (std::is_invocable_v<F&&, std::stop_token>)
        std::forward<F>(f)(std::move(st));
      else
        std::forward<F>(f)();
    }));
#endif
  }

  [[nodiscard]] bool joinable() const noexcept { return thread_.joinable(); }
  [[nodiscard]] std::thread::id get_id() const noexcept { return thread_.get_id(); }
  [[nodiscard]] std::stop_source get_stop_source() noexcept { return thread_.get_stop_source(); }
  [[nodiscard]] std::stop_token get_stop_token() const noexcept { return thread_.get_stop_token(); }
  bool request_stop() noexcept { return thread_.request_stop(); }
  [[nodiscard]] std::jthread::native_handle_type native_handle() { return thread_.native_handle(); }

  // Wait for the owned thread to finish after checking the debug no-long-wait contract.
  void join()
  {
    Debug(assert_join_allowed("explicitly joining JoinThread"));
    thread_.join();
  }

  // Relinquish ownership without waiting. The detached thread keeps its own stop state alive.
  void detach() { thread_.detach(); }

  void swap(JoinThread& other) noexcept
  {
    thread_.swap(other.thread_);
#ifdef CWDEBUG
    label_.swap(other.label_);
#endif
  }

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT

 private:
  explicit JoinThread(std::jthread thread COMMA_CWDEBUG_ONLY(std::string label)) : thread_(std::move(thread)) COMMA_CWDEBUG_ONLY(label_(std::move(label))) { }

#ifdef CWDEBUG
  void assert_join_allowed(std::string_view CWDEBUG_ONLY(operation)) const
  {
    Debug(assert_no_session_lock_held(operation));
  }
#endif

  std::jthread thread_;
#ifdef CWDEBUG
  std::string label_;
#endif
};

// Start a plain (non-cooperative) thread whose first action is init_thread(label).
// Returns a std::thread.
//
// `f` is invoked with no arguments and is moved (never copied) into the thread,
// so move-only captures work. Use this for worker loops that do not participate
// in cooperative cancellation via std::stop_token (e.g. a writer or deadline
// loop that is signaled through its own mutex/condition variable).
template <typename F>
[[nodiscard]] inline std::thread make_thread(std::string CWDEBUG_ONLY(label), F&& f)
{
#ifdef CWDEBUG
  return std::thread([label = std::move(label), f = std::forward<F>(f)]() mutable {
    init_thread(label);
    std::forward<F>(f)();
  });
#else
  return std::thread(std::forward<F>(f));
#endif
}

// Run `f` asynchronously on a new thread (std::launch::async) whose first action
// is init_thread(label), returning a std::future to its result.
//
// `f` is invoked with no arguments; its return value becomes the future's value.
// `f` is moved (never copied). Unlike a raw std::async, this always uses
// std::launch::async (a real new thread) so init_thread always runs on a freshly
// spawned thread.
template <typename F>
[[nodiscard]] inline auto make_async(std::string CWDEBUG_ONLY(label), F&& f) -> std::future<std::invoke_result_t<F&&>>
{
#ifdef CWDEBUG
  return std::async(std::launch::async, [label = std::move(label), f = std::forward<F>(f)]() mutable {
    init_thread(label);
    return std::forward<F>(f)();
  });
#else
  return std::async(std::launch::async, std::forward<F>(f));
#endif
}

}  // namespace ava::core

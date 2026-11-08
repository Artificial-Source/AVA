#pragma once

#include "ava/tui/terminal/Context.h"
#include "ava/core/Signals.h"
#include "memory/MemoryPagePool.h"
#include "memory/VectorAllocator.h"

#include <condition_variable>
#include <mutex>
#include <string_view>
#include "debug.h"
#ifdef CWDEBUG
#include "ava/core/DebugInit.h"
#endif

namespace ava::core {

// Process-lifetime interface published by the production composition root.
class Application
#ifdef CWDEBUG
    : public DebugInit
#endif
{
 public:
  using Vec8Alloc = memory::VectorAllocator<char>;

 private:
  static Application* s_instance;               // There can only be one instance of Application. Allow global access.

  // The order of these members is important; do not change.
  memory::MemoryPagePool mpp_;                  // Pool using the default block size (32 KiB); its first growth allocates at least two blocks (64 KiB).
  Vec8Alloc vec8alloc_;                         // A geometric allocator for sizes 8, 16, 32, 64, ...
  Signals signals_manager_;                     // The POSIX signals manager.
  tui::terminal::Context terminal_context_;     // The terminal context instance.

  // Shutdown request; the flag is protected by the mutex and every change of it is signaled through the condition variable.
  std::mutex shutdown_mutex_;
  std::condition_variable shutdown_cv_;
  bool shutdown_requested_ = false;

 public:
  Application(CWDEBUG_ONLY(bool debug_init_arg));
  Application(Application const&) = delete;
  Application& operator=(Application const&) = delete;
  Application(Application&&) = delete;
  Application& operator=(Application&&) = delete;
  virtual ~Application() noexcept;

  //FIXME: Add mutex for access.
  static Application& instance()
  {
    // Create an `Application` object, derived from ava::core::Application at the top of main, after any debug initialization.
    ASSERT(s_instance != nullptr);
    return *s_instance;
  }

  Vec8Alloc vec8alloc() const { return vec8alloc_; }

  // Initialize the application's terminal Context with `outfd` and `infd`.
  // Null streams select the process's standard terminal; tests can supply streams that outlive this Application.
  void initialize_terminal_context(FILE* outfd = nullptr, FILE* infd = nullptr) { terminal_context_.initialize(outfd, infd); }

  [[nodiscard]] virtual std::string_view application_name() const noexcept = 0;

  // Ask the application to shut down.
  //
  // This is only a request: it returns immediately and never waits for anything to stop.
  // Any thread may call it, including the TUI thread, any number of times; only the first call has an effect.
  // The thread that called wait_for_shutdown_request (the main thread) wakes up and performs the actual,
  // ordered shutdown of all threads.
  void request_shutdown();

#ifdef CWDEBUG
  void attach_gdb();
#endif

  // Accessors

  Signals& signals_manager() { return signals_manager_; }
  Signals const& signals_manager() const { return signals_manager_; }

  tui::terminal::Context& terminal_context()
  {
    // Call initialize_terminal_context(), before accessing Application::terminal_context();
    ASSERT(terminal_context_.is_initialized());
    // Access the terminal Context only from the registered TUI thread.
    ASSERT(tui::terminal::is_tui_thread());
    return terminal_context_;
  }
  tui::terminal::Context const& terminal_context() const
  {
    // Call initialize_terminal_context(), before accessing Application::terminal_context();
    ASSERT(terminal_context_.is_initialized());
    // Access the terminal Context only from the registered TUI thread.
    ASSERT(tui::terminal::is_tui_thread());
    return terminal_context_;
  }

  // Can't print mpp_.
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT

 protected:
  // Block the calling thread until request_shutdown() was called.
  //
  // Returns immediately when the request was already made. Only the thread that performs
  // the shutdown (the main thread) should call this.
  void wait_for_shutdown_request();
};

} // namespace ava::core

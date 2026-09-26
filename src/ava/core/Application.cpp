#include "sys.h"
#include "ava/core/Application.h"

#include <cstdlib>
#include "debug.h"

#if defined(__SANITIZE_THREAD__)
# define AVA_WITH_TSAN 1
#elif defined(__has_feature)
# if __has_feature(thread_sanitizer)
#  define AVA_WITH_TSAN 1
# endif
#endif

#ifndef AVA_WITH_TSAN
# define AVA_WITH_TSAN 0
#endif

#if CW_DEBUG && !AVA_WITH_TSAN && defined(__linux__)
#include <chrono>
#include <thread>
#include <sys/stat.h>
#endif

namespace ava::core {

#if CW_DEBUG && !AVA_WITH_TSAN
namespace {

bool only_one_thread_remains()
{
#ifdef __linux__
  struct stat st;
  auto delay_ms = std::chrono::milliseconds(1);
  int attempt = 0;
  while (++attempt <= 4)
  {
    if (::stat("/proc/self/task", &st) == 0 && st.st_nlink == 3)        // 3: `./`, `../` and one directory for the current thread.
      return true;
    std::this_thread::sleep_for(delay_ms);
    delay_ms *= 2;
  }
#else
  return true;
#endif
  return false;
}

} // namespace
#endif // CW_DEBUG

//static
Application* Application::s_instance;

Application::Application(CWDEBUG_ONLY(bool debug_init_arg))
    :
#ifdef CWDEBUG
      DebugInit(debug_init_arg),
#endif
      mpp_(Vec8Alloc::mpp_block_size),
      vec8alloc_(mpp_),
      signals_manager_({}),
      terminal_context_({})
{
  // Instantiate only one `Application` object derived from ava::core::Application.
  ASSERT(s_instance == nullptr);
  s_instance = this;
}

Application::~Application() noexcept
{
  DoutEntering(dc::notice, "core::Application::~Application()");

  s_instance = nullptr;
  // Prints to dc::memory that memory::NodeMemoryResource::deinit() is called, which in turn silences the destructor.
  // The latter is necessary because by the time that the NodeMemoryResource objects are destructed we have destructed
  // the mutex used by the debug output ostream.
  Vec8Alloc::deinit();

// Don't do this test for ThreadSanitizer build.
#if CW_DEBUG && !AVA_WITH_TSAN
  // The Application should only be destructed after joining with all other threads.
  ASSERT(only_one_thread_remains());
#endif
}

}  // namespace ava::core

#include "sys.h"
#include "ava/core/Application.h"

#include <cstdlib>
#include "debug.h"

#if CW_DEBUG && defined(__linux__)
#include <sys/stat.h>
#endif

namespace ava::core {

#if CW_DEBUG
namespace {

bool only_one_thread_remains()
{
#ifdef __linux__
  struct stat st;
  return ::stat("/proc/self/task", &st) == 0 && st.st_nlink == 3;       // 3: `./`, `../` and one directory for the current thread.
#else
  return true;
#endif
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

#if CW_DEBUG
  // The Application should only be destructed after joining with all other threads.
  ASSERT(only_one_thread_remains());
#endif
}

}  // namespace ava::core

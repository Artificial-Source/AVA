#include "sys.h"
#include "ava/core/Application.h"

#include <cstdlib>
#include "debug.h"

namespace ava::core {

//static
Application* Application::s_instance;

Application::Application(CWDEBUG_ONLY(bool debug_init_arg))
    :
#ifdef CWDEBUG
      DebugInit(debug_init_arg),
#endif
      mpp_(Vec8Alloc::mpp_block_size),
      vec8alloc_(mpp_),
      terminal_context_({})
{
  // Instantiate only one `Application` object derived from ava::core::Application.
  ASSERT(s_instance == nullptr);
  s_instance = this;
}

Application::~Application() noexcept
{
  s_instance = nullptr;
  // Prints to dc::memory that memory::NodeMemoryResource::deinit() is called, which in turn silences the destructor.
  // The latter is necessary because by the time that the NodeMemoryResource objects are destructed we have destructed
  // the mutex used by the debug output ostream.
  Vec8Alloc::deinit();
}

}  // namespace ava::core

#include "sys.h"
#include "ava/core/Application.h"

#include <cstdlib>
#include "debug.h"

namespace ava::core {
namespace {

// These process-lifecycle values are non-atomic by design. main publishes the
// initialized Application before app::run starts workers, and revokes it only
// after app::run returns with all app::run-owned workers joined.
Application* registered_application = nullptr;

} // namespace

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
  ASSERT(registered_application == nullptr);
  registered_application = this;
}

Application::~Application() noexcept
{
  registered_application = nullptr;
  // Prints to dc::memory that memory::NodeMemoryResource::deinit() is called, which in turn silences the destructor.
  // The latter is necessary because by the time that the NodeMemoryResource objects are destructed we have destructed
  // the mutex used by the debug output ostream.
  Vec8Alloc::deinit();
}

//FIXME: Add mutex for access.
Application& Application::instance()
{
  // Create an `Application` object, derived from ava::core::Application at the top of main, after any debug initialization.
  ASSERT(registered_application != nullptr);
  return *registered_application;
}

void Application::reapply_cursor_settings()
{
  //FIXME: implement
  ASSERT(false);
}

}  // namespace ava::core

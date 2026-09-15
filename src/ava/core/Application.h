#pragma once

#include "memory/MemoryPagePool.h"
#include "memory/VectorAllocator.h"

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
  memory::MemoryPagePool mpp_;          // Pool using the default block size (32 KiB); its first growth allocates at least two blocks (64 KiB).
  Vec8Alloc vec8alloc_;                 // A geometric allocator for sizes 8, 16, 32, 64, ...

 public:
  Application(CWDEBUG_ONLY(bool debug_init_arg));
  Application(Application const&) = delete;
  Application& operator=(Application const&) = delete;
  Application(Application&&) = delete;
  Application& operator=(Application&&) = delete;
  virtual ~Application() noexcept;

  Vec8Alloc vec8alloc() const { return vec8alloc_; }

  [[nodiscard]] static Application const& instance();
  [[nodiscard]] virtual std::string_view application_name() const noexcept = 0;

  // Can't print mpp_.
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

}  // namespace ava::core

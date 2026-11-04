// Call-through GNU linker wrapper for test-only observation of actual drawing.
#include "sys.h"
#include "ScopedScreenDrawObserver.h"

#include <utility>

namespace tui_test_support {

// Reach the unmodified production implementation through GNU ld's --wrap alias.
extern "C" bool
screen_draw_real(ava::tui::TuiFrontEnd*, ava::tui::ComposerSnapshot const&, ava::tui::detail::CompletionMatchCache&, std::size_t, ava::tui::detail::TranscriptLayoutCache&, std::size_t, ava::tui::detail::ScreenRowCache&, bool, bool) asm(
    "__real__ZN3ava3tui6detail18draw_screen_cachedEPNS0_11TuiFrontEndERKNS0_16ComposerSnapshotERNS1_20CompletionMatchCacheEmRNS1_21TranscriptLayoutCacheEmRNS1_"
    "14ScreenRowCacheEbb");

// No observation is active until a test installs a scoped owner on this thread.
thread_local ScopedScreenDrawObserver* ScopedScreenDrawObserver::active_ = nullptr;

// A nested observer temporarily replaces the previous thread-local registration.
ScopedScreenDrawObserver::ScopedScreenDrawObserver() noexcept : previous_(std::exchange(active_, this))
{
}

// Restore the earlier registration without retaining the destroyed owner's data.
ScopedScreenDrawObserver::~ScopedScreenDrawObserver()
{
  active_ = previous_;
}

// Expose only an owned observation; querying it never services or draws a frame.
ava::tui::detail::ScreenRowCache const& ScopedScreenDrawObserver::frame() const noexcept
{
  return frame_;
}

// Distinguish a failed real draw from a scope in which drawing has not occurred.
std::optional<bool> ScopedScreenDrawObserver::observed_result() const noexcept
{
  return observed_result_;
}

// Run real rendering and terminal I/O first, then copy precisely its resulting
// cache and return value. In particular, a failed draw leaves valid=false while
// preserving the production cache's previous surfaces; never repair that state.
extern "C" bool screen_draw_wrap(ava::tui::TuiFrontEnd* frontend, ava::tui::ComposerSnapshot const& snapshot,
                                 ava::tui::detail::CompletionMatchCache& completion_cache, std::size_t source_revision,
                                 ava::tui::detail::TranscriptLayoutCache& transcript_cache, std::size_t transcript_generation,
                                 ava::tui::detail::ScreenRowCache& screen_cache, bool freeze_transcript_layout, bool allow_frozen_width_mismatch)
{
  bool const result = screen_draw_real(frontend, snapshot, completion_cache, source_revision, transcript_cache, transcript_generation, screen_cache,
                                       freeze_transcript_layout, allow_frozen_width_mismatch);
  if (auto* observer = ScopedScreenDrawObserver::active_)
  {
    observer->frame_ = screen_cache;
    observer->observed_result_ = result;
  }
  return result;
}

} // namespace tui_test_support

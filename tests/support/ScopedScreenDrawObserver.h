// Test-only observation of the cache produced by real terminal drawing.
#pragma once

#include "ava/debug/print_members_on.h"
#include "ava/tui/composer_internal.h"

#include <cstddef>
#include <optional>

namespace tui_test_support {

// GNU linker entry point; forwards to the real draw before recording its result.
extern "C" bool
screen_draw_wrap(ava::tui::TuiFrontEnd*, ava::tui::ComposerSnapshot const&, ava::tui::detail::CompletionMatchCache&, std::size_t, ava::tui::detail::TranscriptLayoutCache&, std::size_t, ava::tui::detail::ScreenRowCache&, bool, bool) asm(
    "__wrap__ZN3ava3tui6detail18draw_screen_cachedEPNS0_11TuiFrontEndERKNS0_16ComposerSnapshotERNS1_20CompletionMatchCacheEmRNS1_21TranscriptLayoutCacheEmRNS1_"
    "14ScreenRowCacheEbb");

// Observe any wrapped full-screen draw on the constructing test thread. Existing
// users keep one renderer in each scope; this deliberately does not filter owners.
// The innermost observer receives draws until destruction restores its predecessor.
// Construct, inspect and destroy on the same thread, with strictly nested lifetimes.
// Cached content is copied after real drawing; no renderer/cache pointer is retained.
class ScopedScreenDrawObserver final
{
 public:
  // Install this observer while retaining the enclosing observer, if any.
  ScopedScreenDrawObserver() noexcept;
  // Restore the enclosing observer before this observer's copied content dies.
  ~ScopedScreenDrawObserver();
  // Registration and restoration have one stationary owner.
  ScopedScreenDrawObserver(ScopedScreenDrawObserver const&) = delete;
  ScopedScreenDrawObserver& operator=(ScopedScreenDrawObserver const&) = delete;
  ScopedScreenDrawObserver(ScopedScreenDrawObserver&&) = delete;
  ScopedScreenDrawObserver& operator=(ScopedScreenDrawObserver&&) = delete;

  // Return the last post-draw cache copy; initially empty and invalid.
  [[nodiscard]] ava::tui::detail::ScreenRowCache const& frame() const noexcept;
  // Return the real draw result, absent until a wrapped draw has returned.
  [[nodiscard]] std::optional<bool> observed_result() const noexcept;

  // Copied terminal surfaces can contain prompt content; never generate a printer.
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT

 private:
  // Only the call-through linker wrapper may record observations.
  friend bool screen_draw_wrap(ava::tui::TuiFrontEnd*, ava::tui::ComposerSnapshot const&, ava::tui::detail::CompletionMatchCache&, std::size_t,
                               ava::tui::detail::TranscriptLayoutCache&, std::size_t, ava::tui::detail::ScreenRowCache&, bool, bool);

  // Non-owning registration for this test thread, restored by each scope's owner.
  static thread_local ScopedScreenDrawObserver* active_;
  // Enclosing observer outlives this strictly nested scope; never a cache borrow.
  ScopedScreenDrawObserver* previous_;
  // Owned post-draw state, including invalidation after a failed write or flush.
  ava::tui::detail::ScreenRowCache frame_;
  // Exact completed draw result, distinct from the absence of any observed draw.
  std::optional<bool> observed_result_;
};

} // namespace tui_test_support

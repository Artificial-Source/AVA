#pragma once

#include "ava/tui/composer.h"
#include "ava/core/NoLogString.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ava::tui::detail {

inline constexpr std::size_t kMaxMermaidPresentationSourceBytes = 64 * 1024;
inline constexpr std::size_t kMaxMermaidPresentationEntries = 128;
inline constexpr std::size_t kMaxMermaidPresentationOutputBytes = 256 * 1024;
inline constexpr std::size_t kMaxMermaidPresentationBytes = 4 * 1024 * 1024;

// Exact source coordinates for one completed Mermaid fence. `source` is the
// fence body delivered to the application helper; opening/closing rows remain
// part of the semantic TranscriptItem and are replaced only during layout.
struct MermaidFenceBlock
{
  std::size_t block_index = 0;
  std::size_t fence_start = 0;
  std::size_t source_start = 0;
  std::size_t source_end = 0;
  std::size_t fence_end = 0;
  ava::core::NoLogString source;

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

// Pure scanner using the same line-oriented triple-backtick toggling semantics
// as AVA's Markdown transcript renderer. Items with append-only state or a
// retained live stream identity are ineligible.
[[nodiscard]] std::vector<MermaidFenceBlock> scan_completed_assistant_mermaid_fences(TranscriptItem const& item);

struct MermaidAcceptedPresentation
{
  std::uint64_t config_epoch = 0;
  std::uint64_t item_identity = 0;
  std::uint64_t block_identity = 0;
  std::size_t item_index = 0;
  std::size_t block_index = 0;
  std::size_t fence_start = 0;
  std::size_t fence_end = 0;
  ava::core::NoLogString source;
  ava::core::NoLogString text;

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

// Immutable bounded result map published by the runtime reconciliation seam.
// Lookup revalidates epoch, item/block position, source coordinates, and exact
// source bytes so stale or colliding completions fail closed to Markdown.
class MermaidTranscriptProjection final
{
 public:
  MermaidTranscriptProjection(std::uint64_t config_epoch, std::vector<MermaidAcceptedPresentation> accepted);

  [[nodiscard]] std::uint64_t config_epoch() const noexcept;
  [[nodiscard]] bool contains_item(std::size_t item_index) const noexcept;
  [[nodiscard]] std::optional<std::string_view> accepted_text(std::size_t item_index, MermaidFenceBlock const& block) const noexcept;
  [[nodiscard]] std::vector<MermaidAcceptedPresentation> const& accepted() const noexcept;

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT

 private:
  std::uint64_t config_epoch_ = 0;
  std::vector<MermaidAcceptedPresentation> accepted_;
};

[[nodiscard]] inline MermaidTranscriptProjection const* active_mermaid_projection(ComposerSnapshot const& snapshot) noexcept
{
  if (!snapshot.mermaid_enabled || !snapshot.mermaid_projection || snapshot.mermaid_config_epoch == 0 ||
      snapshot.mermaid_projection->config_epoch() != snapshot.mermaid_config_epoch)
    return nullptr;
  return snapshot.mermaid_projection.get();
}

}  // namespace ava::tui::detail

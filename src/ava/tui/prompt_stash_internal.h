#pragma once

#include "ava/debug/print_members_on.h"
#include "ava/tui/composer_editor.h"
#include "ava/core/NoLogString.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace ava::tui {

enum class ActiveSelectList;
struct InputEvent;
struct RuntimeDraftState;
class RuntimePresentationState;
class RuntimeRenderer;
struct SelectListView;
struct TuiKeyBindings;

enum class PromptStashPushResult
{
  Stored,
  EmptyDraft,
  EntryTooLarge,
  EntryLimitReached,
  AggregateLimitReached,
};

enum class PromptStashRestoreResult
{
  Restored,
  NotFound,
  NonemptyDraft,
};

struct PromptStashEntry
{
  std::uint64_t id = 0;
  ava::core::NoLogString text;
  std::size_t cursor = 0;
  std::vector<ComposerPasteEntry> paste_entries;
  std::size_t next_paste_id = 1;
  std::size_t expanded_bytes = 0;

  // Prompt payloads must never be emitted by generated debug printers.
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

// Process-memory-only prompt storage. The runtime owns one instance for the
// lifetime of an interactive composer and never serializes it into a session.
class PromptStash final
{
 public:
  static constexpr std::size_t kMaxEntries = 16;
  static constexpr std::size_t kMaxEntryExpandedBytes = 64 * 1024;
  static constexpr std::size_t kMaxAggregateExpandedBytes = 256 * 1024;

  [[nodiscard]] PromptStashPushResult push(ComposerDraftState const& draft);
  [[nodiscard]] PromptStashRestoreResult restore(std::uint64_t id, ComposerDraftState& draft);
  [[nodiscard]] PromptStashRestoreResult restore_latest(ComposerDraftState& draft);
  void clear() noexcept;

  [[nodiscard]] bool empty() const noexcept { return entries_.empty(); }
  [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }
  [[nodiscard]] std::size_t aggregate_expanded_bytes() const noexcept { return aggregate_expanded_bytes_; }
  [[nodiscard]] std::vector<std::uint64_t> ids_newest_first() const;
  [[nodiscard]] SelectListView select_list_view() const;

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT

 private:
  std::vector<PromptStashEntry> entries_;
  std::size_t aggregate_expanded_bytes_ = 0;
  std::uint64_t next_id_ = 1;
};

// Runtime wiring shared by idle and active-run input. It owns no backend or
// session callbacks and mutates only the composer and TUI presentation.
class RuntimePromptStashController final
{
 public:
  RuntimePromptStashController(RuntimePresentationState& presentation_state, RuntimeDraftState& draft_state, RuntimeRenderer& renderer,
                               ActiveSelectList& active_select_list, TuiKeyBindings const& key_bindings);
  RuntimePromptStashController(RuntimePromptStashController const&) = delete;
  RuntimePromptStashController& operator=(RuntimePromptStashController const&) = delete;

  [[nodiscard]] bool trigger();
  [[nodiscard]] bool open_selector();
  [[nodiscard]] bool pop_latest();
  [[nodiscard]] bool clear();
  [[nodiscard]] std::optional<bool> handle_selector_input(InputEvent const& event);

  [[nodiscard]] PromptStash const& stash() const noexcept { return stash_; }

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT

 private:
  void reset_draft_presentation();
  [[nodiscard]] bool restore(std::uint64_t id);

  RuntimePresentationState& presentation_state_;
  RuntimeDraftState& draft_state_;
  RuntimeRenderer& renderer_;
  ActiveSelectList& active_select_list_;
  TuiKeyBindings const& key_bindings_;
  PromptStash stash_;
};

}  // namespace ava::tui

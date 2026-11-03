// Stores bounded in-memory draft snapshots and wires their selector into the TUI.
// See docs/engineering/tui-architecture.md for the owner relationships.
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

class SelectorHost;
struct InputEvent;
class Composer;
class PendingAttachments;
class RuntimePresentationState;
class RuntimeRenderer;
struct SelectListView;
struct TuiKeyBindings;

// TUI-thread insertion result; every refusal preserves both input draft and stash contents.
enum class PromptStashPushResult
{
  // A complete text/paste snapshot is owned by the stash; the controller may clear the draft.
  Stored,
  // No text exists to snapshot, so the controller may offer existing stash entries instead.
  EmptyDraft,
  // Expanded text exceeds the per-entry bound, including folded paste payloads.
  EntryTooLarge,
  // Entry count or identifier exhaustion prevents allocating another stable stash identity.
  EntryLimitReached,
  // Expanded text would exceed the total stash budget even if the entry fits alone.
  AggregateLimitReached,
};

// TUI-thread restore result; only successful restoration consumes an entry.
enum class PromptStashRestoreResult
{
  // Text, cursor and paste records moved to a fresh editor state with no old undo history.
  Restored,
  // The requested stable identity no longer exists; the empty draft is unchanged.
  NotFound,
  // Restore refuses to overwrite live text, even when the requested entry does not exist.
  NonemptyDraft,
};

// Owned TUI-thread snapshot inside PromptStash. The controller renders copies for
// selection; only restore consumes this entry. Cursor and paste identifiers belong
// to the saved text, while expanded_bytes is fixed at admission for budget accounting.
// It owns its payload and needs no collaborator alive during destruction.
struct PromptStashEntry
{
  // Stable identity assigned on push; never reused after deletion or clear.
  std::uint64_t id = 0;
  // Saved draft text, including folded paste markers; retained only in process memory.
  ava::core::NoLogString text;
  // Saved byte cursor; restore clamps it before exposing the draft to editor operations.
  std::size_t cursor = 0;
  // Owned paste payloads required to expand the saved markers after restoration.
  std::vector<ComposerPasteEntry> paste_entries;
  // Saved allocator position prevents restored paste markers from colliding with later pastes.
  std::size_t next_paste_id = 1;
  // Admission-time expanded size used to release the exact aggregate budget on restore.
  std::size_t expanded_bytes = 0;

  // Prompt payloads must never be emitted by generated debug printers.
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

// RuntimePromptStashController owns this TUI-thread process-memory store for one
// interactive composer. Entries own all text/paste payloads; no borrowed state or
// worker can outlive it. Expanded-size/count bounds hold after every operation.
// Destruction releases drafts locally; nothing is serialized into a session.
class PromptStash final
{
 public:
  // Bound retained draft count independently of text size and selector rendering cost.
  static constexpr std::size_t kMaxEntries = 16;
  // Bound a single draft by expanded text so folded paste markers cannot bypass the limit.
  static constexpr std::size_t kMaxEntryExpandedBytes = 64 * 1024;
  // Bound total retained expanded text across entries; push checks remaining capacity first.
  static constexpr std::size_t kMaxAggregateExpandedBytes = 256 * 1024;

  // TUI thread: copy text, cursor and paste state only after all bounds pass. The caller
  // clears live draft only on Stored; this operation never modifies that source.
  [[nodiscard]] PromptStashPushResult push(ComposerDraftState const& draft);
  // TUI thread: require an empty destination, then move one entry into a fresh editor state.
  // Success consumes its identity and budget; failure leaves both operands unchanged.
  [[nodiscard]] PromptStashRestoreResult restore(std::uint64_t id, ComposerDraftState& draft);
  // TUI thread: restore the newest remaining identity under the same empty-draft rule.
  [[nodiscard]] PromptStashRestoreResult restore_latest(ComposerDraftState& draft);
  // TUI thread: discard retained drafts and budget; preserve the next identity to avoid reuse.
  void clear() noexcept;

  // TUI thread: choose empty-stash feedback without exposing mutable entries.
  [[nodiscard]] bool empty() const noexcept { return entries_.empty(); }
  // TUI thread: report retained count for clear feedback and bounded selectors.
  [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }
  // TUI thread: expose admitted expanded size, not marker-text length or container capacity.
  [[nodiscard]] std::size_t aggregate_expanded_bytes() const noexcept { return aggregate_expanded_bytes_; }
  // TUI thread: return owned identities so restoration may erase entries without invalidating callers.
  [[nodiscard]] std::vector<std::uint64_t> ids_newest_first() const;
  // TUI thread: build owned, sanitized preview rows keyed by stable entry identity; no entry
  // borrow survives later push, restore or clear.
  [[nodiscard]] SelectListView select_list_view() const;

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT

 private:
  // TUI-thread insertion order, oldest first; restore erases one and clear erases all.
  std::vector<PromptStashEntry> entries_;
  // TUI-thread sum of admitted entry sizes; push adds, successful restore subtracts, clear resets.
  std::size_t aggregate_expanded_bytes_ = 0;
  // TUI-thread monotonic allocator; push refuses exhaustion and clear deliberately preserves it.
  std::uint64_t next_id_ = 1;
};

// Session owns this TUI-thread wiring for idle and active-run stash input. It owns
// the stash and borrows editor, presentation, rendering, selector, bindings and
// attachments until destruction. Input handlers use no backend callbacks or locks.
// Successful push precedes draft clearing; successful restore precedes view closure.
// Borrowers outlive it, and teardown releases only local stash payloads.
class RuntimePromptStashController final
{
 public:
  // TUI thread: bind session owners and live bindings; all references outlive the controller.
  RuntimePromptStashController(RuntimePresentationState& presentation_state, Composer& draft_state, RuntimeRenderer& renderer, SelectorHost& selectors,
                               TuiKeyBindings const& key_bindings, PendingAttachments const& attachments);
  // A copied controller would duplicate private prompt storage and split selector identity.
  RuntimePromptStashController(RuntimePromptStashController const&) = delete;
  // Keep the stash and the draft/view owners associated throughout the session.
  RuntimePromptStashController& operator=(RuntimePromptStashController const&) = delete;

  // TUI thread: stash a nonempty draft only when no images are pending; open the selector
  // for an empty draft. Return render success after status, not insertion success.
  [[nodiscard]] bool trigger();
  // TUI thread: publish newest-first stash rows and request a frame, even when empty.
  [[nodiscard]] bool open_selector();
  // TUI thread: restore newest into an empty draft or publish refusal; return render success.
  [[nodiscard]] bool pop_latest();
  // TUI thread: discard all stash entries and close its view if hosted; report through status.
  [[nodiscard]] bool clear();
  // TUI thread: no value declines non-stash input; a value means claimed input and
  // contains render success. Copy the selected identity before restore retires the view.
  [[nodiscard]] std::optional<bool> handle_selector_input(InputEvent const& event);

  // TUI thread: borrow read-only process-memory storage until controller destruction.
  [[nodiscard]] PromptStash const& stash() const noexcept { return stash_; }

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT

 private:
  // TUI thread: consume the selected identity only into an empty draft, then reset
  // interaction and close the selector. Failure preserves the view and beeps.
  [[nodiscard]] bool restore(std::uint64_t id);

  // Borrowed from Session; TUI projection; receives status without adding transcript messages.
  RuntimePresentationState& presentation_state_;
  // Borrowed from Session; sole editor; push copies its draft and restoration transfers back into it.
  Composer& draft_state_;
  // Borrowed from Session; renderer; status and editor updates are published before requesting a frame.
  RuntimeRenderer& renderer_;
  // Borrowed from Session; selector storage; rows contain copies of stash identities and previews.
  SelectorHost& selectors_;
  // Live bindings owned by the frontend; must stay at a stable address across binding reloads.
  TuiKeyBindings const& key_bindings_;
  // Borrowed from Session; attachment owner; its read-only gate prevents losing images during stash.
  PendingAttachments const& attachments_;
  // Controller-owned TUI-thread memory; it survives run completion but never serializes into sessions.
  PromptStash stash_;
};

}  // namespace ava::tui

#pragma once

#include "runtime_state_internal.h"
#include "ava/debug/print_members_on.h"

namespace ava::tui {

// Owns the active selector. The borrowed rendering snapshot outlives this host;
// its select_list is only a published copy, never the source for mutations.
class SelectorHost final
{
 public:
  explicit SelectorHost(ComposerSnapshot& snapshot);
  SelectorHost(SelectorHost const&) = delete;
  SelectorHost& operator=(SelectorHost const&) = delete;

  [[nodiscard]] ActiveSelectList kind() const noexcept;
  [[nodiscard]] std::optional<SelectListView> const& view() const noexcept;
  // Borrowed items expire at the next selector mutation.
  [[nodiscard]] SelectListItemView const* selected_item(std::size_t index) const noexcept;
  [[nodiscard]] std::vector<std::string> visible_values(std::string const& query) const;

  void open(ActiveSelectList kind, SelectListView view);
  void replace(SelectListView view);
  void replace_preserving_selection(SelectListView view, std::size_t selected_index, std::string query);
  void close();
  void update_selection(std::size_t index, std::string query);
  void set_subtitle(std::string subtitle);

  // Selector rows may contain stashed prompts or transcript excerpts, and the
  // borrowed snapshot contains arbitrary messages. Neither aggregate has a safe
  // generated content printer; keep these contents out of debug output.
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT

 private:
  void publish();

  ComposerSnapshot& snapshot_;
  ActiveSelectList kind_ = ActiveSelectList::None;
  std::optional<SelectListView> view_;
};

} // namespace ava::tui

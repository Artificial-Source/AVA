#pragma once

#include "ava/app/command_palette.h"

namespace ava::app::command_palette_detail {

// Session labels have one stable textual form across backend completions and
// TUI session rows.
[[nodiscard]] std::string format_session_labels(std::vector<std::string> const& labels);

// Shared neutral tree ordering used by backend navigation and TUI projection.
// Returned pointers refer to nodes and remain valid only while nodes is unchanged.
[[nodiscard]] std::vector<ava::session::SessionTreeNode const*> sorted_session_tree_nodes(std::vector<ava::session::SessionTreeNode> const& nodes,
                                                                                          std::vector<std::string> ids, SessionSelectorSort sort);

}  // namespace ava::app::command_palette_detail

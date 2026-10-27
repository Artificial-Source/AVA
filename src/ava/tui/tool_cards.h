#pragma once

#include "ava/tui/composer.h"
#include "ava/core/NoLogString.h"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace ava::tui::detail {

struct ToolCardRenderResult
{
  std::vector<ava::core::NoLogString> lines = {};
  std::vector<bool> presentation_private_rows = {};

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

[[nodiscard]] ToolPresentation tool_card_presentation(ToolTimelineItem const& item, ToolPresentation inherited);
[[nodiscard]] bool tool_card_details_visible(ToolTimelineItem const& item, ToolPresentation inherited);
[[nodiscard]] bool tool_card_details_visible(ToolTimelineItem const& item, bool global_details_visible);
[[nodiscard]] bool tool_card_matches_copy_query(ToolTimelineItem const& item, std::string_view query);
[[nodiscard]] std::string tool_card_diff_copy_text(ToolTimelineItem const& item);
[[nodiscard]] std::string tool_card_permission_copy_text(ToolTimelineItem const& item, std::string_view query = {});
[[nodiscard]] std::string tool_card_copy_text(ToolTimelineItem const& item);
[[nodiscard]] ToolCardRenderResult render_tool_card_rows(ToolTimelineItem const& item, std::size_t width, ToolPresentation presentation,
                                                         bool suppress_result_summary = false);
[[nodiscard]] std::vector<std::string> render_tool_card(ToolTimelineItem const& item, std::size_t width, ToolPresentation presentation,
                                                        bool suppress_result_summary = false);
[[nodiscard]] std::vector<std::string> render_tool_card(ToolTimelineItem const& item, std::size_t width, bool global_details_visible,
                                                        bool suppress_result_summary = false);

}  // namespace ava::tui::detail

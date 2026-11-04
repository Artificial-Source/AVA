// Render plugin surfaces through the shared terminal-text contract before adding
// trusted styles. Host attribution and geometry use the same sanitized values.
#include "sys.h"
#include "ava/tui/composer_internal.h"

#include <algorithm>
#include <array>

namespace ava::tui {
namespace {

// TUI thread: measure the identity as displayed, including replacements for
// unsafe bytes. Binding validation is not a substitute for the renderer boundary.
std::string plugin_id_host_text(TuiPluginUiBinding const& binding)
{
  return "Plugin ID · " + sanitize_terminal_text(binding.plugin_id);
}

// TUI thread: command attribution must fit after sanitation; measuring raw SGR
// would hide bytes that the screen shows as literal replacement text.
std::string plugin_command_host_text(TuiPluginUiBinding const& binding)
{
  return "Command · " + sanitize_terminal_text(binding.command);
}

std::string plugin_controls_host_text(TuiPluginUiKind kind)
{
  switch (kind)
  {
    case TuiPluginUiKind::Status:
    case TuiPluginUiKind::Widget:
      return "Ctrl+C stop · 120s max";
    case TuiPluginUiKind::Select:
      return "Enter select · Esc cancel · Ctrl+C stop · 120s max";
    case TuiPluginUiKind::Confirm:
      return "Enter confirm · Esc cancel · Ctrl+C stop · 120s max";
  }
  return {};
}

std::size_t plugin_minimum_surface_rows(TuiPluginUiKind kind)
{
  switch (kind)
  {
    case TuiPluginUiKind::Status:
      return 4;  // identity, command, status, controls
    case TuiPluginUiKind::Widget:
      return 5;  // identity, command, title, one body line, controls
    case TuiPluginUiKind::Select:
      return 6;  // identity, command, title, description, one option, controls
    case TuiPluginUiKind::Confirm:
      return 7;  // identity, command, title, description, both choices, controls
  }
  return 0;
}

}  // namespace

bool plugin_ui_host_chrome_fits(TuiPluginUiBinding const& binding, TuiPluginUiKind kind, std::size_t width, std::size_t max_lines) noexcept
{
  try
  {
    auto const centered_modal = kind == TuiPluginUiKind::Select || kind == TuiPluginUiKind::Confirm;
    auto const inset = centered_modal ? detail::modal_horizontal_inset(width) : std::size_t{2};
    auto const reserved_width = centered_modal ? 2 * inset : inset;
    auto const content_width = width > reserved_width ? width - reserved_width : 0;
    auto const minimum_rows = plugin_minimum_surface_rows(kind) + (centered_modal ? 2 * detail::modal_vertical_inset(max_lines) : 0);
    return max_lines >= minimum_rows && detail::terminal_text_columns(plugin_id_host_text(binding)) <= content_width &&
           detail::terminal_text_columns(plugin_command_host_text(binding)) <= content_width &&
           detail::terminal_text_columns(plugin_controls_host_text(kind)) <= content_width;
  }
  catch (...)
  {
    return false;
  }
}

namespace detail {
namespace {

std::string plugin_surface_line(std::string content, std::size_t width, std::string_view background)
{
  return surface_line(background, "  " + std::move(content), width);
}

std::string plugin_modal_surface_line(std::string content, std::size_t width)
{
  auto const inset = modal_horizontal_inset(width);
  content = fit_line_preserving_sgr(std::move(content), modal_content_width(width));
  return surface_line(kSgrComposerBg, std::string(inset, ' ') + std::move(content), width);
}

std::size_t plugin_option_count(TuiPluginUiModalView const& view)
{
  return view.kind == TuiPluginUiKind::Confirm ? std::size_t{2} : view.options.size();
}

std::size_t plugin_option_row_budget(std::size_t max_lines)
{
  auto const fixed_rows = std::size_t{5} + 2 * modal_vertical_inset(max_lines);
  return max_lines > fixed_rows ? max_lines - fixed_rows : 0;
}

std::size_t plugin_first_visible_option(TuiPluginUiModalView const& view, std::size_t max_lines)
{
  auto const count = plugin_option_count(view);
  auto const budget = plugin_option_row_budget(max_lines);
  if (count == 0 || budget == 0 || count <= budget)
    return 0;
  auto const selected = std::min(view.selected_option, count - 1);
  if (selected < budget)
    return 0;
  return std::min(selected - budget + 1, count - budget);
}

std::pair<std::string, std::optional<std::string>> plugin_option_text(TuiPluginUiModalView const& view, std::size_t index)
{
  if (view.kind == TuiPluginUiKind::Confirm)
  {
    static constexpr std::array<std::string_view, 2> labels{"Confirm plugin action", "Cancel"};
    if (index < labels.size())
      return {std::string(labels[index]), std::nullopt};
    return {};
  }
  if (index >= view.options.size())
    return {};
  return {sanitize_terminal_text(view.options[index].label),
          view.options[index].description ? std::optional<std::string>(sanitize_terminal_text(*view.options[index].description)) : std::nullopt};
}

}  // namespace

// TUI thread: borrow the current dock only while producing owned, bounded rows.
// Sanitize every binding/payload value before composing trusted terminal styles.
std::vector<std::string> render_plugin_ui_dock(TuiPluginUiDockView const& view, std::size_t width, std::size_t max_lines)
{
  auto const kind = view.widgets.empty() ? TuiPluginUiKind::Status : TuiPluginUiKind::Widget;
  if (!plugin_ui_host_chrome_fits(view.binding, kind, width, max_lines))
    return {};

  std::vector<std::string> lines;
  lines.reserve(max_lines);
  auto push = [&](std::string line) {
    if (lines.size() + 1 >= max_lines)
      return false;
    lines.push_back(plugin_surface_line(std::move(line), width, kSgrScreenBg));
    return true;
  };

  lines.push_back(plugin_surface_line(
      std::string(kSgrAccent) + "Plugin ID" + std::string(kSgrReset) + std::string(kSgrScreenBg) + " · " + sanitize_terminal_text(view.binding.plugin_id),
      width, kSgrScreenBg));
  lines.push_back(plugin_surface_line(
      std::string(kSgrMuted) + "Command" + std::string(kSgrReset) + std::string(kSgrScreenBg) + " · " + sanitize_terminal_text(view.binding.command), width,
      kSgrScreenBg));
  if (view.status)
  {
    push(std::string(kSgrMuted) + "status" + std::string(kSgrReset) + std::string(kSgrScreenBg) + "  " + sanitize_terminal_text(*view.status));
  }
  for (auto const& widget : view.widgets)
  {
    if (!push(std::string(kSgrBold) + sanitize_terminal_text(widget.title) + std::string(kSgrReset) + std::string(kSgrScreenBg)))
      break;
    for (auto const& line : widget.lines)
    {
      if (!push("  " + sanitize_terminal_text(line)))
        break;
    }
    if (lines.size() + 1 >= max_lines)
      break;
  }
  lines.push_back(plugin_surface_line(std::string(kSgrMuted) + "Ctrl+C stop · 120s max" + std::string(kSgrReset), width, kSgrScreenBg));
  return lines;
}

// TUI thread: build owned modal rows from a borrowed view; no field may supply
// terminal controls. Sanitation also keeps row fitting consistent with admission.
std::vector<std::string> render_plugin_ui_modal(TuiPluginUiModalView const& view, std::size_t width, std::size_t max_lines)
{
  if (!plugin_ui_host_chrome_fits(view.binding, view.kind, width, max_lines))
    return {};

  std::vector<std::string> lines;
  lines.reserve(max_lines);
  auto push = [&](std::string content) {
    if (lines.size() < max_lines)
      lines.push_back(plugin_modal_surface_line(std::move(content), width));
  };

  if (modal_vertical_inset(max_lines) != 0)
    push({});
  push(std::string(kSgrAccent) + "Plugin ID" + std::string(kSgrReset) + std::string(kSgrComposerBg) + " · " + sanitize_terminal_text(view.binding.plugin_id));
  push(std::string(kSgrMuted) + "Command" + std::string(kSgrReset) + std::string(kSgrComposerBg) + " · " + sanitize_terminal_text(view.binding.command));
  auto const kind_label = view.kind == TuiPluginUiKind::Confirm ? std::string("Confirm") : std::string("Select");
  push(std::string(kSgrBold) + kind_label + " · " + sanitize_terminal_text(view.title) + std::string(kSgrReset) + std::string(kSgrComposerBg));
  push(std::string(kSgrMuted) + sanitize_terminal_text(view.description) + std::string(kSgrReset) + std::string(kSgrComposerBg));

  auto const count = plugin_option_count(view);
  auto const budget = plugin_option_row_budget(max_lines);
  auto const first = plugin_first_visible_option(view, max_lines);
  auto const selected = count == 0 ? std::size_t{0} : std::min(view.selected_option, count - 1);
  for (std::size_t visible = 0; visible < budget && first + visible < count; ++visible)
  {
    auto const index = first + visible;
    auto [label, description] = plugin_option_text(view, index);
    std::string line = index == selected ? std::string(kSgrAccent) + "› " + std::string(kSgrReset) + std::string(kSgrComposerBg) : "  ";
    line += index == selected ? std::string(kSgrBold) + label + std::string(kSgrReset) + std::string(kSgrComposerBg) : label;
    if (description && !description->empty())
      line += "  " + std::string(kSgrMuted) + *description + std::string(kSgrReset) + std::string(kSgrComposerBg);
    push(std::move(line));
  }
  auto const bottom_inset = modal_vertical_inset(max_lines);
  while (lines.size() + 1 + bottom_inset < max_lines)
    push({});
  auto const enter = view.kind == TuiPluginUiKind::Confirm ? std::string("Enter confirm") : std::string("Enter select");
  push(std::string(kSgrMuted) + enter + " · Esc cancel · Ctrl+C stop · 120s max" + std::string(kSgrReset));
  if (bottom_inset != 0)
    push({});
  return lines;
}

std::optional<std::size_t> plugin_ui_option_for_modal_row(TuiPluginUiModalView const& view, std::size_t modal_row, std::size_t width, std::size_t max_lines)
{
  if (!plugin_ui_host_chrome_fits(view.binding, view.kind, width, max_lines))
    return std::nullopt;
  auto const first_option_row = std::size_t{4} + modal_vertical_inset(max_lines);
  auto const budget = plugin_option_row_budget(max_lines);
  if (modal_row < first_option_row || modal_row >= first_option_row + budget)
    return std::nullopt;
  auto const option = plugin_first_visible_option(view, max_lines) + modal_row - first_option_row;
  return option < plugin_option_count(view) ? std::optional<std::size_t>(option) : std::nullopt;
}

}  // namespace detail
}  // namespace ava::tui

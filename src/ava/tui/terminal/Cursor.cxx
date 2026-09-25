#include "sys.h"
#include "Context.h"

#include <utility>

namespace ava::tui::terminal {
namespace {

std::string cursor_style_sequence(CursorSettings const& cursor_settings)
{
  std::string sequence = "\x1b[0 q";    // This is the reset sequence.
  if (cursor_settings.style() != CursorStyle::Default)
    sequence[2] += std::to_underlying(cursor_settings.style()) + (cursor_settings.blink() ? 0 : 1);
  return sequence;
}

} // namespace

bool CursorSettings::operator==(CursorSettings const& cursor_settings) const
{
  return style_ == cursor_settings.style_ && (style_ == CursorStyle::Default || blink_ == cursor_settings.blink_);
}

void CursorState::apply(Context* context, CursorSettings const& cursor_settings)
{
  if (cursor_settings == cursor_settings_)
    return;
  context->write_raw_sequence(cursor_style_sequence(cursor_settings));
  cursor_settings_ = cursor_settings;
  cursor_settings_applied_ = true;
}

void CursorState::release(Context* context) const
{
  if (cursor_settings_applied_ && cursor_settings_.style() != CursorStyle::Default)
    context->write_raw_sequence(cursor_style_sequence({CursorStyle::Default}));
}

void CursorState::reapply(Context* context) const
{
  if (cursor_settings_applied_)
    context->write_raw_sequence(cursor_style_sequence(cursor_settings_));
}

} // namespace ava::tui::terminal

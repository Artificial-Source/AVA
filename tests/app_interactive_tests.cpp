#include "sys.h"
#include "tests/support/test_harness.h"
#include "ava/app/interactive_internal.h"

namespace {

void test_display_settings_command_classification_includes_cursor()
{
  using ava::app::interactive_internal::is_display_settings_command;
  expect(is_display_settings_command("/cursor default") && is_display_settings_command("  /cursor underline steady\t") &&
             !is_display_settings_command("/cursor-shaped") && !is_display_settings_command("/cursorbar"),
         "cursor commands trigger TUI display hydration without broad prefix matches");
}

}  // namespace

void run_app_interactive_tests()
{
  test_display_settings_command_classification_includes_cursor();
}

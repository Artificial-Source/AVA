#include "sys.h"
#include "ava/tui/TuiFrontEnd.h"

#include <utility>

namespace ava::tui {

int TuiFrontEnd::run()
{
  // FIXME: Delegate to the existing runtime until its UI state and orchestration have been moved into this frontend.
  return run_interactive_composer();
}

std::string TuiFrontEnd::load_key_bindings()
{
  std::string keybind_status;
  if (auto loaded = ava::tui::load_key_bindings(keybinds_json_path()); loaded)
    key_bindings_ = std::move(*loaded);
  else
    keybind_status = loaded.error().format();
  return keybind_status;
}

} // namespace ava::tui

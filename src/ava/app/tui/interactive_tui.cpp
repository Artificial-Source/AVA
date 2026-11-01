#include "sys.h"
#include "ava/app/onboarding.h"
#include "ava/app/runtime.h"
#include "ava/app/runtime/GitBranch.h"
#include "ava/app/session_title_coordinator.h"
#include "ava/app/tui/TuiFrontEnd.h"
#include "ava/app/tui/command_palette_views.h"
#include "ava/app/tui/interactive_tui_internal.h"
#include "ava/core/version.h"

#include <future>
#include <iostream>
#include <thread>
#include <utility>

namespace ava::app::interactive_internal {

// Compose application-owned startup data, run one frontend, and translate construction/runtime errors to process status.
int run_tui(InteractiveState state)
{
  auto& unlocked_session = state.unlocked_session;
  auto const initial_title_coordinator = runtime::session_ts::rat(unlocked_session)->session_title_coordinator();
  auto const initial_title_catalog_cursor = initial_title_coordinator ? initial_title_coordinator->catalog_changes_since(0).cursor : std::size_t{0};
  auto initial_application_catalog = build_application_catalog_cache(unlocked_session);
  auto initial_catalog_snapshot = initial_application_catalog;
  std::vector<tui::TranscriptItem> initial_transcript;
  if (auto onboarding = first_run_auth_onboarding_message(unlocked_session))
    initial_transcript.push_back(tui::TranscriptItem{.label = "setup", .text = std::move(*onboarding)});
  static_cast<void>(runtime::refresh_git_branch(unlocked_session));

  int result;
  try
  {
    TuiFrontEnd frontend(unlocked_session, std::move(initial_application_catalog), initial_title_catalog_cursor,
                         tui::TuiRuntimeOptions{.app_version = std::string(ava::core::version::kDisplayVersion),
                                                .initial_transcript = std::move(initial_transcript),
                                                .slash_commands = project_slash_command_items(std::move(initial_catalog_snapshot.slash_commands)),
                                                .slash_catalog_generation = initial_catalog_snapshot.slash_catalog_generation,
                                                .file_references = project_file_reference_items(std::move(initial_catalog_snapshot.file_references)),
                                                .workspace_catalog_generation = initial_catalog_snapshot.workspace_catalog_generation});
    frontend.register_frontend_data(unlocked_session);
    // Run the frontend on its own thread, keeping it alive until that thread joins.
    // Transfer both the exit status and any exception back to this thread.
    std::packaged_task<int()> tui_task([&frontend]{
      Debug(init_thread("tui_thread"));
      return frontend.run();
    });
    auto tui_result = tui_task.get_future();
    std::jthread tui_thread(std::move(tui_task));
    // Since now the main thread is going to sleep until `tui_thread` finished, we might as well have used the main thread as "TUI thread";
    // But I feel that this way it is more clear where the TUI thread begins and ends.
    tui_thread.join();
    result = tui_result.get();
  }
  catch (core::Error const& error)
  {
    std::cerr << error.format() << std::endl;
    result = 1;
  }
  std::cout << std::flush;
  return result;
}

}  // namespace ava::app::interactive_internal

#include "sys.h"
#include "tests/support/test_harness.h"
#include "ava/core/Application.h"
#include "ava/core/path.h"
#include "ava/core/trusted_home.h"
#ifdef CWDEBUG
#include "ava/debug/libcwd_output_sink.h"
#endif

#include <array>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unistd.h>
#include "debug.h"

void run_core_mode_tests();
void run_test_harness_tests();
void run_diagnostics_tests();
void run_acp_tests();
void run_session_tests();
void run_session_run_controller_tests();
void run_session_title_coordinator_tests();
void run_branch_summary_coordinator_tests();
void run_subagent_coordinator_tests();
void run_subagent_delivery_manager_tests();
void run_core_json_permission_tests();
void run_curl_transport_process_tests();
void run_clipboard_image_process_tests();
void run_app_command_classification_tests();
void run_app_command_registry_tests();
void run_tools_tests();
void run_config_context_auth_oauth_tests();
void run_provider_config_tests();
void run_provider_user_catalog_tests();
void run_provider_builtin_generic_tests();
void run_command_tests();
void run_app_compaction_tests();
void run_app_interactive_tests();
void run_app_print_tests();
void run_app_event_serialization_tests();
void run_app_rpc_queue_tests();
void run_app_rpc_resolver_tests();
void run_app_rpc_tests();
void run_app_runtime_tests();
void run_app_event_bus_tests();
void run_provider_openai_tests();
void run_provider_anthropic_tests();
void run_provider_gemini_tests();
void run_provider_live_smoke_tests();
void run_agent_loop_resilience_tests();
void run_agent_loop_tests();
void run_agent_tool_dispatcher_tests();
void run_agent_todo_tests();
void run_tool_scheduler_tests();
void run_lsp_tests();
void run_plugin_tests();
void run_plugin_runner_process_tests();
void run_plugin_ui_tests();
void run_process_adoption_posix_tests();
void run_process_capability_posix_tests();
void run_process_deadline_tests();
void run_process_environment_tests();
void run_process_launch_protocol_posix_tests();
void run_process_monitor_tests();
void run_process_readiness_tests();
void run_process_supervisor_tests();
void run_process_supervisor_posix_tests();
void run_mcp_tests();
void run_mermaid_render_coordinator_tests();
void run_permission_rules_tests();
void run_tui_composer_tests();
void run_terminal_horizontal_layout_tests();
void run_terminal_keyboard_input_mode_tests();
void run_terminal_mouse_input_mode_tests();
void run_terminal_color_tests();
void run_terminal_paragraph_tests();
void run_terminal_window_tests();
void run_run_observer_tests();
void run_runtime_diagnostics_tests();
void run_containment_tests();
#ifdef CWDEBUG
void run_debug_tests();
#endif

namespace {

struct TestSuite
{
  std::string_view name;
  void (*run)();
};

constexpr std::array kTestSuites{
    TestSuite{"core_mode", run_core_mode_tests},
    TestSuite{"test_harness", run_test_harness_tests},
    TestSuite{"diagnostics", run_diagnostics_tests},
    TestSuite{"acp", run_acp_tests},
    TestSuite{"session", run_session_tests},
    TestSuite{"session_run_controller", run_session_run_controller_tests},
    TestSuite{"session_title_coordinator", run_session_title_coordinator_tests},
    TestSuite{"branch_summary_coordinator", run_branch_summary_coordinator_tests},
    TestSuite{"subagent_coordinator", run_subagent_coordinator_tests},
    TestSuite{"subagent_delivery_manager", run_subagent_delivery_manager_tests},
    TestSuite{"core_json_permission", run_core_json_permission_tests},
    TestSuite{"curl_transport_process", run_curl_transport_process_tests},
    TestSuite{"clipboard_image_process", run_clipboard_image_process_tests},
    TestSuite{"app_command_classification", run_app_command_classification_tests},
    TestSuite{"app_command_registry", run_app_command_registry_tests},
    TestSuite{"tools", run_tools_tests},
    TestSuite{"config_context_auth_oauth", run_config_context_auth_oauth_tests},
    TestSuite{"provider_config", run_provider_config_tests},
    TestSuite{"provider_user_catalog", run_provider_user_catalog_tests},
    TestSuite{"provider_builtin_generic", run_provider_builtin_generic_tests},
    TestSuite{"command", run_command_tests},
    TestSuite{"app_compaction", run_app_compaction_tests},
    TestSuite{"app_interactive", run_app_interactive_tests},
    TestSuite{"app_print", run_app_print_tests},
    TestSuite{"app_event_serialization", run_app_event_serialization_tests},
    TestSuite{"app_event_bus", run_app_event_bus_tests},
    TestSuite{"app_rpc_queue", run_app_rpc_queue_tests},
    TestSuite{"app_rpc_resolver", run_app_rpc_resolver_tests},
    TestSuite{"app_rpc", run_app_rpc_tests},
    TestSuite{"app_runtime", run_app_runtime_tests},
    TestSuite{"provider_openai", run_provider_openai_tests},
    TestSuite{"provider_anthropic", run_provider_anthropic_tests},
    TestSuite{"provider_gemini", run_provider_gemini_tests},
    TestSuite{"provider_live_smoke", run_provider_live_smoke_tests},
    TestSuite{"agent_loop_resilience", run_agent_loop_resilience_tests},
    TestSuite{"agent_loop", run_agent_loop_tests},
    TestSuite{"agent_tool_dispatcher", run_agent_tool_dispatcher_tests},
    TestSuite{"agent_todo", run_agent_todo_tests},
    TestSuite{"tool_scheduler", run_tool_scheduler_tests},
    TestSuite{"lsp", run_lsp_tests},
    TestSuite{"plugin", run_plugin_tests},
    TestSuite{"plugin_runner_process", run_plugin_runner_process_tests},
    TestSuite{"plugin_ui", run_plugin_ui_tests},
    TestSuite{"process_adoption_posix", run_process_adoption_posix_tests},
    TestSuite{"process_capability_posix", run_process_capability_posix_tests},
    TestSuite{"process_deadline", run_process_deadline_tests},
    TestSuite{"process_environment", run_process_environment_tests},
    TestSuite{"process_launch_protocol_posix", run_process_launch_protocol_posix_tests},
    TestSuite{"process_monitor", run_process_monitor_tests},
    TestSuite{"process_readiness", run_process_readiness_tests},
    TestSuite{"process_supervisor", run_process_supervisor_tests},
    TestSuite{"process_supervisor_posix", run_process_supervisor_posix_tests},
    TestSuite{"mcp", run_mcp_tests},
    TestSuite{"mermaid_render", run_mermaid_render_coordinator_tests},
    TestSuite{"permission_rules", run_permission_rules_tests},
    TestSuite{"tui_composer", run_tui_composer_tests},
    TestSuite{"terminal_horizontal_layout", run_terminal_horizontal_layout_tests},
    TestSuite{"terminal_keyboard_input_mode", run_terminal_keyboard_input_mode_tests},
    TestSuite{"terminal_mouse_input_mode", run_terminal_mouse_input_mode_tests},
    TestSuite{"terminal_color", run_terminal_color_tests},
    TestSuite{"terminal_paragraph", run_terminal_paragraph_tests},
    TestSuite{"terminal_window", run_terminal_window_tests},
    TestSuite{"run_observer", run_run_observer_tests},
    TestSuite{"runtime_diagnostics", run_runtime_diagnostics_tests},
    TestSuite{"containment", run_containment_tests},
#ifdef CWDEBUG
    TestSuite{"debug", run_debug_tests},
#endif
};

// Own the test executable's Application lifecycle and install its per-suite
// libcwd sink before core::Application initializes debugging and allocators.
//
// The suite token determines the private log filename in CWDEBUG builds. The
// sink remains process-owned while the Application may be temporarily absent
// so the core_mode suite can exercise Application lifecycle invariants.
class TestRunnerApplication final : public ava::core::Application
{
 public:
  explicit TestRunnerApplication(CWDEBUG_ONLY(std::string_view debug_suite_token)) : ava::core::Application(CWDEBUG_ONLY(prepare_debug(debug_suite_token)))
  {
#ifdef CWDEBUG
    if (s_marker_pending_)
    {
      Dout(dc::notice, "AVA libcwd routing marker: suite=" << debug_suite_token);
      s_marker_pending_ = false;
    }
#endif
  }

  [[nodiscard]] std::string_view application_name() const noexcept override { return "ava_tests"; }

#ifdef CWDEBUG
  // Return false only when the requested private debug destination could not be prepared safely.
  [[nodiscard]] static bool debug_setup_succeeded() noexcept { return s_output_sink_->setup_succeeded(); }

  // Return the private debug destination setup error, or an empty string after successful setup.
  [[nodiscard]] static std::string const& debug_setup_error() noexcept { return s_output_sink_->setup_error(); }
#endif

  // Can't print ava::core::Application.
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT

 private:
#ifdef CWDEBUG
  // Install or reuse the process-owned per-suite sink and return whether it
  // requires libcwd initialization. Reuse avoids truncating the log when the
  // core_mode suite temporarily releases and restores the Application.
  static bool prepare_debug(std::string_view debug_suite_token)
  {
    if (s_output_sink_)
      return false;
    s_output_sink_ = std::make_unique<ava::debug::LibcwdOutputSink>("ava_tests." + std::string(debug_suite_token));
    s_marker_pending_ = s_output_sink_->enabled();
    return s_output_sink_->enabled();
  }

  static std::unique_ptr<ava::debug::LibcwdOutputSink> s_output_sink_;
  static bool s_marker_pending_;
#endif
};

#ifdef CWDEBUG
//static
std::unique_ptr<ava::debug::LibcwdOutputSink> TestRunnerApplication::s_output_sink_;
//static
bool TestRunnerApplication::s_marker_pending_ = false;
#endif

// Derive the per-suite token used to name the libcwd log file
// (ava_tests.<token>.libcwd.log). "all" when no suite argument is given,
// the suite name when it matches a registered suite, "invalid" otherwise
// (which also keeps path separators in a bad argv[1] out of the filename).
#ifdef CWDEBUG
std::string_view libcwd_suite_token(int argc, char** argv)
{
  if (argc == 1)
    return "all";
  if (argc == 2)
  {
    std::string_view const requested_suite = argv[1];
    for (auto const& suite : kTestSuites)
    {
      if (suite.name == requested_suite)
        return suite.name;
    }
  }
  return "invalid";
}
#endif

void run_suite(TestSuite const& suite)
{
  ava::tests::clear_skip();
  try
  {
    suite.run();
  }
  catch (std::exception const& ex)
  {
    expect(false, std::string(suite.name) + " tests threw: " + ex.what());
  }
  catch (...)
  {
    expect(false, std::string(suite.name) + " tests threw an unknown exception");
  }

  int const failures = ava::tests::failures();
  if (failures == 0 && ava::tests::skip_requested())
  {
    std::cout << suite.name << " tests skipped: " << ava::tests::skip_message() << '\n';
  }
  else if (failures == 0)
  {
    std::cout << suite.name << " tests passed\n";
  }
}

struct OwnedTestFixtureGuard
{
  ~OwnedTestFixtureGuard() noexcept { static_cast<void>(cleanup_owned_test_directories()); }
};

int finalize_test_run(bool report_all_passed, bool allow_skip)
{
  static_cast<void>(cleanup_owned_test_directories());
  int const failures = ava::tests::failures();
  if (failures != 0)
  {
    std::cerr << failures << " test failure(s)\n";
    return 1;
  }
  if (allow_skip && ava::tests::skip_requested())
    return 77;
  if (report_all_passed)
    std::cout << "ava tests passed\n";
  return 0;
}

}  // namespace

int main(int argc, char** argv)
{
  OwnedTestFixtureGuard owned_fixtures;
#ifdef CWDEBUG
  std::string_view const debug_suite_token = libcwd_suite_token(argc, argv);
#endif

  // Construct the process Application before other startup work. Its base
  // initializes debugging before constructing the allocator-owned members.
  std::optional<TestRunnerApplication> application;
  application.emplace(CWDEBUG_ONLY(debug_suite_token));

#ifdef CWDEBUG
  if (!TestRunnerApplication::debug_setup_succeeded())
  {
    std::cerr << "failed to configure libcwd test output: " << TestRunnerApplication::debug_setup_error() << '\n';
    return 2;
  }
#endif

  if (argc > 2)
  {
    std::cerr << "usage: ava_tests [suite]\n";
    return 2;
  }

  // CTest may change the working directory without updating $PWD, which
  // would cause logical_cwd() to throw. Verify and fix $PWD to match the
  // actual working directory (falling back to the physical path).
  try
  {
    ava::core::logical_cwd();
  }
  catch (...)
  {
    Dout(dc::warning, "For correct testing, the environment variable PWD should be set to the Working Directory that ctest is using.");
    Dout(dc::warning, "Run `ctest` as follows:");
    Dout(dc::warning, "    export PWD=\"$BUILDDIR/tests\" && ctest --test-dir \"$BUILDDIR\" --output-on-failure test \"$@\"");
    Dout(dc::warning, "where $BUILDDIR is your build directory and \"$@\" stands for any optional arguments that you want to pass.");

    // Use the physical path for now.
    char buffer[4096];
    if (::getcwd(buffer, sizeof(buffer)) != nullptr)
      ::setenv("PWD", buffer, 1);
  }

  // Resolve and freeze the trusted account once for the whole process before
  // any suite runs. Several suites (tools, agent_loop, agent_loop_resilience,
  // run_observer, containment) drive the bash tool directly, and the bash
  // command planner calls ava::core::cached_trusted_account(), which asserts
  // that HOME was read exactly once and frozen at startup. This call is
  // idempotent and process-wide, so it is a cheap no-op for every suite after
  // the first.
  if (auto result = ava::core::load_account_once_and_freeze(); !result)
    std::cerr << "warning: failed to load trusted account: " << result.error().format() << '\n';

  if (argc == 2)
  {
    std::string_view const requested_suite = argv[1];
    for (auto const& suite : kTestSuites)
    {
      if (suite.name == requested_suite)
      {
        if (suite.name == "core_mode")
          application.reset();
        run_suite(suite);
        // Drop Application before namespace cleanup so Application-owned fixture
        // threads cannot still be using those directories.
        application.reset();
        return finalize_test_run(false, true);
      }
    }

    std::cerr << "unknown test suite: " << requested_suite << "\n";
    std::cerr << "available test suites:";
    for (auto const& suite : kTestSuites)
    {
      std::cerr << ' ' << suite.name;
    }
    std::cerr << '\n';
    return 2;
  }

  for (auto const& suite : kTestSuites)
  {
    if (suite.name == "core_mode")
    {
      application.reset();
      run_suite(suite);
    }
  }
  application.emplace(CWDEBUG_ONLY(debug_suite_token));
  for (auto const& suite : kTestSuites)
  {
    if (suite.name == "core_mode")
      continue;
    run_suite(suite);
  }

  // Drop Application before namespace cleanup so Application-owned fixture
  // threads cannot still be using those directories. Test-function locals have
  // already ended.
  application.reset();
  return finalize_test_run(true, false);
}

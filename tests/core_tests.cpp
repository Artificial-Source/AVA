#include "sys.h"
#include "tests/support/test_harness.h"
#include "ava/app/Application.h"
#include "ava/app/ava_debug.h"
#include "ava/core/path.h"
#include "ava/core/trusted_home.h"
#ifdef CWDEBUG
#include "ava/debug/libcwd_output_sink.h"
#endif

#ifdef DEBUGGLOBAL
#include "utils/GlobalObjectManager.h"
#endif

#include <array>
#include <cstdlib>
#include <iostream>
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
void run_app_line_shell_tests();
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
    TestSuite{"app_line_shell", run_app_line_shell_tests},
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
  suite.run();

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

int print_failures()
{
  int const failures = ava::tests::failures();
  if (failures != 0)
  {
    std::cerr << failures << " test failure(s)\n";
    return 1;
  }
  return 0;
}

}  // namespace

int main(int argc, char** argv)
{
#ifdef DEBUGGLOBAL
  // Keep GlobalObjectManager's transition to main as the first operation.
  GlobalObjectManager::main_entered();
#endif

#ifdef CWDEBUG
  std::string_view const debug_suite_token = libcwd_suite_token(argc, argv);
  ava::debug::LibcwdOutputSink libcwd_output("ava_tests." + std::string(debug_suite_token));
  Debug(ava::app::debug_init(libcwd_output.enabled()));
  if (!libcwd_output.setup_succeeded())
  {
    std::cerr << "failed to configure libcwd test output: " << libcwd_output.setup_error() << '\n';
    return 2;
  }

  Dout(dc::notice, "AVA libcwd routing marker: suite=" << debug_suite_token);
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

  // Give every ordinary test suite the same process-lifetime Application that
  // production constructs before entering app::run. The core_mode suite owns
  // temporary Application instances in order to test the lifecycle contract,
  // so it must run before this process-wide instance is published.
  std::optional<ava::app::Application> application;

  if (argc == 2)
  {
    std::string_view const requested_suite = argv[1];
    for (auto const& suite : kTestSuites)
    {
      if (suite.name == requested_suite)
      {
        if (suite.name != "core_mode")
          application.emplace();
        run_suite(suite);
        if (ava::tests::failures() == 0 && ava::tests::skip_requested())
          return 77;
        return print_failures();
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
      suite.run();
  }
  application.emplace();
  for (auto const& suite : kTestSuites)
  {
    if (suite.name == "core_mode")
      continue;
    suite.run();
  }

  int const failure_status = print_failures();
  if (failure_status == 0)
  {
    std::cout << "ava tests passed\n";
  }
  return failure_status;
}

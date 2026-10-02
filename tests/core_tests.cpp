#include "sys.h"
#include "tests/support/terminal_test_support.h"
#include "tests/support/test_harness.h"
#include "ava/core/Application.h"
#include "ava/core/path.h"
#include "ava/core/trusted_home.h"
#ifdef CWDEBUG
#include "ava/debug/libcwd_output_sink.h"
#endif

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>
#include "debug.h"

void run_core_mode_tests();
void run_core_utf8_tests();
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
void run_terminal_paragraph_tests();
void run_terminal_window_tests();
void run_tui_terminal_lifecycle_protocol_tests();
void prepare_terminal_horizontal_layout_tests(FILE*, FILE*);
void prepare_terminal_paragraph_tests(FILE*, FILE*);
void prepare_terminal_window_tests(FILE*, FILE*);
void prepare_terminal_keyboard_input_mode_test_case(std::string_view, FILE*, FILE*);
void run_terminal_keyboard_input_mode_test_case(std::string_view);
void prepare_terminal_mouse_input_mode_test_case(std::string_view, FILE*, FILE*);
void run_terminal_mouse_input_mode_test_case(std::string_view);
void verify_terminal_mouse_input_mode_test_case(std::string_view, FILE* output);
void prepare_terminal_color_test_case(std::string_view, FILE*, FILE*);
void run_terminal_color_test_case(std::string_view);
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
    TestSuite{"core_utf8", run_core_utf8_tests},
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
    TestSuite{"terminal_paragraph", run_terminal_paragraph_tests},
    TestSuite{"terminal_window", run_terminal_window_tests},
    TestSuite{"tui_terminal_geometry", run_tui_terminal_lifecycle_protocol_tests},
    TestSuite{"run_observer", run_run_observer_tests},
    TestSuite{"runtime_diagnostics", run_runtime_diagnostics_tests},
    TestSuite{"containment", run_containment_tests},
#ifdef CWDEBUG
    TestSuite{"debug", run_debug_tests},
#endif
};

using TerminalCasePrepare = void (*)(std::string_view, FILE*, FILE*);
using TerminalCaseRun = void (*)(std::string_view);
using TerminalCaseVerify = void (*)(std::string_view, FILE* output);

struct IsolatedTerminalSuite
{
  std::string_view name;
  std::span<std::string_view const> cases;
  TerminalCasePrepare prepare;
  TerminalCaseRun run;
  TerminalCaseVerify verify;
};

constexpr std::array<std::string_view, 12> kKeyboardCases{
    "escape_delay_default", "escape_delay_configured", "kitty_reply",  "fallback_reply", "modify_other_keys_reply", "unrelated_input",
    "startup_input_replay", "split_utf8_replay",       "invalid_utf8", "handoff_input",  "idempotent_stop",         "destructor_stop",
};
constexpr std::array<std::string_view, 2> kMouseCases{"context_lifecycle", "handoff_lifecycle"};
constexpr std::array<std::string_view, 5> kColorCases{"srgb_round_trip", "osc4_protocol", "mutable_palette", "xterm_indexed", "portable_pairs"};

constexpr std::array kIsolatedTerminalSuites{
    IsolatedTerminalSuite{"terminal_keyboard_input_mode", kKeyboardCases, prepare_terminal_keyboard_input_mode_test_case,
                          run_terminal_keyboard_input_mode_test_case, nullptr},
    IsolatedTerminalSuite{"terminal_mouse_input_mode", kMouseCases, prepare_terminal_mouse_input_mode_test_case, run_terminal_mouse_input_mode_test_case,
                          verify_terminal_mouse_input_mode_test_case},
    IsolatedTerminalSuite{"terminal_color", kColorCases, prepare_terminal_color_test_case, run_terminal_color_test_case, nullptr},
};

// Own the test executable's Application lifecycle and install its per-suite
// libcwd sink before core::Application initializes debugging and allocators.
//
// The suite token determines the private log filename in CWDEBUG builds. The
// sink and Application remain process-owned for the complete test run.
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
  // Install the process-owned per-suite sink and return whether it requires
  // libcwd initialization. The defensive reuse path avoids replacing a sink.
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
std::string libcwd_suite_token(int argc, char** argv)
{
  if (argc == 1)
    return "all";
  if (argc == 2 || argc == 3)
  {
    std::string_view const requested_suite = argv[1];
    for (auto const& suite : kTestSuites)
    {
      if (argc == 2 && suite.name == requested_suite)
        return std::string(suite.name);
    }
    for (auto const& suite : kIsolatedTerminalSuites)
    {
      if (suite.name == requested_suite)
      {
        if (argc == 2)
          return std::string(suite.name);
        std::string_view const requested_case = argv[2];
        if (std::ranges::find(suite.cases, requested_case) != suite.cases.end())
          return std::string(suite.name) + "." + std::string(requested_case);
      }
    }
  }
  return "invalid";
}
#endif

// Return the ordinary suite descriptor matching `name`, or null when it is not registered.
TestSuite const* find_test_suite(std::string_view name)
{
  for (auto const& suite : kTestSuites)
  {
    if (suite.name == name)
      return &suite;
  }
  return nullptr;
}

// Return the isolated terminal suite descriptor matching `name`, or null for an ordinary suite.
IsolatedTerminalSuite const* find_isolated_terminal_suite(std::string_view name)
{
  for (auto const& suite : kIsolatedTerminalSuites)
  {
    if (suite.name == name)
      return &suite;
  }
  return nullptr;
}

// Report whether `test_case` is registered for this isolated terminal suite.
bool has_terminal_case(IsolatedTerminalSuite const& suite, std::string_view test_case)
{
  return std::ranges::find(suite.cases, test_case) != suite.cases.end();
}

// Report whether an ordinary suite needs the process Application's terminal Context.
bool is_direct_terminal_suite(std::string_view name)
{
  return name == "tui_composer" || name == "terminal_horizontal_layout" || name == "terminal_paragraph" || name == "terminal_window" ||
         name == "tui_terminal_geometry";
}

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

// Run one isolated terminal case through the same exception boundary as ordinary suites.
void run_isolated_terminal_case(IsolatedTerminalSuite const& suite, std::string_view test_case)
{
  ava::tests::clear_skip();
  try
  {
    suite.run(test_case);
  }
  catch (std::exception const& ex)
  {
    expect(false, std::string(suite.name) + "." + std::string(test_case) + " threw: " + ex.what());
  }
  catch (...)
  {
    expect(false, std::string(suite.name) + "." + std::string(test_case) + " threw an unknown exception");
  }
}

struct OwnedTestFixtureGuard
{
  // Create streams before initialization so setup can preload replies consumed by Context startup.
  void prepare_terminal_streams()
  {
    terminal_input.emplace();
    terminal_output.emplace();
  }

  // Create process-lifetime PTY streams for the geometry case before the Application initializes ncurses.
  void prepare_terminal_pty_streams()
  {
    saved_stdout = ::dup(STDOUT_FILENO);
    master_fd = ::posix_openpt(O_RDWR | O_NOCTTY);
    if (saved_stdout < 0 || master_fd < 0 || ::grantpt(master_fd) != 0 || ::unlockpt(master_fd) != 0)
      throw std::runtime_error("failed to create terminal test PTY");
    char* slave_name = ::ptsname(master_fd);
    int const slave_fd = slave_name == nullptr ? -1 : ::open(slave_name, O_RDWR | O_NOCTTY);
    int const output_fd = slave_fd < 0 ? -1 : ::dup(slave_fd);
    winsize size{};
    size.ws_row = 24;
    size.ws_col = 80;
    if (slave_fd < 0 || output_fd < 0 || ::ioctl(slave_fd, TIOCSWINSZ, &size) != 0 || ::dup2(slave_fd, STDOUT_FILENO) < 0)
      throw std::runtime_error("failed to configure terminal test PTY");
    pty_input = ::fdopen(slave_fd, "r+");
    pty_output = ::fdopen(output_fd, "w");
    if (pty_input == nullptr || pty_output == nullptr)
      throw std::runtime_error("failed to open terminal test PTY streams");
  }

  // Initialize the Application-owned Context exactly once from process-lifetime streams.
  void initialize_terminal_for(TestRunnerApplication& application) { application.initialize_terminal_context(output(), input()); }

  // Return the prepared process terminal input stream.
  FILE* input() { return pty_input != nullptr ? pty_input : terminal_input->get(); }

  // Return the prepared process terminal output stream.
  FILE* output() { return pty_output != nullptr ? pty_output : terminal_output->get(); }

  // Release PTY resources only after the Application and its Context have been destroyed.
  ~OwnedTestFixtureGuard() noexcept
  {
    if (pty_input != nullptr)
      static_cast<void>(std::fclose(pty_input));
    if (pty_output != nullptr)
      static_cast<void>(std::fclose(pty_output));
    if (saved_stdout >= 0)
    {
      static_cast<void>(::dup2(saved_stdout, STDOUT_FILENO));
      static_cast<void>(::close(saved_stdout));
    }
    if (master_fd >= 0)
      static_cast<void>(::close(master_fd));
    static_cast<void>(cleanup_owned_test_directories());
  }

 private:
  std::optional<ScopedTmpFile> terminal_input;
  std::optional<ScopedTmpFile> terminal_output;
  FILE* pty_input = nullptr;
  FILE* pty_output = nullptr;
  int master_fd = -1;
  int saved_stdout = -1;
};

// Configure one ordinary terminal suite before Context initialization.
void prepare_direct_terminal_suite(std::string_view suite_name, OwnedTestFixtureGuard& fixtures)
{
  if (suite_name == "tui_terminal_geometry")
  {
    static_cast<void>(setenv("TERM", "xterm-256color", 1));
    fixtures.prepare_terminal_pty_streams();
  }
  else
    fixtures.prepare_terminal_streams();

  if (suite_name == "tui_composer")
    static_cast<void>(setenv("TERM", "xterm-256color", 1));
  else if (suite_name == "terminal_horizontal_layout")
    prepare_terminal_horizontal_layout_tests(fixtures.input(), fixtures.output());
  else if (suite_name == "terminal_paragraph")
    prepare_terminal_paragraph_tests(fixtures.input(), fixtures.output());
  else if (suite_name == "terminal_window")
    prepare_terminal_window_tests(fixtures.input(), fixtures.output());
}

// Run one ava_tests child and fold its result into the parent no-argument or aggregate-suite run.
void run_test_child(std::string_view suite_name, std::optional<std::string_view> test_case = std::nullopt)
{
  pid_t const pid = ::fork();
  if (pid == 0)
  {
    if (test_case)
      ::execl("/proc/self/exe", "ava_tests", suite_name.data(), test_case->data(), static_cast<char*>(nullptr));
    else
      ::execl("/proc/self/exe", "ava_tests", suite_name.data(), static_cast<char*>(nullptr));
    _exit(127);
  }
  if (pid < 0)
  {
    expect(false, "failed to fork isolated test process for " + std::string(suite_name));
    return;
  }

  int status = 0;
  pid_t wait_result;
  do
  {
    wait_result = ::waitpid(pid, &status, 0);
  }
  while (wait_result < 0 && errno == EINTR);
  bool const passed = wait_result == pid && WIFEXITED(status) && (WEXITSTATUS(status) == 0 || WEXITSTATUS(status) == 77);
  expect(passed, "isolated test process failed for " + std::string(suite_name) + (test_case ? "." + std::string(*test_case) : std::string{}));
}

// Run all cases in a suite through fresh processes so Context initialization cannot leak between cases.
void run_isolated_terminal_suite(IsolatedTerminalSuite const& suite)
{
  for (std::string_view const test_case : suite.cases)
    run_test_child(suite.name, test_case);
}

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

} // namespace

int main(int argc, char** argv)
{
  if (argc > 3)
  {
    std::cerr << "usage: ava_tests [suite [case]]\n";
    return 2;
  }

  std::string_view const requested_suite = argc >= 2 ? argv[1] : "";
  std::string_view const requested_case = argc == 3 ? argv[2] : "";
  TestSuite const* const ordinary_suite = argc >= 2 ? find_test_suite(requested_suite) : nullptr;
  IsolatedTerminalSuite const* const isolated_suite = argc >= 2 ? find_isolated_terminal_suite(requested_suite) : nullptr;
  bool const unknown_case = argc == 3 && (isolated_suite == nullptr || !has_terminal_case(*isolated_suite, requested_case));
  bool const unknown_suite = argc == 2 && ordinary_suite == nullptr && isolated_suite == nullptr;

  OwnedTestFixtureGuard owned_fixtures;
  bool const initialize_direct_terminal = argc == 2 && ordinary_suite != nullptr && is_direct_terminal_suite(ordinary_suite->name);
  bool const initialize_isolated_terminal = argc == 3;
  try
  {
    if (initialize_direct_terminal)
      prepare_direct_terminal_suite(ordinary_suite->name, owned_fixtures);
    else if (initialize_isolated_terminal)
    {
      owned_fixtures.prepare_terminal_streams();
      isolated_suite->prepare(requested_case, owned_fixtures.input(), owned_fixtures.output());
    }
  }
  catch (std::exception const& ex)
  {
    std::cerr << "failed to prepare terminal test fixture: " << ex.what() << '\n';
    return 1;
  }

#ifdef CWDEBUG
  std::string const debug_suite_token = libcwd_suite_token(argc, argv);
#endif

  bool debug_setup_failed = false;
  {
    // Construct the process Application in this single composition-root location. Its base initializes debugging before allocator-owned members.
    TestRunnerApplication application{CWDEBUG_ONLY(debug_suite_token)};

#ifdef CWDEBUG
    if (!TestRunnerApplication::debug_setup_succeeded())
    {
      std::cerr << "failed to configure libcwd test output: " << TestRunnerApplication::debug_setup_error() << '\n';
      debug_setup_failed = true;
    }
#endif

    if (debug_setup_failed)
      return 2;

    if (unknown_case || unknown_suite)
    {
      std::cerr << (unknown_case ? "unknown terminal test case: " : "unknown test suite: ") << requested_suite;
      if (unknown_case)
        std::cerr << '.' << requested_case;
      std::cerr << '\n';
      return 2;
    }

    if (initialize_direct_terminal || initialize_isolated_terminal)
      owned_fixtures.initialize_terminal_for(application);

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

    // Resolve and freeze the trusted account once for the whole process before any suite runs.
    if (auto result = ava::core::load_account_once_and_freeze(); !result)
      std::cerr << "warning: failed to load trusted account: " << result.error().format() << '\n';

    if (argc == 3)
      run_isolated_terminal_case(*isolated_suite, requested_case);
    else if (argc == 2)
    {
      if (ordinary_suite != nullptr)
        run_suite(*ordinary_suite);
      else
        run_isolated_terminal_suite(*isolated_suite);
    }
    else
    {
      for (auto const& suite : kTestSuites)
      {
        if (is_direct_terminal_suite(suite.name))
          run_test_child(suite.name);
        else
          run_suite(suite);
      }
      for (auto const& suite : kIsolatedTerminalSuites)
        run_isolated_terminal_suite(suite);
    }
  }

  // Mouse lifecycle assertions include output emitted by the Application-owned Context destructor.
  if (argc == 3 && isolated_suite->verify != nullptr)
    isolated_suite->verify(requested_case, owned_fixtures.output());

  return finalize_test_run(argc == 1, argc != 1);
}

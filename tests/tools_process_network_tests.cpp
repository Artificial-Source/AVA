#include "sys.h"
#include "tests/support/process_group_test_support.h"
#include "tests/support/test_harness.h"
#include "tests/support/test_timeout.h"
#include "ava/containment/containment.h"
#include "ava/http/transport.h"
#include "ava/agent/mode.h"
#include "ava/tools/bash_tool.h"
#include "ava/tools/tool_permission.h"
#include "ava/tools/spill_files.h"
#include "ava/tools/webfetch_tool.h"
#include "ava/tools/websearch_tool.h"
#include "ava/session/session_store.h"
#include "ava/permissions/permission.h"
#include "ava/permissions/permission_rules.h"
#include "ava/provider/provider.h"
#include "ava/core/AnchorSet.h"
#include "ava/core/error.h"
#include "ava/core/ids.h"
#include "ava/core/path.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>

namespace {

std::string read_text_file_for_test(std::filesystem::path const& path)
{
  std::ifstream file(path, std::ios::binary);
  std::ostringstream out;
  out << file.rdbuf();
  return out.str();
}

std::optional<pid_t> read_pid_file_for_test(std::filesystem::path const& path)
{
  std::ifstream file(path, std::ios::binary);
  long long value = 0;
  file >> value;
  if (!file || value <= 0)
    return std::nullopt;
  return static_cast<pid_t>(value);
}

bool wait_for_process_group_exit(pid_t pgid)
{
  auto const deadline = ava::tests::now_plus_seconds(5);
  while (ava::test::process_group_has_live_member(pgid))
  {
    if (std::chrono::steady_clock::now() >= deadline)
      return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return true;
}

ava::core::Result<ava::tools::BashResult> run_bash_for_test(ava::tools::ToolContext context, std::string_view command,
                                                            ava::tools::BashOptions options = {})
{
  if (!context.anchor_set)
  {
    if (context.spill_dir.empty())
      context.spill_dir = context.workspace_dir.parent_path() / (context.workspace_dir.filename().string() + ".command-spill");
    std::error_code error;
    std::filesystem::create_directories(context.spill_dir, error);
    if (error || ::chmod(context.spill_dir.c_str(), S_IRWXU) != 0)
      return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Io, "failed to create command test spill anchor"));
    auto anchors = ava::core::AnchorSet::open({context.workspace_dir, context.spill_dir});
    if (!anchors)
      return std::unexpected(std::move(anchors.error()));
    context.anchor_set = *anchors;
  }
  return ava::tools::run_bash(context, command, options);
}

class RecordingCommandExecutor final : public ava::tools::CommandExecutor
{
 public:
  [[nodiscard]] ava::core::Result<ava::tools::CommandExecutionResult> execute(ava::tools::CommandExecutionRequest request) const override
  {
    requests.push_back(std::move(request));
    if (fail)
      return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Io, "injected command execution failed"));
    return result;
  }

  mutable std::vector<ava::tools::CommandExecutionRequest> requests;
  ava::tools::CommandExecutionResult result;
  bool fail = false;
};

class StaticTransport final : public ava::http::Transport
{
 public:
  explicit StaticTransport(ava::http::HttpResponse response) : response_(std::move(response)) { }

  [[nodiscard]] ava::core::Result<ava::http::HttpResponse> send(ava::http::HttpRequest const& request) override
  {
    requests.push_back(request);
    return response_;
  }

  std::vector<ava::http::HttpRequest> requests;

 private:
  ava::http::HttpResponse response_;
};

class CancelAwareTransport final : public ava::http::Transport
{
 public:
  [[nodiscard]] ava::core::Result<ava::http::HttpResponse> send(ava::http::HttpRequest const& request) override
  {
    requests.push_back(request);
    return ava::http::HttpResponse{.status_code = 200, .headers = {{"content-type", "text/plain"}}, .body = "ok"};
  }

  [[nodiscard]] ava::core::Result<ava::http::HttpResponse> send(ava::http::HttpRequest const& request, CancelCallback cancel_requested) override
  {
    requests.push_back(request);
    saw_cancel_callback = static_cast<bool>(cancel_requested);
    if (cancel_requested && cancel_requested())
    {
      return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Unknown, "transport request canceled"));
    }
    return ava::http::HttpResponse{.status_code = 200, .headers = {{"content-type", "text/plain"}}, .body = "ok"};
  }

  bool saw_cancel_callback = false;
  std::vector<ava::http::HttpRequest> requests;
};

void test_bash_tool()
{
  auto const root = create_empty_root("test_bash_tool");
  expect(::chmod(root.c_str(), S_IRWXU) == 0, "bash fixture root is owner-only for sealed command planning");
  {
    std::ofstream visible(root / "visible-entry", std::ios::binary | std::ios::trunc);
    visible << "visible\n";
  }

  ava::tools::ToolContext const context{.workspace_dir = root, .mode = ava::agent::Mode::Build};
  bool const development_containment_available =
      ava::containment::probe_landlock_abi_version() >= ava::containment::kRequiredLandlockAbiVersion && ava::containment::seccomp_network_filter_supported();

  auto pwd = run_bash_for_test(context, "ls", ava::tools::BashOptions{.timeout = std::chrono::milliseconds(1000)});
  expect(pwd.has_value(), "run_bash auto-allows a sealed standard inspection command");
  if (pwd)
  {
    expect(pwd->exit_code == 0, "run_bash records exit code");
    expect(pwd->output.find("visible-entry") != std::string::npos, "run_bash uses the sealed workspace directory");
  }
  else
    Dout(dc::warning, "run_bash returned error: " << pwd.error());

  auto capped_output = run_bash_for_test(context, "ls", ava::tools::BashOptions{.timeout = std::chrono::milliseconds(1000), .max_bytes = 4});
  expect(capped_output && capped_output->exit_code == 0 && capped_output->truncated && capped_output->byte_limited && capped_output->totals_known &&
             capped_output->output.size() == 4 && capped_output->output_bytes == capped_output->output.size() &&
             capped_output->total_bytes > capped_output->output.size(),
         "local run_bash bounds retained output while reporting exact total bytes");

  ava::tools::ToolContext const line_context{
      .workspace_dir = root,
      .mode = ava::agent::Mode::Build,
      .permission_resolver = [](ava::permissions::PermissionPrompt const&) -> ava::core::Result<ava::permissions::PermissionResolutionDecision> {
        return ava::permissions::PermissionResolution::Allow;
      }};
  auto line_capped_output = run_bash_for_test(line_context, "seq 1 5", ava::tools::BashOptions{.timeout = std::chrono::milliseconds(1000), .max_lines = 2});
  expect(line_capped_output && line_capped_output->exit_code == 0 && line_capped_output->truncated && line_capped_output->line_limited &&
             !line_capped_output->byte_limited && line_capped_output->totals_known && line_capped_output->output == "4\n5\n" &&
             line_capped_output->total_lines == 5 && line_capped_output->output_lines == 2 && line_capped_output->omitted_lines == 3,
         "local run_bash retains output by line tail and reports exact totals before applying byte caps");

  std::vector<ava::tools::ToolProgressEvent> bash_progress;
  ava::tools::ToolContext const progress_context{.workspace_dir = root,
                                                 .mode = ava::agent::Mode::Build,
                                                 .progress_sink = [&bash_progress](ava::tools::ToolProgressEvent const& event) -> ava::core::VoidResult {
                                                   bash_progress.push_back(event);
                                                   return {};
                                                 },
                                                 .current_tool_name = "bash",
                                                 .current_call_id = "call_progress"};
  auto progress_pwd = run_bash_for_test(progress_context, "ls", ava::tools::BashOptions{.timeout = std::chrono::milliseconds(1000)});
  expect(progress_pwd && std::ranges::any_of(bash_progress,
                                             [](ava::tools::ToolProgressEvent const& event) {
                                               return event.call_id == "call_progress" && event.tool_name == "bash" && event.status == "completed";
                                             }),
         "run_bash emits bounded progress events with current call metadata");

  auto const bash_spill_dir = root.parent_path() / (root.filename().string() + "-bash-spill");
  ava::tools::ToolContext const bash_spill_context{
      .workspace_dir = root,
      .spill_dir = bash_spill_dir,
      .mode = ava::agent::Mode::Build,
      .permission_resolver = [](ava::permissions::PermissionPrompt const&) -> ava::core::Result<ava::permissions::PermissionResolutionDecision> {
        return ava::permissions::PermissionResolution::Allow;
      },
      .current_tool_name = "bash",
      .current_call_id = "call/bash:spill"};
  auto bash_spill =
      run_bash_for_test(bash_spill_context, "printf 0123456789abcdef", ava::tools::BashOptions{.timeout = std::chrono::milliseconds(1000), .max_bytes = 4});
  expect(bash_spill && bash_spill->truncated && !bash_spill->spill_path.empty() && bash_spill->spill_path.parent_path() == bash_spill_dir &&
             std::filesystem::exists(bash_spill->spill_path),
         "run_bash spills truncated combined output under the configured spill directory");
  if (bash_spill && !bash_spill->spill_path.empty())
  {
    expect(read_text_file_for_test(bash_spill->spill_path) == "0123456789abcdef",
           "bash spill file contains raw combined output from the beginning of the stream");
  }

  auto capped_spill =
      run_bash_for_test(bash_spill_context, "seq 1 1000000", ava::tools::BashOptions{.timeout = std::chrono::milliseconds(5000), .max_bytes = 1});
  expect(capped_spill && capped_spill->truncated && capped_spill->spill_truncated && !capped_spill->spill_path.empty() &&
             std::filesystem::file_size(capped_spill->spill_path) == ava::tools::kMaxSpillFileBytes,
         "run_bash caps individual spill files and reports spill truncation");

  auto const hijack_path = root / "printenv";
  {
    std::ofstream hijack(hijack_path, std::ios::binary | std::ios::trunc);
    hijack << "#!/bin/sh\nprintf hijacked-path\n";
  }
  expect(chmod(hijack_path.c_str(), 0700) == 0, "test can create executable PATH hijack fixture");
  {
    ScopedEnvVar const path_guard("PATH", root.string() + ":.:relative");
    auto sanitized_pwd = run_bash_for_test(context, "ls", ava::tools::BashOptions{.timeout = std::chrono::milliseconds(1000)});
    expect(!sanitized_pwd && sanitized_pwd.error().format().find("PATH") != std::string::npos,
           "run_bash rejects an unsafe inherited startup PATH before it can influence a sealed command");
  }

  auto denied = run_bash_for_test(context, "rm -rf important");
  expect(!denied, "run_bash fails closed for a destructive command without a critical one-shot resolver");

  auto chained = run_bash_for_test(context, "pwd; rm -rf important");
  expect(!chained, "raw shell syntax fails closed without a critical one-shot resolver");
  auto allowed_prefix_chain = run_bash_for_test(context, "pwd; whoami");
  expect(!allowed_prefix_chain, "an otherwise benign raw shell chain still requires critical one-shot approval");

  expect(!run_bash_for_test(context, "cmake -E cat ~/.config/ava/auth.json"), "a critical cmake file helper fails closed without approval");
  expect(!run_bash_for_test(context, "cmake -P docs/plan.md"), "critical cmake script execution fails closed without approval");
  expect(!run_bash_for_test(context, "cmake -E copy docs/plan.md src/new.cpp"), "a critical cmake copy helper fails closed without approval");

  auto const repository_test_dir = root / "repository-test-fixture";
  auto const repository_test_marker = repository_test_dir / "executed-marker";
  auto const safe_command_bin = root / "sealed-command-bin";
  std::filesystem::create_directories(repository_test_dir);
  std::filesystem::create_directories(safe_command_bin);
  expect(::chmod(repository_test_dir.c_str(), S_IRWXU) == 0 && ::chmod(safe_command_bin.c_str(), S_IRWXU) == 0,
         "repository command fixture paths are owner-only for sealed recipe planning");
  for (auto const name : {"cmake", "ctest"})
  {
    std::ofstream executable(safe_command_bin / name, std::ios::binary | std::ios::trunc);
    executable << "#!/bin/sh\nexit 0\n";
    executable.close();
    expect(::chmod((safe_command_bin / name).c_str(), S_IRUSR | S_IWUSR | S_IXUSR) == 0, "repository command fixture can create a sealed executable identity");
  }
  ScopedEnvVar const repository_path("PATH", safe_command_bin.string() + ":/usr/bin:/bin");
  {
    std::ofstream test_file(repository_test_dir / "CTestTestfile.cmake", std::ios::binary | std::ios::trunc);
    test_file << "add_test(NAME repository_controlled_code COMMAND /usr/bin/touch " << repository_test_marker.generic_string() << ")\n";
  }
  auto repository_build = run_bash_for_test(context, "cmake --build " + repository_test_dir.generic_string());
  expect(development_containment_available ? repository_build && repository_build->exit_code == 0 && !std::filesystem::exists(repository_test_marker)
                                           : !repository_build && repository_build.error().format().find("no_resolver") != std::string::npos,
         "repository builds auto-allow only when verified development containment is available; otherwise they ask and fail closed without a resolver");

  auto repository_test = run_bash_for_test(context, "ctest --test-dir " + repository_test_dir.generic_string());
  expect(development_containment_available ? repository_test && repository_test->exit_code == 0 && !std::filesystem::exists(repository_test_marker)
                                           : !repository_test && repository_test.error().format().find("no_resolver") != std::string::npos,
         "repository tests auto-allow only when verified development containment is available; otherwise they ask and fail closed without a resolver");

  ava::tools::ToolContext const timeout_context{
      .workspace_dir = root,
      .mode = ava::agent::Mode::Build,
      .permission_resolver = [](ava::permissions::PermissionPrompt const&) -> ava::core::Result<ava::permissions::PermissionResolutionDecision> {
        return ava::permissions::PermissionResolution::Allow;
      }};
  auto timeout = run_bash_for_test(timeout_context, "sleep 30", ava::tools::BashOptions{.timeout = std::chrono::milliseconds(50)});
  expect(timeout && timeout->timed_out, "run_bash times out long command");

  auto const timeout_group_file = root / "bash-timeout-child-pgid.txt";
  std::error_code remove_error;
  std::filesystem::remove(timeout_group_file, remove_error);
  int timeout_tree_prompts = 0;
  ava::tools::ToolContext const timeout_tree_context{
      .workspace_dir = root,
      .mode = ava::agent::Mode::Build,
      .permission_resolver =
          [&timeout_tree_prompts](ava::permissions::PermissionPrompt const& prompt) -> ava::core::Result<ava::permissions::PermissionResolutionDecision> {
        ++timeout_tree_prompts;
        expect(prompt.operation == ava::permissions::Operation::RunCommand, "bash process-tree timeout resolver receives run operation");
        return ava::permissions::PermissionResolution::Allow;
      }};
  auto timeout_tree = run_bash_for_test(timeout_tree_context, "/bin/sh -c \"sleep 30 & printf $$ > " + timeout_group_file.generic_string() + "; wait\"",
                                        ava::tools::BashOptions{.timeout = std::chrono::milliseconds(4000), .max_bytes = 1024});
  auto const timeout_pgid = read_pid_file_for_test(timeout_group_file);
  expect(timeout_tree && timeout_tree->timed_out && timeout_tree_prompts == 1 && timeout_pgid && wait_for_process_group_exit(*timeout_pgid),
         "run_bash timeout terminates child processes in the command process group");

  int cancel_checks = 0;
  ava::tools::ToolContext const cancel_context{
      .workspace_dir = root,
      .mode = ava::agent::Mode::Build,
      .permission_resolver = [](ava::permissions::PermissionPrompt const&) -> ava::core::Result<ava::permissions::PermissionResolutionDecision> {
        return ava::permissions::PermissionResolution::Allow;
      },
      .cancel_requested =
          [&cancel_checks] {
            ++cancel_checks;
            return cancel_checks >= 3;
          }};
  auto canceled = run_bash_for_test(cancel_context, "sleep 30", ava::tools::BashOptions{.timeout = std::chrono::milliseconds(5000)});
  expect(canceled && canceled->canceled && !canceled->timed_out && canceled->exit_code == -1,
         "run_bash observes tool cancellation and reports a canceled process result");

  auto const cancel_group_file = root / "bash-cancel-child-pgid.txt";
  std::filesystem::remove(cancel_group_file, remove_error);
  int cancel_tree_prompts = 0;
  ava::tools::ToolContext const cancel_tree_context{
      .workspace_dir = root,
      .mode = ava::agent::Mode::Build,
      .permission_resolver =
          [&cancel_tree_prompts](ava::permissions::PermissionPrompt const& prompt) -> ava::core::Result<ava::permissions::PermissionResolutionDecision> {
        ++cancel_tree_prompts;
        expect(prompt.operation == ava::permissions::Operation::RunCommand, "bash process-tree cancel resolver receives run operation");
        return ava::permissions::PermissionResolution::Allow;
      },
      .cancel_requested = [&cancel_group_file] { return std::filesystem::exists(cancel_group_file); }};
  auto cancel_tree = run_bash_for_test(cancel_tree_context, "/bin/sh -c \"sleep 30 & printf $$ > " + cancel_group_file.generic_string() + "; wait\"",
                                       ava::tools::BashOptions{.timeout = std::chrono::milliseconds(12000), .max_bytes = 1024});
  auto const cancel_pgid = read_pid_file_for_test(cancel_group_file);
  expect(
      cancel_tree && cancel_tree->canceled && !cancel_tree->timed_out && cancel_tree_prompts == 1 && cancel_pgid && wait_for_process_group_exit(*cancel_pgid),
      "run_bash cancellation terminates child processes in the command process group");

  auto ask_without_resolver = run_bash_for_test(context, "true");
  expect(!ask_without_resolver && ask_without_resolver.error().format().find("resolution: no_resolver") != std::string::npos,
         "run_bash fails closed for ask decisions without a resolver");

  int bash_prompts = 0;
  ava::tools::ToolContext allow_context{
      .workspace_dir = root,
      .mode = ava::agent::Mode::Build,
      .permission_resolver =
          [&bash_prompts](ava::permissions::PermissionPrompt const& prompt) -> ava::core::Result<ava::permissions::PermissionResolutionDecision> {
        ++bash_prompts;
        expect(prompt.operation == ava::permissions::Operation::RunCommand, "bash resolver receives run operation");
        expect(prompt.command == "true", "bash resolver receives command text");
        return ava::permissions::PermissionResolution::Allow;
      }};
  auto ask_allowed = run_bash_for_test(allow_context, "true");
  expect(ask_allowed && ask_allowed->exit_code == 0 && bash_prompts == 1, "run_bash allows ask decisions when resolver allows once");

  ava::tools::ToolContext deny_context{
      .workspace_dir = root,
      .mode = ava::agent::Mode::Build,
      .permission_resolver = [](ava::permissions::PermissionPrompt const&) -> ava::core::Result<ava::permissions::PermissionResolutionDecision> {
        return ava::permissions::PermissionResolution::Deny;
      }};
  auto ask_denied = run_bash_for_test(deny_context, "true");
  expect(!ask_denied && ask_denied.error().format().find("resolution: deny") != std::string::npos, "run_bash fails closed when resolver denies ask decisions");

  ava::tools::ToolContext failing_context{
      .workspace_dir = root,
      .mode = ava::agent::Mode::Build,
      .permission_resolver = [](ava::permissions::PermissionPrompt const&) -> ava::core::Result<ava::permissions::PermissionResolutionDecision> {
        return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Io, "resolver failed"));
      }};
  auto ask_failed = run_bash_for_test(failing_context, "true");
  expect(!ask_failed && ask_failed.error().format().find("resolution: resolver_failed") != std::string::npos, "run_bash fails closed when resolver fails");
}

void test_bash_runtime_and_invocation_contract()
{
  auto const root = temp_root() / "bash-runtime-invocation-contract";
  std::error_code cleanup;
  std::filesystem::remove_all(root, cleanup);
  std::filesystem::create_directories(root);
  expect(::chmod(root.c_str(), S_IRWXU) == 0, "bash runtime contract fixture workspace is owner-only");

  std::vector<ava::tools::PermissionAuditEvent> model_audits;
  ava::tools::ToolContext model_context{.workspace_dir = root,
                                        .mode = ava::agent::Mode::Build,
                                        .permission_audit_sink = [&model_audits](ava::tools::PermissionAuditEvent const& event) -> ava::core::VoidResult {
                                          model_audits.push_back(event);
                                          return {};
                                        }};
  auto model_git = run_bash_for_test(model_context, "git status", ava::tools::BashOptions{.timeout = std::chrono::milliseconds(1000)});
  auto const model_metadata = model_audits.empty() ? std::optional<ava::permissions::CommandPermissionMetadata>{} : model_audits.front().command_metadata;

  auto legacy_context = model_context;
  legacy_context.command_runtime.mode = ava::command::CommandRuntimeMode::Legacy;
  auto legacy = run_bash_for_test(legacy_context, "git status");
  auto prompt_only_context = model_context;
  prompt_only_context.command_runtime.mode = ava::command::CommandRuntimeMode::PromptOnly;
  auto prompt_only = run_bash_for_test(prompt_only_context, "git status");

  std::optional<ava::permissions::PermissionPrompt> user_prompt;
  auto user_context = model_context;
  user_context.permission_resolver =
      [&user_prompt](ava::permissions::PermissionPrompt const& prompt) -> ava::core::Result<ava::permissions::PermissionResolutionDecision> {
    user_prompt = prompt;
    return ava::permissions::PermissionResolution::AllowSessionGrant;
  };
  auto user_git = run_bash_for_test(
      user_context, "git status",
      ava::tools::BashOptions{.timeout = std::chrono::milliseconds(1000), .invocation_source = ava::tools::BashOptions::InvocationSource::UserRawShell});

  expect(model_context.command_runtime.mode == ava::command::CommandRuntimeMode::Enabled && model_git && model_metadata &&
             model_metadata->level == ava::command::CommandLevel::Standard &&
             model_metadata->execution_domain == ava::command::CommandExecutionDomain::DirectArgv &&
             model_metadata->family == ava::command::CommandFamily::Inspection && !legacy && !prompt_only &&
             legacy.error().message().find("Enabled") != std::string::npos && prompt_only.error().message().find("Enabled") != std::string::npos,
         "Enabled is the ToolContext default and only executable runtime mode; model git status remains a Standard direct-argv recipe");
  expect(!user_git && user_prompt && user_prompt->command_metadata && user_prompt->command_metadata->level == ava::command::CommandLevel::Critical &&
             user_prompt->command_metadata->family == ava::command::CommandFamily::RawShell &&
             user_prompt->command_metadata->execution_domain == ava::command::CommandExecutionDomain::RawShell &&
             user_prompt->command_metadata->backend_maximum_scope == ava::command::InteractiveScope::Once &&
             !ava::permissions::command_permission_allows_reusable_grant(*user_prompt->command_metadata),
         "explicit user shell source makes git status Critical raw-shell, prompts, and rejects reusable grants");
}

void test_injected_command_executor()
{
  auto const root = create_empty_root("test_injected_command_executor");

  auto const workspace = root / "workspace";
  std::filesystem::create_directories(workspace);
  expect(::chmod(temp_root().c_str(), S_IRWXU) == 0 && ::chmod(workspace.c_str(), S_IRWXU) == 0,
         "injected command workspace is owner-only for sealed command planning");
  auto executor = std::make_shared<RecordingCommandExecutor>();
  executor->result = ava::tools::CommandExecutionResult{
      .exit_code = 0, .output = "one\ntwo\nthree\n", .containment_applied = false, .containment_profile_id = {}, .containment_network_mode = {}};
  int prompts = 0;
  std::string permission_fingerprint;
  ava::tools::ToolContext context{
      .workspace_dir = workspace,
      .mode = ava::agent::Mode::Build,
      .permission_resolver = [&prompts, &permission_fingerprint](
                                 ava::permissions::PermissionPrompt const& prompt) -> ava::core::Result<ava::permissions::PermissionResolutionDecision> {
        ++prompts;
        expect(prompt.operation == ava::permissions::Operation::RunCommand, "injected command permission uses the command operation");
        expect(prompt.command_metadata && prompt.command_metadata->level == ava::command::CommandLevel::Critical &&
                   !prompt.command_metadata->executor_identity_verified &&
                   prompt.command_metadata->backend_maximum_scope == ava::command::InteractiveScope::Once,
               "injected command permission treats the executor as unverified and one-shot only");
        permission_fingerprint = prompt.command_metadata ? prompt.command_metadata->fingerprint : std::string{};
        return ava::permissions::PermissionResolution::Allow;
      },
      .command_executor = executor};
  auto result =
      run_bash_for_test(context, "printf 'one two'", ava::tools::BashOptions{.timeout = std::chrono::milliseconds(900), .max_bytes = 8, .max_lines = 2});
  expect(prompts == 1, "unverified injected execution prompts exactly once");
  expect(result && result->line_limited && result->totals_known && result->total_lines == 3 && result->total_bytes == executor->result.output.size() &&
             result->output == "three\n",
         "unverified injected execution returns the expected locally bounded result");
  expect(executor->requests.size() == 1, "unverified injected execution sends exactly one request");
  if (executor->requests.size() == 1)
  {
    auto const& request = executor->requests.front();
    expect(request.argv.size() == 3 && request.argv[1] == "-c" && request.argv[2] == "printf 'one two'",
           "unverified injected execution sends the expected shell argv");
    expect(request.cwd == workspace.lexically_normal() && request.timeout == std::chrono::milliseconds(900) && request.output_byte_limit == 8 &&
               request.plan_metadata && request.plan_metadata->fingerprint.starts_with("sha256:ava-command-plan-") &&
               request.plan_metadata->fingerprint == permission_fingerprint && !request.plan_metadata->executor_identity_verified &&
               request.environment_profile && !request.environment_profile->local_execution_authority,
           "unverified injected execution sends the expected one-shot plan and redacted environment contract");
  }

  ava::tools::ToolContext denied = context;
  denied.permission_resolver = [](ava::permissions::PermissionPrompt const&) -> ava::core::Result<ava::permissions::PermissionResolutionDecision> {
    return ava::permissions::PermissionResolution::Deny;
  };
  auto denied_result = run_bash_for_test(denied, "touch denied-marker");
  auto raw_shell = run_bash_for_test(context, "printf ok; touch denied-marker");
  expect(!denied_result, "injected execution does not call the executor after permission denial");
  expect(static_cast<bool>(raw_shell), "an approved raw shell plan reaches the injected executor");
  expect(executor->requests.size() == 2, "permission denial and approved raw shell execution produce exactly two executor requests");
  if (executor->requests.size() == 2)
  {
    auto const& request = executor->requests.back();
    expect(request.argv.size() == 3 && request.argv[1] == "-c" && request.argv[2] == "printf ok; touch denied-marker",
           "approved raw shell execution sends the expected argv to the injected executor");
  }
  expect(!std::filesystem::exists(workspace / "denied-marker"), "injected execution never creates the denied local marker");

  executor->fail = true;
  auto failed = run_bash_for_test(context, "true");
  expect(!failed, "an installed command executor failure returns an error instead of falling back to local fork/exec");
  if (!failed)
    expect(failed.error().message() == "injected command execution failed", "injected command executor failure preserves the exact error contract");
}

void test_sealed_local_bash_contract()
{
  auto const root = temp_root() / "sealed-local-bash-contract";
  auto const workspace = root / "workspace";
  auto const first_bin = root / "first-bin";
  auto const second_bin = root / "second-bin";
  std::error_code cleanup;
  std::filesystem::remove_all(root, cleanup);
  std::filesystem::create_directories(workspace);
  std::filesystem::create_directories(first_bin);
  std::filesystem::create_directories(second_bin);
  expect(::chmod(temp_root().c_str(), S_IRWXU) == 0 && ::chmod(root.c_str(), S_IRWXU) == 0 && ::chmod(workspace.c_str(), S_IRWXU) == 0 &&
             ::chmod(first_bin.c_str(), S_IRWXU) == 0 && ::chmod(second_bin.c_str(), S_IRWXU) == 0,
         "sealed local bash fixture owns every planning root");
  auto write_executable = [](std::filesystem::path const& path, std::string_view body) {
    std::ofstream executable(path, std::ios::binary | std::ios::trunc);
    executable << body;
    executable.close();
    return ::chmod(path.c_str(), S_IRUSR | S_IWUSR | S_IXUSR) == 0;
  };
  expect(write_executable(first_bin / "inspect",
                          "#!/bin/sh\nprintf 'argc=%s first=<%s> second=<%s>\\n' \"$#\" \"$1\" \"$2\"\n"
                          "if [ -z \"${AVA_BASH_SECRET_SENTINEL+x}\" ]; then printf 'sentinels=absent\\n'; else printf 'sentinels=present\\n'; fi\n"
                          "if IFS= read -r line; then printf 'stdin=data\\n'; else printf 'stdin=eof\\n'; fi\n") &&
             write_executable(first_bin / "chosen", "#!/bin/sh\nprintf 'first-choice\\n'\n") &&
             write_executable(second_bin / "chosen", "#!/bin/sh\nprintf 'second-choice\\n'\n") &&
             write_executable(first_bin / "stale", "#!/bin/sh\nprintf old-stale\\n") && write_executable(first_bin / "npm", "#!/bin/sh\nprintf npm-ran\\n") &&
             write_executable(first_bin / "python", "#!/bin/sh\nprintf python-ran\\n") &&
             write_executable(first_bin / "python3", "#!/bin/sh\nprintf 'env-python-ran\\n'\n") &&
             write_executable(first_bin / "env-python", "#!/usr/bin/env python3\nprintf 'script-body-should-not-run-directly\\n'\n") &&
             write_executable(first_bin / "bash", "#!/bin/sh\nprintf bash-ran\\n") && write_executable(first_bin / "rm", "#!/bin/sh\nprintf rm-ran\\n"),
         "sealed local bash fixture creates executable identities");

  ScopedEnvVar const path_guard("PATH", first_bin.string() + ":/usr/bin:/bin");
  bool const development_containment_available =
      ava::containment::probe_landlock_abi_version() >= ava::containment::kRequiredLandlockAbiVersion && ava::containment::seccomp_network_filter_supported();
  ScopedEnvVar const sentinel_guard("AVA_BASH_SECRET_SENTINEL", "must-not-reach-child");
  std::vector<ava::permissions::PermissionPrompt> prompts;
  std::vector<ava::tools::PermissionAuditEvent> audits;
  ava::tools::ToolContext const context{
      .workspace_dir = workspace,
      .mode = ava::agent::Mode::Build,
      .permission_resolver = [&prompts](ava::permissions::PermissionPrompt const& prompt) -> ava::core::Result<ava::permissions::PermissionResolutionDecision> {
        prompts.push_back(prompt);
        return ava::permissions::PermissionResolution::Allow;
      },
      .permission_audit_sink = [&audits](ava::tools::PermissionAuditEvent const& event) -> ava::core::VoidResult {
        audits.push_back(event);
        return {};
      }};

  auto const rule_store = ava::permissions::PermissionRuleStore{
      .global_rules_file = root / "permission-rules.json", .workspace_rules_file = workspace / ".ava" / "permission-rules.json", .workspace_dir = workspace};
  auto deny_ls =
      ava::permissions::add_persistent_permission_rule(rule_store, ava::permissions::PermissionRuleDraft{.scope = ava::permissions::PermissionRuleScope::Global,
                                                                                                         .action = ava::permissions::PermissionAction::Deny,
                                                                                                         .operation = ava::permissions::Operation::RunCommand,
                                                                                                         .mode = ava::permissions::PermissionRuleMode::Build,
                                                                                                         .tool_name = "bash",
                                                                                                         .target_path = {},
                                                                                                         .command = "ls",
                                                                                                         .command_recipe_key = {},
                                                                                                         .recipe_display = {},
                                                                                                         .critical_acknowledged = false,
                                                                                                         .reason = "test operator deny",
                                                                                                         .actor = "test"});
  auto denied_context = context;
  denied_context.auto_allow_deny_preflight = ava::permissions::build_persistent_permission_deny_preflight(rule_store);
  auto denied_inspection = run_bash_for_test(denied_context, "ls", ava::tools::BashOptions{.timeout = std::chrono::milliseconds(1000)});
  expect(deny_ls && !denied_inspection && prompts.empty(),
         "an explicit persistent deny remains authoritative before a standard inspection command can auto-run");
  audits.clear();

  auto exact = run_bash_for_test(context, "inspect '' second", ava::tools::BashOptions{.timeout = std::chrono::milliseconds(1000)});
  auto audit_json = audits.empty() ? std::string{} : ava::tools::permission_audit_data_json(audits.front());
  std::array<std::string_view, 15> const audit_fields{"\"level\"",
                                                      "\"family\"",
                                                      "\"fingerprint\"",
                                                      "\"execution_domain\"",
                                                      "\"resolved_executable\"",
                                                      "\"origin\"",
                                                      "\"cwd\"",
                                                      "\"containment_available\"",
                                                      "\"containment_status\"",
                                                      "\"backend_maximum_scope\"",
                                                      "\"global_recipe_key\"",
                                                      "\"workspace_recipe_key\"",
                                                      "\"recipe_display\"",
                                                      "\"effective_allowed_scopes\"",
                                                      "\"environment_profile_id\""};
  bool const complete_audit_metadata =
      std::ranges::all_of(audit_fields, [&audit_json](std::string_view field) { return audit_json.find(field) != std::string::npos; });
  auto secret_audit_event = audits.empty() ? ava::tools::PermissionAuditEvent{} : audits.front();
  secret_audit_event.operation = ava::permissions::Operation::RunCommand;
  secret_audit_event.command = "npm run test -- --token never-persist-this";
  secret_audit_event.command_metadata = ava::permissions::CommandPermissionMetadata{};
  secret_audit_event.command_metadata->level = ava::command::CommandLevel::Critical;
  auto const secret_audit_json = ava::tools::permission_audit_data_json(secret_audit_event);
  auto env_python = run_bash_for_test(context, "env-python", ava::tools::BashOptions{.timeout = std::chrono::milliseconds(1000)});
  expect(exact && exact->output.find("argc=2 first=<> second=<second>") != std::string::npos && exact->output.find("sentinels=absent") != std::string::npos &&
             exact->output.find("stdin=eof") != std::string::npos && env_python && env_python->output == "env-python-ran\n" && prompts.size() == 2 &&
             prompts.front().command_metadata && !audits.empty() && audits.front().command_metadata &&
             prompts.front().command_metadata->fingerprint == audits.front().command_metadata->fingerprint && complete_audit_metadata &&
             audit_json.find("must-not-reach-child") == std::string::npos && audit_json.find("AVA_BASH_SECRET_SENTINEL") == std::string::npos &&
             secret_audit_json.find("never-persist-this") == std::string::npos && secret_audit_json.find("<redacted one-shot command>") != std::string::npos,
         "one sealed plan carries exact empty argv, stdin EOF, redacted environment, and the same fingerprint through prompt and audit");

  ava::tools::ToolContext no_relookup_context = context;
  no_relookup_context.permission_resolver =
      [&second_bin](ava::permissions::PermissionPrompt const&) -> ava::core::Result<ava::permissions::PermissionResolutionDecision> {
    static_cast<void>(::setenv("PATH", (second_bin.string() + ":/usr/bin:/bin").c_str(), 1));
    return ava::permissions::PermissionResolution::Allow;
  };
  auto no_relookup = run_bash_for_test(no_relookup_context, "chosen", ava::tools::BashOptions{.timeout = std::chrono::milliseconds(1000)});
  expect(no_relookup && no_relookup->output == "first-choice\n", "local execution uses the prepared executable without a post-permission PATH lookup");

  static_cast<void>(::setenv("PATH", (first_bin.string() + ":/usr/bin:/bin").c_str(), 1));
  ava::tools::ToolContext stale_context = context;
  stale_context.permission_resolver =
      [&first_bin, &write_executable](ava::permissions::PermissionPrompt const&) -> ava::core::Result<ava::permissions::PermissionResolutionDecision> {
    return write_executable(first_bin / "stale", "#!/bin/sh\nprintf replacement\\n")
               ? ava::core::Result<ava::permissions::PermissionResolutionDecision>(ava::permissions::PermissionResolution::Allow)
               : ava::core::Result<ava::permissions::PermissionResolutionDecision>(
                     std::unexpected(ava::core::Error(ava::core::ErrorCategory::Io, "failed to replace stale executable")));
  };
  auto stale = run_bash_for_test(stale_context, "stale", ava::tools::BashOptions{.timeout = std::chrono::milliseconds(1000)});
  expect(!stale && stale.error().format().find("stale") != std::string::npos, "stale executable identity blocks execution after permission");

  std::vector<ava::permissions::PermissionPrompt> critical_prompts;
  ava::tools::ToolContext deny_critical_context{
      .workspace_dir = workspace,
      .mode = ava::agent::Mode::Build,
      .permission_resolver =
          [&critical_prompts](ava::permissions::PermissionPrompt const& prompt) -> ava::core::Result<ava::permissions::PermissionResolutionDecision> {
        critical_prompts.push_back(prompt);
        return ava::permissions::PermissionResolution::Deny;
      }};
  auto npm = run_bash_for_test(ava::tools::ToolContext{.workspace_dir = workspace, .mode = ava::agent::Mode::Build}, "npm run test");
  auto python_bare = run_bash_for_test(deny_critical_context, "python -c 'print(1)'");
  auto python_absolute = run_bash_for_test(deny_critical_context, (first_bin / "python").string() + " -c 'print(1)'");
  auto bash_inline = run_bash_for_test(deny_critical_context, "bash -c true");
  auto destructive = run_bash_for_test(deny_critical_context, "rm -rf remove-me");
  auto raw = run_bash_for_test(deny_critical_context, "printf raw; true");
  bool const all_critical = critical_prompts.size() == 5 && std::ranges::all_of(critical_prompts, [](ava::permissions::PermissionPrompt const& prompt) {
                              return prompt.command_metadata && prompt.command_metadata->level == ava::command::CommandLevel::Critical &&
                                     prompt.command_metadata->backend_maximum_scope == ava::command::InteractiveScope::Once;
                            });
  bool const npm_behavior = development_containment_available ? npm && npm->exit_code == 0 && npm->output.find("npm-ran") != std::string::npos
                                                              : !npm && npm.error().format().find("no_resolver") != std::string::npos;
  expect(npm_behavior && !python_bare && !python_absolute && !bash_inline && !destructive && !raw && all_critical &&
             critical_prompts[0].command_metadata->family == critical_prompts[1].command_metadata->family &&
             critical_prompts[0].command_metadata->resolved_executable == critical_prompts[1].command_metadata->resolved_executable,
         "npm project code auto-allows under verified development containment, while bare/absolute Python, bash, destructive, and raw-shell commands remain "
         "critical one-shot prompts");

  auto const binary_path = first_bin / "descriptor-binary";
  std::error_code copy_error;
  std::filesystem::copy_file("/bin/true", binary_path, std::filesystem::copy_options::overwrite_existing, copy_error);
  if (!copy_error)
    static_cast<void>(::chmod(binary_path.c_str(), S_IRUSR | S_IWUSR | S_IXUSR));
  auto binary = run_bash_for_test(context, "descriptor-binary", ava::tools::BashOptions{.timeout = std::chrono::milliseconds(1000)});

  auto const bound_script = first_bin / "descriptor-race";
  auto const replacement_script = first_bin / "descriptor-race.replacement";
  bool swapped_after_binding = false;
  bool const race_script_written = write_executable(bound_script, "#!/bin/sh\nprintf approved-descriptor\n") &&
                                   write_executable(replacement_script, "#!/bin/sh\nprintf replacement-must-not-run\n");
  ava::tools::ToolContext bound_context = context;
  bound_context.announce_execution_after_permission = true;
  bound_context.execution_started = std::make_shared<std::atomic_bool>(false);
  bound_context.progress_sink = [&bound_script, &replacement_script,
                                 &swapped_after_binding](ava::tools::ToolProgressEvent const& event) -> ava::core::VoidResult {
    if (event.status != "in_progress")
      return {};
    std::error_code rename_error;
    std::filesystem::rename(replacement_script, bound_script, rename_error);
    if (rename_error)
      return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Io, "failed to swap descriptor race fixture"));
    swapped_after_binding = true;
    return {};
  };
  auto bound_script_result = run_bash_for_test(bound_context, "descriptor-race", ava::tools::BashOptions{.timeout = std::chrono::milliseconds(1000)});

  auto const bound_interpreter = first_bin / "descriptor-interpreter";
  auto const replacement_interpreter = first_bin / "descriptor-interpreter.replacement";
  auto const interpreter_script = first_bin / "descriptor-interpreter-race";
  std::error_code interpreter_copy_error;
  std::filesystem::copy_file("/bin/sh", bound_interpreter, std::filesystem::copy_options::overwrite_existing, interpreter_copy_error);
  std::error_code replacement_copy_error;
  std::filesystem::copy_file("/bin/false", replacement_interpreter, std::filesystem::copy_options::overwrite_existing, replacement_copy_error);
  bool interpreter_swapped_after_binding = false;
  bool const interpreter_fixture_written = !interpreter_copy_error && !replacement_copy_error &&
                                           ::chmod(bound_interpreter.c_str(), S_IRUSR | S_IWUSR | S_IXUSR) == 0 &&
                                           ::chmod(replacement_interpreter.c_str(), S_IRUSR | S_IWUSR | S_IXUSR) == 0 &&
                                           write_executable(interpreter_script,
                                                            "#!" + bound_interpreter.string() + "\nprintf approved-interpreter-descriptor\n");
  ava::tools::ToolContext interpreter_context = context;
  interpreter_context.announce_execution_after_permission = true;
  interpreter_context.execution_started = std::make_shared<std::atomic_bool>(false);
  interpreter_context.progress_sink = [&bound_interpreter, &replacement_interpreter,
                                       &interpreter_swapped_after_binding](ava::tools::ToolProgressEvent const& event) -> ava::core::VoidResult {
    if (event.status != "in_progress")
      return {};
    std::error_code rename_error;
    std::filesystem::rename(replacement_interpreter, bound_interpreter, rename_error);
    if (rename_error)
      return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Io, "failed to swap descriptor-bound interpreter fixture"));
    interpreter_swapped_after_binding = true;
    return {};
  };
  auto bound_interpreter_result =
      run_bash_for_test(interpreter_context, "descriptor-interpreter-race", ava::tools::BashOptions{.timeout = std::chrono::milliseconds(1000)});

  auto const failed_script = first_bin / "descriptor-exec-failure";
  bool const failure_script_written = write_executable(failed_script, "#!/bin/sh\nprintf must-not-start\n");
  ava::tools::ToolContext failure_context = context;
  failure_context.announce_execution_after_permission = true;
  failure_context.execution_started = std::make_shared<std::atomic_bool>(false);
  failure_context.progress_sink = [&failed_script](ava::tools::ToolProgressEvent const& event) -> ava::core::VoidResult {
    if (event.status == "in_progress" && ::chmod(failed_script.c_str(), S_IRUSR | S_IWUSR) != 0)
      return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Io, "failed to revoke descriptor-exec fixture permission"));
    return {};
  };
  auto descriptor_exec_failure =
      run_bash_for_test(failure_context, "descriptor-exec-failure", ava::tools::BashOptions{.timeout = std::chrono::milliseconds(1000)});

  expect(!copy_error && binary && binary->exit_code == 0, "a regular binary executes from its approved descriptor");
  expect(race_script_written && swapped_after_binding && bound_script_result && bound_script_result->exit_code == 0 &&
             bound_script_result->output == "approved-descriptor",
         bound_script_result ? "a shebang script executes from its approved descriptor after a post-binding canonical-path replacement: output=" +
                                   bound_script_result->output
                             : "a shebang script executes from its approved descriptor after a post-binding canonical-path replacement: " +
                                   bound_script_result.error().format());
  expect(interpreter_fixture_written && interpreter_swapped_after_binding && bound_interpreter_result && bound_interpreter_result->exit_code == 0 &&
             bound_interpreter_result->output == "approved-interpreter-descriptor",
         bound_interpreter_result ? "a shebang interpreter executes from its approved descriptor after a post-binding pathname replacement: output=" +
                                        bound_interpreter_result->output
                                  : "a shebang interpreter executes from its approved descriptor after a post-binding pathname replacement: " +
                                        bound_interpreter_result.error().format());
  expect(failure_script_written && !descriptor_exec_failure && descriptor_exec_failure.error().message().find("bound descriptor") != std::string::npos &&
             descriptor_exec_failure.error().format().find("descriptor-exec-failure") == std::string::npos,
         "descriptor-exec failure is actionable, redacted, and occurs after process-group cleanup");

  auto const normal_group_file = workspace / "normal-background-pgid";
  ava::tools::ToolContext const group_context{
      .workspace_dir = workspace,
      .mode = ava::agent::Mode::Build,
      .permission_resolver = [](ava::permissions::PermissionPrompt const&) -> ava::core::Result<ava::permissions::PermissionResolutionDecision> {
        return ava::permissions::PermissionResolution::Allow;
      }};
  auto normal_group = run_bash_for_test(group_context, "sleep 30 & printf $$ > " + normal_group_file.string(),
                                           ava::tools::BashOptions{.timeout = std::chrono::milliseconds(1000)});
  auto const normal_pgid = read_pid_file_for_test(normal_group_file);
  expect(normal_group && normal_group->exit_code == 0 && normal_pgid && wait_for_process_group_exit(*normal_pgid),
         "normal leader completion cleans verified background children before returning");
}

void test_sealed_process_group_sentinel_and_grace()
{
  auto const root = temp_root() / "sealed-process-group-sentinel-grace";
  std::error_code cleanup;
  std::filesystem::remove_all(root, cleanup);
  auto const workspace = root / "workspace";
  std::filesystem::create_directories(workspace);
  expect(::chmod(temp_root().c_str(), S_IRWXU) == 0 && ::chmod(root.c_str(), S_IRWXU) == 0 && ::chmod(workspace.c_str(), S_IRWXU) == 0,
         "sentinel/grace fixture owns every root");
  auto const first_bin = root / "bin";
  std::filesystem::create_directories(first_bin);
  expect(::chmod(first_bin.c_str(), S_IRWXU) == 0, "sentinel/grace fixture bin is owner-only");
  auto write_executable = [](std::filesystem::path const& path, std::string_view body) {
    std::ofstream executable(path, std::ios::binary | std::ios::trunc);
    executable << body;
    executable.close();
    return ::chmod(path.c_str(), S_IRUSR | S_IWUSR | S_IXUSR) == 0;
  };

  // A fixture that verifies the leader has no unexpected child processes.
  // If the sentinel were forked by the leader (old design), it would appear
  // here as an unexpected child. With the AVA-owned sibling design, the
  // leader's /proc/self/task/$$/children is empty.
  expect(write_executable(first_bin / "child-check",
                          "#!/bin/sh\n"
                          "children=''\n"
                          "while IFS= read -r line; do\n"
                          "  children=\"${children}${line} \"\n"
                          "done < /proc/self/task/$$/children 2>/dev/null\n"
                          "if [ -n \"$children\" ]; then\n"
                          "  printf 'unexpected-children: %s\\n' \"$children\"\n"
                          "else\n"
                          "  printf 'no-unexpected-children\\n'\n"
                          "fi\n"),
         "sentinel/grace fixture creates the child-check executable");

  // A fixture that starts a background subshell with a SIGTERM handler that
  // writes a marker after a short delay, then the leader exits normally.
  auto const grace_marker = workspace / "grace-marker";
  std::filesystem::remove(grace_marker, cleanup);
  expect(write_executable(first_bin / "grace-leader",
                          "#!/bin/sh\n"
                          "(\n"
                          "  trap 'sleep 0.1; touch " +
                              grace_marker.string() +
                              "; exit 0' TERM\n"
                              "  while true; do sleep 0.05; done\n"
                              ") &\n"
                              "exit 0\n"),
         "sentinel/grace fixture creates the grace-leader executable");

  ScopedEnvVar const path_guard("PATH", first_bin.string() + ":/usr/bin:/bin");
  ava::tools::ToolContext const context{
      .workspace_dir = workspace,
      .mode = ava::agent::Mode::Build,
      .permission_resolver = [](ava::permissions::PermissionPrompt const&) -> ava::core::Result<ava::permissions::PermissionResolutionDecision> {
        return ava::permissions::PermissionResolution::Allow;
      }};

  struct sigaction previous_term_action{};
  struct sigaction ignored_term_action{};
  ignored_term_action.sa_handler = SIG_IGN;
  static_cast<void>(::sigemptyset(&ignored_term_action.sa_mask));
  bool const term_action_installed = ::sigaction(SIGTERM, &ignored_term_action, &previous_term_action) == 0;
  sigset_t blocked_term{};
  sigset_t previous_mask{};
  static_cast<void>(::sigemptyset(&blocked_term));
  static_cast<void>(::sigaddset(&blocked_term, SIGTERM));
  bool const term_mask_installed = term_action_installed && ::sigprocmask(SIG_BLOCK, &blocked_term, &previous_mask) == 0;
  expect(term_mask_installed, "sentinel/grace fixture installs inherited ignored and blocked SIGTERM state");

  // Fix 1: the sentinel is an AVA-owned sibling, so the leader (after exec)
  // has no unexpected child processes.
  auto child_check = run_bash_for_test(context, "child-check", ava::tools::BashOptions{.timeout = std::chrono::milliseconds(2000)});
  expect(child_check && child_check->exit_code == 0 && child_check->output.find("no-unexpected-children") != std::string::npos,
         "the sealed command sentinel is an AVA-owned sibling: a direct command sees no sentinel child");

  // Fix 2: the verified process group receives a finite SIGTERM grace period
  // even when the leader has already exited normally. The background child's
  // TERM handler completes during grace, and no SIGKILL is needed.
  auto grace = run_bash_for_test(context, "grace-leader", ava::tools::BashOptions{.timeout = std::chrono::milliseconds(2000)});
  expect(grace && grace->exit_code == 0 && std::filesystem::exists(grace_marker),
         "background child TERM handler completes during the finite grace period on normal leader exit");

  // Timeout coverage: a long-running command with a background child is
  // terminated via the timeout path, which also applies the grace period.
  auto const timeout_marker = workspace / "timeout-marker";
  std::filesystem::remove(timeout_marker, cleanup);
  auto const timeout_group_file = workspace / "timeout-child-pgid";
  std::filesystem::remove(timeout_group_file, cleanup);
  expect(write_executable(first_bin / "timeout-leader",
                          "#!/bin/sh\n"
                          "(\n"
                          "  trap 'sleep 0.1; touch " +
                              timeout_marker.string() +
                              "; exit 0' TERM\n"
                              "  printf $$ > " +
                              timeout_group_file.string() +
                              "\n"
                              "  while true; do sleep 0.05; done\n"
                              ") &\n"
                              "while true; do sleep 1; done\n"),
         "sentinel/grace fixture creates the timeout-leader executable");
  auto timeout_result = run_bash_for_test(context, "timeout-leader", ava::tools::BashOptions{.timeout = std::chrono::milliseconds(500), .max_bytes = 1024});
  auto const timeout_pgid = read_pid_file_for_test(timeout_group_file);
  expect(timeout_result && timeout_result->timed_out && timeout_pgid && wait_for_process_group_exit(*timeout_pgid) && std::filesystem::exists(timeout_marker),
         "timeout termination applies the grace period to the verified process group");

  // Cancel coverage: cancellation also applies the grace period.
  auto const cancel_marker = workspace / "cancel-marker";
  std::filesystem::remove(cancel_marker, cleanup);
  auto const cancel_group_file = workspace / "cancel-child-pgid";
  std::filesystem::remove(cancel_group_file, cleanup);
  expect(write_executable(first_bin / "cancel-leader",
                          "#!/bin/sh\n"
                          "(\n"
                          "  trap 'sleep 0.1; touch " +
                              cancel_marker.string() +
                              "; exit 0' TERM\n"
                              "  printf $$ > " +
                              cancel_group_file.string() +
                              "\n"
                              "  while true; do sleep 0.05; done\n"
                              ") &\n"
                              "while true; do sleep 1; done\n"),
         "sentinel/grace fixture creates the cancel-leader executable");
  ava::tools::ToolContext const cancel_context{
      .workspace_dir = workspace,
      .mode = ava::agent::Mode::Build,
      .permission_resolver = [](ava::permissions::PermissionPrompt const&) -> ava::core::Result<ava::permissions::PermissionResolutionDecision> {
        return ava::permissions::PermissionResolution::Allow;
      },
      .cancel_requested = [&cancel_group_file] { return std::filesystem::exists(cancel_group_file); }};
  auto cancel_result =
      run_bash_for_test(cancel_context, "cancel-leader", ava::tools::BashOptions{.timeout = std::chrono::milliseconds(5000), .max_bytes = 1024});
  auto const cancel_pgid = read_pid_file_for_test(cancel_group_file);
  expect(cancel_result && cancel_result->canceled && !cancel_result->timed_out && cancel_pgid && wait_for_process_group_exit(*cancel_pgid) &&
             std::filesystem::exists(cancel_marker),
         "cancellation applies the grace period to the verified process group");

  if (term_mask_installed)
    static_cast<void>(::sigprocmask(SIG_SETMASK, &previous_mask, nullptr));
  if (term_action_installed)
    static_cast<void>(::sigaction(SIGTERM, &previous_term_action, nullptr));
}

void test_webfetch_tool()
{
  auto const root = create_empty_root("test_webfetch_tool");

  auto const workspace = root / "workspace";
  std::filesystem::create_directories(workspace);

  StaticTransport transport(ava::http::HttpResponse{.status_code = 200, .headers = {{"content-type", "text/plain; charset=utf-8"}}, .body = "abcdef"});
  int prompts = 0;
  ava::tools::ToolContext const context{
      .workspace_dir = workspace,
      .mode = ava::agent::Mode::Build,
      .permission_resolver = [&prompts](ava::permissions::PermissionPrompt const& prompt) -> ava::core::Result<ava::permissions::PermissionResolutionDecision> {
        ++prompts;
        expect(prompt.operation == ava::permissions::Operation::NetworkFetch, "webfetch resolver receives network operation");
        expect(prompt.command == "https://example.com/page", "webfetch resolver receives URL as command target");
        return ava::permissions::PermissionResolution::Allow;
      }};
  auto fetched =
      ava::tools::webfetch(context, "https://example.com/page", ava::tools::WebFetchOptions{.max_bytes = 3, .timeout_ms = 5000, .transport = &transport});
  expect(fetched && fetched->content == "abc" && fetched->truncated && fetched->byte_limited && fetched->total_bytes == 6 && fetched->output_bytes == 3 &&
             fetched->output_lines == 1 && fetched->content_type == "text/plain; charset=utf-8" && prompts == 1 && transport.requests.size() == 1 &&
             transport.requests[0].method == "GET" && transport.requests[0].timeout_ms == 5000 && !transport.requests[0].follow_redirects &&
             transport.requests[0].include_response_headers,
         "webfetch requires permission and bounds fetched text content");

  StaticTransport multiline_transport(
      ava::http::HttpResponse{.status_code = 200, .headers = {{"content-type", "text/plain; charset=utf-8"}}, .body = "one\ntwo\nthree\nfour\n"});
  auto fetched_lines = ava::tools::webfetch(
      context, "https://example.com/page",
      ava::tools::WebFetchOptions{.max_bytes = 1024, .offset_line = 2, .max_lines = 2, .timeout_ms = 5000, .transport = &multiline_transport});
  expect(fetched_lines && fetched_lines->content == "two\nthree\n" && fetched_lines->line_limited && !fetched_lines->byte_limited &&
             fetched_lines->output_lines == 2 && fetched_lines->start_line == 2 && fetched_lines->end_line == 3 && fetched_lines->total_lines == 4 &&
             fetched_lines->next_offset_line == 4,
         "webfetch supports line offset and limit continuation metadata");

  StaticTransport html_transport(ava::http::HttpResponse{.status_code = 200,
                                                         .headers = {{"content-type", "text/html; charset=utf-8"}},
                                                         .body = "<html><body><h1>Title</h1><script>hidden()</script><p>A&amp;B</p></body></html>"});
  auto html_text = ava::tools::webfetch(
      context, "https://example.com/page",
      ava::tools::WebFetchOptions{.max_bytes = 1024, .timeout_ms = 5000, .format = ava::tools::WebFetchFormat::Text, .transport = &html_transport});
  expect(html_text && html_text->content.find("Title") != std::string::npos && html_text->content.find("A&B") != std::string::npos &&
             html_text->content.find("hidden") == std::string::npos && html_transport.requests[0].headers.at("Accept").find("text/plain") != std::string::npos,
         "webfetch supports text output for HTML responses with basic tag stripping");

  StaticTransport unused_transport(ava::http::HttpResponse{.status_code = 200, .headers = {}, .body = "unused"});
  auto invalid_scheme = ava::tools::webfetch(context, "file:///etc/passwd", ava::tools::WebFetchOptions{.transport = &unused_transport});
  expect(!invalid_scheme && unused_transport.requests.empty(), "webfetch rejects non-http URLs before transport use");

  auto private_ip = ava::tools::webfetch(context, "http://127.0.0.1:8080", ava::tools::WebFetchOptions{.transport = &unused_transport});
  expect(!private_ip && unused_transport.requests.empty(), "webfetch rejects local IP hosts before transport use");

  auto short_ipv4 = ava::tools::webfetch(context, "http://127.1:8080", ava::tools::WebFetchOptions{.transport = &unused_transport});
  expect(!short_ipv4 && unused_transport.requests.empty(), "webfetch rejects shortened IPv4 literal hosts");

  auto decimal_ipv4 = ava::tools::webfetch(context, "http://2130706433/", ava::tools::WebFetchOptions{.transport = &unused_transport});
  expect(!decimal_ipv4 && unused_transport.requests.empty(), "webfetch rejects decimal IPv4 literal hosts");

  auto hex_ipv4 = ava::tools::webfetch(context, "http://0x7f000001/", ava::tools::WebFetchOptions{.transport = &unused_transport});
  expect(!hex_ipv4 && unused_transport.requests.empty(), "webfetch rejects hexadecimal IPv4 literal hosts");

  StaticTransport digit_domain_transport(ava::http::HttpResponse{.status_code = 200, .headers = {}, .body = "domain ok"});
  ava::tools::ToolContext const permissive_network_context{
      .workspace_dir = workspace,
      .mode = ava::agent::Mode::Build,
      .permission_resolver = [](ava::permissions::PermissionPrompt const& prompt) -> ava::core::Result<ava::permissions::PermissionResolutionDecision> {
        expect(prompt.operation == ava::permissions::Operation::NetworkFetch, "digit-leading domain still requests network permission");
        return ava::permissions::PermissionResolution::Allow;
      }};
  auto digit_domain = ava::tools::webfetch(permissive_network_context, "https://1.be/", ava::tools::WebFetchOptions{.transport = &digit_domain_transport});
  expect(digit_domain && digit_domain->content == "domain ok", "webfetch allows digit-leading DNS names that are not IP aliases");

  ava::tools::ToolContext const no_resolver_context{.workspace_dir = workspace, .mode = ava::agent::Mode::Build};
  auto no_resolver = ava::tools::webfetch(no_resolver_context, "https://example.com/page", ava::tools::WebFetchOptions{.transport = &unused_transport});
  expect(!no_resolver && no_resolver.error().format().find("no_resolver") != std::string::npos && unused_transport.requests.empty(),
         "webfetch fails closed without network permission resolver");

  ava::tools::ToolContext const deny_context{
      .workspace_dir = workspace,
      .mode = ava::agent::Mode::Build,
      .permission_resolver = [](ava::permissions::PermissionPrompt const& prompt) -> ava::core::Result<ava::permissions::PermissionResolutionDecision> {
        expect(prompt.operation == ava::permissions::Operation::NetworkFetch, "webfetch deny resolver receives network fetch operation");
        return ava::permissions::PermissionResolution::Deny;
      }};
  auto denied_fetch = ava::tools::webfetch(deny_context, "https://example.com/page", ava::tools::WebFetchOptions{.transport = &unused_transport});
  expect(!denied_fetch && denied_fetch.error().format().find("resolution: deny") != std::string::npos && unused_transport.requests.empty(),
         "webfetch fails closed when network permission resolver denies before transport use");

  StaticTransport binary_transport(ava::http::HttpResponse{.status_code = 200, .headers = {{"content-type", "application/octet-stream"}}, .body = "abc"});
  auto binary = ava::tools::webfetch(context, "https://example.com/page", ava::tools::WebFetchOptions{.transport = &binary_transport});
  expect(!binary && binary.error().message().find("binary") != std::string::npos, "webfetch rejects binary response content types");

  CancelAwareTransport cancel_aware_transport;
  int cancel_checks = 0;
  ava::tools::ToolContext const cancel_during_transport_context{
      .workspace_dir = workspace,
      .mode = ava::agent::Mode::Build,
      .permission_resolver = [](ava::permissions::PermissionPrompt const&) -> ava::core::Result<ava::permissions::PermissionResolutionDecision> {
        return ava::permissions::PermissionResolution::Allow;
      },
      .cancel_requested =
          [&cancel_checks] {
            ++cancel_checks;
            return cancel_checks >= 3;
          },
  };
  auto canceled_fetch =
      ava::tools::webfetch(cancel_during_transport_context, "https://example.com/page", ava::tools::WebFetchOptions{.transport = &cancel_aware_transport});
  expect(!canceled_fetch && canceled_fetch.error().message().find("canceled") != std::string::npos && cancel_aware_transport.saw_cancel_callback &&
             cancel_aware_transport.requests.size() == 1,
         "webfetch passes cancellation into the transport request boundary");
}

void test_websearch_tool()
{
  auto const root = create_empty_root("test_websearch_tool");

  auto const workspace = root / "workspace";
  std::filesystem::create_directories(workspace);

  StaticTransport transport(ava::http::HttpResponse{.status_code = 200,
                                                    .headers = {{"content-type", "application/json"}},
                                                    .body =
                                                        "{\"Heading\":\"AVA\",\"AbstractText\":\"Native C++ agent\",\"AbstractURL\":\"https://ava.example/\","
                                                        "\"RelatedTopics\":[{\"Text\":\"AVA docs\",\"FirstURL\":\"https://ava.example/docs\"},"
                                                        "{\"Topics\":[{\"Text\":\"AVA releases\",\"FirstURL\":\"https://ava.example/releases\"}]}]}"});
  int prompts = 0;
  ava::tools::ToolContext const context{
      .workspace_dir = workspace,
      .mode = ava::agent::Mode::Build,
      .permission_resolver = [&prompts](ava::permissions::PermissionPrompt const& prompt) -> ava::core::Result<ava::permissions::PermissionResolutionDecision> {
        ++prompts;
        expect(prompt.operation == ava::permissions::Operation::NetworkSearch, "websearch resolver receives network search operation");
        expect(prompt.command == "ava agent", "websearch resolver receives query as command target");
        return ava::permissions::PermissionResolution::Allow;
      }};

  auto searched = ava::tools::websearch(
      context, " ava agent ", ava::tools::WebSearchOptions{.max_results = 2, .context_max_chars = 10000, .timeout_ms = 7000, .transport = &transport});
  expect(searched && searched->query == "ava agent" && searched->results.size() == 2 && searched->results[0].title == "AVA" &&
             searched->results[1].url == "https://ava.example/docs" && searched->total_results == 3 && searched->truncated && prompts == 1 &&
             transport.requests.size() == 1 && transport.requests[0].method == "GET" && transport.requests[0].url.find("q=ava+agent") != std::string::npos &&
             transport.requests[0].timeout_ms == 7000 && transport.requests[0].follow_redirects,
         "websearch requires permission, queries a bounded search endpoint, and parses structured results");

  auto missing_factory_context = context;
  missing_factory_context.permission_resolver =
      [](ava::permissions::PermissionPrompt const&) -> ava::core::Result<ava::permissions::PermissionResolutionDecision> {
    return ava::permissions::PermissionResolution::Allow;
  };
  auto missing_factory = ava::tools::websearch(missing_factory_context, "factory authority");
  expect(!missing_factory && missing_factory.error().message() == "websearch transport process authority is unavailable",
         "websearch without an explicit fake or session transport factory fails before process reservation");

  StaticTransport unused_transport(ava::http::HttpResponse{.status_code = 200, .headers = {}, .body = "{}"});
  auto invalid_query = ava::tools::websearch(context, "\n", ava::tools::WebSearchOptions{.transport = &unused_transport});
  expect(!invalid_query && unused_transport.requests.empty(), "websearch rejects empty/control queries before transport use");

  ava::tools::ToolContext const no_resolver_context{.workspace_dir = workspace, .mode = ava::agent::Mode::Build};
  auto no_resolver = ava::tools::websearch(no_resolver_context, "ava agent", ava::tools::WebSearchOptions{.transport = &unused_transport});
  expect(!no_resolver && no_resolver.error().format().find("no_resolver") != std::string::npos && unused_transport.requests.empty(),
         "websearch fails closed without network search permission resolver");

  ava::tools::ToolContext const deny_context{
      .workspace_dir = workspace,
      .mode = ava::agent::Mode::Build,
      .permission_resolver = [](ava::permissions::PermissionPrompt const& prompt) -> ava::core::Result<ava::permissions::PermissionResolutionDecision> {
        expect(prompt.operation == ava::permissions::Operation::NetworkSearch, "websearch deny resolver receives network search operation");
        return ava::permissions::PermissionResolution::Deny;
      }};
  auto denied_search = ava::tools::websearch(deny_context, "ava agent", ava::tools::WebSearchOptions{.transport = &unused_transport});
  expect(!denied_search && denied_search.error().format().find("resolution: deny") != std::string::npos && unused_transport.requests.empty(),
         "websearch fails closed when network search permission resolver denies before transport use");
}
}  // namespace

void test_credential_command_audit_omits_secrets()
{
  // Local (non-ACP) audit surfaces must never persist unique secret values
  // from credential-bearing commands. When secret detection refuses recipe
  // minting, recipe_display is empty and the audit command field is replaced
  // with a redacted marker.
  struct SecretCase
  {
    std::string_view label;
    std::string command;
    std::string secret;
  };
  std::vector<SecretCase> const cases{
      {"curl -u separate", "curl -u alice:s3cr3t https://example.test/releases", "s3cr3t"},
      {"curl --user= concat", "curl --user=alice:s3cr3t https://example.test/releases", "s3cr3t"},
      {"curl -H Authorization", "curl -H 'Authorization: Bearer tok_audit_1' https://example.test/releases", "tok_audit_1"},
      {"curl --header= concat", "curl --header='Authorization: Bearer tok_audit_2' https://example.test/releases", "tok_audit_2"},
      {"wget --http-password", "wget --http-user=alice --http-password=wgetpass https://example.test/releases", "wgetpass"},
      {"url query token", "curl 'https://example.test/api?token=query_audit_secret'", "query_audit_secret"},
  };

  for (auto const& test_case : cases)
  {
    ava::tools::PermissionAuditEvent event;
    event.permission_request_id = std::string("permreq_") + std::string(test_case.label);
    event.operation = ava::permissions::Operation::RunCommand;
    event.mode = ava::agent::Mode::Build;
    event.tool_name = "bash";
    event.action = ava::permissions::PermissionAction::Ask;
    event.reason = "command requires approval";
    event.risk = ava::permissions::PermissionRisk::High;
    event.command = test_case.command;
    event.resolution_source = "policy";
    ava::permissions::CommandPermissionMetadata metadata;
    // Simulate a contained Sensitive command whose secret detection refused
    // recipe minting: recipe keys and display are empty.
    metadata.level = ava::command::CommandLevel::Sensitive;
    metadata.global_recipe_key = {};
    metadata.workspace_recipe_key = {};
    metadata.recipe_display = {};
    event.command_metadata = std::move(metadata);
    auto const json = ava::tools::permission_audit_data_json(event);
    expect(json.find(test_case.secret) == std::string::npos,
           std::string("local audit JSON must not contain credential secret for: ") + std::string(test_case.label));
    expect(json.find("<redacted one-shot command>") != std::string::npos || json.find("\"command\":\"\"") != std::string::npos,
           std::string("local audit JSON must redact credential-bearing one-shot command for: ") + std::string(test_case.label));
  }
}

void test_denied_command_diagnostics_redact_arguments()
{
  auto const root = temp_root() / "denied-command-diagnostic-redaction";
  std::error_code cleanup;
  std::filesystem::remove_all(root, cleanup);
  std::filesystem::create_directories(root);
  expect(::chmod(root.c_str(), S_IRWXU) == 0, "denied-command diagnostic fixture is owner-only");
  std::string const secret = "AVA_DENIED_COMMAND_SECRET_SENTINEL";
  std::vector<ava::tools::PermissionAuditEvent> audits;
  std::vector<std::string> prompts;
  auto audit_sink = [&audits](ava::tools::PermissionAuditEvent const& event) -> ava::core::VoidResult {
    audits.push_back(event);
    return {};
  };
  auto run = [&](ava::permissions::PermissionResolver resolver, bool redact_permission_audit_arguments) {
    ava::tools::ToolContext context{.workspace_dir = root,
                                    .mode = ava::agent::Mode::Build,
                                    .permission_resolver = std::move(resolver),
                                    .permission_audit_sink = audit_sink,
                                    .redact_permission_audit_arguments = redact_permission_audit_arguments};
    return ava::tools::ensure_permission(context, ava::permissions::Operation::RunCommand, root, "printf " + secret, "bash", "command requires permission");
  };

  auto no_resolver = run(nullptr, false);
  auto denied = run(
      [&prompts](ava::permissions::PermissionPrompt const& prompt) -> ava::core::Result<ava::permissions::PermissionResolutionDecision> {
        prompts.push_back(prompt.command);
        return ava::permissions::PermissionResolutionDecision{ava::permissions::PermissionResolution::Deny, "denied by local user"};
      },
      true);
  auto resolver_failed = run(
      [&prompts](ava::permissions::PermissionPrompt const& prompt) -> ava::core::Result<ava::permissions::PermissionResolutionDecision> {
        prompts.push_back(prompt.command);
        return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Io, "resolver failed"));
      },
      false);
  auto canceled = run(
      [&prompts](ava::permissions::PermissionPrompt const& prompt) -> ava::core::Result<ava::permissions::PermissionResolutionDecision> {
        prompts.push_back(prompt.command);
        return ava::permissions::PermissionResolutionDecision{ava::permissions::PermissionResolution::Cancel, "canceled by local user"};
      },
      true);

  auto contains_secret = [&](ava::core::VoidResult const& result) { return !result && result.error().format().find(secret) != std::string::npos; };
  bool const audits_redacted = std::ranges::all_of(audits, [&](ava::tools::PermissionAuditEvent const& event) {
    return event.command.find(secret) == std::string::npos && event.reason.find(secret) == std::string::npos &&
           event.resolution_reason.find(secret) == std::string::npos && ava::tools::permission_audit_data_json(event).find(secret) == std::string::npos;
  });
  expect(
      !no_resolver && !denied && !resolver_failed && !canceled && !contains_secret(no_resolver) && !contains_secret(denied) &&
          !contains_secret(resolver_failed) && !contains_secret(canceled) && prompts.size() == 3 &&
          std::ranges::all_of(prompts, [&](std::string const& prompt) { return prompt.find(secret) != std::string::npos; }) && audits_redacted,
      "RunCommand denial diagnostics and audits redact arguments on no-resolver, deny, resolver-failure, and cancel paths while the local prompt retains them");
}

void test_payload_command_audits_persist_no_markers_or_recipes()
{
  auto const test_root = temp_root();
  expect(::chmod(test_root.c_str(), S_IRWXU) == 0, "payload audit test secures its test-root ancestor");
  auto const root = test_root / "payload-command-audit";
  std::error_code cleanup;
  std::filesystem::remove_all(root, cleanup);
  auto const workspace = root / "workspace";
  std::filesystem::create_directories(workspace);
  expect(::chmod(root.c_str(), S_IRWXU) == 0 && ::chmod(workspace.c_str(), S_IRWXU) == 0, "payload audit fixture keeps sealed planning roots owner-only");

  ava::session::SessionStore store(ava::session::SessionStoreOptions{.root_dir = root / "sessions", .workspace_dir = workspace, .session_id = "payload-audit"});
  auto lease = ava::session::SessionLease::create_and_acquire(store.session_path());
  expect(lease.has_value(), "payload audit fixture acquires its persistent session lease");
  if (!lease)
    return;

  struct PayloadCase
  {
    std::string label;
    std::string command;
    std::string marker;
  };
  std::vector<PayloadCase> const cases{
      {.label = "json body",
       .command = "curl --json '{\"body\":\"payload-json-marker-492e\"}' https://example.test/upload",
       .marker = "payload-json-marker-492e"},
      {.label = "file payload", .command = "curl --data-binary @payload-file-marker-26ca https://example.test/upload", .marker = "payload-file-marker-26ca"},
      {.label = "config payload", .command = "curl --config payload-config-marker-8b17 https://example.test/upload", .marker = "payload-config-marker-8b17"},
      {.label = "opaque URL query", .command = "curl 'https://example.test/upload?key=payload-query-marker-c213'", .marker = "payload-query-marker-c213"},
  };

  bool all_denied_before_execution = true;
  for (auto const& test_case : cases)
  {
    ava::tools::ToolContext context{
        .workspace_dir = workspace,
        .mode = ava::agent::Mode::Build,
        .permission_resolver = [](ava::permissions::PermissionPrompt const&) -> ava::core::Result<ava::permissions::PermissionResolutionDecision> {
          return ava::permissions::PermissionResolution::Deny;
        },
        .permission_audit_sink = [&store, &lease](ava::tools::PermissionAuditEvent const& event) -> ava::core::VoidResult {
          return store.append(*lease, ava::session::SessionEntry{.id = ava::core::make_id("entry"),
                                                                 .parent_id = {},
                                                                 .type = ava::session::EntryType::PermissionDecision,
                                                                 .timestamp = ava::session::now_timestamp(),
                                                                 .data_json = ava::tools::permission_audit_data_json(event)});
        },
        .redact_permission_audit_arguments = false,
    };
    auto result = run_bash_for_test(context, test_case.command);
    all_denied_before_execution = all_denied_before_execution && !result && result.error().format().find("resolution: deny") != std::string::npos;
  }

  auto entries = store.load(*lease);
  bool markers_absent = entries.has_value();
  bool stable_recipe_keys_absent = entries.has_value();
  bool recipe_displays_empty = entries.has_value();
  if (entries)
  {
    for (auto const& entry : *entries)
    {
      if (entry.type != ava::session::EntryType::PermissionDecision)
        continue;
      for (auto const& test_case : cases) markers_absent = markers_absent && entry.data_json.find(test_case.marker) == std::string::npos;
      stable_recipe_keys_absent = stable_recipe_keys_absent && entry.data_json.find("\"global_recipe_key\":\"\"") != std::string::npos &&
                                  entry.data_json.find("\"workspace_recipe_key\":\"\"") != std::string::npos;
      recipe_displays_empty = recipe_displays_empty && entry.data_json.find("\"recipe_display\":\"\"") != std::string::npos;
    }
  }
  expect(all_denied_before_execution && entries && markers_absent && stable_recipe_keys_absent && recipe_displays_empty,
         "JSON body and file/config payload commands persist one-shot redacted audits with no payload markers or stable recipe keys");
}

void test_command_permission_user_guidance_propagation()
{
  auto const root = temp_root() / "command-permission-user-guidance";
  std::error_code cleanup;
  std::filesystem::remove_all(root, cleanup);
  std::filesystem::create_directories(root);
  expect(::chmod(root.c_str(), S_IRWXU) == 0, "command guidance fixture workspace is owner-only");

  std::vector<ava::tools::PermissionAuditEvent> audits;
  auto audit_sink = [&audits](ava::tools::PermissionAuditEvent const& event) -> ava::core::VoidResult {
    audits.push_back(event);
    return {};
  };

  auto guided_capture = std::make_shared<ava::tools::PermissionDenialGuidanceCapture>();
  ava::tools::ToolContext guided_context{
      .workspace_dir = root,
      .mode = ava::agent::Mode::Build,
      .permission_resolver =
          [](ava::permissions::PermissionPrompt const&) -> ava::core::Result<ava::permissions::PermissionResolutionDecision> {
            ava::permissions::PermissionResolutionDecision denied{ava::permissions::PermissionResolution::Deny, "not approved"};
            denied.user_guidance = "use the sealed workspace recipe instead";
            return denied;
          },
      .permission_audit_sink = audit_sink,
      .permission_denial_guidance_capture = guided_capture};
  auto guided = ava::tools::ensure_permission(guided_context, ava::permissions::Operation::RunCommand, root, "true", "bash", "command requires permission");
  expect(!guided && guided_capture->provider_user_guidance == "use the sealed workspace recipe instead" &&
             guided.error().format().find("user_guidance") == std::string::npos &&
             guided.error().format().find("use the sealed workspace recipe instead") == std::string::npos &&
             guided.error().format().find("resolution: deny") != std::string::npos,
         "RunCommand permission denial captures validated guidance without entering Error context/format");

  bool audits_free_of_guidance = !audits.empty();
  for (auto const& event : audits)
  {
    auto const json = ava::tools::permission_audit_data_json(event);
    audits_free_of_guidance = audits_free_of_guidance && json.find("user_guidance") == std::string::npos &&
                              json.find("provider_user_guidance") == std::string::npos &&
                              json.find("use the sealed workspace recipe instead") == std::string::npos &&
                              event.reason.find("sealed workspace recipe") == std::string::npos &&
                              event.resolution_reason.find("sealed workspace recipe") == std::string::npos;
  }
  expect(audits_free_of_guidance, "RunCommand permission audits never serialize one-shot user_guidance");

  audits.clear();
  auto forged_capture = std::make_shared<ava::tools::PermissionDenialGuidanceCapture>();
  ava::tools::ToolContext forged_context{
      .workspace_dir = root,
      .mode = ava::agent::Mode::Build,
      .permission_resolver =
          [](ava::permissions::PermissionPrompt const&) -> ava::core::Result<ava::permissions::PermissionResolutionDecision> {
            ava::permissions::PermissionResolutionDecision denied{ava::permissions::PermissionResolution::Deny};
            denied.user_guidance = "no\nwire\x7fleak";
            return denied;
          },
      .permission_audit_sink = audit_sink,
      .permission_denial_guidance_capture = forged_capture};
  auto forged = ava::tools::ensure_permission(forged_context, ava::permissions::Operation::RunCommand, root, "true", "bash", "command requires permission");
  expect(!forged && forged_capture->provider_user_guidance.empty() && forged.error().format().find("user_guidance") == std::string::npos &&
             forged.error().format().find("wire") == std::string::npos,
         "malformed forged RunCommand user_guidance is dropped and never leaks");
}

void run_tools_process_network_tests()
{
  test_bash_tool();
  test_bash_runtime_and_invocation_contract();
  test_injected_command_executor();
  test_sealed_local_bash_contract();
  test_sealed_process_group_sentinel_and_grace();
  test_webfetch_tool();
  test_websearch_tool();
  test_credential_command_audit_omits_secrets();
  test_denied_command_diagnostics_redact_arguments();
  test_payload_command_audits_persist_no_markers_or_recipes();
  test_command_permission_user_guidance_propagation();
}

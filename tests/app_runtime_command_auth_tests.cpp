#include "sys.h"
#include "tests/app_runtime_test_declarations.h"
#include "tests/support/fake_transport.h"
#include "tests/support/test_harness.h"
#include "ava/app/commands.h"
#include "ava/app/runtime/Session.h"
#include "ava/agent/mode.h"
#include "ava/agent/question.h"
#include "ava/config/auth.h"
#include "ava/config/openai_oauth.h"
#include "ava/core/error.h"
#include "ava/core/result.h"

#include <expected>
#include <string>
#ifndef AVA_FAKE_MCP_SERVER_PATH
#define AVA_FAKE_MCP_SERVER_PATH ""
#endif

namespace ava::tests::app_runtime_tests {

using namespace ava::tests;

// Exercise authentication commands using unlocked_session and compare the rebuilt prompt with plan_system_prompt.
//
// The commands mutate session mode, prompt, and stored credentials. Each command acquires its own session access.
void app_command_dispatcher_auth_part(ava::app::runtime::session_ts& unlocked_session, std::string const& plan_system_prompt)
{
  auto mode = ava::app::run_command(unlocked_session, ava::app::CommandRequest{.command = "/mode"});
  auto mode_snapshot = [&] {
    SCOPED_CRITICAL_AREA_R(session_r, unlocked_session);
    return std::pair{session_r->mode(), session_r->system_prompt()};
  }();
  auto const& [current_mode, system_prompt] = mode_snapshot;
  expect(mode && mode->handled && current_mode == ava::agent::Mode::Build && !mode->output.empty() && mode->output[0].find("build") != std::string::npos,
         "command dispatcher /mode toggles runtime mode");
  expect(system_prompt != plan_system_prompt && system_prompt.find("Implement changes directly") != std::string::npos &&
             system_prompt.find("dispatcher context changed after session open") != std::string::npos,
         "command dispatcher /mode rebuilds the mode-specific system prompt with context");
  auto const paths = ava::app::runtime::session_ts::rat(unlocked_session)->paths();

  bool saw_secret_prompt = false;
  auto connect = ava::app::run_command(
      unlocked_session, ava::app::CommandRequest{.command = "/login moonshot api-key", .question_resolver = [&](ava::agent::QuestionPrompt const& prompt) {
                                             saw_secret_prompt =
                                                 prompt.modal && prompt.secret && prompt.allow_custom && prompt.question.find("moonshot") != std::string::npos;
                                             return ava::agent::QuestionAnswer{.selected_options = {}, .custom_text = "slash-moonshot-api-key"};
                                           }});
  expect(connect && connect->handled && saw_secret_prompt && !connect->output.empty() &&
             connect->output[0].find("Stored moonshot API key credential") != std::string::npos,
         "command dispatcher /login alias stores provider API key credentials via masked prompt");
  ava::tests::FakeTransport credential_transport({});
  auto slash_moonshot = ava::config::provider_credential_for_request(paths, "moonshot", credential_transport);
  expect(slash_moonshot && slash_moonshot->has_value() && (*slash_moonshot)->access_token == "slash-moonshot-api-key" &&
             (*slash_moonshot)->credential_type == "api_key",
         "slash provider connect writes loadable provider credential");

  std::size_t connect_prompt_count = 0;
  auto connect_modal = ava::app::run_command(
      unlocked_session, ava::app::CommandRequest{.command = "/connect", .question_resolver = [&](ava::agent::QuestionPrompt const& prompt) {
                                             if (connect_prompt_count == 0)
                                             {
                                               expect(prompt.modal && prompt.searchable && prompt.allow_custom && prompt.question == "Select provider",
                                                      "slash /connect opens provider selection as searchable modal");
                                               ++connect_prompt_count;
                                               return ava::agent::QuestionAnswer{.selected_options = {"anthropic"}, .custom_text = ""};
                                             }
                                             if (connect_prompt_count == 1)
                                             {
                                               expect(prompt.modal && prompt.secret && prompt.question.find("anthropic") != std::string::npos,
                                                      "slash /connect opens secret prompt as masked modal");
                                               ++connect_prompt_count;
                                               return ava::agent::QuestionAnswer{.selected_options = {}, .custom_text = "slash-api-key"};
                                             }
                                             expect(false, "slash /connect should not prompt for a non-OpenAI credential type");
                                             ++connect_prompt_count;
                                             return ava::agent::QuestionAnswer{};
                                           }});
  expect(connect_modal && connect_modal->handled && connect_prompt_count == 2 && !connect_modal->output.empty() &&
             connect_modal->output[0].find("Stored anthropic API key credential") != std::string::npos,
         "command dispatcher /connect walks provider and secret modals for API-key-only providers");
  auto slash_anthropic = ava::config::provider_credential_for_request(paths, "anthropic", credential_transport);
  expect(slash_anthropic && slash_anthropic->has_value() && (*slash_anthropic)->access_token == "slash-api-key" &&
             (*slash_anthropic)->credential_type == "api_key",
         "slash provider connect modal writes loadable API key credential");

  std::size_t openai_connect_prompt_count = 0;
  auto connect_openai_modal = ava::app::run_command(
      unlocked_session, ava::app::CommandRequest{
                      .command = "/connect", .question_resolver = [&](ava::agent::QuestionPrompt const& prompt) {
                        if (openai_connect_prompt_count == 0)
                        {
                          bool saw_active_openai = false;
                          std::size_t kimi_moonshot_count = 0;
                          bool saw_split_kimi = false;
                          bool saw_manual_token_text = false;
                          for (auto const& option : prompt.options)
                          {
                            saw_active_openai = saw_active_openai || option.label == "OpenAI ✓";
                            if (option.label.find("Kimi / Moonshot") != std::string::npos)
                              ++kimi_moonshot_count;
                            saw_split_kimi = saw_split_kimi || option.label.starts_with("Kimi -") || option.label.starts_with("Moonshot -");
                            saw_manual_token_text = saw_manual_token_text || option.label.find("token") != std::string::npos;
                          }
                          expect(prompt.modal && prompt.searchable && prompt.allow_custom && prompt.question == "Select provider" && saw_active_openai &&
                                     kimi_moonshot_count == 1 && !saw_split_kimi && !saw_manual_token_text,
                                 "slash /connect opens provider modal with active OpenAI and merged API-key-only providers");
                          ++openai_connect_prompt_count;
                          return ava::agent::QuestionAnswer{.selected_options = {"openai"}, .custom_text = ""};
                        }
                        if (openai_connect_prompt_count == 1)
                        {
                          bool saw_browser = false;
                          bool saw_headless = false;
                          bool saw_api_key = false;
                          bool saw_previous = false;
                          for (auto const& option : prompt.options)
                          {
                            saw_browser = saw_browser || option.value == "openai_browser_oauth";
                            saw_headless = saw_headless || option.value == "openai_headless_oauth";
                            saw_api_key = saw_api_key || option.value == "api_key";
                            saw_previous = saw_previous || option.value == "back";
                          }
                          expect(prompt.modal && !prompt.searchable && !prompt.secret && prompt.question == "Choose login method" && saw_browser &&
                                     saw_headless && saw_api_key && saw_previous,
                                 "slash /connect OpenAI method modal lists browser, headless, API key, and previous options");
                          ++openai_connect_prompt_count;
                          return ava::agent::QuestionAnswer{.selected_options = {"api_key"}, .custom_text = ""};
                        }
                        expect(openai_connect_prompt_count == 2 && prompt.modal && prompt.secret && prompt.question.find("openai") != std::string::npos,
                               "slash /connect OpenAI API key choice opens masked secret modal");
                        ++openai_connect_prompt_count;
                        return ava::agent::QuestionAnswer{.selected_options = {}, .custom_text = "slash-openai-modal-api-key"};
                      }});
  expect(connect_openai_modal && connect_openai_modal->handled && openai_connect_prompt_count == 3 && !connect_openai_modal->output.empty() &&
             connect_openai_modal->output[0].find("Stored openai API key credential") != std::string::npos,
         "command dispatcher /connect OpenAI walks provider, method, and secret modals");
  auto slash_openai_from_modal = ava::config::load_openai_credential(paths);
  expect(slash_openai_from_modal && slash_openai_from_modal->has_value() && (*slash_openai_from_modal)->type == ava::config::OpenAICredentialType::ApiKey &&
             (*slash_openai_from_modal)->access_token == "slash-openai-modal-api-key",
         "slash OpenAI connect modal writes loadable OpenAI credential");

  std::size_t back_connect_prompt_count = 0;
  auto connect_back_modal = ava::app::run_command(
      unlocked_session, ava::app::CommandRequest{.command = "/connect", .question_resolver = [&](ava::agent::QuestionPrompt const& prompt) {
                                             if (back_connect_prompt_count == 0)
                                             {
                                               ++back_connect_prompt_count;
                                               return ava::agent::QuestionAnswer{.selected_options = {"openai"}, .custom_text = ""};
                                             }
                                             if (back_connect_prompt_count == 1)
                                             {
                                               expect(prompt.question == "Choose login method", "slash /connect can navigate back from method modal");
                                               ++back_connect_prompt_count;
                                               return ava::agent::QuestionAnswer{.selected_options = {"back"}, .custom_text = ""};
                                             }
                                             if (back_connect_prompt_count == 2)
                                             {
                                               expect(prompt.question == "Select provider", "slash /connect back returns to provider modal");
                                               ++back_connect_prompt_count;
                                               return ava::agent::QuestionAnswer{.selected_options = {"anthropic"}, .custom_text = ""};
                                             }
                                             if (back_connect_prompt_count == 3)
                                             {
                                               expect(prompt.modal && prompt.secret && prompt.question.find("anthropic") != std::string::npos,
                                                      "slash /connect back skips method modal for API-key-only providers");
                                               ++back_connect_prompt_count;
                                               return ava::agent::QuestionAnswer{.selected_options = {}, .custom_text = "slash-back-api-key"};
                                             }
                                             expect(false, "slash /connect back should not show an extra non-OpenAI method modal");
                                             ++back_connect_prompt_count;
                                             return ava::agent::QuestionAnswer{};
                                           }});
  expect(connect_back_modal && connect_back_modal->handled && back_connect_prompt_count == 4 && !connect_back_modal->output.empty() &&
             connect_back_modal->output[0].find("Stored anthropic API key credential") != std::string::npos,
         "command dispatcher /connect previous option returns to provider modal");

  auto connect_cancel = ava::app::run_command(
      unlocked_session, ava::app::CommandRequest{.command = "/connect", .question_resolver = [](ava::agent::QuestionPrompt const&) {
                                             return ava::core::Result<ava::agent::QuestionAnswer>{
                                                 std::unexpected(ava::core::Error(ava::core::ErrorCategory::Tool, "question prompt canceled"))};
                                           }});
  expect(connect_cancel && connect_cancel->handled && connect_cancel->output.empty(),
         "command dispatcher /connect treats modal cancellation as a silent close");
  auto const anthropic_after_cancel = ava::config::provider_credential_for_request(paths, "anthropic", credential_transport);
  auto const openai_after_cancel = ava::config::load_openai_credential(paths);
  expect_all("canceling /connect leaves existing credentials unchanged",
             {{"anthropic", anthropic_after_cancel && *anthropic_after_cancel && (**anthropic_after_cancel).access_token == "slash-back-api-key"},
              {"openai", openai_after_cancel && *openai_after_cancel && (**openai_after_cancel).access_token == "slash-openai-modal-api-key"}});
  for (std::string const& secret : {std::string{}, std::string(" \t\n")})
  {
    auto rejected_connect =
        ava::app::run_command(unlocked_session, ava::app::CommandRequest{.command = "/connect anthropic",
                                                                         .event_sink = nullptr,
                                                                         .permission_resolver = nullptr,
                                                                         .question_resolver =
                                                                             [secret](ava::agent::QuestionPrompt const&) {
                                                                               return ava::agent::QuestionAnswer{.selected_options = {}, .custom_text = secret};
                                                                             },
                                                                         .compaction_summary_generator = nullptr,
                                                                         .cancel_requested = nullptr,
                                                                         .plugin_ui_capability = nullptr,
                                                                         .propagate_compaction_errors = false,
                                                                         .hotkeys = {}});
    auto const credential = ava::config::provider_credential_for_request(paths, "anthropic", credential_transport);
    expect_all("empty or whitespace /connect secret is rejected without replacing the stored credential",
               {{"handled rejection", rejected_connect && rejected_connect->handled && !rejected_connect->output.empty()},
                {"unchanged credential", credential && *credential && (**credential).access_token == "slash-back-api-key"}});
  }

  bool saw_openai_secret_prompt = false;
  auto connect_openai_api = ava::app::run_command(
      unlocked_session, ava::app::CommandRequest{.command = "/connect openai api-key", .question_resolver = [&](ava::agent::QuestionPrompt const& prompt) {
                                             saw_openai_secret_prompt =
                                                 prompt.modal && prompt.secret && prompt.allow_custom && prompt.question.find("openai") != std::string::npos;
                                             return ava::agent::QuestionAnswer{.selected_options = {}, .custom_text = "slash-openai-api-key"};
                                           }});
  expect(connect_openai_api && connect_openai_api->handled && saw_openai_secret_prompt && !connect_openai_api->output.empty() &&
             connect_openai_api->output[0].find("Stored openai API key credential") != std::string::npos,
         "command dispatcher /connect openai api-key prompts once and stores OpenAI API key credential");
  auto slash_openai = ava::config::load_openai_credential(paths);
  expect(slash_openai && slash_openai->has_value() && (*slash_openai)->type == ava::config::OpenAICredentialType::ApiKey &&
             (*slash_openai)->access_token == "slash-openai-api-key",
         "slash OpenAI API key connect writes loadable OpenAI credential");

  auto connect_without_tui = ava::app::run_command(unlocked_session, ava::app::CommandRequest{.command = "/connect anthropic"});
  expect(connect_without_tui && connect_without_tui->handled && !connect_without_tui->output.empty() &&
             connect_without_tui->output[0].find("--api-key-stdin") != std::string::npos &&
             connect_without_tui->output[0].find("--api-key-env") != std::string::npos,
         "command dispatcher /connect no-TUI error lists API-key headless setup flags");
}

}  // namespace ava::tests::app_runtime_tests

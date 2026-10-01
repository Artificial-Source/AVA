#include "sys.h"
#include "tests/support/test_harness.h"
#include "tests/support/tui_test_support.h"
#include "ava/app/commands.h"
#include "ava/agent/mode.h"
#include "ava/tui/composer.h"
#include "ava/tui/runtime.h"
#include "ava/tui/runtime_internal.h"
#include "ava/tui/session_grants.h"
#include "ava/permissions/permission.h"

#include <algorithm>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

void run_tui_permission_tests_part_1()
{
  auto prompt_input = ava::tui::handle_permission_prompt_input(ava::tui::PermissionPromptChoice::Deny, ava::tui::InputEvent{.key = ava::tui::terminal::Key::Tab});
  expect(prompt_input.action == ava::tui::PermissionPromptInputAction::Redraw && prompt_input.selected_choice == ava::tui::PermissionPromptChoice::Allow,
         "permission prompt tab toggles focus to allow");
  prompt_input = ava::tui::handle_permission_prompt_input(ava::tui::PermissionPromptChoice::Allow, ava::tui::InputEvent{.key = ava::tui::terminal::Key::Tab});
  expect(prompt_input.action == ava::tui::PermissionPromptInputAction::Redraw && prompt_input.selected_choice == ava::tui::PermissionPromptChoice::Deny,
         "permission prompt tab toggles focus back to deny");
  prompt_input = ava::tui::handle_permission_prompt_input(ava::tui::PermissionPromptChoice::Allow, ava::tui::InputEvent{.key = ava::tui::terminal::Key::ArrowLeft});
  expect(prompt_input.action == ava::tui::PermissionPromptInputAction::Redraw && prompt_input.selected_choice == ava::tui::PermissionPromptChoice::Deny,
         "permission prompt left arrow selects deny");
  prompt_input = ava::tui::handle_permission_prompt_input(ava::tui::PermissionPromptChoice::Deny, ava::tui::InputEvent{.key = ava::tui::terminal::Key::ArrowRight});
  expect(prompt_input.action == ava::tui::PermissionPromptInputAction::Redraw && prompt_input.selected_choice == ava::tui::PermissionPromptChoice::Allow,
         "permission prompt right arrow selects allow");
  prompt_input = ava::tui::handle_permission_prompt_input(ava::tui::PermissionPromptChoice::Allow, ava::tui::InputEvent{.key = ava::tui::terminal::Key::ArrowUp});
  expect(prompt_input.action == ava::tui::PermissionPromptInputAction::Redraw && prompt_input.selected_choice == ava::tui::PermissionPromptChoice::Deny,
         "permission prompt up arrow selects the previous action consistently with list modals");
  prompt_input = ava::tui::handle_permission_prompt_input(ava::tui::PermissionPromptChoice::Deny, ava::tui::InputEvent{.key = ava::tui::terminal::Key::ArrowDown});
  expect(prompt_input.action == ava::tui::PermissionPromptInputAction::Redraw && prompt_input.selected_choice == ava::tui::PermissionPromptChoice::Allow,
         "permission prompt down arrow selects the next action consistently with list modals");
  prompt_input = ava::tui::handle_permission_prompt_input(ava::tui::PermissionPromptChoice::Allow, ava::tui::InputEvent{.key = ava::tui::terminal::Key::Enter});
  expect(prompt_input.action == ava::tui::PermissionPromptInputAction::ResolveAllow, "permission prompt enter confirms selected allow");
  prompt_input =
      ava::tui::handle_permission_prompt_input(ava::tui::PermissionPromptChoice::Deny, ava::tui::InputEvent{.key = ava::tui::terminal::Key::Character, .character = ' '});
  expect(prompt_input.action == ava::tui::PermissionPromptInputAction::ResolveDeny, "permission prompt space confirms selected deny");
  prompt_input = ava::tui::handle_permission_prompt_input(ava::tui::PermissionPromptChoice::Allow, ava::tui::InputEvent{.key = ava::tui::terminal::Key::Space});
  expect(prompt_input.action == ava::tui::PermissionPromptInputAction::ResolveAllow, "permission prompt semantic Space confirms the selected choice");
  prompt_input = ava::tui::handle_permission_prompt_input(ava::tui::PermissionPromptChoice::Allow, ava::tui::InputEvent{.key = ava::tui::terminal::Key::Escape});
  expect(prompt_input.action == ava::tui::PermissionPromptInputAction::ResolveDeny, "permission prompt escape resolves deny");
  prompt_input = ava::tui::handle_permission_prompt_input(ava::tui::PermissionPromptChoice::Allow, ava::tui::InputEvent{.key = ava::tui::terminal::Key::CtrlC});
  expect(prompt_input.action == ava::tui::PermissionPromptInputAction::ResolveDeny, "permission prompt ctrl-c resolves deny");
  prompt_input = ava::tui::handle_permission_prompt_input(ava::tui::PermissionPromptChoice::Allow, ava::tui::InputEvent{.key = ava::tui::terminal::Key::CtrlD});
  expect(prompt_input.action == ava::tui::PermissionPromptInputAction::ResolveDeny, "permission prompt ctrl-d resolves deny");
  prompt_input =
      ava::tui::handle_permission_prompt_input(ava::tui::PermissionPromptChoice::Deny, ava::tui::InputEvent{.key = ava::tui::terminal::Key::Character, .character = 'A'});
  expect(prompt_input.action == ava::tui::PermissionPromptInputAction::ResolveAllow, "permission prompt A resolves allow");
  prompt_input = ava::tui::handle_permission_prompt_input(ava::tui::PermissionPromptChoice::Allow,
                                                          ava::tui::InputEvent{.key = ava::tui::terminal::Key::Character, .character = 'D'});
  expect(prompt_input.action == ava::tui::PermissionPromptInputAction::ResolveDeny, "permission prompt D resolves deny");
  prompt_input = ava::tui::handle_permission_prompt_input(ava::tui::PermissionPromptChoice::Allow,
                                                          ava::tui::InputEvent{.key = ava::tui::terminal::Key::Character, .character = 'x'});
  expect(prompt_input.action == ava::tui::PermissionPromptInputAction::None && prompt_input.selected_choice == ava::tui::PermissionPromptChoice::Allow,
         "permission prompt ignores unmapped character keys without changing focus");
  prompt_input = ava::tui::handle_permission_prompt_input(ava::tui::PermissionPromptChoice::Allow,
                                                          ava::tui::InputEvent{.key = ava::tui::terminal::Key::Character, .character = 'S'}, false);
  expect(prompt_input.action == ava::tui::PermissionPromptInputAction::None && prompt_input.selected_choice == ava::tui::PermissionPromptChoice::Allow,
         "permission prompt ignores session shortcut when session grant is unavailable");
  prompt_input = ava::tui::handle_permission_prompt_input(ava::tui::PermissionPromptChoice::Allow,
                                                          ava::tui::InputEvent{.key = ava::tui::terminal::Key::Character, .character = 'R'}, false, false, false);
  expect(prompt_input.action == ava::tui::PermissionPromptInputAction::None && prompt_input.selected_choice == ava::tui::PermissionPromptChoice::Allow,
         "permission prompt ignores remembered-rule shortcut when rule storage is unavailable");
  prompt_input =
      ava::tui::handle_permission_prompt_input(ava::tui::PermissionPromptChoice::Allow, ava::tui::InputEvent{.key = ava::tui::terminal::Key::Tab}, false, true, true);
  expect(prompt_input.action == ava::tui::PermissionPromptInputAction::Redraw && prompt_input.selected_choice == ava::tui::PermissionPromptChoice::DenyRemember,
         "permission prompt cycles to remembered deny when rule storage is available");
  prompt_input = ava::tui::handle_permission_prompt_input(ava::tui::PermissionPromptChoice::DenyRemember, ava::tui::InputEvent{.key = ava::tui::terminal::Key::Enter},
                                                          false, true, true);
  expect(prompt_input.action == ava::tui::PermissionPromptInputAction::ResolveDenyRemember, "permission prompt enter confirms remembered deny");
  prompt_input = ava::tui::handle_permission_prompt_input(ava::tui::PermissionPromptChoice::Allow,
                                                          ava::tui::InputEvent{.key = ava::tui::terminal::Key::Character, .character = 'R'}, false, true, true);
  expect(
      prompt_input.action == ava::tui::PermissionPromptInputAction::Redraw && prompt_input.selected_choice == ava::tui::PermissionPromptChoice::AllowRemember,
      "permission prompt R toggles the selected allow choice into a remembered allow");
  prompt_input = ava::tui::handle_permission_prompt_input(ava::tui::PermissionPromptChoice::AllowRemember, ava::tui::InputEvent{.key = ava::tui::terminal::Key::Enter},
                                                          false, true, true);
  expect(prompt_input.action == ava::tui::PermissionPromptInputAction::ResolveAllowRemember, "permission prompt enter confirms remembered allow");
  prompt_input = ava::tui::handle_permission_prompt_input(ava::tui::PermissionPromptChoice::Deny,
                                                          ava::tui::InputEvent{.key = ava::tui::terminal::Key::Character, .character = 'R'}, false, false, true);
  expect(prompt_input.action == ava::tui::PermissionPromptInputAction::Redraw && prompt_input.selected_choice == ava::tui::PermissionPromptChoice::DenyRemember,
         "permission prompt keeps remembered deny available when a Critical command cannot be remembered as allow");
  prompt_input = ava::tui::handle_permission_prompt_input(ava::tui::PermissionPromptChoice::Allow,
                                                          ava::tui::InputEvent{.key = ava::tui::terminal::Key::Character, .character = 'R'}, false, false, true);
  expect(prompt_input.action == ava::tui::PermissionPromptInputAction::None && prompt_input.selected_choice == ava::tui::PermissionPromptChoice::Allow,
         "permission prompt does not expose remembered allow when the backend only permits one-shot approval");
  prompt_input = ava::tui::handle_permission_prompt_input(ava::tui::PermissionPromptChoice::Allow,
                                                          ava::tui::InputEvent{.key = ava::tui::terminal::Key::Character, .character = 'S'}, true, true, true);
  expect(prompt_input.action == ava::tui::PermissionPromptInputAction::ResolveAllowSession &&
             prompt_input.selected_choice == ava::tui::PermissionPromptChoice::AllowSession,
         "permission prompt S resolves allow session when session grant is available");
  prompt_input =
      ava::tui::handle_permission_prompt_input(ava::tui::PermissionPromptChoice::Allow, ava::tui::InputEvent{.key = ava::tui::terminal::Key::Tab}, true, true, true);
  expect(prompt_input.action == ava::tui::PermissionPromptInputAction::Redraw && prompt_input.selected_choice == ava::tui::PermissionPromptChoice::AllowSession,
         "permission prompt tab advances from allow to allow session when available");
  prompt_input = ava::tui::handle_permission_prompt_input(ava::tui::PermissionPromptChoice::AllowSession, ava::tui::InputEvent{.key = ava::tui::terminal::Key::Tab}, true,
                                                          true, true);
  expect(prompt_input.action == ava::tui::PermissionPromptInputAction::Redraw && prompt_input.selected_choice == ava::tui::PermissionPromptChoice::DenyRemember,
         "permission prompt tab advances from allow session to remembered deny when available");
  prompt_input = ava::tui::handle_permission_prompt_input(ava::tui::PermissionPromptChoice::AllowSession,
                                                          ava::tui::InputEvent{.key = ava::tui::terminal::Key::Character, .character = 'R'}, true, true, true);
  expect(
      prompt_input.action == ava::tui::PermissionPromptInputAction::Redraw && prompt_input.selected_choice == ava::tui::PermissionPromptChoice::AllowRemember,
      "permission prompt R toggles allow session into remembered allow");
  ava::permissions::CommandPermissionMetadata one_shot_metadata;
  one_shot_metadata.level = ava::command::CommandLevel::Critical;
  one_shot_metadata.backend_maximum_scope = ava::command::InteractiveScope::Once;
  ava::permissions::PermissionPrompt one_shot_prompt;
  one_shot_prompt.operation = ava::permissions::Operation::RunCommand;
  one_shot_prompt.command_metadata = one_shot_metadata;
  auto const one_shot_remember_availability = ava::tui::permission_prompt_remember_availability(one_shot_prompt, true);
  auto const unavailable_storage_remember_availability = ava::tui::permission_prompt_remember_availability(one_shot_prompt, false);
  expect(!one_shot_remember_availability.allow_remember_available && one_shot_remember_availability.deny_remember_available &&
             !unavailable_storage_remember_availability.allow_remember_available && !unavailable_storage_remember_availability.deny_remember_available,
         "tui runtime enables a persistent deny but not a persistent allow for one-shot Critical prompts when rule storage exists");

  ava::tui::PermissionPromptView guidance_prompt{
      .tool_name = "bash", .operation = "bash", .target = "", .command = "true", .reason = "ask", .selected_choice = ava::tui::PermissionPromptChoice::Allow};
  auto guidance_input = ava::tui::handle_permission_prompt_input(guidance_prompt, ava::tui::InputEvent{.key = ava::tui::terminal::Key::Character, .character = 'G'});
  expect(guidance_input.action == ava::tui::PermissionPromptInputAction::Redraw && guidance_input.guidance_mode &&
             guidance_input.selected_choice == ava::tui::PermissionPromptChoice::Deny && guidance_input.guidance_text.empty(),
         "permission prompt G enters guidance mode as one-shot reject without resolving");
  guidance_prompt.guidance_mode = true;
  guidance_prompt.selected_choice = ava::tui::PermissionPromptChoice::Deny;
  guidance_input = ava::tui::handle_permission_prompt_input(guidance_prompt, ava::tui::InputEvent{.key = ava::tui::terminal::Key::Character, .character = 'A'});
  expect(guidance_input.action == ava::tui::PermissionPromptInputAction::Redraw && guidance_input.guidance_mode && guidance_input.guidance_text == "A" &&
             guidance_input.selected_choice == ava::tui::PermissionPromptChoice::Deny,
         "permission guidance mode treats A as text and never authorizes");
  guidance_prompt.guidance_text = "A";
  guidance_input = ava::tui::handle_permission_prompt_input(guidance_prompt, ava::tui::InputEvent{.key = ava::tui::terminal::Key::Space});
  expect(guidance_input.action == ava::tui::PermissionPromptInputAction::Redraw && guidance_input.guidance_text == "A ",
         "permission guidance mode Space appends a space instead of resolving");
  guidance_prompt.guidance_text = "A ";
  guidance_input = ava::tui::handle_permission_prompt_input(guidance_prompt,
                                                            ava::tui::InputEvent{.key = ava::tui::terminal::Key::Character, .character = 'x', .text = "safe\nline\x01"});
  expect(guidance_input.action == ava::tui::PermissionPromptInputAction::Redraw && guidance_input.guidance_text == "A safeline",
         "permission guidance mode strips controls and newlines from pasted text");
  guidance_prompt.guidance_text = "base";
  // Combining acute accent on 'e' (U+0301) is one compact cluster with the base.
  guidance_input = ava::tui::handle_permission_prompt_input(
      guidance_prompt, ava::tui::InputEvent{.key = ava::tui::terminal::Key::Character, .character = '\0', .text = std::string("e\xCC\x81")});
  expect(guidance_input.guidance_text == std::string("basee\xCC\x81"), "permission guidance mode appends combining clusters intact");
  guidance_prompt.guidance_text = guidance_input.guidance_text;
  guidance_input = ava::tui::handle_permission_prompt_input(guidance_prompt, ava::tui::InputEvent{.key = ava::tui::terminal::Key::Backspace});
  expect(guidance_input.action == ava::tui::PermissionPromptInputAction::Redraw && guidance_input.guidance_text == "base",
         "permission guidance Backspace removes one compact cluster atomically");
  // ZWJ family emoji cluster must erase as one unit.
  std::string const zwj_family = "\xF0\x9F\x91\xA8\xE2\x80\x8D\xF0\x9F\x91\xA9\xE2\x80\x8D\xF0\x9F\x91\xA7";
  guidance_prompt.guidance_text = "x" + zwj_family;
  guidance_input = ava::tui::handle_permission_prompt_input(guidance_prompt, ava::tui::InputEvent{.key = ava::tui::terminal::Key::Backspace});
  expect(guidance_input.guidance_text == "x", "permission guidance Backspace removes a ZWJ emoji cluster atomically");

  guidance_prompt.guidance_text = std::string(ava::permissions::kMaxPermissionUserGuidanceBytes, 'a');
  auto const at_cap = guidance_prompt.guidance_text;
  guidance_input = ava::tui::handle_permission_prompt_input(guidance_prompt, ava::tui::InputEvent{.key = ava::tui::terminal::Key::Character, .character = 'z'});
  expect(guidance_input.guidance_text == at_cap && guidance_input.guidance_text.size() == ava::permissions::kMaxPermissionUserGuidanceBytes,
         "permission guidance enforces the exact 2048-byte cap without growth");
  // Multi-byte cluster at the boundary must not partially split.
  guidance_prompt.guidance_text = std::string(ava::permissions::kMaxPermissionUserGuidanceBytes - 1, 'b');
  guidance_input = ava::tui::handle_permission_prompt_input(
      guidance_prompt, ava::tui::InputEvent{.key = ava::tui::terminal::Key::Character, .character = '\0', .text = std::string("\xC3\xA9")});  // é
  expect(guidance_input.guidance_text.size() == ava::permissions::kMaxPermissionUserGuidanceBytes - 1 &&
             guidance_input.guidance_text.find("\xC3") == std::string::npos,
         "permission guidance over-cap rejects a multi-byte cluster without splitting it");

  guidance_prompt.guidance_text = "do not escalate";
  guidance_input = ava::tui::handle_permission_prompt_input(guidance_prompt, ava::tui::InputEvent{.key = ava::tui::terminal::Key::Enter});
  expect(guidance_input.action == ava::tui::PermissionPromptInputAction::ResolveDeny &&
             guidance_input.selected_choice == ava::tui::PermissionPromptChoice::Deny && guidance_input.guidance_text == "do not escalate",
         "permission guidance Enter resolves one-shot reject with the optional text");

  guidance_prompt.guidance_text = "will discard";
  guidance_input = ava::tui::handle_permission_prompt_input(guidance_prompt, ava::tui::InputEvent{.key = ava::tui::terminal::Key::Escape});
  expect(guidance_input.action == ava::tui::PermissionPromptInputAction::ResolveDeny && guidance_input.guidance_text.empty(),
         "permission guidance Escape resolves reject with no guidance");
  guidance_prompt.guidance_text = "will discard";
  guidance_input = ava::tui::handle_permission_prompt_input(guidance_prompt, ava::tui::InputEvent{.key = ava::tui::terminal::Key::CtrlC});
  expect(guidance_input.action == ava::tui::PermissionPromptInputAction::ResolveDeny && guidance_input.guidance_text.empty(),
         "permission guidance Ctrl-C resolves reject with no guidance");
  guidance_prompt.guidance_text = "will discard";
  guidance_input = ava::tui::handle_permission_prompt_input(guidance_prompt, ava::tui::InputEvent{.key = ava::tui::terminal::Key::CtrlD});
  expect(guidance_input.action == ava::tui::PermissionPromptInputAction::ResolveDeny && guidance_input.guidance_text.empty(),
         "permission guidance Ctrl-D resolves reject with no guidance");

  guidance_prompt.guidance_text = "locked";
  for (auto const key : {ava::tui::terminal::Key::Tab, ava::tui::terminal::Key::ArrowLeft, ava::tui::terminal::Key::ArrowRight, ava::tui::terminal::Key::ArrowUp, ava::tui::terminal::Key::ArrowDown})
  {
    guidance_input = ava::tui::handle_permission_prompt_input(guidance_prompt, ava::tui::InputEvent{.key = key});
    expect(guidance_input.action == ava::tui::PermissionPromptInputAction::None && guidance_input.guidance_mode &&
               guidance_input.selected_choice == ava::tui::PermissionPromptChoice::Deny && guidance_input.guidance_text == "locked",
           "permission guidance mode ignores navigation/authorization keys");
  }

  guidance_prompt.guidance_mode = true;
  guidance_prompt.guidance_text.clear();
  guidance_input = ava::tui::handle_permission_prompt_input(guidance_prompt, ava::tui::InputEvent{.key = ava::tui::terminal::Key::Enter});
  expect(guidance_input.action == ava::tui::PermissionPromptInputAction::ResolveDeny && guidance_input.guidance_text.empty(),
         "empty guidance Enter is ordinary one-shot reject");

  // Choice-only overload remains unchanged: G is unmapped there.
  auto choice_only = ava::tui::handle_permission_prompt_input(ava::tui::PermissionPromptChoice::Allow,
                                                              ava::tui::InputEvent{.key = ava::tui::terminal::Key::Character, .character = 'G'});
  expect(choice_only.action == ava::tui::PermissionPromptInputAction::None && choice_only.selected_choice == ava::tui::PermissionPromptChoice::Allow,
         "choice-only permission input overload leaves G unmapped for compatibility");

  expect(ava::permissions::validated_permission_user_guidance("use the workspace recipe") == std::string("use the workspace recipe"),
         "permission guidance validator accepts compact control-free UTF-8");
  expect(!ava::permissions::validated_permission_user_guidance(""), "permission guidance validator rejects empty guidance");
  expect(!ava::permissions::validated_permission_user_guidance("line\nbreak"), "permission guidance validator rejects newlines as forged controls");
  expect(!ava::permissions::validated_permission_user_guidance(std::string("bad\x01byte")), "permission guidance validator rejects control bytes");
  expect(!ava::permissions::validated_permission_user_guidance(std::string(ava::permissions::kMaxPermissionUserGuidanceBytes + 1, 'x')),
         "permission guidance validator rejects over-cap forged guidance");
  expect(!ava::permissions::validated_permission_user_guidance(std::string("bad\xC3")), "permission guidance validator rejects invalid UTF-8");

  // Runtime stop / Esc-family paths must never carry guidance on the decision.
  ava::permissions::PermissionResolutionDecision stop_decision{ava::permissions::PermissionResolution::Deny};
  expect(stop_decision.user_guidance.empty() && stop_decision.reason.empty() && ava::permissions::to_string(stop_decision) == "deny",
         "runtime stop-style one-shot deny keeps user_guidance distinct and empty");
  ava::permissions::PermissionResolutionDecision guided_decision{ava::permissions::PermissionResolution::Deny};
  guided_decision.user_guidance = "stay in the workspace";
  expect(guided_decision.user_guidance == "stay in the workspace" && guided_decision.reason.empty() && ava::permissions::to_string(guided_decision) == "deny",
         "PermissionResolutionDecision carries one-shot guidance separately from reason/resolution");
}
void run_tui_permission_tests_part_2()
{
  auto const permission_modal =
      ava::tui::render_composer(ava::tui::ComposerSnapshot{.mode = "build",
                                                           .provider = "openai",
                                                           .model = "gpt-5.5",
                                                           .session_id = "session_test",
                                                           .input = "",
                                                           .status = "permission required",
                                                           .transcript = {ava::tui::TranscriptItem{.label = "ava", .text = "thinking"}},
                                                           .permission_prompt = ava::tui::PermissionPromptView{.tool_name = "bash\x1b[31m",
                                                                                                               .operation = "bash",
                                                                                                               .target = "/tmp/outside",
                                                                                                               .command = "git push origin main",
                                                                                                               .reason = "command can change external state",
                                                                                                               .risk = "high",
                                                                                                               .request_id = "permreq_push"},
                                                           .width = 80,
                                                           .height = 12});
  expect(
      std::ranges::any_of(permission_modal, [](std::string const& line) { return strip_sgr(line).find("! Permission required") != std::string::npos; }) &&
          std::ranges::any_of(permission_modal, [](std::string const& line) { return strip_sgr(line).find("Shell command") != std::string::npos; }) &&
          std::ranges::any_of(permission_modal, [](std::string const& line) { return strip_sgr(line).find("$ git push origin main") != std::string::npos; }) &&
          std::ranges::any_of(permission_modal,
                              [](std::string const& line) {
                                auto const visible = strip_sgr(line);
                                return visible.find("› Reject") != std::string::npos && visible.find("Allow once") != std::string::npos &&
                                       visible.find('[') == std::string::npos;
                              }) &&
          std::ranges::any_of(permission_modal,
                              [](std::string const& line) {
                                auto visible = strip_sgr(line);
                                return visible.find("A/D shortcuts") != std::string::npos && visible.find("Enter confirm") != std::string::npos &&
                                       visible.find("Esc reject") != std::string::npos;
                              }) &&
          std::ranges::any_of(permission_modal,
                              [](std::string const& line) {
                                auto visible = strip_sgr(line);
                                return visible.find("risk high") != std::string::npos && visible.find("id permreq_push") == std::string::npos &&
                                       visible.find("reason command can change external state") != std::string::npos;
                              }) &&
          std::ranges::none_of(permission_modal, [](std::string const& line) { return strip_sgr(line).find("permreq_push") != std::string::npos; }) &&
          std::ranges::none_of(permission_modal, [](std::string const& line) { return line.find("\x1b[7m") != std::string::npos; }) &&
          std::ranges::none_of(permission_modal,
                               [](std::string const& line) { return line.find("bash") != std::string::npos && line.find("\x1b[31m") != std::string::npos; }),
      "tui renders a quiet permission dock with human action, warning selection, and no resolver metadata");
  expect(std::ranges::all_of(permission_modal, [](std::string const& line) { return line.find('\n') == std::string::npos && visible_columns(line) <= 80; }) &&
             std::ranges::any_of(permission_modal, [](std::string const& line) { return strip_sgr(line).find("› Reject") != std::string::npos; }) &&
             std::ranges::any_of(permission_modal, [](std::string const& line) { return strip_sgr(line).find("Allow once") != std::string::npos; }) &&
             std::ranges::any_of(permission_modal, [](std::string const& line) { return strip_sgr(line).find("Enter confirm") != std::string::npos; }) &&
             std::ranges::any_of(permission_modal, [](std::string const& line) { return strip_sgr(line).find("Esc reject") != std::string::npos; }),
         "tui permission dock controls stay within 80 visible columns without losing controls");

  auto dock_contract_prompt = ava::tui::PermissionPromptView{.tool_name = "bash\x1b[31m",
                                                             .operation = "run_command",
                                                             .target = "/tmp/hidden-target",
                                                             .command = "printf safe\x1b[31m",
                                                             .reason = "changes external state\x1b[31m",
                                                             .risk = "high",
                                                             .request_id = "resolver-id-must-stay-hidden"};
  auto const roomy_permission_rows = ava::tui::detail::render_permission_prompt(dock_contract_prompt, 96, 7);
  auto const eighty_permission_rows = ava::tui::detail::render_permission_prompt(dock_contract_prompt, 80, 7);
  auto const& joined_permission_rows = tui_test_support::join_visible_lines;
  auto const roomy_permission_text = joined_permission_rows(roomy_permission_rows);
  auto const eighty_permission_text = joined_permission_rows(eighty_permission_rows);
  expect(roomy_permission_text.find("! Permission required") != std::string::npos && roomy_permission_text.find("Shell command") != std::string::npos &&
             roomy_permission_text.find("$ printf safe?[31m") != std::string::npos && roomy_permission_text.find("risk high") != std::string::npos &&
             roomy_permission_text.find("reason changes external state?[31m") != std::string::npos &&
             roomy_permission_text.find("resolver-id-must-stay-hidden") == std::string::npos && roomy_permission_text.find("--") == std::string::npos &&
             std::ranges::none_of(roomy_permission_rows, [](std::string const& row) { return row.find("\x1b[7m") != std::string::npos; }),
         "tui roomy permission renderer shows action, command, risk, and reason without ids, dashed error chrome, or reverse video");
  expect(eighty_permission_text.find("› Reject") != std::string::npos && eighty_permission_text.find("Allow once") != std::string::npos &&
             eighty_permission_text.find("Enter confirm") != std::string::npos &&
             std::ranges::all_of(eighty_permission_rows,
                                 [](std::string const& row) { return row.find('\n') == std::string::npos && visible_columns(row) <= 80; }) &&
             std::ranges::none_of(eighty_permission_rows, [](std::string const& row) { return row.find("\x1b[31m") != std::string::npos; }),
         "tui 80-column permission renderer keeps choices understandable and sanitizes untrusted controls");
  for (auto const budget : {std::size_t{1}, std::size_t{2}, std::size_t{3}, std::size_t{4}})
  {
    auto const tiny_rows = ava::tui::detail::render_permission_prompt(dock_contract_prompt, 20, budget);
    auto const tiny_text = joined_permission_rows(tiny_rows);
    expect(!tiny_rows.empty() && tiny_rows.size() <= budget &&
               std::ranges::all_of(tiny_rows, [](std::string const& row) { return row.find('\n') == std::string::npos && visible_columns(row) <= 20; }) &&
               tiny_text.find("resolver-id-must-stay-hidden") == std::string::npos && tiny_text.find("›") != std::string::npos,
           "tui permission renderer degrades safely at tiny row budget " + std::to_string(budget));
  }

  auto const external_permission_modal = ava::tui::render_composer(ava::tui::ComposerSnapshot{
      .mode = "build",
      .provider = "openai",
      .model = "gpt-5.5",
      .session_id = "session_test",
      .input = "",
      .status = "permission required",
      .transcript = {},
      .permission_prompt =
          ava::tui::PermissionPromptView{
              .tool_name = "read_file", .operation = "read", .target = "/tmp/outside.txt", .reason = "target is outside the workspace"},
      .width = 80,
      .height = 8});
  expect(std::ranges::any_of(external_permission_modal,
                             [](std::string const& line) { return strip_sgr(line).find("Access external directory /tmp/outside.txt") != std::string::npos; }),
         "tui permission dock uses OpenCode-style external-directory wording for outside-workspace targets");

  auto const remembered_permission_modal =
      ava::tui::render_composer(ava::tui::ComposerSnapshot{.mode = "build",
                                                           .provider = "openai",
                                                           .model = "gpt-5.5",
                                                           .session_id = "session_test",
                                                           .input = "",
                                                           .status = "permission required",
                                                           .transcript = {},
                                                           .permission_prompt = ava::tui::PermissionPromptView{.tool_name = "bash",
                                                                                                               .operation = "bash",
                                                                                                               .target = "/workspace",
                                                                                                               .command = "git push origin main",
                                                                                                               .reason = "command can change external state",
                                                                                                               .risk = "high",
                                                                                                               .allow_remember_available = true,
                                                                                                               .deny_remember_available = true},
                                                           .width = 96,
                                                           .height = 10});
  expect(
      std::ranges::any_of(remembered_permission_modal,
                          [](std::string const& line) {
                            auto visible = strip_sgr(line);
                            return visible.find("Always reject") != std::string::npos && visible.find("Always allow") != std::string::npos;
                          }) &&
          std::ranges::any_of(remembered_permission_modal, [](std::string const& line) { return strip_sgr(line).find("R remember") != std::string::npos; }) &&
          std::ranges::all_of(remembered_permission_modal, [](std::string const& line) { return visible_columns(line) <= 96; }),
      "tui permission dock exposes remembered reject and always-allow choices when rule storage is available");

  auto const session_permission_modal =
      ava::tui::render_composer(ava::tui::ComposerSnapshot{.mode = "build",
                                                           .provider = "openai",
                                                           .model = "gpt-5.5",
                                                           .session_id = "session_test",
                                                           .input = "",
                                                           .status = "permission required",
                                                           .transcript = {},
                                                           .permission_prompt = ava::tui::PermissionPromptView{.tool_name = "bash",
                                                                                                               .operation = "bash",
                                                                                                               .target = "/workspace",
                                                                                                               .command = "cmake --build build",
                                                                                                               .reason = "sealed workspace recipe",
                                                                                                               .risk = "medium",
                                                                                                               .allow_session_available = true,
                                                                                                               .allow_remember_available = true,
                                                                                                               .deny_remember_available = true},
                                                           .width = 120,
                                                           .height = 10});
  expect(std::ranges::any_of(session_permission_modal,
                             [](std::string const& line) {
                               auto visible = strip_sgr(line);
                               return visible.find("Allow session") != std::string::npos && visible.find("Allow once") != std::string::npos &&
                                      visible.find("Always allow") != std::string::npos && visible.find("Always reject") != std::string::npos;
                             }) &&
             std::ranges::any_of(session_permission_modal,
                                 [](std::string const& line) {
                                   auto visible = strip_sgr(line);
                                   return visible.find("A/S/D shortcuts") != std::string::npos && visible.find("R remember") != std::string::npos;
                                 }) &&
             std::ranges::all_of(session_permission_modal, [](std::string const& line) { return visible_columns(line) <= 120; }),
         "tui permission dock exposes allow session between allow once and always-allow when session grant is available");

  auto const deny_only_remember_permission_modal =
      ava::tui::render_composer(ava::tui::ComposerSnapshot{.mode = "build",
                                                           .provider = "openai",
                                                           .model = "gpt-5.5",
                                                           .session_id = "session_test",
                                                           .input = "",
                                                           .status = "permission required",
                                                           .transcript = {},
                                                           .permission_prompt = ava::tui::PermissionPromptView{.tool_name = "bash",
                                                                                                               .operation = "bash",
                                                                                                               .target = "/workspace",
                                                                                                               .command = "curl https://example.test",
                                                                                                               .reason = "containment unavailable",
                                                                                                               .risk = "critical",
                                                                                                               .allow_remember_available = false,
                                                                                                               .deny_remember_available = true},
                                                           .width = 96,
                                                           .height = 10});
  expect(std::ranges::any_of(deny_only_remember_permission_modal,
                             [](std::string const& line) {
                               auto visible = strip_sgr(line);
                               return visible.find("Always reject") != std::string::npos && visible.find("Always allow") == std::string::npos;
                             }) &&
             std::ranges::any_of(deny_only_remember_permission_modal,
                                 [](std::string const& line) { return strip_sgr(line).find("R remember") != std::string::npos; }),
         "tui permission dock renders a persistent deny without offering an unavailable persistent allow");

  auto const allow_focused_modal = ava::tui::render_composer(
      ava::tui::ComposerSnapshot{.mode = "build",
                                 .provider = "openai",
                                 .model = "gpt-5.5",
                                 .session_id = "session_test",
                                 .input = "",
                                 .status = "permission required",
                                 .transcript = {},
                                 .permission_prompt = ava::tui::PermissionPromptView{.tool_name = "bash",
                                                                                     .operation = "bash",
                                                                                     .target = "",
                                                                                     .command = "true",
                                                                                     .reason = "unknown risk",
                                                                                     .selected_choice = ava::tui::PermissionPromptChoice::Allow},
                                 .width = 80,
                                 .height = 14});
  expect(std::ranges::any_of(allow_focused_modal,
                             [](std::string const& line) {
                               return strip_sgr(line).find("› Allow once") != std::string::npos && line.find("\x1b[7m") == std::string::npos &&
                                      line.find("\x1b[38;2;251;191;36m") != std::string::npos;
                             }),
         "tui permission dock highlights the selected allow choice without reverse video");

  auto const diff_permission_modal = ava::tui::render_composer(
      ava::tui::ComposerSnapshot{.mode = "build",
                                 .provider = "openai",
                                 .model = "gpt-5.5",
                                 .session_id = "session_test",
                                 .input = "",
                                 .status = "permission required",
                                 .transcript = {},
                                 .permission_prompt = ava::tui::PermissionPromptView{.tool_name = "write_file",
                                                                                     .operation = "write_file",
                                                                                     .target = "/tmp/outside.txt",
                                                                                     .command = "",
                                                                                     .reason = "external mutation",
                                                                                     .diff_preview = "--- /tmp/outside.txt\n+++ /tmp/outside.txt\n@@ -1,1 +1,1 "
                                                                                                     "@@\n-old line\n+new line\n",
                                                                                     .diff_truncated = true},
                                 .width = 54,
                                 .height = 15});
  expect(
      std::ranges::all_of(diff_permission_modal, [](std::string const& line) { return line.find('\n') == std::string::npos && visible_columns(line) <= 54; }) &&
          std::ranges::any_of(diff_permission_modal, [](std::string const& line) { return strip_sgr(line).find("diff:") != std::string::npos; }) &&
          std::ranges::any_of(diff_permission_modal, [](std::string const& line) { return strip_sgr(line).find("-old line") != std::string::npos; }) &&
          std::ranges::any_of(diff_permission_modal, [](std::string const& line) { return strip_sgr(line).find("+new line") != std::string::npos; }) &&
          std::ranges::any_of(diff_permission_modal, [](std::string const& line) { return strip_sgr(line).find("[diff truncated]") != std::string::npos; }) &&
          std::ranges::any_of(diff_permission_modal,
                              [](std::string const& line) {
                                return strip_sgr(line).find("› Reject") != std::string::npos && strip_sgr(line).find("Allow once") != std::string::npos;
                              }) &&
          std::ranges::any_of(diff_permission_modal, [](std::string const& line) { return strip_sgr(line).find("Esc reject") != std::string::npos; }),
      "tui permission dock renders backend-provided mutation diffs while preserving fail-closed controls");

  auto const narrow_diff_permission_modal = ava::tui::render_composer(
      ava::tui::ComposerSnapshot{.mode = "build",
                                 .provider = "openai",
                                 .model = "gpt-5.5",
                                 .session_id = "session_test",
                                 .input = "",
                                 .status = "permission required",
                                 .transcript = {},
                                 .permission_prompt = ava::tui::PermissionPromptView{.tool_name = "edit",
                                                                                     .operation = "edit",
                                                                                     .target = "/tmp/outside.txt",
                                                                                     .command = "",
                                                                                     .reason = "external mutation",
                                                                                     .diff_preview = "--- old\n+++ new\n@@ -1,1 +1,1 @@\n-old\n+new\n"},
                                 .width = 28,
                                 .height = 10});
  expect(std::ranges::all_of(narrow_diff_permission_modal,
                             [](std::string const& line) { return line.find('\n') == std::string::npos && visible_columns(line) <= 28; }) &&
             std::ranges::any_of(narrow_diff_permission_modal, [](std::string const& line) { return strip_sgr(line).find("diff:") != std::string::npos; }) &&
             std::ranges::any_of(narrow_diff_permission_modal,
                                 [](std::string const& line) {
                                   auto const visible = strip_sgr(line);
                                   return visible.find("› Reject") != std::string::npos && visible.find("Allow once") != std::string::npos;
                                 }),
         "tui permission dock keeps diff previews bounded at narrow widths");

  auto const long_permission_modal =
      ava::tui::render_composer(ava::tui::ComposerSnapshot{.mode = "build",
                                                           .provider = "openai",
                                                           .model = "gpt-5.5",
                                                           .session_id = "session_test",
                                                           .input = "",
                                                           .status = "permission required",
                                                           .transcript = {},
                                                           .permission_prompt = ava::tui::PermissionPromptView{.tool_name = "",
                                                                                                               .operation = "write_file",
                                                                                                               .target = std::string(120, 't'),
                                                                                                               .command = std::string(120, 'c'),
                                                                                                               .reason = std::string(120, 'r'),
                                                                                                               .risk = "critical"},
                                                           .width = 80,
                                                           .height = 10});
  expect(
      std::ranges::all_of(long_permission_modal, [](std::string const& line) { return line.find('\n') == std::string::npos && visible_columns(line) <= 80; }) &&
          std::ranges::any_of(long_permission_modal,
                              [](std::string const& line) {
                                auto const visible = strip_sgr(line);
                                return visible.find("cccc") != std::string::npos && visible.find("...") != std::string::npos;
                              }) &&
          std::ranges::any_of(long_permission_modal,
                              [](std::string const& line) {
                                auto const visible = strip_sgr(line);
                                return visible.find("risk critical") != std::string::npos && visible.find("reason rrrr") != std::string::npos;
                              }),
      "tui permission dock keeps risk and reason visible when detail text is truncated");

  auto const tight_permission_modal = ava::tui::render_composer(ava::tui::ComposerSnapshot{
      .mode = "build",
      .provider = "openai",
      .model = "gpt-5.5",
      .session_id = "session_test",
      .input = "",
      .status = "permission required",
      .transcript = {ava::tui::TranscriptItem{.label = "ava", .text = "thinking"}},
      .permission_prompt = ava::tui::PermissionPromptView{.tool_name = "bash", .operation = "bash", .target = "", .command = "true", .reason = ""},
      .width = 36,
      .height = 8});
  expect(std::ranges::any_of(tight_permission_modal, [](std::string const& line) { return line.find("Permission required") != std::string::npos; }) &&
             tight_permission_modal.size() <= 8 &&
             std::ranges::all_of(tight_permission_modal,
                                 [](std::string const& line) { return line.find('\n') == std::string::npos && visible_columns(line) <= 36; }) &&
             std::ranges::any_of(tight_permission_modal,
                                 [](std::string const& line) {
                                   return strip_sgr(line).find("› Reject") != std::string::npos && strip_sgr(line).find("Allow once") != std::string::npos;
                                 }),
         "tui permission dock keeps header and controls visible in tight height");

  auto const ultra_tight_permission_modal = ava::tui::render_composer(ava::tui::ComposerSnapshot{
      .mode = "build",
      .provider = "openai",
      .model = "gpt-5.5",
      .session_id = "session_test",
      .input = "",
      .status = "permission required",
      .transcript = {},
      .permission_prompt = ava::tui::PermissionPromptView{.tool_name = "bash", .operation = "bash", .target = "", .command = "true", .reason = ""},
      .width = 20,
      .height = 8});
  expect(std::ranges::all_of(ultra_tight_permission_modal,
                             [](std::string const& line) { return line.find('\n') == std::string::npos && visible_columns(line) <= 20; }) &&
             ultra_tight_permission_modal.size() <= 8 &&
             std::ranges::any_of(ultra_tight_permission_modal,
                                 [](std::string const& line) {
                                   auto const visible = strip_sgr(line);
                                   return visible.find("Permission") != std::string::npos;
                                 }) &&
             std::ranges::any_of(ultra_tight_permission_modal,
                                 [](std::string const& line) {
                                   auto const visible = strip_sgr(line);
                                   return visible.find("› Reject") != std::string::npos && visible.find("Once") != std::string::npos &&
                                          visible.find('[') == std::string::npos;
                                 }) &&
             std::ranges::any_of(ultra_tight_permission_modal,
                                 [](std::string const& line) {
                                   auto const visible = strip_sgr(line);
                                   return visible.find("A allow") != std::string::npos && visible.find("D reject") != std::string::npos;
                                 }),
         "tui permission dock preserves reject and allow choices at minimum width");

  std::vector<ava::tui::TranscriptItem> permission_overflow_items;
  for (int index = 0; index < 8; ++index)
  {
    permission_overflow_items.push_back(ava::tui::TranscriptItem{.label = "ava", .text = "permission item " + std::to_string(index)});
  }
  auto const permission_starved = ava::tui::render_composer(ava::tui::ComposerSnapshot{
      .mode = "build",
      .provider = "openai",
      .model = "gpt-5.5",
      .session_id = "session_test",
      .input = "hidden input",
      .status = "permission required",
      .transcript = permission_overflow_items,
      .permission_prompt = ava::tui::PermissionPromptView{.tool_name = "bash", .operation = "bash", .target = "", .command = "true", .reason = ""},
      .width = 40,
      .height = 8});
  expect(permission_starved.size() <= 8 && std::ranges::all_of(permission_starved, [](std::string const& line) { return visible_columns(line) <= 40; }) &&
             std::ranges::none_of(permission_starved, [](std::string const& line) { return strip_sgr(line).find("lines hidden") != std::string::npos; }) &&
             std::ranges::any_of(permission_starved, [](std::string const& line) { return strip_sgr(line).find("│  hidden input") != std::string::npos; }),
         "tui permission prompt stays above the composer without hidden-line banners");
}
void run_tui_permission_tests_part_3()
{
  auto const permission_frame = ava::tui::render_composer(ava::tui::ComposerSnapshot{
      .mode = "build",
      .provider = "openai",
      .model = "gpt-5.5",
      .session_id = "session_test",
      .input = "do not focus composer",
      .status = "permission required",
      .transcript = {},
      .permission_prompt = ava::tui::PermissionPromptView{.tool_name = "bash", .operation = "bash", .target = "", .command = "true", .reason = ""},
      .width = 60,
      .height = 12});
  expect(permission_frame.size() == 12 &&
             std::ranges::any_of(permission_frame,
                                 [](std::string const& line) { return strip_sgr(line).find("│  do not focus composer") != std::string::npos; }) &&
             std::ranges::any_of(permission_frame, [](std::string const& line) { return strip_sgr(line).find("Permission required") != std::string::npos; }),
         "tui composer frame renders permission dock above composer while active");
  for (std::size_t height = 8; height <= 11; ++height)
  {
    auto compact_permission = ava::tui::ComposerSnapshot{};
    compact_permission.mode = "build";
    compact_permission.provider = "openai";
    compact_permission.model = "gpt-5.5";
    compact_permission.session_id = "session_test";
    compact_permission.input = "draft one\ndraft two\ndraft three\ndraft four\ndraft five\ndraft six";
    compact_permission.status = "permission required";
    compact_permission.permission_prompt =
        ava::tui::PermissionPromptView{.tool_name = "bash", .operation = "bash", .target = "", .command = "true", .reason = ""};
    compact_permission.width = 64;
    compact_permission.height = height;
    auto const frame = ava::tui::render_composer(compact_permission);
    expect(std::ranges::any_of(frame, [](std::string const& line) { return strip_sgr(line).find("Permission required") != std::string::npos; }) &&
               std::ranges::any_of(frame, [](std::string const& line) { return strip_sgr(line).find("Allow once") != std::string::npos; }),
           "minimum-height permission dock remains visible above a wrapped composer draft");
  }

  auto guidance_view = ava::tui::PermissionPromptView{.tool_name = "bash",
                                                      .operation = "bash",
                                                      .target = "",
                                                      .command = "git push origin main",
                                                      .reason = "external state",
                                                      .risk = "high",
                                                      .selected_choice = ava::tui::PermissionPromptChoice::Deny,
                                                      .guidance_mode = true,
                                                      .guidance_text = "do not escalate privileges"};
  auto const guidance_rows = ava::tui::detail::render_permission_prompt(guidance_view, 96, 8);
  auto const guidance_text = tui_test_support::join_visible_lines(guidance_rows);
  expect(guidance_text.find("Guidance:") != std::string::npos && guidance_text.find("do not escalate privileges") != std::string::npos &&
             guidance_text.find("› Reject") != std::string::npos && guidance_text.find("Allow once") != std::string::npos &&
             std::ranges::any_of(guidance_rows,
                                 [](std::string const& row) {
                                   auto const visible = strip_sgr(row);
                                   return visible.find("reject") != std::string::npos && visible.find("Esc") != std::string::npos;
                                 }) &&
             guidance_text.find("A/D shortcuts") == std::string::npos,
         "permission guidance mode renders a Guidance row and reject-only controls without allow shortcuts");

  guidance_view.guidance_text.clear();
  auto const empty_guidance_rows = ava::tui::detail::render_permission_prompt(guidance_view, 96, 6);
  expect(tui_test_support::join_visible_lines(empty_guidance_rows).find("optional reason for the model") != std::string::npos,
         "empty guidance mode shows the optional model-facing placeholder");

  for (auto const budget : {std::size_t{1}, std::size_t{2}, std::size_t{3}, std::size_t{4}})
  {
    auto tiny_guidance = guidance_view;
    tiny_guidance.guidance_text = "keep it local";
    auto const tiny_rows = ava::tui::detail::render_permission_prompt(tiny_guidance, 28, budget);
    auto const tiny_text = tui_test_support::join_visible_lines(tiny_rows);
    expect(!tiny_rows.empty() && tiny_rows.size() <= budget &&
               std::ranges::all_of(tiny_rows, [](std::string const& row) { return row.find('\n') == std::string::npos && visible_columns(row) <= 28; }) &&
               tiny_text.find("Guidance:") != std::string::npos &&
               (budget == 1 || tiny_text.find("keep it local") != std::string::npos || tiny_text.find("Guidance:") != std::string::npos),
           "permission guidance renderer keeps Guidance identity at tiny row budget " + std::to_string(budget));
  }

  {
    ScopedEnvVar no_color_guard("NO_COLOR", "1");
    auto plain_guidance_view = guidance_view;
    plain_guidance_view.guidance_mode = true;
    plain_guidance_view.guidance_text = "do not escalate privileges";
    auto const plain_guidance = ava::tui::render_composer(ava::tui::ComposerSnapshot{.mode = "build",
                                                                                     .provider = "openai",
                                                                                     .model = "gpt-5.5",
                                                                                     .session_id = "session_test",
                                                                                     .input = "",
                                                                                     .status = "permission guidance",
                                                                                     .transcript = {},
                                                                                     .permission_prompt = plain_guidance_view,
                                                                                     .width = 80,
                                                                                     .height = 12});
    expect(std::ranges::any_of(plain_guidance,
                               [](std::string const& line) {
                                 return line.find("Guidance:") != std::string::npos && line.find("do not escalate privileges") != std::string::npos &&
                                        line.find('\x1b') == std::string::npos;
                               }) &&
               std::ranges::all_of(plain_guidance, [](std::string const& line) { return line.find('\x1b') == std::string::npos; }),
           "permission guidance remains truthful under NO_COLOR without ANSI styling");
  }

  auto const guide_keys_modal = ava::tui::render_composer(
      ava::tui::ComposerSnapshot{.mode = "build",
                                 .provider = "openai",
                                 .model = "gpt-5.5",
                                 .session_id = "session_test",
                                 .input = "",
                                 .status = "permission required",
                                 .transcript = {},
                                 .permission_prompt = ava::tui::PermissionPromptView{.tool_name = "bash",
                                                                                     .operation = "bash",
                                                                                     .target = "",
                                                                                     .command = "true",
                                                                                     .reason = "ask",
                                                                                     .selected_choice = ava::tui::PermissionPromptChoice::Deny},
                                 .width = 120,
                                 .height = 10});
  expect(std::ranges::any_of(guide_keys_modal,
                             [](std::string const& line) {
                               auto const visible = strip_sgr(line);
                               return visible.find("G=guide") != std::string::npos || visible.find("G guide") != std::string::npos;
                             }),
         "permission dock advertises G guide rejection in key help");

  auto remember_recipe = ava::tui::PermissionPromptView{.tool_name = "bash",
                                                        .operation = "bash",
                                                        .target = "",
                                                        .command = "cmake --build build",
                                                        .reason = "sealed",
                                                        .allow_remember_available = true,
                                                        .deny_remember_available = true,
                                                        .recipe_display = "cmake --build <build>",
                                                        .workspace_recipe_key = "deadbeefhash",
                                                        .selected_choice = ava::tui::PermissionPromptChoice::DenyRemember};
  auto const recipe_preview_rows = ava::tui::detail::render_permission_prompt(remember_recipe, 120, 10);
  auto const recipe_preview = tui_test_support::join_visible_lines(recipe_preview_rows);
  expect(recipe_preview.find("Always reject: workspace recipe · cmake --build <build>") != std::string::npos &&
             recipe_preview.find("deadbeefhash") == std::string::npos,
         "remembered deny preview shows workspace recipe display without recipe hashes");

  remember_recipe.recipe_display.clear();
  remember_recipe.workspace_recipe_key.clear();
  remember_recipe.command = "git push origin main";
  auto const command_preview = tui_test_support::join_visible_lines(ava::tui::detail::render_permission_prompt(remember_recipe, 120, 10));
  expect(command_preview.find("Always reject: workspace exact command · $ git push origin main") != std::string::npos,
         "remembered deny preview shows workspace exact command basis");

  auto remember_path = ava::tui::PermissionPromptView{.tool_name = "write_file",
                                                      .operation = "write_file",
                                                      .target = "/tmp/outside.txt",
                                                      .reason = "external",
                                                      .allow_remember_available = true,
                                                      .deny_remember_available = true,
                                                      .selected_choice = ava::tui::PermissionPromptChoice::AllowRemember};
  auto const path_preview = tui_test_support::join_visible_lines(ava::tui::detail::render_permission_prompt(remember_path, 120, 10));
  expect(path_preview.find("Always allow: workspace exact path · /tmp/outside.txt") != std::string::npos,
         "remembered allow preview shows workspace exact path basis");

  auto remember_tool = ava::tui::PermissionPromptView{.tool_name = "web_fetch",
                                                      .operation = "network_fetch",
                                                      .target = "",
                                                      .reason = "network",
                                                      .allow_remember_available = true,
                                                      .deny_remember_available = true,
                                                      .selected_choice = ava::tui::PermissionPromptChoice::DenyRemember};
  auto const tool_preview = tui_test_support::join_visible_lines(ava::tui::detail::render_permission_prompt(remember_tool, 120, 10));
  expect(tool_preview.find("Always reject: workspace exact operation · web_fetch") != std::string::npos,
         "remembered deny preview shows workspace exact tool/operation basis");

  for (auto const budget : {std::size_t{1}, std::size_t{2}, std::size_t{3}, std::size_t{4}})
  {
    auto const tiny_remember = tui_test_support::join_visible_lines(ava::tui::detail::render_permission_prompt(remember_recipe, 40, budget));
    expect(tiny_remember.find("workspace exact command") == std::string::npos && tiny_remember.find("Always reject:") == std::string::npos,
           "remembered rule preview stays hidden at tiny row budget " + std::to_string(budget));
  }
}
namespace {
void test_tui_session_grant_registry()
{
  auto make_prompt = [] {
    ava::permissions::CommandPermissionMetadata metadata;
    metadata.level = ava::command::CommandLevel::Standard;
    metadata.executor_identity_verified = true;
    metadata.containment_available = true;
    metadata.containment_status = ava::permissions::CommandContainmentStatus::Available;
    metadata.backend_maximum_scope = ava::command::InteractiveScope::Session;
    metadata.global_recipe_key = "global-recipe";
    metadata.workspace_recipe_key = "workspace-recipe";
    metadata.effective_allowed_scopes = {ava::command::InteractiveScope::Once, ava::command::InteractiveScope::Session};
    return ava::permissions::PermissionPrompt{.permission_request_id = "permreq_tui_session_grant",
                                              .operation = ava::permissions::Operation::RunCommand,
                                              .mode = ava::agent::Mode::Build,
                                              .workspace_dir = {},
                                              .target_path = {},
                                              .command = "ctest --test-dir build",
                                              .tool_name = "bash",
                                              .reason = "sealed workspace recipe",
                                              .risk = ava::permissions::PermissionRisk::Medium,
                                              .command_metadata = std::move(metadata)};
  };

  auto prompt = make_prompt();
  ava::tui::TuiSessionGrantRegistry registry;
  auto const created = registry.add("session_a", prompt);
  auto const reused = registry.add("session_a", prompt);
  expect(ava::tui::tui_session_grant_eligible(prompt) && created == ava::tui::TuiSessionGrantInsertResult::Added &&
             reused == ava::tui::TuiSessionGrantInsertResult::AlreadyPresent && registry.size() == 1 && registry.matches("session_a", prompt),
         "TUI session grants create once and reuse the exact eligible command recipe");

  auto different_mode = prompt;
  different_mode.mode = ava::agent::Mode::Plan;
  auto different_tool = prompt;
  different_tool.tool_name = "shell";
  auto different_recipe = prompt;
  different_recipe.command_metadata->workspace_recipe_key = "another-workspace-recipe";
  expect(!registry.matches("session_b", prompt) && !registry.matches("session_a", different_mode) && !registry.matches("session_a", different_tool) &&
             !registry.matches("session_a", different_recipe),
         "TUI session grants require exact session, mode, tool, and workspace recipe matches");

  auto no_longer_eligible = prompt;
  no_longer_eligible.command_metadata->effective_allowed_scopes = {ava::command::InteractiveScope::Once};
  expect(!ava::tui::tui_session_grant_eligible(no_longer_eligible) && !registry.matches("session_a", no_longer_eligible) &&
             registry.add("session_a", no_longer_eligible) == ava::tui::TuiSessionGrantInsertResult::Ineligible,
         "TUI session grants recheck backend eligibility before reuse or creation");

  ava::tui::TuiSessionGrantRegistry capped_registry;
  bool filled_to_cap = true;
  for (std::size_t index = 0; index < ava::tui::TuiSessionGrantRegistry::kMaxGrants; ++index)
  {
    auto distinct_prompt = make_prompt();
    distinct_prompt.command_metadata->workspace_recipe_key = "workspace-recipe-" + std::to_string(index);
    filled_to_cap = filled_to_cap && capped_registry.add("session_a", distinct_prompt) == ava::tui::TuiSessionGrantInsertResult::Added;
  }
  auto overflow_prompt = make_prompt();
  overflow_prompt.command_metadata->workspace_recipe_key = "workspace-recipe-overflow";
  expect(filled_to_cap && capped_registry.size() == ava::tui::TuiSessionGrantRegistry::kMaxGrants &&
             capped_registry.add("session_a", overflow_prompt) == ava::tui::TuiSessionGrantInsertResult::Full,
         "TUI session grants fail closed at the 64-grant in-memory cap");

  ava::tui::TuiSessionGrantRegistry same_session_registry;
  static_cast<void>(same_session_registry.add("session_a", prompt));
  expect(!same_session_registry.clear_for_session_transition("session_a", "session_a") && same_session_registry.matches("session_a", prompt),
         "applying an unchanged runtime session id preserves grants created in this TUI process");

  struct SessionTransition
  {
    std::string command;
    std::string session_id;
  };
  std::vector<SessionTransition> const transitions = {{"/new", "session_new"},
                                                      {"/resume session_resume", "session_resume"},
                                                      {"/fork", "session_fork"},
                                                      {"/clone", "session_clone"},
                                                      {"/import --confirm", "session_import"}};
  bool all_submit_transitions_clear = true;
  for (auto const& transition : transitions)
  {
    ava::tui::TuiSessionGrantRegistry transition_registry;
    static_cast<void>(transition_registry.add("session_a", prompt));
    ava::tui::TuiRuntimeStateSnapshot post_submit_state;
    post_submit_state.session_id = transition.session_id;
    ava::tui::TuiSubmitResult completed_submit;
    completed_submit.state_snapshot = std::move(post_submit_state);
    bool const cleared =
        completed_submit.state_snapshot && transition_registry.clear_for_session_transition("session_a", completed_submit.state_snapshot->session_id);
    all_submit_transitions_clear =
        all_submit_transitions_clear && cleared && transition_registry.size() == 0 && !transition_registry.matches(transition.session_id, prompt);
  }
  expect(all_submit_transitions_clear,
         "post-submit runtime snapshots clear TUI session grants for /new, /resume, /fork, /clone, and confirmed import transitions");

  auto const session_only_permission_modal =
      ava::tui::render_composer(ava::tui::ComposerSnapshot{.mode = "build",
                                                           .provider = "openai",
                                                           .model = "gpt-5.5",
                                                           .session_id = "session_a",
                                                           .input = "",
                                                           .status = "permission required",
                                                           .transcript = {},
                                                           .permission_prompt = ava::tui::PermissionPromptView{.tool_name = "bash",
                                                                                                               .operation = "bash",
                                                                                                               .target = "",
                                                                                                               .command = "ctest --test-dir build",
                                                                                                               .reason = "",
                                                                                                               .risk = "medium",
                                                                                                               .allow_session_available = true},
                                                           .width = 120,
                                                           .height = 10});
  expect(
      std::ranges::any_of(session_only_permission_modal,
                          [](std::string const& line) { return strip_sgr(line).find("A/S/D shortcuts") != std::string::npos; }) &&
          std::ranges::none_of(session_only_permission_modal, [](std::string const& line) { return strip_sgr(line).find("R remember") != std::string::npos; }),
      "TUI permission key help does not advertise R when only an in-memory session grant is available");
}
}  // namespace

void run_tui_session_grant_tests()
{
  test_tui_session_grant_registry();
}

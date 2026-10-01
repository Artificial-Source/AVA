#include "sys.h"
#include "tests/support/app_runtime_support.h"
#include "tests/support/test_harness.h"
#include "ava/app/command_plugins.h"
#include "ava/app/headless_policy.h"
#include "ava/app/plugin_ui_capability.h"
#include "ava/app/project_trust.h"
#include "ava/app/runtime/Session.h"
#include "ava/tui/composer.h"
#include "ava/tui/composer_internal.h"
#include "ava/tui/runtime_plugin_ui_internal.h"
#include "ava/plugin/enablement.h"
#include "ava/plugin/manifest.h"
#include "ava/plugin/runner.h"
#include "ava/plugin/ui_protocol.h"
#include "ava/permissions/permission.h"
#include "ava/core/json.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <future>
#include <iomanip>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>
#include <signal.h>
#include <sys/types.h>
#include <unistd.h>

namespace {

static_assert(!std::is_copy_constructible_v<ava::app::PluginUiInvocationCapability>);
static_assert(!std::is_move_constructible_v<ava::app::PluginUiInvocationCapability>);
static_assert(!std::is_copy_constructible_v<ava::app::PluginUiInvocationClaim>);
static_assert(std::is_move_constructible_v<ava::app::PluginUiInvocationClaim>);

void write_text(std::filesystem::path const& path, std::string const& text)
{
  std::filesystem::create_directories(path.parent_path());
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  file << text;
}

std::string read_text(std::filesystem::path const& path)
{
  std::ifstream file(path, std::ios::binary);
  std::ostringstream buffer;
  buffer << file.rdbuf();
  return buffer.str();
}

std::string shell_single_quote(std::string_view value)
{
  std::string quoted = "'";
  for (char const ch : value)
  {
    if (ch == '\'')
      quoted += "'\\''";
    else
      quoted.push_back(ch);
  }
  quoted += '\'';
  return quoted;
}

std::string ui_manifest_json(std::string_view id, std::string_view capabilities, std::string_view script_name = "plugin.sh")
{
  return std::string("{\n") +
         "  \"schema_version\": 1,\n"
         "  \"id\": \"" +
         ava::core::json::escape(id) +
         "\",\n"
         "  \"name\": \"UI Test Plugin\",\n"
         "  \"version\": \"0.1.0\",\n"
         "  \"api_version\": \"ava.plugin.v1\",\n"
         "  \"entrypoint\": {\"command\": \"/bin/sh\", \"args\": [\"" +
         ava::core::json::escape(script_name) +
         "\"]},\n"
         "  \"capabilities\": " +
         std::string(capabilities) +
         ",\n"
         "  \"contributes\": {\"tools\": [{\"name\": \"tool\", \"description\": \"tool\", \"input_schema\": {\"type\": \"object\"}}], "
         "\"commands\": [{\"name\": \"run\", \"description\": \"run\"}], \"event_hooks\": [{\"event\": \"tool.result\"}]}\n"
         "}";
}

ava::plugin::PluginManifest load_ui_manifest(std::filesystem::path const& plugin_dir, std::string_view id, std::string_view capabilities)
{
  write_text(plugin_dir / "plugin.json", ui_manifest_json(id, capabilities));
  auto manifest = ava::plugin::load_plugin_manifest(plugin_dir / "plugin.json");
  expect(manifest.has_value(), manifest ? "plugin UI test manifest loads" : "plugin UI test manifest loads: " + manifest.error().format());
  return manifest.value_or(ava::plugin::PluginManifest{});
}

std::optional<ava::process::ProcessScopeV1> plugin_ui_test_process_scope()
{
  auto supervisor = std::make_shared<ava::process::Supervisor>();
  auto scope = ava::process::ProcessScopeV1::application(std::move(supervisor));
  expect(scope.has_value(), scope ? "plugin UI test process scope is available" : "plugin UI test process scope is available: " + scope.error().format());
  if (!scope)
    return std::nullopt;
  return std::move(*scope);
}

ava::plugin::PluginRunnerOptions ui_runner_options(std::filesystem::path const& workspace,
                                                   std::chrono::milliseconds request_timeout = std::chrono::milliseconds(500))
{
  ava::plugin::PluginRunnerOptions options;
  options.workspace_dir = workspace;
  options.startup_timeout = std::chrono::milliseconds(500);
  options.request_timeout = request_timeout;
  options.process_scope = plugin_ui_test_process_scope();
  return options;
}

std::string initialized_record()
{
  return "{\"id\":\"ava_1\",\"type\":\"initialized\",\"api_version\":\"ava.plugin.v1\",\"plugin_version\":\"0.1.0\",\"contributions\":{}}";
}

std::string status_record(std::string_view id, std::string_view text)
{
  return "{\"id\":\"" + ava::core::json::escape(id) + "\",\"type\":\"ui.status\",\"text\":\"" + ava::core::json::escape(text) + "\"}";
}

std::string widget_record(std::string_view id, std::string_view title, std::vector<std::string> const& lines)
{
  std::string record =
      "{\"id\":\"" + ava::core::json::escape(id) + "\",\"type\":\"ui.widget\",\"title\":\"" + ava::core::json::escape(title) + "\",\"lines\":[";
  for (std::size_t index = 0; index < lines.size(); ++index)
  {
    if (index != 0)
      record += ',';
    record += "\"" + ava::core::json::escape(lines[index]) + "\"";
  }
  record += "]}";
  return record;
}

std::string select_record(std::string_view id, std::string_view title, std::string_view description, std::size_t choice_count, std::size_t option_id_bytes = 0,
                          std::size_t label_bytes = 1, std::size_t first_description_bytes = 0)
{
  std::string record = "{\"id\":\"" + ava::core::json::escape(id) + "\",\"type\":\"ui.select\",\"title\":\"" + ava::core::json::escape(title) +
                       "\",\"description\":\"" + ava::core::json::escape(description) + "\",\"choices\":[";
  for (std::size_t index = 0; index < choice_count; ++index)
  {
    if (index != 0)
      record += ',';
    std::ostringstream suffix;
    suffix << std::setw(2) << std::setfill('0') << index;
    std::string option_id = "o" + suffix.str();
    if (option_id_bytes > option_id.size())
      option_id.insert(1, option_id_bytes - option_id.size(), 'x');
    record += "{\"id\":\"" + option_id + "\",\"label\":\"" + std::string(label_bytes, 'l') + "\"";
    if (index == 0 && first_description_bytes != 0)
      record += ",\"description\":\"" + std::string(first_description_bytes, 'd') + "\"";
    record += '}';
  }
  record += "]}";
  return record;
}

ava::core::Result<ava::plugin::PluginUiRequest> parse_ui_once(std::string const& record)
{
  ava::plugin::PluginUiProtocolState state;
  return ava::plugin::parse_plugin_ui_request(record, state);
}

std::string nested_arrays(std::size_t depth)
{
  return std::string(depth, '[') + "0" + std::string(depth, ']');
}

void test_plugin_ui_manifest_capabilities_are_independent()
{
  auto const root = create_empty_root("plugin-ui-manifest-capabilities");
  std::array<std::string_view, 4> const capabilities{ava::plugin::kPluginUiStatusCapability, ava::plugin::kPluginUiWidgetCapability,
                                                     ava::plugin::kPluginUiSelectCapability, ava::plugin::kPluginUiConfirmCapability};
  for (std::size_t index = 0; index < capabilities.size(); ++index)
  {
    auto const id = "com.example.cap" + std::to_string(index);
    auto manifest = load_ui_manifest(root / id, id, "[\"commands\",\"" + std::string(capabilities[index]) + "\"]");
    for (std::size_t other = 0; other < capabilities.size(); ++other)
      expect(ava::plugin::plugin_has_capability(manifest, capabilities[other]) == (index == other), "plugin UI manifest capabilities are independent");
  }
  auto old_manifest = load_ui_manifest(root / "com.example.old", "com.example.old", "[\"commands\"]");
  expect(std::ranges::none_of(capabilities, [&](std::string_view capability) { return ava::plugin::plugin_has_capability(old_manifest, capability); }),
         "old plugin manifests remain valid without UI capabilities");
}

void test_plugin_ui_protocol_happy_paths_and_actions()
{
  ava::plugin::PluginUiProtocolState state;
  auto status = ava::plugin::parse_plugin_ui_request(status_record("status_1", "Working — café ✓"), state);
  auto widget = ava::plugin::parse_plugin_ui_request(widget_record("widget_1", "Summary", {"one", "two"}), state);
  auto select = ava::plugin::parse_plugin_ui_request(
      "{\"id\":\"select_1\",\"type\":\"ui.select\",\"title\":\"Choose\",\"description\":\"Pick one\",\"choices\":["
      "{\"id\":\"first\",\"label\":\"First\"},{\"id\":\"second\",\"label\":\"Second\",\"description\":\"details\"}]}",
      state);
  auto confirm =
      ava::plugin::parse_plugin_ui_request("{\"id\":\"confirm_1\",\"type\":\"ui.confirm\",\"title\":\"Continue\",\"description\":\"Apply changes?\"}", state);
  expect(status && std::get<ava::plugin::PluginUiStatusRequest>(*status).text == "Working — café ✓", "ui.status preserves validated UTF-8 in its DTO");
  expect(widget && std::get<ava::plugin::PluginUiWidgetRequest>(*widget).lines.size() == 2, "ui.widget parses into a validated DTO");
  expect(select && std::get<ava::plugin::PluginUiSelectRequest>(*select).choices.size() == 2, "ui.select parses into a validated DTO");
  expect(confirm && std::get<ava::plugin::PluginUiConfirmRequest>(*confirm).description == "Apply changes?", "ui.confirm parses into a validated DTO");

  if (status && select && confirm)
  {
    ava::plugin::PluginUiAction const ack{.action = ava::plugin::PluginUiActionKind::Ack, .option_id = {}};
    ava::plugin::PluginUiAction const selected{.action = ava::plugin::PluginUiActionKind::Select, .option_id = "second"};
    ava::plugin::PluginUiAction const confirmed{.action = ava::plugin::PluginUiActionKind::Confirm, .option_id = {}};
    ava::plugin::PluginUiAction const canceled{.action = ava::plugin::PluginUiActionKind::Cancel, .option_id = {}};
    expect(ava::plugin::validate_plugin_ui_action(*status, ack).has_value(), "status accepts only ack");
    expect(ava::plugin::validate_plugin_ui_action(*select, selected).has_value() && ava::plugin::validate_plugin_ui_action(*select, canceled).has_value(),
           "select accepts an exact declared option id or a host-owned cancel action");
    expect(ava::plugin::validate_plugin_ui_action(*confirm, confirmed).has_value() && ava::plugin::validate_plugin_ui_action(*confirm, canceled).has_value(),
           "confirm accepts host-owned confirm and cancel actions");
    auto serialized = ava::plugin::serialize_plugin_ui_action("select_1", selected);
    expect(serialized && *serialized == "{\"id\":\"select_1\",\"type\":\"ui.action\",\"action\":\"select\",\"option_id\":\"second\"}",
           "UI actions serialize to the exact ava.plugin.v1 reply record");
    auto const invalid_kind = static_cast<ava::plugin::PluginUiActionKind>(255);
    expect(!ava::plugin::serialize_plugin_ui_action("select_1", {.action = invalid_kind, .option_id = {}}),
           "UI action serialization rejects unknown enum values instead of coercing them to cancel");
    expect(!ava::plugin::validate_plugin_ui_action(*status, canceled) &&
               !ava::plugin::validate_plugin_ui_action(*select, {.action = ava::plugin::PluginUiActionKind::Select, .option_id = "missing"}) &&
               !ava::plugin::validate_plugin_ui_action(*confirm, ack),
           "mismatched and out-of-order UI actions fail closed");
  }
}

void test_plugin_ui_protocol_exact_limits()
{
  expect(parse_ui_once(status_record(std::string(96, 'i'), std::string(256, 't'))).has_value(), "UI id and status text exact maxima are accepted");
  expect(!parse_ui_once(status_record(std::string(97, 'i'), "ok")), "UI ids above 96 bytes are rejected");
  expect(!parse_ui_once(status_record("status", std::string(257, 't'))), "status text above 256 bytes is rejected");
  auto exact_widget_lines = std::vector<std::string>(7, std::string(256, 'l'));
  exact_widget_lines.push_back(std::string(255, 'l'));
  expect(parse_ui_once(widget_record("widget", "t", exact_widget_lines)).has_value(), "widget exact eight-line and title-plus-lines 2 KiB maxima are accepted");
  expect(!parse_ui_once(widget_record("widget", "title", std::vector<std::string>(9, "line"))), "a ninth widget line is rejected before materialization");
  exact_widget_lines.back().push_back('x');
  expect(!parse_ui_once(widget_record("widget", "t", exact_widget_lines)), "widget title-plus-lines aggregate text overflow is rejected");
  expect(!parse_ui_once(widget_record("widget", std::string(257, 't'), {"line"})) && !parse_ui_once(widget_record("widget", "title", {std::string(257, 'l')})),
         "widget title and line component limits reject 256 bytes plus one");
  expect(parse_ui_once(select_record("select", "title", "description", 32)).has_value(), "select accepts exactly 32 choices");
  expect(!parse_ui_once(select_record("select", "title", "description", 33)), "select rejects a 33rd choice before vector allocation");

  auto modal_at_limit = select_record("m", "t", "", 32, 96, 159, 30);
  auto modal_over_limit = select_record("m", "t", "", 32, 96, 159, 31);
  expect(parse_ui_once(modal_at_limit).has_value(), "select accepts an exact 8 KiB validated modal payload");
  expect(!parse_ui_once(modal_over_limit), "select rejects an 8 KiB plus one validated modal payload");

  auto record_at_limit = status_record("record", "ok");
  record_at_limit.append(64 * 1024 - record_at_limit.size(), ' ');
  auto record_over_limit = record_at_limit + ' ';
  expect(parse_ui_once(record_at_limit).has_value(), "UI parser accepts an exact 64 KiB protocol record");
  expect(!parse_ui_once(record_over_limit), "UI parser rejects a 64 KiB plus one protocol record");
}

void test_plugin_ui_protocol_rejects_terminal_controls_and_malformed_input()
{
  std::vector<std::string> const unsafe_records{
      "{\"id\":\"x\",\"type\":\"ui.status\",\"text\":\"line\\nnext\"}",
      "{\"id\":\"x\",\"type\":\"ui.status\",\"text\":\"line\\rnext\"}",
      "{\"id\":\"x\",\"type\":\"ui.status\",\"text\":\"\\u001b[31mred\"}",
      "{\"id\":\"x\",\"type\":\"ui.status\",\"text\":\"\\u001b]0;title\\u0007\"}",
      "{\"id\":\"x\",\"type\":\"ui.status\",\"text\":\"\\u001bPpayload\\u001b\\\\\"}",
      "{\"id\":\"x\",\"type\":\"ui.status\",\"text\":\"\\u009b31mC1-CSI\"}",
      "{\"id\":\"x\",\"type\":\"ui.status\",\"text\":\"\\u061carabic-mark\"}",
      "{\"id\":\"x\",\"type\":\"ui.status\",\"text\":\"\\u200edirection-mark\"}",
      "{\"id\":\"x\",\"type\":\"ui.status\",\"text\":\"\\u202eoverride\"}",
      "{\"id\":\"x\",\"type\":\"ui.status\",\"text\":\"\\u2066isolate\"}",
      "{\"id\":\"x\",\"type\":\"ui.status\",\"text\":\"\\u206adeprecated-bidi\"}",
      "{\"id\":\"bad id\",\"type\":\"ui.status\",\"text\":\"ok\"}",
      "{\"id\":\"_bad\",\"type\":\"ui.status\",\"text\":\"ok\"}",
      "{\"id\":\"é\",\"type\":\"ui.status\",\"text\":\"ok\"}",
      "{\"id\":\"x\",\"type\":\"ui.confirm\",\"title\":\"Confirm\",\"description\":\"d\",\"confirm_label\":\"Plugin says yes\"}",
      "{\"id\":\"x\",\"id\":\"y\",\"type\":\"ui.status\",\"text\":\"ok\"}",
      "{\"id\":\"x\",\"type\":\"ui.widget\",\"title\":\"t\",\"lines\":[\"ok\",7]}",
      "{\"id\":\"x\",\"type\":\"ui.select\",\"title\":\"t\",\"description\":\"d\",\"choices\":[{\"id\":\"same\",\"label\":\"a\"},{\"id\":\"same\",\"label\":"
      "\"b\"}]}",
      "{\"id\":\"x\",\"type\":\"ui.unknown\",\"text\":\"ok\"}",
      "{\"id\":\"x\",\"type\":\"ui.status\",\"text\":\"\\ud800\"}",
      "{\"id\":\"x\",\"type\":\"ui.status\",\"text\":\"\\udc00\"}",
  };
  for (auto const& record : unsafe_records) expect(!parse_ui_once(record), "unsafe or malformed plugin UI record is rejected");

  std::string invalid_utf8 = "{\"id\":\"x\",\"type\":\"ui.status\",\"text\":\"";
  invalid_utf8.push_back(static_cast<char>(0xC3));
  invalid_utf8.push_back('(');
  invalid_utf8 += "\"}";
  expect(!parse_ui_once(invalid_utf8), "invalid UTF-8 in plugin UI text is rejected");

  auto const depth_128 = "{\"x\":" + nested_arrays(127) + "}";
  auto const depth_129 = "{\"x\":" + nested_arrays(128) + "}";
  expect(ava::core::json::is_valid_object_with_max_depth(depth_128, 128) && !ava::core::json::is_valid_object_with_max_depth(depth_129, 128),
         "plugin-specific JSON validation accepts depth 128 and rejects depth 129 exactly");
  auto deep = "{\"id\":\"x\",\"type\":\"ui.status\",\"text\":\"ok\",\"future\":" + nested_arrays(129) + "}";
  expect(!parse_ui_once(deep), "plugin UI records deeper than 128 containers are rejected before recursive parsing");
}

void test_plugin_ui_protocol_state_limits_and_duplicates()
{
  ava::plugin::PluginUiProtocolState duplicate_state;
  expect(ava::plugin::parse_plugin_ui_request(status_record("duplicate", "one"), duplicate_state).has_value() &&
             !ava::plugin::parse_plugin_ui_request(status_record("duplicate", "two"), duplicate_state),
         "duplicate UI request ids fail closed within an invocation");

  expect(ava::plugin::kPluginUiRecordMaxCount == 64, "UI record flood defense remains exactly 64 records");

  ava::plugin::PluginUiProtocolState status_state;
  expect(ava::plugin::parse_plugin_ui_request(status_record("s1", "one"), status_state).has_value() &&
             !ava::plugin::parse_plugin_ui_request(status_record("s2", "two"), status_state),
         "status cap accepts one total and rejects a second status");

  ava::plugin::PluginUiProtocolState widget_state;
  expect(ava::plugin::parse_plugin_ui_request(widget_record("w1", "one", {"line"}), widget_state).has_value() &&
             ava::plugin::parse_plugin_ui_request(widget_record("w2", "two", {"line"}), widget_state).has_value() &&
             !ava::plugin::parse_plugin_ui_request(widget_record("w3", "three", {"line"}), widget_state),
         "widget cap accepts two and rejects a third widget");

  ava::plugin::PluginUiProtocolState modal_state;
  bool first_eight = true;
  for (std::size_t index = 0; index < 8; ++index)
  {
    auto record = "{\"id\":\"m" + std::to_string(index) + "\",\"type\":\"ui.confirm\",\"title\":\"Confirm\",\"description\":\"description\"}";
    first_eight = first_eight && ava::plugin::parse_plugin_ui_request(record, modal_state).has_value();
  }
  expect(first_eight && !ava::plugin::parse_plugin_ui_request("{\"id\":\"m8\",\"type\":\"ui.confirm\",\"title\":\"Confirm\",\"description\":\"description\"}",
                                                              modal_state),
         "sequential modal cap accepts eight and rejects the ninth modal");
}

ava::plugin::PluginUiHandler happy_runner_handler(std::chrono::steady_clock::time_point deadline, std::vector<std::string>& seen_types)
{
  return ava::plugin::PluginUiHandler{
      .deadline = deadline,
      .callback = [&seen_types](ava::plugin::PluginUiRequest const& request, std::chrono::steady_clock::time_point,
                                ava::plugin::CancelCallback cancel_requested) -> ava::core::Result<ava::plugin::PluginUiAction> {
        if (cancel_requested && cancel_requested())
          return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Tool, "canceled"));
        seen_types.emplace_back(ava::plugin::plugin_ui_request_type(request));
        if (std::holds_alternative<ava::plugin::PluginUiSelectRequest>(request))
          return ava::plugin::PluginUiAction{.action = ava::plugin::PluginUiActionKind::Select, .option_id = "second"};
        if (std::holds_alternative<ava::plugin::PluginUiConfirmRequest>(request))
          return ava::plugin::PluginUiAction{.action = ava::plugin::PluginUiActionKind::Confirm, .option_id = {}};
        return ava::plugin::PluginUiAction{.action = ava::plugin::PluginUiActionKind::Ack, .option_id = {}};
      }};
}

void test_plugin_runner_command_ui_happy_path()
{
  auto const root = create_empty_root("plugin-ui-runner-happy");
  auto const workspace = root / "workspace";
  auto const plugin_dir = root / "plugins" / "com.example.uihappy";
  std::filesystem::create_directories(workspace);
  auto manifest = load_ui_manifest(plugin_dir, "com.example.uihappy", "[\"commands\",\"ui.status\",\"ui.widget\",\"ui.select\",\"ui.confirm\"]");
  write_text(plugin_dir / "plugin.sh",
             "IFS= read -r initialize\n"
             "printf '%s\\n' '" +
                 initialized_record() +
                 "'\n"
                 "IFS= read -r command\n"
                 "printf '%s\\n' '{\"id\":\"status\",\"type\":\"ui.status\",\"text\":\"working\"}'\n"
                 "IFS= read -r action; printf '%s\\n' \"$action\" >> actions.txt\n"
                 "printf '%s\\n' '{\"id\":\"widget\",\"type\":\"ui.widget\",\"title\":\"summary\",\"lines\":[\"one\",\"two\"]}'\n"
                 "IFS= read -r action; printf '%s\\n' \"$action\" >> actions.txt\n"
                 "printf '%s\\n' "
                 "'{\"id\":\"select\",\"type\":\"ui.select\",\"title\":\"choose\",\"description\":\"one\",\"choices\":[{\"id\":\"first\",\"label\":\"First\"},{"
                 "\"id\":\"second\",\"label\":\"Second\"}]}'\n"
                 "IFS= read -r action; printf '%s\\n' \"$action\" >> actions.txt\n"
                 "printf '%s\\n' '{\"id\":\"confirm\",\"type\":\"ui.confirm\",\"title\":\"continue\",\"description\":\"apply\"}'\n"
                 "IFS= read -r action; printf '%s\\n' \"$action\" >> actions.txt\n"
                 "printf '%s\\n' '{\"id\":\"ava_command_ui_call\",\"type\":\"command.result\",\"ok\":true,\"content\":\"done\",\"metadata\":{}}'\n"
                 "while IFS= read -r line; do :; done\n");

  auto process = ava::plugin::PluginProcess::start(std::move(manifest), ui_runner_options(workspace));
  expect(process.has_value(), process ? "UI runner fixture initializes" : "UI runner fixture initializes: " + process.error().format());
  if (!process)
    return;
  std::vector<std::string> seen_types;
  auto result = (*process)->call_command("run", "{}", "ui_call", nullptr, nullptr,
                                         happy_runner_handler(std::chrono::steady_clock::now() + std::chrono::seconds(2), seen_types));
  auto shutdown = (*process)->shutdown();
  auto const actions = read_text(plugin_dir / "actions.txt");
  expect(result && result->ok && result->content == "done" && shutdown.has_value(), "direct command completes after synchronous plugin UI exchanges");
  expect(seen_types == std::vector<std::string>({"ui.status", "ui.widget", "ui.select", "ui.confirm"}),
         "runner dispatches each validated plugin UI DTO in protocol order");
  expect(actions.find("\"action\":\"ack\"") != std::string::npos && actions.find("\"action\":\"select\"") != std::string::npos &&
             actions.find("\"option_id\":\"second\"") != std::string::npos && actions.find("\"action\":\"confirm\"") != std::string::npos,
         "runner returns exact validated host actions through strict framing");
}

ava::core::Result<ava::plugin::PluginCommandCallResult> run_single_ui_fixture(std::filesystem::path const& root, std::string_view id,
                                                                              std::string_view capabilities, ava::plugin::PluginUiHandler handler = {},
                                                                              ava::plugin::CancelCallback cancel_requested = {})
{
  auto const workspace = root / "workspace";
  auto const plugin_dir = root / "plugins" / std::string(id);
  std::filesystem::create_directories(workspace);
  auto manifest = load_ui_manifest(plugin_dir, id, capabilities);
  write_text(plugin_dir / "plugin.sh",
             "IFS= read -r initialize\n"
             "printf '%s\\n' '" +
                 initialized_record() +
                 "'\n"
                 "IFS= read -r command\n"
                 "printf '%s\\n' '{\"id\":\"request\",\"type\":\"ui.status\",\"text\":\"RAW_UI_CANARY_6f21\"}'\n"
                 "IFS= read -r action\n"
                 "printf '%s\\n' '{\"id\":\"ava_command_call\",\"type\":\"command.result\",\"ok\":true,\"content\":\"done\"}'\n");
  auto process = ava::plugin::PluginProcess::start(std::move(manifest), ui_runner_options(workspace));
  if (!process)
    return std::unexpected(std::move(process.error()));
  return (*process)->call_command("run", "{}", "call", std::move(cancel_requested), nullptr, std::move(handler));
}

void test_plugin_runner_rejects_unauthorized_or_invalid_ui_safely()
{
  bool called = false;
  auto make_handler = [&](ava::plugin::PluginUiAction action = {.action = ava::plugin::PluginUiActionKind::Ack, .option_id = {}}) {
    return ava::plugin::PluginUiHandler{.deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2),
                                        .callback = [&, action](ava::plugin::PluginUiRequest const&, std::chrono::steady_clock::time_point,
                                                                ava::plugin::CancelCallback) -> ava::core::Result<ava::plugin::PluginUiAction> {
                                          called = true;
                                          return action;
                                        }};
  };

  auto missing_cap = run_single_ui_fixture(create_empty_root("plugin-ui-missing-cap"), "com.example.missingcap", "[\"commands\"]", make_handler());
  expect(!missing_cap && !called && missing_cap.error().format().find("RAW_UI_CANARY_6f21") == std::string::npos,
         "runner requires the exact manifest capability before invoking a UI handler and redacts raw UI bytes");

  called = false;
  auto wrong_cap = run_single_ui_fixture(create_empty_root("plugin-ui-wrong-cap"), "com.example.wrongcap", "[\"commands\",\"ui.widget\"]", make_handler());
  expect(!wrong_cap && !called, "each plugin UI record requires its exact independent manifest capability");

  called = false;
  auto no_handler = run_single_ui_fixture(create_empty_root("plugin-ui-no-handler"), "com.example.nohandler", "[\"commands\",\"ui.status\"]");
  expect(!no_handler && !called && no_handler.error().format().find("RAW_UI_CANARY_6f21") == std::string::npos,
         "ordinary direct commands cannot present plugin UI without an authority handler");

  called = false;
  auto bad_action = run_single_ui_fixture(create_empty_root("plugin-ui-bad-action"), "com.example.badaction", "[\"commands\",\"ui.status\"]",
                                          make_handler({.action = ava::plugin::PluginUiActionKind::Cancel, .option_id = {}}));
  expect(!bad_action && called && bad_action.error().message().find("invalid action") != std::string::npos,
         "runner rejects handler replies that do not exactly match the request kind");
}

void test_non_command_runner_paths_cannot_present_ui()
{
  auto run_path = [&](std::string_view suffix, auto&& invoke) {
    auto const root = create_empty_root("plugin-ui-non-command-" + std::string(suffix));
    auto const workspace = root / "workspace";
    auto const plugin_dir = root / "plugins" / ("com.example." + std::string(suffix));
    std::filesystem::create_directories(workspace);
    auto manifest = load_ui_manifest(plugin_dir, "com.example." + std::string(suffix), "[\"tools\",\"event_hooks\",\"dynamic.prompts\",\"ui.status\"]");
    write_text(plugin_dir / "plugin.sh",
               "IFS= read -r initialize\n"
               "printf '%s\\n' '" +
                   initialized_record() +
                   "'\n"
                   "IFS= read -r request\n"
                   "printf '%s\\n' '{\"id\":\"blocked\",\"type\":\"ui.status\",\"text\":\"NON_COMMAND_RAW_CANARY_81ac\"}'\n"
                   "while IFS= read -r line; do :; done\n");
    auto process = ava::plugin::PluginProcess::start(std::move(manifest), ui_runner_options(workspace));
    expect(process.has_value(), "non-command UI rejection fixture starts");
    if (!process)
      return;
    auto error = invoke(**process);
    expect(!error.empty() && error.find("NON_COMMAND_RAW_CANARY_81ac") == std::string::npos,
           "non-command plugin runner path rejects UI records without exposing raw plugin bytes");
  };

  run_path("toolpath", [](ava::plugin::PluginProcess& process) {
    auto result = process.call_tool("tool", "{}", "call");
    return result ? std::string{} : result.error().format();
  });
  run_path("eventpath", [](ava::plugin::PluginProcess& process) {
    auto result = process.observe_event("tool.result", "{}", "call");
    return result ? std::string{} : result.error().format();
  });
  run_path("resourcepath", [](ava::plugin::PluginProcess& process) {
    auto result = process.list_resources(ava::plugin::PluginDynamicResourceKind::Prompt);
    return result ? std::string{} : result.error().format();
  });
}

void test_plugin_runner_ui_deadline_and_handler_failure_cleanup()
{
  auto const root = create_empty_root("plugin-ui-cleanup");
  auto const workspace = root / "workspace";
  auto const plugin_dir = root / "plugins" / "com.example.cleanup";
  auto const pgid_file = plugin_dir / "pgid.txt";
  std::filesystem::create_directories(workspace);
  auto manifest = load_ui_manifest(plugin_dir, "com.example.cleanup", "[\"commands\",\"ui.status\"]");
  write_text(plugin_dir / "plugin.sh", "printf '%s\\n' $$ > " + shell_single_quote(pgid_file.generic_string()) + "\nIFS= read -r initialize\nprintf '%s\\n' '" +
                                           initialized_record() +
                                           "'\nIFS= read -r command\nsleep 30 &\nprintf '%s\\n' "
                                           "'{\"id\":\"cleanup\",\"type\":\"ui.status\",\"text\":\"cleanup\"}'\nIFS= read -r action\n");
  auto process = ava::plugin::PluginProcess::start(std::move(manifest), ui_runner_options(workspace));
  expect(process.has_value(), "UI cleanup fixture starts");
  if (!process)
    return;
  auto result = (*process)->call_command(
      "run", "{}", "cleanup", nullptr, nullptr,
      ava::plugin::PluginUiHandler{.deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2),
                                   .callback = [](ava::plugin::PluginUiRequest const&, std::chrono::steady_clock::time_point,
                                                  ava::plugin::CancelCallback) -> ava::core::Result<ava::plugin::PluginUiAction> {
                                     return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Tool, "RAW_HANDLER_ERROR_CANARY_1a9d"));
                                   }});
  std::ifstream pgid_input(pgid_file);
  pid_t pgid = 0;
  pgid_input >> pgid;
  bool group_gone = false;
  if (pgid > 0)
  {
    for (int attempt = 0; attempt < 100 && !group_gone; ++attempt)
    {
      errno = 0;
      group_gone = ::kill(-pgid, 0) != 0 && errno == ESRCH;
      if (!group_gone)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
  }
  expect(!result && result.error().format().find("RAW_HANDLER_ERROR_CANARY_1a9d") == std::string::npos && pgid > 0 && group_gone,
         "UI handler failures are generic and terminate the owned plugin process group");

  auto const deadline_root = create_empty_root("plugin-ui-deadline-bound");
  auto too_long = run_single_ui_fixture(
      deadline_root, "com.example.deadline", "[\"commands\",\"ui.status\"]",
      ava::plugin::PluginUiHandler{.deadline = std::chrono::steady_clock::now() + ava::plugin::kPluginUiCommandDeadlineMax + std::chrono::seconds(1),
                                   .callback = [](ava::plugin::PluginUiRequest const&, std::chrono::steady_clock::time_point,
                                                  ava::plugin::CancelCallback) -> ava::core::Result<ava::plugin::PluginUiAction> {
                                     return ava::plugin::PluginUiAction{.action = ava::plugin::PluginUiActionKind::Ack, .option_id = {}};
                                   }});
  expect(!too_long && too_long.error().message().find("deadline") != std::string::npos, "runner rejects UI command deadlines above 120 seconds");
  expect(ava::plugin::PluginRunnerOptions{}.request_timeout == std::chrono::seconds(5), "ordinary plugin command timeout remains five seconds");

  bool canceled = false;
  auto canceled_result = run_single_ui_fixture(
      create_empty_root("plugin-ui-canceled"), "com.example.canceled", "[\"commands\",\"ui.status\"]",
      ava::plugin::PluginUiHandler{.deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2),
                                   .callback = [&canceled](ava::plugin::PluginUiRequest const&, std::chrono::steady_clock::time_point,
                                                           ava::plugin::CancelCallback) -> ava::core::Result<ava::plugin::PluginUiAction> {
                                     canceled = true;
                                     return ava::plugin::PluginUiAction{.action = ava::plugin::PluginUiActionKind::Ack, .option_id = {}};
                                   }},
      [&canceled] { return canceled; });
  expect(!canceled_result && canceled_result.error().message().find("canceled") != std::string::npos,
         "command cancellation after presentation terminates the plugin before a UI action is written");

  auto const active_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
  bool deadline_observed = false;
  auto deadline_result = run_single_ui_fixture(
      create_empty_root("plugin-ui-active-deadline"), "com.example.activedeadline", "[\"commands\",\"ui.status\"]",
      ava::plugin::PluginUiHandler{
          .deadline = active_deadline,
          .callback = [&deadline_observed](ava::plugin::PluginUiRequest const&, std::chrono::steady_clock::time_point,
                                           ava::plugin::CancelCallback cancel_requested) -> ava::core::Result<ava::plugin::PluginUiAction> {
            while (!cancel_requested()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
            deadline_observed = true;
            return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Tool, "deadline"));
          }});
  expect(!deadline_result && deadline_observed && deadline_result.error().message().find("timed out") != std::string::npos,
         "one absolute UI command deadline cancels a synchronous presenter and terminates the child");

  auto const exit_root = create_empty_root("plugin-ui-child-exit");
  auto const exit_workspace = exit_root / "workspace";
  auto const exit_plugin_dir = exit_root / "plugins" / "com.example.childexit";
  std::filesystem::create_directories(exit_workspace);
  auto exit_manifest = load_ui_manifest(exit_plugin_dir, "com.example.childexit", "[\"commands\",\"ui.status\"]");
  write_text(exit_plugin_dir / "plugin.sh",
             "IFS= read -r initialize\nprintf '%s\\n' '" + initialized_record() +
                 "'\nIFS= read -r command\nprintf '%s\\n' '{\"id\":\"exit\",\"type\":\"ui.status\",\"text\":\"leaving\"}'\nexit 0\n");
  auto exit_process = ava::plugin::PluginProcess::start(std::move(exit_manifest), ui_runner_options(exit_workspace));
  expect(exit_process.has_value(), "child-exit UI fixture starts");
  if (exit_process)
  {
    bool observed_exit = false;
    auto exit_result =
        (*exit_process)
            ->call_command("run", "{}", "exit", nullptr, nullptr,
                           ava::plugin::PluginUiHandler{
                               .deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2),
                               .callback = [&observed_exit](ava::plugin::PluginUiRequest const&, std::chrono::steady_clock::time_point,
                                                            ava::plugin::CancelCallback cancel_requested) -> ava::core::Result<ava::plugin::PluginUiAction> {
                                 while (!cancel_requested()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
                                 observed_exit = true;
                                 return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Tool, "child exited"));
                               }});
    expect(!exit_result && observed_exit, "plugin child exit invalidates the synchronous presenter cancellation callback and unblocks the host");
  }
}

void test_plugin_runner_ui_authority_alone_can_raise_command_timeout()
{
  auto run_case = [](std::string_view suffix, bool with_authority) {
    auto const root = create_empty_root("plugin-ui-raised-timeout-" + std::string(suffix));
    auto const workspace = root / "workspace";
    auto const plugin_dir = root / "plugins" / ("com.example.raised" + std::string(suffix));
    std::filesystem::create_directories(workspace);
    auto manifest = load_ui_manifest(plugin_dir, "com.example.raised" + std::string(suffix), "[\"commands\",\"ui.status\"]");
    write_text(plugin_dir / "plugin.sh",
               "IFS= read -r initialize\nprintf '%s\\n' '" + initialized_record() +
                   "'\nIFS= read -r command\n/bin/sleep 0.15\nprintf '%s\\n' '{\"id\":\"raised\",\"type\":\"ui.status\",\"text\":\"ready\"}'\n"
                   "IFS= read -r action\nprintf '%s\\n' '{\"id\":\"ava_command_raised\",\"type\":\"command.result\",\"ok\":true,\"content\":\"done\"}'\n");
    auto process = ava::plugin::PluginProcess::start(std::move(manifest), ui_runner_options(workspace, std::chrono::milliseconds(60)));
    if (!process)
      return ava::core::Result<ava::plugin::PluginCommandCallResult>(std::unexpected(std::move(process.error())));
    ava::plugin::PluginUiHandler handler;
    if (with_authority)
    {
      handler = ava::plugin::PluginUiHandler{.deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1),
                                             .callback = [](ava::plugin::PluginUiRequest const&, std::chrono::steady_clock::time_point,
                                                            ava::plugin::CancelCallback) -> ava::core::Result<ava::plugin::PluginUiAction> {
                                               return ava::plugin::PluginUiAction{.action = ava::plugin::PluginUiActionKind::Ack, .option_id = {}};
                                             }};
    }
    return (*process)->call_command("run", "{}", "raised", nullptr, nullptr, std::move(handler));
  };

  auto capable = run_case("capable", true);
  auto ordinary = run_case("ordinary", false);
  bool const correct = capable && capable->ok && !ordinary && ordinary.error().message().find("timed out") != std::string::npos;
  expect(correct, "only an explicit UI authority raises the direct command request deadline; ordinary command timeout remains unchanged" +
                      std::string(correct ? ""
                                          : " (capable=" + (capable ? std::string("ok") : capable.error().format()) +
                                                ", ordinary=" + (ordinary ? std::string("ok") : ordinary.error().format()) + ")"));
}

struct CapabilityFixture
{
  std::shared_ptr<int> runtime = std::make_shared<int>(1);
  std::vector<ava::app::PluginUiPresentationRequest> presented;
  std::vector<ava::app::PluginUiInvocationBinding> closed;

  ava::app::PluginUiPresenter presenter()
  {
    return [this](ava::app::PluginUiPresentationRequest const& request, std::chrono::steady_clock::time_point,
                  ava::plugin::CancelCallback) -> ava::core::Result<ava::plugin::PluginUiAction> {
      presented.push_back(request);
      return ava::plugin::PluginUiAction{.action = ava::plugin::PluginUiActionKind::Ack, .option_id = {}};
    };
  }

  ava::app::PluginUiPresenterClose closer()
  {
    return [this](ava::app::PluginUiInvocationBinding const& binding) { closed.push_back(binding); };
  }
};

ava::core::Result<std::shared_ptr<ava::app::PluginUiInvocationCapability>> make_capability(
    CapabilityFixture& fixture, std::string command = "/plugin run com.example.ui run {}", std::string invocation_id = "invocation_1",
    std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2))
{
  return ava::app::make_tui_plugin_ui_invocation_capability(std::move(command), std::move(invocation_id), deadline, fixture.runtime, fixture.presenter(),
                                                            fixture.closer());
}

void test_plugin_ui_capability_claim_binding_and_lifecycle()
{
  CapabilityFixture fixture;
  auto capability = make_capability(fixture);
  expect(capability.has_value(), "TUI factory mints a capability from an exact canonical direct plugin command");
  if (!capability)
    return;
  {
    auto claim = ava::app::claim_plugin_ui_invocation_capability(*capability, "/plugin run com.example.ui run {}", "com.example.ui", "run");
    expect(claim.has_value(), "exact plugin id, command, canonical command, runtime, and deadline claim succeeds once");
    if (claim)
    {
      ava::plugin::PluginUiStatusRequest status{.id = "status", .text = "working"};
      auto handler = claim->handler();
      auto action = handler.callback(ava::plugin::PluginUiRequest(status), handler.deadline, [] { return false; });
      expect(action && action->action == ava::plugin::PluginUiActionKind::Ack && fixture.presented.size() == 1 &&
                 fixture.presented.front().binding.plugin_id == "com.example.ui" && fixture.presented.front().binding.command_name == "run" &&
                 fixture.presented.front().binding.invocation_id == "invocation_1" &&
                 std::get<ava::plugin::PluginUiStatusRequest>(fixture.presented.front().request).text == "working",
             "presenter receives exact canonical binding metadata and only a validated DTO");
      auto second = ava::app::claim_plugin_ui_invocation_capability(*capability, "/plugin run com.example.ui run {}", "com.example.ui", "run");
      expect(!second, "capability enforces a strict one-claim rule");
    }
  }
  expect(fixture.closed.size() == 1 && fixture.closed.front().invocation_id == "invocation_1", "claimed capability closes exactly once on completion");

  CapabilityFixture guarded_fixture;
  auto guarded_capability = make_capability(guarded_fixture, "/plugin run com.example.ui run {\"value\":1}", "guarded");
  if (guarded_capability)
  {
    {
      ava::app::PluginUiInvocationGuard const guard(*guarded_capability);
    }
    auto claim = ava::app::claim_plugin_ui_invocation_capability(*guarded_capability, "/plugin run com.example.ui run {\"value\":1}", "com.example.ui", "run");
    expect(!claim && guarded_fixture.closed.size() == 1, "unclaimed command authority guard closes pre-claim failure paths and prevents reuse");
  }
  else
  {
    expect(false, "TUI factory accepts canonical direct plugin commands with bounded JSON object arguments");
  }

  CapabilityFixture invalid_request_fixture;
  auto invalid_request_capability = make_capability(invalid_request_fixture, "/plugin run com.example.ui run", "invalid_request");
  auto invalid_request_claim =
      invalid_request_capability
          ? ava::app::claim_plugin_ui_invocation_capability(*invalid_request_capability, "/plugin run com.example.ui run", "com.example.ui", "run")
          : ava::core::Result<ava::app::PluginUiInvocationClaim>(std::unexpected(ava::core::Error(ava::core::ErrorCategory::Unknown, "fixture failed")));
  if (invalid_request_claim)
  {
    std::string overlong_utf8{static_cast<char>(0xC1), static_cast<char>(0x81)};
    auto handler = invalid_request_claim->handler();
    auto action = handler.callback(ava::plugin::PluginUiRequest(ava::plugin::PluginUiStatusRequest{.id = "status", .text = std::move(overlong_utf8)}),
                                   handler.deadline, [] { return false; });
    expect(!action && invalid_request_fixture.presented.empty() && invalid_request_fixture.closed.size() == 1,
           "authority revalidates constructed DTOs so presenters cannot receive invalid UTF-8 or terminal text");
  }
  else
  {
    expect(false, "invalid constructed DTO authority fixture is claimed");
  }

  auto expect_mismatch = [](std::string_view label, std::string_view canonical, std::string_view plugin_id, std::string_view command_name) {
    CapabilityFixture mismatch_fixture;
    auto mismatch_capability = make_capability(mismatch_fixture);
    auto claim =
        mismatch_capability
            ? ava::app::claim_plugin_ui_invocation_capability(*mismatch_capability, canonical, plugin_id, command_name)
            : ava::core::Result<ava::app::PluginUiInvocationClaim>(std::unexpected(ava::core::Error(ava::core::ErrorCategory::Unknown, "fixture failed")));
    expect(!claim && claim.error().message() == "plugin UI capability is unavailable" && mismatch_fixture.closed.size() == 1,
           std::string(label) + " binding mismatch fails generically and consumes capability");
  };
  expect_mismatch("canonical command", "/plugin run com.example.ui run", "com.example.ui", "run");
  expect_mismatch("plugin id", "/plugin run com.example.ui run {}", "com.example.other", "run");
  expect_mismatch("command name", "/plugin run com.example.ui run {}", "com.example.ui", "other");
}

void test_plugin_ui_capability_factory_validation_expiry_and_runtime_loss()
{
  CapabilityFixture fixture;
  std::vector<std::string> const invalid_commands{" /plugin run com.example.ui run {}", "/plugin  run com.example.ui run {}",
                                                  "/plugin run com.example.ui  run {}", "/plugin run com.example.ui run []",
                                                  "/plugin run Com.Example.ui run {}",  "/plugins run com.example.ui run {}"};
  for (auto const& command : invalid_commands)
  {
    auto capability = make_capability(fixture, command);
    expect(!capability, "TUI factory rejects noncanonical or non-direct plugin commands");
  }
  auto too_long = make_capability(fixture, "/plugin run com.example.ui run {}", "invocation",
                                  std::chrono::steady_clock::now() + ava::plugin::kPluginUiCommandDeadlineMax + std::chrono::seconds(1));
  expect(!too_long, "TUI factory rejects deadlines above 120 seconds");
  auto near_max = make_capability(fixture, "/plugin run com.example.ui run {}", "invocation", std::chrono::steady_clock::now() + std::chrono::seconds(119));
  expect(near_max.has_value(), "TUI factory accepts a deadline below the exact 120-second maximum");
  if (near_max)
    near_max->reset();

  CapabilityFixture expired_fixture;
  auto expiry = std::chrono::steady_clock::now() + std::chrono::milliseconds(50);
  auto expired_capability = make_capability(expired_fixture, "/plugin run com.example.ui run {}", "expiry", expiry);
  while (std::chrono::steady_clock::now() <= expiry) std::this_thread::sleep_for(std::chrono::milliseconds(1));
  auto expired_claim =
      expired_capability
          ? ava::app::claim_plugin_ui_invocation_capability(*expired_capability, "/plugin run com.example.ui run {}", "com.example.ui", "run")
          : ava::core::Result<ava::app::PluginUiInvocationClaim>(std::unexpected(ava::core::Error(ava::core::ErrorCategory::Unknown, "fixture failed")));
  expect(!expired_claim && expired_fixture.closed.size() == 1, "expired capability claim fails generically and closes once");

  CapabilityFixture dead_fixture;
  auto dead_capability = make_capability(dead_fixture, "/plugin run com.example.ui run {}", "dead");
  dead_fixture.runtime.reset();
  auto dead_claim =
      dead_capability
          ? ava::app::claim_plugin_ui_invocation_capability(*dead_capability, "/plugin run com.example.ui run {}", "com.example.ui", "run")
          : ava::core::Result<ava::app::PluginUiInvocationClaim>(std::unexpected(ava::core::Error(ava::core::ErrorCategory::Unknown, "fixture failed")));
  expect(!dead_claim && dead_fixture.closed.size() == 1, "runtime-dead capability claim fails generically and closes once");

  CapabilityFixture loss_fixture;
  auto loss_capability = make_capability(loss_fixture, "/plugin run com.example.ui run {}", "loss");
  auto loss_claim =
      loss_capability
          ? ava::app::claim_plugin_ui_invocation_capability(*loss_capability, "/plugin run com.example.ui run {}", "com.example.ui", "run")
          : ava::core::Result<ava::app::PluginUiInvocationClaim>(std::unexpected(ava::core::Error(ava::core::ErrorCategory::Unknown, "fixture failed")));
  expect(loss_claim.has_value(), "live runtime can be claimed before runtime-loss test");
  if (loss_claim)
  {
    loss_fixture.runtime.reset();
    auto handler = loss_claim->handler();
    auto action = handler.callback(ava::plugin::PluginUiRequest(ava::plugin::PluginUiStatusRequest{.id = "status", .text = "working"}), handler.deadline,
                                   [] { return false; });
    expect(!action && action.error().message() == "plugin UI capability is unavailable" && loss_fixture.presented.empty(),
           "runtime loss after claim prevents presentation and returns a generic failure");
  }
  expect(loss_fixture.closed.size() == 1, "runtime loss closes the claimed capability exactly once");
}

void test_plugin_ui_capability_active_runtime_loss_unblocks_presenter()
{
  auto runtime = std::make_shared<int>(1);
  std::mutex mutex;
  std::condition_variable cv;
  bool entered = false;
  std::size_t closes = 0;
  auto capability = ava::app::make_tui_plugin_ui_invocation_capability(
      "/plugin run com.example.ui run", "active_loss", std::chrono::steady_clock::now() + std::chrono::seconds(2), runtime,
      [&](ava::app::PluginUiPresentationRequest const&, std::chrono::steady_clock::time_point,
          ava::plugin::CancelCallback cancel_requested) -> ava::core::Result<ava::plugin::PluginUiAction> {
        std::unique_lock lock(mutex);
        entered = true;
        cv.notify_all();
        while (!cancel_requested()) cv.wait_for(lock, std::chrono::milliseconds(5));
        return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Tool, "runtime closed"));
      },
      [&](ava::app::PluginUiInvocationBinding const&) { ++closes; });
  expect(capability.has_value(), "active runtime-loss fixture capability is created");
  if (!capability)
    return;
  auto claim = ava::app::claim_plugin_ui_invocation_capability(*capability, "/plugin run com.example.ui run", "com.example.ui", "run");
  expect(claim.has_value(), "active runtime-loss fixture capability is claimed");
  if (!claim)
    return;

  std::thread invalidator([&] {
    std::unique_lock lock(mutex);
    if (!cv.wait_for(lock, std::chrono::seconds(1), [&] { return entered; }))
      return;
    runtime.reset();
    cv.notify_all();
  });
  auto handler = claim->handler();
  auto action = handler.callback(ava::plugin::PluginUiRequest(ava::plugin::PluginUiStatusRequest{.id = "status", .text = "working"}), handler.deadline,
                                 [] { return false; });
  invalidator.join();
  expect(!action && action.error().message() == "plugin UI capability is unavailable" && closes == 1,
         "weak runtime loss is exposed through presenter cancellation and closes active authority exactly once");
}

void test_plugin_ui_permission_mapping_and_headless_denial()
{
  auto parsed = ava::permissions::parse_operation("plugin.ui.present");
  expect(parsed == ava::permissions::Operation::PluginUiPresent && ava::permissions::to_string(*parsed) == "plugin.ui.present",
         "plugin.ui.present round-trips through permission and audit mapping");
  auto const root = create_empty_root("plugin-ui-permission");
  auto const workspace = root / "workspace";
  auto const manifest = workspace / ".ava" / "plugins" / "com.example.ui" / "plugin.json";
  std::filesystem::create_directories(manifest.parent_path());
  auto decision = ava::permissions::decide(ava::permissions::PermissionRequest{.operation = ava::permissions::Operation::PluginUiPresent,
                                                                               .mode = ava::core::Mode::Build,
                                                                               .workspace_dir = workspace,
                                                                               .target_path = manifest,
                                                                               .command = "com.example.ui:run"});
  expect(decision.action == ava::permissions::PermissionAction::Ask && decision.risk == ava::permissions::PermissionRisk::High,
         "plugin UI presentation is an explicit high-risk permission decision");

  auto resolver = ava::app::build_headless_permission_resolver(
      ava::app::HeadlessPermissionPolicyOptions{.allow_read_only = true, .allowed_tools = {"plugin", "plugin_ui", "*"}});
  auto resolved = resolver(ava::permissions::PermissionPrompt{.permission_request_id = "perm_ui",
                                                              .operation = ava::permissions::Operation::PluginUiPresent,
                                                              .mode = ava::core::Mode::Build,
                                                              .workspace_dir = workspace,
                                                              .target_path = manifest,
                                                              .command = "com.example.ui:run",
                                                              .tool_name = "plugin_ui",
                                                              .reason = "plugin UI presentation requires explicit approval",
                                                              .risk = ava::permissions::PermissionRisk::High});
  expect(resolved && *resolved == ava::permissions::PermissionResolution::Deny,
         "headless policy cannot authorize plugin UI even with broad plugin tool allow entries");
}

ava::app::runtime::session_ts plugin_ui_test_session(ava::config::XdgPaths const& paths, std::filesystem::path const& workspace)
{
  auto trusted = ava::app::set_project_trust_decision(paths, workspace, true);
  expect(trusted.has_value(), "plugin UI integration workspace is trusted");
  ava::app::runtime::OpenContext context;
  context.workspace_dir = workspace;
  context.current_dir = workspace;
  context.paths = paths;
  context.application_process_scope = plugin_ui_test_process_scope();
  auto session = ava::app::runtime::Session::open(context, {.sessionless = true,
                                                            .requested_session_id = std::nullopt,
                                                            .fork_session_id = std::nullopt,
                                                            .initial_session_name = std::nullopt,
                                                            .continue_last_session = false,
                                                            .initial_reasoning_level = std::nullopt});
  expect(session.has_value(), session ? "plugin UI integration session opens" : "plugin UI integration session opens: " + session.error().format());
  return std::move(*session);
}

std::string app_ui_script(std::string_view ui_text, int request_count = 1)
{
  std::string script = "IFS= read -r initialize\nprintf '%s\\n' '" + initialized_record() +
                       "'\nIFS= read -r command\nrequest_id=$(printf '%s' \"$command\" | /bin/sed -n 's/.*\"id\":\"\\([^\"]*\\)\".*/\\1/p')\n";
  for (int index = 0; index < request_count; ++index)
  {
    if (index == 0)
    {
      script += "printf '%s\\n' '{\"id\":\"status0\",\"type\":\"ui.status\",\"text\":\"" + ava::core::json::escape(ui_text) + "\"}'\n";
    }
    else
    {
      script += "printf '%s\\n' '{\"id\":\"widget" + std::to_string(index) + "\",\"type\":\"ui.widget\",\"title\":\"" + ava::core::json::escape(ui_text) +
                "\",\"lines\":[\"line\"]}'\n";
    }
    script += "IFS= read -r action\nprintf '%s\\n' \"$action\" >> actions.txt\n";
  }
  script += "printf '{\"id\":\"%s\",\"type\":\"command.result\",\"ok\":true,\"content\":\"done\"}\\n' \"$request_id\"\n";
  return script;
}

void test_app_direct_command_claims_ui_and_caches_permission_once()
{
  auto const root = create_empty_root("plugin-ui-app-allow");
  auto const workspace = root / "workspace";
  auto const paths = ava::tests::app_test_paths(root);
  auto const plugin_dir = workspace / ".ava" / "plugins" / "com.example.ui";
  std::filesystem::create_directories(workspace);
  write_text(plugin_dir / "plugin.json", ui_manifest_json("com.example.ui", "[\"commands\",\"ui.status\",\"ui.widget\"]"));
  write_text(plugin_dir / "plugin.sh", app_ui_script("APP_UI_RAW_CANARY_0c42", 2));
  auto enabled =
      ava::plugin::set_plugin_enabled(paths.ava_state_dir / "plugin-enablement.json", workspace, "com.example.ui", true, ava::plugin::PluginScope::Project);
  expect(enabled.has_value(), "plugin UI integration plugin is enabled");
  auto session = plugin_ui_test_session(paths, workspace);

  CapabilityFixture fixture;
  auto capability = make_capability(fixture);
  std::vector<ava::permissions::PermissionPrompt> prompts;
  auto allow = [&prompts](ava::permissions::PermissionPrompt const& prompt) -> ava::core::Result<ava::permissions::PermissionResolutionDecision> {
    prompts.push_back(prompt);
    return ava::permissions::PermissionResolution::Allow;
  };
  auto result = ava::app::run_plugin_command(
      session, ava::app::CommandRequest{
                   .command = "/plugin run com.example.ui run {}", .permission_resolver = allow, .plugin_ui_capability = capability ? *capability : nullptr});
  auto const first_ui_prompts =
      std::ranges::count_if(prompts, [](auto const& prompt) { return prompt.operation == ava::permissions::Operation::PluginUiPresent; });
  expect(result && result->handled && !result->output.empty() && result->output.back().find("done") != std::string::npos && fixture.presented.size() == 2,
         "capable direct plugin command presents validated UI and completes");
  expect(first_ui_prompts == 1, "plugin.ui.present invocation authority is approved once before process launch and reused for all UI records");
  expect(fixture.closed.size() == 1, "direct command completion closes the claimed capability exactly once");

  CapabilityFixture second_fixture;
  auto second_capability = make_capability(second_fixture, "/plugin run com.example.ui run {}", "invocation_2");
  auto second_result =
      ava::app::run_plugin_command(session, ava::app::CommandRequest{.command = "/plugin run com.example.ui run {}",
                                                                     .permission_resolver = allow,
                                                                     .plugin_ui_capability = second_capability ? *second_capability : nullptr});
  auto const all_ui_prompts =
      std::ranges::count_if(prompts, [](auto const& prompt) { return prompt.operation == ava::permissions::Operation::PluginUiPresent; });
  expect(second_result && second_fixture.presented.size() == 2 && second_fixture.closed.size() == 1 && all_ui_prompts == 2,
         "plugin UI permission approval is cached within one invocation but not inferred across direct command invocations");
}

void test_app_ui_permission_deny_and_exact_binding_fail_closed()
{
  auto const run_case = [](std::string_view suffix, std::string_view manifest_capabilities, std::string capability_command, bool deny_ui,
                           bool deny_command = false, bool supply_capability = true, bool emit_ui = true) {
    auto const root = create_empty_root("plugin-ui-app-" + std::string(suffix));
    auto const workspace = root / "workspace";
    auto const paths = ava::tests::app_test_paths(root);
    auto const plugin_dir = workspace / ".ava" / "plugins" / "com.example.ui";
    std::filesystem::create_directories(workspace);
    write_text(plugin_dir / "plugin.json", ui_manifest_json("com.example.ui", manifest_capabilities));
    write_text(plugin_dir / "plugin.sh", "printf '%s\\n' executed > executed.txt\n" + app_ui_script("APP_DENY_RAW_CANARY_7b9e", emit_ui ? 1 : 0));
    auto enabled =
        ava::plugin::set_plugin_enabled(paths.ava_state_dir / "plugin-enablement.json", workspace, "com.example.ui", true, ava::plugin::PluginScope::Project);
    expect(enabled.has_value(), "plugin UI denial fixture is enabled");
    auto session = plugin_ui_test_session(paths, workspace);
    CapabilityFixture fixture;
    std::shared_ptr<ava::app::PluginUiInvocationCapability> capability;
    if (supply_capability)
    {
      auto minted = make_capability(fixture, std::move(capability_command));
      expect(minted.has_value(), "plugin UI denial fixture capability is created");
      if (minted)
        capability = std::move(*minted);
    }
    std::vector<ava::permissions::PermissionPrompt> prompts;
    auto resolver = [&prompts, deny_ui,
                     deny_command](ava::permissions::PermissionPrompt const& prompt) -> ava::core::Result<ava::permissions::PermissionResolutionDecision> {
      prompts.push_back(prompt);
      if ((deny_ui && prompt.operation == ava::permissions::Operation::PluginUiPresent) ||
          (deny_command && prompt.operation == ava::permissions::Operation::PluginCommandRun))
        return ava::permissions::PermissionResolution::Deny;
      return ava::permissions::PermissionResolution::Allow;
    };
    auto result = ava::app::run_plugin_command(
        session, ava::app::CommandRequest{.command = "/plugin run com.example.ui run {}", .permission_resolver = resolver, .plugin_ui_capability = capability});
    std::string output;
    if (result)
    {
      for (auto const& line : result->output) output += line;
    }
    auto authority = [&] {
      ava::app::runtime::session_ts::wat session_w(session);
      return session_w->read_authority_1();
    }();
    auto entries = [&]() -> ava::core::Result<std::vector<ava::session::SessionEntry>> {
      if (!authority)
        return std::unexpected(std::move(authority.error()));
      return authority->load();
    }();
    expect(entries.has_value(), "plugin UI denial fixture session remains readable and valid");
    bool const session_contains_raw_ui = entries && std::ranges::any_of(*entries, [](ava::session::SessionEntry const& entry) {
                                           return entry.data_json.find("APP_DENY_RAW_CANARY_7b9e") != std::string::npos;
                                         });
    return std::tuple{
        output, prompts, fixture.presented.size(), fixture.closed.size(), std::filesystem::exists(plugin_dir / "executed.txt"), session_contains_raw_ui};
  };

  auto [denied_output, denied_prompts, denied_presentations, denied_closes, denied_executed, denied_session_raw] =
      run_case("deny", "[\"commands\",\"ui.status\"]", "/plugin run com.example.ui run {}", true, false, true, false);
  expect(!denied_executed && denied_presentations == 0 && denied_closes == 1 && !denied_session_raw &&
             denied_output.find("APP_DENY_RAW_CANARY_7b9e") == std::string::npos &&
             std::ranges::count_if(denied_prompts, [](auto const& prompt) { return prompt.operation == ava::permissions::Operation::PluginUiPresent; }) == 1,
         "an unused declared UI capability still requires invocation-authority approval, and denial starts no child, never calls the presenter, and keeps "
         "raw UI bytes out of output and session history");

  auto [mismatch_output, mismatch_prompts, mismatch_presentations, mismatch_closes, mismatch_executed, mismatch_session_raw] =
      run_case("mismatch", "[\"commands\",\"ui.status\"]", "/plugin run com.example.other run {}", false);
  expect(!mismatch_executed && mismatch_presentations == 0 && mismatch_closes == 1 && !mismatch_session_raw &&
             mismatch_output.find("plugin UI capability is unavailable") != std::string::npos &&
             std::ranges::none_of(mismatch_prompts, [](auto const& prompt) { return prompt.operation == ava::permissions::Operation::PluginUiPresent; }),
         "exact binding mismatch consumes capability before process launch or UI permission");

  auto [old_output, old_prompts, old_presentations, old_closes, old_executed, old_session_raw] =
      run_case("old-manifest", "[\"commands\"]", "/plugin run com.example.ui run {}", false);
  expect(old_executed && old_presentations == 0 && old_closes == 1 && !old_session_raw && old_output.find("APP_DENY_RAW_CANARY_7b9e") == std::string::npos &&
             std::ranges::none_of(old_prompts, [](auto const& prompt) { return prompt.operation == ava::permissions::Operation::PluginUiPresent; }),
         "old manifest cannot use UI even when a direct-command capability is supplied");

  auto [ordinary_output, ordinary_prompts, ordinary_presentations, ordinary_closes, ordinary_executed, ordinary_session_raw] =
      run_case("ordinary-deny", "[\"commands\",\"ui.status\"]", "/plugin run com.example.ui run {}", false, true);
  expect(!ordinary_executed && ordinary_presentations == 0 && ordinary_closes == 1 && !ordinary_session_raw &&
             ordinary_output.find("APP_DENY_RAW_CANARY_7b9e") == std::string::npos &&
             std::ranges::none_of(ordinary_prompts, [](auto const& prompt) { return prompt.operation == ava::permissions::Operation::PluginUiPresent; }),
         "ordinary plugin permission denial occurs before claim but still consumes and closes one-command UI authority");

  auto [headless_output, headless_prompts, headless_presentations, headless_closes, headless_executed, headless_session_raw] =
      run_case("no-authority", "[\"commands\",\"ui.status\"]", {}, false, false, false);
  expect(headless_executed && headless_presentations == 0 && headless_closes == 0 && !headless_session_raw &&
             headless_output.find("APP_DENY_RAW_CANARY_7b9e") == std::string::npos &&
             std::ranges::none_of(headless_prompts, [](auto const& prompt) { return prompt.operation == ava::permissions::Operation::PluginUiPresent; }),
         "default-null headless, RPC, ACP, print, queued, and synthetic command requests cannot present UI or expose raw UI bytes");
}

void test_external_disable_revokes_active_ui_command()
{
  auto const root = create_empty_root("plugin-ui-external-disable");
  auto const workspace = root / "workspace";
  auto const paths = ava::tests::app_test_paths(root);
  auto const plugin_dir = workspace / ".ava" / "plugins" / "com.example.ui";
  std::filesystem::create_directories(workspace);
  write_text(plugin_dir / "plugin.json", ui_manifest_json("com.example.ui", "[\"commands\",\"ui.confirm\"]"));
  write_text(plugin_dir / "plugin.sh",
             "printf '%s\\n' $$ > child.pid\n"
             "IFS= read -r initialize\n"
             "printf '%s\\n' '" +
                 initialized_record() +
                 "'\n"
                 "IFS= read -r command\n"
                 "printf '%s\\n' '{\"id\":\"DISABLE_RAW_CANARY_51ac\",\"type\":\"ui.confirm\",\"title\":\"External disable modal\","
                 "\"description\":\"wait for external disable\"}'\n"
                 "IFS= read -r action\n"
                 "printf '%s\\n' \"{\\\"id\\\":\\\"ava_command_cmd_unused\\\",\\\"type\\\":\\\"command.result\\\",\\\"ok\\\":true,"
                 "\\\"content\\\":\\\"unexpected completion\\\"}\"\n");
  auto enabled =
      ava::plugin::set_plugin_enabled(paths.ava_state_dir / "plugin-enablement.json", workspace, "com.example.ui", true, ava::plugin::PluginScope::Project);
  expect(enabled.has_value(), "external-disable fixture is initially enabled");
  auto session = plugin_ui_test_session(paths, workspace);

  ava::tui::RuntimePluginUiCoordinator coordinator;
  ava::tui::ComposerSnapshot snapshot;
  snapshot.width = 80;
  snapshot.height = 12;
  auto const artificial_start = std::chrono::steady_clock::now() - ava::tui::kTuiPluginUiInvocationDeadline + std::chrono::seconds(3);
  auto endpoint = coordinator.begin_submission("/plugin run com.example.ui run {}", "disable_invocation", artificial_start);
  expect(endpoint.has_value(), "external-disable fixture arms the real TUI coordinator with a finite test deadline");
  if (!endpoint)
    return;

  std::mutex mutex;
  bool presenter_observed_cancel = false;
  std::size_t closes = 0;
  auto capability = ava::app::make_tui_plugin_ui_invocation_capability(
      "/plugin run com.example.ui run {}", "disable_invocation", endpoint->deadline, endpoint->runtime_token,
      [endpoint = *endpoint, &mutex, &presenter_observed_cancel](
          ava::app::PluginUiPresentationRequest const& presentation, std::chrono::steady_clock::time_point deadline,
          ava::plugin::CancelCallback cancel_requested) -> ava::core::Result<ava::plugin::PluginUiAction> {
        auto const* confirm = std::get_if<ava::plugin::PluginUiConfirmRequest>(&presentation.request);
        if (!confirm)
          return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Tool, "unexpected UI request"));
        auto const binding = ava::tui::TuiPluginUiBinding{
            .plugin_id = presentation.binding.plugin_id, .command = presentation.binding.command_name, .invocation_id = presentation.binding.invocation_id};
        auto request = ava::tui::TuiPluginUiRequest{.binding = binding,
                                                    .request_id = confirm->id,
                                                    .kind = ava::tui::TuiPluginUiKind::Confirm,
                                                    .text = {},
                                                    .title = confirm->title,
                                                    .description = confirm->description,
                                                    .lines = {},
                                                    .options = {}};
        auto observe_cancel = cancel_requested;
        auto const reply = endpoint.present(request, deadline, std::move(cancel_requested));
        bool canceled = false;
        try
        {
          canceled = observe_cancel && observe_cancel();
        }
        catch (...)
        {
          canceled = true;
        }
        {
          std::lock_guard lock(mutex);
          presenter_observed_cancel = canceled;
        }
        return ava::plugin::PluginUiAction{.action = reply.action == ava::tui::TuiPluginUiReplyKind::Confirm ? ava::plugin::PluginUiActionKind::Confirm
                                                                                                             : ava::plugin::PluginUiActionKind::Cancel,
                                           .option_id = {}};
      },
      [endpoint = *endpoint, &mutex, &closes](ava::app::PluginUiInvocationBinding const& binding) {
        endpoint.close(ava::tui::TuiPluginUiBinding{.plugin_id = binding.plugin_id, .command = binding.command_name, .invocation_id = binding.invocation_id});
        std::lock_guard lock(mutex);
        ++closes;
      });
  expect(capability.has_value(), "external-disable fixture creates exact foreground TUI authority");
  if (!capability)
    return;

  auto allow = [](ava::permissions::PermissionPrompt const&) -> ava::core::Result<ava::permissions::PermissionResolutionDecision> {
    return ava::permissions::PermissionResolution::Allow;
  };
  auto result = std::async(std::launch::async, [&] {
    return ava::app::run_plugin_command(
        session, ava::app::CommandRequest{.command = "/plugin run com.example.ui run {}", .permission_resolver = allow, .plugin_ui_capability = *capability});
  });
  bool modal_opened = false;
  auto const open_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
  while (!modal_opened && result.wait_for(std::chrono::milliseconds::zero()) != std::future_status::ready && std::chrono::steady_clock::now() < open_deadline)
  {
    static_cast<void>(coordinator.poll(snapshot, false));
    modal_opened = snapshot.plugin_ui_modal.has_value();
    if (!modal_opened)
      std::this_thread::yield();
  }
  std::string rendered_before_disable;
  if (modal_opened)
  {
    for (auto const& line : ava::tui::render_composer(snapshot)) rendered_before_disable += strip_sgr(line) + '\n';
  }

  auto const disabled_at = std::chrono::steady_clock::now();
  auto disabled =
      ava::plugin::set_plugin_enabled(paths.ava_state_dir / "plugin-enablement.json", workspace, "com.example.ui", false, ava::plugin::PluginScope::Project);
  auto const finished = result.wait_for(std::chrono::seconds(2)) == std::future_status::ready;
  auto command_result = result.get();
  auto const revoke_elapsed = std::chrono::steady_clock::now() - disabled_at;
  static_cast<void>(coordinator.poll(snapshot, false));

  std::string output;
  if (command_result)
  {
    for (auto const& line : command_result->output) output += line;
  }
  bool child_gone = false;
  auto const pid_text = read_text(plugin_dir / "child.pid");
  if (!pid_text.empty())
  {
    auto const child_pid = static_cast<pid_t>(std::stol(pid_text));
    errno = 0;
    child_gone = ::kill(child_pid, 0) < 0 && errno == ESRCH;
  }
  auto current_enabled =
      ava::plugin::plugin_enabled(paths.ava_state_dir / "plugin-enablement.json", workspace, "com.example.ui", ava::plugin::PluginScope::Project);
  auto authority = [&] {
    ava::app::runtime::session_ts::wat session_w(session);
    return session_w->read_authority_1();
  }();
  auto entries = authority ? authority->load() : ava::core::Result<std::vector<ava::session::SessionEntry>>(std::unexpected(std::move(authority.error())));
  bool const session_has_canary =
      entries && std::ranges::any_of(*entries, [](auto const& entry) { return entry.data_json.find("DISABLE_RAW_CANARY_51ac") != std::string::npos; });
  std::size_t close_count = 0;
  bool cancel_seen = false;
  {
    std::lock_guard lock(mutex);
    close_count = closes;
    cancel_seen = presenter_observed_cancel;
  }
  expect(modal_opened && rendered_before_disable.find("External disable modal") != std::string::npos &&
             rendered_before_disable.find("DISABLE_RAW_CANARY_51ac") == std::string::npos && disabled && finished && revoke_elapsed < std::chrono::seconds(2) &&
             cancel_seen && child_gone && close_count == 1 && current_enabled && !*current_enabled && !snapshot.plugin_ui_modal && !snapshot.plugin_ui_dock &&
             !session_has_canary && output.find("DISABLE_RAW_CANARY_51ac") == std::string::npos &&
             output.find((paths.ava_state_dir / "plugin-enablement.json").string()) == std::string::npos,
         "an atomic external disable is observed during a real blocking modal, cancels and clears the presenter, terminates and reaps the child, closes "
         "authority once, leaves the plugin disabled for future invocations, hides protocol ids, and exposes neither raw UI nor enablement details");
}

void test_command_request_defaults_without_ui_authority()
{
  ava::app::CommandRequest request;
  expect(!request.plugin_ui_capability,
         "CommandRequest defaults to no plugin UI authority for RPC, ACP, print, headless, tools, hooks, jobs, and synthetic paths");
}

ava::tui::TuiPluginUiRequest tui_request(ava::tui::TuiPluginUiBinding binding, std::string request_id, ava::tui::TuiPluginUiKind kind)
{
  return {
      .binding = std::move(binding), .request_id = std::move(request_id), .kind = kind, .text = {}, .title = {}, .description = {}, .lines = {}, .options = {}};
}

ava::tui::TuiPluginUiReply present_update_and_poll(ava::tui::RuntimePluginUiCoordinator& coordinator, ava::tui::TuiPluginUiEndpoint const& endpoint,
                                                   ava::tui::ComposerSnapshot& snapshot, ava::tui::TuiPluginUiRequest request)
{
  auto reply = std::async(std::launch::async, [endpoint, request = std::move(request), &coordinator]() {
    return endpoint.present(request, endpoint.deadline, [&coordinator] { return coordinator.deadline_reached(std::chrono::steady_clock::time_point::min()); });
  });
  auto const wait_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
  while (reply.wait_for(std::chrono::milliseconds::zero()) != std::future_status::ready && std::chrono::steady_clock::now() < wait_deadline)
  {
    static_cast<void>(coordinator.poll(snapshot, false));
    std::this_thread::yield();
  }
  if (reply.wait_for(std::chrono::milliseconds::zero()) != std::future_status::ready)
  {
    coordinator.cancel_active();
    static_cast<void>(coordinator.poll(snapshot, false));
  }
  return reply.get();
}

bool open_plugin_modal(ava::tui::RuntimePluginUiCoordinator& coordinator, ava::tui::ComposerSnapshot& snapshot, std::future<ava::tui::TuiPluginUiReply>& reply,
                       bool conflict = false)
{
  auto const wait_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
  while (!snapshot.plugin_ui_modal && reply.wait_for(std::chrono::milliseconds::zero()) != std::future_status::ready &&
         std::chrono::steady_clock::now() < wait_deadline)
  {
    static_cast<void>(coordinator.poll(snapshot, conflict));
    std::this_thread::yield();
  }
  if (!snapshot.plugin_ui_modal && reply.wait_for(std::chrono::milliseconds::zero()) != std::future_status::ready)
  {
    coordinator.cancel_active();
    static_cast<void>(coordinator.poll(snapshot, false));
  }
  return snapshot.plugin_ui_modal.has_value();
}

void test_tui_plugin_ui_coordinator_ordering_limits_and_lifecycle()
{
  expect(ava::tui::is_direct_foreground_plugin_run_submission("/plugin run com.example.ui run {}") &&
             ava::tui::is_direct_foreground_plugin_run_submission("/plugin run com.example.ui Run.Command") &&
             ava::tui::is_direct_foreground_plugin_run_submission("/plugin run com.example.ui run") &&
             !ava::tui::is_direct_foreground_plugin_run_submission(" /plugin run com.example.ui run {}") &&
             !ava::tui::is_direct_foreground_plugin_run_submission("/plugin  run com.example.ui run {}") &&
             !ava::tui::is_direct_foreground_plugin_run_submission("/plugin run com.example.ui run {} ") &&
             !ava::tui::is_direct_foreground_plugin_run_submission("/plugin run COM.EXAMPLE.ui run {}"),
         "TUI plugin authority recognizes only an exact canonical direct foreground submission");

  ava::tui::RuntimePluginUiCoordinator coordinator;
  ava::tui::ComposerSnapshot snapshot;
  snapshot.width = 80;
  snapshot.height = 24;
  snapshot.input = "draft remains";
  snapshot.transcript = {{.label = "you", .text = "history remains"}};
  auto endpoint = coordinator.begin_submission("/plugin run com.example.ui run {}", "invocation-1");
  expect(endpoint && static_cast<bool>(*endpoint), "direct submission arms one bounded TUI-local presenter endpoint");
  if (!endpoint)
    return;
  auto const binding = ava::tui::TuiPluginUiBinding{.plugin_id = "com.example.ui", .command = "run", .invocation_id = "invocation-1"};
  auto wrong_binding = tui_request(ava::tui::TuiPluginUiBinding{.plugin_id = "com.example.other", .command = "run", .invocation_id = "invocation-1"},
                                   "wrong-binding", ava::tui::TuiPluginUiKind::Status);
  wrong_binding.text = "must reject";
  auto const wrong_binding_reply = endpoint->present(wrong_binding, endpoint->deadline, [] { return false; });
  expect(wrong_binding_reply.action == ava::tui::TuiPluginUiReplyKind::Cancel,
         "TUI coordinator rejects a presenter DTO whose plugin/command binding differs from the armed canonical submission");
  auto unsafe_dto = tui_request(binding, "unsafe-dto", ava::tui::TuiPluginUiKind::Status);
  unsafe_dto.text = "\x1b[31mspoof";
  auto const unsafe_dto_reply = endpoint->present(unsafe_dto, endpoint->deadline, [] { return false; });
  expect(unsafe_dto_reply.action == ava::tui::TuiPluginUiReplyKind::Cancel,
         "TUI coordinator independently rejects terminal-control text at its bounded DTO boundary");

  auto status = tui_request(binding, "status", ava::tui::TuiPluginUiKind::Status);
  status.text = "working safely";
  auto const status_reply = present_update_and_poll(coordinator, *endpoint, snapshot, std::move(status));
  bool updates_ok = status_reply.action == ava::tui::TuiPluginUiReplyKind::Ack;
  for (std::size_t index = 0; index < 4; ++index)
  {
    auto widget = tui_request(binding, "widget-" + std::to_string(index), ava::tui::TuiPluginUiKind::Widget);
    widget.title = "widget " + std::to_string(index);
    widget.lines = std::vector<std::string>(8, "line");
    updates_ok = updates_ok && present_update_and_poll(coordinator, *endpoint, snapshot, std::move(widget)).action == ava::tui::TuiPluginUiReplyKind::Ack;
  }
  auto overflow = tui_request(binding, "widget-overflow", ava::tui::TuiPluginUiKind::Widget);
  overflow.title = "overflow";
  overflow.lines = {"line"};
  auto const overflow_reply = present_update_and_poll(coordinator, *endpoint, snapshot, std::move(overflow));
  expect(updates_ok && overflow_reply.action == ava::tui::TuiPluginUiReplyKind::Cancel && snapshot.plugin_ui_dock &&
             snapshot.plugin_ui_dock->widgets.size() == 4 && snapshot.plugin_ui_dock->widgets[0].title == "widget 0" &&
             snapshot.plugin_ui_dock->widgets[3].title == "widget 3" && snapshot.input == "draft remains" && snapshot.transcript.size() == 1 &&
             ava::tui::kTuiPluginUiMaxWidgets == 4 && ava::tui::kTuiPluginUiMaxWidgetLines == 32 && ava::tui::kTuiPluginUiMaxTextBytes == 8 * 1024 &&
             ava::tui::kTuiPluginUiMaxQueuedRecords == 64,
         "coordinator acknowledges ordered main-thread updates, enforces 4-widget/32-line/8-KiB/64-record globals, and does not mutate transcript or draft");

  auto old_endpoint = *endpoint;
  coordinator.finish_submission(snapshot);
  auto replacement = coordinator.begin_submission("/plugin run com.example.ui run {}", "invocation-2");
  auto stale = tui_request(binding, "stale", ava::tui::TuiPluginUiKind::Status);
  stale.text = "stale";
  auto const stale_reply = old_endpoint.present(stale, old_endpoint.deadline, [] { return false; });
  expect(replacement && stale_reply.action == ava::tui::TuiPluginUiReplyKind::Cancel && !snapshot.plugin_ui_dock && !snapshot.plugin_ui_modal,
         "session/run cleanup rejects stale presenter bindings and clears ephemeral plugin presentation");
  coordinator.shutdown(snapshot);
  expect(replacement && replacement->runtime_token.expired() && !snapshot.plugin_ui_dock && !snapshot.plugin_ui_modal,
         "TUI shutdown invalidates the runtime token and clears all plugin-owned presentation");
}

void test_tui_plugin_ui_modal_input_conflict_cancel_and_deadline()
{
  ava::tui::RuntimePluginUiCoordinator coordinator;
  ava::tui::ComposerSnapshot snapshot;
  snapshot.width = 80;
  snapshot.height = 10;
  snapshot.input = "untouched draft";
  snapshot.transcript = {{.label = "assistant", .text = "untouched transcript"}};
  auto endpoint = coordinator.begin_submission("/plugin run com.example.ui choose {}", "modal-1");
  if (!endpoint)
  {
    expect(false, "modal fixture arms direct presenter");
    return;
  }
  auto const binding = ava::tui::TuiPluginUiBinding{.plugin_id = "com.example.ui", .command = "choose", .invocation_id = "modal-1"};
  auto selection = tui_request(binding, "select-secret", ava::tui::TuiPluginUiKind::Select);
  selection.title = "Choose safely";
  selection.description = "Host owns this modal";
  selection.options = {{.id = "SECRET_OPTION_ALPHA", .label = "Alpha", .description = std::nullopt},
                       {.id = "SECRET_OPTION_BETA", .label = "Beta", .description = std::string("second")}};
  auto selected_reply =
      std::async(std::launch::async, [endpoint = *endpoint, selection]() { return endpoint.present(selection, endpoint.deadline, [] { return false; }); });
  bool const opened = open_plugin_modal(coordinator, snapshot, selected_reply);
  auto down = ava::tui::InputEvent{.key = ava::tui::terminal::Key::ArrowDown, .character = '\0', .text = {}, .mouse_column = 0, .mouse_row = 0};
  auto enter = ava::tui::InputEvent{.key = ava::tui::terminal::Key::Enter, .character = '\0', .text = {}, .mouse_column = 0, .mouse_row = 0};
  auto const down_result = coordinator.handle_input(snapshot, down);
  auto const enter_result = coordinator.handle_input(snapshot, enter);
  auto const selected = selected_reply.get();
  expect(opened && down_result == ava::tui::TuiPluginUiInputResult::Redraw && enter_result == ava::tui::TuiPluginUiInputResult::Redraw &&
             selected.action == ava::tui::TuiPluginUiReplyKind::Select && selected.option_id == "SECRET_OPTION_BETA" && snapshot.input == "untouched draft" &&
             snapshot.transcript.size() == 1,
         "host-owned arrows and Enter resolve a selection by hidden option ID without changing composer or transcript state");

  coordinator.finish_submission(snapshot);
  endpoint = coordinator.begin_submission("/plugin run com.example.ui choose {}", "modal-escape");
  auto const escape_binding = ava::tui::TuiPluginUiBinding{.plugin_id = "com.example.ui", .command = "choose", .invocation_id = "modal-escape"};
  auto escape_confirm = tui_request(escape_binding, "escape", ava::tui::TuiPluginUiKind::Confirm);
  escape_confirm.title = "Escape";
  escape_confirm.description = "cancel only this modal";
  auto escape_reply = std::async(
      std::launch::async, [endpoint = *endpoint, escape_confirm]() { return endpoint.present(escape_confirm, endpoint.deadline, [] { return false; }); });
  bool const escape_opened = open_plugin_modal(coordinator, snapshot, escape_reply);
  auto escape = ava::tui::InputEvent{.key = ava::tui::terminal::Key::Escape, .character = '\0', .text = {}, .mouse_column = 0, .mouse_row = 0};
  auto const escape_input = coordinator.handle_input(snapshot, escape);
  auto const escaped = escape_reply.get();
  expect(escape_opened && escape_input == ava::tui::TuiPluginUiInputResult::Redraw && escaped.action == ava::tui::TuiPluginUiReplyKind::Cancel &&
             !snapshot.plugin_ui_modal,
         "host-owned Esc resolves a short-identity modal as cancel without stopping the invocation");

  coordinator.finish_submission(snapshot);
  endpoint = coordinator.begin_submission("/plugin run com.example.ui choose {}", "modal-conflict");
  auto const conflict_binding = ava::tui::TuiPluginUiBinding{.plugin_id = "com.example.ui", .command = "choose", .invocation_id = "modal-conflict"};
  auto confirm = tui_request(conflict_binding, "confirm", ava::tui::TuiPluginUiKind::Confirm);
  confirm.title = "Proceed?";
  confirm.description = "Safe default is cancel";
  auto conflict_reply =
      std::async(std::launch::async, [endpoint = *endpoint, confirm]() { return endpoint.present(confirm, endpoint.deadline, [] { return false; }); });
  static_cast<void>(open_plugin_modal(coordinator, snapshot, conflict_reply, true));
  auto const conflicted = conflict_reply.get();
  expect(conflicted.action == ava::tui::TuiPluginUiReplyKind::Cancel && !snapshot.plugin_ui_modal,
         "existing host prompt/modal authority wins and cancels conflicting plugin presentation");

  coordinator.finish_submission(snapshot);
  endpoint = coordinator.begin_submission("/plugin run com.example.ui choose {}", "modal-stop");
  auto const stop_binding = ava::tui::TuiPluginUiBinding{.plugin_id = "com.example.ui", .command = "choose", .invocation_id = "modal-stop"};
  auto stop_confirm = tui_request(stop_binding, "stop", ava::tui::TuiPluginUiKind::Confirm);
  stop_confirm.title = "Stop";
  stop_confirm.description = "Ctrl+C path";
  auto stop_reply = std::async(std::launch::async,
                               [endpoint = *endpoint, stop_confirm]() { return endpoint.present(stop_confirm, endpoint.deadline, [] { return false; }); });
  bool const stop_opened = open_plugin_modal(coordinator, snapshot, stop_reply);
  coordinator.cancel_active();
  auto const stop_ready = stop_reply.wait_for(std::chrono::seconds(1)) == std::future_status::ready;
  auto const stopped = stop_reply.get();
  static_cast<void>(coordinator.poll(snapshot, false));
  expect(stop_opened && stop_ready && stopped.action == ava::tui::TuiPluginUiReplyKind::Cancel && !snapshot.plugin_ui_modal && !snapshot.plugin_ui_dock,
         "the Ctrl+C stop path cancels a short-identity modal and clears all ephemeral presentation");

  coordinator.finish_submission(snapshot);
  auto const started = std::chrono::steady_clock::now();
  endpoint = coordinator.begin_submission("/plugin run com.example.ui choose {}", "modal-3", started);
  auto const binding3 = ava::tui::TuiPluginUiBinding{.plugin_id = "com.example.ui", .command = "choose", .invocation_id = "modal-3"};
  auto deadline_request = tui_request(binding3, "deadline", ava::tui::TuiPluginUiKind::Confirm);
  deadline_request.title = "Deadline";
  deadline_request.description = "must cancel";
  auto deadline_reply = std::async(
      std::launch::async, [endpoint = *endpoint, deadline_request]() { return endpoint.present(deadline_request, endpoint.deadline, [] { return false; }); });
  bool const deadline_opened = open_plugin_modal(coordinator, snapshot, deadline_reply);
  auto const deadline_poll = coordinator.poll(snapshot, false, endpoint->deadline);
  auto const expired = deadline_reply.get();
  expect(deadline_opened && deadline_poll.deadline_expired && expired.action == ava::tui::TuiPluginUiReplyKind::Cancel && !snapshot.plugin_ui_modal &&
             !snapshot.plugin_ui_dock && endpoint->deadline == started + ava::tui::kTuiPluginUiInvocationDeadline,
         "one absolute 120-second direct UI deadline cancels and unblocks a visible modal");

  coordinator.finish_submission(snapshot);
  endpoint = coordinator.begin_submission("/plugin run com.example.ui choose {}", "modal-4");
  auto const binding4 = ava::tui::TuiPluginUiBinding{.plugin_id = "com.example.ui", .command = "choose", .invocation_id = "modal-4"};
  auto canceled_request = tui_request(binding4, "child-exit", ava::tui::TuiPluginUiKind::Confirm);
  canceled_request.title = "Child exit";
  canceled_request.description = "cancel callback";
  std::atomic_bool child_exited = false;
  auto child_reply = std::async(std::launch::async, [endpoint = *endpoint, canceled_request, &child_exited]() {
    return endpoint.present(canceled_request, endpoint.deadline, [&child_exited] { return child_exited.load(); });
  });
  bool const child_opened = open_plugin_modal(coordinator, snapshot, child_reply);
  child_exited.store(true);
  auto const child_ready = child_reply.wait_for(std::chrono::seconds(1)) == std::future_status::ready;
  if (!child_ready)
    coordinator.cancel_active();
  auto const child_result = child_reply.get();
  static_cast<void>(coordinator.poll(snapshot, false));
  expect(child_opened && child_ready && child_result.action == ava::tui::TuiPluginUiReplyKind::Cancel,
         "child exit/cancel polling finitely unblocks a modal without waiting under coordinator locks");
}

void test_tui_plugin_ui_attribution_fit_policy()
{
  auto const shared_prefix = std::string(120, 'a');
  auto const first_id = shared_prefix + "oneone01";
  auto const second_id = shared_prefix + "twotwo02";
  auto const max_command = std::string(96, 'c');
  expect(
      first_id.size() == 128 && second_id.size() == 128 && first_id != second_id && first_id.starts_with(shared_prefix) && second_id.starts_with(shared_prefix),
      "attribution fixture uses distinct maximum valid ids with one long shared prefix");

  auto narrow_case = [&](std::string const& plugin_id, std::string_view invocation_prefix) {
    for (std::size_t height = 8; height <= 12; ++height)
    {
      ava::tui::RuntimePluginUiCoordinator coordinator;
      ava::tui::ComposerSnapshot snapshot;
      snapshot.width = 80;
      snapshot.height = height;
      auto const invocation_id = std::string(invocation_prefix) + std::to_string(height);
      auto const canonical = "/plugin run " + plugin_id + " " + max_command + " {}";
      auto endpoint = coordinator.begin_submission(canonical, invocation_id);
      if (!endpoint)
        return false;
      auto request = tui_request({.plugin_id = plugin_id, .command = max_command, .invocation_id = invocation_id}, "select", ava::tui::TuiPluginUiKind::Select);
      request.title = "Choose";
      request.description = "attributed choice";
      request.options = {{.id = "one", .label = "One", .description = std::nullopt}};
      auto reply =
          std::async(std::launch::async, [endpoint = *endpoint, request] { return endpoint.present(request, endpoint.deadline, [] { return false; }); });
      bool ever_visible = false;
      auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
      while (reply.wait_for(std::chrono::milliseconds::zero()) != std::future_status::ready && std::chrono::steady_clock::now() < deadline)
      {
        static_cast<void>(coordinator.poll(snapshot, false));
        ever_visible = ever_visible || snapshot.plugin_ui_modal.has_value() || snapshot.plugin_ui_dock.has_value();
        if (ever_visible)
          coordinator.cancel_active();
        std::this_thread::yield();
      }
      if (reply.wait_for(std::chrono::milliseconds::zero()) != std::future_status::ready)
        coordinator.cancel_active();
      auto const result = reply.get();
      static_cast<void>(coordinator.poll(snapshot, false));
      if (ever_visible || result.action != ava::tui::TuiPluginUiReplyKind::Cancel || snapshot.plugin_ui_modal || snapshot.plugin_ui_dock)
        return false;
    }
    return true;
  };

  auto wide_to_narrow_case = [&](std::string const& plugin_id) {
    enum class Trigger
    {
      ArrowThenEnter,
      EnterThenArrow,
      PollThenInput,
    };
    auto const arrow = ava::tui::InputEvent{.key = ava::tui::terminal::Key::ArrowDown, .character = '\0', .text = {}, .mouse_column = 0, .mouse_row = 0};
    auto const enter = ava::tui::InputEvent{.key = ava::tui::terminal::Key::Enter, .character = '\0', .text = {}, .mouse_column = 0, .mouse_row = 0};

    for (std::size_t height = 8; height <= 12; ++height)
    {
      auto exercise = [&](ava::tui::TuiPluginUiKind kind, std::string_view suffix, Trigger trigger) {
        ava::tui::RuntimePluginUiCoordinator coordinator;
        ava::tui::ComposerSnapshot snapshot;
        snapshot.width = 160;
        snapshot.height = height;
        auto const invocation_id = "wide_narrow_" + std::to_string(height) + std::string(suffix);
        auto endpoint = coordinator.begin_submission("/plugin run " + plugin_id + " " + max_command + " {}", invocation_id);
        if (!endpoint)
          return false;
        auto request = tui_request({.plugin_id = plugin_id, .command = max_command, .invocation_id = invocation_id}, "modal", kind);
        request.title = "Resize guard";
        request.description = "must cancel invisibly";
        if (kind == ava::tui::TuiPluginUiKind::Select)
          request.options = {{.id = "must-not-select", .label = "Forbidden selection", .description = std::nullopt}};
        auto reply =
            std::async(std::launch::async, [endpoint = *endpoint, request] { return endpoint.present(request, endpoint.deadline, [] { return false; }); });
        if (!open_plugin_modal(coordinator, snapshot, reply))
        {
          static_cast<void>(reply.get());
          return false;
        }

        snapshot.width = 80;
        bool invisible = true;
        for (auto const& line : ava::tui::render_composer(snapshot)) invisible = invisible && strip_sgr(line).find(plugin_id) == std::string::npos;

        bool dispatch_ok = false;
        if (trigger == Trigger::ArrowThenEnter)
        {
          auto const arrow_result = coordinator.handle_input(snapshot, arrow);
          auto const enter_result = coordinator.handle_input(snapshot, enter);
          dispatch_ok = arrow_result == ava::tui::TuiPluginUiInputResult::Redraw && enter_result == ava::tui::TuiPluginUiInputResult::Unhandled;
        }
        else if (trigger == Trigger::EnterThenArrow)
        {
          auto const enter_result = coordinator.handle_input(snapshot, enter);
          auto const arrow_result = coordinator.handle_input(snapshot, arrow);
          dispatch_ok = enter_result == ava::tui::TuiPluginUiInputResult::Redraw && arrow_result == ava::tui::TuiPluginUiInputResult::Unhandled;
        }
        else
        {
          auto const polled = coordinator.poll(snapshot, false);
          auto const arrow_result = coordinator.handle_input(snapshot, arrow);
          auto const enter_result = coordinator.handle_input(snapshot, enter);
          dispatch_ok =
              polled.changed && arrow_result == ava::tui::TuiPluginUiInputResult::Unhandled && enter_result == ava::tui::TuiPluginUiInputResult::Unhandled;
        }

        auto const ready = reply.wait_for(std::chrono::seconds(1)) == std::future_status::ready;
        if (!ready)
          coordinator.cancel_active();
        auto const result = reply.get();
        return invisible && dispatch_ok && result.action == ava::tui::TuiPluginUiReplyKind::Cancel && result.option_id.empty() && !snapshot.plugin_ui_modal &&
               !snapshot.plugin_ui_dock;
      };

      if (!exercise(ava::tui::TuiPluginUiKind::Select, "_enter", Trigger::EnterThenArrow) ||
          !exercise(ava::tui::TuiPluginUiKind::Confirm, "_arrow", Trigger::ArrowThenEnter) ||
          !exercise(ava::tui::TuiPluginUiKind::Select, "_poll", Trigger::PollThenInput))
      {
        return false;
      }
    }
    return true;
  };

  auto render_wide = [&](std::string const& plugin_id) {
    ava::tui::ComposerSnapshot snapshot;
    snapshot.width = 160;
    snapshot.height = 12;
    snapshot.plugin_ui_modal = ava::tui::TuiPluginUiModalView{.binding = {.plugin_id = plugin_id, .command = max_command, .invocation_id = "wide"},
                                                              .request_id = "select",
                                                              .kind = ava::tui::TuiPluginUiKind::Select,
                                                              .title = "Choose",
                                                              .description = "attributed choice",
                                                              .options = {{.id = "one", .label = "One", .description = std::nullopt}},
                                                              .selected_option = 0};
    std::string visible;
    for (auto const& line : ava::tui::render_composer(snapshot)) visible += strip_sgr(line) + '\n';
    return visible;
  };

  auto const first_wide = render_wide(first_id);
  auto const second_wide = render_wide(second_id);
  bool const complete_wide_identity = first_wide.find(first_id) != std::string::npos && first_wide.find(max_command) != std::string::npos &&
                                      second_wide.find(second_id) != std::string::npos && second_wide.find(max_command) != std::string::npos;
  expect(narrow_case(first_id, "narrow_one") && narrow_case(second_id, "narrow_two") && wide_to_narrow_case(first_id) && complete_wide_identity &&
             first_wide != second_wide && first_wide.find("…") == std::string::npos && second_wide.find("…") == std::string::npos,
         "80-column maximum identities fail closed without a selectable surface at every 8-12-row height; a modal opened at 160 columns is canceled after "
         "shrinking and neither arrows nor Enter can resolve select/confirm; sufficient width renders both complete distinct ids and the full command "
         "without ellipsis");

  ava::tui::ComposerSnapshot dock_snapshot;
  dock_snapshot.width = 160;
  dock_snapshot.height = 12;
  dock_snapshot.plugin_ui_dock = ava::tui::TuiPluginUiDockView{
      .binding = {.plugin_id = first_id, .command = max_command, .invocation_id = "wide_dock"}, .status = "working", .widgets = {}};
  std::string dock_visible;
  for (auto const& line : ava::tui::render_composer(dock_snapshot)) dock_visible += strip_sgr(line) + '\n';
  expect(dock_visible.find(first_id) != std::string::npos && dock_visible.find(max_command) != std::string::npos &&
             dock_visible.find("Ctrl+C stop · 120s max") != std::string::npos && dock_visible.find("…") == std::string::npos,
         "sufficient-width dock chrome also renders complete maximum attribution and fixed controls");

  ava::tui::RuntimePluginUiCoordinator queued_coordinator;
  ava::tui::ComposerSnapshot queued_snapshot;
  queued_snapshot.width = 160;
  queued_snapshot.height = 12;
  auto queued_endpoint = queued_coordinator.begin_submission("/plugin run " + first_id + " " + max_command + " {}", "queued_resize");
  if (!queued_endpoint)
  {
    expect(false, "queued resize fixture arms a presenter endpoint");
    return;
  }
  auto queued_status =
      tui_request({.plugin_id = first_id, .command = max_command, .invocation_id = "queued_resize"}, "queued-status", ava::tui::TuiPluginUiKind::Status);
  queued_status.text = "must not be acknowledged";
  std::mutex enqueue_mutex;
  std::condition_variable enqueued_cv;
  bool enqueued = false;
  queued_coordinator.set_after_enqueue_for_test([&] {
    std::lock_guard lock(enqueue_mutex);
    enqueued = true;
    enqueued_cv.notify_all();
  });
  auto queued_reply = std::async(
      std::launch::async, [endpoint = *queued_endpoint, queued_status] { return endpoint.present(queued_status, endpoint.deadline, [] { return false; }); });
  auto const queued_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
  {
    std::unique_lock lock(enqueue_mutex);
    static_cast<void>(enqueued_cv.wait_until(lock, queued_deadline, [&] { return enqueued; }));
  }
  queued_snapshot.width = 80;
  while (queued_reply.wait_for(std::chrono::milliseconds::zero()) != std::future_status::ready && std::chrono::steady_clock::now() < queued_deadline)
  {
    static_cast<void>(queued_coordinator.poll(queued_snapshot, false));
    std::this_thread::yield();
  }
  if (queued_reply.wait_for(std::chrono::milliseconds::zero()) != std::future_status::ready)
    queued_coordinator.cancel_active();
  auto const queued_result = queued_reply.get();

  ava::tui::RuntimePluginUiCoordinator visible_coordinator;
  ava::tui::ComposerSnapshot visible_snapshot;
  visible_snapshot.width = 160;
  visible_snapshot.height = 12;
  auto visible_endpoint = visible_coordinator.begin_submission("/plugin run " + first_id + " " + max_command + " {}", "visible_resize");
  if (!visible_endpoint)
  {
    expect(false, "visible resize fixture arms a presenter endpoint");
    return;
  }
  auto visible_widget =
      tui_request({.plugin_id = first_id, .command = max_command, .invocation_id = "visible_resize"}, "visible-widget", ava::tui::TuiPluginUiKind::Widget);
  visible_widget.title = "Wide dock";
  visible_widget.lines = {"visible before resize"};
  auto const visible_result = present_update_and_poll(visible_coordinator, *visible_endpoint, visible_snapshot, std::move(visible_widget));
  visible_snapshot.width = 80;
  auto const invalidated = visible_coordinator.cancel_unfittable_surfaces(visible_snapshot);
  auto after_invalidation =
      tui_request({.plugin_id = first_id, .command = max_command, .invocation_id = "visible_resize"}, "after-invalidation", ava::tui::TuiPluginUiKind::Status);
  after_invalidation.text = "must remain canceled";
  auto const invalidated_reply = visible_endpoint->present(after_invalidation, visible_endpoint->deadline, [] { return false; });

  expect(enqueued && queued_endpoint && queued_result.action == ava::tui::TuiPluginUiReplyKind::Cancel && !queued_snapshot.plugin_ui_dock &&
             !queued_snapshot.plugin_ui_modal && visible_endpoint && visible_result.action == ava::tui::TuiPluginUiReplyKind::Ack && invalidated &&
             invalidated_reply.action == ava::tui::TuiPluginUiReplyKind::Cancel && !visible_snapshot.plugin_ui_dock && !visible_snapshot.plugin_ui_modal,
         "a resize published before a queued dock poll prevents stale ACK/publication, and an already-visible widget dock that loses complete host chrome "
         "is cleared while its binding is invalidated against future requests");
}

void test_tui_plugin_ui_close_poll_race_does_not_publish_after_invalidation()
{
  ava::tui::RuntimePluginUiCoordinator coordinator;
  ava::tui::ComposerSnapshot snapshot;
  snapshot.width = 80;
  snapshot.height = 24;
  auto endpoint = coordinator.begin_submission("/plugin run com.example.ui run {}", "close_race");
  if (!endpoint)
  {
    expect(false, "close/poll race fixture arms a presenter endpoint");
    return;
  }
  auto const binding = ava::tui::TuiPluginUiBinding{.plugin_id = "com.example.ui", .command = "run", .invocation_id = "close_race"};
  auto request = tui_request(binding, "status", ava::tui::TuiPluginUiKind::Status);
  request.text = "must never publish";
  auto reply = std::async(std::launch::async, [endpoint = *endpoint, request] { return endpoint.present(request, endpoint.deadline, [] { return false; }); });

  std::mutex mutex;
  std::condition_variable cv;
  bool swapped_nonempty = false;
  bool release_poll = false;
  auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
  std::future<ava::tui::TuiPluginUiPollResult> polling;
  while (!swapped_nonempty && std::chrono::steady_clock::now() < deadline)
  {
    bool hook_ran = false;
    coordinator.set_after_queue_swap_for_test([&](std::size_t count) {
      std::unique_lock lock(mutex);
      hook_ran = true;
      swapped_nonempty = count != 0;
      cv.notify_all();
      if (swapped_nonempty)
        cv.wait(lock, [&] { return release_poll; });
    });
    polling = std::async(std::launch::async, [&] { return coordinator.poll(snapshot, false); });
    {
      std::unique_lock lock(mutex);
      static_cast<void>(cv.wait_until(lock, deadline, [&] { return hook_ran; }));
    }
    if (!swapped_nonempty)
    {
      static_cast<void>(polling.get());
      std::this_thread::yield();
    }
  }

  bool close_returned = false;
  if (swapped_nonempty)
  {
    endpoint->close(binding);
    close_returned = true;
    {
      std::lock_guard lock(mutex);
      release_poll = true;
    }
    cv.notify_all();
  }
  auto const poll_result = polling.valid() ? polling.get() : ava::tui::TuiPluginUiPollResult{};
  if (!close_returned)
    coordinator.cancel_active();
  auto const reply_ready = reply.wait_for(std::chrono::seconds(1)) == std::future_status::ready;
  auto const action = reply.get();
  static_cast<void>(coordinator.poll(snapshot, false));
  expect(swapped_nonempty && close_returned && reply_ready && action.action == ava::tui::TuiPluginUiReplyKind::Cancel && !snapshot.plugin_ui_dock &&
             !snapshot.plugin_ui_modal && !poll_result.changed,
         "a generation close serialized after dequeue but before apply cancels the record and cannot ACK or transiently publish a surface");
}

void test_plugin_ui_centered_modal_insets_and_hit_geometry()
{
  ava::tui::TuiPluginUiModalView view{.binding = {.plugin_id = "com.example.ui", .command = "choose", .invocation_id = "hidden"},
                                      .request_id = "hidden",
                                      .kind = ava::tui::TuiPluginUiKind::Select,
                                      .title = "Choose",
                                      .description = "Select one",
                                      .options = {{.id = "alpha", .label = "Alpha", .description = std::nullopt}},
                                      .selected_option = 0};
  auto const roomy = ava::tui::detail::render_plugin_ui_modal(view, 80, 14);
  auto const narrow = ava::tui::detail::render_plugin_ui_modal(view, 55, 12);
  auto const roomy_identity = strip_sgr(roomy[1]);
  auto const roomy_option = strip_sgr(roomy[5]);
  auto const narrow_identity = strip_sgr(narrow[0]);
  expect(roomy.size() == 14 && strip_sgr(roomy.front()) == std::string(80, ' ') && strip_sgr(roomy.back()) == std::string(80, ' ') &&
             roomy_identity.starts_with("    Plugin ID") && roomy_identity.ends_with(std::string(4, ' ')) && roomy_option.starts_with("    › Alpha") &&
             roomy_option.ends_with(std::string(4, ' ')) && !ava::tui::detail::plugin_ui_option_for_modal_row(view, 0, 80, 14) &&
             ava::tui::detail::plugin_ui_option_for_modal_row(view, 5, 80, 14) == std::size_t{0} && narrow.size() == 12 &&
             narrow_identity.starts_with("  Plugin ID") && !narrow_identity.starts_with("    ") && narrow_identity.ends_with(std::string(2, ' ')) &&
             strip_sgr(narrow.front()).find("Plugin ID") != std::string::npos && strip_sgr(narrow.back()).find("Enter select") != std::string::npos &&
             ava::tui::detail::plugin_ui_option_for_modal_row(view, 4, 55, 12) == std::size_t{0} &&
             ava::tui::plugin_ui_host_chrome_fits(view.binding, view.kind, 80, 14),
         "plugin Select modal uses responsive complete insets, reserves roomy blank rows from selection, and retains host attribution and controls");

  view.kind = ava::tui::TuiPluginUiKind::Confirm;
  auto const confirm = ava::tui::detail::render_plugin_ui_modal(view, 80, 14);
  expect(confirm.size() == 14 && strip_sgr(confirm[5]).find("Confirm plugin action") != std::string::npos &&
             strip_sgr(confirm[12]).find("Enter confirm · Esc cancel · Ctrl+C stop · 120s max") != std::string::npos &&
             ava::tui::detail::plugin_ui_option_for_modal_row(view, 5, 80, 14) == std::size_t{0},
         "plugin Confirm modal keeps both host-owned choice attribution and controls inside the inset geometry");
}

void test_tui_plugin_ui_renderer_is_bounded_sanitized_and_host_owned()
{
  ava::tui::ComposerSnapshot snapshot;
  snapshot.width = 80;
  snapshot.plugin_ui_modal =
      ava::tui::TuiPluginUiModalView{.binding = {.plugin_id = "com.example.ui", .command = "choose", .invocation_id = "HIDDEN_INVOCATION"},
                                     .request_id = "HIDDEN_REQUEST",
                                     .kind = ava::tui::TuiPluginUiKind::Select,
                                     .title = "Choose\x1b[31m safely",
                                     .description = "description",
                                     .options = {{.id = "HIDDEN_OPTION_A", .label = "Alpha", .description = std::nullopt},
                                                 {.id = "HIDDEN_OPTION_B", .label = "Beta", .description = std::string("second")}},
                                     .selected_option = 1};
  bool bounded = true;
  std::string visible;
  for (std::size_t height = 8; height <= 12; ++height)
  {
    snapshot.height = height;
    auto const frame = ava::tui::render_composer(snapshot);
    bounded = bounded && frame.size() == height;
    if (height == 12)
    {
      for (auto const& line : frame) visible += strip_sgr(line) + "\n";
    }
  }
  expect(bounded && visible.find("com.example.ui") != std::string::npos && visible.find("choose") != std::string::npos &&
             visible.find("Alpha") != std::string::npos && visible.find("Beta") != std::string::npos && visible.find("HIDDEN_OPTION") == std::string::npos &&
             visible.find("HIDDEN_REQUEST") == std::string::npos && visible.find("HIDDEN_INVOCATION") == std::string::npos &&
             visible.find("Enter select") != std::string::npos && visible.find("Esc cancel") != std::string::npos &&
             visible.find("Ctrl+C stop") != std::string::npos && visible.find("120s max") != std::string::npos,
         "8-12-row renderer sanitizes plugin fields, uses fixed identity/command chrome, hides protocol IDs, and keeps host-owned controls visible" +
             std::string(bounded ? "" : " (unbounded)") + "\n" + visible);
}

}  // namespace

void run_plugin_ui_tests()
{
  test_plugin_ui_manifest_capabilities_are_independent();
  test_plugin_ui_protocol_happy_paths_and_actions();
  test_plugin_ui_protocol_exact_limits();
  test_plugin_ui_protocol_rejects_terminal_controls_and_malformed_input();
  test_plugin_ui_protocol_state_limits_and_duplicates();
  test_plugin_runner_command_ui_happy_path();
  test_plugin_runner_rejects_unauthorized_or_invalid_ui_safely();
  test_non_command_runner_paths_cannot_present_ui();
  test_plugin_runner_ui_deadline_and_handler_failure_cleanup();
  test_plugin_runner_ui_authority_alone_can_raise_command_timeout();
  test_plugin_ui_capability_claim_binding_and_lifecycle();
  test_plugin_ui_capability_factory_validation_expiry_and_runtime_loss();
  test_plugin_ui_capability_active_runtime_loss_unblocks_presenter();
  test_plugin_ui_permission_mapping_and_headless_denial();
  test_app_direct_command_claims_ui_and_caches_permission_once();
  test_app_ui_permission_deny_and_exact_binding_fail_closed();
  test_external_disable_revokes_active_ui_command();
  test_command_request_defaults_without_ui_authority();
  test_tui_plugin_ui_coordinator_ordering_limits_and_lifecycle();
  test_tui_plugin_ui_modal_input_conflict_cancel_and_deadline();
  test_tui_plugin_ui_attribution_fit_policy();
  test_tui_plugin_ui_close_poll_race_does_not_publish_after_invalidation();
  test_plugin_ui_centered_modal_insets_and_hit_geometry();
  test_tui_plugin_ui_renderer_is_bounded_sanitized_and_host_owned();
}

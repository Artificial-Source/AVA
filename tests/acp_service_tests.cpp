#include "sys.h"
#include "tests/acp_test_declarations.h"
#include "tests/support/acp_test_support.h"
#include "tests/support/app_runtime_support.h"
#include "tests/support/fake_transport.h"
#include "tests/support/test_harness.h"
#include "ava/http/transport.h"
#include "ava/app/acp/service.h"
#include "ava/app/runtime_credentials.h"
#include "ava/config/model_config.h"
#include "ava/session/attachments.h"
#include "ava/session/record.h"
#include "ava/session/session_metadata.h"
#include "ava/session/session_store.h"
#include "ava/provider/provider.h"
#include "ava/provider/registry.h"
#include "ava/core/error.h"
#include "ava/core/ids.h"
#include "ava/core/json.h"
#include "ava/core/path.h"
#include "ava/core/result.h"
#include "ava/core/runtime_outcome.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <fstream>
#include <ios>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>
#include <sys/wait.h>
#include <unistd.h>

using namespace std::chrono_literals;
using ava::app::acp::JsonRpcId;
using namespace acp_test;
namespace runtime = ava::app::runtime;

void test_acp_service_gating_reinitialize_and_negotiation()
{
  using namespace ava::app::acp;
  AgentService service("1.0.0");
  Request before{.id = std::int64_t(1), .method = "session/new", .params_json = std::string("{}")};
  auto preinit = service.handle_request(before, {});
  expect(!preinit && preinit.error().code == -32600, "ACP service rejects methods before initialize");

  Request initialize{.id = std::int64_t(2), .method = "initialize", .params_json = std::string(R"({"protocolVersion":99})")};
  auto initialized = service.handle_request(initialize, {});
  expect(initialized && initialized->find("\"protocolVersion\":1") != std::string::npos && initialized->find("\"loadSession\":false") != std::string::npos &&
             initialized->find("\"image\":true") != std::string::npos && service.initialized(),
         "ACP M4 initializes successfully with the immutable truthful capability matrix");
  auto again = service.handle_request(initialize, {});
  expect(!again && again.error().code == -32600, "ACP initialize remains single-shot");
  auto unknown = service.handle_request(Request{.id = std::int64_t(3), .method = "unknown", .params_json = std::string("{}")}, {});
  expect(!unknown && unknown.error().code == -32601, "ACP initialized service returns method-not-found for unadvertised operations");
}

void test_acp_service_mutating_request_terminal_commits()
{
  using namespace ava::app::acp;
  auto const root = std::filesystem::temp_directory_path() / ava::core::make_id("acp-service-commit");
  auto const workspace = root / "workspace";
  std::filesystem::create_directories(workspace);
  configure_acp_test_model(root);
  auto const paths = ava::tests::app_test_paths(root);

  AgentServiceOptions options;
  options.agent_version = "1";
  options.launch_root = ava::core::normalized_absolute_path(workspace);
  options.paths = paths;
  std::set<std::int64_t> canceled_ids{1, 2, 4, 6};
  AgentService service(options);
  service.bind_request_terminal_committer([&canceled_ids](JsonRpcId const& id) {
    auto const* value = std::get_if<std::int64_t>(&id);
    return value == nullptr || !canceled_ids.contains(*value);
  });

  auto canceled_initialize = service.handle_request(initialize_request(1), {});
  expect(!canceled_initialize && canceled_initialize.error().code == -32800 && !service.initialized(),
         "cancellation before initialize commit leaves the service uninitialized");
  auto initialized = service.handle_request(initialize_request(10), {});
  expect(initialized && service.initialized(), "a committed initialize publishes the validated initialized state");

  auto omitted_list = service.handle_request(Request{.id = std::int64_t(11), .method = "session/list", .params_json = std::nullopt}, {});
  auto null_list = service.handle_request(Request{.id = std::int64_t(12), .method = "session/list", .params_json = std::string("null")}, {});
  expect(omitted_list && null_list, "session/list treats omitted and null params as the pinned empty object");
  auto omitted_new = service.handle_request(Request{.id = std::int64_t(13), .method = "session/new", .params_json = std::nullopt}, {});
  auto null_new = service.handle_request(Request{.id = std::int64_t(14), .method = "session/new", .params_json = std::string("null")}, {});
  expect(!omitted_new && omitted_new.error().code == -32602 && !null_new && null_new.error().code == -32602,
         "required-parameter methods still reject omitted and null params at method level");

  auto canceled_new = service.handle_request(
      Request{.id = std::int64_t(2), .method = "session/new", .params_json = std::string("{\"cwd\":\"") + workspace.string() + "\",\"mcpServers\":[]}"}, {});
  auto sessions_after_cancel = ava::session::SessionStore::list_sessions(workspace, paths.sessions_dir);
  expect(!canceled_new && canceled_new.error().code == -32800 && sessions_after_cancel && sessions_after_cancel->empty(),
         "cancellation before session/new commit creates no persistent or registry session");

  auto created = service.handle_request(
      Request{.id = std::int64_t(3), .method = "session/new", .params_json = std::string("{\"cwd\":\"") + workspace.string() + "\",\"mcpServers\":[]}"}, {});
  auto session_id = created ? ava::core::json::string_field(*created, "sessionId") : std::nullopt;
  expect(session_id.has_value(), "a committed session/new performs the registry mutation and returns its session id");
  if (session_id)
  {
    auto canceled_close = service.handle_request(
        Request{.id = std::int64_t(4), .method = "session/close", .params_json = std::string("{\"sessionId\":\"") + *session_id + "\"}"}, {});
    auto committed_close = service.handle_request(
        Request{.id = std::int64_t(5), .method = "session/close", .params_json = std::string("{\"sessionId\":\"") + *session_id + "\"}"}, {});
    expect(!canceled_close && canceled_close.error().code == -32800 && committed_close,
           "a canceled close leaves the host active and a committed close removes it");

    auto canceled_resume = service.handle_request(
        Request{.id = std::int64_t(6),
                .method = "session/resume",
                .params_json = std::string("{\"sessionId\":\"") + *session_id + "\",\"cwd\":\"" + workspace.string() + "\",\"mcpServers\":[]}"},
        {});
    auto committed_resume = service.handle_request(
        Request{.id = std::int64_t(7),
                .method = "session/resume",
                .params_json = std::string("{\"sessionId\":\"") + *session_id + "\",\"cwd\":\"" + workspace.string() + "\",\"mcpServers\":[]}"},
        {});
    expect(!canceled_resume && canceled_resume.error().code == -32800 && committed_resume,
           "a canceled resume inserts no host and a committed resume remains available");
    static_cast<void>(service.handle_request(
        Request{.id = std::int64_t(8), .method = "session/close", .params_json = std::string("{\"sessionId\":\"") + *session_id + "\"}"}, {}));
  }

  service.shutdown();
  std::error_code cleanup;
  std::filesystem::remove_all(root, cleanup);
}

void test_acp_session_request_schema_defaults_and_invalid_item_skipping()
{
  using namespace ava::app::acp;
  auto const root = std::filesystem::temp_directory_path() / ava::core::make_id("acp-schema-defaults");
  auto const workspace = root / "workspace";
  std::filesystem::create_directories(workspace);
  configure_acp_test_model(root);
  auto const paths = ava::tests::app_test_paths(root);

  AgentServiceOptions options;
  options.agent_version = "1";
  options.launch_root = ava::core::normalized_absolute_path(workspace);
  options.paths = paths;
  AgentService service(options);
  static_cast<void>(service.handle_request(initialize_request(), {}));

  auto create = [&](std::int64_t id, std::string fields) {
    return service.handle_request(
        Request{.id = id, .method = "session/new", .params_json = std::string("{\"cwd\":\"") + workspace.string() + "\"" + std::move(fields) + "}"}, {});
  };
  auto missing_mcp = create(2, R"(,"_meta":[])");
  auto malformed_collections = create(3, R"(,"additionalDirectories":7,"mcpServers":{})");
  auto invalid_items = create(4, R"(,"additionalDirectories":[7,{},null],"mcpServers":[7,{"name":4},{"name":"bad","command":"relative","args":[],"env":[]}])");
  std::string many_invalid_items = R"(,"mcpServers":[)";
  for (std::size_t index = 0; index < kMaxConnectionSessions; ++index)
  {
    if (index != 0)
      many_invalid_items.push_back(',');
    many_invalid_items += "null";
  }
  many_invalid_items += "]";
  auto skipped_before_bound = create(5, std::move(many_invalid_items));
  auto valid_directory = create(6, R"(,"additionalDirectories":["/other"],"mcpServers":[])");
  auto valid_http = create(7, R"(,"mcpServers":[{"type":"http","name":"remote","url":"https://example.test/mcp","headers":[]}])");

  std::vector<std::string> created_ids;
  for (auto const* result : {&missing_mcp, &malformed_collections, &invalid_items, &skipped_before_bound})
    if (*result)
      if (auto id = ava::core::json::string_field(**result, "sessionId"))
        created_ids.push_back(std::move(*id));
  expect(
      created_ids.size() == 4 && !valid_directory && valid_directory.error().message.find("additionalDirectories") != std::string::npos && !valid_http &&
          valid_http.error().message.find("implicit ACP stdio") != std::string::npos,
      "ACP applies field-local defaults and invalid-item skipping before count bounds or rejection of normalized valid unsupported roots and MCP transports");

  for (auto const& id : created_ids)
    static_cast<void>(
        service.handle_request(Request{.id = std::int64_t(20), .method = "session/close", .params_json = std::string("{\"sessionId\":\"") + id + "\"}"}, {}));
  service.shutdown();
  std::error_code cleanup;
  std::filesystem::remove_all(root, cleanup);
}

void test_acp_session_capacity_is_reserved_before_persistence()
{
  using namespace ava::app::acp;
  auto const root = std::filesystem::temp_directory_path() / ava::core::make_id("acp-session-capacity");
  auto const workspace = root / "workspace";
  std::filesystem::create_directories(workspace);
  configure_acp_test_model(root);
  auto const paths = ava::tests::app_test_paths(root);

  AgentServiceOptions options;
  options.agent_version = "1";
  options.launch_root = ava::core::normalized_absolute_path(workspace);
  options.paths = paths;
  AgentService service(options);
  static_cast<void>(service.handle_request(initialize_request(), {}));

  std::vector<std::string> session_ids;
  for (std::size_t index = 0; index < kMaxConnectionSessions; ++index)
  {
    auto created = service.handle_request(Request{.id = static_cast<std::int64_t>(index + 2),
                                                  .method = "session/new",
                                                  .params_json = std::string("{\"cwd\":\"") + workspace.string() + "\",\"mcpServers\":[]}"},
                                          {});
    if (created)
      if (auto id = ava::core::json::string_field(*created, "sessionId"))
        session_ids.push_back(std::move(*id));
  }
  auto rejected = service.handle_request(
      Request{.id = std::int64_t(100), .method = "session/new", .params_json = std::string("{\"cwd\":\"") + workspace.string() + "\",\"mcpServers\":[]}"}, {});
  auto persisted = ava::session::SessionStore::list_sessions(workspace, paths.sessions_dir);
  expect(session_ids.size() == kMaxConnectionSessions && !rejected && rejected.error().message.find("session limit") != std::string::npos && persisted &&
             persisted->size() == kMaxConnectionSessions,
         "ACP reserves connection capacity before session/new creates durable state and leaves no inaccessible overflow session");

  for (auto const& id : session_ids)
    static_cast<void>(
        service.handle_request(Request{.id = std::int64_t(200), .method = "session/close", .params_json = std::string("{\"sessionId\":\"") + id + "\"}"}, {}));
  service.shutdown();
  std::error_code cleanup;
  std::filesystem::remove_all(root, cleanup);
}

void test_acp_startup_model_is_pinned_across_config_mutation()
{
  using namespace ava::app::acp;
  auto const root = std::filesystem::temp_directory_path() / ava::core::make_id("acp-model-pin");
  auto const workspace = root / "workspace";
  std::filesystem::create_directories(workspace);
  configure_acp_test_model(root);
  auto const paths = ava::tests::app_test_paths(root);

  std::string request_body;
  std::vector<ava::config::ModelInfo> observed_models;
  auto base_factory = recording_bundle_factory(&request_body);
  AgentServiceOptions options;
  options.agent_version = "1";
  options.launch_root = ava::core::normalized_absolute_path(workspace);
  options.paths = paths;
  options.provider_bundle_factory = [&observed_models, base_factory](ava::app::runtime::session_ts const& unlocked_session, ava::app::runtime::RunOptions run_options,
                                                                     std::string_view label) mutable {
    observed_models.push_back(ava::app::runtime::session_ts::crat(unlocked_session)->model());
    return base_factory(unlocked_session, std::move(run_options), label);
  };
  AgentService service(options);
  service.bind_update_sender([](std::string_view, std::string_view) -> ava::core::VoidResult { return {}; });
  auto initialized = service.handle_request(initialize_request(), {});
  expect(initialized && initialized->find(R"("image":false)") != std::string::npos, "ACP initialize advertises capability from the startup model snapshot");

  ava::tests::write_app_test_file(
      paths.models_file,
      R"({"default_provider":"moonshot","default_model":"mutated-image","models":[{"provider":"moonshot","id":"mutated-image","name":"Mutated","family":"fake","supports_tools":false,"supports_streaming":false,"input_modalities":["text","image"],"output_modalities":["text"]}]})");
  auto created = service.handle_request(
      Request{.id = std::int64_t(2), .method = "session/new", .params_json = std::string("{\"cwd\":\"") + workspace.string() + "\",\"mcpServers\":[]}"}, {});
  auto session_id = created ? ava::core::json::string_field(*created, "sessionId") : std::nullopt;
  expect(session_id.has_value(), "ACP creates a session after model config mutation");
  if (!session_id)
    return;
  auto prompted =
      service.handle_request(Request{.id = std::int64_t(3),
                                     .method = "session/prompt",
                                     .params_json = std::string("{\"sessionId\":\"") + *session_id + "\",\"prompt\":[{\"type\":\"text\",\"text\":\"pin\"}]}"},
                             {});
  expect(prompted && observed_models.size() == 1 && observed_models.front().provider_id == "moonshot" && observed_models.front().model_id == "acp-test" &&
             std::ranges::find(observed_models.front().input_modalities, "image") == observed_models.front().input_modalities.end(),
         "ACP session/new uses the exact startup model despite later config edits");

  static_cast<void>(service.handle_request(
      Request{.id = std::int64_t(4), .method = "session/close", .params_json = std::string("{\"sessionId\":\"") + *session_id + "\"}"}, {}));
  auto resumed = service.handle_request(
      Request{.id = std::int64_t(5),
              .method = "session/resume",
              .params_json = std::string("{\"sessionId\":\"") + *session_id + "\",\"cwd\":\"" + workspace.string() + "\",\"mcpServers\":[]}"},
      {});
  auto resumed_prompt = service.handle_request(
      Request{.id = std::int64_t(6),
              .method = "session/prompt",
              .params_json = std::string("{\"sessionId\":\"") + *session_id + "\",\"prompt\":[{\"type\":\"text\",\"text\":\"resume\"}]}"},
      {});
  expect(resumed && resumed_prompt && observed_models.size() == 2 && observed_models.back().model_id == "acp-test" &&
             std::ranges::find(observed_models.back().input_modalities, "image") == observed_models.back().input_modalities.end(),
         "ACP session/resume keeps the same immutable startup model and capabilities");

  auto const invalid_root = std::filesystem::temp_directory_path() / ava::core::make_id("acp-model-unresolved");
  std::filesystem::create_directories(invalid_root / "workspace");
  auto invalid_paths = ava::tests::app_test_paths(invalid_root);
  std::filesystem::create_directories(invalid_paths.ava_config_dir);
  ava::tests::write_app_test_file(invalid_paths.models_file, R"({"default_provider":"missing","default_model":"missing"})");
  AgentServiceOptions invalid_options;
  invalid_options.agent_version = "1";
  invalid_options.launch_root = ava::core::normalized_absolute_path(invalid_root / "workspace");
  invalid_options.paths = invalid_paths;
  AgentService invalid_service(invalid_options);
  auto unresolved = invalid_service.handle_request(initialize_request(), {});
  expect(!unresolved && unresolved.error().message.find("cannot be resolved exactly") != std::string::npos &&
             unresolved.error().message.find("restart ava --acp") != std::string::npos && !invalid_service.initialized(),
         "ACP initialize fails actionably when the startup provider/model cannot resolve");

  ava::tests::write_app_test_file(
      invalid_paths.models_file,
      R"({"default_provider":"synthetic","default_model":"declared","models":[{"provider":"synthetic","id":"declared","name":"Declared","family":"fake","supports_tools":false,"supports_streaming":false,"input_modalities":["text"],"output_modalities":["text"]}]})");
  AgentService unknown_provider_service(invalid_options);
  auto unknown_provider = unknown_provider_service.handle_request(initialize_request(), {});
  expect(!unknown_provider && unknown_provider.error().message.find("startup provider is not registered") != std::string::npos &&
             unknown_provider.error().message.find("provider: synthetic") != std::string::npos && !unknown_provider_service.initialized(),
         "ACP rejects a declared startup model whose provider is unavailable");

  std::string synthetic_body;
  auto synthetic_options = invalid_options;
  synthetic_options.provider_bundle_factory = recording_bundle_factory(&synthetic_body);
  AgentService synthetic_provider_service(std::move(synthetic_options));
  auto synthetic_initialized = synthetic_provider_service.handle_request(initialize_request(), {});
  expect(synthetic_initialized && synthetic_provider_service.initialized(),
         "ACP custom provider bundle factories may supply synthetic declared providers for tests and embeddings");

  std::error_code cleanup;
  std::filesystem::remove_all(root, cleanup);
  std::filesystem::remove_all(invalid_root, cleanup);
}

void test_acp_resume_projects_history_for_pinned_startup_model()
{
  using namespace ava::app::acp;
  auto const root = std::filesystem::temp_directory_path() / ava::core::make_id("acp-resume-model-history");
  auto const workspace = root / "workspace";
  std::filesystem::create_directories(workspace);
  auto const paths = ava::tests::app_test_paths(root);
  std::filesystem::create_directories(paths.ava_config_dir);
  ava::tests::write_app_test_file(
      paths.models_file,
      R"({"default_provider":"moonshot","default_model":"image-model","models":[{"provider":"moonshot","id":"image-model","name":"Image","family":"fake","supports_tools":false,"supports_streaming":false,"input_modalities":["text","image"],"output_modalities":["text"]}]})");

  std::string image_body;
  AgentServiceOptions image_options;
  image_options.agent_version = "1";
  image_options.launch_root = ava::core::normalized_absolute_path(workspace);
  image_options.paths = paths;
  image_options.provider_bundle_factory = recording_bundle_factory(&image_body);
  AgentService image_service(image_options);
  image_service.bind_update_sender([](std::string_view, std::string_view) -> ava::core::VoidResult { return {}; });
  static_cast<void>(image_service.handle_request(initialize_request(), {}));
  auto created = image_service.handle_request(
      Request{.id = std::int64_t(2), .method = "session/new", .params_json = std::string("{\"cwd\":\"") + workspace.string() + "\",\"mcpServers\":[]}"}, {});
  auto session_id = created ? ava::core::json::string_field(*created, "sessionId") : std::nullopt;
  RequestResult prompted;
  if (session_id)
  {
    prompted =
        image_service.handle_request(Request{.id = std::int64_t(3),
                                             .method = "session/prompt",
                                             .params_json = std::string("{\"sessionId\":\"") + *session_id +
                                                            "\",\"prompt\":[{\"type\":\"image\",\"data\":\"iVBORw0KGgo=\",\"mimeType\":\"image/png\"}]}"},
                                     {});
    static_cast<void>(image_service.handle_request(
        Request{.id = std::int64_t(4), .method = "session/close", .params_json = std::string("{\"sessionId\":\"") + *session_id + "\"}"}, {}));
  }
  image_service.shutdown();
  expect(session_id && prompted, "ACP image-capable fixture persists compatible image history before restart");

  ava::tests::write_app_test_file(
      paths.models_file,
      R"({"default_provider":"moonshot","default_model":"text-model","models":[{"provider":"moonshot","id":"text-model","name":"Text","family":"fake","supports_tools":false,"supports_streaming":false,"input_modalities":["text"],"output_modalities":["text"]}]})");
  std::string text_body;
  AgentServiceOptions text_options = image_options;
  text_options.provider_bundle_factory = recording_bundle_factory(&text_body);
  AgentService text_service(std::move(text_options));
  text_service.bind_update_sender([](std::string_view, std::string_view) -> ava::core::VoidResult { return {}; });
  static_cast<void>(text_service.handle_request(initialize_request(), {}));
  RequestResult resumed;
  if (session_id)
    resumed = text_service.handle_request(
        Request{.id = std::int64_t(5),
                .method = "session/resume",
                .params_json = std::string("{\"sessionId\":\"") + *session_id + "\",\"cwd\":\"" + workspace.string() + "\",\"mcpServers\":[]}"},
        {});
  RequestResult resumed_prompt;
  if (session_id && resumed)
  {
    resumed_prompt = text_service.handle_request(
        Request{.id = std::int64_t(6),
                .method = "session/prompt",
                .params_json = std::string("{\"sessionId\":\"") + *session_id + "\",\"prompt\":[{\"type\":\"text\",\"text\":\"continue without images\"}]}"},
        {});
  }
  expect(session_id && resumed, "ACP resume accepts image history under the immutable text-only startup model");
  expect(static_cast<bool>(resumed_prompt), "ACP resumed text-only session accepts a subsequent prompt");
  expect(text_body.find("historical image omitted: mime=image/png bytes=8") != std::string::npos && text_body.find("data:image/png") == std::string::npos &&
             text_body.find("attachments/") == std::string::npos,
         "ACP resumed request uses a non-identifying historical image placeholder");
  text_service.shutdown();

  std::error_code cleanup;
  std::filesystem::remove_all(root, cleanup);
}

void test_acp_session_lifecycle_real_prompt_and_provider_ownership()
{
  using namespace ava::app::acp;
  auto root = std::filesystem::temp_directory_path() / ava::core::make_id("acp-m3-test");
  auto workspace = root / "workspace";
  auto nested = workspace / "nested";
  std::filesystem::create_directories(nested);
  auto paths = ava::tests::app_test_paths(root);
  std::filesystem::create_directories(paths.ava_config_dir);
  ava::tests::write_app_test_file(
      paths.models_file,
      R"({"default_provider":"moonshot","default_model":"acp-test","models":[{"provider":"moonshot","id":"acp-test","name":"ACP Test","family":"fake","supports_tools":false,"supports_streaming":false,"input_modalities":["text"],"output_modalities":["text"]}]})");

  std::mutex ownership_mutex;
  std::size_t bundle_count = 0;
  ava::app::RuntimeProviderRunBundleFactory factory = [&](ava::app::runtime::session_ts const&, ava::app::runtime::RunOptions options,
                                                          std::string_view) -> ava::core::Result<ava::app::RuntimeProviderRunBundle> {
    auto transport = std::make_unique<ava::tests::FakeTransport>(std::vector<ava::http::HttpResponse>{ava::http::HttpResponse{
        .status_code = 200, .headers = {}, .body = R"({"choices":[{"message":{"content":"owned response"},"finish_reason":"stop"}]})"}});
    {
      std::lock_guard lock(ownership_mutex);
      ++bundle_count;
    }
    options.access_token = "fake-test-key";
    options.stream = false;
    std::unique_ptr<ava::http::Transport> auth_transport = std::make_unique<ava::tests::FakeTransport>(std::vector<ava::http::HttpResponse>{});
    std::unique_ptr<ava::http::Transport> run_transport = std::move(transport);
    auto created_provider = ava::provider::builtin_provider_registry().create("moonshot");
    if (!created_provider)
      return std::unexpected(std::move(created_provider.error()));
    std::unique_ptr<ava::provider::Provider> provider = std::move(*created_provider);
    return ava::app::RuntimeProviderRunBundle{
        .provider = std::move(provider), .transport = std::move(run_transport), .auth_transport = std::move(auth_transport), .options = std::move(options)};
  };

  AgentServiceOptions options;
  options.agent_version = "1.0.0";
  options.launch_root = ava::core::normalized_absolute_path(workspace);
  options.paths = paths;
  options.provider_bundle_factory = std::move(factory);
  AgentService service(std::move(options));
  std::vector<std::string> updates;
  service.bind_update_sender([&](std::string_view session_id, std::string_view update) -> ava::core::VoidResult {
    updates.push_back(std::string(session_id) + ":" + std::string(update));
    return {};
  });

  auto initialized = service.handle_request(Request{.id = std::int64_t(1), .method = "initialize", .params_json = std::string(R"({"protocolVersion":1})")}, {});
  expect(initialized && initialized->find(R"("image":false)") != std::string::npos && initialized->find(R"("loadSession":false)") != std::string::npos,
         "ACP M4 test service derives text-only startup capabilities from its effective default model");
  auto first = service.handle_request(
      Request{.id = std::int64_t(2), .method = "session/new", .params_json = std::string("{\"cwd\":\"") + workspace.string() + "\",\"mcpServers\":[]}"}, {});
  auto second = service.handle_request(
      Request{.id = std::int64_t(3), .method = "session/new", .params_json = std::string("{\"cwd\":\"") + nested.string() + "\",\"mcpServers\":[]}"}, {});
  auto first_id = first ? ava::core::json::string_field(*first, "sessionId") : std::nullopt;
  auto second_id = second ? ava::core::json::string_field(*second, "sessionId") : std::nullopt;
  expect(first_id && second_id && *first_id != *second_id, "ACP connection creates independent session ids");
  if (first_id && second_id)
  {
    auto image_prompt =
        service.handle_request(Request{.id = std::int64_t(30),
                                       .method = "session/prompt",
                                       .params_json = std::string("{\"sessionId\":\"") + *first_id +
                                                      "\",\"prompt\":[{\"type\":\"image\",\"data\":\"iVBORw0KGgo=\",\"mimeType\":\"image/png\"}]}"},
                               {});
    auto image_store = ava::session::SessionStore::open(workspace, *first_id, paths.sessions_dir);
    bool const attachment_storage_absent = image_store && !std::filesystem::exists(ava::session::attachment_storage_root(*image_store));
    expect(!image_prompt && image_prompt.error().code == -32602 && bundle_count == 0 && attachment_storage_absent,
           "ACP rejects image content for a text-only session before provider setup or attachment import");

    auto prompt_params = [](std::string const& id, std::string_view text) {
      return std::string("{\"sessionId\":\"") + id + "\",\"prompt\":[{\"type\":\"text\",\"text\":\"" + std::string(text) + "\"}]}";
    };
    auto first_prompt = service.handle_request(Request{.id = std::int64_t(4), .method = "session/prompt", .params_json = prompt_params(*first_id, "one")}, {});
    auto second_prompt =
        service.handle_request(Request{.id = std::int64_t(5), .method = "session/prompt", .params_json = prompt_params(*second_id, "two")}, {});
    auto prompt_detail = first_prompt ? *first_prompt : first_prompt.error().message;
    prompt_detail += " / ";
    prompt_detail += (second_prompt ? *second_prompt : second_prompt.error().message);
    expect(first_prompt && second_prompt && *first_prompt == R"({"stopReason":"end_turn"})" && *second_prompt == R"({"stopReason":"end_turn"})",
           "ACP text prompts execute through the real runtime backend: " + prompt_detail);
  }
  expect(bundle_count == 2, "each ACP prompt run creates its own provider transport bundle");
  expect(updates.size() == 2 && updates[0].find("owned response") != std::string::npos && updates[1].find("owned response") != std::string::npos,
         "ACP emits one final text update per non-streaming prompt without duplication");

  service.shutdown();
  std::error_code cleanup_error;
  std::filesystem::remove_all(root, cleanup_error);
}

void test_acp_exact_identity_persisted_cwd_and_restart()
{
  using namespace ava::app::acp;
  auto root = std::filesystem::temp_directory_path() / ava::core::make_id("acp-identity");
  auto workspace = root / "workspace";
  auto nested = workspace / "nested";
  std::filesystem::create_directories(nested);
  configure_acp_test_model(root);
  auto paths = ava::tests::app_test_paths(root);

  AgentServiceOptions options;
  options.agent_version = "1";
  options.launch_root = ava::core::normalized_absolute_path(workspace);
  options.paths = paths;
  AgentService first(options);
  expect(first.handle_request(initialize_request(), {}).has_value(), "ACP cwd test initializes first host");
  auto created = first.handle_request(
      Request{.id = std::int64_t(2), .method = "session/new", .params_json = std::string("{\"cwd\":\"") + nested.string() + "\",\"mcpServers\":[]}"}, {});
  auto id = created ? ava::core::json::string_field(*created, "sessionId") : std::nullopt;
  expect(id.has_value(), "ACP cwd test creates a nested-cwd session");
  if (!id)
    return;
  auto listed = first.handle_request(Request{.id = std::int64_t(3), .method = "session/list", .params_json = std::string("{}")}, {});
  expect(listed && listed->find(nested.string()) != std::string::npos, "session/list reports persisted original cwd rather than launch root");
  expect(first.handle_request(Request{.id = std::int64_t(4), .method = "session/close", .params_json = std::string("{\"sessionId\":\"") + *id + "\"}"}, {})
             .has_value(),
         "ACP cwd test closes first host");
  first.shutdown();

  auto store = ava::session::SessionStore::open(options.launch_root, *id, paths.sessions_dir);
  auto summary = store ? store->inspect_bounded(kAcpSessionReadLimits) : ava::core::Result<ava::session::SessionSummary>(std::unexpected(store.error()));
  expect(summary && summary->original_cwd == nested, "canonical original cwd persists in protocol-neutral session metadata");

  AgentService second(options);
  static_cast<void>(second.handle_request(initialize_request(), {}));
  auto prefix = id->substr(0, id->size() - 2);
  auto load = second.handle_request(Request{.id = std::int64_t(5), .method = "session/load", .params_json = std::string("{}")}, {});
  expect(!load && load.error().code == -32601, "ACP rejects unadvertised session/load rather than returning partial rich history");
  auto prefix_resume = second.handle_request(Request{.id = std::int64_t(50),
                                                     .method = "session/resume",
                                                     .params_json = std::string("{\"sessionId\":\"") + prefix + "\",\"cwd\":\"" + nested.string() + "\"}"},
                                             {});
  expect(!prefix_resume && prefix_resume.error().code == -32002, "ACP exact resume lookup rejects a unique CLI-style id prefix without scanning");
  auto mismatch = second.handle_request(Request{.id = std::int64_t(6),
                                                .method = "session/resume",
                                                .params_json = std::string("{\"sessionId\":\"") + *id + "\",\"cwd\":\"" + workspace.string() + "\"}"},
                                        {});
  expect(!mismatch && mismatch.error().code == -32602, "ACP load/resume rejects client-selected cwd drift after restart");
  auto resumed = second.handle_request(
      Request{
          .id = std::int64_t(7), .method = "session/resume", .params_json = std::string("{\"sessionId\":\"") + *id + "\",\"cwd\":\"" + nested.string() + "\"}"},
      {});
  expect(resumed.has_value(), "ACP resumes exact id when requested cwd matches persisted cwd");
  second.shutdown();

  auto const oversized_path = store->session_path();
  std::string const oversized_bytes(kAcpSessionReadLimits.max_file_bytes + 1, 'x');
  {
    std::ofstream file(oversized_path, std::ios::binary | std::ios::trunc);
    file.write(oversized_bytes.data(), static_cast<std::streamsize>(oversized_bytes.size()));
  }
  AgentService bounded(options);
  static_cast<void>(bounded.handle_request(initialize_request(), {}));
  auto oversized_resume = bounded.handle_request(
      Request{
          .id = std::int64_t(8), .method = "session/resume", .params_json = std::string("{\"sessionId\":\"") + *id + "\",\"cwd\":\"" + nested.string() + "\"}"},
      {});
  bounded.shutdown();
  std::ifstream oversized_file(oversized_path, std::ios::binary);
  std::string oversized_after((std::istreambuf_iterator<char>(oversized_file)), std::istreambuf_iterator<char>());
  bool quarantine_exists = false;
  auto const quarantine_prefix = oversized_path.filename().string() + ".torn-tail.";
  std::error_code quarantine_iter_error;
  for (std::filesystem::directory_iterator iterator(oversized_path.parent_path(), quarantine_iter_error), end; !quarantine_iter_error && iterator != end;
       iterator.increment(quarantine_iter_error))
  {
    quarantine_exists = quarantine_exists || iterator->path().filename().string().starts_with(quarantine_prefix);
  }
  expect(!oversized_resume && oversized_after == oversized_bytes && !quarantine_exists,
         "ACP session/resume passes bounded recovery limits and rejects an oversized file unchanged without quarantine");

  std::error_code cleanup;
  std::filesystem::remove_all(root, cleanup);
}

void test_acp_cross_process_lease_and_bounded_streaming()
{
  auto root = std::filesystem::temp_directory_path() / ava::core::make_id("acp-lease");
  auto workspace = root / "workspace";
  std::filesystem::create_directories(workspace);
  auto store = ava::session::SessionStore::create(workspace, root / "sessions");
  expect(store.has_value(), "lease test creates store");
  if (!store)
    return;
  static_cast<void>(append_session_entry_for_test(*store, ava::session::SessionEntry{.id = ava::core::make_id("entry"),
                                                                                     .parent_id = "",
                                                                                     .type = ava::session::EntryType::SessionStart,
                                                                                     .timestamp = ava::session::now_timestamp(),
                                                                                     .data_json = "{}"}));
  auto lease = ava::session::SessionLease::acquire(store->session_path());
  auto same_process = ava::session::SessionLease::acquire(store->session_path());
  expect(lease && !same_process && same_process.error().message().find("already owned") != std::string::npos,
         "session lease excludes a second owner in the same process with an actionable error");
  pid_t child = fork();
  if (child == 0)
  {
    auto contested = ava::session::SessionLease::acquire(store->session_path());
    _exit(!contested && contested.error().message().find("already owned") != std::string::npos ? 0 : 1);
  }
  int status = 0;
  static_cast<void>(waitpid(child, &status, 0));
  expect(WIFEXITED(status) && WEXITSTATUS(status) == 0, "session lease excludes a second AVA process for the host lifetime");

  if (lease)
  {
    static_cast<void>(store->append(*lease, ava::session::SessionEntry{.id = ava::core::make_id("entry"),
                                                                       .parent_id = "",
                                                                       .type = ava::session::EntryType::UserMessage,
                                                                       .timestamp = ava::session::now_timestamp(),
                                                                       .data_json = "{\"text\":\"0123456789abcdef\"}"}));
  }
  auto bounded = store->load_bounded(ava::session::SessionReadLimits{.max_file_bytes = 32, .max_line_bytes = 32, .max_entries = 2});
  expect(!bounded && bounded.error().message().find("bounded") != std::string::npos,
         "bounded streaming session open rejects an oversized transcript without unbounded allocation");
  auto recovered = store->load_bounded(ava::session::SessionReadLimits{.max_file_bytes = 4096, .max_line_bytes = 2048, .max_entries = 8});
  expect(recovered && recovered->size() == 2, "bounded session reader recovers on a later request with valid budgets");
  std::error_code cleanup;
  std::filesystem::remove_all(root, cleanup);
}

void test_acp_list_pagination_cancel_race_stop_reasons_and_file_safety()
{
  using namespace ava::app::acp;
  auto root = std::filesystem::temp_directory_path() / ava::core::make_id("acp-pagination");
  auto workspace = root / "workspace";
  std::filesystem::create_directories(workspace);
  configure_acp_test_model(root);
  auto paths = ava::tests::app_test_paths(root);
  for (int index = 0; index < 55; ++index)
  {
    auto store = ava::session::SessionStore::create(workspace, paths.sessions_dir);
    if (!store)
      continue;
    static_cast<void>(append_session_entry_for_test(
        *store, ava::session::SessionEntry{.id = ava::core::make_id("entry"),
                                           .parent_id = "",
                                           .type = ava::session::EntryType::SessionStart,
                                           .timestamp = "2026-07-12T12:" + std::to_string(index / 10) + std::to_string(index % 10) + ":00Z",
                                           .data_json = "{\"original_cwd\":\"" + ava::core::json::escape(workspace.string()) + "\"}"}));
  }
  std::string request_body;
  AgentServiceOptions options;
  options.agent_version = "1";
  options.launch_root = ava::core::normalized_absolute_path(workspace);
  options.paths = paths;
  options.provider_bundle_factory = recording_bundle_factory(&request_body);
  AgentService service(options);
  service.bind_update_sender([](std::string_view, std::string_view) -> ava::core::VoidResult { return {}; });
  static_cast<void>(service.handle_request(initialize_request(), {}));
  auto first_page = service.handle_request(Request{.id = std::int64_t(2), .method = "session/list", .params_json = std::string("{}")}, {});
  auto const first_items = first_page ? ava::core::json::objects_in_array_field(*first_page, "sessions") : std::vector<std::string>{};
  auto cursor = first_page ? ava::core::json::string_field(*first_page, "nextCursor").value_or("") : "";
  expect(first_page && first_items.size() == kSessionListPageSize && !cursor.empty() && first_page->size() < kMaxRecordBytes,
         "session/list returns a bounded first page and pinned-schema nextCursor");
  auto second_page =
      service.handle_request(Request{.id = std::int64_t(3), .method = "session/list", .params_json = std::string("{\"cursor\":\"") + cursor + "\"}"}, {});
  auto const second_items = second_page ? ava::core::json::objects_in_array_field(*second_page, "sessions") : std::vector<std::string>{};
  expect(second_page && second_items.size() == 5 && !ava::core::json::string_field(*second_page, "nextCursor"),
         "session/list cursor yields a stable bounded remainder page");
  auto invalid_cursor =
      service.handle_request(Request{.id = std::int64_t(4), .method = "session/list", .params_json = std::string(R"({"cursor":"v2:forged"})")}, {});
  expect(!invalid_cursor && invalid_cursor.error().code == -32602, "session/list rejects forged or unsupported cursor state");

  for (int index = 0; index < 2000; ++index)
  {
    auto store = ava::session::SessionStore::create(workspace, paths.sessions_dir);
    if (!store)
      continue;
    static_cast<void>(append_session_entry_for_test(
        *store, ava::session::SessionEntry{.id = ava::core::make_id("entry"),
                                           .parent_id = "",
                                           .type = ava::session::EntryType::SessionStart,
                                           .timestamp = ava::session::now_timestamp(),
                                           .data_json = "{\"original_cwd\":\"" + ava::core::json::escape(workspace.string()) + "\"}"}));
    static_cast<void>(append_session_metadata_for_test(
        *store, ava::session::SessionMetadataUpdate{.name = std::optional<std::string>(std::string(256, 't')), .actor = "test"}));
  }
  std::vector<std::string> retained_cursors;
  for (int index = 0; index < 12; ++index)
  {
    auto listed = service.handle_request(Request{.id = std::int64_t(100 + index), .method = "session/list", .params_json = std::string("{}")}, {});
    auto token = listed ? ava::core::json::string_field(*listed, "nextCursor") : std::nullopt;
    if (token)
      retained_cursors.push_back(*token);
  }
  auto evicted =
      retained_cursors.empty()
          ? RequestResult(std::unexpected(JsonRpcError{}))
          : service.handle_request(
                Request{.id = std::int64_t(200), .method = "session/list", .params_json = std::string("{\"cursor\":\"") + retained_cursors.front() + "\"}"},
                {});
  auto newest =
      retained_cursors.empty()
          ? RequestResult(std::unexpected(JsonRpcError{}))
          : service.handle_request(
                Request{.id = std::int64_t(201), .method = "session/list", .params_json = std::string("{\"cursor\":\"") + retained_cursors.back() + "\"}"}, {});
  expect(retained_cursors.size() == 12 && !evicted && evicted.error().code == -32602 && newest,
         "session/list enforces aggregate snapshot bytes with deterministic oldest-first eviction and invalidates evicted cursors: cursors=" +
             std::to_string(retained_cursors.size()) + " evicted=" + (evicted ? std::string("success") : std::to_string(evicted.error().code)) +
             " newest=" + (newest ? std::string("success") : std::to_string(newest.error().code)));

  auto secret = workspace / "secret.txt";
  ava::tests::write_app_test_file(secret, "MUST_NOT_ENTER_PROVIDER_CONTEXT");
  auto created = service.handle_request(
      Request{.id = std::int64_t(5), .method = "session/new", .params_json = std::string("{\"cwd\":\"") + workspace.string() + "\",\"mcpServers\":[]}"}, {});
  auto id = created ? ava::core::json::string_field(*created, "sessionId") : std::nullopt;
  if (id)
  {
    service.handle_notification(Notification{.method = "session/cancel", .params_json = std::string("{\"sessionId\":\"") + *id + "\"}"}, {});
    auto canceled = service.handle_request(
        Request{.id = std::int64_t(6),
                .method = "session/prompt",
                .params_json = std::string("{\"sessionId\":\"") + *id + "\",\"prompt\":[{\"type\":\"text\",\"text\":\"queued cancel\"}]}"},
        {});
    expect(canceled && *canceled == R"({"stopReason":"end_turn"})" && !request_body.empty(),
           std::string("idle session/cancel is a no-op and cannot cancel a future prompt: ") + (canceled ? *canceled : canceled.error().message) +
               " body=" + request_body.substr(0, 128));
    auto prompt =
        service.handle_request(Request{.id = std::int64_t(7),
                                       .method = "session/prompt",
                                       .params_json = std::string("{\"sessionId\":\"") + *id + "\",\"prompt\":[{\"type\":\"text\",\"text\":\"@secret.txt\"}]}"},
                               {});
    auto detail = std::string("ACP prompt preserves @file as literal text and performs no implicit local file read: ") +
                  (prompt ? *prompt : prompt.error().message) + " body=" + request_body.substr(0, 512);
    expect(request_body.find("@secret.txt") != std::string::npos && request_body.find("MUST_NOT_ENTER_PROVIDER_CONTEXT") == std::string::npos, detail);
  }
  bool outcomes_exhaustive = true;
  for (auto const& entry : ava::core::kRuntimeTerminalOutcomeCatalog)
  {
    auto mapped = acp_stop_reason(entry.outcome);
    std::string_view expected;
    switch (entry.outcome)
    {
      case ava::core::RuntimeTerminalOutcome::Completed:
        expected = "end_turn";
        break;
      case ava::core::RuntimeTerminalOutcome::MaxTokens:
        expected = "max_tokens";
        break;
      case ava::core::RuntimeTerminalOutcome::MaxTurnRequests:
        expected = "max_turn_requests";
        break;
      case ava::core::RuntimeTerminalOutcome::Refusal:
        expected = "refusal";
        break;
      case ava::core::RuntimeTerminalOutcome::Cancelled:
        expected = "cancelled";
        break;
      case ava::core::RuntimeTerminalOutcome::Error:
        break;
    }
    outcomes_exhaustive = outcomes_exhaustive && (entry.outcome == ava::core::RuntimeTerminalOutcome::Error ? !mapped : mapped && *mapped == expected);
  }
  expect(outcomes_exhaustive, "ACP exhaustively maps the closed protocol-neutral runtime outcome catalog");
  service.shutdown();
  std::error_code cleanup;
  std::filesystem::remove_all(root, cleanup);
}

void test_acp_session_cwd_allows_symlinked_workspace_path()
{
  using namespace ava::app::acp;
  auto const root = ScopedTestDirectory::create_unique(temp_root(), "acp-symlinked-cwd-");
  // Mirror a workspace configured through an absolute symlink, e.g.
  // /home/user/projects/github -> /usr/src/projects_github.
  auto const real_target = root / "real-target";
  auto const projects = root / "projects";
  auto const real_workspace = real_target / "ai-cli" / "AVA";
  std::filesystem::create_directories(real_workspace);
  std::filesystem::create_directories(projects);
  std::error_code link_error;
  auto const workspace_via_link = projects / "linked-workspace";
  std::filesystem::create_directory_symlink(real_workspace, workspace_via_link, link_error);
  expect(!link_error, "test creates a launch workspace whose configured root is an absolute symlink");
  if (link_error)
    return;

  configure_acp_test_model(root);
  auto const paths = ava::tests::app_test_paths(root);
  AgentServiceOptions options;
  options.agent_version = "1";
  options.launch_root = workspace_via_link;
  options.paths = paths;
  AgentService service(options);
  service.bind_request_terminal_committer([](JsonRpcId const&) { return true; });
  static_cast<void>(service.handle_request(initialize_request(), {}));

  // A session cwd whose path traverses the absolute symlink must be accepted;
  // only the resolved location must stay within the launch-approved root.
  auto created = service.handle_request(
      Request{
          .id = std::int64_t(1), .method = "session/new", .params_json = std::string("{\"cwd\":\"") + workspace_via_link.string() + "\",\"mcpServers\":[]}"},
      {});
  auto session_id = created ? ava::core::json::string_field(*created, "sessionId") : std::nullopt;
  expect(session_id.has_value(), "session/new accepts a cwd whose path traverses an absolute symlink");

  // Containment is still enforced against the resolved launch root.
  auto const outside = root / "outside";
  std::filesystem::create_directories(outside);
  auto outside_new = service.handle_request(
      Request{.id = std::int64_t(2), .method = "session/new", .params_json = std::string("{\"cwd\":\"") + outside.string() + "\",\"mcpServers\":[]}"}, {});
  expect(!outside_new && outside_new.error().code == -32602, "session/new still rejects a cwd outside the launch root");

  auto const escaped_nested = outside / "nested";
  std::filesystem::create_directories(escaped_nested);
  std::filesystem::create_directory_symlink(outside, real_workspace / "escape", link_error);
  if (!link_error)
  {
    auto const escaped_cwd = workspace_via_link / "escape" / "nested";
    auto persisted_before_escape = ava::session::SessionStore::list_sessions(workspace_via_link, paths.sessions_dir);
    auto escaped_new = service.handle_request(
        Request{.id = std::int64_t(20), .method = "session/new", .params_json = std::string("{\"cwd\":\"") + escaped_cwd.string() + "\",\"mcpServers\":[]}"},
        {});
    auto escaped_list = service.handle_request(
        Request{.id = std::int64_t(21), .method = "session/list", .params_json = std::string("{\"cwd\":\"") + escaped_cwd.string() + "\"}"}, {});
    auto persisted = ava::session::SessionStore::list_sessions(workspace_via_link, paths.sessions_dir);
    expect(!escaped_new && escaped_new.error().code == -32602 && !escaped_list && escaped_list.error().code == -32602,
           "session/new and session/list reject an intermediate-symlink cwd escape as invalid parameters");
    expect(persisted_before_escape && persisted && persisted->size() == persisted_before_escape->size(),
           "session/new intermediate-symlink cwd escape creates no persistent session");

    if (session_id)
    {
      auto escaped_resume = service.handle_request(
          Request{.id = std::int64_t(22),
                  .method = "session/resume",
                  .params_json = std::string("{\"sessionId\":\"") + *session_id + "\",\"cwd\":\"" + escaped_cwd.string() + "\",\"mcpServers\":[]}"},
          {});
      expect(!escaped_resume && escaped_resume.error().code == -32602, "session/resume rejects an escaped cwd before the retained-session fast path");
      static_cast<void>(service.handle_request(
          Request{.id = std::int64_t(23), .method = "session/close", .params_json = std::string("{\"sessionId\":\"") + *session_id + "\"}"}, {}));
      auto contained_resume = service.handle_request(
          Request{.id = std::int64_t(24),
                  .method = "session/resume",
                  .params_json = std::string("{\"sessionId\":\"") + *session_id + "\",\"cwd\":\"" + workspace_via_link.string() + "\",\"mcpServers\":[]}"},
          {});
      expect(contained_resume.has_value(), "an escaped retained-session resume does not prevent a later contained resume");
    }
  }

  auto const contained = real_workspace / "contained";
  std::filesystem::create_directories(contained);
  link_error.clear();
  std::filesystem::create_directory_symlink("contained", real_workspace / "final-link", link_error);
  if (!link_error)
  {
    auto linked_new =
        service.handle_request(Request{.id = std::int64_t(3),
                                       .method = "session/new",
                                       .params_json = std::string("{\"cwd\":\"") + (workspace_via_link / "final-link").string() + "\",\"mcpServers\":[]}"},
                               {});
    expect(!linked_new && linked_new.error().code == -32602, "session/new rejects a final cwd symlink beneath the launch root");
  }
}

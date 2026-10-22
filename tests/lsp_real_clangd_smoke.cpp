#include "sys.h"
#include "tests/support/session_test_support.h"
#include "tests/support/test_env.h"
#include "ava/lsp/builtin_recipes.h"
#include "ava/lsp/lsp_client.h"
#include "ava/core/AnchorSet.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <string_view>

int main()
{
  auto const* opt_in = std::getenv("AVA_LSP_REAL_CLANGD_SMOKE");
  if (opt_in == nullptr || std::string_view(opt_in) != "1")
  {
    ava::tests::request_skip("real clangd smoke requires AVA_LSP_REAL_CLANGD_SMOKE=1");
    return 77;
  }

  ava::tests::HermeticTestEnvironment environment("ava-clangd-smoke-");
  auto const fixture = environment.root() / "workspace";
  std::filesystem::create_directory(fixture);
  auto const source_path = fixture / "main.cpp";
  ava::tests::write_test_file(source_path, "int helper() { return 1; }\nint main() {\n  return helper();\n}\n");
  auto anchors = ava::core::AnchorSet::open({fixture});
  if (!ava::tests::require(anchors.has_value(), anchors ? "clangd smoke anchor opens" : "clangd smoke anchor opens: " + anchors.error().format()))
    return 1;
  auto inspections = ava::lsp::inspect_builtin_servers({"clangd"}, fixture, *anchors);
  auto const clangd = std::ranges::find_if(inspections, [](auto const& item) { return item.id == "clangd"; });
  if (clangd == inspections.end() || clangd->status != ava::lsp::BuiltinServerStatus::Available || !clangd->executable)
  {
    ava::tests::request_skip("real clangd smoke requires an available owner-safe clangd executable");
    return 77;
  }

  auto client = ava::lsp::SubprocessLspClient::start({
      .argv = {clangd->executable->executable_path.string(), "--background-index"},
      .workspace_root = fixture,
      .server_root = fixture,
      .anchor_set = *anchors,
      .process_cwd = fixture,
      .startup_timeout = std::chrono::milliseconds(5000),
      .request_timeout = std::chrono::milliseconds(5000),
      .language_id = "cpp",
      .executable_identity = clangd->executable,
  });
  if (!ava::tests::require(client.has_value(), client ? "clangd initializes" : "clangd initializes: " + client.error().format()))
    return 1;
  auto definitions = (*client)->definitions(source_path, 2, 9);
  if (!ava::tests::require(definitions && !definitions->empty(),
                           definitions ? "clangd returns a helper definition" : "clangd returns a helper definition: " + definitions.error().format()))
    return 1;
  ava::tests::expect_eq(definitions->front().path, source_path, "clangd resolves the helper in the source file");
  ava::tests::expect_eq(definitions->front().range.start_line, 0, "clangd resolves the declaration on line zero");
  ava::tests::expect_eq(definitions->front().range.start_column, 4, "clangd resolves the helper declaration, not its callsite");
  client->reset();
  return ava::tests::failures() == 0 ? 0 : 1;
}

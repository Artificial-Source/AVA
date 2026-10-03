#include "ava/core/json.h"

#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unistd.h>

namespace {

std::optional<std::string> read_message()
{
  std::string line;
  if (!std::getline(std::cin, line))
    return std::nullopt;
  if (!line.empty() && line.back() == '\r')
    line.pop_back();
  return line;
}

void write_message(std::string_view body)
{
  std::cout << body << '\n';
  std::cout.flush();
}

std::string json_string(std::string_view value)
{
  return "\"" + ava::core::json::escape(value) + "\"";
}

std::string response(std::string_view id, std::string_view result)
{
  return "{\"jsonrpc\":\"2.0\",\"id\":" + json_string(id) + ",\"result\":" + std::string(result) + "}";
}

std::string error_response(std::string_view id, std::string_view message)
{
  return "{\"jsonrpc\":\"2.0\",\"id\":" + json_string(id) + ",\"error\":{\"code\":-32000,\"message\":" + json_string(message) + "}}";
}

void write_process_group_marker(std::string const& path)
{
  if (path.empty())
    return;
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  file << static_cast<long long>(getpgrp()) << '\n';
}

void write_environment_marker(std::string const& path)
{
  if (path.empty())
    return;
  auto value = [](char const* name) {
    auto const* found = std::getenv(name);
    return found ? std::string(found) : std::string("<unset>");
  };
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  file << "EXPLICIT=" << value("AVA_MCP_EXPLICIT") << '\n';
  file << "INHERITED=" << value("AVA_MCP_PARENT_MARKER") << '\n';
  file << "SECRET=" << value("AVA_MCP_PARENT_SECRET") << '\n';
  file << "PATH=" << value("PATH") << '\n';
}

void append_event_log(std::string_view event)
{
  char const* path = std::getenv("AVA_FAKE_MCP_EVENT_LOG");
  if (path == nullptr || path[0] == '\0')
    return;
  std::ofstream file(path, std::ios::binary | std::ios::app);
  file << event << '\n';
}

void write_cwd_marker(std::string const& path)
{
  if (path.empty())
    return;
  // Prefer $PWD (logical path preserved by the shell) over getcwd() (physical path).
  char const* pwd = std::getenv("PWD");
  std::string cwd;
  if (pwd != nullptr && pwd[0] == '/')
    cwd = pwd;
  else
  {
    char buffer[4096]{};
    if (getcwd(buffer, sizeof(buffer)) == nullptr)
      return;
    cwd = buffer;
  }
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  file << cwd << '\n';
}

}  // namespace

int main(int argc, char** argv)
{
  std::string mode = "ok";
  if (argc > 1)
    mode = argv[1];
  std::string const marker_path = argc > 2 ? argv[2] : "";
  append_event_log("launch");
  if (mode == "env-marker")
    write_environment_marker(marker_path);
  if (mode == "stderr-noise")
  {
    std::cerr << std::string(96, 'x') << "CANARY_MCP_STDERR_19d4";
    std::cerr.flush();
  }

  while (auto message = read_message())
  {
    auto const method = ava::core::json::string_field(*message, "method");
    auto const id = ava::core::json::string_field(*message, "id");
    if (!method || !id)
      continue;
    append_event_log(*method);

    if ((mode == "timeout-initialize" || mode == "timeout-initialize-marker") && *method == "initialize")
    {
      write_process_group_marker(marker_path);
      std::this_thread::sleep_for(std::chrono::seconds(2));
      continue;
    }
    if (mode == "exit-initialize" && *method == "initialize")
    {
      std::cerr << "CANARY_MCP_STDERR_INIT_a138";
      std::cerr.flush();
      return 42;
    }
    if (mode == "malformed" && *method == "initialize")
    {
      write_message("not-json");
      continue;
    }
    if (mode == "duplicate-initialize" && *method == "initialize")
    {
      write_message("{\"jsonrpc\":\"2.0\",\"id\":" + json_string(*id) +
                    ",\"result\":{},\"result\":{\"protocolVersion\":\"2024-11-05\",\"capabilities\":{},"
                    "\"serverInfo\":{\"name\":\"fake-mcp\",\"version\":\"1.0.0\"}}}");
      continue;
    }

    if (*method == "initialize")
    {
      if (mode == "cwd-marker")
        write_cwd_marker(marker_path);
      auto const protocol_version = mode == "mismatched-version" ? std::string("\"protocolVersion\":\"2025-03-26\",")
                                    : mode == "missing-version"  ? std::string{}
                                                                 : std::string("\"protocolVersion\":\"2024-11-05\",");
      auto const capabilities =
          mode == "missing-capabilities" ? std::string{} : std::string("\"capabilities\":{\"tools\":{},\"prompts\":{},\"resources\":{}},");
      if (mode == "close-stdin-after-initialize")
      {
        close(STDIN_FILENO);
        write_message(response(*id, "{" + protocol_version + capabilities + "\"serverInfo\":{\"name\":\"fake-mcp\",\"version\":\"1.0.0\"}}"));
        for (;;)
          pause();
      }
      write_message(response(*id, "{" + protocol_version + capabilities + "\"serverInfo\":{\"name\":\"fake-mcp\",\"version\":\"1.0.0\"}}"));
    }
    else if (*method == "tools/list")
    {
      if (mode == "exit-after-initialize")
      {
        std::cerr << "CANARY_MCP_STDERR_LIST_4b72";
        std::cerr.flush();
        return 43;
      }
      auto const params = ava::core::json::object_field(*message, "params").value_or("{}");
      bool const has_cursor = ava::core::json::field_value_start(params, "cursor").has_value();
      auto const cursor = ava::core::json::string_field(params, "cursor").value_or("");
      if (mode == "missing-tools")
      {
        write_message(response(*id, "{\"notTools\":[]}"));
      }
      else if (mode == "empty-cursor-tools" && !has_cursor)
      {
        write_message(response(*id,
                               "{\"tools\":[{\"name\":\"echo\",\"description\":\"Echo test tool\","
                               "\"inputSchema\":{\"type\":\"object\"}}],\"nextCursor\":\"\"}"));
      }
      else if (mode == "empty-cursor-tools" && has_cursor && cursor.empty())
      {
        write_message(response(*id,
                               "{\"tools\":[{\"name\":\"second\",\"description\":\"Second test tool\","
                               "\"inputSchema\":{\"type\":\"object\"}}]}"));
      }
      else if (mode == "paginated-tools" && cursor.empty())
      {
        write_message(response(*id,
                               "{\"tools\":[{\"name\":\"echo\",\"description\":\"Echo test tool\","
                               "\"inputSchema\":{\"type\":\"object\"}}],\"nextCursor\":\"page-2\"}"));
      }
      else if (mode == "paginated-tools" && cursor == "page-2")
      {
        write_message(response(*id,
                               "{\"tools\":[{\"name\":\"second\",\"description\":\"Second test tool\","
                               "\"inputSchema\":{\"type\":\"object\"}}]}"));
      }
      else
      {
        write_message(response(*id,
                               "{\"tools\":[{\"name\":\"echo\",\"description\":\"Echo test tool\","
                               "\"inputSchema\":{\"type\":\"object\",\"properties\":{\"text\":{\"type\":"
                               "\"string\"}},\"required\":[\"text\"]}}]}"));
      }
    }
    else if (*method == "tools/call")
    {
      if (mode == "exit-tool")
      {
        std::cerr << "CANARY_MCP_STDERR_CALL_0c95";
        std::cerr.flush();
        return 44;
      }
      if (mode == "error-call")
      {
        write_message(error_response(*id, "CANARY_MCP_RESPONSE_ERROR_f2a7"));
      }
      else if (mode == "tool-error")
      {
        write_message(response(*id, "{\"isError\":true,\"content\":[{\"type\":\"text\",\"text\":\"CANARY_MCP_TOOL_CONTENT_682e\"}]}"));
      }
      else if (mode == "tool-error-canceled-text")
      {
        write_message(response(*id,
                               "{\"isError\":true,\"content\":[{\"type\":\"text\",\"text\":\"job was "
                               "canceled upstream\"}]}"));
      }
      else if (mode == "slow-tool" || mode == "slow-tool-marker")
      {
        write_process_group_marker(marker_path);
        std::this_thread::sleep_for(std::chrono::seconds(2));
        write_message(response(*id,
                               "{\"isError\":false,\"content\":[{\"type\":\"text\",\"text\":\"MCP slow "
                               "call ok\"}]}"));
      }
      else
      {
        write_message(response(*id,
                               "{\"isError\":false,\"content\":[{\"type\":\"text\",\"text\":\"MCP call "
                               "ok\"}]}"));
      }
    }
    else if (*method == "prompts/list")
    {
      if (mode == "slow-prompts-list" || mode == "slow-prompts-list-marker")
      {
        write_process_group_marker(marker_path);
        std::this_thread::sleep_for(std::chrono::seconds(2));
      }
      auto const params = ava::core::json::object_field(*message, "params").value_or("{}");
      auto const cursor = ava::core::json::string_field(params, "cursor").value_or("");
      if (mode == "missing-prompts")
      {
        write_message(response(*id, "{\"notPrompts\":[]}"));
      }
      else if (mode == "paginated-prompts" && cursor.empty())
      {
        write_message(response(*id,
                               "{\"prompts\":[{\"name\":\"release-notes\",\"description\":\"Draft release notes\","
                               "\"arguments\":[{\"name\":\"topic\",\"required\":true}]}],\"nextCursor\":\"page-2\"}"));
      }
      else if (mode == "paginated-prompts" && cursor == "page-2")
      {
        write_message(response(*id, "{\"prompts\":[{\"name\":\"second-prompt\",\"description\":\"Second prompt\"}]}"));
      }
      else
      {
        write_message(response(*id,
                               "{\"prompts\":[{\"name\":\"release-notes\",\"description\":\"Draft release notes\","
                               "\"arguments\":[{\"name\":\"topic\",\"description\":\"Release topic\","
                               "\"required\":true}]}]}"));
      }
    }
    else if (*method == "prompts/get")
    {
      if (mode == "slow-prompt" || mode == "slow-prompt-marker")
      {
        write_process_group_marker(marker_path);
        std::this_thread::sleep_for(std::chrono::seconds(2));
      }
      auto const params = ava::core::json::object_field(*message, "params").value_or("{}");
      auto const arguments = ava::core::json::object_field(params, "arguments").value_or("{}");
      auto const topic = ava::core::json::string_field(arguments, "topic").value_or("unknown");
      write_message(
          response(*id, "{\"messages\":[{\"role\":\"user\",\"content\":{\"type\":\"text\",\"text\":" + json_string("MCP prompt for " + topic) + "}}]}"));
    }
    else if (*method == "resources/list")
    {
      if (mode == "exit-resources-list")
      {
        std::cerr << "CANARY_MCP_STDERR_RESOURCES_31e6";
        std::cerr.flush();
        return 45;
      }
      auto const params = ava::core::json::object_field(*message, "params").value_or("{}");
      auto const cursor = ava::core::json::string_field(params, "cursor").value_or("");
      if (mode == "missing-resources")
      {
        write_message(response(*id, "{\"notResources\":[]}"));
      }
      else if (mode == "paginated-resources" && cursor.empty())
      {
        write_message(response(*id,
                               "{\"resources\":[{\"uri\":\"file:///workspace/one.md\",\"name\":\"one\","
                               "\"mimeType\":\"text/markdown\"}],\"nextCursor\":\"page-2\"}"));
      }
      else if (mode == "paginated-resources" && cursor == "page-2")
      {
        write_message(response(*id,
                               "{\"resources\":[{\"uri\":\"file:///workspace/two.md\",\"name\":\"two\","
                               "\"mimeType\":\"text/markdown\"}]}"));
      }
      else
      {
        write_message(response(*id,
                               "{\"resources\":[{\"uri\":\"file:///workspace/notes.md\",\"name\":\"project-notes\","
                               "\"description\":\"Project notes resource\",\"mimeType\":\"text/markdown\"}]}"));
      }
    }
    else if (*method == "resources/read")
    {
      if (mode == "slow-resource" || mode == "slow-resource-marker")
      {
        write_process_group_marker(marker_path);
        std::this_thread::sleep_for(std::chrono::seconds(2));
      }
      auto const params = ava::core::json::object_field(*message, "params").value_or("{}");
      auto const uri = ava::core::json::string_field(params, "uri").value_or("");
      if (mode == "resource-blob")
      {
        write_message(response(*id, "{\"contents\":[{\"uri\":" + json_string(uri) + ",\"mimeType\":\"application/octet-stream\",\"blob\":\"ZmFrZQ==\"}]}"));
      }
      else if (mode == "resource-missing-contents")
      {
        write_message(response(*id, "{\"notContents\":[]}"));
      }
      else
      {
        write_message(response(*id, "{\"contents\":[{\"uri\":" + json_string(uri) + ",\"mimeType\":\"text/markdown\",\"text\":\"MCP resource content\"}]}"));
      }
    }
    else
    {
      write_message(error_response(*id, "unsupported method"));
    }
  }
  return 0;
}

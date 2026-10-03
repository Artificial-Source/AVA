#include "process_gate.h"

#include <array>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {

constexpr char process_gate_fd_environment[] = "AVA_TEST_CONTROL_FD";

class Fd
{
 public:
  explicit Fd(int fd = -1) : fd_(fd) { }
  Fd(Fd const&) = delete;
  Fd& operator=(Fd const&) = delete;
  Fd(Fd&& other) noexcept : fd_(std::exchange(other.fd_, -1)) { }
  Fd& operator=(Fd&& other) noexcept
  {
    if (this != &other)
    {
      close();
      fd_ = std::exchange(other.fd_, -1);
    }
    return *this;
  }
  ~Fd() { close(); }

  [[nodiscard]] int get() const noexcept { return fd_; }

 private:
  void close() noexcept
  {
    if (fd_ >= 0)
      static_cast<void>(::close(fd_));
    fd_ = -1;
  }

  int fd_ = -1;
};

std::string errno_text()
{
  return std::strerror(errno);
}

// Adopt the optional harness control descriptor supplied to this directly launched fixture.
//
// The environment communicates only the inherited descriptor number. Absence leaves standalone fake-provider launches
// unchanged; malformed values fail before the provider publishes its listening port.
std::optional<ava::test::ProcessGateSet> process_gates_from_environment()
{
  char const* value = std::getenv(process_gate_fd_environment);
  if (value == nullptr || *value == '\0')
    return std::nullopt;
  int descriptor = -1;
  char const* end = value + std::strlen(value);
  auto const [parsed_end, error] = std::from_chars(value, end, descriptor);
  if (error != std::errc{} || parsed_end != end || descriptor < 0)
    throw std::runtime_error(std::string(process_gate_fd_environment) + " must contain a nonnegative inherited descriptor");
  return ava::test::ProcessGateSet(descriptor);
}

// Wait for one harness-owned release gate and convert protocol failures into fixture diagnostics.
bool wait_for_process_gate(ava::test::ProcessGateSet& process_gates, std::size_t gate, std::string_view operation)
{
  try
  {
    process_gates.wait(gate, std::chrono::seconds(30));
    return true;
  }
  catch (std::exception const& error)
  {
    std::cerr << operation << ": " << error.what() << '\n';
    return false;
  }
}

std::string_view trim_ascii(std::string_view text)
{
  while (!text.empty() && (text.front() == ' ' || text.front() == '\t'))
    text.remove_prefix(1);
  while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r'))
    text.remove_suffix(1);
  return text;
}

bool starts_with_case_insensitive(std::string_view text, std::string_view prefix)
{
  if (text.size() < prefix.size())
    return false;
  for (std::size_t index = 0; index < prefix.size(); ++index)
  {
    auto const left = static_cast<unsigned char>(text[index]);
    auto const right = static_cast<unsigned char>(prefix[index]);
    auto const lower_left = static_cast<char>(left >= 'A' && left <= 'Z' ? left - 'A' + 'a' : left);
    auto const lower_right = static_cast<char>(right >= 'A' && right <= 'Z' ? right - 'A' + 'a' : right);
    if (lower_left != lower_right)
      return false;
  }
  return true;
}

std::optional<std::size_t> content_length(std::string_view headers)
{
  std::size_t start = 0;
  while (start < headers.size())
  {
    auto const end = headers.find('\n', start);
    auto const line = headers.substr(start, end == std::string_view::npos ? headers.size() - start : end - start);
    if (starts_with_case_insensitive(line, "content-length:"))
    {
      auto value = trim_ascii(line.substr(std::string_view("content-length:").size()));
      std::size_t parsed = 0;
      for (char const ch : value)
      {
        if (ch < '0' || ch > '9')
          return std::nullopt;
        parsed = parsed * 10 + static_cast<std::size_t>(ch - '0');
      }
      return parsed;
    }
    if (end == std::string_view::npos)
      break;
    start = end + 1;
  }
  return std::nullopt;
}

bool write_all(int fd, std::string_view text)
{
  while (!text.empty())
  {
    auto const written = ::send(fd, text.data(), text.size(), 0);
    if (written <= 0)
      return false;
    text.remove_prefix(static_cast<std::size_t>(written));
  }
  return true;
}

bool write_port_file_atomically(std::filesystem::path const& port_file, std::uint16_t port)
{
  if (port == 0)
  {
    std::cerr << "refusing to publish an invalid loopback port\n";
    return false;
  }

  auto const template_path = port_file.parent_path() / (port_file.filename().string() + ".tmp.XXXXXX");
  auto temporary_name = template_path.string();
  int const fd = ::mkstemp(temporary_name.data());
  if (fd < 0)
  {
    std::cerr << "failed to create temporary port file " << template_path << ": " << errno_text() << '\n';
    return false;
  }
  std::filesystem::path const temporary_path = temporary_name;
  auto const cleanup = [&] {
    if (::unlink(temporary_path.c_str()) != 0 && errno != ENOENT)
      std::cerr << "failed to remove temporary port file " << temporary_path << ": " << errno_text() << '\n';
  };

  auto const text = std::to_string(port) + "\n";
  std::size_t offset = 0;
  while (offset < text.size())
  {
    auto const written = ::write(fd, text.data() + offset, text.size() - offset);
    if (written < 0 && errno == EINTR)
      continue;
    if (written <= 0)
    {
      if (written < 0)
        std::cerr << "failed to write temporary port file " << temporary_path << ": " << errno_text() << '\n';
      else
        std::cerr << "failed to write temporary port file " << temporary_path << ": wrote zero bytes\n";
      static_cast<void>(::close(fd));
      cleanup();
      return false;
    }
    offset += static_cast<std::size_t>(written);
  }

  struct stat metadata{};
  if (::fstat(fd, &metadata) != 0)
  {
    std::cerr << "failed to validate temporary port file " << temporary_path << ": " << errno_text() << '\n';
    static_cast<void>(::close(fd));
    cleanup();
    return false;
  }
  if (!S_ISREG(metadata.st_mode) || metadata.st_size != static_cast<off_t>(text.size()))
  {
    std::cerr << "temporary port file validation failed for " << temporary_path << '\n';
    static_cast<void>(::close(fd));
    cleanup();
    return false;
  }
  if (::close(fd) != 0)
  {
    std::cerr << "failed to close temporary port file " << temporary_path << ": " << errno_text() << '\n';
    cleanup();
    return false;
  }
  if (::rename(temporary_path.c_str(), port_file.c_str()) != 0)
  {
    std::cerr << "failed to publish port file " << port_file << ": " << errno_text() << '\n';
    cleanup();
    return false;
  }
  return true;
}

std::string json_escape(std::string_view text)
{
  std::string escaped;
  escaped.reserve(text.size());
  for (char const ch : text)
  {
    switch (ch)
    {
      case '"':
        escaped += "\\\"";
        break;
      case '\\':
        escaped += "\\\\";
        break;
      case '\n':
        escaped += "\\n";
        break;
      case '\r':
        escaped += "\\r";
        break;
      case '\t':
        escaped += "\\t";
        break;
      default:
        if (static_cast<unsigned char>(ch) < 0x20)
        {
          escaped += '?';
        }
        else
        {
          escaped.push_back(ch);
        }
        break;
    }
  }
  return escaped;
}

std::string read_http_request(int fd)
{
  std::string request;
  std::array<char, 4096> buffer{};
  constexpr std::size_t kMaxRequestBytes = 1024 * 1024;
  while (request.size() < kMaxRequestBytes)
  {
    auto const read = ::recv(fd, buffer.data(), buffer.size(), 0);
    if (read <= 0)
      break;
    request.append(buffer.data(), static_cast<std::size_t>(read));
    auto const header_end = request.find("\r\n\r\n");
    if (header_end == std::string::npos)
      continue;
    auto const length = content_length(std::string_view(request).substr(0, header_end + 2)).value_or(0);
    if (request.size() >= header_end + 4 + length)
      break;
  }
  return request;
}

std::string text_body(std::string_view text)
{
  return "{\"choices\":[{\"message\":{\"content\":\"" + json_escape(text) +
         "\"},\"finish_reason\":\"stop\"}],"
         "\"usage\":{\"prompt_tokens\":1,\"completion_tokens\":1,\"total_tokens\":2}}";
}

std::string hostile_terminal_text_body()
{
  return "{\"choices\":[{\"message\":{\"content\":\"safe\\u001b]8;;https://example.invalid\\u0007link\\u001b]8;;\\u0007 output\"},"
         "\"finish_reason\":\"stop\"}],\"usage\":{\"prompt_tokens\":1,\"completion_tokens\":1,\"total_tokens\":2}}";
}

std::string tool_body(std::string_view call_id, std::string_view name, std::string_view arguments)
{
  return "{\"choices\":[{\"message\":{\"tool_calls\":[{\"id\":\"" + json_escape(call_id) +
         "\",\"type\":\"function\","
         "\"function\":{\"name\":\"" +
         json_escape(name) + "\",\"arguments\":\"" + json_escape(arguments) + "\"}}]},\"finish_reason\":\"tool_calls\"}]}";
}

std::string read_tool_body(std::string_view path, std::string_view call_id = "call_read")
{
  auto const arguments = std::string("{\"path\":\"") + json_escape(path) + "\"}";
  return "{\"choices\":[{\"message\":{\"tool_calls\":[{\"id\":\"" + json_escape(call_id) +
         "\",\"type\":\"function\",\"function\":{\"name\":\"read_file\",\"arguments\":\"" + json_escape(arguments) +
         "\"}}]},\"finish_reason\":\"tool_calls\"}]}";
}

std::string grep_tool_body(std::string_view include)
{
  auto const arguments = std::string("{\"pattern\":\"needle\",\"include\":\"") + json_escape(include) + "\",\"max_matches\":5,\"literal\":true}";
  return "{\"choices\":[{\"message\":{\"tool_calls\":[{\"id\":\"call_grep\",\"type\":\"function\","
         "\"function\":{\"name\":\"grep\",\"arguments\":\"" +
         json_escape(arguments) + "\"}}]},\"finish_reason\":\"tool_calls\"}]}";
}

std::string write_tool_body(std::string_view path)
{
  auto const arguments = std::string("{\"path\":\"") + json_escape(path) + "\",\"content\":\"rpc new\\n\"}";
  return "{\"choices\":[{\"message\":{\"tool_calls\":[{\"id\":\"call_write\",\"type\":\"function\","
         "\"function\":{\"name\":\"write_file\",\"arguments\":\"" +
         json_escape(arguments) + "\"}}]},\"finish_reason\":\"tool_calls\"}]}";
}

std::string bash_tool_body(std::string_view pgid_path)
{
  auto const marker_path = std::filesystem::path(pgid_path).parent_path() / "bash-child-leak.txt";
  auto const command = std::string("printf '%s' $$ > \"") + std::string(pgid_path) + "\"; (sleep 30; printf leaked > \"" + marker_path.string() + "\") & wait";
  auto const arguments = std::string("{\"command\":\"") + json_escape(command) + "\",\"timeout_ms\":4000}";
  return "{\"choices\":[{\"message\":{\"tool_calls\":[{\"id\":\"call_bash\",\"type\":\"function\","
         "\"function\":{\"name\":\"bash\",\"arguments\":\"" +
         json_escape(arguments) + "\"}}]},\"finish_reason\":\"tool_calls\"}]}";
}

std::string terminal_tool_body()
{
  return tool_body("call_terminal", "bash", R"({"command":"touch terminal-e2e-marker","timeout_ms":5000,"max_lines":20})");
}

std::string subagent_workspace_task_body()
{
  return tool_body("call_task_live", "task",
                   R"({"description":"Live workspace audit","prompt":"Inspect delegated fixture.","subagent_type":"general","background":true})");
}

std::string subagent_workspace_job_list_body()
{
  return tool_body("call_job_list_poll", "job", R"({"action":"list"})");
}

std::string question_tool_body()
{
  std::string const arguments =
      "{\"header\":\"Pick\",\"question\":\"Continue?\",\"options\":[{\"value\":\"yes\",\"label\":\"Yes\"}],"
      "\"allow_custom\":true}";
  return "{\"choices\":[{\"message\":{\"tool_calls\":[{\"id\":\"call_question\",\"type\":\"function\","
         "\"function\":{\"name\":\"question\",\"arguments\":\"" +
         json_escape(arguments) + "\"}}]},\"finish_reason\":\"tool_calls\"}]}";
}

std::string multi_question_tool_body()
{
  std::string const arguments =
      "{\"header\":\"Pick\",\"question\":\"Choose providers\",\"options\":[{\"value\":\"alpha\",\"label\":\"Alpha\"},"
      "{\"value\":\"beta\",\"label\":\"Beta\"},{\"value\":\"gamma\",\"label\":\"Gamma\"}],\"multiple\":true,\"allow_custom\":true}";
  return "{\"choices\":[{\"message\":{\"tool_calls\":[{\"id\":\"call_question\",\"type\":\"function\","
         "\"function\":{\"name\":\"question\",\"arguments\":\"" +
         json_escape(arguments) + "\"}}]},\"finish_reason\":\"tool_calls\"}]}";
}

std::string skill_tool_body()
{
  std::string const arguments = "{\"name\":\"cli-skill\"}";
  return "{\"choices\":[{\"message\":{\"tool_calls\":[{\"id\":\"call_skill\",\"type\":\"function\","
         "\"function\":{\"name\":\"skill\",\"arguments\":\"" +
         json_escape(arguments) + "\"}}]},\"finish_reason\":\"tool_calls\"}]}";
}

std::string websearch_tool_body()
{
  std::string const arguments = "{\"query\":\"OpenAI API docs\",\"num_results\":2,\"contextMaxCharacters\":2000}";
  return "{\"choices\":[{\"message\":{\"tool_calls\":[{\"id\":\"call_websearch\",\"type\":\"function\","
         "\"function\":{\"name\":\"websearch\",\"arguments\":\"" +
         json_escape(arguments) + "\"}}]},\"finish_reason\":\"tool_calls\"}]}";
}

std::string webfetch_tool_body()
{
  std::string const arguments = "{\"url\":\"https://example.com/\",\"format\":\"text\",\"max_bytes\":2048}";
  return "{\"choices\":[{\"message\":{\"tool_calls\":[{\"id\":\"call_webfetch\",\"type\":\"function\","
         "\"function\":{\"name\":\"webfetch\",\"arguments\":\"" +
         json_escape(arguments) + "\"}}]},\"finish_reason\":\"tool_calls\"}]}";
}

std::string mcp_tool_body()
{
  std::string const arguments = "{\"text\":\"hello from cli\"}";
  return "{\"choices\":[{\"message\":{\"tool_calls\":[{\"id\":\"call_mcp\",\"type\":\"function\","
         "\"function\":{\"name\":\"mcp_demo_echo\",\"arguments\":\"" +
         json_escape(arguments) + "\"}}]},\"finish_reason\":\"tool_calls\"}]}";
}

std::string parallel_read_search_body(int request_index)
{
  if (request_index == 0)
  {
    auto const read_args = std::string("{\"path\":\"notes.txt\"}");
    auto const list_args = std::string("{\"path\":\".\",\"max_entries\":20}");
    auto const glob_args = std::string("{\"pattern\":\"*.txt\"}");
    auto const grep_args = std::string("{\"pattern\":\"alpha\",\"include\":\"*.txt\",\"max_matches\":10,\"literal\":true}");
    return std::string("{\"choices\":[{\"message\":{\"tool_calls\":[") +
           "{\"id\":\"call_read\",\"type\":\"function\",\"function\":{\"name\":\"read_file\",\"arguments\":\"" + json_escape(read_args) + "\"}}," +
           "{\"id\":\"call_list\",\"type\":\"function\",\"function\":{\"name\":\"list_directory\",\"arguments\":\"" + json_escape(list_args) + "\"}}," +
           "{\"id\":\"call_glob\",\"type\":\"function\",\"function\":{\"name\":\"glob\",\"arguments\":\"" + json_escape(glob_args) + "\"}}," +
           "{\"id\":\"call_grep\",\"type\":\"function\",\"function\":{\"name\":\"grep\",\"arguments\":\"" + json_escape(grep_args) + "\"}}" +
           "]},\"finish_reason\":\"tool_calls\"}]}";
  }
  if (request_index == 1)
    return tool_body("call_write", "write_file", "{\"path\":\"notes.txt\",\"content\":\"mutated-visible\\n\"}");
  if (request_index == 2)
    return read_tool_body("notes.txt", "call_post_read");
  return text_body("parallel-read-search complete");
}

std::string e2e_tool_body(int request_index, std::string_view target_path)
{
  std::string const workspace_path = "src/todo.txt";
  auto const edit_path = target_path.empty() ? workspace_path : std::string(target_path);
  if (request_index == 0)
  {
    auto const arguments = std::string("{\"path\":\"") + json_escape(workspace_path) + "\",\"limit\":40}";
    return tool_body("call_read_e2e", "read_file", arguments);
  }
  if (request_index == 1)
  {
    std::string const arguments = "{\"pattern\":\"TODO\",\"include\":\"src/*.txt\",\"max_matches\":10,\"literal\":true}";
    return tool_body("call_grep_e2e", "grep", arguments);
  }
  if (request_index == 2)
  {
    std::string const arguments = "{\"path\":\"src\",\"max_entries\":20}";
    return tool_body("call_list_e2e", "list_directory", arguments);
  }
  if (request_index == 3)
  {
    auto const old_text = std::string("status: TODO\n") + "detail: replace TODO with DONE and verify.\n";
    auto const new_text = std::string("status: DONE\n") + "detail: replace TODO with DONE and verify.\n";
    auto const arguments = std::string("{\"edits\":[{\"path\":\"") + json_escape(edit_path) + "\",\"old_text\":\"" + json_escape(old_text) +
                           "\",\"new_text\":\"" + json_escape(new_text) + "\"}]}";
    return tool_body("call_patch_e2e", "apply_patch", arguments);
  }
  if (request_index == 4)
  {
    auto const arguments = std::string("{\"command\":\"cat ") + json_escape(edit_path) + "\",\"timeout_ms\":5000,\"max_lines\":20}";
    return tool_body("call_bash_e2e", "bash", arguments);
  }
  return text_body("E2E task complete: TODO fixed and verification command passed.");
}

bool write_streaming_marker(std::filesystem::path const& directory, std::string_view name)
{
  auto const path = directory / name;
  std::ofstream marker(path, std::ios::binary | std::ios::trunc);
  if (!marker)
  {
    std::cerr << "streaming-scroll failed to create marker " << path << '\n';
    return false;
  }
  marker << name << '\n';
  marker.close();
  if (!marker)
  {
    std::cerr << "streaming-scroll failed to commit marker " << path << '\n';
    return false;
  }
  return true;
}

bool send_streaming_scroll_response(int client_fd, std::string const& request, std::chrono::milliseconds delay, std::filesystem::path const& marker_directory,
                                    ava::test::ProcessGateSet* process_gates)
{
  auto const first_line_end = request.find("\r\n");
  auto const first_line = request.substr(0, first_line_end);
  if (!first_line.starts_with("POST ") || first_line.find("/chat/completions ") == std::string::npos)
  {
    std::cerr << "streaming-scroll requires a normal chat-completions POST request\n";
    return false;
  }
  if (request.find("\"stream\":true") == std::string::npos)
  {
    std::cerr << "streaming-scroll requires stream=true in the chat-completions request\n";
    return false;
  }
  std::error_code marker_error;
  if (marker_directory.empty() || !std::filesystem::is_directory(marker_directory, marker_error) || marker_error)
  {
    std::cerr << "streaming-scroll requires an existing scenario-owned marker directory\n";
    return false;
  }

  if (!write_all(client_fd,
                 "HTTP/1.1 200 OK\r\n"
                 "Content-Type: text/event-stream\r\n"
                 "Cache-Control: no-cache\r\n"
                 "Connection: close\r\n\r\n"))
  {
    std::cerr << "streaming-scroll response header write failed: " << errno_text() << '\n';
    return false;
  }

  for (int index = 0; index < 60; ++index)
  {
    if (index == 30)
    {
      if (!write_streaming_marker(marker_directory, "paused"))
        return false;
      if (process_gates == nullptr)
      {
        std::cerr << "streaming-scroll requires a process-gate control descriptor\n";
        return false;
      }
      if (!wait_for_process_gate(*process_gates, 0, "streaming-scroll continuation gate failed"))
        return false;
    }

    std::ostringstream content;
    content << "stream line " << std::setw(3) << std::setfill('0') << index;
    if (index != 29)
      content << "\\n";
    auto const event =
        "data: {\"id\":\"chatcmpl-stream-scroll\",\"object\":\"chat.completion.chunk\",\"model\":\"ava-tui-fake\",\"choices\":[{\"index\":0,"
        "\"delta\":{\"content\":\"" +
        content.str() + "\"},\"finish_reason\":null}]}\n\n";
    if (!write_all(client_fd, event))
    {
      std::cerr << "streaming-scroll delta " << index << " write failed: " << errno_text() << '\n';
      return false;
    }
    // Deliberately pace individual SSE deltas so terminal tests can exercise live streaming states; this is not a synchronization wait.
    std::this_thread::sleep_for(delay);
    if (index == 29 &&
        !write_all(client_fd,
                   "data: {\"id\":\"chatcmpl-stream-scroll\",\"object\":\"chat.completion.chunk\",\"model\":\"ava-tui-fake\",\"choices\":[{\"index\":0,"
                   "\"delta\":{\"content\":\"\\n\"},\"finish_reason\":null}]}\n\n"))
    {
      std::cerr << "streaming-scroll line terminator write failed: " << errno_text() << '\n';
      return false;
    }
  }

  if (!write_all(client_fd,
                 "data: {\"id\":\"chatcmpl-stream-scroll\",\"object\":\"chat.completion.chunk\",\"model\":\"ava-tui-fake\",\"choices\":[{\"index\":0,"
                 "\"delta\":{\"content\":\"STREAM COMPLETE\"},\"finish_reason\":null}]}\n\n"))
  {
    std::cerr << "streaming-scroll final marker write failed: " << errno_text() << '\n';
    return false;
  }
  // Preserve one final live-stream frame before the stop event; the scenario controls continuation separately through gate 0.
  std::this_thread::sleep_for(delay);
  if (!write_all(client_fd,
                 "data: {\"id\":\"chatcmpl-stream-scroll\",\"object\":\"chat.completion.chunk\",\"model\":\"ava-tui-fake\",\"choices\":[{\"index\":0,"
                 "\"delta\":{},\"finish_reason\":\"stop\"}],\"usage\":{\"prompt_tokens\":1,\"completion_tokens\":61,\"total_tokens\":62}}\n\n"
                 "data: [DONE]\n\n"))
  {
    std::cerr << "streaming-scroll completion write failed: " << errno_text() << '\n';
    return false;
  }
  return write_streaming_marker(marker_directory, "completed");
}

struct ProviderResponse
{
  int status_code = 200;
  std::string reason = "OK";
  std::string body;
};

ProviderResponse response_for(std::string_view scenario, int request_index, std::string_view target_path)
{
  if (scenario == "rpc-stream")
  {
    return ProviderResponse{.body =
                                "data: {\"choices\":[{\"delta\":{\"content\":\"rpc \"}}]}\n\n"
                                "data: {\"choices\":[{\"delta\":{\"content\":\"stream\"},\"finish_reason\":\"stop\"}],"
                                "\"usage\":{\"prompt_tokens\":1,\"completion_tokens\":1,\"total_tokens\":2}}\n\n"
                                "data: [DONE]\n\n"};
  }
  if (scenario == "http-error")
  {
    return ProviderResponse{.status_code = 500,
                            .reason = "Internal Server Error",
                            .body =
                                "{\"error\":{\"type\":\"FAKE_UNKNOWN_DISCRIMINATOR_CANARY\",\"message\":\"provider unavailable\","
                                "\"reasoning_content\":\"secret reasoning\","
                                "\"thinking\":\"secret thinking\",\"api_key\":\"secret-key\"}}"};
  }
  if (scenario == "terminal-hostile-text")
  {
    return ProviderResponse{.body = hostile_terminal_text_body()};
  }
  if (scenario == "read-tool")
  {
    return ProviderResponse{.body = request_index == 0 ? read_tool_body(target_path) : text_body("after permission deny")};
  }
  if (scenario == "read-tool-twice")
  {
    if (request_index == 0 || request_index == 2)
      return ProviderResponse{.body = read_tool_body(target_path, "call_read_" + std::to_string(request_index / 2 + 1))};
    return ProviderResponse{.body = text_body(request_index == 1 ? "first session grant" : "second session grant")};
  }
  if (scenario == "read-tool-thrice")
  {
    if (request_index == 0 || request_index == 2 || request_index == 4)
    {
      return ProviderResponse{.body = read_tool_body(target_path, "call_read_" + std::to_string(request_index / 2 + 1))};
    }
    if (request_index == 1)
      return ProviderResponse{.body = text_body("first controlled grant")};
    return ProviderResponse{.body = text_body(request_index == 3 ? "second controlled grant" : "third controlled grant")};
  }
  if (scenario == "read-missing-tool")
  {
    return ProviderResponse{.body = request_index == 0 ? read_tool_body(target_path) : text_body("after tool failure")};
  }
  if (scenario == "grep-tool")
  {
    return ProviderResponse{.body = request_index == 0 ? grep_tool_body(target_path) : text_body("after grep tool")};
  }
  if (scenario == "write-tool")
  {
    return ProviderResponse{.body = request_index == 0 ? write_tool_body(target_path) : text_body("after permission deny")};
  }
  if (scenario == "bash-timeout-tree")
  {
    return ProviderResponse{.body = request_index == 0 ? bash_tool_body(target_path) : text_body("after bash process cleanup")};
  }
  if (scenario == "terminal-tool")
  {
    return ProviderResponse{.body = request_index == 0 ? terminal_tool_body() : text_body("after ACP terminal")};
  }
  if (scenario == "question-tool")
  {
    return ProviderResponse{.body = request_index == 0 ? question_tool_body() : text_body("after question reply")};
  }
  if (scenario == "question-tool-multi")
  {
    return ProviderResponse{.body = request_index == 0 ? multi_question_tool_body() : text_body("after multi question reply")};
  }
  if (scenario == "skill-tool")
  {
    return ProviderResponse{.body = request_index == 0 ? skill_tool_body() : text_body("after skill tool")};
  }
  if (scenario == "websearch-tool")
  {
    return ProviderResponse{.body = request_index == 0 ? websearch_tool_body() : text_body("after websearch tool")};
  }
  if (scenario == "webfetch-tool")
  {
    return ProviderResponse{.body = request_index == 0 ? webfetch_tool_body() : text_body("after webfetch tool")};
  }
  if (scenario == "mcp-tool")
  {
    return ProviderResponse{.body = request_index == 0 ? mcp_tool_body() : text_body("after mcp tool")};
  }
  if (scenario == "subagent-workspace")
  {
    if (request_index == 0)
      return ProviderResponse{.body = subagent_workspace_task_body()};
    return ProviderResponse{.body = text_body(request_index == 1 ? "Committed child answer." : "Parent continued after background start.")};
  }
  if (scenario == "end-to-end-workflow")
  {
    return ProviderResponse{.body = e2e_tool_body(request_index, target_path)};
  }
  if (scenario == "parallel-read-search")
  {
    return ProviderResponse{.body = parallel_read_search_body(request_index)};
  }
  if (scenario == "compact" || scenario == "compact-delayed" || scenario == "compact-follow-up")
  {
    if (request_index == 0)
      return ProviderResponse{.body = text_body("before compact")};
    if (request_index == 1 || (scenario == "compact-follow-up" && request_index == 3))
      return ProviderResponse{.body = text_body("# Goal\nHeadless compact summary\n# Next Steps\nContinue.")};
    if (scenario == "compact-follow-up" && request_index == 4)
      return ProviderResponse{.body = read_tool_body(target_path, "call_compact_failed_read")};
    if (scenario == "compact-follow-up" && request_index == 5)
    {
      return ProviderResponse{.status_code = 400, .reason = "Bad Request", .body = "{\"error\":{\"message\":\"queued follow-up rejected by fake provider\"}}"};
    }
    return ProviderResponse{.body = text_body("after compact queued answer")};
  }
  if (scenario == "markdown-links")
  {
    return ProviderResponse{.body = text_body("[Docs](https://e.test/d) https://e.test/b u@e.test")};
  }
  if (scenario == "mermaid")
  {
    return ProviderResponse{.body = text_body("before mermaid\n```mermaid\nTMUX_MERMAID_SOURCE_A-->B\n```\nafter mermaid")};
  }
  return ProviderResponse{.body = text_body("headless active prompt complete")};
}

}  // namespace

int main(int argc, char** argv)
{
  if (argc != 4 && argc != 6)
  {
    std::cerr << "usage: ava_fake_provider_server PORT_FILE REQUEST_LOG STREAM_DELAY_MS [SCENARIO TARGET_PATH]\n";
    return 2;
  }

  std::filesystem::path const port_file = argv[1];
  std::filesystem::path const request_log = argv[2];
  auto const delay = std::chrono::milliseconds(std::stoi(argv[3]));
  std::string const scenario = argc == 6 ? argv[4] : "text";
  std::string const target_path = argc == 6 ? argv[5] : "";
  std::optional<ava::test::ProcessGateSet> process_gates;
  try
  {
    process_gates = process_gates_from_environment();
  }
  catch (std::exception const& error)
  {
    std::cerr << error.what() << '\n';
    return 2;
  }
  int const request_count =
      scenario == "http-error"                 ? 3
      : scenario == "branch-summary"           ? 12
      : scenario == "text-three"               ? 3
      : scenario == "text-three-delayed-first" ? 3
      : scenario == "text-three-delayed-third" ? 4
      : scenario == "compact-follow-up"        ? 6
      : scenario == "streaming-scroll"         ? 2
      : scenario == "end-to-end-workflow"      ? 6
      : scenario == "parallel-read-search"     ? 4
      : scenario == "read-tool-twice"          ? 4
      : scenario == "read-tool-thrice"         ? 6
      : scenario == "subagent-workspace"       ? 4
      : (scenario == "read-tool" || scenario == "read-missing-tool" || scenario == "grep-tool" || scenario == "write-tool" || scenario == "bash-timeout-tree" ||
         scenario == "question-tool" || scenario == "question-tool-multi" || scenario == "skill-tool" || scenario == "websearch-tool" ||
         scenario == "webfetch-tool" || scenario == "mcp-tool" || scenario == "terminal-tool" || scenario == "compact" || scenario == "compact-delayed")
          ? 2
          : 1;

  Fd server(::socket(AF_INET, SOCK_STREAM, 0));
  if (server.get() < 0)
  {
    std::cerr << "socket failed: " << errno_text() << '\n';
    return 1;
  }

  int reuse = 1;
  if (::setsockopt(server.get(), SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) != 0)
  {
    std::cerr << "setsockopt failed: " << errno_text() << '\n';
    return 1;
  }

  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  address.sin_port = 0;
  if (::bind(server.get(), reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0)
  {
    std::cerr << "bind failed: " << errno_text() << '\n';
    return 1;
  }
  if (::listen(server.get(), 1) != 0)
  {
    std::cerr << "listen failed: " << errno_text() << '\n';
    return 1;
  }

  socklen_t address_len = sizeof(address);
  if (::getsockname(server.get(), reinterpret_cast<sockaddr*>(&address), &address_len) != 0)
  {
    std::cerr << "getsockname failed: " << errno_text() << '\n';
    return 1;
  }
  if (!write_port_file_atomically(port_file, ntohs(address.sin_port)))
    return 1;

  {
    std::ofstream file(request_log, std::ios::binary | std::ios::trunc);
  }

  for (int request_index = 0; request_index < request_count; ++request_index)
  {
    Fd client(::accept(server.get(), nullptr, nullptr));
    if (client.get() < 0)
    {
      std::cerr << "accept failed: " << errno_text() << '\n';
      return 1;
    }

    auto const request = read_http_request(client.get());
    {
      std::ofstream file(request_log, std::ios::binary | std::ios::app);
      file << "--- request " << (request_index + 1) << " ---\n" << request << '\n';
    }
    // Publish request receipt without requiring the harness to be waiting yet; gate N corresponds to zero-based request N.
    if (process_gates)
      process_gates->open(static_cast<std::size_t>(request_index));
    if (scenario == "streaming-scroll" && request_index == 0)
    {
      if (!send_streaming_scroll_response(client.get(), request, delay, target_path, process_gates ? &*process_gates : nullptr))
        return 1;
      continue;
    }
    if (scenario == "subagent-workspace" && request_index > 0)
    {
      if (!process_gates)
      {
        std::cerr << "subagent-workspace requires a process-gate control descriptor\n";
        return 1;
      }
      if (!wait_for_process_gate(*process_gates, 0, "subagent-workspace release gate failed"))
        return 1;
    }
    auto const gate_this_request = scenario == "compact-follow-up"          ? request_index == 1 || request_index == 3
                                   : scenario == "compact-delayed"          ? request_index == 1
                                   : scenario == "text-three-delayed-third" ? request_index == 2
                                   : scenario == "text-three-delayed-first" ? request_index == 0
                                   : scenario == "text-delayed"             ? request_index == 0
                                                                            : false;
    if (gate_this_request)
    {
      if (!process_gates)
      {
        std::cerr << "delayed fake-provider requests require a process-gate control descriptor\n";
        return 1;
      }
      if (!wait_for_process_gate(*process_gates, static_cast<std::size_t>(request_index), "delayed provider release gate failed"))
        return 1;
    }

    auto provider_response = response_for(scenario, request_index, target_path);
    if (scenario == "branch-summary" && request.find("Summarize only the supplied abandoned parent-session branch") != std::string::npos)
      provider_response.body = text_body("BRANCH-SUMMARY-SECRET-PAYLOAD-91A useful abandoned parent context");
    if (scenario == "subagent-workspace" && request_index > 0)
    {
      if (request.find("You are AVA's general subagent") != std::string::npos)
        provider_response.body = text_body("Committed child answer.");
      else if (request.find("Tool call (job): arguments_json=") == std::string::npos)
        provider_response.body = subagent_workspace_job_list_body();
      else
        provider_response.body = text_body("Parent continued after background start.");
    }
    std::string const response = "HTTP/1.1 " + std::to_string(provider_response.status_code) + " " + provider_response.reason +
                                 "\r\nContent-Type: application/json\r\nContent-Length: " + std::to_string(provider_response.body.size()) +
                                 "\r\nConnection: close\r\n\r\n" + provider_response.body;
    if (!write_all(client.get(), response))
    {
      std::cerr << "response write failed: " << errno_text() << '\n';
      return 1;
    }
  }
  return 0;
}

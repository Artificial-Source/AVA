#include "sys.h"
#include "tests/support/app_runtime_support.h"
#include "ava/http/transport.h"
#include "ava/core/ids.h"
#include "ava/core/json.h"

#include <algorithm>
#include <fstream>
#include <iterator>
#include <ranges>
#include <utility>
#include <sys/stat.h>

namespace ava::tests {

ava::config::XdgPaths app_test_paths(std::filesystem::path const& root)
{
  std::filesystem::create_directories(root);
  ::chmod(root.c_str(), S_IRWXU);
  auto const config_home = root / "config";
  auto const state_home = root / "state";
  auto const data_home = root / "data";
  auto const ava_config = config_home / "ava";
  auto const ava_state = state_home / "ava";
  auto const sessions = ava_state / "sessions";
  for (auto const& directory : {config_home, state_home, data_home, ava_config, ava_state, sessions})
  {
    std::filesystem::create_directories(directory);
    ::chmod(directory.c_str(), S_IRWXU);
  }
  // Ordinary runtime tests must never make incidental provider calls. Focused
  // title tests inject their own bounded coordinator and fake generator.
  {
    auto const titles_path = ava_config / "session-titles.json";
    std::ofstream titles(titles_path, std::ios::binary | std::ios::trunc);
    titles << "{\"schema_version\":1,\"enabled\":false}\n";
    titles.close();
    ::chmod(titles_path.c_str(), S_IRUSR | S_IWUSR);
  }
  return ava::config::XdgPaths{.config_home = config_home,
                               .state_home = state_home,
                               .data_home = data_home,
                               .ava_config_dir = ava_config,
                               .ava_state_dir = ava_state,
                               .auth_file = ava_config / "auth.json",
                               .compaction_file = ava_config / "compaction.json",
                               .global_agents_file = ava_config / "AGENTS.md",
                               .models_file = ava_config / "models.json",
                               .providers_file = ava_config / "providers.json",
                               .prompts_dir = ava_config / "prompts",
                               .sessions_dir = sessions};
}

std::string app_test_plugin_manifest_json(std::string_view id, std::string_view name)
{
  return std::string("{\n") +
         "  \"schema_version\": 1,\n"
         "  \"id\": \"" +
         ava::core::json::escape(id) +
         "\",\n"
         "  \"name\": \"" +
         ava::core::json::escape(name) +
         "\",\n"
         "  \"version\": \"0.1.0\",\n"
         "  \"api_version\": \"ava.plugin.v1\",\n"
         "  \"description\": \"test plugin\",\n"
         "  \"entrypoint\": {\"command\": \"node\", \"args\": [\"plugin.js\", \"--safe\"]},\n"
         "  \"capabilities\": [\"tools\", \"commands\"],\n"
         "  \"contributes\": {\n"
         "    \"tools\": [{\"name\": \"todo_add\", \"description\": \"Add todo\", \"input_schema\": {\"type\": "
         "\"object\", \"additionalProperties\": false}}],\n"
         "    \"commands\": [{\"name\": \"todo\", \"description\": \"Show todos\"}]\n"
         "  }\n"
         "}";
}

std::string app_test_mcp_config_json(std::string_view id, std::string_view name, std::string_view command)
{
  return std::string("{\"servers\":[{\"id\":\"") + ava::core::json::escape(id) + "\",\"name\":\"" + ava::core::json::escape(name) + "\",\"command\":\"" +
         ava::core::json::escape(command) + "\",\"enabled\":true}]}";
}

void write_app_test_file(std::filesystem::path const& path, std::string const& text)
{
  std::filesystem::create_directories(path.parent_path());
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  file << text;
}

std::string app_read_binary_file(std::filesystem::path const& path)
{
  std::ifstream file(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

void BlockingInputBuf::push(std::string text)
{
  {
    std::lock_guard lock(mutex_);
    for (char const ch : text) buffer_.push_back(ch);
  }
  cv_.notify_all();
}

void BlockingInputBuf::close() noexcept
{
  {
    std::lock_guard lock(mutex_);
    closed_ = true;
  }
  cv_.notify_all();
}

bool BlockingInputBuf::wait_until_blocked(std::chrono::milliseconds timeout)
{
  std::unique_lock lock(mutex_);
  return cv_.wait_for(lock, timeout, [&] { return blocked_; });
}

bool BlockingInputBuf::wait_until_eof_observed(std::chrono::milliseconds timeout)
{
  std::unique_lock lock(mutex_);
  return cv_.wait_for(lock, timeout, [&] { return eof_observed_; });
}

int BlockingInputBuf::underflow()
{
  std::unique_lock lock(mutex_);
  blocked_ = buffer_.empty() && !closed_;
  cv_.notify_all();
  cv_.wait(lock, [&] { return closed_ || !buffer_.empty(); });
  blocked_ = false;
  if (buffer_.empty())
  {
    eof_observed_ = true;
    cv_.notify_all();
    return traits_type::eof();
  }
  current_ = buffer_.front();
  buffer_.pop_front();
  setg(&current_, &current_, &current_ + 1);
  return traits_type::to_int_type(current_);
}

std::string ThreadSafeStringBuf::str() const
{
  std::lock_guard lock(mutex_);
  return text_;
}

// Return true if the buffer contains `value` at or beyond search_pos_ and updates search_pos_.
bool ThreadSafeStringBuf::wait_contains(std::string_view value, std::chrono::milliseconds timeout)
{
  std::unique_lock lock(mutex_);
  std::string::size_type pos;
  bool found = cv_.wait_for(lock, timeout, [&] { return (pos = text_.find(value, search_pos_)) != std::string::npos; });
  search_pos_ = found ? pos + value.length() : text_.length();
  return found;
}

int ThreadSafeStringBuf::overflow(int ch)
{
  if (ch == traits_type::eof())
    return traits_type::not_eof(ch);
  {
    std::lock_guard lock(mutex_);
    text_.push_back(static_cast<char>(ch));
  }
  cv_.notify_all();
  return ch;
}

std::streamsize ThreadSafeStringBuf::xsputn(char const* s, std::streamsize count)
{
  {
    std::lock_guard lock(mutex_);
    text_.append(s, static_cast<std::size_t>(count));
  }
  cv_.notify_all();
  return count;
}

ChunkedStreamingTransport::ChunkedStreamingTransport(std::vector<std::string> chunks, int status_code) : chunks_(std::move(chunks)), status_code_(status_code)
{
  for (auto const& chunk : chunks_) response_body_ += chunk;
}

ava::core::Result<ava::http::HttpResponse> ChunkedStreamingTransport::send(ava::http::HttpRequest const& request)
{
  requests_.push_back(request);
  return ava::http::HttpResponse{.status_code = status_code_, .headers = {}, .body = response_body_};
}

bool ChunkedStreamingTransport::supports_streaming() const noexcept
{
  return true;
}

ava::core::Result<ava::http::HttpResponse> ChunkedStreamingTransport::send_streaming(ava::http::HttpRequest const& request, BodyChunkSink on_body_chunk,
                                                                                     CancelCallback cancel_requested)
{
  requests_.push_back(request);
  for (auto const& chunk : chunks_)
  {
    if (cancel_requested && cancel_requested())
    {
      return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Unknown, "stream canceled"));
    }
    if (status_code_ >= 200 && status_code_ < 300 && on_body_chunk)
    {
      if (auto delivered = on_body_chunk(chunk); !delivered)
        return std::unexpected(std::move(delivered.error()));
    }
  }
  return ava::http::HttpResponse{.status_code = status_code_, .headers = {}, .body = response_body_};
}

std::vector<ava::http::HttpRequest> const& ChunkedStreamingTransport::requests() const noexcept
{
  return requests_;
}

BlockingResponseTransport::BlockingResponseTransport(ava::http::HttpResponse response) : response_(std::move(response))
{
}

ava::core::Result<ava::http::HttpResponse> BlockingResponseTransport::send(ava::http::HttpRequest const& request)
{
  {
    std::lock_guard lock(mutex_);
    requests_.push_back(request);
    requested_ = true;
  }
  cv_.notify_all();
  std::unique_lock lock(mutex_);
  cv_.wait(lock, [&] { return released_; });
  return response_;
}

bool BlockingResponseTransport::wait_for_request(std::chrono::milliseconds timeout) const
{
  std::unique_lock lock(mutex_);
  return cv_.wait_for(lock, timeout, [&] { return requested_; });
}

void BlockingResponseTransport::release()
{
  {
    std::lock_guard lock(mutex_);
    released_ = true;
  }
  cv_.notify_all();
}

std::vector<ava::http::HttpRequest> BlockingResponseTransport::requests() const
{
  std::lock_guard lock(mutex_);
  return requests_;
}

ava::http::HttpResponse sse_response(std::string body)
{
  return ava::http::HttpResponse{.status_code = 200, .headers = {}, .body = std::move(body)};
}

std::string read_file_call_sse(std::string_view path, std::string_view call_id)
{
  auto const escaped_id = ava::core::json::escape(call_id);
  return "data: {\"type\":\"response.function_call.added\",\"call_id\":\"" + escaped_id +
         "\",\"name\":\"read_file\"}\n\n"
         "data: {\"type\":\"response.function_call_arguments.delta\",\"call_id\":\"" +
         escaped_id + "\",\"delta\":\"{\\\"path\\\":\\\"" + ava::core::json::escape(path) +
         "\\\"}\"}\n\n"
         "data: [DONE]\n\n";
}

std::string write_file_call_sse(std::string_view path, std::string_view content)
{
  auto const args = "{\"path\":\"" + ava::core::json::escape(path) + "\",\"content\":\"" + ava::core::json::escape(content) + "\"}";
  return "data: {\"type\":\"response.function_call.added\",\"call_id\":\"call_write\",\"name\":\"write_file\"}\n\n"
         "data: "
         "{\"type\":\"response.function_call_arguments.delta\",\"call_id\":\"call_write\",\"delta\":\"" +
         ava::core::json::escape(args) +
         "\"}\n\n"
         "data: [DONE]\n\n";
}

std::string question_call_sse()
{
  return "data: {\"type\":\"response.function_call.added\",\"call_id\":\"call_question\",\"name\":\"question\"}\n\n"
         "data: "
         "{\"type\":\"response.function_call_arguments.delta\",\"call_id\":\"call_question\",\"delta\":\"{"
         "\\\"header\\\":\\\"Pick\\\",\\\"question\\\":\\\"Continue?\\\",\\\"options\\\":[{\\\"value\\\":\\\"yes\\\","
         "\\\"label\\\":\\\"Yes\\\"}],\\\"allow_custom\\\":true}\"}\n\n"
         "data: [DONE]\n\n";
}

std::string multi_question_call_sse()
{
  return "data: {\"type\":\"response.function_call.added\",\"call_id\":\"call_question\",\"name\":\"question\"}\n\n"
         "data: "
         "{\"type\":\"response.function_call_arguments.delta\",\"call_id\":\"call_question\",\"delta\":\"{"
         "\\\"header\\\":\\\"Pick\\\",\\\"question\\\":\\\"Choose providers\\\",\\\"options\\\":[{"
         "\\\"value\\\":\\\"alpha\\\",\\\"label\\\":\\\"Alpha\\\"},{\\\"value\\\":\\\"beta\\\","
         "\\\"label\\\":\\\"Beta\\\"}],\\\"multiple\\\":true,\\\"allow_custom\\\":true}\"}\n\n"
         "data: [DONE]\n\n";
}

std::string final_text_sse(std::string_view text)
{
  return "data: {\"type\":\"response.output_text.delta\",\"delta\":\"" + ava::core::json::escape(text) +
         "\"}\n\n"
         "data: [DONE]\n\n";
}

std::string extract_json_string_field(std::string_view text, std::string_view key)
{
  std::string const needle = "\"" + std::string(key) + "\":\"";
  auto const start = text.find(needle);
  if (start == std::string_view::npos)
    return "";
  auto const value_start = start + needle.size();
  auto const value_end = text.find('"', value_start);
  if (value_end == std::string_view::npos)
    return "";
  return std::string(text.substr(value_start, value_end - value_start));
}

std::string extract_last_json_string_field(std::string_view text, std::string_view key)
{
  std::string const needle = "\"" + std::string(key) + "\":\"";
  auto const start = text.rfind(needle);
  if (start == std::string_view::npos)
    return "";
  auto const value_start = start + needle.size();
  auto const value_end = text.find('"', value_start);
  if (value_end == std::string_view::npos)
    return "";
  return std::string(text.substr(value_start, value_end - value_start));
}

std::size_t count_substrings(std::string_view text, std::string_view needle)
{
  std::size_t count = 0;
  std::size_t position = 0;
  while ((position = text.find(needle, position)) != std::string_view::npos)
  {
    ++count;
    position += needle.size();
  }
  return count;
}

std::size_t count_compaction_entries(std::vector<ava::session::SessionEntry> const& entries)
{
  return static_cast<std::size_t>(std::ranges::count_if(entries, [](auto const& entry) { return entry.type == ava::session::EntryType::Compaction; }));
}

std::optional<ava::session::SessionEntry> latest_compaction_entry(std::vector<ava::session::SessionEntry> const& entries)
{
  for (auto iterator = entries.rbegin(); iterator != entries.rend(); ++iterator)
  {
    if (iterator->type == ava::session::EntryType::Compaction)
      return *iterator;
  }
  return std::nullopt;
}

MutatingSummaryTransport::MutatingSummaryTransport(ava::agent::SessionAppendSink append_sink, std::vector<ava::http::HttpResponse> responses,
                                                   std::size_t mutate_requests)
    : append_sink_(std::move(append_sink)), responses_(std::move(responses)), mutate_requests_(mutate_requests)
{
}

ava::core::Result<ava::http::HttpResponse> MutatingSummaryTransport::send(ava::http::HttpRequest const& request)
{
  requests_.push_back(request);
  if (requests_.size() <= mutate_requests_)
  {
    if (append_sink_)
    {
      static_cast<void>(append_sink_(ava::session::SessionEntry{.id = "entry_concurrent_change_" + std::to_string(requests_.size()),
                                                                .parent_id = "",
                                                                .type = ava::session::EntryType::UserMessage,
                                                                .timestamp = ava::session::now_timestamp(),
                                                                .data_json = "{\"text\":\"concurrent change\"}"}));
    }
  }
  if (responses_.empty())
  {
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Provider, "fake transport has no response"));
  }
  auto response = responses_.front();
  responses_.erase(responses_.begin());
  return response;
}

std::vector<ava::http::HttpRequest> const& MutatingSummaryTransport::requests() const noexcept
{
  return requests_;
}

}  // namespace ava::tests

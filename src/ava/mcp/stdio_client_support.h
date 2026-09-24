#pragma once

#include "ava/mcp/stdio_client.h"
#include "ava/core/result.h"

#include <array>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>
#include <signal.h>
#include <sys/types.h>

namespace ava::mcp {

inline constexpr char kTrustedExecPath[] = "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin";

class UniqueFd
{
 public:
  explicit UniqueFd(int fd = -1) noexcept;
  UniqueFd(UniqueFd const&) = delete;
  UniqueFd& operator=(UniqueFd const&) = delete;
  UniqueFd(UniqueFd&& other) noexcept;
  UniqueFd& operator=(UniqueFd&& other) noexcept;
  ~UniqueFd();

  [[nodiscard]] int get() const noexcept;
  [[nodiscard]] int release() noexcept;
  void reset(int fd = -1) noexcept;

  AVA_DEBUG_PRINT_MEMBERS_ON

 private:
  int fd_ = -1;
};

[[nodiscard]] ava::core::Error mcp_error(ava::core::ErrorCategory category, std::string message, McpServerConfig const& server);
[[nodiscard]] ava::core::Error errno_error(std::string message, McpServerConfig const& server);
[[nodiscard]] ava::core::Error protocol_error(std::string message, McpServerConfig const& server);
[[nodiscard]] bool is_canceled(CancelCallback const& cancel_requested);
[[nodiscard]] ava::core::Error canceled_error(std::string message, McpServerConfig const& server);

[[nodiscard]] ava::core::Result<std::array<int, 2>> make_pipe(McpServerConfig const& server);
[[nodiscard]] bool set_child_process_group(pid_t pid);
[[nodiscard]] pid_t waitpid_retry(pid_t pid, int* status, int options);
[[nodiscard]] ssize_t read_retry(int fd, char* data, std::size_t size);
[[nodiscard]] ssize_t write_retry(int fd, char const* data, std::size_t size);
[[nodiscard]] std::size_t remaining_ms(std::chrono::steady_clock::time_point deadline);
void close_fd(int& fd) noexcept;
void close_nonstandard_fds();

[[nodiscard]] std::string json_string(std::string_view value);
[[nodiscard]] std::string exit_detail(int status);
[[nodiscard]] std::vector<std::string> mcp_argv(McpServerConfig const& server);
[[nodiscard]] std::filesystem::path child_working_dir(McpServerConfig const& server, McpStdioClientOptions const& options);

}  // namespace ava::mcp

#include "sys.h"
#include "ava/debug/print_members_on.h"
#include "ava/app/rpc/input.h"
#include "ava/app/rpc/protocol.h"
#include "ava/core/UniqueFd.h"

#include <array>
#include <cerrno>
#include <istream>
#include <memory>
#include <utility>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

namespace ava::app::rpc {
namespace {

ava::core::Error input_io_error(std::string message)
{
  return ava::core::Error(ava::core::ErrorCategory::Io, std::move(message));
}

void notify_terminal(RpcInputTerminalCallback const& on_terminal, RpcInputTerminalOutcome outcome)
{
  if (on_terminal)
    on_terminal(outcome);
}

class PosixRpcLineReader final : public RpcLineReader
{
 public:
  PosixRpcLineReader(int input_fd, int wake_read_fd, int wake_write_fd) : input_fd_(input_fd), wake_read_fd_(wake_read_fd), wake_write_fd_(wake_write_fd) { }

  ~PosixRpcLineReader() override
  {
    close(wake_read_fd_);
    close(wake_write_fd_);
  }

  [[nodiscard]] ava::core::Result<bool> read_line(std::string& line, RpcInputTerminalCallback const& on_terminal = {}) override
  {
    line.clear();
    bool oversized = false;
    while (true)
    {
      if (canceled_.load(std::memory_order_acquire))
      {
        notify_terminal(on_terminal, RpcInputTerminalOutcome::Canceled);
        return false;
      }

      if (buffer_offset_ == buffer_size_)
      {
        pollfd descriptors[2] = {
            pollfd{.fd = input_fd_, .events = POLLIN, .revents = 0},
            pollfd{.fd = wake_read_fd_, .events = POLLIN, .revents = 0},
        };
        int ready = 0;
        do
        {
          ready = poll(descriptors, 2, -1);
        } while (ready == -1 && errno == EINTR);
        if (ready == -1)
        {
          notify_terminal(on_terminal, RpcInputTerminalOutcome::Error);
          return std::unexpected(input_io_error("failed to poll RPC stdin"));
        }
        if (canceled_.load(std::memory_order_acquire) || (descriptors[1].revents & (POLLIN | POLLHUP | POLLERR | POLLNVAL)) != 0)
        {
          notify_terminal(on_terminal, RpcInputTerminalOutcome::Canceled);
          return false;
        }
        if ((descriptors[0].revents & POLLNVAL) != 0)
        {
          notify_terminal(on_terminal, RpcInputTerminalOutcome::Error);
          return std::unexpected(input_io_error("failed to read RPC stdin"));
        }
        if ((descriptors[0].revents & (POLLIN | POLLHUP | POLLERR)) == 0)
          continue;

        ssize_t count = 0;
        do
        {
          count = read(input_fd_, buffer_.data(), buffer_.size());
        } while (count == -1 && errno == EINTR);
        if (count == -1)
        {
          notify_terminal(on_terminal, RpcInputTerminalOutcome::Error);
          return std::unexpected(input_io_error("failed to read RPC stdin"));
        }
        if (count == 0)
        {
          if (canceled_.load(std::memory_order_acquire))
          {
            notify_terminal(on_terminal, RpcInputTerminalOutcome::Canceled);
            return false;
          }
          if (oversized)
          {
            notify_terminal(on_terminal, RpcInputTerminalOutcome::Error);
            return std::unexpected(invalid_rpc("RPC request line is too large"));
          }
          if (line.empty())
          {
            notify_terminal(on_terminal, RpcInputTerminalOutcome::Eof);
            return false;
          }
          notify_terminal(on_terminal, RpcInputTerminalOutcome::EofWithFinalRecord);
          return true;
        }
        buffer_offset_ = 0;
        buffer_size_ = static_cast<std::size_t>(count);
      }

      char const ch = buffer_[buffer_offset_++];
      if (ch == '\n')
        break;
      if (line.size() >= kMaxRpcLineBytes)
      {
        oversized = true;
        continue;
      }
      if (!oversized)
        line.push_back(ch);
    }
    if (oversized)
      return std::unexpected(invalid_rpc("RPC request line is too large"));
    return true;
  }

  void cancel() noexcept override
  {
    if (canceled_.exchange(true, std::memory_order_acq_rel))
      return;
    char const wake = 1;
    ssize_t written = 0;
    do
    {
      written = write(wake_write_fd_, &wake, 1);
    } while (written == -1 && errno == EINTR);
  }

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT

 private:
  int input_fd_ = -1;
  int wake_read_fd_ = -1;
  int wake_write_fd_ = -1;
  std::array<char, 8192> buffer_{};
  std::size_t buffer_offset_ = 0;
  std::size_t buffer_size_ = 0;
  std::atomic_bool canceled_ = false;
};

}  // namespace

StreamRpcLineReader::StreamRpcLineReader(std::istream& input, RpcInputWake wake) : input_(input), wake_(std::move(wake))
{
}

ava::core::Result<bool> StreamRpcLineReader::read_line(std::string& line, RpcInputTerminalCallback const& on_terminal)
{
  line.clear();
  bool oversized = false;
  while (true)
  {
    if (canceled_.load(std::memory_order_acquire))
    {
      notify_terminal(on_terminal, RpcInputTerminalOutcome::Canceled);
      return false;
    }

    auto next = std::istream::traits_type::eof();
    try
    {
      next = input_.get();
    }
    catch (...)
    {
      // Streams may throw on EOF or rethrow streambuf failures. Translate their
      // state below, after giving a concurrent cancellation precedence.
    }
    // A wake callback can cause get() to return with badbit rather than eofbit. Cancellation takes
    // precedence over stream classification so output failure always has a prompt, stable terminal outcome.
    if (canceled_.load(std::memory_order_acquire))
    {
      notify_terminal(on_terminal, RpcInputTerminalOutcome::Canceled);
      return false;
    }
    if (next == std::istream::traits_type::eof())
    {
      if (!input_.eof() || input_.bad())
      {
        notify_terminal(on_terminal, RpcInputTerminalOutcome::Error);
        return std::unexpected(input_io_error("failed to read RPC stdin"));
      }
      if (oversized)
      {
        notify_terminal(on_terminal, RpcInputTerminalOutcome::Error);
        return std::unexpected(invalid_rpc("RPC request line is too large"));
      }
      if (line.empty())
      {
        notify_terminal(on_terminal, RpcInputTerminalOutcome::Eof);
        return false;
      }
      notify_terminal(on_terminal, RpcInputTerminalOutcome::EofWithFinalRecord);
      return true;
    }

    char const ch = static_cast<char>(next);
    if (ch == '\n')
      break;
    if (line.size() >= kMaxRpcLineBytes)
    {
      oversized = true;
      continue;
    }
    if (!oversized)
      line.push_back(ch);
  }
  if (oversized)
    return std::unexpected(invalid_rpc("RPC request line is too large"));
  return true;
}

void StreamRpcLineReader::cancel() noexcept
{
  if (canceled_.exchange(true, std::memory_order_acq_rel))
    return;
  if (wake_)
  {
    try
    {
      wake_();
    }
    catch (...)
    {
    }
  }
}

ava::core::Result<std::unique_ptr<RpcLineReader>> make_posix_rpc_line_reader(int input_fd)
{
  if (input_fd < 0)
    return std::unexpected(input_io_error("RPC line reader requires a valid input descriptor"));
  int wake_fds[2] = {-1, -1};
  if (pipe2(wake_fds, O_CLOEXEC | O_NONBLOCK) == -1)
    return std::unexpected(input_io_error("failed to create RPC stdin wake pipe"));
  ava::core::UniqueFd wake_read(wake_fds[0]);
  ava::core::UniqueFd wake_write(wake_fds[1]);
  std::unique_ptr<RpcLineReader> reader = std::make_unique<PosixRpcLineReader>(input_fd, wake_read.get(), wake_write.get());
  static_cast<void>(wake_read.release());
  static_cast<void>(wake_write.release());
  return reader;
}

}  // namespace ava::app::rpc

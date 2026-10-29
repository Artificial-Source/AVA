#include "sys.h"
#include "ava/debug/print_members_on.h"
#include "ava/app/acp/envelope_intent.h"
#include "ava/app/acp/transport.h"
#include "ava/core/UniqueFd.h"

#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <memory>
#include <utility>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

namespace ava::app::acp {
namespace {

ava::core::Error io_error(std::string message)
{
  return ava::core::Error(ava::core::ErrorCategory::Io, std::move(message));
}

void close_fd(int& fd) noexcept
{
  if (fd >= 0)
  {
    static_cast<void>(close(fd));
    fd = -1;
  }
}

class FdRecordTransport final : public RecordTransport
{
 public:
  FdRecordTransport(int input_fd, int output_fd, int read_wake_read, int read_wake_write, int write_wake_read, int write_wake_write, int output_flags)
      : input_fd_(input_fd),
        output_fd_(output_fd),
        read_wake_read_(read_wake_read),
        read_wake_write_(read_wake_write),
        write_wake_read_(write_wake_read),
        write_wake_write_(write_wake_write),
        output_flags_(output_flags)
  {
  }

  ~FdRecordTransport() override
  {
    cancel();
    if (output_fd_ >= 0 && output_flags_ >= 0)
      static_cast<void>(fcntl(output_fd_, F_SETFL, output_flags_));
    close_fd(read_wake_read_);
    close_fd(read_wake_write_);
    close_fd(write_wake_read_);
    close_fd(write_wake_write_);
  }

  [[nodiscard]] ReadRecord read_record() override
  {
    std::string line;
    bool oversized = false;
    EnvelopeIntentScanner envelope_scanner;
    auto oversized_record = [&]() {
      auto const scan = envelope_scanner.finish();
      return ReadRecord{.status = ReadRecordStatus::RecoverableError,
                        .record = {},
                        .diagnostic = "ACP record exceeds byte limit",
                        .intent = loop_safe_oversized_intent(scan)};
    };
    while (true)
    {
      if (canceled_.load(std::memory_order_acquire))
        return {.status = ReadRecordStatus::Canceled, .record = {}, .diagnostic = {}};
      if (buffer_offset_ == buffer_size_)
      {
        pollfd descriptors[2] = {
            pollfd{.fd = input_fd_, .events = POLLIN, .revents = 0},
            pollfd{.fd = read_wake_read_, .events = POLLIN, .revents = 0},
        };
        int ready = 0;
        do
        {
          ready = poll(descriptors, 2, -1);
        } while (ready == -1 && errno == EINTR);
        if (ready == -1)
          return {.status = ReadRecordStatus::FatalError, .record = {}, .diagnostic = "failed to poll ACP input"};
        if (canceled_.load(std::memory_order_acquire) || (descriptors[1].revents & (POLLIN | POLLHUP | POLLERR | POLLNVAL)) != 0)
          return {.status = ReadRecordStatus::Canceled, .record = {}, .diagnostic = {}};
        if ((descriptors[0].revents & POLLNVAL) != 0)
          return {.status = ReadRecordStatus::FatalError, .record = {}, .diagnostic = "invalid ACP input descriptor"};
        if ((descriptors[0].revents & (POLLIN | POLLHUP | POLLERR)) == 0)
          continue;

        ssize_t count = 0;
        do
        {
          count = read(input_fd_, buffer_.data(), buffer_.size());
        } while (count == -1 && errno == EINTR);
        if (count == -1)
          return {.status = ReadRecordStatus::FatalError, .record = {}, .diagnostic = "failed to read ACP input"};
        if (count == 0)
        {
          if (oversized)
            return oversized_record();
          if (!line.empty())
          {
            if (line.back() == '\r')
              line.pop_back();
            return {.status = ReadRecordStatus::Record, .record = std::move(line), .diagnostic = {}};
          }
          return {.status = ReadRecordStatus::EndOfFile, .record = {}, .diagnostic = {}};
        }
        buffer_offset_ = 0;
        buffer_size_ = static_cast<std::size_t>(count);
      }

      char const ch = buffer_[buffer_offset_++];
      if (ch == '\n')
      {
        if (oversized)
          return oversized_record();
        if (!line.empty() && line.back() == '\r')
          line.pop_back();
        return {.status = ReadRecordStatus::Record, .record = std::move(line), .diagnostic = {}};
      }
      envelope_scanner.consume(ch);
      if (line.size() >= kMaxRecordBytes)
        oversized = true;
      else if (!oversized)
        line.push_back(ch);
    }
  }

  [[nodiscard]] ava::core::VoidResult write_record(std::string const& record) override
  {
    auto const deadline = std::chrono::steady_clock::now() + kWriteStallTimeout;
    std::size_t offset = 0;
    while (offset < record.size())
    {
      if (canceled_.load(std::memory_order_acquire))
        return std::unexpected(io_error("ACP transport canceled"));
      pollfd descriptors[2] = {
          pollfd{.fd = output_fd_, .events = POLLOUT, .revents = 0},
          pollfd{.fd = write_wake_read_, .events = POLLIN, .revents = 0},
      };
      auto const remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
      if (remaining <= std::chrono::milliseconds::zero())
        return std::unexpected(io_error("ACP output stalled"));
      int ready = 0;
      do
      {
        ready = poll(descriptors, 2, static_cast<int>(remaining.count()));
      } while (ready == -1 && errno == EINTR);
      if (ready == 0)
        return std::unexpected(io_error("ACP output stalled"));
      if (ready == -1)
        return std::unexpected(io_error("failed to poll ACP output"));
      if (canceled_.load(std::memory_order_acquire) || (descriptors[1].revents & (POLLIN | POLLHUP | POLLERR | POLLNVAL)) != 0)
        return std::unexpected(io_error("ACP output canceled"));
      if ((descriptors[0].revents & (POLLERR | POLLHUP | POLLNVAL)) != 0)
        return std::unexpected(io_error("ACP output connection closed"));
      if ((descriptors[0].revents & POLLOUT) == 0)
        continue;
      ssize_t count = 0;
      do
      {
        count = write(output_fd_, record.data() + offset, record.size() - offset);
      } while (count == -1 && errno == EINTR);
      if (count == -1 && (errno == EAGAIN || errno == EWOULDBLOCK))
        continue;
      if (count <= 0)
        return std::unexpected(io_error("failed to write ACP output"));
      offset += static_cast<std::size_t>(count);
    }
    return {};
  }

  void cancel() noexcept override
  {
    if (canceled_.exchange(true, std::memory_order_acq_rel))
      return;
    wake(read_wake_write_);
    wake(write_wake_write_);
  }

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT

 private:
  static void wake(int fd) noexcept
  {
    char const byte = 1;
    ssize_t count = 0;
    do
    {
      count = write(fd, &byte, 1);
    } while (count == -1 && errno == EINTR);
  }

  int input_fd_ = -1;
  int output_fd_ = -1;
  int read_wake_read_ = -1;
  int read_wake_write_ = -1;
  int write_wake_read_ = -1;
  int write_wake_write_ = -1;
  int output_flags_ = -1;
  std::array<char, 8192> buffer_{};
  std::size_t buffer_offset_ = 0;
  std::size_t buffer_size_ = 0;
  std::atomic_bool canceled_ = false;
};

}  // namespace

ava::core::Result<std::unique_ptr<RecordTransport>> make_fd_record_transport(int input_fd, int output_fd)
{
  if (input_fd < 0 || output_fd < 0)
    return std::unexpected(io_error("ACP transport requires valid descriptors"));
  int read_wake[2] = {-1, -1};
  if (pipe2(read_wake, O_CLOEXEC | O_NONBLOCK) == -1)
    return std::unexpected(io_error("failed to create ACP read wake pipe"));
  ava::core::UniqueFd read_wake_read(read_wake[0]);
  ava::core::UniqueFd read_wake_write(read_wake[1]);
  int write_wake[2] = {-1, -1};
  if (pipe2(write_wake, O_CLOEXEC | O_NONBLOCK) == -1)
    return std::unexpected(io_error("failed to create ACP write wake pipe"));
  ava::core::UniqueFd write_wake_read(write_wake[0]);
  ava::core::UniqueFd write_wake_write(write_wake[1]);
  auto const output_flags = fcntl(output_fd, F_GETFL);
  if (output_flags == -1)
    return std::unexpected(io_error("failed to read ACP output flags"));
  // Allocate the owner before changing the borrowed output descriptor's flags.
  std::unique_ptr<RecordTransport> transport = std::make_unique<FdRecordTransport>(input_fd, output_fd, read_wake_read.get(), read_wake_write.get(),
                                                                                   write_wake_read.get(), write_wake_write.get(), output_flags);
  static_cast<void>(read_wake_read.release());
  static_cast<void>(read_wake_write.release());
  static_cast<void>(write_wake_read.release());
  static_cast<void>(write_wake_write.release());
  if (fcntl(output_fd, F_SETFL, output_flags | O_NONBLOCK) == -1)
    return std::unexpected(io_error("failed to configure bounded ACP output"));
  return transport;
}

}  // namespace ava::app::acp

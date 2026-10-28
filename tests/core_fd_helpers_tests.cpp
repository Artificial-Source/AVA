#include "sys.h"
#include "tests/support/session_test_support.h"
#include "tests/support/syscall_errno_injection.h"
#include "tests/support/test_harness.h"
#include "ava/process/deadline.h"
#include "ava/core/UniqueFd.h"
#include "ava/core/ascii.h"
#include "ava/core/atomic_file.h"
#include "ava/core/environment_value.h"
#include "ava/core/fd_io.h"
#include "ava/core/json.h"
#include "ava/core/saturating.h"
#include "ava/core/string_utils.h"
#include "ava/core/text_case.h"
#include "ava/core/utc_timestamp.h"
#include "ava/core/utf8_prefix.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

using ava::core::UniqueFd;

static_assert(!std::is_copy_constructible_v<UniqueFd> && !std::is_copy_assignable_v<UniqueFd>);
static_assert(std::is_nothrow_move_constructible_v<UniqueFd> && std::is_nothrow_move_assignable_v<UniqueFd>);
static_assert(ava::core::saturating_add<std::uint8_t>(250, 8) == 255);
static_assert(ava::core::utf8_prefix_size("a\xc2\xa2", 2) == 1);

UniqueFd fixture_file(std::filesystem::path const& path, std::string_view contents = {})
{
  UniqueFd fd(::open(path.c_str(), O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0600));
  expect(fd && ava::core::write_all(fd.get(), contents) == 0, "core helper fixture creates its private file");
  if (fd)
    expect(::lseek(fd.get(), 0, SEEK_SET) == 0, "core helper fixture rewinds its file");
  return fd;
}

void test_unique_fd()
{
  auto const root = create_empty_root("core-unique-fd");
  UniqueFd empty;
  expect(!empty && empty.get() == -1 && empty.release() == -1 && empty.close_checked() == 0, "empty descriptors have no ownership");
  auto first = fixture_file(root / "first");
  int const first_fd = first.get();
  UniqueFd moved(std::move(first));
  expect(!first && moved.get() == first_fd, "move construction transfers descriptor ownership");
  auto second = fixture_file(root / "second");
  int const second_fd = second.get();
  second = std::move(moved);
  expect(!moved && second.get() == first_fd && ::fcntl(second_fd, F_GETFD) == -1 && errno == EBADF,
         "move assignment closes the old descriptor and transfers the new one");
  second.reset(second.get());
  expect(::fcntl(second.get(), F_GETFD) >= 0, "resetting to the owned descriptor preserves ownership");
  int const released = second.release();
  expect(!second && ::fcntl(released, F_GETFD) >= 0, "release does not close the descriptor");
  {
    UniqueFd final_owner(released);
  }
  expect(::fcntl(released, F_GETFD) == -1 && errno == EBADF, "destruction closes the owned descriptor");
  auto reset_owner = fixture_file(root / "reset");
  int const reset_fd = reset_owner.get();
  reset_owner.reset();
  expect(!reset_owner && ::fcntl(reset_fd, F_GETFD) == -1 && errno == EBADF, "reset closes and empties the descriptor");
  auto checked = fixture_file(root / "checked");
  expect(checked.close_checked() == 0 && !checked, "checked close reports success and clears ownership");
  checked = fixture_file(root / "bad-close");
  expect(::close(checked.get()) == 0, "checked-close failure fixture invalidates its descriptor");
  expect(checked.close_checked() == EBADF && !checked, "checked close preserves errno and clears ownership on failure");

  auto interrupted = fixture_file(root / "interrupted-close");
  auto const status = ava::tests::run_in_child_with_syscall_errno(
      SYS_close, ava::tests::SyscallArgumentFilter{.index = 0, .value = static_cast<std::uint32_t>(interrupted.get())}, EINTR,
      [&] { return interrupted.close_checked() == 0 && !interrupted ? 0 : 1; });
  ava::tests::expect_child_success(status, "checked close treats EINTR as closed without retrying");
}

void test_descriptor_sync_and_bounds()
{
  auto const root = create_empty_root("core-fd-io");
  std::string const contents(9000, 'x');
  auto fd = fixture_file(root / "contents", contents);
  expect(ava::core::fsync_with_restart(fd.get()) == 0 && ava::core::fdatasync_with_restart(fd.get()) == 0, "sync helpers flush a regular file");
  expect(ava::core::fsync_with_restart(-1) == EBADF && ava::core::fdatasync_with_restart(-1) == EBADF, "sync helpers capture syscall errno");
  for (long const number : {SYS_fsync, SYS_fdatasync})
  {
    auto const status = ava::tests::run_in_child_with_syscall_errno(number, std::nullopt, EIO, [&] {
      return (number == SYS_fsync ? ava::core::fsync_with_restart(fd.get()) : ava::core::fdatasync_with_restart(fd.get())) == EIO ? 0 : 1;
    });
    ava::tests::expect_child_success(status, "sync helpers preserve injected EIO");
  }
  auto read = ava::core::read_descriptor_bounded(fd.get(), contents.size());
  expect(read && *read == contents, "bounded read accepts its exact limit across multiple reads");
  expect(::lseek(fd.get(), 0, SEEK_SET) == 0, "bounded read fixture rewinds");
  read = ava::core::read_descriptor_bounded(fd.get(), contents.size() - 1);
  expect(!read && read.error() == EFBIG, "bounded read rejects one byte beyond its limit");
  expect(::lseek(fd.get(), 0, SEEK_SET) == 0, "zero-limit fixture rewinds");
  read = ava::core::read_descriptor_bounded(fd.get(), 0);
  expect(!read && read.error() == EFBIG && ::lseek(fd.get(), 0, SEEK_CUR) == 1, "zero limit consumes only the overflow probe byte");
  auto empty = fixture_file(root / "empty");
  read = ava::core::read_descriptor_bounded(empty.get(), 0);
  expect(read && read->empty(), "zero limit accepts an empty descriptor");
  read = ava::core::read_descriptor_bounded(-1, 1);
  expect(!read && read.error() == EBADF, "bounded read reports syscall failure");
  expect(ava::core::write_all(-1, {}) == 0 && ava::core::write_all(-1, "x") == EBADF, "write_all handles empty input and syscall failure");
  auto const no_progress =
      ava::tests::run_in_child_with_syscall_errno(SYS_write, ava::tests::SyscallArgumentFilter{.index = 0, .value = static_cast<std::uint32_t>(fd.get())}, 0,
                                                  [&] { return ava::core::write_all(fd.get(), "x") == EIO ? 0 : 1; });
  ava::tests::expect_child_success(no_progress, "write_all rejects a zero-progress write");
  expect(::lseek(fd.get(), 0, SEEK_SET) == 0, "cancelable read fixture rewinds");
  read = ava::core::read_descriptor_bounded(fd.get(), contents.size(), [] { return true; });
  expect(!read && read.error() == ECANCELED && ::lseek(fd.get(), 0, SEEK_CUR) == 0, "bounded read cancellation consumes no bytes before the first read");
  int cancellation_checks = 0;
  read = ava::core::read_descriptor_bounded(fd.get(), contents.size(), [&] { return ++cancellation_checks == 2; });
  expect(!read && read.error() == ECANCELED && cancellation_checks == 2 && ::lseek(fd.get(), 0, SEEK_CUR) == 4096,
         "bounded read checks cancellation between chunks without closing its borrowed descriptor");
  expect(::lseek(fd.get(), 0, SEEK_SET) == 0, "uncanceled read fixture rewinds");
  read = ava::core::read_descriptor_bounded(fd.get(), contents.size(), [] { return false; });
  expect(read && *read == contents, "an uncanceled callback read preserves the ordinary bounded-read result");
}

// The timer handler only uses async-signal-safe IO. All state belongs to an
// isolated child; no test-runner signal disposition or descriptor is changed.
sig_atomic_t volatile signal_fd = -1;
sig_atomic_t volatile signal_drains_pipe = 0;
sig_atomic_t volatile signal_calls = 0;
sig_atomic_t volatile signal_bytes = 0;
sig_atomic_t volatile signal_error = 0;

void service_pipe_on_signal(int)
{
  int const saved_errno = errno;
  signal_calls = signal_calls + 1;
  if (signal_drains_pipe)
  {
    std::array<char, 4096> bytes;
    auto const count = ::read(signal_fd, bytes.data(), bytes.size());
    if (count < 0 && errno != EAGAIN && errno != EINTR)
      signal_error = 1;
    if (count > 0)
    {
      signal_bytes = signal_bytes + static_cast<sig_atomic_t>(count);
      for (ssize_t index = 0; index < count; ++index)
      {
        if (bytes[static_cast<std::size_t>(index)] != 'x')
          signal_error = 1;
      }
    }
  }
  else
  {
    if (::write(signal_fd, "x", 1) != 1 || ::close(signal_fd) != 0)
      signal_error = 1;
  }
  errno = saved_errno;
}

bool start_pipe_timer(bool recurring)
{
  struct sigaction action{};
  action.sa_handler = service_pipe_on_signal;
  ::sigemptyset(&action.sa_mask);
  // Deliberately omit SA_RESTART so a blocked syscall returns EINTR/short progress.
  if (::sigaction(SIGALRM, &action, nullptr) != 0)
    return false;
  itimerval timer{};
  timer.it_value.tv_usec = 1000;
  if (recurring)
    timer.it_interval = timer.it_value;
  return ::setitimer(ITIMER_REAL, &timer, nullptr) == 0;
}

enum class PipeInterruption
{
  Read,
  WriteBeforeProgress,
  WriteAfterProgress,
};

int exercise_interrupted_pipe_io(PipeInterruption interruption)
{
  std::array<int, 2> descriptors{};
  if (::pipe2(descriptors.data(), O_CLOEXEC) != 0)
    return 1;
  UniqueFd reader(descriptors[0]);
  UniqueFd writer(descriptors[1]);
  bool const writing = interruption != PipeInterruption::Read;
  bool const initially_full = interruption == PipeInterruption::WriteBeforeProgress;
  signal_calls = 0;
  signal_bytes = 0;
  signal_error = 0;
  signal_drains_pipe = writing;
  if (!writing)
  {
    signal_fd = writer.release(); // The one-shot handler writes, then closes this end.
    if (!start_pipe_timer(false))
      return 2;
    auto const result = ava::core::read_descriptor_bounded(reader.get(), 1);
    return result && *result == "x" && signal_calls == 1 && signal_error == 0 ? 0 : 3;
  }

  if (::fcntl(reader.get(), F_SETFL, O_NONBLOCK) != 0)
    return 4;
  int const capacity = ::fcntl(writer.get(), F_SETPIPE_SZ, 4096);
  if (capacity <= 0)
    return 5;
  std::string const initial(initially_full ? static_cast<std::size_t>(capacity) : 0, 'x');
  std::string const payload(initially_full ? 1 : static_cast<std::size_t>(capacity) * 4, 'x');
  if (ava::core::write_all(writer.get(), initial) != 0)
    return 6;
  signal_fd = reader.get();
  if (!start_pipe_timer(true))
    return 7;
  int const written = ava::core::write_all(writer.get(), payload);
  itimerval const stopped{};
  if (::setitimer(ITIMER_REAL, &stopped, nullptr) != 0)
    return 8;
  writer.reset();
  auto const remaining = ava::core::read_descriptor_bounded(reader.get(), initial.size() + payload.size());
  bool const exact = remaining && remaining->size() + static_cast<std::size_t>(signal_bytes) == initial.size() + payload.size() &&
                     std::ranges::all_of(*remaining, [](char byte) { return byte == 'x'; });
  // The initially full pipe forces EINTR before any progress. The empty pipe
  // accepts one capacity, then signals force positive short writes while draining.
  return written == 0 && exact && signal_calls > 0 && signal_error == 0 ? 0 : 9;
}

void test_interrupted_pipe_io()
{
  for (auto const interruption : {PipeInterruption::Read, PipeInterruption::WriteBeforeProgress, PipeInterruption::WriteAfterProgress})
  {
    auto const status =
        ava::tests::run_in_child_with_syscall_errno(std::span<ava::tests::SyscallErrno const>{}, [&] { return exercise_interrupted_pipe_io(interruption); });
    ava::tests::expect_child_success(status, interruption == PipeInterruption::Read ? "bounded read retries an interrupted pipe read"
                                                                                    : "write_all retries EINTR and short pipe writes without dropping bytes");
  }
}

void test_open_regular_and_paths()
{
  auto const root = create_empty_root("core-regular-open");
  auto original = fixture_file(root / "regular", "abc");
  UniqueFd directory(::open(root.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
  expect(static_cast<bool>(directory), "regular-open fixture opens its parent");
  auto file = ava::core::open_regular_nofollow(directory.get(), "regular");
  expect(file && file->status.st_size == 3 && S_ISREG(file->status.st_mode), "regular open returns descriptor-verified metadata");
  expect(file && (::fcntl(file->fd.get(), F_GETFL) & O_NONBLOCK) == 0 && (::fcntl(file->fd.get(), F_GETFD) & FD_CLOEXEC) != 0,
         "regular open clears nonblocking and preserves close-on-exec");
  expect(::symlinkat("regular", directory.get(), "symlink") == 0, "regular-open fixture creates its final symlink");
  UniqueFd borrowed(::openat(directory.get(), "symlink", O_RDONLY | O_NONBLOCK | O_CLOEXEC));
  int const borrowed_number = borrowed.get();
  auto prepared = ava::core::prepare_regular_descriptor(borrowed.get());
  expect(prepared && prepared->st_size == 3 && borrowed.get() == borrowed_number && (::fcntl(borrowed.get(), F_GETFL) & O_NONBLOCK) == 0,
         "preparing a borrowed descriptor preserves its identity and the caller's symlink-resolution policy");
  auto invalid_prepared = ava::core::prepare_regular_descriptor(-1);
  expect(!invalid_prepared && invalid_prepared.error().failure == ava::core::OpenRegularFailure::Stat && invalid_prepared.error().error_number == EBADF,
         "preparing an invalid descriptor preserves its stat failure");
  invalid_prepared = ava::core::prepare_regular_descriptor(directory.get());
  expect(!invalid_prepared && invalid_prepared.error().failure == ava::core::OpenRegularFailure::NotRegular && ::fcntl(directory.get(), F_GETFD) >= 0,
         "preparing a non-regular descriptor rejects its type without closing it");
  auto const flag_failure = ava::tests::run_in_child_with_syscall_errno(SYS_fcntl, ava::tests::SyscallArgumentFilter{.index = 1, .value = F_SETFL}, EIO, [&] {
    auto const result = ava::core::prepare_regular_descriptor(borrowed.get());
    return !result && result.error().failure == ava::core::OpenRegularFailure::Flags && result.error().error_number == EIO &&
                   ::fcntl(borrowed.get(), F_GETFD) >= 0
               ? 0
               : 1;
  });
  ava::tests::expect_child_success(flag_failure, "preparing a borrowed descriptor reports flag failures without taking ownership");
  auto symlink = ava::core::open_regular_nofollow(directory.get(), "symlink");
  expect(!symlink && symlink.error().failure == ava::core::OpenRegularFailure::Open && symlink.error().error_number == ELOOP,
         "regular open rejects a final symlink");
  auto folder = ava::core::open_regular_nofollow(directory.get(), ".");
  expect(!folder && folder.error().failure == ava::core::OpenRegularFailure::NotRegular, "regular open distinguishes a non-regular directory");
  auto missing = ava::core::open_regular_nofollow(directory.get(), "missing");
  expect(!missing && missing.error().failure == ava::core::OpenRegularFailure::Open && missing.error().error_number == ENOENT,
         "regular open preserves missing-path errno");
  expect(::mkfifoat(directory.get(), "fifo", 0600) == 0, "regular-open fixture creates its FIFO");
  auto const fifo = ava::tests::run_in_child_with_syscall_errno(std::span<ava::tests::SyscallErrno const>{}, [&] {
    auto result = ava::core::open_regular_nofollow(directory.get(), "fifo");
    return !result && result.error().failure == ava::core::OpenRegularFailure::NotRegular ? 0 : 1;
  });
  ava::tests::expect_child_success(fifo, "regular open rejects a FIFO without waiting for a writer");

  expect(ava::core::descriptor_proc_path(original.get()) == "/proc/self/fd/" + std::to_string(original.get()), "proc path names the exact descriptor");
  auto physical = ava::core::descriptor_physical_path(original.get());
  expect(physical && *physical == std::filesystem::canonical(root / "regular"), "physical path resolves the opened inode");
  auto checked_path = ava::core::descriptor_physical_path_checked(original.get());
  expect(checked_path && physical && *checked_path == *physical, "checked and optional physical-path helpers resolve the same inode");
  expect(::unlinkat(directory.get(), "regular", 0) == 0 && !ava::core::descriptor_physical_path(original.get()), "physical path rejects a deleted inode");
  checked_path = ava::core::descriptor_physical_path_checked(original.get());
  expect(!checked_path && checked_path.error().failure == ava::core::DescriptorPathFailure::Deleted && checked_path.error().error_number == 0,
         "checked physical path distinguishes a deleted inode without inventing errno");
  expect(!ava::core::descriptor_physical_path(-1), "physical path rejects an invalid descriptor");
  checked_path = ava::core::descriptor_physical_path_checked(-1);
  expect(!checked_path && checked_path.error().failure == ava::core::DescriptorPathFailure::Readlink && checked_path.error().error_number == ENOENT,
         "checked physical path captures readlink errno for an absent descriptor");
  std::array<int, 2> pipe{};
  if (::pipe2(pipe.data(), O_CLOEXEC) == 0)
  {
    UniqueFd read_end(pipe[0]);
    UniqueFd write_end(pipe[1]);
    expect(!ava::core::descriptor_physical_path(read_end.get()), "physical path rejects non-absolute anonymous descriptor targets");
    checked_path = ava::core::descriptor_physical_path_checked(read_end.get());
    expect(!checked_path && checked_path.error().failure == ava::core::DescriptorPathFailure::NotAbsolute && checked_path.error().error_number == 0,
           "checked physical path distinguishes a non-absolute descriptor identity");
  }
  else
    expect(false, "physical-path fixture creates its pipe");
  auto long_parent = root;
  for (int index = 0; index < 4; ++index)
    long_parent /= std::string(100, 'p');
  std::filesystem::create_directories(long_parent);
  auto long_file = fixture_file(long_parent / "long-name");
  physical = ava::core::descriptor_physical_path(long_file.get());
  expect(physical && *physical == std::filesystem::canonical(long_parent / "long-name"), "physical path grows its readlink buffer without truncation");
}

void test_rename_and_directory_stream()
{
  auto const root = create_empty_root("core-rename-directory");
  UniqueFd directory(::open(root.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
  auto source = fixture_file(root / "source", "source");
  auto occupied = fixture_file(root / "occupied", "occupied");
  expect(ava::core::rename_noreplace(directory.get(), "source", directory.get(), "occupied") == EEXIST && std::filesystem::exists(root / "source"),
         "noreplace rename preserves both names on collision");
  expect(ava::core::rename_noreplace(directory.get(), "source", directory.get(), "published") == 0 && !std::filesystem::exists(root / "source"),
         "noreplace rename publishes to a free name");
  expect(ava::core::read_descriptor_bounded(occupied.get(), 8) == std::expected<std::string, int>("occupied"), "noreplace leaves existing contents intact");
#ifdef SYS_renameat2
  for (int const error : {EINVAL, ENOSYS})
  {
    auto const status = ava::tests::run_in_child_with_syscall_errno(SYS_renameat2, std::nullopt, error, [&] {
      return ava::core::rename_noreplace(directory.get(), "published", directory.get(), "unavailable") == error &&
                     std::filesystem::exists(root / "published") && !std::filesystem::exists(root / "unavailable")
                 ? 0
                 : 1;
    });
    ava::tests::expect_child_success(status, "noreplace rename propagates unsupported errors without downgrading");
  }
#endif
  int const borrowed = directory.get();
  {
    auto stream = ava::core::open_directory_stream(std::move(directory));
    expect(stream && !directory && ::dirfd(stream->get()) == borrowed, "directory stream adopts the descriptor without duplicating it");
    bool found = false;
    if (stream)
    {
      while (auto const* entry = ::readdir(stream->get()))
        found = found || std::string_view(entry->d_name) == "published";
    }
    expect(found, "directory stream enumerates the anchored directory");
  }
  expect(::fcntl(borrowed, F_GETFD) == -1 && errno == EBADF, "directory stream destruction closes its descriptor");
  int const non_directory = source.get();
  auto invalid = ava::core::open_directory_stream(std::move(source));
  expect(!invalid && invalid.error() == ENOTDIR && !source && ::fcntl(non_directory, F_GETFD) == -1 && errno == EBADF,
         "failed directory stream creation closes its consumed descriptor");
  invalid = ava::core::open_directory_stream(UniqueFd{});
  expect(!invalid && invalid.error() == EBADF, "directory stream reports invalid-descriptor errno");
}

void test_text_environment_and_deadline_helpers()
{
  for (unsigned int byte = 0; byte < 256; ++byte)
  {
    auto const ch = static_cast<char>(byte);
    auto const control = byte < 0x20 || byte == 0x7f;
    expect(ava::core::is_c0_or_del(static_cast<unsigned char>(byte)) == control && ava::core::has_c0_or_del(std::string_view(&ch, 1)) == control,
           "shared C0/DEL predicates preserve all 256 byte classifications");
    auto const ascii_space = byte == ' ' || byte == '\t' || byte == '\n' || byte == '\r' || byte == '\f' || byte == '\v';
    expect(ava::core::is_ascii_space(static_cast<unsigned char>(byte)) == ascii_space, "ASCII whitespace classification is exact for all 256 bytes");
    auto const expected_ascii = byte >= 'A' && byte <= 'Z' ? static_cast<char>(byte + ('a' - 'A')) : ch;
    expect(ava::core::ascii_lower(ch) == expected_ascii && ava::core::ascii_lower_copy(std::string_view(&ch, 1)) == std::string(1, expected_ascii),
           "ASCII folding preserves every non-ASCII byte");
    auto folded = std::string(1, ch);
    ava::core::locale_lower_in_place(folded);
    auto const expected_locale = std::string(1, static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
    expect(folded == expected_locale && ava::core::locale_lower_copy(std::string_view(&ch, 1)) == expected_locale,
           "locale folding preserves the unsigned-byte cctype contract");
  }
  expect(!ava::core::has_c0_or_del({}) && ava::core::ascii_lower_copy({}).empty() && ava::core::locale_lower_copy({}).empty(),
         "shared byte helpers accept empty text");
  expect(ava::core::trim_view(std::string_view{" \t\r\n\f\vtext \t\r\n\f\v"}) == "text" && ava::core::trim(" \t\r\n\f\v").empty(),
         "shared trim preserves the six C-locale whitespace bytes");
  expect(ava::core::ascii_trim(" \t\r\n\f\vtext \t\r\n\f\v") == "text" && ava::core::ascii_trim(" \t\r\n\f\v").empty() &&
             ava::core::ascii_trim("\xa0text\xa0") == "\xa0text\xa0",
         "ASCII trimming removes only its explicit whitespace set");

  using Clock = std::chrono::steady_clock;
  constexpr auto tick = Clock::duration{1};
  constexpr auto origin = Clock::time_point{};
  constexpr auto maximum = Clock::time_point::max();
  constexpr auto minimum = Clock::time_point::min();
  expect(ava::process::saturating_deadline_add(origin, Clock::duration::zero()) == origin && ava::process::saturating_deadline_add(origin, -tick) == origin &&
             ava::process::saturating_deadline_add(origin, Clock::duration::min()) == origin,
         "nonpositive deadline budgets leave the original deadline unchanged");
  expect(ava::process::saturating_deadline_add(origin, tick) == origin + tick && ava::process::saturating_deadline_add(maximum - tick, tick) == maximum &&
             ava::process::saturating_deadline_add(maximum, tick) == maximum && ava::process::saturating_deadline_add(maximum - tick, tick + tick) == maximum,
         "deadline addition preserves ordinary sums and saturates at the exact maximum");
  expect(ava::process::saturating_deadline_add(minimum, Clock::duration::max()) == minimum + Clock::duration::max(),
         "deadline addition remains safe for negative time points and the largest positive budget");

  expect(ava::core::format_utc_timestamp(0) == "1970-01-01T00:00:00Z", "UTC timestamp retains the persisted epoch representation");
  auto const leap_day = std::chrono::sys_days(std::chrono::year{2000} / 2 / 29) + std::chrono::hours{12} + std::chrono::minutes{34} + std::chrono::seconds{56};
  expect(ava::core::format_utc_timestamp(std::chrono::system_clock::to_time_t(leap_day)) == "2000-02-29T12:34:56Z",
         "UTC timestamp preserves leap-day and whole-second formatting");

  constexpr auto variable = "AVA_TEST_NONEMPTY_ENVIRONMENT_VALUE";
  ::ScopedEnvVar absent(variable, std::nullopt);
  expect(!ava::core::nonempty_environment_value(variable), "missing environment values remain absent");
  {
    ::ScopedEnvVar empty(variable, "");
    expect(!ava::core::nonempty_environment_value(variable), "empty environment values remain absent");
  }
  {
    ::ScopedEnvVar present(variable, "  retained value  ");
    auto const value = ava::core::nonempty_environment_value(variable);
    expect(value && *value == "  retained value  ", "nonempty environment lookup does not trim or normalize bytes");
    ::ScopedEnvVar changed(variable, "replacement");
    expect(value && *value == "  retained value  ", "environment lookup owns its copied value");
  }
}

void test_saturating_add_and_utf8_prefix()
{
  constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
  expect(ava::core::saturating_add<std::uint64_t>(0, 0) == 0 && ava::core::saturating_add<std::uint64_t>(4, 5) == 9,
         "unsigned saturation preserves ordinary sums");
  expect(ava::core::saturating_add(maximum - 1, std::uint64_t{1}) == maximum && ava::core::saturating_add(maximum, std::uint64_t{1}) == maximum &&
             ava::core::saturating_add(maximum, maximum) == maximum,
         "unsigned saturation handles exact maximum and overflow");
  std::string_view const text = "a\xc2\xa2\xe2\x82\xac\xf0\x9f\x92\xa9z";
  constexpr std::array<std::size_t, 13> prefixes{0, 1, 1, 3, 3, 3, 6, 6, 6, 6, 10, 11, 11};
  for (std::size_t limit = 0; limit < prefixes.size(); ++limit)
    expect(ava::core::utf8_prefix_size(text, limit) == prefixes[limit], "UTF-8 prefix respects scalar byte boundaries");
  expect(ava::core::utf8_prefix_size({}, 10) == 0 && ava::core::utf8_prefix_size(text, std::numeric_limits<std::size_t>::max()) == text.size(),
         "UTF-8 prefix handles empty input and an unbounded limit");
  std::string_view const malformed = "\xff\x80\xc0\xe2\x82";
  for (std::size_t limit = 0; limit <= malformed.size(); ++limit)
    expect(ava::core::utf8_prefix_size(malformed, limit) == limit, "UTF-8 prefix consumes malformed and truncated scalars one byte at a time");
  expect(ava::core::utf8_prefix_size("\xff\xc2\xa2", 2) == 1, "invalid bytes do not permit splitting a later valid scalar");
}

void test_restart_on_eintr()
{
  int calls = 0;
  auto const result = ava::core::restart_on_eintr([&] {
    if (++calls == 1)
    {
      errno = EINTR;
      return -1;
    }
    return 7;
  });
  expect(result == 7 && calls == 2, "public syscall restart retries EINTR and preserves the successful syscall result");
  calls = 0;
  auto const failed = ava::core::restart_on_eintr([&] {
    ++calls;
    errno = EIO;
    return -1;
  });
  expect(failed == -1 && errno == EIO && calls == 1, "public syscall restart preserves non-EINTR failures without retrying");
}

int exercise_child_wait_deadlines()
{
  for (bool const expired : {false, true})
  {
    auto const child = ::fork();
    if (child < 0)
      return 1;
    if (child == 0)
    {
      if (expired)
      {
        while (true)
          static_cast<void>(::pause());
      }
      ::_exit(42);
    }
    auto const deadline = expired ? std::chrono::steady_clock::now() : ava::tests::now_plus_seconds(5);
    auto const status = ava::tests::wait_child_until(child, deadline);
    int const wait_error = errno;
    if (expired ? status.has_value() || wait_error != ETIMEDOUT : !status || !WIFEXITED(*status) || WEXITSTATUS(*status) != 42)
      return 2;
    if (::waitpid(child, nullptr, WNOHANG) != -1 || errno != ECHILD)
      return 3;
  }
  return 0;
}

void test_child_wait_deadlines()
{
  expect(!ava::tests::wait_child_until(0, ava::tests::now_plus_seconds(5)) && errno == EINVAL, "bounded child wait rejects non-child identifiers");
  expect(exercise_child_wait_deadlines() == 0, "bounded child wait returns exit status and reaps both exited and timed-out children");
#ifdef SYS_pidfd_open
  auto const status = ava::tests::run_in_child_with_syscall_errno(SYS_pidfd_open, std::nullopt, ENOSYS, exercise_child_wait_deadlines);
  ava::tests::expect_child_success(status, "bounded child wait preserves status and timeout cleanup when pidfds are unavailable");
#endif
}

void test_json_compact()
{
  auto const compacted = ava::core::json::compact(" \r\n { \"a\" : [ 1, true, null ], \"text\": \"a b\\n\\\"c\\\\d\" }\t ");
  expect(compacted && *compacted == R"({"a":[1,true,null],"text":"a b\n\"c\\d"})", "JSON compaction preserves string content and escapes");
  expect(compacted && ava::core::json::is_valid_object(*compacted) && ava::core::json::compact(*compacted) == compacted,
         "JSON compaction preserves validity and is idempotent");
  for (std::string_view const invalid : {"{\"a\":\"raw\nline\"}", "{\"a\":\"raw\rline\"}", "\"\\\n\""})
    expect(!ava::core::json::compact(invalid), "JSON compaction rejects raw line breaks inside strings, including after a backslash");
  expect(ava::core::json::compact(" \n\r\t ") == std::optional<std::string>(""), "JSON compaction accepts an empty frame like the original framing helper");
  expect(ava::core::json::compact(" \"unterminated ") == std::optional<std::string>("\"unterminated "), "JSON compaction is not a second syntax validator");
  expect(ava::core::json::compact("\"a\tb\"") == std::optional<std::string>("\"a\tb\""), "JSON compaction preserves non-line-break string bytes verbatim");
}

void test_atomic_publication_failure_preserves_previous_file()
{
  auto const root = create_empty_root("core-atomic-publish-failure");
  auto const target = root / "settings.json";
  ava::tests::write_test_file(target, "previous\n");
  constexpr ava::tests::SyscallErrno rules[] = {
#ifdef SYS_rename
      {.number = SYS_rename, .argument = std::nullopt, .error_number = EIO},
#endif
#ifdef SYS_renameat
      {.number = SYS_renameat, .argument = std::nullopt, .error_number = EIO},
#endif
#ifdef SYS_renameat2
      {.number = SYS_renameat2, .argument = std::nullopt, .error_number = EIO},
#endif
  };
  auto const status = ava::tests::run_in_child_with_syscall_errno(std::span(rules), [&] {
    auto const result = ava::core::write_text_file_atomic(target, "replacement\n", "atomic publication fixture");
    return !result && result.error().category() == ava::core::ErrorCategory::Io ? 0 : 1;
  });
  auto const outcome = ava::tests::expect_child_success(status, "atomic publication reports a failed rename");
  if (outcome != ava::tests::ChildTestOutcome::Passed)
    return;
  ava::tests::expect_eq(ava::tests::read_session_test_binary_file(target), std::string("previous\n"),
                        "failed publication preserves the previous durable contents");
  bool temporary_found = false;
  for (auto const& entry : std::filesystem::directory_iterator(root))
    temporary_found = temporary_found || entry.path().filename().string().starts_with("settings.json.tmp.");
  expect(!temporary_found, "failed atomic publication removes its unpublished temporary");
}

}  // namespace

void run_core_fd_helpers_tests()
{
  test_unique_fd();
  test_descriptor_sync_and_bounds();
  test_interrupted_pipe_io();
  test_open_regular_and_paths();
  test_rename_and_directory_stream();
  test_atomic_publication_failure_preserves_previous_file();
  test_saturating_add_and_utf8_prefix();
  test_text_environment_and_deadline_helpers();
  test_json_compact();
  test_restart_on_eintr();
  test_child_wait_deadlines();
}

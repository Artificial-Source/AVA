#include "sys.h"
#include "ava/containment/containment.h"

#include <array>
#include <bit>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/netlink.h>
#include <linux/seccomp.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <unistd.h>

#ifndef AF_INET
#include <sys/socket.h>
#endif

namespace ava::containment {
namespace {

// Detect the native audit architecture at compile time. The seccomp filter
// checks the architecture of each syscall so a 64-bit filter cannot be bypassed
// by a 32-bit syscall (x32 ABI on x86_64).
#if defined(__x86_64__)
constexpr std::uint32_t kNativeAuditArch = AUDIT_ARCH_X86_64;
constexpr bool kIsX86_64 = true;
#elif defined(__aarch64__)
constexpr std::uint32_t kNativeAuditArch = AUDIT_ARCH_AARCH64;
constexpr bool kIsX86_64 = false;
#else
// On unsupported architectures the filter is not built; the plan reports
// Unavailable and callers downgrade to Ask.
constexpr std::uint32_t kNativeAuditArch = 0;
constexpr bool kIsX86_64 = false;
#endif

// io_uring_setup has a stable syscall number (425) on both supported
// architectures. Define it unconditionally so the filter always blocks it,
// even when the installed kernel headers predate io_uring. A static assertion
// verifies the header constant matches when both are available.
constexpr std::uint32_t kStableIoUringSetupSyscall = 425;

#ifdef __NR_io_uring_setup
static_assert(static_cast<std::uint32_t>(__NR_io_uring_setup) == kStableIoUringSetupSyscall,
              "stable io_uring_setup syscall number must match the kernel header");
constexpr std::uint32_t kIoUringSetupSyscall = static_cast<std::uint32_t>(__NR_io_uring_setup);
#else
constexpr std::uint32_t kIoUringSetupSyscall = kStableIoUringSetupSyscall;
#endif

// On x86_64, the x32 ABI shares the AUDIT_ARCH_X86_64 architecture but uses
// syscall numbers with __X32_SYSCALL_BIT (0x40000000) set. Rejecting any
// syscall number with this bit prevents x32 bypass of the filter.
#if defined(__x86_64__)
constexpr std::uint32_t kX32SyscallBit = 0x40000000;
#ifdef __X32_SYSCALL_BIT
static_assert(static_cast<std::uint32_t>(__X32_SYSCALL_BIT) == kX32SyscallBit, "stable x32 syscall bit must match the kernel header");
#endif
#else
constexpr std::uint32_t kX32SyscallBit = 0;
#endif

[[nodiscard]] constexpr auto build_network_filter(bool network_allowed)
{
  constexpr auto denied = SECCOMP_RET_ERRNO | (EPERM & SECCOMP_RET_DATA);
  auto const network_action = network_allowed ? SECCOMP_RET_ALLOW : denied;
  // Linux consumes these socket arguments as ints. Inspect their low 32 bits,
  // including on big-endian AArch64 where those bits occupy the second word.
  constexpr auto low_word = std::endian::native == std::endian::big ? sizeof(std::uint32_t) : 0;
  constexpr auto domain_offset = offsetof(struct seccomp_data, args[0]) + low_word;
  constexpr auto type_offset = offsetof(struct seccomp_data, args[1]) + low_word;
  constexpr auto protocol_offset = offsetof(struct seccomp_data, args[2]) + low_word;

  // Both modes reject foreign architectures, x32, and io_uring socket bypasses.
  // Non-stdio inherited descriptors must be closed before exec. Private UNIX
  // stream/seqpacket pairs cannot address external services, so their send
  // syscalls remain available even when external networking is denied.
  return std::to_array<sock_filter>({
      BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, arch)),
      // Native architecture skips the kill; foreign architectures fall through.
      BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, kNativeAuditArch, 1, 0),
      BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS),
      BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, nr)),
      // x32 falls through to kill; native syscall numbers skip it.
      BPF_JUMP(BPF_JMP | BPF_JSET | BPF_K, kIsX86_64 ? kX32SyscallBit : 0, 0, 1),
      BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS),
      // io_uring_setup falls through to deny; other syscalls skip it.
      BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, kIoUringSetupSyscall, 0, 1),
      BPF_STMT(BPF_RET | BPF_K, denied),
      // connect falls through to network_action; other syscalls skip it.
      BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_connect, 0, 1),
      BPF_STMT(BPF_RET | BPF_K, network_action),

      // socket(): IP and route-query sockets only in network-allowed mode.
      // The route exception supports glibc getaddrinfo/AI_ADDRCONFIG.
      // Non-socket syscalls skip eight instructions to the socketpair check.
      BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_socket, 0, 8),
      BPF_STMT(BPF_LD | BPF_W | BPF_ABS, domain_offset),
      // IPv4 skips four instructions to network_action; otherwise test IPv6.
      BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, AF_INET, 4, 0),
      // IPv6 skips three instructions to network_action; otherwise test netlink.
      BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, AF_INET6, 3, 0),
      // Netlink falls through to its protocol check; other domains skip to deny.
      BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, AF_NETLINK, 0, 3),
      BPF_STMT(BPF_LD | BPF_W | BPF_ABS, protocol_offset),
      // NETLINK_ROUTE falls through to network_action; other protocols skip to deny.
      BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, NETLINK_ROUTE, 0, 1),
      BPF_STMT(BPF_RET | BPF_K, network_action),
      BPF_STMT(BPF_RET | BPF_K, denied),

      // socketpair(): only connected UNIX stream/seqpacket peers, in both
      // modes. Datagram pairs can send to unrelated sockets and are denied.
      // Non-socketpair syscalls skip eight instructions to the final allow.
      BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_socketpair, 0, 8),
      BPF_STMT(BPF_LD | BPF_W | BPF_ABS, domain_offset),
      // UNIX falls through to its type check; other domains skip five to deny.
      BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, AF_UNIX, 0, 5),
      BPF_STMT(BPF_LD | BPF_W | BPF_ABS, type_offset),
      BPF_STMT(BPF_ALU | BPF_AND | BPF_K, static_cast<std::uint32_t>(~(SOCK_CLOEXEC | SOCK_NONBLOCK))),
      // Streams skip the seqpacket check to allow; other types fall through.
      BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SOCK_STREAM, 1, 0),
      // Seqpacket falls through to allow; all other types skip to deny.
      BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SOCK_SEQPACKET, 0, 1),
      BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
      BPF_STMT(BPF_RET | BPF_K, denied),

      // Includes sendto/sendmsg/sendmmsg for the permitted connected pairs.
      BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
  });
}

static_assert(build_network_filter(false).size() == 29, "update the documented seccomp jump offsets when changing the filter");

}  // namespace

bool seccomp_network_filter_supported() noexcept
{
  if (kNativeAuditArch == 0)
    return false;

  // Non-mutating probe: verify the kernel supports SECCOMP_RET_ERRNO via
  // SECCOMP_GET_ACTION_AVAIL. This avoids installing a filter that the kernel
  // silently ignores or rejects at exec time.
  std::uint32_t const action = SECCOMP_RET_ERRNO;
  long const result = ::syscall(SYS_seccomp, SECCOMP_GET_ACTION_AVAIL, 0u, &action);
  // An outer sandbox may deny this query. Never infer support from a different
  // probe: the command must downgrade to Ask when the required action is unknown.
  return result == 0;
}

ava::core::VoidResult apply_seccomp_network_filter(bool network_allowed)
{
  if (!seccomp_network_filter_supported())
    return std::unexpected(
        ava::core::Error(ava::core::ErrorCategory::PermissionDenied, "seccomp network filter is not supported on this architecture or kernel"));

  // no_new_privs must be set before installing a seccomp filter. It is
  // inherited across execve and prevents the child from gaining privileges
  // through setuid binaries.
  if (::prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0)
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::PermissionDenied, "failed to set PR_SET_NO_NEW_PRIVS before seccomp"));

  auto filter = build_network_filter(network_allowed);

  struct sock_fprog prog{};
  prog.len = static_cast<std::uint16_t>(filter.size());
  prog.filter = filter.data();

  if (::syscall(SYS_seccomp, SECCOMP_SET_MODE_FILTER, 0u, &prog) != 0)
  {
    // Fall back to prctl if seccomp(2) is unavailable (older kernels).
    if (::prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &prog) != 0)
      return std::unexpected(ava::core::Error(ava::core::ErrorCategory::PermissionDenied, "failed to install seccomp network filter"));
  }
  return {};
}

ava::core::VoidResult apply_no_new_privs()
{
  if (::prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0)
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::PermissionDenied, "failed to set PR_SET_NO_NEW_PRIVS"));
  return {};
}

}  // namespace ava::containment

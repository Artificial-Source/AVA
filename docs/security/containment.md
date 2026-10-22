# Development Containment

## Overview

AVA's sealed local command runner applies verified Linux development containment
to standard commands that execute mutable project code (build, test, etc.) before
releasing the child process to exec. Containment uses two kernel mechanisms:

1. **Landlock** (filesystem containment): restricts filesystem access to
   explicitly allowed roots.
2. **seccomp** (socket/network containment): installs a filter in both network
   modes, restricting external sockets while retaining private pair IPC.

## Product Contract

| Command Level | Containment | Permission |
|---------------|-------------|------------|
| Standard (inspection, no path race) | Not required | Allow |
| Standard (path-bearing inspection) + available | Applied before exec | Allow |
| Standard (mutable) + available | Applied before exec | Allow |
| Standard (mutable) + unavailable | Not applied | Critical, one-shot Ask (uncontained warning) |
| Sensitive + available | Applied after approval | Ask |
| Sensitive + unavailable | Not applied | Critical, one-shot Ask (uncontained warning) |
| Critical / Raw / Unverified | Applied only when mutable-code policy requires it and a verified local plan is available | Ask once |

Critical, raw-shell, and unverified delegated commands have an absolute backend
maximum of Once. Stable, secret-free direct-argv Standard/Sensitive recipes may
advertise Session or Workspace scopes; grants bind typed recipe keys, never raw
command text or an instantaneous plan fingerprint.

When containment is unavailable, the command is downgraded to Critical risk and
requires explicit one-shot approval. After approval it may run uncontained, but
metadata and audit must state unavailable/uncontained. No reusable grant is
issued. This preserves user authority: unavailable containment must never look
like ordinary Sensitive/Standard containment.

This contract covers contained child execution, not file-tool reads performed
inside AVA. Those reads use application permissions: `--allow read-only` and
matching `--allow-tool` flags currently auto-approve otherwise-Ask reads/searches
outside the workspace, but do not override hard secret-path denies. They are not
a workspace-only read boundary; see the [file-tool policy](sandboxing.md#secrets-protected-paths-and-destructive-commands).

## Root Validation

Root validation is split between synthetic and writable roots:

- **Synthetic roots** (HOME, XDG, TMP): must be exact current-user 0700
  (owner-only, no group/world permissions). These are private, per-run
  directories created by AVA.
- **Writable workspace**: may be current-user-owned 0755 or 0750 (group
  read/execute permitted). Group write is accepted only for the verified
  private-primary-group exception (for example 0770 with a primary group that
  has no other members); shared-group or world write, non-directory targets,
  anchor escapes, and identity mismatches are rejected. A logical workspace
  symlink is accepted when the selected descriptor anchor safely resolves it.

## Authority and Trusted-Home Boundaries

At command sealing:

- The workspace must not equal or be an ancestor of the trusted real home
  directory. Ordinary projects nested under home are allowed.
- The workspace must not overlap with any AVA authority node (config, state,
  session directories, or exact protected credential files).
- Synthetic environment roots must be disjoint from workspace and trusted home.
  An exact synthetic run root may live beneath the pre-opened session/spill
  authority; granting that exact descriptor does not grant its authority parent.
- PATH directories and executable/interpreter files must be disjoint from AVA
  authority roots and exact protected credential files. Executable checks cover
  both requested and canonical file paths; parent-directory grants are narrowed
  separately as described below.

Workspace and PATH authority overlaps are rejected at sealing, and overlapping
fixed system roots are omitted. Startup PATH entries that are `/`, equal the
trusted home, or contain it are dropped from discovery, including trailing-slash
spellings and symlink/device-inode aliases.

Executable/interpreter parents normally receive directory read/execute rules.
If a parent equals or contains the trusted home, or overlaps an AVA authority
root, only the exact executable/interpreter file receives READ_FILE and EXECUTE.
Home checks cover lexical spellings (including trailing slashes) and descriptor
device/inode aliases; an uninspectable parent does not gain broad authority.
These file checks and grants are implemented in
`src/ava/command/command_plan.cpp` and
`src/ava/containment/containment_plan.cpp`.

### Executable and Interpreter Target Admission

The canonical target of the executable and every resolved shebang interpreter
must be covered by the roots the containment plan grants. Coverage compares
physical target paths with descriptor-resolved directory identities, so a
symlinked workspace root is not rejected merely for its logical spelling.
If a launcher or interpreter resolves outside all granted roots, the plan is
`Unavailable`; AVA does not add an arbitrary canonical parent to make it fit.
The command instead requires explicit Critical, one-shot approval to run
uncontained, rather than auto-running. This can affect nvm, pipx, or Nix
toolchain symlink layouts when their targets are outside the granted roots.

## What Containment Provides

### Filesystem (Landlock)

Landlock rights are ABI-version-aware:

- **ABI 1**: execute, write_file, read_file, read_dir, remove_dir, remove_file,
  make_char, make_dir, make_reg, make_sock, make_fifo, make_block, make_sym
- **ABI 2**: + `REFER` (rename/link across roots)
- **ABI 3** (minimum required): + `TRUNCATE`
- **ABI 5**: + `IOCTL_DEV` (included in handled mask when supported; writable
  roots may receive it)
- **ABI 6**: + signal and abstract UNIX socket scoping; contained commands
  cannot signal processes or connect to abstract sockets outside their Landlock
  domain.

- **Handled rights**: every supported Landlock filesystem access right for the
  probed ABI version is handled (denied by default unless explicitly allowed).
- **Rule identity**: every allow root is descriptor-identified in the parent;
  the child reopens the preserved logical spelling, verifies that identity, and
  adds that same descriptor to Landlock. A post-approval pathname replacement
  fails before restriction/exec.
- **Writable roots**: the descriptor-bound logical workspace and per-run synthetic HOME/XDG/TMP
  roots receive read/write/create/execute permissions (including IOCTL_DEV when
  ABI >= 5).
- **Read/execute roots**: system roots (`/usr`, `/lib`, `/lib64`, `/bin`,
  `/sbin`, `/etc`), sealed PATH/toolchain entries, and ordinary executable and
  shebang-interpreter parents receive READ_FILE, READ_DIR, and EXECUTE.
  Parents covering trusted home or overlapping AVA authority instead receive
  only exact-file READ_FILE and EXECUTE rules.
- **Regular-file rules**: file rules use only file-valid rights (READ_FILE and
  EXECUTE as appropriate), never READ_DIR, which Landlock rejects on
  non-directory paths. Redundant recipe-file rules already beneath the writable
  workspace are omitted.
- **Device files**: `/dev/null`, `/dev/zero`, `/dev/urandom`, `/dev/random`
  receive read/write permissions. Device files are deliberate: they do not
  receive IOCTL_DEV automatically.
- **Not granted by default**: arbitrary `/proc`, external checkout siblings,
  broad `/tmp`, real home, AVA config/state/session/auth, and SSH/cloud/provider
  secrets. Landlock does not subtract individual secret paths from an otherwise
  allowed workspace or toolchain root.

### Network (seccomp)

A BPF seccomp filter is installed after `PR_SET_NO_NEW_PRIVS` in **both** network
modes; networking approval never omits the filter or the filesystem rules.

| Mode | `socket()` | `connect()` |
| --- | --- | --- |
| `network_enabled=false` (default for Standard mutable commands) | Denied with `EPERM` for every domain | Denied with `EPERM` |
| `network_enabled=true` (for example `curl`, `git fetch`) | Allowed only for `AF_INET`, `AF_INET6`, and `AF_NETLINK` with protocol `NETLINK_ROUTE`; other domains and non-route netlink protocols return `EPERM` | Allowed by this filter |

In both modes:

- `AF_UNIX` `socket()` is denied, including abstract sockets. Local AF_UNIX
  services such as session D-Bus, `docker.sock`, ssh-agent, and gpg-agent are
  unreachable by design, even when IP networking is approved. The route-netlink
  exception supports address discovery such as `getaddrinfo`/`AI_ADDRCONFIG`.
- `socketpair()` permits only `AF_UNIX` `SOCK_STREAM` or `SOCK_SEQPACKET`, with
  optional `SOCK_CLOEXEC`/`SOCK_NONBLOCK`. Datagram pairs and other forms return
  `EPERM` because datagram pairs can address unrelated sockets.
- `sendto()`, `sendmsg()`, and `sendmmsg()` remain allowed. Together with private
  stream/seqpacket pairs, this preserves IPC needed by Python asyncio and Node
  child-process pipes without permitting new external sockets in denied mode.
- `io_uring_setup()` returns `EPERM`, preventing an io_uring socket bypass.

The filter is defined in
`src/ava/containment/seccomp_network.cpp`.

On x86_64, the filter rejects every syscall number with `__X32_SYSCALL_BIT` set
before normal dispatch, preventing x32 ABI bypass. `io_uring_setup` is blocked
unconditionally on both supported architectures (x86_64 and aarch64) using a
stable syscall number (425) verified by static assertion against the kernel
header — no silent header-dependent omission.

Inherited non-stdio file descriptors are closed after containment installation
and before descriptor exec, using `close_range(2)` with a bounded fallback. No
user code runs while those parent descriptors remain inherited. Descriptor
closure, **not send-syscall blocking**, is the defence against inherited sockets:
the filter permits sends on an already-open socket. Only handshake, approved
executable, and sealed script-source descriptors needed for descriptor execution
survive this closure in `src/ava/tools/bash_tool.cpp`.

The parent probes seccomp support with a non-mutating
`SECCOMP_GET_ACTION_AVAIL` query for `SECCOMP_RET_ERRNO`. **Any** failed query,
including denial by an outer sandbox, makes network seccomp `Unavailable` and
downgrades the command to Critical, one-shot Ask. There is no `PR_GET_SECCOMP`
fallback. Actual filter installation in the child remains mandatory and fails
closed before user code executes.

Whenever a containment plan is applied, `PR_SET_NO_NEW_PRIVS` is set and
inherited across descriptor exec, preventing privilege escalation through
setuid binaries inside that contained execution.

## Verified Scope and Limitations

### What is verified

- Landlock filesystem policy **follows descendants** that call `setsid` or
  otherwise change session/group. A contained child and all its descendants
  remain subject to the filesystem access rules.
- seccomp filter policy **follows descendants** in the same way.
- On Landlock ABI >= 6, signal and abstract UNIX socket scoping follow the
  contained command's domain and its descendants. The command cannot signal
  AVA or other processes, or connect to abstract sockets, outside that domain.
  See `src/ava/containment/landlock.cpp`.
- Process-group teardown uses `SIGTERM` → grace period → `SIGKILL` on the
  verified PGID (process group ID), with an AVA-owned sentinel preserving
  the PGID from recycling between leader completion and descendant cleanup.
- Child signal state is reset (default handlers, empty mask) in both the
  leader and sentinel before exec.
- The main executable and every resolved shebang interpreter are opened and
  identity-checked before release. Script chains are transformed to execute the
  final approved interpreter descriptor while retaining only approved script
  source descriptors; the kernel never re-resolves a shebang interpreter path.

### What is NOT claimed

- **No per-file secret filtering inside allowed roots.** File-tool secret-path
  denies do not mediate reads made directly by a contained child.
- **No signal/abstract-socket scoping below ABI 6.** On older supported ABIs,
  contained code can signal same-UID processes outside its process group,
  including AVA, subject to ordinary OS permission checks. The seccomp socket
  restrictions still apply.
- **No `.git` write protection.** The writable workspace rule includes an
  in-workspace `.git` directory. Contained code can plant hooks or configuration
  that affect later Git invocations, including ones run outside containment.
- **No cgroup containment claim.** AVA does not use cgroups to restrict
  resource usage (CPU, memory, PID namespace, etc.) of contained commands.
  Process cleanup is PGID-only; descendants that call `setsid` to escape the
  process group are still subject to Landlock/seccomp policy but cannot be
  signaled via PGID.
- **No namespace isolation claim.** Containment does not use mount namespaces,
  PID namespaces, user namespaces, or network namespaces. The child sees the
  same mount table and PID space as the parent.
- **External OS sandboxing is required** for full containment of descendants
  that call `setsid` and then attempt to create new process trees that might
  outlive the AVA session. Landlock and seccomp continue enforcing the granted
  filesystem/socket policy, with the limitations above; they do not prevent
  resource exhaustion or fork bombs.

## Fail-Closed Semantics

- If the Landlock ABI is below the required version, containment is reported
  as Unavailable and standard mutable commands downgrade to Critical Ask.
- If the parent `SECCOMP_GET_ACTION_AVAIL` probe fails for any reason,
  containment is reported as Unavailable and the command requires Critical,
  one-shot Ask.
- If the child fails to install Landlock or seccomp rules before exec, the
  parent terminates the verified group and returns an actionable error.
  User code does not start. Commands are never auto-run if child install fails.
- If any allow root fails validation (not owner-only for synthetic, unsafe
  shared-group/world writable workspace, anchor escape, or identity mismatch),
  containment is reported as Unavailable.

## Truthful Status

- Pre-permission metadata reports containment as `Available` (planned), never
  `Active`. The planning boolean is `network_filter_planned`, not `installed`.
- Summaries say `planned` before fork; `containment_applied` is set to true in
  `BashResult` and emitted process audit only after the parent verifies the
  child installed containment before exec.
- `BashResult` and `ProcessResult` audit include `containment_applied`,
  `containment_profile_id`, and `containment_network_mode` (denied/allowed)
  only after parent verification.
- RPC serialization includes `containment_profile_id` and
  `containment_network_allowed` when command metadata is already serialized.
- Paths that could expose secrets (real home, AVA authority roots, SSH/cloud
  provider paths) are never included in metadata, audit, RPC, or ACP output.

## Metadata and Audit

Permission metadata, audit events, RPC, and ACP surfaces show:
- Containment profile ID and availability/status (the redacted containment
  summary additionally reports the probed ABI version)
- Availability (Available/Unavailable/NotRequired)
- Filesystem scope (workspace writable, synthetic environment writable,
  read-only root count, device file count) — without leaking actual paths
- Network allowed/denied status and planned/applied state

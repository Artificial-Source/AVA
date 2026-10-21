# AVA Backend Usability Approval Ledger

This document is the single approval ledger for the backend-usability work identified during AVA dogfooding. Each workstream is researched, discussed, approved, and implemented independently when the user authorizes it. The approved result and implementation status are recorded here before moving to the next workstream.

External projects are behavior references only. AVA may adopt useful product behavior without copying their architecture or source.

## Approval Workflow

1. Research one workstream in AVA and relevant reference projects.
2. Explain the findings and proposed AVA behavior.
3. Discuss tradeoffs until the user approves a direction.
4. Record only the approved direction in this ledger.
5. Repeat for the next workstream.
6. Implement only the approved workstream when the user explicitly authorizes it.
7. Validate and record the result before researching the next workstream.

## Workstream Status

| # | Workstream | Status | Implementation |
| --- | --- | --- | --- |
| 1 | Normal coding-command permissions and execution | Direction approved 2026-07-19; recorded below | Sealed-plan execution landed (2026-07-19 record below); public structured argv/cwd input remains open |
| 2 | Foreground/background subagent execution and job controls | Direction approved 2026-07-20; recorded below | Implemented and validated 2026-07-20 |
| 3 | Privacy-safe diagnostics and support bundles | Direction approved 2026-07-20; recorded below | Implemented and validated 2026-07-20 |
| 4 | Context-compaction correctness and configuration | Direction approved 2026-07-20; recorded below | Core compaction landed (2026-07-20 record below); provider terminal-outcome validation and unknown-key rejection remain open |
| 5 | Automatic session titles | Direction approved 2026-07-21 | Implemented and validated 2026-07-21 |

---

# Workstream 1: Coding-Command Permissions And Execution

## Implementation Record

Implemented on `develop` on 2026-07-19. AVA now seals one authoritative command plan before permission evaluation and uses that same executable identity, argv/raw-shell payload, working directory, environment profile, and capability classification for execution. Bare and absolute executable spellings no longer take different policy paths.

The backend provides Standard, Sensitive, and Critical command levels. Recognized read-only inspection runs automatically. Recognized project builds/tests can run automatically only under the verified Linux development containment profile; otherwise they downgrade to a prompt. Sensitive and Critical commands ask rather than being rejected solely by executable name, while backend-owned maximum scopes prevent a frontend from widening one-time authority.

Schema-v2 global and external workspace permission rules use opaque typed recipe identities rather than raw wildcard command text. Matching Denies are checked before Standard auto-Allow, including model-initiated foreground/background subagent commands. TUI supports allow once, allow for the current AVA session, and always in the workspace; RPC exposes bounded session grants; global rules remain an explicit user-managed choice. ACP cannot reuse local recipe authority while its executor identity is unverified.

Local children use a positive-list, secret-free environment and filtered system, safe `~/.local/bin`, and workspace PATH discovery. Real host `HOME`, arbitrary inherited host state, credentials, and network remain unavailable. Linux Landlock plus seccomp network denial provide the verified development profile, including descendant inheritance; unsupported kernels and unverified delegated executors downgrade rather than claiming containment. AVA config/session/auth roots are excluded from command workspaces and synthetic child environment roots.

The implementation intentionally keeps checked-in `.ava/permission-rules.json` files non-authoritative and ignored; project-owned files cannot grant their own execution authority. Print mode remains fail-closed for unresolved prompts, while external global/workspace rules and RPC replies provide explicit automation authority. A dedicated `--allow-command` convenience flag remains a possible follow-up rather than adding blanket `--allow-tool bash`.

Primary implementation areas are `src/ava/command/`, `src/ava/containment/`, `src/ava/permissions/`, `src/ava/tools/bash_tool.cpp`, runtime model-command wiring, TUI/RPC/ACP permission resolvers, and the corresponding command/containment/permission/frontend tests.

## Validation Record

Validation used no paid live-provider calls.

- The normal configured CTest suite passed all 77 executed checks out of 94 registered checks; the 17 skips were the credential-gated provider check, optional official ACP SDK check, 13 opt-in tmux scenarios, and the Kitty-image/OSC-8 terminal smokes.
- The sanitizer CTest suite passed the same 77 executed checks with zero failures in 205.99 seconds under the intentional two-job cap.
- All 13 opt-in tmux TUI scenarios then executed concurrently and passed in 12.74 seconds. This includes raw-shell one-shot prompting, exact persistent denial, redacted durable audit/diagnostics, session-grant behavior, and fixture authority permissions.
- Focused command, containment, tool/process, AgentLoop, permission-rule, runtime, RPC, ACP, and TUI tests passed. They cover Standard auto-Allow and deny preflight, mutable-project containment, one-shot Critical behavior, bare/absolute and trusted symlink-alias identity parity, private-primary-group workspaces, typed recipe scope separation, secret-free environments, generic PATH containment, and foreground/background subagent routes.
- The package test, Python smoke-driver compilation, `clang-format --dry-run --Werror` on changed C++, and `git diff --check` passed.
- One final integrated security/correctness gate reviewed the complete command path under the documented malicious-model/repository threat model and reported no blocking findings.

## Approved Product Direction

AVA will use an OpenCode-like permission experience without adopting OpenCode's blanket shell authority. Most recognized, contained, normal development commands should run without prompting. Commands with sensitive or difficult-to-predict effects should ask the user rather than being categorically forbidden. The more authority a command needs, the narrower and more explicit its approval becomes.

The user remains the final authority. A valid critical command is not rejected merely because of its executable name. Commands that AVA cannot safely classify or contain are presented as critical and require informed approval. Malformed requests, ambiguous argument encoding, invalid paths, stale permission identities, and failed containment remain fail-closed implementation errors rather than user-overridable command policy.

## Verified Reference Behavior

The comparison used these pinned local references:

- Pi: `d53b567601da35db61208418da956a43275ccf28`
- OpenCode: `7a8e7c88f495acf5af3e7584e8ec1dbab2fe04ec`

### OpenCode

OpenCode's effective default build-agent permission begins with `"*": "allow"`. Consequently, normal tool and shell calls do not prompt by default. Its notable default exceptions include:

- external-directory access: ask;
- reading `.env`-style files: ask, except `.env.example`;
- repeated identical tool calls (doom-loop detection): ask.

OpenCode supports `allow`, `ask`, and `deny` rules, command patterns, global configuration, project configuration, and an in-memory "always" response for the active instance. Its shell scanner derives human-readable reusable patterns such as a specific package-manager script or Git subcommand.

OpenCode's convenience comes with broad authority: it runs a real shell, inherits the host environment, and grants shell commands the host user's filesystem, process, and network authority. Project configuration can also contribute permission rules. AVA will adopt the low-friction permission experience, not those trust assumptions.

Primary reference locations:

- OpenCode defaults: `docs/reference-code/opencode/packages/opencode/src/agent/agent.ts`
- Permission evaluation and replies: `docs/reference-code/opencode/packages/opencode/src/permission/index.ts`
- Shell scanning and reusable patterns: `docs/reference-code/opencode/packages/opencode/src/tool/shell.ts`
- Global/project config loading: `docs/reference-code/opencode/packages/opencode/src/config/config.ts`

### Pi

Pi prioritizes direct terminal ergonomics. Its built-in Bash tool executes commands without a built-in command classifier or approval prompt, inherits the process environment and PATH, and relies on optional extensions to block or replace tool calls. It has strong cancellation, process-tree cleanup, and output truncation, but its default command-authority model is intentionally more permissive than AVA's target.

Primary reference locations:

- Pi Bash tool: `docs/reference-code/pi/packages/coding-agent/src/core/tools/bash.ts`
- Shell and environment handling: `docs/reference-code/pi/packages/coding-agent/src/utils/shell.ts`
- Optional tool-call interception: `docs/reference-code/pi/packages/coding-agent/src/core/extensions/types.ts`

## Original AVA Problems Addressed

1. **Normal tools are hard-denied before approval.** Bare Python, Node, shell, Make, Ninja, and package-manager frontends never reach the resolver.
2. **Classification is identity-inconsistent.** `python3 --version` is hard-denied while `/usr/bin/python3 --version` reaches an approval prompt because classification compares raw `argv[0]` instead of the resolved executable identity.
3. **Policy and execution parse independently.** `permission.cpp` and `bash_tool.cpp` can disagree about the command that was approved.
4. **PATH is not useful for normal development.** Local execution replaces PATH with fixed system directories and excludes common user and project toolchains.
5. **The remaining parent environment is inherited.** Provider, cloud, secret, and behavior-altering variables can reach approved children.
6. **Remembered rules match exact raw strings.** They are awkward when harmless arguments change, yet they do not express the actual executable, workspace, effect class, or environment.
7. **Frontends expose different scopes.** TUI offers once or persistent exact rules, RPC offers once/session grants, headless has no command-specific allow surface, and ACP executes in a different resolution domain.
8. **No OS authority boundary exists for ordinary project code.** A recognizable command such as `npm test`, CMake, Make, or pytest may execute arbitrary mutable project code.

Current AVA locations:

- Command classification: `src/ava/permissions/permission.{h,cpp}`
- Exact persistent rules: `src/ava/permissions/permission_rules.{h,cpp}`
- Local parsing and execution: `src/ava/tools/bash_tool.{h,cpp}`
- Delegated executor contract: `src/ava/tools/tool_io.h`
- Headless resolution: `src/ava/app/headless_policy.{h,cpp}`
- RPC resolution and grants: `src/ava/app/rpc/resolvers.{h,cpp}`
- TUI permission presentation: `src/ava/tui/composer_permission.cpp`, `src/ava/tui/runtime.cpp`
- ACP terminal delegation: `src/ava/app/acp/client_tools.cpp`

## Approved Three-Level Policy

### Level 1: Standard Development

Most commands in this level run without asking when AVA can recognize and contain them. Global or per-project user policy may change any default auto-allow to `ask` or `deny`.

Initial families include:

- exact `pwd`, `ls`, `git status`, `git diff`, and `git log -1` inspection;
- `cmake --build`, CTest, Ninja, and Make recipes;
- exact named npm, pnpm, Yarn, and Bun project scripts;
- pytest and explicitly configured workspace scripts;
- additional recipes only after each family has a bounded grammar and tests.

A project-code command is eligible for automatic execution only when all of these are true:

- AVA recognizes a typed command family and effect;
- the executable, arguments, workspace, and working directory form one validated command plan;
- all declared project paths stay within the workspace or separately approved roots;
- the command does not request network, privilege elevation, external state, raw shell evaluation, or an unbounded destructive effect;
- AVA supplies a secret-free child environment;
- the standard development containment profile is active.

If containment is unavailable, read-only inspection may remain automatic, but commands executing project code downgrade to `ask`. AVA must never claim that a build/test command is intrinsically harmless: remembered or automatic permission authorizes a constrained recipe, while containment limits what mutable project code can access.

### Level 2: Sensitive

Sensitive commands ask with a clear effect warning. The user may allow once, for the current session, or persist an eligible exact/typed rule globally or for one project.

Examples include:

- dependency installation or updates;
- Git push and other remote mutations;
- package publishing;
- deployment;
- network-enabled execution;
- deletion or broad mutation inside the workspace;
- commands with known external side effects.

AVA must show the exact command, executable, working directory, requested capabilities, and rule that would be remembered. Sensitive rules are never generated as broad executable-only wildcards.

### Level 3: Critical Or Unrestricted

Critical commands ask rather than being categorically forbidden. The default interactive prompt offers reject or run once, with an explicit statement that AVA cannot fully predict or contain the operation.

Examples include:

- privilege-changing commands such as `sudo`, `su`, or `doas`;
- system or device management;
- destructive access outside the workspace;
- raw shell evaluation, pipelines, redirects, substitutions, and command chains;
- inline interpreter programs such as `bash -c`, `python -c`, or `node -e`;
- unknown launch wrappers or commands whose execution identity cannot be represented by a standard recipe.

Normal UI flows do not offer a remembered critical grant. The command plan's maximum interactive approval scope is `once`. An advanced user may separately create an exact global or external per-project rule only with an explicit risk acknowledgement; that user-authored authority is validated before prompting and does not let a frontend widen a run-once reply. AVA never auto-generates a critical allow rule or a wildcard critical rule. Interactive password entry is not passed through the embedded tool runner; terminal handoff is deferred and commands that require it report an actionable unsupported-interactive-execution result.

## Command Representations

The sealed-plan subsystem supports argv and raw-shell plans internally. The public model/RPC `bash` tool still requires a `command` string and exposes neither `argv` nor `cwd`; the runtime tool context selects the working directory (`src/ava/agent/tool_metadata.h`, `src/ava/agent/tool_dispatch_bash.cpp`, and `src/ava/agent/agent_turn_executor.cpp`).

### Structured Command Lane

Approved but not implemented at the public tool boundary: the preferred model/RPC representation would use exact arguments plus an optional workspace-contained working directory:

```json
{
  "argv": ["npm", "run", "test"],
  "cwd": "packages/frontend"
}
```

Until the schema, argument parser, and invocation wiring land together, the command string is the only public input and is parsed into the immutable plan. Exact preservation of empty arguments and argument boundaries remains an acceptance requirement for the proposed structured lane.

### Raw Shell Lane

Shell syntax is supported as a critical/unrestricted operation rather than silently interpreted as a normal command. The complete shell text is displayed and authorized as one exact operation. It receives no reusable rule through the ordinary prompt. The raw-shell lane must not be described as statically safe merely because a parser lists its apparent subcommands.

## One Authoritative Command Plan

A new narrow command subsystem will produce one immutable plan before permission evaluation. Policy, prompting, audit, local execution, and delegated execution consume that same plan.

The plan includes:

- original safe display text;
- exact argv or exact raw-shell payload;
- logical workspace and working-directory identity backed by the shared `AnchorSet`/`AnchorOpen` authority and descriptor-bound physical identity;
- resolved executable path, origin, and identity;
- supported interpreter/shebang chain where applicable;
- effective secret-free environment profile;
- semantic command family and effect class;
- requested filesystem/network/privilege capabilities;
- maximum approval scope;
- policy/environment schema versions;
- a stable fingerprint for grants, replies, and audit.

Execution must use the approved identity without falling back to a fresh PATH lookup. Executable and working-directory replacement between approval and launch must fail closed. Linux descriptor-anchored execution is preferred where it can correctly preserve supported interpreter chains; unsupported identity guarantees make the plan one-shot-only or unavailable rather than silently weaker.

## Global And Per-Project Policy

### Global User Policy

Proposed location:

```text
$XDG_CONFIG_HOME/ava/command-policy.json
```

It contains machine-wide defaults, typed rules, safe toolchain discovery paths, and user-selected policy overrides. It is user-owned, bounded, strictly validated, symlink-rejected, atomically written, and never model-writable.

### Effective Per-Project Policy

Proposed location:

```text
$XDG_CONFIG_HOME/ava/workspace-command-policies/<workspace-hash>/command-policy.json
```

The workspace hash uses AVA's normalized absolute logical-path convention without resolving symlinks. Using XDG config is intentional because these are durable user policy choices, not transient runtime state. Moving the workspace changes its hash and requires fresh approval. The file stores user-approved exact plans or typed recipes outside the checkout so project code and model file tools cannot grant themselves authority.

### Checked-In Project Suggestions

Optional project file:

```text
<workspace>/.ava/command-policy.json
```

This file is loaded only after explicit project trust. It may propose recipes, working directories, or stricter denies, but it cannot grant execution authority by itself. AVA shows changes for user review and records the approved fingerprint externally. Any changed proposal invalidates that approval until reviewed again.

### Precedence

1. Invalid or stale plan/containment state fails closed.
2. Matching user deny rules override matching allows.
3. Global denies cannot be weakened by a project.
4. Project-specific user policy is more specific than global defaults where no deny applies.
5. Global user policy overrides AVA's built-in defaults where the command's maximum scope permits it.
6. AVA's built-in default applies when no user rule matches.

Project files never override external user authority.

## Typed Recipes

A recipe is a finite AVA-owned schema derived from a validated command plan, not a model-authored string wildcard. Examples:

- `CMakeBuild(workspace, build_directory)`
- `CTest(workspace, test_directory)`
- `PackageScript(workspace, manager, exact_script_name)`
- `WorkspaceScript(workspace, interpreter_family, exact_script_path)`

Recipes match structured fields, permitted argument classes, executable origin, workspace identity, working-directory policy, environment-policy version, and effect class. They never fall back to "first executable token plus any arguments." Existing deny rules are evaluated first.

The first rollout may support exact plan grants before reusable families. Each reusable family lands independently with adversarial matching tests. Approving a project-code recipe intentionally authorizes future mutable project code under that recipe; the UI must say this plainly, and the containment profile—not a content hash—is the damage boundary.

## Toolchain Discovery And Child Environment

AVA will derive command discovery from:

- a filtered snapshot of startup PATH;
- explicit global user toolchain directories;
- recognized workspace-local candidates such as activated virtual environments and `node_modules/.bin`.

Accepted discovery directories must resolve absolutely, have acceptable ownership and write permissions, and retain provenance. Empty/relative PATH entries and unsafe writable global directories are rejected. Workspace-local executables remain project-controlled and are classified accordingly. AVA does not source shell startup files.

Children receive a positive-list environment containing only documented compatibility values such as locale, user/home, temporary and XDG paths, the derived PATH, and explicitly approved non-secret toolchain variables. Provider, cloud, API, token, secret, arbitrary `AVA_*`, and loader/interpreter injection variables are excluded. Values are not persisted in permission audit records.

## Development Containment Profile

Permission decides whether a command may start; containment limits what approved mutable project code can do. Standard project-code auto-execution requires a verified containment profile.

Target behavior:

- system toolchains and required runtime libraries are readable/executable;
- the workspace and explicitly selected build/temp locations are writable;
- AVA auth, SSH keys, cloud credentials, unrelated home configuration, and external workspaces are unavailable;
- network is disabled unless the plan has separately approved network capability;
- restrictions are inherited by descendants, including descendants that create new sessions;
- AVA does not claim cgroup, namespace, or descendant containment without verified platform evidence.

The implementation plan must select and verify a Linux containment mechanism before automatic project-code execution is enabled. If the required containment guarantees are not available, the backend downgrades the command to an interactive permission prompt and reports why.

## Permission Experience

For an eligible standard/sensitive command, frontends may display:

- reject;
- allow once;
- allow this exact plan/recipe for the session;
- always allow this eligible recipe in this workspace.

Global rules are managed explicitly rather than casually created from a prompt. Critical commands normally expose reject/run-once only.

The prompt must show:

- exact command or argv;
- resolved executable and origin;
- working directory;
- effect/risk level;
- project-code, outside-workspace, network, privilege, and containment status;
- exact scope/recipe that a remembered choice creates.

The backend owns the maximum **interactive** approval scope and rejects scope escalation regardless of what TUI, RPC, ACP, or headless input requests. A manually authored external policy rule is a separate authority path: it must be eligible for the command level, exact when required, independently validated, and explicitly risk-acknowledged for critical commands. Config cannot make a frontend reply broader than the plan permits.

## Headless And ACP

Headless execution remains fail-closed for unresolved prompts. It gains process-local exact-plan approvals and, later, explicitly selected typed recipes. It never gains blanket `--allow-tool bash`. Critical raw-shell authorization is not inferred from a broad headless flag.

ACP executes in a client-controlled resolution domain. Reusable local recipes cannot apply until the ACP execution contract can carry and enforce the approved executable, cwd, environment profile, plan fingerprint, and capability bounds. Until then ACP command execution is once/session only or fails closed when equivalence cannot be established.

## Design Phase Status

The following phases were the approved planning inputs. The sealed-plan foundation has landed, but the public structured-input item in Phase 1 remains open; the dated implementation and validation records above do not establish that input lane.

### Phase 1: Identity Foundation

- Add the command-plan subsystem and one parser.
- Open: expose structured argv and workspace-contained cwd at the public model/RPC tool boundary while retaining command-string input.
- Resolve executable/interpreter identities before policy.
- Route local and injected execution through the exact approved plan.
- Preserve current behavior behind a rollout switch while proving equivalence.

### Phase 2: Policy And Storage

- Add the three effect levels and backend-enforced maximum approval scope.
- Add global strict config and external workspace-keyed policy storage.
- Keep existing permission-rule schema behavior unchanged; never reinterpret old exact allows as broader recipes.
- Preserve legacy deny authority and prevent newly eligible build/test classes from being widened by old allows.
- Add explain/list/add/remove/revoke diagnostics.

### Phase 3: Toolchain And Environment

- Add filtered startup PATH and explicit toolchain discovery.
- Resolve system, user, and project-local executable provenance.
- Build a positive-list child environment.
- Bind environment/policy versions into the plan fingerprint.

### Phase 4: Containment

- Implement and verify the Linux standard development profile.
- Prove filesystem, secret, network, descendant, timeout, and teardown behavior.
- Keep automatic project-code execution disabled until containment evidence passes.

### Phase 5: Permission Defaults And Recipes

- Auto-allow recognized standard commands only under the approved preconditions.
- Convert normal development frontends from hard deny to structured standard/sensitive policy.
- Add exact plan grants, then typed recipe families one at a time.
- Add the critical raw-shell/run-once path.

### Phase 6: Frontend And Protocol Parity

- Align TUI, print, RPC, ACP, and direct command surfaces.
- Expose once/session/workspace choices only when backend-eligible.
- Add exact process-local headless approvals.
- Add visible rule explanations and revocation.

### Phase 7: Validation And Rollout

- Run focused, full, sanitizer, RPC/ACP, and real-toolchain smokes.
- Feature-gate rollout and preserve fail-closed rollback.
- Document the default command catalog, config schemas, containment guarantees, and unsupported platform behavior.
- Enable new defaults only after final review.

## Context Map

### Command And Containment Subsystems

| Path | Responsibility |
| --- | --- |
| `src/ava/command/command_plan.{h,cpp}` | Lossless parsing, argv/raw-shell representation, immutable plan and fingerprint |
| `src/ava/command/discovery.{h,cpp}` | PATH snapshot, executable/interpreter resolution and provenance |
| `src/ava/command/environment.{h,cpp}` | Positive-list child environment profiles |
| `src/ava/command/policy.{h,cpp}` | Three-level structural classification and maximum scope |
| `src/ava/command/recipes.{h,cpp}` | Exact grants and finite typed recipe matching |
| `src/ava/command/policy_store.{h,cpp}` | Global and external workspace policy validation/storage |
| `src/ava/command/containment.{h,cpp}` | Verified development containment profile and capability report |

The final implementation retains these responsibilities across `src/ava/command/`, `src/ava/containment/`, and the existing permission/tool/runtime boundaries; filenames differ where smaller existing seams were safer than the provisional layout.

### Existing Files Expected To Change

| Path | Planned role |
| --- | --- |
| `src/ava/permissions/permission.{h,cpp}` | Delegate command decisions to validated plans while retaining generic operation policy |
| `src/ava/permissions/permission_rules.{h,cpp}` | Preserve v1 compatibility and deny precedence; protect new stores |
| `src/ava/tools/bash_tool.{h,cpp}` | Compatibility adapter and exact-plan execution; no independent reparse/lookup |
| `src/ava/tools/tool_io.h` | Carry sealed execution specs to delegated executors |
| `src/ava/agent/tool_metadata.h` | Advertise structured argv/cwd and raw-shell behavior accurately |
| `src/ava/app/headless_policy.*` | Exact process-local command authorization |
| `src/ava/app/rpc/resolvers.*` | Plan fingerprints and backend-enforced scopes |
| `src/ava/app/acp/client_tools.cpp` | Capability-aware delegated execution or fail-closed behavior |
| `src/ava/tui/composer_permission.cpp` | Present backend-provided choices and recipe details |
| `src/ava/tui/runtime.cpp` | Submit typed choices; stop owning scope eligibility |
| `src/main.cpp` and CLI parsing owner | New command-policy/headless controls without growing unrelated main logic |
| `docs/core/configuration.md`, `docs/core/usage.md`, `docs/rpc-protocol.md` | Config, behavior, protocol, security and operational documentation |

### Proposed Test Areas

| Test area | Required evidence |
| --- | --- |
| Command-plan unit tests | Quoting/empty args, raw shell, control bytes, exact identity, fingerprint stability |
| Policy table tests | Standard/sensitive/critical families, absolute paths, wrappers, path/effect bounds |
| Recipe tests | No cross-subcommand, cross-script, cross-workspace, wildcard, or scope widening |
| Discovery tests | User toolchains, virtualenvs, project shims, symlinks, ownership, replacement races |
| Environment tests | Secret sentinels excluded; required safe/toolchain variables present |
| Containment tests | Workspace/system/secret/network access and descendant inheritance |
| Permission-store tests | Ownership, symlinks, corruption, locking, precedence, proposal fingerprint invalidation |
| Frontend tests | TUI/RPC/headless/ACP choices cannot exceed backend maximum scope |
| Process tests | Timeout, cancellation, TERM-to-KILL, spill limits, no detached survivors |
| Real smokes | CMake, CTest, Ninja/Make, Python/pytest, and Node/package scripts |

## Acceptance Criteria

- Recognized contained standard development commands run without repetitive prompts.
- A user can change standard defaults globally or for one workspace.
- Sensitive commands prompt with clear effects and only create the scope the user selected.
- Critical commands prompt rather than being rejected by executable name; ordinary UI grants run once only.
- An explicitly risk-acknowledged exact critical rule can be manually stored outside the checkout.
- Bare and absolute executable paths receive the same classification.
- The exact approved executable, argv/raw shell, cwd, environment profile, and capability set are what execute.
- Project configuration cannot grant itself authority or modify effective external approvals.
- Matching denies always win and frontends cannot upgrade a plan's maximum scope.
- Safe auto-execution is disabled when required containment cannot be verified.
- Provider/cloud/API/token/secret values do not reach command children or permission logs.
- Existing timeout, cancellation, process-group cleanup, output caps, spill files, and audit behavior remain intact.
- ACP either proves execution equivalence or fails closed for unsupported reusable authorization.
- Existing v1 rules remain readable and rollback does not silently broaden authority.

## Rollback Strategy

- Schema-v2 typed recipes remain distinct from schema-v1 raw command Allows; v1 command Allows are read but non-authoritative.
- Do not migrate old command Allows into typed recipes automatically.
- On rollback, Standard auto-Allow returns to prompt/deny behavior; it never falls back to broader shell execution.
- Existing exact Denies remain authoritative during migration and rollback.

## Explicit Non-Goals

- Copying OpenCode's default `"*": "allow"` for all shell commands.
- Copying Pi's unrestricted default Bash authority.
- Treating command-name recognition as proof that project code is harmless.
- Letting checked-in project files, plugins, MCP servers, or model output grant command authority.
- Adding model-authored regex/glob permission patterns.
- Claiming sandbox, cgroup, network, or descendant containment without verified evidence.

---

# Workstream 2: Foreground And Background Subagents

## Implementation Record

Implemented on `develop` on 2026-07-20. AVA now runs the same configured specialized workers in explicit foreground and background modes. Foreground tasks synchronously return their bounded summary; background tasks return immediately and continue concurrently. A running foreground task can be promoted without restarting it or changing its job, task, child-session, provider-call, or usage identity.

An application-scoped coordinator owns live workers and all job state across in-process session navigation. Jobs, results, pending deliveries, and retry state exist only for that AVA process: shutdown requests cooperative cancellation, and close or crash provides no job recovery or later result delivery. Child-session history remains durable and inspectable independently of process-local job state.

Background terminal summaries are delivered to the parent after the current committed turn boundary through isolated, zero-tool provider runs. Structured synthetic provenance, stable delivery identities, bounded same-process retries, and commit acknowledgement prevent duplicate same-process integration and ordinary user text from forging completion. Retained parent capsules preserve exact session append/read authority and refresh model, reasoning, mode, trust, prompt, and credential state before delivery.

The model-visible `job` tool supports owner-scoped list, status, wait, result, and cancel. RPC exposes the same owner-bound controls plus out-of-band promotion as public `schema_version:1` JSON. Interactive `/jobs` exposes those controls as human text (display-only ordinals and process-local title/type; exact job IDs remain authority) rather than that JSON snapshot; TUI `/jobs` controls remain usable during an active run. Rich child-chat tabs/navigation and a dedicated promotion keybinding remain frontend follow-up work over the implemented backend contract.

## Validation Record

Validation used no paid live-provider calls.

- The current normal configured CTest suite passed all 89 executed checks out of 111 registered checks in 11.41 seconds. The 22 skips were the credential-gated provider check, optional real-clangd and official ACP SDK checks, 15 opt-in tmux scenarios, and four terminal smokes.
- The original implementation's sanitizer CTest suite passed its 80 executed checks with zero failures in 210.27 seconds under the intentional two-job cap.
- The original 13 opt-in tmux TUI scenarios executed concurrently and passed in 12.85 seconds.
- Focused coordinator, delivery-manager, AgentLoop, task/job-tool, runtime/session, RPC, command-registry, and TUI tests passed. Coverage includes foreground completion, direct background execution, promotion without restart, cancellation races, process locality, delivery retry/acknowledgement, structured provenance, parent isolation, active-safe controls, stale configuration/credential refresh, and navigation while work remains active.
- The integrated security/correctness review identified six material findings. The focused re-review verified W2-002 through W2-006; its remaining W2-001 unrelated-parent admission race was fixed with per-parent start accounting and a deterministic cross-process regression test. No known material finding remains open.
- Changed C++ was formatted and `git diff --check` passed.

## Approved Product Direction

AVA exposes the same configured specialized workers through two explicit execution modes.

A foreground subagent is synchronous: the parent agent waits for the child, the current conversation is blocked until the child finishes or is promoted, and permission/question interaction may use the active foreground UI. Its final bounded summary returns directly as the `task` tool result.

A background subagent is asynchronous: launch returns immediately, the parent conversation remains usable, and the child runs concurrently under delegated authority without interrupting the main conversation with modal prompts. A task may start in background mode or a running foreground task may be promoted to background by a future frontend action. Promotion does not restart the child or consume a second task identity.

Background work continues while the user navigates among the parent and child conversations in the same AVA process. Explicit cancellation or AVA process shutdown stops it; execution is not resurrected after restart. The child session remains durable and inspectable after completion, cancellation, or interruption.

When either mode finishes, the parent agent receives the child's bounded final summary. Foreground delivery is the synchronous tool result. Background delivery is automatic: if the parent is idle, delivery starts promptly; if the parent has an active turn, AVA queues the completion and delivers it at the next safe committed turn boundary rather than interleaving records into an active assistant transaction. Completion delivery is process-local and deduplicated by stable identity within the current process; it is not recovered after close or crash.

The backend provides owner-bound list, status, wait, result, cancel, and promotion operations. Model-facing job controls expose list, status, wait, result, and cancel; owner-safe promotion remains an out-of-band backend, RPC, and `/jobs` operation because a blocked parent model cannot invoke it. These surfaces use the same authority and state machine. Rich child-chat navigation, tabs, and a promotion keyboard shortcut remain frontend follow-up work, but the backend contract required by that UI belongs to this workstream.

## Approved Boundaries

- Foreground and background are execution modes, not duplicate agent-definition formats.
- Parent, child task, and live job identities remain explicit and stable; live execution state is not confused with durable child-session history.
- Background children do not display permission or question modals over the main conversation. Operations lacking delegated or automatic authority stop with an actionable child outcome until richer child-chat interaction is designed.
- Background completion summaries are bounded; the child session is the source of truth for full history.
- Navigation within the process does not cancel jobs. Process shutdown requests cooperative cancellation and joins workers; it does not persist job state.
- Completion notification is additive to same-process status/result lookup. Neither surface survives process exit.
- Recursive unbounded background task graphs remain disabled, and existing running/retention/token/output limits remain enforced.
- Pi and OpenCode are behavior references only. Pi core intentionally omits subagents; OpenCode's experimental background-task notification behavior informs AVA's product flow without defining its architecture.

## Final-Review Finding Ledger

| ID | Status |
| --- | --- |
| W2-001 | Fixed and validated 2026-07-20 |
| W2-002 | Fixed and validated 2026-08-03 |
| W2-003 | Fixed and validated 2026-07-20 |
| W2-004 | Fixed and validated 2026-07-20 |
| W2-005 | Fixed and validated 2026-07-20 |
| W2-006 | Fixed and validated 2026-07-20 |

---

# Workstream 3: Privacy-Safe Diagnostics And Support

## Approved Product Direction

AVA will separate rich local developer debugging from safe user-facing diagnostics. Carlo Wood's existing libcwd integration and subsystem channels remain the deep, developer-only debug layer. `ava doctor`, last-failure records, metadata traces, and support exports remain libcwd-independent, available in ordinary release builds, and safe to share by construction.

The support-facing boundary uses closed typed categories, stable codes, retryability, counts, and fixed recovery hints. It never serializes raw formatted errors or caller-controlled diagnostic strings. Provider/model/plugin/MCP/LSP/network/session/path/command text, credentials, environment values, prompts, reasoning, tool arguments/results, stderr, response bodies, headers, URLs, and configuration contents are excluded from support artifacts. AVA does not upload diagnostics or enable telemetry.

### Failure Privacy

Untrusted MCP/plugin response fragments, remote error messages, stderr, and failure metadata must not flow into model-visible tool results, session records, RPC/events, or portable exports. Those paths receive stable bounded failure projections while successful tool content remains unchanged. Explicit developer-only libcwd diagnostics may record bounded operation/state metadata but do not become support-bundle input.

### Doctor And Local Diagnostics

`ava doctor` is an offline, passive readiness check with a machine-readable `--json` form. It reports fixed statuses and remediation for AVA version/platform, private XDG storage, configuration parse/readiness, provider credential presence and storage safety, plugin/MCP/LSP configuration, and permission-rule readiness. It does not call a provider, refresh credentials, launch an integration or subprocess, access the network, mutate configuration, or create a session. Active probes are deferred unless added later as an explicit separately approved mode.

Metadata-only tracing is disabled by default and enabled explicitly with `--trace`. It reuses AVA's bounded RunObserver architecture, replaces identifying values with process-local opaque aliases, omits content and paths, writes only to owner-private bounded local state, and never changes the observed runtime result. A sanitized owner-only last-failure record stores only timestamp, component class, stable category/code, retryability, and fixed recovery guidance.

### Support Export

`ava support export` writes a unique owner-private local JSON artifact beneath AVA's state directory. It contains only generated version/platform facts, the passive doctor projection, trace schema/counters, and the sanitized last-failure projection. It never includes trace event lines, sessions, exports, prompts, reasoning, commands, paths, configurations, identities, credentials, raw errors, stderr, or provider/plugin/MCP payloads. Publication is bounded, descriptor-safe, no-follow, and no-replace. No automatic upload exists.

## Verified Reference Behavior

Grok Build provides useful structured `inspect --json`, MCP health reports, bounded local crash evidence, and metadata-oriented diagnostics. OpenCode provides strong debug/inspection entry points and typed integration states. Pi offers convenient one-shot local debug capture. AVA adopts those product ideas without copying their broad raw logs, recursive session archives, response-body/header retention, transcript dumps, or automatic diagnostic uploads.

## Approved Implementation Sequence

1. Establish one typed support-safe diagnostic representation and close MCP/plugin failure-data leaks.
2. Add private bounded diagnostic storage, passive doctor checks, sanitized last-failure persistence, and support export.
3. Expose privacy-safe RunObserver tracing and add libcwd operation/state instrumentation for local developer debugging.
4. Add deterministic canary tests, CLI/RPC/ACP framing coverage, documentation, and final normal/sanitizer/terminal validation.

## Implementation Record

Implemented on `develop` on 2026-07-20. AVA now projects MCP and plugin failures through one closed support-safe representation before model-visible results, sessions, RPC, or portable export, while successful integration content remains unchanged. Stable component/category/code/retryability/recovery fields replace raw remote messages, stderr, response fragments, and metadata on those failure paths.

`ava doctor` and `ava doctor --json` provide passive offline readiness checks without provider calls, credential-value reads, token refresh, subprocesses, sessions, mutation, or diagnostic writes. `ava support export` publishes one unique owner-private, no-replace JSON artifact beneath AVA's XDG state root and returns its exact local path; it never uploads. A typed best-effort last-failure record persists only stable failure classes and fixed recovery guidance.

Explicit `--trace` works with TUI, print, RPC, and ACP modes. It uses a bounded asynchronous RunObserver adapter, per-trace opaque aliases, allowlisted numeric/boolean metadata, unique `0600` JSONL files, and a cumulative typed counter/writer-health snapshot merged safely across concurrent processes. Trace events and filenames are never support-export input. Existing libcwd channels remain the richer developer-only layer and receive only bounded operation/state/count messages from this workstream.

All diagnostic state uses descriptor-relative, nonblocking no-follow inspection, owner/private directory and file modes, single-link regular-file validation, atomic or no-replace publication, bounded parsing, and fail-closed unsafe-path handling. Default successful AVA startup remains artifact-free.

Primary implementation areas are `src/ava/diagnostics/`, `src/ava/app/doctor_support.cpp`, application/runtime tracing and failure boundaries, MCP/plugin safe-failure projections, package documentation, and focused diagnostics/runtime/CLI/ACP tests.

## Validation Record

Validation used no paid live-provider calls.

- The normal configured CTest suite passed all 84 executed checks out of 101 registered checks in 80.27 seconds. The 17 expected skips were the credential-gated live-provider check, optional official ACP SDK interop, 13 opt-in tmux scenarios, and the Kitty-image/OSC-8 terminal smokes.
- The sanitizer suite passed the same 84 executed checks with zero failures in 210.93 seconds under the two-job cap.
- All 13 opt-in tmux scenarios executed concurrently and passed in 13.14 seconds.
- Focused diagnostics, runtime diagnostics, RunObserver, ACP, passive doctor/support, trace lifecycle, MCP/plugin RPC privacy, and Linux package tests passed.
- Tests cover passive/no-write doctor behavior, strict closed schemas, hostile canaries, default artifact-free startup, unique private/no-replace publication, symlink/FIFO/hardlink/mode rejection, trace bounds and aliasing, concurrent traces, cumulative counter locking, legacy v1 counter compatibility, last-failure classification, ACP stdout framing, and safe historical replay/export.
- One integrated security/correctness review produced three concrete findings. `AVA-WS3-001` (concurrent counter overwrite), `AVA-WS3-002` (legacy v1 counter compatibility), and `AVA-WS3-003` (double-counted writer failures) were fixed; the narrow follow-up verification marked all three fixed with no newly introduced blocker.
- Changed C++ formatting, package Markdown-link validation, and `git diff --check` passed.

## Review Finding Ledger

| ID | Resolution |
| --- | --- |
| AVA-WS3-001 | Fixed and validated 2026-07-20 |
| AVA-WS3-002 | Fixed and validated 2026-07-20 |
| AVA-WS3-003 | Fixed and validated 2026-07-20 |

## Explicit Non-Goals

- Automatic telemetry, uploads, or remote support services.
- Bundling session/transcript/export content or raw debug/libcwd logs.
- Active provider, MCP, plugin, LSP, or command probes in the passive doctor path.
- Treating regex redaction as permission to serialize arbitrary free-form content.
- Changing compaction behavior or the concurrently implemented Workstream 4 contract.

---

# Workstream 4: Context-Compaction Correctness And Configuration

## Approved Product Direction

AVA retains append-only checkpoint compaction: it summarizes the active provider-visible conversation, persists a compaction boundary plus a bounded recent tail, and reconstructs future context from that checkpoint without rewriting or deleting physical session history.

Manual `/compact`, automatic threshold compaction, and one context-overflow recovery attempt use one shared active-context selection, summarization, retained-tail, metadata, cancellation, and append-authority path. They must not repeatedly summarize raw material already replaced by the latest valid compaction boundary. The approved safety target is that failure or cancellation appends no checkpoint and leaves the current context unchanged. This remains incomplete: `src/ava/agent/context_compaction.cpp` accepts nonempty summary text without requiring an explicit successful provider terminal outcome, so a length-truncated, refused, provider-cancelled, or error-ended summary can still be checkpointed.

### User Configuration

The existing owner-controlled `$XDG_CONFIG_HOME/ava/compaction.json` remains the configuration surface and becomes type-strict and semantically validated. The default automatic threshold is 80 percent of the active conversation model's context window. Users may choose either an integer `auto_threshold_percent` or the existing absolute `auto_threshold_tokens`; specifying both is invalid. Percentage thresholds are bounded below 100 percent so AVA retains response headroom. An explicit absolute threshold of zero continues to disable automatic compaction. When model context metadata is unavailable, percentage calculation uses a documented conservative fallback window rather than silently changing to unrelated behavior.

Compaction uses the active provider and model by default. Users may explicitly configure a different summary model. A model-only override uses the active provider; a provider plus model selects that exact configured provider/model, including a different provider. Explicit selections must resolve through AVA's provider/model registry and normal credential path, remain visible in compaction metadata/events, and fail actionably when unavailable or incompatible. AVA never silently falls back to another provider/model.

Known fields reject wrong JSON types, invalid ranges, ambiguous threshold/retention combinations, unknown provider/model selections, and incompatible configuration. `/reload compaction` reports the same semantic diagnostics used by actual compaction instead of declaring a malformed file valid. Unknown object keys are currently ignored by `parse_compaction_config` in `src/ava/session/compaction.cpp`; an unknown-only object retains defaults. Allowlist-based unknown-key rejection remains open.

### Retained Recent Context

The default recent tail keeps the latest two complete user turns up to 20,000 estimated tokens. Users may configure both the turn and token bounds. Existing `keep_recent_messages` configuration remains readable as a legacy alternative, but ambiguous simultaneous legacy/new retention selectors fail validation rather than receiving hidden precedence.

Cut points preserve complete record groups and provider semantics. AVA never retains a tool result without its call, splits an in-flight tool lifecycle, or character-splits JSON/tool payloads. If one completed turn exceeds the tail budget, AVA follows the useful Pi/OpenCode behavior shape: summarize the completed prefix and retain the newest structurally safe suffix, with explicit omission metadata. The persisted recent tail remains a bounded sanitized continuation projection; physical session-v4 records and exact tool-result bindings remain intact in the append-only session.

### Threshold, Retry, And Safety Behavior

- Automatic compaction evaluates the active context after the latest compaction boundary, not full physical history.
- Manual and overflow compaction use that same active context for summary input, pre-compaction estimates, recent-tail selection, and persisted metadata.
- A provider context-overflow result may trigger one successful compaction and one replay of the original request. A second overflow is terminal and actionable; compaction never loops.
- Local cancellation checks before summary completion and before the guarded append stop compaction without an entry. Rejecting a provider-reported cancelled terminal outcome is still open; it is not equivalent to the local cancellation callback being set.
- Current lease-bound `SessionReadAuthority`, controller-owned append routing, stale-snapshot detection, bounded retry, offline guard, and safe turn-boundary behavior remain mandatory.
- Existing session history is never pruned, rewritten, or destructively compacted. Context projection may omit replaced material without mutating the audit record.

### Diagnostics And Visibility

Compaction lifecycle data identifies the reason (`manual`, `automatic`, or `overflow`), selected provider/model, active pre-compaction estimate, retained-tail estimate, configured/effective threshold, and overflow retry outcome. Fields are additive and privacy-safe; public events, sessions, RPC, and exports do not include raw provider payloads or private reasoning data.

## Verified Reference Behavior

Pi uses automatic and manual compaction, an active-model summary call, bounded recent retention, structurally safe cut points, split-turn handling for oversized turns, cancellation without a successful checkpoint, and a single bounded overflow recovery path. OpenCode similarly keeps recent turns, supports manual/automatic summaries and overflow replay, and emits compaction lifecycle state; its mutable old-tool-output pruning and experimental hooks are not suitable for AVA's append-only audit model.

AVA adopts the useful behavior shape, not either project's architecture, source, history rewriting, extension hooks, or hidden provider-selection behavior.

## Approved Implementation Sequence

1. Make compaction config parsing strict; add percentage thresholds, turn retention, active-by-default provider/model selection, explicit cross-provider selection, and reload/runtime compatibility validation.
2. Introduce one active-context projection and token-accounting path shared by manual, automatic, and overflow compaction.
3. Persist the configured recent tail for every compaction mode using complete turn/tool boundaries and safe oversized-turn handling.
4. Add bounded additive compaction reason/model/threshold/pre/post/retry metadata to session/runtime/RPC surfaces.
5. Update `docs/core/configuration.md`, `docs/core/usage.md`, protocol notes where fields are public, and focused deterministic tests.

## Acceptance Criteria

- Default automatic compaction triggers at 80 percent of the active model context window; a valid user percentage or absolute-token threshold is respected exactly, and zero absolute tokens disables it.
- Malformed, ambiguous, unknown, or incompatible configuration must fail with actionable diagnostics and cannot silently default or disable compaction. **Open:** recognized-field validation is implemented, but unknown-key rejection is not.
- The active model is used by default; an explicit valid same-provider or cross-provider summary model is honored without silent fallback.
- Manual, automatic, and overflow compaction summarize only active context and persist equivalent bounded recent-tail semantics.
- Recent retention keeps complete turn/tool groups, defaults to two turns/20,000 tokens, and safely handles one oversized turn.
- Token metadata describes the active pre/post-compaction context rather than full physical history.
- Context overflow can cause at most one compaction-assisted retry.
- Cancellation, summary failure, stale snapshots beyond the existing bound, or append failure must never produce a partial/false successful checkpoint. **Open:** summary persistence must require an explicit successful provider terminal outcome, including rejection of length-truncated, refused, provider-cancelled, error-ended, or missing-successful-terminal summaries.
- Session-v4 physical ordering, tool-result identity, read/append authority, replay, export privacy, and strict validation remain intact.
- Deterministic fake-provider tests, focused CTest, the full default suite, sanitizer coverage for touched ownership paths, and `git diff --check` pass without paid live-provider calls.

## Explicit Non-Goals

- Completing the master plan's full M7 canonical prompt/tool-schema artifact and cache-prefix provenance system in this usability slice.
- Rewriting or deleting old session records or copying OpenCode's mutable output-pruning behavior.
- Automatic provider-generated branch summaries; the separately implemented direct-TUI abandoned-parent summary remains metadata-only and user-confirmed.
- Silent provider/model fallback, hidden cross-provider calls, repeated overflow retries, or extension hooks that rewrite compaction input/results.

## Implementation Record — 2026-07-20

Implementation in the current working tree now provides:

- strict compaction JSON field typing, bounded integer percentage thresholds, legacy absolute-token/disable behavior, and explicit conflict diagnostics;
- active provider/model summary selection by default plus validated same-provider and explicit cross-provider model selection through normal credentials, with no silent fallback;
- one ordered public active-context projection for summaries/accounting and one physical post-checkpoint projection for exact session-v4 retained-tail reconstruction;
- default two-turn/20,000-token retention, legacy message-count compatibility, atomic tool call/result handling, UTF-8-safe plain-text truncation, and a recognizable latest-user anchor for oversized turns;
- equivalent manual, automatic, and overflow checkpoint data, including reason, selected model, configured/effective threshold, active pre/post estimates, retained estimate, and bounded overflow-retry state;
- additive legacy-readable session validation and privacy-safe event/RPC serialization; and
- focused regression coverage for strict numeric parsing, threshold percentages, cross-provider selection, repeated compaction boundaries, manual recent context, physical session-v4 ordering/bindings, oversized-turn behavior, atomic tool groups, cancellation, overflow retry, runtime, and RPC paths.

Material review findings and disposition:

| ID | Disposition | Evidence |
|---|---|---|
| COMP-1 | Fixed | Legacy v0-v4 and direct default compaction entries remain replay-valid; new additive fields are optional but strict when present. |
| COMP-2 | Fixed | Retained context rebuilds from physical post-checkpoint records while summaries/accounting use the ordered public projection. |
| COMP-3 | Fixed | Structured tool groups are atomic; oversized turns retain a bounded user anchor without character-truncating tool payloads. |
| COMP-4 | Fixed | Fractional, exponent, typed, negative, and overflowing numeric config values are rejected. |

Validation run without paid provider calls:

- `scripts/build.sh --build-dir build` — passed; no work remained after the focused build.
- `scripts/run-tests.sh --build-dir build --output-on-failure` — 97/97 registered tests completed successfully: 80 passed and 17 expected optional/live/tmux/image smokes skipped.
- `scripts/build.sh --build-dir build-sanitize --jobs 2` — passed.
- `scripts/run-tests.sh --build-dir build-sanitize --jobs 2 -R '^ava_tests\\.(session|app_compaction|app_runtime|agent_loop|app_rpc)$' --output-on-failure` — 5/5 focused sanitizer tests passed.
- `git --no-pager diff --check` — passed.

---

# Workstream 5: Automatic Session Titles

## Approved Product Direction

A newly created persistent root session receives a useful title after its first durably committed ordinary user turn. At the post-`AdmissionGuard` scheduling boundary, AVA derives a deterministic, bounded, UTF-8-safe fallback from the original first-user source and synchronously appends it as `session_metadata`. Only the optional provider/model refinement is asynchronous. Queue pressure, print/RPC one-shot teardown, cancellation, offline operation, provider failure, and coordinator shutdown therefore cannot leave an admitted new root without the local fallback.

The application-scoped coordinator permanently admits each session id at most once per process and binds that admission to the captured `AgentLoopResult::committed_turn_id` and first source text. The captured commit entry must belong to the first ordinary user turn. Later turns may append while refinement is queued, but they cannot invalidate or replace the captured first-turn identity. Synthetic delivery turns, sessionless sessions, resumed/non-new sessions, branches/clones/subagent children, manually named sessions (including `name:""` suppression), and already titled sessions do not initiate generation.

Provider refinement is bounded, tool-free, credential-isolated, cancelable, and sanitized before persistence. It may replace exactly the admitted fallback only while the session remains an unnamed root and no explicit manual name, including empty suppression, has appeared. Failures retain the fallback. Metadata appends reload the exact lease-bound history and retry at most three times only when stable `append_commit_state:not_started` proves that no bytes were committed; partial/unknown and committed-to-inode error states are never retried. ASCII punctuation trimming is deliberately locale-independent so arbitrary valid UTF-8 title bytes are preserved.

Fork and clone creation fold title metadata from the complete lease-bound source independently of the copied entry prefix. An explicit branch name wins. Otherwise the latest manual source name, including empty suppression, is persisted; relevant generated-title metadata is retained. Branch ancestry/provenance and automatic-generation exclusions are unchanged.

## Implementation Record — 2026-07-21

Primary implementation areas are `src/ava/app/session_title_coordinator.{h,cpp}`, the post-admission call in `src/ava/app/runtime_prompt.cpp`, full-source metadata folding in `src/ava/session/session_branch.cpp`, and focused title/session tests. Session metadata remains append-only: `generated_title` is separate from manual `name`, and the effective title is manual `name` whenever a manual value has ever been written, otherwise `generated_title`.

RPC and session list/tree surfaces expose the effective title immediately through their existing query results. `session_metadata` and `session_tree` also expose `generated_title` and `has_manual_name` for precedence-aware clients. There is intentionally no automatic-title event in RPC, ACP, or the TUI event stream; clients refresh through existing session metadata/list/tree commands.

Confirmed review findings and disposition:

| ID | Disposition | Evidence |
|---|---|---|
| AST-001 | Fixed | The deterministic fallback is durably appended before provider work is queued, so active/queued shutdown and one-shot teardown retain a title. |
| AST-002 | Fixed | Work carries the captured commit entry id; validation binds it to the first ordinary user turn while allowing later appended turns. |
| AST-003 | Fixed | Fork/clone title metadata is folded from the full source, including generated titles and manual-empty suppression beyond an earlier fork prefix. |

## Validation Record

Validation used deterministic fake generators/transports and made no live or paid provider calls. Coverage includes fallback-before-teardown and under queue pressure, queue delay followed by a second turn, captured first-commit identity, bounded definitely-not-committed append retry with a concurrent tail advance, branch/clone inheritance, manual-empty suppression, direct provider-request isolation, serialization, and multilingual UTF-8 preservation.

- The focused automatic-title CTest passed.
- Focused session/branch, runtime, RPC, and ACP CTests passed.
- The complete configured 102-test CTest suite passed with 85 executed checks and 17 expected optional/live/tmux/image skips.
- The configured production/test build, `clang-format --dry-run --Werror` on changed C++, and `git diff --check` passed.

## Explicit Non-Goals

- A live automatic-title event or a separate title subscription protocol.
- Retitling resumed historical sessions, branches, clones, child sessions, or sessions with any manual-name history.
- Retrying provider requests after cancellation/shutdown or silently selecting another provider/model.
- Rewriting earlier session metadata records or changing branch ancestry semantics.

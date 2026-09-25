# Troubleshooting AVA

Use this symptom-first runbook for common local failures. Start with `ava doctor` (or `ava doctor --json`) because it is passive: it does not call providers, read credential values, launch integrations, mutate config, or create sessions. For data handling and support exports, see [diagnostics.md](diagnostics.md).

## AVA or a build/test command will not start

**Symptom:** `scripts/build.sh` or `scripts/run-tests.sh` reports a build-tree lock.

- Do not run a build and tests concurrently in the same tree.
- An interrupted wrapper deliberately leaves `<build-dir>/.ava-build-tree.lock.d` fail-closed when worker teardown cannot be proven.
- Verify no build, CTest, or detached child process for that tree remains; only then remove the stale lock directory manually and retry. Do not delete it merely because its recorded PID is gone.
- Use `--build-dir` to select the intended configured tree and `--jobs N` to bound parallelism. See [CONTRIBUTING.md](https://github.com/Artificial-Source/AVA/blob/develop/docs/development/contributing.md#quick-start-build-and-test) and [TESTING.md](testing.md#normal-test-run).

**Symptom:** AVA reports another writer/session lease or unsafe state/config paths.

- Exit the other AVA process using that session. Do not bypass a lease or alter ownership/modes to force startup.
- Check that XDG config/state directories are owned by your user and are not symlink/FIFO replacements. Run `ava doctor` for path-specific status. See [CONFIG.md](../core/configuration.md) and [session-format.md](../session-format.md).

## Authentication, provider, or model fails

**Symptom:** missing credential, expired OAuth, unauthorized response, or disconnected authentication guidance.

- Run `ava connect <provider>` (OpenAI also supports browser or headless OAuth) or use a documented provider environment variable for this process.
- Confirm status with `ava doctor` or `/providers`; these surfaces report source/status, not secret values.
- For expired OpenAI OAuth without a usable refresh token, reconnect. A stored credential takes precedence over environment credentials.
- Never paste a key into a prompt, issue, trace, or command line. See [CONFIG.md](../core/configuration.md#auth), [environment-variables.md](../core/environment-variables.md#provider-credentials-and-endpoints), and [providers.md](https://github.com/Artificial-Source/AVA/blob/develop/docs/core/providers.md).

**Symptom:** unknown model, unsupported reasoning level, endpoint error, or a model appears configured but cannot run.

- Use `/models` and inspect `models.json` diagnostics; select a registered `provider/model` and one of its declared reasoning levels.
- Distinguish local configuration from execution evidence: provider/model listing does not prove account access or endpoint availability.
- Check provider status/rate limits/network reachability before classifying an AVA regression. Raw provider bodies are intentionally not exposed.

## TUI, keyboard, mouse, or display is wrong

- Run `/settings` to inspect concise display choices, `/hotkeys` to inspect effective bindings, or `/keybindings validate` for a validation-only config check.
- Try `NO_COLOR=1 ava` for a plain diagnostic run. Under tmux, terminal capabilities may need explicit forwarding; do not infer support from `TERM` alone.
- If keys arrive incorrectly, test without tmux and without custom `keybinds.json`, then restore settings one at a time. Plain Up/Down scroll transcript history by default; they do not replace the draft.
- For cursor/Alacritty issues, retry outside mux paths. Direct Alacritty DA2 gating is asynchronous and best effort; startup never waits for it. `/cursor default steady` returns cursor ownership to the terminal.
- If Mermaid projection fails, the original completed fence should remain visible. Check that `mermaid.enabled` is true and `argv[0]` is the intended absolute bounded helper; an invalid reload keeps the last good config. AVA does not provide full Mermaid or extension-owned renderers.
- Interactive AVA requires a terminal on both stdin and stdout. If the TUI cannot start, use `ava --print` or `ava --rpc` instead of piping into an interactive invocation. Keyboard TUI coverage is not screen-reader certification.
- Clipboard images require a supported image type and an available Wayland/X11 helper; unsupported terminals retain text metadata rather than an inline preview.
- See [terminal-setup.md](https://github.com/Artificial-Source/AVA/blob/develop/docs/operations/terminal-setup.md), [themes-keybindings.md](https://github.com/Artificial-Source/AVA/blob/develop/docs/interfaces/themes-keybindings.md), and the TUI section of [USAGE.md](../core/usage.md#tui-layout).

## A permission is denied or containment differs from expectations

- Read the prompt's operation, risk, path/command summary, and denial reason. Use `/permissions diagnose`, `/permissions audit`, and `/permissions explain <rule_id>` for exact local authority.
- Tool visibility and permission are independent: `--tools` exposes schemas but grants nothing; `--allow-tool` may approve only supported families and does not expose hidden tools. See [tools.md](../core/tools.md#visibility-is-not-authority).
- Project plugins, MCP/LSP config, skills, subagents, and stronger prompt resources require `/trust project`; inspect first with `/trust status`.
- AVA verifies process-group teardown, not descendants that escape by creating a new session. Use an external container/VM/OS sandbox for stronger containment. See [security-sandboxing.md](https://github.com/Artificial-Source/AVA/blob/develop/docs/security/sandboxing.md) and the normative [security/containment.md](https://github.com/Artificial-Source/AVA/blob/develop/docs/security/containment.md).

## A session is corrupt or cannot resume

- Stop all AVA writers and preserve the original session plus any sibling `.torn-tail.*.bin` quarantine before investigation.
- Retry an explicit resume or fork operation: AVA acquires the session lease and can quarantine only a strict-invalid unterminated final suffix before continuing. Newline-terminated corruption and semantic/version errors fail closed and are not silently repaired.
- Never append quarantine bytes after newer records, edit a live session in place, or concatenate an unterminated file. Reconstruct/repair only an offline copy, validate it, and retain originals until recovery is confirmed.
- Use `/export` for a validated portable copy when the session remains readable. See [session-format.md](../session-format.md) and the operator quarantine procedure in [release-checklist.md](release-checklist.md#session-quarantine-policy).

## LSP tools are absent, time out, or return no results

- Run `ava doctor` and inspect global/project `lsp.json` parse, trust, server, and built-in `clangd` status. Missing/invalid configuration leaves LSP tools unavailable.
- Trust project LSP config explicitly. Confirm file extensions select the intended server and that global commands meet safe executable/path rules.
- Approve both `lsp.server.launch` and `lsp.query` where policy requires them. A denial does not fall back to an arbitrary executable.
- Server timeouts/cancellation terminate the verified process group; fix the server/config rather than repeatedly increasing limits blindly. See [lsp.md](../extensions/lsp.md).

## Plugin or MCP commands/tools are missing or fail

- Use `/plugins validate`, `/plugins failures`, and `/plugins inspect` for plugin discovery and enablement. Use `/mcp list`, `/mcp inspect`, `/mcp tools`, and `/mcp restart` for configured servers.
- Confirm project trust, executable/config paths, protocol compatibility, and applicable permission decisions. A discovered extension is not automatically authorized.
- Plugin/MCP public errors intentionally omit raw process/provider payloads. See [plugin-system.md](../extensions/plugin-system.md), [mcp.md](../extensions/mcp.md), and [plugin-compatibility-policy.md](../plugin-compatibility-policy.md).

## Collect diagnostics or request support

1. Record `ava --version`, the AVA commit/revision, Linux distribution, terminal/tmux versions when relevant, and exact redacted reproduction steps.
2. Run `ava doctor --json`. For a reproducible invocation, opt into a bounded private trace with `--trace`; traces are off by default.
3. Run `ava support export` to create a sanitized local JSON support artifact. AVA does not upload it. Review even sanitized artifacts before sharing.
4. Include expected versus actual behavior and the smallest safe logs. Never include credentials, auth files, prompts/session content, proprietary source, raw provider payloads, or exploit details.

Open ordinary bugs and documentation problems through the repository's public issue templates after searching existing issues. Report suspected vulnerabilities privately as directed by the root [SECURITY.md](https://github.com/Artificial-Source/AVA/blob/develop/SECURITY.md), not in a public issue. Support scope and required report details are in [SUPPORT.md](https://github.com/Artificial-Source/AVA/blob/develop/SUPPORT.md).

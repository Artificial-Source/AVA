# AVA Configuration

AVA uses XDG paths on Linux. Process environment inputs and script-only controls are cataloged in [environment-variables.md](environment-variables.md); optional language-server configuration has a focused reference in [lsp.md](../extensions/lsp.md). For model-visible operations versus permission authority, see [tools.md](tools.md).

| Kind | Path |
| --- | --- |
| Config directory | `$XDG_CONFIG_HOME/ava` or `~/.config/ava` |
| Auth file | `$XDG_CONFIG_HOME/ava/auth.json` or `~/.config/ava/auth.json` |
| Automatic session titles | `$XDG_CONFIG_HOME/ava/session-titles.json` or `~/.config/ava/session-titles.json` |
| Global permission rules | `$XDG_CONFIG_HOME/ava/permission-rules.json` or `~/.config/ava/permission-rules.json` |
| Workspace-keyed permission rules | `$XDG_CONFIG_HOME/ava/workspace-permission-rules/<hash>/permission-rules.json` or `~/.config/ava/workspace-permission-rules/<hash>/permission-rules.json` |
| Session state | `$XDG_STATE_HOME/ava/sessions` or `~/.local/state/ava/sessions` |
| Project trust state | `$XDG_STATE_HOME/ava/project-trust.json` or `~/.local/state/ava/project-trust.json` |
| Private diagnostic state | `$XDG_STATE_HOME/ava/diagnostics` or `~/.local/state/ava/diagnostics` |
| Private runtime traces | `$XDG_STATE_HOME/ava/diagnostics/traces` or `~/.local/state/ava/diagnostics/traces` |
| Local support exports | `$XDG_STATE_HOME/ava/support` or `~/.local/state/ava/support` |

## Settings Architecture And Resource Packages

AVA intentionally uses narrow domain-specific config files instead of a single Pi-style merged `settings.json`. Each file has one owner and validator: `providers.json` for user-defined runtime provider endpoints (see [custom providers](custom-providers.md)), `auth.json` for provider credentials, `models.json` for model registry overrides and scoped cycling, `display.json` plus `themes/*.json` for TUI appearance, `keybinds.json` for TUI actions, prompt files under the config/project resource directories, plugin/MCP/LSP config files, compaction config, trust state, and permission rules. This keeps secret handling, model context, executable resources, and UI preferences behind separate safety boundaries and avoids silently granting authority through a model-writable project settings file.

Config writes that AVA performs validate the candidate before committing where possible. Display theme/image writes and keybinding init/import/set/reset use owner-only atomic replacement and reject symlink targets; invalid hand-edited files surface path-specific diagnostics through `/reload`, startup alerts, or the relevant validation command while the previous active runtime state remains in use. Project-local resources that can influence execution or stronger model context remain gated by `/trust project`; plain context files (`AGENTS.md`/`CLAUDE.md`) can load without trust, but project prompt commands, skills, plugins, MCP/LSP config, and project system prompt files are skipped until trusted.

Pi-style package/resource management (`packages list|install|remove|update|config` and `/packages`) is deferred. AVA does not install remote npm/git packages, self-update, or fetch marketplace resources because that needs source allowlists, provenance/signing, compatibility policy, rollback, and trust UX. Today users install resources manually by placing files in the documented global config directories or in a trusted project `.ava/` directory:

- custom TUI themes: `$XDG_CONFIG_HOME/ava/themes/*.json`
- global prompt resources: `$XDG_CONFIG_HOME/ava/SYSTEM.md` and `APPEND_SYSTEM.md`
- global prompt commands, skills, and subagents: `$XDG_CONFIG_HOME/ava/commands/`, `skills/`, and `agents/`
- global plugins/MCP/LSP config: `$XDG_CONFIG_HOME/ava/plugins/`, `mcp.json`, and `lsp.json`
- trusted project resources: `.ava/commands/`, `.ava/skills/`, `.ava/agents/`, `.ava/plugins/`, `.ava/mcp.json`, `.ava/lsp.json`, `.ava/SYSTEM.md`, and `.ava/APPEND_SYSTEM.md`

`ava packages ...` and `/packages ...` currently report this deferral instead of performing side effects or sending the request to the model. AVA also does not enable analytics/telemetry, version checks, package updates, or self-update behavior. `--offline` disables provider model calls for prompt turns and provider-backed compaction before credential resolution; it is not an OS/network sandbox, so network-capable tools still depend on tool visibility plus permission policy.

## Automatic Session Titles

Persistent root sessions receive a deterministic title synchronously after their first ordinary prompt commits. AVA then makes one bounded asynchronous refinement attempt with the active provider and model by default, without tools or session assistant records; failure leaves the local fallback intact. Manual `/name` values always win; `/name --clear` records an empty manual value and permanently suppresses generated-title fallback for that session. Branches, imports, resumed sessions, child sessions, sessionless runs, and synthetic delivery turns do not initiate generation.

Strict optional configuration lives in `session-titles.json`:

```json
{"schema_version":1,"enabled":false}
```

Set `enabled` to `false` to disable generation. The process override `AVA_SESSION_TITLES=off` also disables generation (`on` defers to the file/default); any other value is rejected. To override the model on the active provider, add `"model":"..."`. A cross-provider override requires both `"provider":"..."` and `"model":"..."`; that provider's own configured credential is resolved for the bounded attempt. Unknown fields and unsupported schema versions are rejected.

## Diagnostics State

`ava doctor` is a read-only offline inspection and writes no state. Explicit `--trace` runs and terminal failures use owner-only state beneath `$XDG_STATE_HOME/ava/diagnostics`; `ava support export` publishes a unique owner-only JSON artifact beneath `$XDG_STATE_HOME/ava/support` and prints its local path. Existing directories must be current-user-owned and private, diagnostic files must be regular single-link mode `0600`, and AVA rejects unsafe symlink, FIFO, hardlink, or group/world-writable replacements rather than following them. Traces are capped at 10,000 events and 10 MiB. No diagnostic setting enables telemetry or automatic upload. See [`diagnostics.md`](../operations/diagnostics.md) for the data contract and privacy exclusions.

## TUI Display

The TUI has built-in `dark`, `light`, and `plain` display modes. Persist a built-in or custom mode with `/theme`:

```sh
/theme light
/theme plain
/theme ocean
/theme reset
```

Image preview visibility and maximum preview width (in terminal cells) are optional fields in the same file:

```sh
/images                 # show configured and effective visibility
/images on
/images off
/images reset           # clear show_images and restore default on
/image-width            # show configured and effective width
/image-width 80
/image-width reset      # clear image_width_cells and restore default 60
```

The settings are stored in `$XDG_CONFIG_HOME/ava/display.json`:

```json
{
  "theme": "light",
  "show_images": true,
  "image_width_cells": 60,
  "cursor_style": "bar",
  "cursor_blink": true,
  "mermaid": {
    "enabled": false,
    "argv": ["/absolute/helper", "..."]
  }
}
```

Defaults when keys are absent preserve historical behavior: automatic theme selection, `show_images=true`, `image_width_cells=60`, terminal-owned cursor shape/blink, and Mermaid disabled. `cursor_style` accepts `default`, `block`, `underline`, or `bar`; `cursor_blink` is boolean. `/cursor <style> [blink|steady]` updates both. AVA emits DECSCUSR only while a non-default cursor is forced, resets it for shell/editor/suspend handoff, reapplies it on return, and restores the terminal default on exit. Width accepts integers only in the inclusive range 8–160 and is clamped again to the current content viewport when rendering. Field-specific writes update or erase only the owned key and preserve every other recognized or unknown top-level field. A malformed recognized field rejects load and update, leaves the file unchanged, and keeps the last known-good presentation active.

`mermaid` is optional and disabled by default. When enabled, `argv` must begin with an absolute application-owned helper path and remains bounded. Only completed assistant fenced blocks labelled `mermaid` are eligible. The helper receives literal diagram text for a presentation-only projection; disabled, pending, failed, or stale projection shows the original fence. The TUI never launches the subprocess, transcript/session/provider semantics are unchanged, and this is not full Mermaid support or an arbitrary renderer interface. Invalid automatic reload retains the last good display configuration.

After editing `display.json` by hand, the interactive TUI automatically reloads the file without restarting. `/reload theme` is still available for an explicit retry or diagnostic. Invalid JSON, unsupported theme values, non-boolean `show_images`, or out-of-range/`float`/`string` `image_width_cells` values are reported with the config path, and the previous active presentation remains in use.

Custom themes live under `$XDG_CONFIG_HOME/ava/themes/*.json`. AVA accepts a lean Pi-style file with `name`, optional `vars`, eight required color roles, and optional surface backgrounds:

```json
{
  "name": "ocean",
  "vars": {
    "primary": "#0066cc",
    "paper": 255
  },
  "colors": {
    "text": "",
    "muted": 242,
    "success": 34,
    "warning": "#ffaa00",
    "error": "#ff0000",
    "accent": "primary",
    "screenBg": "paper",
    "composerBg": 236,
    "toolBg": 235,
    "questionBg": 237
  }
}
```

Color values can be `""` for the terminal default, a 0-255 xterm color number, a 6-digit hex RGB string approximated to xterm-256, or a variable name from `vars`. Optional `toolBg` and `questionBg` each fall back to `composerBg` when omitted. Custom theme names must be unique, non-empty, and free of whitespace or path separators. `/settings` shows valid custom theme files as selectable theme rows; invalid custom files are ignored during discovery and reported when directly selected through `display.json` or `/theme`. An editor-facing schema for this AVA-native format lives at [`docs/schema/theme.schema.json`](../schema/theme.schema.json); the runtime loader remains the authoritative validator.

Custom theme discovery and the interactive display watch are application-owned and bounded. AVA opens each candidate once with a no-follow descriptor read (no size-check/reopen race), rejects symlinks and special files, and applies these conservative limits from `display_settings.h`:

- `kMaxTuiCustomThemeFileBytes` = 64 KiB per file
- `kMaxTuiCustomThemeCandidates` = 64 candidate files/results
- `kMaxTuiCustomThemeCatalogAggregateBytes` = 256 KiB aggregate read work per scan

Candidate files are considered in normalized absolute-path order. Listing and the watch catalog keep the first valid file per theme name and omit later duplicates; configured/`/theme` named load remains fail-closed on duplicate names with an actionable local error. A single oversized, invalid, or unreadable unconfigured custom theme is skipped and must not fail a configured built-in display reload. Configured custom themes stay fail-closed: invalid or unreadable selected themes do not become authoritative.

You can also override the theme for the current process:

```sh
AVA_TUI_THEME=light ava
AVA_TUI_THEME=plain ava
NO_COLOR=1 ava
```

`dark` is the default fallback ncurses palette. `light` selects a light built-in palette. `plain` disables ANSI styling. Precedence is `NO_COLOR`, then an active `/settings` highlight preview (presentation-only), then `AVA_TUI_THEME`, then `display.json`, then startup OSC 11 background inference on direct terminals (skipped under tmux), then `COLORFGBG`, then the built-in dark fallback. The compact `/settings` TUI separates Theme choices from Display image/width controls and can report `OSC 11` as the resolved source. Highlighting a previewable row never writes config; Enter confirms once through the same authoritative writers as `/theme`, `/images`, and `/image-width`. During an interactive TUI run, AVA polls `display.json` and the selected custom theme file and applies valid changes automatically.

## Terminal Hyperlinks Under tmux

AVA emits OSC 8 hyperlinks only when the detected terminal path has an explicit support signal. tmux's `terminal-features` and `terminal-overrides` state is client/server configuration, not pane environment, so AVA does not infer hyperlink forwarding from `$TMUX`, `$TERM_PROGRAM`, or `TERM=tmux-*` alone; a false positive would hide visible URL fallbacks while tmux strips or withholds OSC 8 output. If your tmux config enables hyperlinks, for example with `set -ga terminal-features ',*:hyperlinks'` or equivalent `Hls`/`Hlr` overrides, launch AVA with:

```sh
AVA_TUI_TMUX_HYPERLINKS=1 ava
```

To make the hint available to panes from tmux configuration, pair the tmux hyperlink setting with `set-environment -g AVA_TUI_TMUX_HYPERLINKS 1`.

The override only affects the tmux fallback path: image protocols remain disabled under tmux, while assistant Markdown links may use OSC 8.

## External Editor

Set `VISUAL` or `EDITOR` to the editor command AVA should use for the TUI external-editor shortcut. Pressing Ctrl+G writes the current composer draft to an owner-only temporary file, restores the terminal to shell mode, runs the configured editor command with that file path, and replaces the draft with the saved file when the editor exits successfully. If the editor exits nonzero, AVA keeps the original draft.

## Clipboard Image Paste

Pressing Ctrl+V in the TUI imports a supported image from the clipboard as a pending attachment when AVA can read one through Linux clipboard helpers. AVA probes `wl-paste` for Wayland and `xclip` for X11, accepts PNG, JPEG, WebP, and GIF bytes, then stores the image through the same session-owned attachment path used by `/attach`. Set `AVA_CLIPBOARD_IMAGE_FILE=/absolute/path/to/image.png` to bypass the OS clipboard and import that image file; this is intended for deterministic terminal smoke tests.

## Providers (`providers.json`)

Optional `$XDG_CONFIG_HOME/ava/providers.json` declares user-defined runtime providers
for OpenAI Chat Completions, OpenAI Responses, and Anthropic Messages protocols.
The file is loaded once at process start into an immutable application catalog;
malformed or unsafe present files fail closed. Models stay in `models.json` and must
use a matching `api_family`. Credentials stay in `auth.json` or the configured env
name. Provider definitions require a process restart — `/reload providers` reports
restart-required. Full schema, ownership rules, and examples:
[custom-providers.md](custom-providers.md).

Built-in vendor status (including xAI, Groq, Cerebras, Together, Fireworks, and
Mistral) remains in [providers.md](providers.md).

## Auth

Create an OpenAI OAuth credential with:

```sh
ava connect openai
```

The command opens an OpenAI login method picker. Browser OAuth prints an OpenAI authorization URL, waits for the callback on `http://localhost:1455/auth/callback`, and stores the resulting credential owner-only at the AVA auth path. Headless OAuth uses OpenAI's device-code flow and prints `https://auth.openai.com/codex/device` plus the code to enter.

Interactive provider login is available for API keys:

```sh
ava auth login
ava login anthropic
ava auth login moonshot --api-key
ava connect kimi --api-key
```

When the provider is omitted, `ava auth login`, `ava login`, and interactive `ava connect` open a searchable terminal provider picker before asking for login method and secret. Secrets are read without terminal echo when stdin is a TTY. In the TUI, use `/connect` or `/login` to open the same provider flow as a modal; OpenAI shows browser OAuth, headless OAuth, and API key options. Non-OpenAI providers use API-key setup unless their own documented auth flow is explicitly implemented.

Headless API-key setup is available for OpenAI, Anthropic, DeepSeek, Gemini, Moonshot/Kimi, OpenRouter, Z.AI, xAI, Groq, Cerebras, Together, Fireworks, Mistral, and user-defined `api_key` provider ids:

```sh
ava connect openai --headless-oauth
printf '%s\n' "$OPENAI_API_KEY" | ava connect openai --api-key-stdin
printf '%s\n' "$ANTHROPIC_API_KEY" | ava connect anthropic --api-key-stdin
ava connect gemini --api-key-env GEMINI_API_KEY
ava connect moonshot --api-key-env MOONSHOT_API_KEY
ava connect kimi --api-key-env KIMI_API_KEY
```

OpenAI OAuth credential format:

```json
{"openai":{"type":"oauth","access_token":"...","refresh_token":"...","expires_at":1893456000}}
```

API key format:

```json
{"openai":{"type":"api_key","api_key":"sk-..."},"anthropic":{"type":"api_key","api_key":"sk-ant-..."}}
```

Anthropic Claude OAuth bearer tokens can also be supplied with `ANTHROPIC_OAUTH_TOKEN` or the Anthropic SDK-compatible `ANTHROPIC_AUTH_TOKEN`. When no stored Anthropic credential is present, `ANTHROPIC_OAUTH_TOKEN` is preferred over `ANTHROPIC_AUTH_TOKEN`, and both are preferred over `ANTHROPIC_API_KEY`. Stored Anthropic OAuth entries use the provider-scoped auth shape below; `refresh_token`, `expires_at`, `account_id`, and `source` are optional, but expired or near-expiry OAuth entries require a refresh token to be used safely. AVA writes canonical `refresh_token` and `expires_at` fields; it also accepts `refresh` and `expires` aliases when reading manually-created files. AVA does not initiate Anthropic interactive OAuth because Anthropic does not document a third-party authorization or device flow for AVA-style clients.

```json
{"anthropic":{"type":"oauth","access_token":"...","refresh_token":"...","expires_at":1893456000,"account_id":"acct_...","source":"claude"}}
```

Auth files are written owner-only. Provider credential setup preserves existing provider entries in the same auth file. Explicit AVA auth entries take precedence; AVA also attempts to read legacy `~/.ava/credentials.json` and the legacy-compatible XDG auth file for OpenAI migration when no AVA OpenAI credential is stored.

OAuth credentials refresh automatically before use when a refresh token is present. If OpenAI refresh fails or the credential has no refresh token, rerun `ava connect openai`; for stored Anthropic OAuth, update `auth.json` with a fresh OAuth credential or remove the stored Anthropic entry before relying on Anthropic environment credentials.

## Permission Rules

Persistent permission rules are stored owner-only outside model-writable workspace files. Global rules use `$XDG_CONFIG_HOME/ava/permission-rules.json`. Workspace-scoped rules are keyed by the normalized workspace path and written under `$XDG_CONFIG_HOME/ava/workspace-permission-rules/<workspace-hash>/permission-rules.json`; use `/permissions list` or RPC `permission_rules` as the authoritative way to inspect the exact path for a workspace. Legacy `$WORKSPACE/.ava/permission-rules.json` files are intentionally ignored for enforcement because normal file tools can edit workspace files. Storage walks absolute path components through descriptors, rejects symlinks and group/world-writable ancestors except root-owned sticky shared namespaces such as `/tmp` and current-user-owned directories whose group write is limited to a verified private primary group; world-writable non-root-sticky and non-private group-writable ancestors remain rejected. The final rules directory must be current-user-owned mode `0700` and its file mode `0600`. Writes create missing components with descriptor-relative `mkdirat`/`openat`, atomically replace through the validated parent descriptor, and fail closed on an unsafe replacement rather than silently dropping Denies. Rule storage also rejects malformed JSON, unsupported schema versions, broad path rules, and unsupported operations before prompting a fallback resolver.

Use `/permissions list`, `/permissions explain <rule_id>`, `/permissions add ...`, `/permissions remove <rule_id>`, `/permissions audit ...`, and `/permissions diagnose ...` in interactive mode, or the matching RPC `permission_rules`, `permission_rule_add`, and `permission_rule_remove` requests for automation. Interactive list, add/remove receipts, and explain titles lead with truthful human summaries and display-only ordinals; exact `permrule_…` ids remain required for explain/remove and stay available on secondary receipt lines plus full explain detail. Rules can match exact path-oriented operations such as `read`, `search`, `edit`, and `lsp.query`, or exact command-oriented operations such as `bash`, `network.fetch`, `network.search`, `lsp.server.launch`, MCP, and plugin prompts. Task launch is prompt-free and audited unless an exact persisted task deny matches; only sensitive nested or child actions prompt. Foreground child Ask actions still use normal permission UI and background child Ask actions fail closed. Hard policy denies are never upgraded by persistent rules or headless flags.

Permission-rule files now write strict `schema_version: 2`. A `bash` Allow for a sealed command must contain the exact `command_recipe_key` shown by the permission prompt or audit: use the workspace key for workspace storage and the global key for global storage. Recipe keys bind the typed recipe, sealed executable identity/origin, environment profile schema, containment profile/network mode, and preserved logical workspace where applicable; they never use session IDs, timestamps, synthetic environment paths, or environment entries. Schema-v1 files are read for compatibility, but v1 command Allows are non-authoritative while exact v1 command Denies remain active. `/permissions add ... recipe_key=<key>` and RPC `permission_rule_add` accept audited keys. Critical and raw-shell command Allows are always one-shot: new `critical_acknowledged=true` rules are rejected, and old schema-v2 exact Critical acknowledgements remain parseable only so users can inspect/remove them; they never recover execution authority. Matching Denies always take precedence.

### Audit command format and secret safety

Local (non-ACP) permission audit entries use `recipe_display` as the command field for reusable sealed commands. When a command does not have a stable recipe (one-shot Critical, raw shell, or credential-bearing), the audit command field is replaced with the redacted marker `<redacted one-shot command>` instead of persisting raw argv. This prevents credential-bearing CLI forms from leaking into audit JSON.

Session grants for `bash` operations bind the exact `command_recipe_key` (workspace recipe key) rather than raw command fingerprint authority. The grant stores `command_recipe_display` for display and audit only; grant matching is recipe-key authoritative, not text-based. Raw command text is retained in the grant only for display and audit, never for matching.

Secret detection conservatively refuses stable recipe minting for common credential-bearing CLI forms across separate, concatenated short, long, and `--option=value` variants: curl/wget `-u`, `-H`, `--user`, `--proxy-user`, `--header`, `--proxy-header`, `--cookie`, `--oauth2-bearer`, `--aws-sigv4`, cert/key/pass options, wget http/proxy user/password/header, and general token/secret/password/api-key/auth/credential/bearer/signature/cookie option names. Credential-like URL query fields and URL userinfo (`scheme://user:pass@host`) are also detected. This favors false-negative safety (disabling reusable remember) over persistence of plaintext.

### v1 removal migration

To migrate from schema-v1 command Allows:

1. Run `/permissions list` or RPC `permission_rules` to inspect all stored rules.
2. Remove each schema-v1 `bash` Allow individually with `/permissions remove <rule_id>` or RPC `permission_rule_remove`. v1 command Allows must be removed one at a time; adding v2 rules is blocked until all v1 command Allows are cleared from the file. The file migrates to `schema_version: 2` automatically after the last v1 command Allow is removed.
3. Re-add eligible commands as schema-v2 recipe-key Allows using the `command_recipe_key` and `recipe_display` from the permission prompt or audit of a sealed plan. Critical/raw-shell commands have no persistent-Allow migration and require one-shot approval each time.

Exact raw Deny rules may store command text in owner-only (mode 0600) rule storage. Users should not embed secrets in Deny rules; secret-like Allow rules are not reusable because recipe minting is refused for credential-bearing commands.

The `task` operation covers model-visible subagent delegation through the built-in `task` tool. Rules and headless policies match the exact `task` tool name plus the requested `subagent_type` command, such as `general`, `explore`, or a configured custom subagent.

## Models

Built-in default: `openai/gpt-5.5`. Built-in OpenAI choices also include `gpt-5.6-sol`, `gpt-5.6-terra`, and `gpt-5.6-luna`; select one with `/model` or set it as `default_model` below. The GPT-5.6 profiles intentionally use a 272K context boundary so AVA stays in OpenAI's short-context pricing tier; each retains the documented 128K maximum output and current short-context token prices.

Optional model override file: `$XDG_CONFIG_HOME/ava/models.json`.

```json
{
  "default_provider": "openai",
  "default_model": "gpt-5.5",
  "models": [
    {
      "provider": "openai",
      "id": "gpt-5.5",
      "family": "gpt-5",
      "context_window_tokens": 272000,
      "max_output_tokens": 128000,
      "pricing": {
        "input_per_million": 1.25,
        "output_per_million": 10.0,
        "cache_read_per_million": 0.125,
        "cache_write_per_million": 1.25,
        "reasoning_per_million": 10.0
      }
    }
  ]
}
```

Model entries are additive overrides. `provider` and `id` are required; omitted fields on built-in model overrides inherit the built-in metadata, including `context_window_tokens`. For brand-new custom models, set `context_window_tokens` so token percentage and context-aware compaction can work. Optional fields include `display_name`, `family`, `api_family`, `context_window_tokens`, `max_output_tokens`, `supports_tools`, `supports_streaming`, `supports_reasoning`, `reports_usage`, `input_modalities`, `output_modalities`, `reasoning_levels`, `reasoning_level_map`/`thinking_level_map`, `reasoning_format`, `compatibility_quirks`, and `pricing`. `input_modalities` currently recognizes `text` and `image`; models that omit `image` reject replayed image attachments before provider requests. Built-in OpenAI Responses, Anthropic Messages, and Gemini GenerateContent image-capable profiles declare `text` plus `image`; custom compatible-provider image models should do the same only when their endpoint accepts chat-completions image URL blocks. `/providers [query]` reports provider runtime availability (built-in and user-defined), credential source status without secret values, OAuth disposition, endpoint settings, and compatibility quirks. `/models <query>` and the TUI model selector report advisory diagnostics for custom models that omit context windows, input modalities, API family, support flags, usage/pricing metadata, or reasoning levels, plus provider/API-family mismatches, unknown API families, invalid provider ids, and unregistered providers. These diagnostics do not block switching when the provider is registered; rows for unregistered providers are disabled because backend model switching rejects them. Pricing values are USD per one million tokens and are local static metadata; AVA does not fetch live prices. Cost is reported only when the saved provider usage and configured pricing are complete for the billable token types in that assistant response.

Accepted pricing aliases include `input_usd_per_1m`, `output_usd_per_1m`, `cache_read_usd_per_1m`, `cache_write_usd_per_1m`, and `reasoning_usd_per_1m`.

Reasoning controls are model/API-family specific. OpenAI Responses models accept reasoning level/effort metadata. OpenAI chat-completions compatible routes accept level-only controls where supported. Anthropic Messages models accept `enabled` with a budget at least 1024 tokens and below the model output limit; `adaptive` is accepted only for profiles that explicitly list it and does not accept a manual budget. Kimi/Moonshot-style compatible routes can preserve `reasoning_content` when their model metadata declares the matching reasoning format and compatibility quirk. DeepSeek uses the compatible chat endpoint, maps AVA `high`/`xhigh` reasoning levels to `reasoning_effort=high|max`, and parses `reasoning_content` from responses; request-time portable projection retains it only for an exact-compatible target and strips it for incompatible targets. Gemini GenerateContent models currently reject explicit reasoning options until AVA has model metadata and request semantics for them. `reasoning_level_map` and the Pi-compatible alias `thinking_level_map` are JSON objects keyed by user-facing AVA/Pi reasoning levels. Values may be a provider-level string rewrite, `true` to allow the same level string through, or `false`/`null` to block the level fail-closed. AVA parses all map keys, including future/provider-specific names, so custom model typos should be tested with `/models`/RPC before use. The Pi-compatible startup flag `--thinking off|<level>` is an alias for the same runtime reasoning selection: `off` clears explicit reasoning and other levels must be supported by the active model's resolved `reasoning_level_map`/`reasoning_levels` policy.

Provider request metadata also supports prompt/cache-control hints where a provider API can use them. Anthropic content-part serialization preserves native tool use/results, thinking signatures or redacted thinking markers, cache usage, stop reasons, and provider-native reasoning blocks in session replay without exposing opaque signatures in exports.

## LSP Servers

Optional LSP config files:

```text
$XDG_CONFIG_HOME/ava/lsp.json
<workspace>/.ava/lsp.json
```

Both files retain the schema-v1 explicit `servers` array. Global servers are loaded before trusted workspace servers. Missing files simply leave LSP tools unavailable; malformed present files disable the configured provider for that context. A global file that defines `builtin_servers` must be owned by the current user, have one hard link, and not be group- or other-writable; explicit-only global files retain their existing metadata compatibility. Global LSP config must use absolute executable/script paths or trusted `PATH` command names; workspace-relative executables or script arguments such as `./server`, `.ava/server.js`, or `node_modules/.bin/server` are rejected unless they live in trusted project LSP config. Global LSP subprocesses launch from the global config directory, or `/` if that source would be workspace-contained, while the LSP protocol still receives the current workspace root; project LSP subprocesses launch from the trusted workspace.

The built-in recipe is separately **off by default**. Only the owner-controlled global file may opt into the exact `clangd` id with the optional bounded top-level `builtin_servers` array. The project file rejects this field, even after project trust. An absent or empty array disables the recipe. Every id other than `clangd`, plus duplicates, wrong types, control bytes, and more than one id, is rejected.

```json
{
  "version": 1,
  "builtin_servers": ["clangd"],
  "servers": [
    {
      "id": "cpp",
      "argv": ["clangd", "--background-index"],
      "file_extensions": [".cpp", ".h"],
      "language_id": "cpp",
      "timeout_ms": 3000,
      "startup_timeout_ms": 3000
    }
  ]
}
```

The sole built-in is `clangd` for common C/C++ extensions with `clangd --background-index`. An explicit server with id `clangd` stays authoritative and suppresses the built-in. Opt-in discovers only an already-installed executable in fixed system directories or the narrow owner-safe direct user location `~/.local/bin`. Discovery rejects workspace-local, symlinked, hardlinked, writable, script/wrapper, or otherwise unsafe executable identities and unsafe directory chains. It never downloads, installs, checks for updates, invokes a package/toolchain manager, consults project `.venv`, `node_modules`, project bins, or checked-in scripts, or performs network access. The sealed logical-path executable identity includes owner, group, mode, link count, device, inode, size, and ctime. Launch reopens and revalidates every field and executes the verified descriptor with `fexecve`, rather than performing a new `PATH` lookup. This installed-only `clangd` integration is the sole automatic LSP recipe. Every other server requires an explicit `servers` declaration and remains subject to the existing trust and launch-permission boundary.

`id` is a unique short identifier using letters, digits, `_`, `-`, or `.`. `argv` is a non-empty JSON string array executed directly without a shell. `file_extensions` is an optional JSON string array; when omitted or empty, the server can match any file. `language_id` defaults to `plaintext` and is used for bounded document synchronization. `timeout_ms` is parsed first, defaults to `3000`, and bounds individual operations. `startup_timeout_ms` independently bounds initialization and defaults to the parsed `timeout_ms`; both must be base-10 integers from `100` through `30000`. Known fields reject wrong JSON types, mixed arrays, duplicate server ids, control bytes, oversized values, and configs over 64 KiB.

Using an LSP tool first requests `lsp.query` for the target file or workspace. Starting a subprocess separately requests high-risk `lsp.server.launch`. Explicit configured servers retain their exact JSON-array argv permission identity. A built-in prompt uses a bounded object containing clear exact argv plus a deterministic fingerprint of the sealed executable identity, so exact, session, and persistent grants cannot transfer to a replacement inode or changed metadata. This permission representation is never reparsed for execution. Project-local server code is available only through trusted `$WORKSPACE/.ava/lsp.json`.

Provider construction and schema registration are lazy. `ava doctor` and `/context lsp` are passive and never launch a server. Doctor emits one fixed-id `clangd` check; `/context lsp` reports fixed `disabled`, `available`, `not-found`, or `unsafe` status and fixed reasons without exposing discovered paths, argv, fingerprints, config contents, or raw discovery errors.

Built-in clients route deterministically by document. `clangd` uses the nearest logical ancestor containing `compile_commands.json`, `compile_flags.txt`, or `.clangd`; the logical workspace is the fallback. Markers select only a root and never add argv, environment, or launch authority. Clients are cached by built-in id and logical root, and concurrent first use of one root is deduplicated.

AVA reads a document or config through the shared `AnchorSet`/`AnchorOpen` path and one nonblocking, close-on-exec descriptor, then verifies and bounds that regular descriptor. Logical workspace, document, root, and config identities are retained rather than canonicalized. Contained symlinks may resolve only within the selected writable anchor; escapes are rejected. External compatibility reads cannot use a symlink to enter any writable anchor. FIFO, nonregular, missing, escaped, and over-limit inputs fail instead of being treated as text.

An approved LSP server inherits only a compatibility allowlist: `HOME`, `USER`, `LOGNAME`, `TMPDIR`/`TMP`/`TEMP`, `LANG`, `LANGUAGE`, `LC_ALL` and `LC_*`, XDG config/cache/data/state homes, `TERM`, `COLORTERM`, and AVA's fixed trusted `PATH`. Provider, cloud, API-key, token, secret, arbitrary `AVA_*`, and unlisted toolchain variables are not forwarded. One absolute operation deadline spans bounded file acquisition, full-text `didOpen`/versioned `didChange`, and the request/response exchange. Advertised capabilities, messages, notifications, returned diagnostics, tracked documents, and diagnostic cache size/text/count are bounded. Pull diagnostics and centrally dispatched workspace-confined publish diagnostics are supported; changed content clears stale target state, malformed/out-of-workspace notifications fail with fixed local errors, and server stderr or arbitrary protocol payloads do not enter provider-visible failures. Cancellation, timeout, and client teardown send TERM and then KILL to the verified server process group.

## Compaction

Optional compaction config file: `$XDG_CONFIG_HOME/ava/compaction.json`.

```json
{
  "auto_threshold_percent": 80,
  "keep_recent_turns": 2,
  "keep_recent_tokens": 20000,
  "max_summary_bytes": 16384
}
```

`/compact` generates a provider-backed summary and records an append-only compaction boundary. Manual, automatic, and context-overflow compaction summarize only the active context at and after the latest valid boundary, then retain the same bounded recent-turn projection. Physical session history is not rewritten.

The summary call uses the active provider and model by default. A `model` override selects that model on the active provider. A `provider` plus `model` selects that exact configured pair, including a cross-provider pair, through the normal registry and credential path. `provider` without `model` is invalid. Explicit selections are revalidated for every parent-session compaction and by `/reload compaction`; AVA does not silently fall back. Task children snapshot threshold and retention settings at launch but deliberately summarize with the child's active provider/model, so a parent-global compaction model override does not silently change child summary identity.

Automatic compaction defaults to `auto_threshold_percent: 80`, applied to the active conversation model's context window. When that window is unknown, AVA uses a conservative 100,000-token effective window (80,000 tokens at the default percentage). Percent values must be integers from 1 through 95. The legacy absolute `auto_threshold_tokens` remains supported; explicit `0` disables automatic compaction. The percent and token forms cannot appear together.

`keep_recent_turns` defaults to two complete newest user turns and `keep_recent_tokens` defaults to 20,000 estimated tokens. Tool call/result groups and committed session-v4 groups remain structurally complete. An oversized completed turn uses a UTF-8-safe, structurally safe suffix and records explicit omission metadata. The legacy `keep_recent_messages` selector remains supported as an alternative, but it cannot be combined with `keep_recent_turns`. `max_summary_bytes` must be an integer from 1 through 1,048,576. All known fields reject wrong JSON types; unknown fields remain tolerated for forward compatibility.

## Prompts

Prompt override path:

```text
$XDG_CONFIG_HOME/ava/prompts/<provider>/<family>/<mode>.txt
```

Examples:

```text
~/.config/ava/prompts/openai/gpt-5.5/build.txt
~/.config/ava/prompts/openai/gpt-5.5/plan.txt
```

Context instruction files are discovered from the workspace root to the current directory. In each directory AVA loads the first present file by Pi-compatible priority: `AGENTS.md`, `AGENTS.MD`, `CLAUDE.md`, then `CLAUDE.MD`. The global context file path is `$XDG_CONFIG_HOME/ava/AGENTS.md` by default and uses the same sibling fallback names when `AGENTS.md` is absent. Context files are bounded, symlink-rejected, and loaded without requiring project trust, matching their role as visible user-authored instructions.

AVA records base prompt metadata separately from the effective provider system prompt. `/context` reports whether the selected base prompt came from an override, the source path when one exists, byte count, and a content fingerprint. Persisted JSONL `session_start` entries record the prompt override boolean plus loaded context sources; runtime/RPC `session_start` events intentionally expose only mode/provider/model. The full prompt text is owned by the effective `system_prompt` assembled from base prompt selection, `SYSTEM.md`/`APPEND_SYSTEM.md`, context files, skills, and extension resources; this avoids duplicating prompt text in runtime state.

System prompt resource paths:

```text
$XDG_CONFIG_HOME/ava/SYSTEM.md
$XDG_CONFIG_HOME/ava/APPEND_SYSTEM.md
$WORKSPACE/.ava/SYSTEM.md
$WORKSPACE/.ava/APPEND_SYSTEM.md
```

`SYSTEM.md` replaces the selected built-in or provider/family/mode prompt text.
`APPEND_SYSTEM.md` appends to the selected prompt text. Context files, loaded
skills, available subagents, and extension resources are still appended after these resources. Trusted project files win over
global files with the same name; untrusted project files are skipped and the
global file is used when present.

Process-local CLI prompt overrides:

```sh
ava --system-prompt "Use this system prompt" --append-system-prompt "Extra instruction"
ava --append-system-prompt "First extra instruction" --append-system-prompt "Second extra instruction"
```

`--system-prompt` replaces built-in prompts, provider/family prompt overrides,
and `SYSTEM.md` files for the current process. Repeated
`--append-system-prompt` values are joined with blank lines and replace
discovered `APPEND_SYSTEM.md` files for the current process. Context files,
prompt commands, skills, subagents, and plugin prompt/skill resources are still appended
after the CLI prompt text.

Prompt command template paths:

```text
$XDG_CONFIG_HOME/ava/commands/*.md
$XDG_CONFIG_HOME/ava/command/*.md
$WORKSPACE/.ava/commands/*.md
$WORKSPACE/.ava/command/*.md
```

The filename becomes the slash command name, for example `review.md` becomes `/review`.
Frontmatter can set `description`, `argument-hint`, `argument_hint`, or `hint`.
The template body supports `$1`, `$2`, `$@`, `$ARGUMENTS`, `${1:-default}`,
`${@:N}`, `${@:N:L}`, and `$$`.

## Subagents

The built-in `task` tool can run foreground or background child sessions through configured subagents. A definition can instead be selected as the process's primary agent with `ava --agent <name>`. See [subagents.md](subagents.md) for operational behavior, job controls, delivery, durability, and limits. AVA always provides two built-ins:

- `general`: inherits parent tool visibility except `task`, `job`, and `todowrite` are hidden.
- `explore`: read-only preset that exposes `read_file`, `list_directory`, `glob`, and `grep` while hiding mutation, shell, network, LSP, `task`, `job`, and `todowrite`.

Configured agents are Markdown files with YAML-like frontmatter. Global files are discovered regardless of project trust from:

```text
$XDG_CONFIG_HOME/ava/agents/*.md
$XDG_CONFIG_HOME/ava/agent/*.md
~/.agents/agents/*.md
~/.agents/agent/*.md
~/.claude/agents/*.md
~/.claude/agent/*.md
```

Project-local files are discovered only after `/trust project` from:

```text
$WORKSPACE/.ava/agents/*.md
$WORKSPACE/.ava/agent/*.md
$WORKSPACE/.agents/agents/*.md
$WORKSPACE/.agents/agent/*.md
$WORKSPACE/.claude/agents/*.md
$WORKSPACE/.claude/agent/*.md
```

Example:

```markdown
---
name: reviewer
description: Review a focused implementation change.
mode: all
tools: read-only
max_tool_iterations: 14
---
Inspect the requested files and return concise findings with file references.
```

`name` defaults to the file stem when omitted and must use letters, digits, `.`, `_`, or `-`. Names are capped at 128 bytes, cannot contain consecutive separators, and cannot end with a separator. `description` is required. `mode: subagent` (the default) is usable only by `task`, `mode: primary` is selectable only by `--agent`, and `mode: all` is available in both catalogs. `tools` accepts inherit/default forms or `read-only`, `read_only`, `readonly`, or `explore`; invalid presets make a definition non-primary-selectable (legacy task definitions retain inherited visibility). Optional `max_tool_iterations` must appear at most once and be an integer from 1 through 1000. It supplies the task-child default and can be overridden by the model's validated `task.max_tool_iterations`; without either value the child inherits the parent loop's configured limit. `hidden: true`, `yes`, or `1` keeps a task subagent out of the prompt's visible `available_subagents` list while preserving explicit lookup.

For task children, inherit starts from parent visibility and still removes `task`, `job`, and `todowrite`. For a selected primary, inherit leaves startup tool visibility unchanged, including those tools. A read-only primary narrows visibility to AVA's built-in `read_file`, `list_directory`, `glob`, and `grep` set by intersection with `--tools`, `--exclude-tools`, `--no-builtin-tools`, and `--no-tools`; it never grants permission or widens CLI visibility.

Trusted project definitions override same-name global definitions. Project-only definitions are unavailable while untrusted. The built-in `general` and `explore` names are reserved only in the task-subagent catalog, so a configured primary may use either name. Each file and catalog is bounded; selected unknown, malformed, unreadable, invalid, or unavailable definitions fail startup before a provider request.

## Project Trust

AVA stores project trust decisions outside the workspace in `$XDG_STATE_HOME/ava/project-trust.json`.
The file records normalized workspace paths and whether project-local resources are trusted.
Project `AGENTS.md`/`CLAUDE.md` context files still load without a trust decision, but these project-local resources are skipped until the workspace is trusted:

```text
$WORKSPACE/.ava/commands/
$WORKSPACE/.ava/command/
$WORKSPACE/.ava/agents/
$WORKSPACE/.ava/agent/
$WORKSPACE/.ava/skills/
$WORKSPACE/.agents/agents/
$WORKSPACE/.agents/agent/
$WORKSPACE/.agents/skills/
$WORKSPACE/.claude/agents/
$WORKSPACE/.claude/agent/
$WORKSPACE/.claude/skills/
$WORKSPACE/.ava/plugins/
$WORKSPACE/.ava/mcp.json
$WORKSPACE/.ava/lsp.json
$WORKSPACE/.ava/SYSTEM.md
$WORKSPACE/.ava/APPEND_SYSTEM.md
```

Use `/trust status` to inspect the current decision, `/trust project` to trust this workspace,
`/trust deny` to keep project resources skipped, and `/trust clear` to remove the explicit decision.
The TUI `/settings` view shows the same project trust state and routes trust actions through these backend commands.

# Thinking and Reasoning Modes

This guide describes AVA's current thinking/reasoning behavior from AVA's own
code and docs. It does not copy Pi implementation details.

## Vocabulary

AVA has two separate mode families:

- **Build/plan modes** are AVA runtime modes. They choose prompt text and
  backend permission posture for the whole session or turn.
- **Provider reasoning levels** are model/provider request controls. They ask a
  compatible provider to use a specific reasoning or thinking effort for future
  provider calls.

Do not treat these as aliases. Switching to plan mode does not enable provider
thinking, and choosing a reasoning level does not relax build/plan permissions.

## Build and plan modes

Start AVA in build or plan mode with `--mode build` or `--mode plan`; the TUI
also exposes `/mode` and a Tab fallback when no autocomplete, slash palette, or
path completion consumes Tab. The current user-facing summary lives in
[USAGE.md](usage.md#starting-ava) and [USAGE.md](usage.md#modes).

Current behavior:

- **Build mode** is the normal coding mode. File mutations still go through the
  permission system.
- **Plan mode** changes the selected prompt to "Plan before changing files" and
  denies source-code mutation through the backend permission policy. Planning
  Markdown may still be written when the path is Markdown and looks like docs,
  plan, or version material.
- Mode changes are persisted as session `mode_change` entries for audit/export
  and stats. A new or resumed process still starts from the CLI/runtime mode
  selected for that process.

## Provider reasoning levels

Reasoning controls are available only when the active model declares
`supports_reasoning` and non-empty `reasoning_levels`. Model metadata and
custom model configuration are documented in [CONFIG.md](configuration.md#models), and
the RPC command catalog is documented in
[rpc-protocol.md](../rpc-protocol.md#command-catalog).

Ways to change provider reasoning:

- In the TUI, **Shift+Tab** is the documented default `variant_cycle` binding.
  It cycles through the active model's declared `reasoning_levels` between
  turns, then clears the explicit selection so provider defaults apply again.
  During an active run AVA reports that reasoning can be changed between turns.
- In RPC, use `set_reasoning` with `reasoning_level` and optional provider-valid
  `reasoning_budget_tokens`/`reasoning_display`, or `clear_reasoning` to remove
  AVA's explicit reasoning option from future requests.
- `/models [query]` and the model selector expose each model's reasoning
  support, levels, format, and diagnostics.

Unsupported explicit reasoning choices fail before a provider request is built:
no reasoning support, no levels, an unknown level, or provider-invalid
budget/display fields return local errors instead of silently sending an invalid
request. Historical reasoning does not itself prevent a model switch; AVA clears
the explicit selection and projects history for the new target on later requests.

Current built-in support:

- **OpenAI Responses**: `gpt-5.5` declares `low`, `medium`, `high`, and `xhigh`.
  `gpt-5.6-sol`, `gpt-5.6-terra`, and `gpt-5.6-luna` also expose `max` plus a
  user-facing `minimal` alias mapped to provider effort `low` for API-key and
  ChatGPT OAuth compatibility. AVA sends `reasoning.effort=<level>` with summary
  `auto`. Budget and display fields are not accepted for this family.
- **Anthropic Messages**: built-in Claude Sonnet 4.5 exposes `enabled`. AVA maps
  this to native `thinking` with a budget, defaulting to a safe budget below the
  model output limit. RPC callers must provide a valid Anthropic budget when
  setting this directly; optional display values must be `summarized` or
  `omitted`.
- **DeepSeek**: built-ins expose `high` and `xhigh`; AVA maps them to
  `reasoning_effort=high|max` and parses `reasoning_content`, but intentionally
  does not replay prior DeepSeek reasoning content.
- **Kimi/Moonshot-compatible routes**: built-in Kimi and Moonshot thinking
  models expose `enabled` and use compatible chat-completions `thinking` fields.
  Kimi's built-in profile preserves compatible `reasoning_content` replay when
  metadata and quirks allow it.
- **OpenRouter**: the built-in OpenRouter model currently has reasoning controls
  disabled. AVA has compatible request/error coverage, but OpenRouter-native
  reasoning remains future provider-breadth work.
- **Gemini GenerateContent**: explicit AVA reasoning controls remain unsupported,
  but provider-emitted `thought:true` parts appear as reasoning, not answer text.
  Private `thoughtSignature` metadata replays only on matching follow-up parts
  for exact-compatible history; it is never exposed in public events or exports.
  If replay metadata exceeds 64 KiB or 1,024 parts, AVA drops it without failing
  the response and sends that history unsigned on the next request.

## `/thinking` and inline thinking visibility

`/thinking` is a display toggle, not a provider reasoning command. In the TUI it
hides or shows inline thinking/reasoning blocks already present in the transcript
and updates the status text. Outside the interactive TUI, the command returns an
explanation that thinking visibility is a TUI display toggle. The configurable
Pi-style action id `app.thinking.toggle` remains available but is unbound by
default. Ctrl+T opens the thinking-mode selector between turns, while the
reasoning-cycle shortcut remains Shift+Tab.

Hiding thinking blocks does not remove session entries, does not affect provider
replay, and does not change the active `reasoning_level`.

## Session persistence and replay

Reasoning-related session behavior is append-only and provider-aware:

- `session_start` records the selected mode, provider/model, capability flags,
  `reasoning_levels`, and `reasoning_format` when known.
- `reasoning_change` is appended when AVA's explicit reasoning selection changes
  or is cleared. Enabled entries include provider, model, format when known,
  level, and optional budget/display. Disabled entries keep provider/model and
  format when known, then set `enabled:false`.
- On session restore, AVA recovers only the latest valid `reasoning_change` that
  matches the restored provider/model after the latest session-start or
  model-change boundary. Model switching clears the in-memory selection.
- Provider-emitted visible or native thinking is saved as `reasoning_block`
  entries before the associated assistant message when there is text, a
  signature, or redacted provider data to preserve.
- `reasoning_block` entries can replay as native content only for an exact
  source provider/model/API match with matching reasoning metadata. When the
  target differs or provenance is missing, request-time projection preserves
  visible assistant text but omits incompatible native reasoning and
  provider-private content. Switching from Anthropic to another provider is
  therefore allowed; it does not send Anthropic thinking blocks to that provider.
  Projection does not rewrite persisted history, and malformed committed
  assistant-output records can still fail local replay validation.
- RPC `get_messages` sanitizes reasoning entries: visible non-redacted text may
  be returned, but raw signatures and opaque redacted-thinking payloads are
  replaced by status fields. Markdown/HTML exports report redaction and signature
  presence and include non-redacted text only. Portable JSONL is likewise
  sanitized and non-lossless: it rebuilds reasoning blocks from safe fields,
  omits provider-private replay metadata, and uses a neutral placeholder for
  redacted reasoning while remaining locally re-importable.
- `get_session_stats` counts reasoning changes and blocks, and
  `validate_session` checks durable model/reasoning entry shape.
- With `--no-session`, the same in-process entries exist for the current run, but
  no resumable JSONL history is written under the configured sessions directory.

## Live testing notes

Deterministic coverage lives in `ava_tests` for provider request/SSE parsing,
session validation, RPC commands, and TUI keybinding/rendering paths. Current
testing guidance is in [TESTING.md](../operations/testing.md).

Run live provider smokes only with intentional credentials and never record
secret values:

```sh
AVA_LIVE_PROVIDER_SMOKE=1 ctest --test-dir build -R provider_live_smoke --output-on-failure
AVA_LIVE_PROVIDER_SMOKE=1 sh scripts/live-model-dogfood.sh
AVA_LIVE_PROVIDER_SMOKE=1 sh scripts/live-coding-dogfood.sh
```

Useful reasoning checks include:

- `list_models`/`/models` shows `supports_reasoning`, `reasoning_levels`, and
  `reasoning_format` for the target model.
- RPC `set_reasoning` returns updated `get_state` fields or a local validation
  error before any prompt.
- A live prompt with a reasoning-capable provider emits `reasoning_start`,
  `reasoning_delta`, and `reasoning_end` only when that provider exposes visible
  reasoning. Providers may still reason internally without streaming visible
  blocks.
- TUI thinking visibility can be checked with `/thinking`; TUI reasoning
  selection can be checked with Ctrl+T between turns, and cycling with Shift+Tab
  when the terminal reports BackTab/Shift+Tab correctly. Gated tmux/PTY smoke coverage is referenced in
  [product/mvp-baseline.md](https://github.com/Artificial-Source/AVA/blob/develop/docs/product/mvp-baseline.md#testing-release-and-quality-bar).

## Pi parity and divergence

AVA tracks Pi-facing ergonomics where they fit AVA's safety boundaries:

- AVA accepts the Pi-style `app.thinking.toggle` action id for configurable
  thinking-block visibility, but leaves it unbound by default.
- AVA uses Ctrl+T for the staged thinking-mode selector and keeps Shift+Tab as
  the separate reasoning-cycle shortcut.
- AVA accepts Pi-style CLI `--thinking off|<level>` as a startup alias for the
  existing reasoning controls. `off` clears explicit reasoning; other values use
  the active model's `reasoning_levels` and provider/API-family validation.
- AVA rejects unsupported explicit reasoning levels and provider-invalid
  parameters locally rather than guessing. Model switches are allowed;
  incompatible historical native/private content is portably projected instead
  of making the switch fail.
- AVA persists provider-native reasoning blocks for safe replay and audit while
  sanitizing signatures/redacted payloads in user-facing RPC/export surfaces.

See [docs/goals/pi-mvp-parity/cli-commands-sessions-share-import.md](https://github.com/Artificial-Source/AVA/blob/develop/docs/goals/pi-mvp-parity/cli-commands-sessions-share-import.md)
for the CLI parity disposition matrix.

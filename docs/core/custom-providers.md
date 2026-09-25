# Custom providers

This is the authoritative guide for authoring **user-defined providers** in AVA.
Built-in vendor status lives in [`providers.md`](providers.md). Model field details
live under [configuration § models](configuration.md#models). Credential storage
lives under [configuration § auth](configuration.md#auth).

## Three files, three jobs

| File | Purpose | Secrets? |
| --- | --- | --- |
| `$XDG_CONFIG_HOME/ava/providers.json` (default `~/.config/ava/providers.json`) | Declares **runtime provider endpoints** (protocol, base URL, auth mode). Loaded once at process start into an immutable catalog. | No. Never put API keys here. |
| `$XDG_CONFIG_HOME/ava/models.json` | Declares **models** (ids, context, pricing, `api_family`, modalities). Additive overrides of the built-in catalog. | No. |
| `$XDG_CONFIG_HOME/ava/auth.json` | Stores **provider-scoped credentials** written by `/connect` / `ava connect`. | Yes. Owner-only. |

AVA does **not** use a single Pi-style merged settings file for this. Keep endpoint
definition, model metadata, and secrets behind separate validators.

## Startup load and restart policy

1. At process start, AVA builds one application-scoped `ProviderCatalog`.
2. If `providers.json` is **missing**, the catalog contains only built-ins (success).
3. If `providers.json` is **present**, it is validated all-or-nothing. Any schema,
   URL, ownership, permission, hard-link, symlink, size, duplicate-id, or built-in
   collision error fails catalog construction and the process does not start with a
   half-applied provider set.
4. Successful definitions become immutable descriptors and factories for the process
   lifetime. AVA does **not** re-read the file, hot-swap factories, or mutate the
   catalog in place.

`/reload models` reloads `models.json` only.
`/reload providers` and `/reload all` report that **provider definitions require a
process restart**. Edit `providers.json`, then restart AVA.

## File authority rules

When `providers.json` exists it must satisfy all of:

- regular file (not a directory, FIFO, device, or symlink — open uses `O_NOFOLLOW`)
- owned by the effective user
- exactly one hard link
- not group- or world-writable (mode bits `0022` must be clear)
- at most **256 KiB**
- opened once through a descriptor with `O_CLOEXEC | O_NONBLOCK` (and `O_NOFOLLOW`)

Recommended:

```sh
chmod 600 ~/.config/ava/providers.json
```

Do not place the file under a group-writable directory that untrusted users can
replace with a symlink or FIFO.

## Schema version 1

Root object only:

```json
{
  "version": 1,
  "providers": [ /* 0..32 entries */ ]
}
```

- `version` must be the integer `1`.
- `providers` must be an array with at most **32** entries.
- Unknown root members are rejected.
- Duplicate object keys are rejected (strict JSON).
- Maximum JSON nesting depth is **8**.
- Provider `id` values must be unique within the file and must **not** collide with
  any built-in provider id (including metadata-only ids such as `vercel`).

### Provider entry fields

| Field | Required | Default | Limits / rules |
| --- | --- | --- | --- |
| `id` | yes | — | 1–64 bytes; `[a-z0-9_-]` only |
| `display_name` | yes | — | 1–128 bytes; valid UTF-8; no control bytes |
| `protocol` | yes | — | one of `openai_chat_completions`, `openai_responses`, `anthropic_messages` |
| `base_url` | yes | — | see [URL rules](#url-and-path-rules); max 2048 bytes |
| `request_path` | no | protocol default (below) | absolute path; max 1024 bytes; see path rules |
| `auth` | no | `api_key` | `api_key` or `none` |
| `api_key_env` | no for `api_key`; **forbidden** for `none` | derived `<ID>_API_KEY` (uppercase, non-alnum → `_`) | shell env name: leading ASCII uppercase letter or underscore, then only uppercase letters, digits, or underscores; max 128 |
| `compatibility` | no | `{}` | object; only `include_stream_usage` (boolean) is allowed, and only for `openai_chat_completions` |

Protocol default request paths:

| Protocol | Default `request_path` |
| --- | --- |
| `openai_chat_completions` | `/v1/chat/completions` |
| `openai_responses` | `/v1/responses` |
| `anthropic_messages` | `/v1/messages` |

The runtime always uses the **canonical joined endpoint**
(`normalized base_url` + `request_path`). There is no separate mutable origin/path
join at request time and no redirect following on generic custom providers.

### Auth modes

| `auth` | Behavior |
| --- | --- |
| `api_key` | Credential required. Resolution order: **stored** provider-scoped API key in `auth.json`, then **only** the configured `api_key_env`. No other env names are tried. Requests send `Authorization: Bearer <token>` (OpenAI-family) or `x-api-key` (Anthropic Messages). |
| `none` | Explicit successful no-credential state. Skips stored/env lookup entirely. Omits Authorization and x-api-key. `/connect` reports that no credential is required and **does not write** `auth.json`. Never manufactures an empty token string. |

Custom providers never offer OAuth. Built-in OpenAI/Anthropic OAuth behavior is
unchanged and does not apply to user-defined ids.

### Intentionally unsupported

Custom providers do **not** support:

- arbitrary extra headers, query strings, or body patches
- OAuth / browser login flows
- plugins or request middleware hooks
- hot reload of endpoints or factories
- redirect following
- Codex / ChatGPT OAuth URL or header mutations on generic Responses adapters

## URL and path rules

`base_url`:

- scheme `https` for remote hosts, or `http` **only** for `localhost`, `127.0.0.1`, or `[::1]`
- no userinfo (`user:pass@`), query, or fragment
- no backslashes; no control bytes
- host may be DNS name, IPv4, or bracketed IPv6
- optional port 1–65535
- optional base path that itself must satisfy request-path rules
- trailing slashes are trimmed during canonicalization
- percent-encodings that smuggle `.`, `/`, `\`, controls, or nested `%` are rejected

`request_path`:

- must start with `/`
- no `?`, `#`, `\`, empty segments, `.` or `..` segments
- no ambiguous percent-encoding as above
- valid UTF-8

## Models must match the protocol

Models stay in `models.json`. For a custom provider model:

1. Set `"provider"` to the same `id` as in `providers.json`.
2. Set `"api_family"` to match the provider protocol:

| Provider `protocol` | Required model `api_family` |
| --- | --- |
| `openai_chat_completions` | `openai_chat_completions` |
| `openai_responses` | `openai_responses` |
| `anthropic_messages` | `anthropic_messages` |

A mismatch disables the model row with a fixed diagnostic (no secret leakage).
Set `context_window_tokens` on new custom models so context percentage and
compaction can work. See [configuration § models](configuration.md#models).

## Commands and surfaces

| Surface | Behavior with custom providers |
| --- | --- |
| `/providers` | Lists built-ins and user-defined providers; credential source/status without secret values; endpoint and quirks. |
| `/models`, `/model`, Ctrl+L | Lists models including custom ones; diagnostics for missing metadata and api_family/protocol mismatches. |
| `/connect`, `/login`, `ava connect <id>` | API-key providers accept/store a provider-scoped key and show the configured env name. `auth:none` reports no credential required and does not write `auth.json`. |
| TUI, print, RPC, ACP, compaction, titles, subagents | Use the same pinned process catalog and credential policy. |

## Copy-paste examples

Placeholders only — never commit real secrets.

### 1) Remote OpenAI Chat Completions + matching model

`~/.config/ava/providers.json`:

```json
{
  "version": 1,
  "providers": [
    {
      "id": "my-chat",
      "display_name": "My Chat Gateway",
      "protocol": "openai_chat_completions",
      "base_url": "https://llm.example.com",
      "request_path": "/v1/chat/completions",
      "auth": "api_key",
      "api_key_env": "MY_CHAT_API_KEY",
      "compatibility": {
        "include_stream_usage": true
      }
    }
  ]
}
```

```sh
chmod 600 ~/.config/ava/providers.json
export MY_CHAT_API_KEY='replace-me'
# or: ava connect my-chat --api-key
```

`~/.config/ava/models.json` (additive fragment):

```json
{
  "version": 1,
  "models": [
    {
      "provider": "my-chat",
      "id": "gateway-large",
      "display_name": "Gateway Large",
      "family": "gateway",
      "api_family": "openai_chat_completions",
      "context_window_tokens": 128000,
      "max_output_tokens": 8192,
      "supports_tools": true,
      "supports_streaming": true,
      "supports_reasoning": false,
      "input_modalities": ["text"],
      "output_modalities": ["text"]
    }
  ]
}
```

Restart AVA after editing `providers.json`. Then `/models my-chat/gateway-large`.

### 2) OpenAI Responses provider

```json
{
  "version": 1,
  "providers": [
    {
      "id": "my-responses",
      "display_name": "My Responses Endpoint",
      "protocol": "openai_responses",
      "base_url": "https://llm.example.com",
      "request_path": "/v1/responses",
      "auth": "api_key",
      "api_key_env": "MY_RESPONSES_API_KEY"
    }
  ]
}
```

Model `api_family` must be `openai_responses`. Generic Responses adapters always
send `max_output_tokens` when configured and **never** apply OpenAI Codex OAuth
URL/header mutations.

### 3) Anthropic Messages provider

```json
{
  "version": 1,
  "providers": [
    {
      "id": "my-anthropic",
      "display_name": "My Anthropic-Compatible",
      "protocol": "anthropic_messages",
      "base_url": "https://anthropic.example.com",
      "auth": "api_key",
      "api_key_env": "MY_ANTHROPIC_API_KEY"
    }
  ]
}
```

Default path is `/v1/messages`. Model `api_family` must be `anthropic_messages`.
Generic adapters send standard `anthropic-version` and `x-api-key` only (no OAuth
beta Authorization swap).

### 4) Local loopback server with `auth: none`

```json
{
  "version": 1,
  "providers": [
    {
      "id": "local-dev",
      "display_name": "Local Dev Server",
      "protocol": "openai_chat_completions",
      "base_url": "http://127.0.0.1:8080",
      "request_path": "/v1/chat/completions",
      "auth": "none"
    }
  ]
}
```

HTTP is allowed only for loopback hosts. No credential is required; `/connect`
will not write `auth.json`.

## Troubleshooting (sanitized)

| Symptom | Likely cause |
| --- | --- |
| AVA fails to start mentioning `providers config` | Present file failed schema, size, ownership, mode, link-count, symlink, or URL validation. Fix the file or remove it. |
| `must be owned by the effective user` / `must not be hard-linked` / `must not be group- or world-writable` | File authority rules. Use a private regular file: `chmod 600`, single link, your uid. |
| `user provider id collides with a reserved provider id` | Choose an id that is not a built-in (e.g. not `openai`, `xai`, `groq`, …). |
| Model row disabled: api_family / protocol mismatch | Align `models.json` `api_family` with `providers.json` `protocol` (table above). |
| Credential missing for custom provider | Store via `ava connect <id>` / `/connect`, or export exactly the configured `api_key_env`. |
| `/reload providers` says restart required | Expected. Restart the process after editing `providers.json`. |
| Want custom headers / OAuth for a gateway | Unsupported for user-defined providers. Use a local proxy that presents one of the three protocols, or request a first-class built-in. |

Diagnostics never print API keys, raw provider response bodies, or free-form
provider error text into public surfaces.

## Related

- [providers.md](providers.md) — built-in matrix and live-smoke status
- [configuration.md](configuration.md) — auth, models, reload surfaces
- [environment-variables.md](environment-variables.md) — credential and base-URL envs
- Source: `src/ava/config/provider_config*.cpp`, `src/ava/provider/catalog.cpp`,
  `src/ava/config/builtin_generic_providers.cpp`

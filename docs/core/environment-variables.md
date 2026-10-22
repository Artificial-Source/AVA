# Environment variables

This is the reference for environment input read by AVA and its repository scripts. Configuration files and command-line options are documented in [CONFIG.md](configuration.md); provider behavior is summarized in [providers.md](https://github.com/Artificial-Source/AVA/blob/develop/docs/core/providers.md). Values described as secrets must not be put in project files, shell history, logs, or command lines.

## Resolution and secret handling

AVA reads its process environment at the point the relevant feature is used. A stored provider credential in `$XDG_CONFIG_HOME/ava/auth.json` takes precedence over an environment credential. For Anthropic, when no stored credential exists, `ANTHROPIC_OAUTH_TOKEN`, then `ANTHROPIC_AUTH_TOKEN`, then `ANTHROPIC_API_KEY` wins. Nonempty provider base-URL overrides replace their provider default. `NO_COLOR` wins over `AVA_TUI_THEME`; the full theme ordering is in [CONFIG.md](configuration.md#tui-display).

Environment credentials are deliberately not persisted merely by using AVA. `ava connect <provider> --api-key-env NAME` is the explicit operation that reads an arbitrary named variable and stores it owner-only. AVA status and diagnostics report credential source/status, never its value; provider diagnostics also redact raw provider bodies. See [diagnostics.md](../operations/diagnostics.md).

## Stable runtime variables

| Variable | Meaning |
| --- | --- |
| `AVA_SESSION_TITLES` | `on` defers to `session-titles.json`; `off` disables automatic session-title generation. Other values are rejected. |
| `AVA_TUI_THEME` | Process-only built-in theme override: `dark`, `light`, or `plain`. Unknown/empty values fall through to automatic detection. |
| `AVA_TUI_TMUX_HYPERLINKS` | Set to `1` to opt into OSC 8 links in tmux after tmux itself has been configured to forward hyperlinks. It affects only the tmux fallback. |
| `AVA_DISABLE_BROWSER_OPEN` | Any presence prevents AVA from launching a browser for OAuth; AVA still prints the URL. |
| `AVA_CLIPBOARD_IMAGE_FILE` | Absolute image file to import for Ctrl+V instead of probing the Linux clipboard. Intended primarily for deterministic use; supported image types are the same as clipboard paste. |
| `AVA_EXTERNAL_EDITOR_FILE` | Internal, short-lived path supplied by AVA only while it invokes `VISUAL`/`EDITOR`; editor commands use it instead of a positional filename. Do not pre-set it as configuration. |

### Provider credentials and endpoints

These values are credentials; export them only into the AVA process that needs them.

| Provider | Credential variable(s) | Base URL variable |
| --- | --- | --- |
| OpenAI | `OPENAI_API_KEY` | None (the native runtime uses its configured default) |
| Anthropic | `ANTHROPIC_OAUTH_TOKEN`, `ANTHROPIC_AUTH_TOKEN`, or `ANTHROPIC_API_KEY` | `ANTHROPIC_BASE_URL` |
| DeepSeek | `DEEPSEEK_API_KEY` | `DEEPSEEK_BASE_URL` |
| Gemini | `GEMINI_API_KEY` | `GEMINI_BASE_URL` |
| Kimi | `KIMI_API_KEY` | `KIMI_BASE_URL` |
| Moonshot | `MOONSHOT_API_KEY` | `MOONSHOT_BASE_URL` |
| OpenRouter | `OPENROUTER_API_KEY` | `OPENROUTER_BASE_URL` |
| Z.AI Coding Plan (Global) | `ZAI_API_KEY` | `ZAI_BASE_URL` |
| Z.AI Coding Plan (China) | `ZAI_CODING_CN_API_KEY` | `ZAI_CODING_CN_BASE_URL` |
| xAI | `XAI_API_KEY` | `XAI_BASE_URL` |
| Groq | `GROQ_API_KEY` | `GROQ_BASE_URL` |
| Cerebras | `CEREBRAS_API_KEY` | `CEREBRAS_BASE_URL` |
| Together | `TOGETHER_API_KEY` | `TOGETHER_BASE_URL` |
| Fireworks | `FIREWORKS_API_KEY` | `FIREWORKS_BASE_URL` |
| Mistral | `MISTRAL_API_KEY` | `MISTRAL_BASE_URL` |
| User-defined (`providers.json`) | Exactly the entry's `api_key_env` (default `<ID>_API_KEY`) | Endpoint is fixed in `providers.json` (no separate base-URL env) |

Defaults, OAuth details, custom-provider rules, and implemented-provider boundaries are in [providers.md](providers.md) and [custom-providers.md](custom-providers.md).

## Standard desktop and shell inputs

| Variable | Use |
| --- | --- |
| `HOME` | Trusted-home/config compatibility and built-in LSP user-bin discovery. It must be usable as an absolute home where required. |
| `XDG_CONFIG_HOME`, `XDG_STATE_HOME`, `XDG_DATA_HOME`, `XDG_CACHE_HOME`, `XDG_RUNTIME_DIR` | XDG roots for AVA state/config and normal platform integration. AVA's exact paths are documented in [CONFIG.md](configuration.md). |
| `PATH` | Command discovery and browser lookup in AVA. It is not blindly forwarded to sealed commands or LSP servers. |
| `PWD` | Preserves the logical startup working-directory identity where safe. |
| `VISUAL`, `EDITOR` | External-editor command, tried in that order, for the TUI editor shortcut. |
| `BROWSER` | Browser command preference for OAuth URL opening. |
| `NO_COLOR` | Presence disables color and overrides all theme selection. |
| `COLORFGBG` | Terminal background hint used only after explicit theme settings. |
| `TERM`, `TERM_PROGRAM`, `TERMINAL_EMULATOR`, `TMUX`, `TMUX_PANE`, `KITTY_WINDOW_ID`, `GHOSTTY_RESOURCES_DIR`, `ITERM_SESSION_ID`, `WEZTERM_EXECUTABLE`, `WEZTERM_PANE`, `WT_SESSION`, `WARP_SESSION_ID`, `WARP_TERMINAL_SESSION_UUID`, `TERMUX_VERSION` | Terminal/image/hyperlink capability detection. These are hints, not security authority. |
| `WAYLAND_DISPLAY` | Selects Wayland clipboard probing; AVA otherwise can use X11 clipboard helpers. |

## Developer debugging controls

| Variable | Meaning |
| --- | --- |
| `AVA_DEBUG_OUTPUT` | Set to `1` to opt an ordinary process into libcwd developer diagnostics in libcwd-enabled builds; libcwd output is off by default even there. `AVA_NO_DEBUG_OUTPUT` wins when both are set. |
| `AVA_NO_DEBUG_OUTPUT` | Hard suppression of libcwd debug initialization; wins over `AVA_DEBUG_OUTPUT`. Only a validated private test output stream (nonempty `AVA_DEBUG_OUTPUT_DIR` under a test identity) overrides it. Used by tests and useful to silence developer debug output. |
| `AVA_DEBUG_OUTPUT_DIR` | Absolute directory for libcwd debug output. Empty does not opt in; a relative path is rejected. |
| `LIBCWD_NO_STARTUP_MSGS` | libcwd compatibility switch used to suppress its startup message. |
| `LIBCWD_RCFILE_NAME`, `LIBCWD_RCFILE_OVERRIDE_NAME` | libcwd configuration inputs, not AVA product settings. |

`AVA_DEBUG_MAXLEN` is a **CMake cache variable**, not an environment variable; see [build configuration](https://github.com/Artificial-Source/AVA/blob/develop/docs/operations/build-configuration.md).

## Child-process inheritance

Child programs do **not** receive AVA's ambient environment as a product contract.

* Curl HTTP children use the exact `ava-curl-v1` profile captured when application process authority is created. They receive a fixed trusted `PATH`, `LANG=C.UTF-8`, `LC_ALL=C.UTF-8`, `PWD=/`, and only the proxy/CA variables `http_proxy`, `https_proxy`, `ftp_proxy`, `all_proxy`, `no_proxy`, their uppercase forms, `CURL_CA_BUNDLE`, `SSL_CERT_FILE`, and `SSL_CERT_DIR` when those values were present at capture. Later parent mutations are ignored. Provider/cloud credentials, loader variables, askpass/agent sockets, arbitrary `AVA_*` values, and all other ambient variables are excluded.
* Linux clipboard helpers use the exact `ava-clipboard-desktop-v1` profile captured with application process authority. They receive the fixed trusted `PATH` and only captured `HOME`, `USER`, `LOGNAME`, `TMPDIR`, `TMP`, `TEMP`, `LANG`, `LANGUAGE`, `LC_ALL`, other `LC_*`, `XDG_RUNTIME_DIR`, `DISPLAY`, `WAYLAND_DISPLAY`, `XAUTHORITY`, and `DBUS_SESSION_BUS_ADDRESS` values that were present. Later parent mutations are ignored. Proxy/CA values, provider or cloud credentials, loader variables, askpass/agent sockets, and arbitrary `AVA_*` values are excluded. `AVA_CLIPBOARD_IMAGE_FILE` and `TERMUX_VERSION` are parent-only selection inputs and are never forwarded; test scenarios are likewise parent-owned and never enter the helper environment.
* Sealed `bash` execution receives AVA-created private HOME/XDG/TMP roots, a bounded PATH, and a constructed environment; provider credentials and arbitrary `AVA_*` values are excluded.
* LSP servers receive only `HOME`, `USER`, `LOGNAME`, `TMPDIR`/`TMP`/`TEMP`, `LANG`, `LANGUAGE`, `LC_ALL`, other `LC_*`, `XDG_CONFIG_HOME`, `XDG_CACHE_HOME`, `XDG_DATA_HOME`, `XDG_STATE_HOME`, `TERM`, and `COLORTERM`, plus a fixed trusted `PATH`. In particular all `AVA_*`, provider/cloud/API/token/secret variables, and arbitrary inherited values are not forwarded.
* MCP children use the exact `ava-mcp-explicit-v1` profile. They receive process-computed `PWD`, the server's explicit config `env` pairs (at most 64), and the trusted `PATH` when config omits `PATH`. Nothing is inherited from the host: not `HOME`, provider/cloud credentials, proxy/CA variables, or arbitrary `AVA_*` values. Session, global, and project launches share this profile. Config JSON is unchanged; operators must set required values such as `HOME` or API credentials in the server's explicit `env`. See [mcp.md](../extensions/mcp.md).
* Plugin children use the exact `ava-plugin-minimal-v1` profile: only `PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin`, `LANG=C.UTF-8`, `LC_ALL=C.UTF-8`, and `PWD` set to the logical plugin working directory. They do not inherit `HOME`, user `PATH` additions, credentials, proxies, extra locale variables, or arbitrary `AVA_*` values; the manifest has no explicit environment field. This closed environment is not an OS filesystem sandbox. See [plugin-system.md](../extensions/plugin-system.md#plugin-authoring-guide).

## Supported maintainer and script inputs

These variables are supported by the named repository scripts; they are not AVA runtime configuration. Prefer the script's command-line option where one exists.

| Variable | Script and meaning |
| --- | --- |
| `AVA_CMAKE_COMMAND` | `scripts/build.sh`: CMake executable/wrapper; defaults to `cmake`. Primarily a runner test seam. |
| `AVA_CTEST_COMMAND` | `scripts/run-tests.sh`: CTest executable/wrapper; defaults to `ctest`. Primarily a runner test seam. |
| `AVA_TEST_TIMING_DIR` | Python tmux smoke harness: absolute directory for opt-in per-invocation JSONL timing traces. Empty or unset disables tracing; a relative path is rejected. The harness writes directly to private mode-0600 files and does not send timing records through AVA, tmux panes, stdout, or stderr. See [testing](../operations/testing.md#per-test-python-harness-timing-traces). |
| `AVA_LIVE_PROVIDER_SMOKE` | Explicit opt-in gate for credentialed live-provider CTests, dogfood scripts, and `live-provider-matrix.sh`; use `1`. No live provider runs by default. |
| `AVA_EXE` | `live-model-dogfood.sh`, `live-coding-dogfood.sh`, and `live-provider-matrix.sh`: AVA executable path. The dogfood scripts default to `./build/ava`; the matrix derives it from its build directory unless set. |
| `AVA_BUILD_DIR` | `live-provider-matrix.sh`: configured build tree, default `build`; used to derive executables for matrix targets. |
| `AVA_TESTS_EXE` | `live-provider-matrix.sh`: `ava_tests` executable override for the `provider-live-smoke` target; otherwise derived from `AVA_BUILD_DIR`. |
| `AVA_LIVE_DOGFOOD_ROOT` | `live-model-dogfood.sh`: absolute existing private parent in which the script allocates an unpredictable evidence child. The parent must be current-euid-owned, non-symlink, and exact mode 0700; `/`, `HOME`, the checkout, and checkout descendants are rejected. The script never removes the parent or pre-existing paths beneath it. |
| `AVA_LIVE_DOGFOOD_KEEP` | Any nonempty value retains and reports the invocation-created model-dogfood evidence child; otherwise cleanup removes only that child. |
| `AVA_LIVE_CODING_DOGFOOD_ROOT` | `live-coding-dogfood.sh`: private-parent override with the same validation and child-allocation semantics as `AVA_LIVE_DOGFOOD_ROOT`. |
| `AVA_LIVE_CODING_DOGFOOD_KEEP` | Any nonempty value retains and reports the invocation-created coding-dogfood evidence child; otherwise cleanup removes only that child. |
| `AVA_LIVE_PROVIDER_MATRIX_TARGET` | `live-provider-matrix.sh`: `provider-live-smoke` (default), `model-dogfood`, or `coding-dogfood`. The older `AVA_LIVE_MATRIX_TARGET` is a compatibility fallback only when this variable is unset. |
| `AVA_LIVE_PROVIDER_MATRIX_SUMMARY` | `live-provider-matrix.sh`: aggregate TSV path, defaulting beneath `${TMPDIR:-/tmp}`; a relative value resolves from the caller's starting directory. The script creates or truncates this file. |
| `AVA_LIVE_PROVIDER_MATRIX_KEEP` | Any nonempty value retains the matrix temporary root and child logs and propagates the matching dogfood keep control. Dogfood matrix cases receive distinct private parents beneath that invocation-owned root, so concurrent matrix invocations do not share evidence children. |
| `AVA_LIVE_<PROVIDER>_MODEL` | Optional model override used by live selection/matrix code: `<PROVIDER>` is `OPENAI`, `ANTHROPIC`, `DEEPSEEK`, `GEMINI`, `KIMI`, `MOONSHOT`, or `OPENROUTER`. Defaults and result classification are in [TESTING.md](../operations/testing.md#provider-live-smoke-matrix). |

`AVA_EXE`, dogfood roots, and keep flags affect evidence execution/storage only; they do not enable the credential gate. Live runs can consume credentials and make paid/network provider calls, so opt in deliberately and review retained logs as private material.

## Optional smoke gates

These are stable opt-in test controls, not normal AVA settings:

| Variable | Opt-in coverage |
| --- | --- |
| `AVA_TUI_TMUX_SMOKE` | Isolated tmux TUI scenarios. |
| `AVA_TUI_OSC8_SMOKE` | Direct PTY OSC 8 hyperlink smoke. |
| `AVA_TUI_KITTY_IMAGE_SMOKE`, `AVA_TUI_ITERM2_IMAGE_SMOKE` | Direct PTY inline-image protocol smokes. |
| `AVA_TUI_TERMINAL_LIFECYCLE_SMOKE` | Direct PTY terminal lifecycle smoke. |
| `AVA_LSP_REAL_CLANGD_SMOKE` | Offline smoke against an already-installed safe `clangd`; never installs/downloads it. |

See [TESTING.md](../operations/testing.md) for exact commands, prerequisites, skip-77 behavior, and release classification. Optional ACP SDK/acpx interop is selected by CMake options rather than runtime environment variables.

## Implementation-only test namespaces

The test harnesses also create private coordination variables. They are deliberately **not** supported user or maintainer controls and should not be copied into shell profiles or external automation:

- `AVA_PACKAGE_TEST_*` and `AVA_PACKAGE_CLEANUP_CLOSE_PIPES` coordinate package fixtures, snapshots, mutation markers, and process-cleanup regressions.
- `AVA_PARALLEL_*` is internal state owned by `parallel-runner-common.sh` for its build-tree lock, child process group, and signal escalation. Setting these externally can break fail-closed cleanup.
- ACP/Zed fixture variables such as `AVA_ACP_STDERR`, `AVA_ACP_WRAPPER_PROOF`, `AVA_ACP_SUBPROCESS_PARENT_SECRET`, and `AVA_ZED_DOGFOOD_PHASE_ROOT` are harness-to-child channels, not ACP configuration.
- Fixture/canary names such as `AVA_*_FAKE_KEY_NOT_A_SECRET`, `AVA_*_SECRET_SENTINEL`, `AVA_LSP_TEST_SECRET`, and one-off marker/gate variables exist only to prove isolation or non-disclosure. Their exact inventory may change with tests.

CTest sets controls such as `AVA_SESSION_TITLES=off`, `AVA_NO_DEBUG_OUTPUT=1`, and fake executable/root definitions for deterministic cases. Treat all undocumented `AVA_*` values observed only in fixtures as implementation detail unless this page or the owning current test guide explicitly declares them supported.

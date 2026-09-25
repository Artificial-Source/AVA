# Terminal Setup And Troubleshooting

AVA's interactive TUI is a native wide-character ncurses (`ncursesw`) application. It relies on the terminal's `TERM`/terminfo entry for screen mode, keyboard decoding, mouse events, resize handling, colors, and Unicode cell widths, then emits a small set of terminal extension sequences for enhanced keys, links, clipboard copy, bracketed paste, and inline image previews.

Use this guide when the TUI fails to start, keys or mouse events behave differently across terminals, image previews fall back to text, a clipboard copy request is sent but the OS clipboard is unchanged, or remote/tmux/screen sessions lose terminal features.

Interactive AVA requires a terminal on both stdin and stdout. Piped stdin or redirected stdout without `--print`, `--rpc`, or `--acp` exits before the TUI starts. Keyboard TUI coverage is not screen-reader certification.

For general command usage see [USAGE.md](../core/usage.md); for persisted display/theme files see [CONFIG.md](../core/configuration.md); for terminal smoke coverage see [TESTING.md](testing.md).

## Quick Health Check

Run these in the same shell where you plan to start `ava`:

```sh
printf 'TERM=%s\n' "$TERM"
printf 'LANG=%s LC_ALL=%s LC_CTYPE=%s\n' "${LANG-}" "${LC_ALL-}" "${LC_CTYPE-}"
infocmp "$TERM" >/dev/null
locale
```

Healthy defaults are usually:

- A real terminal entry such as `xterm-256color`, `xterm-kitty`, `wezterm`, `tmux-256color`, or `screen-256color`; not `dumb`, empty, or an outer-terminal name forced inside a multiplexer.
- A UTF-8 locale (`LANG`, `LC_ALL`, or `LC_CTYPE` contains `UTF-8`/`UTF8`). AVA calls `setlocale(LC_ALL, "")` before entering ncurses; a non-UTF-8 locale can break wide-character rendering.
- A resolvable terminfo entry on the machine running `ava`; this matters most over SSH and in containers.
- Runtime and build availability for wide-character ncurses. AVA requires `ncursesw`; a narrow ncurses build is not supported.

If `infocmp "$TERM"` fails, fix the terminfo entry before debugging AVA. If AVA reports `failed to initialize ncurses screen`, first check `TERM`, terminfo, and whether stdin/stdout are real TTYs.

## TERM, terminfo, And ncursesw

`TERM` names the terminal description that ncurses loads. It should describe the terminal layer that AVA is directly running inside:

| Situation | Typical `TERM` | Notes |
| --- | --- | --- |
| Direct xterm-like terminal | `xterm-256color` | Conservative fallback when no emulator-specific entry exists. |
| Kitty direct session | `xterm-kitty` or `xterm-256color` | `xterm-kitty` needs terminfo installed on the host running AVA. |
| WezTerm direct session | `wezterm` or `xterm-256color` | Use the emulator-provided terminfo when available. |
| tmux pane | `tmux-256color` or `screen-256color` | Do not force the outer terminal's `TERM` inside tmux. |
| GNU screen pane | `screen-256color` | AVA treats screen as a conservative text-only environment for images/links. |
| SSH/container | Whatever exists remotely | Copy/install terminfo or use a conservative fallback. |

Troubleshooting checklist:

1. Make sure the package that provides `ncursesw` runtime data is installed on the host running AVA.
2. Verify `infocmp "$TERM"` succeeds in that host/container/SSH session.
3. Prefer fixing terminfo over overriding `TERM`. A wrong-but-present `TERM` can make keys, mouse, resize, and alternate-screen cleanup worse.
4. Avoid `TERM=dumb` for the interactive TUI. Use headless print/RPC modes when running without a real terminal.

## tmux And screen

AVA works inside tmux and screen, but multiplexers intentionally hide or transform terminal capabilities.

### tmux

- Inside tmux, use `tmux-256color` when the terminfo entry exists; otherwise use `screen-256color`.
- Do not set `TERM=xterm-kitty`, `TERM=wezterm`, or another outer-terminal name inside tmux. tmux is the terminal AVA sees.
- Mouse selection, wheel scrolling, and modal row clicks depend on tmux forwarding mouse events to the pane. If mouse actions do nothing, check tmux mouse support, for example `set -g mouse on` in tmux configuration.
- OSC 52 clipboard copy can be filtered by tmux. If `/copy` reports that its request was sent but the system clipboard is unchanged, check tmux's clipboard settings such as `set-clipboard` and the outer terminal's OSC 52 policy.
- AVA currently disables inline image protocols under tmux and shows textual image metadata instead. Use a direct Kitty/Ghostty/WezTerm/Warp/iTerm2 session when inline previews are required.
- OSC 8 hyperlinks are also conservative under tmux in AVA's current detection path. Expect visible URL fallback/text unless running in a direct hyperlink-capable terminal.

### GNU screen

- Use `screen-256color` if available.
- AVA treats screen as text-only for image previews and hyperlinks.
- Screen may also filter OSC 52, bracketed paste, enhanced keys, and mouse events depending on configuration and version. If a feature works outside screen but not inside, assume screen is the limiting layer first.

## Keyboard Protocols

AVA requests the Kitty keyboard protocol on startup and falls back to xterm `modifyOtherKeys` when Kitty-style negotiation is not available. This improves detection for keys that legacy terminals often collapse, such as `Ctrl+Space`, `Ctrl+/`, `Ctrl+digit`, `Shift+Ctrl+P`, and modified arrow/delete combinations.

Direct environment-identified Alacritty uses a best-effort asynchronous strict DA2 version check: versions through 0.14.0 (packed through 2401) request conservative Kitty flags 5, while 2402 and later request flags 7. AVA skips DA2 probing through tmux/screen and other mux paths, places a finite bound on the asynchronous reply, and never blocks startup for it.

Expected behavior:

- Plain printable text should insert normally.
- Common navigation keys should work through ncurses/terminfo.
- Enhanced modified keys work best in direct Kitty-compatible terminals and recent xterm-like terminals.
- tmux/screen may need newer versions or explicit configuration before they pass enhanced key reports through unchanged.

If a key inserts stray text or does nothing:

1. Try the same key outside tmux/screen.
2. Check that `TERM` is correct and terminfo exists.
3. Use `/hotkeys` or `/keybindings` to see the effective semantic action names.
4. Prefer a custom keybinding in `$XDG_CONFIG_HOME/ava/keybinds.json` when your terminal cannot report the desired chord reliably.

## Suspend, External Editor, And Exit Cleanup

Persist cursor shape with `/cursor <default|block|underline|bar> [blink|steady]`. AVA emits DECSCUSR only while a non-default shape/blink policy is forced, resets it before suspend or external-editor handoff, reapplies it after return, and restores the terminal default on exit.

Ctrl+Z (suspend) and Ctrl+G (external editor) temporarily return the TTY to shell mode. After `def_prog_mode`/`endwin`, AVA balances and disables the protocols it owns—Kitty keyboard stack entry, negotiated `modifyOtherKeys`, bracketed paste, and mouse—so the shell or `$VISUAL`/`$EDITOR` does not inherit a leaked Kitty stack or sticky paste/mouse modes. After `reset_prog_mode`, AVA refreshes geometry from `TIOCGWINSZ` (including resizes that happened while SIGTSTP-stopped), then re-arms paste, mouse, and the already-negotiated keyboard mode exactly once. Resume never re-probes OSC 11 and never grows the Kitty keyboard stack.

On final TUI exit (normal quit, SIGTERM teardown, or partial enter failure), AVA again balances owned protocols, discards pending curses then kernel input nonblockingly (`flushinp` then `tcflush(TCIFLUSH)`), restores termios, shows the cursor, and leaves the alternate screen. Input flush does not sleep, drain output, or throw.

## Inline Images

Use `/attach <path>` or Ctrl+V clipboard image import to queue PNG, JPEG, WebP, or GIF attachments. AVA displays a pending attachment row with MIME type, byte count, and preview mode. The next normal prompt sends the image to image-capable providers.

Persist preview controls in `$XDG_CONFIG_HOME/ava/display.json`:

```text
/images on|off|reset
/image-width <8..160>|reset
```

Defaults when keys are absent are `show_images=true` and `image_width_cells=60`. Width is clamped again to the current content viewport. When `show_images` is `false`, AVA keeps safe textual attachment metadata, does not load preview bytes, and emits no Kitty/iTerm2 graphics protocol bytes.

AVA's preview capability is environment-detected:

| Terminal environment | AVA preview behavior |
| --- | --- |
| Kitty (`KITTY_WINDOW_ID` or `TERM_PROGRAM=kitty`) | Kitty graphics protocol. |
| Ghostty, WezTerm, Warp | Kitty-compatible graphics protocol when their identifying environment variables are present. |
| iTerm2 (`ITERM_SESSION_ID` or `TERM_PROGRAM=iterm.app`) | iTerm2 inline image protocol. |
| tmux or screen | Text-only metadata; image protocols disabled. |
| VS Code, Windows Terminal, Alacritty, unknown terminals | Text-only image metadata. |
| `NO_COLOR=1` or `AVA_TUI_THEME=plain` | Text-only metadata; graphics overlays are suppressed with other styling. |
| `show_images=false` in `display.json` | Text-only metadata; no preview-byte load and no graphics emission. |

Notes:

- Direct terminal sessions are the intended path for inline previews.
- iTerm2 protocol support is runtime/protocol support only. macOS packaging docs are deferred; this guide does not add a macOS install path.
- If an imported image renders as text metadata only, AVA will still attach it; only the terminal-side preview is missing or suppressed.
- If Ctrl+V does not import an image on Linux, install or configure `wl-paste` for Wayland or `xclip` for X11, or use `/attach <path>` directly. `AVA_CLIPBOARD_IMAGE_FILE=/absolute/path.png` is reserved for deterministic smoke tests.

References: [Kitty keyboard protocol](https://sw.kovidgoyal.net/kitty/keyboard-protocol/), [Kitty graphics protocol](https://sw.kovidgoyal.net/kitty/graphics-protocol/), and [iTerm2 inline images](https://iterm2.com/documentation-images.html).

## OSC 8 Hyperlinks

AVA can render Markdown links as OSC 8 terminal hyperlinks when the active terminal is detected as hyperlink-capable and styling is enabled. In plain/text-only cases, links remain visible as normal text/Markdown URL fallback.

Expected support:

- Direct Kitty-compatible and iTerm2-capable sessions usually support OSC 8.
- VS Code, Windows Terminal, and Alacritty are treated as hyperlink-capable text-image terminals.
- tmux and screen are conservative in AVA's current detection path; expect text fallback unless support is explicitly added and verified.
- `NO_COLOR=1` and `AVA_TUI_THEME=plain` disable OSC 8 emission.

If a link is visible but not clickable, check the terminal's hyperlink setting and whether a multiplexer is filtering OSC sequences. AVA still preserves the link target in visible text where hyperlink mode is not active.

Reference: [iTerm2 OSC 8 documentation](https://iterm2.com/documentation-escape-codes.html).

## OSC 52 Clipboard Copy

The TUI plain `/copy` command and F5 copy the latest assistant response through OSC 52 (`OSC 52 ; c ; <base64> ST`) to the terminal clipboard path. AVA uses this for latest assistant text, selected public user-turn text (`/copy user`), tool details, diffs, and permission details. Payloads larger than 64 KiB are rejected without silent truncation. Under tmux AVA uses a bounded passthrough-safe form. AVA reports local construction or terminal-write failures, and labels a successful write only as a request sent because downstream terminal, SSH, or multiplexer delivery is unknowable. Double-click selects a word, triple-click selects a rendered line, drag selects a range, and navigation shows a transient scrollbar.

AVA rejects empty clipboard text and any payload larger than 64 KiB (65,536 raw bytes) without emitting a partial sequence or spawning an external clipboard helper. Oversized copies report the existing clipboard failure status.

Troubleshooting:

- If AVA says the copy request was sent but the OS clipboard did not change, your terminal, SSH client, tmux, or screen likely blocked OSC 52.
- Check terminal security settings for clipboard writes from shell applications.
- In tmux, check clipboard forwarding/settings such as `set-clipboard`; also confirm the outer terminal allows OSC 52.
- Over SSH, both the remote session and local terminal path must allow OSC 52. Some bastions and terminal gateways strip OSC sequences.
- Use terminal selection/manual copy as the fallback when OSC 52 is disabled by policy.

Reference: [xterm control sequences](https://invisible-island.net/xterm/ctlseqs/ctlseqs.html).

## Bracketed Paste

AVA enables bracketed paste while the TUI is active, disables it for shell/editor handoff, re-enables it on resume, and disables it on exit. This prevents pasted multi-line text from being interpreted as a sequence of submitted commands.

Expected behavior:

- Pasted text lands in the composer draft.
- Raw bracket markers (`[200~` and `[201~`) should not appear in the visible draft.
- Large multi-line pastes collapse to a `[paste #N ...]` marker, behave as one editable unit for cursor movement/deletion, and expand back to the original text when submitted.

If paste submits line-by-line or bracket markers appear:

1. Retry outside tmux/screen.
2. Check whether your terminal has bracketed paste disabled.
3. Check whether the multiplexer or remote gateway strips `CSI ? 2004 h/l` or paste wrapper sequences.

## Mouse

AVA enables xterm SGR button-motion reporting and ncurses mouse support when available. It uses mouse input for:

- Selecting slash-palette, file-reference, path-completion, settings, keybinding, model, and session rows.
- Moving the composer cursor and replacing dragged composer selections when coordinates are reported.
- Drag-selecting visible transcript text for OSC 52 clipboard copy with the configured `copy_selection` key (Ctrl+C by default when the draft is empty). A click without movement still toggles tool/thinking headers; dragging from a header selects instead.
- Mouse-wheel transcript scrolling and returning to the live tail.

If mouse actions do not work:

- Confirm the terminal allows applications to receive mouse events.
- In tmux, check `set -g mouse on` and retry in a fresh pane.
- In screen, expect more conservative behavior; test outside screen before filing an AVA bug.
- Remember that terminal mouse mode changes normal text selection. Hold Shift for terminal-native selection; AVA ignores Shift-modified button reports while preserving wheel scrolling.
- Transcript copy is bounded to 64 KiB and uses OSC 52. Status distinguishes a sent request, an empty/oversize selection, or local terminal-write failure without claiming clipboard delivery; the highlight remains after a request is sent.

## Color And Theme Environment

AVA theme precedence is:

1. `NO_COLOR`
2. `AVA_TUI_THEME`
3. `$XDG_CONFIG_HOME/ava/display.json`, including custom themes under `$XDG_CONFIG_HOME/ava/themes/*.json`
4. startup OSC 11 terminal-background detection on direct terminals (skipped under tmux)
5. terminal background inference from `COLORFGBG`
6. built-in dark fallback

Built-in light/dark keep the ordinary screen canvas at the terminal-default background.

Useful one-shot overrides:

```sh
NO_COLOR=1 ava
AVA_TUI_THEME=plain ava
AVA_TUI_THEME=light ava
AVA_TUI_THEME=dark ava
```

Notes:

- `NO_COLOR=1` and `AVA_TUI_THEME=plain` preserve layout/content but strip ANSI styling and suppress graphics overlays.
- OSC 11 runs once at interactive startup on direct terminals and is skipped under tmux; it is not re-probed on suspend/resume.
- `COLORFGBG` is only a hint when no explicit theme is set and OSC 11 did not decide. AVA reads the final foreground/background field and chooses a light or dark built-in palette when it can infer luminance.
- `/settings` marks the resolved theme as current without exposing source diagnostics. `/theme` persists normal display choices to `display.json`.

## SSH, Containers, And Remote Terminals

Remote runs add two common failure modes: the remote host may not know your local `TERM`, and protocol environment variables may not cross the SSH/container boundary.

Checklist:

- Run `infocmp "$TERM"` on the remote host, not only locally.
- If `TERM=xterm-kitty` is unknown remotely, install/copy Kitty terminfo or use a conservative `TERM=xterm-256color` for that session.
- Prefer terminal-provided SSH helpers when available, such as Kitty's SSH kitten, because they can install terminfo and preserve terminal integration more reliably than a bare SSH command.
- For containers, install runtime terminfo data inside the image or pass a `TERM` value that exists in the container.
- Inline images need a direct terminal path and AVA's identifying environment variables (`KITTY_WINDOW_ID`, `WEZTERM_PANE`, `ITERM_SESSION_ID`, etc.). SSH, sudo, containers, tmux, and screen may drop those variables.
- OSC 52 copy and OSC 8 links can be filtered by SSH clients, terminal gateways, bastions, or multiplexers. If blocked, AVA cannot force them through safely.
- Browser-based login may not work from a remote headless shell; use the headless OAuth/API-key flows documented in [CONFIG.md](../core/configuration.md).

## Symptom Guide

| Symptom | Likely cause | First checks |
| --- | --- | --- |
| `failed to initialize ncurses screen` | Bad/missing `TERM`, missing terminfo, no TTY | `printf '%s\n' "$TERM"`; `infocmp "$TERM"`; ensure interactive terminal. |
| Unicode borders/icons look wrong | Non-UTF-8 locale or font width mismatch | `locale`; choose UTF-8 locale and a font with box drawing/emoji coverage. |
| Alt/Ctrl/Shift key chord is not recognized | Terminal/mux does not report enhanced keys | Test direct terminal; check `/hotkeys`; add a keybinding fallback. |
| Mouse clicks/wheel do nothing | Terminal or mux is not forwarding mouse reports | Enable application mouse support; in tmux check `set -g mouse on`. |
| Paste submits multiple commands | Bracketed paste not delivered | Test outside multiplexer; check terminal paste mode/settings. |
| `/attach` works but no inline preview | Text-only image capability | Check `/settings` image preview row; use direct Kitty/Ghostty/WezTerm/Warp/iTerm2. |
| `/copy` says request sent but clipboard unchanged | OSC 52 blocked downstream | Check terminal clipboard policy, SSH path, and tmux/screen settings. |
| Markdown links are not clickable | OSC 8 disabled or filtered | Check terminal hyperlink setting; avoid tmux/screen for clickable link testing. |
| Colors are too bright/dim or absent | Theme override or `NO_COLOR` | Check `/settings`; unset `NO_COLOR`; set `AVA_TUI_THEME=dark` or `light`. |

## Known Limits Compared With Pi

- AVA is ncursesw-first. It does not embed Pi's TypeScript virtual-terminal runtime or copy Pi's terminal architecture.
- AVA's terminal evidence is CTest renderer/editor coverage plus opt-in PTY/tmux smokes, not a universal terminal emulator model. See [TESTING.md](testing.md) for current smoke commands.
- Inline image previews are direct-terminal only in AVA today; tmux/screen use text metadata even when the outer terminal could theoretically support a passthrough protocol.
- OSC 8 and OSC 52 are best-effort terminal protocols. If a terminal, SSH path, or multiplexer blocks them, AVA keeps visible/copyable text but cannot override that policy.
- Windows Terminal is recognized as a text-image, OSC-8-capable environment, but Windows packaging/setup docs are deferred.
- Termux packaging/setup docs are deferred.
- macOS packaging/setup docs are deferred. iTerm2 inline image protocol support exists when AVA is run in an environment that exposes iTerm2 session variables, but this guide does not document a packaged macOS install path.

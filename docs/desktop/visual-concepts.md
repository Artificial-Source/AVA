# AVA desktop visual concepts

**Rejected first pass — 2026-10-04.** The user rejected these flat light/dark
mockups and requested a premium, clean, minimalist **liquid-glass** direction,
with modern design research, visually inspected inspiration, and Astra at max
reasoning as the designer. These files are retained for iteration history,
not a current design baseline or candidate for approval.

They are static illustrations, not a running UI, production client code, or
implementation approval. The approved product scope and library families are
unchanged; final visuals remain undecided. Canonical direction lives in the
[desktop brief](README.md#current-visual-direction).

## Artifacts

| Theme | PNG | SVG |
| --- | --- | --- |
| Light | [concepts/ava-light.png](concepts/ava-light.png) | [concepts/ava-light.svg](concepts/ava-light.svg) |
| Dark | [concepts/ava-dark.png](concepts/ava-dark.png) | [concepts/ava-dark.svg](concepts/ava-dark.svg) |

Both rejected images are 1440×960. They are linked above rather than displayed
as the current direction.

Generator: [concepts/generate_concepts.py](concepts/generate_concepts.py).
Checks: [concepts/validate_concepts.py](concepts/validate_concepts.py),
report [concepts/contrast-report.txt](concepts/contrast-report.txt).

## How these were produced

Pencil / pen.dev was **not** used. `pen` 0.3.3 is present and
**not authenticated**; Pencil MCP connect fails unless a local Pen app with
`.pen` is kept open. No signup, login, upload, or paid-bridge retry was
attempted.

Fallback: original SVG drawn with a stdlib Python generator, system fonts
(Noto Sans, DejaVu Sans Mono), and headless Inkscape PNG export. No npm/pip
installs, no bundled font files, no third-party icon source, and no AVA
backend/provider requests.

Regenerate from the repo root (or `docs/desktop/concepts/`):

```sh
python3 docs/desktop/concepts/generate_concepts.py --png
python3 docs/desktop/concepts/validate_concepts.py
```

`--png` requires `/usr/bin/inkscape`. SVG output does not.

## Composition

Same hierarchy in both themes. The window is the artboard (1440×960), Linux
window controls on the right as monoline icons — not Mac traffic lights, not
Apple fonts.

- **Compact chrome:** AVA mark, current chat title, `main`, shared working
  copy, `worktree off`, a closed **Changes** affordance. No file tree.
- **Modest library:** project groups `AVA` and `agent-lab` (fixture). Current
  chat selected; other rows use **icon + text** status (`Working`,
  `Needs approval`), not color alone.
- **Working set tabs:** three user-chosen chats, including one from
  `agent-lab`. No project tabs. Tab close is not shown as cancelling work.
- **Conversation:** open assistant prose on a continuous canvas (not boxed
  cards). Subdued user turn. Quiet expandable-looking tool summary. Short
  mono excerpt, not a terminal.
- **One composer:** Build selected / Plan idle, example model display,
  **Send** because the open chat is idle. Shared working copy and worktree-off
  repeated as context. Stop is not shown here.
- **Right of the reading column** is empty on purpose: Changes stays closed.
  This is a focused coding agent, not an IDE.

Light and dark would follow system preference in a real app. These files are
two static renderings of that idea.

## Rejected palette

These exact colours and flat material treatment are **rejected**, not approved
or the starting point for the replacement design. The first pass used pale
off-white / charcoal neutrals with one quiet sage accent.

| Token | Light | Dark |
| --- | --- | --- |
| desktop (corner reveal) | `#E4DFD4` | `#141513` |
| canvas | `#F3F0E8` | `#1E1F1D` |
| sidebar / footer | `#E9E4D8` | `#171816` |
| composer | `#EBE6DB` | `#232422` |
| code wash | `#E7E2D6` | `#262725` |
| text | `#1C1B17` | `#EEECE5` |
| secondary | `#45423B` | `#C4C0B6` |
| muted | `#5A564E` | `#A8A39A` |
| accent / accent fill | `#2C5A4B` | `#8FBA9F` / `#2F5D4E` |
| on-accent | `#F3F0E8` | `#F3F0E8` |
| warning | `#6F4708` | `#E6C37A` |
| warning wash | `#F0E2BE` | `#3A3018` |
| selected row | `#E1E9E4` | `#1E2823` |
| hairline / border | `#DDD7CB` / `#D2CCC0` | `#2C2E2B` |

Type: Noto Sans for UI and prose; DejaVu Sans Mono for the path and the
one-line excerpt. Icons are original 16px strokes at Phosphor-like weight,
not imported library source.

## Fixtures (illustrative only)

| Item | Fixture |
| --- | --- |
| Open chat | `session-format review` — **idle / completed**. Composer shows **Send**. |
| Other AVA chat | `permission policy` — **Working** (icon + label). |
| Other project | `agent-lab` marked **fixture**; `MCP tool wiring` — **Needs approval** (icon + label). |
| Tabs | The same three chats; third tab is from `agent-lab`. |
| Git | `main` · shared working copy · **worktree off** (default, not imposed). |
| Model | `Example model` · `illustrative display` |
| Tool row | `Read · docs/session-format.md` with an `example` caption |
| Footer | `Concept · example data · static visual, not a live session` |
| Counts | Idle chat · 1 working · 1 needs approval — matches rows, tabs, and footer |

Copy is invented for layout. It is not a provider result, session log, or
command that was run. No credentials, session files, raw IDs, or real
message bodies.

## Checks actually run

`validate_concepts.py` was run after PNG export:

- SVG well-formed XML, root `svg`, `width`/`height`/`viewBox` 1440×960
- No external `http(s)` resources (SVG xmlns and internal `url(#clip)` allowed)
- No `image`, `foreignObject`, `script`, or `iframe`
- PNG size 1440×960 via PIL
- Relative-luminance contrast for declared token pairs; body/secondary ≥ 4.5:1,
  chrome/icon ≥ 3:1

These are finite checks on static art. They are **not** WCAG certification,
keyboard testing, scroll testing, screen-reader testing, or runtime GUI
testing.

## Explicitly out of scope

- No production Electron/React code, CSS Modules, or package pins from this
  work
- No backend, public RPC, provider, or permission-dialog behavior
- No claim that closing the last window, app quit, or a daemon is decided
- No Zeron GPUI/daemon/sync/embedded browser, no Mac chrome
- No editors, terminals, cloud sync, or extra side panes
- No fake token metrics, test-success claims, or commands executed

The [desktop category index](README.md) records the current scope and visual
direction. The checks above describe only the rejected artifacts.

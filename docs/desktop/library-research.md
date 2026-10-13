# Desktop Library Research

Dated snapshot of **open-source library candidates** for a later optional
Electron desktop client, supplemented by the approved library direction below.
**Library families are selected; exact pins and production adoption are pending.**
No new-stack dependencies have been installed. **No new-stack test has run.**

**Retrieved:** 2026-10-02 (machine local); 2026-10-03 UTC. Licenses from
retrieved GitHub LICENSE files or package metadata cited below. A fetched docs
page is evidence that documentation existed on that date, **not** certification
of release cadence, peer compatibility, or that current npm versions work
together.

Electron is an **accepted host direction**. The renderer, styling, icons,
Markdown, highlighter, and unit-test families below are approved. Exact versions,
compatibility, production bundling, virtualizer, and packager remain unresolved.
The legacy prototype in `apps/desktop/` is operational guidance only, not a
greenfield baseline ([desktop index](README.md)).

## Approved library direction

**User-approved 2026-10-04**, after confirming licence costs and TypeScript's
Apache-2.0 obligations alongside AVA's intended MIT licence:

- React + TypeScript for the UI and typed host/preload code.
- electron-vite for development/build, not packaging.
- Radix Primitives as the single headless component family, with bespoke styling.
- CSS Modules and shared CSS-variable tokens.
- Phosphor icons.
- react-markdown + remark-gfm for chat; Shiki for code highlighting.
- Vitest + Testing Library for unit/component checks.

This is approval of the library direction, not a tested production dependency
set or permission to implement the full client. The user later authorized a
separate React/TypeScript **mock-data UI prototype**, including verified exact
pins for that prototype only; see the [prototype record](react-prototype.md). It does not replace the legacy client or authorize
backend integration or production adoption. Preserve each dependency's licence obligations;
AVA's MIT licence does not replace third-party licences. Exact pins, shipped
notices, transitive licences, peers, CSP/sandbox compatibility, and GUI evidence
must be checked before production adoption. No packager, virtualizer, animation
runtime, router, state library, or pre-styled UI kit is selected.

The remaining candidate comparison preserves the dated research and its
unresolved checks; unselected alternatives are not additional approvals.

AVA bounds that any later stack must still honor:
[architecture](architecture.md) (sandboxed renderer, no Node in renderer, main
owns binary/workspace/process/IPC),
[Electron security](https://www.electronjs.org/docs/latest/tutorial/security),
and [fuses](https://www.electronjs.org/docs/latest/tutorial/fuses). Production
fuses are not weakened to make tests pass.

No library, headless kit, or CSS tool makes a finished app accessible or
polished. Headless kits, plain CSS, CSS Modules, and Tailwind can **all**
produce a bespoke surface. Do not read “Radix means clone” or “Tailwind means
generic.”

## Candidate table

| Package / product | Role | License (retrieved) | Evidence | Risk / unresolved |
| --- | --- | --- | --- | --- |
| **electron** | Desktop host (main / preload / renderer) | MIT ([electron/electron LICENSE](https://github.com/electron/electron/blob/main/LICENSE)) | [Security](https://www.electronjs.org/docs/latest/tutorial/security); [fuses](https://www.electronjs.org/docs/latest/tutorial/fuses) | Host accepted; version pin, fuses, and Linux GUI evidence not done here |
| **react** / **react-dom** | Renderer UI runtime | MIT ([facebook/react LICENSE](https://github.com/facebook/react/blob/main/LICENSE)) | Considered with Electron; no adoption | Peer/tool compatibility at pin time |
| **typescript** | Typed renderer/main source | Apache-2.0 ([TypeScript LICENSE.txt](https://github.com/microsoft/TypeScript/blob/main/LICENSE.txt)) | Considered; no pin | Compiler/tooling floor unresolved |
| **vite** | Renderer/dev bundler family | MIT ([vitejs/vite LICENSE](https://github.com/vitejs/vite/blob/main/LICENSE)) | Considered with electron-vite / Forge Vite | Exact Vite floor omitted until a pin is approved |
| **electron-vite** | Dev/build: main, preload, renderer Vite configs; HMR | MIT ([alex8088/electron-vite LICENSE](https://github.com/alex8088/electron-vite/blob/master/LICENSE)) | [Guide](https://electron-vite.org/guide/); [distribution](https://electron-vite.org/guide/distribution) | **Not a packager.** [Isolated build](https://electron-vite.org/guide/isolated-build) page is **experimental**; useful for **multiple preloads** / single-file sandbox bundles. Do **not** require that experimental flag globally. |
| **Electron Forge** | Scaffold + package + makers (deb/rpm/zip/…) | MIT ([electron/forge LICENSE](https://github.com/electron/forge/blob/main/LICENSE)) | [electronforge.io](https://www.electronforge.io/) | Official pipeline. Do not also adopt electron-builder as a second packager. |
| **@electron-forge/plugin-vite** | Forge Vite bundling | MIT (Forge) | [Vite plugin](https://www.electronforge.io/config/plugins/vite); [Forge v7.5.0 notes](https://github.com/electron/forge/releases/tag/v7.5.0) | **Experimental.** Upstream warns minor releases may break. Webpack templates are Forge’s non-experimental bundler path (out of this Vite comparison). |
| **electron-builder** | Packaging / installers / auto-update | MIT ([electron-builder LICENSE](https://github.com/electron-userland/electron-builder/blob/master/LICENSE)) | [electron.build](https://www.electron.build/) | Packager, not a compiler. Overlaps Forge **makers**, not electron-vite. Config-page extract failed in the research pass. **No packager is mandatory until distribution scope is approved.** |
| **react-aria-components** (React Aria) | Unstyled keyboard/focus/menus/forms/dialogs | Apache-2.0 ([adobe/react-spectrum LICENSE](https://github.com/adobe/react-spectrum/blob/main/LICENSE)) | [Styling](https://react-spectrum.adobe.com/react-aria/styling.html); [accessibility](https://react-spectrum.adobe.com/react-aria/accessibility.html) | Adobe monorepo gravity. Docs ≠ completed WCAG for AVA. |
| **Radix Primitives** | Unstyled primitives | MIT ([radix-ui/primitives LICENSE](https://github.com/radix-ui/primitives/blob/main/LICENSE)) | [Introduction](https://www.radix-ui.com/primitives/docs/overview/introduction); [accessibility](https://www.radix-ui.com/primitives/docs/overview/accessibility) | Install family is consolidated [`radix-ui`](https://www.npmjs.com/package/radix-ui) or per-primitive `@radix-ui/react-*` (e.g. dialog). **`@radix-ui/primitives` is not safe install advice.** |
| **@base-ui/react** (Base UI) | Unstyled primitives | MIT ([mui/base-ui LICENSE](https://github.com/mui/base-ui/blob/master/LICENSE)) | [About](https://base-ui.com/react/overview/about); [accessibility](https://base-ui.com/react/overview/accessibility) | Younger than Radix; still MIT. shadcn Dialog docs now point here. |
| **shadcn/ui** | **Source registry / CLI**, not an npm UI runtime | MIT ([shadcn-ui/ui LICENSE.md](https://github.com/shadcn-ui/ui/blob/main/LICENSE.md)) | [Docs](https://ui.shadcn.com/docs) | Copied source becomes first-party. Defaults are someone else’s look. Optional recipe, not a fourth primitive runtime. |
| **@mantine/core** | Pre-styled kit | MIT ([mantinedev/mantine LICENSE](https://github.com/mantinedev/mantine/blob/master/LICENSE)) | [Getting started](https://mantine.dev/getting-started/) | Fast chrome; costly to override for a bespoke surface. |
| **@mui/material** | Pre-styled Material Design | MIT ([mui/material-ui LICENSE](https://github.com/mui/material-ui/blob/master/LICENSE)) | [Getting started](https://mui.com/material-ui/getting-started/) | Distinct from **MUI X** ([licensing](https://mui.com/x/introduction/licensing/)): community MIT + commercial Pro/Premium. Paid grids are out of a minimal OSS desktop. |
| **tailwindcss** | Utility CSS + `@theme` tokens | MIT ([tailwindlabs/tailwindcss LICENSE](https://github.com/tailwindlabs/tailwindcss/blob/main/LICENSE)) | [Theme](https://tailwindcss.com/docs/theme) | Token mechanism, not an appearance promise. CSP still needs retest. |
| **CSS / CSS Modules** | Bespoke CSS; Vite `*.module.css` | Spec ([css-modules](https://github.com/css-modules/css-modules)); Vite [CSS modules](https://vite.dev/guide/features.html#css-modules) | First-class in Vite | No single npm license for “CSS Modules.” Custom CSS is **not** an automatic CSP guarantee. |
| **@phosphor-icons/react** | Icon set | MIT (Phosphor homepage / react LICENSE retrieved in research) | GitHub LICENSE files | Not SF Symbols. Confirm shipped tarball at pin time. |
| **Apple SF Pro / SF Mono / SF Symbols** | **Do not ship** | Apple [fonts](https://developer.apple.com/fonts/) license as reviewed: mock-ups of **Apple OS** UIs; **no embed**; **no non-Apple OS** mock-ups | License page | Use system/generic stacks on Linux. Prudent alternative, not a broader legal opinion. |
| **react-markdown** + **remark-gfm** | Markdown + GFM | MIT both ([react-markdown security](https://github.com/remarkjs/react-markdown#security)) | Default avoids raw HTML | Plugins, custom `components`, or unsafe `urlTransform` can break safety. `rehype-sanitize` is extra and **not only** a raw-HTML concern. **No arbitrary raw HTML/JSX for model text.** |
| **shiki** | TextMate/VS Code highlighter | MIT ([shikijs/shiki LICENSE](https://github.com/shikijs/shiki/blob/main/LICENSE)) | [Guide](https://shiki.style/guide) | WASM/workers/CSP in a sandboxed Electron renderer **untested here**. Prefer core + needed languages if later adopted. |
| **diff** (jsdiff) | Text diff **algorithm**, not a UI | BSD-3-Clause ([kpdecker/jsdiff](https://github.com/kpdecker/jsdiff)) | LICENSE + package.json | Viewer would still be custom. Skip Monaco/IDE shells. |
| **react-virtuoso** **core** | List/grid/feed virtualization | MIT **declared** in [`packages/react-virtuoso/package.json`](https://github.com/petyosi/react-virtuoso/blob/master/packages/react-virtuoso/package.json) | package.json `"license": "MIT"` | **Root LICENSE 404** in the research pass. Shipped-tarball license review **unresolved**. Do not make legal claims beyond that metadata. `@virtuoso.dev/message-list` is a **separately licensed** derivative and was **not verified**; do not treat it as core. |
| **@tanstack/react-virtual** | Headless virtualizer | MIT ([TanStack/virtual LICENSE](https://github.com/TanStack/virtual/blob/main/LICENSE)) | [Introduction](https://tanstack.com/virtual/latest/docs/introduction); [chat](https://tanstack.com/virtual/latest/docs/chat) | More markup/CSS ownership. Exact APIs omitted until a version is selected. |
| **vitest** | Unit/component runner | MIT ([vitest-dev/vitest LICENSE](https://github.com/vitest-dev/vitest/blob/main/LICENSE)) | [Guide](https://vitest.dev/guide/) | Floors omitted until pins. Not an Electron GUI test. |
| **@testing-library/react** | DOM-user queries | MIT | [Intro](https://testing-library.com/docs/react-testing-library/intro/) | Not a runner. |
| **Playwright `_electron`** | GUI E2E | Apache-2.0 ([microsoft/playwright LICENSE](https://github.com/microsoft/playwright/blob/main/LICENSE)) | [class-electron](https://playwright.dev/docs/api/class-electron) **EXPERIMENTAL** | `_electron.launch` `chromiumSandbox` **defaults false** → security-valid tests must set **true**; `bypassCSP` defaults false — keep false. **No** `--no-sandbox` and **no** host prerequisite bypass. Native dialogs need **main-process test doubles**. Production fuses not weakened for testing. **No GUI test was attempted in this task.** Legacy Ubuntu chrome-sandbox helper prerequisite remains documented on the prototype package; equivalent review would be required before any real GUI evidence. |
| **motion** | Optional animation | MIT (not installed) | Optional if a later design needs motion | Skip unless needed. Honor `prefers-reduced-motion`. Not part of any stack here. |

Maintenance evidence above is **docs/repo/release visibility** on the snapshot
date, not a claim that all peers or current npm releases are compatible.

## Build versus packaging

Do not conflate compiler, experimental bundler plugins, and packagers.

- **electron-vite** is Vite-based **dev/build**, not a packager. Isolated
  entries help **multiple preloads**; do not mandate the experimental isolated
  build globally.
- **Forge** is official **scaffold + packager + makers**. Its Vite plugin is
  experimental, with an upstream warning about minor breaking changes.
- **electron-builder** is **packaging**. Role overlap is with Forge makers.

**No packager is required until a distribution/installer scope is approved.**
Do not pick both packagers. Do not treat packaging research as a first-release
desktop artifact ([product principles](../product/principles.md)).

## Renderer UI kits and styling

React Aria, Radix Primitives, and Base UI document **behavior** (ARIA,
keyboard, focus), not appearance. **Radix Primitives is the approved family**;
its exact packages/pins still need validation. Do not add overlapping primitive
kits. Native app menus and file dialogs stay **main-process Electron**.

shadcn copies source into the app. Inventory copied files and which primitive
backend they use as first-party code if that recipe is ever approved.

Mantine / Material UI are pre-styled. They do not forbid a bespoke look, but
they optimize for their own visual language. Skip unless a later review accepts
that language. MUI X Pro/Premium is commercial.

Tailwind `@theme` and CSS Modules / `:root` custom properties can both drive
bespoke tokens. That is a velocity vs abstraction choice, **not** an appearance
promise.

Contrast, hit targets, focus rings, `prefers-reduced-motion`, and labels remain
app work. Overlay, virtualizer, and highlighter CSS must be retested under the
**production CSP**; custom CSS does not guarantee CSP compliance.

## Markdown, code, diff, virtualization

Agent/model text is untrusted. Default react-markdown without `rehype-raw`.
Unsafe plugins, components, or `urlTransform` reopen XSS. Sanitize is not only
about raw HTML.

Add Markdown when transcripts render MD; Shiki when fenced code is in-scope;
jsdiff when patch **text** is in-scope. Always retest CSP. Skip Monaco, xterm,
AI SDKs, global-state libraries, and routers unless a later approved use case
exists — they are **not** requirements.

Virtualize only after measured long-transcript jank. Candidates: virtuoso
**core** (MIT-declared; shipped-license unresolved) **or** TanStack Virtual.
Do not add the separately licensed message-list product without review.

## Essential versus optional

**Approved families awaiting validation/adoption:** Electron; React/TypeScript;
electron-vite; Radix Primitives; CSS Modules/shared tokens; Phosphor;
react-markdown/remark-gfm; Shiki; Vitest/Testing Library. Electron security/fuses
remain mandatory implementation checks, not evidence that a new client is safe.

**Unselected or conditional:** a packager only if distribution is approved;
Playwright Electron for later GUI E2E (experimental); shadcn registry copy;
rehype-sanitize depending on the final Markdown pipeline; jsdiff; virtuoso core
or TanStack Virtual; Mantine/MUI; Motion. Do not install these just because they
appear in the comparison.

**Out of this shortlist:** routers, global state libraries, AI SDKs, terminals,
Monaco/IDE shells, MUI X paid grids.

## Unresolved before any production adoption

The family-level direction is approved; implementation still requires separate
approval. Require exact pins, peer/tool compatibility, shipped-license and
transitive artifact review, and security/sandbox/perf/real-GUI evidence.
Specifically unresolved here:

- No npm installs; no live “latest” versions.
- Transitive licenses / SBOM.
- react-virtuoso **root** LICENSE and tarball; message-list terms.
- electron-builder configuration-page extract.
- Shiki/wasm/CSS overlays under Electron CSP + sandbox.
- Playwright × production fuse matrix beyond documented inspect-launch
  constraints.
- CSS Modules as a spec rather than a versioned runtime license.
- Phosphor react package tarball re-read at pin time.

This research did **not** run a new Electron stack, GUI test, or CSP probe.

## Source ledger

Retrieved 2026-10-02 local / 2026-10-03 UTC.

1. https://www.electronjs.org/docs/latest/tutorial/security
2. https://www.electronjs.org/docs/latest/tutorial/fuses
3. https://github.com/electron/electron/blob/main/LICENSE
4. https://electron-vite.org/guide/
5. https://electron-vite.org/guide/isolated-build
6. https://electron-vite.org/guide/distribution
7. https://www.electronforge.io/config/plugins/vite
8. https://github.com/electron/forge/releases/tag/v7.5.0
9. https://github.com/electron/forge/blob/main/LICENSE
10. https://www.electron.build/
11. https://react-spectrum.adobe.com/react-aria/styling.html
12. https://react-spectrum.adobe.com/react-aria/accessibility.html
13. https://www.radix-ui.com/primitives/docs/overview/introduction
14. https://www.radix-ui.com/primitives/docs/overview/accessibility
15. https://base-ui.com/react/overview/about
16. https://base-ui.com/react/overview/accessibility
17. https://ui.shadcn.com/docs
18. https://tailwindcss.com/docs/theme
19. https://developer.apple.com/fonts/
20. https://github.com/remarkjs/react-markdown#security
21. https://shiki.style/guide
22. https://tanstack.com/virtual/latest/docs/introduction
23. https://tanstack.com/virtual/latest/docs/chat
24. https://playwright.dev/docs/api/class-electron
25. https://vitest.dev/guide/
26. https://mui.com/x/introduction/licensing/
27. https://github.com/petyosi/react-virtuoso/blob/master/packages/react-virtuoso/package.json
28. https://github.com/facebook/react/blob/main/LICENSE
29. https://github.com/microsoft/TypeScript/blob/main/LICENSE.txt
30. https://github.com/vitejs/vite/blob/main/LICENSE

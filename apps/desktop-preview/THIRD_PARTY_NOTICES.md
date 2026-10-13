# Third-party dependency notes — UI preview only

This package uses a **subset** of AVA's approved desktop library families. It is
not a production adoption, legal certification, SBOM, or complete shipped-artifact
license audit. AVA's own license does not replace third-party terms.

## Direct runtime dependencies

The exact installed runtime packages' LICENSE files were read. Their complete
texts are preserved in
[`public/third-party-runtime-licenses.txt`](public/third-party-runtime-licenses.txt),
which Vite also copies into `dist/`. These are direct-package notices, not a
claim that every transitive artifact has been audited. MIT permits use under its
conditions, including preservation of copyright and permission notices, without
a license fee or warranty.

| Package | Exact pin | Direct inspected evidence |
| --- | --- | --- |
| `react` | 19.3.0 | MIT `LICENSE`; Meta Platforms, Inc. and affiliates |
| `react-dom` | 19.3.0 | MIT `LICENSE`; Meta Platforms, Inc. and affiliates |
| `@phosphor-icons/react` | 2.1.10 | MIT `LICENSE`; Copyright 2020 Phosphor Icons |
| `react-markdown` | 10.1.0 | MIT `license`; Espen Hovlandsdal |
| `remark-gfm` | 4.0.1 | MIT `license`; Titus Wormer |
| `shiki` | **4.4.3** | MIT `LICENSE`; Copyright 2021 Pine Wu / 2023 Anthony Fu |

Registry records are at `https://registry.npmjs.org/<package>/<pin>`; their
`dist.tarball` fields identify the upstream artifacts. The original researcher
inspected runtime tarballs; this implementation pass read the six installed
runtime licenses and the corrected Shiki 4.4.3 tarball LICENSE. Local installed
packages retain their upstream files.

## Direct development dependencies

TypeScript **7.0.2** declares **Apache-2.0**. The remaining selected development
packages declare **MIT**: Vite 7.3.6, plugin-react 5.2.0, Node types 24.13.1, React
and React DOM types 19.3.0, Testing Library React 16.3.3 / user-event 14.6.7 /
jest-dom 7.0.1 / DOM 10.4.2, and jsdom 29.1.1. This is registry **metadata**
evidence, not a tarball-license inspection of every development dependency.

Vitest **5.0.2**'s tarball LICENSE was separately inspected: its core is MIT
(VoidZero Inc. and Vitest contributors), and its bundled third-party notices also
include **BSD-3-Clause and ISC**, not just MIT. Vitest is a development dependency,
not part of the built browser UI. This finite inspection is not a transitive audit.

The initially requested Shiki 4.5.0 and Vitest 5.0.3 were too recent for npm's
existing **`min-release-age=7`** setting. The parent authorized exact same-family
corrections to **4.4.3** and **5.0.2** after metadata/license/engine/peer checks.
The subsequent install succeeded, produced `package-lock.json`, and left the
release-age guard unchanged at **7**. No install scripts, audit upload, force,
engine override, or security bypass was used.

The selected jsdom version accommodates Node 24.13.1; jsdom 30.1.2 does not.
Testing Library DOM is explicitly pinned because it is a required peer. CSS
Modules is native Vite functionality and has no additional direct package.
Native HTML selects avoid installing an unneeded Radix primitive. No Electron,
packager, Motion, Tailwind, UI kit, or optional canvas is included.

## Boundaries and outstanding review

- The npm lock pins the resolved dependency tree and integrity hashes, but a lock
  is **not** a transitive license/SBOM audit. Engine/peer metadata and the passing
  Node/DOM checks are not browser or Electron evidence.
- Shiki's package license does not complete a provenance review of its bundled
  TypeScript TextMate grammar, Nord theme, or transitive regex implementation.
- The direct notices file is **not a complete distribution notice inventory**.
  A production distribution must inventory everything it actually ships and
  preserve all applicable licenses/notices, including build-tool third-party
  terms if those artifacts are distributed. This prototype is not that review.
- No Apple fonts, SF Symbols, copied competitor code/assets, installed font
  binaries, or remote font assets are bundled. The original AVA SVG mark and
  mineral palette come from AVA's first-party approved HTML study. Geist names
  only request an already-installed font; fallback fonts are not redistributed.

See [the prototype record](../../docs/desktop/react-prototype.md) for all 18 exact
direct pins, scope, primary-source links, executable checks, and limits.

# AVA Release Artifact Checklist

This checklist covers the local Linux host artifact and exact-byte candidate CI. Runtime version `1.0.0` is not a published release. Candidate CI retains archives and evidence, but does **not** commit, tag, push, publish a release, sign, notarize, generate an SBOM, or call a live provider.

For the current decision and exact-byte blockers, see the [release-readiness ledger](https://github.com/Artificial-Source/AVA/blob/develop/docs/product/release-readiness.md). For the required but not-yet-implemented official lifecycle, see [publication.md](https://github.com/Artificial-Source/AVA/blob/develop/docs/operations/publication.md). For deterministic, sanitizer, terminal, and opt-in live-provider evidence, see [testing.md](testing.md).

## Artifact contract

The version source is the top-level `project(ava VERSION X.Y.Z)` declaration. The packaged executable must report exactly `ava X.Y.Z` from `ava --version`.

The archive pair is:

```text
ava-X.Y.Z-linux-ARCH.tar.gz
ava-X.Y.Z-linux-ARCH.tar.gz.sha256
```

The first-publication `ARCH` is **`x64` only**. The generic local packager also normalizes other native ELF architectures for development checks; those outputs are not official candidates. The archive has one top-level `ava-X.Y.Z-linux-ARCH/` directory with this exact allowlist:

```text
bin/ava
share/doc/ava/README.md
share/doc/ava/LICENSE
share/doc/ava/THIRD_PARTY_NOTICES.md
share/doc/ava/PROVENANCE.json
share/doc/ava/docs/core/usage.md
share/doc/ava/docs/core/configuration.md
share/doc/ava/docs/core/context-resources.md
share/doc/ava/docs/core/custom-providers.md
share/doc/ava/docs/core/environment-variables.md
share/doc/ava/docs/core/providers.md
share/doc/ava/docs/core/subagents.md
share/doc/ava/docs/core/thinking-modes.md
share/doc/ava/docs/core/tools.md
share/doc/ava/docs/interfaces/themes-keybindings.md
share/doc/ava/docs/operations/testing.md
share/doc/ava/docs/operations/terminal-setup.md
share/doc/ava/docs/operations/troubleshooting.md
share/doc/ava/docs/operations/diagnostics.md
share/doc/ava/docs/operations/release-checklist.md
share/doc/ava/docs/extensions/lsp.md
share/doc/ava/docs/extensions/mcp.md
share/doc/ava/docs/extensions/plugin-system.md
share/doc/ava/docs/security/sandboxing.md
share/doc/ava/docs/security/containment.md
share/doc/ava/docs/development/session-versioning.md
share/doc/ava/docs/development/side-effect-safety-checklist.md
share/doc/ava/docs/headless-protocol.md
share/doc/ava/docs/rpc-protocol.md
share/doc/ava/docs/acp.md
share/doc/ava/docs/acp-support.json
share/doc/ava/docs/session-format.md
share/doc/ava/docs/plugin-compatibility-policy.md
share/doc/ava/docs/interop/evidence/README.md
share/doc/ava/docs/interop/evidence/zed-1.9.0-2026-07-14.md
share/doc/ava/docs/product/mvp-coverage-ledger.md
share/doc/ava/docs/schema/theme.schema.json
```

The installed `README.md` comes from the artifact-specific source template at `docs/operations/release-artifact-readme.md`, not the repository README. The curated documentation payload is exactly 32 source files (30 Markdown and 2 JSON) mirrored under their source categories; category indexes and historical/planning documents remain source-only. `THIRD_PARTY_NOTICES.md` is the distribution notice; `PROVENANCE.json` is a deterministic, privacy-safe description of this binary, its source/dependency state, architecture, and ELF dynamic dependencies. The staged documentation layout must follow the categorized source layout so included local links resolve. `scripts/verify-markdown-links.py` verifies every staged Markdown relative path before archive creation.

The allowlist excludes reference repositories, source and test trees, build trees, examples, credentials, auth/config/session state, provider output, raw interoperability evidence, and optional desktop source and planning docs. CMake component `ava` must remain exact; always pass `--component ava` for a manual stage:

```sh
cmake --install build-release \
  --prefix /absolute/private/stage/outside/the/checkout \
  --component ava
```

## Portability boundary

This is a dynamically linked **Linux x64 host artifact**, not a universal or manylinux-style bundle. Official candidates target **x86-64-v2 (Nehalem-class or newer)**, with BMI2/AVX disabled. The minimum host is Ubuntu 24.04 (glibc 2.39), libstdc++ exposing `GLIBCXX_3.4.32`/`CXXABI_1.3.13`, `libncursesw.so.6`/`libtinfo.so.6` (6.4+), terminfo, and `curl` 8.5+. `scripts/inspect-linux-artifact.py` caps required symbols at `GLIBC_2.38`, `GLIBCXX_3.4.32`, and `CXXABI_1.3.13`, checks decoded ISA/encodings and `/lib64/ld-linux-x86-64.so.2`, and lists dynamic dependencies. Ubuntu container extraction and QEMU Nehalem smoke qualify the userspace and CPU profile separately from the native terminal gates.

The first-publication target is Linux x64 only. Every published architecture requires its own native exact-candidate CI build, full deterministic and required terminal/sanitizer evidence, clean source/package gates, retained exact archive/checksum bytes, and minimum-floor smoke. Cross-compilation and source branches are not qualification evidence. AArch64, MSVC, Windows, and macOS were not qualified; multi-config is not release-qualified. Dirty working trees are always unqualified.

Build-only dependencies are not packaged. Building from this checkout needs CMake 3.27+, a C++23 compiler, Boost and ncurses development files, Git, and configured source dependencies. Canonical debug-enabled development/sanitizer configurations also require a writable `GITACHE_ROOT`, Python 3, and JSON-capable Universal Ctags. Packaging additionally needs Bash, Python 3, `tar`, `sha256sum`, and Linux `renameat2`. Python is not a runtime dependency.

## Release-candidate checks

### 1. Preflight

- Confirm the intended top-level CMake version and exact `ava --version` output.
- Confirm scope remains terminal `ava` plus the documented allowlist.
- Confirm no credentials, local configuration, session files, quarantines, or provider output can enter the stage.
- Confirm tag/push/official-publication/signing/CPack/cross-build/desktop work remains out of scope unless separately approved; candidate CI upload is required evidence.

### 2. Developer build and deterministic tests

Prepare the canonical preset prerequisites, then build and test:

```sh
export GITACHE_ROOT="${GITACHE_ROOT:-$HOME/.cache/ava/gitache}"
mkdir -p "$GITACHE_ROOT"
cmake --preset dev
scripts/build.sh
scripts/run-tests.sh
```

Python 3 and JSON-capable Universal Ctags are required for the complete debug-enabled registration/generation path.

Focused release-closure coverage uses the registered CTest names. Python 3 and built `ava`, `ava_fake_provider_server`, and `ava_fake_mcp_server` executables are prerequisites for `ava_cli.acp_subprocess`.

```sh
scripts/run-tests.sh \
  -R '^(ava_tests\.(session|app_runtime|app_rpc|agent_loop|agent_loop_resilience|acp)|ava_cli\.acp_subprocess|ava_release\.package_linux)$'
```

The deterministic full-binary model/tool smoke is:

```sh
scripts/run-tests.sh -R '^ava_cli\.headless_e2e_model_smoke$'
```

No command above opts into live-provider calls.

### 3. Sanitizer gate

```sh
cmake --preset sanitize
scripts/build.sh --build-dir build-sanitize --jobs 2
scripts/run-tests.sh --build-dir build-sanitize --jobs 2
```

If the host cannot run ASan/UBSan, record the exact environment blocker rather than claiming a pass.

### 4. Terminal evidence

A developer run is supplemental, but exact-candidate qualification requires all 23 tmux and four direct PTY gates with zero skips under `AVA-REL-011`:

```sh
AVA_TUI_TMUX_SMOKE=1 scripts/run-tests.sh --build-dir build --jobs 23 -R '^ava_tui\.tmux_smoke_'
AVA_TUI_KITTY_IMAGE_SMOKE=1 AVA_TUI_ITERM2_IMAGE_SMOKE=1 \
AVA_TUI_TERMINAL_LIFECYCLE_SMOKE=1 AVA_TUI_OSC8_SMOKE=1 \
  scripts/run-tests.sh --build-dir build --jobs 4 \
  -R '^ava_tui\.(kitty_image_smoke|iterm2_image_smoke|terminal_lifecycle_smoke|osc8_smoke)$'
```

Intentionally credential-gated live-provider checks remain classified in [testing.md](testing.md). They are not package-script steps or substitutes for deterministic terminal evidence.

## Build and verify the Linux host artifact

Ordinary build mode configures a fresh private Release build tree, builds the current checkout, and publishes to a caller-supplied secure output directory. Package source builds disable Gitache and libcwd, force the pinned in-tree nlohmann JSON source, and set CMake FetchContent fully disconnected, so packaging performs no dependency download. Ordinary mode emits provenance but does not by itself make a qualification claim:

```sh
scripts/package-linux.sh --output-dir /absolute/path/outside/AVA
```

The static package/provenance qualification contract is explicit and fail-closed. It rejects supplied binaries, any source or dependency worktree change (including untracked files), mismatched gitlinks, missing or unsupported host/binary architecture evidence, disagreement between the canonical packaging-host architecture and detected ELF architecture, unexpected ELF `DT_NEEDED` entries, and direct license-file SHA-256 policy mismatches or missing license evidence:

```sh
scripts/package-linux.sh --require-release-qualified \
  --output-dir /absolute/path/outside/AVA
```

The flag and resulting `PROVENANCE.json` field `release_qualified:true` prove only those implemented static source/gitlink/license/version/native-architecture/dynamic-dependency/package gates. They do not prove full CTest, native CI, sanitizer/TSan, the 23 tmux plus four PTY gates, retention of exact bytes, or official publication. Complete qualification requires the [release ledger](https://github.com/Artificial-Source/AVA/blob/develop/docs/product/release-readiness.md) and [publication runbook](https://github.com/Artificial-Source/AVA/blob/develop/docs/operations/publication.md).

Accepted-binary mode does not configure or build:

```sh
scripts/package-linux.sh \
  --binary /absolute/path/to/ava \
  --fake-provider /absolute/path/to/ava_fake_provider_server \
  --output-dir /absolute/path/outside/AVA
```

Accepted-binary mode rejects any executable whose exact `ava X.Y.Z` output differs from the current checkout's top-level CMake version before checkout documentation is staged. `--fake-provider` is optional in this mode; omitting it produces an explicit deterministic model-smoke skip.

If `--output-dir` is omitted, the script creates and reports a new unpredictable mode-`0700` directory outside the checkout. A supplied output directory must be empty, outside the checkout, owned by the effective user, and have exact mode `0700`. The script rejects a symlink output directory and never overwrites existing archive/checksum destinations (including symlinks).

All staging, allowlist/link checks, archive and checksum creation, checksum verification, extraction, CLI smoke, and fake-provider model smoke happen inside one private unpredictable work directory. The archive is never executed, extracted, or verified after publication. Publication is the final operation: unique temporary files are written and synced through one verified output-directory descriptor, moved with atomic no-replace `renameat2`, directory-synced, and the output namespace identity is revalidated. The script removes only its private work directory; it never recursively deletes caller output.

The deterministic package/provenance harness covers dependency-disconnected source configuration, secure accepted-binary packaging (explicitly unqualified), default private output, staged allowlist including notices/provenance, checksum/extraction, source/dependency worktree and license-hash qualification rejection, architecture/dynamic-dependency gates, insecure and in-repository output rejection, version mismatch, and symlink no-clobber behavior:

```sh
scripts/run-tests.sh -R '^ava_release\.package_linux$'
```

Official candidate preparation uses the tested build, never the fresh-build packaging mode. In a clean checkout:

```sh
CC=gcc-13 CXX=g++-13 cmake --preset release-candidate -G Ninja -DCMAKE_CXX_SCAN_FOR_MODULES=OFF
scripts/build.sh --build-dir build-candidate --jobs 6
python3 scripts/release-candidate.py seal --build-dir build-candidate
scripts/run-tests.sh --build-dir build-candidate --jobs 6
# Run the exact 23 tmux and four PTY gates in CI with zero skips.
scripts/package-linux.sh --build-dir "$PWD/build-candidate" --require-release-qualified \
  --output-dir /absolute/private/output
python3 scripts/release-candidate.py record --build-dir build-candidate --directory /absolute/private/output
python3 scripts/inspect-linux-artifact.py /absolute/private/output/ava-1.0.0-linux-x64.tar.gz
scripts/smoke-linux-minimum-host.sh /absolute/private/output "$PWD/build-candidate/tests/ava_fake_provider_server"
```

The seal binds the configured commit and executable/helper digests before tests. Packaging checks it before and after staging; the manifest proves the archived executable is identical. The `release-candidate`/`verify-promotion` CI jobs add same-commit sanitizer/TSan dependencies, exact 23+4 zero-skip checks, 90-day archive/checksum/evidence retention, and independent re-download verification. Local checks cannot substitute for the retained GitHub run.

Independent inspection remains useful:

```sh
tar -tzf /absolute/output/ava-X.Y.Z-linux-ARCH.tar.gz
(
  cd /absolute/output
  sha256sum -c ava-X.Y.Z-linux-ARCH.tar.gz.sha256
)
```

## Session quarantine policy

Leased recovery may create one sibling `<session>.jsonl.torn-tail.<unique>.bin` file with exact mode `0600`. It contains only the exact strict-JSON-invalid unterminated suffix. Recovery first writes and syncs a non-final temporary sibling, atomically publishes a unique final quarantine without overwrite, syncs the anchored session directory, and only then truncates the verified source inode. Prepublication failure cleans the temporary name and leaves the source byte-for-byte unchanged. A namespace change after publication aborts truncation and preserves the completed quarantine; the error reports its path.

Quarantines are operator recovery aids and are never replayed or packaged automatically. Stop AVA writers and preserve both files before investigation. Never append a quarantine after newer records. Reconstruct the previous bytes only in a separate offline copy, repair and strictly validate that copy, and retain the originals until verification is complete. See [`development/session-versioning.md`](../development/session-versioning.md) and [`session-format.md`](../session-format.md).

## Final local checks

```sh
shellcheck scripts/package-linux.sh  # when installed
python3 -m py_compile scripts/verify-markdown-links.py scripts/publish-linux-artifacts.py scripts/generate-release-provenance.py tests/package_linux_tests.py tests/release_provenance_tests.py
git --no-pager diff --check
```

Format changed C++ with the repository `clang-format`. Record exact commands, pass/skip results, and environment blockers; do not report checks that were not run.

## Work outside this local checklist

Exact-byte CI retention is implemented but still requires a successful retained run before the first release; official publication remains separate. Follow `AVA-REL-011` through `AVA-REL-013` in the [release ledger](https://github.com/Artificial-Source/AVA/blob/develop/docs/product/release-readiness.md). Post-release P2/P3 work includes deterministic archives, SBOM/signing/attestation policy, CPack/distribution formats, cross-build matrices, runtime bundling, registries/package managers, and desktop artifacts.

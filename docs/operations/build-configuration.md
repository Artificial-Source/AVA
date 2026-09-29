# Build configuration reference

This document describes the current CMake surface. For prerequisites and normal workflows, see [CONTRIBUTING.md](../development/contributing.md); for test selection, see [TESTING.md](testing.md).

## Presets

CMake 3.27+ reads `CMakePresets.json` (version 6); 3.27 is required because current CTests use timeout signal/grace properties introduced in that release. The `dev` and `sanitize` presets are canonical. Python 3 is required for the complete test/documentation/package gates, and debug-enabled libcwd print-member generation requires JSON-capable Universal Ctags. Prepare a writable `GITACHE_ROOT`, configure with `cmake --preset NAME`, then use the repository wrappers so build and test operations share the per-tree lock:

```sh
export GITACHE_ROOT="${GITACHE_ROOT:-$HOME/.cache/ava/gitache}"
mkdir -p "$GITACHE_ROOT"
cmake --preset dev
scripts/build.sh --build-dir build
scripts/run-tests.sh --build-dir build
```

Direct configuration is noncanonical unless it supplies the cache-equivalent `BetaTest`, `EnableDebug=ON`, `EnableAvaBuildTests=ON`, compile-command, and sanitizer settings listed in [contributing](../development/contributing.md).

| Preset | Binary directory | Key cache settings |
| --- | --- | --- |
| `dev` | `build` | `EnableAvaBuildTests=ON`, `CMAKE_BUILD_TYPE=BetaTest`, `EnableDebug=ON`, `CMAKE_EXPORT_COMPILE_COMMANDS=ON` |
| `sanitize` | `build-sanitize` | Inherits `dev`; `EnableAvaSanitizers=ON` |
| `tsan` | `build-tsan` | Inherits `dev`; `AVA_ENABLE_TSAN=ON` |
| `release` | `build-release` | Tests on; `CMAKE_BUILD_TYPE=Release` |
| `desktop-qml` | `build-desktop-qml` | `AVA_BUILD_DESKTOP_QML=ON`, tests off, compile commands on |

There are corresponding build presets. Test presets are `dev`, `sanitize`, `release-rpc`, `sanitize-rpc`, `release-acp`, `sanitize-acp`, and `tsan`; the latter intentionally runs a focused race-sensitive subset.

## AVA cache options

Pass cache values at configure time, for example `cmake --preset dev -DAVA_ENABLE_GITACHE=OFF`.

| Option | Default | Effect |
| --- | --- | --- |
| `AVA_ENABLE_GITACHE` | `ON` | Fetches/enables gitache-managed developer dependencies. With it off, required dependency provisioning must already be available. |
| `EnableAvaBuildTests` | Compatibility-derived | Canonical AICxx/cwds option that builds AVA tests. |
| `AVA_BUILD_TESTS` | `ON` | Compatibility alias/default input for `EnableAvaBuildTests`. An explicitly supplied `EnableAvaBuildTests` wins. |
| `EnableAvaSanitizers` | Compatibility-derived | Canonical AICxx/cwds option: AddressSanitizer plus UndefinedBehaviorSanitizer. |
| `AVA_ENABLE_SANITIZERS` | `OFF` | Compatibility alias/default input for `EnableAvaSanitizers`; the canonical option wins when explicitly set. |
| `AVA_ENABLE_TSAN` | `OFF` | Applies ThreadSanitizer to project-built objects for focused coordinator/ACP race coverage. It is mutually exclusive with ASan/UBSan. |
| `AVA_BUILD_DESKTOP_QML` | `OFF` | Builds the experimental Qt Quick/QML desktop prototype under `src/ava/desktop`. |
| `AVA_DEBUG_MAXLEN` | `100` | Numeric default debug-stream string truncation length; `0` means unlimited. It generates `config::ava_debug_maxlen_c`. |
| `AVA_REQUIRE_ACP_SDK_INTEROP` | `OFF` | Requires Node and installed official SDK packages at configure/test time; fails rather than skipping when unavailable. |
| `AVA_ENABLE_ACPX_INTEROP` | `OFF` | Enables the opt-in acpx interoperability smoke. It requires Python and the pinned Node setup. |

`AVA_NODE_EXECUTABLE` is a discovered CMake cache entry used only by ACP interop tests; normally do not set it. The two ACP options only exist when tests are configured. See [acp.md](../acp.md) and [TESTING.md](testing.md); they are not production features.

## Relevant AICxx/cwds options and build types

AVA imports AICxx through `cmake/aicxx/Project` and uses its `cw_option` mechanism for `EnableAvaBuildTests` and `EnableAvaSanitizers`. The preset also sets `EnableDebug=ON`; that is an AICxx debugging option, not an AVA alias. Do not replace canonical `EnableAva*` names in new automation with the compatibility names.

The configured AICxx build-type validation accepts:

`Release`, `Debug`, `RelWithDebInfo`, `BetaTest`, `RelWithDebug`, `Perf`, `Tracy`, and `None`.

`BetaTest` is the normal developer preset: optimized developer build with assertions/tests as configured. `Release` is the release preset. Select one through `CMAKE_BUILD_TYPE` for single-config generators; do not assume an unlisted CMake build type will be accepted. AVA itself requires C++23, disables compiler extensions, and treats warnings as errors (with its documented GCC maybe-uninitialized exception).

## Generators, parallelism, and compiler caches

Ninja is the recommended generator. Enable compile commands for clangd/clang-tidy with `-DCMAKE_EXPORT_COMPILE_COMMANDS=ON` (already set by `dev`). A compiler launcher is configured once, not exported as a runtime feature:

```sh
cmake --preset dev -DCMAKE_CXX_COMPILER_LAUNCHER=sccache
# or: -DCMAKE_CXX_COMPILER_LAUNCHER=ccache
```

Use a writable `SCCACHE_DIR` or `CCACHE_DIR` if the default cache location is unsuitable. Cache entries are naturally separated by compiler flags, so configure sanitizer and normal trees independently.

Use `scripts/build.sh` and `scripts/run-tests.sh`, optionally `--jobs N`. The wrappers honor `CMAKE_BUILD_PARALLEL_LEVEL` and `CTEST_PARALLEL_LEVEL`, choose a positive logical-core count otherwise, and serialize work with a per-build-tree lock. Do not run a build and test concurrently in one tree. A native `cmake --build ... --parallel N` does not make CTest parallel; use the test wrapper or `ctest --parallel N`.

## Platform and dependency boundaries

The maintained build is a Linux C++23 terminal application with wide ncurses, Boost, CMake, and Gitache/AICxx dependencies. Linux-specific containment, descriptor handling, and package/test paths are not portable promises. The QML target is explicitly experimental.

| Combination | Classification |
| --- | --- |
| Ubuntu 24.04.4 x64, GCC 13.3, Unix Makefiles, `BetaTest`, debug enabled | Tested: configure/build/full CTest passed on 2026-08-23 |
| Ubuntu 24.04.4 x64, GCC 13.3, Ninja, `Release`, debug disabled | Tested additional configuration: configure/build/full CTest/version passed |
| Clang 18 | Best-effort and environment-blocked during the audit; not supported/qualified |
| MSVC, Windows, macOS | Unsupported and untested |
| AArch64 | No accepted native exact-candidate evidence for the first publication |
| Multi-config generators | Not release-qualified; build/output/package configuration selection remains unproven |

The first official publication target is Linux x64 only. The audited x64 artifact requires BMI2, `GLIBC_2.38`, `GLIBCXX_3.4.32`, `CXXABI_1.3.13`, `libncursesw.so.6`, `libtinfo.so.6`, and `curl`. Only architectures with native exact-candidate evidence may publish. Dirty working trees are always unqualified.

CMake's conditional MSVC branches do not imply support. A `--require-release-qualified` package or `release_qualified:true` provenance field proves only the implemented static source/gitlink/license/version/native-architecture/dynamic-dependency/package gates, not CTest, native CI, terminal gates, exact-byte retention, or publication. Complete support/qualification comes from the [release ledger](../product/release-readiness.md) and [publication runbook](publication.md).

For a clean source checkout, dependency pins/submodules and static provenance rules remain authoritative; see [contributing](../development/contributing.md), [release checklist](release-checklist.md), and `THIRD_PARTY_NOTICES.md`.

# AVA Contributing

For the complete CMake option reference, see [build-configuration.md](../operations/build-configuration.md). Documentation changes follow [documentation.md](documentation-policy.md), including the offline source-link gate.

For the community entry point and project policies, see the root [`CONTRIBUTING.md`](../../CONTRIBUTING.md). Please read the [`Code of Conduct`](../../CODE_OF_CONDUCT.md), [`Governance`](../../GOVERNANCE.md), [`Security`](../../SECURITY.md), and [`Support`](../../SUPPORT.md) guidance before participating. Use private security reporting for suspected vulnerabilities; do not open public issues with exploit details or secrets.

## Prerequisites

- CMake 3.27 or newer; current CTests use timeout signal/grace properties introduced in 3.27.
- A C++23 compiler. GCC 13 is the tested Ubuntu 24.04 x64 compiler; other compilers are best-effort until the matrix below passes.
- `ctest` from CMake and Python 3 for the complete registered test, documentation, install, and package gates.
- Boost development headers and CMake package (`boost-devel` on Fedora).
- Wide-character ncurses development headers/library (`ncurses-devel` on Fedora).
- A writable `GITACHE_ROOT` before configuring the canonical non-Release presets.
- JSON-capable Universal Ctags when debug/libcwd print-member generation is enabled.
- `clang-format` version 22 or newer.
- `clang-tidy` when touching core logic or safety-sensitive paths.
- Optional `ccache` or `sccache` for faster repeated compilation.
- Internet access to `github.com` is required during configuration when pinned sources are not already present.

## Cloning the repository

```sh
git clone --branch develop --single-branch --recurse-submodules https://github.com/Artificial-Source/AVA.git
cd AVA
```

A recursive clone is ready for the CMake quick start below. For an older nonrecursive clone, run `git submodule update --init --checkout --recursive`. `./autogen.sh` is optional maintainer convenience: it initializes missing submodules at AVA's pinned commits, sets `push.recurseSubmodules` when missing, and prints build guidance. It does not configure or build AVA and is not required after the recursive clone command.

The canonical `dev`, `sanitize`, and `tsan` presets enable debug instrumentation. Before configuring them, set `GITACHE_ROOT` to an existing writable directory where Gitache packages can be compiled. The current requirement is approximately 50 MB. For example:

```sh
export GITACHE_ROOT="${GITACHE_ROOT:-$HOME/.cache/ava/gitache}"
mkdir -p "$GITACHE_ROOT"
```

When libcwd/debug print-member generation is enabled, configuration also requires Universal Ctags with JSON output support; Exuberant Ctags is insufficient. Python 3 is required for the complete debug-enabled test registration.

## Quick Start (build and test)

After preparing `GITACHE_ROOT` above, use the canonical developer preset:

```sh
cmake --preset dev
scripts/build.sh
scripts/run-tests.sh
```

Canonical sanitizer pass:

```sh
cmake --preset sanitize
scripts/build.sh --build-dir build-sanitize --jobs 2
scripts/run-tests.sh --build-dir build-sanitize --jobs 2
```

A direct configure is noncanonical unless it is cache-equivalent to the preset. The equivalent fallback commands are:

```sh
cmake -S . -B build \
  -DEnableAvaBuildTests=ON \
  -DCMAKE_BUILD_TYPE=BetaTest \
  -DEnableDebug=ON \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON

cmake -S . -B build-sanitize \
  -DEnableAvaBuildTests=ON \
  -DEnableAvaSanitizers=ON \
  -DCMAKE_BUILD_TYPE=BetaTest \
  -DEnableDebug=ON \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
```

Use the repository build/test wrappers after either direct configuration. Do not describe a Release-default direct configure as equivalent to `dev` or `sanitize`.

The repository build and test runners detect the available logical cores and pass an explicit positive parallel level to CMake/CTest. Use `--jobs N`, `CMAKE_BUILD_PARALLEL_LEVEL=N`, or `CTEST_PARALLEL_LEVEL=N` to cap it; build options such as `--target` and CTest options such as `-R` are forwarded. The runners share one build-tree lock because concurrent builds/tests and several fixed integration-test roots are unsafe. Sanitizer examples use two jobs to limit memory pressure.

Parallel jobs speed clean builds; a compiler cache speeds repeated builds. To enable an installed cache, configure once with `cmake --preset dev -DCMAKE_CXX_COMPILER_LAUNCHER=ccache` (or `sccache`). Compiler flags keep sanitizer and ordinary cache entries separate. The pinned AICxx configuration also selects `ccache` when `CCACHE_DIR` is set; in that mode an installed `ccache` is required and takes precedence over a different launcher. Leave `CCACHE_DIR` unset when selecting `sccache`.

For repeated developer builds with ccache:

```sh
export CCACHE_DIR="${CCACHE_DIR:-$HOME/.cache/ava/ccache}"
mkdir -p "$CCACHE_DIR"
cmake --preset dev -G Ninja
ccache -z
scripts/build.sh --build-dir build --jobs 6
ccache -s
scripts/run-tests.sh --build-dir build --jobs 6
```

Keep ccache's strict defaults: do not enable sloppiness or disable compiler/header checks. `ccache -s` distinguishes cold compiles from direct/preprocessed hits; a cold object cache still compiles every translation unit. Reusing compiler objects does not reduce configure, no-op, or test execution time and does not change assertions, instrumentation, test selection, or timeouts. To measure an uncached baseline in the same configured tree, run the build with `CCACHE_DISABLE=1`. To remove the launcher entirely, unset `CCACHE_DIR` and reconfigure with `-DCMAKE_CXX_COMPILER_LAUNCHER=`.

Tests currently build into `tests/ava_tests`, registered as focused `ava_tests.<suite>` CTests from sources under `tests/`. LSP coverage uses the `ava_fake_lsp_server` support executable.

## Tested build matrix

| Combination | Status | 2026-08-23 evidence |
| --- | --- | --- |
| Ubuntu 24.04.4 x64, GCC 13.3, Unix Makefiles, `BetaTest`, debug enabled | Tested | Configure, build, and full CTest passed |
| Ubuntu 24.04.4 x64, GCC 13.3, Ninja, `Release`, debug disabled | Tested additional build | Configure, build, full CTest, and version check passed |
| Clang 18 on the audit host | Best-effort; environment-blocked | Scanner/default GCC 16 interaction and Clang/libstdc++ C++23 `std::expected` combinations did not produce a qualifying build |
| MSVC, Windows, macOS | Unsupported | No native clean build/test evidence |
| AArch64 | Not qualified for the first publication | No native exact-candidate evidence in the frozen audit |
| Multi-config generators | Not release-qualified | Configuration/output/package selection is not proven configuration-safe |

Only Linux x64 is targeted for the first official publication, and dirty working trees are always unqualified. Candidate builds target **x86-64-v2 (no BMI2/AVX)** with Ubuntu 24.04/glibc 2.39 as the minimum host, libstdc++ exposing `GLIBCXX_3.4.32`/`CXXABI_1.3.13`, ncursesw/tinfo ABI 6 (6.4+), terminfo, and `curl` 8.5+. Inspection caps binary symbol requirements at `GLIBC_2.38`, `GLIBCXX_3.4.32`, and `CXXABI_1.3.13`; publication still requires minimum-host smoke of the exact retained bytes. See [build configuration](../operations/build-configuration.md) and the [release-readiness ledger](../product/release-readiness.md).

## Configuration

Lets assume that, next to `GITACHE_ROOT` (see above), the following environment variables are set:
* `REPOROOT` : full path of the repository root of ava (i.e. the directory that you cloned ava into.
* `BUILDDIR` : full path to the build directory; this can be inside or outside the repository root (e.g. `$REPOROOT/build` is fine) but should be an empty or non-existent directory.
* `CMAKE_CONFIG` : the CMake build type. Accepted values are `Release`, `Debug`, `RelWithDebInfo`, `BetaTest`, `RelWithDebug`, `Perf`, `Tracy`, and `None`, as enforced by `cmake/aicxx/cmake/CW_OPTIONS.cmake`.

Then one can configure AVA with:

```sh
cmake -S "$REPOROOT" -B "$BUILDDIR" -DCMAKE_BUILD_TYPE="$CMAKE_CONFIG" [OPTIONS]
```

where **`OPTIONS`** is one or more of the following:

* `-GNinja` : use ninja instead of make (highly recommended)
* `--log-level=NOTICE` : reduce CMake output to notices, warnings and errors. Possible values are `ERROR`, `WARNING`, `NOTICE`, `STATUS` (default), `VERBOSE`, `DEBUG`, and `TRACE`.
  As a developer you should use the default (STATUS) or more verbose, otherwise the CMake options are not shown.
* `-DCMAKE_CXX_COMPILER_LAUNCHER=sccache` (or `ccache`) : use an installed compiler cache to speed up recompilations; set `SCCACHE_DIR` or `CCACHE_DIR` when a non-default writable cache directory is needed.
* `-DAVA_BUILD_TESTS=ON` : to compile the testsuite of ava.
* `-DCMAKE_EXPORT_COMPILE_COMMANDS=ON` : generate the `compile_commands.json` compilation database, useful for tools that need the exact compiler command line for each source file.
  Typical tools that use it are:
  - `clangd`, used by editors and IDEs for C/C++ language-server features such as code completion, diagnostics, go-to-definition and include resolution.
  - `clang-tidy`, for static analysis and automated lint/fix checks.
  - other Clang-based tooling, including custom LibTooling tools, refactoring tools, include analyzers and source indexers.

Provided the `--log-level` is `STATUS` other options are printed, using colors, during configuration.
For example:
```
-- Option EnableDebug (Build for debugging) =
        ON (default)
```
If everything is green then you are using the defaults, which is usually the best.
If an option is printed in red then it was manually overridden and different from the default.
For example, using `-DCMAKE_BUILD_TYPE=Debug` will print
```
-- Option CMAKE_BUILD_TYPE =
        Debug
```
where `Debug` is in red, because the default is `Release`.

## Building

After (re)configuration AVA can be build by issuing the command:
```sh
scripts/build.sh --build-dir "$BUILDDIR" --jobs "$CPUS" --config "$CMAKE_CONFIG" [--verbose]
```

Adding `--verbose` shows the exact commands that are being executed by ninja;
mostly useful if one want to inspect if the expected compiler arguments are being passed to the compiler.

Note that the `--config "$CMAKE_CONFIG"` is only needed for multi-target generators, so not for `Makefile` or `Ninja`.
`CPUS` should be set to the number of processors that you want to use for compilation. For example, use `CPUS=$(nproc --all)`.

## Formatting And Static Checks

Format changed C++ with the repository `.clang-format`:

```sh
clang-format -i <changed-cpp-or-header-files>
```

Run clang-tidy against files you changed after configuring the build:

```sh
clang-tidy <changed-cpp-files> -p build
```

Verify that every `ASSERT` under `src/ava/` keeps its required immediately
preceding comment, both directly and through its two focused CTests:

```sh
python3 scripts/verify-assert-comments.py .
scripts/run-tests.sh --build-dir build \
  -R '^ava_tests\.assert_comments_(checker|source)$' \
  --output-on-failure
```

For Markdown changes, run both direct repository gates and their four focused
CTest cases:

```sh
python3 scripts/verify-markdown-links.py . --source-tree
python3 scripts/verify-documentation-structure.py .
scripts/run-tests.sh --build-dir build \
  -R '^ava_tests\.(markdown_(link_verifier|links_source)|documentation_structure_(checker|source))$' \
  --output-on-failure
```

When documentation paths or release-artifact payloads change, also run the
offline package, install, and provenance checks:

```sh
scripts/run-tests.sh --build-dir build \
  -R '^ava_release\.(provenance|install_component|package_timeout_cleanup)$' \
  --output-on-failure
scripts/run-tests.sh --build-dir build-release \
  -R '^ava_release\.package_linux$' --output-on-failure
```

On Linux, `ava_release.package_linux` is registered even in developer builds, but skips with exit 77 when libcwd is enabled (including canonical `dev`/`EnableDebug` builds). Use the Release tree for real packaging coverage. `ava_release.package_timeout_cleanup` independently checks CTest timeout cleanup. In libcwd builds, `ava_release.install_component` still checks every installed file and skips only the installed-binary version probe.

Always finish with:

```sh
git --no-pager diff --check
```

## Review Expectations

- Keep changes small and subsystem-owned.
- Preserve backend permission boundaries for file writes and process execution.
- Add focused regression tests for safety-sensitive changes.
- For plugin or MCP contract changes, follow [`docs/plugin-compatibility-policy.md`](../plugin-compatibility-policy.md) and update deterministic golden fixtures when stable serialized shapes intentionally change.
- Do not include `build*/` trees or `docs/reference-code/` repositories in reviews.

## GitHub Actions dependencies

Third-party GitHub Actions must be pinned to a full 40-character commit SHA with an exact release-tag comment (for example `# v7.0.1`). Before bumping an action, verify the official tag-to-SHA mapping and the action's release/runner requirements, then update every `uses:` occurrence in the workflow. GitHub-hosted runners are the current CI baseline; Node 24-based action majors require Actions Runner `>= 2.327.1`. Never replace a pinned SHA with a mutable tag such as `@v7`. Dependabot opens weekly GitHub Actions update PRs against `develop`, but human review of the pin, comment, and runner fit remains required.

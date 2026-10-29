# AVA @AVA_VERSION@

AVA is a native terminal coding assistant. This release provides version
@AVA_VERSION@ for Linux x64.

Source commit: `@AVA_COMMIT@`

## What is included

- A terminal editor with file completion, prompt history, model and session
  pickers, Markdown and diff rendering, themes, and permission dialogs.
- OpenAI, Anthropic, Gemini, and OpenAI-compatible provider paths, local model
  configuration, reasoning controls, and mid-session model switching.
- Resumable local sessions, branches, forks, compaction, image attachments, usage
  records, and local Markdown, HTML, and JSONL export.
- File, search, shell, and web tools; local instructions and skills; configurable
  agents, task subagents, and background jobs.
- Local plugins, stdio MCP servers, saved-file LSP queries, print mode, local
  JSONL RPC v1, and a separate ACP endpoint for editor clients.
- Persistent operation permissions and command containment. Deny rules take
  precedence. Tool calls truncated by a provider token limit do not run.

The [changelog](https://github.com/Artificial-Source/AVA/blob/@AVA_COMMIT@/CHANGELOG.md)
has the full summary. Review the
[product principles](https://github.com/Artificial-Source/AVA/blob/@AVA_COMMIT@/docs/product/principles.md)
and [release ledger](https://github.com/Artificial-Source/AVA/blob/@AVA_COMMIT@/docs/product/release-readiness.md)
for the scope and qualification record.

## Supported platform

Supported artifact: **Linux x64 only, x86-64-v2 (Nehalem-class or newer; no
BMI2 or AVX requirement)**. Minimum qualified host: Ubuntu 24.04, glibc 2.39,
libstdc++ providing `GLIBCXX_3.4.32` and `CXXABI_1.3.13`, ncursesw/tinfo ABI 6
(6.4+), a usable terminfo database, and `curl` 8.5+ on `PATH`.

Binary symbol requirements do not exceed `GLIBC_2.38`, `GLIBCXX_3.4.32`, or
`CXXABI_1.3.13`. The loader is `/lib64/ld-linux-x86-64.so.2`. Host libraries
`libc.so.6`, `libm.so.6`, `libgcc_s.so.1`, `libstdc++.so.6`, `libncursesw.so.6`,
and `libtinfo.so.6` are required. No Windows, macOS, AArch64, or other
distribution artifact is qualified. Python and CMake are not AVA runtime
requirements. CPU-floor testing used a Nehalem CPU profile, not physical
Nehalem hardware or an independently booted minimum kernel.

## Download, verify, and install

Download `ava-@AVA_VERSION@-linux-x64.tar.gz` and
`ava-@AVA_VERSION@-linux-x64.tar.gz.sha256` from this release's assets. In a new
private directory containing both files, run:

```sh
sha256sum --check --strict ava-@AVA_VERSION@-linux-x64.tar.gz.sha256
tar -xzf ava-@AVA_VERSION@-linux-x64.tar.gz
./ava-@AVA_VERSION@-linux-x64/bin/ava --version
./ava-@AVA_VERSION@-linux-x64/bin/ava --help
./ava-@AVA_VERSION@-linux-x64/bin/ava doctor
```

The version must be `ava @AVA_VERSION@`. Missing optional credentials or local
tools may cause doctor warnings. Read the included `share/doc/ava/README.md`,
then add the extracted `bin` directory to your `PATH` if desired. Keep the rest of
the extracted directory for its license notices and runtime guides.

Approved SHA-256 values:

```text
@AVA_ARCHIVE_SHA256@  ava-@AVA_VERSION@-linux-x64.tar.gz
@AVA_CHECKSUM_SHA256@  ava-@AVA_VERSION@-linux-x64.tar.gz.sha256
@AVA_MANIFEST_SHA256@  candidate.json (retained CI record; not a release asset)
```

Checksums detect changed downloads. They are not independent proof of origin.
This release has no detached signatures, SBOM, or artifact attestations.

## Known limitations

- Not every provider/model has a live qualification result. OpenAI and
  Kimi-for-coding have recorded live prompt evidence. Several other paths have
  offline tests or still need credentials for live tests. Usage and cost
  estimates can be incomplete.
- Containment is not an OS or container sandbox guarantee. Local extensions
  run code. Review permissions and extension sources before granting access.
- Background jobs and pending delivery stop with the AVA process. They do not
  recover after restart. Child session files remain available for inspection.
- MCP uses local stdio only. LSP reads saved files, not unsaved editor buffers.
  Ordinary tools run sequentially by default. Only print/RPC have an opt-in
  for bounded parallel read/search calls.
- JSONL export does not include attachment bytes. Back up local session and
  attachment files before changing installations. Compatibility with
  development snapshots is not a release promise.
- There is no cloud sharing, remote plugin installer, package marketplace,
  automatic update, remote catalog refresh, or telemetry. No desktop client or
  web service is part of this release.
- Terminal tests do not certify screen-reader access or all physical terminals.
  LaTeX and arbitrary extension renderers are not included.

## Report a problem

Search [existing issues](https://github.com/Artificial-Source/AVA/issues), then
open a bug report. Include `ava --version`, the commit above, your Linux and
terminal versions, CPU, the archive checksum, the command you ran, expected
behavior, and redacted logs. Do not upload credentials, private session data,
or proprietary source. The
[support guide](https://github.com/Artificial-Source/AVA/blob/@AVA_COMMIT@/SUPPORT.md)
and [troubleshooting guide](https://github.com/Artificial-Source/AVA/blob/@AVA_COMMIT@/docs/operations/troubleshooting.md)
can help narrow the problem.

Report suspected vulnerabilities privately using the
[security policy](https://github.com/Artificial-Source/AVA/blob/@AVA_COMMIT@/SECURITY.md),
not a public issue. The publication policy names any supported release line.
Corrections ship as a new patch. Old tags and assets are never silently
replaced. See the
[publication policy](https://github.com/Artificial-Source/AVA/blob/@AVA_COMMIT@/docs/operations/publication.md).

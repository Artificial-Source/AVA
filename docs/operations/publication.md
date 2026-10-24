# AVA Official Publication Runbook and Design

**Status: required for the first official release, but not yet an implemented publish workflow.** This page defines the operator lifecycle that must be reviewed and dry-run before publication. The 2026-08-23 audit did not run a public publish command, create a tag, or create a release. Current blockers are authoritative in the [release-readiness ledger](../product/release-readiness.md).

## Roles and invariants

- One operator prepares the candidate packet; an independent approver verifies scope, exact bytes, release notes, and supported-platform claims.
- The published tag, release commit, release body, archive/checksum names, and retained CI artifact digests must agree exactly.
- Build once, test those bytes, retain those bytes, and promote those bytes. Publication never rebuilds.
- Draft creation and asset staging must be no-clobber. Existing tag, release, archive, checksum, or conflicting version aborts the operation.
- A published version is immutable. Corrections use a new patch version; withdrawal adds warnings/removes discoverability where the hosting platform permits but never silently replaces bytes.

## 1. Select the candidate

1. Confirm the release ledger names the candidate version, commit, supported architecture, and `READY` decision with all blockers closed.
2. Require a clean exact candidate source tree and matching initialized gitlinks for CI. A developer's dirty tree, even one that passed local tests, is not a candidate identity.
3. Confirm the top-level CMake version and the retained executable's exact `ava X.Y.Z` output agree.
4. Confirm the candidate is on the approved release ancestry and no tag or official release already exists for the version.
5. Record the exact native CI run and retained artifact IDs. For the first publication, the only expected platform is Linux x64 unless the ledger has admitted another natively evidenced architecture.

## 2. Prepare the changelog and release body

The reviewed release packet must contain:

- version, tag name, exact commit, date, and supported artifact name;
- a concise user-focused change summary derived from reviewed commits, not from an unfiltered diff dump;
- security and data-authority changes, including fixed release blockers where disclosure is safe;
- supported host/CPU/runtime floor and dynamic dependencies;
- installation/extraction and checksum-verification instructions;
- known limitations, expected skips, deferred P2/P3 work, and unsupported platforms;
- upgrade/session compatibility notes and any required backups;
- links to the current principles, release ledger, testing guide, artifact checklist, security guidance, and this runbook;
- an explicit statement that telemetry, automatic updates, package-manager publication, signing, SBOM, and attestations are absent when they remain absent.

The approver compares the release body against `CHANGELOG.md`, the exact candidate diff, the retained artifact's `PROVENANCE.json`, and the platform evidence. Review must remove private paths, credentials, raw captures, provider payloads, internal evidence roots, and claims based only on static competitor inspection.

Use this support paragraph verbatim in the reviewed release body (then add the reviewed version, commit, changes, limitations and artifact digests):

> Supported artifact: **Linux x64 only, x86-64-v2 (Nehalem-class or newer; no BMI2 or AVX requirement)**. Minimum qualified host: Ubuntu 24.04, glibc 2.39, libstdc++ providing `GLIBCXX_3.4.32` and `CXXABI_1.3.13`, ncursesw/tinfo ABI 6 (6.4+), a usable terminfo database, and `curl` 8.5+ on `PATH`. Binary symbol requirements do not exceed `GLIBC_2.38`, `GLIBCXX_3.4.32`, or `CXXABI_1.3.13`; loader `/lib64/ld-linux-x86-64.so.2` and host `libc.so.6`, `libm.so.6`, `libgcc_s.so.1`, `libstdc++.so.6`, `libncursesw.so.6`, `libtinfo.so.6` are required. No Windows, macOS, AArch64, or other distribution artifact is qualified. Python/CMake are not AVA runtime requirements. No automatic updates, signing, SBOM, or attestation is provided.

## 3. Tag policy

- Official release tags use `vX.Y.Z` and point directly to the approved candidate commit.
- A tag is created only after the release packet and retained-byte evidence are approved.
- Annotated tag text identifies the version and release, without embedding private logs or credentials.
- Existing tags are never moved or force-replaced. Any remote/local pre-existence or commit mismatch aborts.
- The tag and draft release remain non-public until asset validation and final approval complete.

This design deliberately does not provide a live tag/push command. The eventual implemented workflow must expose a reviewable draft step and a separately authorized publication step.

## 4. Exact retained CI artifacts

For each supported architecture, retain exactly:

```text
ava-X.Y.Z-linux-ARCH.tar.gz
ava-X.Y.Z-linux-ARCH.tar.gz.sha256
```

The release CI record must establish that the executable inside this archive is the one that passed the complete candidate gate: full deterministic CTest, canonical sanitizer evidence, focused TSan, all 23 tmux and four PTY gates, install/provenance/package checks, checksum/extraction, version/help/doctor, and fake-provider smoke. Retention duration must cover approval, publication, post-publication verification, and the project's incident window.

Before staging, download the retained pair into a new mode-0700 operator directory outside the checkout, verify the CI-recorded digests, verify the adjacent checksum, inspect the exact member allowlist, and extract into a new private directory. Never accept a locally rebuilt substitute.

Implementation: `.github/workflows/ci.yml` runs `release-candidate` on manual dispatch (`release_candidate=true`) and `v*` tags, not routine pushes/PRs. This avoids duplicating the costly complete terminal/container/package wave on every edit; routine CI still runs deterministic tests and its small terminal selection. GCC 13 on Ubuntu 24.04 fixes the runtime floor independently of newer developer compilers. The job depends on successful `sanitizer` and `tsan` jobs from the same workflow commit, builds the final binary once, and packages its sealed build through `--build-dir`. Nothing in promotion invokes CMake/build.

Retained artifacts are `linux-x64-candidate-RUN-ATTEMPT` and `linux-x64-evidence-RUN-ATTEMPT`, each with **90-day retention**. The candidate artifact contains the unchanged archive/checksum and `candidate.json` with archive/checksum/executable SHA-256 values, source revision and member inventory. The run summary independently records the manifest SHA-256 and same-run sanitizer/TSan conclusions. Evidence contains JUnit, binary inspection and container smoke logs. `verify-promotion` downloads that exact artifact and verifies the manifest digest supplied by the producing job plus all enclosed digests/commit/provenance, then runs extracted version/help. Its summary records the matching before/after values. It does not tag or publish.

For a later operator promotion, copy the full revision and manifest SHA-256 from the approved run summary, not from the download itself. In a new private directory outside the checkout:

```sh
gh run download RUN --name linux-x64-candidate-RUN-ATTEMPT --dir /absolute/private/download
python3 scripts/release-candidate.py verify --directory /absolute/private/download \
  --revision FULL_APPROVED_COMMIT --manifest-sha256 APPROVED_MANIFEST_SHA256
python3 scripts/inspect-linux-artifact.py /absolute/private/download/ava-1.0.0-linux-x64.tar.gz
```

Verify that `release-candidate`, `sanitizer`, `tsan`, and `verify-promotion` all succeeded for that same commit/run before draft staging. Expired artifacts require a new candidate qualification; never rebuild during promotion. This download/verify step is implemented, but the separate reviewed/no-clobber official draft lifecycle remains `AVA-REL-012`.

## 5. Draft, approve, and publish without clobber

1. Create a non-public draft associated with the approved tag/commit through the future reviewed workflow or hosting UI.
2. Stage only the exact retained archive/checksum pair. Reject duplicate names, symlinks, unexpected files, partial uploads, or any destination that already exists.
3. Render and review the release body in the hosting UI. Confirm supported-platform wording and known limits remain visible.
4. Have the independent approver re-check tag/commit identity, asset names/sizes/digests, checksum contents, and release body.
5. Record approval identity/time and the final remote object identifiers.
6. Publish through the separately authorized approval action. Do not rebuild, rename, edit bytes, or replace an existing asset during this step.

The repository currently has secure local no-replace publication primitives, but no official remote publication workflow. Those local primitives do not prove this lifecycle.

## 6. Checksums and authenticity status

SHA-256 checksum files are required and published adjacent to each archive. They protect transfer verification only; an unsigned checksum from the same location is not an independent authenticity proof.

Artifact attestations, a standard SPDX/CycloneDX SBOM, and detached signing are **not implemented for this first-publication audit**. They remain optional post-release hardening unless a downstream policy promotes them through the release ledger. If later enabled, their generated statements must bind the same retained digest and have documented offline/hosted verification; existing release bytes remain unchanged.

## 7. Post-publication verification

Use a clean machine or fresh isolated user environment that meets the published minimum floor:

1. Query the public tag/release and confirm tag-to-commit equality.
2. Download assets from the public release, not from CI or an operator directory.
3. Compare downloaded SHA-256 values with both the checksum file and pre-publication retained digests.
4. Inspect the member allowlist and documentation count, then extract to a new directory.
5. Run `bin/ava --version`, `bin/ava --help`, and passive `bin/ava doctor` under scrubbed private HOME/XDG roots.
6. Run the deterministic fake-provider packaged smoke without live credentials.
7. Open every release-body/documentation link and confirm the supported floor and limitations are visible.
8. Record PASS/FAIL and evidence identifiers. Any mismatch starts the rollback/withdrawal decision; never repair a published asset in place.

## 8. Correction, rollback, and withdrawal

### Before publication

A failed check cancels the draft. Delete only the unpublished draft/staged uploads through the hosting platform, preserve the evidence, fix the candidate, choose a new exact commit, and rerun complete qualification. Reusing the same version is allowed only while no public tag/release exists and policy owners confirm that no public observer could have consumed it.

### After publication

- **Documentation or code correction:** prepare a new patch version and repeat the full lifecycle. Do not move the old tag or replace assets.
- **Bad release with safe prior version:** mark the affected release prominently as withdrawn/unsafe, remove it from “latest” or package discovery where possible, direct users to the prior safe version, and publish a fixed patch after qualification.
- **Security issue:** follow private security response, coordinate disclosure timing, state affected/safe versions, revoke compromised credentials or signing material if applicable, and preserve forensic evidence privately.
- **Artifact mismatch or suspected compromise:** stop promotion immediately, make the release non-latest, warn against download/use, preserve remote identifiers/digests, and investigate before publishing any replacement patch.

Withdrawal does not erase history or mutate bytes. The release page should retain enough notice for users holding the old digest to understand its status.

## 9. Supported versions

Before the first publication, AVA has no officially supported released version. After publication, the project supports only the latest published `1.0.x` patch unless a later policy explicitly names another line. Security and support pages must identify affected and fixed versions precisely; an unreleased runtime number or development branch is not a supported release.

End-of-support, if introduced, requires a dated announcement, replacement path, and updates to release/security/support documentation. It cannot be inferred from a new runtime bump alone.

## Dry-run closure record

`AVA-REL-012` closes only after operators exercise this lifecycle without making a public release: select an exact retained candidate, prepare the complete release packet, verify no-clobber draft staging in an isolated test/draft surface, record independent approval, rehearse downloaded-asset verification, and walk one patch-correction plus one withdrawal scenario. The official publication then requires a separate explicit authorization.

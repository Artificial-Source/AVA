# AVA Official Publication Runbook

**Status: prepared; awaiting owner approval and GitHub run.** Local preparation
never authorizes publication. No command in the preparation tool contacts
GitHub. Only an owner may run the remote commands below. The current cut and
remaining gates are in the [release ledger](../product/release-readiness.md).

## Roles and invariants

- One operator prepares the packet. An independent owner reviews the exact
  commit, evidence, notes, support claims, and digests. Both record approval.
- Build once, test those bytes, retain those bytes, and promote those bytes.
  Never rebuild during download, draft staging, publication, or verification.
- Publish Linux x64 only. The qualified floor is Ubuntu 24.04, x86-64-v2
  (Nehalem-class; no BMI2 or AVX), glibc 2.39, libstdc++ with `GLIBCXX_3.4.32`
  and `CXXABI_1.3.13`, ncursesw/tinfo ABI 6 (6.4+), terminfo, and curl 8.5+.
  The [canonical release body](release-body-1.0.0.md) records the loader,
  libraries, symbol ceilings, and limitations. Do not add another architecture
  without native exact-candidate qualification and a new owner decision.
- Existing tags, releases (including drafts), or asset names stop creation.
  Never use force pushes, asset replacement, or `gh release upload --clobber`.
- A published version, tag, body, and assets are immutable. Corrections ship as
  a new patch. An incident notice may be appended to warn users, but must not
  hide the original identity or replace its assets.

## 1. Select and qualify the exact candidate

Review the [1.0 changelog](../../CHANGELOG.md),
[product principles](../product/principles.md),
[testing guide](testing.md), [artifact checklist](release-checklist.md), and
[security policy](../../SECURITY.md). Choose the final integrated source commit
before running CI. A later documentation commit is not the source identity of
an earlier packet. If the source changes, qualify a new packet.

The following are **owner commands**, not commands run by the local dry-run.
Set the values from the chosen commit and the producing run's independent
summary. Do not derive the approved manifest digest from the download itself.
Run these commands from the matching clean checkout with its pinned gitlinks.

```sh
export REPOSITORY=Artificial-Source/AVA
export REVISION=FULL_40_CHARACTER_APPROVED_SOURCE_COMMIT
export CANDIDATE_REF=OWNER_SELECTED_BRANCH_AT_THAT_COMMIT
export VERSION=1.0.0
export TAG=v1.0.0

# Owner: make the integrated branch available, then request exact-byte CI.
git push origin "$CANDIDATE_REF"
gh workflow run ci.yml --repo "$REPOSITORY" --ref "$CANDIDATE_REF" \
  -f release_candidate=true
gh run list --repo "$REPOSITORY" --workflow ci.yml --event workflow_dispatch \
  --commit "$REVISION" --limit 10

# Copy the actual run ID and attempt from that listing/run page.
export RUN=APPROVED_RUN_ID
export ATTEMPT=APPROVED_RUN_ATTEMPT
gh run watch "$RUN" --repo "$REPOSITORY" --exit-status
gh run view "$RUN" --repo "$REPOSITORY" \
  --json databaseId,headSha,attempt,conclusion,jobs,url
gh api "repos/$REPOSITORY/actions/runs/$RUN/artifacts" \
  --jq '.artifacts[] | {id,name,expired,expires_at,digest}'
```

Check `headSha == REVISION` and the approved attempt. Require successful
`sanitizer`, `tsan`, `release-candidate`, and `verify-promotion` jobs from that
same workflow commit and attempt, plus all other required CI jobs. Review the
full deterministic report, exactly 23 tmux and four PTY successes with zero
skips, install/provenance/package checks, inspection, and minimum-host smoke.
A historical run does not qualify new bytes.

Both retained artifacts last **90 days**:

- `linux-x64-candidate-RUN-ATTEMPT`: archive, adjacent checksum, `candidate.json`.
- `linux-x64-evidence-RUN-ATTEMPT`: JUnit reports, build identity, terminal
  evidence, ISA/library inspection, and minimum-host logs.

The run summary separately records `candidate.json` SHA-256, source commit,
archive/checksum/executable digests, and same-run sanitizer/TSan conclusions.
Record artifact IDs and expiry dates before approval. Expired or missing
artifacts require a new complete qualification run; promotion does not rebuild.
A tag push may trigger another candidate run. It must not replace the already
approved run or its bytes.

## 2. Download and prepare the release packet

Use a new mode-0700 directory outside the checkout. Preserve both artifacts
and the producing run summary privately through approval and the incident
window. Do not put credentials, private paths, raw provider payloads, or
unredacted logs in release notes.

```sh
export MANIFEST_SHA256=SHA256_FROM_APPROVED_PRODUCING_RUN_SUMMARY
export PACKET_ROOT=$(mktemp -d "${TMPDIR:-/tmp}/ava-publication.XXXXXXXX")
chmod 0700 "$PACKET_ROOT"
mkdir -m 0700 "$PACKET_ROOT/candidate" "$PACKET_ROOT/evidence"
gh run download "$RUN" --repo "$REPOSITORY" \
  --name "linux-x64-candidate-$RUN-$ATTEMPT" --dir "$PACKET_ROOT/candidate"
gh run download "$RUN" --repo "$REPOSITORY" \
  --name "linux-x64-evidence-$RUN-$ATTEMPT" --dir "$PACKET_ROOT/evidence"
python3 scripts/release-candidate.py verify --directory "$PACKET_ROOT/candidate" \
  --revision "$REVISION" --manifest-sha256 "$MANIFEST_SHA256"
python3 scripts/inspect-linux-artifact.py \
  "$PACKET_ROOT/candidate/ava-1.0.0-linux-x64.tar.gz" \
  --output "$PACKET_ROOT/inspection.json"
python3 scripts/release-candidate.py draft --dry-run \
  --directory "$PACKET_ROOT/candidate" --revision "$REVISION" \
  --manifest-sha256 "$MANIFEST_SHA256" --repository "$REPOSITORY" \
  --body docs/operations/release-body-1.0.0.md --output "$PACKET_ROOT/plan"
sha256sum "$PACKET_ROOT/plan/release-body.md" "$PACKET_ROOT/plan/draft-commands.sh"
```

`draft --dry-run` validates the private input directory, exact file set,
manifest/digests, checksum, safe member inventory, commit, packaged provenance,
version, release-body placeholders, and absence of the local tag. It extracts
and runs `bin/ava --version` with fresh HOME/XDG roots and no inherited
credentials. It creates a new private plan directory with exclusive writes,
renders the canonical body, and prints the owner commands. It never runs
`gh`, pushes a tag, creates a release, or builds anything. Reusing an output
directory fails without changing the existing plan.

An offline dry-run cannot prove that a remote name is free or that GitHub
retained the packet. The generated commands check remote tag absence and all
release/draft names immediately before staging. Authentication or API failures
stop the commands; they are not treated as absence. A tag or draft found there
requires investigation, not an overwrite or automatic resume.

The release body and changelog must agree on features, fixed safety issues,
support floor, provider live-test limits, process-local jobs, saved-file LSP,
local stdio MCP, attachment backups, and unsupported platforms. No telemetry,
automatic updates, package-manager publication, detached signing, SBOM, or
attestation is provided. Unsigned checksums detect transfer changes; they do
not independently authenticate the publisher.

Review the rendered Markdown before approval. If its contents need changing,
edit the canonical template, select the final source commit, rerun qualification
if that commit changes, and produce a fresh plan. Do not silently edit a body
after its digest has been approved.

## 3. Record approval

Store this record in an owner-controlled release log, not in the candidate
packet or public notes. Fill every field. Missing names, digests, evidence,
or decisions block the next step. An agent cannot approve for an owner.

```text
Version / tag: 1.0.0 / v1.0.0
Repository: Artificial-Source/AVA
Source commit (full SHA):
Candidate workflow URL / run ID / attempt:
Candidate artifact ID / expiry:
Evidence artifact ID / expiry:
Same-run required jobs and conclusions:
Deterministic / tmux 23-of-23 / PTY 4-of-4 report IDs:
Minimum-host / CPU-profile / ISA inspection evidence:
Executable SHA-256:
Archive name / size / SHA-256:
Checksum name / size / SHA-256:
candidate.json SHA-256 (from producing run summary):
Rendered release-body SHA-256:
Generated draft-commands.sh SHA-256:
Changelog reviewed at source commit:
Supported floor and known limits accepted:
Clean-install / fake-provider evidence:
Patch-correction drill result / withdrawal drill result:
Operator name / UTC time / approve exact commit and digests:
Independent owner name / UTC time / approve exact commit and digests:
Draft release ID / URL (fill after staging):
Remote tag object SHA / peeled commit (fill after staging):
Downloaded draft assets and body match / evidence (fill after staging):
Final publish approval: owner name / UTC time / exact draft ID and digests:
Public release ID / URL / published time (fill after publish):
Post-publication evidence / PASS or incident decision:
```

Before staging, walk the correction and withdrawal cases in section 7. Record
who made each decision and what commands they would use. A local technical
rehearsal is not independent owner approval or a remote withdrawal drill.

## 4. Create the draft without clobbering

Only after the first two approvals, inspect and run the generated owner script:

```sh
bash "$PACKET_ROOT/plan/draft-commands.sh"
```

It re-verifies the retained bytes and body digest, checks that the remote tag
and any release/draft with this version are absent, creates an annotated local
tag at the approved commit, pushes it without force, and runs this command
with the exact retained paths (not a new build):

```sh
gh release create "$TAG" --repo "$REPOSITORY" --draft --verify-tag \
  --title "AVA $VERSION" --notes-file "$PACKET_ROOT/plan/release-body.md" \
  "$PACKET_ROOT/candidate/ava-1.0.0-linux-x64.tar.gz" \
  "$PACKET_ROOT/candidate/ava-1.0.0-linux-x64.tar.gz.sha256"
```

**Pushing a tag makes the tag public even while the release is a draft.** The
first approvals must authorize that fact. Do not claim the draft hides a tag.
The script stops on an existing local tag, remote tag, release, or failed
upload. If a network failure leaves a tag or partial draft, preserve the
state and evidence; do not rerun, delete, or upload over it automatically.
The owner must inspect it and choose the pre-publication branch in section 7.

Inspect the draft body and confirm its links and limitations are visible.
Run section 6 against the authenticated draft download before publication,
then record the release ID, annotated tag object, peeled commit, asset
IDs/sizes/digests, and body digest. The independent owner gives a separate
final approval for that exact remote draft. No edit or upload is allowed
between final approval and publish.

## 5. Publish separately

The final owner confirms the ledger has no unmet code, CI, platform, or
approval gate, and authorizes these exact commands:

```sh
gh release view "$TAG" --repo "$REPOSITORY" \
  --json databaseId,isDraft,tagName,targetCommitish,body,assets,url
# Confirm this is the approved draft ID and approved asset/body identities.
gh release edit "$TAG" --repo "$REPOSITORY" --draft=false --latest
```

This publishes the existing draft. Do not create another release, rebuild,
rename assets, replace bytes, or move the tag. Record the public release URL,
ID, and timestamp, then immediately repeat the following checks using a new
download directory. Update the ledger to published only after these pass.

## 6. Verify the draft, then verify the public release

Run from a clean machine or fresh isolated user environment meeting the
published floor. For the public pass, use unauthenticated downloads where
possible; do not substitute the CI artifact or an operator's retained copy.
For the draft pass, authenticate as the reviewing owner.

```sh
export CHECK_ROOT=$(mktemp -d "${TMPDIR:-/tmp}/ava-release-check.XXXXXXXX")
chmod 0700 "$CHECK_ROOT"
git ls-remote --tags "https://github.com/$REPOSITORY.git" \
  "refs/tags/$TAG" "refs/tags/$TAG^{}" > "$CHECK_ROOT/remote-tag.txt"
python3 - "$CHECK_ROOT/remote-tag.txt" "$REVISION" "$TAG" <<'PY'
import pathlib, sys
refs = dict(line.split()[::-1] for line in pathlib.Path(sys.argv[1]).read_text().splitlines())
ref = 'refs/tags/' + sys.argv[3]
assert refs.get(ref + '^{}', refs.get(ref)) == sys.argv[2], 'remote tag/commit mismatch'
print('remote tag matches approved commit')
PY
gh release view "$TAG" --repo "$REPOSITORY" \
  --json databaseId,isDraft,tagName,targetCommitish,body,assets,url \
  > "$CHECK_ROOT/remote-release.json"
# Check the approved release ID, exact tag, exactly two assets, and isDraft
# (true before final approval, false after publication). targetCommitish
# alone does not prove the tag's peeled commit.
gh release download "$TAG" --repo "$REPOSITORY" --dir "$CHECK_ROOT" \
  --pattern ava-1.0.0-linux-x64.tar.gz \
  --pattern ava-1.0.0-linux-x64.tar.gz.sha256
cp "$PACKET_ROOT/candidate/candidate.json" "$CHECK_ROOT/candidate.json"
python3 scripts/release-candidate.py verify --directory "$CHECK_ROOT" \
  --revision "$REVISION" --manifest-sha256 "$MANIFEST_SHA256"
(cd "$CHECK_ROOT" && sha256sum --check --strict ava-1.0.0-linux-x64.tar.gz.sha256)
python3 scripts/inspect-linux-artifact.py "$CHECK_ROOT/ava-1.0.0-linux-x64.tar.gz"
python3 - "$CHECK_ROOT/remote-release.json" "$PACKET_ROOT/plan/release-body.md" <<'PY'
import json, pathlib, sys
remote = json.loads(pathlib.Path(sys.argv[1]).read_text())
assert remote['body'].replace('\r\n', '\n').rstrip() == pathlib.Path(sys.argv[2]).read_text().rstrip(), 'release body mismatch'
assert sorted(asset['name'] for asset in remote['assets']) == [
    'ava-1.0.0-linux-x64.tar.gz', 'ava-1.0.0-linux-x64.tar.gz.sha256'], 'unexpected assets'
print('release body and asset names match')
PY
mkdir -m 0700 "$CHECK_ROOT/extracted" "$CHECK_ROOT/home" \
  "$CHECK_ROOT/config" "$CHECK_ROOT/state"
tar -xzf "$CHECK_ROOT/ava-1.0.0-linux-x64.tar.gz" -C "$CHECK_ROOT/extracted"
export CHECK_BINARY="$CHECK_ROOT/extracted/ava-1.0.0-linux-x64/bin/ava"
test "$(env -i PATH=/usr/bin:/bin HOME="$CHECK_ROOT/home" \
  XDG_CONFIG_HOME="$CHECK_ROOT/config" XDG_STATE_HOME="$CHECK_ROOT/state" \
  "$CHECK_BINARY" --version)" = 'ava 1.0.0'
env -i PATH=/usr/bin:/bin HOME="$CHECK_ROOT/home" \
  XDG_CONFIG_HOME="$CHECK_ROOT/config" XDG_STATE_HOME="$CHECK_ROOT/state" \
  "$CHECK_BINARY" --help
env -i PATH=/usr/bin:/bin HOME="$CHECK_ROOT/home" \
  XDG_CONFIG_HOME="$CHECK_ROOT/config" XDG_STATE_HOME="$CHECK_ROOT/state" \
  "$CHECK_BINARY" doctor
```

The verifier compares both downloaded file digests with the independently
approved manifest and confirms the packaged executable, commit, member list,
and provenance. `candidate.json` is copied only as the verification record;
it is not an uploaded release asset. Save the remote asset IDs and sizes from
`remote-release.json` and compare them with the approval record.

Also run the deterministic packaged fake-provider smoke from the
[artifact checklist](release-checklist.md#build-and-verify-the-linux-host-artifact), using a retained
qualified helper if available. Do not rebuild AVA for this check. The retained
CI evidence already records that smoke for the exact archive; if the helper
is unavailable locally, record that fact and have the owner accept that CI
proof explicitly rather than claiming another local run. Open every release
link and do a fresh install on the minimum host. Record optional doctor
warnings separately from failures. Any tag, body, digest, or smoke mismatch
starts the incident decision below; never fix an asset in place.

## 7. Correction, rollback, and withdrawal

### Before publication

- **Nothing is public and a local check fails:** stop. Preserve evidence,
  fix the source or notes, choose the final commit, and rerun qualification.
- **An unpublished draft exists but no public tag was pushed:** the owner may
  delete only that draft and its uploads after recording its ID and digests:
  `gh release delete "$TAG" --repo "$REPOSITORY" --yes`. Do not use
  `--cleanup-tag`. Reuse of the version needs explicit owner approval and
  proof that no public observer could have consumed it.
- **A tag was pushed, or exposure is uncertain:** treat that version as used.
  Do not move or delete the public tag. Cancel the draft, record the failure,
  and use the next patch version for corrected code or notes.

### After publication

1. **Only code or documentation needs correction, with no urgent risk?** Keep
   the old version intact. Prepare `1.0.1` (or the next unused patch) and
   repeat this entire lifecycle with new notes, approvals, and exact bytes.
2. **The release is unsafe and a safe prior release exists?** Append a dated
   withdrawal warning that names affected/safe versions. Remove the unsafe
   release from latest, point latest to the safe release, and tell users how
   to install its unchanged archive. There is no in-place downgrade command.
3. **This is 1.0.0 and no safe prior release exists?** Mark it withdrawn and
   non-latest, warn users to stop using it, and state that no supported safe
   binary is available until a qualified patch ships. Do not recommend an
   arbitrary development build as a supported fallback.
4. **Digest mismatch, suspected compromise, or security issue?** Stop all
   promotion. Preserve IDs, digests, logs, and the original bytes privately.
   Follow the [private security process](../../SECURITY.md). Revoke affected
   credentials if needed, agree disclosure timing, and qualify a new patch.

Owner incident commands, after writing and approving a notice that preserves
the original body and appends the date, reason, affected digest, and safe path:

```sh
gh release edit "$TAG" --repo "$REPOSITORY" --latest=false \
  --notes-file /absolute/owner-approved-withdrawal-notice.md
# Only when a known-safe published release exists:
gh release edit "$SAFE_TAG" --repo "$REPOSITORY" --latest
```

Keep the withdrawn page, tag, and assets so users holding the old digest can
identify it. Removing public bytes is exceptional (for example, exposed
secrets) and needs a separate owner/security decision plus a public notice;
it never authorizes replacing those bytes under the same name/version.

Rehearse both cases before publication: a harmless note/code correction must
choose a new patch; an unsafe first release must become non-latest with no
invented safe predecessor. Record the participants, decisions, and proposed
commands in the approval log. No remote incident command is run by a local
dry-run.

## 8. Supported versions

Before first publication there is no officially supported released version.
After publication, only the latest published, non-withdrawn `1.0.x` patch is
supported unless owners announce another line. Support is best-effort, not a
response-time or maintenance-duration promise. Security reports must name
affected and fixed versions; an unreleased runtime number is not a release.

A new major/minor line does not silently end support. Owners must publish a
dated policy naming the supported lines, replacement path, and any end date,
then update the [security](../../SECURITY.md) and [support](../../SUPPORT.md)
pages. Withdrawal with no safe patch means there is temporarily no supported
safe binary.

## Local rehearsal and remaining owner actions

A local packet can exercise the preparation tool without remote access. The
REL-012 ledger records its commit, digests, commands, and results. This is not
proof of GitHub retention, draft creation, independent approval, public
installation, or remote withdrawal. Owners must still run candidate CI,
review and approve the packet, create and verify the draft, give final publish
approval, publish, and record the public verification. REL-012 stays pending
until those remote and human gates are complete.

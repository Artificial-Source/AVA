# AVA Documentation Policy

This document defines AVA's documentation information architecture and
maintainer update rules. It is about where truth belongs, not a replacement for
any product or protocol contract.

## Information architecture

Use one primary home for each kind of information:

| Audience/purpose | Primary location | Content |
| --- | --- | --- |
| Repository entry | root [`README.md`](../../README.md) | concise product summary, install/build start, links into current docs |
| Documentation navigation | [`docs/README.md`](../README.md) and root [`llms.txt`](../../llms.txt) | single human spine in task order and concise robot entry point |
| Users/operators | `docs/core/`, `docs/interfaces/`, `docs/operations/` | current commands, configuration, interfaces, behavior, limits, and recovery |
| Desktop planning | `docs/desktop/` | greenfield Electron planning and research; not shipped UI or first-release scope |
| Client/extension authors | `docs/extensions/` plus fixed contracts in `docs/` | current setup/authoring guides and versioned normative wire/file contracts |
| Maintainers | `docs/development/` and [`docs/AGENTS.md`](../AGENTS.md) | current architecture, source map, invariants, and engineering/documentation policy |
| Active engineering programs | `docs/engineering/` | evidence-backed programs, reproducible ledgers, baselines, ADRs, and report sets |
| Security | `docs/security/` | practical threat guidance plus fixed verified guarantees and limitations |
| Testing/release | `docs/operations/testing.md`, `docs/operations/release-checklist.md` | reproducible validation and release gates |
| Product state | `docs/product/` | current capability baselines and evidence mappings |
| Future work | `docs/plans/`, `docs/roadmap/`, `docs/goals/` | proposals, sequencing, acceptance criteria, unresolved work |
| History/evidence | `docs/history/`, `docs/versions/`, `docs/interop/` | what was claimed or observed at a particular point in time; `docs/interop/evidence/` holds the dated reports |
| Schemas/machine status | `docs/schema/`, JSON manifests | machine-readable contracts or status checked by tests |

The mechanically allowed top-level taxonomy is `core`, `interfaces`, `desktop`, `extensions`, `operations`, `development`, `engineering`, `security`, `product`, `plans`, `roadmap`, `goals`, `history`, `versions`, `interop`, and `schema`, plus optional excluded `reference-code`. `engineering/` is narrowly reserved for active, evidence-backed engineering programs and the reproducible ledgers, baselines, ADRs, and report sets that support them. Current general architecture and policy remain under `development/`; historical completed ledgers remain under `history/`. Only `README.md`, `AGENTS.md`, `acp.md`, `acp-support.json`, `rpc-protocol.md`, `headless-protocol.md`, `session-format.md`, and `plugin-compatibility-policy.md` remain as fixed `docs/`-root files. Every owned category has its required `README.md`; nested implementation inputs such as `operations/docker/Dockerfile` are not document graph nodes.

Do not make a historical ledger, roadmap, or reference checkout the only place
that current user or maintainer behavior is explained. `docs/reference-code/`
(if present locally) is comparative material only: it is not AVA architecture,
implementation, or documentation authority and must not be included in broad
source-based claims.

## Document classes and precedence

Every document should make its class apparent from its title, opening, and
location.

### Normative contracts

A normative document tells an external consumer or maintainer what AVA must do.
It uses explicit language such as “must”, versioned fields, compatibility rules,
and validated limits. Current examples include:

- [`rpc-protocol.md`](../rpc-protocol.md) for proprietary RPC v1;
- [`acp.md`](../acp.md) and [`acp-support.json`](../acp-support.json) for the supported
  ACP profile and evidence labels;
- [`headless-protocol.md`](../headless-protocol.md) for shared headless behavior;
- [`session-format.md`](../session-format.md) and
  [`development/session-versioning.md`](session-versioning.md) for
  persisted session compatibility;
- [`plugin-compatibility-policy.md`](../plugin-compatibility-policy.md) for stable
  plugin/MCP compatibility rules;
- [`security/containment.md`](../security/containment.md) for the verified command
  containment contract; and
- machine-readable schema/golden files where a test declares them normative.

A normative contract and its conformance tests must change together. If the
implementation and a normative document disagree, treat it as a defect: do not
silently “resolve” the discrepancy in a descriptive page. For current observed
behavior during debugging, production source plus passing focused tests is the
best evidence; for promised external compatibility, the versioned normative
contract defines the intended obligation.

### Descriptive current-state documents

A descriptive document explains the current implementation or use without
creating a new wire or compatibility promise. This includes
[`development/architecture.md`](architecture.md),
[`development/codebase-guide.md`](codebase-guide.md),
[`core/usage.md`](../core/usage.md), [`core/configuration.md`](../core/configuration.md), and topic guides. Descriptive
docs should link to normative details rather than restating large field tables,
config catalogs, or protocol examples.

Descriptive documents follow production source, CMake ownership, and tests. They
must say when a component is optional, experimental, platform-specific,
credential-gated, or not integrated.

### Planning documents

Roadmaps, goals, proposals, and `*-plan.md` documents describe desired work.
They are not evidence that a feature exists. Use future tense and explicit
statuses such as proposed, accepted, in progress, deferred, or complete. Once a
plan lands, update the current user/architecture/contract docs and either reduce
the plan to rationale or mark it historical; do not force readers to reconstruct
current behavior from completion checkboxes.

### History and evidence

Version files, dated interoperability reports, implementation ledgers, and
release journals preserve what was claimed, tested, or decided at a point in
time. They may remain intentionally stale. Date the evidence, identify the exact
version/commit or environment when relevant, and never cite it alone as proof of
current behavior. A “latest result” statement is evidence, not a timeless
contract.

## Source-of-truth matrix

| Question | Primary authority | Required corroboration/update |
| --- | --- | --- |
| What targets/files are built? | `CMakeLists.txt` files | architecture/codebase guide when module shape changes |
| Which module owns behavior? | production code under `src/ava/` and target/include dependency checks | [`architecture.md`](architecture.md), codebase guide |
| What does a CLI command or TUI control do? | implementation plus focused/whole-process tests | [`USAGE.md`](../core/usage.md), topic guide, help text where applicable |
| Where/how is configuration loaded? | `src/ava/config/`, app loaders, tests | [`CONFIG.md`](../core/configuration.md) and JSON schema if applicable |
| What bytes/fields may an RPC client rely on? | [`rpc-protocol.md`](../rpc-protocol.md) | RPC serialization tests, contract manifest/goldens |
| What ACP behavior is supported? | [`acp.md`](../acp.md), [`acp-support.json`](../acp-support.json) | ACP tests and dated interoperability evidence |
| What is persisted in a session? | [`session-format.md`](../session-format.md) and versioning policy | session serialization/validation/projection tests |
| What may a plugin/MCP author rely on? | plugin/MCP normative and compatibility docs | protocol/golden/fake-process tests |
| What is the security guarantee? | security docs scoped to a tested mechanism | production enforcement plus adversarial tests; never infer broader claims |
| Is a feature release-ready? | current product baseline and release checklist | default/focused test evidence; roadmap completion alone is insufficient |
| Why was a design chosen? | current architecture plus focused engineering rationale | history/ledger may provide dated context, not current authority |
| What should be built next? | roadmap/goal/plan document | status and acceptance criteria; no present-tense product claim |
| What dependency/license ships? | build pins, gitlinks, lockfiles, license/notices | provenance/package tests and `THIRD_PARTY_NOTICES.md` |

Source comments are authoritative for local invariants that cannot be separated
from an API or algorithm, but they are not a substitute for an external contract
or discoverable maintainer guide. Test names and fixtures prove covered behavior;
they should not be the sole user documentation.

## Public API boundary

AVA's supported public surfaces are the installed executable behavior and the
explicitly documented, versioned wire/file/extension contracts. **Headers under
`src/ava/` are internal C++ implementation details, not a stable public library
API.** CMake `PUBLIC` linkage and public header visibility express transitive
build requirements inside this repository; they do not promise source, binary,
or ABI compatibility to external C++ consumers. Do not describe an internal
class or header as supported integration API unless AVA deliberately introduces
and documents such a contract.

## Style rules

- Lead with purpose and audience. State whether the page is normative,
  descriptive, planning, historical, experimental, or evidence.
- Prefer repository-relative links and exact source/test paths in backticks.
- Use one canonical explanation and link to it. Do not copy protocol field
  catalogs, configuration lists, security guarantees, or long commands across
  several pages.
- Use present tense only for behavior implemented and covered now. Use future
  tense and a visible status for proposals.
- Qualify platform, build-option, credential, trust, and opt-in requirements.
  “Supported” must identify the supported surface and evidence.
- Use stable machine identifiers exactly; use prose labels for people. Do not
  branch a documented client on free-form diagnostic text.
- Keep examples credential-free and use placeholders such as
  `/absolute/path/to/ava`. Never include secrets, private provider payloads, or
  unsanitized support artifacts.
- Link source directories/files instead of pasting large implementation blocks.
  Small snippets are appropriate only when they define a user/client operation.
- Use GitHub-rendered Markdown. Prefer tables and fenced text diagrams. Mermaid
  is acceptable only when GitHub renders the used syntax reliably; include an
  equivalent textual explanation so the architecture remains usable in plain
  renderers.
- Use descriptive link text, unique headings, fenced language tags, and
  lowercase descriptive `.md` names. Reserve uppercase filenames for category
  entry points such as `README.md` and scoped instructions such as `AGENTS.md`.
- Avoid line-number links to a moving branch. Exact commit permalinks are
  appropriate for external evidence; current in-repository docs should link to
  paths and symbols.

## Freshness rules

A current-state document has no implied grace period: update it in the same
change that makes its statement false. Review these triggers:

- An entry point, module responsibility, dependency direction, process, or
  authority boundary changes: update architecture and the codebase guide.
- User-visible behavior, defaults, commands, configuration paths, environment
  variables, limits, or troubleshooting changes: update the owning user/topic
  guide.
- A stable serialized shape or compatibility rule changes: update the normative
  contract, version policy as needed, tests, manifests/schemas, and goldens in
  the same patch.
- A security control or limitation changes: update the precise security page.
  Do not leave a broader stale claim elsewhere.
- A planned item becomes implemented/deferred/rejected: update its status and
  the current-state docs; capability/evidence ledgers must point to current
  tests.
- A release/evidence result is recorded: include date, exact scope, expected
  skips/gates, and whether live credentials/network were used. Never replace a
  reproducible command with only a result claim.
- A file is renamed or moved: update inbound links and navigation in the same
  patch.

For broad edits, search for the old term, path, option, field, version, and claim
across `README.md`, `docs/`, source help text, schemas, fixtures, and tests.
Exclude build trees, vendored/reference code, and historical ledgers when
looking for current architectural authority; update historical text only when it
contains a broken link or is explicitly labeled as current.

## Required documentation updates by change type

| Change | Minimum documentation review/update |
| --- | --- |
| New/changed CLI command, flag, alias, or exit behavior | root/help output as applicable, `docs/core/usage.md`, `docs/core/configuration.md` if configured, headless docs for automation behavior |
| TUI control, rendering, keybinding, theme, or terminal capability | `docs/core/usage.md`, `docs/interfaces/themes-keybindings.md` and/or `docs/operations/terminal-setup.md`; testing evidence when a real terminal is required |
| Model/provider/auth support | `docs/core/providers.md`, `docs/core/configuration.md`, user setup; architecture only for a new seam |
| RPC event/request/response or framing | normative `rpc-protocol.md`, contract manifest/goldens, headless docs, version/compatibility statement |
| ACP method/capability/interoperability | `acp.md`, `acp-support.json`, tests and dated evidence policy/report when a client claim changes |
| Session envelope/payload/projection/import/export | `session-format.md`, session-versioning policy, protocol docs that expose it, migration/compatibility tests |
| Built-in tool or permission operation | `docs/core/usage.md`, security guide when authority changes, side-effect checklist, protocol tool/event docs as applicable |
| Command policy/containment/process cleanup | `docs/security/sandboxing.md` and containment contract; exact limitations and platform fallbacks |
| Plugin/MCP authoring or wire behavior | `docs/extensions/` and compatibility policy; golden/schema fixtures |
| Context/trust/resource discovery | `docs/core/configuration.md`, `docs/core/context-resources.md`, security trust section |
| Module/entry-point/dependency/ownership change | `docs/development/architecture.md`, development codebase guide |
| Build dependency, option, prerequisite, or test runner | `docs/development/contributing.md`, `docs/operations/testing.md`, release/provenance docs where applicable |
| Planned desktop client | `docs/desktop/` index and architecture; keep optional/future status explicit; do not treat `apps/desktop/` as the greenfield design baseline |
| Roadmap/capability status | owning plan/roadmap plus current product baseline/evidence ledger; user docs if shipped |
| Release packaging/install contract | release checklist, artifact README, testing commands, notices/provenance as applicable |

“No documentation impact” is acceptable for an internal refactor only after
checking that paths, ownership, diagrams, tests, and user-observable behavior
remain truthful.

## Link-validation workflow

AVA owns an offline relative-target checker. Run it from the repository root after documentation changes:

```sh
python3 scripts/verify-markdown-links.py . --source-tree
```

The configured-tree CTest coverage is:

```sh
scripts/run-tests.sh --build-dir build \
  -R '^ava_tests\.markdown_(link_verifier|links_source)$' --output-on-failure
```

The exact test names are `ava_tests.markdown_link_verifier` (focused checker behavior) and `ava_tests.markdown_links_source` (the first-party source-tree gate). Python 3 is optional at CMake configuration time; these tests are registered when its interpreter is found.

In `--source-tree` mode the checker walks first-party Markdown while excluding Git metadata, the gitignored local `.plans/` workspace, build trees (`build`, `build-*`, and `cmake-build-*`), generated/cache/package directories, dependency trees (`cmake/aicxx`, `cwds`, `enchantum`, `src/json`, `threadsafe`, and `utils`), and local `docs/reference-code/`. The artifact template at `docs/operations/release-artifact-readme.md` is relocated to the artifact documentation root before its links become valid and remains source-tree-excluded. Exact exclusions live in [`scripts/verify-markdown-links.py`](../../scripts/verify-markdown-links.py).

Exact absolute `https://github.com/Artificial-Source/AVA/blob/develop/<path>[#fragment]` self-links are repository-local in source-tree mode: their percent-decoded targets must exist and remain root-confined even when they point into an otherwise excluded tree, and fragments on Markdown targets must resolve. Other absolute URLs remain external and no network request is made. Without `--source-tree`, the program is in **artifact mode**: all absolute URLs, including AVA self-links, remain external, and it recursively checks all Markdown beneath the supplied staged documentation root with no source-tree exclusions. Local Markdown fragments are still checked in artifact mode. `scripts/package-linux.sh` runs that mode after staging, where the relocated artifact README and exact package allowlist must resolve together.

The checker validates local relative target paths and Markdown fragments in inline links, full reference links, and collapsed reference links; percent-decodes paths and fragments; rejects links that escape the checked root; and ignores fenced-code examples. Its deterministic GitHub-compatible anchor inventory covers ATX headings (including entity decoding, inline link display text, formatting/punctuation removal, whitespace hyphenation, and duplicate suffixes) plus explicit HTML `id`/`name` anchors. Anchors are cached per target during a run.

The checker deliberately does **not** validate shortcut reference links, ordinary external URLs, or images. It makes no network requests and currently reports the source file but not a source line number. Therefore maintainers must still:

1. review shortcut reference links;
2. review material external links manually;
3. search renamed/deleted paths for inbound references; and
4. run `git --no-pager diff --check`.

A successful source-tree run proves only that checked local relative targets, local Markdown anchors, and exact AVA self-link targets/Markdown anchors exist within the documented scope. It is not evidence that external sites, examples, or historical claims are current.

## Structure and reachability workflow

Run the deterministic hierarchy gate from the repository root:

```sh
python3 scripts/verify-documentation-structure.py .
```

Its focused and real-tree CTests are exactly `ava_tests.documentation_structure_checker` and `ava_tests.documentation_structure_source`:

```sh
scripts/run-tests.sh --build-dir build \
  -R '^ava_tests\.documentation_structure_(checker|source)$' --output-on-failure
```

The gate enforces the allowed taxonomy and required category indexes, builds a bounded first-party Markdown/JSON graph excluding `docs/reference-code/`, and requires every node to be reachable from `docs/README.md`. JSON files are terminal nodes. Each category index directly owns its immediate Markdown/JSON files and child `README.md` indexes. The reachable artifact README source template is not traversed because its outbound links intentionally resolve only after installation at the artifact root.

The same gate validates every local Markdown-style link in `llms.txt` for existence and root confinement, and requires the concise human/category/evidence spine plus the fixed ACP, RPC, headless, session, and plugin compatibility contracts. `llms.txt` need not enumerate every leaf: category ownership and whole-graph reachability provide that guarantee without a duplicate JSON catalog.

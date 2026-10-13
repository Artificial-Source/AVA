# Chat display presets

## Revision 19 — browser-only chat appearance

Accepted on **2026-10-13 (UTC)** after Astra MAX implementation, Main's independent
verification and one actual-source/actual-PNG Sol MAX gate: **No material findings.**
This follows [revision 18's supplied chat/run rendering](chat-run-refinement.md).

| Preset | Presentation |
|---|---|
| **Compact** (default) | Keep action, resource and status summaries visible; minimize tool detail by default, including during a run. |
| **Auto** | Open running detail, fold finished detail, and keep failed/unavailable results open by default. Manual inspection is retained. |
| **Expanded** | Open supplied tool detail, output and diffs by default, including after completion. |

Choose **Chat display** above the parent transcript or in **Settings → Conversation
behavior**. The ordinary toolbar stays outside the transcript's native scroller;
it is not a sticky overlay. The selector also works in empty parent chats.

A different preset reapplies its defaults without replacing stable turn/tool rows.
Within a preset, manual row/group/all choices take precedence and survive incremental
same-ID updates; departed rows/groups lose their old choices. Focus, pointer,
selection and wheel reading inside Auto detail count as inspection. Passive delivery
and generated bottom-follow scrolling do not. Deliberate later collapse or a different
preset can close inspected detail. Native Latest remains scoped to its own pane.

Successful completed output still has an independent **Show/Hide** control. Hide is
reported as derived **Custom · output hidden**, not a fourth stored policy. Selecting
a preset restores Show; restoring Show alone does not erase manual choices. Code/diff
wrap and local-send scrolling remain separate. Conversation-only/global reset retain
existing chats, history, model/mode, drafts, references and backend-independent facts.
Child inspection remains read-only; no child tool content or selector is invented.
Settings samples and the screen-review workspace remain isolated illustrations.

A preset changes presentation, never execution or supplied status. Stop does not imply
per-tool cancellation: an explicitly terminal parent can fold stale-running snapshot
detail while retaining its supplied Working status. Tool summaries and existing
failure/permission/question explanations are not replaced by a messages-only policy.
No output, thinking content, file changes or execution result is invented. Stored
supplied output and the **200-line / 20,000-character display cap** remain distinct.

## Reference fit

This is a pinned-source product comparison, not a latest-release or persistence-parity
claim. Reference code, architecture, assets and themes were not copied.

- **OpenCode v2**, `740072694de71fae23f35131591e7c32a9713014`, provides Everything,
  Detailed, Compact, Quiet and Messages only, plus advanced category defaults.
  Actual presets: `packages/session-ui/src/timeline/detail.ts:19–73`; rendered labels:
  `packages/app/src/runtime/i18n/en.ts:1182–1215`; manual disclosures and failure
  exceptions: `packages/session-ui/src/timeline/session-timeline-row.tsx:187–304`.
- **Zeron**, `1c83cda221bd18a3b55ea49e5b25ec2c148955bc`, opens trailing streaming tool
  groups outside compact mode, then defaults closed:
  `crates/ui/src/transcript.rs:1384–1403,7013–7027`.
- **Pi**, `898ab804050730e9dcefb4443875d5a932aa6a32`, has a session-wide expanded/collapsed
  tool-output toggle and per-card interaction:
  `packages/coding-agent/src/modes/interactive/interactive-mode.ts:4397–4417`.
  This is not a persisted display-policy setting.

AVA uses three straightforward policies suited to its supplied data. It intentionally
omits hidden-activity presets and unsupported thinking controls. Existing
[Appearance customization](appearance-customization.md),
[Settings behavior](settings-behavior.md) and
[Settings scrolling/reset placement](settings-scroll-layout.md) remain separate.

## Verification

Main independently passed **179 nearest tests / 14 files**, typecheck, **745 full
tests / 61 files** with four workers, and a warning-free build. The entry is
**356,512 raw / 109,361 Node-default gzip bytes**, with **143,488 bytes** below the
unchanged **500,000-byte cap**. No dependency, package/config, backend, asset,
license or notice changes were needed.

**132 Chrome checks** covered five viewports: 1440×960, 1440×560, 850×820, 520×850
and 3440×1440. Coverage includes all policies; running/completed/failed/stopped
supplied snapshots; preset/manual intent; retained DOM/editor/output identity;
draft/caret/focus; native keyboard policy selection and explicit local Send;
output wheel reading/Latest; selected output through terminal delivery; reduced
motion; Settings/Custom/reset; parent/child/other-chat/new-chat behavior; and
unchanged attribution/accounting facts. Browser select helpers exercise the actual
DOM; trusted keyboard changes were separately recorded as `isTrusted: true`.
This does not claim every test action was hardware input or new hidden-tab coverage.

Six painted-text states had minimum **5.405:1**. This bounded compositor check
excludes icons, native input/selected-option paint, hidden/inert/screen-reader-only
or clipped text and modal background. Ordinary console/page errors were empty:
**422 requests / 30 unique URLs / zero external HTTP**.

The sole integrated Sol MAX gate read **16 actual source/test files and 32 actual
PNGs**: two before states and all 30 current captures below. Main independently
verified successful image-tool reads, current hashes, native model/API and MAX
message metadata. There were no material findings and no source corrections after
handoff. QA-only label/fixture/step-count/CLI-locator diagnoses did not alter source
or weaken accepted behavior. Complete older acceptance bodies and captures remain
preserved.

This is still **browser-only and visit-local**; refresh resets preferences. There is
no live auth/provider/tool execution, configuration/session/host-filesystem write or
native-host authority. Finite sandboxed Chrome evidence is not all-browser, AT/WCAG,
FPS/GPU, production, packaging or legal qualification.

## Reviewed captures

Every image below is a hash-matching actual capture read by the gate, not a montage
or filename-only visual claim.

### Display choices and terminal behavior

- [Compact, desktop](concepts/react-chat-display/ava-react-chat-compact-1440x960.png)
- [Expanded, desktop](concepts/react-chat-display/ava-react-chat-expanded-1440x960.png)
- [Auto with a supplied failed result](concepts/react-chat-display/ava-react-chat-auto-failure-1440x960.png)
- [Native keyboard Expanded selection](concepts/react-chat-display/ava-react-chat-display-keyboard-expanded-1440x960.png)
- [Auto Read running](concepts/react-chat-display/ava-react-auto-read-running-1440x960.png)
- [Auto completed and folded](concepts/react-chat-display/ava-react-auto-completed-1440x960.png)
- [Expanded completed](concepts/react-chat-display/ava-react-expanded-completed-1440x960.png)
- [Auto failed terminal](concepts/react-chat-display/ava-react-auto-failed-terminal-1440x960.png)
- [Stopped parent with unchanged supplied tool status](concepts/react-chat-display/ava-react-auto-stopped-supplied-status-1440x960.png)
- [Empty parent selector](concepts/react-chat-display/ava-react-empty-chat-display-1440x960.png)

### Reading, selection and motion

- [Inspected output retained at terminal](concepts/react-chat-display/ava-react-auto-inspected-terminal-1440x960.png)
- [Uninspected burst folded](concepts/react-chat-display/ava-react-auto-burst-folded-1440x960.png)
- [Wheel reading retained at terminal](concepts/react-chat-display/ava-react-auto-wheel-reading-terminal-1440x960.png)
- [Output Latest with named-pane focus](concepts/react-chat-display/ava-react-auto-output-latest-1440x960.png)
- [Selected output retained at terminal](concepts/react-chat-display/ava-react-auto-selected-output-completed-1440x960.png)
- [Native editor retained through Auto completion](concepts/react-chat-display/ava-react-auto-native-editor-terminal-1440x960.png)
- [Reduced-motion Auto running](concepts/react-chat-display/ava-react-auto-reduced-motion-running-1440x960.png)

### Responsive layout and Settings

- [Compact, 520×850](concepts/react-chat-display/ava-react-chat-compact-520x850.png)
- [Expanded, 1440×560](concepts/react-chat-display/ava-react-chat-expanded-1440x560.png)
- [Expanded, 850×820](concepts/react-chat-display/ava-react-chat-expanded-850x820.png)
- [Expanded, 520×850](concepts/react-chat-display/ava-react-chat-expanded-520x850.png)
- [Expanded, 3440×1440](concepts/react-chat-display/ava-react-chat-expanded-3440x1440.png)
- [Higher-contrast large text, 520×850](concepts/react-chat-display/ava-react-chat-expanded-higher-large-520x850.png)
- [Settings, 1440×960](concepts/react-chat-display/ava-react-settings-chat-display-1440x960.png)
- [Settings, 1440×560](concepts/react-chat-display/ava-react-settings-chat-display-1440x560.png)
- [Settings, 850×820](concepts/react-chat-display/ava-react-settings-chat-display-850x820.png)
- [Settings, 520×850](concepts/react-chat-display/ava-react-settings-chat-display-520x850.png)
- [Settings, 3440×1440](concepts/react-chat-display/ava-react-settings-chat-display-3440x1440.png)
- [Custom successful-output visibility](concepts/react-chat-display/ava-react-settings-custom-output-1440x960.png)
- [Conversation-only reset](concepts/react-chat-display/ava-react-conversation-reset-chat-1440x960.png)

# Chat and run rendering refinement

## Revision 18 — browser-only supplied streaming

Accepted on **2026-10-12** after Astra MAX implementation, Main's independent
verification and the sole **GPT-6.1 Sol MAX** actual-source/actual-PNG gate:
**No material findings.** This is an optional, visit-local browser preview at
**`http://127.0.0.1:5182/`**, not provider, AVA backend or Electron integration.
Earlier [normal-app](normal-app-refinement.md), [tool/resource](provider-resource-chat-refinement.md),
[Settings behavior](settings-behavior.md) and [Settings layout](settings-scroll-layout.md)
records and captures remain intact history.

### What works

- Quiet, aligned tool rows retain supplied status and failure facts even when
  details are collapsed. Each tool has its own disclosure; explicit group and
  Expand/Collapse all choices override individual choices without resetting them
  on ordinary updates. Expanded output and supplied diffs remain read-only.
- **Run preview** explicitly starts supplied Completion, Failure or Burst scenes.
  Pause, Resume and Next snapshot affect fixture delivery only. Ordinary Send
  still adds only a local user message; it neither starts a model nor replies to
  the draft. Preview attribution is supplied, not inferred from current settings.
- Partial prose/code is safe incremental plain text. Finished responses receive
  rich formatting without repeatedly highlighting unfinished code. A selected
  partial response stays in that stable form until selection is released.
- Transcript and output follow their own bottom only while the reader has not
  moved away. **Latest** and **Latest output** are explicit actions. Arrivals do
  not steal composer focus/selection, rebuild historical turns or reset mounted
  tool disclosures. Keyboard Latest hands its own disappearing control's focus
  back to the named pane, not a global focus trap.
- Stop retains accepted partial text/tool facts and rejects queued/late delivery.
  Retry explicitly starts a new local identity. Leaving the parent/chat, opening
  Settings or Models, inspecting a child, or creating a chat cancels this preview;
  returning never revives it. This is not a real backend cancellation protocol.

### Safety and ownership

Events are accepted atomically for exact chat/run/generation and ordered sequence.
Duplicates, gaps, wrong identities, terminal revival and invalid bounds do not
partially mutate the accepted projection. Limits are **4,096 events**, **64 tools**,
**16,384 UTF-16 units per delta**, **100,000 assistant-text units** and **100,000
aggregate tool-output units**. Painting is separately bounded to **200 lines /
20,000 characters**, with an honest display-limit label rather than pretending the
supplied data was truncated. Split surrogate pairs are retained safely. Markdown
images do not initiate remote fetching; no raw HTML or new dependency was added.

Supplied tool states retained after Stop are the last accepted facts, not inferred
per-tool execution or cancellation. Changes are never inferred from output or
success. Current/default model ownership, immutable history/attribution, context
and cumulative accounting, permissions, user definitions, references and local
preferences remain separate. The optional whole run and rich-content owners have
visible load failure and stale/lifetime protections; packages/configuration,
backend, legacy client, assets and closed Settings findings remain unchanged.

### Independent acceptance

| Check | Final evidence |
| --- | --- |
| Nearest tests | **191 tests / 19 files** |
| Typecheck | Passed |
| Full suite | **699 tests / 58 files**, four workers |
| Build | Warning-free, **504 modules** |
| Entry budget | **351,358 raw / 107,530 Node-default gzip bytes**; unchanged 500,000 raw cap; **148,642 bytes headroom** |
| Native Chrome | **106 checks**, five viewports: 1440×960, 1440×560, 850×820, 520×850, 3440×1440 |
| Painted glyph contrast | **Eight finite states**, minimum **5.405:1** |
| Ordinary console/page errors | Empty |
| Network observation | 391 requests / 54 unique URLs / zero external HTTP |
| Sole review | **38 actual source/test files + 24 actual PNGs** (two before, 22 current); **No material findings.** |

Burst coverage uses **1,000 text increments and 600 output increments**, including
an overflowing first 50-output-increment stage and subsequent growth beyond the
paint limit. Native input verified draft/selection and DOM identity, independent
output/transcript wheel reading, Latest, per-row/group/all precedence, completion,
failure, Stop/retry, navigation/Settings/model/child/new-chat departure, native
editor/shortcut guards, reduced motion and responsive geometry.

Two Main findings were fixed before the integrated gate:

- **CHAT18-F01:** conditional Latest controls lost keyboard focus on removal.
  Explicit action-scoped named-pane handoff and follow/read guards passed both
  regression and native checks; no focus on arrivals or modal repair was added.
- **CHAT18-F02:** Stop's reused button could turn into submit Send during a trusted
  click and inadvertently add the draft. Distinct semantic element keys plus
  prevented Stop default preserve the draft, including native completion between
  pointer press and release. Explicit later Send still works.

This is finite normally sandboxed Chrome browser-preview acceptance, not universal
flawlessness, FPS/GPU/performance, all-browser, assistive-technology/WCAG, provider,
backend, native, production or packaging qualification. Headless CDP tab switching
did not establish `document.hidden`; native background-document coverage is not
claimed. Approved scope departure and hidden/inert UI guards remain tested.

### Reviewed captures

Current captures are published under [react-chat-run](concepts/react-chat-run/).

- [Default compact tool rows](concepts/react-chat-run/ava-react-tools-premium-default-1440x960.png)
- [Supplied queued run](concepts/react-chat-run/ava-react-stream-start-1440x960.png)
- [Working tool and partial output](concepts/react-chat-run/ava-react-tool-running-expanded-1440x960.png)
- [Partial code remains stable plain text](concepts/react-chat-run/ava-react-partial-code-1440x960.png)
- [Supplied inline diff during delivery](concepts/react-chat-run/ava-react-tool-diff-streaming-1440x960.png)
- [Completed tool facts](concepts/react-chat-run/ava-react-run-completed-final-1440x960.png)
- [Failed response and output](concepts/react-chat-run/ava-react-run-failed-final-1440x960.png)
- [Stopped partial response](concepts/react-chat-run/ava-react-run-stopped-final-1440x960.png)
- [First overflowing output stage](concepts/react-chat-run/ava-react-burst-first-output-growth-1440x960.png)
- [Growing output preserves reading-away](concepts/react-chat-run/ava-react-burst-output-reading-away-1440x960.png)
- [Keyboard Latest output restores pane focus](concepts/react-chat-run/ava-react-burst-output-latest-1440x960.png)
- [Burst completion](concepts/react-chat-run/ava-react-burst-completed-1440x960.png)
- [Short desktop](concepts/react-chat-run/ava-react-tools-streaming-1440x560.png)
- [Compact stacked inspector](concepts/react-chat-run/ava-react-tools-streaming-850x820.png)
- [Narrow stacked inspector](concepts/react-chat-run/ava-react-tools-streaming-520x850.png)
- [Ultrawide bounded reading column](concepts/react-chat-run/ava-react-tools-streaming-3440x1440.png)
- [Narrow inspector closed](concepts/react-chat-run/ava-react-tools-chat-inspector-closed-520x850.png)
- [Departures retain original chat draft/history](concepts/react-chat-run/ava-react-run-departures-preserved-1440x960.png)
- [Native selected response remains stable at completion](concepts/react-chat-run/ava-react-selected-stream-completed-1440x960.png)
- [Reduced-motion receiving state](concepts/react-chat-run/ava-react-stream-reduced-motion-1440x960.png)
- [Higher contrast and large prose at narrow size](concepts/react-chat-run/ava-react-tools-stream-higher-large-520x850.png)
- [Terminal during native Stop pointer press](concepts/react-chat-run/ava-react-native-stop-terminal-race-1440x960.png)

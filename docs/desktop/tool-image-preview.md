# Inline tool image preview

Revision 20 · browser-preview acceptance · 2026-10-13 UTC.

Opening **Inspect image** now embeds its explicitly supplied image inside the
existing tool details. It follows Compact, Auto, Expanded and deliberate
row/group/all disclosure intent; loading an image does not change those policies.

## Presentation and source boundary

- The normal example carries an image-only opaque `previewId`. A closed scalar
  lookup selects one owned same-origin raster; paths, filenames, action names,
  output and context chips never become image URLs.
- The original Search results example is a metadata-free **640 × 400 RGB PNG**,
  **53,055 bytes**, drawn in Pillow with ordinary DejaVu Sans. No competitor
  screenshot, private image, provider logo or external asset was copied. The
  [original raster](../../apps/desktop-preview/public/tool-previews/search-results.png)
  is supplied example content, not a successful live image read.
- The existing image tool remains **Failed**, with the same ID, path and
  **No failure cause supplied.** Preview availability never invents success,
  cancellation, execution, output or accounting.
- Native lazy/async decoding, descriptive alt text and a contained opaque frame
  reserve bounded space. Same-image immutable tool updates retain its image DOM;
  deliberate collapse may unmount it. Missing references and genuine load/decode
  failures show an unavailable preview, never a broken-image or guessed source.
- Current-image/source ownership rejects stale or departed callbacks. Passive
  image delivery does not pin Auto inspection or move focus, selection, editor
  state or scroll. Existing intentional pointer/wheel inspection remains separate.
- Strict supplied-run validation accepts only the known image ID, rejects it on
  file resources and rejects malformed shapes atomically. Accepted references are
  copied; explicitly omitted/replaced references do not retain old media. Existing
  event, text, output, tool and display limits remain unchanged.

Only the normal example opts in. Path-only catalog/ScreenReview examples stay
unavailable, and read-only children gain no invented tools. This is browser-only
supplied presentation: no host filesystem reading, live provider/backend,
attachment upload, download/open action, persistence, gallery or image resolver
platform is added. Existing [chat display presets](chat-display-presets.md) and
[run-preview authority](chat-run-refinement.md) remain intact.

## Verified acceptance

Main independently verified **227 nearest tests / 11 files**, typecheck,
**834 full tests / 64 files** with four workers, and a warning-free **508-module**
production build. The unchanged **500,000-byte** entry cap holds:
**357,036 raw / 109,530 Node-default gzip**, **142,964 bytes** headroom. The
production raster is byte-identical to the inspected original.

**42 native Chrome checks** covered five viewports (1440 × 960, 1440 × 560,
850 × 820, 520 × 850 and 3440 × 1440), including an inspector-closed narrow view.
Actual decoded natural dimensions, visible pixels, same-origin source, contained
scaling, no horizontal overflow, disclosures/presets, mounted Settings return,
child Back, draft/model/history/status preservation and local-user-only Send were
checked. An intentionally blocked local raster in a separate fresh browser
verified real error fallback, focus/facts retention and deliberate reopen recovery.
Actual path-only catalog examples remained unavailable without an image action.

Three painted **HTML-text** states had minimum sampled contrast **5.405:1**.
Bitmap glyphs, SVG/icons, native input/select/textarea paint, hidden/inert/srOnly,
clipped and modal-background text are excluded. Normal browser logs were empty;
90 monitored requests had no external HTTP URLs. The separate deliberate-error
session contains the expected aborted local image request, not a runtime exception.
These finite checks do not qualify all browsers, assistive technology, WCAG,
performance/GPU, native-host/backend integration or packaging. Streaming-image
snapshot/inspection and stale-event coverage is deterministic unit coverage, not
new native live-provider or hidden-document coverage.

The sole integrated native Sol MAX review read **11 actual source/test text files**
and **12 actual PNGs** (nine current, two before and the original raster):
**No material findings.** Main verified actual native model/API/MAX message metadata,
required reads and current hashes; wire reasoning effort is not exposed. Source
stayed frozen through the review.

## Reviewed captures

- [Embedded image, desktop](concepts/react-tool-image/ava-react-image-expanded-1440x960.png)
- [Auto keeps the supplied failed image detail open](concepts/react-tool-image/ava-react-image-auto-failed-1440x960.png)
- [Short desktop](concepts/react-tool-image/ava-react-image-expanded-1440x560.png)
- [Compact layout](concepts/react-tool-image/ava-react-image-expanded-850x820.png)
- [Narrow layout](concepts/react-tool-image/ava-react-image-expanded-520x850.png)
- [Narrow, inspector closed](concepts/react-tool-image/ava-react-image-expanded-inspector-closed-520x850.png)
- [Ultrawide](concepts/react-tool-image/ava-react-image-expanded-3440x1440.png)
- [Actual local image load error](concepts/react-tool-image/ava-react-image-load-error-1440x960.png)
- [No supplied image in separate catalog example](concepts/react-tool-image/ava-react-image-not-supplied-1440x960.png)

## Ownership

Six approved browser source files changed and six were added (including the
original raster); no deletion, dependency/configuration/cap change or modification
to frozen R18/R19 owners occurred. At the source gate, all 202 other existing
browser files were byte-identical. Publication only adds this acceptance and
reviewed captures and prepends current-scope documentation; complete older bodies
and captures are retained by this refinement. Concurrent backend/TUI work,
including a separately committed AgentDefinition provenance-link update, is not
part of this acceptance and is left untouched; no unrelated commit ownership is
claimed.

#!/usr/bin/env python3
"""Generate AVA desktop visual concepts (static SVG, optional PNG).

Static fixtures only. Not a running app, GUI, backend, or provider session.
Requires system fonts Noto Sans and DejaVu Sans Mono for PNG export.
Inkscape is used for rasterization when --png is passed or Inkscape is found.

Usage:
  python3 generate_concepts.py
  python3 generate_concepts.py --png
"""

from __future__ import annotations

import argparse
import shutil
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
W, H = 1440, 960
SIDEBAR_W = 248
CHROME_H = 46
TAB_H = 38
FOOTER_H = 26
COMPOSER_H = 126
WIN_R = 12

FONT = "Noto Sans"
MONO = "DejaVu Sans Mono"

# Candidate tokens — not approved final hex. Same hierarchy in both themes.
THEMES = {
    "light": {
        "name": "light",
        "desktop": "#E4DFD4",
        "canvas": "#F3F0E8",
        "sidebar": "#E9E4D8",
        "composer": "#EBE6DB",
        "code": "#E7E2D6",
        "user_wash": "#E7E2D6",
        "border": "#D2CCC0",
        "border_strong": "#C4BDB0",
        "text": "#1C1B17",
        "secondary": "#45423B",
        "muted": "#5A564E",
        "accent": "#2C5A4B",
        "accent_fill": "#2C5A4B",
        "accent_soft": "#D8E4DC",
        "on_accent": "#F3F0E8",
        "warning": "#6F4708",
        "warning_bg": "#F0E2BE",
        "selected": "#E1E9E4",
        "tab_inactive": "#5A564E",
        "hairline": "#DDD7CB",
        "icon": "#45423B",
        "window_ctrl": "#5A564E",
        "fixture_bg": "#E3DDD1",
        "tool_line": "#DDD7CB",
    },
    "dark": {
        "name": "dark",
        "desktop": "#141513",
        "canvas": "#1E1F1D",
        "sidebar": "#171816",
        "composer": "#232422",
        "code": "#262725",
        "user_wash": "#262725",
        "border": "#2C2E2B",
        "border_strong": "#3A3C38",
        "text": "#EEECE5",
        "secondary": "#C4C0B6",
        "muted": "#A8A39A",
        "accent": "#8FBA9F",
        "accent_fill": "#2F5D4E",
        "accent_soft": "#24352C",
        "on_accent": "#F3F0E8",
        "warning": "#E6C37A",
        "warning_bg": "#3A3018",
        "selected": "#1E2823",
        "tab_inactive": "#A8A39A",
        "hairline": "#2C2E2B",
        "icon": "#C4C0B6",
        "window_ctrl": "#A8A39A",
        "fixture_bg": "#222320",
        "tool_line": "#2C2E2B",
    },
}

# Foreground/background pairs checked by validate_concepts.py
CONTRAST_PAIRS = [
    ("body primary on canvas", "text", "canvas", 4.5),
    ("body secondary on canvas", "secondary", "canvas", 4.5),
    ("muted labels on canvas", "muted", "canvas", 4.5),
    ("muted labels on sidebar", "muted", "sidebar", 4.5),
    ("accent text on canvas", "accent", "canvas", 4.5),
    ("on-accent on fill", "on_accent", "accent_fill", 4.5),
    ("warning text on warning wash", "warning", "warning_bg", 4.5),
    ("warning text on canvas", "warning", "canvas", 4.5),
    ("secondary on sidebar", "secondary", "sidebar", 4.5),
    ("text on selected row", "text", "selected", 4.5),
    ("text on composer", "text", "composer", 4.5),
    ("secondary on composer", "secondary", "composer", 4.5),
    ("muted on composer", "muted", "composer", 4.5),
    ("secondary on user wash", "secondary", "user_wash", 4.5),
    ("text on code wash", "text", "code", 4.5),
    ("window controls on chrome", "window_ctrl", "canvas", 3.0),
    ("icon on sidebar", "icon", "sidebar", 3.0),
    ("selected tab marker", "accent", "canvas", 3.0),
    ("selected sidebar marker", "accent", "selected", 3.0),
    ("mode label on chip", "accent", "accent_soft", 4.5),
]


def esc(text: str) -> str:
    return (
        text.replace("&", "&amp;")
        .replace("<", "&lt;")
        .replace(">", "&gt;")
        .replace('"', "&quot;")
    )


class Svg:
    def __init__(self, theme: dict) -> None:
        self.t = theme
        self.n = 0
        self.parts: list[str] = []

    def add(self, piece: str) -> None:
        if piece and not piece.endswith("\n"):
            piece += "\n"
        self.parts.append(piece)

    def uid(self, prefix: str) -> str:
        self.n += 1
        return f"{prefix}-{self.t['name']}-{self.n}"

    def rect(
        self,
        x: float,
        y: float,
        w: float,
        h: float,
        fill: str | None = None,
        stroke: str | None = None,
        sw: float = 1,
        rx: float = 0,
        extra: str = "",
    ) -> None:
        attrs = [f'x="{_n(x)}" y="{_n(y)}" width="{_n(w)}" height="{_n(h)}"']
        if rx:
            attrs.append(f'rx="{_n(rx)}"')
        attrs.append(f'fill="{fill if fill else "none"}"')
        if stroke:
            attrs.append(f'stroke="{stroke}" stroke-width="{_n(sw)}"')
        if extra:
            attrs.append(extra)
        self.add(f'<rect {" ".join(attrs)}/>')

    def line(
        self, x1: float, y1: float, x2: float, y2: float, color: str, sw: float = 1
    ) -> None:
        self.add(
            f'<line x1="{_n(x1)}" y1="{_n(y1)}" x2="{_n(x2)}" y2="{_n(y2)}" '
            f'stroke="{color}" stroke-width="{_n(sw)}" stroke-linecap="square"/>'
        )

    def text(
        self,
        x: float,
        y: float,
        content: str,
        size: float,
        fill: str,
        weight: str = "400",
        family: str = FONT,
        anchor: str = "start",
        extra: str = "",
    ) -> None:
        self.add(
            f'<text x="{_n(x)}" y="{_n(y)}" font-family="{family}" '
            f'font-size="{_n(size)}" font-weight="{weight}" fill="{fill}" '
            f'text-anchor="{anchor}" {extra}>{esc(content)}</text>'
        )

    def icon(self, name: str, x: float, y: float, color: str, size: float = 16) -> None:
        scale = size / 16.0
        self.add(
            f'<g transform="translate({_n(x)} {_n(y)}) scale({_n(scale)})" '
            f'fill="none" stroke="{color}" stroke-width="1.5" '
            f'stroke-linecap="round" stroke-linejoin="round">'
        )
        self.add(ICON_PATHS[name])
        self.add("</g>")

    def finish(self) -> str:
        return "".join(self.parts)


def _n(value: float) -> str:
    if isinstance(value, int) or float(value).is_integer():
        return str(int(value))
    return f"{value:.2f}".rstrip("0").rstrip(".")


# Original 16×16 strokes approximating Phosphor regular weight. Not library source.
ICON_PATHS = {
    "caret-down": '<polyline points="4 6.2 8 10.2 12 6.2"/>',
    "caret-right": '<polyline points="6.2 4 10.2 8 6.2 12"/>',
    "plus": '<path d="M8 3.5 v9"/><path d="M3.5 8 h9"/>',
    "minus": '<path d="M3.5 8 h9"/>',
    "square": '<rect x="4" y="4" width="8" height="8" rx="1"/>',
    "x": '<path d="M5 5 l6 6"/><path d="M11 5 l-6 6"/>',
    "branch": (
        '<circle cx="4.5" cy="4" r="1.7"/>'
        '<circle cx="4.5" cy="12" r="1.7"/>'
        '<circle cx="11.5" cy="8" r="1.7"/>'
        '<path d="M4.5 5.7 v4.6"/>'
        '<path d="M6.2 4.2 h2.4 a2.6 2.6 0 0 1 2.6 2.6"/>'
    ),
    "copy": (
        '<rect x="5.5" y="5.5" width="7" height="7" rx="1.2"/>'
        '<path d="M4.2 10.5 H3.8 A1.3 1.3 0 0 1 2.5 9.2 V3.8 A1.3 1.3 0 0 1 3.8 2.5 h5.4 '
        'A1.3 1.3 0 0 1 10.5 3.8 v0.4"/>'
    ),
    "changes": (
        '<rect x="2.5" y="3" width="11" height="10" rx="1.5"/>'
        '<path d="M10 3 v10"/>'
        '<path d="M4.7 6.5 h3.2"/><path d="M4.7 9.5 h3.2"/>'
    ),
    "file": (
        '<path d="M5 2.5 h4.2 L12.5 6 v7.5 H5 z"/>'
        '<path d="M9.2 2.5 V6 h3.3"/>'
    ),
    "send": '<path d="M8 12.2 V4.2"/><path d="M5 7 L8 4 l3 3"/>',
    "working": '<path d="M8 3.2 a4.8 4.8 0 1 1-4.1 2.4"/>',
    "warning": (
        '<circle cx="8" cy="8" r="5.4"/>'
        '<path d="M8 5.2 v3.4"/>'
        '<path d="M8 11.1 v.01"/>'
    ),
    "dot": '<circle cx="8" cy="8" r="2.1" fill="currentColor" stroke="none"/>',
    "check": '<path d="M3.8 8.2 l2.6 2.6 5.8-5.8"/>',
}


def _baseline(cy: float, size: float) -> float:
    return cy + size * 0.36


def draw_window_controls(s: Svg, right: float, cy: float) -> None:
    color = s.t["window_ctrl"]
    x = right - 18
    for name in ("x", "square", "minus"):
        s.icon(name, x - 8, cy - 8, color, 16)
        x -= 32


def draw_chrome(s: Svg) -> None:
    t = s.t
    s.rect(0, 0, W, CHROME_H, t["canvas"])
    s.line(0, CHROME_H, W, CHROME_H, t["hairline"], 1)

    s.rect(16, 15, 16, 16, t["accent_fill"], rx=3.5)
    s.add(
        f'<path d="M21.2 20.2 h5.6 M21.2 26.2 h3.8" fill="none" '
        f'stroke="{t["on_accent"]}" stroke-width="1.4" stroke-linecap="round"/>'
    )
    s.text(40, _baseline(23, 13), "AVA", 13, t["text"], "600")

    s.line(78, 16, 78, 30, t["hairline"], 1)

    s.text(92, _baseline(23, 13), "session-format review", 13, t["text"], "500")
    s.text(248, _baseline(23, 12), "·", 12, t["muted"])
    s.icon("branch", 262, 15, t["icon"], 16)
    s.text(282, _baseline(23, 12), "main", 12, t["secondary"], "500")
    s.text(318, _baseline(23, 12), "·", 12, t["muted"])
    s.icon("copy", 332, 15, t["icon"], 16)
    s.text(352, _baseline(23, 12), "shared working copy", 12, t["secondary"])
    s.text(488, _baseline(23, 12), "·", 12, t["muted"])
    s.text(502, _baseline(23, 12), "worktree off", 12, t["muted"])

    # Changes closed — label + icon, not a pane.
    s.icon("changes", 1248, 15, t["icon"], 16)
    s.text(1268, _baseline(23, 12), "Changes", 12, t["secondary"], "500")
    draw_window_controls(s, W - 10, 23)


def draw_sidebar(s: Svg) -> None:
    t = s.t
    y0 = CHROME_H
    y1 = H - FOOTER_H
    s.rect(0, y0, SIDEBAR_W, y1 - y0, t["sidebar"])
    s.line(SIDEBAR_W, y0, SIDEBAR_W, y1, t["hairline"], 1)

    s.text(16, _baseline(y0 + 22, 11), "Library", 11, t["muted"], "600")
    s.icon("plus", SIDEBAR_W - 32, y0 + 14, t["icon"], 16)

    # Project: AVA
    gy = y0 + 46
    s.icon("caret-down", 12, gy - 2, t["muted"], 14)
    s.text(30, _baseline(gy + 5, 11), "AVA", 11, t["text"], "600")

    # Current chat (idle / selected)
    row_y = gy + 18
    s.rect(8, row_y, SIDEBAR_W - 16, 36, t["selected"], rx=6)
    s.rect(8, row_y, 2, 36, t["accent"], rx=1)
    s.text(22, _baseline(row_y + 18, 13), "session-format review", 13, t["text"], "500")

    # Working chat
    row_y = row_y + 42
    s.text(22, _baseline(row_y + 14, 13), "permission policy", 13, t["text"])
    s.icon("working", 20, row_y + 24, t["accent"], 14)
    s.text(38, _baseline(row_y + 31, 11), "Working", 11, t["accent"], "500")

    # Project: agent-lab (fixture)
    gy = row_y + 56
    s.icon("caret-down", 12, gy - 2, t["muted"], 14)
    s.text(30, _baseline(gy + 5, 11), "agent-lab", 11, t["text"], "600")
    s.rect(98, gy - 2, 44, 16, t["fixture_bg"], stroke=t["border"], sw=1, rx=8)
    s.text(120, _baseline(gy + 6, 9), "fixture", 9, t["muted"], "500", anchor="middle")

    # Needs approval
    row_y = gy + 20
    s.text(22, _baseline(row_y + 14, 13), "MCP tool wiring", 13, t["text"])
    s.icon("warning", 20, row_y + 24, t["warning"], 14)
    s.text(38, _baseline(row_y + 31, 11), "Needs approval", 11, t["warning"], "500")

    s.text(
        16,
        y1 - 16,
        "2 projects · 3 chats",
        11,
        t["muted"],
    )


def draw_tabs(s: Svg) -> None:
    t = s.t
    x = SIDEBAR_W
    y = CHROME_H
    s.rect(x, y, W - x, TAB_H, t["canvas"])
    s.line(x, y + TAB_H, W, y + TAB_H, t["hairline"], 1)

    size = 12.5
    tx = x + 18

    s.text(tx, _baseline(y + 18, size), "session-format review", size, t["text"], "600")
    s.line(tx, y + TAB_H - 1, tx + 154, y + TAB_H - 1, t["accent"], 2)
    tx += 186

    s.icon("working", tx, y + 11, t["accent"], 14)
    s.text(tx + 18, _baseline(y + 18, size), "permission policy", size, t["secondary"])
    s.text(tx + 140, _baseline(y + 18, 11), "Working", 11, t["accent"], "500")
    tx += 228

    s.icon("warning", tx, y + 11, t["warning"], 14)
    s.text(tx + 18, _baseline(y + 18, size), "agent-lab · MCP tool wiring", size, t["secondary"])
    s.text(tx + 210, _baseline(y + 18, 11), "Needs approval", 11, t["warning"], "500")


def draw_conversation(s: Svg) -> None:
    t = s.t
    x = SIDEBAR_W
    y = CHROME_H + TAB_H
    h = H - FOOTER_H - COMPOSER_H - y
    s.rect(x, y, W - x, h, t["canvas"])

    cx = x + 52
    cw = 760
    cy = y + 28

    s.text(cx, _baseline(cy, 11), "You", 11, t["muted"], "600")
    cy += 20
    s.text(
        cx,
        _baseline(cy, 14),
        "Walk through how a completed run is stored.",
        14,
        t["secondary"],
    )
    s.text(
        cx,
        _baseline(cy + 22, 14),
        "Keep the notes practical for a reviewer.",
        14,
        t["secondary"],
    )

    cy += 72
    s.rect(cx, cy - 2, 6, 6, t["accent_fill"], rx=1.5)
    s.text(cx + 12, _baseline(cy + 1, 11), "AVA", 11, t["accent"], "600")

    cy += 28
    s.text(
        cx,
        _baseline(cy, 18),
        "What a completed run leaves behind",
        18,
        t["text"],
        "600",
    )

    cy += 32
    body = [
        "A finished chat keeps a readable record of the conversation and tool activity.",
        "Review the work in order, inspect the changes, and return to the task later.",
        "The conversation stays central; details are available when you need them.",
    ]
    for line in body:
        s.text(cx, _baseline(cy, 15), line, 15, t["text"])
        cy += 24

    cy += 14
    s.text(
        cx,
        _baseline(cy, 15),
        "For the review:",
        15,
        t["text"],
    )
    cy += 26
    bullets = [
        "Read the saved conversation in order.",
        "Inspect tool activity without dumping every payload.",
        "Keep background chats visible while you read.",
    ]
    for line in bullets:
        s.add(
            f'<circle cx="{_n(cx + 4)}" cy="{_n(cy - 1)}" r="2.1" fill="{t["accent"]}"/>'
        )
        s.text(cx + 16, _baseline(cy, 15), line, 15, t["text"])
        cy += 24

    cy += 14
    s.line(cx, cy, cx + cw, cy, t["tool_line"], 1)
    cy += 12
    s.icon("caret-down", cx, cy, t["muted"], 14)
    s.icon("file", cx + 18, cy, t["icon"], 14)
    s.text(cx + 38, _baseline(cy + 7, 12), "Read", 12, t["secondary"], "500")
    s.text(cx + 72, _baseline(cy + 7, 12), "·", 12, t["muted"])
    s.text(
        cx + 84,
        _baseline(cy + 7, 12.5),
        "docs/session-format.md",
        12.5,
        t["text"],
        "400",
        MONO,
    )
    s.text(cx + cw, _baseline(cy + 7, 11), "example", 11, t["muted"], anchor="end")
    cy += 22
    s.line(cx, cy, cx + cw, cy, t["tool_line"], 1)

    cy += 24
    close = [
        "The next step is to review the changes, then continue in this chat or start",
        "a focused follow-up. Your other chats can keep working in the background.",
    ]
    for line in close:
        s.text(cx, _baseline(cy, 15), line, 15, t["text"])
        cy += 24

    cy += 10
    s.rect(cx, cy - 14, cw, 32, t["code"], rx=6)
    s.text(
        cx + 12,
        _baseline(cy + 2, 12.5),
        "saved conversation  ·  tool activity  ·  reviewed changes",
        12.5,
        t["text"],
        "400",
        MONO,
    )


def draw_composer(s: Svg) -> None:
    t = s.t
    y = H - FOOTER_H - COMPOSER_H
    x = SIDEBAR_W
    s.rect(x, y, W - x, COMPOSER_H, t["composer"])
    s.line(x, y, W, y, t["hairline"], 1)

    cx = x + 52
    cy = y + 18

    # Build / Plan
    s.rect(cx, cy, 58, 24, t["accent_soft"], rx=6)
    s.text(cx + 29, _baseline(cy + 12, 12), "Build", 12, t["accent"], "600", anchor="middle")
    s.rect(cx + 62, cy, 52, 24, fill="none", stroke=t["border_strong"], sw=1, rx=6)
    s.text(
        cx + 88,
        _baseline(cy + 12, 12),
        "Plan",
        12,
        t["secondary"],
        anchor="middle",
    )

    s.text(cx + 132, _baseline(cy + 12, 12), "·", 12, t["muted"])
    s.text(cx + 146, _baseline(cy + 12, 12), "Example model",
        12, t["secondary"], "500")
    s.text(cx + 250, _baseline(cy + 12, 12), "·", 12, t["muted"])
    s.text(cx + 264, _baseline(cy + 12, 12), "illustrative display", 12, t["muted"])

    field_y = cy + 34
    field_w = 668
    s.rect(cx, field_y, field_w, 40, t["canvas"], stroke=t["border"], sw=1, rx=8)
    s.text(
        cx + 14,
        _baseline(field_y + 20, 14),
        "Message AVA…",
        14,
        t["muted"],
    )

    s.rect(cx + field_w + 12, field_y + 4, 72, 32, t["accent_fill"], rx=7)
    s.icon("send", cx + field_w + 20, field_y + 12, t["on_accent"], 16)
    s.text(
        cx + field_w + 38,
        _baseline(field_y + 20, 12),
        "Send",
        12,
        t["on_accent"],
        "600",
    )

    s.icon("branch", cx, field_y + 52, t["muted"], 14)
    s.text(
        cx + 18,
        _baseline(field_y + 59, 11),
        "main  ·  shared working copy  ·  worktree off",
        11,
        t["muted"],
    )


def draw_footer(s: Svg) -> None:
    t = s.t
    y = H - FOOTER_H
    s.rect(0, y, W, FOOTER_H, t["sidebar"])
    s.line(0, y, W, y, t["hairline"], 1)
    s.text(
        16,
        _baseline(y + 13, 11),
        "Concept · example data · static visual, not a live session",
        11,
        t["muted"],
    )
    s.text(
        W - 16,
        _baseline(y + 13, 11),
        "Idle chat  ·  1 working  ·  1 needs approval",
        11,
        t["muted"],
        anchor="end",
    )


def build_svg(theme_name: str) -> str:
    t = THEMES[theme_name]
    s = Svg(t)
    clip = s.uid("win")
    header = [
        '<?xml version="1.0" encoding="UTF-8"?>',
        f"<!-- AVA desktop visual concept ({theme_name}). Static fixture; not a running app. Generated by generate_concepts.py -->",
        f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{H}" viewBox="0 0 {W} {H}" role="img">',
        f'<title>AVA desktop concept — {theme_name} theme (example data)</title>',
        "<desc>Static visual concept of a focused coding-agent desktop. Illustrative copy only. "
        "No live backend, provider, or accessibility claim.</desc>",
        "<defs>",
        f'<clipPath id="{clip}"><rect x="0" y="0" width="{W}" height="{H}" rx="{WIN_R}"/></clipPath>',
        "</defs>",
        f'<rect width="{W}" height="{H}" fill="{t["desktop"]}"/>',
        f'<g clip-path="url(#{clip})">',
    ]
    s.add("\n".join(header) + "\n")
    s.rect(0, 0, W, H, t["canvas"])
    draw_chrome(s)
    draw_sidebar(s)
    draw_tabs(s)
    draw_conversation(s)
    draw_composer(s)
    draw_footer(s)
    s.add("</g>\n")
    s.add(
        f'<rect x="0.5" y="0.5" width="{W - 1}" height="{H - 1}" rx="{WIN_R}" '
        f'fill="none" stroke="{t["border_strong"]}" stroke-width="1"/>\n'
    )
    s.add("</svg>\n")
    return s.finish()


def export_png(svg_path: Path, png_path: Path) -> None:
    inkscape = shutil.which("inkscape")
    if not inkscape:
        raise SystemExit("inkscape not found on PATH")
    cmd = [
        inkscape,
        str(svg_path),
        "--export-type=png",
        f"--export-filename={png_path}",
        f"--export-width={W}",
        f"--export-height={H}",
    ]
    subprocess.run(cmd, check=True)


def main() -> int:
    parser = argparse.ArgumentParser(description="Generate AVA visual concepts")
    parser.add_argument(
        "--png",
        action="store_true",
        help="also export PNG via headless Inkscape",
    )
    parser.add_argument(
        "--png-if-present",
        action="store_true",
        help="export PNG when Inkscape is available",
    )
    args = parser.parse_args()
    HERE.mkdir(parents=True, exist_ok=True)
    for name in ("light", "dark"):
        svg_path = HERE / f"ava-{name}.svg"
        svg_path.write_text(build_svg(name), encoding="utf-8")
        print(f"wrote {svg_path}")
        want_png = args.png or args.png_if_present
        if want_png:
            if args.png and not shutil.which("inkscape"):
                print("inkscape not found", file=sys.stderr)
                return 1
            if shutil.which("inkscape"):
                png_path = HERE / f"ava-{name}.png"
                export_png(svg_path, png_path)
                print(f"wrote {png_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

#!/usr/bin/env python3
"""Finite checks for AVA visual concepts.

Validates SVG XML, dimensions, absence of external resources, PNG size,
and WCAG-style contrast ratios for declared token pairs.
Does not test keyboard, scroll, runtime, or accessibility of a real UI.
"""

from __future__ import annotations

import re
import sys
import xml.etree.ElementTree as ET
from pathlib import Path

from generate_concepts import CONTRAST_PAIRS, H, THEMES, W

HERE = Path(__file__).resolve().parent
NS_RE = re.compile(r'\sxmlns(?::\w+)?="http://www.w3.org/2000/svg"')
INTERNAL_URL_RE = re.compile(r'url\(#.*?\)')
HTTP_RE = re.compile(r'https?://', re.IGNORECASE)
IMPORT_RE = re.compile(r'@import', re.IGNORECASE)
HREF_RE = re.compile(r"(?:xlink:)?href\s*=\s*['\"](?!#)", re.IGNORECASE)
FORBIDDEN_TAGS = ("image", "foreignObject", "script", "iframe")


def rel_luminance(hex_color: str) -> float:
    h = hex_color.lstrip("#")
    r, g, b = int(h[0:2], 16) / 255, int(h[2:4], 16) / 255, int(h[4:6], 16) / 255

    def f(c: float) -> float:
        return c / 12.92 if c <= 0.04045 else ((c + 0.055) / 1.055) ** 2.4

    return 0.2126 * f(r) + 0.7152 * f(g) + 0.0722 * f(b)


def contrast_ratio(a: str, b: str) -> float:
    l1, l2 = rel_luminance(a), rel_luminance(b)
    hi, lo = max(l1, l2), min(l1, l2)
    return (hi + 0.05) / (lo + 0.05)


def check_svg(path: Path) -> list[str]:
    errors: list[str] = []
    raw = path.read_text(encoding="utf-8")
    scanned = INTERNAL_URL_RE.sub("", NS_RE.sub("", raw))
    if HTTP_RE.search(scanned) or IMPORT_RE.search(scanned) or HREF_RE.search(scanned):
        errors.append(f"{path.name}: external resource reference found")
    try:
        root = ET.fromstring(raw)
    except ET.ParseError as exc:
        return [f"{path.name}: XML parse error: {exc}"]
    tag = root.tag.split("}")[-1]
    if tag != "svg":
        errors.append(f"{path.name}: root is {tag}, not svg")
    width = root.get("width")
    height = root.get("height")
    view_box = root.get("viewBox")
    if width != str(W) or height != str(H):
        errors.append(f"{path.name}: width/height {width}x{height}, expected {W}x{H}")
    if view_box != f"0 0 {W} {H}":
        errors.append(f"{path.name}: viewBox {view_box!r}, expected 0 0 {W} {H}")
    for el in root.iter():
        local = el.tag.split("}")[-1]
        if local in FORBIDDEN_TAGS:
            errors.append(f"{path.name}: forbidden element <{local}>")
    return errors


def check_png(path: Path) -> list[str]:
    errors: list[str] = []
    try:
        from PIL import Image
    except ImportError:
        return [f"{path.name}: PIL not installed"]
    if not path.is_file():
        return [f"{path.name}: missing"]
    with Image.open(path) as img:
        if img.size != (W, H):
            errors.append(f"{path.name}: size {img.size}, expected {(W, H)}")
    return errors


def check_contrast() -> tuple[list[str], list[str]]:
    errors: list[str] = []
    lines: list[str] = []
    for theme_name, tokens in THEMES.items():
        lines.append(f"[{theme_name}]")
        for label, fg_key, bg_key, minimum in CONTRAST_PAIRS:
            fg, bg = tokens[fg_key], tokens[bg_key]
            ratio = contrast_ratio(fg, bg)
            status = "PASS" if ratio >= minimum else "FAIL"
            lines.append(
                f"  {status}  {ratio:5.2f}:1  need {minimum:.1f}  {label}  {fg} on {bg}"
            )
            if ratio < minimum:
                errors.append(
                    f"{theme_name}: {label} {ratio:.2f}:1 < {minimum:.1f} ({fg} on {bg})"
                )
        lines.append("")
    return errors, lines


def main() -> int:
    errors: list[str] = []
    for name in ("light", "dark"):
        errors.extend(check_svg(HERE / f"ava-{name}.svg"))
        errors.extend(check_png(HERE / f"ava-{name}.png"))
    contrast_errors, contrast_lines = check_contrast()
    errors.extend(contrast_errors)
    report = HERE / "contrast-report.txt"
    body = [
        "AVA visual concept checks",
        "Static token contrast only. Not a WCAG certification or runtime a11y test.",
        "",
        *contrast_lines,
    ]
    if errors:
        body.append("FAILURES")
        body.extend(f"- {e}" for e in errors)
    else:
        body.append("All finite checks passed.")
    report.write_text("\n".join(body) + "\n", encoding="utf-8")
    print(report.read_text(encoding="utf-8"), end="")
    if errors:
        print("FAILED", file=sys.stderr)
        return 1
    print("OK")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

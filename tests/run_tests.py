#!/usr/bin/env python3
"""
Pixel-level regression tests for termrenderer, exercised with the bundled
Noto font chain (--noto).

Each case renders a small canned input to a PNG and asserts a pixel-level
property. The checks are intentionally about *fidelity*: that box borders
stay continuous, that a deliberately dashed box character is NOT silently
redrawn as solid, that CJK falls back to a CJK-capable Noto face, and that
colour emoji render via the BGRA path. They guard against "workarounds"
that would replace an input character's intended rendering.

No third-party packages: the PNG is decoded by pnglib (pure stdlib).

Usage:
    python3 tests/run_tests.py [--binary ./termrenderer]
"""

import argparse
import os
import shutil
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import pnglib  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FIX = os.path.join(ROOT, "tests", "fixtures")

# Render geometry. Generous rows/cols so fixtures sit at the top-left and
# each cell is a comfortable size; thresholds in pnglib are independent of
# the exact glyph metrics.
COLS, ROWS = 12, 16

CASES = [
    # name, fixture, assertion-description
    ("box_borders_continuous", "box_solid.txt",
     "solid box chars render as one continuous border per side"),
    ("dashed_not_solidified", "verticals.txt",
     "U+2506 (dashed) stays dashed; only U+2502 (solid) is continuous"),
    ("cjk_fallback_renders", "cjk.txt",
     "CJK falls back to a CJK-capable Noto face and produces ink"),
    ("color_emoji_bgra", "emoji.txt",
     "colour emoji render in their own colours (BGRA path)"),
]


def render(binary, fixture, out_png):
    cmd = [binary, "--noto", "--cols", str(COLS), "--rows", str(ROWS),
           "--output", out_png, "--", "cat", fixture]
    r = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True)
    if r.returncode != 0:
        raise RuntimeError("render failed (%d): %s" % (r.returncode, r.stderr))
    if not os.path.exists(out_png):
        raise RuntimeError("no output png produced")


def check_box_borders(px, w, h, ch, bg):
    hb = pnglib.horizontal_borders(px, w, h, ch, bg, minlen=35)
    if len(hb) < 3:
        return False, "expected >=3 continuous horizontal borders, found %d %s" % (len(hb), hb)
    vl = pnglib.vertical_lines(px, w, h, ch, bg, minpx=70)
    solids = [v for v in vl if v[1] == 1]
    if len(solids) < 3:
        return False, "expected >=3 continuous vertical borders, found %d (%s)" % (len(solids), vl)
    return True, "3 horizontal + %d vertical continuous borders" % len(solids)


def check_dashed(px, w, h, ch, bg):
    vl = pnglib.vertical_lines(px, w, h, ch, bg, minpx=70)
    solid = [v for v in vl if v[1] == 1]
    dashed = [v for v in vl if v[1] > 1]
    if len(solid) != 1:
        return False, "expected exactly 1 solid (single-segment) line, found %d (%s)" % (len(solid), vl)
    if len(dashed) != 1:
        return False, "expected exactly 1 dashed (multi-segment) line, found %d (%s)" % (len(dashed), vl)
    return True, "solid=%s segments, dashed=%s segments (faithful to U+2506)" % (solid[0][1], dashed[0][1])


def check_cjk(px, w, h, ch, bg):
    total = pnglib.fg_count(px, w, h, ch, bg)
    if total < 250:
        return False, "too little ink (%d px); CJK likely not rendered" % total
    # CJK cells of "AB中文CD" sit around x in [18, 42]; ink there proves the
    # wide CJK glyphs were drawn (not just the ASCII).
    region = pnglib.fg_in_rect(px, w, h, ch, bg, x0=18, x1=42)
    if region < 20:
        return False, "no CJK-region ink (%d px in x[18,42])" % region
    return True, "fg=%d px, CJK-region ink=%d px" % (total, region)


def check_emoji(px, w, h, ch, bg):
    chroma = pnglib.chromatic_pixels(px, w, h, ch)
    if chroma < 50:
        return False, "only %d chromatic px; colour emoji BGRA path not active" % chroma
    return True, "%d chromatic px (colour emoji drawn)" % chroma


CHECKS = {
    "box_borders_continuous": check_box_borders,
    "dashed_not_solidified": check_dashed,
    "cjk_fallback_renders": check_cjk,
    "color_emoji_bgra": check_emoji,
}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--binary", default=os.environ.get("TERMRENDERER",
                                                        os.path.join(ROOT, "termrenderer")))
    ap.add_argument("--keep", metavar="DIR", default=None,
                    help="copy each rendered PNG into DIR for inspection")
    args = ap.parse_args()

    if not os.path.exists(args.binary):
        print("error: termrenderer binary not found: %s" % args.binary)
        print("build it first with `make`, or pass --binary PATH")
        return 2

    if args.keep:
        os.makedirs(args.keep, exist_ok=True)

    tmp = tempfile.mkdtemp(prefix="termrenderer-tests-")
    passed = 0
    failed = 0
    for name, fixture, desc in CASES:
        png = os.path.join(tmp, name + ".png")
        fixture_path = os.path.join(FIX, fixture)
        try:
            render(args.binary, fixture_path, png)
            if args.keep:
                shutil.copy(png, os.path.join(args.keep, name + ".png"))
            w, h, ch, px = pnglib.load_png(png)
            bg = pnglib.background(px, w, ch)
            ok, detail = CHECKS[name](px, w, h, ch, bg)
        except Exception as e:  # noqa: BLE001
            ok, detail = False, "exception: %s" % e
        status = "PASS" if ok else "FAIL"
        print("[%s] %-22s %s" % (status, name, desc))
        print("        %s" % detail)
        if ok:
            passed += 1
        else:
            failed += 1

    print("\n%d passed, %d failed" % (passed, failed))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""Compare two pdf_zoom_regression.py runs (baseline vs current).

Pixel comparison uses the settled screenshot of each step (the sharp frame, after
any deferred refinement). Timing rows come from the application's PERF log and
are indicative only: Azahar says nothing about physical Old/New 3DS speed.
"""
import json
import sys
from pathlib import Path

from PIL import Image, ImageChops

DIFF_LEVEL = 32          # per-channel difference that counts as a changed pixel
MAX_CHANGED_FRACTION = 0.01


def load(run):
    return json.loads((Path(run) / "result.json").read_text())


def tolerant_fraction(ia, ib):
    """Changed fraction when each pixel may match any neighbour within 1px."""
    best = None
    for dx in (-1, 0, 1):
        for dy in (-1, 0, 1):
            shifted = ImageChops.offset(ib, dx, dy)
            d = ImageChops.difference(ia, shifted).convert("L")
            best = d if best is None else ImageChops.darker(best, d)
    hist = best.histogram()
    return sum(hist[DIFF_LEVEL + 1:]) / (ia.size[0] * ia.size[1])


def diff_stats(a, b):
    """(changed fraction at DIFF_LEVEL, exact-equal, mean abs diff, max channel diff)."""
    ia, ib = Image.open(a).convert("RGB"), Image.open(b).convert("RGB")
    if ia.size != ib.size:
        return 1.0, False, 255.0, 255
    d = ImageChops.difference(ia, ib)
    gray = d.convert("L")
    n = ia.size[0] * ia.size[1]
    mean = sum(i * c for band in d.split() for i, c in enumerate(band.histogram())) / (3.0 * n)
    peak = max(hi for lo, hi in d.getextrema())
    return tolerant_fraction(ia, ib), d.getbbox() is None, mean, peak


def changed_fraction(a, b):
    return diff_stats(a, b)[0]


def ms(summary, stage, key="median_us"):
    alias = {"raster_any": "pdf.raster", "upload_any": "gpu.upload"}
    entry = summary.get(stage) or summary.get(alias.get(stage, ""), {})
    return entry.get(key, 0) // 1000


def main(base_dir, cur_dir):
    base, cur = load(base_dir), load(cur_dir)
    cur_steps = {s["name"]: s for s in cur["steps"]}
    failures = 0
    print(f"profile: baseline={base.get('profile')} current={cur.get('profile')}")
    print(f"{'step':14s} {'changed px (top)':>17s}   raster n/ms (base->cur)   upload ms   first_present ms")
    totals = {"base_rasters": 0, "cur_rasters": 0}
    for b in base["steps"]:
        c = cur_steps.get(b["name"])
        if not c or "shot" not in b or "shot" not in c:
            continue
        frac, exact, mean, peak = diff_stats(b["shot"]["top"], c["shot"]["top"])
        bad = frac > MAX_CHANGED_FRACTION
        failures += bad
        bs, cs = b["summary"], c["summary"]
        br, cr = bs.get("raster_any", bs["pdf.raster"]), cs.get("raster_any", cs["pdf.raster"])
        totals["base_rasters"] += br["count"]
        totals["cur_rasters"] += cr["count"]
        print(f"{b['name']:14s} {frac * 100:16.2f}%{'!' if bad else ' '}{'=' if exact else ' '} mean={mean:5.2f} peak={peak:3d}  "
              f"{br['count']}/{ms(bs, 'raster_any'):4d} -> {cr['count']}/{ms(cs, 'raster_any'):4d}      "
              f"{ms(bs, 'upload_any'):3d} -> {ms(cs, 'upload_any'):3d} (hw {cs.get('gpu.upload_hw', {}).get('count', 0)}x{ms(cs, 'gpu.upload_hw')})     "
              f"{ms(bs, 'first_present'):4d} -> {ms(cs, 'first_present'):4d}  "
              f"dl {ms(bs, 'pdf.display_list'):4d} -> {ms(cs, 'pdf.display_list'):4d}  "
              f"prev {ms(bs, 'pdf.preview_total'):4d} -> {ms(cs, 'pdf.preview_total'):4d}")
    print(f"\nrasters over the whole run: baseline {totals['base_rasters']} -> current {totals['cur_rasters']}")
    print(f"progressive zoom-in: baseline {base.get('progressive_zoom_in')} current {cur.get('progressive_zoom_in')}")
    print(f"pan rasters: baseline {base.get('pan_rasters')} -> current {cur.get('pan_rasters')}"
          f"  nudge rasters (current only): {cur.get('nudge_rasters')}")
    print(f"pixel regressions (> {MAX_CHANGED_FRACTION * 100:.0f}% changed after a +-1px tolerance): {failures}")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main(*sys.argv[1:3]))

#!/usr/bin/env python3
"""Compare two changes_regression result.json files (baseline vs current)."""
import json
import sys
from pathlib import Path

base, cur = (json.loads((Path(p) / "result.json").read_text()) for p in sys.argv[1:3])
bad = 0


def same(label, a, b):
    global bad
    ok = a == b
    bad += not ok
    print(("SAME " if ok else "DIFF ") + label + ("" if ok else f"\n   baseline={a}\n   current ={b}"))


for key in ("page_count", "chapters", "page_hashes", "page_lengths", "cache_files"):
    if key in base or key in cur:
        same(key, base.get(key), cur.get(key))
if "mobi" in base or "mobi" in cur:
    same("mobi.pages", base.get("mobi", {}).get("pages"), cur.get("mobi", {}).get("pages"))
    same("mobi.chapters", base.get("mobi", {}).get("chapters"), cur.get("mobi", {}).get("chapters"))
    same("mobi.chapter_page_hashes", base.get("mobi", {}).get("chapter_page_hashes"), cur.get("mobi", {}).get("chapter_page_hashes"))
for name in sorted(set(base.get("search", {})) | set(cur.get("search", {}))):
    b, c = base.get("search", {}).get(name), cur.get("search", {}).get(name)
    same(f"search[{name}] result position", b and b["start"], c and c["start"])
    same(f"search[{name}] status/tokens/wrapped",
         b and {k: b["done"][k] for k in ("status", "tokens", "wrapped")},
         c and {k: c["done"][k] for k in ("status", "tokens", "wrapped")})
    if b and c:
        print(f"     scan_ms baseline={b['done']['scan_ms']} current={c['done']['scan_ms']}")

try:
    from PIL import Image, ImageChops
    for name in ("epub-ch1", "epub-ch3-arabic"):
        for kind in ("top", "bottom"):
            a = Path(sys.argv[1]) / f"{name}-{kind}.png"
            b = Path(sys.argv[2]) / f"{name}-{kind}.png"
            if a.exists() and b.exists():
                ia, ib = Image.open(a).convert("RGB"), Image.open(b).convert("RGB")
                if ia.size != ib.size:
                    print(f"DIFF screenshot {name}-{kind}: size {ia.size} vs {ib.size}")
                    bad += 1
                    continue
                diff = ImageChops.difference(ia, ib).convert("L").point(lambda v: 255 if v > 24 else 0)
                changed = sum(1 for v in diff.getdata() if v) / (ia.size[0] * ia.size[1])
                ok = changed < 0.002
                bad += not ok
                print(("SAME " if ok else "DIFF ") + f"screenshot {name}-{kind}: {changed * 100:.3f}% pixels differ")
except ImportError:
    print("PIL unavailable: screenshots not compared")
print("\nIDENTICAL BEHAVIOUR" if not bad else f"\n{bad} DIFFERENCES")
sys.exit(1 if bad else 0)

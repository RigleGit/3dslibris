#!/usr/bin/env python3
"""Azahar regression for PDF zoom/pan on the real debug 3dsx.

Opens a generated, dense PDF, steps the zoom up and down, pans through the
bottom-screen overview panel, turns a page, and records the application's PERF
log (pdf.raster, pdf.interactive_total, gpu.upload, first_present) plus
screenshots after every settled step. Run once per ROM (--role baseline /
--role current) and compare with compare_pdf_runs.py.

Azahar timings are indicative only: they say nothing about physical Old 3DS
speed or memory. The invariants asserted here are behavioural (nothing renders
forever, no render errors, the sharp frame eventually appears, progressive
zoom presents before it rasterizes).
"""
import argparse
import asyncio
import json
import re
import statistics
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import changes_fixtures as fx  # noqa: E402
from changes_regression import Check, Run  # noqa: E402
from mcp import ClientSession  # noqa: E402
from mcp.client.stdio import StdioServerParameters, stdio_client  # noqa: E402

PERF = re.compile(r"PERF (?:view=(?P<view>\d+) )?doc=\S+ (?:format=(?P<fmt>\S+) )?page=(?P<page>-?\d+) "
                  r"zoom=(?P<zoom>-?\d+)(?: stage=(?P<stage>\S+) us=(?P<us>\d+))?")
ZOOM_IN_STEPS = 4
QUICK = False  # --quick: stop after the zoom-in steps (development aid)
PAN_POINTS = [(160, 120), (110, 90), (210, 150), (130, 170), (200, 80), (160, 120)]
# Small overview taps, run last at max zoom: most stay inside the resident
# tiles, some cross one tile border. They do not take part in baseline pixel
# comparisons (the viewport state they leave behind differs from older runs).
NUDGE_POINTS = [(163, 122), (166, 124), (170, 126), (176, 130), (182, 134), (190, 140),
                (198, 146), (190, 140), (182, 134), (170, 126)]


def parse(lines):
    """PERF lines -> [(kind, stage, zoom, us)] in log order."""
    events = []
    for line in lines:
        m = PERF.search(line)
        if not m:
            continue
        if m.group("view") is not None:
            events.append(("view", m.group("fmt") or "", int(m.group("zoom")), 0))
        elif m.group("stage"):
            events.append(("stage", m.group("stage"), int(m.group("zoom")), int(m.group("us"))))
    return events


def stage_us(events, name):
    return [us for kind, stage, _, us in events if kind == "stage" and stage == name]


def summarize(events):
    out = {}
    names = ("pdf.raster", "pdf.interactive_total", "gpu.upload", "first_present", "pdf.display_list",
             "pdf.tile_raster", "gpu.upload_hw", "gpu.upload_cpu", "pdf.tile_pass", "pdf.preview_total",
             "pdf.display_list_kept")
    for name in names:
        values = stage_us(events, name)
        out[name] = {"count": len(values),
                     "median_us": int(statistics.median(values)) if values else 0,
                     "max_us": max(values) if values else 0,
                     "total_us": sum(values)}
    # Whole-frame raster work and texture uploads, whichever path produced them.
    for key, parts in (("raster_any", ("pdf.raster", "pdf.tile_raster")),
                       ("upload_any", ("gpu.upload", "gpu.upload_hw", "gpu.upload_cpu"))):
        values = [us for kind, stage, _, us in events if kind == "stage" and stage in parts]
        out[key] = {"count": len(values), "median_us": int(statistics.median(values)) if values else 0,
                    "max_us": max(values) if values else 0, "total_us": sum(values)}
    return out


def presents_before_raster(events):
    """True when the first present after the view line precedes any pdf.raster."""
    seen_view = False
    for kind, stage, _, _ in events:
        if kind == "view":
            seen_view = True
        elif seen_view and kind == "stage":
            if stage == "first_present":
                return True
            if stage in ("pdf.raster", "pdf.tile_raster"):
                return False
    return None


class PdfRun(Run):
    is_new_3ds = True

    async def start(self, source, label):
        self.launch = await self.probe.call(
            "azahar_launch", rom_path=str(self.rom), source_sdmc=str(source), sdmc_mode="copy",
            is_new_3ds=self.is_new_3ds, app_logs={"app_log": f"{fx.BASE}/3dslibris.log"},
            log_archive_dir=str(self.output / f"logs-{label}"))
        if not self.launch["has_elf"] or not self.launch["has_mailbox"]:
            raise RuntimeError("ELF/mailbox unavailable")
        self.log_seen = 0
        await self.probe.mode(1)
        return self.launch

    async def settle(self, quiet=1.6, limit=25.0, first=0.0):
        """Collect log lines until the application has been quiet for `quiet` s.

        `first` is the minimum time to wait for the first line: a slow (Old 3DS)
        open or render can start well after the input was acknowledged.
        """
        loop = asyncio.get_event_loop()
        end, last_new, lines = loop.time() + limit, loop.time(), []
        started = loop.time()
        while loop.time() < end:
            fresh = [l for l in await self.logs() if "PERF" in l or "MUPDF" in l or "MuPDF" in l]
            if fresh:
                lines += fresh
                last_new = loop.time()
            elif loop.time() - last_new >= quiet and (lines or loop.time() - started >= first):
                break
            await asyncio.sleep(0.35)
        return lines

    async def shot(self, label):
        return await self.probe.capture(label, self.launch["pid"])


async def scenario(session, rom, output, role, chk, record, new3ds):
    run = PdfRun(session, rom, output, role)
    run.is_new_3ds = new3ds
    record['profile'] = 'new3ds' if new3ds else 'old3ds'
    src = Path(tempfile.mkdtemp(prefix="pdfzoom-src-"))
    fx.stage_pdf(src)
    steps = record["steps"] = []

    async def step(name, action, expect_render=True, expect_present=True):
        before = await run.logs()  # drain
        del before
        await action()
        lines = await run.settle(first=4.0 if expect_render else 1.5)
        events = parse(lines)
        shot = await run.shot(name)
        entry = {"name": name, "summary": summarize(events), "events": len(events),
                 "progressive": presents_before_raster(events), "shot": shot,
                 "errors": [l for l in lines if "render error" in l.lower() or "render-disabled" in l]}
        steps.append(entry)
        (output / f"{name}.log").write_text("\n".join(lines) + "\n")
        chk.ok(f"{name}: settled without render errors", not entry["errors"], entry["errors"][:2])
        if expect_render:
            chk.ok(f"{name}: frame rasterized at least once", entry["summary"]["raster_any"]["count"] > 0, entry["summary"])
        if expect_present:
            chk.ok(f"{name}: presented a frame", entry["summary"]["first_present"]["count"] > 0, entry["summary"])
        return entry

    try:
        await run.start(src, "pdfzoom")
        await run.logs()
        await run.probe.press("A")  # open the only book
        await run.probe.mode(0)
        opened = await run.settle(quiet=2.0, limit=60.0, first=15.0)
        events = parse(opened)
        chk.ok("pdf: opened as PDF", any(k == "stage" and f.startswith("pdf.") for k, f, _, _ in events), len(events))
        (output / "open.log").write_text("\n".join(opened) + "\n")
        await asyncio.sleep(1.0)
        steps.append({"name": "open", "summary": summarize(events), "shot": await run.shot("open")})

        for i in range(ZOOM_IN_STEPS):
            await step(f"zoom-in-{i + 1}", lambda: run.probe.press("A"))
        if QUICK:
            return
        for i, (x, y) in enumerate(PAN_POINTS):
            async def tap(x=x, y=y):
                await run.probe.call("azahar_touch_tap", x=x, y=y, hold_ms=120, after_ms=200)
            await step(f"pan-{i + 1}", tap, expect_render=False, expect_present=False)
        for i in range(ZOOM_IN_STEPS):
            await step(f"zoom-out-{i + 1}", lambda: run.probe.press("B"), expect_render=False, expect_present=False)
        await step("page-next", lambda: run.probe.press("R"))
        await step("page-zoom-in", lambda: run.probe.press("A"))
        # Zoomed page turns, forward and back, at mosaic zoom (index 5-6): the
        # kept display list should make the return trip skip pdf.display_list,
        # and the overview should render after the sharp top screen presented.
        await step("page-zoom-in-2", lambda: run.probe.press("A"))
        await step("page-zoom-in-3", lambda: run.probe.press("A"))
        await step("page-zoom-in-4", lambda: run.probe.press("A"))
        await step("page-next-2", lambda: run.probe.press("R"))
        await step("page-prev", lambda: run.probe.press("L"))
        await step("page-next-3", lambda: run.probe.press("R"))
        await step("page-prev-2", lambda: run.probe.press("L"))
        for i, (x, y) in enumerate(NUDGE_POINTS):
            async def nudge(x=x, y=y):
                await run.probe.call("azahar_touch_tap", x=x, y=y, hold_ms=120, after_ms=200)
            await step(f"nudge-{i + 1}", nudge, expect_render=False, expect_present=False)

        zoom_steps = [s for s in steps if s["name"].startswith("zoom-in-")]
        prog = [s["progressive"] for s in zoom_steps]
        record["progressive_zoom_in"] = prog
        if role == "current":
            chk.ok("current: zoom-in presents the scaled tile before re-rasterizing",
                   any(p is True for p in prog[1:]), prog)
        # Idle: no further work once everything settled (no refine loop).
        idle = [l for l in await run.settle(quiet=2.5, limit=5.0)]
        chk.ok("idle: no PERF/render work after settling", not idle, idle[:3])
        raster = sum(s["summary"]["raster_any"]["count"] for s in steps if s["name"].startswith("pan-"))
        record["pan_rasters"] = raster
        record["nudge_rasters"] = sum(s["summary"]["raster_any"]["count"] for s in steps if s["name"].startswith("nudge-"))
        # Panning must move the visible region (compare top-screen pixels).
        from PIL import Image, ImageChops
        tops = [Path(s["shot"]["top"]) for s in steps if s["name"] in ("zoom-in-4",) or s["name"].startswith("pan-")]
        tops = tops[:7]
        moved = sum(1 for a, b in zip(tops, tops[1:])
                    if ImageChops.difference(Image.open(a).convert("RGB"), Image.open(b).convert("RGB")).getbbox())
        chk.ok("pan: overview taps move the visible region", moved >= 3, f"{moved}/{len(tops) - 1} steps changed the top screen")
    finally:
        record["stop"] = await run.stop("pdfzoom")


async def main(args):
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    chk, record = Check(), {"rom": str(args.rom), "role": args.role}
    parameters = StdioServerParameters(command=sys.executable, args=["-m", "azahar_mcp.server"],
                                       cwd=str(args.mcp_repo.resolve()))
    with (output / "mcp-server.log").open("w") as errors:
        async with stdio_client(parameters, errlog=errors) as (read, write):
            async with ClientSession(read, write) as session:
                await session.initialize()
                try:
                    await scenario(session, args.rom, output, args.role, chk, record, not args.old3ds)
                except Exception as exc:  # keep partial evidence
                    import traceback
                    chk.ok("scenario completed without exception", False, f"{type(exc).__name__}: {exc}")
                    traceback.print_exc()
    record["checks"] = chk.items
    (output / "result.json").write_text(json.dumps(record, indent=2, default=str) + "\n")
    print(f"\n{len(chk.items) - len(chk.failed)}/{len(chk.items)} checks passed; evidence {output}")
    return 0 if not chk.failed else 1


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mcp-repo", type=Path, required=True)
    parser.add_argument("--rom", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--role", default="current")
    parser.add_argument("--old3ds", action="store_true", help="launch the emulator as an Old 3DS")
    parser.add_argument("--quick", action="store_true", help="stop after the zoom-in steps")
    _args = parser.parse_args()
    QUICK = _args.quick
    sys.exit(asyncio.run(main(_args)))

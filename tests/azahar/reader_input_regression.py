#!/usr/bin/env python3
"""Azahar reader input regression (#178 #182 #177 #181) on the debug 3dsx.

Usage (project root):
  AZ=/path/to/azahar-mcp
  $AZ/.venv/bin/python tests/azahar/reader_input_regression.py \
      --mcp-repo $AZ --rom 3dslibris-debug.3dsx --output <NEW_DIR> --scenarios 178 182 177 181

Every verdict comes from live memory reads (Book/App/SettingsController fields
resolved from the matching ELF's DWARF) plus screenshots, never from the app's
own self-report. Overlapping holds (Y held while D-pad is pressed) are produced
by writing the mailbox directly, because azahar_press_buttons blocks until the
command ends.
"""
import argparse
import asyncio
import json
import shutil
import struct
import sys
import time
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tests/azahar"))
import changes_lib as lib  # noqa: E402
from font_size_regression import Layout, Probe  # noqa: E402
from mcp import ClientSession  # noqa: E402
from mcp.client.stdio import StdioServerParameters, stdio_client  # noqa: E402
from azahar_mcp.input.live_buttons import key_mask, FRAME_MS  # noqa: E402

BASE = "3ds/3dslibris"
MB = "g_azahar_mcp_input"


class Types(lib.Types):
    WANTED = lib.Types.WANTED | {"ReaderRuntimeState", "NavigationState", "PrefsViewState",
                                 "SettingsController", "BrowserState"}


class Check:
    def __init__(self):
        self.items = []

    def ok(self, name, cond, detail=""):
        self.items.append({"name": name, "ok": bool(cond), "detail": str(detail)})
        print(("  PASS " if cond else "  FAIL ") + name + (f"  [{detail}]" if detail else ""), flush=True)
        return bool(cond)

    @property
    def failed(self):
        return [c for c in self.items if not c["ok"]]


def stage(dest, landscape, flip=0, circle_pad_page_turn=0):
    """Repo fixture tests/fixtures/epub-render-test zipped fresh into a copied SDMC."""
    app = Path(dest) / BASE
    (app / "book").mkdir(parents=True)
    shutil.copytree(ROOT / "sdmc" / BASE / "font", app / "font")
    (app / "3dslibris.xml").write_text(
        '<?xml version="1.0"?><dslibris><screen colorMode="0" flip="%d" landscape="%d"/>'
        '<font size="12"/><paragraph indent="0" spacing="0"/>'
        '<option swapshoulder="0" time24h="1" showTimeRemaining="0" browserView="gallery" '
        'fixedLayoutRtl="0" fixedLayoutWideTopScreen="0" circlePadPageTurn="%d" librarySortMode="0" '
        'disableEbookImages="0" />'
        '<books path="sdmc:/3ds/3dslibris/book" reopen="0"/></dslibris>' % (flip, landscape, circle_pad_page_turn),
        encoding="utf-8")
    src = ROOT / "tests/fixtures/epub-render-test"
    with zipfile.ZipFile(app / "book/render-test.epub", "w") as z:
        z.write(src / "mimetype", "mimetype", zipfile.ZIP_STORED)
        for p in sorted(src.rglob("*")):
            if p.is_file() and p.name != "mimetype":
                z.write(p, p.relative_to(src).as_posix(), zipfile.ZIP_DEFLATED)
    return Path(dest)


class Run:
    def __init__(self, session, rom, outdir, label):
        self.rom, self.out, self.label = Path(rom).resolve(), Path(outdir), label
        elf = self.rom.with_suffix(".elf")
        self.p = Probe(session, self.out, Layout(elf))
        self.t = Types(elf)
        self.m = lib.Session(self.p, self.t)
        self.pid = None

    async def start(self, sdmc):
        r = await self.p.call("azahar_launch", rom_path=str(self.rom), source_sdmc=str(sdmc),
                              sdmc_mode="copy", app_logs={"app_log": f"{BASE}/3dslibris.log"},
                              log_archive_dir=str(self.out / f"logs-{self.label}"))
        if not r["has_elf"] or not r["has_mailbox"]:
            raise RuntimeError("ELF/mailbox unavailable")
        self.pid = r.get("pid")
        await self.p.mode(1)
        return r

    async def stop(self):
        return await self.p.call("azahar_stop", label=self.label)

    # ---- raw input (overlapping holds) ----
    async def inject(self, names, ms):
        """Start a hold and return immediately; a later inject() replaces it
        without a release edge for keys present in both."""
        frames = max(1, round(ms / FRAME_MS))
        body = struct.pack("<IIIHH", key_mask(names), frames, 0, 0, 0)
        base = await self._mb_addr()
        await self.p.call("azahar_write_memory", target=hex(base + 0x10), data=body.hex(), format="bytes")
        import random
        await self.p.call("azahar_write_memory", target=hex(base + 0x08),
                          data=str(random.randint(1, 0xFFFFFFFE)), format="u32")

    async def _mb_addr(self):
        if not hasattr(self, "_mbaddr"):
            syms = await self.p.call("azahar_find_symbols", pattern=MB, limit=5)
            self._mbaddr = int(syms[0]["address"], 0) if isinstance(syms[0]["address"], str) else syms[0]["address"]
        return self._mbaddr

    async def idle(self, ms):
        await asyncio.sleep(ms / 1000)

    # ---- state reads ----
    async def app(self):
        return await self.p.number("App::s_instance_")

    async def book(self):
        return await self.m.book()

    async def pos(self):
        return await self.m.position(await self.book())

    async def pagecount(self):
        _, n = await self.m.pages(await self.book())
        return n

    async def focus(self):
        a = await self.app()
        active = await self.m.u8(a + self.t.at("App", "reader_state_") + self.t.at("ReaderRuntimeState", "inline_link_focus_active"))
        b = await self.book()
        raw = await self.m.u32(b + self.t.at("Book", "focused_inline_link_index"))
        idx = raw - (1 << 32) if raw >= 1 << 31 else raw
        return bool(active), idx

    async def bookmarks(self):
        b = await self.book()
        head = b + self.t.at("Book", "bookmarks")
        node, out = await self.m.u32(head), []
        while node != head and len(out) < 100:
            out.append(await self.m.u16(node + 8))
            node = await self.m.u32(node)
        return out

    async def prefs_state(self):
        a = await self.app()
        nav = a + self.t.at("App", "nav_")
        sel = await self.m.u32(nav + self.t.at("NavigationState", "prefs") + self.t.at("PrefsViewState", "selected_index"))
        sc = await self.m.u32(a + self.t.at("App", "settings_controller_"))
        page = await self.m.u32(sc + self.t.at("SettingsController", "prefs_general_page_"))
        from_book = await self.m.u8(nav + self.t.at("NavigationState", "prefs") + self.t.at("PrefsViewState", "from_book"))
        return {"page": page, "sel": sel, "from_book": bool(from_book)}

    async def shot(self, name):
        try:
            return await self.p.capture(name, self.pid)
        except Exception as e:  # screen locked etc.
            print("    screenshot unavailable:", e, flush=True)
            return None

    async def open_book(self):
        await self.p.press("A")
        await self.p.mode(0)
        await asyncio.sleep(1.5)

    async def goto(self, target):
        for _ in range(300):
            pos = await self.pos()
            if pos == target:
                return
            await self.p.press("R" if target > pos else "L", min(abs(target - pos), 10))
            await asyncio.sleep(0.3)
        raise RuntimeError(f"cannot reach page {target}")


async def sc_178(sess, rom, out, chk, rec):
    """Landscape: bookmark jumps on UP/DOWN, hold-repeat on LEFT/RIGHT, Circle Pad single press."""
    print("== #178 landscape", flush=True)
    root = Path(out) / "sdmc-178"
    stage(root, landscape=1, circle_pad_page_turn=0)
    r = Run(sess, rom, out, "178")
    try:
        await r.start(root)
        await r.open_book()
        n = await r.pagecount()
        rec["178_pages"] = n
        chk.ok("178 book resident, >=12 pages", n >= 12, n)
        await r.goto(3)
        await r.p.press("Y")  # tap Y = toggle bookmark
        await asyncio.sleep(0.5)
        await r.goto(7)
        await r.p.press("Y")
        await asyncio.sleep(0.5)
        bm = await r.bookmarks()
        rec["178_bookmarks"] = bm
        chk.ok("178 bookmarks on pages 3 and 7", sorted(bm) == [3, 7], bm)
        await r.goto(0)
        trail = []
        for key in ["DOWN", "DOWN", "DOWN", "UP", "UP", "UP"]:
            await r.p.press(key)
            await asyncio.sleep(0.4)
            trail.append((key, await r.pos()))
        rec["178_trail"] = trail
        print("    trail", trail, flush=True)
        # Bookmark jumps wrap around (FindBookmarkJumpTarget), and only ever land on bookmarks.
        chk.ok("178 DOWN x3 visits bookmarks 3, 7, wraps to 3", [p for _, p in trail[:3]] == [3, 7, 3], trail)
        chk.ok("178 UP x3 visits bookmarks 7, 3, wraps to 7", [p for _, p in trail[3:]] == [7, 3, 7], trail)
        await r.shot("178-after-jumps")
        # D-pad RIGHT: tap = exactly 1 page
        await r.goto(0)
        await r.p.press("RIGHT")
        await asyncio.sleep(0.5)
        chk.ok("178 D-pad RIGHT tap = +1 page", await r.pos() == 1, await r.pos())
        # held RIGHT repeats
        before = await r.pos()
        await r.inject("RIGHT", 1500)
        await r.idle(2200)
        after = await r.pos()
        rec["178_dright_hold"] = (before, after)
        chk.ok("178 D-pad RIGHT hold 1.5s repeats (>=3 pages)", after - before >= 3, f"{before}->{after}")
        before = await r.pos()
        await r.inject("LEFT", 1500)
        await r.idle(2200)
        after = await r.pos()
        rec["178_dleft_hold"] = (before, after)
        chk.ok("178 D-pad LEFT hold 1.5s repeats backwards (>=3 pages)", before - after >= 3, f"{before}->{after}")
        # Circle Pad, pref off: one page per press, never repeats
        await r.goto(2)
        await r.inject("CPAD_RIGHT", 100)
        await r.idle(800)
        chk.ok("178 Circle Pad RIGHT tap (pref off) = +1", await r.pos() == 3, await r.pos())
        await r.inject("CPAD_RIGHT", 1500)
        await r.idle(2200)
        p = await r.pos()
        chk.ok("178 Circle Pad RIGHT hold 1.5s (pref off) = exactly +1", p == 4, p)
        await r.inject("CPAD_LEFT", 1500)
        await r.idle(2200)
        p = await r.pos()
        chk.ok("178 Circle Pad LEFT hold 1.5s (pref off) = exactly -1", p == 3, p)
        # Circle Pad vertical must not bookmark-jump or turn pages differently than before.
        await r.inject("CPAD_DOWN", 1500)
        await r.idle(2200)
        p = await r.pos()
        rec["178_cpad_down"] = p
        chk.ok("178 Circle Pad DOWN hold jumps to next bookmark once (7), no page walk", p == 7, p)
    finally:
        rec["178_stop"] = await r.stop()


async def sc_182(sess, rom, out, chk, rec, landscape):
    tag = "182-" + ("land" if landscape else "port")
    print(f"== #182 link focus ({'landscape' if landscape else 'portrait'})", flush=True)
    root = Path(out) / f"sdmc-{tag}"
    stage(root, landscape=int(landscape), circle_pad_page_turn=0)
    r = Run(sess, rom, out, tag)
    nextk, prevk = ("DOWN", "UP") if landscape else ("DOWN", "UP")  # DDOWN/DUP in both
    try:
        await r.start(root)
        await r.open_book()
        start_pos = await r.pos()
        chk.ok(f"{tag} no focus before hold", (await r.focus())[0] is False)
        # Hold Y alone > 350 ms, keep holding.
        await r.inject("Y", 6000)
        await r.idle(700)
        active, idx = await r.focus()
        chk.ok(f"{tag} Y hold >350ms enters link focus", active, (active, idx))
        await r.shot(f"{tag}-focus-entered")
        # While Y is still held, replace the command with Y+DOWN: Y is continuous (no new edge).
        trace = []
        await r.inject(["Y", nextk], 2200)
        for _ in range(8):
            await r.idle(250)
            a, i = await r.focus()
            trace.append((await r.pos(), a, i))
        rec[f"{tag}_phaseA"] = trace
        print("    Y held + next:", trace, flush=True)
        moved = len({(p, i) for p, _, i in trace}) - 1
        chk.ok(f"{tag} focus advances repeatedly while Y still held (>=3 distinct steps)", moved >= 3, trace)
        chk.ok(f"{tag} focus stays active (Y not re-edged)", all(a for _, a, _ in trace), trace)
        await r.idle(800)  # everything released now (command expired)
        a, i = await r.focus()
        chk.ok(f"{tag} still in focus after release", a, (a, i))
        # Y released: hold next only, long enough to cross pages.
        p0 = await r.pos()
        trace = []
        await r.inject(nextk, 6000)
        for _ in range(24):
            await r.idle(250)
            a, i = await r.focus()
            trace.append((await r.pos(), a, i))
        rec[f"{tag}_phaseB"] = trace
        print("    next only:", trace, flush=True)
        steps = len({(p, i) for p, _, i in trace}) - 1
        chk.ok(f"{tag} focus keeps advancing with Y released (>=6 distinct steps)", steps >= 6, trace)
        chk.ok(f"{tag} focus crosses to a later page", trace[-1][0] > p0, f"{p0}->{trace[-1][0]}")
        await r.shot(f"{tag}-focus-later-page")
        # Let the command expire, then confirm it stops.
        await r.idle(800)
        s1 = (await r.pos(), (await r.focus())[1])
        await r.idle(1500)
        s2 = (await r.pos(), (await r.focus())[1])
        chk.ok(f"{tag} release stops movement", s1 == s2, (s1, s2))
        # Backwards with prev held.
        trace = []
        await r.inject(prevk, 2500)
        for _ in range(8):
            await r.idle(250)
            trace.append((await r.pos(), (await r.focus())[1]))
        await r.idle(800)
        rec[f"{tag}_prev"] = trace
        chk.ok(f"{tag} prev hold moves focus back repeatedly (>=3 steps)", len(set(trace)) - 1 >= 3, trace)
        # Leaving focus: B exits; no extra page turn.
        pbefore = await r.pos()
        await r.p.press("B")
        await asyncio.sleep(0.6)
        a, _ = await r.focus()
        chk.ok(f"{tag} B leaves link focus", not a)
        chk.ok(f"{tag} leaving focus does not turn the page", await r.pos() == pbefore, (pbefore, await r.pos()))
    finally:
        rec[f"{tag}_stop"] = await r.stop()


async def sc_177(sess, rom, out, chk, rec):
    print("== #177 HUD bookmark marker", flush=True)
    for landscape in (1, 0):
        tag = "177-" + ("land" if landscape else "port")
        root = Path(out) / f"sdmc-{tag}"
        stage(root, landscape=landscape)
        r = Run(sess, rom, out, tag)
        try:
            await r.start(root)
            await r.open_book()
            await r.goto(2)
            await asyncio.sleep(0.8)
            rec[f"{tag}_shots"] = {"before": await r.shot(f"{tag}-before")}
            chk.ok(f"{tag} no bookmark initially", await r.bookmarks() == [], await r.bookmarks())
            await r.p.press("Y")
            await asyncio.sleep(1.0)
            bm = await r.bookmarks()
            chk.ok(f"{tag} Y tap adds bookmark on page 2", bm == [2], bm)
            rec[f"{tag}_shots"]["marked"] = await r.shot(f"{tag}-marked")
            await r.p.press("Y")
            await asyncio.sleep(1.0)
            chk.ok(f"{tag} second Y tap removes it", await r.bookmarks() == [], await r.bookmarks())
            rec[f"{tag}_shots"]["unmarked"] = await r.shot(f"{tag}-unmarked")
        finally:
            rec[f"{tag}_stop"] = await r.stop()


async def sc_181(sess, rom, out, chk, rec):
    print("== #181 settings paging", flush=True)
    for flip in (0, 1):
        hand = "turned-right" if flip else "turned-left"
        prev = "LEFT" if flip else "RIGHT"
        nxt = "RIGHT" if flip else "LEFT"
        for use in ("dpad", "shoulder"):
            tag = f"181-{hand}-{use}"
            root = Path(out) / f"sdmc-{tag}"
            stage(root, landscape=0, flip=flip)
            r = Run(sess, rom, out, tag)
            # key.l/key.r swap with handedness (App::ApplyRenderOrientation): logical next/prev
            # shoulder is physical R/L when turned-left and L/R when turned-right.
            kn, kp = (nxt, prev) if use == "dpad" else (("L", "R") if flip else ("R", "L"))
            try:
                await r.start(root)
                # Library context
                await r.p.press("SELECT")
                await r.p.mode(2)
                await asyncio.sleep(0.5)
                s = await r.prefs_state()
                chk.ok(f"{tag} library settings opens on page 0", s["page"] == 0 and not s["from_book"], s)
                # walk forward to last slot of page 0
                last = None
                for _ in range(40):
                    before = await r.prefs_state()
                    await r.p.press(kn)
                    await asyncio.sleep(0.25)
                    after = await r.prefs_state()
                    if after["page"] != before["page"]:
                        last = before["sel"]
                        break
                chk.ok(f"{tag} library last slot + next -> page 2 slot 0", last is not None and after["page"] == 2 and after["sel"] == 0, (last, after))
                await r.shot(f"{tag}-lib-page2")
                await r.p.press(kp)
                await asyncio.sleep(0.3)
                back = await r.prefs_state()
                chk.ok(f"{tag} library page2 slot0 + prev -> page 0 LAST slot", back["page"] == 0 and back["sel"] == last, (last, back))
                await r.shot(f"{tag}-lib-page0-last")
                # page 0 slot 0 + prev: stays
                await r.p.press(kp, 20)
                await asyncio.sleep(0.3)
                s = await r.prefs_state()
                chk.ok(f"{tag} library page 0 slot 0 + prev stays", s["page"] == 0 and s["sel"] == 0, s)
                # style customization -> page 1
                await r.p.press("A")
                await asyncio.sleep(0.5)
                s = await r.prefs_state()
                chk.ok(f"{tag} style customization opens page 1", s["page"] == 1 and s["sel"] == 0, s)
                await r.p.press(kp)
                await asyncio.sleep(0.3)
                s = await r.prefs_state()
                chk.ok(f"{tag} library page1 slot0 + prev -> page 0 LAST slot", s["page"] == 0 and s["sel"] == last, (last, s))
                # leave settings, open a book, book context
                await r.p.press("B")
                await r.p.mode(1)
                await r.open_book()
                await r.p.press("SELECT")
                await r.p.mode(2)
                await asyncio.sleep(0.5)
                s = await r.prefs_state()
                chk.ok(f"{tag} book settings page 0", s["page"] == 0 and s["from_book"], s)
                await r.p.press(kp, 30)
                last_b = None
                for _ in range(40):
                    before = await r.prefs_state()
                    await r.p.press(kn)
                    await asyncio.sleep(0.25)
                    after = await r.prefs_state()
                    if after["page"] != before["page"]:
                        last_b = before["sel"]
                        break
                chk.ok(f"{tag} book last slot + next -> page 1 slot 0", last_b is not None and after["page"] == 1 and after["sel"] == 0, (last_b, after))
                await r.p.press(kp)
                await asyncio.sleep(0.3)
                s = await r.prefs_state()
                chk.ok(f"{tag} book page1 slot0 + prev -> page 0 LAST slot", s["page"] == 0 and s["sel"] == last_b, (last_b, s))
                # UP/DOWN unchanged: font size row still changes value
                rec[f"{tag}_last"] = (last, last_b)
                if use == "dpad":
                    await r.p.press(kp, 30)
                    await r.p.press("A")
                    await asyncio.sleep(0.4)
                    st = await r.prefs_state()
                    if st["page"] == 1:
                        await r.p.press(kn, 2)  # font size row
                        b = await r.book()
                        v0 = await r.p.number(hex(b + r.t.at("Book", "style_font_size_override")))
                        await r.p.press("DOWN")
                        await asyncio.sleep(0.3)
                        v1 = await r.p.number(hex(b + r.t.at("Book", "style_font_size_override")))
                        await r.p.press("UP")
                        await asyncio.sleep(0.3)
                        v2 = await r.p.number(hex(b + r.t.at("Book", "style_font_size_override")))
                        rec[f"{tag}_fontsize"] = (v0, v1, v2)
                        # v0 == 0xFFFFFFFF: no per-book override yet (base size 12). Value axis is
                        # reversed by handedness: turned-left DOWN = +1, turned-right DOWN = -1.
                        exp = 13 if not flip else 11
                        chk.ok(f"{tag} UP/DOWN still change font size (DOWN -> {exp}, UP back to 12)", v1 == exp and v2 == 12, (v0, v1, v2))
                    else:
                        chk.ok(f"{tag} could reach book page 1 for UP/DOWN check", False, st)
            finally:
                rec[f"{tag}_stop"] = await r.stop()


async def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--mcp-repo", type=Path, required=True)
    ap.add_argument("--rom", type=Path, required=True)
    ap.add_argument("--output", type=Path, required=True)
    ap.add_argument("--scenarios", nargs="+", default=["178", "182", "177", "181"])
    a = ap.parse_args()
    a.output = a.output.resolve()
    a.rom = a.rom.resolve()
    a.output.mkdir(parents=True, exist_ok=False)
    chk, rec = Check(), {"rom": str(a.rom)}
    params = StdioServerParameters(command=sys.executable, args=["-m", "azahar_mcp.server"], cwd=str(a.mcp_repo.resolve()))
    with (a.output / "mcp-server.log").open("w") as err:
        async with stdio_client(params, errlog=err) as (rd, wr):
            async with ClientSession(rd, wr) as sess:
                await sess.initialize()
                for s in a.scenarios:
                    try:
                        if s == "178":
                            await sc_178(sess, a.rom, a.output, chk, rec)
                        elif s == "182":
                            await sc_182(sess, a.rom, a.output, chk, rec, True)
                            await sc_182(sess, a.rom, a.output, chk, rec, False)
                        elif s == "177":
                            await sc_177(sess, a.rom, a.output, chk, rec)
                        elif s == "181":
                            await sc_181(sess, a.rom, a.output, chk, rec)
                    except Exception as e:
                        import traceback
                        traceback.print_exc()
                        chk.ok(f"scenario {s} ran to completion", False, repr(e))
    rec["checks"] = chk.items
    (a.output / "result.json").write_text(json.dumps(rec, indent=1, default=str))
    print(f"\n{len(chk.items) - len(chk.failed)}/{len(chk.items)} checks passed")
    return 1 if chk.failed else 0


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))

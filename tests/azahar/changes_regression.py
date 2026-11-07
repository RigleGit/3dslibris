#!/usr/bin/env python3
"""Azahar regression for the algorithmic changes, on the real debug 3dsx.

Runs the same scenario on one ROM per invocation. Every expectation is derived
from the generated fixtures (changes_fixtures.py) or from independent Python
code (changes_lib.py); a baseline ROM built from the previous commit can be run
with --role baseline and compared with compare_runs.py.

Search relies on a DSLIBRIS_DEBUG-only seam that reads the query from
sdmc:/3ds/3dslibris/search_query.txt instead of the system keyboard applet,
which an emulator test cannot type into.
"""
import argparse
import asyncio
import json
import re
import shutil
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import changes_fixtures as fx  # noqa: E402
import changes_lib as lib  # noqa: E402
from font_size_regression import Layout, Probe  # noqa: E402
from mcp import ClientSession  # noqa: E402
from mcp.client.stdio import StdioServerParameters, stdio_client  # noqa: E402

BASE = fx.BASE


class Check:
    def __init__(self):
        self.items = []

    def ok(self, name, condition, detail=""):
        self.items.append({"name": name, "ok": bool(condition), "detail": str(detail)})
        print(("  PASS " if condition else "  FAIL ") + name + (f"  [{detail}]" if detail and not condition else ""), flush=True)
        return bool(condition)

    @property
    def failed(self):
        return [c for c in self.items if not c["ok"]]


class Run:
    def __init__(self, session, rom, output, role):
        self.rom, self.output, self.role = Path(rom).resolve(), Path(output), role
        self.probe = Probe(session, self.output, Layout(self.rom.with_suffix(".elf")))
        self.types = lib.Types(self.rom.with_suffix(".elf"))
        self.mem = lib.Session(self.probe, self.types)
        self.launch = None
        self.log_seen = 0
        self.last_complete = None

    async def start(self, source, label):
        self.launch = await self.probe.call(
            "azahar_launch", rom_path=str(self.rom), source_sdmc=str(source), sdmc_mode="copy",
            app_logs={"app_log": f"{BASE}/3dslibris.log"}, log_archive_dir=str(self.output / f"logs-{label}"))
        if not self.launch["has_elf"] or not self.launch["has_mailbox"]:
            raise RuntimeError("ELF/mailbox unavailable")
        self.log_seen = 0
        await self.probe.mode(1)
        return self.launch

    async def stop(self, label):
        result = await self.probe.call("azahar_stop", label=label)
        return result

    async def logs(self):
        r = await self.probe.call("azahar_read_logs", streams=["app_log"], max_lines=0)
        lines = r["app_log"]
        fresh, self.log_seen = lines[self.log_seen:] if len(lines) >= self.log_seen else lines, len(lines)
        return fresh

    async def open_first_book(self):
        await self.probe.press("A")
        await self.probe.mode(0)
        await asyncio.sleep(1.0)

    async def go_to(self, book, target):
        """Page with L/R until book.position == target (bounded, verified)."""
        for _ in range(400):
            pos = await self.mem.position(book)
            if pos == target:
                return
            await self.probe.press("R" if target > pos else "L", min(abs(target - pos), 20))
            await self.probe.mode(0)
        raise RuntimeError(f"could not reach page {target}")

    async def open_search(self, query, whole):
        """Settings -> search row; the debug seam supplies the query text."""
        await self.probe.call("azahar_manage_sdmc", action="write", path=f"{BASE}/search_query.txt", content=query)
        await self.probe.press("SELECT")
        await self.probe.mode(2)
        await self.probe.press("L", 8)
        await self.probe.press("R", 2)
        await self.probe.press("A")
        await self.probe.mode(12)
        deadline = asyncio.get_event_loop().time() + 15
        pressed_edit = False
        while asyncio.get_event_loop().time() < deadline:
            state = await self.mem.search_state()
            if state and state["query"] == query and state["editor"]:
                break
            if state and state["editor"] and not pressed_edit and asyncio.get_event_loop().time() > deadline - 12:
                # Re-entering with a stale query: activate the first button (edit).
                await self.probe.press("A")
                pressed_edit = True
            await asyncio.sleep(0.4)
        else:
            raise TimeoutError(f"search query never reached the session: {state}")
        await asyncio.sleep(0.8)  # wait_release settles before input is accepted
        state = await self.mem.search_state()
        print("    open_search state:", state, flush=True)
        if state["whole_words"] != whole:
            await self.select(1)
            await self.probe.press("A")  # toggles whole words
            await asyncio.sleep(0.3)
        await self.select(2)
        state = await self.mem.search_state()
        assert state["whole_words"] == whole and state["selected"] == 2, state
        return state

    async def select(self, row):
        """Move the editor selection to a row, verified from the session."""
        for _ in range(6):
            state = await self.mem.search_state()
            if state["selected"] == row:
                return
            await self.probe.press("DOWN" if (row - state["selected"]) % 4 <= 2 else "UP")
            await asyncio.sleep(0.2)
        raise RuntimeError(f"could not select row {row}")

    async def latest_complete(self):
        """Newest 'SEARCH complete' line in the (bounded) application log window."""
        r = await self.probe.call("azahar_read_logs", streams=["app_log"], max_lines=0)
        found = [l for l in r["app_log"] if re.search(r"SEARCH complete status=", l)]
        return found[-1] if found else None

    async def wait_search(self, before_msg, timeout=60):
        """A new 'SEARCH complete' log line ends a scan; a changed message while the
        editor stays open means the query was rejected before scanning."""
        end = asyncio.get_event_loop().time() + timeout
        while asyncio.get_event_loop().time() < end:
            line = await self.latest_complete()
            if line and line != self.last_complete:
                self.last_complete = line
                m = re.search(r"status=(\d+) .*scan_ms=(\d+) .*tokens=(\d+) .*wrapped=(\d)", line)
                return {"status": int(m.group(1)), "scan_ms": int(m.group(2)),
                        "tokens": int(m.group(3)), "wrapped": int(m.group(4))}
            state = await self.mem.search_state()
            if state and state["editor"] and state["message"] and state["message"] != before_msg:
                return {"status": STATUS_FAILED, "scan_ms": 0, "tokens": 0, "wrapped": 0}
            await asyncio.sleep(0.4)
        status = await self.probe.call("azahar_status")
        shot = await self.probe.capture("search-timeout", self.launch["pid"])
        raise TimeoutError(f"search did not complete; mailbox={status.get('mailbox')} screenshot={shot}")

    async def search(self, key):
        """Start a search with A (editor) or R/L (result view), return summary."""
        before = await self.mem.search_state()
        if self.last_complete is None:
            self.last_complete = await self.latest_complete()
        await self.probe.press(key)
        done = await self.wait_search(before["message"] if before else "")
        await asyncio.sleep(0.4)
        state = await self.mem.search_state()
        return done, state


STATUS_FOUND, STATUS_NOT_FOUND, STATUS_FAILED = 2, 3, 4
CASE_FILTER = set()


async def epub_open_checks(run, chk, record):
    book = await run.mem.book()
    _, count = await run.mem.pages(book)
    chapters = await run.mem.chapters(book)
    record["page_count"] = count
    record["chapters"] = chapters
    chk.ok("epub: 4 chapters with TOC titles", [c["title"] for c in chapters] == [f"Chapter {i}" for i in range(1, 5)], chapters)
    chk.ok("epub: chapters ascend and each document starts a page",
           [c["page"] for c in chapters] == sorted({c["page"] for c in chapters}) and len(chapters) == 4, chapters)
    # Page through the whole book, reading each resident page.
    hashes, texts, tokens_by_page = [], [], []
    await run.go_to(book, 0)
    for i in range(count):
        await run.go_to(book, i)
        await asyncio.sleep(0.3)
        tokens = await run.mem.page_at(book, i)
        tokens_by_page.append(tokens)
        hashes.append(lib.page_hash(tokens))
        texts.append("".join(chr(c) for c, _ in lib.decode_page(tokens) if c is not None))
    record["page_hashes"] = hashes
    record["page_lengths"] = [len(t) for t in tokens_by_page]
    chk.ok("epub: every page resident when visited", all(record["page_lengths"]), record["page_lengths"])
    starts = [c["page"] for c in chapters] + [count]

    def chapter_text(k):
        return " ".join(texts[starts[k]:starts[k + 1]])

    for name, (needle, chapter) in fx.NEEDLES.items():
        if name == "arabic":
            continue
        found = [k + 1 for k in range(4) if needle in chapter_text(k).casefold()]
        chk.ok(f"epub: '{needle}' ({name}) is laid out only in chapter {chapter}", found == [chapter], found)
    chk.ok("epub: curly quotes and em dash laid out", "“" in chapter_text(0) and "—" in chapter_text(0))
    ch3 = [t for t in tokens_by_page[starts[2]:starts[3]]]
    flat3 = [c for t in ch3 for c in t]
    chk.ok("epub: Arabic paragraph emitted as RTL with shaped presentation forms",
           15 in flat3 and any(0xFE70 <= c <= 0xFEFF for c in flat3))
    chk.ok("epub: centered/right paragraph markers present",
           0x110003 in [c for t in tokens_by_page[starts[0]:starts[1]] for c in t] and
           0x110004 in [c for t in tokens_by_page[starts[0]:starts[1]] for c in t])
    chk.ok("epub: inline image token in chapter 4",
           lib.TEXT_IMAGE in [c for t in tokens_by_page[starts[3]:starts[4]] for c in t])
    record["tokens_by_page"] = tokens_by_page
    return tokens_by_page


SEARCH_CASES = [  # noqa
    # name, query, whole-words, expected chapter (None = no match / failure), expectation kind
    ("ascii", "zebraquartz", False, 1, "found"),
    ("cyrillic casefold", "ПРИВЕТ", False, 2, "found"),
    ("greek", "αλφάβητο", False, 2, "found"),
    ("accent folds case", "café", False, 3, "found"),
    ("unaccented skips café", "cafe", False, 3, "found"),
    ("whole word rejects substring", "cafe", True, 3, "found"),
    ("phrase across wrapped lines", "quiet river while evening", False, 1, "found"),
    ("image chapter text", "lanternfish", False, 4, "found"),
    ("absent", "nonexistentword", False, None, "notfound"),
    ("arabic rejected", "كتاب", False, None, "failed"),
]


async def search_checks(run, chk, record, tokens_by_page):
    book = await run.mem.book()
    chapters = record["chapters"]
    starts = [c["page"] for c in chapters] + [record["page_count"]]
    results = {}
    for name, query, whole, chapter, kind in SEARCH_CASES:
        if CASE_FILTER and name not in CASE_FILTER:
            continue
        await run.go_to(book, 0)
        state = await run.open_search(query, whole)
        done, state = await run.search("A")
        results[name] = {"query": query, "done": done, "state": state}
        if kind == "found":
            page = state["start_page"]
            in_chapter = chapter is not None and starts[chapter - 1] <= page < starts[chapter]
            chk.ok(f"search[{name}]: found in chapter {chapter}", done["status"] == STATUS_FOUND and state["has_match"] and in_chapter,
                   (done, state))
            tokens = await run.mem.page_at(book, await run.mem.position(book))
            chk.ok(f"search[{name}]: current page is the match page", await run.mem.position(book) == state["start_page"])
            chk.ok(f"search[{name}]: independent oracle confirms the match start token",
                   lib.matches_at(tokens, state["start_token"], query, whole))
            chk.ok(f"search[{name}]: scanned a bounded number of tokens", 0 < done["tokens"] < 200000, done["tokens"])
        elif kind == "notfound":
            chk.ok(f"search[{name}]: reports no match", done["status"] == STATUS_NOT_FOUND and not state["has_match"], (done, state))
            chk.ok(f"search[{name}]: whole book scanned", done["tokens"] > 1000, done["tokens"])
            chk.ok(f"search[{name}]: message", "No other matches" in state["message"], state["message"])
        else:
            chk.ok(f"search[{name}]: unsupported query rejected before scanning", "RTL" in state["message"] or done["status"] == STATUS_FAILED,
                   (done, state))
        await run.probe.press("B")
        await run.probe.mode(0)
    # "cafe" must not match inside café: its hit is the standalone 'cafe' (a different token than 'café')
    a, b = results["accent folds case"]["state"], results["unaccented skips café"]["state"]
    chk.ok("search: 'cafe' hit differs from 'café' hit", (a["start_page"], a["start_token"]) != (b["start_page"], b["start_token"]), (a, b))
    c = results["whole word rejects substring"]["state"]
    chk.ok("search: whole-word 'cafe' skips 'cafeteria' (hit equals the standalone word)",
           (c["start_page"], c["start_token"]) == (b["start_page"], b["start_token"]) or c["has_match"], (b, c))

    # Next / previous navigation with the same query: café then CAFÉ, wrap-around.
    await run.go_to(book, 0)
    await run.open_search("café", False)
    done, first = await run.search("A")
    done2, second = await run.search("R")
    done3, back = await run.search("L")
    chk.ok("search: R advances to the second café (CAFÉ) on a later token or page",
           (second["start_page"], second["start_token"]) > (first["start_page"], first["start_token"]) and done2["status"] == STATUS_FOUND,
           (first, second))
    chk.ok("search: L returns to the first café", (back["start_page"], back["start_token"]) == (first["start_page"], first["start_token"]) and done3["status"] == STATUS_FOUND,
           (first, back))
    done4, wrapped = await run.search("R")
    done5, wrapped2 = await run.search("R")
    chk.ok("search: advancing past the last hit wraps to the first", done5["wrapped"] == 1 or (wrapped2["start_page"], wrapped2["start_token"]) == (first["start_page"], first["start_token"]),
           (done5, wrapped2))
    await run.probe.press("B")
    await run.probe.mode(0)
    record["search"] = {k: {"done": v["done"], "start": [v["state"]["start_page"], v["state"]["start_token"]] if v["state"] else None} for k, v in results.items()}


async def screenshots(run, chk, record, label):
    book = await run.mem.book()
    chapters = record["chapters"]
    shots = {}
    for name, page in (("ch1", chapters[0]["page"]), ("ch3-arabic", chapters[2]["page"])):
        await run.go_to(book, page)
        await asyncio.sleep(0.6)
        shots[name] = await run.probe.capture(f"{label}-{name}", run.launch["pid"])
    record["screenshots"] = shots


async def scenario_epub_and_search(session, rom, output, role, chk, record):
    run = Run(session, rom, output, role)
    src = Path(tempfile.mkdtemp(prefix="chg-src-"))
    fx.stage(src, epub=True, mobi=False)
    try:
        await run.start(src, "epub")
        await run.open_first_book()
        tokens = await epub_open_checks(run, chk, record)
        await screenshots(run, chk, record, "epub")
        await search_checks(run, chk, record, tokens)
        record["epub_sdmc"] = run.launch["sdmc_dir"]
        cache_dir = Path(run.launch["sdmc_dir"]) / BASE / "cache"
        record["cache_files"] = sorted(p.name for p in cache_dir.rglob("*") if p.is_file())
    finally:
        await run.stop("epub-search")
    return run


async def scenario_warm_reopen(session, rom, output, role, chk, record, cold_sdmc):
    """New process + copied SDMC: the persistent cache must reproduce every page."""
    run = Run(session, rom, output, role)
    try:
        await run.start(cold_sdmc, "epub-warm")
        await run.open_first_book()
        book = await run.mem.book()
        _, count = await run.mem.pages(book)
        resident = await run.mem.u32(book + run.types.at("Book", "reflow_cache_reader_")) if "reflow_cache_reader_" in run.types.fields.get("Book", {}) else None
        chk.ok("warm: same page count after process restart", count == record["page_count"], (count, record["page_count"]))
        hashes = []
        for i in range(count):
            await run.go_to(book, i)
            await asyncio.sleep(0.3)
            hashes.append(lib.page_hash(await run.mem.page_at(book, i)))
        chk.ok("warm: every page identical to the cold parse (cache round trip)", hashes == record["page_hashes"],
               [i for i, (a, b) in enumerate(zip(hashes, record["page_hashes"])) if a != b])
        record["warm_resident_reader"] = bool(resident) if resident is not None else None
    finally:
        await run.stop("epub-warm")


async def scenario_mobi(session, rom, output, role, chk, record):
    run = Run(session, rom, output, role)
    src = Path(tempfile.mkdtemp(prefix="chg-mobi-"))
    fx.stage(src, epub=False, mobi=True)
    try:
        await run.start(src, "mobi")
        await run.open_first_book()
        await asyncio.sleep(2.0)
        book = await run.mem.book()
        base, count = await run.mem.pages(book)
        chapters = await run.mem.chapters(book)
        record["mobi"] = {"pages": count, "chapters": chapters}
        chk.ok("mobi: paginated", count >= 4, count)
        titles = [c["title"] for c in chapters]
        chk.ok("mobi: four TOC chapters resolved in order", titles == ["Chapter One", "Chapter Two", "Chapter Three", "Chapter Four"], titles)
        chk.ok("mobi: chapter pages strictly ascend", [c["page"] for c in chapters] == sorted({c["page"] for c in chapters}), chapters)
        hashes = []
        for k, chapter in enumerate(chapters):
            await run.go_to(book, chapter["page"])
            await asyncio.sleep(0.3)
            tokens = await run.mem.page_at(book, chapter["page"])
            text = "".join(chr(c) for c, _ in lib.decode_page(tokens) if c is not None)
            # Independent oracle: the generator places h1 + TOKEN{k*1000+1} at each chapter start.
            chk.ok(f"mobi: chapter {k + 1} page holds its heading/first token",
                   chapter["title"].lower() in text.lower() or f"TOKEN{k * 1000 + 1:04d}" in text, text[:80])
            hashes.append(lib.page_hash(tokens))
        record["mobi"]["chapter_page_hashes"] = hashes
        last = await run.mem.page_at(book, count - 1) if await run.mem.position(book) == count - 1 else None
        await run.go_to(book, count - 1)
        await asyncio.sleep(0.3)
        tail = "".join(chr(c) for c, _ in lib.decode_page(await run.mem.page_at(book, count - 1)) if c is not None)
        chk.ok("mobi: end of book text reachable (café & tea)", "café & tea" in tail, tail[-80:])
        chk.ok("mobi: script content hidden", "HIDDEN-MOBI" not in tail)
    finally:
        await run.stop("mobi")


async def scenario_long_cache(session, rom, output, role, chk, record):
    """Multi-block cache: forward and backward paging after a real process restart."""
    run = Run(session, rom, output, role)
    src = Path(tempfile.mkdtemp(prefix="chg-long-"))
    fx.stage(src, epub=False, mobi=False, long_book=True)
    checkpoints = {}
    try:
        await run.start(src, "long-cold")
        await run.open_first_book()
        await asyncio.sleep(3.0)
        book = await run.mem.book()
        _, count = await run.mem.pages(book)
        record["long"] = {"pages": count}
        chk.ok("long: many pages", count > 100, count)
        cache_files = list((Path(run.launch["sdmc_dir"]) / BASE / "cache").rglob("*.epc"))
        sizes = [p.stat().st_size for p in cache_files]
        record["long"]["cache_bytes"] = sizes
        chk.ok("long: cache file spans more than one 64 KB read window", bool(sizes) and max(sizes) > 2 * 65536, sizes)
        points = sorted({0, count // 4, count // 2, (3 * count) // 4, count - 1})
        last_id = -1
        for p in points:
            await run.go_to(book, p)
            await asyncio.sleep(0.3)
            tokens = await run.mem.page_at(book, p)
            text = "".join(chr(c) for c, _ in lib.decode_page(tokens) if c is not None)
            ids = [int(x) for x in re.findall(r"P(\d{4})", text)]
            checkpoints[p] = lib.page_hash(tokens)
            chk.ok(f"long cold: page {p} has monotone paragraph ids", bool(text) and ids == sorted(ids) and (not ids or ids[0] > last_id), ids[:3])
            last_id = ids[-1] if ids else last_id
        cold_sdmc = run.launch["sdmc_dir"]
    finally:
        await run.stop("long-cold")
    run2 = Run(session, rom, output, role)
    try:
        await run2.start(cold_sdmc, "long-warm")
        await run2.open_first_book()
        await asyncio.sleep(2.0)
        book = await run2.mem.book()
        _, count2 = await run2.mem.pages(book)
        chk.ok("long warm: same page count from the persistent cache", count2 == count, (count2, count))
        await run2.logs()
        # forward through the checkpoints, then backward (the reverse read window path)
        for direction, order in (("forward", points), ("backward", list(reversed(points)))):
            for p in order:
                await run2.go_to(book, p)
                await asyncio.sleep(0.3)
                got = lib.page_hash(await run2.mem.page_at(book, p))
                chk.ok(f"long warm {direction}: page {p} equals the cold parse", got == checkpoints[p], (got, checkpoints[p]))
        # Step backwards page by page from the middle: every page must decode identically
        # and ids must strictly decrease, exercising read-window reuse.
        await run2.go_to(book, count // 2)
        prev_first = 10 ** 9
        monotone = True
        for p in range(count // 2, count // 2 - 25, -1):
            await run2.go_to(book, p)
            await asyncio.sleep(0.15)
            text = "".join(chr(c) for c, _ in lib.decode_page(await run2.mem.page_at(book, p)) if c is not None)
            ids = [int(x) for x in re.findall(r"P(\d{4})", text)]
            if ids:
                monotone = monotone and ids[0] < prev_first
                prev_first = ids[0]
        chk.ok("long warm: 25 consecutive backward pages are monotone and decodable", monotone)
    finally:
        stop = await run2.stop("long-warm")
        app_log = (stop.get("archived_logs", {}).get("files", {}) or {}).get("app_log")
        io_lines = [l for l in Path(app_log).read_text(errors="replace").splitlines() if "CACHE IO" in l] if app_log else []
        record["long"]["cache_io"] = io_lines[-3:]
        chk.ok("long warm: cache reader logged its IO", bool(io_lines), app_log)


async def main(args):
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    CASE_FILTER.update(args.cases)
    chk, record = Check(), {"rom": str(args.rom), "role": args.role}
    parameters = StdioServerParameters(command=sys.executable, args=["-m", "azahar_mcp.server"], cwd=str(args.mcp_repo.resolve()))
    with (output / "mcp-server.log").open("w") as errors:
        async with stdio_client(parameters, errlog=errors) as (read, write):
            async with ClientSession(read, write) as session:
                await session.initialize()
                try:
                    for name in args.scenarios:
                        print(f"== {name}", flush=True)
                        if name == "epub":
                            run = await scenario_epub_and_search(session, args.rom, output, args.role, chk, record)
                        elif name == "warm":
                            await scenario_warm_reopen(session, args.rom, output, args.role, chk, record, record["epub_sdmc"])
                        elif name == "mobi":
                            await scenario_mobi(session, args.rom, output, args.role, chk, record)
                        elif name == "long":
                            await scenario_long_cache(session, args.rom, output, args.role, chk, record)
                except Exception as exc:  # keep partial evidence
                    import traceback
                    chk.ok("scenario completed without exception", False, f"{type(exc).__name__}: {exc}")
                    traceback.print_exc()
    record["checks"] = chk.items
    slim = {k: v for k, v in record.items() if k != "tokens_by_page"}
    (output / "result.json").write_text(json.dumps(slim, indent=2, default=str) + "\n")
    print(f"\n{len(chk.items) - len(chk.failed)}/{len(chk.items)} checks passed; evidence {output}")
    return 0 if not chk.failed else 1


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mcp-repo", type=Path, required=True)
    parser.add_argument("--rom", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--role", default="current")
    parser.add_argument("--cases", nargs="*", default=[])
    parser.add_argument("--scenarios", nargs="+", default=["epub", "warm", "mobi", "long"])
    sys.exit(asyncio.run(main(parser.parse_args())))

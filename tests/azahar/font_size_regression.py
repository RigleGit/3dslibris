#!/usr/bin/env python3
"""Exercise #172 through azahar-mcp's real stdio protocol and live UI.

This is an opt-in emulator test, never part of make test-host. No application
state is patched: memory writes only enable existing diagnostic controls.
"""

import argparse
import asyncio
import hashlib
import json
from pathlib import Path
import platform
import re
import shutil
import struct
import sys
import time
import zipfile

from elftools.elf.elffile import ELFFile
from mcp import ClientSession
from mcp.client.stdio import StdioServerParameters, stdio_client
from PIL import Image


ROOT = Path(__file__).resolve().parents[2]
BASE = "3ds/3dslibris"
SIZES = (8, 12, 14, 16, 18, 20)


class Layout:
    """Resolve actual member offsets from this ELF, never from a saved address."""

    def __init__(self, elf_path):
        self.fields = {}
        wanted = {"App", "Text", "Book", "FontManager", "ReaderRuntimeState",
                  "NavigationState", "PrefsViewState", "FT_FaceRec_",
                  "FT_SizeRec_", "FT_Size_Metrics_", "Page"}
        with elf_path.open("rb") as stream:
            elf = ELFFile(stream)
            if elf.elfclass != 32 or elf["e_machine"] != "EM_ARM":
                raise RuntimeError("Expected a 32-bit ARM ELF")
            # Final linked ARM ELF already has relocated DWARF values.
            dwarf = elf.get_dwarf_info(relocate_dwarf_sections=False)
            for cu in dwarf.iter_CUs():
                for die in cu.iter_DIEs():
                    name = die.attributes.get("DW_AT_name")
                    name = name.value.decode() if name and isinstance(name.value, bytes) else None
                    if name not in wanted or "DW_AT_byte_size" not in die.attributes:
                        continue
                    members = {}
                    for child in die.iter_children():
                        attrs = child.attributes
                        if child.tag == "DW_TAG_member" and "DW_AT_name" in attrs and "DW_AT_data_member_location" in attrs:
                            members[attrs["DW_AT_name"].value.decode()] = attrs["DW_AT_data_member_location"].value
                    self.fields[name] = members
                    wanted.remove(name)
                if not wanted:
                    break
        if wanted:
            raise RuntimeError(f"ELF lacks required DWARF types: {sorted(wanted)}")

    def at(self, type_name, field):
        value = self.fields[type_name][field]
        if not isinstance(value, int):
            raise RuntimeError(f"Unsupported DWARF member location: {type_name}.{field}")
        return value


def stage_fixture(destination, landscape=False):
    """Use only a generated book and bundled fonts, never the user's SDMC."""
    app = destination / BASE
    (app / "book").mkdir(parents=True)
    shutil.copytree(ROOT / "sdmc" / BASE / "font", app / "font")
    armenian = ROOT / "third_party/mupdf/resources/fonts/noto/NotoSerifArmenian-Regular.otf"
    shutil.copy2(armenian, app / "font" / armenian.name)
    (app / "3dslibris.xml").write_text(
        '<?xml version="1.0"?><dslibris><screen colorMode="0" flip="0" '
        f'landscape="{int(landscape)}"/><font size="12" '
        f'fallback1="{armenian.name}"/><paragraph indent="0" spacing="0"/>'
        '<books path="sdmc:/3ds/3dslibris/book" reopen="0"/></dslibris>', encoding="utf-8")
    sentence = "you last spring, I will not watch one of the letters disappear at the edge. "
    body = '<p>Ա</p>' + ''.join(f'<p>{sentence * 6}</p>' for _ in range(4))
    body += '<p style="text-align:center">Centered spaces and curly quotes ‘one two three’.</p>'
    body += '<p style="text-align:right">Right aligned: one two three four five.</p>'
    body += '<p>Unicode spaces: one&#160;two&#8194;three. 中文 日本語.</p>'
    body += '<p dir="rtl">שלום עולם مرحبا بالعالم</p>'
    with zipfile.ZipFile(app / "book/font-size-172.epub", "w") as archive:
        archive.writestr("mimetype", "application/epub+zip")
        archive.writestr("META-INF/container.xml", '<container xmlns="urn:oasis:names:tc:opendocument:xmlns:container" version="1.0"><rootfiles><rootfile full-path="content.opf" media-type="application/oebps-package+xml"/></rootfiles></container>')
        archive.writestr("content.opf", '<package xmlns="http://www.idpf.org/2007/opf" version="2.0" unique-identifier="id"><metadata xmlns:dc="http://purl.org/dc/elements/1.1/"><dc:identifier id="id">font-172</dc:identifier><dc:title>Ա Font size 172</dc:title><dc:language>en</dc:language></metadata><manifest><item id="body" href="body.xhtml" media-type="application/xhtml+xml"/></manifest><spine><itemref idref="body"/></spine></package>')
        archive.writestr("body.xhtml", '<html xmlns="http://www.w3.org/1999/xhtml"><head><title>Font size 172</title></head><body>' + body + '</body></html>')
    return destination


def clipping_evidence(remaining, logs):
    if not 0 < remaining <= 10000:
        raise RuntimeError("Clipping diagnostic budget is exhausted or invalid")
    events = []
    for line in logs.get("app_log", []):
        match = re.search(r"TXT clipR .* overflow=(\d+) clip=(\d+) .* style=(\d+)$", line)
        if match:
            events.append({"overflow": int(match[1]), "clip": int(match[2]), "style": int(match[3]), "line": line})
    # Counter/log agreement prevents a green result from disabled, capped,
    # buffered or lost diagnostics. UI glyphs may exceed the layout margin
    # without being clipped: style4 intentionally has a wider clip boundary.
    if len(events) != 10000 - remaining:
        raise RuntimeError(f"Incomplete clipping diagnostics: counter={10000-remaining}, log={len(events)}")
    clipped = [e for e in events if e["style"] != 4 and e["clip"] > 0]
    return {"clipping_events": len(clipped), "diagnostic_events": len(events), "events": events, "logs": logs}


def record_stop(report, response):
    report["stop"] = response
    if response.get("status") != "stopped" or response.get("returncode") != 0:
        report["status"] = "error"
        report["stop_error"] = f"Unclean emulator stop: {response}"


class Probe:
    def __init__(self, session, output, layout):
        self.session, self.output, self.layout = session, output, layout
        self.calls = []

    async def call(self, name, **arguments):
        result = (await self.session.call_tool(name, arguments)).model_dump(by_alias=True, exclude_none=True)
        self.calls.append({"tool": name, "arguments": arguments, "result": result})
        if result.get("isError"):
            raise RuntimeError(f"{name}: {result.get('content')}")
        value = result.get("structuredContent")
        if value is None:
            text = ''.join(c.get("text", "") for c in result.get("content", []) if c["type"] == "text")
            value = json.loads(text)
        if isinstance(value, dict) and set(value) == {"result"}:
            return value["result"]
        return value

    async def number(self, address, kind="u32"):
        result = await self.call("azahar_read_memory", target=str(address), size={"u8": 1, "u16": 2, "u32": 4}[kind], format=kind)
        return result["value"]

    async def symbol(self, name):
        symbols = await self.call("azahar_find_symbols", pattern=name, limit=10)
        exact = [s for s in symbols if s.get("kind") == "OBJECT" and
                 (s["name"] == name or s.get("demangled") == name or
                  s.get("demangled", "").endswith("::" + name))]
        if len(exact) != 1:
            raise RuntimeError(f"Missing or ambiguous symbol: {name}: {symbols}")
        return exact[0]["name"]

    async def write(self, symbol, value):
        await self.call("azahar_write_memory", target=symbol, data=str(value), format="u32")
        if await self.number(symbol) != value:
            raise RuntimeError(f"MCP write/read roundtrip failed for {symbol}")

    async def mode(self, expected, timeout=45):
        end = time.monotonic() + timeout
        previous = None
        while time.monotonic() < end:
            status = await self.call("azahar_status")
            mailbox = status.get("mailbox", {})
            if status.get("running") and mailbox.get("magic_ok") and mailbox["user"][0] == expected:
                if previous is not None and mailbox["frame"] > previous:
                    return mailbox
                previous = mailbox["frame"]
            await asyncio.sleep(0.15)
        raise TimeoutError(f"No advancing mailbox in app mode {expected}: {status}")

    async def press(self, key, times=1):
        result = await self.call("azahar_press_buttons", buttons=key, times=times, hold_ms=50, after_ms=100)
        state = result["state"]
        if state["seq"] != state["ack"]:
            raise RuntimeError("Input was not acknowledged by the application")

    async def pointers(self):
        app = await self.number("App::s_instance_")
        text = await self.number(hex(app + self.layout.at("App", "ts")))
        book = await self.number(hex(app + self.layout.at("App", "reader_state_") + self.layout.at("ReaderRuntimeState", "bookcurrent")))
        return app, text, book

    async def ppem(self, face):
        size = await self.number(hex(face + self.layout.at("FT_FaceRec_", "size")))
        return await self.number(hex(size + self.layout.at("FT_SizeRec_", "metrics") + self.layout.at("FT_Size_Metrics_", "y_ppem")), "u16")

    async def clipping(self, budget):
        remaining = await self.number(budget)
        logs = await self.call("azahar_read_logs", streams=["app_log"], max_lines=0)
        return clipping_evidence(remaining, logs)

    async def capture(self, label, pid):
        # The server has a desktop fallback. Require a target window first so
        # the regression never captures unrelated desktop content.
        if platform.system() == "Darwin":
            from azahar_mcp.capture.screen_capture import get_macos_window_id_for_pid
            if get_macos_window_id_for_pid(pid) is None:
                raise RuntimeError("Azahar window unavailable; refusing desktop capture")
        paths = await self.call("azahar_screenshot", split=True)
        saved = {}
        for kind, source in paths.items():
            path = Path(source)
            with Image.open(path) as image:
                image.verify()
            dest = self.output / f"{label}-{kind}.png"
            shutil.copy2(path, dest)
            saved[kind] = str(dest)
        return saved


async def run_one(session, rom, source, output, sizes, landscape):
    output.mkdir(parents=True)
    layout = Layout(rom.with_suffix(".elf"))
    if b"TXT clipR side=" not in rom.with_suffix(".elf").read_bytes():
        raise RuntimeError("TRACE clipping diagnostics are absent; use build_font_probe.sh")
    probe = Probe(session, output, layout)
    result = {"rom": str(rom), "sha256": hashlib.sha256(rom.read_bytes()).hexdigest(), "orientation": "landscape" if landscape else "portrait", "status": "error", "cases": []}
    try:
        launched = await probe.call("azahar_launch", rom_path=str(rom), source_sdmc=str(source), sdmc_mode="copy", app_logs={"app_log": f"{BASE}/3dslibris.log"}, log_archive_dir=str(output / "logs"))
        result["session"] = launched
        if not launched["has_elf"] or not launched["has_mailbox"]:
            raise RuntimeError("ELF/mailbox integration unavailable")
        await probe.mode(1)
        level = await probe.symbol("g_level")
        categories = await probe.symbol("g_categories")
        budget = await probe.symbol("g_text_clip_right_budget")
        await probe.write(level, 5)
        await probe.write(categories, 0xffffffff)
        await probe.press("A")
        await probe.mode(0)
        current_size = 12
        for px in sizes:
            await probe.press("SELECT")
            await probe.mode(2)
            # Settings always use portrait-left, including a landscape book.
            # SELECT retains the selected row: L clamps to row zero.
            await probe.press("L", 8)
            await probe.press("A")  # style customization -> book style page
            await probe.press("R", 2)  # font-size row
            if not result["cases"]:
                # Logical portrait-left (180,90) -> native touch (229,180).
                # Test a real tap as well as digital input, then verify its effect.
                tapped = await probe.call("azahar_touch_tap", x=229, y=180, hold_ms=50, after_ms=100)
                if tapped["status"] != "tapped_via_mailbox" or tapped["state"]["seq"] != tapped["state"]["ack"]:
                    raise RuntimeError("Touch input was not acknowledged via the mailbox")
                _, _, book = await probe.pointers()
                actual = await probe.number(hex(book + layout.at("Book", "style_font_size_override")))
                if actual != current_size + 1:
                    raise RuntimeError(f"Touch did not increase font size: {actual}")
                current_size += 1
            delta = px - current_size
            if delta:
                await probe.press("DOWN" if delta > 0 else "UP", abs(delta))
            else:
                await probe.press("DOWN")
                await probe.press("UP")
            _, _, book = await probe.pointers()
            override = await probe.number(hex(book + layout.at("Book", "style_font_size_override")))
            if override != px:
                raise RuntimeError(f"UI did not apply font-size override {px}; observed {override}")
            # Reset before returning to the reader, excluding settings labels.
            await probe.write(budget, 10000)
            await probe.call("azahar_read_logs", streams=["app_log"], max_lines=0)
            await probe.press("B")
            heartbeat = await probe.mode(0)
            await asyncio.sleep(0.25)
            _, text, book = await probe.pointers()
            applied = await probe.number(hex(text + layout.at("Text", "pixelsize")))
            fm = await probe.number(hex(text + layout.at("Text", "fm")))
            fallback = await probe.number(hex(fm + layout.at("FontManager", "fallback_faces_")))
            fallback_ppem = await probe.ppem(fallback)
            # Confirm real book content was laid out, not an empty/failed view.
            pages = await probe.number(hex(book + layout.at("Book", "pages")))
            page = await probe.number(hex(pages))
            buf = await probe.number(hex(page + layout.at("Page", "buf")))
            length = await probe.number(hex(page + layout.at("Page", "length")))
            if not 0 < length < 16384:
                raise RuntimeError(f"Invalid fixture page length: {length}")
            raw = await probe.call("azahar_read_memory", target=hex(buf), size=length * 4, format="bytes")
            tokens = struct.unpack(f"<{length}I", bytes.fromhex(raw["value"]))
            if 0x0531 not in tokens or not all(ord(c) in tokens for c in "spring"):
                raise RuntimeError("Generated fixture content is not resident on the first page")
            case = {"size": px, "applied_size": applied, "fallback_ppem": fallback_ppem, "page_tokens": length, "heartbeat": heartbeat["frame"], "failures": [], **await probe.clipping(budget)}
            if applied != px:
                case["failures"].append(f"reader size {applied}, expected {px}")
            if fallback_ppem != px:
                case["failures"].append(f"Armenian fallback remains {fallback_ppem}px, expected {px}px")
            if case["clipping_events"]:
                case["failures"].append(f"renderer clipped ink in {case['clipping_events']} book glyphs")
            case["screenshots"] = await probe.capture(f"size-{px}", launched["pid"])
            case["status"] = "failed" if case["failures"] else "passed"
            result["cases"].append(case)
            print(f"{result['orientation']} {rom.stem} {px}px: {case['status']} {case['failures']}", flush=True)
            current_size = px
        await probe.press("START")
        await probe.mode(1)
        _, text, _ = await probe.pointers()
        fm = await probe.number(hex(text + layout.at("Text", "fm")))
        browser = await probe.number(hex(fm + layout.at("FontManager", "faces") + 4 * 4))
        result["browser_ppem"] = await probe.ppem(browser)
        if result["browser_ppem"] != 12:
            raise RuntimeError("Browser face did not retain its native 12px size")
        fallback = await probe.number(hex(fm + layout.at("FontManager", "fallback_faces_")))
        result["browser_fallback_ppem"] = await probe.ppem(fallback)
        if result["browser_fallback_ppem"] != 12:
            raise RuntimeError("Browser fallback did not follow the browser's 12px face")
        # A warm reopen must retain the per-book override, use the resident
        # cache and leave its files unchanged, rather than silently repaginate.
        cache_dir = Path(launched["sdmc_dir"]) / BASE / "cache/epub"
        def cache_state():
            return {p.name: (p.stat().st_size, p.stat().st_mtime_ns) for p in cache_dir.glob("*.epc")}
        before = cache_state()
        if not before:
            raise RuntimeError("No persistent cache was produced by the fixture")
        result["saved_preferences"] = await probe.call("azahar_manage_sdmc", action="read", path=f"{BASE}/3dslibris.xml")
        result["cold_stop"] = await probe.call("azahar_stop", label="before-warm-reopen")
        if result["cold_stop"].get("status") != "stopped" or result["cold_stop"].get("returncode") != 0:
            raise RuntimeError("The cold emulator session did not stop cleanly")
        # Restart with a copy of this test's SDMC. START/A alone reuses the
        # resident Book object and cannot demonstrate a persistent cache load.
        warm_session = await probe.call("azahar_launch", rom_path=str(rom), source_sdmc=launched["sdmc_dir"], sdmc_mode="copy", app_logs={"app_log": f"{BASE}/3dslibris.log"}, log_archive_dir=str(output / "logs"))
        result["warm_session"] = warm_session
        await probe.mode(1)
        cache_dir = Path(warm_session["sdmc_dir"]) / BASE / "cache/epub"
        await probe.write(level, 5)
        await probe.write(categories, 0xffffffff)
        await probe.write(budget, 10000)
        await probe.call("azahar_read_logs", streams=["app_log"], max_lines=0)
        await probe.press("A")
        await probe.mode(0)
        _, text, book = await probe.pointers()
        applied = await probe.number(hex(text + layout.at("Text", "pixelsize")))
        resident = await probe.number(hex(book + layout.at("Book", "reflow_cache_reader_")))
        fm = await probe.number(hex(text + layout.at("Text", "fm")))
        fallback = await probe.number(hex(fm + layout.at("FontManager", "fallback_faces_")))
        warm = {"size": applied, "fallback_ppem": await probe.ppem(fallback), "resident": bool(resident), "cache_unchanged": before == cache_state(), **await probe.clipping(budget)}
        warm["status"] = "passed" if (applied == sizes[-1] and warm["fallback_ppem"] == applied and warm["resident"] and warm["cache_unchanged"] and not warm["clipping_events"]) else "failed"
        warm["screenshots"] = await probe.capture("warm-reopen", warm_session["pid"])
        result["warm_reopen"] = warm
        result["status"] = "failed" if any(c["failures"] for c in result["cases"]) or warm["status"] == "failed" else "passed"
    except Exception as exc:
        result["error"] = f"{type(exc).__name__}: {exc}"
    finally:
        try:
            record_stop(result, await probe.call("azahar_stop", label="font-172"))
        except Exception as exc:
            result["stop_error"] = str(exc)
            result["status"] = "error"
        (output / "calls.json").write_text(json.dumps(probe.calls, indent=2) + "\n")
        (output / "result.json").write_text(json.dumps(result, indent=2) + "\n")
    return result


async def main(args):
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    results = []
    parameters = StdioServerParameters(command=sys.executable, args=["-m", "azahar_mcp.server"], cwd=str(args.mcp_repo.resolve()))
    with (output / "mcp-server.log").open("w") as errors:
        async with stdio_client(parameters, errlog=errors) as (read, write):
            async with ClientSession(read, write) as session:
                initialized = await session.initialize()
                (output / "mcp-initialize.json").write_text(initialized.model_dump_json(by_alias=True, indent=2))
                for landscape in ([False, True] if args.orientation == "both" else [args.orientation == "landscape"]):
                    orientation = "landscape" if landscape else "portrait"
                    source = stage_fixture(output / f"source-{orientation}", landscape)
                    if args.reference_rom:
                        results.append(await run_one(session, args.reference_rom.resolve(), source, output / f"reference-{orientation}", [14, 18], landscape))
                    results.append(await run_one(session, args.rom.resolve(), source, output / f"current-{orientation}", args.sizes, landscape))
    current = [r for r in results if r["rom"] == str(args.rom.resolve())]
    references = [r for r in results if r not in current]
    # A control must fail for the actual clipping defect, never for transport,
    # launch, absent diagnostics, missing captures, or a UI timeout.
    control_ok = all(r["status"] == "failed" and any(c["clipping_events"] > 0 for c in r["cases"]) for r in references)
    passed = all(r["status"] == "passed" for r in current) and bool(current) and control_ok
    summary = {"status": "passed" if passed else "failed", "negative_control_detected_clipping": control_ok if references else None, "runs": results}
    (output / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
    print(f"Result: {summary['status']}; evidence: {output}")
    return 0 if passed else 1


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mcp-repo", type=Path, required=True)
    parser.add_argument("--rom", type=Path, required=True)
    parser.add_argument("--reference-rom", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--sizes", type=int, nargs="+", default=list(SIZES), choices=range(8, 21))
    parser.add_argument("--orientation", choices=["portrait", "landscape", "both"], default="both")
    sys.exit(asyncio.run(main(parser.parse_args())))

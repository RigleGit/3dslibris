"""Shared Azahar helpers for the algorithmic-change regression (debug 3dsx)."""
import asyncio
import hashlib
import json
import struct
import sys
import time
import unicodedata
from pathlib import Path

from elftools.elf.elffile import ELFFile

sys.path.insert(0, str(Path(__file__).resolve().parent))
from font_size_regression import Probe  # noqa: E402  (reuse the real MCP stdio client)

BASE = "3ds/3dslibris"
# Special tokens with one trailing payload word, mirroring text_search.cpp Payload().
TEXT_IMAGE, TEXT_HR, TEXT_HR_BOUNDS = 6, 28, 0x11000C
PAYLOAD = {TEXT_HR_BOUNDS: 2, 6: 1, 0x110008: 1, 0x11000A: 1, 0x11000B: 1, 0x110007: 1,
           17: 1, 0x110005: 1, 31: 1}
SPACES = {10, 13, 9, 0x110002, 0x110003, 0x110004}


class Types:
    """Member offsets and sizes from the matching ARM ELF's DWARF."""

    WANTED = {"App", "Text", "Book", "ReaderRuntimeState", "Page", "ChapterEntry",
              "ReaderController", "ReaderSearchSession"}

    def __init__(self, elf_path):
        self.fields, self.sizes = {}, {}
        wanted = set(self.WANTED)
        with Path(elf_path).open("rb") as stream:
            elf = ELFFile(stream)
            dwarf = elf.get_dwarf_info(relocate_dwarf_sections=False)
            for cu in dwarf.iter_CUs():
                for die in cu.iter_DIEs():
                    name = die.attributes.get("DW_AT_name")
                    name = name.value.decode() if name and isinstance(name.value, bytes) else None
                    if name not in wanted or "DW_AT_byte_size" not in die.attributes:
                        continue
                    members = {}
                    for child in die.iter_children():
                        a = child.attributes
                        if child.tag == "DW_TAG_member" and "DW_AT_name" in a and "DW_AT_data_member_location" in a:
                            members[a["DW_AT_name"].value.decode()] = a["DW_AT_data_member_location"].value
                    self.fields[name] = members
                    self.sizes[name] = die.attributes["DW_AT_byte_size"].value
                    wanted.discard(name)
                if not wanted:
                    break
        if wanted:
            raise RuntimeError(f"ELF lacks DWARF types: {sorted(wanted)}")

    def at(self, type_name, field):
        return self.fields[type_name][field]


class Session:
    """Memory-level access to the running application, no state is patched."""

    def __init__(self, probe, types):
        self.p, self.t = probe, types

    async def u32(self, address):
        return await self.p.number(hex(address))

    async def u16(self, address):
        return await self.p.number(hex(address), "u16")

    async def u8(self, address):
        return await self.p.number(hex(address), "u8")

    async def raw(self, address, size):
        if size == 0:
            return b""
        r = await self.p.call("azahar_read_memory", target=hex(address), size=size, format="bytes")
        return bytes.fromhex(r["value"])

    async def string(self, address):
        # libstdc++ new ABI: {pointer, length, union{local[16], capacity}}.
        ptr, length = await self.u32(address), await self.u32(address + 4)
        return (await self.raw(ptr, min(length, 512))).decode("utf-8", "replace")

    async def app(self):
        return await self.p.number("App::s_instance_")

    async def book(self):
        _, _, book = await self.p.pointers()
        return book

    async def pages(self, book):
        base = await self.u32(book + self.t.at("Book", "pages"))
        end = await self.u32(book + self.t.at("Book", "pages") + 4)
        return base, (end - base) // 4

    async def page_tokens(self, page_ptr):
        length = await self.u32(page_ptr + self.t.at("Page", "length"))
        buf = await self.u32(page_ptr + self.t.at("Page", "buf"))
        if not 0 <= length < 16384:
            raise RuntimeError(f"bad page length {length}")
        data = await self.raw(buf, length * 4)
        return list(struct.unpack(f"<{length}I", data))

    async def page_at(self, book, index):
        base, count = await self.pages(book)
        if not 0 <= index < count:
            raise RuntimeError(f"page {index} outside {count}")
        return await self.page_tokens(await self.u32(base + 4 * index))

    async def position(self, book):
        return await self.u32(book + self.t.at("Book", "position"))

    async def chapters(self, book):
        base = await self.u32(book + self.t.at("Book", "chapters"))
        end = await self.u32(book + self.t.at("Book", "chapters") + 4)
        size = self.t.sizes["ChapterEntry"]
        out = []
        for i in range((end - base) // size if size else 0):
            entry = base + i * size
            out.append({"page": await self.u16(entry + self.t.at("ChapterEntry", "page")),
                        "level": await self.u8(entry + self.t.at("ChapterEntry", "level")),
                        "title": await self.string(entry + self.t.at("ChapterEntry", "title"))})
        return out

    async def search_session(self):
        app = await self.app()
        controller = await self.u32(app + self.t.at("App", "reader_controller_"))
        return await self.u32(controller + self.t.at("ReaderController", "search_"))

    async def search_state(self):
        s = await self.search_session()
        if not s:
            return None
        at = lambda f: s + self.t.at("ReaderSearchSession", f)  # noqa: E731
        start_page = at("match")
        return {
            "has_match": bool(await self.u8(at("has_match"))),
            "whole_words": bool(await self.u8(at("whole_words"))),
            "editor": bool(await self.u8(at("editor"))),
            "selected": await self.u32(at("selected")),
            "start_page": await self.u32(start_page), "start_token": await self.u32(start_page + 4),
            "end_page": await self.u32(start_page + 8), "end_token": await self.u32(start_page + 12),
            "query": await self.string(at("query")),
            "message": await self.string(at("message")),
        }


def decode_page(tokens):
    """Searchable characters (cp or None for a barrier) with their token index."""
    out, i = [], 0
    while i < len(tokens):
        cp, start = tokens[i], i
        i += 1 + PAYLOAD.get(cp, 0)
        if cp == 0x00AD:
            continue
        if cp in (TEXT_IMAGE, TEXT_HR, TEXT_HR_BOUNDS):
            out.append((None, start))
        elif cp in SPACES:
            out.append((32, start))
        elif cp in PAYLOAD or cp < 32 or cp > 0x10FFFF:
            continue
        else:
            out.append((cp, start))
    return out


def fold(text):
    return unicodedata.normalize("NFD", text.casefold())


def is_word(ch):
    return unicodedata.category(ch)[0] in "LMN" or unicodedata.category(ch) == "Pc"


def matches_at(tokens, token_index, query, whole=False):
    """Independent check that a search match starts at token_index of this page."""
    chars = decode_page(tokens)
    q = " ".join(fold(query).split())
    stream = []
    for cp, idx in chars:
        if cp is None:
            stream.append((None, idx))
            continue
        for c in fold(chr(cp)):
            stream.append((c, idx))
    # collapse whitespace like the engine
    collapsed = []
    for c, idx in stream:
        if c is not None and c.isspace():
            c = " "
            if collapsed and collapsed[-1][0] == " ":
                continue
        collapsed.append((c, idx))
    text = "".join(c if c is not None else "\0" for c, _ in collapsed)
    for pos in range(len(collapsed)):
        if collapsed[pos][1] != token_index or not text.startswith(q, pos):
            continue
        after = text[pos + len(q)] if pos + len(q) < len(text) else ""
        if after and unicodedata.category(after) in ("Mn", "Mc", "Me"):
            continue
        if whole:
            before = text[pos - 1] if pos else ""
            if (before and is_word(before)) or (after and is_word(after)):
                continue
        return True
    return False


def page_hash(tokens):
    return hashlib.sha256(struct.pack(f"<{len(tokens)}I", *tokens)).hexdigest()[:16]

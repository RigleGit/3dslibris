"""Owned fixtures for the algorithmic-change Azahar regression.

Every expected value here is produced by this file, never by the application.
"""
import io
import pathlib
import sys
import zipfile

ROOT = pathlib.Path(__file__).resolve().parents[2]
BASE = "3ds/3dslibris"

CSS_A = """
p.center { text-align: center; }
p.right { text-align: right; }
p.note { margin-left: 2em; font-size: 90%; }
em.s { font-style: italic; }
h2 { text-align: center; }
"""
CSS_B = """
p.tight { margin-top: 0; margin-bottom: 0; }
span.big { font-size: 120%; }
"""
FILLER = ("The quick brown fox walks along the quiet river while evening light "
          "settles over the distant hills and the old stone bridge. ")
# Needles with the book location they must be found in (chapter index, 1-based).
NEEDLES = {
    "ascii": ("zebraquartz", 1),
    "cyrillic": ("привет", 2),
    "greek": ("αλφάβητο", 2),
    "accent": ("café", 3),
    "arabic": ("كتاب", 3),
    "image": ("lanternfish", 4),
}


def _para(n, tail=""):
    return "<p>" + FILLER * n + tail + "</p>"


def chapter_body(index):
    heading = "<h2>Chapter %d</h2>" % index
    if index == 1:
        body = (_para(8) + _para(5, " The word zebraquartz appears once, with “curly quotes” — and a dash. ") +
                '<p class="center">Centered line ‘one two three’.</p>' +
                '<p class="right">Right aligned: one two three four.</p>' + _para(8))
    elif index == 2:
        body = (_para(6) + "<p>Привет мир, это проверка кириллицы и привет снова.</p>" +
                "<p>Η αλφάβητο ελληνική γραμματοσειρά δοκιμή.</p>" + _para(10))
    elif index == 3:
        body = (_para(5) + "<p>Un café con leche; CAFÉ again; cafeteria and cafe differ.</p>" +
                '<p dir="rtl">هذا كتاب جميل ومفيد للقراءة كل يوم</p>' +
                '<p dir="rtl">السلام عليكم ورحمة الله وبركاته وهذا كتاب آخر</p>' + _para(9))
    else:
        body = (_para(4) + '<p><img src="cover.png" alt="fixture"/></p>' +
                "<p>A rare lanternfish ends the fourth chapter.</p>" + _para(9))
    return heading + body


def png_bytes():
    from PIL import Image
    image = Image.new("RGB", (64, 48), (200, 40, 40))
    for x in range(64):
        image.putpixel((x, x % 48), (20, 20, 220))
    buffer = io.BytesIO()
    image.save(buffer, "PNG")
    return buffer.getvalue()


def build_epub(path):
    with zipfile.ZipFile(path, "w") as z:
        z.writestr("mimetype", "application/epub+zip", compress_type=zipfile.ZIP_STORED)
        z.writestr("META-INF/container.xml",
                   '<container xmlns="urn:oasis:names:tc:opendocument:xmlns:container" version="1.0">'
                   '<rootfiles><rootfile full-path="OEBPS/content.opf" '
                   'media-type="application/oebps-package+xml"/></rootfiles></container>')
        items = "".join('<item id="c%d" href="ch%d.xhtml" media-type="application/xhtml+xml"/>' % (i, i)
                        for i in range(1, 5))
        refs = "".join('<itemref idref="c%d"/>' % i for i in range(1, 5))
        z.writestr("OEBPS/content.opf",
                   '<package xmlns="http://www.idpf.org/2007/opf" version="2.0" unique-identifier="id">'
                   '<metadata xmlns:dc="http://purl.org/dc/elements/1.1/"><dc:identifier id="id">chg-1</dc:identifier>'
                   '<dc:title>Changes Fixture</dc:title><dc:language>en</dc:language></metadata><manifest>'
                   '<item id="ncx" href="toc.ncx" media-type="application/x-dtbncx+xml"/>'
                   '<item id="a" href="a.css" media-type="text/css"/><item id="b" href="b.css" media-type="text/css"/>'
                   '<item id="img" href="cover.png" media-type="image/png"/>' + items +
                   '</manifest><spine toc="ncx">' + refs + '</spine></package>')
        points = "".join('<navPoint id="n%d" playOrder="%d"><navLabel><text>Chapter %d</text></navLabel>'
                         '<content src="ch%d.xhtml"/></navPoint>' % (i, i, i, i) for i in range(1, 5))
        z.writestr("OEBPS/toc.ncx",
                   '<ncx xmlns="http://www.daisy.org/z3986/2005/ncx/" version="2005-1"><head/>'
                   '<docTitle><text>Changes Fixture</text></docTitle><navMap>' + points + '</navMap></ncx>')
        z.writestr("OEBPS/a.css", CSS_A)
        z.writestr("OEBPS/b.css", CSS_B)
        z.writestr("OEBPS/cover.png", png_bytes())
        for i in range(1, 5):
            # ch1+ch2 share a sheet list (reuse path); ch3 reorders it (miss path).
            links = ('<link rel="stylesheet" href="b.css"/><link rel="stylesheet" href="a.css"/>'
                     if i == 3 else
                     '<link rel="stylesheet" href="a.css"/><link rel="stylesheet" href="b.css"/>')
            z.writestr("OEBPS/ch%d.xhtml" % i,
                       '<html xmlns="http://www.w3.org/1999/xhtml"><head><title>Chapter %d</title>%s</head>'
                       '<body>%s</body></html>' % (i, links, chapter_body(i)))


def _words(count, seed):
    """Deterministic pseudo-random words: poorly compressible, so the page cache
    spans several 64 KB read windows."""
    state, out = seed, []
    for _ in range(count):
        state = (state * 1103515245 + 12345) & 0x7FFFFFFF
        length = 3 + (state >> 8) % 7
        word = []
        for _ in range(length):
            state = (state * 1103515245 + 12345) & 0x7FFFFFFF
            word.append(chr(ord("a") + (state >> 16) % 26))
        out.append("".join(word))
    return " ".join(out)


def build_long_epub(path, paragraphs=900):
    """Many pages with monotone paragraph ids, to cross several cache blocks."""
    body = "".join("<p>P%04d %s</p>" % (i, _words(55, i + 1)) for i in range(paragraphs))
    with zipfile.ZipFile(path, "w") as z:
        z.writestr("mimetype", "application/epub+zip", compress_type=zipfile.ZIP_STORED)
        z.writestr("META-INF/container.xml",
                   '<container xmlns="urn:oasis:names:tc:opendocument:xmlns:container" version="1.0">'
                   '<rootfiles><rootfile full-path="c.opf" media-type="application/oebps-package+xml"/>'
                   '</rootfiles></container>')
        z.writestr("c.opf",
                   '<package xmlns="http://www.idpf.org/2007/opf" version="2.0" unique-identifier="id">'
                   '<metadata xmlns:dc="http://purl.org/dc/elements/1.1/"><dc:identifier id="id">long-1</dc:identifier>'
                   '<dc:title>Long Fixture</dc:title><dc:language>en</dc:language></metadata><manifest>'
                   '<item id="b" href="b.xhtml" media-type="application/xhtml+xml"/></manifest>'
                   '<spine><itemref idref="b"/></spine></package>')
        z.writestr("b.xhtml", '<html xmlns="http://www.w3.org/1999/xhtml"><head><title>L</title></head>'
                   '<body>%s</body></html>' % body)


def build_mobi(path):
    import subprocess, tempfile
    with tempfile.TemporaryDirectory() as tmp:
        out = pathlib.Path(tmp) / "m"
        subprocess.check_call([sys.executable, str(ROOT / "tests/fixtures/generate_mobi_reading.py"), str(out)])
        pathlib.Path(path).write_bytes((out / "palmdoc.mobi").read_bytes())


def stage(destination, landscape=False, epub=True, mobi=True, long_book=False):
    """Generated books + bundled fonts only; never the user's SDMC."""
    import shutil
    app = pathlib.Path(destination) / BASE
    (app / "book").mkdir(parents=True)
    shutil.copytree(ROOT / "sdmc" / BASE / "font", app / "font")
    (app / "3dslibris.xml").write_text(
        '<?xml version="1.0"?><dslibris><screen colorMode="0" flip="0" '
        'landscape="%d"/><font size="12"/><paragraph indent="0" spacing="0"/>'
        '<books path="sdmc:/3ds/3dslibris/book" reopen="0"/></dslibris>' % int(landscape),
        encoding="utf-8")
    if epub:
        build_epub(app / "book" / "a-changes.epub")
    if mobi:
        build_mobi(app / "book" / "b-changes.mobi")
    if long_book:
        build_long_epub(app / "book" / "c-long.epub")
    return pathlib.Path(destination)

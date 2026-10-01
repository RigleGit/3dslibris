"""Generate owned, multi-record MOBI6 books for the real Book reading path.

Header offsets cross-checked against calibre's independent BookHeader reader:
https://github.com/kovidgoyal/calibre/blob/master/src/calibre/ebooks/mobi/reader/headers.py
No production parser or serializer produces the expected text or TOC.
"""
import pathlib
import struct
import sys


def palmdoc_literals(data):
    # PalmDOC literal runs can encode any byte (including UTF-8) in blocks of 8.
    return b''.join(bytes([len(data[i:i + 8])]) + data[i:i + 8]
                    for i in range(0, len(data), 8))


def build_book(compression):
    labels = ('Chapter One', 'Chapter Two', 'Chapter Three', 'Chapter Four')
    front = '<html><body><nav>' + ''.join(
        '<a filepos="0000000000">%s</a>' % label for label in labels) + '</nav>'
    markup = front
    offsets = []
    for chapter, label in enumerate(labels):
        offsets.append(len(markup.encode('utf-8')))
        words = ' '.join('TOKEN%04d' % i
                         for i in range(chapter * 1000 + 1, chapter * 1000 + 1001))
        markup += '<h1>%s</h1><p>%s</p>' % (label, words)
    markup += '<p>café &amp; tea MOBIEND</p><script>HIDDEN-MOBI</script></body></html>'
    for offset in offsets:
        markup = markup.replace('0000000000', '%010d' % offset, 1)
    text = markup.encode('utf-8')
    records = [text[i:i + 4096] for i in range(0, len(text), 4096)]
    title = 'Fixture MOBI café'.encode('utf-8')
    record0 = bytearray(248)
    struct.pack_into('>HHIHHHH', record0, 0,
                     compression, 0, len(text), len(records), 4096, 0, 0)
    record0[16:20] = b'MOBI'
    struct.pack_into('>5I', record0, 20, 232, 2, 65001, 1234, 6)
    struct.pack_into('>II', record0, 84, len(record0), len(title))
    struct.pack_into('>I', record0, 104, 6)
    struct.pack_into('>I', record0, 108, 0xffffffff)  # No image records.
    struct.pack_into('>I', record0, 244, 0xffffffff)  # Inline filepos TOC.
    record0.extend(title + b'\0')
    if compression == 2:
        records = [palmdoc_literals(record) for record in records]
    records.insert(0, bytes(record0))
    header = bytearray(78)
    header[:12] = b'MOBI fixture'
    header[60:68] = b'BOOKMOBI'
    struct.pack_into('>H', header, 76, len(records))
    cursor = 78 + 8 * len(records) + 2
    table = bytearray()
    for uid, record in enumerate(records, 1):
        table.extend(struct.pack('>IB', cursor, 0) + uid.to_bytes(3, 'big'))
        cursor += len(record)
    return bytes(header + table + b'\0\0') + b''.join(records)


folder = pathlib.Path(sys.argv[1])
folder.mkdir(parents=True, exist_ok=True)
for compression, name in ((1, 'raw.mobi'), (2, 'palmdoc.mobi')):
    data = build_book(compression)
    (folder / name).write_bytes(data)
    # Truncate inside the offset table, with the requested file otherwise present.
    (folder / ('truncated-' + name)).write_bytes(data[:85])
(folder / 'cache').mkdir()

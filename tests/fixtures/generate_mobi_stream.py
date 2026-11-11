"""Generate MOBI books that exercise the Stream tier against the Safe tier.

The books differ in the places the two tiers decode differently: encodings,
record boundaries inside UTF-8 / UTF-16 sequences, trailing entries, hard
wrapped lines (the wrap fix), mojibake, carriage returns, inline images and an
NCX index. No production code produces the expected output: the test compares
the two tiers page for page.
"""
import pathlib
import random
import struct
import sys

WORDS = ('the quick brown fox jumps over the lazy dog and runs far away from '
         'home while a calm river flows past old stone bridges near towns '
         'where people speak softly about weather harvest travel music '
         'letters memory silver winter morning lantern garden window').split()


def palmdoc_literals(data):
    return b''.join(bytes([len(data[i:i + 8])]) + data[i:i + 8]
                    for i in range(0, len(data), 8))


def palmdoc_lz(data):
    """A small but real PalmDOC compressor (literals, spaces, back references)."""
    out = bytearray()
    i = 0
    while i < len(data):
        best_len, best_dist = 0, 0
        for dist in range(1, min(i, 2047) + 1):
            length = 0
            while (length < 10 and i + length < len(data) and
                   data[i + length - dist] == data[i + length]):
                length += 1
            if length > best_len:
                best_len, best_dist = length, dist
        if best_len >= 3:
            pair = 0x8000 | (best_dist << 3) | (best_len - 3)
            out += struct.pack('>H', pair)
            i += best_len
            continue
        c = data[i]
        if c == 0x20 and i + 1 < len(data) and 0x40 <= data[i + 1] <= 0x7f:
            out.append(data[i + 1] ^ 0x80)
            i += 2
        elif c == 0 or 0x09 <= c <= 0x7f:
            out.append(c)
            i += 1
        else:
            run = bytearray([c])
            i += 1
            while (i < len(data) and len(run) < 8 and
                   not (data[i] == 0 or 0x09 <= data[i] <= 0x7f)):
                run.append(data[i])
                i += 1
            out.append(len(run))
            out += run
    return bytes(out)


def vwi(n):
    parts = []
    while True:
        parts.append(n & 0x7f)
        n >>= 7
        if not n:
            break
    parts.reverse()
    parts[-1] |= 0x80
    return bytes(parts)


def sentence(rng, n):
    return ' '.join(rng.choice(WORDS) for _ in range(n))


def markup(rng, chapters=6, paragraphs=24, *, wrap=False, entities=True,
           images=0, mojibake=False, carriage=False, lists=True, utf8=True):
    out = ['<html><head><style>p{margin:0}</style><script>var x=1;</script></head><body>']
    heads = []
    image_no = 0
    for ch in range(chapters):
        heads.append(('Chapter %d' % (ch + 1), None))
        out.append('<h1>Chapter %d</h1>' % (ch + 1))
        for p in range(paragraphs):
            kind = rng.randrange(12)
            if wrap and kind < 7:
                lines = [sentence(rng, rng.randrange(4, 9)) for _ in range(rng.randrange(2, 5))]
                body = '<br/>'.join(lines) + ('.' if rng.randrange(3) else '')
            else:
                body = sentence(rng, rng.randrange(5, 40)) + '.'
            if entities and rng.randrange(4) == 0:
                body += ' &amp; caf&eacute; &#233;&#x41; &lt;ok&gt; &nbsp;x &bogus; & alone'
            if utf8 and rng.randrange(5) == 0:
                body += ' café “quoted” € \U0001f600 中文'
            if mojibake and rng.randrange(3) == 0:
                body += ' Ã© Ã±x Ã¡rbol'
            if carriage and rng.randrange(4) == 0:
                body += '&#13;&#xD;next'
            if images and rng.randrange(6) == 0:
                image_no = image_no % images + 1
                body += ' <img recindex="%d"/> ' % image_no
            if lists and kind == 11:
                out.append('<ul><li>%s</li><li>%s</li></ul>' % (sentence(rng, 5), sentence(rng, 6)))
            elif kind == 10:
                out.append('<div>%s</div>' % body)
            else:
                out.append('<p>%s</p>' % body)
    out.append('<script>HIDDEN</script></body></html>')
    return ''.join(out), heads


def build(text, *, compression=2, encoding=65001, record_size=4096,
          trailing=0, ncx=None, text_len=None, title='Stream fixture'):
    records = [text[i:i + record_size] for i in range(0, len(text), record_size)]
    if compression == 2:
        records = [palmdoc_lz(r) if i % 2 else palmdoc_literals(r)
                   for i, r in enumerate(records)]
    flags = 0
    if trailing:
        flags = 3  # one trailing entry and the multibyte overlap
        records = [r + b'\x00' + b'abc\x84' for r in records]
    text_records = len(records)
    extra = []
    ncx_index = 0xffffffff
    if ncx:
        ncx_index = 1 + text_records
        tagx_rules = [(1, 1, 0x01, 0), (3, 1, 0x02, 0), (4, 1, 0x04, 0), (0, 0, 0, 1)]
        tagx = b'TAGX' + struct.pack('>II', 12 + 4 * len(tagx_rules), 1) + b''.join(
            bytes(r) for r in tagx_rules)
        main = bytearray(184)
        main[0:4] = b'INDX'
        struct.pack_into('>I', main, 4, 184)
        struct.pack_into('>I', main, 24, 1)      # one INDX data record
        struct.pack_into('>I', main, 52, 1)      # one CNCX record
        struct.pack_into('>I', main, 180, 184)   # TAGX offset
        main += tagx
        cncx = bytearray()
        entries = []
        for name, pos, level in ncx:
            label = name.encode('utf-8')
            key = len(cncx)
            cncx += vwi(len(label)) + label
            body = bytes([len(str(len(entries)))]) + str(len(entries)).encode()
            body += bytes([0x07]) + vwi(pos) + vwi(key) + vwi(level)
            entries.append(body)
        data = bytearray(184)
        data[0:4] = b'INDX'
        struct.pack_into('>I', data, 4, 184)
        offsets, blob = [], b''
        for e in entries:
            offsets.append(184 + len(blob))
            blob += e
        idxt = len(data) + len(blob)
        struct.pack_into('>I', data, 20, idxt)
        struct.pack_into('>I', data, 24, len(entries))
        data += blob + b'IDXT' + b''.join(struct.pack('>H', o) for o in offsets)
        extra = [bytes(main), bytes(data), bytes(cncx) + b'\x00']
    first_non_book = 1 + text_records if extra else 0xffffffff
    title_bytes = title.encode('utf-8')
    record0 = bytearray(248)
    struct.pack_into('>HHIHHHH', record0, 0, compression, 0,
                     len(text) if text_len is None else text_len,
                     text_records, record_size, 0, 0)
    record0[16:20] = b'MOBI'
    struct.pack_into('>5I', record0, 20, 232, 2, encoding, 1234, 6)
    struct.pack_into('>II', record0, 84, len(record0), len(title_bytes))
    struct.pack_into('>I', record0, 104, 6)
    struct.pack_into('>I', record0, 108, first_non_book)
    struct.pack_into('>I', record0, 240, flags)
    struct.pack_into('>I', record0, 244, ncx_index)
    record0.extend(title_bytes + b'\0')
    all_records = [bytes(record0)] + records + extra
    header = bytearray(78)
    header[:12] = b'MOBI fixture'
    header[60:68] = b'BOOKMOBI'
    struct.pack_into('>H', header, 76, len(all_records))
    cursor = 78 + 8 * len(all_records) + 2
    table = bytearray()
    for uid, record in enumerate(all_records, 1):
        table.extend(struct.pack('>IB', cursor, 0) + uid.to_bytes(3, 'big'))
        cursor += len(record)
    return bytes(header + table + b'\0\0') + b''.join(all_records)


def filepos_for(text, heads):
    """Byte offsets of the chapter <h1> tags in the encoded markup."""
    out = []
    at = 0
    for name, _ in heads:
        at = text.index(('<h1>%s</h1>' % name).encode('utf-8'), at)
        out.append(at)
        at += 1
    return out


def main():
    folder = pathlib.Path(sys.argv[1])
    folder.mkdir(parents=True, exist_ok=True)
    rng = random.Random(7)

    def write(name, data):
        (folder / name).write_bytes(data)

    # UTF-8, wrapped lines, mojibake, entities, carriage returns, images.
    html, heads = markup(rng, wrap=True, mojibake=True, carriage=True, images=3)
    text = html.encode('utf-8')
    write('tokens-palmdoc.mobi', build(text, compression=2))
    write('tokens-raw.mobi', build(text, compression=1))
    write('trailing-raw.mobi', build(text, compression=1, trailing=True))
    write('trailing-palmdoc.mobi', build(text, compression=2, trailing=True))
    # Header text length shorter than the data (cut inside a character).
    write('short-textlen.mobi', build(text, compression=1, text_len=len(text) - 777))
    # NCX index pointing at the chapter headings.
    pos = filepos_for(text, heads)
    write('ncx-palmdoc.mobi', build(
        text, compression=2,
        ncx=[(name, p, 0) for (name, _), p in zip(heads, pos)]))
    # A wrapped book without images or mojibake: the plain wrap fix.
    html2, _ = markup(rng, wrap=True, images=0, mojibake=False, entities=False)
    write('plain-wrap.mobi', build(html2.encode('utf-8'), compression=2))
    # cp1252 bytes (legacy decode) and UTF-8 labelled as cp1252 (guess).
    html3, _ = markup(rng, wrap=True, utf8=False, entities=False)
    legacy = html3.replace('<h1>', '<h1>café “').encode('cp1252', 'replace')
    write('cp1252.mobi', build(legacy, compression=2, encoding=1252))
    html4, _ = markup(rng, wrap=True, mojibake=True)
    write('utf8-as-cp1252.mobi', build(html4.encode('utf-8'), compression=2, encoding=1252))
    write('unknown-encoding.mobi', build(html4.encode('utf-8'), compression=1, encoding=0))
    # UTF-16 with surrogate pairs crossing record boundaries.
    html5, _ = markup(rng, chapters=3, paragraphs=30, wrap=True)
    html5 = html5.replace('<p>', '<p>\U0001f600 ')
    write('utf16-raw.mobi', build(b'\xff\xfe' + html5.encode('utf-16-le'),
                                  compression=1, encoding=1200))
    # Larger than several pagination windows.
    html6, heads6 = markup(rng, chapters=40, paragraphs=80, wrap=True,
                           mojibake=True, images=4)
    big = html6.encode('utf-8')
    write('large-palmdoc.mobi', build(big, compression=2, ncx=[
        (name, p, 0) for (name, _), p in zip(heads6, filepos_for(big, heads6))]))
    # Many tiny records: every boundary lands inside a sequence somewhere.
    write('tiny-records.mobi', build(text, compression=1, record_size=131))


if __name__ == '__main__':
    main()

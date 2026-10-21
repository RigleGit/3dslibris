"""Owned EPUB/MOBI/FB2 fixtures for image selection, detail and alpha tests.

The same original resources appear repeatedly and inside links. Files also
serve as manual ARM/console fixtures; no downloaded books or image tools.
"""
import base64
from pathlib import Path
import struct
import sys
import zipfile
import zlib


def chunk(tag, data):
    return struct.pack('>I', len(data)) + tag + data + struct.pack('>I', zlib.crc32(tag + data) & 0xffffffff)


def png(width, height, rows):
    return b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, 8, 6, 0, 0, 0)) + chunk(b'IDAT', zlib.compress(rows)) + chunk(b'IEND', b'')


def mobi(markup, images):
    text = markup.encode()
    records = [text[i:i+4096] for i in range(0, len(text), 4096)]
    header0 = bytearray(248)
    struct.pack_into('>HHIHHHH', header0, 0, 1, 0, len(text), len(records), 4096, 0, 0)
    header0[16:20] = b'MOBI'
    struct.pack_into('>5I', header0, 20, 232, 2, 65001, 1234, 6)
    title = b'Image viewer fixture'
    struct.pack_into('>II', header0, 84, len(header0), len(title))
    struct.pack_into('>I', header0, 108, len(records)+1)
    struct.pack_into('>I', header0, 244, 0xffffffff)
    header0.extend(title+b'\0')
    records = [bytes(header0)] + records + list(images.values())
    header = bytearray(78)
    header[:12] = b'Image viewer'
    header[60:68] = b'BOOKMOBI'
    struct.pack_into('>H', header, 76, len(records))
    table = bytearray()
    offset = 78 + 8*len(records) + 2
    for uid, record in enumerate(records, 1):
        table += struct.pack('>IB', offset, 0) + uid.to_bytes(3, 'big')
        offset += len(record)
    return bytes(header+table+b'\0\0') + b''.join(records)


def generate(folder):
    folder = Path(folder)
    folder.mkdir(parents=True, exist_ok=True)
    tiny = png(16, 8, (b'\0'+bytes((255,0,0,255))*16)*8)
    # Alternating fine lines and a transparent hole reveal thumbnail reuse,
    # smoothing on pan release, aspect distortion and incorrect alpha handling.
    rows = []
    for y in range(1024):
        row = bytearray([0])
        for x in range(1024):
            value = 0 if x % 8 == 0 or y % 8 == 0 else 255
            alpha = 0 if 384 <= x < 640 and 384 <= y < 640 else 255
            row.extend((value,value,value,alpha))
        rows.append(row)
    fine = png(1024, 1024, b''.join(rows))
    frame = b'\x2c\x00\x00\x00\x00\x01\x00\x01\x00\x00\x02\x02'
    gif = b'GIF89a\x01\x00\x01\x00\x80\x00\x00' + b'\xff\x00\x00\x00\x00\xff' + frame+b'\x44\x01\x00'+frame+b'\x4c\x01\x00\x3b'
    images = {'tiny.png': tiny, 'fine.png': fine, 'first.gif': gif}
    # A tiny compressed resource that requires >16 MiB to decode fully.
    (folder/'oversized-image.base64').write_bytes(base64.b64encode(
        png(3000,3000,bytes(3000*(1+3000*4)))))
    filler = '<p>' + 'Continuity across pages. ' * 200 + '</p>'
    body = ('<h1>Image viewer</h1><p>Hold Y, then A to open an image.</p>'
            '<p><a href="#end">linked text <img src="tiny.png"/> after image</a></p>'
            '<p>Repeated: <img src="tiny.png"/></p>' + filler +
            '<h2 id="end">Original detail</h2><p>Zoom, pan, zoom out. The central hole is transparent.</p>'
            '<p><img src="fine.png"/></p><h2>GIF: first frame must be red</h2><p><img src="first.gif"/></p>')
    document = '<html xmlns="http://www.w3.org/1999/xhtml"><head><title>Image viewer</title></head><body>'+body+'</body></html>'
    container = '<container xmlns="urn:oasis:names:tc:opendocument:xmlns:container" version="1.0"><rootfiles><rootfile full-path="OEBPS/content.opf" media-type="application/oebps-package+xml"/></rootfiles></container>'
    package = ('<package xmlns="http://www.idpf.org/2007/opf" version="3.0" unique-identifier="id">'
        '<metadata xmlns:dc="http://purl.org/dc/elements/1.1/"><dc:identifier id="id">image-viewer</dc:identifier><dc:title>Image viewer fixture</dc:title><dc:language>en</dc:language></metadata>'
        '<manifest><item id="book" href="book.xhtml" media-type="application/xhtml+xml"/>'
        '<item id="nav" href="nav.xhtml" media-type="application/xhtml+xml" properties="nav"/>' + ''.join(
            '<item id="image%d" href="%s" media-type="image/%s"/>' % (i,name,'gif' if name.endswith('.gif') else 'png')
            for i,name in enumerate(images)) + '</manifest><spine><itemref idref="book"/></spine></package>')
    nav = '<html xmlns="http://www.w3.org/1999/xhtml" xmlns:epub="http://www.idpf.org/2007/ops"><body><nav epub:type="toc"><ol><li><a href="book.xhtml">Images</a></li></ol></nav></body></html>'
    with zipfile.ZipFile(folder/'images.epub','w',compression=zipfile.ZIP_DEFLATED) as archive:
        archive.writestr('mimetype','application/epub+zip',compress_type=zipfile.ZIP_STORED)
        archive.writestr('META-INF/container.xml',container)
        archive.writestr('OEBPS/content.opf',package)
        archive.writestr('OEBPS/nav.xhtml',nav)
        archive.writestr('OEBPS/book.xhtml',document)
        for name, data in images.items(): archive.writestr('OEBPS/'+name,data)
        assert archive.testzip() is None
    # One resource, three occurrences: widths belong to each img element.
    width_image = png(240, 60, (b'\0' + bytes((255, 0, 0, 255)) * 240) * 60)
    for variant, widths in [('authored-first', (80, None, 120)),
                            ('natural-first', (None, 80, None))]:
        body = ''.join('<p style="margin:0"><img src="tiny.png"%s/></p>' %
                       ('' if width is None else ' width="%d"' % width)
                       for width in widths)
        with zipfile.ZipFile(folder / ('widths-' + variant + '.epub'), 'w',
                             compression=zipfile.ZIP_DEFLATED) as archive:
            archive.writestr('mimetype', 'application/epub+zip',
                             compress_type=zipfile.ZIP_STORED)
            archive.writestr('META-INF/container.xml', container)
            archive.writestr('OEBPS/content.opf', package)
            archive.writestr('OEBPS/nav.xhtml', nav)
            archive.writestr('OEBPS/book.xhtml',
                             '<html xmlns="http://www.w3.org/1999/xhtml"><body>' +
                             body + '</body></html>')
            for name, data in images.items():
                archive.writestr('OEBPS/' + name,
                                 width_image if name == 'tiny.png' else data)
    mobile = document
    for index,name in enumerate(images,1): mobile = mobile.replace('src="'+name+'"','recindex="%d"' % index)
    (folder/'images.mobi').write_bytes(mobi(mobile,images))
    fb2body = ('<section><title><p>Image viewer</p></title><p>Hold Y to select. Original detail and alpha.</p>'
        '<p><a l:href="#end">linked text <image l:href="#tiny.png"/> after image</a></p>'
        '<p>Repeated: <image l:href="#tiny.png"/></p>' + filler +
        '<section id="end"><title><p>Original detail</p></title><image l:href="#fine.png"/>'
        '<p>GIF first frame is red.</p><image l:href="#first.gif"/></section></section>')
    (folder/'images.fb2').write_text('<FictionBook xmlns="http://www.gribuser.ru/xml/fictionbook/2.0" xmlns:l="http://www.w3.org/1999/xlink"><description><title-info><genre>reference</genre><book-title>Image viewer fixture</book-title><lang>en</lang></title-info></description><body>'+fb2body+'</body>'+''.join(
        '<binary id="%s" content-type="image/%s">%s</binary>' % (name,'gif' if name.endswith('.gif') else 'png',base64.b64encode(data).decode()) for name,data in images.items())+'</FictionBook>')


if __name__ == '__main__':
    generate(sys.argv[1])

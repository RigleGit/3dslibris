"""Owned EPUBs distinguishing interruption, recoverable XML and fatal ZIP CRC.

Validate the intact archives independently with Python's zipfile and Expat.
The CRC variant changes only stored XHTML payload bytes; XML stays well formed.
"""
import pathlib
import struct
import sys
import xml.parsers.expat
import zipfile

folder = pathlib.Path(sys.argv[1])
folder.mkdir(parents=True, exist_ok=True)
container = ('<?xml version="1.0"?><container version="1.0" '
             'xmlns="urn:oasis:names:tc:opendocument:xmlns:container">'
             '<rootfiles><rootfile full-path="OEBPS/content.opf" '
             'media-type="application/oebps-package+xml"/></rootfiles></container>')
package = ('<package xmlns="http://www.idpf.org/2007/opf" version="3.0" '
           'unique-identifier="id"><metadata xmlns:dc="http://purl.org/dc/elements/1.1/">'
           '<dc:identifier id="id">recovery-fixture</dc:identifier>'
           '<dc:title>EPUB Recovery Fixture</dc:title><dc:language>en</dc:language>'
           '</metadata><manifest><item id="nav" href="nav.xhtml" '
           'media-type="application/xhtml+xml" properties="nav"/>' + ''.join(
               '<item id="%s" href="%s.xhtml" media-type="application/xhtml+xml"/>'
               % (name, name) for name in ('first', 'middle', 'last')) +
           '</manifest><spine><itemref idref="first"/><itemref idref="middle"/>'
           '<itemref idref="last"/></spine></package>')
nav = ('<html xmlns="http://www.w3.org/1999/xhtml" '
       'xmlns:epub="http://www.idpf.org/2007/ops"><body>'
       '<nav epub:type="toc"><ol>' + ''.join(
           '<li><a href="%s.xhtml#%s-anchor">%s chapter</a></li>' % (name, name, name)
           for name in ('first', 'middle', 'last')) + '</ol></nav></body></html>')


def document(name, body):
    return ('<html xmlns="http://www.w3.org/1999/xhtml"><head><title>%s</title>'
            '</head><body><h1 id="%s-anchor">%s chapter</h1>%s</body></html>'
            % (name, name, name, body))


first = document('first', '<p>FIRST-START <a href="#first-anchor">link label</a></p>' +
                 ''.join('<p>' + ' '.join('word%04d' % i for i in range(80)) + '</p>'
                         for _ in range(220)) + '<p>FIRST-END</p>')
middle = document('middle', '<p>MIDDLE-SPINE</p>')
last = document('last', '<p>FINAL-SPINE</p>')
entries = {'META-INF/container.xml': container, 'OEBPS/content.opf': package,
           'OEBPS/nav.xhtml': nav, 'OEBPS/first.xhtml': first,
           'OEBPS/middle.xhtml': middle, 'OEBPS/last.xhtml': last}
for text in entries.values():
    xml.parsers.expat.ParserCreate().Parse(text, True)

for variant in ('valid', 'badxml', 'badcrc'):
    path = folder / (variant + '.epub')
    with zipfile.ZipFile(path, 'w', compression=zipfile.ZIP_STORED) as archive:
        archive.writestr('mimetype', 'application/epub+zip')
        for name, text in entries.items():
            if variant == 'badxml' and name == 'OEBPS/middle.xhtml':
                text = text.replace('</body>', '<unclosed></body>')
                try:
                    xml.parsers.expat.ParserCreate().Parse(text, True)
                except xml.parsers.expat.ExpatError:
                    pass
                else:
                    raise AssertionError('badxml must contain an XML error')
            archive.writestr(name, text)
    with zipfile.ZipFile(path) as archive:
        assert archive.testzip() is None
        if variant == 'badcrc':
            info = archive.getinfo('OEBPS/middle.xhtml')
            data = bytearray(path.read_bytes())
            name_length, extra_length = struct.unpack_from('<HH', data, info.header_offset + 26)
            payload = info.header_offset + 30 + name_length + extra_length
            offset = payload + middle.encode().index(b'MIDDLE-SPINE')
            data[offset] = ord('m')
            xml.parsers.expat.ParserCreate().Parse(bytes(data[payload:payload + info.file_size]), True)
            path.write_bytes(data)
    if variant == 'badcrc':
        with zipfile.ZipFile(path) as archive:
            assert archive.testzip() == 'OEBPS/middle.xhtml'
            assert b'FIRST-START' in archive.read('OEBPS/first.xhtml')
            assert b'FINAL-SPINE' in archive.read('OEBPS/last.xhtml')
    (folder / ('cache-' + variant)).mkdir()
(folder / 'cache-cancel').mkdir()

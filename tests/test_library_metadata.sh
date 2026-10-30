#!/usr/bin/env bash
set -euo pipefail
source "$(dirname "$0")/test_build.sh"
TASK_TMP="$(mktemp -d)"
trap 'rm -rf "$TASK_TMP"' EXIT
mkdir -p "$TASK_TMP/sdmc:/3ds/3dslibris/cache/meta"
python3 - "$TASK_TMP" <<'FIXTURES'
import sys,zipfile,struct
from pathlib import Path
p=Path(sys.argv[1])
container='<container><rootfiles><rootfile full-path="OEBPS/book.opf"/></rootfiles></container>'
def epub(name,opf):
 with zipfile.ZipFile(p/name,'w',compression=zipfile.ZIP_DEFLATED) as z:
  z.writestr('META-INF/container.xml',container)
  z.writestr('OEBPS/book.opf',opf)
  z.writestr('OEBPS/chapter.xhtml','invalid book body, must never be read')
def opf(title='',author=''):
 return '<package xmlns:dc="http://purl.org/dc/elements/1.1/"><metadata><dc:title>'+title+'</dc:title><dc:creator>'+author+'</dc:creator></metadata><manifest><item id="cover" properties="cover-image" href="images/cover.jpg" media-type="image/jpeg"/></manifest></package>'
epub('sample.epub',opf('El océano','José Pérez'))
epub('empty.epub',opf())
for name,meta in (('fallback-cover.epub',''),('nonimage-cover.epub','<meta name="cover" content="cover-page"/>')):
 epub(name,'<package xmlns:dc="dc"><metadata>'+meta+'</metadata><manifest><item id="logo" href="images/logo.png" media-type="image/png"/><item id="cover-page" href="cover.xhtml" media-type="application/xhtml+xml"/><item id="illustration" href="images/my%20cover.jpg" media-type="image/jpeg"/></manifest></package>')
epub('large-title.epub',opf('x'*4097))
epub('large-xml.epub',opf()+' '*(4*1024*1024))
fb2='<FictionBook><description><title-info><book-title>El océano</book-title><author><first-name>José</first-name><last-name>Pérez</last-name></author></title-info></description><body><intentionally invalid'
for name in ('sample.fb2','uncached.fb2'): (p/name).write_text(fb2)
(p/'bad.fb2').write_text('<FictionBook><description>')
(p/'entity.fb2').write_text('<!DOCTYPE FictionBook [<!ENTITY x SYSTEM "file:///etc/passwd">]><FictionBook><body/></FictionBook>')
with zipfile.ZipFile(p/'sample.cbz','w') as z:
 z.writestr('ComicInfo.xml','<ComicInfo><Title>El océano</Title><Writer>José Pérez</Writer></ComicInfo>')
 z.writestr('page.png','invalid image, must never be decoded')
with zipfile.ZipFile(p/'sample.xps','w') as z:
 z.writestr('_rels/.rels','<Relationships><Relationship Type="http://schemas.openxmlformats.org/package/2006/relationships/metadata/core-properties" Target="/Metadata/Core.xml"/></Relationships>')
 z.writestr('Metadata/Core.xml','<cp:coreProperties xmlns:cp="core" xmlns:dc="dc"><dc:title>El océano</dc:title><dc:creator>José Pérez</dc:creator></cp:coreProperties>')
(p/'sample.oxps').write_bytes((p/'sample.xps').read_bytes())
(p/'bad.cbz').write_bytes(b'not zip')
# Only record 0 is metadata; the following text record is deliberately invalid.
b32=lambda n:struct.pack('>I',n)
mobi=bytearray(0xE8);mobi[:4]=b'MOBI';mobi[4:8]=b32(len(mobi));mobi[12:16]=b32(65001);mobi[0x80:0x84]=b32(0x40)
records=[]
for typ,value in ((100,'José Pérez'),(503,'El océano')):
 data=value.encode();records.append(b32(typ)+b32(8+len(data))+data)
exth=b'EXTH'+b32(12+sum(map(len,records)))+b32(len(records))+b''.join(records)
rec0=bytes(16)+mobi+exth
pdb=bytearray(78);pdb[60:68]=b'BOOKMOBI';pdb[76:78]=struct.pack('>H',2)
with open(p/'sample.mobi','wb') as f:
 f.write(pdb+b32(94)+bytes(4)+b32(94+len(rec0))+bytes(4)+rec0)
 f.seek(16*1024*1024);f.write(b'invalid compressed text')
(p/'bad.mobi').write_bytes(b'bad')
FIXTURES
MINIZIP="$TEST_ROOT/third_party/mupdf/thirdparty/zlib/contrib/minizip"
for name in unzip ioapi; do
 "$CC_BIN" ${CFLAGS:-} -c "$MINIZIP/$name.c" -o "$TASK_TMP/$name.o"
done
cd "$TASK_TMP"
build_test test_library_metadata \
 "$TEST_ROOT/tests/test_library_metadata.cpp" \
 "$TEST_ROOT/source/formats/common/library_metadata.cpp" \
 "$TEST_ROOT/source/formats/common/library_metadata_mupdf.cpp" \
 "$TEST_ROOT/source/formats/common/book_meta_cache.cpp" \
 "$TEST_ROOT/source/formats/common/binary_io_utils.cpp" \
 "$TEST_ROOT/source/shared/string_utils.cpp" \
 "$TEST_ROOT/source/shared/utf8_utils.cpp" \
 "$TASK_TMP/unzip.o" "$TASK_TMP/ioapi.o" --expat -lz

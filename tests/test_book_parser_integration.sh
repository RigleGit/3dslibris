set -eu
source "$(dirname "$0")/test_build.sh"

CC_BIN="${CC:-cc}"
CXX_BIN="${CXX:-c++}"

ZLIB_DIR="$TEST_ROOT/third_party/mupdf/thirdparty/zlib"
MINIZIP_DIR="$ZLIB_DIR/contrib/minizip"

EXPAT_OBJS=()
while IFS= read -r obj; do
  [ -n "$obj" ] || continue
  EXPAT_OBJS+=("$obj")
done <<'EOF'
EOF

_build_zlib_objs() {
  local -a objs
  objs=()
  for f in inflate inftrees inffast zutil adler32 crc32; do
    local src="$ZLIB_DIR/${f}.c"
    local obj="$TEST_OUTDIR/zlib_${f}.o"
    if [ -f "$src" ] && [ ! -f "$obj" ]; then
      # shellcheck disable=SC2086
      "$CC_BIN" -std=c99 ${CFLAGS:-} -I"$ZLIB_DIR" -c "$src" -o "$obj"
    fi
    [ -f "$obj" ] && objs+=("$obj")
  done
  for f in unzip ioapi; do
    local src="$MINIZIP_DIR/${f}.c"
    local obj="$TEST_OUTDIR/minizip_${f}.o"
    if [ -f "$src" ] && [ ! -f "$obj" ]; then
      # shellcheck disable=SC2086
      "$CC_BIN" -std=c99 ${CFLAGS:-} -I"$ZLIB_DIR" -I"$ZLIB_DIR/contrib" -c "$src" -o "$obj"
    fi
    [ -f "$obj" ] && objs+=("$obj")
  done
  for obj in "${objs[@]}"; do
    printf '%s\n' "$obj"
  done
}

ZLIB_MINIZIP_OBJS=()
while IFS= read -r obj; do
  [ -n "$obj" ] || continue
  ZLIB_MINIZIP_OBJS+=("$obj")
done <<EOF
$(_build_zlib_objs)
EOF

if [ -f "$TEST_ROOT/third_party/expat/xmlparse.c" ]; then
  expat_flags="-DXML_CONTEXT_BYTES=1024 -DXML_DTD=1 -DXML_GE=1 -DHAVE_GETRANDOM -DHAVE_SYS_RANDOM_H"
  expat_inc="-I$TEST_ROOT/third_party/expat"
  for f in xmlparse xmlrole xmltok; do
    src="$TEST_ROOT/third_party/expat/${f}.c"
    if [ -f "$src" ] && [ ! -f "$TEST_OUTDIR/expat_${f}.o" ]; then
      # shellcheck disable=SC2086
      "$CC_BIN" -std=c99 ${CFLAGS:-} $expat_flags $expat_inc -c "$src" -o "$TEST_OUTDIR/expat_${f}.o"
    fi
    [ -f "$TEST_OUTDIR/expat_${f}.o" ] && EXPAT_OBJS+=("$TEST_OUTDIR/expat_${f}.o")
  done
fi

CBZ_TMP="$(mktemp -d)"
trap 'rm -rf "$CBZ_TMP"' EXIT
export TEST_CBZ_READING_DIR="$CBZ_TMP"
export TEST_MOBI_READING_DIR="$CBZ_TMP/mobi"
export TEST_EPUB_RECOVERY_DIR="$CBZ_TMP/epub"
export TEST_ODT_READING_DIR="$CBZ_TMP/odt"
export TEST_COVER_READING_DIR="$CBZ_TMP/covers"
export TEST_INLINE_IMAGE_READING_DIR="$CBZ_TMP/inline-images"
python3 "$TEST_ROOT/tests/fixtures/generate_epub_recovery.py" "$TEST_EPUB_RECOVERY_DIR"
python3 "$TEST_ROOT/tests/fixtures/generate_mobi_reading.py" "$TEST_MOBI_READING_DIR"
python3 - "$CBZ_TMP" <<'PYFIXTURE'
import pathlib
import base64
import struct
import sys
import zipfile
import zlib
import xml.parsers.expat
folder = pathlib.Path(sys.argv[1])
def chunk(kind, data):
    return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data) & 0xffffffff)
def png(rgb, width=2, height=2):
    return (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, 8, 2, 0, 0, 0)) +
            chunk(b'IDAT', zlib.compress((b'\0' + bytes(rgb) * width) * height)) + chunk(b'IEND', b''))
with zipfile.ZipFile(folder / 'first.cbz', 'w', compression=zipfile.ZIP_DEFLATED) as archive:
    archive.writestr('10-blue.png', png((0, 0, 255)))
    archive.writestr('2-red.png', png((255, 0, 0)))
    archive.writestr('30-broken.png', b'not an image')
    archive.writestr('notes.txt', b'not a comic page')
    archive.writestr('ComicInfo.xml', '<ComicInfo><Pages><Page Image="1" Bookmark="Blue chapter"/></Pages></ComicInfo>')
with zipfile.ZipFile(folder / 'second.cbz', 'w', compression=zipfile.ZIP_DEFLATED) as archive:
    archive.writestr('green.png', png((0, 255, 0)))
odt_folder = folder / 'odt'
odt_folder.mkdir()
with zipfile.ZipFile(odt_folder / 'basic.odt', 'w', compression=zipfile.ZIP_DEFLATED) as archive:
    archive.writestr('content.xml', '''<?xml version="1.0"?>
<office:document-content xmlns:office="urn:oasis:names:tc:opendocument:xmlns:office:1.0"
 xmlns:text="urn:oasis:names:tc:opendocument:xmlns:text:1.0">
<office:body><office:text><text:h text:outline-level="1">Chapter One</text:h>
<text:p>ODT first paragraph &amp; decoded entity.</text:p>
<text:h text:outline-level="2">Section Two</text:h>
<text:p>ODT final paragraph.</text:p></office:text></office:body>
</office:document-content>''')
cover_folder = folder / 'covers'
cover_folder.mkdir()
red = png((255, 0, 0))
(cover_folder / 'red.png').write_bytes(red)
(cover_folder / 'boundary.png').write_bytes(png((255, 0, 0), 2048, 1))
(cover_folder / 'over-width.png').write_bytes(png((255, 0, 0), 2049, 1))
(cover_folder / 'over-height.png').write_bytes(png((255, 0, 0), 1, 2049))
(cover_folder / 'corrupt-body.png').write_bytes(red[:41])
(cover_folder / 'corrupt-header').write_bytes(b'broken image header')
gif = base64.b64decode('R0lGODlhAQABAIAAAAAAAP///yH5BAEAAAAALAAAAAABAAEAAAICRAEAOw==')
(cover_folder / 'black.gif').write_bytes(gif)
(cover_folder / 'zero-width.gif').write_bytes(gif[:6] + b'\0\0' + gif[8:])
(cover_folder / 'zero-height.gif').write_bytes(gif[:8] + b'\0\0' + gif[10:])
# Fixed 1x1 green JPEG fixture; tests require no image-generation dependency.
(cover_folder / 'green.jpg').write_bytes(base64.b64decode(
    '/9j/4AAQSkZJRgABAQAAAQABAAD/2wBDAAEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQH/2wBDAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQH/wAARCAABAAEDAREAAhEBAxEB/8QAFAABAAAAAAAAAAAAAAAAAAAACP/EABQQAQAAAAAAAAAAAAAAAAAAAAD/xAAUAQEAAAAAAAAAAAAAAAAAAAAK/8QAFBEBAAAAAAAAAAAAAAAAAAAAAP/aAAwDAQACEQMRAD8AWAr4U7//2Q=='))
image_folder = folder / 'inline-images'
image_folder.mkdir()
container = ('<container xmlns="urn:oasis:names:tc:opendocument:xmlns:container">'
             '<rootfiles><rootfile full-path="content.opf" media-type="application/oebps-package+xml"/>'
             '</rootfiles></container>')
package = ('<package xmlns="http://www.idpf.org/2007/opf" version="3.0" unique-identifier="id">'
           '<metadata xmlns:dc="http://purl.org/dc/elements/1.1/"><dc:identifier id="id">images</dc:identifier>'
           '<dc:title>Repeated images</dc:title><dc:language>en</dc:language></metadata><manifest>'
           '<item id="body" href="content.xhtml" media-type="application/xhtml+xml"/>'
           '<item id="nav" href="nav.xhtml" media-type="application/xhtml+xml" properties="nav"/>' +
           ''.join('<item id="%s" href="%s.png" media-type="image/png"/>' % (name, name) for name in 'abc') +
           '</manifest><spine><itemref idref="body"/></spine></package>')
nav = ('<html xmlns="http://www.w3.org/1999/xhtml" xmlns:epub="http://www.idpf.org/2007/ops">'
       '<body><nav epub:type="toc"><ol>' + ''.join(
           '<li><a href="content.xhtml#g%d">Group %d</a></li>' % (i, i) for i in range(3)) +
       '</ol></nav></body></html>')
for shared in (True, False):
    groups = []
    for group in range(3):
        images = ''.join('<p style="margin:0"><img src="%s.png"%s/></p>' %
                         ('a' if shared else name, width) for name, width in
                         zip('abc', (' width="40"', ' style="width:80px"', '')))
        groups.append('<div id="g%d"%s><h2>Group %d</h2>%s<p>AFTER%d</p></div>' %
                      (group, ' style="page-break-after:always"' if group < 2 else '', group, images, group))
    content = '<html xmlns="http://www.w3.org/1999/xhtml"><body>' + ''.join(groups) + '</body></html>'
    entries = {'META-INF/container.xml': container, 'content.opf': package,
               'nav.xhtml': nav, 'content.xhtml': content}
    for value in entries.values():
        xml.parsers.expat.ParserCreate().Parse(value, True)
    path = image_folder / ('shared.epub' if shared else 'separate.epub')
    with zipfile.ZipFile(path, 'w', compression=zipfile.ZIP_DEFLATED) as archive:
        archive.writestr('mimetype', 'application/epub+zip', compress_type=zipfile.ZIP_STORED)
        for name, data in entries.items():
            archive.writestr(name, data)
        for name in 'abc':
            archive.writestr(name + '.png', png((255, 0, 0), 160, 32))
    with zipfile.ZipFile(path) as archive:
        assert archive.testzip() is None
(image_folder / 'cache').mkdir()
PYFIXTURE

# Count decode calls in the test executable while using the real implementation.
"$CXX_BIN" -std=c++11 ${CXXFLAGS:-} \
  -I"$TEST_ROOT/include" -I"$TEST_ROOT/third_party/stb" \
  -DLoadFromMemory=LoadFromMemoryForCoverTest \
  -c "$TEST_ROOT/source/core/stb_image_impl.cpp" \
  -o "$TEST_OUTDIR/cover_stb_image_impl.o"

"$CXX_BIN" -std=c++11 \
  ${CXXFLAGS:-} \
  -DDSLIBRIS_HOST_TEST -DDSLIBRIS_REAL_CBZ_TEST -DDSLIBRIS_REAL_MOBI_TEST -DDSLIBRIS_REAL_ODT_TEST \
  -include "$TEST_ROOT/tests/stubs/cbz_platform.h" \
  "-I$TEST_ROOT/tests/stubs" \
  "-I$TEST_ROOT/include" \
  "-I$TEST_ROOT/third_party/utf8proc" \
  "-I$TEST_ROOT/third_party/libunibreak/src" \
  "-I$ZLIB_DIR" \
  "-I$ZLIB_DIR/contrib" \
  "-I$TEST_ROOT/third_party/stb" \
  -DTEST_FIXTURES_DIR=\""$TEST_ROOT/tests/fixtures"\" \
  "$TEST_ROOT/tests/test_book_parser_integration.cpp" \
  "$TEST_ROOT/tests/stubs/book_reflow_worker_stub.cpp" \
  "$TEST_ROOT/tests/stubs/book_worker_lifecycle_stub.cpp" \
  "$TEST_ROOT/tests/stubs/book_fixed_layout_stubs.cpp" \
  "$TEST_ROOT/source/book/book_inline_image.cpp" \
  "$TEST_ROOT/tests/stubs/epub_cover_stub.cpp" \
  "$TEST_ROOT/tests/stubs/fixed_format_parser_stubs.cpp" \
  "$TEST_ROOT/tests/stubs/mupdf_bidi_stub.cpp" \
  "$TEST_ROOT/source/book/book.cpp" \
  "$TEST_ROOT/source/book/book_parser.cpp" \
  "$TEST_ROOT/source/book/book_xml_parser.cpp" \
  "$TEST_ROOT/source/book/book_xml_parser_support.cpp" \
  "$TEST_ROOT/source/book/book_xml_table_handler.cpp" \
  "$TEST_ROOT/source/book/book_xml_heading_handler.cpp" \
  "$TEST_ROOT/source/book/book_xml_image_handler.cpp" \
  "$TEST_ROOT/source/book/book_xml_anchor_handler.cpp" \
  "$TEST_ROOT/source/book/book_xml_flow_emission.cpp" \
  "$TEST_ROOT/source/book/book_xml_screen_advance.cpp" \
  "$TEST_ROOT/source/book/book_xml_element_style.cpp" \
  "$TEST_ROOT/source/book/book_xml_inline_handler.cpp" \
  "$TEST_ROOT/source/book/book_xml_block_handler.cpp" \
  "$TEST_ROOT/source/book/book_xml_fb2_handler.cpp" \
  "$TEST_ROOT/source/book/book_xml_block_utils.cpp" \
  "$TEST_ROOT/source/book/book_xml_css_resolver.cpp" \
  "$TEST_ROOT/source/book/book_xml_css_style_utils.cpp" \
  "$TEST_ROOT/source/book/book_xml_css_inline_style.cpp" \
  "$TEST_ROOT/source/book/book_xml_flow_layout.cpp" \
  "$TEST_ROOT/source/book/book_xml_hidden_utils.cpp" \
  "$TEST_ROOT/source/book/book_xml_list_utils.cpp" \
  "$TEST_ROOT/source/book/book_xml_parser_style_utils.cpp" \
  "$TEST_ROOT/source/book/book_xml_table_utils.cpp" \
  "$TEST_ROOT/source/book/book_xml_text_emit.cpp" \
  "$TEST_ROOT/source/book/book_open_index.cpp" \
  "$TEST_ROOT/source/book/epub_css_class_map.cpp" \
  "$TEST_ROOT/source/book/epub_css_tokenizer.cpp" \
  "$TEST_ROOT/source/book/heading_layout.cpp" \
  "$TEST_ROOT/source/book/inline_image_layout.cpp" \
  "$TEST_ROOT/source/book/inline_image_page_layout_utils.cpp" \
  "$TEST_ROOT/source/book/inline_image_screen_layout.cpp" \
  "$TEST_ROOT/source/book/page.cpp" \
  "$TEST_ROOT/source/book/page_alignment_utils.cpp" \
  "$TEST_ROOT/source/book/book_renderer.cpp" \
  "$TEST_ROOT/source/book/layout_reflow.cpp" \
  "$TEST_ROOT/source/core/parse.cpp" \
  "$TEST_OUTDIR/cover_stb_image_impl.o" \
  "$TEST_ROOT/source/formats/epub/epub_parser.cpp" \
  "$TEST_ROOT/source/formats/epub/epub.cpp" \
  "$TEST_ROOT/source/formats/epub/epub_stylesheet_utils.cpp" \
  "$TEST_ROOT/source/formats/epub/epub_manifest.cpp" \
  "$TEST_ROOT/source/formats/epub/epub_toc.cpp" \
  "$TEST_ROOT/source/formats/epub/epub_zip_utils.cpp" \
  "$TEST_ROOT/source/formats/epub/epub_ncx_parser.cpp" \
  "$TEST_ROOT/source/formats/epub/epub_package_toc_utils.cpp" \
  "$TEST_ROOT/source/formats/epub/epub_toc_diag_utils.cpp" \
  "$TEST_ROOT/source/formats/epub/epub_toc_package_loader_utils.cpp" \
  "$TEST_ROOT/source/formats/epub/epub_toc_title_match_utils.cpp" \
  "$TEST_ROOT/source/formats/epub/epub_page_cache.cpp" \
  "$TEST_ROOT/source/formats/epub/epub_cache.cpp" \
  "$TEST_ROOT/source/formats/cbz/cbz_parser.cpp" \
  "$TEST_ROOT/source/formats/cbz/cbz_document.cpp" \
  "$TEST_ROOT/source/formats/cbz/cbz_archive.cpp" \
  "$TEST_ROOT/source/formats/cbz/cbz_decode.cpp" \
  "$TEST_ROOT/source/formats/cbz/cbz_view.cpp" \
  "$TEST_ROOT/source/formats/cbz/cbz_worker.cpp" \
  "$TEST_ROOT/source/formats/common/pdf_view_utils.cpp" \
  "$TEST_ROOT/source/formats/common/fixed_layout_blit_utils.cpp" \
  "$TEST_ROOT/source/formats/txt/txt_parser.cpp" \
  "$TEST_ROOT/source/formats/txt/txt_loader.cpp" \
  "$TEST_ROOT/source/formats/markdown/markdown_parser.cpp" \
  "$TEST_ROOT/source/formats/fb2/fb2_parser.cpp" \
  "$TEST_ROOT/source/formats/fb2/fb2.cpp" \
  "$TEST_ROOT/source/formats/rtf/rtf_parser.cpp" \
  "$TEST_ROOT/source/formats/rtf/rtf_loader.cpp" \
  "$TEST_ROOT/source/formats/odt/odt_parser.cpp" \
  "$TEST_ROOT/source/formats/odt/odt_loader.cpp" \
  "$TEST_ROOT/source/formats/common/plain_parser.cpp" \
  "$TEST_ROOT/source/formats/common/plain_text_stream.cpp" \
  "$TEST_ROOT/source/formats/common/plain_text_perf_utils.cpp" \
  "$TEST_ROOT/source/formats/common/text_helpers.cpp" \
  "$TEST_ROOT/source/formats/common/xml_book_parser.cpp" \
  "$TEST_ROOT/source/formats/common/xml_parse_utils.cpp" \
  "$TEST_ROOT/source/formats/common/file_read_utils.cpp" \
  "$TEST_ROOT/source/formats/common/binary_io_utils.cpp" \
  "$TEST_ROOT/source/formats/common/book_meta_cache.cpp" \
  "$TEST_ROOT/source/formats/common/page_cache_utils.cpp" \
  "$TEST_ROOT/source/formats/common/html_entity_utils.cpp" \
  "$TEST_ROOT/source/formats/common/href_normalization.cpp" \
  "$TEST_ROOT/source/formats/common/page_text_extract_utils.cpp" \
  "$TEST_ROOT/source/formats/common/epub_image_utils.cpp" \
  "$TEST_ROOT/source/formats/common/zip_read_utils.cpp" \
  "$TEST_ROOT/source/formats/mobi/mobi.cpp" \
  "$TEST_ROOT/source/formats/mobi/mobi_parser.cpp" \
  "$TEST_ROOT/source/formats/mobi/mobi_parse_book.cpp" \
  "$TEST_ROOT/source/formats/mobi/mobi_book_hooks.cpp" \
  "$TEST_ROOT/source/formats/mobi/mobi_safe_markup_extract.cpp" \
  "$TEST_ROOT/source/formats/mobi/mobi_toc_prepare.cpp" \
  "$TEST_ROOT/source/formats/mobi/mobi_toc_resolver.cpp" \
  "$TEST_ROOT/source/formats/mobi/mobi_structured_toc_parser.cpp" \
  "$TEST_ROOT/source/formats/mobi/mobi_position_map.cpp" \
  "$TEST_ROOT/source/formats/mobi/mobi_text_cleanup.cpp" \
  "$TEST_ROOT/source/formats/mobi/mobi_cleanup_policy.cpp" \
  "$TEST_ROOT/source/formats/mobi/mobi_toc_finalize.cpp" \
  "$TEST_ROOT/source/formats/mobi/mobi_toc_finalize_policy.cpp" \
  "$TEST_ROOT/source/formats/mobi/mobi_toc_apply.cpp" \
  "$TEST_ROOT/source/formats/mobi/mobi_cover_extract.cpp" \
  "$TEST_ROOT/source/formats/mobi/mobi_cover_meta_cache.cpp" \
  "$TEST_ROOT/source/formats/mobi/mobi_text_decode.cpp" \
  "$TEST_ROOT/source/formats/mobi/mobi_parser_core.cpp" \
  "$TEST_ROOT/source/formats/mobi/mobi_record_scan.cpp" \
  "$TEST_ROOT/source/formats/mobi/mobi_record_decode.cpp" \
  "$TEST_ROOT/source/formats/mobi/mobi_page_cache.cpp" \
  "$TEST_ROOT/source/formats/mobi/mobi_heading_markers.cpp" \
  "$TEST_ROOT/source/reader/inline_link_utils.cpp" \
  "$TEST_ROOT/source/shared/string_utils.cpp" \
  "$TEST_ROOT/source/shared/debug_log.cpp" \
  "$TEST_ROOT/source/shared/text_layout_utils.cpp" \
  "$TEST_ROOT/source/shared/text_unicode_utils.cpp" \
  "$TEST_ROOT/source/shared/text_bidi_utils.cpp" \
  "$TEST_ROOT/source/shared/text_arabic_shaping.cpp" \
  "$TEST_ROOT/source/shared/open_cancel_poll.cpp" \
  "$TEST_ROOT/source/shared/open_cancel_poll_utils.cpp" \
  "$TEST_ROOT/source/shared/app_flow_utils.cpp" \
  "$TEST_ROOT/source/shared/utf8_utils.cpp" \
  "$TEST_ROOT/source/shared/cover_decode_utils.cpp" \
  "$TEST_ROOT/source/shared/image_scale_utils.cpp" \
  "${THIRD_PARTY_OBJS[@]}" \
  "${EXPAT_OBJS[@]}" \
  "${ZLIB_MINIZIP_OBJS[@]}" \
  ${LDFLAGS:-} \
  -lz \
  -o "$TEST_OUTDIR/test_book_parser_integration"

"$TEST_OUTDIR/test_book_parser_integration"

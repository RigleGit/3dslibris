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
python3 "$TEST_ROOT/tests/fixtures/generate_epub_recovery.py" "$TEST_EPUB_RECOVERY_DIR"
python3 "$TEST_ROOT/tests/fixtures/generate_mobi_reading.py" "$TEST_MOBI_READING_DIR"
python3 - "$CBZ_TMP" <<'PYFIXTURE'
import pathlib
import struct
import sys
import zipfile
import zlib
folder = pathlib.Path(sys.argv[1])
def chunk(kind, data):
    return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data) & 0xffffffff)
def png(rgb):
    return (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', 2, 2, 8, 2, 0, 0, 0)) +
            chunk(b'IDAT', zlib.compress((b'\0' + bytes(rgb) * 2) * 2)) + chunk(b'IEND', b''))
with zipfile.ZipFile(folder / 'first.cbz', 'w', compression=zipfile.ZIP_DEFLATED) as archive:
    archive.writestr('10-blue.png', png((0, 0, 255)))
    archive.writestr('2-red.png', png((255, 0, 0)))
    archive.writestr('30-broken.png', b'not an image')
    archive.writestr('notes.txt', b'not a comic page')
    archive.writestr('ComicInfo.xml', '<ComicInfo><Pages><Page Image="1" Bookmark="Blue chapter"/></Pages></ComicInfo>')
with zipfile.ZipFile(folder / 'second.cbz', 'w', compression=zipfile.ZIP_DEFLATED) as archive:
    archive.writestr('green.png', png((0, 255, 0)))
PYFIXTURE

"$CXX_BIN" -std=c++11 \
  ${CXXFLAGS:-} \
  -DDSLIBRIS_HOST_TEST -DDSLIBRIS_REAL_CBZ_TEST -DDSLIBRIS_REAL_MOBI_TEST \
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
  "$TEST_ROOT/tests/stubs/book_inline_image_stub.cpp" \
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
  "$TEST_ROOT/source/core/stb_image_impl.cpp" \
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

#!/bin/bash
set -eu
source "$(dirname "$0")/test_build.sh"

"${CXX:-c++}" -std=c++11 \
  ${CXXFLAGS:-} \
  -DDSLIBRIS_HOST_TEST \
  "-I$TEST_ROOT/tests/stubs" \
  "-I$TEST_ROOT/include" \
  "-I$TEST_ROOT/third_party/utf8proc" \
  "-I$TEST_ROOT/third_party/libunibreak/src" \
  "$TEST_ROOT/tests/test_epub_page_cache.cpp" \
  "$TEST_ROOT/tests/stubs/book_reflow_worker_stub.cpp" \
  "$TEST_ROOT/tests/stubs/book_worker_lifecycle_stub.cpp" \
  "$TEST_ROOT/tests/stubs/book_fixed_layout_stubs.cpp" \
  "$TEST_ROOT/tests/stubs/book_inline_image_stub.cpp" \
  "$TEST_ROOT/tests/stubs/epub_parser_stub.cpp" \
  "$TEST_ROOT/tests/stubs/fixed_format_parser_stubs.cpp" \
  "$TEST_ROOT/tests/stubs/mupdf_bidi_stub.cpp" \
  "$TEST_ROOT/source/book/book.cpp" \
  "$TEST_ROOT/source/book/page.cpp" \
  "$TEST_ROOT/source/book/page_alignment_utils.cpp" \
  "$TEST_ROOT/source/book/book_renderer.cpp" \
  "$TEST_ROOT/source/book/layout_reflow.cpp" \
  "$TEST_ROOT/source/book/heading_layout.cpp" \
  "$TEST_ROOT/source/book/inline_image_layout.cpp" \
  "$TEST_ROOT/source/formats/epub/epub_page_cache.cpp" \
  "$TEST_ROOT/source/formats/mobi/mobi_heading_markers.cpp" \
  "$TEST_ROOT/source/formats/common/page_cache_utils.cpp" \
  "$TEST_ROOT/source/formats/common/binary_io_utils.cpp" \
  "$TEST_ROOT/source/formats/common/href_normalization.cpp" \
  "$TEST_ROOT/source/formats/common/text_helpers.cpp" \
  "$TEST_ROOT/source/reader/inline_link_utils.cpp" \
  "$TEST_ROOT/source/shared/app_flow_utils.cpp" \
  "$TEST_ROOT/source/shared/debug_log.cpp" \
  "$TEST_ROOT/source/shared/open_cancel_poll.cpp" \
  "$TEST_ROOT/source/shared/open_cancel_poll_utils.cpp" \
  "$TEST_ROOT/source/shared/string_utils.cpp" \
  "$TEST_ROOT/source/shared/text_arabic_shaping.cpp" \
  "$TEST_ROOT/source/shared/text_bidi_utils.cpp" \
  "$TEST_ROOT/source/shared/text_layout_utils.cpp" \
  "$TEST_ROOT/source/shared/text_unicode_utils.cpp" \
  "$TEST_ROOT/source/shared/utf8_utils.cpp" \
  "${THIRD_PARTY_OBJS[@]}" \
  ${LDFLAGS:-} \
  -lz \
  -o "$TEST_OUTDIR/test_epub_page_cache"

"$TEST_OUTDIR/test_epub_page_cache" "$@"

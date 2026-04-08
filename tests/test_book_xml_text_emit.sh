set -eu
source "$(dirname "$0")/test_build.sh"
build_test test_book_xml_text_emit \
  "$TEST_ROOT/tests/test_book_xml_text_emit.cpp" \
  "$TEST_ROOT/source/core/parse.cpp" \
  "$TEST_ROOT/source/shared/text_unicode_utils.cpp" \
  "$TEST_ROOT/source/shared/text_layout_utils.cpp" \
  "$TEST_ROOT/source/shared/text_bidi_utils.cpp" \
  "$TEST_ROOT/source/shared/text_arabic_shaping.cpp" \
  "$TEST_ROOT/source/book/book_xml_text_emit.cpp" \
  "$TEST_ROOT/tests/stubs/mupdf_bidi_stub.cpp" \
  -I"$TEST_ROOT/tests/stubs"

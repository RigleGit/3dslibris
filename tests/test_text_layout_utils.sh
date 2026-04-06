set -eu
source "$(dirname "$0")/test_build.sh"
build_test test_text_layout_utils \
  "$TEST_ROOT/tests/test_text_layout_utils.cpp" \
  "$TEST_ROOT/source/shared/text_unicode_utils.cpp" \
  "$TEST_ROOT/source/shared/text_layout_utils.cpp" \
  "$TEST_ROOT/source/shared/text_bidi_utils.cpp" \
  "$TEST_ROOT/source/shared/text_arabic_shaping.cpp" \
  "$TEST_ROOT/tests/stubs/mupdf_bidi_stub.cpp" \
  -I"$TEST_ROOT/tests/stubs"

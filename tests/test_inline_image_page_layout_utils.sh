set -eu
source "$(dirname "$0")/test_build.sh"
build_test test_inline_image_page_layout_utils \
  "$TEST_ROOT/tests/test_inline_image_page_layout_utils.cpp" \
  "$TEST_ROOT/source/book/inline_image_page_layout_utils.cpp"

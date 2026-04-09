set -eu
source "$(dirname "$0")/test_build.sh"
build_test test_book_xml_block_utils \
  "$TEST_ROOT/tests/test_book_xml_block_utils.cpp" \
  "$TEST_ROOT/source/book/book_xml_block_utils.cpp" \
  -I"$TEST_ROOT/tests/stubs"

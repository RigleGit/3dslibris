set -eu
source "$(dirname "$0")/test_build.sh"
build_test test_book_xml_list_utils \
  "$TEST_ROOT/tests/test_book_xml_list_utils.cpp" \
  "$TEST_ROOT/source/core/parse.cpp" \
  "$TEST_ROOT/source/book/book_xml_list_utils.cpp" \
  -I"$TEST_ROOT/tests/stubs"

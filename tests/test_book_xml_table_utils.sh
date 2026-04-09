set -eu
source "$(dirname "$0")/test_build.sh"
build_test test_book_xml_table_utils \
  "$TEST_ROOT/tests/test_book_xml_table_utils.cpp" \
  "$TEST_ROOT/source/book/book_xml_table_utils.cpp"

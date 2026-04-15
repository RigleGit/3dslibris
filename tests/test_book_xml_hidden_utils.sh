set -eu

. "$(CDPATH= cd -- "$(dirname "$0")" && pwd)/test_build.sh"

build_test test_book_xml_hidden_utils \
  "$TEST_ROOT/tests/test_book_xml_hidden_utils.cpp" \
  "$TEST_ROOT/source/book/book_xml_hidden_utils.cpp"

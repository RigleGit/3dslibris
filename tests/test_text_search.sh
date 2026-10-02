set -eu
source "$(dirname "$0")/test_build.sh"
build_test test_text_search \
  "$TEST_ROOT/tests/test_text_search.cpp" \
  "$TEST_ROOT/source/book/text_search.cpp"

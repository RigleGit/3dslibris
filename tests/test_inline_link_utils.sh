set -eu

. "$(CDPATH= cd -- "$(dirname "$0")" && pwd)/test_build.sh"

build_test test_inline_link_utils \
  "$TEST_ROOT/tests/test_inline_link_utils.cpp" \
  "$TEST_ROOT/source/reader/inline_link_utils.cpp"

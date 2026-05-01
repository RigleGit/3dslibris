set -eu
source "$(dirname "$0")/test_build.sh"
build_test test_parse_block_margin_state \
  "$TEST_ROOT/tests/test_parse_block_margin_state.cpp" \
  "$TEST_ROOT/source/core/parse.cpp" \
  "-I$TEST_ROOT/tests/stubs"

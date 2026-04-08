set -eu
source "$(dirname "$0")/test_build.sh"
build_test test_text_render_layout_utils \
  "$TEST_ROOT/tests/test_text_render_layout_utils.cpp"

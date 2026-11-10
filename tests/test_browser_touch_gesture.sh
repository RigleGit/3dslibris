set -eu
source "$(dirname "$0")/test_build.sh"
build_test test_browser_touch_gesture \
  "$TEST_ROOT/tests/test_browser_touch_gesture.cpp"

set -eu
source "$(dirname "$0")/test_build.sh"
build_test test_reflow_open_stats \
  "$TEST_ROOT/tests/test_reflow_open_stats.cpp"

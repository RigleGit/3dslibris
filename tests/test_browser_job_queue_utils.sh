set -eu
source "$(dirname "$0")/test_build.sh"
build_test test_browser_job_queue_utils -- \
  -I"$TEST_ROOT/tests/stubs" -I"$TEST_ROOT/include" \
  "$TEST_ROOT/tests/test_browser_job_queue_utils.cpp"

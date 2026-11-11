set -eu
source "$(dirname "$0")/test_build.sh"
build_test test_reflow_open_profile \
  "$TEST_ROOT/tests/test_reflow_open_profile.cpp" \
  "$TEST_ROOT/source/formats/common/reflow_open_profile.cpp"

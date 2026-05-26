set -eu
source "$(dirname "$0")/test_build.sh"
build_test test_prefs_input_utils \
  "$TEST_ROOT/tests/test_prefs_input_utils.cpp" \
  "$TEST_ROOT/source/settings/prefs_input_utils.cpp"

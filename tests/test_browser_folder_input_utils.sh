set -eu
source "$(dirname "$0")/test_build.sh"
build_test test_browser_folder_input_utils \
  "$TEST_ROOT/tests/test_browser_folder_input_utils.cpp" \
  "$TEST_ROOT/source/library/browser_folder_input_utils.cpp"

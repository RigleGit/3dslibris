set -eu
source "$(dirname "$0")/test_build.sh"
build_test test_aspect_fit_utils \
  "$TEST_ROOT/tests/test_aspect_fit_utils.cpp"

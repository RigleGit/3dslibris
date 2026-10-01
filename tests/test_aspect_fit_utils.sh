set -eu
source "$(dirname "$0")/test_build.sh"
# Keep the default no-option build shape: native Bash 3.2 with set -u must
# compile and link successfully when optional flag and library arrays are empty.
build_test test_aspect_fit_utils \
  "$TEST_ROOT/tests/test_aspect_fit_utils.cpp"

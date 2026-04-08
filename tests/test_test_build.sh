set -eu
source "$(dirname "$0")/test_build.sh"
build_test test_build_helper_fixture \
  "$TEST_ROOT/tests/fixtures/test_build_helper_fixture.cpp"

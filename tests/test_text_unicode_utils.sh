set -eu
source "$(dirname "$0")/test_build.sh"
build_test test_text_unicode_utils \
  "$TEST_ROOT/tests/test_text_unicode_utils.cpp" \
  "$TEST_ROOT/source/shared/text_unicode_utils.cpp"

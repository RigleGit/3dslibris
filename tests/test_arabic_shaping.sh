set -eu
source "$(dirname "$0")/test_build.sh"
build_test test_arabic_shaping \
  "$TEST_ROOT/tests/test_arabic_shaping.cpp" \
  "$TEST_ROOT/source/shared/text_arabic_shaping.cpp"

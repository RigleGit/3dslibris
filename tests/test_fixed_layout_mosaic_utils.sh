set -eu
source "$(dirname "$0")/test_build.sh"
build_test test_fixed_layout_mosaic_utils \
  "$TEST_ROOT/tests/test_fixed_layout_mosaic_utils.cpp" \
  "$TEST_ROOT/source/formats/common/fixed_layout_mosaic_utils.cpp"

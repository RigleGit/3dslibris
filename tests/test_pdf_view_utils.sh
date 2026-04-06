set -eu
source "$(dirname "$0")/test_build.sh"
build_test test_pdf_view_utils \
  "$TEST_ROOT/tests/test_pdf_view_utils.cpp" \
  "$TEST_ROOT/source/formats/common/pdf_view_utils.cpp"

set -eu
source "$(dirname "$0")/test_build.sh"
TASK_TMP="$(mktemp -d)"
trap 'rm -rf "$TASK_TMP"' EXIT
mkdir -p "$TASK_TMP/sdmc:/3ds/3dslibris/cache/meta"
cd "$TASK_TMP"
build_test test_book_meta_cache -Dftell=ObserveMetadataTell \
  -include "$TEST_ROOT/tests/host_stdio_first.h" \
  "$TEST_ROOT/tests/test_book_meta_cache.cpp" \
  "$TEST_ROOT/source/formats/common/book_meta_cache.cpp" \
  "$TEST_ROOT/source/formats/common/binary_io_utils.cpp" \
  "$TEST_ROOT/source/shared/string_utils.cpp"

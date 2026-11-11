set -eu
source "$(dirname "$0")/test_build.sh"
build_test test_mobi_stream_remap \
  "$TEST_ROOT/tests/test_mobi_stream_remap.cpp" \
  "$TEST_ROOT/source/formats/mobi/mobi_position_map.cpp"

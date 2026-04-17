set -eu
source "$(dirname "$0")/test_build.sh"
if [ ! -f "$TEST_ROOT/third_party/minizip/unzip.h" ] && \
   [ ! -f "/opt/devkitpro/portlibs/3ds/include/minizip/unzip.h" ] && \
   [ ! -f "/usr/include/minizip/unzip.h" ]; then
  echo "SKIP test_xml_parse_utils: minizip headers not available in host environment"
  exit 0
fi
build_test test_xml_parse_utils \
  "$TEST_ROOT/tests/test_xml_parse_utils.cpp" \
  "$TEST_ROOT/source/formats/common/xml_parse_utils.cpp" \
  --stubs minizip_unzip_stubs.cpp

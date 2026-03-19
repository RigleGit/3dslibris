set -eu

ROOT="$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)"

if rg -n '#include "unzip.h"|#include "ioapi.h"' \
  "$ROOT/include" "$ROOT/source" "$ROOT/tests" \
  --glob '!third_party/**' --glob '!tests/test_minizip_devendorized.sh'; then
  echo "found legacy MiniZip includes"
  exit 1
fi

if rg -n 'source/core/(unzip|ioapi)\.c|include/minizip' \
  "$ROOT/Makefile" "$ROOT/tests" \
  --glob '!third_party/**' --glob '!tests/test_minizip_devendorized.sh'; then
  echo "found vendored MiniZip usage in build or tests"
  exit 1
fi

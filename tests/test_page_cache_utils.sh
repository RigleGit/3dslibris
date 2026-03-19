set -eu

ROOT="$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)"
OUTDIR="${TMPDIR:-/tmp}/3dslibris-tests"
mkdir -p "$OUTDIR"

c++ -std=c++11 \
  "$ROOT/tests/test_page_cache_utils.cpp" \
  "$ROOT/source/formats/common/page_cache_utils.cpp" \
  -I"$ROOT/include" \
  -o "$OUTDIR/test_page_cache_utils"

"$OUTDIR/test_page_cache_utils"

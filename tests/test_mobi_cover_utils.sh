set -eu

ROOT="$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)"
OUTDIR="${TMPDIR:-/tmp}/3dslibris-tests"
mkdir -p "$OUTDIR"

c++ -std=c++11 \
  "$ROOT/tests/test_mobi_cover_utils.cpp" \
  -I"$ROOT/include" \
  -o "$OUTDIR/test_mobi_cover_utils"

"$OUTDIR/test_mobi_cover_utils"

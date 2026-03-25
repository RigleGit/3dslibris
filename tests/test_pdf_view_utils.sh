set -eu

ROOT="$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)"
OUTDIR="${TMPDIR:-/tmp}/3dslibris-tests"
mkdir -p "$OUTDIR"

c++ -std=c++11 \
  "$ROOT/tests/test_pdf_view_utils.cpp" \
  "$ROOT/source/shared/pdf_view_utils.cpp" \
  -I"$ROOT/include" \
  -o "$OUTDIR/test_pdf_view_utils"

"$OUTDIR/test_pdf_view_utils"

set -eu

ROOT="$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)"
OUTDIR="${TMPDIR:-/tmp}/3dslibris-tests"
mkdir -p "$OUTDIR"

c++ -std=c++11 \
  "$ROOT/tests/test_text_size_limits.cpp" \
  -I"$ROOT/include" \
  -o "$OUTDIR/test_text_size_limits"

"$OUTDIR/test_text_size_limits"

set -eu

ROOT="$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)"
OUTDIR="${TMPDIR:-/tmp}/3dslibris-tests"
mkdir -p "$OUTDIR"

c++ -std=c++11 \
  "$ROOT/tests/test_app_flow_utils.cpp" \
  "$ROOT/source/shared/app_flow_utils.cpp" \
  -I"$ROOT/include" \
  -o "$OUTDIR/test_app_flow_utils"

"$OUTDIR/test_app_flow_utils"

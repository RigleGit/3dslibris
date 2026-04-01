set -eu

ROOT="$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)"
OUTDIR="${TMPDIR:-/tmp}/3dslibris-tests"
mkdir -p "$OUTDIR"

DTD="${EXPAT_ENABLE_DTD:-1}"
NS="${EXPAT_ENABLE_NS:-1}"
CONTEXT="${EXPAT_CONTEXT_BYTES:-1024}"

CCFLAGS="-DXML_STATIC -DHAVE_MEMMOVE -DXML_POOR_ENTROPY -I$ROOT/include -I$ROOT/third_party/expat"
CCFLAGS="$CCFLAGS -DDSLIBRIS_EXPAT_ENABLE_DTD=$DTD -DDSLIBRIS_EXPAT_ENABLE_NS=$NS -DDSLIBRIS_EXPAT_CONTEXT_BYTES=$CONTEXT"

cc -c $CCFLAGS "$ROOT/third_party/expat/loadlibrary.c" -o "$OUTDIR/loadlibrary-expat-features.o"
cc -c $CCFLAGS "$ROOT/third_party/expat/xmlparse.c" -o "$OUTDIR/xmlparse-expat-features.o"
cc -c $CCFLAGS "$ROOT/third_party/expat/xmlrole.c" -o "$OUTDIR/xmlrole-expat-features.o"
cc -c $CCFLAGS "$ROOT/third_party/expat/xmltok.c" -o "$OUTDIR/xmltok-expat-features.o"
cc -c $CCFLAGS "$ROOT/third_party/expat/xmltok_impl.c" -o "$OUTDIR/xmltok_impl-expat-features.o"
cc -c $CCFLAGS "$ROOT/third_party/expat/xmltok_ns.c" -o "$OUTDIR/xmltok_ns-expat-features.o"

c++ -std=c++11 \
  "$ROOT/tests/test_expat_features.cpp" \
  "$OUTDIR/loadlibrary-expat-features.o" \
  "$OUTDIR/xmlparse-expat-features.o" \
  "$OUTDIR/xmlrole-expat-features.o" \
  "$OUTDIR/xmltok-expat-features.o" \
  "$OUTDIR/xmltok_impl-expat-features.o" \
  "$OUTDIR/xmltok_ns-expat-features.o" \
  -I"$ROOT/include" \
  -I"$ROOT/third_party/expat" \
  -DEXPECT_DTD="$DTD" \
  -DEXPECT_NS="$NS" \
  -DEXPECT_CONTEXT_BYTES="$CONTEXT" \
  -o "$OUTDIR/test_expat_features"

"$OUTDIR/test_expat_features"

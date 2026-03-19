set -eu

ROOT="$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)"
OUTDIR="${TMPDIR:-/tmp}/3dslibris-tests"
mkdir -p "$OUTDIR"

cc -std=c99 -c "$ROOT/third_party/utf8proc/utf8proc.c" \
  -I"$ROOT/third_party/utf8proc" \
  -o "$OUTDIR/utf8proc.o"
cc -std=c99 -c "$ROOT/third_party/libunibreak/src/linebreak.c" \
  -I"$ROOT/third_party/libunibreak/src" \
  -o "$OUTDIR/linebreak.o"
cc -std=c99 -c "$ROOT/third_party/libunibreak/src/linebreakdata.c" \
  -I"$ROOT/third_party/libunibreak/src" \
  -o "$OUTDIR/linebreakdata.o"
cc -std=c99 -c "$ROOT/third_party/libunibreak/src/linebreakdef.c" \
  -I"$ROOT/third_party/libunibreak/src" \
  -o "$OUTDIR/linebreakdef.o"
cc -std=c99 -c "$ROOT/third_party/libunibreak/src/eastasianwidthdata.c" \
  -I"$ROOT/third_party/libunibreak/src" \
  -o "$OUTDIR/eastasianwidthdata.o"
cc -std=c99 -c "$ROOT/third_party/libunibreak/src/eastasianwidthdef.c" \
  -I"$ROOT/third_party/libunibreak/src" \
  -o "$OUTDIR/eastasianwidthdef.o"
cc -std=c99 -c "$ROOT/third_party/libunibreak/src/unibreakbase.c" \
  -I"$ROOT/third_party/libunibreak/src" \
  -o "$OUTDIR/unibreakbase.o"
cc -std=c99 -c "$ROOT/third_party/libunibreak/src/unibreakdef.c" \
  -I"$ROOT/third_party/libunibreak/src" \
  -o "$OUTDIR/unibreakdef.o"

c++ -std=c++11 \
  "$ROOT/tests/test_text_layout_utils.cpp" \
  "$ROOT/source/shared/text_unicode_utils.cpp" \
  "$ROOT/source/shared/text_layout_utils.cpp" \
  "$OUTDIR/utf8proc.o" \
  "$OUTDIR/linebreak.o" \
  "$OUTDIR/linebreakdata.o" \
  "$OUTDIR/linebreakdef.o" \
  "$OUTDIR/eastasianwidthdata.o" \
  "$OUTDIR/eastasianwidthdef.o" \
  "$OUTDIR/unibreakbase.o" \
  "$OUTDIR/unibreakdef.o" \
  -I"$ROOT/include" \
  -I"$ROOT/third_party/utf8proc" \
  -I"$ROOT/third_party/libunibreak/src" \
  -o "$OUTDIR/test_text_layout_utils"

"$OUTDIR/test_text_layout_utils"

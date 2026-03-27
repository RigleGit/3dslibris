set -eu

ROOT="$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)"
OUTDIR="${TMPDIR:-/tmp}/3dslibris-tests"
WORKDIR="$OUTDIR/cbz-archive"
mkdir -p "$OUTDIR" "$WORKDIR"

c++ -std=c++11 \
  "$ROOT/tests/test_cbz_archive.cpp" \
  "$ROOT/source/formats/cbz/cbz_archive.cpp" \
  "$ROOT/third_party/minizip/source/ioapi.c" \
  "$ROOT/third_party/minizip/source/unzip.c" \
  -I"$ROOT/include" \
  -I"$ROOT/third_party/minizip" \
  -I"$ROOT/third_party/minizip/include" \
  -I"$ROOT/third_party/minizip/source" \
  -lz \
  -o "$OUTDIR/test_cbz_archive"

ARCHIVE="$WORKDIR/sample.cbz"
python3 - "$ARCHIVE" <<'PY'
import sys, zipfile
archive = sys.argv[1]
with zipfile.ZipFile(archive, "w") as zf:
    zf.writestr("META-INF/container.xml", b"skip")
    zf.writestr(".DS_Store", b"skip")
    zf.writestr("./002-page.png", b"png")
    zf.writestr("010-last.jpeg", b"jpeg")
    zf.writestr("001-cover.jpg", b"jpg")
    zf.writestr("folder/", b"")
PY

"$OUTDIR/test_cbz_archive" "$ARCHIVE"

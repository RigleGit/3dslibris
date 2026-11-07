#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
if [ -z "${MUPDF_HOST_LIB_DIR:-}" ]; then
  echo 'SKIP: native MuPDF libraries required (MUPDF_HOST_LIB_DIR)'
  exit 77
fi
TASK_TMP="$(mktemp -d)"
trap 'rm -rf "$TASK_TMP"' EXIT
python3 - "$ROOT" "$TASK_TMP" <<'PYFIXTURE'
from pathlib import Path
import struct, sys, zipfile, zlib
root, tmp = map(Path, sys.argv[1:])
def chunk(tag, data):
    return struct.pack('>I', len(data))+tag+data+struct.pack('>I', zlib.crc32(tag+data))
png = (b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR', struct.pack('>IIBBBBB', 4097, 1, 8, 2, 0, 0, 0))+
       chunk(b'IDAT', zlib.compress(b'\0'+b'\0\xff\0'*4097))+chunk(b'IEND', b''))
with zipfile.ZipFile(tmp/'covers.epub', 'w') as archive:
    archive.write(root/'tests/fixtures/large-portrait-cover.jpg', 'cover.jpg')
    archive.writestr('oversized.png', png)
    archive.writestr('oversized.jpg', png)
PYFIXTURE
cat > "$TASK_TMP/allocator.h" <<'HEADER'
#include <stddef.h>
void *CoverMalloc(size_t);
void *CoverRealloc(void *, size_t);
void CoverFree(void *);
#define STBI_MALLOC(n) CoverMalloc(n)
#define STBI_REALLOC(p,n) CoverRealloc(p,n)
#define STBI_FREE(p) CoverFree(p)
HEADER
CXX_BIN="${CXX:-c++}"
if [ "$(uname -s)" = Darwin ]; then GC_FLAG=-Wl,-dead_strip; else GC_FLAG=-Wl,--gc-sections; fi
MINIZIP="$ROOT/third_party/mupdf/thirdparty/zlib/contrib/minizip"
for name in unzip ioapi; do
  "${CC:-cc}" ${CFLAGS:-} -c "$MINIZIP/$name.c" -o "$TASK_TMP/$name.o"
done
PNG_CFLAGS=$(pkg-config --cflags libpng)
PNG_LIBS=$(pkg-config --libs libpng)
"$CXX_BIN" -std=c++11 ${CXXFLAGS:-} -D__3DS__ $PNG_CFLAGS -include mupdf/fitz.h \
  -I"$ROOT/tests/cover_stubs" -I"$ROOT/third_party/mupdf/include" \
  -I"$ROOT/tests/stubs" -I"$ROOT/include" -I"$ROOT/third_party/stb" -I"$MINIZIP/.." \
  -c "$ROOT/source/formats/epub/epub_cover.cpp" -o "$TASK_TMP/epub.o"
"$CXX_BIN" -std=c++11 ${CXXFLAGS:-} -include "$TASK_TMP/allocator.h" \
  -I"$ROOT/include" -I"$ROOT/third_party/stb" -c "$ROOT/source/core/stb_image_impl.cpp" -o "$TASK_TMP/stb.o"
"$CXX_BIN" -std=c++11 ${CXXFLAGS:-} -D__3DS__ \
  -I"$ROOT/tests/cover_stubs" -I"$ROOT/tests/stubs" -I"$ROOT/include" \
  -I"$ROOT/third_party/stb" -c "$ROOT/source/shared/cover_decode_utils.cpp" -o "$TASK_TMP/cover.o"
"$CXX_BIN" -std=c++11 ${CXXFLAGS:-} -D__3DS__ \
  -I"$ROOT/third_party/mupdf/include" -I"$ROOT/tests/stubs" -I"$ROOT/include" \
  -I"$ROOT/third_party/stb" -c "$ROOT/source/formats/common/raster_image_decode.cpp" -o "$TASK_TMP/raster.o"
"$CXX_BIN" -std=c++11 ${CXXFLAGS:-} -I"$ROOT/tests/cover_stubs" \
  -I"$ROOT/third_party/mupdf/include" -I"$ROOT/tests/stubs" -I"$ROOT/include" \
  "$ROOT/tests/test_cover_mupdf_decode.cpp" "$TASK_TMP/cover.o" "$TASK_TMP/raster.o" \
  "$TASK_TMP/epub.o" "$TASK_TMP/unzip.o" "$TASK_TMP/ioapi.o" \
  -I"$MINIZIP/.." "$ROOT/source/formats/common/epub_image_utils.cpp" \
  "$ROOT/source/formats/common/zip_read_utils.cpp" \
  "$TASK_TMP/stb.o" "$ROOT/source/shared/image_scale_utils.cpp" \
  "$ROOT/source/formats/common/fixed_layout_surface.cpp" \
  "$MUPDF_HOST_LIB_DIR/libmupdf.a" "$MUPDF_HOST_LIB_DIR/libmupdf-third.a" \
  ${LDFLAGS:-} "$GC_FLAG" $PNG_LIBS -lm -o "$TASK_TMP/test"
"$TASK_TMP/test" "$ROOT/tests/fixtures/large-green-cover.jpg" "$TASK_TMP/covers.epub"

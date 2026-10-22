set -eu

ROOT="$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)"
OUTDIR="${TMPDIR:-/tmp}/3dslibris-tests"
WORKDIR="$OUTDIR/cbz-decode"
mkdir -p "$OUTDIR" "$WORKDIR"

"${CXX:-c++}" -std=c++11 \
  ${CXXFLAGS:-} \
  "$ROOT/tests/test_cbz_decode.cpp" \
  "$ROOT/source/formats/cbz/cbz_decode.cpp" \
  "$ROOT/source/core/stb_image_impl.cpp" \
  "$ROOT/source/formats/common/pdf_view_utils.cpp" \
  -I"$ROOT/include" \
  -I"$ROOT/third_party/stb" \
  ${LDFLAGS:-} \
  -o "$OUTDIR/test_cbz_decode"

PNG_PATH="$WORKDIR/sample.png"
JPG_PATH="$WORKDIR/sample.jpg"
python3 - "$PNG_PATH" "$JPG_PATH" <<'PY'
import base64
import struct
import sys
import zlib


def png_chunk(tag: bytes, data: bytes) -> bytes:
    return (struct.pack(">I", len(data)) + tag + data +
            struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF))


width = 8
height = 8
rows = []
for y in range(height):
    row = bytearray([0])
    for x in range(width):
        row.extend(((x * 32) & 0xFF, (y * 32) & 0xFF, 160))
    rows.append(bytes(row))

ihdr = struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)
idat = zlib.compress(b"".join(rows), 9)
png = (
    b"\x89PNG\r\n\x1a\n" +
    png_chunk(b"IHDR", ihdr) +
    png_chunk(b"IDAT", idat) +
    png_chunk(b"IEND", b"")
)

with open(sys.argv[1], "wb") as f:
    f.write(png)
# Fixed 8x8 JPEG of the same RGB gradient, independent of image converters.
with open(sys.argv[2], "wb") as f:
    f.write(base64.b64decode("/9j/4AAQSkZJRgABAQAASABIAAD/4QBMRXhpZgAATU0AKgAAAAgAAYdpAAQAAAABAAAAGgAAAAAAA6ABAAMAAAABAAEAAKACAAQAAAABAAAACKADAAQAAAABAAAACAAAAAD/7QA4UGhvdG9zaG9wIDMuMAA4QklNBAQAAAAAAAA4QklNBCUAAAAAABDUHYzZjwCyBOmACZjs+EJ+/8AAEQgACAAIAwEiAAIRAQMRAf/EAB8AAAEFAQEBAQEBAAAAAAAAAAABAgMEBQYHCAkKC//EALUQAAIBAwMCBAMFBQQEAAABfQECAwAEEQUSITFBBhNRYQcicRQygZGhCCNCscEVUtHwJDNicoIJChYXGBkaJSYnKCkqNDU2Nzg5OkNERUZHSElKU1RVVldYWVpjZGVmZ2hpanN0dXZ3eHl6g4SFhoeIiYqSk5SVlpeYmZqio6Slpqeoqaqys7S1tre4ubrCw8TFxsfIycrS09TV1tfY2drh4uPk5ebn6Onq8fLz9PX29/j5+v/EAB8BAAMBAQEBAQEBAQEAAAAAAAABAgMEBQYHCAkKC//EALURAAIBAgQEAwQHBQQEAAECdwABAgMRBAUhMQYSQVEHYXETIjKBCBRCkaGxwQkjM1LwFWJy0QoWJDThJfEXGBkaJicoKSo1Njc4OTpDREVGR0hJSlNUVVZXWFlaY2RlZmdoaWpzdHV2d3h5eoKDhIWGh4iJipKTlJWWl5iZmqKjpKWmp6ipqrKztLW2t7i5usLDxMXGx8jJytLT1NXW19jZ2uLj5OXm5+jp6vLz9PX29/j5+v/bAEMAAgICAgICAwICAwUDAwMFBgUFBQUGCAYGBgYGCAoICAgICAgKCgoKCgoKCgwMDAwMDA4ODg4ODw8PDw8PDw8PD//bAEMBAgICBAQEBwQEBxALCQsQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEP/dAAQAAf/aAAwDAQACEQMRAD8A5X4Ufsqf6n/Q/T+Gvob/AIZU/wCnP/x2voH4Uf8ALH8K+h6OKPE3OPrkv3v9fePwK8Ys+/1bofvvz7LzP//Z"))
PY

"$OUTDIR/test_cbz_decode" "$PNG_PATH" "$JPG_PATH"

# Optional integration of the actual production MuPDF branch on the host.
# Provide native libraries built from this repository's vendored version.
if [ -n "${MUPDF_HOST_LIB_DIR:-}" ]; then
  case "$(uname -s)" in
    Darwin) MUPDF_GC_FLAG="-Wl,-dead_strip" ;;
    *) MUPDF_GC_FLAG="-Wl,--gc-sections" ;;
  esac
  "${CXX:-c++}" -std=c++11 ${CXXFLAGS:-} -fno-exceptions -fno-rtti \
    -D__3DS__ -I"$ROOT/third_party/mupdf/include" \
    -I"$ROOT/tests/stubs" -I"$ROOT/include" -I"$ROOT/third_party/stb" \
    -Dfz_new_buffer_from_copied_data=TestMuPdfCopiedBuffer \
    -Dfz_new_buffer_from_shared_data=TestMuPdfSharedBuffer \
    -c "$ROOT/source/formats/cbz/cbz_decode.cpp" -o "$WORKDIR/cbz_decode_mupdf.o"
  "${CXX:-c++}" -std=c++11 ${CXXFLAGS:-} -fno-exceptions -fno-rtti \
    -I"$ROOT/tests" -I"$ROOT/include" -I"$ROOT/third_party/mupdf/include" \
    "$ROOT/tests/test_cbz_mupdf_decode.cpp" "$WORKDIR/cbz_decode_mupdf.o" \
    "$ROOT/source/formats/common/pdf_view_utils.cpp" \
    "$MUPDF_HOST_LIB_DIR/libmupdf.a" "$MUPDF_HOST_LIB_DIR/libmupdf-third.a" \
    ${LDFLAGS:-} "$MUPDF_GC_FLAG" -lm -o "$OUTDIR/test_cbz_mupdf_decode"
  "$OUTDIR/test_cbz_mupdf_decode" "$PNG_PATH" "$JPG_PATH"
fi

set -eu

ROOT="$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)"
OUTDIR="${TMPDIR:-/tmp}/3dslibris-tests"
WORKDIR="$OUTDIR/cbz-decode"
mkdir -p "$OUTDIR" "$WORKDIR"

c++ -std=c++11 \
  "$ROOT/tests/test_cbz_decode.cpp" \
  "$ROOT/source/formats/cbz/cbz_decode.cpp" \
  "$ROOT/source/core/stb_image_impl.cpp" \
  "$ROOT/source/shared/pdf_view_utils.cpp" \
  -I"$ROOT/include" \
  -I"$ROOT/third_party/stb" \
  -o "$OUTDIR/test_cbz_decode"

PNG_PATH="$WORKDIR/sample.png"
JPG_PATH="$WORKDIR/sample.jpg"
python3 - "$PNG_PATH" <<'PY'
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
PY

sips -s format jpeg "$PNG_PATH" --out "$JPG_PATH" >/dev/null

"$OUTDIR/test_cbz_decode" "$PNG_PATH" "$JPG_PATH"

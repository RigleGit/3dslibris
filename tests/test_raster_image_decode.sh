#!/bin/bash
set -eu
ROOT="$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)"
OUTDIR="${TMPDIR:-/tmp}/3dslibris-tests/raster-image"
mkdir -p "$OUTDIR"
"${CXX:-c++}" -std=c++11 ${CXXFLAGS:-} \
  "$ROOT/tests/test_raster_image_decode.cpp" \
  "$ROOT/source/formats/common/raster_image_decode.cpp" \
  "$ROOT/source/formats/common/pdf_view_utils.cpp" \
  -I"$ROOT/include" -I"$ROOT/third_party/stb" ${LDFLAGS:-} -o "$OUTDIR/test"
python3 - "$OUTDIR" <<'PY'
import struct, sys, zlib, base64
from pathlib import Path
out = Path(sys.argv[1])
def chunk(tag, data):
    return struct.pack('>I', len(data)) + tag + data + struct.pack('>I', zlib.crc32(tag+data)&0xffffffff)
def png(w,h,rows):
    return b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR',struct.pack('>IIBBBBB',w,h,8,6,0,0,0))+chunk(b'IDAT',zlib.compress(rows))+chunk(b'IEND',b'')
(out/'alpha.png').write_bytes(png(2,1,bytes([0,0,0,0,128,0,0,255,0])))
(out/'huge.png').write_bytes(png(3000,3000,bytes(3000*(1+3000*4))))
frame = b'\x2c\x00\x00\x00\x00\x01\x00\x01\x00\x00\x02\x02'
(out/'first.gif').write_bytes(b'GIF89a\x01\x00\x01\x00\x80\x00\x00' +
    b'\xff\x00\x00\x00\x00\xff' + frame + b'\x44\x01\x00' + frame + b'\x4c\x01\x00\x3b')
PY
"$OUTDIR/test" "$OUTDIR/alpha.png" "$OUTDIR/huge.png" "$OUTDIR/first.gif"

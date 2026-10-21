#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
source "$ROOT/tests/test_build.sh"
TASK_TMP="$(mktemp -d)"
trap 'rm -rf "$TASK_TMP"' EXIT
mkdir -p "$TASK_TMP/fallback" "$TASK_TMP/loaded/gfx/ui_icons"
# Owned 8x8 opaque icon; the same clipping contract covers external PNGs and
# procedural fallbacks. Isolated directories prevent installed assets leaking in.
python3 - "$TASK_TMP/loaded/gfx/ui_icons/gear.png" <<'PY'
import struct,sys,zlib
from pathlib import Path
def chunk(tag,data):
    return struct.pack('>I',len(data))+tag+data+struct.pack('>I',zlib.crc32(tag+data))
raw=(b'\0'+b'\x00\x00\x00\xff'*8)*8
png=b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR',struct.pack('>IIBBBBB',8,8,8,6,0,0,0))
png+=chunk(b'IDAT',zlib.compress(raw))+chunk(b'IEND',b'')
Path(sys.argv[1]).write_bytes(png)
PY
cd "$TASK_TMP/fallback"
build_test test_ui_button_skin -- -I"$ROOT/tests/stubs/image_viewer" -I"$ROOT/tests/stubs" -I"$ROOT/include" -I"$ROOT/third_party/stb" -I"$ROOT/third_party/utf8proc" -I"$ROOT/third_party/libunibreak/src" \
 "$ROOT/tests/test_ui_button_skin.cpp" "$ROOT/source/ui/ui_button_skin.cpp" \
 "$ROOT/source/ui/button.cpp" "$ROOT/source/core/stb_image_impl.cpp" \
 "$ROOT/source/shared/text_unicode_utils.cpp" "$ROOT/source/shared/text_layout_utils.cpp" \
 "$ROOT/source/shared/text_bidi_utils.cpp" "$ROOT/source/shared/text_arabic_shaping.cpp" \
 "$ROOT/tests/stubs/mupdf_bidi_stub.cpp"
cd "$TASK_TMP/loaded"
"$TEST_OUTDIR/test_ui_button_skin"

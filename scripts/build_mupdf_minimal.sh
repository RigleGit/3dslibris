#!/bin/sh
# SPDX-License-Identifier: AGPL-3.0-or-later

set -eu

ROOT="$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)"
MUPDF_DIR="$ROOT/third_party/mupdf"
OUT_DIR="$MUPDF_DIR/build/3ds-minimal"
MAKE_BIN="${MAKE:-make}"

cd "$MUPDF_DIR"

"$MAKE_BIN" build=release OUT=build/3ds-minimal minimal3ds=yes shared=no threading=no HAVE_PTHREAD=no HAVE_GLUT=no HAVE_OBJCOPY=no html=no xps=no svg=no mujs=no brotli=no extract=no barcode=no tofu=yes clean >/dev/null 2>&1 || true
"$MAKE_BIN" build=release OUT=build/3ds-minimal minimal3ds=yes libs shared=no threading=no HAVE_PTHREAD=no HAVE_GLUT=no HAVE_OBJCOPY=no html=no xps=no svg=no mujs=no brotli=no extract=no barcode=no tofu=yes \
  CC=/opt/devkitpro/devkitARM/bin/arm-none-eabi-gcc \
  CXX=/opt/devkitpro/devkitARM/bin/arm-none-eabi-g++ \
  AR=/opt/devkitpro/devkitARM/bin/arm-none-eabi-ar \
  RANLIB=/opt/devkitpro/devkitARM/bin/arm-none-eabi-ranlib \
  XCFLAGS="-D__3DS__ -march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft -O2 -ffunction-sections -fdata-sections -DFZ_ENABLE_ICC=0 -DFZ_ENABLE_JPX=0" \
  XLIBS="-lm"

test -f "$OUT_DIR/libmupdf.a"
test -f "$OUT_DIR/libmupdf-third.a"

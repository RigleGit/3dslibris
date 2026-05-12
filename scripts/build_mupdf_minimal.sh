#!/bin/sh
# SPDX-License-Identifier: AGPL-3.0-or-later

set -eu

ROOT="$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)"
MUPDF_DIR="$ROOT/third_party/mupdf"
OUT_DIR="$MUPDF_DIR/build/3ds-minimal"
MAKE_BIN="${MAKE:-make}"
: "${DEVKITARM:=/opt/devkitpro/devkitARM}"

CC_BIN="$DEVKITARM/bin/arm-none-eabi-gcc"
CXX_BIN="$DEVKITARM/bin/arm-none-eabi-g++"
AR_BIN="$DEVKITARM/bin/arm-none-eabi-ar"
RANLIB_BIN="$DEVKITARM/bin/arm-none-eabi-ranlib"

cd "$MUPDF_DIR"

"$MAKE_BIN" \
  build=release \
  OUT=build/3ds-minimal \
  minimal3ds=yes \
  shared=no \
  threading=no \
  HAVE_PTHREAD=no \
  HAVE_GLUT=no \
  HAVE_OBJCOPY=no \
  html=no \
  xps=yes \
  svg=no \
  mujs=no \
  brotli=no \
  extract=no \
  barcode=no \
  tofu=yes \
  tofu_cjk=yes \
  clean >/dev/null 2>&1 || true

"$MAKE_BIN" \
  build=release \
  OUT=build/3ds-minimal \
  minimal3ds=yes \
  libs \
  shared=no \
  threading=no \
  HAVE_PTHREAD=no \
  HAVE_GLUT=no \
  HAVE_OBJCOPY=no \
  html=no \
  xps=yes \
  svg=no \
  mujs=no \
  brotli=no \
  extract=no \
  barcode=no \
  tofu=yes \
  tofu_cjk=yes \
  CC="$CC_BIN" \
  CXX="$CXX_BIN" \
  AR="$AR_BIN" \
  RANLIB="$RANLIB_BIN" \
  XCFLAGS="-D__3DS__ -march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft -mword-relocations -O2 -ffunction-sections -fdata-sections -DFZ_ENABLE_ICC=0 -DFZ_ENABLE_JPX=0 -w" \
  XLIBS="-lm"

test -f "$OUT_DIR/libmupdf.a"
test -f "$OUT_DIR/libmupdf-third.a"
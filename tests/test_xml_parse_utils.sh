#!/bin/sh
set -eu

ROOT="$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)"
OUTDIR="${TMPDIR:-/tmp}/3dslibris-tests"
mkdir -p "$OUTDIR"

DTD="${EXPAT_ENABLE_DTD:-1}"
NS="${EXPAT_ENABLE_NS:-1}"
CONTEXT="${EXPAT_CONTEXT_BYTES:-1024}"

CCFLAGS="-DXML_STATIC -DHAVE_MEMMOVE -DXML_POOR_ENTROPY -I$ROOT/include -I$ROOT/third_party/minizip -I$ROOT/third_party/minizip/include -I$ROOT/source/expat"
CCFLAGS="$CCFLAGS -DDSLIBRIS_EXPAT_ENABLE_DTD=$DTD -DDSLIBRIS_EXPAT_ENABLE_NS=$NS -DDSLIBRIS_EXPAT_CONTEXT_BYTES=$CONTEXT"

cc -c $CCFLAGS "$ROOT/third_party/minizip/source/ioapi.c" -o "$OUTDIR/ioapi.o"
cc -c $CCFLAGS "$ROOT/third_party/minizip/source/unzip.c" -o "$OUTDIR/unzip.o"
cc -c $CCFLAGS "$ROOT/source/expat/loadlibrary.c" -o "$OUTDIR/loadlibrary.o"
cc -c $CCFLAGS "$ROOT/source/expat/xmlparse.c" -o "$OUTDIR/xmlparse.o"
cc -c $CCFLAGS "$ROOT/source/expat/xmlrole.c" -o "$OUTDIR/xmlrole.o"
cc -c $CCFLAGS "$ROOT/source/expat/xmltok.c" -o "$OUTDIR/xmltok.o"
cc -c $CCFLAGS "$ROOT/source/expat/xmltok_impl.c" -o "$OUTDIR/xmltok_impl.o"
cc -c $CCFLAGS "$ROOT/source/expat/xmltok_ns.c" -o "$OUTDIR/xmltok_ns.o"

c++ -std=c++11 \
  "$ROOT/tests/test_xml_parse_utils.cpp" \
  "$ROOT/source/formats/common/xml_parse_utils.cpp" \
  "$OUTDIR/ioapi.o" \
  "$OUTDIR/unzip.o" \
  "$OUTDIR/loadlibrary.o" \
  "$OUTDIR/xmlparse.o" \
  "$OUTDIR/xmlrole.o" \
  "$OUTDIR/xmltok.o" \
  "$OUTDIR/xmltok_impl.o" \
  "$OUTDIR/xmltok_ns.o" \
  -I"$ROOT/include" \
  -I"$ROOT/third_party/minizip" \
  -I"$ROOT/third_party/minizip/include" \
  -I"$ROOT/source/expat" \
  -lz \
  -o "$OUTDIR/test_xml_parse_utils"

"$OUTDIR/test_xml_parse_utils"

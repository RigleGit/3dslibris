#!/usr/bin/env bash
set -euo pipefail
source "$(dirname "$0")/test_build.sh"

WORKDIR="$TEST_OUTDIR/cbz-archive"
mkdir -p "$WORKDIR"
export TEST_CBZ_ARCHIVE_PATH="$WORKDIR/sample.cbz"
export TEST_CBZ_INVALID_PATH="$WORKDIR/not-a-zip.cbz"

python3 - "$TEST_CBZ_ARCHIVE_PATH" "$TEST_CBZ_INVALID_PATH" <<'PYFIXTURE'
import sys
import zipfile

# Deliberately shuffled ZIP order; payloads identify the entry read by offset.
names = [
    "Chapter 10/1.png", "Chapter 2/10.png", "Chapter 5/1.png",
    "Chapter 02/1.png", "Chapter 2/02.png", "Chapter 2/2.png",
    "./002-page.PNG", "001-cover.jpg", "folder/../010-last.JPEG",
    "nested\\003-middle.jpg", "Volume 2/1.png", "Volume 1/Chapter 10/1.png",
    "Volume 1/Chapter 2/1.png", "number1000000000000000000000.png",
    "number999999999999999999999.png", "number10.png", "number9.png",
    "zero00.png", "zero0.png",
]
with zipfile.ZipFile(sys.argv[1], "w", compression=zipfile.ZIP_DEFLATED) as zf:
    for name in names:
        normalized = name.replace("\\", "/")
        while normalized.startswith("./"):
            normalized = normalized[2:]
        zf.writestr(name, normalized.encode("ascii"))
    zf.writestr("notes.txt", b"not a page")
    zf.writestr("__MACOSX/", b"")
    zf.writestr("ComicInfo.xml", b'<ComicInfo><Page Image="3" Bookmark="Outside pages"/><Pages>'
                b'<Page Image="-1" Bookmark="Invalid index"/>'
                b'<Page Image="4" Bookmark=""/>'
                b'<Page Bookmark="Missing index"/>'
                b'<Page Image="6" Bookmark="Chapter five"/>'
                b'<Page Image="0" Bookmark="Cover"/>'
                b'</Pages></ComicInfo>')
with open(sys.argv[2], 'wb') as invalid:
    invalid.write(b'not a ZIP archive')
PYFIXTURE

# Build the vendored minizip for the host instead of silently skipping the
# regression when only the cross-compiled 3DS library is installed.
MINIZIP="$TEST_ROOT/third_party/mupdf/thirdparty/zlib/contrib/minizip"
for name in unzip ioapi; do
  "$CC_BIN" ${CFLAGS:-} -c "$MINIZIP/$name.c" -o "$WORKDIR/$name.o"
done

build_test test_cbz_archive \
  "$TEST_ROOT/tests/test_cbz_archive.cpp" \
  "$TEST_ROOT/source/formats/cbz/cbz_archive.cpp" \
  "$TEST_ROOT/source/formats/common/zip_read_utils.cpp" \
  "$TEST_ROOT/source/formats/common/xml_parse_utils.cpp" \
  "$WORKDIR/unzip.o" "$WORKDIR/ioapi.o" \
  --expat -lz

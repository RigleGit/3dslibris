#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
source "$ROOT/tests/test_build.sh"
TASK_TMP="$(mktemp -d)"
trap 'rm -rf "$TASK_TMP"' EXIT
export FOLDER_COVER_TEST_ROOT="$TASK_TMP/library"
mkdir -p "$FOLDER_COVER_TEST_ROOT"
python3 - "$ROOT" "$TASK_TMP" <<'PYGEN'
from pathlib import Path
import subprocess,sys,zlib,struct
root,tmp=map(Path,sys.argv[1:])
(tmp/'3ds.h').write_text('#pragma once\n#include <stdint.h>\ntypedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32; typedef uint64_t u64;\n')
# Compile production owners with platform/model substitutes, preserving real
# directory I/O, cache serialization, idle scheduling and raster decoder.
for name in ('app_browser_covers','app_browser_jobs','cover_cache'):
    source=(root/'source/library'/f'{name}.cpp').read_text()
    source='\n'.join(line for line in source.splitlines() if not line.startswith('#include'))
    prefix='#include "test_folder_cover_support.h"\n'
    if name=='cover_cache': prefix+='#define ReadPathToStringLimited CountedReadPathToStringLimited\n'
    (tmp/f'{name}.cpp').write_text(prefix+source)
source=(root/'source/shared/cover_decode_utils.cpp').read_text()
source='\n'.join(line for line in source.splitlines() if not line.startswith('#include'))
(tmp/'decode.cpp').write_text('#include "test_folder_cover_support.h"\n#define LoadFromMemory CountedLoadFromMemory\n'+source)
extracted='#include "test_folder_cover_support.h"\n#define ReadPathToStringLimited CountedReadPathToStringLimited\n'
path=root/'source/library/library_controller.cpp'
for signature in ('static bool PathIsDirectory(', 'static bool PathIsRegularFile(', 'static bool HasBrowserEntryKey(', 'static bool DirectoryHasDirectSupportedBook(', 'static void TryLoadFolderCover(', 'static void AppendFolderEntry('):
    if signature in path.read_text():
        extracted+=subprocess.check_output(['python3',str(root/'tests/extract_test_function.py'),str(path),signature],text=True)
extracted+='\nvoid AppendTestFolder(App *app,const std::string &root,const std::string &name,std::set<std::string> *seen) { AppendFolderEntry(app,root,name,seen); }\n'
(tmp/'folders.cpp').write_text(extracted)
def chunk(name,data): return struct.pack('>I',len(data))+name+data+struct.pack('>I',zlib.crc32(name+data))
png=b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR',struct.pack('>IIBBBBB',85,115,8,2,0,0,0))+chunk(b'IDAT',zlib.compress((b'\x00'+b'\xaa\x55\x22'*85)*115))+chunk(b'IEND',b'')
(tmp/'cover.png').write_bytes(png)
PYGEN
export FOLDER_COVER_PNG="$TASK_TMP/cover.png"
build_test test_folder_cover_jobs -- -I"$TASK_TMP" -I"$ROOT/tests" -I"$ROOT/third_party/stb" \
  "$ROOT/tests/test_folder_cover_jobs.cpp" "$TASK_TMP/folders.cpp" \
  "$TASK_TMP/app_browser_covers.cpp" "$TASK_TMP/app_browser_jobs.cpp" \
  "$TASK_TMP/cover_cache.cpp" "$TASK_TMP/decode.cpp" \
  "$ROOT/source/core/stb_image_impl.cpp" "$ROOT/source/shared/image_scale_utils.cpp" \
  "$ROOT/source/formats/common/file_read_utils.cpp" "$ROOT/source/library/cover_override_utils.cpp" \
  "$ROOT/source/library/browser_view_utils.cpp"

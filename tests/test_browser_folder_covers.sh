#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
source "$ROOT/tests/test_build.sh"
TASK_TMP="$(mktemp -d)"
trap 'rm -rf "$TASK_TMP"' EXIT
export TEST_FOLDER_COVERS_DIR="$TASK_TMP/library"
python3 - "$TEST_FOLDER_COVERS_DIR" <<'PY'
import pathlib, struct, sys, zlib
root = pathlib.Path(sys.argv[1])
root.mkdir()
def png(rgb):
    def chunk(kind, data):
        return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data) & 0xffffffff)
    return (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', 2, 2, 8, 2, 0, 0, 0)) +
            chunk(b'IDAT', zlib.compress((b'\0' + bytes(rgb) * 2) * 2)) + chunk(b'IEND', b''))
for index in range(128):
    name = 'Series.%03d' % index
    (root / name).mkdir()
    (root / name / 'book.txt').write_text('A real directory entry.')
    (root / (name + '.png')).write_bytes(png((255, 0, 0)))
(root / 'blue.png').write_bytes(png((0, 0, 255)))
(root / 'empty').mkdir()
(root / 'unsupported').mkdir()
(root / 'unsupported' / 'notes.bin').write_bytes(b'not a book')
PY
extract() {
  python3 "$ROOT/tests/extract_test_function.py" "$ROOT/$1" "$2" >> "$TASK_TMP/$3.inc"
}
for signature in 'static bool PathIsDirectory(' 'static bool PathIsRegularFile(' 'static bool HasBrowserEntryKey(' 'static bool DirectoryHasDirectSupportedBook('; do
  extract source/library/library_controller.cpp "$signature" folder_index
done
extract source/library/library_controller.cpp 'static void AppendFolderEntry(' folder_index
extract source/book/book.cpp 'void Book::SetBrowserFolderEntry(' folder_book
for signature in 'static BrowserViewMode CurrentBrowserViewMode(' 'static int CurrentBrowserPageSize(' 'static bool ShouldCurrentBrowserLoadCovers(' 'static bool IsVisibleFolderCover(' 'static bool SupportsBrowserCoverWarmup(' 'static void ApplyEmbeddedCoverResult(' 'static size_t CountQueuedHeavyJobs(' 'static std::string BuildBookPath('; do
  extract source/library/app_browser_jobs.cpp "$signature" folder_jobs
done
for signature in 'bool LibraryController::HasQueuedJob(' 'void LibraryController::PrioritizeSelectedBookJobs(' 'void LibraryController::EnqueueJob(' 'void LibraryController::QueueBookWarmup(' 'void LibraryController::TickBrowserWarmup(' 'void LibraryController::ProcessJobs(' 'size_t LibraryController::PauseBrowserJobs('; do
  extract source/library/app_browser_jobs.cpp "$signature" folder_jobs
done
for signature in 'void LibraryController::UnloadNonVisibleBrowserCoverCaches(' 'void LibraryController::LoadVisibleBrowserCoverCaches('; do
  extract source/library/app_browser_covers.cpp "$signature" folder_covers
done
extract source/library/cover_cache.cpp 'bool TryLoadAdjacentOverride(' folder_sidecar
extract source/shared/cover_decode_utils.cpp 'bool DecodeImageToCoverThumb(' folder_decode
build_test test_browser_folder_covers -- -I"$TASK_TMP" -I"$ROOT/tests/stubs" \
  -I"$ROOT/include" -I"$ROOT/third_party/stb" \
  "$ROOT/tests/test_browser_folder_covers.cpp" \
  "$ROOT/source/library/browser_view_utils.cpp" \
  "$ROOT/source/library/cover_override_utils.cpp" \
  "$ROOT/source/formats/common/file_read_utils.cpp" \
  "$ROOT/source/shared/app_flow_utils.cpp" \
  "$ROOT/source/shared/image_scale_utils.cpp" \
  "$ROOT/source/core/stb_image_impl.cpp"

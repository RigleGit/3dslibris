#include "formats/cbz/cbz_decode.h"
#include "formats/cbz/cbz_state.h"
#include "formats/common/format_limits.h"
#include "formats/common/pdf_view_utils.h"
#include "formats/common/fixed_layout_preview_constants.h"
#include "shared/fixed_layout_perf.h"
#include <algorithm>
#include <cassert>
#include <string>
#include <cstdio>
namespace debug_runtime { static bool sync = true; bool ForceSynchronousCbzDecode() { return sync; } }
struct FixtureState : Book::CbzState {
  FixtureState() { entries.resize(2); viewport.zoom_index=3; }
};
static int reads = 0, decodes = 0, max_decode_zoom = 99;
static bool fail_read=false, fail_scale=false;
static std::vector<int> attempted_zooms;
CbzArchiveReader::CbzArchiveReader() : archive_(nullptr) {}
CbzArchiveReader::~CbzArchiveReader() {}
bool CbzArchiveReader::Read(const std::string &, const CbzPageEntry &, std::vector<unsigned char> *b, size_t) {
  ++reads; if (fail_read) return false; b->assign(10, 1); return true;
}
const char *GetLastCbzArchiveError() { return "read failed"; }
const char *GetLastCbzDecodeError() { return "decode failed"; }
bool DecodeCbzPageImage(const std::vector<unsigned char> &, int z, int, int, CbzDecodedPage *d) {
  ++decodes; attempted_zooms.push_back(z);
  if (z > max_decode_zoom) return false;
  d->original_width = 600; d->original_height = 900;
  d->source_bitmap.width = z < 4 ? 400 : 600;
  d->source_bitmap.height = z < 4 ? 600 : 900;
  d->source_bitmap.pixels.assign(d->source_bitmap.width*d->source_bitmap.height, 42);
  return true;
}
bool ScaleCbzBitmap(const CbzBitmap &, int w, int h, bool, CbzBitmap *o) {
  if (fail_scale) return false;
  o->width=w; o->height=h; o->pixels.assign(w*h,42); return true;
}
#include "cbz_source_under_test.inc"
int main() {
  FixtureState s;
  assert(EnsureCbzPreviewCache(&s, 0));
  assert(EnsureCbzInteractiveCache(&s, 0));
  assert(reads == 1 && decodes == 1 && "synchronous page must read and decode once");
  assert(s.current_interactive.pixels.empty() &&
         "synchronous view must not duplicate a sufficient decoded source");
  assert(s.current_source.zoom_index == 4 &&
         "synchronous CBZ pages need a readable source at low zoom");
  assert(EnsureCbzInteractiveCache(&s, 0) && reads == 1);
  s.viewport.zoom_index = 2;
  assert(EnsureCbzInteractiveCache(&s, 0) && reads == 1);
  s.viewport.zoom_index = 4;
  assert(EnsureCbzInteractiveCache(&s, 0) && reads == 1);
  assert(EnsureCbzPreviewCache(&s, 1));
  assert(EnsureCbzInteractiveCache(&s, 1) && reads == 2);
  s.viewport.zoom_index = 6;
  assert(EnsureCbzInteractiveCache(&s, 1) && reads == 2 && "full-resolution source must survive a zoom increase");
  FixtureState bucket;
  bucket.viewport.zoom_index = 2;
  assert(EnsureCbzPreviewCache(&bucket, 0));
  assert(bucket.current_source.zoom_index == 4);
  const int bucket_reads = reads;
  bucket.viewport.zoom_index = 3;
  assert(EnsureCbzInteractiveCache(&bucket, 0) && reads == bucket_reads && "decoded dimensions already cover this zoom");
  // A wide source is insufficient if its height does not cover the new zoom.
  bucket.current_source.zoom_index = 2;
  bucket.current_source.bitmap.height = 100;
  bucket.current_interactive.page = -1;
  assert(EnsureCbzInteractiveCache(&bucket, 0) && reads == bucket_reads + 1);
  // The deferred path still decodes a cheap preview, not full zoom.
  debug_runtime::sync = false;
  FixtureState deferred;
  assert(EnsureCbzPreviewCache(&deferred, 0));
  assert(deferred.current_source.zoom_index == 0);
  // A failed high-resolution decode must retain the low-resolution fallback.
  debug_runtime::sync = true; max_decode_zoom = 0;
  FixtureState fallback;
  assert(EnsureCbzPreviewCache(&fallback, 0));
  assert(fallback.current_source.zoom_index == 0);
  // A failure on another page must not discard the usable page/source.
  max_decode_zoom=99;
  FixtureState errors;
  assert(EnsureCbzPreviewCache(&errors, 0));
  const u16 *source=errors.current_source.bitmap.pixels.data();
  fail_read=true; const int before_read=reads;
  assert(!EnsureCbzPreviewCache(&errors, 1));
  assert(reads == before_read + 1 && errors.failed_page == 1 && errors.last_error == "read failed");
  assert(errors.current_source.page == 0 && errors.current_source.bitmap.pixels.data() == source);
  assert(errors.current_preview.page == 0 && !errors.current_preview.pixels.empty());
  assert(!EnsureCbzPreviewCache(&errors, 1) && reads == before_read + 1);
  fail_read=false; errors.failed_page=-1; max_decode_zoom=-1;
  attempted_zooms.clear();
  assert(!EnsureCbzInteractiveCache(&errors, 1));
  assert((attempted_zooms == std::vector<int>{4, 3, 2, 1, 0}));
  assert(errors.last_error == "decode failed" && errors.current_source.page == 0);
  assert(errors.current_source.bitmap.pixels.data() == source);
  max_decode_zoom=99; errors.failed_page=-1; fail_scale=true;
  assert(!EnsureCbzPreviewCache(&errors, 1));
  assert(errors.current_source.page == 1 && errors.current_preview.page == 0);
  const int loaded_reads=reads;
  fail_scale=false;
  assert(EnsureCbzPreviewCache(&errors, 1) && reads == loaded_reads);
  assert(errors.last_error.empty() && errors.failed_page == -1);
  const int before_invalid=reads;
  assert(!EnsureCbzSourceLoaded(&errors, -1, 0));
  assert(!EnsureCbzSourceLoaded(&errors, 2, 0));
  assert(reads == before_invalid);
  puts("PASS: CBZ source reuse, bounded fallback, and cache preservation on failures");
}

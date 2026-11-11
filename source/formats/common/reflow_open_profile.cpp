#include "formats/common/reflow_open_profile.h"
#include <cstdio>

namespace reflow_open_profile {
namespace {
const unsigned long long kMiB = 1024ULL * 1024ULL;
// Page tokens are u32 per source character plus per-page bookkeeping.
const unsigned kTokenBytes = 4;
// Bytes of codec/staging/index scratch kept by the cache writer.
const unsigned long long kWriterScratch = 512ULL * 1024ULL;
// Fast needs this many times the estimate free: allocator fragmentation and
// string growth by doubling make a tight fit unsafe.
const unsigned kMarginNum = 3, kMarginDen = 2;
const unsigned long long kFloorNew = 8 * kMiB, kFloorOld = 6 * kMiB;
// Largest source for which the up-front estimate is trusted.
const unsigned long long kMaxFast = 24 * kMiB;
} // namespace

unsigned long long EstimateFastPeak(const Inputs &in) {
  const unsigned long long file = in.file_size;
  // MOBI: text_len is the uncompressed text; otherwise assume the file size
  // (FB2/plain XML expands to roughly its own size in text).
  const unsigned long long text = in.text_len ? in.text_len : file;
  const unsigned long long merged = in.format == Mobi ? text : 0;
  const unsigned long long utf8 = text + text / 4; // legacy -> UTF-8 growth
  // The three stages that overlap, largest wins:
  //   decode:  raw + merged + utf8
  //   markup:  raw + utf8 + extracted text
  //   paging:  extracted text + page tokens + writer scratch
  const unsigned long long decode = file + merged + utf8;
  const unsigned long long markup = file + utf8 + text;
  const unsigned long long paging =
      text + text * kTokenBytes + text / 16 + kWriterScratch;
  unsigned long long peak = decode > markup ? decode : markup;
  if (paging > peak)
    peak = paging;
  if (in.format == Fb2) {
    // Expat feeds 64 KiB windows: the file is not held whole.
    const unsigned long long fb2 = text * kTokenBytes + text / 16 + kWriterScratch;
    peak = fb2;
  }
  return peak;
}

unsigned long long RequiredFor(const Inputs &in) {
  return EstimateFastPeak(in) * kMarginNum / kMarginDen +
         (in.new3ds ? kFloorNew : kFloorOld);
}

Profile SafeProfile() {
  Profile p;
  p.tier = Safe;
  return p;
}

Profile Choose(const Inputs &in) {
  Profile p = SafeProfile();
  p.estimated_peak = EstimateFastPeak(in);
  p.required = RequiredFor(in);
  if (in.format == Epub)
    return p; // EPUB keeps its own pipeline; only the tier is reported
  if (!in.file_size || in.file_size > kMaxFast || !in.free_bytes ||
      in.free_bytes < p.required)
    return p;
  p.tier = Fast;
  p.read_chunk = 256 * 1024;
  p.reserve_raw = true;
  p.page_budget_ms = 25;
  p.page_budget_pages = 0;
  p.flush_words = 131072;
  p.writer_budget_ms = 25;
  // One slice may overrun the budget by its own length (25 ms), keeping a
  // frame near 70 ms before the splash redraw.
  p.frame_budget_ms = 45;
  return p;
}

const char *TierName(Tier tier) { return tier == Fast ? "fast" : "safe"; }

size_t Describe(char *out, size_t size, const Inputs &in, const Profile &p) {
  if (!out || !size)
    return 0;
  const int n = snprintf(
      out, size,
      "tier=%s fmt=%u dev=%s file=%llu text=%llu free=%llu est=%llu need=%llu "
      "read=%u pages=%u/%u flush=%u frame=%u comp=%u val=%u",
      TierName(p.tier), (unsigned)in.format, in.new3ds ? "new" : "old",
      in.file_size, in.text_len, in.free_bytes, p.estimated_peak, p.required,
      p.read_chunk, p.page_budget_ms, p.page_budget_pages, p.flush_words,
      p.frame_budget_ms,
      p.compress ? 1u : 0u, (unsigned)p.validation);
  return n < 0 ? 0 : (size_t)n < size ? (size_t)n : size - 1;
}
} // namespace reflow_open_profile

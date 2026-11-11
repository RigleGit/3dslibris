#include "formats/common/reflow_open_profile.h"
#include "test_assert.h"
#include <cstring>

using namespace reflow_open_profile;

static const unsigned long long MiB = 1024ULL * 1024ULL;

static Inputs MkMobi(unsigned long long file, unsigned long long text, bool nw,
                   unsigned long long free_bytes) {
  Inputs in;
  in.format = reflow_open_profile::Mobi;
  in.file_size = file;
  in.text_len = text;
  in.new3ds = nw;
  in.free_bytes = free_bytes;
  return in;
}

struct FakeHeap {
  unsigned long long limit;
  unsigned calls;
};
static bool FakeProbe(unsigned long long bytes, void *ctx) {
  FakeHeap *h = static_cast<FakeHeap *>(ctx);
  ++h->calls;
  return bytes <= h->limit;
}

int main() {
  {
    const unsigned long long cap = 64 * MiB;
    FakeHeap all = {1ULL << 40, 0};
    test::ExpectEqU64("cap fits in one probe",
                      LargestBlock(cap, MiB, FakeProbe, &all), cap);
    test::ExpectEqU("one probe when the cap fits", all.calls, 1);
    FakeHeap mid = {23 * MiB + 12345, 0};
    const unsigned long long got = LargestBlock(cap, MiB, FakeProbe, &mid);
    test::ExpectEq("largest block within one resolution step",
                   got <= mid.limit && mid.limit - got <= MiB, 1);
    test::ExpectEq("bounded probe count", mid.calls <= 8, 1);
    FakeHeap none = {0, 0};
    test::ExpectEqU64("nothing fits", LargestBlock(cap, MiB, FakeProbe, &none), 0);
    test::ExpectEqU64("no probe function", LargestBlock(cap, MiB, 0, 0), 0);
    test::ExpectEqU64("zero cap", LargestBlock(0, MiB, FakeProbe, &all), 0);
    // The probed value drives the tier: the reference book needs ~29 MB.
    const Inputs ref = MkMobi(4143096, 2671319, false, 0);
    FakeHeap old_tight = {20 * MiB, 0}, old_roomy = {40 * MiB, 0};
    Inputs a = ref, b = ref;
    a.free_bytes = LargestBlock(cap, MiB, FakeProbe, &old_tight);
    b.free_bytes = LargestBlock(cap, MiB, FakeProbe, &old_roomy);
    test::ExpectEq("20 MB block keeps the reference book safe", Choose(a).tier == Safe, 1);
    test::ExpectEq("40 MB block allows fast on old", Choose(b).tier == Fast, 1);
  }
  // A Promised Land sized book (4.1 MB file, 2.7 MB text).
  const Inputs book = MkMobi(4143096, 2671319, false, 0);
  const unsigned long long need = RequiredFor(book);
  test::ExpectEq("need is positive and above the estimate",
                 need > EstimateFastPeak(book), 1);

  // Boundary: exactly the requirement is Fast, one byte less is Safe.
  Inputs at = book;
  at.free_bytes = need;
  test::ExpectEq("exact fit is fast", Choose(at).tier == Fast, 1);
  at.free_bytes = need - 1;
  test::ExpectEq("one byte short is safe", Choose(at).tier == Safe, 1);

  // Unknown or zero free memory never selects Fast.
  at.free_bytes = 0;
  test::ExpectEq("unknown free memory is safe", Choose(at).tier == Safe, 1);

  // Plenty of memory: Fast with larger batches than Safe.
  Inputs rich = MkMobi(4143096, 2671319, true, 64 * MiB);
  const Profile fast = Choose(rich), safe = SafeProfile();
  test::ExpectEq("rich is fast", fast.tier == Fast, 1);
  test::ExpectEq("fast reads more per call", fast.read_chunk > safe.read_chunk, 1);
  test::ExpectEq("fast read stays bounded for a frame", fast.read_chunk <= 256 * 1024, 1);
  test::ExpectEq("fast has no page cap", fast.page_budget_pages == 0, 1);
  test::ExpectEq("safe keeps the 3.1 page cap", safe.page_budget_pages == 8, 1);
  test::ExpectEq("safe keeps 64 KiB reads", safe.read_chunk == 65536, 1);
  test::ExpectEq("fast flushes more per batch", fast.flush_words > safe.flush_words, 1);
  test::ExpectEq("fast frame slice stays under 100 ms",
                 fast.page_budget_ms + fast.writer_budget_ms <= 100, 1);
  test::ExpectEq("fast frame budget plus one slice stays under 100 ms",
                 fast.frame_budget_ms + fast.page_budget_ms + 20 <= 100, 1);
  test::ExpectEq("safe frame budget unchanged", safe.frame_budget_ms == 30, 1);
  test::ExpectEq("compression default stays on", fast.compress && safe.compress, 1);

  // Old needs a larger floor than New for the same book.
  Inputs oldd = book, neww = book;
  neww.new3ds = true;
  test::ExpectEq("old floor is not smaller", RequiredFor(oldd) != RequiredFor(neww), 1);
  const unsigned long long mid = RequiredFor(neww);
  oldd.free_bytes = neww.free_bytes = (RequiredFor(oldd) < mid ? RequiredFor(oldd) : mid);
  // Same free amount between the two requirements picks by device.
  const unsigned long long lo = RequiredFor(oldd) < RequiredFor(neww) ? RequiredFor(oldd) : RequiredFor(neww);
  const unsigned long long hi = RequiredFor(oldd) < RequiredFor(neww) ? RequiredFor(neww) : RequiredFor(oldd);
  test::ExpectEq("floors differ", lo < hi, 1);

  // Huge book: even with a lot of memory the source cap keeps Safe.
  Inputs huge = MkMobi(40 * MiB, 40 * MiB, true, 512 * MiB);
  test::ExpectEq("huge book is safe", Choose(huge).tier == Safe, 1);
  // 10 MB MOBI on Old with modest memory: Safe (pages alone need ~5x text).
  Inputs ten = MkMobi(10 * MiB, 9 * MiB, false, 40 * MiB);
  test::ExpectEq("10 MB on old with 40 MB free is safe", Choose(ten).tier == Safe, 1);
  // Low memory.
  test::ExpectEq("low memory is safe",
                 Choose(MkMobi(4143096, 2671319, true, 8 * MiB)).tier == Safe, 1);

  // Small book fits everywhere with modest memory.
  Inputs small = MkMobi(300 * 1024, 200 * 1024, false, 16 * MiB);
  test::ExpectEq("small book on old is fast", Choose(small).tier == Fast, 1);

  // FB2 estimate does not count the whole file as resident.
  Inputs fb2;
  fb2.format = reflow_open_profile::Fb2;
  fb2.file_size = fb2.text_len = 2 * MiB;
  Inputs mobi_same = MkMobi(2 * MiB, 2 * MiB, true, 0);
  test::ExpectEq("fb2 estimate differs from mobi",
                 EstimateFastPeak(fb2) != EstimateFastPeak(mobi_same), 1);

  // EPUB is always reported as safe (own pipeline).
  Inputs epub = MkMobi(5 * MiB, 0, true, 512 * MiB);
  epub.format = reflow_open_profile::Epub;
  test::ExpectEq("epub stays safe", Choose(epub).tier == Safe, 1);

  // Zero size book never picks Fast.
  test::ExpectEq("empty file is safe", Choose(MkMobi(0, 0, true, 512 * MiB)).tier == Safe, 1);

  // Description contains the tier and inputs on one line.
  char buf[256];
  Describe(buf, sizeof(buf), rich, fast);
  test::ExpectEq("describe tier", strstr(buf, "tier=fast") != NULL, 1);
  test::ExpectEq("describe device", strstr(buf, "dev=new") != NULL, 1);
  test::ExpectEq("describe one line", strchr(buf, '\n') == NULL, 1);
  char tiny[16];
  Describe(tiny, sizeof(tiny), rich, fast);
  test::ExpectEq("describe truncation terminates", strlen(tiny) < sizeof(tiny), 1);
  return 0;
}

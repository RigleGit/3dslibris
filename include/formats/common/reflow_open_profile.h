#pragma once
// Adaptive open strategy for the cooperative MOBI/FB2 (and EPUB reporting)
// preparation, chosen once from the book size and the memory available.
//
// Pure: callers gather the inputs (file size, header text length, device, free
// memory) and apply the returned parameters. Both tiers stay cooperative on the
// main thread; the tier only changes batch sizes and how much memory is spent to
// avoid reallocations and per-call overhead.
#include <cstddef>

namespace reflow_open_profile {
enum Tier { Safe = 0, Fast = 1, Stream = 2 };
enum Format { Epub = 0, Mobi = 1, Fb2 = 2 };
enum Validation {
  FullReread = 0,  // re-read the whole cache after publishing (current)
  IndexOnly = 1    // header, index and index CRC only; blocks already CRCed
};

struct Inputs {
  Format format = Mobi;
  unsigned long long file_size = 0;
  // MOBI record-0 text length (0 when unknown / not MOBI / not yet parsed).
  unsigned long long text_len = 0;
  bool new3ds = false;
  // Largest block malloc can still provide (LargestBlock probe, a conservative
  // stand-in for the free heap; 0 = unknown/none). Kernel region-free is not
  // usable: the heap already holds the whole application region.
  unsigned long long free_bytes = 0;
};

struct Profile {
  Tier tier = Safe;
  // Bytes per source read call; kept small enough that one SD read cannot
  // push a frame past ~100 ms on Old 3DS.
  unsigned read_chunk = 64 * 1024;
  bool reserve_raw = false;              // reserve the raw buffer up front
  unsigned page_budget_ms = 20;          // ContinueState time budget
  unsigned page_budget_pages = 8;        // ContinueState page cap (0 = none)
  unsigned flush_words = 65536;          // pages handed to the writer per batch
  unsigned writer_budget_ms = 50;        // writer slice per Step
  unsigned frame_budget_ms = 30;         // Step loop time per opening frame
  bool compress = true;                  // zlib level 1 per block
  Validation validation = FullReread;
  unsigned long long estimated_peak = 0; // Fast estimate, for the log
  unsigned long long required = 0;       // Fast estimate plus safety margin
};

// Largest block (multiple of `resolution`, at most `cap`) for which probe()
// succeeds, found by binary search. The probe must not touch the memory it
// allocates and must be monotone (a smaller request never fails when a larger
// one succeeded). Typical cost: one call when `cap` fits, else about
// log2(cap / resolution) + 1 calls.
typedef bool (*ProbeFn)(unsigned long long bytes, void *context);
unsigned long long LargestBlock(unsigned long long cap,
                                unsigned long long resolution, ProbeFn probe,
                                void *context);

// Estimated peak working set of the Fast path for these inputs.
unsigned long long EstimateFastPeak(const Inputs &in);
// Required free bytes for Fast: estimate times the safety factor plus a floor
// reserved for the rest of the application. Old 3DS keeps a larger floor.
unsigned long long RequiredFor(const Inputs &in);
// Estimated peak of the Safe path (the same stages as Fast, without the
// batching headroom), and the free bytes Safe needs: the estimate plus a small
// reserve for the rest of the application.
unsigned long long EstimateSafePeak(const Inputs &in);
unsigned long long RequiredForSafe(const Inputs &in);
Profile Choose(const Inputs &in);
// The Stream parameter set: MOBI read through a file-backed record reader, so
// the peak does not follow the size of the book.
Profile StreamProfile();
// The Safe parameter set, used when Fast must be abandoned mid-open.
Profile SafeProfile();
const char *TierName(Tier tier);
// One log line body: "tier=fast file=... text=... free=... need=... dev=old".
size_t Describe(char *out, size_t size, const Inputs &in, const Profile &p);
} // namespace reflow_open_profile

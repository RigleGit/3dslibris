// Writer batching, block fill and old tiny-block compatibility for the shared
// MOBI/FB2 block cache. Pages are deterministic and independent of the parser.
#include "book/book.h"
#include "book/book_context.h"
#include "book/page.h"
#include "formats/common/page_cache_codec.h"
#include "formats/common/reflow_append_policy.h"
#include "formats/common/reflow_block_cache.h"
#include "shared/status_reporter.h"

#include <cstdio>
#include <cstdlib>
#include <dirent.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

#ifdef DSLIBRIS_TEST_CLOCK
long long dslibris_test_clock = 0;
long long dslibris_test_clock_step = 1;
#endif

namespace mobi_page_cache {
void SavePending(Book *) {}
}

namespace {
int failures = 0;
void Check(bool ok, const char *label) {
  if (!ok) {
    fprintf(stderr, "FAIL: %s\n", label);
    ++failures;
  }
}
void Require(bool ok, const char *label) {
  if (!ok) {
    fprintf(stderr, "fixture failed: %s\n", label);
    std::exit(1);
  }
}

struct Fixture {
  BookContext ctx;
  Book book;
  std::string directory;
  std::vector<unsigned> lengths;
  unsigned long long total_words = 0;
  Fixture() : book(ctx) {
    char pattern[] = "/tmp/3dslibris-reflow-blocks-XXXXXX";
    Require(mkdtemp(pattern) != NULL, "temporary directory");
    directory = pattern;
  }
  ~Fixture() {
    book.Close();
    DIR *dir = opendir(directory.c_str());
    if (dir) {
      struct dirent *e;
      while ((e = readdir(dir)) != NULL)
        if (e->d_name[0] != '.')
          remove((directory + "/" + e->d_name).c_str());
      closedir(dir);
    }
    rmdir(directory.c_str());
  }
  // Pages of 300-900 tokens that depend on their index only.
  void AddPages(unsigned count) {
    unsigned state = 12345;
    for (unsigned i = 0; i < count; ++i) {
      state = state * 1103515245u + 12345u;
      const unsigned length = 300 + (state >> 16) % 600;
      static const char *const kWords[] = {
          "the ", "and ", "of ", "to ", "in ", "was ", "that ", "with ",
          "book ", "page ", "reader ", "chapter ", "night ", "water ",
          "light ", "house ", "walked ", "slowly ", "through ", "garden "};
      std::vector<u32> text;
      while (text.size() < length) {
        state = state * 1103515245u + 12345u;
        for (const char *w = kWords[(state >> 16) % 20]; *w; ++w)
          text.push_back((u32)*w);
      }
      text.resize(length);
      book.AppendPage()->SetBuffer(text.data(), length);
      lengths.push_back(length);
      total_words += length;
    }
  }
  std::string Path(const char *name) const { return directory + "/" + name; }
};

void Drain(reflow_block_cache::Writer &w) {
  unsigned guard = 0;
  while (w.Step() == reflow_block_cache::Pending && ++guard < 1000000) {
  }
}

// Mirrors ReflowPreparation: pages are produced a few at a time and handed
// over according to the policy. batch == 0 hands every produced batch over
// immediately (the old behavior).
reflow_block_cache::WriterStats WriteBook(Fixture &f, const std::string &path,
                                          unsigned produce_per_step,
                                          unsigned flush_words) {
  reflow_block_cache::Writer w;
  Require(w.Begin(&f.book, path, reflow_block_cache::Mobi), "begin writer");
  const unsigned total = f.book.GetPageCount();
  unsigned written = 0, produced = 0, pending_words = 0;
  while (written < total) {
    const unsigned step_end =
        produced + produce_per_step < total ? produced + produce_per_step : total;
    for (; produced < step_end; ++produced)
      pending_words += f.lengths[produced];
    const bool finished = produced == total;
    if (reflow_append_policy::ShouldAppend(produced - written, pending_words,
                                           finished, false, flush_words)) {
      Require(w.AppendDocument(written, produced), "append document");
      Drain(w);
      Require(w.GetResult() == reflow_block_cache::Ready, "document ready");
      written = produced;
      pending_words = 0;
    }
  }
  Require(w.Finish(), "finish");
  Drain(w);
  Check(w.GetResult() == reflow_block_cache::Complete, "publish complete");
  const reflow_block_cache::WriterStats stats = w.GetStats();
  w.Abort();
  return stats;
}

bool LoadMatches(Fixture &f, const std::string &path) {
  Book loaded(f.ctx);
  reflow_block_cache::Reader reader;
  if (reader.Begin(&loaded, path, 0, false, reflow_block_cache::Mobi) !=
      reflow_block_cache::Pending)
    return false;
  while (reader.Step() == reflow_block_cache::Pending) {
  }
  if (reader.GetResult() != reflow_block_cache::Complete ||
      loaded.GetPageCount() != f.book.GetPageCount())
    return false;
  for (unsigned i = 0; i < loaded.GetPageCount(); ++i) {
    Page *a = loaded.GetPage(i), *b = f.book.GetPage(i);
    if (a->GetLength() != b->GetLength())
      return false;
    for (int j = 0; j < a->GetLength(); ++j)
      if (a->GetBuffer()[j] != b->GetBuffer()[j])
        return false;
  }
  reader.Close();
  loaded.Close();
  return true;
}

void TestBatchingFillsBlocksAndRoundtrips() {
  Fixture f;
  f.AddPages(900); // about 540k tokens: roughly 33 full blocks
  const unsigned full = (unsigned)(f.total_words / reflow_append_policy::kBlockWords);
  const auto tiny = WriteBook(f, f.Path("tiny.mpc"), 8, 0);
  const auto batched = WriteBook(f, f.Path("batched.mpc"), 8,
                                 reflow_append_policy::kDefaultFlushWords);
  Check(tiny.documents == 113, "old behavior: one document per eight pages");
  Check(batched.documents <= f.total_words /
                                 reflow_append_policy::kDefaultFlushWords + 1,
        "batched documents carry several blocks of text");
  Check(batched.blocks <= full + 2 * batched.documents + 1,
        "batched blocks are full except one tail per document and kind");
  Check(tiny.blocks > batched.blocks * 3 / 2,
        "tiny documents produce many more blocks");
  Check(batched.packed_bytes / batched.blocks >
            tiny.packed_bytes / tiny.blocks,
        "average block is larger after batching");
  Check(batched.write_calls <= tiny.write_calls, "no more write calls");
  Check(LoadMatches(f, f.Path("batched.mpc")), "batched cache loads identically");
  Check(LoadMatches(f, f.Path("tiny.mpc")),
        "cache written with many small documents still loads identically");
  struct stat a, b;
  Require(!stat(f.Path("batched.mpc").c_str(), &a) &&
              !stat(f.Path("tiny.mpc").c_str(), &b),
          "stat caches");
  Check(a.st_size <= b.st_size, "batched cache is not larger");
}

void TestPartialLastBlockAndSingleDocument() {
  Fixture f;
  f.AddPages(1); // less than one block
  Check(WriteBook(f, f.Path("one.mpc"), 1, reflow_append_policy::kDefaultFlushWords)
                .blocks == 1,
        "single short page uses one text block (no search block)");
  Check(LoadMatches(f, f.Path("one.mpc")), "single page loads");
  Fixture g;
  g.AddPages(400);
  // Everything in one document, partial tail block.
  const auto all = WriteBook(g, g.Path("all.mpc"), 400, 0);
  Check(all.documents == 1, "single document");
  // Pages never straddle blocks, so each block holds at least kBlockWords
  // minus one page (<= 900 tokens).
  const unsigned long long min_blocks =
      (g.total_words + reflow_append_policy::kBlockWords - 1) /
      reflow_append_policy::kBlockWords;
  Check(all.blocks >= min_blocks &&
            all.blocks <= g.total_words / (reflow_append_policy::kBlockWords - 900) + 1,
        "blocks are full and the partial last block is flushed");
  Check(LoadMatches(g, g.Path("all.mpc")), "single document loads");
}

void TestPolicy() {
  using namespace reflow_append_policy;
  Check(!ShouldAppend(0, 0, true, true), "nothing pending never appends");
  Check(!ShouldAppend(5, 1000, false, false), "small batch waits");
  Check(ShouldAppend(5, kDefaultFlushWords, false, false), "full batch flushes");
  Check(ShouldAppend(5, kDefaultFlushWords - 1, false, false) == false,
        "just below the threshold waits");
  Check(ShouldAppend(5, 10, true, false), "end of parse flushes remainder");
  Check(ShouldAppend(5, 10, false, true), "memory pressure flushes early");
  Check(ShouldAppend(kMaxPendingPages, 0, false, false),
        "page bound flushes empty pages");
  Check(ShouldAppend(1, 5000, false, false, 4000), "custom threshold");
}

// Each osGetTime() call advances the stub clock by dslibris_test_clock_step ms,
// so a Step budget translates into a bounded amount of writer work.
void TestWriterSliceHonorsBudget() {
  Fixture f;
  f.AddPages(300);
  auto steps_for = [&](unsigned budget) {
    reflow_block_cache::Writer w;
    Require(w.Begin(&f.book, f.Path("budget.mpc"), reflow_block_cache::Mobi),
            "begin");
    Require(w.AppendDocument(0, 300), "append");
    unsigned steps = 0;
    while (w.Step(budget) == reflow_block_cache::Pending && ++steps < 1000000) {
    }
    Check(w.GetResult() == reflow_block_cache::Ready, "document ready");
    ++steps;
    w.Abort();
    return steps;
  };
#ifdef DSLIBRIS_TEST_CLOCK
  dslibris_test_clock_step = 1;
  const unsigned fine = steps_for(2), coarse = steps_for(200);
  Check(fine > coarse * 4, "a small slice budget yields far more often");
  dslibris_test_clock_step = 0;
  Check(steps_for(25) >= 1, "zero-cost clock still completes");
  dslibris_test_clock_step = 1;
#else
  (void)steps_for;
#endif
}

void TestCancelLeavesNoPartialCache() {
  Fixture f;
  f.AddPages(300);
  reflow_block_cache::Writer w;
  const std::string path = f.Path("cancel.mpc");
  Require(w.Begin(&f.book, path, reflow_block_cache::Mobi), "begin");
  Require(w.AppendDocument(0, 150), "append");
  Drain(w);
  Check(access((path + ".tmp").c_str(), F_OK) == 0, "temporary exists mid-write");
  w.Abort();
  Check(access((path + ".tmp").c_str(), F_OK) != 0, "abort removes the temporary");
  Check(access(path.c_str(), F_OK) != 0, "no cache published");
  // Cancel in the middle of a document: only part drained.
  Require(w.Begin(&f.book, path, reflow_block_cache::Mobi), "begin again");
  Require(w.AppendDocument(0, 300), "append all");
  w.Step();
  w.Abort();
  Check(access((path + ".tmp").c_str(), F_OK) != 0 &&
            access(path.c_str(), F_OK) != 0,
        "mid-document cancel leaves nothing behind");
}
} // namespace

int main() {
  TestPolicy();
  TestBatchingFillsBlocksAndRoundtrips();
  TestPartialLastBlockAndSingleDocument();
  TestWriterSliceHonorsBudget();
  TestCancelLeavesNoPartialCache();
  if (failures)
    fprintf(stderr, "%d failure(s)\n", failures);
  return failures ? 1 : 0;
}

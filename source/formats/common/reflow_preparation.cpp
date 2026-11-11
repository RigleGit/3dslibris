#include "shared/path_utils.h"
#include "formats/common/reflow_preparation.h"
#include "formats/epub/epub_preparation.h"
#include "formats/common/reflow_block_cache.h"
#include "formats/common/reflow_open_clock.h"
#include "formats/common/reflow_open_stats.h"
#include "formats/common/reflow_open_profile.h"
#include "formats/common/reflow_append_policy.h"
#include "formats/common/cache_memory_utils.h"
#include "book/page.h"
#include "formats/mobi/mobi_preparation.h"
#include "formats/mobi/mobi_page_cache.h"
#include "formats/fb2/fb2_preparation.h"
#include "formats/fb2/fb2_page_cache.h"
#include "book/book.h"
#include "book/page.h"
#include "book/book_parse_deps.h"
#include "shared/path_constants.h"
#include "shared/string_utils.h"
#include "shared/debug_log.h"
#include <dirent.h>
#include <memory>
#include <new>
#include <sys/stat.h>
#include <unistd.h>
#include <cstdio>

namespace {
// Temporary text of a stream-tier preparation that was interrupted (crash,
// power loss) is left next to the cache: remove it on the next start.
void RemoveStaleStreamScratch(const std::string &cache_path) {
  const size_t slash = cache_path.find_last_of('/');
  if (slash == std::string::npos)
    return;
  const std::string dir = cache_path.substr(0, slash);
  DIR *d = opendir(dir.c_str());
  if (!d)
    return;
  const char *const suffixes[] = {".stream.txt", ".stream2.txt"};
  while (struct dirent *e = readdir(d)) {
    const std::string name = e->d_name;
    for (unsigned i = 0; i < 2; ++i) {
      const std::string suffix = suffixes[i];
      if (name.size() > suffix.size() &&
          name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0)
        unlink((dir + "/" + name).c_str());
    }
  }
  closedir(d);
}
} // namespace

struct ReflowPreparation::State {
  epub_parser::Preparation epub;
  std::unique_ptr<mobi_parser::Preparation> mobi;
  std::unique_ptr<fb2_parser::Preparation> fb2;
  std::unique_ptr<mobi_page_cache::LegacyLoader> legacy;
  std::unique_ptr<reflow_block_cache::Reader> reader;
  reflow_block_cache::Writer writer;
  reflow_block_cache::Format format = reflow_block_cache::Epub;
  Book *book = NULL;
  std::string source, path;
  struct stat identity;
  size_t budget = 0;
  unsigned written = 0;
  bool paused = false, resume = false, source_error = false,
       persistence_failed = false, writing = false, parsed = false;
  enum Phase {
    Cache,
    Legacy,
    Parse,
    Append,
    Commit,
    Validate,
    Activate,
    Done
  } phase = Cache;
  Result result = Pending;
  u64 started = 0;
  reflow_open_stats::Stats stats;
  reflow_open_profile::Profile profile;
  reflow_block_cache::WriterStats wstats;
  bool timing_logged = false, checksummed = false;
  unsigned long long page_words = 0;
  unsigned page_hash = 2166136261u;
  // Debug identity of the prepared content (before pages are released), so
  // two builds can be compared page-for-page from the log alone.
  void ChecksumPages() {
#ifdef DSLIBRIS_DEBUG
    if (checksummed)
      return;
    checksummed = true;
    for (unsigned i = 0; i < book->GetPageCount(); ++i) {
      Page *page = book->GetPage(i);
      const int n = page ? page->GetLength() : 0;
      page_hash = (page_hash ^ (unsigned)n) * 16777619u;
      for (int j = 0; j < n; ++j)
        page_hash = (page_hash ^ page->GetBuffer()[j]) * 16777619u;
      page_words += n > 0 ? n : 0;
    }
#endif
  }
  unsigned frames = 0, max_frame = 0, max_work = 0, splash_ms = 0,
           splash_draws = 0;
  u64 last_frame_ms = 0;
  unsigned scanned = 0, pending_words = 0;
  reflow_open_profile::Inputs inputs;
  bool refined = false;
  u64 last_probe_ms = 0;
  // Closed pages are batched so the writer builds full blocks. Pages stay
  // resident until validation either way, so batching does not raise the peak.
  bool ShouldAppendNow() {
    const unsigned count = book->GetPageCount();
    for (; scanned < count; ++scanned) {
      Page *page = book->GetPage(scanned);
      if (page && page->GetLength() > 0)
        pending_words += (unsigned)page->GetLength();
    }
    bool low_memory = false;
    if (!parsed && pending_words >= reflow_append_policy::kBlockWords) {
      const u64 now = osGetTime();
      if (now - last_probe_ms >= 500) {
        last_probe_ms = now;
        low_memory = !cache_memory::HasHeadroom(256 * 1024, 0);
      }
    }
    return reflow_append_policy::ShouldAppend(
        count > written ? count - written : 0, pending_words, parsed,
        low_memory, profile.flush_words);
  }
  void SnapshotWriter() {
    if (writing)
      wstats = writer.GetStats();
  }
  void LogTiming(const char *outcome) {
    if (timing_logged)
      return;
    timing_logged = true;
    char phases[256];
    stats.FormatPhases(phases, sizeof(phases));
    // Each line stays under the 256-byte log payload.
    DBG_LOGF(book->GetStatusReporter(),
             "REFLOW timing fmt=%u tier=%s outcome=%s total=%llu pages=%u "
             "words=%llu hash=%08x frames=%u max_frame=%u max_work=%u "
             "splash_ms=%u/%u",
             (unsigned)format, reflow_open_profile::TierName(profile.tier),
             outcome, (unsigned long long)(osGetTime() - started),
             book->GetPageCount(), page_words, page_hash, frames, max_frame,
             max_work, splash_ms, splash_draws);
    DBG_LOGF(book->GetStatusReporter(), "REFLOW phases ms/calls %s", phases);
    DBG_LOGF(book->GetStatusReporter(),
             "REFLOW pages calls=%u per_call=%u/%u docs=%u blocks=%u "
             "avg_block=%llu max_block=%llu encode_ms=%llu deflate_ms=%llu "
             "crc_ms=%llu io_ms=%llu writes=%u",
             stats.page_calls,
             stats.page_calls ? stats.pages_total / stats.page_calls : 0u,
             stats.pages_max, wstats.documents, wstats.blocks,
             wstats.blocks ? wstats.packed_bytes / wstats.blocks : 0ULL,
             wstats.max_block, wstats.encode_us / 1000,
             wstats.deflate_us / 1000, wstats.crc_us / 1000, wstats.io_ms,
             wstats.write_calls);
  }
  void ReleaseParser() {
    mobi.reset();
    fb2.reset();
  }
  bool StartParser() {
    if (format == reflow_block_cache::Mobi) {
      mobi.reset(new (std::nothrow) mobi_parser::Preparation);
      if (!mobi) {
        result = NoMemory;
        return false;
      }
      if (!mobi->Begin(book, source)) {
        result = mobi->GetResult();
        return false;
      }
      mobi->SetStats(&stats);
      mobi->SetProfile(&profile);
      mobi->SetScratchBase(path);
      RemoveStaleStreamScratch(path);
      return true;
    }
    fb2.reset(new (std::nothrow) fb2_parser::Preparation);
    if (!fb2) {
      result = NoMemory;
      return false;
    }
    if (!fb2->Begin(book, source)) {
      result = fb2->GetResult();
      return false;
    }
    fb2->SetStats(&stats);
    return true;
  }
  void WriterFailed() {
    DBG_LOGF(book->GetStatusReporter(), "Reflow cache unavailable format=%u result=%d pages=%u", (unsigned)format, (int)writer.GetResult(), book->GetPageCount());
    writer.Abort();
    writing = false;
    persistence_failed = true;
  }
  Result Finish() {
    ReleaseParser();
    SnapshotWriter();
    writer.Abort();
    writing = false;
    phase = Done;
    result = Complete;
    DBG_LOGF(book->GetStatusReporter(),
             "Reflow preparation format=%u pages=%u saved=%u total_ms=%llu",
             (unsigned)format, book->GetPageCount(), persistence_failed ? 0 : 1,
             (unsigned long long)(osGetTime() - started));
    LogTiming(persistence_failed ? "unsaved" : "saved");
    return Complete;
  }
  bool BeginWriter() {
    writing = writer.Begin(book, path, format);
    if (!writing)
      WriterFailed();
    else
      writer.SetCompress(profile.compress);
    return writing;
  }
};
ReflowPreparation::ReflowPreparation() : state_(NULL) {}
ReflowPreparation::~ReflowPreparation() {
  if (state_ && state_->format != reflow_block_cache::Epub &&
      state_->result == Pending)
    Cancel();
  delete state_;
}
bool ReflowPreparation::Begin(Book *book, const std::string &source,
                              size_t budget, bool new3ds) {
  if (state_ && (GetResult() == Pending || GetResult() == Paused))
    Cancel();
  delete state_;
  state_ = new (std::nothrow) State;
  if (!state_ || !book)
    return false;
  State &s = *state_;
  s.book = book;
  s.source = source;
  s.budget = budget;
  s.started = osGetTime();
  if (!paths::StatWithMtime(source.c_str(), &s.identity)) {
    s.source_error = true;
    s.result = IoError;
    return false;
  }
  if (HasExtCI(book->GetFileName(), ".mobi"))
    s.format = reflow_block_cache::Mobi;
  else if (HasExtCI(book->GetFileName(), ".fb2"))
    s.format = reflow_block_cache::Fb2;
  {
    reflow_open_profile::Inputs in;
    in.format = (reflow_open_profile::Format)s.format;
    in.file_size = (unsigned long long)s.identity.st_size;
    in.new3ds = new3ds;
    const unsigned long long headroom = reflow_open_stats::HeapHeadroom();
    in.free_bytes = headroom;
    s.profile = reflow_open_profile::Choose(in);
#ifdef DSLIBRIS_DEBUG
    // Console A/B seams: empty marker files next to the log force a tier or
    // disable compression for the next open. Debug builds only.
    const char *const base = "sdmc:/3ds/3dslibris/";
    if (access((std::string(base) + "reflow_safe.flag").c_str(), F_OK) == 0)
      s.profile = reflow_open_profile::SafeProfile();
    else if (access((std::string(base) + "reflow_stream.flag").c_str(), F_OK) ==
                 0 &&
             in.format == reflow_open_profile::Mobi) {
      const unsigned long long estimate = s.profile.estimated_peak,
                               required = s.profile.required;
      s.profile = reflow_open_profile::StreamProfile();
      s.profile.estimated_peak = estimate;
      s.profile.required = required;
    } else if (access((std::string(base) + "reflow_fast.flag").c_str(), F_OK) == 0 &&
             in.format != reflow_open_profile::Epub) {
      const bool had = s.profile.tier == reflow_open_profile::Fast;
      if (!had) {
        in.free_bytes = ~0ULL;
        s.profile = reflow_open_profile::Choose(in);
        in.free_bytes = headroom;
      }
    }
    if (access((std::string(base) + "reflow_nocompress.flag").c_str(), F_OK) == 0)
      s.profile.compress = false;
#endif
    s.inputs = in;
    char line[240];
    reflow_open_profile::Describe(line, sizeof(line), in, s.profile);
    DBG_LOGF(book->GetStatusReporter(), "REFLOW profile %s", line);
  }
  if (s.format == reflow_block_cache::Epub)
    return s.epub.Begin(book, source, budget);
  if (book->GetPageCount()) {
    s.result = Failed;
    return false;
  }
  book->PrepareForOpen();
  book->CaptureEbookImageSetting();
  const auto deps = BuildBookParseDeps(book);
  const auto &l = deps.layout;
  const auto params = page_cache_utils::BuildLayoutParams(
      source.c_str(), l.pixel_size, l.linespacing, deps.paragraph_spacing,
      deps.paragraph_indent, deps.orientation, l.margin_left, l.margin_right,
      l.margin_top, l.margin_bottom, l.regular_font_path.c_str());
  s.path = s.format == reflow_block_cache::Mobi
               ? mobi_page_cache::CachePath(book, source.c_str(), params)
               : fb2_page_cache::CachePath(book, source.c_str(), params);
  mkdir(paths::GetCacheBaseDir().c_str(), 0777);
  const size_t slash = s.path.find_last_of('/');
  if (slash != std::string::npos)
    mkdir(s.path.substr(0, slash).c_str(), 0777);
  bool legacy = false;
  if (s.format == reflow_block_cache::Mobi) {
    FILE *fp = fopen(s.path.c_str(), "rb");
    if (fp) {
      u32 magic = 0;
      u16 version = 0;
      fread(&magic, 1, 4, fp);
      fread(&version, 1, 2, fp);
      fclose(fp);
      legacy = magic == 0x4d504347U && version == 28;
    }
  }
  if (legacy) {
    s.legacy.reset(new (std::nothrow) mobi_page_cache::LegacyLoader);
    if (!s.legacy) {
      s.result = NoMemory;
      return false;
    }
    s.legacy->Begin(book, s.path);
    s.phase = State::Legacy;
  } else {
    s.reader.reset(new (std::nothrow) reflow_block_cache::Reader);
    if (!s.reader) {
      s.result = NoMemory;
      return false;
    }
    const auto r = s.reader->Begin(book, s.path, budget, false, s.format);
    if (r == reflow_block_cache::NoMemory)
      s.result = NoMemory;
  }
  return true;
}
void ReflowPreparation::SignalPause() {
  if (!state_)
    return;
  state_->paused = true;
  state_->resume = false;
  if (state_->fb2)
    state_->fb2->SignalPause();
}
void ReflowPreparation::SignalResume() {
  if (state_)
    state_->resume = true;
}
ReflowPreparation::Result ReflowPreparation::Step() {
  if (!state_)
    return NoMemory;
  State &s = *state_;
  if (s.resume) {
    s.resume = false;
    struct stat identity;
    if (!paths::StatWithMtime(s.source.c_str(), &identity) ||
        identity.st_size != s.identity.st_size ||
        identity.st_mtime != s.identity.st_mtime)
      s.source_error = true;
    s.paused = false;
    if (s.fb2)
      s.fb2->SignalResume();
  }
  if (s.source_error)
    return s.result = IoError;
  if (s.paused)
    return Paused;
  if (s.result == Cancelled)
    return Cancelled;
  if (s.format == reflow_block_cache::Epub) {
    const Result r = static_cast<Result>(s.epub.Step());
    if (r == Complete)
      s.LogTiming(s.epub.PersistenceFailed() ? "unsaved" : "saved");
    return r;
  }
  if (s.result != Pending)
    return s.result;
  if (s.phase == State::Cache || s.phase == State::Legacy) {
    const auto r =
        s.phase == State::Legacy ? s.legacy->Step() : s.reader->Step();
    if (r == reflow_block_cache::Pending)
      return Pending;
    if (r == reflow_block_cache::Complete) {
      if (s.phase == State::Legacy) {
        s.legacy.reset();
        s.parsed = true;
        s.BeginWriter();
        if (s.writing && s.writer.AppendDocument(0, s.book->GetPageCount())) {
          s.written = s.scanned = s.book->GetPageCount();
          s.phase = State::Append;
        } else {
          s.WriterFailed();
          return s.Finish();
        }
      } else {
        if (!s.book->AdoptPreparedReflowPageCache(s.reader.get()))
          return s.result = Failed;
        s.reader.release();
        s.book->MarkMobiRenderSettingsApplied(s.book->GetMobiLineWrapFix());
        return s.Finish();
      }
    } else {
      s.legacy.reset();
      s.reader.reset();
      const unsigned session = s.book->GetOpenSessionId();
      s.book->Close();
      s.book->SetOpenSessionId(session);
      s.book->CaptureEbookImageSetting();
      if (r == reflow_block_cache::NoMemory)
        return s.result = NoMemory;
      if (r == reflow_block_cache::IoError)
        return s.result = IoError;
      if (r != reflow_block_cache::Missing && r != reflow_block_cache::Corrupt)
        return s.result = Failed;
      if (!s.StartParser())
        return s.result;
      s.BeginWriter();
      s.phase = State::Parse;
    }
  } else if (s.phase == State::Parse) {
    const u64 parse_begin = reflow_open_stats::NowUs();
    const Result r = s.mobi ? s.mobi->Step() : s.fb2->Step();
    s.stats.Add(reflow_open_stats::Parse,
                reflow_open_stats::NowUs() - parse_begin);
    if (r == Paused)
      return Paused;
    if (r != Pending && r != Complete)
      return s.result = r;
    s.parsed = r == Complete;
    if (!s.refined && s.mobi) {
      const unsigned long long text_len = s.mobi->TextLength();
      if (text_len) {
        // The header gives the real text size: re-evaluate once. Only a
        // downgrade is applied; the read stage already ran with the first pick.
        s.refined = true;
        s.inputs.text_len = text_len;
        // The source is already resident; the estimate counts it again, so
        // add it back to what is free now.
        s.inputs.free_bytes =
            reflow_open_stats::HeapHeadroom() + s.inputs.file_size;
        const auto again = reflow_open_profile::Choose(s.inputs);
        char line[240];
        reflow_open_profile::Describe(line, sizeof(line), s.inputs, again);
        DBG_LOGF(s.book->GetStatusReporter(), "REFLOW profile refined %s", line);
        if (s.profile.tier == reflow_open_profile::Fast &&
            again.tier == reflow_open_profile::Safe)
          s.profile = reflow_open_profile::SafeProfile();
        else if (s.profile.tier != reflow_open_profile::Stream &&
                 again.tier == reflow_open_profile::Stream) {
          // The real text size does not fit the resident pipeline either.
          s.profile = again;
          DBG_LOGF(s.book->GetStatusReporter(),
                   "REFLOW profile degraded to stream: text=%llu does not fit",
                   text_len);
        }
      }
    }
    if (s.parsed)
      s.ChecksumPages();
    if (s.writing && s.book->GetPageCount() > s.written &&
        s.ShouldAppendNow()) {
      const u64 append_begin = reflow_open_stats::NowUs();
      const bool appended =
          s.writer.AppendDocument(s.written, s.book->GetPageCount());
      s.stats.Add(reflow_open_stats::Append,
                  reflow_open_stats::NowUs() - append_begin);
      if (appended) {
        s.written = s.book->GetPageCount();
        s.pending_words = 0;
        s.phase = State::Append;
      } else
        s.WriterFailed();
    }
    if (s.parsed && s.phase == State::Parse) {
      s.ReleaseParser();
      if (s.writing && s.writer.Finish())
        s.phase = State::Commit;
      else {
        s.WriterFailed();
        return s.Finish();
      }
    }
  } else if (s.phase == State::Append) {
    const u64 append_begin = reflow_open_stats::NowUs();
    const auto r = s.writer.Step(s.profile.writer_budget_ms);
    s.stats.Add(reflow_open_stats::Append,
                reflow_open_stats::NowUs() - append_begin);
    if (r == reflow_block_cache::Pending)
      return Pending;
    if (r != reflow_block_cache::Ready)
      s.WriterFailed();
    if (s.parsed) {
      s.ReleaseParser();
      if (s.writing && s.writer.Finish())
        s.phase = State::Commit;
      else {
        s.WriterFailed();
        return s.Finish();
      }
    } else
      s.phase = State::Parse;
  } else if (s.phase == State::Commit) {
    const u64 commit_begin = reflow_open_stats::NowUs();
    const auto r = s.writer.Step(s.profile.writer_budget_ms);
    s.stats.Add(reflow_open_stats::Commit,
                reflow_open_stats::NowUs() - commit_begin);
    if (r == reflow_block_cache::Pending)
      return Pending;
    if (r != reflow_block_cache::Complete) {
      s.WriterFailed();
      return s.Finish();
    }
    s.SnapshotWriter();
    s.writer.Abort();
    s.writing = false;
    if (!s.budget)
      return s.Finish();
    s.reader.reset(new (std::nothrow) reflow_block_cache::Reader);
    if (!s.reader || s.reader->Begin(s.book, s.path, s.budget, true,
                                     s.format) != reflow_block_cache::Pending) {
      s.persistence_failed = true;
      return s.Finish();
    }
    s.phase = State::Validate;
  } else if (s.phase == State::Validate) {
    const u64 validate_begin = reflow_open_stats::NowUs();
    const auto r = s.reader->Step();
    s.stats.Add(reflow_open_stats::Validate,
                reflow_open_stats::NowUs() - validate_begin);
    if (r == reflow_block_cache::Pending)
      return Pending;
    if (r != reflow_block_cache::Complete ||
        !s.reader->ActivatePreparedPages() ||
        !s.book->AdoptPreparedReflowPageCache(s.reader.get())) {
      s.reader.reset();
      s.persistence_failed = true;
      return s.Finish();
    }
    s.reader.release();
    s.phase = State::Activate;
  } else if (s.phase == State::Activate) {
    const int r = s.book->AdvanceReflowPageCache();
    if (r > 0)
      return Pending;
    if (r < 0)
      return s.result = s.book->GetReflowPageCacheResult() ==
                                reflow_block_cache::NoMemory
                            ? NoMemory
                            : IoError;
    return s.Finish();
  }
  return s.result;
}
void ReflowPreparation::Cancel() {
  if (!state_)
    return;
  State &s = *state_;
  s.paused = s.resume = s.source_error = false;
  if (s.result == Pending)
    s.SnapshotWriter();
  if (s.result == Pending)
    s.LogTiming("cancelled");
  s.result = Cancelled;
  if (s.format == reflow_block_cache::Epub) {
    s.epub.Cancel();
    return;
  }
  s.writer.Abort();
  s.reader.reset();
  s.legacy.reset();
  s.ReleaseParser();
  s.book->Close();
  s.result = Cancelled;
}
ReflowPreparation::Result ReflowPreparation::GetResult() const {
  if (!state_)
    return NoMemory;
  if (state_->result == Cancelled)
    return Cancelled;
  if (state_->source_error)
    return IoError;
  if (state_->paused)
    return Paused;
  return state_->format == reflow_block_cache::Epub
             ? static_cast<Result>(state_->epub.GetResult())
             : state_->result;
}
const char *ReflowPreparation::Label() const {
  if (!state_)
    return "not enough memory to prepare book";
  const State &s = *state_;
  if (s.source_error)
    return "source changed or unavailable";
  if (s.format == reflow_block_cache::Epub)
    return s.epub.Label();
  if (s.result == NoMemory)
    return "not enough memory to prepare book";
  if (s.result == IoError)
    return "cannot read book or cache";
  if (s.result == Failed)
    return "cannot prepare book";
  if (s.result == Complete && s.persistence_failed)
    return "cache unavailable; A: read / B: back";
  switch (s.phase) {
  case State::Cache:
  case State::Legacy:
    return "loading cache... B: cancel";
  case State::Parse:
  case State::Append:
    if (s.mobi)
      return s.mobi->Label();
    if (s.fb2)
      return s.fb2->Label();
    return "saving cache... B: cancel";
  case State::Commit:
    return "finalizing cache... B: cancel";
  default:
    return "checking cache... B: cancel";
  }
}
unsigned ReflowPreparation::Progress() const {
  if (!state_)
    return 0;
  const State &s = *state_;
  if (s.format == reflow_block_cache::Epub)
    return s.epub.Progress();
  if (s.phase == State::Parse || s.phase == State::Append)
    return s.mobi   ? s.mobi->Progress()
           : s.fb2  ? s.fb2->Progress()
           : s.writing ? s.writer.Progress()
                       : 0;
  return 0;
}
unsigned ReflowPreparation::Total() const {
  if (!state_)
    return 0;
  const State &s = *state_;
  if (s.format == reflow_block_cache::Epub)
    return s.epub.Total();
  if (s.phase == State::Parse || s.phase == State::Append)
    return s.mobi   ? s.mobi->Total()
           : s.fb2  ? s.fb2->Total()
           : s.writing ? s.book->GetPageCount()
                       : 0;
  return 0;
}
bool ReflowPreparation::PersistenceFailed() const {
  return state_ && (state_->format == reflow_block_cache::Epub
                        ? state_->epub.PersistenceFailed()
                        : state_->persistence_failed);
}
void ReflowPreparation::NoteFrame(unsigned long long now_ms,
                                  unsigned work_ms) {
  if (!state_)
    return;
  State &s = *state_;
  ++s.frames;
  if (s.last_frame_ms && now_ms - s.last_frame_ms > s.max_frame)
    s.max_frame = (unsigned)(now_ms - s.last_frame_ms);
  s.last_frame_ms = now_ms;
  if (work_ms > s.max_work)
    s.max_work = work_ms;
}
void ReflowPreparation::NoteSplash(unsigned ms) {
  if (!state_)
    return;
  state_->splash_ms += ms;
  ++state_->splash_draws;
}
unsigned ReflowPreparation::FrameBudgetMs() const {
  return state_ ? state_->profile.frame_budget_ms : 30u;
}

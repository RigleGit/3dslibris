#include "formats/common/reflow_preparation.h"
#include "formats/epub/epub_preparation.h"
#include "formats/common/reflow_block_cache.h"
#include "formats/mobi/mobi_preparation.h"
#include "formats/mobi/mobi_page_cache.h"
#include "formats/fb2/fb2_preparation.h"
#include "formats/fb2/fb2_page_cache.h"
#include "book/book.h"
#include "book/book_parse_deps.h"
#include "shared/path_constants.h"
#include "shared/string_utils.h"
#include "shared/debug_log.h"
#include <memory>
#include <new>
#include <sys/stat.h>
#include <cstdio>

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
    writer.Abort();
    writing = false;
    phase = Done;
    result = Complete;
    DBG_LOGF(book->GetStatusReporter(),
             "Reflow preparation format=%u pages=%u saved=%u total_ms=%llu",
             (unsigned)format, book->GetPageCount(), persistence_failed ? 0 : 1,
             (unsigned long long)(osGetTime() - started));
    return Complete;
  }
  bool BeginWriter() {
    writing = writer.Begin(book, path, format);
    if (!writing)
      WriterFailed();
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
                              size_t budget) {
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
  if (stat(source.c_str(), &s.identity) != 0) {
    s.source_error = true;
    s.result = IoError;
    return false;
  }
  if (HasExtCI(book->GetFileName(), ".mobi"))
    s.format = reflow_block_cache::Mobi;
  else if (HasExtCI(book->GetFileName(), ".fb2"))
    s.format = reflow_block_cache::Fb2;
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
      legacy = magic == 0x4d504347U && version == 26;
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
    if (stat(s.source.c_str(), &identity) != 0 ||
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
  if (s.format == reflow_block_cache::Epub)
    return static_cast<Result>(s.epub.Step());
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
          s.written = s.book->GetPageCount();
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
    const Result r = s.mobi ? s.mobi->Step() : s.fb2->Step();
    if (r == Paused)
      return Paused;
    if (r != Pending && r != Complete)
      return s.result = r;
    s.parsed = r == Complete;
    if (s.writing && s.book->GetPageCount() > s.written) {
      if (s.writer.AppendDocument(s.written, s.book->GetPageCount())) {
        s.written = s.book->GetPageCount();
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
    const auto r = s.writer.Step();
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
    const auto r = s.writer.Step();
    if (r == reflow_block_cache::Pending)
      return Pending;
    if (r != reflow_block_cache::Complete) {
      s.WriterFailed();
      return s.Finish();
    }
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
    const auto r = s.reader->Step();
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
    return s.mobi ? s.mobi->Progress() : s.fb2 ? s.fb2->Progress() : 0;
  return 0;
}
unsigned ReflowPreparation::Total() const {
  if (!state_)
    return 0;
  const State &s = *state_;
  if (s.format == reflow_block_cache::Epub)
    return s.epub.Total();
  if (s.phase == State::Parse || s.phase == State::Append)
    return s.mobi ? s.mobi->Total() : s.fb2 ? s.fb2->Total() : 0;
  return 0;
}
bool ReflowPreparation::PersistenceFailed() const {
  return state_ && (state_->format == reflow_block_cache::Epub
                        ? state_->epub.PersistenceFailed()
                        : state_->persistence_failed);
}

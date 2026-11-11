#include "formats/mobi/mobi_parser.h"
#include "formats/mobi/mobi_preparation.h"
#include "formats/mobi/mobi_toc_apply.h"
#include "formats/mobi/mobi_toc_finalize_policy.h"
#include "formats/mobi/mobi_record_decode.h"
#include "formats/mobi/mobi_book_hooks.h"
#include "formats/mobi/mobi_safe_markup_extract.h"
#include "formats/common/cache_memory_utils.h"
#include "formats/common/reflow_open_clock.h"
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "shared/debug_log.h"
#include "formats/common/buffered_status_log.h"
#include "formats/common/book_error.h"
#include "formats/common/plain_text_perf_utils.h"
#include "shared/fixed_layout_perf.h"
#include "formats/common/text_helpers.h"
#include "formats/mobi/mobi_decode_plan.h"
#include "formats/mobi/mobi_cleanup_policy.h"
#include "formats/mobi/mobi_page_cache.h"
#include "formats/mobi/mobi_parser_core.h"
#include "formats/mobi/mobi_structured_toc_parser.h"
#include "formats/mobi/mobi_position_map.h"
#include "formats/mobi/mobi_text_cleanup.h"
#include "formats/mobi/mobi_text_decode.h"
#include "parse.h"
#include "shared/open_cancel_poll.h"
#include "shared/status_reporter.h"

#include <memory>
#include <new>
#include <3ds.h>
#include <stdio.h>
#include <sys/param.h>
#include <utility>
#include <vector>

namespace mobi_parser {
namespace {

static const size_t kMobiMaxBytes = 64 * 1024 * 1024;

static bool ShouldAbortMobiOpen(Book *book) {
  return book && ((book->GetStatusReporter() &&
                   book->GetStatusReporter()->ShouldAbortWork()) ||
                  book->IsOpenAbortRequested());
}

using mobi_parser_core::MobiHeaderInfo;
using mobi_toc_finalize::MobiHeadingHint;
using mobi_toc_finalize::MobiTocFinalizeResult;
using plain_text_perf_utils::LogPlainTextStreamPerf;
using plain_text_perf_utils::PlainTextStreamPerf;

struct MobiParseState {
  plain_text_stream::State stream;
  std::string text_utf8;
  std::vector<MobiHeadingHint> heading_hints;
  std::vector<mobi_structured_toc_parser::MobiStructuredTocEntry>
      structured_toc;
  std::vector<std::pair<u32, u32>> html_to_text_map;
  std::vector<u32> text_cursor_per_page;
  bool have_structured_toc = false, structured_from_filepos = false,
       line_wrap_fix_applied = false;
  u32 text_len_for_pos = 0;
};

using MobiDeferredState = MobiParseState;

// noinline: keeps this frame separate from ParseFile's.
// parsedata_t is ~16KB; we initialize out->parsedata in-place (already on the
// heap inside State) to avoid any local copy on the stack.
static __attribute__((noinline)) bool InitPlainTextStreamStateLocal(
    Book *book, const std::string &text_utf8, const BookParseDeps &deps,
    bool detect_heuristic_headings, plain_text_stream::State *out) {
  if (!book || !deps.ts || !out)
    return false;
  // Initialize out->parsedata in-place — no local parsedata_t copy (~16KB).
  // InitState's self-assignment (state->parsedata = out->parsedata) is safe per
  // the C++ standard and optimized away by the compiler in -O2.
  parse_init(&out->parsedata);
  out->parsedata.reporter = deps.reporter;
  out->parsedata.ts = deps.ts;
  out->parsedata.prefs = deps.prefs;
  out->parsedata.book = book;
  plain_text_stream::InitState(out, out->parsedata, text_utf8,
                               detect_heuristic_headings);
  return true;
}

static size_t MobiInlineTokenLengthAt(const std::string &text, size_t pos,
                                      u16 *image_id_out = NULL) {
  if (image_id_out)
    *image_id_out = 0;
  if (pos >= text.size())
    return 0;

  size_t image_pos = pos;
  const unsigned char c0 = (unsigned char)text[pos];
  if (c0 == TEXT_IMAGE) {
    if (pos + 2 >= text.size())
      return 0;
  } else if ((c0 == TEXT_IMAGE_CONTEXT_DEFAULT ||
              c0 == TEXT_IMAGE_LEADING_PARAGRAPH ||
              c0 == TEXT_IMAGE_FIGURE_WITH_CAPTION) &&
             pos + 3 < text.size() &&
             (unsigned char)text[pos + 1] == TEXT_IMAGE) {
    image_pos = pos + 1;
  } else {
    return 0;
  }

  if (image_id_out) {
    *image_id_out =
        (u16)(((u8)text[image_pos + 1] << 8) | (u8)text[image_pos + 2]);
  }
  return image_pos == pos ? 3u : 4u;
}

static void CollectMobiInlineImageTokenIds(const std::string &text,
                                           std::vector<u16> *ids) {
  if (!ids)
    return;
  ids->clear();
  for (size_t i = 0; i < text.size();) {
    u16 image_id = 0;
    const size_t token_len = MobiInlineTokenLengthAt(text, i, &image_id);
    if (token_len == 0) {
      i++;
      continue;
    }
    ids->push_back(image_id);
    i += token_len;
  }
}

static bool HasMobiInlineImageTokens(const std::string &text) {
  for (size_t i = 0; i < text.size();) {
    const size_t token_len = MobiInlineTokenLengthAt(text, i, NULL);
    if (token_len != 0)
      return true;
    i++;
  }
  return false;
}

#ifdef DSLIBRIS_DEBUG
static void LogMobiBuffer(IStatusReporter *reporter, const char *phase,
                          const char *name, const std::string &buffer) {
  DBG_LOGF(reporter, "MOBI buffer: phase=%s name=%s size=%lu capacity=%lu",
           phase, name, (unsigned long)buffer.size(),
           (unsigned long)buffer.capacity());
}
#endif

static void CleanupDecodedMobiText(IStatusReporter *reporter, std::string *text,
                                   std::vector<std::pair<u32, u32>> *html_map,
                                   bool line_wrap_fix_applied) {
  if (!text)
    return;

  const size_t text_pre_cleanup = text->size();
  const bool have_map = html_map && !html_map->empty();
  const bool run_post_normalize =
      mobi_cleanup_policy::ShouldRunPostNormalizeCleanup(have_map,
                                                         line_wrap_fix_applied);
  const bool needs_newlines = text->find('\r') != std::string::npos;
  const bool needs_mojibake =
      run_post_normalize && text->find("\xC3\x83") != std::string::npos;
  // Most already-clean books need neither a remap copy nor a replacement
  // string. Check the only repair prefix before retaining megabytes of text.
  if (!needs_newlines && !needs_mojibake && !line_wrap_fix_applied)
    return;
  std::string text_before_cleanup;
  if (have_map)
    text_before_cleanup = *text;

#ifdef DSLIBRIS_DEBUG
  LogMobiBuffer(reporter, "cleanup_begin", "text", *text);
  LogMobiBuffer(reporter, "cleanup_begin", "remap_copy", text_before_cleanup);
  if (reporter)
    DBG_LOGF(reporter,
             "MOBI cleanup: step=normalize-begin bytes=%u have_map=%d wrap=%d "
             "mem=%u",
             (unsigned)text->size(), have_map ? 1 : 0,
             line_wrap_fix_applied ? 1 : 0,
             (unsigned)osGetMemRegionFree(MEMREGION_ALL));
#endif

  if (needs_newlines)
    NormalizeNewlines(text);

#ifdef DSLIBRIS_DEBUG
  if (reporter)
    DBG_LOGF(reporter, "MOBI cleanup: step=normalize-done bytes=%u mem=%u",
             (unsigned)text->size(),
             (unsigned)osGetMemRegionFree(MEMREGION_ALL));
#endif

  if (!run_post_normalize) {
#ifdef DSLIBRIS_DEBUG
    if (reporter)
      DBG_LOGF(reporter, "MOBI cleanup: step=post-normalize-skipped mem=%u",
               (unsigned)osGetMemRegionFree(MEMREGION_ALL));
#endif
    return;
  }

  const bool track_image_tokens = (needs_mojibake || line_wrap_fix_applied) &&
                                  HasMobiInlineImageTokens(*text);
  std::string text_before_mobi_cleanup;
  std::vector<u16> image_ids_before_cleanup;
  if (track_image_tokens) {
    text_before_mobi_cleanup = std::move(*text);
    CollectMobiInlineImageTokenIds(text_before_mobi_cleanup,
                                   &image_ids_before_cleanup);
  }

#ifdef DSLIBRIS_DEBUG
  LogMobiBuffer(reporter, "cleanup_moved", "image_backup",
                text_before_mobi_cleanup);
  if (reporter)
    DBG_LOGF(reporter, "MOBI cleanup: step=mojibake-begin track=%d mem=%u",
             track_image_tokens ? 1 : 0,
             (unsigned)osGetMemRegionFree(MEMREGION_ALL));
#endif

  if (needs_mojibake) {
    if (track_image_tokens) {
      *text = mobi_text_cleanup::RepairCommonMojibakePreservingMobiImageTokens(
          text_before_mobi_cleanup);
    } else {
      mobi_text_cleanup::RepairCommonMojibakeInPlace(text);
    }
  } else if (track_image_tokens) {
    // The original stays available for the wrap-fix image-ID rollback.
    *text = text_before_mobi_cleanup;
  }

#ifdef DSLIBRIS_DEBUG
  if (reporter)
    DBG_LOGF(reporter, "MOBI cleanup: step=mojibake-done bytes=%u mem=%u",
             (unsigned)text->size(),
             (unsigned)osGetMemRegionFree(MEMREGION_ALL));
#endif

  if (line_wrap_fix_applied) {
#ifdef DSLIBRIS_DEBUG
    if (reporter)
      DBG_LOGF(reporter, "MOBI cleanup: step=wrap-fix-begin bytes=%u mem=%u",
               (unsigned)text->size(),
               (unsigned)osGetMemRegionFree(MEMREGION_ALL));
#endif
    if (track_image_tokens) {
      *text =
          mobi_text_cleanup::FixBrokenParagraphWrapsPreservingMobiImageTokens(
              *text);
    } else {
      *text = mobi_text_cleanup::FixBrokenParagraphWraps(*text);
    }
#ifdef DSLIBRIS_DEBUG
    if (reporter)
      DBG_LOGF(reporter, "MOBI cleanup: step=wrap-fix-done bytes=%u mem=%u",
               (unsigned)text->size(),
               (unsigned)osGetMemRegionFree(MEMREGION_ALL));
#endif
  }

  size_t text_post_cleanup = text->size();
  if (track_image_tokens) {
    std::vector<u16> image_ids_after_cleanup;
    CollectMobiInlineImageTokenIds(*text, &image_ids_after_cleanup);
    if (image_ids_before_cleanup != image_ids_after_cleanup) {
      *text = std::move(text_before_mobi_cleanup);
      text_post_cleanup = text->size();
    }
  }

  if (have_map && text_post_cleanup != text_pre_cleanup) {
#ifdef DSLIBRIS_DEBUG
    if (reporter)
      DBG_LOGF(reporter, "MOBI cleanup: step=remap-begin pre=%u post=%u mem=%u",
               (unsigned)text_pre_cleanup, (unsigned)text_post_cleanup,
               (unsigned)osGetMemRegionFree(MEMREGION_ALL));
#endif
    mobi_position_map::RemapHtmlToTextAfterCleanup(text_before_cleanup, *text,
                                                   html_map);
#ifdef DSLIBRIS_DEBUG
    if (reporter)
      DBG_LOGF(reporter, "MOBI cleanup: step=remap-done mem=%u",
               (unsigned)osGetMemRegionFree(MEMREGION_ALL));
#endif
  }

#ifdef DSLIBRIS_DEBUG
  if (reporter)
    DBG_LOGF(reporter, "MOBI cleanup: step=done bytes=%u mem=%u",
             (unsigned)text->size(),
             (unsigned)osGetMemRegionFree(MEMREGION_ALL));
#endif
}

} // namespace

u8 ParseFile(Book *book, const char *path, const Hooks &hooks) {
  Preparation prep;
  if (!book || !path || !prep.Begin(book, path, &hooks))
    return 1;
  ReflowPreparation::Result result;
  do {
    if (ShouldAbortMobiOpen(book))
      return BOOK_ERR_CANCELLED;
    result = prep.Step();
  } while (result == ReflowPreparation::Pending);
  return result == ReflowPreparation::Complete ? 0 : 1;
}

// Parser-only cooperative preparation. Persistence and residency are owned by
// ReflowPreparation, so parsing never saves from Book mode or from Close.
struct Preparation::State {
  Book *book;
  BookParseDeps deps;
  Hooks hooks;
  int fd = -1;
  size_t file_size = 0, read_bytes = 0;
  std::string raw, merged, utf8;
  MobiHeaderInfo header;
  MobiDeferredState deferred;
  mobi_record_decode::MergeSession merger;
  mobi_text_decode::Session decoder;
  mobi_safe_markup_extract::Session extractor;
  mobi_toc_apply::Session toc;
  bool toc_started = false;
  reflow_open_stats::Stats *stats = NULL;
  std::vector<ChapterEntry> fallback;
  enum Phase {
    Read,
    Header,
    Merge,
    Decode,
    Markup,
    Cleanup,
    TocPrepare,
    Init,
    Pages,
    Toc,
    Done
  } phase = Read;
  ReflowPreparation::Result result = ReflowPreparation::Pending;
  ~State() {
    if (fd >= 0)
      close(fd);
  }
};
Preparation::Preparation() : state_(NULL) {}
Preparation::~Preparation() {
  delete state_;
}
bool Preparation::Begin(Book *book, const std::string &source,
                        const Hooks *hooks) {
  delete state_;
  state_ = new (std::nothrow) State;
  if (!state_ || !book)
    return false;
  State &s = *state_;
  s.book = book;
  s.deps = BuildBookParseDeps(book);
  s.hooks = hooks ? *hooks : mobi_book_hooks::Make();
  s.fd = open(source.c_str(), O_RDONLY);
  struct stat info;
  if (s.fd < 0 || fstat(s.fd, &info) != 0) {
    s.result = ReflowPreparation::IoError;
    return false;
  }
  if (info.st_size <= 0 || (uint64_t)info.st_size > kMobiMaxBytes) {
    s.result = ReflowPreparation::Failed;
    return false;
  }
  s.file_size = info.st_size;
  s.deferred.line_wrap_fix_applied = book->GetMobiLineWrapFix();
  return true;
}
ReflowPreparation::Result Preparation::GetResult() const {
  return state_ ? state_->result : ReflowPreparation::NoMemory;
}
void Preparation::SetStats(reflow_open_stats::Stats *stats) {
  if (state_)
    state_->stats = stats;
}
ReflowPreparation::Result Preparation::Step() {
  if (!state_ || !state_->stats || state_->result != ReflowPreparation::Pending)
    return StepPhase();
  State &s = *state_;
  // State::Phase order matches reflow_open_stats::Slot up to Toc.
  const unsigned slot = (unsigned)s.phase;
  const unsigned pages_before = s.book->GetPageCount();
  const uint64_t begin = reflow_open_stats::NowUs();
  const ReflowPreparation::Result r = StepPhase();
  if (slot <= (unsigned)reflow_open_stats::Toc) {
    s.stats->Add((reflow_open_stats::Slot)slot,
                 reflow_open_stats::NowUs() - begin);
    if (slot == (unsigned)reflow_open_stats::Pages)
      s.stats->NotePages(s.book->GetPageCount() - pages_before);
  }
  return r;
}
ReflowPreparation::Result Preparation::StepPhase() {
  if (!state_)
    return ReflowPreparation::Failed;
  State &s = *state_;
  if (s.result != ReflowPreparation::Pending)
    return s.result;
  if (s.phase == State::Read) {
    const size_t n = std::min<size_t>(65536, s.file_size - s.read_bytes);
    if (s.raw.size() + n > s.raw.capacity() &&
        !cache_memory::HasHeadroom(
            std::max(s.raw.size() + n, s.raw.capacity() * 2), 512 * 1024))
      return s.result = ReflowPreparation::NoMemory;
    const size_t old = s.raw.size();
    s.raw.resize(old + n);
    if (read(s.fd, &s.raw[old], n) != (ssize_t)n)
      return s.result = ReflowPreparation::IoError;
    s.read_bytes += n;
    if (s.read_bytes == s.file_size) {
      close(s.fd);
      s.fd = -1;
      s.phase = State::Header;
    }
  } else if (s.phase == State::Header) {
    if (mobi_parser_core::ParseMobiHeader(s.raw, &s.header) != 0)
      return s.result = ReflowPreparation::Failed;
    mobi_text_decode::ApplyEmbeddedTitle(s.book, s.raw, s.header);
    mobi_record_decode::MobiRecord0Header h;
    h.compression = s.header.compression;
    h.text_len = s.header.text_len;
    h.text_rec_count = s.header.text_rec_count;
    h.trailing_flags = s.header.trailing_flags;
    h.huffcdic_record_index = s.header.huffcdic_record_index;
    h.num_huffcdic_records = s.header.num_huffcdic_records;
    if (!s.merger.Begin(&s.raw, &s.header.offsets, h, &s.merged))
      return s.result = ReflowPreparation::Failed;
    s.phase = State::Merge;
  } else if (s.phase == State::Merge) {
    const int r = s.merger.Step();
    if (r < 0)
      return s.result = r == -2 ? ReflowPreparation::NoMemory
                                : ReflowPreparation::Failed;
    if (!r) {
      if (!s.decoder.Begin(&s.merged, s.header.encoding, &s.utf8))
        return s.result = ReflowPreparation::NoMemory;
      s.phase = State::Decode;
    }
  } else if (s.phase == State::Decode) {
    if (s.decoder.Step()) {
      std::string().swap(s.merged);
      s.book->ClearInlineImages();
      mobi_safe_markup_extract::InlineImageCallbacks images;
      if (s.book->GetEbookImagesEnabled()) {
        images.user_data = s.book;
        images.register_inline_image = [](void *b, const std::string &path) {
          return static_cast<Book *>(b)->RegisterInlineImage(path);
        };
      }
      const bool capture =
          mobi_decode_plan::Build(s.header.text_len ? s.header.text_len
                                                    : s.utf8.size())
              .capture_toc_metadata;
      if (!s.extractor.Begin(&s.utf8, &s.deferred.text_utf8, images,
                             capture ? &s.deferred.html_to_text_map : NULL))
        return s.result = ReflowPreparation::NoMemory;
      s.phase = State::Markup;
    }
    if (s.decoder.NoMemory())
      return s.result = ReflowPreparation::NoMemory;
  } else if (s.phase == State::Markup) {
    if (s.extractor.Step())
      s.phase = State::Cleanup;
    if (s.extractor.NoMemory())
      return s.result = ReflowPreparation::NoMemory;
  } else if (s.phase == State::Cleanup) {
    CleanupDecodedMobiText(s.deps.reporter, &s.deferred.text_utf8,
                           s.deferred.html_to_text_map.empty()
                               ? NULL
                               : &s.deferred.html_to_text_map,
                           s.deferred.line_wrap_fix_applied);
    s.phase = State::TocPrepare;
  } else if (s.phase == State::TocPrepare) {
    s.deferred.text_len_for_pos =
        s.header.text_len ? s.header.text_len : s.utf8.size();
    s.deferred.have_structured_toc = mobi_toc_prepare::Prepare(
        s.raw, s.header.offsets, s.header.ncx_index, s.header.encoding, &s.utf8,
        s.deferred.text_len_for_pos, s.hooks.make_structured_toc_callbacks(),
        s.hooks.make_inline_title_callbacks(), &s.deferred.structured_toc,
        &s.deferred.structured_from_filepos, s.deps.reporter);
    std::string().swap(s.raw);
    std::string().swap(s.utf8);
    s.phase = State::Init;
  } else if (s.phase == State::Init) {
    if (!InitPlainTextStreamStateLocal(s.book, s.deferred.text_utf8, s.deps,
                                       false, &s.deferred.stream))
      return s.result = ReflowPreparation::Failed;
    s.book->MarkMobiRenderSettingsApplied(s.deferred.line_wrap_fix_applied);
    s.phase = State::Pages;
  } else if (s.phase == State::Pages) {
    const size_t before = s.deferred.stream.text_bytes_fed;
    const bool done = plain_text_stream::ContinueState(
        &s.deferred.stream, s.deferred.text_utf8, 20, 8, 0,
        &s.deferred.text_cursor_per_page, NULL,
        s.hooks.make_plain_continue_callbacks());
    if (done)
      s.phase = State::Toc;
    else if (before == s.deferred.stream.text_bytes_fed)
      return s.result = ReflowPreparation::Failed;
  } else if (s.phase == State::Toc) {
    const auto callbacks = s.hooks.make_finalize_callbacks();
    const bool apply =
        s.deferred.have_structured_toc &&
        mobi_toc_finalize_policy::ShouldApplyStructuredToc(
            s.deferred.html_to_text_map, s.deferred.text_cursor_per_page);
    if (apply) {
      if (!s.toc_started) {
        s.fallback = s.book->GetChapters();
        s.book->ClearChapters();
        mobi_toc_apply::BuildCallbacks build = {
            callbacks.normalize_heading_needle,
            callbacks.page_has_heading_needle,
            callbacks.add_chapter_at_page_if_unique};
        if (!s.toc.Begin(s.book, &s.deferred.structured_toc,
                         s.deferred.text_len_for_pos,
                         !s.deferred.structured_from_filepos,
                         &s.deferred.html_to_text_map,
                         &s.deferred.text_cursor_per_page, build))
          return s.result = ReflowPreparation::NoMemory;
        s.toc_started = true;
      }
      if (!s.toc.Step())
        return ReflowPreparation::Pending;
    }
    mobi_toc_finalize::FinalizePreparedToc(
        s.book, s.deps.reporter, s.deferred.structured_toc,
        s.deferred.have_structured_toc, s.deferred.structured_from_filepos,
        s.deferred.heading_hints, s.deferred.text_len_for_pos,
        s.deferred.html_to_text_map, s.deferred.text_cursor_per_page, callbacks,
        NULL, apply ? s.toc.Mapped() : static_cast<size_t>(-1), s.toc.Direct(),
        apply ? &s.fallback : NULL);
    s.book->MarkMobiRenderSettingsApplied(s.deferred.line_wrap_fix_applied);
    s.phase = State::Done;
    s.result = s.book->GetPageCount() ? ReflowPreparation::Complete
                                      : ReflowPreparation::Failed;
  }
  return s.result;
}
const char *Preparation::Label() const {
  if (!state_)
    return "cannot prepare MOBI";
  switch (state_->phase) {
  case State::Read:
    return "reading book... B: cancel";
  case State::Header:
  case State::Merge:
    return "decompressing book... B: cancel";
  case State::Decode:
    return "decoding text... B: cancel";
  case State::Markup:
    return "preparing text... B: cancel";
  case State::Cleanup:
    return "normalizing text... B: cancel";
  case State::Init:
  case State::Pages:
    return "paginating book... B: cancel";
  default:
    return "preparing index... B: cancel";
  }
}
unsigned Preparation::Progress() const {
  if (!state_)
    return 0;
  const State &s = *state_;
  switch (s.phase) {
  case State::Read:
    return s.read_bytes;
  case State::Merge:
    return s.merger.Progress();
  case State::Decode:
    return s.decoder.Progress();
  case State::Markup:
    return s.extractor.Progress();
  case State::Pages:
    return s.deferred.stream.text_bytes_fed;
  case State::Toc:
    return s.toc.Progress();
  default:
    return 0;
  }
}
unsigned Preparation::Total() const {
  if (!state_)
    return 0;
  const State &s = *state_;
  switch (s.phase) {
  case State::Read:
    return s.file_size;
  case State::Merge:
    return s.header.text_rec_count;
  case State::Decode:
    return s.decoder.Total();
  case State::Markup:
    return s.utf8.size();
  case State::Pages:
    return s.deferred.text_utf8.size();
  case State::Toc:
    return s.deferred.structured_toc.size();
  default:
    return 0;
  }
}

} // namespace mobi_parser

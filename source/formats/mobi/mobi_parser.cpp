#include "formats/mobi/mobi_parser.h"

#include "debug_log.h"
#include "formats/common/buffered_status_log.h"
#include "formats/common/book_error.h"
#include "formats/common/plain_text_perf_utils.h"
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
static const u32 kMobiInitialOpenBudgetMs = 320;
static const u16 kMobiInitialOpenPageBudget = 24;
static const u32 kMobiSynchronousBudgetMs = 0;
static const u16 kMobiSynchronousPageBudget = 0;
static const unsigned int kMobiSynchronousPassLimit = 32;

static bool ShouldAbortMobiOpen(Book *book) {
  return book &&
         ((book->GetStatusReporter() &&
           book->GetStatusReporter()->ShouldAbortWork()) ||
          book->IsOpenAbortRequested());
}

#ifdef DSLIBRIS_DEBUG
static void FlushBufferedStatusLog(
    IStatusReporter *reporter, buffered_status_log::BufferedStatusLog *log) {
  if (!reporter || !log)
    return;
  log->Flush(
      [&](const std::string &chunk) { reporter->PrintStatus(chunk.c_str()); });
}
#endif

using plain_text_perf_utils::LogPlainTextStreamPerf;
using plain_text_perf_utils::PlainTextStreamPerf;
using mobi_parser_core::MobiHeaderInfo;
using mobi_toc_finalize::MobiHeadingHint;
using mobi_toc_finalize::MobiTocFinalizeResult;

struct MobiParseState {
  plain_text_stream::State stream;
  std::string source_path;
  std::string markup_utf8;
  std::string text_utf8;
  std::vector<MobiHeadingHint> heading_hints;
  std::vector<mobi_structured_toc_parser::MobiStructuredTocEntry> structured_toc;
  std::vector<std::pair<u32, u32>> html_to_text_map;
  std::vector<u32> text_cursor_per_page;
  bool have_structured_toc;
  bool structured_from_filepos;
  bool toc_metadata_ready;
  bool structured_toc_loaded;
  bool toc_applied;
  bool cache_saved;
  bool used_utf8_guess;
  bool used_legacy_guess;
  bool line_wrap_fix_applied;
  bool finalized;
  u32 text_len_for_pos;
  u64 t_parse_begin;
  u64 t_after_read;
  u64 t_after_decompress;
  u64 t_after_decode;
  u64 t_after_markup_scan;
  u64 t_after_cleanup;
  u64 t_after_initial_pages;
  u64 t_after_markup;
  u64 t_after_pages;
  u64 t_after_toc;

  MobiParseState()
      : have_structured_toc(false), structured_from_filepos(false),
        toc_metadata_ready(false), structured_toc_loaded(false),
        toc_applied(false), cache_saved(false), used_utf8_guess(false),
        used_legacy_guess(false), line_wrap_fix_applied(false), finalized(false),
        text_len_for_pos(0), t_parse_begin(0), t_after_read(0),
        t_after_decompress(0), t_after_decode(0), t_after_markup_scan(0),
        t_after_cleanup(0), t_after_initial_pages(0), t_after_markup(0),
        t_after_pages(0), t_after_toc(0) {}
};

using MobiDeferredState = MobiParseState;

static bool TryLoadMobiPageCache(Book *book, const char *book_path,
                                  const BookParseDeps &deps) {
  if (!book || !book_path || !deps.reporter)
    return false;
  const TextLayoutSnapshot &layout = deps.layout;
  if (layout.pixel_size == 0)
    return false;
  return mobi_page_cache::TryLoad(
      book, book_path, layout.pixel_size, layout.linespacing,
      deps.paragraph_spacing, deps.paragraph_indent, deps.orientation,
      layout.margin_left, layout.margin_right, layout.margin_top,
      layout.margin_bottom, layout.regular_font_path.c_str(),
      book->GetMobiLineWrapFix());
}

static void SaveMobiPageCache(Book *book, const char *book_path,
                               const BookParseDeps &deps,
                               bool line_wrap_fix_enabled) {
  if (!book || !book_path || !deps.reporter ||
      book->GetPageCount() == 0)
    return;
  const TextLayoutSnapshot &layout = deps.layout;
  if (layout.pixel_size == 0)
    return;
#ifdef DSLIBRIS_DEBUG
  DBG_LOGF(deps.reporter, "MOBI: cache-save start pages=%u",
           (unsigned)book->GetPageCount());
#endif
  mobi_page_cache::Save(book, book_path, layout.pixel_size,
                        layout.linespacing, deps.paragraph_spacing,
                        deps.paragraph_indent, deps.orientation,
                        layout.margin_left, layout.margin_right,
                        layout.margin_top, layout.margin_bottom,
                        layout.regular_font_path.c_str(),
                        line_wrap_fix_enabled);
#ifdef DSLIBRIS_DEBUG
  DBG_LOGF(deps.reporter, "MOBI: cache-save done");
#endif
}

static std::string DecodeMobiBytesToUtf8(const std::string &in, u32 encoding,
                                         bool *used_utf8_guess,
                                         bool *used_legacy_guess) {
  return mobi_text_decode::DecodeBytesToUtf8(in, encoding, used_utf8_guess,
                                             used_legacy_guess);
}

// noinline: keeps this frame separate from ParseFile's.
// parsedata_t is ~16KB; we initialize out->parsedata in-place (already on the
// heap inside State) to avoid any local copy on the stack.
static __attribute__((noinline)) bool InitPlainTextStreamStateLocal(Book *book,
                                          const std::string &text_utf8,
                                          const BookParseDeps &deps,
                                          bool detect_heuristic_headings,
                                          plain_text_stream::State *out) {
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
  plain_text_stream::InitState(out, out->parsedata, text_utf8, detect_heuristic_headings);
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
    *image_id_out = (u16)(((u8)text[image_pos + 1] << 8) |
                          (u8)text[image_pos + 2]);
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

struct MobiDecodedText {
  std::string utf8;
  std::string text;
  std::vector<MobiHeadingHint> heading_hints;
  std::vector<std::pair<u32, u32>> html_to_text_map;
  bool toc_metadata_ready;
  bool used_utf8_guess;
  bool used_legacy_guess;

  MobiDecodedText()
      : toc_metadata_ready(false), used_utf8_guess(false),
        used_legacy_guess(false) {}
};

static void CleanupDecodedMobiText(IStatusReporter *reporter, std::string *text,
                                   std::vector<std::pair<u32, u32>> *html_map,
                                   bool line_wrap_fix_applied) {
  if (!text)
    return;

  const size_t text_pre_cleanup = text->size();
  const bool have_map = html_map && !html_map->empty();
  std::string text_before_cleanup;
  if (have_map)
    text_before_cleanup = *text;

#ifdef DSLIBRIS_DEBUG
  if (reporter)
    DBG_LOGF(reporter,
             "MOBI cleanup: step=normalize-begin bytes=%u have_map=%d wrap=%d mem=%u",
             (unsigned)text->size(), have_map ? 1 : 0,
             line_wrap_fix_applied ? 1 : 0,
             (unsigned)osGetMemRegionFree(MEMREGION_ALL));
#endif

  NormalizeNewlines(text);

#ifdef DSLIBRIS_DEBUG
  if (reporter)
    DBG_LOGF(reporter, "MOBI cleanup: step=normalize-done bytes=%u mem=%u",
             (unsigned)text->size(),
             (unsigned)osGetMemRegionFree(MEMREGION_ALL));
#endif

  if (!mobi_cleanup_policy::ShouldRunPostNormalizeCleanup(
          have_map, line_wrap_fix_applied)) {
#ifdef DSLIBRIS_DEBUG
    if (reporter)
      DBG_LOGF(reporter, "MOBI cleanup: step=post-normalize-skipped mem=%u",
               (unsigned)osGetMemRegionFree(MEMREGION_ALL));
#endif
    return;
  }

  const bool track_image_tokens = HasMobiInlineImageTokens(*text);
  std::string text_before_mobi_cleanup;
  std::vector<u16> image_ids_before_cleanup;
  if (track_image_tokens) {
    text_before_mobi_cleanup = *text;
    CollectMobiInlineImageTokenIds(*text, &image_ids_before_cleanup);
  }

#ifdef DSLIBRIS_DEBUG
  if (reporter)
    DBG_LOGF(reporter, "MOBI cleanup: step=mojibake-begin track=%d mem=%u",
             track_image_tokens ? 1 : 0,
             (unsigned)osGetMemRegionFree(MEMREGION_ALL));
#endif

  if (track_image_tokens) {
    *text = mobi_text_cleanup::RepairCommonMojibakePreservingMobiImageTokens(
        *text);
  } else {
    *text = mobi_text_cleanup::RepairCommonMojibake(*text);
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
      *text = text_before_mobi_cleanup;
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

static void BuildMobiTocMetadataFromUtf8(
    Book *book, const BookParseDeps &deps, const std::string &utf8,
    bool line_wrap_fix_applied, std::vector<MobiHeadingHint> *heading_hints,
    std::vector<std::pair<u32, u32>> *html_to_text_map,
    const Hooks &hooks) {
  if (!book || !heading_hints || !html_to_text_map || !hooks.extract_markup_to_text)
    return;

  heading_hints->clear();
  html_to_text_map->clear();

  std::string text = hooks.extract_markup_to_text(book, deps, utf8,
                                                  heading_hints, html_to_text_map);
  CleanupDecodedMobiText(deps.reporter, &text, html_to_text_map,
                         line_wrap_fix_applied);
}

static void DecodeAndCleanupMobiText(Book *book, const BookParseDeps &deps,
                                     const MobiHeaderInfo &header,
                                     const std::string &merged,
                                     bool collect_toc_metadata,
                                     MobiDecodedText *decoded,
                                     u64 *t_after_decode,
                                     u64 *t_after_markup_scan,
                                     u64 *t_after_cleanup,
                                     u64 *t_after_markup,
                                     const Hooks &hooks) {
  if (!decoded || !hooks.extract_markup_to_text)
    return;

  IStatusReporter *reporter = deps.reporter;
#ifdef DSLIBRIS_DEBUG
  if (reporter)
    DBG_LOGF(reporter,
             "MOBI decode: step=utf8-begin merged_bytes=%u mem_free=%u",
             (unsigned)merged.size(),
             (unsigned)osGetMemRegionFree(MEMREGION_ALL));
#endif

  decoded->utf8 = DecodeMobiBytesToUtf8(merged, header.encoding,
                                        &decoded->used_utf8_guess,
                                        &decoded->used_legacy_guess);
  if (t_after_decode)
    *t_after_decode = osGetTime();

#ifdef DSLIBRIS_DEBUG
  if (reporter)
    DBG_LOGF(reporter,
             "MOBI decode: step=utf8-done utf8_bytes=%u mem_free=%u",
             (unsigned)decoded->utf8.size(),
             (unsigned)osGetMemRegionFree(MEMREGION_ALL));
#endif

  book->ClearInlineImages();

#ifdef DSLIBRIS_DEBUG
  if (reporter)
    DBG_LOGF(reporter,
             "MOBI decode: step=markup-extract-begin mem_free=%u",
             (unsigned)osGetMemRegionFree(MEMREGION_ALL));
#endif

  decoded->text =
      hooks.extract_markup_to_text(book, deps, decoded->utf8,
                                   collect_toc_metadata ? &decoded->heading_hints
                                                        : NULL,
                                   collect_toc_metadata
                                       ? &decoded->html_to_text_map
                                       : NULL);
  if (t_after_markup_scan)
    *t_after_markup_scan = osGetTime();

#ifdef DSLIBRIS_DEBUG
  if (reporter)
    DBG_LOGF(reporter,
             "MOBI decode: step=markup-extract-done text_bytes=%u mem_free=%u",
             (unsigned)decoded->text.size(),
             (unsigned)osGetMemRegionFree(MEMREGION_ALL));
#endif

#ifdef DSLIBRIS_DEBUG
  if (reporter)
    DBG_LOGF(reporter, "MOBI decode: step=cleanup-call-begin mem_free=%u",
             (unsigned)osGetMemRegionFree(MEMREGION_ALL));
#endif

  CleanupDecodedMobiText(deps.reporter, &decoded->text,
                         collect_toc_metadata ? &decoded->html_to_text_map
                                             : NULL,
                         book->GetMobiLineWrapFix());

#ifdef DSLIBRIS_DEBUG
  if (reporter)
    DBG_LOGF(reporter, "MOBI decode: step=cleanup-call-returned mem_free=%u",
             (unsigned)osGetMemRegionFree(MEMREGION_ALL));
#endif

  if (t_after_cleanup)
    *t_after_cleanup = osGetTime();
  decoded->toc_metadata_ready = collect_toc_metadata;

#ifdef DSLIBRIS_DEBUG
  if (reporter)
    DBG_LOGF(reporter,
             "MOBI decode: step=cleanup-done text_bytes=%u mem_free=%u",
             (unsigned)decoded->text.size(),
             (unsigned)osGetMemRegionFree(MEMREGION_ALL));
#endif

  if (t_after_markup)
    *t_after_markup = osGetTime();
}

static void PrepareMobiDeferredState(const char *path,
                                     const MobiHeaderInfo &header,
                                     size_t merged_size, u64 t_parse_begin,
                                     u64 t_after_read, u64 t_after_decompress,
                                     u64 t_after_decode,
                                     u64 t_after_markup_scan,
                                     u64 t_after_cleanup, u64 t_after_markup,
                                     bool line_wrap_fix_applied,
                                     bool retain_markup_utf8,
                                     MobiDecodedText *decoded,
                                     MobiDeferredState *deferred) {
  if (!decoded || !deferred)
    return;

  deferred->source_path = path ? path : "";
  if (retain_markup_utf8)
    deferred->markup_utf8.swap(decoded->utf8);
  else
    deferred->markup_utf8.clear();
  deferred->text_utf8.swap(decoded->text);
  deferred->heading_hints.swap(decoded->heading_hints);
  deferred->html_to_text_map.swap(decoded->html_to_text_map);
  deferred->toc_metadata_ready = decoded->toc_metadata_ready;
  deferred->structured_toc_loaded = false;
  deferred->toc_applied = false;
  deferred->cache_saved = false;
  deferred->used_utf8_guess = decoded->used_utf8_guess;
  deferred->used_legacy_guess = decoded->used_legacy_guess;
  deferred->line_wrap_fix_applied = line_wrap_fix_applied;
  deferred->finalized = false;
  deferred->text_len_for_pos =
      (header.text_len > 0) ? header.text_len : (u32)merged_size;
  deferred->t_parse_begin = t_parse_begin;
  deferred->t_after_read = t_after_read;
  deferred->t_after_decompress = t_after_decompress;
  deferred->t_after_decode = t_after_decode;
  deferred->t_after_markup_scan = t_after_markup_scan;
  deferred->t_after_cleanup = t_after_cleanup;
  deferred->t_after_initial_pages = 0;
  deferred->t_after_markup = t_after_markup;
  deferred->t_after_pages = 0;
  deferred->t_after_toc = 0;
}

static bool StartInitialMobiPagination(Book *book, const BookParseDeps &deps,
                                       MobiDeferredState *deferred,
                                       bool *pages_done_initial,
                                       const Hooks &hooks) {
  if (!deferred || !pages_done_initial || !hooks.make_plain_continue_callbacks)
    return false;
  if (!InitPlainTextStreamStateLocal(book, deferred->text_utf8, deps, false,
                                     &deferred->stream)) {
    return false;
  }
  book->MarkMobiRenderSettingsApplied(deferred->line_wrap_fix_applied);
  PlainTextStreamPerf perf;
  const plain_text_stream::ContinueCallbacks callbacks =
      hooks.make_plain_continue_callbacks();
  *pages_done_initial = plain_text_stream::ContinueState(
      &deferred->stream, deferred->text_utf8, kMobiInitialOpenBudgetMs,
      kMobiInitialOpenPageBudget, 1, &deferred->text_cursor_per_page, &perf,
      callbacks);
#ifdef DSLIBRIS_DEBUG
  LogPlainTextStreamPerf(deps.reporter, "PLAIN-MOBI initial", perf,
                         *pages_done_initial);
#endif
  deferred->t_after_initial_pages = osGetTime();
  deferred->t_after_pages = deferred->t_after_initial_pages;
  return true;
}

static void FinalizeImmediateMobiParse(Book *book, const char *path,
                                       const BookParseDeps &deps,
                                       const std::string &raw,
                                       const MobiHeaderInfo &header,
                                       const std::string &utf8,
                                       MobiDeferredState *deferred,
                                       MobiTocFinalizeResult *toc_result,
                                       const Hooks &hooks) {
  if (!book || !deferred || !hooks.make_structured_toc_callbacks ||
      !hooks.make_inline_title_callbacks || !hooks.make_finalize_callbacks)
    return;

  IStatusReporter *reporter = deps.reporter;
  MobiTocFinalizeResult local_result;
  if (!toc_result)
    toc_result = &local_result;

  if (!deferred->toc_metadata_ready) {
    const std::string &markup_source =
        !deferred->markup_utf8.empty() ? deferred->markup_utf8 : utf8;
    BuildMobiTocMetadataFromUtf8(book, deps, markup_source,
                                 deferred->line_wrap_fix_applied,
                                 &deferred->heading_hints,
                                 &deferred->html_to_text_map, hooks);
    deferred->toc_metadata_ready = true;
  }

  if (!deferred->have_structured_toc) {
    deferred->have_structured_toc = mobi_toc_prepare::Prepare(
        raw, header.offsets, header.ncx_index, header.encoding, &utf8,
        deferred->text_len_for_pos, hooks.make_structured_toc_callbacks(),
        hooks.make_inline_title_callbacks(), &deferred->structured_toc,
        &deferred->structured_from_filepos, reporter);
  }
  mobi_toc_finalize::FinalizePreparedToc(
      book, reporter, deferred->structured_toc, deferred->have_structured_toc,
      deferred->structured_from_filepos, deferred->heading_hints,
      deferred->text_len_for_pos, deferred->html_to_text_map,
      deferred->text_cursor_per_page, hooks.make_finalize_callbacks(),
      toc_result);
  deferred->t_after_toc = osGetTime();
#ifdef DSLIBRIS_DEBUG
  if (reporter) {
    DBG_LOGF(reporter, "MOBI: post-toc pages=%u chapters=%u",
             (unsigned)book->GetPageCount(),
             (unsigned)book->GetChapters().size());
  }
#endif
  SaveMobiPageCache(book, path, deps, deferred->line_wrap_fix_applied);
  book->MarkMobiRenderSettingsApplied(deferred->line_wrap_fix_applied);
}

} // namespace

u8 ParseFile(Book *book, const char *path, const Hooks &hooks) {
  const Hooks *hooks_ptr = &hooks;
  if (!book || !path || !hooks_ptr || !hooks_ptr->extract_markup_to_text ||
      !hooks_ptr->make_structured_toc_callbacks ||
      !hooks_ptr->make_inline_title_callbacks ||
      !hooks_ptr->make_finalize_callbacks ||
      !hooks_ptr->make_plain_continue_callbacks)
    return 251;
  const BookParseDeps deps = BuildBookParseDeps(book);
  IStatusReporter *reporter = deps.reporter;
  const u64 t_parse_begin = osGetTime();
#ifdef DSLIBRIS_DEBUG
  buffered_status_log::BufferedStatusLog debug_log(768);
  struct DebugLogGuard {
    IStatusReporter *reporter;
    buffered_status_log::BufferedStatusLog *log;
    ~DebugLogGuard() { FlushBufferedStatusLog(reporter, log); }
  } debug_log_guard = {reporter, &debug_log};
  auto append_debug_log = [&](const std::string &line) {
    if (reporter)
      debug_log.Append(line);
  };
  auto log_stage = [&](const char *stage) {
    if (!reporter || !stage)
      return;
    DBG_LOGF(reporter, "MOBI: stage=%s", stage);
  };
#else
  auto append_debug_log = [&](const std::string &) {};
  auto log_stage = [&](const char *) {};
#endif
  log_stage("enter");
  if (reporter)
    append_debug_log("MOBI: parse begin");

  log_stage("after-erase");

  if (TryLoadMobiPageCache(book, path, deps)) {
    log_stage("cache-hit");
    if (reporter) {
      char msg[224];
      snprintf(msg, sizeof(msg), "MOBI: page cache hit pages=%u chapters=%u",
               (unsigned)book->GetPageCount(),
               (unsigned)book->GetChapters().size());
      append_debug_log(msg);

      char tmsg[160];
      snprintf(tmsg, sizeof(tmsg), "MOBI: timing cache_total=%llums",
               (unsigned long long)(osGetTime() - t_parse_begin));
      append_debug_log(tmsg);
      append_debug_log("MOBI: parse end");
    }
    book->MarkMobiRenderSettingsApplied(book->GetMobiLineWrapFix());
    return 0;
  }

  std::string raw;
  u64 t_after_read = 0;
  u8 rc =
      mobi_parser_core::LoadMobiSource(path, &raw, &t_after_read, kMobiMaxBytes);
  if (rc != 0)
    return rc;
  log_stage("source-loaded");
  if (ShouldAbortMobiOpen(book))
    return BOOK_ERR_CANCELLED;
  if (open_cancel_poll::Poll(book, reporter, "mobi-source"))
    return BOOK_ERR_CANCELLED;

  MobiHeaderInfo header;
  rc = mobi_parser_core::ParseMobiHeader(raw, &header);
  if (rc != 0) {
    if (rc == 255 &&
        !mobi_parser_core::IsSupportedCompression(header.compression) &&
        reporter) {
      if (header.compression == 17480)
        reporter->PrintStatus("MOBI: unsupported compression (HUFF/CDIC)");
      else
        reporter->PrintStatus("MOBI: unsupported compression");
    }
    return rc;
  }
  log_stage("header-parsed");
  if (open_cancel_poll::Poll(book, reporter, "mobi-header"))
    return BOOK_ERR_CANCELLED;

  if (reporter) {
    char msg[224];
    snprintf(msg, sizeof(msg),
             "MOBI: header comp=%u enc=%u text_len=%u text_recs=%u "
             "first_non_book=%u ncx=%u",
             (unsigned)header.compression, (unsigned)header.encoding,
             (unsigned)header.text_len, (unsigned)header.text_rec_count,
             (unsigned)header.first_non_book_index, (unsigned)header.ncx_index);
    append_debug_log(msg);
  }

  if (!mobi_parser_core::IsSupportedCompression(header.compression)) {
    if (reporter) {
      if (header.compression == 17480)
        reporter->PrintStatus("MOBI: unsupported compression (HUFF/CDIC)");
      else
        reporter->PrintStatus("MOBI: unsupported compression");
    }
    return 255;
  }

  mobi_text_decode::ApplyEmbeddedTitle(book, raw, header);

  std::string merged;
  if (!mobi_parser_core::BuildMobiMergedText(raw, header, &merged)) {
    if (reporter)
      reporter->PrintStatus("MOBI: failed to decode text records");
    return 255;
  }
  log_stage("text-merged");
  const u64 t_after_decompress = osGetTime();
  if (ShouldAbortMobiOpen(book))
    return BOOK_ERR_CANCELLED;
  if (open_cancel_poll::Poll(book, reporter, "mobi-text"))
    return BOOK_ERR_CANCELLED;

#ifdef DSLIBRIS_DEBUG
  if (reporter)
    DBG_LOGF(reporter,
             "MOBI pre-decode: merged_bytes=%u mem_free=%u",
             (unsigned)merged.size(),
             (unsigned)osGetMemRegionFree(MEMREGION_ALL));
#endif

  MobiDecodedText decoded;
  u64 t_after_decode = 0;
  u64 t_after_markup_scan = 0;
  u64 t_after_cleanup = 0;
  u64 t_after_markup = 0;
  const size_t text_bytes =
      (header.text_len > 0) ? (size_t)header.text_len : merged.size();
  const mobi_decode_plan::Plan decode_plan = mobi_decode_plan::Build(text_bytes);
  DecodeAndCleanupMobiText(
      book, deps, header, merged, decode_plan.capture_toc_metadata, &decoded,
      &t_after_decode, &t_after_markup_scan, &t_after_cleanup, &t_after_markup,
      hooks);
  { std::string().swap(merged); }
  log_stage("text-decoded");
  if (ShouldAbortMobiOpen(book))
    return BOOK_ERR_CANCELLED;
  if (open_cancel_poll::Poll(book, reporter, "mobi-decode"))
    return BOOK_ERR_CANCELLED;

  // parsedata_t::buf[4096] = 16KB inside State; stack-allocating overflows 32KB main stack.
  std::unique_ptr<MobiDeferredState> deferred_uptr(
      new (std::nothrow) MobiDeferredState());
  if (!deferred_uptr)
    return 1;
  MobiDeferredState &deferred = *deferred_uptr;
  PrepareMobiDeferredState(path, header, text_bytes, t_parse_begin,
                           t_after_read, t_after_decompress, t_after_decode,
                           t_after_markup_scan, t_after_cleanup, t_after_markup,
                           book->GetMobiLineWrapFix(),
                           decode_plan.retain_markup_utf8, &decoded, &deferred);
  deferred.have_structured_toc = mobi_toc_prepare::Prepare(
      raw, header.offsets, header.ncx_index, header.encoding, &decoded.utf8,
      deferred.text_len_for_pos, hooks.make_structured_toc_callbacks(),
      hooks.make_inline_title_callbacks(), &deferred.structured_toc,
      &deferred.structured_from_filepos, reporter);
  log_stage("toc-prepared");

  { std::string().swap(raw); }

  bool pages_done_initial = false;
  if (!StartInitialMobiPagination(book, deps, &deferred, &pages_done_initial,
                                  hooks))
    return 1;
  log_stage("initial-pages");
  if (ShouldAbortMobiOpen(book))
    return BOOK_ERR_CANCELLED;
  if (open_cancel_poll::Poll(book, reporter, "mobi-pages-initial"))
    return BOOK_ERR_CANCELLED;

  if (!pages_done_initial) {
    unsigned int pass_count = 0;
    while (!deferred.stream.completed) {
      if (ShouldAbortMobiOpen(book)) {
        return BOOK_ERR_CANCELLED;
      }
      if (open_cancel_poll::Poll(book, reporter, "mobi-deferred-sync")) {
        return BOOK_ERR_CANCELLED;
      }
      if (++pass_count > kMobiSynchronousPassLimit) {
        return 1;
      }
      PlainTextStreamPerf perf;
      const plain_text_stream::ContinueCallbacks callbacks =
          hooks.make_plain_continue_callbacks();
      const bool done = plain_text_stream::ContinueState(
          &deferred.stream, deferred.text_utf8, kMobiSynchronousBudgetMs,
          kMobiSynchronousPageBudget, 0, &deferred.text_cursor_per_page, &perf,
          callbacks);
#ifdef DSLIBRIS_DEBUG
      LogPlainTextStreamPerf(reporter, "PLAIN-MOBI sync", perf, done);
#endif
      if (done)
        deferred.t_after_pages = osGetTime();
    }
    log_stage("sync-pages-done");
    MobiTocFinalizeResult toc_result;
    FinalizeImmediateMobiParse(book, path, deps, raw, header, decoded.utf8,
                               &deferred, &toc_result, hooks);
    log_stage("finalized");
    if (reporter) {
      append_debug_log("MOBI: parse end");
    }
    return book->GetPageCount() > 0 ? 0 : 1;
  }

  MobiTocFinalizeResult toc_result;
  FinalizeImmediateMobiParse(book, path, deps, raw, header, decoded.utf8,
                             &deferred, &toc_result, hooks);
  log_stage("finalized");

  if (reporter) {
    char msg[320];
    snprintf(
        msg, sizeof(msg),
        "MOBI: text bytes=%u headings=%u mapped=%u structured=%u direct=%u "
        "chapters=%u guess_utf8=%u guess_legacy=%u filepos_toc=%u",
        (unsigned)deferred.text_utf8.size(),
        (unsigned)deferred.heading_hints.size(),
        (unsigned)toc_result.mapped_chapters,
        (unsigned)toc_result.structured_entries,
        (unsigned)toc_result.structured_direct,
        (unsigned)book->GetChapters().size(), deferred.used_utf8_guess ? 1u : 0u,
        deferred.used_legacy_guess ? 1u : 0u,
        toc_result.structured_from_filepos ? 1u : 0u);
    append_debug_log(msg);

    char tmsg[320];
    snprintf(
        tmsg, sizeof(tmsg),
        "MOBI: timing read=%llums decomp=%llums decode=%llums "
        "markup_scan=%llums cleanup=%llums initial_pages=%llums "
        "deferred_pages=%llums deferred_toc=%llums total=%llums",
        (unsigned long long)(deferred.t_after_read - deferred.t_parse_begin),
        (unsigned long long)(deferred.t_after_decompress -
                             deferred.t_after_read),
        (unsigned long long)(deferred.t_after_decode -
                             deferred.t_after_decompress),
        (unsigned long long)(deferred.t_after_markup_scan -
                             deferred.t_after_decode),
        (unsigned long long)(deferred.t_after_cleanup -
                             deferred.t_after_markup_scan),
        (unsigned long long)(deferred.t_after_initial_pages -
                             deferred.t_after_cleanup),
        (unsigned long long)(deferred.t_after_pages -
                             deferred.t_after_initial_pages),
        (unsigned long long)(deferred.t_after_toc - deferred.t_after_pages),
        (unsigned long long)(deferred.t_after_toc - deferred.t_parse_begin));
    append_debug_log(tmsg);
  }

  if (reporter)
    append_debug_log("MOBI: parse end");
  return 0;
}

} // namespace mobi_parser

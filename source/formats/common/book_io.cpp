/*
    3dslibris - book_io.cpp
    New module for the 3DS port by Rigle.

    Summary:
    - Input/output and parser dispatch for non-EPUB formats.
    - UTF-8 normalization and encoding repair utilities.
    - TXT/RTF/ODT loading, extraction, and chapter/index helper generation.
*/

#include "book/book.h"
#include "book/book_parse_deps.h"

#include "shared/status_reporter.h"
#include "book/book_xml.h"
#include "formats/common/book_error.h"
#include "debug_log.h"
#include "book/heading_layout.h"
#include "formats/common/plain_text_perf_utils.h"
#include "formats/mobi/mobi.h"
#include "formats/mobi/mobi_deferred_runtime.h"
#include "formats/mobi/mobi_markup_extract.h"
#include "formats/mobi/mobi_parser.h"
#include "formats/mobi/mobi_toc_finalize.h"
#include "formats/mobi/mobi_toc_prepare.h"
#include "formats/mobi/mobi_text_decode.h"
#include "formats/pdf/pdf.h"
#include "formats/cbz/cbz.h"
#include "book/page.h"
#include "formats/common/page_cache_utils.h"
#include "formats/common/plain_text_stream.h"
#include "formats/common/page_text_extract_utils.h"
#include "formats/common/xml_parse_utils.h"
#include "parse.h"
#include "formats/common/text_helpers.h"
#include "formats/rtf/rtf_loader.h"
#include "formats/txt/txt_loader.h"
#include "formats/odt/odt_loader.h"
#include "shared/parser_limits.h"
#include "string_utils.h"
#include "shared/utf8_utils.h"
#include <algorithm>
#include <ctype.h>
#include <set>
#include <stdio.h>
#include <string.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <utility>
#include <vector>

namespace {

static const size_t kPlainTextMaxBytes = 12 * 1024 * 1024;

typedef BookParseDeps BookIoDeps;
using plain_text_perf_utils::CapturePlainTextPerfBaseline;
using plain_text_perf_utils::FillPlainTextStreamPerf;
using plain_text_perf_utils::LogPlainTextStreamPerf;
using plain_text_perf_utils::PlainTextPerfBaseline;
using plain_text_perf_utils::PlainTextStreamPerf;
using mobi_toc_finalize::MobiHeadingHint;

static BookIoDeps BuildBookIoDeps(Book *book) { return BuildBookParseDeps(book); }

static void InitParsedataWithBookIoDeps(parsedata_t *parsedata, Book *book,
                                        const BookIoDeps &deps) {
  if (!parsedata)
    return;
  parse_init(parsedata);
  parsedata->reporter = deps.reporter;
  parsedata->ts = deps.ts;
  parsedata->prefs = deps.prefs;
  parsedata->book = book;
}

static std::string DecodeMobiBytesToUtf8(const std::string &in, u32 encoding,
                                         bool *used_utf8_guess,
                                         bool *used_legacy_guess) {
  return mobi_text_decode::DecodeBytesToUtf8(in, encoding, used_utf8_guess,
                                             used_legacy_guess);
}

static std::string TrimAsciiWhitespace(const std::string &in) {
  size_t start = 0;
  while (start < in.size() && isspace((unsigned char)in[start]))
    start++;
  size_t end = in.size();
  while (end > start && isspace((unsigned char)in[end - 1]))
    end--;
  return in.substr(start, end - start);
}

static std::string CollapseAsciiWhitespace(const std::string &in) {
  std::string out;
  out.reserve(in.size());
  bool pending_space = false;
  for (size_t i = 0; i < in.size(); i++) {
    unsigned char c = (unsigned char)in[i];
    if (isspace(c)) {
      pending_space = true;
      continue;
    }
    if (pending_space && !out.empty())
      out.push_back(' ');
    pending_space = false;
    out.push_back((char)c);
  }
  return out;
}

static std::string FoldLatinForMatch(const std::string &in) {
  std::string out;
  out.reserve(in.size());
  for (size_t i = 0; i < in.size(); i++) {
    unsigned char c = (unsigned char)in[i];
    if (c < 0x80) {
      out.push_back((char)tolower(c));
      continue;
    }
    if (c == 0xC3 && i + 1 < in.size()) {
      unsigned char c2 = (unsigned char)in[i + 1];
      switch (c2) {
      case 0x81:
      case 0xA1:
        out.push_back('a');
        i++;
        continue;
      case 0x89:
      case 0xA9:
        out.push_back('e');
        i++;
        continue;
      case 0x8D:
      case 0xAD:
        out.push_back('i');
        i++;
        continue;
      case 0x93:
      case 0xB3:
        out.push_back('o');
        i++;
        continue;
      case 0x9A:
      case 0xBA:
      case 0x9C:
      case 0xBC:
        out.push_back('u');
        i++;
        continue;
      case 0x91:
      case 0xB1:
        out.push_back('n');
        i++;
        continue;
      case 0x87:
      case 0xA7:
        out.push_back('c');
        i++;
        continue;
      default:
        break;
      }
    }
  }
  return out;
}

static bool StartsWithChapterPrefix(const std::string &folded) {
  static const char *kPrefixes[] = {
      "chapter", "capitulo", "parte", "part",
      "seccion", "section",  "book",  "libro",
  };
  for (size_t i = 0; i < sizeof(kPrefixes) / sizeof(kPrefixes[0]); i++) {
    const char *prefix = kPrefixes[i];
    size_t len = strlen(prefix);
    if (folded.size() < len || folded.compare(0, len, prefix) != 0)
      continue;
    if (folded.size() == len)
      return true;
    char next = folded[len];
    if (next == ' ' || next == ':' || next == '-' || next == '.' ||
        next == ')' || isdigit((unsigned char)next))
      return true;
  }
  return false;
}

static bool IsRomanHeadingToken(const std::string &token) {
  if (token.empty() || token.size() > 8)
    return false;
  for (size_t i = 0; i < token.size(); i++) {
    char c = token[i];
    if (c != 'i' && c != 'v' && c != 'x' && c != 'l' && c != 'c' && c != 'd' &&
        c != 'm')
      return false;
  }
  return true;
}

static std::string TrimRomanTokenSuffix(const std::string &token) {
  size_t end = token.size();
  while (end > 0) {
    char c = token[end - 1];
    if (c == '.' || c == ')' || c == ':' || c == '-')
      end--;
    else
      break;
  }
  return token.substr(0, end);
}

static bool IsUpperAsciiRomanToken(const std::string &token) {
  std::string clean = TrimRomanTokenSuffix(token);
  if (clean.empty() || clean.size() > 8)
    return false;
  for (size_t i = 0; i < clean.size(); i++) {
    char c = clean[i];
    if (c != 'I' && c != 'V' && c != 'X' && c != 'L' && c != 'C' && c != 'D' &&
        c != 'M')
      return false;
  }
  return true;
}

static bool LooksLikeTocLeaderLine(const std::string &folded) {
  if (folded.find("...") == std::string::npos &&
      folded.find(" . .") == std::string::npos)
    return false;

  int i = (int)folded.size() - 1;
  while (i >= 0 && isspace((unsigned char)folded[(size_t)i]))
    i--;
  int digits = 0;
  while (i >= 0 && isdigit((unsigned char)folded[(size_t)i])) {
    digits++;
    i--;
  }
  return digits > 0;
}

static int CountAsciiWords(const std::string &line) {
  int words = 0;
  bool in_word = false;
  for (size_t i = 0; i < line.size(); i++) {
    unsigned char c = (unsigned char)line[i];
    if (isspace(c)) {
      in_word = false;
      continue;
    }
    if (!in_word) {
      words++;
      in_word = true;
    }
  }
  return words;
}

static bool EndsWithStandaloneDigits(const std::string &folded,
                                     int *digits_out) {
  if (digits_out)
    *digits_out = 0;
  if (folded.empty())
    return false;

  int i = (int)folded.size() - 1;
  while (i >= 0 && isspace((unsigned char)folded[(size_t)i]))
    i--;
  if (i < 0)
    return false;

  int digits = 0;
  while (i >= 0 && isdigit((unsigned char)folded[(size_t)i])) {
    digits++;
    i--;
  }
  if (digits == 0)
    return false;
  if (digits_out)
    *digits_out = digits;

  if (i < 0)
    return false;
  return isspace((unsigned char)folded[(size_t)i]) != 0;
}

static bool LooksLikeFalsePositiveHeading(const std::string &compact,
                                          const std::string &folded,
                                          bool strong_signal) {
  if (compact.empty())
    return true;

  if (folded == "contents" || folded == "table of contents" ||
      folded == "indice" || folded == "indice de contenido" ||
      folded == "contenido")
    return true;

  if (LooksLikeTocLeaderLine(folded))
    return true;

  if (folded.find('?') != std::string::npos ||
      folded.find('!') != std::string::npos ||
      folded.find(';') != std::string::npos)
    return true;

  int trailing_digits = 0;
  if (!strong_signal && EndsWithStandaloneDigits(folded, &trailing_digits) &&
      trailing_digits >= 1 && trailing_digits <= 4 &&
      CountAsciiWords(compact) >= 3) {
    // Common "title .... page" / "title page" in printed TOCs.
    return true;
  }

  return false;
}

static bool LooksLikePlainChapterHeading(const std::string &line,
                                         bool *strong_signal_out) {
  if (strong_signal_out)
    *strong_signal_out = false;

  std::string trimmed = TrimAsciiWhitespace(line);
  if (trimmed.size() < 4 || trimmed.size() > 120)
    return false;

  std::string compact = CollapseAsciiWhitespace(trimmed);
  std::string folded = FoldLatinForMatch(compact);
  if (folded.size() < 4)
    return false;
  if (StartsWithChapterPrefix(folded)) {
    if (strong_signal_out)
      *strong_signal_out = true;
    return true;
  }

  size_t split = folded.find(' ');
  std::string first =
      split == std::string::npos ? folded : folded.substr(0, split);
  std::string rest = split == std::string::npos ? "" : folded.substr(split + 1);

  size_t split_compact = compact.find(' ');
  std::string first_compact = split_compact == std::string::npos
                                  ? compact
                                  : compact.substr(0, split_compact);
  std::string rest_compact = split_compact == std::string::npos
                                 ? ""
                                 : compact.substr(split_compact + 1);

  if (IsRomanHeadingToken(first) && IsUpperAsciiRomanToken(first_compact) &&
      !TrimAsciiWhitespace(rest).empty() &&
      !TrimAsciiWhitespace(rest_compact).empty()) {
    if (strong_signal_out)
      *strong_signal_out = true;
    return true;
  }

  size_t p = 0;
  while (p < folded.size() && isdigit((unsigned char)folded[p]))
    p++;
  if (p > 0 && p < folded.size() &&
      (folded[p] == '.' || folded[p] == ')' || folded[p] == '-') &&
      p + 1 < folded.size() && folded[p + 1] == ' ') {
    if (strong_signal_out)
      *strong_signal_out = true;
    return true;
  }

  return false;
}

static bool IsBlankLine(const std::string &line) {
  return TrimAsciiWhitespace(line).empty();
}

static bool ShouldAcceptHeuristicHeading(const std::string &line,
                                         bool prev_blank, bool next_blank,
                                         bool prev_candidate,
                                         bool next_candidate,
                                         bool strong_signal) {
  std::string compact = CollapseAsciiWhitespace(TrimAsciiWhitespace(line));
  std::string folded = FoldLatinForMatch(compact);
  if (LooksLikeFalsePositiveHeading(compact, folded, strong_signal))
    return false;

  if (prev_blank || next_blank)
    return true;

  // Dense clusters of candidate lines are usually in-book printed TOCs.
  if (prev_candidate || next_candidate)
    return false;

  // Allow compact chapter-like headings even without blank separators.
  return strong_signal && CountAsciiWords(compact) <= 8;
}

static void AddChapterAtPageIfUnique(Book *book, u16 page,
                                     const std::string &title, u8 level) {
  if (!book)
    return;
  std::string clean = CollapseAsciiWhitespace(TrimAsciiWhitespace(title));
  if (clean.empty())
    return;
  if (book->GetChapters().size() >= 512)
    return;

  const std::vector<ChapterEntry> &chapters = book->GetChapters();
  if (!chapters.empty()) {
    const ChapterEntry &last = chapters.back();
    if (last.page == page && last.title == clean && last.level == level)
      return;
  }
  book->AddChapter(page, clean, level);
}

static bool IsMostlyDigitsOrPunctuation(const std::string &s) {
  std::string t = CollapseAsciiWhitespace(TrimAsciiWhitespace(s));
  if (t.empty())
    return true;
  int alpha = 0;
  int total = 0;
  for (size_t i = 0; i < t.size(); i++) {
    unsigned char c = (unsigned char)t[i];
    if (c < 0x80) {
      if (isalnum(c))
        total++;
      if (isalpha(c))
        alpha++;
      continue;
    }
    // Non-ASCII UTF-8 lead byte -> treat as alpha-like token content.
    if ((c & 0xC0) == 0xC0) {
      alpha++;
      total++;
    }
  }
  if (total == 0)
    return true;
  return alpha <= 1;
}

static size_t PruneMobiFrontMatterTocCluster(Book *book, IStatusReporter *reporter) {
  if (!book)
    return 0;
  const std::vector<ChapterEntry> &chapters = book->GetChapters();
  if (chapters.size() < 20)
    return 0;

  const u16 page_count = book->GetPageCount();
  if (page_count < 220)
    return 0;

  // In large MOBI books, printed "Table of Contents" pages can be mapped as
  // dozens of pseudo-chapters concentrated near the beginning.
  u16 front_page_limit = page_count / 18;
  if (front_page_limit < 28)
    front_page_limit = 28;
  if (front_page_limit > 96)
    front_page_limit = 96;

  size_t prefix_count = 0;
  std::set<u16> prefix_unique_pages;
  for (size_t i = 0; i < chapters.size(); i++) {
    if (chapters[i].page > front_page_limit)
      break;
    prefix_count++;
    prefix_unique_pages.insert(chapters[i].page);
  }

  if (prefix_count < 12)
    return 0;
  if (prefix_count + 8 >= chapters.size())
    return 0; // Avoid pruning when almost everything is in front matter.

  const size_t uniq = prefix_unique_pages.size();
  const bool highly_dense = (uniq * 100 < prefix_count * 55);
  const bool large_prefix = (prefix_count * 100 >= chapters.size() * 35);
  if (!highly_dense || !large_prefix)
    return 0;

  std::vector<ChapterEntry> kept;
  kept.reserve(chapters.size() - prefix_count);
  for (size_t i = prefix_count; i < chapters.size(); i++)
    kept.push_back(chapters[i]);

  if (kept.size() < 6)
    return 0;

  book->ClearChapters();
  for (size_t i = 0; i < kept.size(); i++)
    book->AddChapter(kept[i].page, kept[i].title, kept[i].level);

  if (reporter) {
    char msg[224];
    snprintf(
        msg, sizeof(msg),
        "MOBI: TOC front-matter pruned removed=%u remain=%u front_limit<=%u",
        (unsigned)prefix_count, (unsigned)kept.size(),
        (unsigned)front_page_limit);
    DBG_LOG(reporter, msg);
  }
  return prefix_count;
}

static bool LooksLikeStructuredMobiChapterTitle(const std::string &title) {
  std::string compact = CollapseAsciiWhitespace(TrimAsciiWhitespace(title));
  if (compact.empty())
    return false;
  std::string folded = FoldLatinForMatch(compact);
  if (StartsWithChapterPrefix(folded))
    return true;

  size_t sp = compact.find(' ');
  std::string first =
      (sp == std::string::npos) ? compact : compact.substr(0, sp);
  std::string first_folded =
      (sp == std::string::npos) ? folded : folded.substr(0, folded.find(' '));
  if (IsUpperAsciiRomanToken(first) && IsRomanHeadingToken(first_folded))
    return true;

  size_t p = 0;
  while (p < folded.size() && isdigit((unsigned char)folded[p]))
    p++;
  if (p > 0 && p < folded.size() &&
      (folded[p] == '.' || folded[p] == ')' || folded[p] == '-') &&
      p + 1 < folded.size() && folded[p + 1] == ' ')
    return true;

  return false;
}

static bool IsMobiHeuristicChapterSetNoisy(Book *book,
                                           mobi_toc_finalize::MobiChapterQualityStats *stats_out) {
  if (!book)
    return false;
  const std::vector<ChapterEntry> &chapters = book->GetChapters();
  if (chapters.size() < 8)
    return false;

  const u16 page_count = book->GetPageCount();
  if (page_count == 0)
    return false;

  u16 early_window = page_count / 12; // ~first 8%
  if (early_window < 12)
    early_window = 12;
  if (early_window > 96)
    early_window = 96;

  size_t early_hits = 0;
  size_t tiny_titles = 0;
  size_t noisy_titles = 0;
  size_t structured_titles = 0;
  std::set<u16> unique_pages;
  for (size_t i = 0; i < chapters.size(); i++) {
    const ChapterEntry &c = chapters[i];
    unique_pages.insert(c.page);
    if (c.page <= early_window)
      early_hits++;
    std::string clean = CollapseAsciiWhitespace(TrimAsciiWhitespace(c.title));
    if (clean.size() < 4)
      tiny_titles++;
    if (IsMostlyDigitsOrPunctuation(clean))
      noisy_titles++;
    if (LooksLikeStructuredMobiChapterTitle(clean))
      structured_titles++;
  }

  if (stats_out) {
    stats_out->chapters = chapters.size();
    stats_out->unique_pages = unique_pages.size();
    stats_out->early_hits = early_hits;
    stats_out->tiny_titles = tiny_titles;
    stats_out->noisy_titles = noisy_titles;
    stats_out->structured_titles = structured_titles;
    stats_out->early_window = early_window;
  }

  const size_t n = chapters.size();
  const bool mostly_early = (early_hits * 100 >= n * 65);
  const bool low_spread = (unique_pages.size() * 100 < n * 35);
  const bool mostly_noisy_titles =
      ((tiny_titles + noisy_titles) * 100 >= n * 55);
  const bool too_unstructured = (structured_titles * 100 < n * 35);
  return mostly_early || low_spread || mostly_noisy_titles || too_unstructured;
}

static void AddChapterIfUnique(Book *book, const std::string &title, u8 level) {
  if (!book)
    return;
  AddChapterAtPageIfUnique(book, book->GetPageCount(), title, level);
}

static void SetNonEpubTocConfidence(Book *book, bool strong) {
  if (!book)
    return;
  size_t n = book->GetChapters().size();
  if (n == 0) {
    book->ClearTocConfidence();
    return;
  }
  u16 count = (n > 65535) ? 65535 : (u16)n;
  if (strong)
    book->SetTocConfidence(TOC_QUALITY_STRONG, count, 0, 0);
  else
    book->SetTocConfidence(TOC_QUALITY_HEURISTIC, 0, count, 0);
}

static void FinalizePlainPage(parsedata_t *p) {
  if (!p || !p->book)
    return;
  if (p->buflen > 0 || p->book->GetPageCount() == 0) {
    Page *page = p->book->AppendPage();
    page->SetBuffer(p->buf, p->buflen);
  }
}

typedef plain_text_stream::State PlainTextStreamState;

static bool InitPlainTextStreamState(Book *book, const std::string &text_utf8,
                                     const BookIoDeps &deps,
                                     bool detect_heuristic_headings,
                                     PlainTextStreamState *out) {
  if (!book || !deps.ts || !out)
    return false;

  parsedata_t base{};
  InitParsedataWithBookIoDeps(&base, book, deps);
  plain_text_stream::InitState(out, base, text_utf8, detect_heuristic_headings);
  return true;
}

static void PlainAdvanceScreen(parsedata_t *p) {
  if (!p || !p->ts || !p->book)
    return;

  Text *ts = p->ts;
  if (p->screen == 1) {
    Page *page = p->book->AppendPage();
    page->SetBuffer(p->buf, p->buflen);
    parse_reset_page_buffer(p);
    if (p->italic)
      parse_append_page_byte(p, TEXT_ITALIC_ON);
    if (p->bold)
      parse_append_page_byte(p, TEXT_BOLD_ON);
    p->screen = 0;
  } else {
    p->screen = 1;
  }

  p->pen.x = ts->margin.left;
  p->pen.y = ts->margin.top + ts->GetHeight();
  p->linebegan = false;
}

static bool PlainForceAdvancePageForBufferLimit(parsedata_t *p, void *ctx) {
  (void)ctx;
  if (!p || !p->ts || !p->book)
    return false;

  Text *ts = p->ts;
  Page *page = p->book->AppendPage();
  page->SetBuffer(p->buf, p->buflen);
  parse_reset_page_buffer(p);
  if (p->italic)
    parse_append_page_byte(p, TEXT_ITALIC_ON);
  if (p->bold)
    parse_append_page_byte(p, TEXT_BOLD_ON);
  p->screen = 0;
  p->pen.x = ts->margin.left;
  p->pen.y = ts->margin.top + ts->GetHeight();
  p->linebegan = false;
  return true;
}

static void PlainLinefeed(parsedata_t *p) {
  if (!p || !p->ts)
    return;
  parse_append_page_byte(p, '\n');
  p->pen.x = p->ts->margin.left;
  p->pen.y += p->ts->GetHeight() + p->ts->linespacing;
  p->linebegan = false;
}

static bool ApplyPlainHeadingKeepWithNext(parsedata_t *p, int heading_level) {
  if (!p || !p->ts)
    return false;
  if (heading_level < 1 || heading_level > 3)
    return false;

  heading_layout::KeepWithNextRequest req{};
  req.pen_y = p->pen.y;
  req.screen_height = (p->screen == 1) ? 320 : 400;
  req.bottom_margin =
      (p->screen == 1) ? MIN(p->ts->margin.bottom, 16) : p->ts->margin.bottom;
  req.line_height = p->ts->GetHeight();
  req.linespacing = p->ts->linespacing;
  req.heading_level = heading_level;
  if (heading_layout::ShouldAdvanceHeadingForKeepWithNext(req)) {
    PlainAdvanceScreen(p);
    return true;
  }
  return false;
}

static void AppendInlineImageToPlainParsedData(parsedata_t *p, u16 image_id,
                                               InlineImageContext image_context) {
  if (!p || !p->ts || !p->book)
    return;

  const size_t token_len = 4;
  if (p->buflen > 0 && ((size_t)p->buflen + token_len) > PAGEBUFSIZE)
    PlainForceAdvancePageForBufferLimit(p, NULL);

  Text *ts = p->ts;
  InlineImageLayoutPlan image_plan{};
  p->book->PlanInlineImageLayout(ts, image_id, p->screen, p->pen.x, p->pen.y,
                                 p->linebegan, image_context,
                                 &image_plan);

  if (image_plan.advance_before)
    PlainAdvanceScreen(p);
  if (image_plan.line_break_before && p->linebegan)
    PlainLinefeed(p);

  if (image_context == INLINE_IMAGE_CONTEXT_LEADING_PARAGRAPH)
    parse_append_page_byte(p, TEXT_IMAGE_LEADING_PARAGRAPH);
  else if (image_context == INLINE_IMAGE_CONTEXT_FIGURE_WITH_CAPTION)
    parse_append_page_byte(p, TEXT_IMAGE_FIGURE_WITH_CAPTION);
  else
    parse_append_page_byte(p, TEXT_IMAGE_CONTEXT_DEFAULT);
  parse_append_page_byte(p, TEXT_IMAGE);
  parse_append_page_byte(p, (u8)((image_id >> 8) & 0xFF));
  parse_append_page_byte(p, (u8)(image_id & 0xFF));
  p->perf_inline_images++;

  switch (image_plan.mode) {
  case INLINE_IMAGE_LAYOUT_INLINE:
    p->pen.x += image_plan.draw_width + ts->GetAdvance(' ');
    p->linebegan = true;
    break;
  case INLINE_IMAGE_LAYOUT_BAND:
    p->pen.x = ts->margin.left;
    p->pen.y += image_plan.vertical_space_after_draw;
    p->linebegan = false;
    break;
  case INLINE_IMAGE_LAYOUT_PAGE:
  default:
    if (p->screen == 1) {
      PlainAdvanceScreen(p);
    } else {
      p->screen = 1;
      p->pen.x = ts->margin.left;
      p->pen.y = ts->margin.top + ts->GetHeight();
      p->linebegan = false;
    }
    break;
  }
}

// text_cursor_per_page: if non-null, receives one entry per page created
// during this call — the text byte offset at the start of that page.
// Together with html_to_text_map (built in ExtractMobiMarkupToText), this
// allows MobiHtmlPosToPage() to convert MOBI TOC byte positions into accurate
// page numbers without the lossy linear ratio that was used before.
static bool ContinuePlainTextStreamState(PlainTextStreamState *state,
                                         const std::string &text_utf8,
                                         u32 budget_ms, u16 page_budget,
                                         u16 min_pages_before_stop,
                                         std::vector<u32> *text_cursor_per_page,
                                         PlainTextStreamPerf *perf_out) {
  plain_text_stream::ContinueCallbacks callbacks;
  callbacks.is_blank_line = IsBlankLine;
  callbacks.looks_like_plain_chapter_heading = LooksLikePlainChapterHeading;
  callbacks.should_accept_heuristic_heading = ShouldAcceptHeuristicHeading;
  callbacks.add_chapter_if_unique = AddChapterIfUnique;
  callbacks.apply_heading_keep_with_next = ApplyPlainHeadingKeepWithNext;
  callbacks.append_inline_image_to_parsedata = AppendInlineImageToPlainParsedData;
  callbacks.finalize_plain_page = FinalizePlainPage;
  callbacks.set_non_epub_toc_confidence = SetNonEpubTocConfidence;
  return plain_text_stream::ContinueState(
      state, text_utf8, budget_ms, page_budget, min_pages_before_stop,
      text_cursor_per_page, perf_out, callbacks);
}

static u8 ParsePlainTextBuffer(Book *book, const std::string &text_utf8,
                               bool detect_heuristic_headings = true) {
  if (!book)
    return 1;
  const BookIoDeps deps = BuildBookIoDeps(book);
  book->ClearChapters();
  book->ClearTocConfidence();

  PlainTextStreamState state;
  if (!InitPlainTextStreamState(book, text_utf8, deps,
                                detect_heuristic_headings, &state))
    return 1;
  PlainTextStreamPerf perf;
  ContinuePlainTextStreamState(&state, text_utf8, 0, 0, 0, nullptr, &perf);
#ifdef DSLIBRIS_DEBUG
  LogPlainTextStreamPerf(deps.reporter, "PLAIN", perf, state.completed);
#endif
  return 0;
}

static void BuildFb2FallbackChapters(Book *book) {
  if (!book)
    return;
  if (!book->GetChapters().empty())
    return;
  if (book->GetPageCount() == 0)
    return;

  std::vector<std::string> lines;
  std::vector<u16> line_pages;
  for (u16 page = 0; page < book->GetPageCount(); page++) {
    const std::vector<std::string> page_lines =
        page_text_extract_utils::ExtractTextLinesFromPage(book->GetPage(page));
    for (size_t i = 0; i < page_lines.size(); i++) {
      lines.push_back(page_lines[i]);
      line_pages.push_back(page);
    }
  }

  if (lines.empty())
    return;

  bool prev_blank = true;
  bool prev_candidate = false;
  for (size_t i = 0; i < lines.size(); i++) {
    bool curr_blank = IsBlankLine(lines[i]);
    bool next_blank = (i + 1 >= lines.size()) || IsBlankLine(lines[i + 1]);

    bool curr_strong = false;
    bool curr_candidate = LooksLikePlainChapterHeading(lines[i], &curr_strong);
    bool next_strong = false;
    bool next_candidate =
        (i + 1 < lines.size()) &&
        LooksLikePlainChapterHeading(lines[i + 1], &next_strong);

    if (curr_candidate && ShouldAcceptHeuristicHeading(
                              lines[i], prev_blank, next_blank, prev_candidate,
                              next_candidate, curr_strong)) {
      AddChapterAtPageIfUnique(book, line_pages[i], lines[i], 0);
    }

    prev_blank = curr_blank;
    prev_candidate = curr_candidate;
  }
}

static u8 ParseTxtFile(Book *book, const char *path) {
  std::string text;
  if (!txt_loader::ReadAndNormalize(path, &text))
    return BOOK_ERR_CORRUPT;
  return ParsePlainTextBuffer(book, text);
}

static u8 ParseRtfFile(Book *book, const char *path) {
  std::string text;
  if (!rtf_loader::ReadAndDecode(path, &text))
    return BOOK_ERR_CORRUPT;
  return ParsePlainTextBuffer(book, text);
}

static std::string DecodeMobiStructuredTocBytes(const std::string &in,
                                                u32 encoding) {
  return DecodeMobiBytesToUtf8(in, encoding, NULL, NULL);
}

static std::string NormalizeMobiStructuredTocTitle(const std::string &in) {
  return CollapseAsciiWhitespace(TrimAsciiWhitespace(in));
}

static bool RejectMobiStructuredTocTitle(const std::string &in) {
  return IsMostlyDigitsOrPunctuation(in);
}

static std::string NormalizeHeadingNeedle(const std::string &s) {
  std::string trimmed = TrimAsciiWhitespace(s);
  for (size_t i = 0; i < trimmed.size(); i++) {
    if (trimmed[i] == '|')
      trimmed[i] = ' ';
  }
  return CollapseAsciiWhitespace(FoldLatinForMatch(trimmed));
}

static bool PageHasHeadingNeedle(const std::vector<std::string> &lines,
                                 const std::string &needle) {
  if (needle.empty())
    return false;
  // Search all lines on the page.  Pages are short (3DS screen ≈ 30-40 lines)
  // and decorative whitespace (pagebreaks, <br>) generates many blank entries
  // that previously exhausted the old cap of 18 before reaching real content.
  const size_t n = lines.size();
  for (size_t i = 0; i < n; i++) {
    std::string norm = NormalizeHeadingNeedle(lines[i]);
    if (norm.empty())
      continue;
    if (norm == needle)
      return true;
    if (norm.size() > needle.size() && norm.find(needle) != std::string::npos)
      return true;
  }
  // Multi-line headings: try concatenating consecutive non-empty lines.
  for (size_t i = 0; i + 1 < n; i++) {
    std::string a = NormalizeHeadingNeedle(lines[i]);
    if (a.empty())
      continue;
    std::string concat = a;
    for (size_t j = i + 1; j < n && concat.size() < needle.size() + 32; j++) {
      std::string b = NormalizeHeadingNeedle(lines[j]);
      if (b.empty())
        continue;
      concat += " " + b;
      if (concat == needle)
        return true;
      if (concat.size() > needle.size() &&
          concat.find(needle) != std::string::npos)
        return true;
    }
  }
  return false;
}

static mobi_toc_finalize::FinalizeCallbacks MakeMobiTocFinalizeCallbacks() {
  mobi_toc_finalize::FinalizeCallbacks callbacks;
  callbacks.normalize_heading_needle = NormalizeHeadingNeedle;
  callbacks.page_has_heading_needle = PageHasHeadingNeedle;
  callbacks.add_chapter_at_page_if_unique = AddChapterAtPageIfUnique;
  callbacks.prune_front_matter_toc_cluster = PruneMobiFrontMatterTocCluster;
  callbacks.is_heuristic_chapter_set_noisy = IsMobiHeuristicChapterSetNoisy;
  return callbacks;
}

static size_t MobiInlineCountWords(const std::string &text) {
  return (size_t)CountAsciiWords(text);
}

static bool MobiInlineMostlyDigitsOrPunct(const std::string &text) {
  return IsMostlyDigitsOrPunctuation(text);
}

static mobi_toc_prepare::StructuredCallbacks
MakeMobiStructuredTocCallbacks() {
  mobi_toc_prepare::StructuredCallbacks callbacks;
  callbacks.decode_bytes_to_utf8 = DecodeMobiStructuredTocBytes;
  callbacks.normalize_title = NormalizeMobiStructuredTocTitle;
  callbacks.reject_title = RejectMobiStructuredTocTitle;
  return callbacks;
}

static mobi_toc_prepare::InlineTitleCallbacks MakeMobiInlineTitleCallbacks() {
  mobi_toc_prepare::InlineTitleCallbacks callbacks;
  callbacks.looks_like_structured_title = LooksLikeStructuredMobiChapterTitle;
  callbacks.fold_latin_for_match = FoldLatinForMatch;
  callbacks.count_ascii_words = MobiInlineCountWords;
  callbacks.is_mostly_digits_or_punctuation = MobiInlineMostlyDigitsOrPunct;
  return callbacks;
}

static mobi_markup_extract::ExtractCallbacks MakeMobiMarkupExtractCallbacks() {
  mobi_markup_extract::ExtractCallbacks callbacks;
  callbacks.trim_ascii_whitespace = TrimAsciiWhitespace;
  callbacks.collapse_ascii_whitespace = CollapseAsciiWhitespace;
  callbacks.fold_latin_for_match = FoldLatinForMatch;
  callbacks.looks_like_structured_chapter_title = LooksLikeStructuredMobiChapterTitle;
  callbacks.add_chapter_at_page_if_unique = AddChapterAtPageIfUnique;
  return callbacks;
}

static std::string ExtractMobiMarkupToText(
    Book *book, const BookIoDeps &deps, const std::string &in,
    std::vector<MobiHeadingHint> *heading_hints,
    std::vector<std::pair<u32, u32>> *html_to_text_map) {
  return mobi_markup_extract::ExtractToText(
      book, deps, in, heading_hints, html_to_text_map,
      MakeMobiMarkupExtractCallbacks());
}

static plain_text_stream::ContinueCallbacks MakeMobiPlainContinueCallbacks() {
  plain_text_stream::ContinueCallbacks callbacks;
  callbacks.is_blank_line = IsBlankLine;
  callbacks.looks_like_plain_chapter_heading = LooksLikePlainChapterHeading;
  callbacks.should_accept_heuristic_heading = ShouldAcceptHeuristicHeading;
  callbacks.add_chapter_if_unique = AddChapterIfUnique;
  callbacks.apply_heading_keep_with_next = ApplyPlainHeadingKeepWithNext;
  callbacks.append_inline_image_to_parsedata = AppendInlineImageToPlainParsedData;
  callbacks.finalize_plain_page = FinalizePlainPage;
  callbacks.set_non_epub_toc_confidence = SetNonEpubTocConfidence;
  return callbacks;
}

static mobi_parser::Hooks MakeMobiParserHooks() {
  mobi_parser::Hooks hooks;
  hooks.extract_markup_to_text = ExtractMobiMarkupToText;
  hooks.make_structured_toc_callbacks = MakeMobiStructuredTocCallbacks;
  hooks.make_inline_title_callbacks = MakeMobiInlineTitleCallbacks;
  hooks.make_finalize_callbacks = MakeMobiTocFinalizeCallbacks;
  hooks.make_plain_continue_callbacks = MakeMobiPlainContinueCallbacks;
  return hooks;
}

} // namespace

u8 Book::Parse(bool fulltext) {
  //! Parse full text (true) or titles only (false).
  //! Expat callback handlers do the heavy work.
  u8 rc = 0;

  char path[MAXPATHLEN];
  snprintf(path, sizeof(path), "%s/%s", GetFolderName(), GetFileName());

  // Lightweight non-XML formats.
  if (fulltext && HasExtCI(GetFileName(), ".txt"))
    return ParseTxtFile(this, path);
  if (fulltext && HasExtCI(GetFileName(), ".rtf"))
    return ParseRtfFile(this, path);
  if (fulltext && HasExtCI(GetFileName(), ".odt"))
    return odt_loader::ParseOdtFile(this, path);
  if (fulltext && HasExtCI(GetFileName(), ".mobi"))
    return mobi_parser::ParseFile(this, path, MakeMobiParserHooks());
  if (fulltext && (HasExtCI(GetFileName(), ".pdf") ||
                   HasExtCI(GetFileName(), ".xps") ||
                   HasExtCI(GetFileName(), ".oxps")))
    return ParsePdfFile(this, path);
  if (fulltext && HasExtCI(GetFileName(), ".cbz"))
    return ParseCbzFile(this, path);

  FILE *fp = fopen(path, "r");
  if (!fp) {
    rc = 255;
    return (rc);
  }

  const BookIoDeps deps = BuildBookIoDeps(this);
  parsedata_t parsedata;
  InitParsedataWithBookIoDeps(&parsedata, this, deps);
  parsedata.fb2_mode = fulltext && HasExtCI(GetFileName(), ".fb2");
#ifdef DSLIBRIS_DEBUG
  const u64 xml_parse_begin = osGetTime();
  const u16 xml_pages_before = GetPageCount();
#endif
  PlainTextPerfBaseline xml_perf_baseline;
  CapturePlainTextPerfBaseline(parsedata, &xml_perf_baseline);

  xml_parse_utils::XmlParserOptions options;
  options.user_data = &parsedata;
  options.default_handler = xml::book::fallback;
  options.processing_instruction = xml::book::instruction;
  options.start_element = xml::book::start;
  options.end_element = xml::book::end;
  options.character_data = xml::book::chardata;
  if (!fulltext) {
    options.start_element = xml::book::metadata::start;
    options.end_element = xml::book::metadata::end;
    options.character_data = xml::book::metadata::chardata;
  }
  xml_parse_utils::XmlParseResult parse_result =
      xml_parse_utils::ParseXmlFileStream(
          fp, options, parser_limits::kXmlStreamBufferSize,
          [](void *user_data) {
            parsedata_t *parsedata = static_cast<parsedata_t *>(user_data);
            return parsedata && parsedata->status;
          });
  fclose(fp);
  if (!parse_result.ok) {
    if (deps.reporter)
      deps.reporter->PrintStatus(
          xml_parse_utils::FormatXmlParseError(parse_result).c_str());
    rc = 254;
  }

  if (rc == 0 && fulltext && parsedata.fb2_mode) {
    bool has_structured_toc = !chapters.empty();
    if (!has_structured_toc)
      BuildFb2FallbackChapters(this);
    if (!chapters.empty())
      SetNonEpubTocConfidence(this, has_structured_toc);
    else
      ClearTocConfidence();
  }

#ifdef DSLIBRIS_DEBUG
  if (deps.reporter && fulltext) {
    PlainTextStreamPerf perf;
    FillPlainTextStreamPerf(parsedata, xml_perf_baseline,
                            osGetTime() - xml_parse_begin, 0,
                            GetPageCount() - xml_pages_before, &perf);
    LogPlainTextStreamPerf(
        deps.reporter, parsedata.fb2_mode ? "FB2 layout" : "XML layout", perf,
        rc == 0);
  }
#endif

  return (rc);
}

bool Book::HasDeferredMobiParse() const {
  return mobi_deferred_runtime::Has(this);
}

bool Book::ContinueDeferredMobiParse(u32 budget_ms, u16 page_budget) {
  return mobi_parser::ContinueDeferredParse(this, budget_ms, page_budget,
                                            MakeMobiParserHooks());
}

void Book::CancelDeferredMobiParse() { mobi_deferred_runtime::Erase(this); }

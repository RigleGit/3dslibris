#include "formats/common/plain_parser.h"

#include "book/heading_layout.h"
#include "book/page.h"
#include "formats/common/book_error.h"
#include "formats/common/page_text_extract_utils.h"
#include "formats/common/plain_text_stream.h"
#include "formats/rtf/rtf_loader.h"
#include "formats/txt/txt_loader.h"
#include "shared/text_render_layout_utils.h"
#include "parse.h"

#include <algorithm>
#include <ctype.h>
#include <memory>
#include <string.h>
#include <sys/param.h>

namespace plain_parser {
namespace {

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
    return true;
  }

  return false;
}

static void InitParsedataWithDeps(parsedata_t *parsedata, Book *book,
                                  const BookParseDeps &deps) {
  if (!parsedata)
    return;
  parse_init(parsedata);
  parsedata->reporter = deps.reporter;
  parsedata->ts = deps.ts;
  parsedata->prefs = deps.prefs;
  parsedata->book = book;
}

static void FinalizePlainPage(parsedata_t *p) {
  if (!p || !p->book)
    return;
  if (p->buflen > 0 || p->book->GetPageCount() == 0) {
    Page *page = p->book->AppendPage();
    page->SetBuffer(p->buf, p->buflen);
  }
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
  req.bottom_margin = (p->screen == 1)
                          ? text_render_layout_utils::ResolveCompactReadingBottomMargin(
                                p->ts->margin.bottom)
                          : (p->ts->margin.bottom +
                             text_render_layout_utils::kFullReadingScreenFooterGuardPx);
  req.line_height = p->ts->GetHeight();
  req.linespacing = p->ts->linespacing;
  req.heading_level = heading_level;
  if (heading_layout::ShouldAdvanceHeadingForKeepWithNext(req)) {
    PlainAdvanceScreen(p);
    return true;
  }
  return false;
}

static void PlainAdvanceScreenOnOverflow(parsedata_t *p) {
  if (!p || !p->ts)
    return;
  const int screen_height = (p->screen == 1) ? 320 : 400;
  const int bottom_margin =
      (p->screen == 1)
          ? text_render_layout_utils::ResolveCompactReadingBottomMargin(
                p->ts->margin.bottom)
          : (p->ts->margin.bottom +
             text_render_layout_utils::kFullReadingScreenFooterGuardPx);
  if (text_render_layout_utils::WouldOverflowReadingScreen(
          p->pen.y, p->ts->GetHeight(), p->ts->linespacing, screen_height,
          bottom_margin)) {
    PlainAdvanceScreen(p);
  }
}

static void AppendInlineImageToPlainParsedData(parsedata_t *p, u16 image_id,
                                               InlineImageContext image_context) {
  if (!p || !p->ts || !p->book)
    return;

  const size_t token_len = 3;
  if (p->buflen > 0 && ((size_t)p->buflen + token_len) > PAGEBUFSIZE)
    PlainForceAdvancePageForBufferLimit(p, NULL);

  Text *ts = p->ts;
  InlineImageLayoutPlan image_plan{};
  p->book->PlanInlineImageLayout(ts, image_id, p->screen, p->pen.x, p->pen.y,
                                 p->linebegan, image_context, &image_plan);

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
  parse_append_page_byte(p, (u32)image_id);
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
    PlainAdvanceScreenOnOverflow(p);
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

} // namespace

std::string TrimAsciiWhitespace(const std::string &in) {
  size_t start = 0;
  while (start < in.size() && isspace((unsigned char)in[start]))
    start++;
  size_t end = in.size();
  while (end > start && isspace((unsigned char)in[end - 1]))
    end--;
  return in.substr(start, end - start);
}

std::string CollapseAsciiWhitespace(const std::string &in) {
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

std::string FoldLatinForMatch(const std::string &in) {
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

int CountAsciiWords(const std::string &line) {
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

bool LooksLikePlainChapterHeading(const std::string &line,
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

bool IsBlankLine(const std::string &line) {
  return TrimAsciiWhitespace(line).empty();
}

bool ShouldAcceptHeuristicHeading(const std::string &line, bool prev_blank,
                                  bool next_blank, bool prev_candidate,
                                  bool next_candidate, bool strong_signal) {
  std::string compact = CollapseAsciiWhitespace(TrimAsciiWhitespace(line));
  std::string folded = FoldLatinForMatch(compact);
  if (LooksLikeFalsePositiveHeading(compact, folded, strong_signal))
    return false;

  if (prev_blank || next_blank)
    return true;

  if (prev_candidate || next_candidate)
    return false;

  return strong_signal && CountAsciiWords(compact) <= 8;
}

bool IsMostlyDigitsOrPunctuation(const std::string &s) {
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
    if ((c & 0xC0) == 0xC0) {
      alpha++;
      total++;
    }
  }
  if (total == 0)
    return true;
  return alpha <= 1;
}

void AddChapterAtPageIfUnique(Book *book, u16 page, const std::string &title,
                              u8 level) {
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

void AddChapterIfUnique(Book *book, const std::string &title, u8 level) {
  if (!book)
    return;
  AddChapterAtPageIfUnique(book, book->GetPageCount(), title, level);
}

void SetNonEpubTocConfidence(Book *book, bool strong) {
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

bool InitState(Book *book, const std::string &text_utf8,
               const BookParseDeps &deps, bool detect_heuristic_headings,
               State *out) {
  if (!book || !deps.ts || !out)
    return false;

  parsedata_t base;
  InitParsedataWithDeps(&base, book, deps);
  plain_text_stream::InitState(out, base, text_utf8, detect_heuristic_headings);
  return true;
}

plain_text_stream::ContinueCallbacks MakeContinueCallbacks() {
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

bool ContinueState(State *state, const std::string &text_utf8, u32 budget_ms,
                   u16 page_budget, u16 min_pages_before_stop,
                   std::vector<u32> *text_cursor_per_page,
                   plain_text_perf_utils::PlainTextStreamPerf *perf_out) {
  return plain_text_stream::ContinueState(
      state, text_utf8, budget_ms, page_budget, min_pages_before_stop,
      text_cursor_per_page, perf_out, MakeContinueCallbacks());
}

u8 ParseBuffer(Book *book, const std::string &text_utf8,
               bool detect_heuristic_headings) {
  if (!book)
    return 1;
  const BookParseDeps deps = BuildBookParseDeps(book);
  book->ClearChapters();
  book->ClearTocConfidence();

  // Allocate State on the heap: parsedata_t contains a 16 KB u32 buf[4096]
  // which, combined with the rest of State, would overflow the 3DS stack (~32 KB)
  // when reached from a deep call chain (e.g. chapter menu → FindApproxTitlePage).
  std::unique_ptr<State> state_heap(new State());
  State &state = *state_heap;
  if (!InitState(book, text_utf8, deps, detect_heuristic_headings, &state))
    return 1;
  plain_text_perf_utils::PlainTextStreamPerf perf;
  ContinueState(&state, text_utf8, 0, 0, 0, nullptr, &perf);
#ifdef DSLIBRIS_DEBUG
  plain_text_perf_utils::LogPlainTextStreamPerf(deps.reporter, "PLAIN", perf,
                                                state.completed);
#endif
  return 0;
}

u8 ParseTxtFile(Book *book, const char *path) {
  std::string text;
  if (!txt_loader::ReadAndNormalize(path, &text))
    return BOOK_ERR_CORRUPT;
  return ParseBuffer(book, text);
}

u8 ParseRtfFile(Book *book, const char *path) {
  std::string text;
  if (!rtf_loader::ReadAndDecode(path, &text))
    return BOOK_ERR_CORRUPT;
  return ParseBuffer(book, text);
}

void BuildFb2FallbackChapters(Book *book) {
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

} // namespace plain_parser

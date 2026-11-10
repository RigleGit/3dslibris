/*
    3dslibris - chapter_menu.cpp
    Adapted from dslibris for Nintendo 3DS.

    Original attribution (dslibris): Ray Haleblian, GPLv2+.
    Modified for Nintendo 3DS by Rigle.

    Summary:
    - Builds chapter/index rows from resolved TOC metadata.
    - Applies title/subtitle normalization for compact list rows.
    - Resolves chapter targets with exact or approximate fallback logic.
*/

#include "menus/chapter_menu.h"

#include <algorithm>
#include <ctype.h>

#include "book/book.h"
#include "book/page.h"
#include "menus/chapter_menu_utils.h"
#include "shared/debug_log.h"
#include "shared/string_utils.h"
#include "shared/text_unicode_utils.h"
#include "ui/text.h"

namespace {

static const char *TocQualityLabel(TocQuality q) {
  switch (q) {
  case TOC_QUALITY_STRONG:
    return "index";
  case TOC_QUALITY_MIXED:
    return "index (~)";
  case TOC_QUALITY_HEURISTIC:
    return "index (approx)";
  case TOC_QUALITY_UNKNOWN:
  default:
    return "index";
  }
}

static std::string NormalizeSearchText(const std::string &raw) {
  std::string in = Trim(raw);
  if (in.empty())
    return "";

  // Remove dotted leader tail and trailing page-like numbers.
  // Example: "Chapter 1.....12" → "Chapter 1"
  size_t leader = std::string::npos;
  size_t run = 0;
  for (size_t i = 0; i < in.size(); i++) {
    if (in[i] == '.') {
      run++;
      if (run >= 3) {
        leader = i + 1 - run;
        break;
      }
    } else {
      run = 0;
    }
  }
  if (leader != std::string::npos) {
    in = Trim(in.substr(0, leader));
    // Only strip trailing page-like numbers when a dotted leader was present;
    // without a leader the trailing digits are part of the title itself
    // (e.g. "CHAPTER 4") and must be preserved for accurate matching.
    size_t end = in.size();
    while (end > 0 && isdigit((unsigned char)in[end - 1]))
      end--;
    if (end < in.size()) {
      size_t ws = end;
      while (ws > 0 && isspace((unsigned char)in[ws - 1]))
        ws--;
      if (ws >= 4)
        in = in.substr(0, ws);
    }
  }

  std::string out;
  out.reserve(in.size());
  bool prev_space = true;
  for (size_t i = 0; i < in.size(); i++) {
    unsigned char c = (unsigned char)in[i];
    if (c < 0x80) {
      if (isalnum(c)) {
        out.push_back((char)tolower(c));
        prev_space = false;
      } else if (!prev_space) {
        out.push_back(' ');
        prev_space = true;
      }
      continue;
    }

    // Preserve UTF-8 bytes (accented chars) as-is for matching against page text.
    out.push_back((char)c);
    prev_space = false;
  }

  return Trim(out);
}

static std::string BuildPageSearchText(Page *page, size_t max_out = 3072) {
  if (!page || !page->GetBuffer() || page->GetLength() <= 0)
    return "";

  const u32 *buf = page->GetBuffer();
  const int len = page->GetLength();
  std::string out;
  out.reserve((size_t)len);
  bool prev_space = true;

  int i = 0;
  while (i < len) {
    u32 c = buf[i];
    if (c == TEXT_IMAGE_CONTEXT_DEFAULT ||
        c == TEXT_IMAGE_LEADING_PARAGRAPH ||
        c == TEXT_IMAGE_FIGURE_WITH_CAPTION) {
      i++;
      continue;
    }
    if (c == TEXT_IMAGE) {
      i += (i + 1 < len) ? 2 : 1;
      if (!prev_space) {
        out.push_back(' ');
        prev_space = true;
      }
      continue;
    }
    if (c == TEXT_RTL_LINE_PX) {
      i += (i + 1 < len) ? 2 : 1;  // skip token + 1 data byte
      continue;
    }
    if (c == TEXT_IMAGE_ALIGN || c == TEXT_LINE_START_X) {
      i += (i + 1 < len) ? 2 : 1;
      continue;
    }
    if (c == TEXT_SCREEN_BREAK) {
      i++;
      continue;
    }
    if (c == TEXT_BOLD_ON || c == TEXT_BOLD_OFF || c == TEXT_ITALIC_ON ||
        c == TEXT_ITALIC_OFF || c == TEXT_UNDERLINE_ON ||
        c == TEXT_UNDERLINE_OFF || c == TEXT_OVERLINE_ON ||
        c == TEXT_OVERLINE_OFF || c == TEXT_STRIKETHROUGH_ON ||
        c == TEXT_STRIKETHROUGH_OFF || c == TEXT_SUPERSCRIPT_ON ||
        c == TEXT_SUPERSCRIPT_OFF || c == TEXT_SUBSCRIPT_ON ||
        c == TEXT_SUBSCRIPT_OFF) {
      i++;
      continue;
    }
    if (c == TEXT_UNDERLINE_STYLE || c == TEXT_FONT_SIZE) {
      i += (i + 1 < len) ? 2 : 1;
      continue;
    }
    if (c < 0x20) {
      i++;
      if (!prev_space) {
        out.push_back(' ');
        prev_space = true;
      }
      continue;
    }

    if (c < 0x80) {
      if (isalnum((int)c)) {
        out.push_back((char)tolower((int)c));
        prev_space = false;
      } else if (!prev_space) {
        out.push_back(' ');
        prev_space = true;
      }
      i++;
    } else {
      char utf8_buf[4];
      int utf8_len = 0;
      if (c < 0x800) {
        utf8_buf[0] = (char)(0xC0 | (c >> 6));
        utf8_buf[1] = (char)(0x80 | (c & 0x3F));
        utf8_len = 2;
      } else if (c < 0x10000) {
        utf8_buf[0] = (char)(0xE0 | (c >> 12));
        utf8_buf[1] = (char)(0x80 | ((c >> 6) & 0x3F));
        utf8_buf[2] = (char)(0x80 | (c & 0x3F));
        utf8_len = 3;
      } else {
        utf8_buf[0] = (char)(0xF0 | (c >> 18));
        utf8_buf[1] = (char)(0x80 | ((c >> 12) & 0x3F));
        utf8_buf[2] = (char)(0x80 | ((c >> 6) & 0x3F));
        utf8_buf[3] = (char)(0x80 | (c & 0x3F));
        utf8_len = 4;
      }
      for (int j = 0; j < utf8_len; j++)
        out.push_back(utf8_buf[j]);
      prev_space = false;
      i++;
    }

    if (max_out > 0 && out.size() >= max_out)
      break;
  }

  return Trim(out);
}

static std::vector<std::string> BuildPageHeadingLines(Page *page,
                                                      size_t max_lines = 12) {
  std::vector<std::string> lines;
  if (!page || !page->GetBuffer() || page->GetLength() <= 0 || max_lines == 0)
    return lines;

  const u32 *buf = page->GetBuffer();
  const int len = page->GetLength();
  std::string cur;
  cur.reserve(96);

  auto flush_line = [&]() {
    std::string n = NormalizeSearchText(cur);
    if (!n.empty())
      lines.push_back(n);
    cur.clear();
  };

  for (int i = 0; i < len; i++) {
    u32 c = buf[i];
    if (c == TEXT_IMAGE_CONTEXT_DEFAULT ||
        c == TEXT_IMAGE_LEADING_PARAGRAPH ||
        c == TEXT_IMAGE_FIGURE_WITH_CAPTION) {
      continue;
    }
    if (c == TEXT_IMAGE) {
      i += (i + 1 < len) ? 1 : 0;
      if (!cur.empty() && cur.back() != ' ')
        cur.push_back(' ');
      continue;
    }
    if (c == TEXT_RTL_LINE_PX) {
      i += (i + 1 < len) ? 1 : 0;  // skip data byte (loop i++ skips token)
      continue;
    }
    if (c == TEXT_IMAGE_ALIGN || c == TEXT_LINE_START_X) {
      i += (i + 1 < len) ? 1 : 0;
      continue;
    }
    if (c == TEXT_SCREEN_BREAK)
      continue;
    if (c == TEXT_BOLD_ON || c == TEXT_BOLD_OFF || c == TEXT_ITALIC_ON ||
        c == TEXT_ITALIC_OFF || c == TEXT_UNDERLINE_ON ||
        c == TEXT_UNDERLINE_OFF || c == TEXT_OVERLINE_ON ||
        c == TEXT_OVERLINE_OFF || c == TEXT_STRIKETHROUGH_ON ||
        c == TEXT_STRIKETHROUGH_OFF || c == TEXT_SUPERSCRIPT_ON ||
        c == TEXT_SUPERSCRIPT_OFF || c == TEXT_SUBSCRIPT_ON ||
        c == TEXT_SUBSCRIPT_OFF) {
      continue;
    }
    if (c == TEXT_UNDERLINE_STYLE || c == TEXT_FONT_SIZE) {
      i += (i + 1 < len) ? 1 : 0;
      continue;
    }
    if (c == '\r')
      continue;
    if (c == '\n') {
      flush_line();
      if (lines.size() >= max_lines)
        break;
      continue;
    }
    if (c < 0x20) {
      if (!cur.empty() && cur.back() != ' ')
        cur.push_back(' ');
      continue;
    }
    if (c < 0x80) {
      cur.push_back((char)c);
      continue;
    }
    char utf8_buf[4];
    int utf8_len = 0;
    if (c < 0x800) {
      utf8_buf[0] = (char)(0xC0 | (c >> 6));
      utf8_buf[1] = (char)(0x80 | (c & 0x3F));
      utf8_len = 2;
    } else if (c < 0x10000) {
      utf8_buf[0] = (char)(0xE0 | (c >> 12));
      utf8_buf[1] = (char)(0x80 | ((c >> 6) & 0x3F));
      utf8_buf[2] = (char)(0x80 | (c & 0x3F));
      utf8_len = 3;
    } else {
      utf8_buf[0] = (char)(0xF0 | (c >> 18));
      utf8_buf[1] = (char)(0x80 | ((c >> 12) & 0x3F));
      utf8_buf[2] = (char)(0x80 | ((c >> 6) & 0x3F));
      utf8_buf[3] = (char)(0x80 | (c & 0x3F));
      utf8_len = 4;
    }
    for (int j = 0; j < utf8_len; j++)
      cur.push_back(utf8_buf[j]);
  }

  if (lines.size() < max_lines)
    flush_line();
  return lines;
}

static int EvaluateHeadingScore(Page *page, const std::string &query) {
  if (!page || query.empty())
    return 0;
  std::vector<std::string> lines = BuildPageHeadingLines(page, 12);
  if (lines.empty())
    return 0;

  int best = 0;
  for (size_t i = 0; i < lines.size(); i++) {
    const std::string &line = lines[i];
    if (line.empty())
      continue;
    int s = 0;
    if (line == query) {
      s = 160;
    } else if ((line.find(query) != std::string::npos ||
                query.find(line) != std::string::npos) &&
               std::min(line.size(), query.size()) >= 4) {
      s = 115;
    }

    if (s == 0 && i + 1 < lines.size()) {
      std::string joined = line + " " + lines[i + 1];
      if (joined == query) {
        s = 145;
      } else if ((joined.find(query) != std::string::npos ||
                  query.find(joined) != std::string::npos) &&
                 std::min(joined.size(), query.size()) >= 5) {
        s = 108;
      }
    }

    if (s > 0) {
      if (line.size() > 88)
        s -= 54;
      else if (line.size() > 68)
        s -= 32;
      if (i == 0)
        s += 20;
      else if (i == 1)
        s += 12;
      else if (i <= 3)
        s += 6;
      if (line.size() <= 44)
        s += 6;
      if (s > best)
        best = s;
    }
  }

  return best;
}

static void EvaluateApproxPage(Book *book, u16 p, const std::string &query,
                               u16 hint_page, int *best_score, u16 *best_page) {
  Page *page = book->GetPage((int)p);
  std::string text = BuildPageSearchText(page, 4096);
  int heading_score = EvaluateHeadingScore(page, query);
  if (text.empty() && heading_score <= 0)
    return;
  size_t pos = text.find(query);
  if (pos == std::string::npos && heading_score < 90)
    return;

  // Prefer occurrences near top of page and near current mapped hint.
  int top_bonus = 0;
  if (heading_score >= 170) {
    top_bonus = 180;
  } else if (pos < 120)
    top_bonus = 120;
  else if (pos < 320)
    top_bonus = 80;
  else if (pos < 900)
    top_bonus = 40;
  else
    top_bonus = 0;

  int dist = (int)p - (int)hint_page;
  if (dist < 0)
    dist = -dist;
  int score = 300 + top_bonus - (dist / 2) + (heading_score * 2);
  if (score > *best_score) {
    *best_score = score;
    *best_page = p;
  }
}

static bool FindApproxTitlePage(Book *book, const std::string &title,
                                u16 hint_page, u16 *page_out) {
  if (!book || !page_out)
    return false;
  // Fixed-layout formats (PDF/CBZ) have no text Page objects; GetPage() would
  // access an empty vector and return garbage. Nothing to search here.
  if (book->IsFixedLayout())
    return false;
  u16 page_count = book->GetPageCount();
  if (page_count == 0)
    return false;
  if (hint_page >= page_count)
    hint_page = (u16)(page_count - 1);

  std::string query = NormalizeSearchText(title);
  if (query.size() < 4)
    return false;

  auto eval_range = [&](u16 p0, u16 p1, int *score, u16 *best) {
    for (u16 p = p0; p < p1 && p < page_count; ++p)
      EvaluateApproxPage(book, p, query, hint_page, score, best);
  };

  int best_score = -100000;
  u16 best_page = hint_page;

  u16 local_start = (hint_page > 24) ? (u16)(hint_page - 24) : 0;
  u16 local_end = (u16)std::min<int>((int)page_count, (int)hint_page + 80);
  eval_range(local_start, local_end, &best_score, &best_page);
  if (best_score >= 260) {
    *page_out = best_page;
    return true;
  }

  eval_range(0, page_count, &best_score, &best_page);
  if (best_score >= 220) {
    *page_out = best_page;
    return true;
  }

  return false;
}

} // namespace

ChapterMenu::ChapterMenu(App *_app) : PagedListMenu(_app, "index") {}

ChapterMenu::~ChapterMenu() {}

void ChapterMenu::HandleInput(const FrameInput &input) {
  if (approx_pending_) {
    if (input.keys_down || !current_book_ ||
        current_book_->GetOpenSessionId() != approx_session_) {
      if (current_book_)
        current_book_->CancelReflowPageRequest();
      approx_pending_ = false;
    } else {
      ActivateSelected();
      return;
    }
  }
  PagedListMenu::HandleInput(input);
}

void ChapterMenu::SelectChapterForPage(u16 current_page) {
  SelectItem((u16)chapter_menu_utils::FindChapterIndexForPage(entry_pages,
                                                              current_page));
}

void ChapterMenu::BuildEntries(Book *book, Text *text,
                               std::vector<std::string> &labels,
                               std::vector<u16> &pages) {
  approx_pending_ = false;
  if (!book)
    return;

  entry_titles.clear();
  entry_pages.clear();
  approx_page_cache.clear();

  SetHeaderTitle(TocQualityLabel(book->GetTocQuality()));

  const std::vector<ChapterEntry> &chapters = book->GetChapters();
  DBG_LOGF(status_reporter, "INDEX build fmt=%d tocq=%d entries=%u pages=%u",
           (int)book->format, (int)book->GetTocQuality(),
           (unsigned)chapters.size(), (unsigned)book->GetPageCount());
  labels.reserve(chapters.size());
  pages.reserve(chapters.size());
  entry_titles.reserve(chapters.size());
  entry_pages.reserve(chapters.size());
  approx_page_cache.reserve(chapters.size());

  entry_levels.clear();
  entry_levels.reserve(chapters.size());
  for (const auto &ch : chapters) {
    // The whole title is kept: the list wraps it and sizes the row for it.
    // Nested entries are marked and indented (see EntryIndent).
    labels.push_back(ch.level > 0 ? "- " + ch.title : ch.title);
    entry_levels.push_back(ch.level);
    pages.push_back(ch.page);
    entry_titles.push_back(ch.title);
    entry_pages.push_back(ch.page);
    approx_page_cache.push_back(-1);
  }
}

int ChapterMenu::EntryIndent(size_t index) const {
  const int level = index < entry_levels.size() ? entry_levels[index] : 0;
  return (level > 6 ? 6 : level) * 8;
}

bool ChapterMenu::ResolveTargetPage(u16 index, u16 *page_out) {
  if (!PagedListMenu::ResolveTargetPage(index, page_out)) {
    DBG_LOGF(status_reporter, "INDEX resolve base failed idx=%u",
             (unsigned)index);
    return false;
  }
  Book *book = current_book_;
  if (!book)
    return true;
  if (index >= entry_titles.size() || index >= entry_pages.size() ||
      index >= approx_page_cache.size()) {
    DBG_LOGF(status_reporter,
             "INDEX resolve bounds idx=%u titles=%u pages=%u approx=%u",
             (unsigned)index, (unsigned)entry_titles.size(),
             (unsigned)entry_pages.size(), (unsigned)approx_page_cache.size());
    return true;
  }

  // EPUB v23 persists targets resolved during complete pagination.
  if (book->HasReflowPageCache() && !HasExtCI(book->GetFileName(), ".mobi"))
    return true;
  TocQuality q = book->GetTocQuality();
  if (q == TOC_QUALITY_UNKNOWN) {
    DBG_LOGF(status_reporter, "INDEX resolve toc unknown idx=%u page=%u",
             (unsigned)index, (unsigned)*page_out);
    return true;
  }

  // For heuristic TOCs the original page hint is a rough guess and we allow
  // larger jumps.  For structured (strong/mixed) TOCs the hint is usually
  // close but can still be off by a few pages because of the linear
  // byte-to-page estimation used by MOBI; allow a tighter correction.
  // 24 is a safety net wide enough to absorb the residual error from the
  // linear fallback when the precise OPT-A mapping tables are unavailable
  // (e.g. page-cache loads), without being so wide that we jump to a wrong
  // chapter of a similar name.
  const int max_delta = (q == TOC_QUALITY_HEURISTIC) ? 32 : 24;

  if (approx_page_cache[index] >= 0) {
    *page_out = (u16)approx_page_cache[index];
    return true;
  }
  if (approx_page_cache[index] == -2)
    return true;

  bool resident_match = false;
  if (book->HasReflowPageCache()) {
    if (!approx_pending_ || approx_index_ != index ||
        approx_session_ != book->GetOpenSessionId()) {
      approx_pending_ = true;
      approx_index_ = index;
      approx_session_ = book->GetOpenSessionId();
      approx_query_ = NormalizeSearchText(entry_titles[index]);
      approx_best_score_ = -100000;
      approx_best_page_ = entry_pages[index];
      approx_phase_ = 0;
      approx_cursor_ = entry_pages[index] > 24 ? entry_pages[index] - 24 : 0;
      approx_end_ =
          std::min<unsigned>(book->GetPageCount(), entry_pages[index] + 80);
    }
    const u64 begin = osGetTime();
    while (approx_query_.size() >= 4 && approx_phase_ < 2 &&
           osGetTime() - begin < 30) {
      if (approx_cursor_ == approx_end_) {
        if ((approx_phase_ == 0 && approx_best_score_ >= 260) ||
            approx_phase_ == 1) {
          approx_phase_ = 2;
          break;
        }
        approx_phase_ = 1;
        approx_cursor_ = 0;
        approx_end_ = book->GetPageCount();
        continue;
      }
      if (!book->IsReflowPageReady(approx_cursor_)) {
        if (!book->RequestReflowPage(approx_cursor_))
          return false;
        if (book->AdvanceReflowPageCache() < 0) {
          approx_pending_ = false;
          return false;
        }
        if (!book->IsReflowPageReady(approx_cursor_))
          continue;
      }
      EvaluateApproxPage(book, approx_cursor_, approx_query_,
                         entry_pages[index], &approx_best_score_,
                         &approx_best_page_);
      ++approx_cursor_;
      book->TrimReflowPageCache();
    }
    if (approx_query_.size() >= 4 && approx_phase_ < 2)
      return false;
    approx_pending_ = false;
    resident_match =
        approx_best_score_ >=
        (approx_phase_ == 2 && approx_cursor_ != approx_end_ ? 260 : 220);
  }
  u16 resolved = *page_out;
  if (book->HasReflowPageCache()
          ? resident_match
          : FindApproxTitlePage(book, entry_titles[index], entry_pages[index],
                                &resolved)) {
    if (book->HasReflowPageCache())
      resolved = approx_best_page_;
    bool accept = true;
    const int from = (int)entry_pages[index];
    const int to = (int)resolved;
    const int delta = (to > from) ? (to - from) : (from - to);

    // Trust rule 1: reject jumps beyond the quality-dependent threshold.
    if (delta > max_delta)
      accept = false;

    // Trust rule 2: preserve chapter order against direct neighbors.
    if (accept && index > 0) {
      const int prev_hint = (int)entry_pages[index - 1];
      if (to < prev_hint)
        accept = false;
    }
    if (accept && (size_t)index + 1 < entry_pages.size()) {
      const int next_hint = (int)entry_pages[index + 1];
      if (to > next_hint)
        accept = false;
    }

    if (accept) {
      approx_page_cache[index] = to;
      if (app) {
        char msg[192];
        snprintf(msg, sizeof(msg), "INDEX remap sel=%u from=%u to=%u delta=%d",
                 (unsigned)index, (unsigned)from, (unsigned)to, delta);
        DBG_LOG(status_reporter, msg);
      }
      *page_out = resolved;
    } else {
      approx_page_cache[index] = -2;
      if (app) {
        char msg[224];
        snprintf(msg, sizeof(msg),
                 "INDEX remap rejected sel=%u from=%u to=%u delta=%d max=%d",
                 (unsigned)index, (unsigned)from, (unsigned)to, delta,
                 max_delta);
        DBG_LOG(status_reporter, msg);
      }
    }
  } else {
    approx_page_cache[index] = -2;
  }

  return true;
}

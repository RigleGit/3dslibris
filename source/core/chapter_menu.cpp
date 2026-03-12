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

#include "chapter_menu.h"

#include <algorithm>
#include <ctype.h>

#include "app.h"
#include "book.h"
#include "debug_log.h"
#include "text.h"

namespace {

static std::string TrimLabel(const std::string &in) {
  size_t start = 0;
  while (start < in.size() && isspace((unsigned char)in[start]))
    start++;
  size_t end = in.size();
  while (end > start && isspace((unsigned char)in[end - 1]))
    end--;
  return in.substr(start, end - start);
}

static std::string BuildTwoLineLabelIfNeeded(const std::string &raw) {
  std::string clean = TrimLabel(raw);
  if (clean.empty())
    return clean;

  size_t split = clean.find(" : ");
  if (split == std::string::npos)
    split = clean.find(": ");
  if (split == std::string::npos)
    split = clean.find(':');

  if (split == std::string::npos)
    return clean;

  std::string title = TrimLabel(clean.substr(0, split));
  size_t rhs_start = split + 1;
  if (rhs_start < clean.size() && clean[rhs_start] == ' ')
    rhs_start++;
  std::string subtitle = TrimLabel(clean.substr(rhs_start));

  if (title.size() < 4 || subtitle.size() < 4)
    return clean;

  return title + "\n" + subtitle;
}

static std::string ApplyLevelPrefix(const std::string &label, u8 level) {
  if (label.empty() || level == 0)
    return label;

  const u8 capped = (level > 6) ? 6 : level;
  std::string prefix((size_t)capped * 2, ' ');
  prefix += "- ";

  std::string out;
  out.reserve(label.size() + prefix.size() * 2);
  bool line_start = true;
  for (size_t i = 0; i < label.size(); i++) {
    if (line_start) {
      out += prefix;
      line_start = false;
    }
    out.push_back(label[i]);
    if (label[i] == '\n')
      line_start = true;
  }
  return out;
}

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
  std::string in = TrimLabel(raw);
  if (in.empty())
    return "";

  // Remove dotted leader tail and trailing page-like numbers.
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
  if (leader != std::string::npos)
    in = TrimLabel(in.substr(0, leader));

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

  return TrimLabel(out);
}

static std::string BuildPageSearchText(Page *page, size_t max_out = 3072) {
  if (!page || !page->GetBuffer() || page->GetLength() <= 0)
    return "";

  const u8 *buf = page->GetBuffer();
  const int len = page->GetLength();
  std::string out;
  out.reserve((size_t)len);
  bool prev_space = true;

  int i = 0;
  while (i < len) {
    unsigned char c = (unsigned char)buf[i];
    if (c == TEXT_IMAGE) {
      i += (i + 2 < len) ? 3 : 1;
      if (!prev_space) {
        out.push_back(' ');
        prev_space = true;
      }
      continue;
    }
    if (c == TEXT_BOLD_ON || c == TEXT_BOLD_OFF || c == TEXT_ITALIC_ON ||
        c == TEXT_ITALIC_OFF) {
      i++;
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
      if (isalnum(c)) {
        out.push_back((char)tolower(c));
        prev_space = false;
      } else if (!prev_space) {
        out.push_back(' ');
        prev_space = true;
      }
      i++;
    } else {
      int step = 1;
      if ((c & 0xE0) == 0xC0)
        step = 2;
      else if ((c & 0xF0) == 0xE0)
        step = 3;
      else if ((c & 0xF8) == 0xF0)
        step = 4;
      if (i + step > len)
        step = 1;
      for (int j = 0; j < step; j++)
        out.push_back((char)buf[i + j]);
      prev_space = false;
      i += step;
    }

    if (max_out > 0 && out.size() >= max_out)
      break;
  }

  return TrimLabel(out);
}

static std::vector<std::string> BuildPageHeadingLines(Page *page,
                                                      size_t max_lines = 12) {
  std::vector<std::string> lines;
  if (!page || !page->GetBuffer() || page->GetLength() <= 0 || max_lines == 0)
    return lines;

  const u8 *buf = page->GetBuffer();
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
    unsigned char c = (unsigned char)buf[i];
    if (c == TEXT_IMAGE) {
      i += (i + 2 < len) ? 2 : 0;
      if (!cur.empty() && cur.back() != ' ')
        cur.push_back(' ');
      continue;
    }
    if (c == TEXT_BOLD_ON || c == TEXT_BOLD_OFF || c == TEXT_ITALIC_ON ||
        c == TEXT_ITALIC_OFF) {
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
    int step = 1;
    if ((c & 0xE0) == 0xC0)
      step = 2;
    else if ((c & 0xF0) == 0xE0)
      step = 3;
    else if ((c & 0xF8) == 0xF0)
      step = 4;
    if (i + step > len)
      step = 1;
    for (int j = 0; j < step; j++)
      cur.push_back((char)buf[i + j]);
    i += (step - 1);
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

static bool FindApproxTitlePage(Book *book, const std::string &title,
                                u16 hint_page, u16 *page_out) {
  if (!book || !page_out)
    return false;
  u16 page_count = book->GetPageCount();
  if (page_count == 0)
    return false;
  if (hint_page >= page_count)
    hint_page = (u16)(page_count - 1);

  std::string query = NormalizeSearchText(title);
  if (query.size() < 4)
    return false;

  auto eval_range = [&](u16 p0, u16 p1, int *best_score, u16 *best_page) {
    if (p0 >= p1 || p0 >= page_count)
      return;
    if (p1 > page_count)
      p1 = page_count;
    for (u16 p = p0; p < p1; p++) {
      Page *page = book->GetPage((int)p);
      std::string text = BuildPageSearchText(page, 4096);
      int heading_score = EvaluateHeadingScore(page, query);
      if (text.empty() && heading_score <= 0)
        continue;
      size_t pos = text.find(query);
      if (pos == std::string::npos && heading_score < 90)
        continue;

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

void ChapterMenu::BuildEntries(std::vector<std::string> &labels,
                               std::vector<u16> &pages) {
  if (!app || !app->bookcurrent)
    return;

  entry_titles.clear();
  entry_pages.clear();
  approx_page_cache.clear();

  SetHeaderTitle(TocQualityLabel(app->bookcurrent->GetTocQuality()));

  const std::vector<ChapterEntry> &chapters = app->bookcurrent->GetChapters();
  labels.reserve(chapters.size());
  pages.reserve(chapters.size());
  entry_titles.reserve(chapters.size());
  entry_pages.reserve(chapters.size());
  approx_page_cache.reserve(chapters.size());

  for (const auto &ch : chapters) {
    std::string label = BuildTwoLineLabelIfNeeded(ch.title);
    labels.push_back(ApplyLevelPrefix(label, ch.level));
    pages.push_back(ch.page);
    entry_titles.push_back(ch.title);
    entry_pages.push_back(ch.page);
    approx_page_cache.push_back(-1);
  }
}

bool ChapterMenu::ResolveTargetPage(u8 index, u16 *page_out) {
  if (!PagedListMenu::ResolveTargetPage(index, page_out))
    return false;
  if (!app || !app->bookcurrent)
    return true;
  if (index >= entry_titles.size() || index >= entry_pages.size() ||
      index >= approx_page_cache.size())
    return true;

  TocQuality q = app->bookcurrent->GetTocQuality();
  if (q != TOC_QUALITY_HEURISTIC)
    return true;

  if (approx_page_cache[index] >= 0) {
    *page_out = (u16)approx_page_cache[index];
    return true;
  }
  if (approx_page_cache[index] == -2)
    return true;

  u16 resolved = *page_out;
  if (FindApproxTitlePage(app->bookcurrent, entry_titles[index],
                          entry_pages[index], &resolved)) {
    bool accept = true;
    const int from = (int)entry_pages[index];
    const int to = (int)resolved;
    const int delta = (to > from) ? (to - from) : (from - to);

    // Trust rule 1: reject big jumps from the original TOC page.
    if (delta > 32)
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
        snprintf(msg, sizeof(msg), "INDEX approx remap sel=%u from=%u to=%u",
                 (unsigned)index, (unsigned)from, (unsigned)to);
        DBG_LOG(app, msg);
      }
      *page_out = resolved;
    } else {
      approx_page_cache[index] = -2;
      if (app) {
        char msg[224];
        snprintf(msg, sizeof(msg),
                 "INDEX approx remap rejected sel=%u from=%u to=%u",
                 (unsigned)index, (unsigned)from, (unsigned)to);
        DBG_LOG(app, msg);
      }
    }
  } else {
    approx_page_cache[index] = -2;
  }

  return true;
}

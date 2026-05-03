/*
    3dslibris - epub_toc_title_match_utils.cpp

    TOC title-to-page matching helpers extracted from epub.cpp.
*/

#include "formats/epub/epub_toc_title_match_utils.h"

#include <algorithm>
#include <ctype.h>

#include "book/page.h"
#include "formats/epub/epub_toc_diag_utils.h"
#include "shared/string_utils.h"
#include "shared/text_token_constants.h"

namespace {

using epub_toc_diag_utils::NormalizeAsciiSearchText;
using epub_toc_diag_utils::NormalizeTocTitle;

static std::string BuildPageSearchText(Page *page, size_t max_out = 2048) {
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
    if (c == TEXT_HR_BOUNDS) {
      i += (i + 2 < len) ? 3 : (i + 1 < len) ? 2 : 1;
      continue;
    }
    if (c == TEXT_BOLD_ON || c == TEXT_BOLD_OFF || c == TEXT_ITALIC_ON ||
        c == TEXT_ITALIC_OFF || c == TEXT_UNDERLINE_ON ||
        c == TEXT_UNDERLINE_OFF || c == TEXT_OVERLINE_ON ||
        c == TEXT_OVERLINE_OFF || c == TEXT_STRIKETHROUGH_ON ||
        c == TEXT_STRIKETHROUGH_OFF || c == TEXT_SUPERSCRIPT_ON ||
        c == TEXT_SUPERSCRIPT_OFF || c == TEXT_SUBSCRIPT_ON ||
        c == TEXT_SUBSCRIPT_OFF || c == TEXT_MONO_ON || c == TEXT_MONO_OFF ||
        c == TEXT_HR || c == TEXT_PRE_ON || c == TEXT_PRE_OFF) {
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
      i++;
      if (isalnum((int)c)) {
        out.push_back((char)tolower((int)c));
        prev_space = false;
      } else if (!prev_space) {
        out.push_back(' ');
        prev_space = true;
      }
      if (max_out > 0 && out.size() >= max_out)
        break;
      continue;
    }

    // Non-ASCII codepoint: encode back to UTF-8 for string matching.
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
    if (max_out > 0 && out.size() >= max_out)
      break;
  }

  return Trim(out);
}

static std::vector<std::string>
BuildPageHeadingLines(Page *page, size_t max_lines = 12,
                      size_t max_line_bytes = 160) {
  std::vector<std::string> lines;
  if (!page || !page->GetBuffer() || page->GetLength() <= 0 || max_lines == 0)
    return lines;

  const u32 *buf = page->GetBuffer();
  const int len = page->GetLength();
  std::string cur;
  cur.reserve(96);

  auto flush_line = [&]() {
    std::string n = NormalizeAsciiSearchText(cur, max_line_bytes);
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
    if (c == TEXT_HR_BOUNDS) {
      i += (i + 2 < len) ? 2 : (i + 1 < len) ? 1 : 0;
      continue;
    }
    if (c == TEXT_BOLD_ON || c == TEXT_BOLD_OFF || c == TEXT_ITALIC_ON ||
        c == TEXT_ITALIC_OFF || c == TEXT_UNDERLINE_ON ||
        c == TEXT_UNDERLINE_OFF || c == TEXT_OVERLINE_ON ||
        c == TEXT_OVERLINE_OFF || c == TEXT_STRIKETHROUGH_ON ||
        c == TEXT_STRIKETHROUGH_OFF || c == TEXT_SUPERSCRIPT_ON ||
        c == TEXT_SUPERSCRIPT_OFF || c == TEXT_SUBSCRIPT_ON ||
        c == TEXT_SUBSCRIPT_OFF || c == TEXT_MONO_ON || c == TEXT_MONO_OFF ||
        c == TEXT_HR || c == TEXT_PRE_ON || c == TEXT_PRE_OFF) {
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

    // Non-ASCII codepoint: encode back to UTF-8 for the string.
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

static int ScoreHeadingLineMatch(const std::string &line,
                                 const std::string &query,
                                 const std::vector<std::string> &tokens,
                                 int line_index) {
  if (line.empty() || query.empty())
    return 0;

  int score = 0;
  if (line == query) {
    score = 150;
  } else if ((line.find(query) != std::string::npos ||
              query.find(line) != std::string::npos) &&
             std::min(line.size(), query.size()) >= 4) {
    score = 105;
  } else {
    int token_hits = 0;
    int token_chars = 0;
    for (size_t i = 0; i < tokens.size() && i < 6; i++) {
      if (line.find(tokens[i]) != std::string::npos) {
        token_hits++;
        token_chars += (int)tokens[i].size();
      }
    }
    if (token_hits >= 2 && token_chars >= 8) {
      score = 56 + std::min(34, token_chars);
    } else if (token_hits >= 1 && token_chars >= 5) {
      score = 34 + std::min(20, token_chars);
    }
  }

  if (score > 0) {
    if (line.size() > 88)
      score -= 54;
    else if (line.size() > 68)
      score -= 32;
    if (line_index == 0)
      score += 16;
    else if (line_index == 1)
      score += 10;
    else if (line_index <= 3)
      score += 6;
    else if (line_index <= 6)
      score += 2;
    if (line.size() <= 44)
      score += 6;
  }
  return score;
}

static int EvaluatePageHeadingScore(Page *page, const std::string &query,
                                    const std::vector<std::string> &tokens) {
  if (!page || query.empty())
    return 0;

  std::vector<std::string> lines = BuildPageHeadingLines(page, 12, 160);
  if (lines.empty())
    return 0;

  int best = 0;
  for (size_t i = 0; i < lines.size(); i++) {
    int s = ScoreHeadingLineMatch(lines[i], query, tokens, (int)i);
    if (s > best)
      best = s;
    if (i + 1 < lines.size()) {
      std::string joined = lines[i] + " " + lines[i + 1];
      int sj = ScoreHeadingLineMatch(joined, query, tokens, (int)i);
      if (sj > best)
        best = sj;
    }
  }
  return best;
}

static std::vector<std::string>
ExtractTitleSearchTokens(const std::string &normalized_title) {
  std::vector<std::string> tokens;
  std::string cur;
  for (size_t i = 0; i <= normalized_title.size(); i++) {
    unsigned char c =
        (i < normalized_title.size()) ? (unsigned char)normalized_title[i] : 0;
    if (c >= 'a' && c <= 'z') {
      cur.push_back((char)c);
    } else if (c >= '0' && c <= '9') {
      cur.push_back((char)c);
    } else {
      if (cur.size() >= 4)
        tokens.push_back(cur);
      cur.clear();
    }
  }
  if (tokens.empty())
    return tokens;
  std::sort(tokens.begin(), tokens.end(),
            [](const std::string &a, const std::string &b) {
              if (a.size() != b.size())
                return a.size() > b.size();
              return a < b;
            });
  tokens.erase(std::unique(tokens.begin(), tokens.end()), tokens.end());
  if (tokens.size() > 8)
    tokens.resize(8);
  return tokens;
}

static bool IsAsciiWordChar(unsigned char c) {
  return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
}

static bool ContainsWholeWordNearTop(const std::string &text,
                                     const std::string &word, size_t max_pos) {
  if (text.empty() || word.empty())
    return false;
  size_t pos = text.find(word);
  while (pos != std::string::npos) {
    if (pos <= max_pos) {
      bool left_ok =
          (pos == 0) || !IsAsciiWordChar((unsigned char)text[pos - 1]);
      size_t end = pos + word.size();
      bool right_ok =
          (end >= text.size()) || !IsAsciiWordChar((unsigned char)text[end]);
      if (left_ok && right_ok)
        return true;
    }
    pos = text.find(word, pos + 1);
  }
  return false;
}

static u16 FindDocEndPage(u16 doc_start, const std::vector<u16> &doc_starts,
                          u16 page_count) {
  for (size_t i = 0; i < doc_starts.size(); i++) {
    if (doc_starts[i] != doc_start)
      continue;
    if (i + 1 < doc_starts.size())
      return doc_starts[i + 1];
    return page_count;
  }
  for (size_t i = 0; i < doc_starts.size(); i++) {
    if (doc_starts[i] > doc_start)
      return doc_starts[i];
  }
  return page_count;
}

} // namespace

namespace epub_toc_title_match_utils {

bool PathLooksLikeTocDocForFallback(const std::string &path) {
  if (path.empty())
    return false;
  std::string lower = ToLowerAscii(path);
  return lower.find("toc") != std::string::npos ||
         lower.find("indice") != std::string::npos ||
         lower.find("index") != std::string::npos ||
         lower.find("contents") != std::string::npos ||
         lower.find("contenido") != std::string::npos ||
         lower.find("nav") != std::string::npos;
}

bool FindTocTitlePageInDocRange(Book *book, u16 doc_start,
                                const std::vector<u16> &doc_starts,
                                const std::string &toc_title, u16 *page_out) {
  if (!book || !page_out)
    return false;

  const u16 page_count = book->GetPageCount();
  if (doc_start >= page_count)
    return false;

  const u16 doc_end = FindDocEndPage(doc_start, doc_starts, page_count);
  if (doc_end <= doc_start + 1)
    return false;

  std::string query = NormalizeAsciiSearchText(toc_title, 192);
  if (query.empty())
    return false;
  std::vector<std::string> tokens = ExtractTitleSearchTokens(query);
  if (tokens.empty() && query.size() < 2)
    return false;

  int best_score = -1;
  int best_hits = 0;
  u16 best_page = doc_start;
  int best_heading_score = -1;
  u16 best_heading_page = doc_start;
  bool have_direct_match = false;
  u16 first_direct_page = doc_start;

  for (u16 p = doc_start; p < doc_end && p < page_count; p++) {
    Page *page = book->GetPage((int)p);
    std::string text = BuildPageSearchText(page, 4096);
    if (text.empty())
      continue;
    size_t direct_pos = text.find(query);
    bool direct_match = (direct_pos != std::string::npos);
    int heading_score = EvaluatePageHeadingScore(page, query, tokens);
    if (heading_score > best_heading_score) {
      best_heading_score = heading_score;
      best_heading_page = p;
    }
    if (heading_score >= 156) {
      *page_out = p;
      return true;
    }
    if (direct_match) {
      if (!have_direct_match) {
        have_direct_match = true;
        first_direct_page = p;
      }
      if (query.size() >= 10) {
        size_t head_len = std::min((size_t)320, text.size());
        if (direct_pos < head_len) {
          *page_out = p;
          return true;
        }
      }
    }

    int score = 0;
    int hits = 0;
    if (direct_match) {
      score += (int)std::min((size_t)query.size(), (size_t)24);
      hits += 2;
      size_t head_len = std::min((size_t)420, text.size());
      if (text.substr(0, head_len).find(query) != std::string::npos) {
        score += (int)std::min((size_t)query.size(), (size_t)24);
        hits += 1;
      }
    }
    for (size_t i = 0; i < tokens.size() && i < 6; i++) {
      if (text.find(tokens[i]) != std::string::npos) {
        score += (int)tokens[i].size();
        hits++;
      }
    }
    if (heading_score > 0) {
      score += heading_score / 2;
      hits++;
    }
    if (score > best_score) {
      best_score = score;
      best_hits = hits;
      best_page = p;
    }
  }

  if (best_heading_score >= 118) {
    *page_out = best_heading_page;
    return true;
  }
  if (best_score >= 12 || (best_score >= 8 && best_hits >= 2)) {
    *page_out = best_page;
    return true;
  }
  if (best_heading_score >= 84) {
    *page_out = best_heading_page;
    return true;
  }
  if (have_direct_match && query.size() >= 5) {
    *page_out = first_direct_page;
    return true;
  }
  if (best_score >= 6 && tokens.size() == 1 && tokens[0].size() >= 5) {
    *page_out = best_page;
    return true;
  }
  return false;
}

bool FindTocTitlePageGlobal(Book *book, const std::string &toc_title,
                            u16 from_page, u16 *page_out, bool allow_wrap,
                            u16 to_page_exclusive) {
  if (!book || !page_out)
    return false;
  u16 page_count = book->GetPageCount();
  if (page_count == 0)
    return false;
  if (to_page_exclusive == 0 || to_page_exclusive > page_count)
    to_page_exclusive = page_count;
  if (from_page >= page_count)
    from_page = 0;
  if (!allow_wrap && from_page >= to_page_exclusive)
    return false;

  std::string query = NormalizeAsciiSearchText(toc_title, 192);
  if (query.empty())
    return false;
  std::vector<std::string> tokens = ExtractTitleSearchTokens(query);
  if (query.size() <= 3) {
    for (u16 pass = 0; pass < (allow_wrap ? 2 : 1); pass++) {
      u16 p0 = (pass == 0) ? from_page : 0;
      u16 p1 = (pass == 0) ? (allow_wrap ? page_count : to_page_exclusive)
                           : from_page;
      for (u16 p = p0; p < p1; p++) {
        Page *page = book->GetPage((int)p);
        std::string text = BuildPageSearchText(page, 1024);
        if (ContainsWholeWordNearTop(text, query, 140)) {
          *page_out = p;
          return true;
        }
      }
    }
  }
  if (tokens.empty() && query.size() < 2)
    return false;

  int best_score = -1;
  int best_hits = 0;
  u16 best_page = from_page;
  int best_heading_score = -1;
  u16 best_heading_page = from_page;
  bool have_direct_match = false;
  u16 first_direct_page = from_page;

  for (u16 pass = 0; pass < (allow_wrap ? 2 : 1); pass++) {
    u16 p0 = (pass == 0) ? from_page : 0;
    u16 p1 =
        (pass == 0) ? (allow_wrap ? page_count : to_page_exclusive) : from_page;
    for (u16 p = p0; p < p1; p++) {
      Page *page = book->GetPage((int)p);
      std::string text = BuildPageSearchText(page, 4096);
      if (text.empty())
        continue;

      size_t direct_pos = text.find(query);
      bool direct_match = (direct_pos != std::string::npos);
      int heading_score = EvaluatePageHeadingScore(page, query, tokens);
      if (heading_score > best_heading_score) {
        best_heading_score = heading_score;
        best_heading_page = p;
      }
      if (heading_score >= 156) {
        *page_out = p;
        return true;
      }
      if (direct_match) {
        if (!have_direct_match) {
          have_direct_match = true;
          first_direct_page = p;
        }
        if (query.size() >= 10) {
          size_t head_len = std::min((size_t)320, text.size());
          if (direct_pos < head_len) {
            *page_out = p;
            return true;
          }
        }
      }

      int score = 0;
      int hits = 0;
      if (direct_match) {
        score += (int)std::min((size_t)query.size(), (size_t)24);
        hits += 2;
        size_t head_len = std::min((size_t)420, text.size());
        if (text.substr(0, head_len).find(query) != std::string::npos) {
          score += (int)std::min((size_t)query.size(), (size_t)24);
          hits += 1;
        }
      }
      for (size_t i = 0; i < tokens.size() && i < 6; i++) {
        if (text.find(tokens[i]) != std::string::npos) {
          score += (int)tokens[i].size();
          hits++;
        }
      }
      if (heading_score > 0) {
        score += heading_score / 2;
        hits++;
      }
      if (!allow_wrap && p >= from_page) {
        int dist_penalty = (int)((p - from_page) / 8);
        score -= dist_penalty;
      }
      if (score > best_score) {
        best_score = score;
        best_hits = hits;
        best_page = p;
      }
    }
  }

  if (best_heading_score >= 118) {
    *page_out = best_heading_page;
    return true;
  }
  if (best_score >= 12 || (best_score >= 8 && best_hits >= 2)) {
    *page_out = best_page;
    return true;
  }
  if (best_heading_score >= 84) {
    *page_out = best_heading_page;
    return true;
  }
  if (have_direct_match && query.size() >= 5) {
    *page_out = first_direct_page;
    return true;
  }
  if (best_score >= 6 && tokens.size() == 1 && tokens[0].size() >= 5) {
    *page_out = best_page;
    return true;
  }
  return false;
}

bool FindChapterPageFromParsedHeadings(
    const std::vector<ChapterEntry> &chapters, const std::string &toc_title,
    u16 min_page, u16 *page_out) {
  if (!page_out || chapters.empty())
    return false;

  std::string query =
      NormalizeAsciiSearchText(NormalizeTocTitle(toc_title), 192);
  if (query.empty())
    return false;

  std::vector<std::string> tokens = ExtractTitleSearchTokens(query);

  struct Candidate {
    u16 page;
    int score;
    bool after_min;
    int distance;
  };
  std::vector<Candidate> candidates;
  candidates.reserve(chapters.size());

  for (const auto &ch : chapters) {
    std::string ctitle =
        NormalizeAsciiSearchText(NormalizeTocTitle(ch.title), 192);
    if (ctitle.empty())
      continue;

    int score = 0;
    if (ctitle == query) {
      score = 120;
    } else if ((ctitle.find(query) != std::string::npos ||
                query.find(ctitle) != std::string::npos) &&
               std::min(ctitle.size(), query.size()) >= 5) {
      score = 80 + (int)std::min((size_t)20,
                                 std::min(ctitle.size(), query.size()) / 2);
    } else if (!tokens.empty()) {
      int token_hits = 0;
      int token_chars = 0;
      for (size_t i = 0; i < tokens.size() && i < 6; i++) {
        if (ctitle.find(tokens[i]) != std::string::npos) {
          token_hits++;
          token_chars += (int)tokens[i].size();
        }
      }
      if (token_hits >= 2 && token_chars >= 9) {
        score = 30 + std::min(30, token_chars);
      }
    }

    if (score <= 0)
      continue;

    bool after_min = ch.page >= min_page;
    int distance = after_min ? (int)(ch.page - min_page)
                             : (int)(min_page - ch.page + 1024);
    candidates.push_back({ch.page, score, after_min, distance});
  }

  if (candidates.empty())
    return false;

  std::sort(candidates.begin(), candidates.end(),
            [](const Candidate &a, const Candidate &b) {
              if (a.after_min != b.after_min)
                return a.after_min > b.after_min;
              if (a.score != b.score)
                return a.score > b.score;
              if (a.distance != b.distance)
                return a.distance < b.distance;
              return a.page < b.page;
            });

  const Candidate best = candidates.front();

  // Keep this fallback conservative: if confidence is low or ambiguous, skip.
  if (best.score < 80)
    return false;

  for (size_t i = 1; i < candidates.size(); i++) {
    const Candidate &alt = candidates[i];
    if (alt.after_min != best.after_min)
      continue;
    int score_diff = alt.score - best.score;
    if (score_diff < 0)
      score_diff = -score_diff;
    if (score_diff <= 2 && alt.page != best.page &&
        alt.distance <= best.distance + 1) {
      return false;
    }
  }

  *page_out = best.page;
  return true;
}

} // namespace epub_toc_title_match_utils

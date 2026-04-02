/*
    3dslibris - epub_toc_diag_utils.cpp

    TOC normalization and debug logging helpers extracted from epub.cpp.
*/

#include "formats/epub/epub_toc_diag_utils.h"

#include <algorithm>
#include <ctype.h>
#include <stdio.h>

#include "debug_log.h"
#include "shared/status_reporter.h"
#include "string_utils.h"

namespace epub_toc_diag_utils {

std::string NormalizeAsciiSearchText(const std::string &raw, size_t max_out) {
  if (raw.empty())
    return "";
  std::string out;
  out.reserve(raw.size());
  bool prev_space = true;
  for (size_t i = 0; i < raw.size(); i++) {
    unsigned char c = (unsigned char)raw[i];
    if (c < 0x80) {
      if (isalnum(c)) {
        out.push_back((char)tolower(c));
        prev_space = false;
      } else if (!prev_space) {
        out.push_back(' ');
        prev_space = true;
      }
    } else {
      // Keep UTF-8 bytes to preserve accented text for exact byte matching.
      out.push_back((char)c);
      prev_space = false;
    }
    if (max_out > 0 && out.size() >= max_out)
      break;
  }
  return Trim(out);
}

std::string NormalizeTocTitle(const std::string &raw) {
  std::string t = Trim(raw);
  if (t.empty())
    return t;
  const std::string original_trimmed = t;
  std::string out;
  out.reserve(t.size());
  bool prev_space = true;
  for (size_t i = 0; i < t.size(); i++) {
    unsigned char c = (unsigned char)t[i];
    bool is_space = isspace(c) || t[i] == '\n' || t[i] == '\r' || t[i] == '\t';
    if (is_space) {
      if (!prev_space)
        out.push_back(' ');
      prev_space = true;
    } else {
      out.push_back((char)c);
      prev_space = false;
    }
    if (out.size() >= 120)
      break;
  }
  out = Trim(out);

  // Trim dotted leaders (e.g. "Chapter ....... 23").
  size_t leader_pos = std::string::npos;
  size_t run = 0;
  for (size_t i = 0; i < out.size(); i++) {
    if (out[i] == '.') {
      run++;
      if (run >= 3) {
        leader_pos = i + 1 - run;
        break;
      }
    } else {
      run = 0;
    }
  }
  const bool had_dotted_leader = (leader_pos != std::string::npos);
  if (had_dotted_leader)
    out = Trim(out.substr(0, leader_pos));

  auto has_ascii_alpha = [](const std::string &s) -> bool {
    for (size_t i = 0; i < s.size(); i++) {
      unsigned char c = (unsigned char)s[i];
      if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'))
        return true;
    }
    return false;
  };

  // Drop trailing numeric page suffix when the remaining title still looks
  // like a real chapter name (avoid erasing numeric-only titles like "1").
  while (!out.empty() && isspace((unsigned char)out.back()))
    out.pop_back();
  size_t end = out.size();
  while (end > 0 && isdigit((unsigned char)out[end - 1]))
    end--;
  if (end < out.size()) {
    size_t ws = end;
    while (ws > 0 && isspace((unsigned char)out[ws - 1]))
      ws--;
    if (ws < out.size()) {
      std::string candidate = Trim(out.substr(0, ws));
      std::string candidate_norm = NormalizeAsciiSearchText(candidate, 96);
      bool chapter_like_prefix = false;
      if (candidate_norm.rfind("chapter", 0) == 0 ||
          candidate_norm.rfind("capitulo", 0) == 0 ||
          candidate_norm.rfind("cap", 0) == 0 ||
          candidate_norm.rfind("part", 0) == 0 ||
          candidate_norm.rfind("parte", 0) == 0 ||
          candidate_norm.rfind("book", 0) == 0 ||
          candidate_norm.rfind("libro", 0) == 0) {
        chapter_like_prefix = true;
      }

      // Keep trailing number when it is likely part of the chapter label
      // ("Capítulo 7", "Part 2"). Drop only probable page suffixes.
      const bool probable_page_suffix =
          had_dotted_leader || (!chapter_like_prefix && candidate.size() >= 8);
      if (probable_page_suffix && !candidate.empty() && candidate.size() >= 4 &&
          has_ascii_alpha(candidate) && has_ascii_alpha(out)) {
        out = candidate;
      }
    }
  }

  while (!out.empty()) {
    unsigned char c = (unsigned char)out.back();
    if (c == '.' || c == ':' || c == ';' || c == '-' || c == ' ')
      out.pop_back();
    else
      break;
  }

  out = Trim(out);
  if (out.empty())
    out = original_trimmed;
  return out;
}

std::string ClipForDiag(const std::string &s, size_t max_bytes) {
  if (s.size() <= max_bytes)
    return s;
  return s.substr(0, max_bytes) + "...";
}

void LogTocEntrySamples(IStatusReporter *reporter, const char *stage,
                        const std::vector<toc_entry_t> &entries,
                        size_t max_entries) {
  if (!reporter || !stage)
    return;

  char summary[128];
  snprintf(summary, sizeof(summary), "EPUB: %s samples=%u/%u", stage,
           (unsigned)std::min(entries.size(), max_entries),
           (unsigned)entries.size());
  DBG_LOG(reporter, summary);

  const size_t sample_count = std::min(entries.size(), max_entries);
  for (size_t i = 0; i < sample_count; i++) {
    std::string raw = Trim(entries[i].title);
    std::string normalized = NormalizeTocTitle(raw);
    std::string href_clip = ClipForDiag(entries[i].href, 60);
    std::string raw_clip = ClipForDiag(raw, 60);
    std::string norm_clip = ClipForDiag(normalized, 60);
    char msg[320];
    snprintf(msg, sizeof(msg),
             "EPUB: %s[%u] lvl=%d href=%s raw=%s norm=%s%s%s", stage,
             (unsigned)i, entries[i].level, href_clip.c_str(), raw_clip.c_str(),
             norm_clip.c_str(), raw.empty() ? " raw-empty" : "",
             normalized.empty() ? " norm-empty" : "");
    DBG_LOG(reporter, msg);
  }
}

void LogResolvedChapterSamples(IStatusReporter *reporter, const char *stage,
                               const std::vector<ChapterEntry> &entries,
                               size_t max_entries) {
  if (!reporter || !stage)
    return;

  char summary[128];
  snprintf(summary, sizeof(summary), "EPUB: %s samples=%u/%u", stage,
           (unsigned)std::min(entries.size(), max_entries),
           (unsigned)entries.size());
  DBG_LOG(reporter, summary);

  const size_t sample_count = std::min(entries.size(), max_entries);
  for (size_t i = 0; i < sample_count; i++) {
    std::string title_clip = ClipForDiag(entries[i].title, 72);
    char msg[256];
    snprintf(msg, sizeof(msg), "EPUB: %s[%u] page=%u lvl=%d title=%s", stage,
             (unsigned)i, (unsigned)entries[i].page, entries[i].level,
             title_clip.c_str());
    DBG_LOG(reporter, msg);
  }
}

void LogTocResolveDecision(IStatusReporter *reporter, size_t index,
                           const toc_entry_t &src,
                           const std::string &normalized_title,
                           const char *method, bool have_page, u16 page,
                           const char *note) {
  if (!reporter || !method)
    return;

  std::string href_clip = ClipForDiag(src.href, 56);
  std::string raw_clip = ClipForDiag(Trim(src.title), 56);
  std::string norm_clip = ClipForDiag(normalized_title, 56);
  char msg[384];
  snprintf(msg, sizeof(msg),
           "EPUB: TOC map[%u] lvl=%d method=%s page=%u href=%s raw=%s norm=%s%s",
           (unsigned)index, src.level, method, have_page ? (unsigned)page : 0u,
           href_clip.c_str(), raw_clip.c_str(), norm_clip.c_str(),
           note ? note : "");
  DBG_LOG(reporter, msg);
}

} // namespace epub_toc_diag_utils


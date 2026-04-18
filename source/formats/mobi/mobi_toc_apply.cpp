/*
    3dslibris - mobi_toc_apply.cpp

    Apply structured MOBI TOC entries to chapter pages.
*/

#include "formats/mobi/mobi_toc_apply.h"

#include <ctype.h>
#include <set>
#include <stdio.h>

#include "book/book.h"
#include "debug_log.h"
#include "formats/common/page_text_extract_utils.h"
#include "formats/mobi/mobi_position_map.h"
#include "shared/status_reporter.h"

namespace {

static const u32 kMobiNullIndex = mobi_structured_toc_parser::kMobiNullIndex;
typedef mobi_structured_toc_parser::MobiStructuredTocEntry MobiStructuredTocEntry;

int FindHeadingNearPage(const std::vector<std::vector<std::string>> &page_lines,
                        const std::string &needle, u16 guess_page, u16 radius,
                        const mobi_toc_apply::BuildCallbacks &callbacks) {
  if (needle.size() < 3 || page_lines.empty() || !callbacks.page_has_heading_needle)
    return -1;
  const u16 page_count = (u16)page_lines.size();
  if (guess_page >= page_count)
    guess_page = page_count - 1;

  for (u16 delta = 0; delta <= radius; delta++) {
    int lo = (int)guess_page - (int)delta;
    if (lo >= 0 && callbacks.page_has_heading_needle(page_lines[(size_t)lo], needle))
      return lo;
    if (delta == 0)
      continue;
    u16 hi = (u16)(guess_page + delta);
    if (hi < page_count &&
        callbacks.page_has_heading_needle(page_lines[(size_t)hi], needle))
      return (int)hi;
  }
  return -1;
}

std::string TrimAsciiWhitespaceLocal(const std::string &in) {
  size_t start = 0;
  while (start < in.size() && isspace((unsigned char)in[start]))
    start++;
  size_t end = in.size();
  while (end > start && isspace((unsigned char)in[end - 1]))
    end--;
  return in.substr(start, end - start);
}

std::string CollapseAsciiWhitespaceLocal(const std::string &in) {
  std::string out;
  out.reserve(in.size());
  bool last_space = false;
  for (size_t i = 0; i < in.size(); i++) {
    unsigned char c = (unsigned char)in[i];
    if (isspace(c)) {
      if (!last_space && !out.empty()) {
        out.push_back(' ');
        last_space = true;
      }
      continue;
    }
    out.push_back((char)c);
    last_space = false;
  }
  return out;
}

} // namespace

namespace mobi_toc_apply {

int HtmlPosToPage(u32 html_pos,
                  const std::vector<std::pair<u32, u32>> &html_to_text_map,
                  const std::vector<u32> &text_cursor_per_page,
                  u32 *text_pos_out) {
  if (text_pos_out)
    *text_pos_out = 0;
  if (html_to_text_map.size() < 2 || text_cursor_per_page.empty())
    return -1;

  size_t lo = 0, hi = html_to_text_map.size() - 1;
  while (lo + 1 < hi) {
    size_t mid = (lo + hi) / 2;
    if (html_to_text_map[mid].first <= html_pos)
      lo = mid;
    else
      hi = mid;
  }
  const u32 h0 = html_to_text_map[lo].first;
  const u32 t0 = html_to_text_map[lo].second;
  const u32 h1 = html_to_text_map[hi].first;
  const u32 t1 = html_to_text_map[hi].second;
  u32 text_pos;
  if (h1 == h0)
    text_pos = t0;
  else {
    double frac = (double)(html_pos - h0) / (double)(h1 - h0);
    if (frac < 0.0)
      frac = 0.0;
    if (frac > 1.0)
      frac = 1.0;
    text_pos = t0 + (u32)(frac * (double)(t1 - t0));
  }

  if (text_pos_out)
    *text_pos_out = text_pos;

  lo = 0;
  hi = text_cursor_per_page.size();
  while (lo + 1 < hi) {
    size_t mid = (lo + hi) / 2;
    if (text_cursor_per_page[mid] <= text_pos)
      lo = mid;
    else
      hi = mid;
  }
  return (int)lo;
}

size_t BuildChaptersFromStructuredToc(
    Book *book, const std::vector<MobiStructuredTocEntry> &entries, u32 text_len,
    size_t *direct_out, bool refine_with_heading_search,
    const std::vector<std::pair<u32, u32>> &html_to_text_map,
    const std::vector<u32> &text_cursor_per_page,
    const BuildCallbacks &callbacks, IStatusReporter *reporter) {
  if (direct_out)
    *direct_out = 0;
  if (!book || entries.empty() || book->GetPageCount() == 0)
    return 0;

  const u16 page_count = book->GetPageCount();
  const bool have_precise_map =
      mobi_position_map::LooksUsableForToc(html_to_text_map, text_cursor_per_page);

  if (reporter) {
    u32 map_last_html = 0, map_last_text = 0;
    if (!html_to_text_map.empty()) {
      map_last_html = html_to_text_map.back().first;
      map_last_text = html_to_text_map.back().second;
    }
    u32 pages_last_cursor = 0;
    if (!text_cursor_per_page.empty())
      pages_last_cursor = text_cursor_per_page.back();
    static char dbg[320];
    snprintf(dbg, sizeof(dbg),
             "TOC-MAP: precise=%d entries=%u html_map=%u text_pages=%u "
             "pages=%u text_len=%u map_end=(%u,%u) pages_end=%u",
             have_precise_map ? 1 : 0, (unsigned)entries.size(),
             (unsigned)html_to_text_map.size(),
             (unsigned)text_cursor_per_page.size(), (unsigned)page_count,
             (unsigned)text_len, (unsigned)map_last_html, (unsigned)map_last_text,
             (unsigned)pages_last_cursor);
    DBG_LOG(reporter, dbg);
  }

  bool needs_heading_search = refine_with_heading_search;
  if (!needs_heading_search && have_precise_map)
    needs_heading_search = true;
  if (!needs_heading_search) {
    for (size_t i = 0; i < entries.size(); i++) {
      if (entries[i].pos == kMobiNullIndex) {
        needs_heading_search = true;
        break;
      }
    }
  }

  std::vector<std::vector<std::string>> page_lines;
  if (needs_heading_search) {
    page_lines.resize(page_count);
    for (u16 p = 0; p < page_count; p++)
      page_lines[p] =
          page_text_extract_utils::ExtractTextLinesFromPage(book->GetPage(p));
  }

  std::set<u16> used_pages;
  size_t mapped = 0;
  size_t direct_used = 0;
  u16 scan_start = 0;
  const u32 denom = (text_len > 0) ? text_len : 1;

  for (size_t i = 0; i < entries.size(); i++) {
    std::string clean =
        CollapseAsciiWhitespaceLocal(TrimAsciiWhitespaceLocal(entries[i].title));
    if (clean.empty())
      continue;

    const std::string needle =
        callbacks.normalize_heading_needle ? callbacks.normalize_heading_needle(clean)
                                           : clean;
    bool has_pos = (entries[i].pos != kMobiNullIndex);
    int best_page = -1;

    if (has_pos) {
      if (have_precise_map) {
        u32 text_pos = 0;
        int precise =
            HtmlPosToPage(entries[i].pos, html_to_text_map, text_cursor_per_page,
                          &text_pos);
        if (precise >= 0 && precise < page_count)
          best_page = precise;
        if (reporter) {
          int linear_page = -1;
          double r = (double)entries[i].pos / (double)denom;
          if (r > 1.0)
            r = 1.0;
          linear_page = (int)((u16)(r * (double)(page_count - 1)));
          static char dbg[256];
          snprintf(dbg, sizeof(dbg),
                   "TOC[%u] pos=%u tpos=%u precise=%d linear=%d title=%.40s",
                   (unsigned)i, (unsigned)entries[i].pos, (unsigned)text_pos,
                   precise, linear_page, clean.c_str());
          DBG_LOG(reporter, dbg);
        }
      }
      if (best_page < 0) {
        double ratio = (double)entries[i].pos / (double)denom;
        if (ratio < 0.0)
          ratio = 0.0;
        if (ratio > 1.0)
          ratio = 1.0;
        best_page = (int)((u16)(ratio * (double)(page_count - 1)));
      }

      if (needs_heading_search && callbacks.page_has_heading_needle) {
        int refined =
            FindHeadingNearPage(page_lines, needle, (u16)best_page, 4, callbacks);
        if (refined >= 0 && refined != best_page) {
          if (reporter) {
            static char rdbg[192];
            snprintf(rdbg, sizeof(rdbg), "TOC[%u] heading refine %d -> %d",
                     (unsigned)i, best_page, refined);
            DBG_LOG(reporter, rdbg);
          }
          best_page = refined;
        } else if (refined >= 0) {
          best_page = refined;
        }
      }
    } else {
      if (!needs_heading_search || !callbacks.page_has_heading_needle)
        continue;
      for (u16 p = scan_start; p < page_count; p++) {
        if (callbacks.page_has_heading_needle(page_lines[p], needle)) {
          best_page = (int)p;
          break;
        }
      }
      if (best_page < 0 && scan_start > 0) {
        for (u16 p = 0; p < scan_start; p++) {
          if (callbacks.page_has_heading_needle(page_lines[p], needle)) {
            best_page = (int)p;
            break;
          }
        }
      }
    }

    if (best_page < 0 || best_page >= page_count)
      continue;

    const u16 page = (u16)best_page;
    if (used_pages.find(page) != used_pages.end())
      continue;

    if (callbacks.add_chapter_at_page_if_unique)
      callbacks.add_chapter_at_page_if_unique(book, page, clean, entries[i].level);
    else
      book->AddChapter(page, clean, entries[i].level);
    used_pages.insert(page);
    mapped++;
    scan_start = page;
    if (has_pos)
      direct_used++;
  }

  if (direct_out)
    *direct_out = direct_used;
  return mapped;
}

} // namespace mobi_toc_apply
